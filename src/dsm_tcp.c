/**
 * @file dsm_tcp.c
 * @brief TCP connection management for DSM
 */

#include "dsm_types.h"
#include "dsm_network.h"
#include "dsm_memory.h"
#include "dsm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/*============================================================================
 * Constants
 *===========================================================================*/

#define MAX_EPOLL_EVENTS    64
#define TCP_BUFFER_SIZE     65536
#define CONNECT_TIMEOUT_SEC 5

/*============================================================================
 * Static Variables
 *===========================================================================*/

static int g_tcp_listen_fd = -1;
static int g_epoll_fd = -1;
static pthread_t g_tcp_server_thread;
static volatile int g_tcp_running = 0;

/*============================================================================
 * Helper Functions
 *===========================================================================*/

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_tcp_nodelay(int fd) {
    int flag = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

static int set_keepalive(int fd) {
    int keepalive = 1;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
}

/*============================================================================
 * Message Handling
 *===========================================================================*/

int dsm_handle_message(int from_fd, dsm_msg_header_t *header, void *payload) {
    dsm_context_t *ctx = dsm_get_context();
    
    LOG_DEBUG("Handling message type=%d from fd=%d, payload_len=%d", 
              header->type, from_fd, header->payload_len);
    
    switch (header->type) {
        case DSM_MSG_JOIN_RESPONSE: {
            /* Worker receives assigned node_id from master */
            if (header->payload_len >= sizeof(uint32_t)) {
                uint32_t assigned_id = *(uint32_t*)payload;
                ctx->local_node_id = assigned_id;
                LOG_INFO("Received node_id assignment: %d", assigned_id);
            }
            break;
        }
        
        case DSM_MSG_NODE_TABLE: {
            /* Receive full node table sync from master */
            LOG_INFO("Received node table update");
            dsm_node_table_deserialize(payload, header->payload_len);
            break;
        }
        
        case DSM_MSG_NEW_NODE: {
            /* Notification of new node joining */
            if (header->payload_len >= sizeof(dsm_msg_node_info_t)) {
                dsm_msg_node_info_t *info = (dsm_msg_node_info_t*)payload;
                LOG_INFO("New node announcement: id=%d, ip=%s, port=%d",
                        info->node_id, info->ip, info->port);
            }
            break;
        }
        
        case DSM_MSG_NODE_LEFT: {
            /* Notification of node leaving */
            if (header->payload_len >= sizeof(uint32_t)) {
                uint32_t left_id = *(uint32_t*)payload;
                LOG_INFO("Node %d left the network", left_id);
                dsm_node_table_remove(left_id);
            }
            break;
        }
        
        case DSM_MSG_PAGE_REQUEST: {
            /* Remote node requesting a page */
            if (header->payload_len >= sizeof(dsm_msg_page_request_t)) {
                dsm_msg_page_request_t *req = (dsm_msg_page_request_t*)payload;
                LOG_INFO("Page request for addr=0x%lx from node %d", 
                         req->global_addr, header->src_node);
                dsm_page_send(header->src_node, req->global_addr);
            }
            break;
        }
        
        case DSM_MSG_PAGE_DATA: {
            /* Received page data */
            if (header->payload_len >= sizeof(dsm_msg_page_data_t)) {
                dsm_msg_page_data_t *data = (dsm_msg_page_data_t*)payload;
                LOG_INFO("Page data received for addr=0x%lx, size=%d", 
                         data->global_addr, data->size);
                dsm_page_install(data->global_addr, data->data, data->size, data->version);
            }
            break;
        }
        
        case DSM_MSG_ALLOC_REQUEST: {
            /* Memory allocation request (master handles) */
            if (ctx->local_role == DSM_ROLE_MASTER) {
                dsm_msg_alloc_request_t *req = (dsm_msg_alloc_request_t*)payload;
                LOG_DEBUG("Allocation request from node %d, size=%zu", 
                         header->src_node, req->size);
                
                /* Allocate from global pool */
                pthread_mutex_lock(&ctx->alloc_mutex);
                uint64_t addr = ctx->next_global_addr;
                size_t aligned_size = ((req->size + DSM_PAGE_SIZE - 1) / DSM_PAGE_SIZE) * DSM_PAGE_SIZE;
                ctx->next_global_addr += aligned_size;
                pthread_mutex_unlock(&ctx->alloc_mutex);
                
                /* Register ownership */
                dsm_ownership_register(addr, aligned_size, header->src_node);
                
                /* Send response */
                dsm_msg_alloc_response_t resp;
                resp.global_addr = addr;
                resp.size = aligned_size;
                resp.owner_id = header->src_node;
                resp.request_id = req->request_id;
                
                dsm_msg_header_t resp_hdr = dsm_create_header(DSM_MSG_ALLOC_RESPONSE,
                                                              header->src_node, sizeof(resp));
                dsm_tcp_send(header->src_node, &resp_hdr, &resp);
            }
            break;
        }
        
        case DSM_MSG_ALLOC_RESPONSE: {
            /* Allocation response from master */
            /* This is handled synchronously in dsm_mem_alloc via a condition variable */
            /* For now, we just log it */
            if (header->payload_len >= sizeof(dsm_msg_alloc_response_t)) {
                dsm_msg_alloc_response_t *resp = (dsm_msg_alloc_response_t*)payload;
                LOG_DEBUG("Allocation response: addr=0x%lx, size=%zu", 
                         resp->global_addr, resp->size);
            }
            break;
        }
        
        case DSM_MSG_LOCK_REQUEST: {
            /* Lock request (master handles) */
            if (ctx->local_role == DSM_ROLE_MASTER && header->payload_len >= sizeof(dsm_msg_lock_t)) {
                dsm_msg_lock_t *req = (dsm_msg_lock_t*)payload;
                dsm_sync_handle_lock_request(header->src_node, req->lock_id);
            }
            break;
        }
        
        case DSM_MSG_LOCK_GRANT: {
            /* Lock granted by master */
            if (header->payload_len >= sizeof(dsm_msg_lock_t)) {
                dsm_msg_lock_t *grant = (dsm_msg_lock_t*)payload;
                LOG_INFO("Received LOCK_GRANT for lock %d", grant->lock_id);
                dsm_lock_t *lock = &ctx->locks[grant->lock_id];
                pthread_mutex_lock(&lock->mutex);
                lock->locked = true;
                lock->holder_id = ctx->local_node_id;
                LOG_DEBUG("Lock %d: set locked=true, holder_id=%d, signaling cond", 
                         grant->lock_id, ctx->local_node_id);
                pthread_cond_signal(&lock->cond);
                pthread_mutex_unlock(&lock->mutex);
            }
            break;
        }
        
        case DSM_MSG_LOCK_RELEASE: {
            /* Lock release (master handles) */
            if (ctx->local_role == DSM_ROLE_MASTER && header->payload_len >= sizeof(dsm_msg_lock_t)) {
                dsm_msg_lock_t *rel = (dsm_msg_lock_t*)payload;
                dsm_sync_handle_lock_release(header->src_node, rel->lock_id);
            }
            break;
        }
        
        case DSM_MSG_BARRIER_ENTER: {
            /* Barrier entry (master handles) */
            if (ctx->local_role == DSM_ROLE_MASTER) {
                dsm_sync_handle_barrier_enter(header->src_node);
            }
            break;
        }
        
        case DSM_MSG_BARRIER_RELEASE: {
            /* Barrier release from master */
            pthread_mutex_lock(&ctx->barrier.mutex);
            pthread_cond_broadcast(&ctx->barrier.cond);
            pthread_mutex_unlock(&ctx->barrier.mutex);
            break;
        }
        
        case DSM_MSG_HEARTBEAT: {
            /* Update heartbeat timestamp */
            dsm_node_table_heartbeat(header->src_node);
            
            /* Send ack */
            dsm_msg_header_t ack_hdr = dsm_create_header(DSM_MSG_HEARTBEAT_ACK, 
                                                         header->src_node, 0);
            dsm_node_t *node = dsm_node_table_get(header->src_node);
            if (node && node->tcp_fd >= 0) {
                dsm_tcp_send_raw(node->tcp_fd, &ack_hdr, sizeof(ack_hdr));
            }
            break;
        }
        
        case DSM_MSG_HEARTBEAT_ACK: {
            dsm_node_table_heartbeat(header->src_node);
            break;
        }
        
        case DSM_MSG_SHUTDOWN: {
            LOG_INFO("Received shutdown from node %d", header->src_node);
            dsm_node_table_remove(header->src_node);
            break;
        }
        
        default:
            LOG_WARN("Unknown message type: %d", header->type);
            break;
    }
    
    return 0;
}

/*============================================================================
 * TCP Server Thread
 *===========================================================================*/

static void handle_new_connection(int client_fd, struct sockaddr_in *client_addr) {
    dsm_context_t *ctx = dsm_get_context();
    char *client_ip = inet_ntoa(client_addr->sin_addr);
    int client_port = ntohs(client_addr->sin_port);
    
    LOG_INFO("New TCP connection from %s:%d (fd=%d)", client_ip, client_port, client_fd);
    
    set_nonblocking(client_fd);
    set_tcp_nodelay(client_fd);
    set_keepalive(client_fd);
    
    /* Add to epoll - use level-triggered mode for reliable message delivery */
    struct epoll_event ev;
    ev.events = EPOLLIN;  /* Level-triggered, NOT edge-triggered */
    ev.data.fd = client_fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
        LOG_ERROR("Failed to add client to epoll: %s", strerror(errno));
        close(client_fd);
        return;
    }
    
    /* For workers: this connection is from master */
    if (ctx->local_role == DSM_ROLE_WORKER && ctx->master_fd < 0) {
        ctx->master_fd = client_fd;
        LOG_INFO("Connected to master via fd=%d", client_fd);
    }
}

static void handle_client_data(int client_fd) {
    static uint8_t recv_buf[TCP_BUFFER_SIZE];
    dsm_msg_header_t header;
    
    /* Process all available messages in a loop */
    while (1) {
        /* Peek at header to see if a complete message is available */
        ssize_t n = recv(client_fd, &header, sizeof(header), MSG_PEEK);
        if (n <= 0) {
            if (n == 0) {
                LOG_INFO("Client fd=%d disconnected", client_fd);
                epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                close(client_fd);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("recv error on fd=%d: %s", client_fd, strerror(errno));
                epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                close(client_fd);
            }
            /* EAGAIN/EWOULDBLOCK means no more data - exit loop normally */
            return;
        }
        
        if (n < (ssize_t)sizeof(header)) {
            return; /* Incomplete header, wait for more data */
        }
        
        /* Actually read the header */
        n = recv(client_fd, &header, sizeof(header), 0);
        if (n != sizeof(header)) {
            LOG_ERROR("Failed to read header from fd=%d", client_fd);
            return;
        }
        
        /* Validate magic */
        if (header.magic != DSM_MAGIC) {
            LOG_ERROR("Invalid magic from fd=%d: 0x%x", client_fd, header.magic);
            return;
        }
        
        /* Read payload if present */
        void *payload = NULL;
        if (header.payload_len > 0) {
            if (header.payload_len > TCP_BUFFER_SIZE) {
                LOG_ERROR("Payload too large: %d", header.payload_len);
                return;
            }
            
            size_t total_read = 0;
            while (total_read < header.payload_len) {
                n = recv(client_fd, recv_buf + total_read, 
                        header.payload_len - total_read, 0);
                if (n <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        usleep(1000); /* Small delay */
                        continue;
                    }
                    LOG_ERROR("Failed to read payload from fd=%d", client_fd);
                    return;
                }
                total_read += n;
            }
            payload = recv_buf;
        }
        
        /* Handle the message */
        LOG_DEBUG("Processing message type=%d from fd=%d", header.type, client_fd);
        dsm_handle_message(client_fd, &header, payload);
    }
}

static void* tcp_server_thread_func(void *arg) {
    (void)arg;
    dsm_context_t *ctx = dsm_get_context();
    struct epoll_event events[MAX_EPOLL_EVENTS];
    
    LOG_DEBUG("TCP server thread started");
    
    while (g_tcp_running && ctx->running) {
        int nfds = epoll_wait(g_epoll_fd, events, MAX_EPOLL_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait error: %s", strerror(errno));
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == g_tcp_listen_fd) {
                /* New connection */
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                
                int client_fd = accept(g_tcp_listen_fd, 
                                       (struct sockaddr*)&client_addr, &client_len);
                if (client_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        LOG_ERROR("accept error: %s", strerror(errno));
                    }
                    continue;
                }
                
                handle_new_connection(client_fd, &client_addr);
            } else {
                /* Data from existing client */
                if (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                    handle_client_data(events[i].data.fd);
                }
            }
        }
    }
    
    LOG_DEBUG("TCP server thread exiting");
    return NULL;
}

/*============================================================================
 * Public Functions
 *===========================================================================*/

int dsm_tcp_init(uint16_t port) {
    LOG_DEBUG("Initializing TCP subsystem on port %d", port);
    
    /* Create listen socket */
    g_tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp_listen_fd < 0) {
        LOG_ERROR("Failed to create TCP socket: %s", strerror(errno));
        return -1;
    }
    
    /* Enable address reuse */
    int reuse = 1;
    setsockopt(g_tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    /* Bind */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(port);
    
    if (bind(g_tcp_listen_fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERROR("Failed to bind TCP socket: %s", strerror(errno));
        close(g_tcp_listen_fd);
        g_tcp_listen_fd = -1;
        return -1;
    }
    
    /* Listen */
    if (listen(g_tcp_listen_fd, SOMAXCONN) < 0) {
        LOG_ERROR("Failed to listen on TCP socket: %s", strerror(errno));
        close(g_tcp_listen_fd);
        g_tcp_listen_fd = -1;
        return -1;
    }
    
    set_nonblocking(g_tcp_listen_fd);
    
    /* Create epoll */
    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0) {
        LOG_ERROR("Failed to create epoll: %s", strerror(errno));
        close(g_tcp_listen_fd);
        g_tcp_listen_fd = -1;
        return -1;
    }
    
    /* Add listen socket to epoll */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_tcp_listen_fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_tcp_listen_fd, &ev) < 0) {
        LOG_ERROR("Failed to add listen fd to epoll: %s", strerror(errno));
        close(g_epoll_fd);
        close(g_tcp_listen_fd);
        g_epoll_fd = -1;
        g_tcp_listen_fd = -1;
        return -1;
    }
    
    dsm_context_t *ctx = dsm_get_context();
    ctx->tcp_listen_fd = g_tcp_listen_fd;
    ctx->epoll_fd = g_epoll_fd;
    
    LOG_INFO("TCP server initialized on port %d", port);
    return 0;
}

void dsm_tcp_shutdown(void) {
    LOG_DEBUG("Shutting down TCP subsystem");
    
    /* Send shutdown to all nodes */
    dsm_context_t *ctx = dsm_get_context();
    if (ctx->node_table) {
        dsm_msg_header_t hdr = dsm_create_header(DSM_MSG_SHUTDOWN, 0, 0);
        dsm_tcp_broadcast(&hdr, NULL);
    }
    
    if (g_epoll_fd >= 0) {
        close(g_epoll_fd);
        g_epoll_fd = -1;
    }
    
    if (g_tcp_listen_fd >= 0) {
        close(g_tcp_listen_fd);
        g_tcp_listen_fd = -1;
    }
    
    ctx->tcp_listen_fd = -1;
    ctx->epoll_fd = -1;
}

int dsm_tcp_start_server(void) {
    if (g_tcp_running) {
        LOG_WARN("TCP server already running");
        return 0;
    }
    
    g_tcp_running = 1;
    
    if (pthread_create(&g_tcp_server_thread, NULL, tcp_server_thread_func, NULL) != 0) {
        LOG_ERROR("Failed to create TCP server thread: %s", strerror(errno));
        g_tcp_running = 0;
        return -1;
    }
    
    LOG_INFO("TCP server thread started");
    return 0;
}

void dsm_tcp_stop_server(void) {
    if (!g_tcp_running) {
        return;
    }
    
    LOG_DEBUG("Stopping TCP server");
    g_tcp_running = 0;
    
    pthread_join(g_tcp_server_thread, NULL);
    LOG_DEBUG("TCP server thread joined");
}

int dsm_tcp_connect(const char *ip, uint16_t port) {
    LOG_DEBUG("Connecting to %s:%d", ip, port);
    
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        LOG_ERROR("Invalid IP address: %s", ip);
        close(sockfd);
        return -1;
    }
    
    /* Set connection timeout */
    struct timeval tv;
    tv.tv_sec = CONNECT_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to connect to %s:%d: %s", ip, port, strerror(errno));
        close(sockfd);
        return -1;
    }
    
    set_tcp_nodelay(sockfd);
    set_keepalive(sockfd);
    set_nonblocking(sockfd);
    
    /* Add to epoll */
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = sockfd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, sockfd, &ev) < 0) {
        LOG_WARN("Failed to add socket to epoll: %s", strerror(errno));
    }
    
    LOG_INFO("Connected to %s:%d (fd=%d)", ip, port, sockfd);
    return sockfd;
}

void dsm_tcp_disconnect(uint32_t node_id) {
    dsm_node_t *node = dsm_node_table_get(node_id);
    if (node && node->tcp_fd >= 0) {
        LOG_INFO("Disconnecting from node %d (fd=%d)", node_id, node->tcp_fd);
        epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, node->tcp_fd, NULL);
        close(node->tcp_fd);
        node->tcp_fd = -1;
        node->state = DSM_NODE_STATE_DISCONNECTED;
    }
}

int dsm_tcp_send_raw(int sockfd, void *data, size_t len) {
    size_t total_sent = 0;
    uint8_t *buf = (uint8_t*)data;
    
    while (total_sent < len) {
        ssize_t sent = send(sockfd, buf + total_sent, len - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            LOG_ERROR("send error: %s", strerror(errno));
            return -1;
        }
        total_sent += sent;
    }
    
    return 0;
}

int dsm_tcp_recv_raw(int sockfd, void *buf, size_t len) {
    size_t total_recv = 0;
    uint8_t *ptr = (uint8_t*)buf;
    
    while (total_recv < len) {
        ssize_t n = recv(sockfd, ptr + total_recv, len - total_recv, 0);
        if (n <= 0) {
            if (n == 0) {
                LOG_ERROR("Connection closed");
                return -1;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            LOG_ERROR("recv error: %s", strerror(errno));
            return -1;
        }
        total_recv += n;
    }
    
    return 0;
}

int dsm_tcp_send(uint32_t node_id, dsm_msg_header_t *header, void *payload) {
    dsm_node_t *node = dsm_node_table_get(node_id);
    if (!node || node->tcp_fd < 0) {
        LOG_ERROR("Cannot send to node %d - not connected", node_id);
        return -1;
    }
    
    /* Send header */
    if (dsm_tcp_send_raw(node->tcp_fd, header, sizeof(*header)) != 0) {
        return -1;
    }
    
    /* Send payload if present */
    if (payload && header->payload_len > 0) {
        if (dsm_tcp_send_raw(node->tcp_fd, payload, header->payload_len) != 0) {
            return -1;
        }
    }
    
    LOG_DEBUG("Sent message type=%d to node %d", header->type, node_id);
    return 0;
}

int dsm_tcp_broadcast(dsm_msg_header_t *header, void *payload) {
    dsm_context_t *ctx = dsm_get_context();
    int success = 0;
    
    if (!ctx->node_table) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&ctx->node_table->rwlock);
    
    for (uint32_t i = 0; i < ctx->node_table->count; i++) {
        dsm_node_t *node = &ctx->node_table->nodes[i];
        
        /* Skip self and disconnected nodes */
        if (node->node_id == ctx->local_node_id || node->tcp_fd < 0) {
            continue;
        }
        
        header->dst_node = node->node_id;
        
        if (dsm_tcp_send_raw(node->tcp_fd, header, sizeof(*header)) == 0) {
            if (!payload || header->payload_len == 0 || 
                dsm_tcp_send_raw(node->tcp_fd, payload, header->payload_len) == 0) {
                success++;
            }
        }
    }
    
    pthread_rwlock_unlock(&ctx->node_table->rwlock);
    
    LOG_DEBUG("Broadcast message type=%d to %d nodes", header->type, success);
    return success > 0 ? 0 : -1;
}
