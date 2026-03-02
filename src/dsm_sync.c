/**
 * @file dsm_sync.c
 * @brief Distributed synchronization primitives (locks, barriers)
 */

#include "dsm.h"
#include "dsm_types.h"
#include "dsm_network.h"
#include "dsm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

/*============================================================================
 * Initialization
 *===========================================================================*/

int dsm_sync_init(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_DEBUG("Initializing synchronization subsystem");
    
    /* Initialize all locks */
    for (int i = 0; i < DSM_MAX_LOCKS; i++) {
        dsm_lock_t *lock = &ctx->locks[i];
        lock->lock_id = i;
        lock->holder_id = 0;
        lock->locked = false;
        lock->waiter_count = 0;
        
        if (pthread_mutex_init(&lock->mutex, NULL) != 0) {
            LOG_ERROR("Failed to init lock %d mutex", i);
            return -1;
        }
        if (pthread_cond_init(&lock->cond, NULL) != 0) {
            pthread_mutex_destroy(&lock->mutex);
            LOG_ERROR("Failed to init lock %d cond", i);
            return -1;
        }
    }
    
    /* Initialize barrier */
    ctx->barrier.barrier_id = 0;
    ctx->barrier.total_nodes = 0;
    ctx->barrier.arrived_count = 0;
    
    if (pthread_mutex_init(&ctx->barrier.mutex, NULL) != 0) {
        LOG_ERROR("Failed to init barrier mutex");
        return -1;
    }
    if (pthread_cond_init(&ctx->barrier.cond, NULL) != 0) {
        pthread_mutex_destroy(&ctx->barrier.mutex);
        LOG_ERROR("Failed to init barrier cond");
        return -1;
    }
    
    LOG_INFO("Synchronization subsystem initialized (%d locks)", DSM_MAX_LOCKS);
    return 0;
}

void dsm_sync_shutdown(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_DEBUG("Shutting down synchronization subsystem");
    
    /* Destroy all locks */
    for (int i = 0; i < DSM_MAX_LOCKS; i++) {
        pthread_mutex_destroy(&ctx->locks[i].mutex);
        pthread_cond_destroy(&ctx->locks[i].cond);
    }
    
    /* Destroy barrier */
    pthread_mutex_destroy(&ctx->barrier.mutex);
    pthread_cond_destroy(&ctx->barrier.cond);
    
    LOG_DEBUG("Synchronization subsystem shut down");
}

/*============================================================================
 * Lock Handling (Master Side)
 *===========================================================================*/

int dsm_sync_handle_lock_request(uint32_t from_node, uint32_t lock_id) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (lock_id >= DSM_MAX_LOCKS) {
        LOG_ERROR("Invalid lock_id: %d", lock_id);
        return -1;
    }
    
    dsm_lock_t *lock = &ctx->locks[lock_id];
    
    LOG_DEBUG("Lock request from node %d for lock %d", from_node, lock_id);
    
    pthread_mutex_lock(&lock->mutex);
    
    if (!lock->locked) {
        /* Lock is free - grant immediately */
        lock->locked = true;
        lock->holder_id = from_node;
        pthread_mutex_unlock(&lock->mutex);
        
        LOG_INFO("Lock %d granted to node %d", lock_id, from_node);
        
        /* Send grant message */
        dsm_msg_lock_t grant;
        grant.lock_id = lock_id;
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_LOCK_GRANT, from_node, sizeof(grant));
        dsm_tcp_send(from_node, &hdr, &grant);
    } else {
        /* Lock is held - add to waiters */
        if (lock->waiter_count < DSM_MAX_NODES) {
            lock->waiters[lock->waiter_count++] = from_node;
            LOG_DEBUG("Node %d added to lock %d wait queue (pos %d)", 
                     from_node, lock_id, lock->waiter_count);
        } else {
            LOG_ERROR("Lock %d waiter queue full", lock_id);
        }
        pthread_mutex_unlock(&lock->mutex);
    }
    
    return 0;
}

int dsm_sync_handle_lock_release(uint32_t from_node, uint32_t lock_id) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (lock_id >= DSM_MAX_LOCKS) {
        LOG_ERROR("Invalid lock_id: %d", lock_id);
        return -1;
    }
    
    dsm_lock_t *lock = &ctx->locks[lock_id];
    
    LOG_DEBUG("Lock release from node %d for lock %d", from_node, lock_id);
    
    pthread_mutex_lock(&lock->mutex);
    
    if (!lock->locked || lock->holder_id != from_node) {
        LOG_WARN("Node %d releasing lock %d not held by it (holder=%d)", 
                from_node, lock_id, lock->holder_id);
        pthread_mutex_unlock(&lock->mutex);
        return -1;
    }
    
    /* Check for waiters */
    if (lock->waiter_count > 0) {
        /* Grant to next waiter */
        uint32_t next_holder = lock->waiters[0];
        
        /* Shift waiters */
        for (uint32_t i = 0; i < lock->waiter_count - 1; i++) {
            lock->waiters[i] = lock->waiters[i + 1];
        }
        lock->waiter_count--;
        
        lock->holder_id = next_holder;
        /* lock->locked remains true */
        
        pthread_mutex_unlock(&lock->mutex);
        
        LOG_INFO("Lock %d transferred from node %d to node %d", 
                lock_id, from_node, next_holder);
        
        /* Send grant to next waiter */
        dsm_msg_lock_t grant;
        grant.lock_id = lock_id;
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_LOCK_GRANT, next_holder, sizeof(grant));
        dsm_tcp_send(next_holder, &hdr, &grant);
    } else {
        /* No waiters - free the lock */
        lock->locked = false;
        lock->holder_id = 0;
        pthread_mutex_unlock(&lock->mutex);
        
        LOG_INFO("Lock %d released by node %d (now free)", lock_id, from_node);
    }
    
    return 0;
}

/*============================================================================
 * Barrier Handling (Master Side)
 *===========================================================================*/

int dsm_sync_handle_barrier_enter(uint32_t from_node) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_DEBUG("Barrier entry from node %d", from_node);
    
    pthread_mutex_lock(&ctx->barrier.mutex);
    
    /* Record arrival */
    ctx->barrier.arrived[ctx->barrier.arrived_count++] = from_node;
    
    uint32_t total_nodes = dsm_node_table_count();
    
    LOG_INFO("Barrier: %d/%d nodes arrived", ctx->barrier.arrived_count, total_nodes);
    
    if (ctx->barrier.arrived_count >= total_nodes) {
        /* All nodes arrived - release barrier */
        LOG_INFO("Barrier complete - releasing all nodes");
        
        /* Send release to all waiting nodes */
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_BARRIER_RELEASE, 0, 0);
        
        for (uint32_t i = 0; i < ctx->barrier.arrived_count; i++) {
            uint32_t node_id = ctx->barrier.arrived[i];
            if (node_id != ctx->local_node_id) {
                hdr.dst_node = node_id;
                dsm_tcp_send(node_id, &hdr, NULL);
            }
        }
        
        /* Reset barrier state */
        ctx->barrier.arrived_count = 0;
        ctx->barrier.barrier_id++;
        
        /* Wake local waiters */
        pthread_cond_broadcast(&ctx->barrier.cond);
    }
    
    pthread_mutex_unlock(&ctx->barrier.mutex);
    return 0;
}

/*============================================================================
 * Public API
 *===========================================================================*/

void dsm_lock(int lock_id) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (lock_id < 0 || lock_id >= DSM_MAX_LOCKS) {
        LOG_ERROR("Invalid lock_id: %d", lock_id);
        return;
    }
    
    dsm_lock_t *lock = &ctx->locks[lock_id];
    
    LOG_DEBUG("Acquiring lock %d", lock_id);
    
    if (ctx->local_role == DSM_ROLE_MASTER) {
        /* Master handles lock locally */
        pthread_mutex_lock(&lock->mutex);
        
        while (lock->locked && lock->holder_id != ctx->local_node_id) {
            pthread_cond_wait(&lock->cond, &lock->mutex);
        }
        
        lock->locked = true;
        lock->holder_id = ctx->local_node_id;
        
        pthread_mutex_unlock(&lock->mutex);
        
        LOG_INFO("Lock %d acquired (master)", lock_id);
    } else {
        /* Worker sends request to master */
        dsm_msg_lock_t req;
        req.lock_id = lock_id;
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_LOCK_REQUEST, 
                                                  ctx->master_id, sizeof(req));
        
        /* Use master_fd directly - it's set during connection */
        int master_fd = ctx->master_fd;
        if (master_fd < 0) {
            /* Fallback: search node table */
            for (uint32_t i = 0; i < dsm_node_table_count(); i++) {
                dsm_node_t *node = dsm_node_table_get(i + 1);
                if (node && node->role == DSM_ROLE_MASTER && node->tcp_fd >= 0) {
                    master_fd = node->tcp_fd;
                    break;
                }
            }
        }
        
        if (master_fd < 0) {
            LOG_ERROR("Cannot acquire lock - master not connected");
            return;
        }
        
        /* Send request */
        LOG_INFO("Sending lock %d request to master via fd=%d", lock_id, master_fd);
        dsm_tcp_send_raw(master_fd, &hdr, sizeof(hdr));
        dsm_tcp_send_raw(master_fd, &req, sizeof(req));
        
        /* Wait for grant */
        LOG_DEBUG("Waiting for lock %d grant (local_node_id=%d)", lock_id, ctx->local_node_id);
        pthread_mutex_lock(&lock->mutex);
        while (!lock->locked || lock->holder_id != ctx->local_node_id) {
            LOG_DEBUG("Lock %d wait: locked=%d, holder_id=%d, local=%d", 
                     lock_id, lock->locked, lock->holder_id, ctx->local_node_id);
            pthread_cond_wait(&lock->cond, &lock->mutex);
        }
        pthread_mutex_unlock(&lock->mutex);
        
        LOG_INFO("Lock %d acquired (worker)", lock_id);
    }
}

void dsm_unlock(int lock_id) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (lock_id < 0 || lock_id >= DSM_MAX_LOCKS) {
        LOG_ERROR("Invalid lock_id: %d", lock_id);
        return;
    }
    
    dsm_lock_t *lock = &ctx->locks[lock_id];
    
    LOG_DEBUG("Releasing lock %d", lock_id);
    
    if (ctx->local_role == DSM_ROLE_MASTER) {
        /* Master handles release locally */
        dsm_sync_handle_lock_release(ctx->local_node_id, lock_id);
        
        /* Also signal local waiters */
        pthread_mutex_lock(&lock->mutex);
        pthread_cond_signal(&lock->cond);
        pthread_mutex_unlock(&lock->mutex);
    } else {
        /* Worker sends release to master */
        dsm_msg_lock_t rel;
        rel.lock_id = lock_id;
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_LOCK_RELEASE, 
                                                  ctx->master_id, sizeof(rel));
        
        /* Use master_fd directly */
        int master_fd = ctx->master_fd;
        if (master_fd < 0) {
            for (uint32_t i = 0; i < dsm_node_table_count(); i++) {
                dsm_node_t *node = dsm_node_table_get(i + 1);
                if (node && node->role == DSM_ROLE_MASTER && node->tcp_fd >= 0) {
                    master_fd = node->tcp_fd;
                    break;
                }
            }
        }
        
        if (master_fd >= 0) {
            dsm_tcp_send_raw(master_fd, &hdr, sizeof(hdr));
            dsm_tcp_send_raw(master_fd, &rel, sizeof(rel));
        }
        
        /* Clear local state */
        pthread_mutex_lock(&lock->mutex);
        lock->locked = false;
        lock->holder_id = 0;
        pthread_mutex_unlock(&lock->mutex);
        
        LOG_INFO("Lock %d released (worker)", lock_id);
    }
}

void dsm_barrier(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_DEBUG("Entering barrier");
    
    if (ctx->local_role == DSM_ROLE_MASTER) {
        /* Master participates in barrier */
        dsm_sync_handle_barrier_enter(ctx->local_node_id);
        
        /* Wait for release (from own handler) */
        pthread_mutex_lock(&ctx->barrier.mutex);
        uint32_t current_id = ctx->barrier.barrier_id;
        while (ctx->barrier.barrier_id == current_id && 
               ctx->barrier.arrived_count > 0) {
            pthread_cond_wait(&ctx->barrier.cond, &ctx->barrier.mutex);
        }
        pthread_mutex_unlock(&ctx->barrier.mutex);
    } else {
        /* Worker sends barrier entry to master */
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_BARRIER_ENTER, 
                                                  ctx->master_id, 0);
        
        /* Use master_fd directly */
        int master_fd = ctx->master_fd;
        if (master_fd < 0) {
            for (uint32_t i = 0; i < dsm_node_table_count(); i++) {
                dsm_node_t *node = dsm_node_table_get(i + 1);
                if (node && node->role == DSM_ROLE_MASTER && node->tcp_fd >= 0) {
                    master_fd = node->tcp_fd;
                    break;
                }
            }
        }
        
        if (master_fd >= 0) {
            dsm_tcp_send_raw(master_fd, &hdr, sizeof(hdr));
        } else {
            LOG_ERROR("Cannot enter barrier - master not connected");
        }
        
        /* Wait for release message */
        pthread_mutex_lock(&ctx->barrier.mutex);
        pthread_cond_wait(&ctx->barrier.cond, &ctx->barrier.mutex);
        pthread_mutex_unlock(&ctx->barrier.mutex);
    }
    
    LOG_INFO("Passed barrier");
}
