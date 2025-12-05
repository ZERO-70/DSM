/**
 * @file dsm_memory.c
 * @brief Memory management subsystem using mmap
 */

#include "dsm.h"
#include "dsm_types.h"
#include "dsm_network.h"
#include "dsm_memory.h"
#include "dsm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>

/*============================================================================
 * Constants
 *===========================================================================*/

#define PAGE_TABLE_INITIAL_CAPACITY     1024
#define OWNERSHIP_TABLE_SIZE            4096

/*============================================================================
 * List Pages Response Storage (for async message handling)
 *===========================================================================*/

static dsm_msg_list_pages_t *g_list_pages_response = NULL;
static size_t g_list_pages_response_size = 0;
static pthread_mutex_t g_list_pages_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_list_pages_cond = PTHREAD_COND_INITIALIZER;
static int g_list_pages_ready = 0;

void dsm_store_list_pages_response(dsm_msg_list_pages_t *resp, size_t size) {
    pthread_mutex_lock(&g_list_pages_mutex);
    
    /* Free any previous response */
    if (g_list_pages_response) {
        free(g_list_pages_response);
    }
    
    /* Copy the response */
    g_list_pages_response = (dsm_msg_list_pages_t*)malloc(size);
    if (g_list_pages_response) {
        memcpy(g_list_pages_response, resp, size);
        g_list_pages_response_size = size;
        g_list_pages_ready = 1;
    }
    
    pthread_cond_signal(&g_list_pages_cond);
    pthread_mutex_unlock(&g_list_pages_mutex);
}

/*============================================================================
 * Ownership Table (Master Only)
 *===========================================================================*/

typedef struct ownership_entry {
    uint64_t            global_addr;
    size_t              size;
    uint32_t            owner_id;
    struct ownership_entry *next;
} ownership_entry_t;

static ownership_entry_t *g_ownership_table[OWNERSHIP_TABLE_SIZE];
static pthread_rwlock_t g_ownership_lock;

static uint32_t ownership_hash(uint64_t addr) {
    return (uint32_t)((addr >> 12) % OWNERSHIP_TABLE_SIZE);
}

int dsm_ownership_register(uint64_t global_addr, size_t size, uint32_t owner_id) {
    ownership_entry_t *entry = (ownership_entry_t*)malloc(sizeof(ownership_entry_t));
    if (!entry) {
        LOG_ERROR("Failed to allocate ownership entry");
        return -1;
    }
    
    entry->global_addr = global_addr;
    entry->size = size;
    entry->owner_id = owner_id;
    
    uint32_t hash = ownership_hash(global_addr);
    
    pthread_rwlock_wrlock(&g_ownership_lock);
    entry->next = g_ownership_table[hash];
    g_ownership_table[hash] = entry;
    pthread_rwlock_unlock(&g_ownership_lock);
    
    LOG_DEBUG("Registered ownership: addr=0x%lx, size=%zu, owner=%d", 
             global_addr, size, owner_id);
    return 0;
}

int dsm_ownership_get(uint64_t global_addr) {
    pthread_rwlock_rdlock(&g_ownership_lock);
    
    /* Search all buckets since address may be within a region stored at different hash */
    for (int i = 0; i < OWNERSHIP_TABLE_SIZE; i++) {
        ownership_entry_t *entry = g_ownership_table[i];
        while (entry) {
            if (global_addr >= entry->global_addr && 
                global_addr < entry->global_addr + entry->size) {
                int owner = entry->owner_id;
                pthread_rwlock_unlock(&g_ownership_lock);
                return owner;
            }
            entry = entry->next;
        }
    }
    
    pthread_rwlock_unlock(&g_ownership_lock);
    return -1;
}

int dsm_ownership_set(uint64_t global_addr, uint32_t owner_id) {
    pthread_rwlock_wrlock(&g_ownership_lock);
    
    /* Search all buckets for EXACT page address match (not range) */
    for (int i = 0; i < OWNERSHIP_TABLE_SIZE; i++) {
        ownership_entry_t *entry = g_ownership_table[i];
        while (entry) {
            /* Only update if this is the EXACT page, not any page in a region */
            if (entry->global_addr == global_addr && entry->size == DSM_PAGE_SIZE) {
                entry->owner_id = owner_id;
                pthread_rwlock_unlock(&g_ownership_lock);
                LOG_DEBUG("Updated ownership: addr=0x%lx, new_owner=%d", global_addr, owner_id);
                return 0;
            }
            entry = entry->next;
        }
    }
    
    pthread_rwlock_unlock(&g_ownership_lock);
    LOG_WARN("[OWNERSHIP] Failed to find page 0x%lx for ownership update", global_addr);
    return -1;
}

static void ownership_table_init(void) {
    memset(g_ownership_table, 0, sizeof(g_ownership_table));
    pthread_rwlock_init(&g_ownership_lock, NULL);
}

static void ownership_table_destroy(void) {
    pthread_rwlock_wrlock(&g_ownership_lock);
    
    for (int i = 0; i < OWNERSHIP_TABLE_SIZE; i++) {
        ownership_entry_t *entry = g_ownership_table[i];
        while (entry) {
            ownership_entry_t *next = entry->next;
            free(entry);
            entry = next;
        }
        g_ownership_table[i] = NULL;
    }
    
    pthread_rwlock_unlock(&g_ownership_lock);
    pthread_rwlock_destroy(&g_ownership_lock);
}

/**
 * Collect all ownership entries into a page list message.
 * This expands regions into individual pages for easier access.
 * Caller must free the returned pointer.
 * Returns NULL on error, sets *out_count to number of pages.
 */
dsm_msg_list_pages_t* dsm_ownership_collect_all(uint32_t *out_count) {
    /* First pass: count total pages (not regions) */
    uint32_t total_pages = 0;
    
    pthread_rwlock_rdlock(&g_ownership_lock);
    
    for (int i = 0; i < OWNERSHIP_TABLE_SIZE; i++) {
        ownership_entry_t *entry = g_ownership_table[i];
        while (entry) {
            /* Count pages in this region */
            total_pages += (entry->size + DSM_PAGE_SIZE - 1) / DSM_PAGE_SIZE;
            entry = entry->next;
        }
    }
    
    if (total_pages == 0) {
        pthread_rwlock_unlock(&g_ownership_lock);
        *out_count = 0;
        /* Return empty message */
        dsm_msg_list_pages_t *msg = (dsm_msg_list_pages_t*)malloc(sizeof(dsm_msg_list_pages_t));
        if (msg) msg->count = 0;
        return msg;
    }
    
    /* Allocate message for all individual pages */
    size_t msg_size = sizeof(dsm_msg_list_pages_t) + total_pages * sizeof(dsm_page_entry_t);
    dsm_msg_list_pages_t *msg = (dsm_msg_list_pages_t*)malloc(msg_size);
    if (!msg) {
        pthread_rwlock_unlock(&g_ownership_lock);
        *out_count = 0;
        return NULL;
    }
    
    /* Second pass: fill entries (expand regions to individual pages) */
    msg->count = total_pages;
    uint32_t idx = 0;
    
    for (int i = 0; i < OWNERSHIP_TABLE_SIZE && idx < total_pages; i++) {
        ownership_entry_t *entry = g_ownership_table[i];
        while (entry && idx < total_pages) {
            /* Expand this region into individual pages */
            uint32_t num_pages = (entry->size + DSM_PAGE_SIZE - 1) / DSM_PAGE_SIZE;
            for (uint32_t p = 0; p < num_pages && idx < total_pages; p++) {
                msg->pages[idx].global_addr = entry->global_addr + p * DSM_PAGE_SIZE;
                msg->pages[idx].size = DSM_PAGE_SIZE;
                msg->pages[idx].owner_id = entry->owner_id;
                idx++;
            }
            entry = entry->next;
        }
    }
    
    pthread_rwlock_unlock(&g_ownership_lock);
    
    *out_count = total_pages;
    LOG_INFO("[LIST_PAGES] Collected %u individual pages from ownership table", total_pages);
    return msg;
}

/*============================================================================
 * Page Table Implementation
 *===========================================================================*/

int dsm_page_table_init(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    ctx->page_table = (dsm_page_table_t*)calloc(1, sizeof(dsm_page_table_t));
    if (!ctx->page_table) {
        LOG_ERROR("Failed to allocate page table");
        return -1;
    }
    
    ctx->page_table->pages = (dsm_page_t**)calloc(PAGE_TABLE_INITIAL_CAPACITY, 
                                                   sizeof(dsm_page_t*));
    if (!ctx->page_table->pages) {
        LOG_ERROR("Failed to allocate page table entries");
        free(ctx->page_table);
        ctx->page_table = NULL;
        return -1;
    }
    
    ctx->page_table->capacity = PAGE_TABLE_INITIAL_CAPACITY;
    ctx->page_table->count = 0;
    
    if (pthread_rwlock_init(&ctx->page_table->rwlock, NULL) != 0) {
        free(ctx->page_table->pages);
        free(ctx->page_table);
        ctx->page_table = NULL;
        return -1;
    }
    
    LOG_DEBUG("Page table initialized with capacity %zu", (size_t)PAGE_TABLE_INITIAL_CAPACITY);
    return 0;
}

void dsm_page_table_destroy(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->page_table) {
        return;
    }
    
    pthread_rwlock_wrlock(&ctx->page_table->rwlock);
    
    for (size_t i = 0; i < ctx->page_table->count; i++) {
        if (ctx->page_table->pages[i]) {
            pthread_mutex_destroy(&ctx->page_table->pages[i]->lock);
            free(ctx->page_table->pages[i]);
        }
    }
    
    free(ctx->page_table->pages);
    pthread_rwlock_unlock(&ctx->page_table->rwlock);
    pthread_rwlock_destroy(&ctx->page_table->rwlock);
    free(ctx->page_table);
    ctx->page_table = NULL;
    
    LOG_DEBUG("Page table destroyed");
}

int dsm_page_register(uint64_t global_addr, void *local_addr, uint32_t owner_id) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->page_table) {
        return -1;
    }
    
    dsm_page_t *page = (dsm_page_t*)calloc(1, sizeof(dsm_page_t));
    if (!page) {
        LOG_ERROR("Failed to allocate page");
        return -1;
    }
    
    page->global_addr = global_addr;
    page->local_addr = local_addr;
    page->size = DSM_PAGE_SIZE;
    page->owner_id = owner_id;
    page->state = (owner_id == ctx->local_node_id) ? DSM_PAGE_MODIFIED : DSM_PAGE_INVALID;
    page->version = 1;
    page->dirty = false;
    page->in_use = false;  /* Initially not allocated from pool */
    page->freed = false;   /* Not freed initially */
    
    /* Initialize copyset - if we own it, we're the only one with a copy */
    memset(page->copyset, 0, sizeof(page->copyset));
    page->copyset_count = 0;
    if (owner_id == ctx->local_node_id) {
        page->copyset[ctx->local_node_id] = 1;
        page->copyset_count = 1;
    }
    
    pthread_mutex_init(&page->lock, NULL);
    
    LOG_INFO("[REGISTER] NEW PAGE: global=0x%lx, local=%p, owner=%d, state=%d, local_node_id=%d",
             global_addr, local_addr, owner_id, page->state, ctx->local_node_id);
    
    pthread_rwlock_wrlock(&ctx->page_table->rwlock);
    
    /* Expand if needed */
    if (ctx->page_table->count >= ctx->page_table->capacity) {
        size_t new_cap = ctx->page_table->capacity * 2;
        dsm_page_t **new_pages = (dsm_page_t**)realloc(ctx->page_table->pages,
                                                        new_cap * sizeof(dsm_page_t*));
        if (!new_pages) {
            pthread_rwlock_unlock(&ctx->page_table->rwlock);
            pthread_mutex_destroy(&page->lock);
            free(page);
            return -1;
        }
        ctx->page_table->pages = new_pages;
        ctx->page_table->capacity = new_cap;
    }
    
    size_t index = ctx->page_table->count;
    ctx->page_table->pages[ctx->page_table->count++] = page;
    
    pthread_rwlock_unlock(&ctx->page_table->rwlock);
    
    LOG_INFO("[REGISTER] Page added at index %zu, total pages now: %zu", 
             index, ctx->page_table->count);
    return 0;
}

dsm_page_t* dsm_page_lookup(uint64_t global_addr) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->page_table) {
        return NULL;
    }
    
    /* Align address to page boundary */
    uint64_t page_addr = global_addr & ~(DSM_PAGE_SIZE - 1);
    
    pthread_rwlock_rdlock(&ctx->page_table->rwlock);
    
    dsm_page_t *result = NULL;
    for (size_t i = 0; i < ctx->page_table->count; i++) {
        if (ctx->page_table->pages[i]->global_addr == page_addr) {
            result = ctx->page_table->pages[i];
            break;
        }
    }
    
    pthread_rwlock_unlock(&ctx->page_table->rwlock);
    return result;
}

dsm_page_t* dsm_page_lookup_local(void *local_addr) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->page_table) {
        LOG_ERROR("dsm_page_lookup_local: page_table is NULL");
        return NULL;
    }
    
    /* Align address to page boundary */
    uintptr_t addr = (uintptr_t)local_addr & ~(DSM_PAGE_SIZE - 1);
    
    LOG_INFO("[LOOKUP] Searching for local_addr=%p (aligned=0x%lx), page_count=%zu",
             local_addr, (unsigned long)addr, ctx->page_table->count);
    
    pthread_rwlock_rdlock(&ctx->page_table->rwlock);
    
    /* DUMP ALL PAGES */
    LOG_INFO("[LOOKUP] === FULL PAGE TABLE DUMP ===");
    for (size_t i = 0; i < ctx->page_table->count; i++) {
        dsm_page_t *p = ctx->page_table->pages[i];
        LOG_INFO("[LOOKUP]   [%zu] local=%p global=0x%lx owner=%d state=%d",
                 i, p->local_addr, p->global_addr, p->owner_id, p->state);
    }
    LOG_INFO("[LOOKUP] === END DUMP ===");
    
    /* Search from END (newest pages first) to handle address reuse by mmap */
    dsm_page_t *result = NULL;
    LOG_INFO("[LOOKUP] Searching from end (index %zu down to 0)...", ctx->page_table->count - 1);
    for (size_t i = ctx->page_table->count; i > 0; i--) {
        dsm_page_t *p = ctx->page_table->pages[i - 1];
        LOG_INFO("[LOOKUP]   Checking [%zu]: local=%p vs target=0x%lx => %s",
                i - 1, p->local_addr, (unsigned long)addr,
                ((uintptr_t)p->local_addr == addr) ? "MATCH" : "no");
        if ((uintptr_t)p->local_addr == addr) {
            result = p;
            LOG_INFO("[LOOKUP] *** MATCH at [%zu]: global=0x%lx, owner=%d, state=%d ***", 
                    i - 1, p->global_addr, p->owner_id, p->state);
            break;
        }
    }
    
    pthread_rwlock_unlock(&ctx->page_table->rwlock);
    
    if (!result) {
        LOG_ERROR("[LOOKUP] No page found for addr 0x%lx", (unsigned long)addr);
    } else {
        LOG_INFO("[LOOKUP] Returning page: global=0x%lx, owner=%d, state=%d",
                 result->global_addr, result->owner_id, result->state);
    }
    
    return result;
}

/**
 * Find a free page from the local pool (pages this node owns but hasn't used)
 * @param num_pages Number of contiguous pages needed
 * @return Pointer to first free page, or NULL if not enough free pages
 */
dsm_page_t* dsm_page_find_free(size_t num_pages) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->page_table || num_pages == 0) {
        return NULL;
    }
    
    pthread_rwlock_rdlock(&ctx->page_table->rwlock);
    
    /* Look for contiguous free pages that we own */
    size_t contiguous = 0;
    dsm_page_t *first_free = NULL;
    
    for (size_t i = 0; i < ctx->page_table->count; i++) {
        dsm_page_t *page = ctx->page_table->pages[i];
        
        /* A page is free ONLY if it was explicitly freed (via dsm_free) */
        if (page->freed) {
            
            if (contiguous == 0) {
                first_free = page;
            }
            contiguous++;
            
            if (contiguous >= num_pages) {
                LOG_INFO("[POOL] Found %zu free page(s) starting at global 0x%lx",
                         num_pages, first_free->global_addr);
                pthread_rwlock_unlock(&ctx->page_table->rwlock);
                return first_free;
            }
        } else {
            /* Reset - need contiguous pages */
            contiguous = 0;
            first_free = NULL;
        }
    }
    
    pthread_rwlock_unlock(&ctx->page_table->rwlock);
    LOG_DEBUG("[POOL] No free pages available in pool (need %zu)", num_pages);
    return NULL;
}

/**
 * Allocate pages from the local pool
 * @param num_pages Number of pages to allocate
 * @return Local address of first page, or NULL on failure
 */
void* dsm_pool_alloc(size_t num_pages) {
    dsm_context_t *ctx = dsm_get_context();
    
    dsm_page_t *first_page = dsm_page_find_free(num_pages);
    if (!first_page) {
        return NULL;
    }
    
    /* Mark pages as in_use and make them accessible */
    pthread_rwlock_rdlock(&ctx->page_table->rwlock);
    
    uint64_t start_addr = first_page->global_addr;
    for (size_t i = 0; i < ctx->page_table->count && num_pages > 0; i++) {
        dsm_page_t *page = ctx->page_table->pages[i];
        
        if (page->global_addr >= start_addr && 
            page->global_addr < start_addr + (num_pages * DSM_PAGE_SIZE)) {
            
            pthread_mutex_lock(&page->lock);
            page->in_use = true;
            page->state = DSM_PAGE_MODIFIED;  /* Exclusive access */
            pthread_mutex_unlock(&page->lock);
            
            /* Ensure page is accessible */
            if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_READ | PROT_WRITE) < 0) {
                LOG_ERROR("[POOL] mprotect failed for page 0x%lx: %s", 
                          page->global_addr, strerror(errno));
            }
            
            LOG_INFO("[POOL] Allocated page: global=0x%lx, local=%p", 
                     page->global_addr, page->local_addr);
        }
    }
    
    pthread_rwlock_unlock(&ctx->page_table->rwlock);
    
    /* Register ownership in master's ownership table */
    dsm_ownership_register(first_page->global_addr, num_pages * DSM_PAGE_SIZE, ctx->local_node_id);
    
    return first_page->local_addr;
}

int dsm_page_request(dsm_page_t *page, int access_type) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!page) {
        return -1;
    }
    
    LOG_DEBUG("Requesting page 0x%lx from owner %d (access=%d)", 
             page->global_addr, page->owner_id, access_type);
    
    /* Build page request message */
    dsm_msg_page_request_t req;
    req.global_addr = page->global_addr;
    req.access_type = access_type;
    
    dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_PAGE_REQUEST, 
                                              page->owner_id, sizeof(req));
    
    /* Send to owner */
    if (dsm_tcp_send(page->owner_id, &hdr, &req) != 0) {
        LOG_ERROR("Failed to send page request");
        return -1;
    }
    
    /* Wait for page data (handled by TCP thread, signals via condition) */
    pthread_mutex_lock(&page->lock);
    while (page->state == DSM_PAGE_PENDING || page->state == DSM_PAGE_INVALID) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5; /* 5 second timeout */
        
        int ret = pthread_cond_timedwait(&ctx->fault_cond, &page->lock, &ts);
        if (ret == ETIMEDOUT) {
            LOG_ERROR("Timeout waiting for page 0x%lx", page->global_addr);
            pthread_mutex_unlock(&page->lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&page->lock);
    
    return 0;
}

int dsm_page_send(uint32_t node_id, uint64_t global_addr) {
    dsm_context_t *ctx = dsm_get_context();
    dsm_page_t *page = dsm_page_lookup(global_addr);
    
    if (!page || !page->local_addr) {
        LOG_ERROR("Page 0x%lx not found or not local", global_addr);
        return -1;
    }
    
    LOG_INFO("Sending page 0x%lx to node %d (read-only copy)", global_addr, node_id);
    
    /* If we have exclusive access (MODIFIED), downgrade to SHARED since
     * we're now sharing with another node */
    pthread_mutex_lock(&page->lock);
    if (page->state == DSM_PAGE_MODIFIED) {
        LOG_INFO("[SHARE] Downgrading page 0x%lx from MODIFIED to SHARED", global_addr);
        page->state = DSM_PAGE_SHARED;
        /* Keep write access for owner, but state indicates shared */
    }
    pthread_mutex_unlock(&page->lock);
    
    /* Build page data message */
    size_t msg_size = sizeof(dsm_msg_page_data_t) + DSM_PAGE_SIZE;
    uint8_t *buf = (uint8_t*)malloc(msg_size);
    if (!buf) {
        LOG_ERROR("Failed to allocate page send buffer");
        return -1;
    }
    
    dsm_msg_page_data_t *data = (dsm_msg_page_data_t*)buf;
    data->global_addr = global_addr;
    data->size = DSM_PAGE_SIZE;
    data->version = page->version;
    memcpy(data->data, page->local_addr, DSM_PAGE_SIZE);
    
    dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_PAGE_DATA, node_id, msg_size);
    
    LOG_INFO("Sending PAGE_DATA message: addr=0x%lx, size=%zu to node %d", 
            global_addr, msg_size, node_id);
    
    int ret = dsm_tcp_send(node_id, &hdr, buf);
    free(buf);
    
    if (ret == 0) {
        /* Add the receiving node to the copyset (if we're master) */
        if (ctx->local_role == DSM_ROLE_MASTER) {
            dsm_page_copyset_add(global_addr, node_id);
        }
        LOG_INFO("Page 0x%lx sent successfully to node %d", global_addr, node_id);
    } else {
        LOG_ERROR("Failed to send page 0x%lx to node %d", global_addr, node_id);
    }
    
    return ret;
}

int dsm_page_install(uint64_t global_addr, void *data, size_t size, uint64_t version) {
    dsm_context_t *ctx = dsm_get_context();
    dsm_page_t *page = dsm_page_lookup(global_addr);
    
    if (!page) {
        LOG_ERROR("Page 0x%lx not registered", global_addr);
        return -1;
    }
    
    LOG_INFO("Installing page 0x%lx (size=%zu, version=%lu)", global_addr, size, version);
    
    pthread_mutex_lock(&page->lock);
    
    /* Copy data to local address */
    if (page->local_addr) {
        /* FIRST enable write access so we can copy */
        if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
            LOG_ERROR("mprotect (write) failed: %s", strerror(errno));
            pthread_mutex_unlock(&page->lock);
            return -1;
        }
        
        /* Now copy the data */
        memcpy(page->local_addr, data, size);
        
        /* Downgrade to read-only (shared page) */
        if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_READ) != 0) {
            LOG_ERROR("mprotect (read) failed: %s", strerror(errno));
            pthread_mutex_unlock(&page->lock);
            return -1;
        }
    }
    
    page->state = DSM_PAGE_SHARED;
    page->version = version;
    
    pthread_mutex_unlock(&page->lock);
    
    /* Signal waiters */
    LOG_INFO("Signaling fault completion for page 0x%lx", global_addr);
    pthread_mutex_lock(&ctx->fault_mutex);
    ctx->fault_handled = 1;
    pthread_cond_broadcast(&ctx->fault_cond);
    pthread_mutex_unlock(&ctx->fault_mutex);
    
    LOG_INFO("Page 0x%lx installed successfully", global_addr);
    return 0;
}

int dsm_page_invalidate(uint64_t global_addr) {
    dsm_page_t *page = dsm_page_lookup(global_addr);
    
    if (!page) {
        return -1;
    }
    
    pthread_mutex_lock(&page->lock);
    
    if (page->local_addr) {
        mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_NONE);
    }
    page->state = DSM_PAGE_INVALID;
    
    pthread_mutex_unlock(&page->lock);
    
    LOG_DEBUG("Page 0x%lx invalidated", global_addr);
    return 0;
}

int dsm_page_mark_modified(uint64_t global_addr) {
    dsm_page_t *page = dsm_page_lookup(global_addr);
    
    if (!page) {
        return -1;
    }
    
    pthread_mutex_lock(&page->lock);
    page->state = DSM_PAGE_MODIFIED;
    page->dirty = true;
    page->version++;
    pthread_mutex_unlock(&page->lock);
    
    return 0;
}

/*============================================================================
 * Region Management
 *===========================================================================*/

dsm_region_t* dsm_region_create(size_t size, uint32_t owner_id, uint64_t global_base) {
    dsm_context_t *ctx = dsm_get_context();
    
    /* Round up to page size */
    size_t aligned_size = ((size + DSM_PAGE_SIZE - 1) / DSM_PAGE_SIZE) * DSM_PAGE_SIZE;
    
    /* Create mmap region */
    void *addr = mmap(NULL, aligned_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        LOG_ERROR("mmap failed: %s", strerror(errno));
        return NULL;
    }
    
    LOG_INFO("Created mmap region at %p, size=%zu", addr, aligned_size);
    
    /* If we own this region, enable write access */
    if (owner_id == ctx->local_node_id) {
        if (mprotect(addr, aligned_size, PROT_READ | PROT_WRITE) != 0) {
            LOG_ERROR("mprotect failed: %s", strerror(errno));
            munmap(addr, aligned_size);
            return NULL;
        }
        
        /* Zero the memory */
        memset(addr, 0, aligned_size);
    }
    
    dsm_region_t *region = (dsm_region_t*)calloc(1, sizeof(dsm_region_t));
    if (!region) {
        munmap(addr, aligned_size);
        return NULL;
    }
    
    region->base_addr = addr;
    region->total_size = aligned_size;
    region->page_count = aligned_size / DSM_PAGE_SIZE;
    region->owner_id = owner_id;
    region->global_base = global_base;
    region->next = NULL;
    
    /* Add to region list */
    if (!ctx->regions) {
        ctx->regions = region;
    } else {
        dsm_region_t *tail = ctx->regions;
        while (tail->next) tail = tail->next;
        tail->next = region;
    }
    
    /* Register pages */
    for (size_t i = 0; i < region->page_count; i++) {
        uint64_t page_global = global_base + i * DSM_PAGE_SIZE;
        void *page_local = (uint8_t*)addr + i * DSM_PAGE_SIZE;
        dsm_page_register(page_global, page_local, owner_id);
    }
    
    LOG_INFO("Region created: base=%p, size=%zu, pages=%zu, owner=%d, global_base=0x%lx",
             addr, aligned_size, region->page_count, owner_id, global_base);
    
    return region;
}

void dsm_region_destroy(dsm_region_t *region) {
    if (!region) {
        return;
    }
    
    dsm_context_t *ctx = dsm_get_context();
    
    /* Remove from list */
    if (ctx->regions == region) {
        ctx->regions = region->next;
    } else {
        dsm_region_t *prev = ctx->regions;
        while (prev && prev->next != region) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = region->next;
        }
    }
    
    if (region->base_addr) {
        munmap(region->base_addr, region->total_size);
    }
    
    free(region);
}

dsm_region_t* dsm_region_find(uint64_t global_addr) {
    dsm_context_t *ctx = dsm_get_context();
    
    dsm_region_t *region = ctx->regions;
    while (region) {
        if (global_addr >= region->global_base &&
            global_addr < region->global_base + region->total_size) {
            return region;
        }
        region = region->next;
    }
    
    return NULL;
}

void* dsm_global_to_local(uint64_t global_addr) {
    dsm_region_t *region = dsm_region_find(global_addr);
    if (!region) {
        return NULL;
    }
    
    uint64_t offset = global_addr - region->global_base;
    return (uint8_t*)region->base_addr + offset;
}

uint64_t dsm_local_to_global(void *local_addr) {
    dsm_context_t *ctx = dsm_get_context();
    
    dsm_region_t *region = ctx->regions;
    while (region) {
        uintptr_t local = (uintptr_t)local_addr;
        uintptr_t base = (uintptr_t)region->base_addr;
        uintptr_t end = base + region->total_size;
        if (local >= base && local < end) {
            uint64_t offset = local - base;
            return region->global_base + offset;
        }
        region = region->next;
    }
    
    return 0;
}

/*============================================================================
 * Memory Initialization
 *===========================================================================*/

int dsm_mem_init(uint32_t chunks) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_INFO("Initializing memory subsystem with %d chunks", chunks);
    
    /* Initialize page table */
    if (dsm_page_table_init() != 0) {
        return -1;
    }
    
    /* Initialize ownership table (used by master) */
    ownership_table_init();
    
    /* Calculate local region size */
    size_t region_size = chunks * DSM_PAGE_SIZE;
    
    /* Determine global base address for this node's region */
    uint64_t global_base;
    if (ctx->local_role == DSM_ROLE_MASTER) {
        /* Master allocates from start */
        global_base = ctx->next_global_addr;
        ctx->next_global_addr += region_size;
    } else {
        /* Workers use global address assigned by master during join */
        /* next_global_addr was set by JOIN_RESPONSE handler */
        global_base = ctx->next_global_addr;
        LOG_INFO("Worker using master-assigned global base: 0x%lx", global_base);
    }
    
    /* Create local region */
    ctx->local_region = dsm_region_create(region_size, ctx->local_node_id, global_base);
    if (!ctx->local_region) {
        LOG_ERROR("Failed to create local region");
        dsm_page_table_destroy();
        return -1;
    }
    
    /* Update next_global_addr to be after our region */
    if (ctx->local_role == DSM_ROLE_MASTER) {
        /* Master already updated next_global_addr above */
    } else {
        /* Workers update for any future local allocations */
        ctx->next_global_addr = global_base + region_size;
    }
    
    /* Register ownership for individual pages (master registers page-by-page) */
    if (ctx->local_role == DSM_ROLE_MASTER) {
        size_t num_pages = region_size / DSM_PAGE_SIZE;
        for (size_t i = 0; i < num_pages; i++) {
            dsm_ownership_register(global_base + i * DSM_PAGE_SIZE, DSM_PAGE_SIZE, ctx->local_node_id);
        }
    }
    
    LOG_INFO("Memory subsystem initialized: %zu bytes at global 0x%lx",
             region_size, global_base);
    return 0;
}

void dsm_mem_shutdown(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_DEBUG("Shutting down memory subsystem");
    
    /* Destroy all regions */
    while (ctx->regions) {
        dsm_region_t *region = ctx->regions;
        ctx->regions = region->next;
        
        if (region->base_addr) {
            munmap(region->base_addr, region->total_size);
        }
        free(region);
    }
    ctx->local_region = NULL;
    
    /* Destroy page table */
    dsm_page_table_destroy();
    
    /* Destroy ownership table */
    ownership_table_destroy();
    
    LOG_DEBUG("Memory subsystem shut down");
}

/*============================================================================
 * Public Memory API
 *===========================================================================*/

void* dsm_mem_alloc(size_t size) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (size == 0) {
        return NULL;
    }
    
    /* Round up to page size */
    size_t aligned_size = ((size + DSM_PAGE_SIZE - 1) / DSM_PAGE_SIZE) * DSM_PAGE_SIZE;
    size_t num_pages = aligned_size / DSM_PAGE_SIZE;
    
    LOG_DEBUG("Allocating %zu bytes (aligned: %zu, pages: %zu)", size, aligned_size, num_pages);
    
    /* FIRST: Try to allocate from existing free pages in pool */
    void *pool_addr = dsm_pool_alloc(num_pages);
    if (pool_addr) {
        LOG_INFO("[ALLOC] Allocated %zu bytes from pool at local %p", aligned_size, pool_addr);
        return pool_addr;
    }
    
    LOG_DEBUG("[ALLOC] Pool exhausted, creating new region");
    
    uint64_t global_addr;
    
    if (ctx->local_role == DSM_ROLE_MASTER) {
        /* Master allocates locally */
        pthread_mutex_lock(&ctx->alloc_mutex);
        global_addr = ctx->next_global_addr;
        ctx->next_global_addr += aligned_size;
        pthread_mutex_unlock(&ctx->alloc_mutex);
        
        /* Register ownership */
        dsm_ownership_register(global_addr, aligned_size, ctx->local_node_id);
        
        /* Create region */
        dsm_region_t *region = dsm_region_create(aligned_size, ctx->local_node_id, global_addr);
        if (!region) {
            return NULL;
        }
        
        /* Mark newly created pages as in_use */
        for (size_t i = 0; i < num_pages; i++) {
            dsm_page_t *page = dsm_page_lookup(global_addr + i * DSM_PAGE_SIZE);
            if (page) {
                page->in_use = true;
            }
        }
        
        LOG_INFO("Allocated %zu bytes at global 0x%lx, local %p",
                aligned_size, global_addr, region->base_addr);
        
        return region->base_addr;
    } else {
        /* Worker requests allocation from master */
        LOG_INFO("[ALLOC] Worker requesting %zu bytes from master", aligned_size);
        
        /* Build allocation request */
        static uint32_t request_id = 0;
        dsm_msg_alloc_request_t req;
        req.size = aligned_size;
        req.request_id = ++request_id;
        
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_ALLOC_REQUEST, 1, sizeof(req));
        
        if (dsm_tcp_send(1, &hdr, &req) != 0) {
            LOG_ERROR("[ALLOC] Failed to send allocation request to master");
            return NULL;
        }
        
        /* Wait for allocation response */
        /* For now, use a simple polling approach - the TCP handler will set ctx->last_alloc_response */
        pthread_mutex_lock(&ctx->alloc_mutex);
        ctx->alloc_pending = true;
        ctx->alloc_response_addr = 0;
        pthread_mutex_unlock(&ctx->alloc_mutex);
        
        /* Wait with timeout */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;
        
        pthread_mutex_lock(&ctx->alloc_mutex);
        while (ctx->alloc_pending) {
            int ret = pthread_cond_timedwait(&ctx->alloc_cond, &ctx->alloc_mutex, &ts);
            if (ret == ETIMEDOUT) {
                LOG_ERROR("[ALLOC] Timeout waiting for allocation response from master");
                ctx->alloc_pending = false;
                pthread_mutex_unlock(&ctx->alloc_mutex);
                return NULL;
            }
        }
        global_addr = ctx->alloc_response_addr;
        pthread_mutex_unlock(&ctx->alloc_mutex);
        
        if (global_addr == 0) {
            LOG_ERROR("[ALLOC] Master returned invalid address");
            return NULL;
        }
        
        LOG_INFO("[ALLOC] Master allocated global address 0x%lx for worker", global_addr);
        
        /* Create local region for this allocation */
        dsm_region_t *region = dsm_region_create(aligned_size, ctx->local_node_id, global_addr);
        if (!region) {
            return NULL;
        }
        
        /* Mark newly created pages as in_use */
        for (size_t i = 0; i < num_pages; i++) {
            dsm_page_t *page = dsm_page_lookup(global_addr + i * DSM_PAGE_SIZE);
            if (page) {
                page->in_use = true;
            }
        }
        
        LOG_INFO("Worker allocated %zu bytes at global 0x%lx, local %p",
                aligned_size, global_addr, region->base_addr);
        
        return region->base_addr;
    }
}

void dsm_mem_free(void *addr) {
    if (!addr) {
        return;
    }
    
    uint64_t global_addr = dsm_local_to_global(addr);
    if (global_addr == 0) {
        LOG_WARN("Attempted to free non-DSM address %p", addr);
        return;
    }
    
    dsm_region_t *region = dsm_region_find(global_addr);
    if (region) {
        LOG_DEBUG("Freeing region at global 0x%lx, local %p", global_addr, addr);
        dsm_region_destroy(region);
    }
}

/* Public API wrappers */
void* dsm_malloc(size_t size) {
    return dsm_mem_alloc(size);
}

void dsm_free(void *addr) {
    dsm_mem_free(addr);
}

/*============================================================================
 * Allocate at Specific Global Address (for TRUE DSM)
 *===========================================================================*/

void* dsm_malloc_at(uint64_t global_addr, size_t size) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (size == 0) {
        return NULL;
    }
    
    /* Round up to page size */
    size_t aligned_size = ((size + DSM_PAGE_SIZE - 1) / DSM_PAGE_SIZE) * DSM_PAGE_SIZE;
    size_t num_pages = aligned_size / DSM_PAGE_SIZE;
    
    LOG_INFO("Allocating %zu bytes at fixed global address 0x%lx", aligned_size, global_addr);
    
    /* FIRST: Check if this global address is within our existing pool */
    dsm_page_t *existing_page = dsm_page_lookup(global_addr);
    if (existing_page && 
        existing_page->owner_id == ctx->local_node_id && 
        !existing_page->in_use) {
        
        LOG_INFO("[MALLOC_AT] Found existing pool page at global 0x%lx, using it", global_addr);
        
        /* Mark pages as in_use */
        for (size_t i = 0; i < num_pages; i++) {
            dsm_page_t *page = dsm_page_lookup(global_addr + i * DSM_PAGE_SIZE);
            if (page && page->owner_id == ctx->local_node_id) {
                pthread_mutex_lock(&page->lock);
                page->in_use = true;
                page->state = DSM_PAGE_MODIFIED;
                pthread_mutex_unlock(&page->lock);
                
                /* Make accessible */
                if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_READ | PROT_WRITE) < 0) {
                    LOG_ERROR("[MALLOC_AT] mprotect failed: %s", strerror(errno));
                }
            }
        }
        
        /* Register in ownership table */
        dsm_ownership_register(global_addr, aligned_size, ctx->local_node_id);
        
        LOG_INFO("[MALLOC_AT] Reused pool page: global 0x%lx, local %p", 
                 global_addr, existing_page->local_addr);
        return existing_page->local_addr;
    }
    
    /* Not in pool - create new region */
    LOG_INFO("[MALLOC_AT] Creating new region at global 0x%lx", global_addr);
    
    /* Register ownership - this node owns these pages */
    dsm_ownership_register(global_addr, aligned_size, ctx->local_node_id);
    
    /* Create mmap region - dsm_region_create initially creates with PROT_NONE */
    dsm_region_t *region = dsm_region_create(aligned_size, ctx->local_node_id, global_addr);
    if (!region) {
        LOG_ERROR("Failed to create region at global 0x%lx", global_addr);
        return NULL;
    }
    
    /* Make pages readable/writable since we're the owner */
    if (mprotect(region->base_addr, aligned_size, PROT_READ | PROT_WRITE) < 0) {
        LOG_ERROR("mprotect failed: %s", strerror(errno));
        return NULL;
    }
    
    /* Update page states in page table to MODIFIED (owned and exclusive) and mark as in_use */
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t page_addr = global_addr + (i * DSM_PAGE_SIZE);
        dsm_page_t *page = dsm_page_lookup(page_addr);
        if (page) {
            page->state = DSM_PAGE_MODIFIED;
            page->in_use = true;
        }
    }
    
    LOG_INFO("Created owned region at global 0x%lx, local %p", global_addr, region->base_addr);
    return region->base_addr;
}

/*============================================================================
 * Map Remote Global Address (for TRUE DSM - triggers page fault on access)
 * 
 * This function maps a remote global address to local memory. Instead of
 * creating new memory, it REUSES a free page from the local pool.
 *===========================================================================*/

void* dsm_map_remote(uint64_t global_addr, size_t size) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (size == 0) {
        return NULL;
    }
    
    /* Round up to page size */
    size_t aligned_size = ((size + DSM_PAGE_SIZE - 1) / DSM_PAGE_SIZE) * DSM_PAGE_SIZE;
    size_t num_pages = aligned_size / DSM_PAGE_SIZE;
    
    LOG_INFO("[MAP_REMOTE] Mapping remote global address 0x%lx (%zu bytes, %zu pages)", 
             global_addr, aligned_size, num_pages);
    
    /* Check if we already have a page entry for this global address */
    dsm_page_t *existing_page = dsm_page_lookup(global_addr);
    if (existing_page) {
        LOG_INFO("[MAP_REMOTE] Page 0x%lx already exists: local=%p, owner=%d, state=%d",
                 global_addr, existing_page->local_addr, existing_page->owner_id, existing_page->state);
        
        /* If we already have a valid local mapping, just return it */
        if (existing_page->local_addr) {
            existing_page->in_use = true;
            LOG_INFO("[MAP_REMOTE] Returning existing local address %p", existing_page->local_addr);
            return existing_page->local_addr;
        }
    }
    
    /* Get the actual current owner from ownership table */
    uint32_t owner_id = 1;  /* Default: assume master owns it */
    if (ctx->local_role == DSM_ROLE_MASTER) {
        int actual_owner = dsm_ownership_get(global_addr);
        if (actual_owner > 0) {
            owner_id = actual_owner;
        }
    }
    
    /* Always create new region for remote pages to preserve global address space invariants */
    LOG_INFO("[MAP_REMOTE] Creating new region for remote page 0x%lx", global_addr);
    
    /* Don't register ownership - the remote owner already registered this page */
    /* dsm_ownership_register(global_addr, aligned_size, owner_id); */
    
    dsm_region_t *region = dsm_region_create(aligned_size, owner_id, global_addr);
    if (!region) {
        LOG_ERROR("Failed to create region for remote addr 0x%lx", global_addr);
        return NULL;
    }
    
    /* Mark newly created pages as in_use */
    for (size_t i = 0; i < num_pages; i++) {
        dsm_page_t *page = dsm_page_lookup(global_addr + i * DSM_PAGE_SIZE);
        if (page) {
            page->in_use = true;
        }
    }
    
    LOG_INFO("[MAP_REMOTE] Created new region: global 0x%lx → local %p (will fault on access)", 
             global_addr, region->base_addr);
    
    return region->base_addr;
}

/*============================================================================
 * Public Page Information API
 *===========================================================================*/

int dsm_get_page_owner(void *addr) {
    if (!addr) {
        LOG_DEBUG("[PAGE_OWNER] NULL address provided");
        return -1;
    }
    
    LOG_DEBUG("[PAGE_OWNER] Looking up owner for addr=%p", addr);
    
    dsm_page_t *page = dsm_page_lookup_local(addr);
    if (!page) {
        LOG_DEBUG("[PAGE_OWNER] Address %p not found in page table", addr);
        return -1;
    }
    
    LOG_INFO("[PAGE_OWNER] addr=%p => global=0x%lx, owner=%d, state=%d",
              addr, page->global_addr, page->owner_id, page->state);
    
    return (int)page->owner_id;
}

int dsm_get_page_info(void *addr, dsm_page_info_t *info) {
    if (!addr || !info) {
        LOG_DEBUG("[PAGE_INFO] NULL address or info pointer");
        return -1;
    }
    
    LOG_DEBUG("[PAGE_INFO] Looking up info for addr=%p", addr);
    
    dsm_page_t *page = dsm_page_lookup_local(addr);
    if (!page) {
        LOG_DEBUG("[PAGE_INFO] Address %p not found in page table", addr);
        return -1;
    }
    
    /* Fill in the info structure */
    info->owner_id    = page->owner_id;
    info->state       = (uint32_t)page->state;
    info->global_addr = page->global_addr;
    info->local_addr  = page->local_addr;
    info->size        = page->size;
    info->version     = page->version;
    info->dirty       = page->dirty ? 1 : 0;
    
    LOG_INFO("[PAGE_INFO] addr=%p => owner=%d, state=%s, global=0x%lx, ver=%lu, dirty=%d",
              addr, info->owner_id, dsm_page_state_to_string(info->state),
              info->global_addr, info->version, info->dirty);
    
    return 0;
}

const char* dsm_page_state_to_string(int state) {
    switch (state) {
        case DSM_PAGE_INVALID:  return "INVALID";
        case DSM_PAGE_SHARED:   return "SHARED";
        case DSM_PAGE_MODIFIED: return "MODIFIED/EXCLUSIVE";
        case DSM_PAGE_PENDING:  return "PENDING";
        default:                return "UNKNOWN";
    }
}

/*============================================================================
 * Copyset Management (tracking which nodes have copies of a page)
 *===========================================================================*/

void dsm_page_copyset_add(uint64_t global_addr, uint32_t node_id) {
    dsm_page_t *page = dsm_page_lookup(global_addr);
    if (!page || node_id >= DSM_MAX_NODES) return;
    
    pthread_mutex_lock(&page->lock);
    if (!page->copyset[node_id]) {
        page->copyset[node_id] = 1;
        page->copyset_count++;
        LOG_DEBUG("[COPYSET] Added node %d to copyset of page 0x%lx (count=%d)",
                  node_id, global_addr, page->copyset_count);
    }
    pthread_mutex_unlock(&page->lock);
}

void dsm_page_copyset_remove(uint64_t global_addr, uint32_t node_id) {
    dsm_page_t *page = dsm_page_lookup(global_addr);
    if (!page || node_id >= DSM_MAX_NODES) return;
    
    pthread_mutex_lock(&page->lock);
    if (page->copyset[node_id]) {
        page->copyset[node_id] = 0;
        page->copyset_count--;
        LOG_DEBUG("[COPYSET] Removed node %d from copyset of page 0x%lx (count=%d)",
                  node_id, global_addr, page->copyset_count);
    }
    pthread_mutex_unlock(&page->lock);
}

void dsm_page_copyset_clear(uint64_t global_addr) {
    dsm_page_t *page = dsm_page_lookup(global_addr);
    if (!page) return;
    
    pthread_mutex_lock(&page->lock);
    memset(page->copyset, 0, sizeof(page->copyset));
    page->copyset_count = 0;
    LOG_DEBUG("[COPYSET] Cleared copyset for page 0x%lx", global_addr);
    pthread_mutex_unlock(&page->lock);
}

/*============================================================================
 * Invalidate/Exclusive Ownership Protocol Implementation
 *===========================================================================*/

/**
 * Send page data with optional ownership transfer
 * Used for both read responses (shared copy) and write responses (exclusive)
 */
int dsm_page_send_with_ownership(uint32_t node_id, uint64_t global_addr, int transfer_ownership) {
    dsm_context_t *ctx = dsm_get_context();
    dsm_page_t *page = dsm_page_lookup(global_addr);
    
    if (!page || !page->local_addr) {
        LOG_ERROR("Page 0x%lx not found or not local", global_addr);
        return -1;
    }
    
    if (transfer_ownership) {
        LOG_INFO("[OWNERSHIP] Transferring ownership of page 0x%lx to node %d", global_addr, node_id);
        
        /* Build ownership transfer message */
        size_t msg_size = sizeof(dsm_msg_ownership_xfer_t) + DSM_PAGE_SIZE;
        uint8_t *buf = (uint8_t*)malloc(msg_size);
        if (!buf) {
            LOG_ERROR("Failed to allocate ownership transfer buffer");
            return -1;
        }
        
        dsm_msg_ownership_xfer_t *xfer = (dsm_msg_ownership_xfer_t*)buf;
        xfer->global_addr = global_addr;
        xfer->new_owner = node_id;
        xfer->old_owner = ctx->local_node_id;
        xfer->size = DSM_PAGE_SIZE;
        xfer->version = page->version + 1;  /* Increment version on ownership change */
        
        pthread_mutex_lock(&page->lock);
        memcpy(xfer->data, page->local_addr, DSM_PAGE_SIZE);
        
        /* We're giving up ownership - invalidate our local copy */
        if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_NONE) != 0) {
            LOG_WARN("mprotect PROT_NONE failed: %s", strerror(errno));
        }
        page->state = DSM_PAGE_INVALID;
        page->owner_id = node_id;  /* Update local record of who owns it */
        pthread_mutex_unlock(&page->lock);
        
        /* Update master's ownership table */
        if (ctx->local_role == DSM_ROLE_MASTER) {
            dsm_ownership_set(global_addr, node_id);
        }
        
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_OWNERSHIP_XFER, node_id, msg_size);
        int ret = dsm_tcp_send(node_id, &hdr, buf);
        free(buf);
        
        if (ret == 0) {
            LOG_INFO("[OWNERSHIP] Page 0x%lx ownership transferred to node %d", global_addr, node_id);
        }
        return ret;
    } else {
        /* Just send a read-only copy (existing dsm_page_send behavior) */
        return dsm_page_send(node_id, global_addr);
    }
}

/**
 * Handle incoming ownership transfer (receiver becomes exclusive owner)
 */
int dsm_handle_ownership_xfer(uint64_t global_addr, void *data, size_t size, 
                               uint64_t version, uint32_t old_owner) {
    dsm_context_t *ctx = dsm_get_context();
    dsm_page_t *page = dsm_page_lookup(global_addr);
    
    if (!page) {
        LOG_ERROR("[OWNERSHIP] Page 0x%lx not registered for ownership transfer", global_addr);
        return -1;
    }
    
    LOG_INFO("[OWNERSHIP] Receiving ownership of page 0x%lx from node %d", global_addr, old_owner);
    
    pthread_mutex_lock(&page->lock);
    
    /* Enable write access - we're now the exclusive owner */
    if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
        LOG_ERROR("mprotect failed during ownership transfer: %s", strerror(errno));
        pthread_mutex_unlock(&page->lock);
        return -1;
    }
    
    /* Copy the page data */
    memcpy(page->local_addr, data, size);
    
    /* Update page metadata - we are now the exclusive owner */
    page->state = DSM_PAGE_MODIFIED;  /* MODIFIED = exclusive write access */
    page->owner_id = ctx->local_node_id;
    page->version = version;
    
    /* Clear copyset and set ourselves as the only copy holder */
    memset(page->copyset, 0, sizeof(page->copyset));
    page->copyset[ctx->local_node_id] = 1;
    page->copyset_count = 1;
    
    pthread_mutex_unlock(&page->lock);
    
    /* Signal any waiting fault handler */
    pthread_mutex_lock(&ctx->fault_mutex);
    pthread_cond_broadcast(&ctx->fault_cond);
    pthread_mutex_unlock(&ctx->fault_mutex);
    
    LOG_INFO("[OWNERSHIP] Now exclusive owner of page 0x%lx (state=MODIFIED)", global_addr);
    return 0;
}

/**
 * Handle page invalidate request (another node is taking ownership)
 */
int dsm_handle_page_invalidate(uint64_t global_addr, uint32_t new_owner) {
    dsm_context_t *ctx = dsm_get_context();
    dsm_page_t *page = dsm_page_lookup(global_addr);
    
    if (!page) {
        /* Page not mapped locally - just ACK */
        LOG_DEBUG("[INVALIDATE] Page 0x%lx not local, sending ACK", global_addr);
        goto send_ack;
    }
    
    LOG_INFO("[INVALIDATE] Invalidating page 0x%lx (new_owner=%d)", global_addr, new_owner);
    
    pthread_mutex_lock(&page->lock);
    
    /* Remove read/write permissions */
    if (page->local_addr && page->state != DSM_PAGE_INVALID) {
        if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_NONE) != 0) {
            LOG_WARN("mprotect PROT_NONE failed: %s", strerror(errno));
        }
    }
    
    /* Update state */
    page->state = DSM_PAGE_INVALID;
    page->owner_id = new_owner;  /* Record who the new owner is */
    
    pthread_mutex_unlock(&page->lock);
    
    LOG_INFO("[INVALIDATE] Page 0x%lx invalidated, new owner is node %d", global_addr, new_owner);

send_ack:
    /* Send acknowledgment back */
    {
        dsm_msg_invalidate_ack_t ack;
        ack.global_addr = global_addr;
        ack.acking_node = ctx->local_node_id;
        
        /* Send to master (who coordinated the invalidation) */
        uint32_t master_id = 1;  /* Master is always node 1 */
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_INVALIDATE_ACK, master_id, sizeof(ack));
        dsm_tcp_send(master_id, &hdr, &ack);
    }
    
    return 0;
}

/**
 * Handle invalidate ACK (master receives this)
 */
int dsm_handle_invalidate_ack(uint32_t from_node, uint64_t global_addr) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_DEBUG("[INVALIDATE_ACK] Received ACK from node %d for page 0x%lx", from_node, global_addr);
    
    pthread_mutex_lock(&ctx->invalidate_mutex);
    
    /* Check if this ACK is for the page we're waiting on */
    if (ctx->invalidate_addr == global_addr && ctx->invalidate_pending > 0) {
        ctx->invalidate_pending--;
        LOG_DEBUG("[INVALIDATE_ACK] Pending ACKs remaining: %d", ctx->invalidate_pending);
        
        /* Signal if all ACKs received */
        if (ctx->invalidate_pending == 0) {
            pthread_cond_signal(&ctx->invalidate_cond);
        }
    }
    
    pthread_mutex_unlock(&ctx->invalidate_mutex);
    return 0;
}

/**
 * Handle page write request (master receives this from node wanting write access)
 * This orchestrates the invalidation of all other copies and ownership transfer
 */
int dsm_handle_page_write_request(uint32_t from_node, uint64_t global_addr) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_INFO("[WRITE_REQ] Node %d requesting write access to page 0x%lx", from_node, global_addr);
    
    /* Find the current owner */
    int current_owner = dsm_ownership_get(global_addr);
    if (current_owner < 0) {
        LOG_ERROR("[WRITE_REQ] Page 0x%lx not found in ownership table", global_addr);
        return -1;
    }
    
    LOG_INFO("[WRITE_REQ] Current owner is node %d (we are node %d)", current_owner, ctx->local_node_id);
    
    /* Get the page's copyset (which nodes have copies) */
    dsm_page_t *page = dsm_page_lookup(global_addr);
    if (!page) {
        LOG_ERROR("[WRITE_REQ] Page 0x%lx not in local page table - will use direct send", global_addr);
        /* Master may not have the page mapped, but still needs to coordinate transfer */
        /* Update ownership and tell requester to get data from current owner */
        dsm_ownership_set(global_addr, from_node);
        
        if ((uint32_t)current_owner == ctx->local_node_id) {
            LOG_ERROR("[WRITE_REQ] BUG: Master claims to own page but can't find it in page table!");
            return -1;
        }
        
        /* Forward write request to actual owner */
        LOG_INFO("[WRITE_REQ] Forwarding to current owner node %d", current_owner);
        dsm_msg_page_write_request_t xfer_req;
        xfer_req.global_addr = global_addr;
        xfer_req.requesting_node = from_node;
        
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_PAGE_WRITE_REQ, current_owner, sizeof(xfer_req));
        return dsm_tcp_send(current_owner, &hdr, &xfer_req);
    }
    
    /* Count how many nodes need to be invalidated (everyone except the requester) */
    pthread_mutex_lock(&page->lock);
    uint32_t nodes_to_invalidate = 0;
    
    /* Build invalidate message */
    dsm_msg_page_invalidate_t inv_msg;
    inv_msg.global_addr = global_addr;
    inv_msg.new_owner = from_node;
    
    /* Send invalidate to all nodes that have copies (except requester) */
    for (uint32_t i = 0; i < DSM_MAX_NODES; i++) {
        if (page->copyset[i] && i != from_node) {
            nodes_to_invalidate++;
        }
    }
    
    /* Also check if WE have a shared copy not tracked in copyset */
    int we_are_owner = ((uint32_t)current_owner == ctx->local_node_id);
    int we_have_untracked_copy = (!page->copyset[ctx->local_node_id] && 
                                   page->state == DSM_PAGE_SHARED && 
                                   ctx->local_node_id != from_node &&
                                   !we_are_owner);
    pthread_mutex_unlock(&page->lock);
    
    LOG_INFO("[WRITE_REQ] Need to invalidate %d node(s) (+ self=%d)", 
             nodes_to_invalidate, we_have_untracked_copy);
    
    /* Handle self-invalidation FIRST (even if nodes_to_invalidate == 0)
     * This handles the case where master has a SHARED copy not tracked in copyset
     */
    pthread_mutex_lock(&page->lock);
    int we_have_copy = page->copyset[ctx->local_node_id] || 
                       (page->state == DSM_PAGE_SHARED);
    if (we_have_copy && ctx->local_node_id != from_node && !we_are_owner) {
        LOG_INFO("[WRITE_REQ] Self-invalidating local copy (state=%d, we're not owner)", page->state);
        page->state = DSM_PAGE_INVALID;
        page->owner_id = from_node;  /* Update owner to new owner */
        page->copyset[ctx->local_node_id] = 0;
        if (page->copyset_count > 0) page->copyset_count--;
        /* Protect memory from access */
        if (page->local_addr) {
            mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_NONE);
        }
    } else if (we_are_owner) {
        LOG_INFO("[WRITE_REQ] We are the owner - will invalidate during transfer");
    }
    pthread_mutex_unlock(&page->lock);

    if (nodes_to_invalidate > 0) {
        /* Count remote nodes to invalidate (not ourselves) */
        uint32_t remote_invalidates = 0;
        pthread_mutex_lock(&page->lock);
        for (uint32_t i = 0; i < DSM_MAX_NODES; i++) {
            if (page->copyset[i] && i != from_node && i != ctx->local_node_id) {
                remote_invalidates++;
            }
        }
        pthread_mutex_unlock(&page->lock);
        
        /* Set up to wait for ACKs from remote nodes only */
        if (remote_invalidates > 0) {
            pthread_mutex_lock(&ctx->invalidate_mutex);
            ctx->invalidate_addr = global_addr;
            ctx->invalidate_pending = remote_invalidates;
            pthread_mutex_unlock(&ctx->invalidate_mutex);
        }
        
        /* Send invalidate messages to remote nodes (self already handled above) */
        pthread_mutex_lock(&page->lock);
        for (uint32_t i = 0; i < DSM_MAX_NODES; i++) {
            if (page->copyset[i] && i != from_node && i != ctx->local_node_id) {
                LOG_INFO("[WRITE_REQ] Sending INVALIDATE to node %d", i);
                dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_PAGE_INVALIDATE, i, sizeof(inv_msg));
                dsm_tcp_send(i, &hdr, &inv_msg);
            }
        }
        pthread_mutex_unlock(&page->lock);
        
        /* Wait for all ACKs from remote nodes (with timeout) */
        if (remote_invalidates > 0) {
            pthread_mutex_lock(&ctx->invalidate_mutex);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 10;  /* 10 second timeout */
            
            while (ctx->invalidate_pending > 0) {
                int ret = pthread_cond_timedwait(&ctx->invalidate_cond, &ctx->invalidate_mutex, &ts);
                if (ret == ETIMEDOUT) {
                    LOG_ERROR("[WRITE_REQ] Timeout waiting for invalidate ACKs");
                    pthread_mutex_unlock(&ctx->invalidate_mutex);
                    return -1;
                }
            }
            pthread_mutex_unlock(&ctx->invalidate_mutex);
            
            LOG_INFO("[WRITE_REQ] All invalidate ACKs received");
        } else {
            LOG_INFO("[WRITE_REQ] No remote invalidations needed");
        }
    }
    
    /* Clear the copyset since we're transferring exclusive ownership */
    dsm_page_copyset_clear(global_addr);
    
    /* Update ownership in master's table */
    dsm_ownership_set(global_addr, from_node);
    
    /* If master owns the page, transfer directly */
    if ((uint32_t)current_owner == ctx->local_node_id) {
        LOG_INFO("[WRITE_REQ] Master owns page, transferring directly to node %d", from_node);
        return dsm_page_send_with_ownership(from_node, global_addr, 1);
    } else {
        /* Tell the current owner to transfer to the requester */
        LOG_INFO("[WRITE_REQ] Telling node %d to transfer page to node %d", current_owner, from_node);
        
        dsm_msg_page_write_request_t xfer_req;
        xfer_req.global_addr = global_addr;
        xfer_req.requesting_node = from_node;
        
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_PAGE_WRITE_REQ, current_owner, sizeof(xfer_req));
        return dsm_tcp_send(current_owner, &hdr, &xfer_req);
    }
}

/**
 * Request write access to a page (called by non-owner wanting to write)
 */
int dsm_page_request_write(dsm_page_t *page) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!page) return -1;
    
    LOG_INFO("[WRITE_REQ] Requesting write access to page 0x%lx", page->global_addr);
    
    /* Build write request message */
    dsm_msg_page_write_request_t req;
    req.global_addr = page->global_addr;
    req.requesting_node = ctx->local_node_id;
    
    /* Send to master (who coordinates ownership transfer) */
    uint32_t master_id = 1;
    dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_PAGE_WRITE_REQ, master_id, sizeof(req));
    
    /* Mark page as pending */
    pthread_mutex_lock(&page->lock);
    page->state = DSM_PAGE_PENDING;
    pthread_mutex_unlock(&page->lock);
    
    if (dsm_tcp_send(master_id, &hdr, &req) != 0) {
        LOG_ERROR("[WRITE_REQ] Failed to send write request to master");
        pthread_mutex_lock(&page->lock);
        page->state = DSM_PAGE_INVALID;
        pthread_mutex_unlock(&page->lock);
        return -1;
    }
    
    /* Wait for ownership transfer (the TCP thread will handle OWNERSHIP_XFER message) */
    pthread_mutex_lock(&ctx->fault_mutex);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    
    while (page->state == DSM_PAGE_PENDING && ctx->running) {
        int ret = pthread_cond_timedwait(&ctx->fault_cond, &ctx->fault_mutex, &ts);
        if (ret == ETIMEDOUT) {
            LOG_ERROR("[WRITE_REQ] Timeout waiting for ownership transfer");
            pthread_mutex_unlock(&ctx->fault_mutex);
            pthread_mutex_lock(&page->lock);
            page->state = DSM_PAGE_INVALID;
            pthread_mutex_unlock(&page->lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&ctx->fault_mutex);
    
    LOG_INFO("[WRITE_REQ] Write access granted for page 0x%lx", page->global_addr);
    return 0;
}

/*============================================================================
 * Debug Functions
 *===========================================================================*/

void dsm_debug_dump_state(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                    DSM DEBUG STATE DUMP                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    
    /* Node Information */
    printf("\n┌─── Node Information ───────────────────────────────────────────────┐\n");
    printf("│ Local Node ID:     %u\n", ctx->local_node_id);
    printf("│ Role:              %s\n", ctx->local_role == DSM_ROLE_MASTER ? "MASTER" : "WORKER");
    printf("│ Master FD:         %d\n", ctx->master_fd);
    printf("│ Running:           %s\n", ctx->running ? "YES" : "NO");
    printf("│ Next Global Addr:  0x%lx\n", ctx->next_global_addr);
    printf("└────────────────────────────────────────────────────────────────────┘\n");
    
    /* Node Table */
    printf("\n┌─── Node Table ─────────────────────────────────────────────────────┐\n");
    uint32_t node_count = dsm_node_table_count();
    printf("│ Total Nodes: %u\n", node_count);
    printf("│\n");
    for (uint32_t i = 1; i <= node_count; i++) {
        dsm_node_t *node = dsm_node_table_get(i);
        if (node) {
            printf("│ Node %u: ip=%s, port=%d, fd=%d, role=%s, chunks=%u\n",
                   node->node_id, node->ip, node->port, node->tcp_fd,
                   node->role == DSM_ROLE_MASTER ? "MASTER" : "WORKER",
                   node->chunks);
        }
    }
    printf("└────────────────────────────────────────────────────────────────────┘\n");
    
    /* Page Table */
    printf("\n┌─── Page Table ─────────────────────────────────────────────────────┐\n");
    if (ctx->page_table) {
        pthread_rwlock_rdlock(&ctx->page_table->rwlock);
        printf("│ Total Pages: %zu\n", ctx->page_table->count);
        printf("│\n");
        for (size_t i = 0; i < ctx->page_table->count && i < 20; i++) {
            dsm_page_t *page = ctx->page_table->pages[i];
            if (page) {
                printf("│ [%2zu] global=0x%08lx local=%p owner=%u state=%-8s ver=%lu %s",
                       i, page->global_addr, page->local_addr, page->owner_id,
                       dsm_page_state_to_string(page->state), page->version,
                       page->in_use ? "[IN_USE]" : "[FREE]");
                
                /* Show copyset if not empty */
                if (page->copyset_count > 0) {
                    printf(" copyset=[");
                    int first = 1;
                    for (uint32_t j = 0; j < DSM_MAX_NODES; j++) {
                        if (page->copyset[j]) {
                            printf("%s%u", first ? "" : ",", j);
                            first = 0;
                        }
                    }
                    printf("]");
                }
                printf("\n");
            }
        }
        if (ctx->page_table->count > 20) {
            printf("│ ... (%zu more pages)\n", ctx->page_table->count - 20);
        }
        pthread_rwlock_unlock(&ctx->page_table->rwlock);
    } else {
        printf("│ (page table not initialized)\n");
    }
    printf("└────────────────────────────────────────────────────────────────────┘\n");
    
    /* Memory Regions */
    printf("\n┌─── Memory Regions ─────────────────────────────────────────────────┐\n");
    int region_count = 0;
    dsm_region_t *region = ctx->regions;
    while (region) {
        printf("│ Region %d: base=%p size=%zu pages=%zu owner=%u global=0x%lx\n",
               region_count++, region->base_addr, region->total_size,
               region->page_count, region->owner_id, region->global_base);
        region = region->next;
    }
    if (region_count == 0) {
        printf("│ (no regions)\n");
    }
    printf("└────────────────────────────────────────────────────────────────────┘\n");
    
    /* Active Locks */
    printf("\n┌─── Active Locks ───────────────────────────────────────────────────┐\n");
    int active_locks = 0;
    for (int i = 0; i < DSM_MAX_LOCKS; i++) {
        dsm_lock_t *lock = &ctx->locks[i];
        if (lock->locked) {
            printf("│ Lock %d: holder=%u waiters=%u\n", 
                   i, lock->holder_id, lock->waiter_count);
            active_locks++;
        }
    }
    if (active_locks == 0) {
        printf("│ (no active locks)\n");
    }
    printf("└────────────────────────────────────────────────────────────────────┘\n");
    
    /* Pending Operations */
    printf("\n┌─── Pending Operations ─────────────────────────────────────────────┐\n");
    printf("│ Fault Running:      %s\n", ctx->fault_running ? "YES" : "NO");
    printf("│ Invalidate Pending: %u (addr=0x%lx)\n",
           ctx->invalidate_pending, ctx->invalidate_addr);
    printf("└────────────────────────────────────────────────────────────────────┘\n");
    
    printf("\n");
}

/*============================================================================
 * Local Pages API Implementation
 *===========================================================================*/

int dsm_get_local_pages(dsm_local_page_t **pages, uint32_t *count) {
    if (!pages || !count) {
        return -1;
    }
    
    *pages = NULL;
    *count = 0;
    
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->page_table) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&ctx->page_table->rwlock);
    
    size_t total = ctx->page_table->count;
    if (total == 0) {
        pthread_rwlock_unlock(&ctx->page_table->rwlock);
        return 0;
    }
    
    /* Allocate output array */
    *pages = (dsm_local_page_t*)malloc(total * sizeof(dsm_local_page_t));
    if (!*pages) {
        pthread_rwlock_unlock(&ctx->page_table->rwlock);
        return -1;
    }
    
    /* Copy page info */
    uint32_t actual_count = 0;
    for (size_t i = 0; i < total; i++) {
        dsm_page_t *page = ctx->page_table->pages[i];
        if (page && page->local_addr) {  /* Only include pages with valid local mapping */
            (*pages)[actual_count].global_addr = page->global_addr;
            (*pages)[actual_count].local_addr = page->local_addr;
            (*pages)[actual_count].size = page->size;
            (*pages)[actual_count].owner_id = page->owner_id;
            (*pages)[actual_count].state = page->state;
            (*pages)[actual_count].in_use = page->in_use;
            actual_count++;
        }
    }
    
    pthread_rwlock_unlock(&ctx->page_table->rwlock);
    
    /* Resize if we skipped some pages */
    if (actual_count < total && actual_count > 0) {
        dsm_local_page_t *resized = realloc(*pages, actual_count * sizeof(dsm_local_page_t));
        if (resized) {
            *pages = resized;
        }
    } else if (actual_count == 0) {
        free(*pages);
        *pages = NULL;
    }
    
    *count = actual_count;
    return 0;
}

/*============================================================================
 * Global/Cluster Page Discovery API Implementation
 *===========================================================================*/

int dsm_get_global_pages(dsm_global_page_t **pages, uint32_t *count) {
    if (!pages || !count) {
        return -1;
    }
    
    *pages = NULL;
    *count = 0;
    
    dsm_context_t *ctx = dsm_get_context();
    
    if (ctx->local_role == DSM_ROLE_MASTER) {
        /* Master: collect from local ownership table */
        uint32_t local_count = 0;
        dsm_msg_list_pages_t *msg = dsm_ownership_collect_all(&local_count);
        
        if (!msg) {
            LOG_ERROR("[DISCOVER] Failed to collect ownership table");
            return -1;
        }
        
        if (local_count == 0) {
            free(msg);
            *count = 0;
            return 0;
        }
        
        /* Convert to public format */
        *pages = (dsm_global_page_t*)malloc(local_count * sizeof(dsm_global_page_t));
        if (!*pages) {
            free(msg);
            return -1;
        }
        
        for (uint32_t i = 0; i < local_count; i++) {
            (*pages)[i].global_addr = msg->pages[i].global_addr;
            (*pages)[i].size = msg->pages[i].size;
            (*pages)[i].owner_id = msg->pages[i].owner_id;
        }
        
        *count = local_count;
        free(msg);
        
        LOG_INFO("[DISCOVER] Master returned %u pages from local table", local_count);
        return 0;
    }
    
    /* Worker: request from master */
    LOG_INFO("[DISCOVER] Requesting page list from master...");
    
    /* Clear previous response */
    pthread_mutex_lock(&g_list_pages_mutex);
    g_list_pages_ready = 0;
    if (g_list_pages_response) {
        free(g_list_pages_response);
        g_list_pages_response = NULL;
    }
    pthread_mutex_unlock(&g_list_pages_mutex);
    
    /* Send request to master */
    dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_LIST_PAGES_REQ, 1, 0);
    if (dsm_tcp_send(1, &hdr, NULL) != 0) {
        LOG_ERROR("[DISCOVER] Failed to send LIST_PAGES_REQ to master");
        return -1;
    }
    
    /* Wait for response with timeout */
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 5;  /* 5 second timeout */
    
    pthread_mutex_lock(&g_list_pages_mutex);
    while (!g_list_pages_ready) {
        int ret = pthread_cond_timedwait(&g_list_pages_cond, &g_list_pages_mutex, &timeout);
        if (ret == ETIMEDOUT) {
            LOG_ERROR("[DISCOVER] Timeout waiting for LIST_PAGES response");
            pthread_mutex_unlock(&g_list_pages_mutex);
            return -1;
        }
    }
    
    /* Process response */
    if (!g_list_pages_response) {
        pthread_mutex_unlock(&g_list_pages_mutex);
        LOG_ERROR("[DISCOVER] No response received");
        return -1;
    }
    
    uint32_t resp_count = g_list_pages_response->count;
    
    if (resp_count == 0) {
        pthread_mutex_unlock(&g_list_pages_mutex);
        *count = 0;
        return 0;
    }
    
    /* Convert to public format */
    *pages = (dsm_global_page_t*)malloc(resp_count * sizeof(dsm_global_page_t));
    if (!*pages) {
        pthread_mutex_unlock(&g_list_pages_mutex);
        return -1;
    }
    
    for (uint32_t i = 0; i < resp_count; i++) {
        (*pages)[i].global_addr = g_list_pages_response->pages[i].global_addr;
        (*pages)[i].size = g_list_pages_response->pages[i].size;
        (*pages)[i].owner_id = g_list_pages_response->pages[i].owner_id;
    }
    
    *count = resp_count;
    pthread_mutex_unlock(&g_list_pages_mutex);
    
    LOG_INFO("[DISCOVER] Received %u pages from master", resp_count);
    return 0;
}
