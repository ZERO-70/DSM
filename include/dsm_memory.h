/**
 * @file dsm_memory.h
 * @brief Memory management interface for DSM
 */

#ifndef DSM_MEMORY_H
#define DSM_MEMORY_H

#include "dsm_types.h"
#include <stddef.h>

/*============================================================================
 * Memory Initialization and Cleanup
 *===========================================================================*/

/**
 * Initialize memory subsystem
 * @param chunks Number of chunks this node donates
 * @return 0 on success, -1 on error
 */
int dsm_mem_init(uint32_t chunks);

/**
 * Shutdown memory subsystem
 */
void dsm_mem_shutdown(void);

/*============================================================================
 * Memory Allocation
 *===========================================================================*/

/**
 * Allocate memory from DSM pool
 * For workers, this requests allocation from master
 * For master, this allocates locally
 * @param size Size in bytes to allocate
 * @return Pointer to allocated memory, NULL on failure
 */
void* dsm_mem_alloc(size_t size);

/**
 * Free previously allocated DSM memory
 * @param ptr Pointer returned by dsm_mem_alloc
 */
void dsm_mem_free(void *ptr);

/*============================================================================
 * Page Management
 *===========================================================================*/

/**
 * Initialize page table
 * @return 0 on success, -1 on error
 */
int dsm_page_table_init(void);

/**
 * Destroy page table
 */
void dsm_page_table_destroy(void);

/**
 * Register a new page
 * @param global_addr Global DSM address
 * @param local_addr Local virtual address
 * @param owner_id Owner node ID
 * @return 0 on success, -1 on error
 */
int dsm_page_register(uint64_t global_addr, void *local_addr, uint32_t owner_id);

/**
 * Look up a page by global address
 * @param global_addr Global DSM address
 * @return Pointer to page metadata, NULL if not found
 */
dsm_page_t* dsm_page_lookup(uint64_t global_addr);

/**
 * Look up a page by local address
 * @param local_addr Local virtual address
 * @return Pointer to page metadata, NULL if not found
 */
dsm_page_t* dsm_page_lookup_local(void *local_addr);

/**
 * Handle a page fault at the given address
 * Called from signal handler via eventfd
 * @param fault_addr Address that caused the fault
 * @return 0 on success, -1 on error
 */
int dsm_page_fault_handle(void *fault_addr);

/**
 * Request a page from its owner
 * @param page Page to request
 * @param access_type 0=read, 1=write
 * @return 0 on success, -1 on error
 */
int dsm_page_request(dsm_page_t *page, int access_type);

/**
 * Send a page to a requesting node
 * @param node_id Requesting node
 * @param global_addr Page address
 * @return 0 on success, -1 on error
 */
int dsm_page_send(uint32_t node_id, uint64_t global_addr);

/**
 * Install received page data
 * @param global_addr Page global address
 * @param data Page data
 * @param size Data size
 * @param version Page version
 * @return 0 on success, -1 on error
 */
int dsm_page_install(uint64_t global_addr, void *data, size_t size, uint64_t version);

/**
 * Invalidate a local page copy
 * @param global_addr Page to invalidate
 * @return 0 on success, -1 on error
 */
int dsm_page_invalidate(uint64_t global_addr);

/**
 * Mark a page as modified
 * @param global_addr Page address
 * @return 0 on success, -1 on error
 */
int dsm_page_mark_modified(uint64_t global_addr);

/*============================================================================
 * Region Management
 *===========================================================================*/

/**
 * Create a new memory region
 * @param size Size of region
 * @param owner_id Owner node ID
 * @param global_base Starting global address
 * @return Pointer to region, NULL on error
 */
dsm_region_t* dsm_region_create(size_t size, uint32_t owner_id, uint64_t global_base);

/**
 * Destroy a memory region
 * @param region Region to destroy
 */
void dsm_region_destroy(dsm_region_t *region);

/**
 * Find region containing a global address
 * @param global_addr Address to look up
 * @return Pointer to region, NULL if not found
 */
dsm_region_t* dsm_region_find(uint64_t global_addr);

/**
 * Get the local address for a global DSM address
 * @param global_addr Global DSM address
 * @return Local virtual address, NULL if not mapped
 */
void* dsm_global_to_local(uint64_t global_addr);

/**
 * Get the global DSM address for a local address
 * @param local_addr Local virtual address
 * @return Global DSM address, 0 if not in DSM
 */
uint64_t dsm_local_to_global(void *local_addr);

/*============================================================================
 * Ownership Management (Master Only)
 *===========================================================================*/

/**
 * Get owner of a page (master queries its metadata)
 * @param global_addr Page address
 * @return Owner node ID, -1 if not found
 */
int dsm_ownership_get(uint64_t global_addr);

/**
 * Set owner of a page
 * @param global_addr Page address
 * @param owner_id New owner
 * @return 0 on success, -1 on error
 */
int dsm_ownership_set(uint64_t global_addr, uint32_t owner_id);

/**
 * Register a new allocation in ownership table
 * @param global_addr Starting address
 * @param size Size of allocation
 * @param owner_id Owner node
 * @return 0 on success, -1 on error
 */
int dsm_ownership_register(uint64_t global_addr, size_t size, uint32_t owner_id);

/*============================================================================
 * Invalidate/Exclusive Ownership Protocol
 *===========================================================================*/

/**
 * Request write access to a page (ownership transfer)
 * This is called when a non-owner node wants to write
 * @param page Page to request write access to
 * @return 0 on success, -1 on error
 */
int dsm_page_request_write(dsm_page_t *page);

/**
 * Handle incoming page write request (master receives)
 * Coordinates invalidation and ownership transfer
 * @param from_node Node requesting write access
 * @param global_addr Page address
 * @return 0 on success, -1 on error
 */
int dsm_handle_page_write_request(uint32_t from_node, uint64_t global_addr);

/**
 * Handle incoming page invalidate request
 * Invalidates local copy and sends ACK
 * @param global_addr Page to invalidate
 * @param new_owner Node that will become the new owner
 * @return 0 on success, -1 on error
 */
int dsm_handle_page_invalidate(uint64_t global_addr, uint32_t new_owner);

/**
 * Handle invalidate acknowledgment (master receives)
 * @param from_node Node acknowledging
 * @param global_addr Page address
 * @return 0 on success, -1 on error
 */
int dsm_handle_invalidate_ack(uint32_t from_node, uint64_t global_addr);

/**
 * Handle ownership transfer (new owner receives)
 * Receives page data and becomes exclusive owner
 * @param global_addr Page address
 * @param data Page data
 * @param size Data size
 * @param version Page version
 * @param old_owner Previous owner
 * @return 0 on success, -1 on error
 */
int dsm_handle_ownership_xfer(uint64_t global_addr, void *data, size_t size, 
                               uint64_t version, uint32_t old_owner);

/**
 * Add a node to a page's copyset (master tracks who has copies)
 * @param global_addr Page address
 * @param node_id Node that now has a copy
 */
void dsm_page_copyset_add(uint64_t global_addr, uint32_t node_id);

/**
 * Remove a node from a page's copyset
 * @param global_addr Page address
 * @param node_id Node to remove
 */
void dsm_page_copyset_remove(uint64_t global_addr, uint32_t node_id);

/**
 * Clear a page's copyset (after invalidation)
 * @param global_addr Page address
 */
void dsm_page_copyset_clear(uint64_t global_addr);

/**
 * Send page with ownership transfer
 * @param node_id Destination node
 * @param global_addr Page address
 * @param transfer_ownership If true, also transfer ownership
 * @return 0 on success, -1 on error
 */
int dsm_page_send_with_ownership(uint32_t node_id, uint64_t global_addr, int transfer_ownership);

/*============================================================================
 * Page Discovery (Internal Functions)
 *===========================================================================*/

/**
 * Collect all ownership entries into a page list message (master only)
 * @param out_count Output: number of entries
 * @return Allocated message (caller must free), NULL on error
 */
dsm_msg_list_pages_t* dsm_ownership_collect_all(uint32_t *out_count);

/**
 * Store a list pages response (called from message handler)
 * @param resp Response message
 * @param size Size of response message
 */
void dsm_store_list_pages_response(dsm_msg_list_pages_t *resp, size_t size);

#endif /* DSM_MEMORY_H */
