/**
 * @file dsm_network.c
 * @brief Network abstraction layer and node table management
 */

#include "dsm_types.h"
#include "dsm_network.h"
#include "dsm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

/*============================================================================
 * Node Table Implementation
 *===========================================================================*/

int dsm_node_table_init(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    ctx->node_table = (dsm_node_table_t*)calloc(1, sizeof(dsm_node_table_t));
    if (!ctx->node_table) {
        LOG_ERROR("Failed to allocate node table");
        return -1;
    }
    
    if (pthread_rwlock_init(&ctx->node_table->rwlock, NULL) != 0) {
        LOG_ERROR("Failed to init node table rwlock");
        free(ctx->node_table);
        ctx->node_table = NULL;
        return -1;
    }
    
    ctx->node_table->count = 0;
    
    LOG_DEBUG("Node table initialized");
    return 0;
}

void dsm_node_table_destroy(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->node_table) {
        return;
    }
    
    pthread_rwlock_wrlock(&ctx->node_table->rwlock);
    
    /* Close all connections */
    for (uint32_t i = 0; i < ctx->node_table->count; i++) {
        dsm_node_t *node = &ctx->node_table->nodes[i];
        pthread_mutex_destroy(&node->lock);
    }
    
    pthread_rwlock_unlock(&ctx->node_table->rwlock);
    pthread_rwlock_destroy(&ctx->node_table->rwlock);
    
    free(ctx->node_table);
    ctx->node_table = NULL;
    
    LOG_DEBUG("Node table destroyed");
}

int dsm_node_table_add(const char *ip, uint16_t port, uint32_t chunks, 
                       dsm_role_t role, int sockfd) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->node_table) {
        LOG_ERROR("Node table not initialized");
        return -1;
    }
    
    pthread_rwlock_wrlock(&ctx->node_table->rwlock);
    
    /* Check if node already exists */
    for (uint32_t i = 0; i < ctx->node_table->count; i++) {
        dsm_node_t *node = &ctx->node_table->nodes[i];
        if (strcmp(node->ip, ip) == 0 && node->port == port) {
            /* Update existing node */
            node->tcp_fd = sockfd;
            node->state = DSM_NODE_STATE_CONNECTED;
            node->last_heartbeat = time(NULL);
            pthread_rwlock_unlock(&ctx->node_table->rwlock);
            LOG_DEBUG("Updated existing node %d", node->node_id);
            return node->node_id;
        }
    }
    
    /* Check capacity */
    if (ctx->node_table->count >= DSM_MAX_NODES) {
        pthread_rwlock_unlock(&ctx->node_table->rwlock);
        LOG_ERROR("Node table full");
        return -1;
    }
    
    /* Add new node */
    uint32_t idx = ctx->node_table->count;
    dsm_node_t *node = &ctx->node_table->nodes[idx];
    
    /* Assign node_id: master=1, workers start at 2 */
    node->node_id = (role == DSM_ROLE_MASTER) ? 1 : ctx->node_table->count + 1;
    node->role = role;
    node->state = (sockfd >= 0) ? DSM_NODE_STATE_CONNECTED : DSM_NODE_STATE_DISCOVERING;
    strncpy(node->ip, ip, sizeof(node->ip) - 1);
    node->ip[sizeof(node->ip) - 1] = '\0';
    node->port = port;
    node->chunks = chunks;
    node->tcp_fd = sockfd;
    node->last_heartbeat = time(NULL);
    
    if (pthread_mutex_init(&node->lock, NULL) != 0) {
        pthread_rwlock_unlock(&ctx->node_table->rwlock);
        LOG_ERROR("Failed to init node mutex");
        return -1;
    }
    
    ctx->node_table->count++;
    
    pthread_rwlock_unlock(&ctx->node_table->rwlock);
    
    LOG_INFO("Added node %d: %s:%d, chunks=%d, role=%s", 
             node->node_id, ip, port, chunks,
             role == DSM_ROLE_MASTER ? "MASTER" : "WORKER");
    
    return node->node_id;
}

int dsm_node_table_remove(uint32_t node_id) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->node_table) {
        return -1;
    }
    
    pthread_rwlock_wrlock(&ctx->node_table->rwlock);
    
    int found = -1;
    for (uint32_t i = 0; i < ctx->node_table->count; i++) {
        if (ctx->node_table->nodes[i].node_id == node_id) {
            found = i;
            break;
        }
    }
    
    if (found < 0) {
        pthread_rwlock_unlock(&ctx->node_table->rwlock);
        LOG_WARN("Node %d not found for removal", node_id);
        return -1;
    }
    
    /* Cleanup node */
    dsm_node_t *node = &ctx->node_table->nodes[found];
    pthread_mutex_destroy(&node->lock);
    
    LOG_INFO("Removing node %d: %s:%d", node->node_id, node->ip, node->port);
    
    /* Shift remaining nodes */
    for (uint32_t i = found; i < ctx->node_table->count - 1; i++) {
        ctx->node_table->nodes[i] = ctx->node_table->nodes[i + 1];
    }
    ctx->node_table->count--;
    
    pthread_rwlock_unlock(&ctx->node_table->rwlock);
    return 0;
}

dsm_node_t* dsm_node_table_get(uint32_t node_id) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->node_table) {
        return NULL;
    }
    
    pthread_rwlock_rdlock(&ctx->node_table->rwlock);
    
    dsm_node_t *result = NULL;
    for (uint32_t i = 0; i < ctx->node_table->count; i++) {
        if (ctx->node_table->nodes[i].node_id == node_id) {
            result = &ctx->node_table->nodes[i];
            break;
        }
    }
    
    pthread_rwlock_unlock(&ctx->node_table->rwlock);
    return result;
}

dsm_node_t* dsm_node_table_find(const char *ip, uint16_t port) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->node_table) {
        return NULL;
    }
    
    pthread_rwlock_rdlock(&ctx->node_table->rwlock);
    
    dsm_node_t *result = NULL;
    for (uint32_t i = 0; i < ctx->node_table->count; i++) {
        dsm_node_t *node = &ctx->node_table->nodes[i];
        if (strcmp(node->ip, ip) == 0 && node->port == port) {
            result = node;
            break;
        }
    }
    
    pthread_rwlock_unlock(&ctx->node_table->rwlock);
    return result;
}

uint32_t dsm_node_table_count(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->node_table) {
        return 0;
    }
    
    pthread_rwlock_rdlock(&ctx->node_table->rwlock);
    uint32_t count = ctx->node_table->count;
    pthread_rwlock_unlock(&ctx->node_table->rwlock);
    
    return count;
}

ssize_t dsm_node_table_serialize(void *buf, size_t buf_len) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->node_table) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&ctx->node_table->rwlock);
    
    uint32_t count = ctx->node_table->count;
    size_t needed = sizeof(dsm_msg_node_table_t) + count * sizeof(dsm_msg_node_info_t);
    
    if (buf_len < needed) {
        pthread_rwlock_unlock(&ctx->node_table->rwlock);
        LOG_ERROR("Buffer too small for node table: need %zu, have %zu", needed, buf_len);
        return -1;
    }
    
    dsm_msg_node_table_t *msg = (dsm_msg_node_table_t*)buf;
    msg->count = count;
    
    for (uint32_t i = 0; i < count; i++) {
        dsm_node_t *node = &ctx->node_table->nodes[i];
        dsm_msg_node_info_t *info = &msg->nodes[i];
        
        info->node_id = node->node_id;
        strncpy(info->ip, node->ip, sizeof(info->ip) - 1);
        info->ip[sizeof(info->ip) - 1] = '\0';
        info->port = node->port;
        info->chunks = node->chunks;
        info->role = node->role;
        info->state = node->state;
    }
    
    pthread_rwlock_unlock(&ctx->node_table->rwlock);
    
    LOG_DEBUG("Serialized node table: %d nodes, %zu bytes", count, needed);
    return needed;
}

int dsm_node_table_deserialize(void *buf, size_t len) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (!ctx->node_table) {
        return -1;
    }
    
    if (len < sizeof(dsm_msg_node_table_t)) {
        LOG_ERROR("Buffer too small for node table header");
        return -1;
    }
    
    dsm_msg_node_table_t *msg = (dsm_msg_node_table_t*)buf;
    
    size_t needed = sizeof(dsm_msg_node_table_t) + msg->count * sizeof(dsm_msg_node_info_t);
    if (len < needed) {
        LOG_ERROR("Buffer too small for node table: need %zu, have %zu", needed, len);
        return -1;
    }
    
    LOG_INFO("Deserializing node table: %d nodes", msg->count);
    
    pthread_rwlock_wrlock(&ctx->node_table->rwlock);
    
    /* Update/add nodes from received table */
    for (uint32_t i = 0; i < msg->count; i++) {
        dsm_msg_node_info_t *info = &msg->nodes[i];
        
        /* Find existing node */
        int found = -1;
        for (uint32_t j = 0; j < ctx->node_table->count; j++) {
            if (ctx->node_table->nodes[j].node_id == info->node_id) {
                found = j;
                break;
            }
        }
        
        if (found >= 0) {
            /* Update existing */
            dsm_node_t *node = &ctx->node_table->nodes[found];
            strncpy(node->ip, info->ip, sizeof(node->ip) - 1);
            node->port = info->port;
            node->chunks = info->chunks;
            node->role = (dsm_role_t)info->role;
            node->state = (dsm_node_state_t)info->state;
        } else if (ctx->node_table->count < DSM_MAX_NODES) {
            /* Add new */
            uint32_t idx = ctx->node_table->count;
            dsm_node_t *node = &ctx->node_table->nodes[idx];
            
            node->node_id = info->node_id;
            strncpy(node->ip, info->ip, sizeof(node->ip) - 1);
            node->ip[sizeof(node->ip) - 1] = '\0';
            node->port = info->port;
            node->chunks = info->chunks;
            node->role = (dsm_role_t)info->role;
            node->state = (dsm_node_state_t)info->state;
            node->tcp_fd = -1;
            node->last_heartbeat = time(NULL);
            pthread_mutex_init(&node->lock, NULL);
            
            ctx->node_table->count++;
            
            LOG_DEBUG("Added node from table: id=%d, ip=%s, port=%d", 
                     node->node_id, node->ip, node->port);
        }
    }
    
    pthread_rwlock_unlock(&ctx->node_table->rwlock);
    return 0;
}

int dsm_node_table_broadcast(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    if (ctx->local_role != DSM_ROLE_MASTER) {
        LOG_WARN("Only master can broadcast node table");
        return -1;
    }
    
    /* Serialize node table */
    uint8_t buf[DSM_MAX_MESSAGE_SIZE];
    ssize_t len = dsm_node_table_serialize(buf, sizeof(buf));
    if (len < 0) {
        return -1;
    }
    
    /* Create header and broadcast */
    dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_NODE_TABLE, 0, len);
    dsm_tcp_broadcast(&hdr, buf);
    
    LOG_INFO("Broadcast node table to all nodes");
    return 0;
}

int dsm_node_table_set_socket(uint32_t node_id, int sockfd) {
    dsm_node_t *node = dsm_node_table_get(node_id);
    if (!node) {
        return -1;
    }
    
    pthread_mutex_lock(&node->lock);
    node->tcp_fd = sockfd;
    node->state = (sockfd >= 0) ? DSM_NODE_STATE_CONNECTED : DSM_NODE_STATE_DISCONNECTED;
    pthread_mutex_unlock(&node->lock);
    
    LOG_DEBUG("Set socket for node %d to fd=%d", node_id, sockfd);
    return 0;
}

void dsm_node_table_heartbeat(uint32_t node_id) {
    dsm_node_t *node = dsm_node_table_get(node_id);
    if (node) {
        pthread_mutex_lock(&node->lock);
        node->last_heartbeat = time(NULL);
        pthread_mutex_unlock(&node->lock);
    }
}

/*============================================================================
 * Network Initialization
 *===========================================================================*/

int dsm_net_init(void) {
    /* Currently just delegates to node table init */
    return dsm_node_table_init();
}

void dsm_net_shutdown(void) {
    dsm_node_table_destroy();
}
