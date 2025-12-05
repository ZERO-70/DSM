/**
 * @file dsm_pagefault.c
 * @brief SIGSEGV handler for demand paging
 * 
 * Strategy: Minimal signal handler + eventfd to wake network thread
 * The signal handler only writes to eventfd and sets the fault address.
 * The fault handler thread does the actual network I/O.
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
 * Page Fault Handling Logic (Invalidate/Exclusive Ownership Protocol)
 * 
 * This implements a full read/write DSM:
 * - Read fault on INVALID page: Fetch shared copy from owner
 * - Write fault on SHARED page we don't own: Request ownership transfer
 * - Write fault on SHARED page we own: Upgrade to MODIFIED (exclusive)
 *===========================================================================*/

int dsm_page_fault_handle(void *fault_addr) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_INFO("[FAULT] ======== PAGE FAULT HANDLER START ========");
    LOG_INFO("[FAULT] fault_addr=%p, local_node_id=%d", fault_addr, ctx->local_node_id);
    
    /* Align to page boundary */
    void *page_addr = (void*)((uintptr_t)fault_addr & ~(DSM_PAGE_SIZE - 1));
    
    LOG_INFO("[FAULT] Aligned page_addr=%p (DSM_PAGE_SIZE=0x%x)", page_addr, DSM_PAGE_SIZE);
    
    /* Find the page in our page table */
    dsm_page_t *page = dsm_page_lookup_local(page_addr);
    if (!page) {
        LOG_ERROR("[FAULT] Faulted on unregistered page at %p", page_addr);
        return -1;
    }
    
    LOG_INFO("[FAULT] Found page: global=0x%lx, owner=%d, state=%s",
             page->global_addr, page->owner_id, dsm_page_state_to_string(page->state));
    
    pthread_mutex_lock(&page->lock);
    dsm_page_state_t current_state = page->state;
    uint32_t page_owner = page->owner_id;
    pthread_mutex_unlock(&page->lock);
    
    /*========================================================================
     * Case 1: Page is MODIFIED (we have exclusive access)
     * This shouldn't happen - the page should have PROT_READ|PROT_WRITE
     *========================================================================*/
    if (current_state == DSM_PAGE_MODIFIED) {
        LOG_INFO("[FAULT] Page is MODIFIED - enabling write access");
        if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
            LOG_ERROR("[FAULT] mprotect failed: %s", strerror(errno));
            return -1;
        }
        return 0;
    }
    
    /*========================================================================
     * Case 2: Page is SHARED (we have read access, might be a write fault)
     *========================================================================*/
    if (current_state == DSM_PAGE_SHARED) {
        LOG_INFO("[FAULT] Page is SHARED - this is a WRITE fault");
        
        /* Check if we already own this page */
        if (page_owner == ctx->local_node_id) {
            /* We own it but it's SHARED - upgrade to MODIFIED (exclusive) */
            LOG_INFO("[FAULT] We own this page - upgrading to MODIFIED (exclusive)");
            
            /* If we're master, we need to invalidate other copies */
            if (ctx->local_role == DSM_ROLE_MASTER) {
                /* Check if anyone else has a copy */
                pthread_mutex_lock(&page->lock);
                int need_invalidate = 0;
                for (uint32_t i = 0; i < DSM_MAX_NODES; i++) {
                    if (page->copyset[i] && i != ctx->local_node_id) {
                        need_invalidate = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&page->lock);
                
                if (need_invalidate) {
                    LOG_INFO("[FAULT] Invalidating other copies before upgrading");
                    /* Send invalidate to all other copy holders */
                    dsm_msg_page_invalidate_t inv_msg;
                    inv_msg.global_addr = page->global_addr;
                    inv_msg.new_owner = ctx->local_node_id;
                    
                    pthread_mutex_lock(&ctx->invalidate_mutex);
                    ctx->invalidate_addr = page->global_addr;
                    ctx->invalidate_pending = 0;
                    
                    pthread_mutex_lock(&page->lock);
                    for (uint32_t i = 0; i < DSM_MAX_NODES; i++) {
                        if (page->copyset[i] && i != ctx->local_node_id) {
                            ctx->invalidate_pending++;
                            dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_PAGE_INVALIDATE, i, sizeof(inv_msg));
                            dsm_tcp_send(i, &hdr, &inv_msg);
                        }
                    }
                    pthread_mutex_unlock(&page->lock);
                    
                    /* Wait for ACKs */
                    if (ctx->invalidate_pending > 0) {
                        struct timespec ts;
                        clock_gettime(CLOCK_REALTIME, &ts);
                        ts.tv_sec += 10;
                        while (ctx->invalidate_pending > 0) {
                            int ret = pthread_cond_timedwait(&ctx->invalidate_cond, &ctx->invalidate_mutex, &ts);
                            if (ret == ETIMEDOUT) {
                                LOG_ERROR("[FAULT] Timeout waiting for invalidate ACKs");
                                pthread_mutex_unlock(&ctx->invalidate_mutex);
                                return -1;
                            }
                        }
                    }
                    pthread_mutex_unlock(&ctx->invalidate_mutex);
                    
                    /* Clear copyset - we're now the only one */
                    dsm_page_copyset_clear(page->global_addr);
                    dsm_page_copyset_add(page->global_addr, ctx->local_node_id);
                }
            }
            
            /* Upgrade to MODIFIED */
            if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
                LOG_ERROR("[FAULT] mprotect failed: %s", strerror(errno));
                return -1;
            }
            pthread_mutex_lock(&page->lock);
            page->state = DSM_PAGE_MODIFIED;
            pthread_mutex_unlock(&page->lock);
            
            LOG_INFO("[FAULT] Upgraded to MODIFIED (exclusive write access)");
            return 0;
        } else {
            /* We have a shared copy but don't own it - need ownership transfer */
            LOG_INFO("[FAULT] Write fault on SHARED page owned by node %d - requesting ownership", page_owner);
            
            /* Request ownership transfer through master */
            pthread_mutex_lock(&page->lock);
            page->state = DSM_PAGE_PENDING;
            pthread_mutex_unlock(&page->lock);
            
            if (dsm_page_request_write(page) != 0) {
                LOG_ERROR("[FAULT] Failed to get write access to page 0x%lx", page->global_addr);
                pthread_mutex_lock(&page->lock);
                page->state = DSM_PAGE_INVALID;
                pthread_mutex_unlock(&page->lock);
                return -1;
            }
            
            LOG_INFO("[FAULT] Ownership transfer complete - now have MODIFIED access");
            return 0;
        }
    }
    
    /*========================================================================
     * Case 3: Page is INVALID - need to fetch from owner (read fault)
     *========================================================================*/
    if (current_state == DSM_PAGE_INVALID) {
        LOG_INFO("[FAULT] Page is INVALID - local page->owner_id=%d", page_owner);
        
        /* Get the ACTUAL current owner from authoritative source */
        int actual_owner = page_owner;
        
        /* Master checks its ownership table (authoritative source) */
        if (ctx->local_role == DSM_ROLE_MASTER) {
            int table_owner = dsm_ownership_get(page->global_addr);
            if (table_owner > 0) {
                actual_owner = table_owner;
                LOG_INFO("[FAULT] Master ownership table says owner is node %d", actual_owner);
            }
        }
        /* Workers trust the local page->owner_id or could query master */
        
        LOG_INFO("[FAULT] Will fetch from actual owner node %d", actual_owner);
        
        /* Special case: we are the actual owner but page is invalid */
        /* This should only happen if we're the original allocator */
        if ((uint32_t)actual_owner == ctx->local_node_id) {
            LOG_WARN("[FAULT] We (node %d) own this page but it's INVALID - enabling access", 
                     ctx->local_node_id);
            if (mprotect(page->local_addr, DSM_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
                LOG_ERROR("[FAULT] mprotect failed: %s", strerror(errno));
                return -1;
            }
            pthread_mutex_lock(&page->lock);
            page->state = DSM_PAGE_MODIFIED;
            page->owner_id = ctx->local_node_id;  /* Ensure consistency */
            pthread_mutex_unlock(&page->lock);
            return 0;
        }
        
        /* Mark as pending and fetch from actual owner */
        pthread_mutex_lock(&page->lock);
        page->state = DSM_PAGE_PENDING;
        pthread_mutex_unlock(&page->lock);
        
        /* Build and send page request */
        dsm_msg_page_request_t req;
        req.global_addr = page->global_addr;
        req.access_type = 0; /* Read access */
        
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_PAGE_REQUEST, actual_owner, sizeof(req));
        
        /* Get socket to owner */
        int owner_fd = -1;
        
        /* Workers sending to master use master_fd */
        if (ctx->local_role == DSM_ROLE_WORKER && actual_owner == 1) {
            owner_fd = ctx->master_fd;
            LOG_DEBUG("[FAULT] Worker using master_fd=%d to reach master", owner_fd);
        }
        /* Master sending to worker, or worker sending to another worker */
        else {
            dsm_node_t *owner_node = dsm_node_table_get(actual_owner);
            if (owner_node) {
                LOG_DEBUG("[FAULT] Node table lookup for node %d: tcp_fd=%d, state=%d",
                         actual_owner, owner_node->tcp_fd, owner_node->state);
                if (owner_node->tcp_fd >= 0) {
                    owner_fd = owner_node->tcp_fd;
                }
            } else {
                LOG_ERROR("[FAULT] Node %d not found in node table!", actual_owner);
            }
        }
        
        if (owner_fd < 0) {
            LOG_ERROR("[FAULT] Cannot reach owner node %d - no valid socket (fd=%d)", 
                      actual_owner, owner_fd);
            LOG_ERROR("[FAULT] Debug: local_role=%s, actual_owner=%d, local_node_id=%d",
                      ctx->local_role == DSM_ROLE_MASTER ? "MASTER" : "WORKER",
                      actual_owner, ctx->local_node_id);
            
            /* Dump node table entries for key nodes */
            for (uint32_t nid = 1; nid <= 4; nid++) {
                dsm_node_t *n = dsm_node_table_get(nid);
                if (n) {
                    LOG_ERROR("[FAULT]   Node %d: fd=%d, state=%d, ip=%s:%d",
                              n->node_id, n->tcp_fd, n->state, n->ip, n->port);
                }
            }
            
            pthread_mutex_lock(&page->lock);
            page->state = DSM_PAGE_INVALID;
            pthread_mutex_unlock(&page->lock);
            return -1;
        }
        
        LOG_INFO("[FAULT] Sending page request to owner %d via fd=%d", actual_owner, owner_fd);
        
        if (dsm_tcp_send_raw(owner_fd, &hdr, sizeof(hdr)) != 0 ||
            dsm_tcp_send_raw(owner_fd, &req, sizeof(req)) != 0) {
            LOG_ERROR("[FAULT] Failed to send page request");
            pthread_mutex_lock(&page->lock);
            page->state = DSM_PAGE_INVALID;
            pthread_mutex_unlock(&page->lock);
            return -1;
        }
        
        /* Wait for page data */
        pthread_mutex_lock(&ctx->fault_mutex);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 10;
        
        while (page->state == DSM_PAGE_PENDING && ctx->running) {
            int ret = pthread_cond_timedwait(&ctx->fault_cond, &ctx->fault_mutex, &ts);
            if (ret == ETIMEDOUT) {
                LOG_ERROR("[FAULT] Timeout waiting for page 0x%lx", page->global_addr);
                pthread_mutex_unlock(&ctx->fault_mutex);
                pthread_mutex_lock(&page->lock);
                page->state = DSM_PAGE_INVALID;
                pthread_mutex_unlock(&page->lock);
                return -1;
            }
        }
        pthread_mutex_unlock(&ctx->fault_mutex);
        
        LOG_INFO("[FAULT] Page 0x%lx fetched successfully (state=%s)", 
                 page->global_addr, dsm_page_state_to_string(page->state));
        return 0;
    }
    
    /*========================================================================
     * Case 4: Page is PENDING - shouldn't get here, but wait for it
     *========================================================================*/
    if (current_state == DSM_PAGE_PENDING) {
        LOG_WARN("[FAULT] Page is already PENDING - waiting for completion");
        
        pthread_mutex_lock(&ctx->fault_mutex);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 10;
        
        while (page->state == DSM_PAGE_PENDING && ctx->running) {
            int ret = pthread_cond_timedwait(&ctx->fault_cond, &ctx->fault_mutex, &ts);
            if (ret == ETIMEDOUT) {
                LOG_ERROR("[FAULT] Timeout waiting for pending page");
                pthread_mutex_unlock(&ctx->fault_mutex);
                return -1;
            }
        }
        pthread_mutex_unlock(&ctx->fault_mutex);
        return 0;
    }
    
    LOG_ERROR("[FAULT] Unknown page state: %d", current_state);
    return -1;
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
