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
 * 
 * @note This call blocks until lock is acquired
 * @note Deadlock detection is not implemented - use carefully
 */
void dsm_lock(int lock_id);

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

#ifdef __cplusplus
}
#endif

#endif /* DSM_H */
