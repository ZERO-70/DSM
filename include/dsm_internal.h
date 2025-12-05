/**
 * @file dsm_internal.h
 * @brief Internal structures and global context for DSM
 */

#ifndef DSM_INTERNAL_H
#define DSM_INTERNAL_H

#include "dsm_types.h"
#include <sys/epoll.h>

/*============================================================================
 * Global Context Structure
 *===========================================================================*/

typedef struct {
    /* Node identity */
    uint32_t            local_node_id;
    dsm_role_t          local_role;
    char                local_ip[DSM_MAX_IP_LEN];
    uint16_t            local_port;
    uint32_t            local_chunks;
    
    /* Network - UDP */
    int                 udp_fd;
    pthread_t           udp_listener_thread;
    volatile int        udp_running;
    
    /* Network - TCP */
    int                 tcp_listen_fd;
    int                 epoll_fd;
    pthread_t           tcp_server_thread;
    volatile int        tcp_running;
    
    /* Master connection (for workers) */
    int                 master_fd;
    uint32_t            master_id;
    
    /* Node management */
    dsm_node_table_t   *node_table;
    
    /* Memory management */
    dsm_region_t       *local_region;       /* This node's donated region */
    dsm_region_t       *regions;            /* All regions (linked list) */
    dsm_page_table_t   *page_table;
    size_t              system_page_size;
    uint64_t            next_global_addr;   /* For master allocation */
    pthread_mutex_t     alloc_mutex;
    pthread_cond_t      alloc_cond;         /* For worker allocation response */
    volatile int        alloc_pending;      /* Waiting for allocation response */
    volatile uint64_t   alloc_response_addr;/* Address returned by master */
    
    /* Page fault handling */
    int                 fault_eventfd;      /* Eventfd for signal handler */
    pthread_t           fault_handler_thread;
    volatile int        fault_running;
    void               *pending_fault_addr; /* Address of pending fault */
    pthread_mutex_t     fault_mutex;
    pthread_cond_t      fault_cond;
    volatile int        fault_handled;      /* Flag: fault has been handled */
    volatile int        fault_result;       /* Result: 0=success, -1=failure */
    volatile int        pending_access_type; /* 0=read, 1=write for current fault */
    
    /* Invalidation tracking (for Invalidate/Exclusive protocol) */
    pthread_mutex_t     invalidate_mutex;
    pthread_cond_t      invalidate_cond;
    volatile uint32_t   invalidate_pending; /* Number of pending invalidate ACKs */
    volatile uint64_t   invalidate_addr;    /* Page being invalidated */
    
    /* Synchronization */
    dsm_lock_t          locks[DSM_MAX_LOCKS];
    dsm_barrier_t       barrier;
    uint32_t            barrier_count;      /* For tracking barrier participants */
    
    /* Sequence numbers */
    uint32_t            seq_num;
    pthread_mutex_t     seq_mutex;
    
    /* Shutdown flag */
    volatile int        running;
    
    /* Initialization complete flag */
    volatile int        initialized;
    
    /* Discovery state */
    volatile int        master_found;
    char                master_ip[DSM_MAX_IP_LEN];
    uint16_t            master_port;
    pthread_mutex_t     discovery_mutex;
    pthread_cond_t      discovery_cond;
    
} dsm_context_t;

/*============================================================================
 * Global Context Access
 *===========================================================================*/

/**
 * Get the global DSM context
 * @return Pointer to global context
 */
dsm_context_t* dsm_get_context(void);

/**
 * Initialize global context
 * @return 0 on success, -1 on error
 */
int dsm_context_init(void);

/**
 * Destroy global context
 */
void dsm_context_destroy(void);

/**
 * Get next sequence number
 * @return Next sequence number
 */
uint32_t dsm_get_next_seq(void);

/*============================================================================
 * Message Handling
 *===========================================================================*/

/**
 * Handle an incoming message
 * @param from_fd Socket the message came from
 * @param header Message header
 * @param payload Message payload
 * @return 0 on success, -1 on error
 */
int dsm_handle_message(int from_fd, dsm_msg_header_t *header, void *payload);

/**
 * Create a message header
 * @param type Message type
 * @param dst_node Destination node (0 for broadcast)
 * @param payload_len Payload length
 * @return Filled header
 */
dsm_msg_header_t dsm_create_header(dsm_msg_type_t type, uint32_t dst_node, 
                                   uint32_t payload_len);

/*============================================================================
 * Page Fault Signal Handler Support
 *===========================================================================*/

/**
 * Install SIGSEGV handler
 * @return 0 on success, -1 on error
 */
int dsm_pagefault_init(void);

/**
 * Remove SIGSEGV handler
 */
void dsm_pagefault_shutdown(void);

/**
 * Start page fault handler thread
 * @return 0 on success, -1 on error
 */
int dsm_pagefault_start_handler(void);

/**
 * Stop page fault handler thread
 */
void dsm_pagefault_stop_handler(void);

/**
 * Check if an address is in DSM managed memory
 * @param addr Address to check
 * @return 1 if in DSM, 0 otherwise
 */
int dsm_is_dsm_address(void *addr);

/*============================================================================
 * Synchronization Helpers
 *===========================================================================*/

/**
 * Initialize synchronization primitives
 * @return 0 on success, -1 on error
 */
int dsm_sync_init(void);

/**
 * Shutdown synchronization subsystem
 */
void dsm_sync_shutdown(void);

/**
 * Handle lock request (master only)
 * @param from_node Requesting node
 * @param lock_id Lock ID
 * @return 0 on success, -1 on error
 */
int dsm_sync_handle_lock_request(uint32_t from_node, uint32_t lock_id);

/**
 * Handle lock release (master only)
 * @param from_node Releasing node
 * @param lock_id Lock ID
 * @return 0 on success, -1 on error
 */
int dsm_sync_handle_lock_release(uint32_t from_node, uint32_t lock_id);

/**
 * Handle barrier entry (master only)
 * @param from_node Node entering barrier
 * @return 0 on success, -1 on error
 */
int dsm_sync_handle_barrier_enter(uint32_t from_node);

#endif /* DSM_INTERNAL_H */
