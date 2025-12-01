/**
 * @file dsm_pagefault.c
 * @brief SIGSEGV handler for demand paging
 * 
 * Strategy: Minimal signal handler + eventfd to wake network thread
 * The signal handler only writes to eventfd and sets the fault address.
 * The fault handler thread does the actual network I/O.
 */

#include "dsm_types.h"
#include "dsm_network.h"
#include "dsm_memory.h"
#include "dsm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

/*============================================================================
 * Static Variables
 *===========================================================================*/

static struct sigaction g_old_sigsegv_action;
static volatile sig_atomic_t g_in_handler = 0;
static pthread_t g_fault_handler_thread;
static volatile int g_fault_running = 0;

/*============================================================================
 * Helper: Check if Address is in DSM
 *===========================================================================*/

int dsm_is_dsm_address(void *addr) {
    dsm_context_t *ctx = dsm_get_context();
    uintptr_t check_addr = (uintptr_t)addr;
    
    dsm_region_t *region = ctx->regions;
    while (region) {
        uintptr_t base = (uintptr_t)region->base_addr;
        uintptr_t end = base + region->total_size;
        if (check_addr >= base && check_addr < end) {
            return 1;
        }
        region = region->next;
    }
    
    return 0;
}

/*============================================================================
 * SIGSEGV Handler (Minimal - Async-Signal-Safe)
 *===========================================================================*/

static void sigsegv_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)ucontext;
    
    dsm_context_t *ctx = dsm_get_context();
    void *fault_addr = info->si_addr;
    
    /* Prevent recursive handling */
    if (g_in_handler) {
        /* Re-raise to default handler */
        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
        return;
    }
    g_in_handler = 1;
    
    /* Check if this is a DSM address */
    /* Note: This check must be simple - we can't use complex functions here */
    dsm_region_t *region = ctx->regions;
    int is_dsm = 0;
    uintptr_t fault_uaddr = (uintptr_t)fault_addr;
    while (region) {
        uintptr_t base = (uintptr_t)region->base_addr;
        uintptr_t end = base + region->total_size;
        if (fault_uaddr >= base && fault_uaddr < end) {
            is_dsm = 1;
            break;
        }
        region = region->next;
    }
    
    if (!is_dsm) {
        /* Not a DSM address - restore old handler and re-raise */
        g_in_handler = 0;
        sigaction(SIGSEGV, &g_old_sigsegv_action, NULL);
        raise(SIGSEGV);
        return;
    }
    
    /* Store fault address for handler thread */
    ctx->pending_fault_addr = fault_addr;
    ctx->fault_handled = 0;
    ctx->fault_result = 0;
    
    /* Signal the fault handler thread via eventfd */
    /* eventfd_write is async-signal-safe */
    uint64_t val = 1;
    ssize_t ret = write(ctx->fault_eventfd, &val, sizeof(val));
    (void)ret; /* Ignore return in signal handler */
    
    /* Wait for fault to be handled */
    /* We spin here because we can't use pthread primitives in signal handler */
    /* The fault handler thread will set fault_handled = 1 when done */
    while (!ctx->fault_handled && ctx->running) {
        /* Busy wait - not ideal but necessary for signal safety */
        for (volatile int i = 0; i < 1000; i++);
    }
    
    /* Check if fault handling failed */
    if (ctx->fault_result != 0) {
        /* Handler failed - restore old handler and re-raise */
        g_in_handler = 0;
        sigaction(SIGSEGV, &g_old_sigsegv_action, NULL);
        raise(SIGSEGV);
        return;
    }
    
    g_in_handler = 0;
    
    /* Return to retry the faulting instruction */
}

/*============================================================================
 * Fault Handler Thread
 *===========================================================================*/

static void* fault_handler_thread_func(void *arg) {
    (void)arg;
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_DEBUG("Page fault handler thread started");
    
    while (g_fault_running && ctx->running) {
        /* Wait for eventfd signal */
        uint64_t val;
        ssize_t n = read(ctx->fault_eventfd, &val, sizeof(val));
        
        if (n != sizeof(val)) {
            if (errno == EAGAIN || errno == EINTR) {
                usleep(1000);
                continue;
            }
            if (!ctx->running) break;
            continue;
        }
        
        void *fault_addr = ctx->pending_fault_addr;
        if (!fault_addr) {
            ctx->fault_handled = 1;
            continue;
        }
        
        LOG_INFO("Handling page fault at %p", fault_addr);
        
        /* Handle the page fault */
        int result = dsm_page_fault_handle(fault_addr);
        
        if (result != 0) {
            LOG_ERROR("Failed to handle page fault at %p", fault_addr);
        }
        
        /* Signal completion with result */
        ctx->pending_fault_addr = NULL;
        ctx->fault_result = result;
        ctx->fault_handled = 1;
    }
    
    LOG_DEBUG("Page fault handler thread exiting");
    return NULL;
}

/*============================================================================
 * Page Fault Handling Logic
 *===========================================================================*/

int dsm_page_fault_handle(void *fault_addr) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_INFO("[FAULT] ======== PAGE FAULT HANDLER START ========");
    LOG_INFO("[FAULT] fault_addr=%p, local_node_id=%d", fault_addr, ctx->local_node_id);
    
    /* Align to page boundary */
    void *page_addr = (void*)((uintptr_t)fault_addr & ~(DSM_PAGE_SIZE - 1));
    
    LOG_INFO("[FAULT] Aligned page_addr=%p (DSM_PAGE_SIZE=0x%x)", page_addr, DSM_PAGE_SIZE);
    
    /* Find the page in our page table */
    LOG_INFO("[FAULT] Calling dsm_page_lookup_local(%p)...", page_addr);
    dsm_page_t *page = dsm_page_lookup_local(page_addr);
    if (!page) {
        LOG_ERROR("[FAULT] Faulted on unregistered page at %p", page_addr);
        return -1;
    }
    
    LOG_INFO("[FAULT] Lookup returned: page=%p, global=0x%lx, local=%p, owner=%d, state=%d",
             (void*)page, page->global_addr, page->local_addr, page->owner_id, page->state);
    
    pthread_mutex_lock(&page->lock);
    
    LOG_INFO("[FAULT] After lock - state=%d (0=INVALID, 1=SHARED, 2=MODIFIED, 3=PENDING)", page->state);
    
    /* Check page state */
    if (page->state == DSM_PAGE_MODIFIED || page->state == DSM_PAGE_SHARED) {
        /* Page is already valid - might be a write fault on shared page */
        LOG_INFO("[FAULT] Page already valid (state=%d), checking ownership...", page->state);
        LOG_INFO("[FAULT] page->owner_id=%d, ctx->local_node_id=%d", page->owner_id, ctx->local_node_id);
        
        /* Try to enable write access if we own it */
        if (page->owner_id == ctx->local_node_id) {
            LOG_INFO("[FAULT] We own this page - upgrading to PROT_WRITE");
            if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
                LOG_ERROR("[FAULT] mprotect failed: %s", strerror(errno));
                pthread_mutex_unlock(&page->lock);
                return -1;
            }
            page->state = DSM_PAGE_MODIFIED;
            pthread_mutex_unlock(&page->lock);
            LOG_INFO("[FAULT] Success - page upgraded to MODIFIED");
            return 0;
        } else {
            /* Write fault on page we don't own - this is a programming error */
            LOG_ERROR("[FAULT] *** WRITE FAULT ON NON-OWNED PAGE ***");
            LOG_ERROR("[FAULT] page->global_addr=0x%lx", page->global_addr);
            LOG_ERROR("[FAULT] page->owner_id=%d, ctx->local_node_id=%d", page->owner_id, ctx->local_node_id);
            LOG_ERROR("[FAULT] page->state=%d, page->local_addr=%p", page->state, page->local_addr);
            LOG_ERROR("Write fault on page 0x%lx owned by node %d (we are node %d)", 
                     page->global_addr, page->owner_id, ctx->local_node_id);
            LOG_ERROR("Read-Replication DSM: Only the owner can write to a page");
            LOG_ERROR("Use dsm_lock() to protect shared data, or allocate on the writing node");
            pthread_mutex_unlock(&page->lock);
            return -1;  /* Fail - will trigger default SIGSEGV handler */
        }
    }
    
    LOG_INFO("[FAULT] Page is INVALID (state=%d) - need to fetch from owner", page->state);
    if (page->owner_id == ctx->local_node_id) {
        /* We own this page but it's invalid? This shouldn't happen */
        LOG_WARN("Local page 0x%lx is invalid - enabling access", page->global_addr);
        if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
            LOG_ERROR("mprotect failed: %s", strerror(errno));
            pthread_mutex_unlock(&page->lock);
            return -1;
        }
        page->state = DSM_PAGE_MODIFIED;
        pthread_mutex_unlock(&page->lock);
        return 0;
    }
    
    /* Mark as pending */
    page->state = DSM_PAGE_PENDING;
    pthread_mutex_unlock(&page->lock);
    
    /* Fetch from owner */
    LOG_INFO("Fetching page 0x%lx from owner node %d", page->global_addr, page->owner_id);
    
    /* Safety check - ensure context is valid */
    if (!ctx) {
        LOG_ERROR("Context is NULL in page fault handler!");
        return -1;
    }
    
    /* If master, query ownership table */
    int owner_id = page->owner_id;
    if (ctx->local_role == DSM_ROLE_MASTER) {
        int actual_owner = dsm_ownership_get(page->global_addr);
        if (actual_owner > 0) {
            owner_id = actual_owner;
        }
    }
    
    LOG_DEBUG("Page fault: Need to fetch from owner_id=%d, local_role=%d, master_fd=%d",
             owner_id, ctx->local_role, ctx->master_fd);
    
    /* Request page from owner */
    LOG_DEBUG("Creating page request message...");
    dsm_msg_page_request_t req;
    req.global_addr = page->global_addr;
    req.access_type = 0; /* Read access */
    
    LOG_DEBUG("Creating message header...");
    dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_PAGE_REQUEST, owner_id, sizeof(req));
    LOG_DEBUG("Header created: magic=0x%x, src=%d, dst=%d", hdr.magic, hdr.src_node, hdr.dst_node);
    
    /* Get the socket to send to owner */
    int owner_fd = -1;
    
    /* If we're a worker and owner is master (node 1), use master_fd directly */
    if (ctx->local_role == DSM_ROLE_WORKER && owner_id == 1) {
        owner_fd = ctx->master_fd;
        LOG_DEBUG("Using master_fd=%d to reach owner node 1", owner_fd);
    } else {
        /* Look up in node table */
        LOG_DEBUG("Looking up owner %d in node table", owner_id);
        dsm_node_t *owner_node = dsm_node_table_get(owner_id);
        if (owner_node && owner_node->tcp_fd >= 0) {
            owner_fd = owner_node->tcp_fd;
            LOG_DEBUG("Found owner_node->tcp_fd=%d", owner_fd);
        } else {
            LOG_DEBUG("Owner node lookup failed: owner_node=%p", (void*)owner_node);
        }
    }
    
    if (owner_fd < 0) {
        LOG_ERROR("Owner node %d not connected (owner_fd=%d, master_fd=%d)", 
                 owner_id, owner_fd, ctx->master_fd);
        pthread_mutex_lock(&page->lock);
        page->state = DSM_PAGE_INVALID;
        pthread_mutex_unlock(&page->lock);
        return -1;
    }
    
    LOG_INFO("Sending page request to owner %d via fd=%d", owner_id, owner_fd);
    
    if (dsm_tcp_send_raw(owner_fd, &hdr, sizeof(hdr)) != 0 ||
        dsm_tcp_send_raw(owner_fd, &req, sizeof(req)) != 0) {
        LOG_ERROR("Failed to send page request to owner %d", owner_id);
        pthread_mutex_lock(&page->lock);
        page->state = DSM_PAGE_INVALID;
        pthread_mutex_unlock(&page->lock);
        return -1;
    }
    
    LOG_INFO("Page request sent successfully to owner %d", owner_id);
    
    /* Wait for page data to arrive (TCP thread will handle it) */
    /* The TCP handler will call dsm_page_install which signals completion */
    LOG_INFO("About to lock fault_mutex at %p", (void*)&ctx->fault_mutex);
    
    if (!ctx) {
        LOG_ERROR("ctx is NULL before mutex lock!");
        return -1;
    }
    
    LOG_INFO("Locking fault_mutex...");
    pthread_mutex_lock(&ctx->fault_mutex);
    LOG_INFO("fault_mutex locked, setting up timeout");
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10; /* 10 second timeout */
    
    LOG_INFO("Entering wait loop for page state (current=%d)", page->state);
    while (page->state == DSM_PAGE_PENDING && ctx->running) {
        int ret = pthread_cond_timedwait(&ctx->fault_cond, &ctx->fault_mutex, &ts);
        if (ret == ETIMEDOUT) {
            LOG_ERROR("Timeout waiting for page 0x%lx from owner %d", 
                     page->global_addr, owner_id);
            pthread_mutex_unlock(&ctx->fault_mutex);
            pthread_mutex_lock(&page->lock);
            page->state = DSM_PAGE_INVALID;
            pthread_mutex_unlock(&page->lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&ctx->fault_mutex);
    
    LOG_INFO("Page 0x%lx fetched successfully", page->global_addr);
    return 0;
}

/*============================================================================
 * Public Functions
 *===========================================================================*/

int dsm_pagefault_init(void) {
    LOG_DEBUG("Initializing page fault handler");
    
    /* Set up SIGSEGV handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    
    if (sigaction(SIGSEGV, &sa, &g_old_sigsegv_action) != 0) {
        LOG_ERROR("Failed to install SIGSEGV handler: %s", strerror(errno));
        return -1;
    }
    
    LOG_INFO("SIGSEGV handler installed");
    return 0;
}

void dsm_pagefault_shutdown(void) {
    LOG_DEBUG("Shutting down page fault handler");
    
    /* Restore old SIGSEGV handler */
    sigaction(SIGSEGV, &g_old_sigsegv_action, NULL);
    
    LOG_DEBUG("SIGSEGV handler restored");
}

int dsm_pagefault_start_handler(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (g_fault_running) {
        LOG_WARN("Fault handler already running");
        return 0;
    }
    
    g_fault_running = 1;
    ctx->fault_running = 1;
    
    if (pthread_create(&g_fault_handler_thread, NULL, fault_handler_thread_func, NULL) != 0) {
        LOG_ERROR("Failed to create fault handler thread: %s", strerror(errno));
        g_fault_running = 0;
        ctx->fault_running = 0;
        return -1;
    }
    
    LOG_INFO("Page fault handler thread started");
    return 0;
}

void dsm_pagefault_stop_handler(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!g_fault_running) {
        return;
    }
    
    LOG_DEBUG("Stopping page fault handler");
    
    g_fault_running = 0;
    ctx->fault_running = 0;
    
    /* Signal eventfd to wake up handler */
    uint64_t val = 1;
    ssize_t ret = write(ctx->fault_eventfd, &val, sizeof(val));
    (void)ret; /* Ignore return value */
    
    pthread_join(g_fault_handler_thread, NULL);
    LOG_DEBUG("Page fault handler thread joined");
}
