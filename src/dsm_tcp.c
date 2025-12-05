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
    
    LOG_DEBUG("Handling message type=%d from fd=%d, src_node=%d, payload_len=%d", 
              header->type, from_fd, header->src_node, header->payload_len);
    
    /* CRITICAL: Associate this socket with the source node ID */
    /* This allows master to send responses back to workers */
    if (header->src_node > 0 && header->src_node != ctx->local_node_id) {
        dsm_node_t *node = dsm_node_table_get(header->src_node);
        if (node) {
            if (node->tcp_fd != from_fd) {
                LOG_INFO("[SOCKET] Associating node %d with fd=%d (was fd=%d)",
                         header->src_node, from_fd, node->tcp_fd);
                dsm_node_table_set_socket(header->src_node, from_fd);
            }
        } else {
            LOG_WARN("[SOCKET] Received message from unknown node %d (fd=%d)",
                     header->src_node, from_fd);
        }
        
        /* CRITICAL: If we're a worker receiving from master (node 1), update master_fd */
        if (header->src_node == 1 && ctx->master_fd != from_fd) {
            LOG_INFO("[SOCKET] Setting master_fd=%d (was %d)", from_fd, ctx->master_fd);
            ctx->master_fd = from_fd;
        }
    }
    
    switch (header->type) {
        case DSM_MSG_JOIN_RESPONSE: {
            /* Worker receives assigned node_id and global address from master */
            if (header->payload_len >= sizeof(dsm_msg_join_response_t)) {
                dsm_msg_join_response_t *resp = (dsm_msg_join_response_t*)payload;
                ctx->local_node_id = resp->assigned_node_id;
                ctx->next_global_addr = resp->global_base_addr;
                LOG_INFO("Received node_id assignment: %d, global_base: 0x%lx, chunks: %d", 
                        resp->assigned_node_id, resp->global_base_addr, resp->chunks);
            } else if (header->payload_len >= sizeof(uint32_t)) {
                /* Fallback for old-style response (just node_id) */
                uint32_t assigned_id = *(uint32_t*)payload;
                ctx->local_node_id = assigned_id;
                LOG_INFO("Received node_id assignment: %d (legacy)", assigned_id);
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
                LOG_INFO("[PAGE_REQ] Page request for addr=0x%lx from node %d", 
                         req->global_addr, header->src_node);
                
                /* Check if we actually own this page and can serve it */
                dsm_page_t *page = dsm_page_lookup(req->global_addr);
                if (!page) {
                    LOG_ERROR("[PAGE_REQ] Page 0x%lx not found in our page table", req->global_addr);
                    break;
                }
                
                /* If the page is INVALID, we don't have valid data to send */
                if (page->state == DSM_PAGE_INVALID) {
                    LOG_WARN("[PAGE_REQ] Page 0x%lx is INVALID on this node", req->global_addr);
                    
                    /* If we're master, check ownership table and forward to actual owner */
                    if (ctx->local_role == DSM_ROLE_MASTER) {
                        int actual_owner = dsm_ownership_get(req->global_addr);
                        LOG_INFO("[PAGE_REQ] Master forwarding request to actual owner node %d", actual_owner);
                        
                        if (actual_owner > 0 && (uint32_t)actual_owner != ctx->local_node_id) {
                            /* Forward the request to the actual owner */
                            dsm_msg_header_t fwd_hdr = dsm_create_header(DSM_MSG_PAGE_REQUEST, 
                                                                          actual_owner, sizeof(*req));
                            /* Modify src_node to be the original requester */
                            dsm_msg_page_request_t fwd_req = *req;
                            /* Note: The actual owner will send directly to header->src_node */
                            
                            /* We need to tell the owner who to reply to */
                            fwd_hdr.src_node = header->src_node;  /* Original requester */
                            dsm_tcp_send(actual_owner, &fwd_hdr, &fwd_req);
                            LOG_INFO("[PAGE_REQ] Forwarded page request to node %d for requester %d",
                                     actual_owner, header->src_node);
                        } else {
                            LOG_ERROR("[PAGE_REQ] Cannot forward: actual_owner=%d, local=%d",
                                      actual_owner, ctx->local_node_id);
                        }
                    }
                    break;
                }
                
                /* We have valid data - send the page */
                LOG_INFO("[PAGE_REQ] Serving page 0x%lx (state=%d) to node %d",
                         req->global_addr, page->state, header->src_node);
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
            if (header->payload_len >= sizeof(dsm_msg_alloc_response_t)) {
                dsm_msg_alloc_response_t *resp = (dsm_msg_alloc_response_t*)payload;
                LOG_INFO("[ALLOC] Received allocation response: addr=0x%lx, size=%zu, owner=%d", 
                         resp->global_addr, resp->size, resp->owner_id);
                
                /* Signal the waiting allocation request */
                pthread_mutex_lock(&ctx->alloc_mutex);
                ctx->alloc_response_addr = resp->global_addr;
                ctx->alloc_pending = false;
                pthread_cond_signal(&ctx->alloc_cond);
                pthread_mutex_unlock(&ctx->alloc_mutex);
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
        
        /*====================================================================
         * Invalidate/Exclusive Ownership Protocol Messages
         *====================================================================*/
        
        case DSM_MSG_PAGE_WRITE_REQ: {
            /* Node requesting write access (ownership transfer) */
            if (header->payload_len >= sizeof(dsm_msg_page_write_request_t)) {
                dsm_msg_page_write_request_t *req = (dsm_msg_page_write_request_t*)payload;
                LOG_INFO("[PROTOCOL] PAGE_WRITE_REQ for addr=0x%lx from node %d", 
                         req->global_addr, req->requesting_node);
                
                if (ctx->local_role == DSM_ROLE_MASTER) {
                    /* Master coordinates the ownership transfer */
                    dsm_handle_page_write_request(req->requesting_node, req->global_addr);
                } else {
                    /* We're the current owner - transfer ownership to requester */
                    LOG_INFO("[PROTOCOL] Transferring page 0x%lx to node %d", 
                             req->global_addr, req->requesting_node);
                    dsm_page_send_with_ownership(req->requesting_node, req->global_addr, 1);
                }
            }
            break;
        }
        
        case DSM_MSG_PAGE_INVALIDATE: {
            /* Master telling us to invalidate our copy */
            if (header->payload_len >= sizeof(dsm_msg_page_invalidate_t)) {
                dsm_msg_page_invalidate_t *inv = (dsm_msg_page_invalidate_t*)payload;
                LOG_INFO("[PROTOCOL] PAGE_INVALIDATE for addr=0x%lx (new_owner=%d)", 
                         inv->global_addr, inv->new_owner);
                dsm_handle_page_invalidate(inv->global_addr, inv->new_owner);
            }
            break;
        }
        
        case DSM_MSG_INVALIDATE_ACK: {
            /* Node acknowledging invalidation */
            if (header->payload_len >= sizeof(dsm_msg_invalidate_ack_t)) {
                dsm_msg_invalidate_ack_t *ack = (dsm_msg_invalidate_ack_t*)payload;
                LOG_INFO("[PROTOCOL] INVALIDATE_ACK for addr=0x%lx from node %d", 
                         ack->global_addr, ack->acking_node);
                dsm_handle_invalidate_ack(ack->acking_node, ack->global_addr);
            }
            break;
        }
        
        case DSM_MSG_OWNERSHIP_XFER: {
            /* Receiving ownership of a page */
            if (header->payload_len >= sizeof(dsm_msg_ownership_xfer_t)) {
                dsm_msg_ownership_xfer_t *xfer = (dsm_msg_ownership_xfer_t*)payload;
                LOG_INFO("[PROTOCOL] OWNERSHIP_XFER for addr=0x%lx from node %d", 
                         xfer->global_addr, xfer->old_owner);
                dsm_handle_ownership_xfer(xfer->global_addr, xfer->data, xfer->size, 
                                          xfer->version, xfer->old_owner);
            }
            break;
        }
        
        /*====================================================================
         * Page Discovery Messages
         *====================================================================*/
        
        case DSM_MSG_LIST_PAGES_REQ: {
            /* Worker requesting list of all pages from master */
            if (ctx->local_role == DSM_ROLE_MASTER) {
                LOG_INFO("[LIST_PAGES] Request from node %d", header->src_node);
                
                uint32_t count = 0;
                dsm_msg_list_pages_t *pages = dsm_ownership_collect_all(&count);
                
                if (pages) {
                    size_t payload_size = sizeof(dsm_msg_list_pages_t) + 
                                          count * sizeof(dsm_page_entry_t);
                    dsm_msg_header_t resp_hdr = dsm_create_header(DSM_MSG_LIST_PAGES_RESP,
                                                                   header->src_node, payload_size);
                    dsm_tcp_send(header->src_node, &resp_hdr, pages);
                    LOG_INFO("[LIST_PAGES] Sent %u pages to node %d", count, header->src_node);
                    free(pages);
                } else {
                    LOG_ERROR("[LIST_PAGES] Failed to collect ownership entries");
                }
            }
            break;
        }
        
        case DSM_MSG_LIST_PAGES_RESP: {
            /* Response from master with list of all pages */
            if (header->payload_len >= sizeof(dsm_msg_list_pages_t)) {
                dsm_msg_list_pages_t *resp = (dsm_msg_list_pages_t*)payload;
                LOG_INFO("[LIST_PAGES] Received %u pages from master", resp->count);
                
                /* Store the response for the waiting thread */
                dsm_store_list_pages_response(resp, header->payload_len);
            }
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
    dsm_context_t *ctx = dsm_get_context();
    int sockfd = -1;
    
    LOG_DEBUG("[TCP_SEND] Sending msg type=%d to node %d (local_role=%d, master_fd=%d)", 
              header->type, node_id, ctx->local_role, ctx->master_fd);
    
    /* Special case: Worker sending to master (node 1) - use master_fd */
    if (ctx->local_role == DSM_ROLE_WORKER && node_id == 1 && ctx->master_fd >= 0) {
        sockfd = ctx->master_fd;
        LOG_DEBUG("[TCP_SEND] Worker using master_fd=%d to send to master", sockfd);
    } else {
        /* Look up in node table */
        dsm_node_t *node = dsm_node_table_get(node_id);
        if (node) {
            LOG_DEBUG("[TCP_SEND] Node table lookup: node_id=%d, tcp_fd=%d, ip=%s, port=%d", 
                      node->node_id, node->tcp_fd, node->ip, node->port);
            if (node->tcp_fd >= 0) {
                sockfd = node->tcp_fd;
            }
        } else {
            LOG_DEBUG("[TCP_SEND] Node %d not found in node table", node_id);
        }
    }
    
    if (sockfd < 0) {
        /* Try connect-on-demand: look up node's IP/port and connect */
        dsm_node_t *node = dsm_node_table_get(node_id);
        if (node && node->ip[0] != '\0' && node->port > 0) {
            LOG_INFO("[TCP_SEND] No connection to node %d - attempting connect-on-demand to %s:%d",
                     node_id, node->ip, node->port);
            sockfd = dsm_tcp_connect(node->ip, node->port);
            if (sockfd >= 0) {
                /* Update node table with new socket */
                dsm_node_table_set_socket(node_id, sockfd);
                LOG_INFO("[TCP_SEND] Connect-on-demand succeeded: node %d now on fd=%d", node_id, sockfd);
            } else {
                LOG_ERROR("[TCP_SEND] Connect-on-demand failed for node %d (%s:%d)", 
                          node_id, node->ip, node->port);
            }
        }
        
        if (sockfd < 0) {
            LOG_ERROR("[TCP_SEND] Cannot send to node %d - not connected (role=%d, master_fd=%d)", 
                      node_id, ctx->local_role, ctx->master_fd);
            /* Dump node table for debugging */
            LOG_DEBUG("[TCP_SEND] === Node table dump ===");
            for (uint32_t i = 1; i <= dsm_node_table_count(); i++) {
                dsm_node_t *n = dsm_node_table_get(i);
                if (n) {
                    LOG_DEBUG("[TCP_SEND]   Node %d: ip=%s, port=%d, fd=%d, role=%d", 
                              n->node_id, n->ip, n->port, n->tcp_fd, n->role);
                }
            }
            return -1;
        }
    }
    
    /* Send header */
    if (dsm_tcp_send_raw(sockfd, header, sizeof(*header)) != 0) {
        LOG_ERROR("[TCP_SEND] Failed to send header to node %d via fd=%d", node_id, sockfd);
        return -1;
    }
    
    /* Send payload if present */
    if (payload && header->payload_len > 0) {
        if (dsm_tcp_send_raw(sockfd, payload, header->payload_len) != 0) {
            LOG_ERROR("[TCP_SEND] Failed to send payload to node %d via fd=%d", node_id, sockfd);
            return -1;
        }
    }
    
    LOG_DEBUG("[TCP_SEND] Success: msg type=%d (%zu+%d bytes) to node %d via fd=%d", 
              header->type, sizeof(*header), header->payload_len, node_id, sockfd);
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
