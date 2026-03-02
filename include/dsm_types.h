/**
 * @file dsm_types.h
 * @brief Common type definitions, constants, and message formats for DSM system
 */

#ifndef DSM_TYPES_H
#define DSM_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <netinet/in.h>

/*============================================================================
 * Constants
 *===========================================================================*/

#define DSM_MAX_NODES           64
#define DSM_MAX_LOCKS           256
#define DSM_PAGE_SIZE           4096
#define DSM_MAX_CHUNKS          1024
#define DSM_DISCOVERY_PORT      9999
#define DSM_DISCOVERY_TIMEOUT   5       /* seconds */
#define DSM_MAX_MESSAGE_SIZE    65536
#define DSM_HEARTBEAT_INTERVAL  2       /* seconds */
#define DSM_NODE_TIMEOUT        10      /* seconds */
#define DSM_MAX_IP_LEN          16
#define DSM_MAGIC               0x44534D00  /* "DSM\0" */

/*============================================================================
 * Logging Macros
 *===========================================================================*/

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3
} dsm_log_level_t;

extern dsm_log_level_t dsm_current_log_level;

#define LOG_DEBUG(fmt, ...) \
    do { if (dsm_current_log_level <= LOG_LEVEL_DEBUG) \
        fprintf(stderr, "[DEBUG] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
    } while(0)

#define LOG_INFO(fmt, ...) \
    do { if (dsm_current_log_level <= LOG_LEVEL_INFO) \
        fprintf(stderr, "[INFO]  %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
    } while(0)

#define LOG_WARN(fmt, ...) \
    do { if (dsm_current_log_level <= LOG_LEVEL_WARN) \
        fprintf(stderr, "[WARN]  %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
    } while(0)

#define LOG_ERROR(fmt, ...) \
    do { if (dsm_current_log_level <= LOG_LEVEL_ERROR) \
        fprintf(stderr, "[ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
    } while(0)

/*============================================================================
 * Enumerations
 *===========================================================================*/

/** Role of the node in the DSM network */
typedef enum {
    DSM_ROLE_UNKNOWN = 0,
    DSM_ROLE_MASTER,
    DSM_ROLE_WORKER
} dsm_role_t;

/** State of a node connection */
typedef enum {
    DSM_NODE_STATE_DISCOVERING = 0,
    DSM_NODE_STATE_CONNECTING,
    DSM_NODE_STATE_CONNECTED,
    DSM_NODE_STATE_READY,
    DSM_NODE_STATE_DISCONNECTED
} dsm_node_state_t;

/** State of a memory page */
typedef enum {
    DSM_PAGE_INVALID = 0,   /* No local copy, must fetch remotely */
    DSM_PAGE_SHARED,        /* Read-only local copy */
    DSM_PAGE_MODIFIED,      /* Exclusive write access */
    DSM_PAGE_PENDING        /* Waiting for data from remote */
} dsm_page_state_t;

/** Protocol message types */
typedef enum {
    /* Discovery and connection */
    DSM_MSG_DISCOVER        = 0x01,     /* UDP broadcast for discovery */
    DSM_MSG_DISCOVER_REPLY  = 0x02,     /* Master's response to discovery */
    DSM_MSG_JOIN_REQUEST    = 0x03,     /* Worker requests to join */
    DSM_MSG_JOIN_RESPONSE   = 0x04,     /* Master acknowledges join */
    DSM_MSG_NODE_TABLE      = 0x05,     /* Full node table sync */
    DSM_MSG_NEW_NODE        = 0x06,     /* Announce new node */
    DSM_MSG_NODE_LEFT       = 0x07,     /* Announce node departure */
    
    /* Page management */
    DSM_MSG_PAGE_REQUEST    = 0x10,     /* Request page data */
    DSM_MSG_PAGE_DATA       = 0x11,     /* Page content transfer */
    DSM_MSG_PAGE_INVALIDATE = 0x12,     /* Invalidate remote copies */
    DSM_MSG_PAGE_ACK        = 0x13,     /* Page operation acknowledgment */
    
    /* Memory allocation */
    DSM_MSG_ALLOC_REQUEST   = 0x20,     /* Request memory allocation */
    DSM_MSG_ALLOC_RESPONSE  = 0x21,     /* Allocation result */
    DSM_MSG_FREE_REQUEST    = 0x22,     /* Request memory free */
    DSM_MSG_FREE_RESPONSE   = 0x23,     /* Free result */
    
    /* Synchronization */
    DSM_MSG_LOCK_REQUEST    = 0x30,     /* Request distributed lock */
    DSM_MSG_LOCK_GRANT      = 0x31,     /* Lock granted */
    DSM_MSG_LOCK_RELEASE    = 0x32,     /* Release lock */
    DSM_MSG_LOCK_ACK        = 0x33,     /* Lock operation ack */
    DSM_MSG_BARRIER_ENTER   = 0x34,     /* Enter barrier */
    DSM_MSG_BARRIER_RELEASE = 0x35,     /* Release from barrier */
    
    /* Maintenance */
    DSM_MSG_HEARTBEAT       = 0x40,     /* Keep-alive */
    DSM_MSG_HEARTBEAT_ACK   = 0x41,     /* Heartbeat response */
    DSM_MSG_SHUTDOWN        = 0x42,     /* Clean shutdown */
    DSM_MSG_METADATA        = 0x43      /* Metadata sync */
} dsm_msg_type_t;

/*============================================================================
 * Structures
 *===========================================================================*/

/** Node information */
typedef struct dsm_node {
    uint32_t            node_id;
    dsm_role_t          role;
    dsm_node_state_t    state;
    char                ip[DSM_MAX_IP_LEN];
    uint16_t            port;
    uint32_t            chunks;
    int                 tcp_fd;         /* TCP socket to this node */
    time_t              last_heartbeat;
    pthread_mutex_t     lock;           /* Per-node lock */
} dsm_node_t;

/** Node table */
typedef struct {
    dsm_node_t          nodes[DSM_MAX_NODES];
    uint32_t            count;
    pthread_rwlock_t    rwlock;
} dsm_node_table_t;

/** Memory page metadata */
typedef struct dsm_page {
    void               *local_addr;     /* Local virtual address */
    uint64_t            global_addr;    /* Global DSM address */
    size_t              size;
    dsm_page_state_t    state;
    uint32_t            owner_id;       /* Node that owns this page */
    uint64_t            version;        /* For consistency */
    bool                dirty;
    pthread_mutex_t     lock;
} dsm_page_t;

/** Page table for tracking all pages */
typedef struct {
    dsm_page_t        **pages;          /* Array of page pointers */
    size_t              capacity;
    size_t              count;
    pthread_rwlock_t    rwlock;
} dsm_page_table_t;

/** Memory region managed by DSM */
typedef struct dsm_region {
    void               *base_addr;      /* Base of mmap'd region */
    size_t              total_size;
    size_t              page_count;
    uint32_t            owner_id;       /* Node that donated this region */
    uint64_t            global_base;    /* Global address start */
    struct dsm_region  *next;
} dsm_region_t;

/** Distributed lock */
typedef struct {
    uint32_t            lock_id;
    uint32_t            holder_id;      /* Node holding the lock, 0 if free */
    bool                locked;
    uint32_t            waiters[DSM_MAX_NODES];
    uint32_t            waiter_count;
    pthread_mutex_t     mutex;
    pthread_cond_t      cond;
} dsm_lock_t;

/** Barrier state */
typedef struct {
    uint32_t            barrier_id;
    uint32_t            total_nodes;
    uint32_t            arrived_count;
    uint32_t            arrived[DSM_MAX_NODES];
    pthread_mutex_t     mutex;
    pthread_cond_t      cond;
} dsm_barrier_t;

/*============================================================================
 * Protocol Message Structures
 *===========================================================================*/

/** Message header - sent before every message */
typedef struct __attribute__((packed)) {
    uint32_t            magic;          /* DSM_MAGIC */
    uint32_t            type;           /* dsm_msg_type_t */
    uint32_t            src_node;
    uint32_t            dst_node;
    uint32_t            seq_num;
    uint32_t            payload_len;
} dsm_msg_header_t;

/** Discovery message payload */
typedef struct __attribute__((packed)) {
    char                ip[DSM_MAX_IP_LEN];
    uint16_t            port;
    uint32_t            chunks;
} dsm_msg_discover_t;

/** Node info in network messages */
typedef struct __attribute__((packed)) {
    uint32_t            node_id;
    char                ip[DSM_MAX_IP_LEN];
    uint16_t            port;
    uint32_t            chunks;
    uint32_t            role;
    uint32_t            state;
} dsm_msg_node_info_t;

/** Node table sync message */
typedef struct __attribute__((packed)) {
    uint32_t            count;
    dsm_msg_node_info_t nodes[];        /* Flexible array */
} dsm_msg_node_table_t;

/** Page request message */
typedef struct __attribute__((packed)) {
    uint64_t            global_addr;
    uint32_t            access_type;    /* 0=read, 1=write */
} dsm_msg_page_request_t;

/** Page data message */
typedef struct __attribute__((packed)) {
    uint64_t            global_addr;
    uint32_t            size;
    uint64_t            version;
    uint8_t             data[];         /* Flexible array - page content */
} dsm_msg_page_data_t;

/** Allocation request */
typedef struct __attribute__((packed)) {
    size_t              size;
    uint32_t            request_id;
} dsm_msg_alloc_request_t;

/** Allocation response */
typedef struct __attribute__((packed)) {
    uint64_t            global_addr;    /* 0 if failed */
    size_t              size;
    uint32_t            owner_id;
    uint32_t            request_id;
} dsm_msg_alloc_response_t;

/** Lock request */
typedef struct __attribute__((packed)) {
    uint32_t            lock_id;
} dsm_msg_lock_t;

/** Barrier message */
typedef struct __attribute__((packed)) {
    uint32_t            barrier_id;
} dsm_msg_barrier_t;

#endif /* DSM_TYPES_H */
