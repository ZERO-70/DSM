/**
 * @file dsm_udp.c
 * @brief UDP broadcast discovery for DSM
 */

#include "dsm_types.h"
#include "dsm_network.h"
#include "dsm_internal.h"
#include "dsm_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*============================================================================
 * Constants
 *===========================================================================*/

#define UDP_BUFFER_SIZE     1024
#define DISCOVER_MSG_FMT    "DISCOVER_DSM/%s/%d/%d"
#define DISCOVER_REPLY_FMT  "DISCOVER_REPLY/%s/%d/%d"

/*============================================================================
 * Static Variables
 *===========================================================================*/

static int g_udp_socket = -1;
static pthread_t g_udp_listener_thread;
static volatile int g_udp_running = 0;

/*============================================================================
 * UDP Listener Thread
 *===========================================================================*/

static void* udp_listener_thread_func(void *arg) {
    (void)arg;
    dsm_context_t *ctx = dsm_get_context();
    
    char buf[UDP_BUFFER_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    
    LOG_DEBUG("UDP listener thread started");
    
    while (g_udp_running && ctx->running) {
        /* Use select for timeout */
        fd_set readfds;
        struct timeval tv;
        
        FD_ZERO(&readfds);
        FD_SET(g_udp_socket, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(g_udp_socket + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("select error: %s", strerror(errno));
            break;
        }
        
        if (ret == 0) {
            continue; /* timeout, check running flag */
        }
        
        ssize_t n = recvfrom(g_udp_socket, buf, sizeof(buf) - 1, 0,
                            (struct sockaddr*)&sender_addr, &sender_len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            LOG_ERROR("recvfrom error: %s", strerror(errno));
            continue;
        }
        
        buf[n] = '\0';
        char *sender_ip = inet_ntoa(sender_addr.sin_addr);
        
        LOG_DEBUG("UDP received from %s:%d: %s", 
                  sender_ip, ntohs(sender_addr.sin_port), buf);
        
        /* Parse message */
        char msg_ip[DSM_MAX_IP_LEN];
        int msg_port, msg_chunks;
        
        /* Handle DISCOVER_DSM message */
        if (sscanf(buf, "DISCOVER_DSM/%15[^/]/%d/%d", msg_ip, &msg_port, &msg_chunks) == 3) {
            LOG_INFO("Discovery from %s:%d with %d chunks", msg_ip, msg_port, msg_chunks);
            
            /* Ignore our own broadcasts */
            if (strcmp(msg_ip, ctx->local_ip) == 0 && msg_port == ctx->local_port) {
                LOG_DEBUG("Ignoring own discovery broadcast");
                continue;
            }
            
            /* If we are master, respond */
            if (ctx->local_role == DSM_ROLE_MASTER) {
                LOG_INFO("Responding to discovery from %s:%d", msg_ip, msg_port);
                
                /* Send reply */
                char reply[UDP_BUFFER_SIZE];
                snprintf(reply, sizeof(reply), DISCOVER_REPLY_FMT,
                        ctx->local_ip, ctx->local_port, ctx->local_chunks);
                
                struct sockaddr_in reply_addr;
                memset(&reply_addr, 0, sizeof(reply_addr));
                reply_addr.sin_family = AF_INET;
                reply_addr.sin_port = htons(DSM_DISCOVERY_PORT);
                inet_pton(AF_INET, msg_ip, &reply_addr.sin_addr);
                
                sendto(g_udp_socket, reply, strlen(reply), 0,
                       (struct sockaddr*)&reply_addr, sizeof(reply_addr));
                
                LOG_DEBUG("Sent discovery reply to %s", msg_ip);
                
                /* Master initiates TCP connection to new node */
                int sockfd = dsm_tcp_connect(msg_ip, msg_port);
                if (sockfd >= 0) {
                    LOG_INFO("TCP connection established to new node %s:%d", msg_ip, msg_port);
                    
                    /* Add node to table */
                    int node_id = dsm_node_table_add(msg_ip, msg_port, msg_chunks, 
                                                     DSM_ROLE_WORKER, sockfd);
                    if (node_id > 0) {
                        LOG_INFO("Assigned node_id %d to new node", node_id);
                        
                        /* Allocate global address range for worker's donated memory */
                        size_t worker_region_size = msg_chunks * DSM_PAGE_SIZE;
                        uint64_t worker_global_base = ctx->next_global_addr;
                        ctx->next_global_addr += worker_region_size;
                        
                        LOG_INFO("Allocated global range 0x%lx - 0x%lx for node %d (%d chunks)",
                                worker_global_base, worker_global_base + worker_region_size - 1,
                                node_id, msg_chunks);
                        
                        /* Register worker's memory page-by-page in ownership table */
                        size_t num_pages = msg_chunks;
                        for (size_t i = 0; i < num_pages; i++) {
                            dsm_ownership_register(worker_global_base + i * DSM_PAGE_SIZE, 
                                                 DSM_PAGE_SIZE, node_id);
                        }
                        
                        /* Send join response with assigned node_id and global address */
                        dsm_msg_join_response_t join_resp;
                        join_resp.assigned_node_id = node_id;
                        join_resp.global_base_addr = worker_global_base;
                        join_resp.chunks = msg_chunks;
                        
                        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_JOIN_RESPONSE, 
                                                                  node_id, sizeof(join_resp));
                        dsm_tcp_send_raw(sockfd, &hdr, sizeof(hdr));
                        dsm_tcp_send_raw(sockfd, &join_resp, sizeof(join_resp));
                        
                        /* Broadcast updated node table to all nodes */
                        dsm_node_table_broadcast();
                    }
                }
            }
        }
        /* Handle DISCOVER_REPLY message */
        else if (sscanf(buf, "DISCOVER_REPLY/%15[^/]/%d/%d", msg_ip, &msg_port, &msg_chunks) == 3) {
            LOG_INFO("Discovery reply from master %s:%d", msg_ip, msg_port);
            
            /* Signal that master was found */
            pthread_mutex_lock(&ctx->discovery_mutex);
            ctx->master_found = 1;
            memset(ctx->master_ip, 0, sizeof(ctx->master_ip));
            snprintf(ctx->master_ip, sizeof(ctx->master_ip), "%s", msg_ip);
            ctx->master_port = msg_port;
            pthread_cond_signal(&ctx->discovery_cond);
            pthread_mutex_unlock(&ctx->discovery_mutex);
        }
    }
    
    LOG_DEBUG("UDP listener thread exiting");
    return NULL;
}

/*============================================================================
 * Public Functions
 *===========================================================================*/

int dsm_udp_init(void) {
    LOG_DEBUG("Initializing UDP subsystem");
    
    /* Create UDP socket */
    g_udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_socket < 0) {
        LOG_ERROR("Failed to create UDP socket: %s", strerror(errno));
        return -1;
    }
    
    /* Enable broadcast */
    int broadcast = 1;
    if (setsockopt(g_udp_socket, SOL_SOCKET, SO_BROADCAST, 
                   &broadcast, sizeof(broadcast)) < 0) {
        LOG_ERROR("Failed to enable broadcast: %s", strerror(errno));
        close(g_udp_socket);
        g_udp_socket = -1;
        return -1;
    }
    
    /* Enable address reuse */
    int reuse = 1;
    if (setsockopt(g_udp_socket, SOL_SOCKET, SO_REUSEADDR, 
                   &reuse, sizeof(reuse)) < 0) {
        LOG_WARN("Failed to enable address reuse: %s", strerror(errno));
    }
    
    /* Bind to discovery port */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(DSM_DISCOVERY_PORT);
    
    if (bind(g_udp_socket, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERROR("Failed to bind UDP socket: %s", strerror(errno));
        close(g_udp_socket);
        g_udp_socket = -1;
        return -1;
    }
    
    dsm_context_t *ctx = dsm_get_context();
    ctx->udp_fd = g_udp_socket;
    
    LOG_INFO("UDP socket bound to port %d", DSM_DISCOVERY_PORT);
    return 0;
}

void dsm_udp_shutdown(void) {
    LOG_DEBUG("Shutting down UDP subsystem");
    
    if (g_udp_socket >= 0) {
        close(g_udp_socket);
        g_udp_socket = -1;
    }
    
    dsm_context_t *ctx = dsm_get_context();
    ctx->udp_fd = -1;
}

int dsm_udp_broadcast_discover(const char *ip, uint16_t port, uint32_t chunks) {
    if (g_udp_socket < 0) {
        LOG_ERROR("UDP socket not initialized");
        return -1;
    }
    
    /* Format discovery message */
    char msg[UDP_BUFFER_SIZE];
    snprintf(msg, sizeof(msg), DISCOVER_MSG_FMT, ip, port, chunks);
    
    /* Broadcast address */
    struct sockaddr_in bcast_addr;
    memset(&bcast_addr, 0, sizeof(bcast_addr));
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_port = htons(DSM_DISCOVERY_PORT);
    bcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
    
    ssize_t sent = sendto(g_udp_socket, msg, strlen(msg), 0,
                          (struct sockaddr*)&bcast_addr, sizeof(bcast_addr));
    if (sent < 0) {
        LOG_ERROR("Failed to send discovery broadcast: %s", strerror(errno));
        return -1;
    }
    
    LOG_INFO("Sent discovery broadcast: %s", msg);
    return 0;
}

int dsm_udp_start_listener(void) {
    if (g_udp_running) {
        LOG_WARN("UDP listener already running");
        return 0;
    }
    
    g_udp_running = 1;
    
    if (pthread_create(&g_udp_listener_thread, NULL, 
                       udp_listener_thread_func, NULL) != 0) {
        LOG_ERROR("Failed to create UDP listener thread: %s", strerror(errno));
        g_udp_running = 0;
        return -1;
    }
    
    LOG_INFO("UDP listener thread started");
    return 0;
}

void dsm_udp_stop_listener(void) {
    if (!g_udp_running) {
        return;
    }
    
    LOG_DEBUG("Stopping UDP listener");
    g_udp_running = 0;
    
    /* Listener will exit on next select timeout */
    pthread_join(g_udp_listener_thread, NULL);
    LOG_DEBUG("UDP listener thread joined");
}

dsm_role_t dsm_udp_wait_discovery(void) {
    dsm_context_t *ctx = dsm_get_context();
    
    pthread_mutex_lock(&ctx->discovery_mutex);
    
    /* Wait for discovery_cond with timeout */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += DSM_DISCOVERY_TIMEOUT;
    
    while (!ctx->master_found && ctx->running) {
        int ret = pthread_cond_timedwait(&ctx->discovery_cond, 
                                         &ctx->discovery_mutex, &ts);
        if (ret == ETIMEDOUT) {
            LOG_DEBUG("Discovery timeout - no master found");
            break;
        }
        if (ret != 0 && ret != ETIMEDOUT) {
            LOG_ERROR("pthread_cond_timedwait error: %d", ret);
            break;
        }
    }
    
    int found = ctx->master_found;
    pthread_mutex_unlock(&ctx->discovery_mutex);
    
    if (found) {
        return DSM_ROLE_WORKER;
    }
    return DSM_ROLE_MASTER;
}
