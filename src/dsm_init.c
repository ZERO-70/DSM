/**
 * @file dsm_init.c
 * @brief DSM initialization, configuration, and cleanup
 */

#include "dsm.h"
#include "dsm_types.h"
#include "dsm_network.h"
#include "dsm_memory.h"
#include "dsm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*============================================================================
 * Global Variables
 *===========================================================================*/

/** Global DSM context */
static dsm_context_t g_dsm_ctx;

/** Log level (default: INFO) */
dsm_log_level_t dsm_current_log_level = LOG_LEVEL_INFO;

/*============================================================================
 * Context Access
 *===========================================================================*/

dsm_context_t* dsm_get_context(void) {
    return &g_dsm_ctx;
}

/*============================================================================
 * Context Initialization
 *===========================================================================*/

int dsm_context_init(void) {
    memset(&g_dsm_ctx, 0, sizeof(g_dsm_ctx));
    
    g_dsm_ctx.local_node_id = 0;
    g_dsm_ctx.local_role = DSM_ROLE_UNKNOWN;
    g_dsm_ctx.udp_fd = -1;
    g_dsm_ctx.tcp_listen_fd = -1;
    g_dsm_ctx.epoll_fd = -1;
    g_dsm_ctx.master_fd = -1;
    g_dsm_ctx.fault_eventfd = -1;
    g_dsm_ctx.system_page_size = DSM_PAGE_SIZE;
    g_dsm_ctx.next_global_addr = 0x1000000; /* Start at 16MB offset */
    
    /* Initialize mutexes and conditions */
    if (pthread_mutex_init(&g_dsm_ctx.alloc_mutex, NULL) != 0) {
        LOG_ERROR("Failed to init alloc_mutex");
        return -1;
    }
    if (pthread_mutex_init(&g_dsm_ctx.fault_mutex, NULL) != 0) {
        LOG_ERROR("Failed to init fault_mutex");
        return -1;
    }
    if (pthread_cond_init(&g_dsm_ctx.fault_cond, NULL) != 0) {
        LOG_ERROR("Failed to init fault_cond");
        return -1;
    }
    if (pthread_mutex_init(&g_dsm_ctx.seq_mutex, NULL) != 0) {
        LOG_ERROR("Failed to init seq_mutex");
        return -1;
    }
    if (pthread_mutex_init(&g_dsm_ctx.discovery_mutex, NULL) != 0) {
        LOG_ERROR("Failed to init discovery_mutex");
        return -1;
    }
    if (pthread_cond_init(&g_dsm_ctx.discovery_cond, NULL) != 0) {
        LOG_ERROR("Failed to init discovery_cond");
        return -1;
    }
    
    /* Initialize invalidate tracking (for Invalidate/Exclusive protocol) */
    if (pthread_mutex_init(&g_dsm_ctx.invalidate_mutex, NULL) != 0) {
        LOG_ERROR("Failed to init invalidate_mutex");
        return -1;
    }
    if (pthread_cond_init(&g_dsm_ctx.invalidate_cond, NULL) != 0) {
        LOG_ERROR("Failed to init invalidate_cond");
        return -1;
    }
    g_dsm_ctx.invalidate_pending = 0;
    g_dsm_ctx.invalidate_addr = 0;
    
    /* Create eventfd for page fault signaling */
    g_dsm_ctx.fault_eventfd = eventfd(0, EFD_NONBLOCK);
    if (g_dsm_ctx.fault_eventfd < 0) {
        LOG_ERROR("Failed to create fault eventfd: %s", strerror(errno));
        return -1;
    }
    
    g_dsm_ctx.running = 1;
    g_dsm_ctx.initialized = 0;
    
    LOG_DEBUG("Context initialized");
    return 0;
}

void dsm_context_destroy(void) {
    g_dsm_ctx.running = 0;
    
    if (g_dsm_ctx.fault_eventfd >= 0) {
        close(g_dsm_ctx.fault_eventfd);
        g_dsm_ctx.fault_eventfd = -1;
    }
    
    pthread_mutex_destroy(&g_dsm_ctx.alloc_mutex);
    pthread_mutex_destroy(&g_dsm_ctx.fault_mutex);
    pthread_cond_destroy(&g_dsm_ctx.fault_cond);
    pthread_mutex_destroy(&g_dsm_ctx.seq_mutex);
    pthread_mutex_destroy(&g_dsm_ctx.discovery_mutex);
    pthread_cond_destroy(&g_dsm_ctx.discovery_cond);
    pthread_mutex_destroy(&g_dsm_ctx.invalidate_mutex);
    pthread_cond_destroy(&g_dsm_ctx.invalidate_cond);
    
    LOG_DEBUG("Context destroyed");
}

uint32_t dsm_get_next_seq(void) {
    uint32_t seq;
    pthread_mutex_lock(&g_dsm_ctx.seq_mutex);
    seq = g_dsm_ctx.seq_num++;
    pthread_mutex_unlock(&g_dsm_ctx.seq_mutex);
    return seq;
}

/*============================================================================
 * Message Handling Helpers
 *===========================================================================*/

dsm_msg_header_t dsm_create_header(dsm_msg_type_t type, uint32_t dst_node, 
                                   uint32_t payload_len) {
    dsm_msg_header_t hdr;
    hdr.magic = DSM_MAGIC;
    hdr.type = type;
    hdr.src_node = g_dsm_ctx.local_node_id;
    hdr.dst_node = dst_node;
    hdr.seq_num = dsm_get_next_seq();
    hdr.payload_len = payload_len;
    return hdr;
}

/*============================================================================
 * Get Local IP Address
 *===========================================================================*/

int dsm_net_get_local_ip(char *ip_buf, size_t buf_len) {
    struct ifaddrs *ifaddr, *ifa;
    int found = 0;
    
    if (getifaddrs(&ifaddr) == -1) {
        LOG_ERROR("getifaddrs failed: %s", strerror(errno));
        return -1;
    }
    
    /* Walk through linked list of interfaces */
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;
        
        /* Only IPv4 */
        if (ifa->ifa_addr->sa_family != AF_INET)
            continue;
        
        /* Skip loopback */
        if (strcmp(ifa->ifa_name, "lo") == 0)
            continue;
        
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        char *addr = inet_ntoa(sin->sin_addr);
        
        /* Skip localhost addresses */
        if (strncmp(addr, "127.", 4) == 0)
            continue;
        
        /* Prefer wlan/wifi interfaces */
        if (strncmp(ifa->ifa_name, "wl", 2) == 0 ||
            strncmp(ifa->ifa_name, "wifi", 4) == 0 ||
            strncmp(ifa->ifa_name, "en", 2) == 0 ||
            strncmp(ifa->ifa_name, "eth", 3) == 0) {
            strncpy(ip_buf, addr, buf_len - 1);
            ip_buf[buf_len - 1] = '\0';
            found = 1;
            LOG_INFO("Found local IP: %s on interface %s", ip_buf, ifa->ifa_name);
            break;
        }
        
        /* Fall back to any non-loopback interface */
        if (!found) {
            strncpy(ip_buf, addr, buf_len - 1);
            ip_buf[buf_len - 1] = '\0';
            found = 1;
        }
    }
    
    freeifaddrs(ifaddr);
    
    if (!found) {
        LOG_ERROR("No suitable network interface found");
        return -1;
    }
    
    return 0;
}

/*============================================================================
 * Public API: Initialization
 *===========================================================================*/

int dsm_init(int argc, char **argv) {
    if (argc < 3) {
        LOG_ERROR("Usage: %s <port> <chunks>", argv[0]);
        return -1;
    }
    
    uint16_t port = (uint16_t)atoi(argv[1]);
    uint32_t chunks = (uint32_t)atoi(argv[2]);
    
    if (port < 1024) {
        LOG_ERROR("Port must be >= 1024");
        return -1;
    }
    
    if (chunks < 1 || chunks > DSM_MAX_CHUNKS) {
        LOG_ERROR("Chunks must be between 1 and %d", DSM_MAX_CHUNKS);
        return -1;
    }
    
    return dsm_init_with_params(port, chunks);
}

int dsm_init_with_params(uint16_t port, uint32_t chunks) {
    LOG_INFO("Initializing DSM - port: %d, chunks: %d", port, chunks);
    
    /* Initialize context */
    if (dsm_context_init() != 0) {
        LOG_ERROR("Failed to initialize context");
        return -1;
    }
    
    g_dsm_ctx.local_port = port;
    g_dsm_ctx.local_chunks = chunks;
    
    /* Step 1: Get local WiFi IP address */
    if (dsm_net_get_local_ip(g_dsm_ctx.local_ip, sizeof(g_dsm_ctx.local_ip)) != 0) {
        LOG_ERROR("Failed to get local IP");
        dsm_context_destroy();
        return -1;
    }
    LOG_INFO("Local IP: %s", g_dsm_ctx.local_ip);
    
    /* Step 2: Initialize node table */
    if (dsm_node_table_init() != 0) {
        LOG_ERROR("Failed to initialize node table");
        dsm_context_destroy();
        return -1;
    }
    
    /* Step 3: Initialize TCP server (must be ready before discovery) */
    if (dsm_tcp_init(port) != 0) {
        LOG_ERROR("Failed to initialize TCP");
        dsm_node_table_destroy();
        dsm_context_destroy();
        return -1;
    }
    
    /* Start TCP server thread */
    if (dsm_tcp_start_server() != 0) {
        LOG_ERROR("Failed to start TCP server");
        dsm_tcp_shutdown();
        dsm_node_table_destroy();
        dsm_context_destroy();
        return -1;
    }
    
    /* Step 4: Initialize UDP discovery */
    if (dsm_udp_init() != 0) {
        LOG_ERROR("Failed to initialize UDP");
        dsm_tcp_shutdown();
        dsm_node_table_destroy();
        dsm_context_destroy();
        return -1;
    }
    
    /* Start UDP listener */
    if (dsm_udp_start_listener() != 0) {
        LOG_ERROR("Failed to start UDP listener");
        dsm_udp_shutdown();
        dsm_tcp_shutdown();
        dsm_node_table_destroy();
        dsm_context_destroy();
        return -1;
    }
    
    /* Step 5: Send discovery broadcast */
    if (dsm_udp_broadcast_discover(g_dsm_ctx.local_ip, port, chunks) != 0) {
        LOG_ERROR("Failed to broadcast discovery");
        dsm_udp_shutdown();
        dsm_tcp_shutdown();
        dsm_node_table_destroy();
        dsm_context_destroy();
        return -1;
    }
    
    /* Step 6: Wait for discovery (5 seconds) */
    LOG_INFO("Waiting for master discovery (%d seconds)...", DSM_DISCOVERY_TIMEOUT);
    dsm_role_t role = dsm_udp_wait_discovery();
    
    if (role == DSM_ROLE_MASTER) {
        LOG_INFO("No master found - becoming MASTER");
        g_dsm_ctx.local_role = DSM_ROLE_MASTER;
        g_dsm_ctx.local_node_id = 1;
        
        /* Add self to node table as master */
        dsm_node_table_add(g_dsm_ctx.local_ip, g_dsm_ctx.local_port, 
                          g_dsm_ctx.local_chunks, DSM_ROLE_MASTER, -1);
    } else {
        LOG_INFO("Master found - becoming WORKER");
        g_dsm_ctx.local_role = DSM_ROLE_WORKER;
        /* Worker node_id will be assigned by master during join */
        /* Wait for node_id assignment before continuing */
        LOG_INFO("Waiting for node_id assignment from master...");
        int wait_count = 0;
        while (g_dsm_ctx.local_node_id == 0 && wait_count < 50) {
            usleep(100000);  /* 100ms */
            wait_count++;
        }
        if (g_dsm_ctx.local_node_id == 0) {
            LOG_ERROR("Timeout waiting for node_id assignment from master");
            dsm_udp_shutdown();
            dsm_tcp_shutdown();
            dsm_node_table_destroy();
            dsm_context_destroy();
            return -1;
        }
        LOG_INFO("Received node_id=%d from master", g_dsm_ctx.local_node_id);
    }
    
    /* Step 7: Initialize memory subsystem */
    if (dsm_mem_init(chunks) != 0) {
        LOG_ERROR("Failed to initialize memory");
        dsm_udp_shutdown();
        dsm_tcp_shutdown();
        dsm_node_table_destroy();
        dsm_context_destroy();
        return -1;
    }
    
    /* Step 8: Initialize page fault handler */
    if (dsm_pagefault_init() != 0) {
        LOG_ERROR("Failed to initialize page fault handler");
        dsm_mem_shutdown();
        dsm_udp_shutdown();
        dsm_tcp_shutdown();
        dsm_node_table_destroy();
        dsm_context_destroy();
        return -1;
    }
    
    /* Start page fault handler thread */
    if (dsm_pagefault_start_handler() != 0) {
        LOG_ERROR("Failed to start page fault handler");
        dsm_pagefault_shutdown();
        dsm_mem_shutdown();
        dsm_udp_shutdown();
        dsm_tcp_shutdown();
        dsm_node_table_destroy();
        dsm_context_destroy();
        return -1;
    }
    
    /* Step 9: Initialize synchronization */
    if (dsm_sync_init() != 0) {
        LOG_ERROR("Failed to initialize synchronization");
        dsm_pagefault_shutdown();
        dsm_mem_shutdown();
        dsm_udp_shutdown();
        dsm_tcp_shutdown();
        dsm_node_table_destroy();
        dsm_context_destroy();
        return -1;
    }
    
    g_dsm_ctx.initialized = 1;
    LOG_INFO("DSM initialized successfully as %s (node_id=%d)", 
             g_dsm_ctx.local_role == DSM_ROLE_MASTER ? "MASTER" : "WORKER",
             g_dsm_ctx.local_node_id);
    
    return 0;
}

/*============================================================================
 * Public API: Finalization
 *===========================================================================*/

void dsm_finalize(void) {
    if (!g_dsm_ctx.initialized) {
        return;
    }
    
    LOG_INFO("Finalizing DSM...");
    
    g_dsm_ctx.running = 0;
    
    /* Stop threads in reverse order */
    dsm_pagefault_stop_handler();
    dsm_udp_stop_listener();
    dsm_tcp_stop_server();
    
    /* Shutdown subsystems */
    dsm_sync_shutdown();
    dsm_pagefault_shutdown();
    dsm_mem_shutdown();
    dsm_udp_shutdown();
    dsm_tcp_shutdown();
    dsm_node_table_destroy();
    dsm_context_destroy();
    
    g_dsm_ctx.initialized = 0;
    LOG_INFO("DSM finalized");
}

/*============================================================================
 * Public API: Status Functions
 *===========================================================================*/

int dsm_is_master(void) {
    return g_dsm_ctx.local_role == DSM_ROLE_MASTER;
}

uint32_t dsm_get_node_id(void) {
    return g_dsm_ctx.local_node_id;
}

uint32_t dsm_get_node_count(void) {
    return dsm_node_table_count();
}

size_t dsm_get_total_memory(void) {
    /* Sum up all chunks from all nodes */
    uint32_t total_chunks = 0;
    dsm_context_t *ctx = dsm_get_context();
    
    if (ctx->node_table) {
        pthread_rwlock_rdlock(&ctx->node_table->rwlock);
        for (uint32_t i = 0; i < ctx->node_table->count; i++) {
            total_chunks += ctx->node_table->nodes[i].chunks;
        }
        pthread_rwlock_unlock(&ctx->node_table->rwlock);
    }
    
    return total_chunks * DSM_PAGE_SIZE;
}

void dsm_set_log_level(int level) {
    if (level >= LOG_LEVEL_DEBUG && level <= LOG_LEVEL_ERROR) {
        dsm_current_log_level = (dsm_log_level_t)level;
    }
}
