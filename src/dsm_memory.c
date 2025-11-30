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
#include <sys/mman.h>

/*============================================================================
 * Constants
 *===========================================================================*/

#define PAGE_TABLE_INITIAL_CAPACITY     1024
#define OWNERSHIP_TABLE_SIZE            4096

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
    uint32_t hash = ownership_hash(global_addr);
    
    pthread_rwlock_rdlock(&g_ownership_lock);
    
    ownership_entry_t *entry = g_ownership_table[hash];
    while (entry) {
        if (global_addr >= entry->global_addr && 
            global_addr < entry->global_addr + entry->size) {
            int owner = entry->owner_id;
            pthread_rwlock_unlock(&g_ownership_lock);
            return owner;
        }
        entry = entry->next;
    }
    
    pthread_rwlock_unlock(&g_ownership_lock);
    return -1;
}

int dsm_ownership_set(uint64_t global_addr, uint32_t owner_id) {
    uint32_t hash = ownership_hash(global_addr);
    
    pthread_rwlock_wrlock(&g_ownership_lock);
    
    ownership_entry_t *entry = g_ownership_table[hash];
    while (entry) {
        if (global_addr >= entry->global_addr && 
            global_addr < entry->global_addr + entry->size) {
            entry->owner_id = owner_id;
            pthread_rwlock_unlock(&g_ownership_lock);
            LOG_DEBUG("Updated ownership: addr=0x%lx, new_owner=%d", global_addr, owner_id);
            return 0;
        }
        entry = entry->next;
    }
    
    pthread_rwlock_unlock(&g_ownership_lock);
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
    pthread_mutex_init(&page->lock, NULL);
    
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
    
    ctx->page_table->pages[ctx->page_table->count++] = page;
    
    pthread_rwlock_unlock(&ctx->page_table->rwlock);
    
    LOG_DEBUG("Registered page: global=0x%lx, local=%p, owner=%d", 
             global_addr, local_addr, owner_id);
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
        return NULL;
    }
    
    /* Align address to page boundary */
    uintptr_t addr = (uintptr_t)local_addr & ~(DSM_PAGE_SIZE - 1);
    
    pthread_rwlock_rdlock(&ctx->page_table->rwlock);
    
    dsm_page_t *result = NULL;
    for (size_t i = 0; i < ctx->page_table->count; i++) {
        if ((uintptr_t)ctx->page_table->pages[i]->local_addr == addr) {
            result = ctx->page_table->pages[i];
            break;
        }
    }
    
    pthread_rwlock_unlock(&ctx->page_table->rwlock);
    return result;
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
    dsm_page_t *page = dsm_page_lookup(global_addr);
    
    if (!page || !page->local_addr) {
        LOG_ERROR("Page 0x%lx not found or not local", global_addr);
        return -1;
    }
    
    LOG_INFO("Sending page 0x%lx to node %d", global_addr, node_id);
    
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
        /* Workers get address from master during join */
        /* For now, use node_id based offset */
        global_base = ctx->next_global_addr + ctx->local_node_id * region_size;
    }
    
    /* Create local region */
    ctx->local_region = dsm_region_create(region_size, ctx->local_node_id, global_base);
    if (!ctx->local_region) {
        LOG_ERROR("Failed to create local region");
        dsm_page_table_destroy();
        return -1;
    }
    
    /* Register ownership for local pages (if master) */
    if (ctx->local_role == DSM_ROLE_MASTER) {
        dsm_ownership_register(global_base, region_size, ctx->local_node_id);
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
    
    LOG_DEBUG("Allocating %zu bytes (aligned: %zu)", size, aligned_size);
    
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
        
        LOG_INFO("Allocated %zu bytes at global 0x%lx, local %p",
                aligned_size, global_addr, region->base_addr);
        
        return region->base_addr;
    } else {
        /* Worker requests from master */
        /* TODO: Implement proper request/response with condition variable */
        /* For now, just allocate locally with reserved address range */
        
        pthread_mutex_lock(&ctx->alloc_mutex);
        global_addr = ctx->next_global_addr;
        ctx->next_global_addr += aligned_size;
        pthread_mutex_unlock(&ctx->alloc_mutex);
        
        dsm_region_t *region = dsm_region_create(aligned_size, ctx->local_node_id, global_addr);
        if (!region) {
            return NULL;
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
    
    LOG_INFO("Allocating %zu bytes at fixed global address 0x%lx", aligned_size, global_addr);
    
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
    
    /* Update page states in page table to SHARED (owned and valid) */
    size_t num_pages = aligned_size / DSM_PAGE_SIZE;
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t page_addr = global_addr + (i * DSM_PAGE_SIZE);
        dsm_page_t *page = dsm_page_lookup(page_addr);
        if (page) {
            page->state = DSM_PAGE_SHARED;  /* Owned and readable */
        }
    }
    
    LOG_INFO("Created owned region at global 0x%lx, local %p", global_addr, region->base_addr);
    return region->base_addr;
}

/*============================================================================
 * Map Remote Global Address (for TRUE DSM - triggers page fault on access)
 *===========================================================================*/

void* dsm_map_remote(uint64_t global_addr, size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    /* Round up to page size */
    size_t aligned_size = ((size + DSM_PAGE_SIZE - 1) / DSM_PAGE_SIZE) * DSM_PAGE_SIZE;
    
    LOG_INFO("Mapping remote global address 0x%lx (%zu bytes)", global_addr, aligned_size);
    
    /* Find who owns this address - for the test, master (node 1) owns 0x2000000 range */
    uint32_t owner_id = 1;  /* Master owns the shared test page */
    
    /* Register in ownership table pointing to remote owner */
    dsm_ownership_register(global_addr, aligned_size, owner_id);
    
    /* Create mmap region with PROT_NONE - access will trigger SIGSEGV */
    dsm_region_t *region = dsm_region_create(aligned_size, owner_id, global_addr);
    if (!region) {
        LOG_ERROR("Failed to create region for remote addr 0x%lx", global_addr);
        return NULL;
    }
    
    /* Pages are created with PROT_NONE, state=INVALID by dsm_region_create */
    /* First access will trigger SIGSEGV → page fault handler → fetch from owner */
    
    LOG_INFO("Mapped remote region: global 0x%lx → local %p (will fault on access)", 
             global_addr, region->base_addr);
    
    return region->base_addr;
}
