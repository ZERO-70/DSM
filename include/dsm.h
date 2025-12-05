/**
 * @file dsm.h
 * @brief Public API for Distributed Shared Memory (DSM) System
 * 
 * This library provides a distributed shared memory abstraction that allows
 * multiple nodes on a LAN to share memory transparently. Memory accesses
 * are handled via page faults, and the system automatically fetches pages
 * from remote nodes when needed.
 * 
 * Usage:
 *   1. Call dsm_init() with port and chunks on all nodes
 *   2. Use dsm_malloc() to allocate shared memory
 *   3. Use dsm_lock()/dsm_unlock() for mutual exclusion
 *   4. Use dsm_barrier() for synchronization
 *   5. Call dsm_finalize() before exit
 */

#ifndef DSM_H
#define DSM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Initialization and Cleanup
 *===========================================================================*/

/**
 * Initialize the DSM system
 * 
 * This function initializes the DSM subsystem on this node. It performs:
 *   1. Gets the local WiFi IP address
 *   2. Broadcasts a UDP discovery message
 *   3. Listens for existing master for 5 seconds
 *   4. Starts TCP server
 *   5. Becomes MASTER if no master found, otherwise becomes WORKER
 *   6. Initializes local memory pool
 * 
 * @param argc Argument count from main()
 * @param argv Argument vector from main()
 *             Expected: program_name <port> <chunks>
 *             - port: TCP port to listen on
 *             - chunks: Number of 4KB chunks to donate to DSM pool
 * 
 * @return 0 on success, -1 on error
 */
int dsm_init(int argc, char **argv);

/**
 * Initialize DSM with explicit parameters
 * 
 * @param port TCP port to listen on
 * @param chunks Number of 4KB chunks to donate to DSM pool
 * @return 0 on success, -1 on error
 */
int dsm_init_with_params(uint16_t port, uint32_t chunks);

/**
 * Finalize and cleanup the DSM system
 * 
 * This function:
 *   1. Sends clean disconnect to all peers
 *   2. Updates metadata on master
 *   3. Unmaps all DSM memory
 *   4. Closes all sockets
 *   5. Joins all threads
 */
void dsm_finalize(void);

/*============================================================================
 * Memory Allocation
 *===========================================================================*/

/**
 * Allocate memory from the distributed shared memory pool
 * 
 * This allocates memory that is accessible from all nodes in the DSM cluster.
 * The memory is initially owned by the allocating node. Other nodes will
 * fault on access and fetch the page content.
 * 
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 * 
 * @note Allocation is always rounded up to page size (4KB)
 * @note This call may block while communicating with master
 */
void* dsm_malloc(size_t size);

/**
 * Allocate memory at a specific global address
 * 
 * This allows allocating memory at a predetermined global address, enabling
 * multiple nodes to share a known memory location. Only one node should 
 * allocate at each address; other nodes should use dsm_map_remote().
 * 
 * @param global_addr The global address to allocate at
 * @param size Number of bytes to allocate
 * @return Local pointer to the allocated memory, or NULL on failure
 * 
 * @note Only the owning node should call this
 * @note Other nodes should use dsm_map_remote() to access this memory
 */
void* dsm_malloc_at(uint64_t global_addr, size_t size);

/**
 * Map a remote global address for reading
 * 
 * This maps a global address owned by another node into this node's address
 * space. The first access will trigger a page fault and fetch the data from
 * the owning node.
 * 
 * @param global_addr The global address to map (must be allocated by another node)
 * @param size Number of bytes to map
 * @return Local pointer to the mapped memory, or NULL on failure
 * 
 * @note The owner node must have allocated this address first
 * @note First read will trigger a page fault to fetch from owner
 */
void* dsm_map_remote(uint64_t global_addr, size_t size);

/**
 * Free previously allocated DSM memory
 * 
 * @param addr Pointer returned by dsm_malloc()
 * 
 * @note This call may block while communicating with master
 */
void dsm_free(void *addr);

/*============================================================================
 * Synchronization Primitives
 *===========================================================================*/

/**
 * Acquire a distributed lock
 * 
 * Locks are managed by the master node. This call blocks until the lock
 * is acquired. Lock IDs should be agreed upon by all nodes.
 * 
 * @param lock_id Lock identifier (0 to DSM_MAX_LOCKS-1)
 * @return 0 on success, -1 on error (e.g., master not connected)
 * 
 * @note This call blocks until lock is acquired
 * @note Deadlock detection is not implemented - use carefully
 */
int dsm_lock(int lock_id);

/**
 * Release a distributed lock
 * 
 * @param lock_id Lock identifier (must match previous dsm_lock call)
 * 
 * @note Releasing a lock not held by this node is undefined behavior
 */
void dsm_unlock(int lock_id);

/**
 * Global synchronization barrier
 * 
 * All nodes must call this function. Execution blocks until all nodes
 * have reached the barrier.
 * 
 * @note All nodes must participate - missing nodes will cause deadlock
 */
void dsm_barrier(void);

/*============================================================================
 * Status and Information
 *===========================================================================*/

/**
 * Check if this node is the master
 * 
 * @return 1 if master, 0 if worker
 */
int dsm_is_master(void);

/**
 * Get this node's ID
 * 
 * @return Node ID
 */
uint32_t dsm_get_node_id(void);

/**
 * Get number of nodes in the cluster
 * 
 * @return Node count
 */
uint32_t dsm_get_node_count(void);

/**
 * Get total DSM memory pool size
 * 
 * @return Total size in bytes
 */
size_t dsm_get_total_memory(void);

/**
 * Set log level
 * 
 * @param level 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
 */
void dsm_set_log_level(int level);

/*============================================================================
 * Page Information API
 *===========================================================================*/

/**
 * Page information structure for detailed queries
 */
typedef struct {
    uint32_t    owner_id;       /* Node that owns this page */
    uint32_t    state;          /* 0=INVALID, 1=SHARED, 2=MODIFIED, 3=PENDING */
    uint64_t    global_addr;    /* Global DSM address */
    void       *local_addr;     /* Local virtual address */
    size_t      size;           /* Page size */
    uint64_t    version;        /* Page version for consistency */
    int         dirty;          /* Whether page has been modified locally */
} dsm_page_info_t;

/**
 * Local page entry for dsm_get_local_pages()
 */
typedef struct {
    uint64_t    global_addr;    /* Global DSM address */
    void       *local_addr;     /* Local virtual address (mapped) */
    size_t      size;           /* Page size */
    uint32_t    owner_id;       /* Node that owns this page */
    uint32_t    state;          /* 0=INVALID, 1=SHARED, 2=MODIFIED, 3=PENDING */
    int         in_use;         /* Whether allocated/in-use from pool */
} dsm_local_page_t;

/**
 * Get all locally mapped pages on this node
 * 
 * Returns information about all pages that are locally mapped in this node's
 * address space, including both local allocations and fetched remote pages.
 * 
 * @param pages     Pointer to array that will be allocated and filled
 * @param count     Pointer to store number of pages returned
 * @return 0 on success, -1 on error
 * 
 * @note Caller must free the returned pages array with free()
 * @note This is a local operation - no network communication
 */
int dsm_get_local_pages(dsm_local_page_t **pages, uint32_t *count);

/**
 * Get the node ID that owns the page containing the given address
 * 
 * @param addr Local pointer to any address within a DSM page
 * @return Owner node ID (1 = master, 2+ = workers), or -1 if address not in DSM
 * 
 * @note This queries the local page table
 */
int dsm_get_page_owner(void *addr);

/**
 * Get detailed information about the page containing the given address
 * 
 * @param addr Local pointer to any address within a DSM page
 * @param info Pointer to structure that will be filled with page information
 * @return 0 on success, -1 if address not in DSM or info is NULL
 * 
 * @note State values: 0=INVALID, 1=SHARED, 2=MODIFIED, 3=PENDING
 */
int dsm_get_page_info(void *addr, dsm_page_info_t *info);

/**
 * Convert a page state value to a human-readable string
 * 
 * @param state Page state value (from dsm_page_info_t.state)
 * @return String like "INVALID", "SHARED", "MODIFIED", "PENDING", or "UNKNOWN"
 */
const char* dsm_page_state_to_string(int state);

/*============================================================================
 * Global/Cluster Page Discovery API
 *===========================================================================*/

/**
 * Global page entry returned by dsm_get_global_pages()
 * 
 * Represents a page allocation known to the master's ownership table.
 * These are all pages in the entire DSM cluster.
 */
typedef struct {
    uint64_t    global_addr;    /* Global DSM address */
    size_t      size;           /* Size of allocation */
    uint32_t    owner_id;       /* Node that owns this page */
} dsm_global_page_t;

/* Legacy alias for backward compatibility */
typedef dsm_global_page_t dsm_remote_page_t;

/**
 * Get all pages in the DSM cluster (global view)
 * 
 * Queries the master's ownership table for ALL page allocations in the system.
 * This gives a cluster-wide view of memory - pages may or may not be locally
 * mapped on this node.
 * 
 * @param pages     Pointer to array that will be allocated and filled
 * @param count     Pointer to store number of pages returned
 * @return 0 on success, -1 on error
 * 
 * @note Caller must free the returned pages array with free()
 * @note Workers query master via network; master returns immediately
 * @note Use dsm_get_local_pages() to see only locally mapped pages
 */
int dsm_get_global_pages(dsm_global_page_t **pages, uint32_t *count);

/* Legacy alias for backward compatibility */
#define dsm_discover_pages dsm_get_global_pages

/*============================================================================
 * Debugging
 *===========================================================================*/

/**
 * Dump DSM state for debugging purposes
 * 
 * Prints comprehensive information about:
 * - Node information (ID, role, connections)
 * - Page table state
 * - Lock states
 * - Memory regions
 * 
 * @note Output goes to stdout with [DEBUG] prefix
 */
void dsm_debug_dump_state(void);

/**
 * Set debug log level
 * 
 * @param level 0=ERROR only, 1=INFO, 2=DEBUG (verbose)
 */
void dsm_set_log_level(int level);

#ifdef __cplusplus
}
#endif

#endif /* DSM_H */
