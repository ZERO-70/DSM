/**
 * @file dsm_network.h
 * @brief Network subsystem interface for DSM
 */

#ifndef DSM_NETWORK_H
#define DSM_NETWORK_H

#include "dsm_types.h"

/*============================================================================
 * Network Initialization and Cleanup
 *===========================================================================*/

/**
 * Initialize the network subsystem
 * @return 0 on success, -1 on error
 */
int dsm_net_init(void);

/**
 * Shutdown the network subsystem
 */
void dsm_net_shutdown(void);

/**
 * Get the local WiFi/LAN IP address
 * @param ip_buf Buffer to store IP address string
 * @param buf_len Length of buffer
 * @return 0 on success, -1 on error
 */
int dsm_net_get_local_ip(char *ip_buf, size_t buf_len);

/*============================================================================
 * UDP Discovery Functions
 *===========================================================================*/

/**
 * Initialize UDP discovery socket
 * @return 0 on success, -1 on error
 */
int dsm_udp_init(void);

/**
 * Shutdown UDP subsystem
 */
void dsm_udp_shutdown(void);

/**
 * Send discovery broadcast message
 * @param ip Local IP address
 * @param port TCP port
 * @param chunks Number of chunks this node donates
 * @return 0 on success, -1 on error
 */
int dsm_udp_broadcast_discover(const char *ip, uint16_t port, uint32_t chunks);

/**
 * Start UDP listener thread
 * @return 0 on success, -1 on error
 */
int dsm_udp_start_listener(void);

/**
 * Stop UDP listener thread
 */
void dsm_udp_stop_listener(void);

/**
 * Wait for discovery to complete (5 second timeout)
 * @return DSM_ROLE_MASTER if no master found, DSM_ROLE_WORKER if master responded
 */
dsm_role_t dsm_udp_wait_discovery(void);

/*============================================================================
 * TCP Connection Functions
 *===========================================================================*/

/**
 * Initialize TCP subsystem
 * @param port Port to listen on
 * @return 0 on success, -1 on error
 */
int dsm_tcp_init(uint16_t port);

/**
 * Shutdown TCP subsystem
 */
void dsm_tcp_shutdown(void);

/**
 * Start TCP server thread
 * @return 0 on success, -1 on error
 */
int dsm_tcp_start_server(void);

/**
 * Stop TCP server thread
 */
void dsm_tcp_stop_server(void);

/**
 * Connect to a remote node
 * @param ip Remote IP address
 * @param port Remote port
 * @return socket fd on success, -1 on error
 */
int dsm_tcp_connect(const char *ip, uint16_t port);

/**
 * Disconnect from a node
 * @param node_id Node to disconnect from
 */
void dsm_tcp_disconnect(uint32_t node_id);

/**
 * Send a message to a specific node
 * @param node_id Target node ID
 * @param header Message header
 * @param payload Payload data (can be NULL)
 * @return 0 on success, -1 on error
 */
int dsm_tcp_send(uint32_t node_id, dsm_msg_header_t *header, void *payload);

/**
 * Send a message to all connected nodes
 * @param header Message header
 * @param payload Payload data (can be NULL)
 * @return 0 on success, -1 on error
 */
int dsm_tcp_broadcast(dsm_msg_header_t *header, void *payload);

/**
 * Send raw data on a socket
 * @param sockfd Socket file descriptor
 * @param data Data to send
 * @param len Length of data
 * @return 0 on success, -1 on error
 */
int dsm_tcp_send_raw(int sockfd, void *data, size_t len);

/**
 * Receive raw data from a socket
 * @param sockfd Socket file descriptor
 * @param buf Buffer to receive into
 * @param len Exact number of bytes to receive
 * @return 0 on success, -1 on error
 */
int dsm_tcp_recv_raw(int sockfd, void *buf, size_t len);

/*============================================================================
 * Node Table Management
 *===========================================================================*/

/**
 * Initialize node table
 * @return 0 on success, -1 on error
 */
int dsm_node_table_init(void);

/**
 * Destroy node table
 */
void dsm_node_table_destroy(void);

/**
 * Add a node to the table
 * @param ip Node IP address
 * @param port Node port
 * @param chunks Node's chunk count
 * @param role Node role
 * @param sockfd TCP socket (or -1 if not connected)
 * @return assigned node_id on success, -1 on error
 */
int dsm_node_table_add(const char *ip, uint16_t port, uint32_t chunks, 
                       dsm_role_t role, int sockfd);

/**
 * Remove a node from the table
 * @param node_id Node to remove
 * @return 0 on success, -1 on error
 */
int dsm_node_table_remove(uint32_t node_id);

/**
 * Get a node by ID
 * @param node_id Node ID to look up
 * @return Pointer to node (caller must not free), NULL if not found
 */
dsm_node_t* dsm_node_table_get(uint32_t node_id);

/**
 * Get a node by IP and port
 * @param ip IP address
 * @param port Port
 * @return Pointer to node, NULL if not found
 */
dsm_node_t* dsm_node_table_find(const char *ip, uint16_t port);

/**
 * Get current node count
 * @return Number of nodes in table
 */
uint32_t dsm_node_table_count(void);

/**
 * Serialize node table for network transmission
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @return Number of bytes written, -1 on error
 */
ssize_t dsm_node_table_serialize(void *buf, size_t buf_len);

/**
 * Deserialize and update node table from network data
 * @param buf Input buffer
 * @param len Data length
 * @return 0 on success, -1 on error
 */
int dsm_node_table_deserialize(void *buf, size_t len);

/**
 * Send node table to all connected workers (master only)
 * @return 0 on success, -1 on error
 */
int dsm_node_table_broadcast(void);

/**
 * Update node's socket fd
 * @param node_id Node ID
 * @param sockfd New socket fd
 * @return 0 on success, -1 on error
 */
int dsm_node_table_set_socket(uint32_t node_id, int sockfd);

/**
 * Update node heartbeat timestamp
 * @param node_id Node ID
 */
void dsm_node_table_heartbeat(uint32_t node_id);

#endif /* DSM_NETWORK_H */
