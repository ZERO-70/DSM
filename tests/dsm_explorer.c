/**
 * DSM Explorer - Interactive DSM Memory Explorer
 * 
 * Usage: ./dsm_explorer <port> <chunks>
 * 
 * A simple interactive tool for exploring DSM memory.
 * Demonstrates the use of:
 *   - dsm_get_local_pages()  - View pages mapped locally
 *   - dsm_get_global_pages() - View all pages in cluster (from master)
 *   - dsm_malloc()           - Allocate shared memory
 *   - dsm_map_remote()       - Map remote page for access
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dsm.h"

#define PAGE_SIZE 4096

/* Show LOCAL pages (pages that are mapped in this node's address space) */
static void show_local_pages(void) {
    uint32_t my_node_id = dsm_get_node_id();
    uint32_t node_count = dsm_get_node_count();
    
    dsm_local_page_t *pages = NULL;
    uint32_t count = 0;
    
    if (dsm_get_local_pages(&pages, &count) != 0) {
        printf("ERROR: Failed to get local pages!\n");
        return;
    }
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                     LOCAL PAGES (This Node)                                ║\n");
    printf("╠════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  Node: %u (%s)  |  Total Nodes: %u                                         \n",
           my_node_id,
           dsm_is_master() ? "MASTER" : "WORKER",
           node_count);
    printf("╠════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  Idx │  Global Address  │   Local Address  │ Owner │ State              \n");
    printf("╠══════╪══════════════════╪══════════════════╪═══════╪════════════════════╣\n");
    
    if (count == 0) {
        printf("║  No pages locally mapped yet. Allocate or fetch pages first.              ║\n");
    } else {
        for (uint32_t i = 0; i < count; i++) {
            const char *state_str = dsm_page_state_to_string(pages[i].state);
            const char *ownership = (pages[i].owner_id == my_node_id) ? " (mine)" : "";
            
            printf("║  %3u │ 0x%014lx │ %16p │   %2u  │ %s%s\n",
                   i,
                   (unsigned long)pages[i].global_addr,
                   pages[i].local_addr,
                   pages[i].owner_id,
                   state_str,
                   ownership);
        }
    }
    
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    if (pages) free(pages);
}

/* Show GLOBAL pages (all pages in cluster from master's ownership table) */
static void show_global_pages(void) {
    uint32_t my_node_id = dsm_get_node_id();
    
    printf("\n=== GLOBAL PAGES (All Cluster Memory) ===\n");
    printf("Querying master for all known pages...\n\n");
    
    dsm_global_page_t *pages = NULL;
    uint32_t count = 0;
    
    if (dsm_get_global_pages(&pages, &count) != 0) {
        printf("ERROR: Failed to get global pages!\n");
        return;
    }
    
    if (count == 0) {
        printf("No pages found in the cluster.\n");
        printf("Use option 4 to allocate a new page.\n\n");
        return;
    }
    
    printf("╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                   GLOBAL PAGES (Cluster-Wide View)                         ║\n");
    printf("╠════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  %-4s │ %-18s │ %-10s │ %-6s │ Location                   \n", "Idx", "Global Address", "Size", "Owner");
    printf("╠══════╪════════════════════╪════════════╪════════╪════════════════════════════╣\n");
    
    for (uint32_t i = 0; i < count; i++) {
        const char *location;
        if (pages[i].owner_id == my_node_id) {
            location = "LOCAL (this node)";
        } else {
            location = "REMOTE";
        }
        
        printf("║  %-4u │ 0x%016lx │ %10zu │   %2u   │ %s\n",
               i,
               (unsigned long)pages[i].global_addr,
               pages[i].size,
               pages[i].owner_id,
               location);
    }
    
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\nTotal: %u pages across all nodes.\n\n", count);
    
    free(pages);
}

/* Read a page by global address or index */
static void read_page(void) {
    printf("\n=== READ PAGE ===\n");
    
    /* Show local pages */
    dsm_local_page_t *local_pages = NULL;
    uint32_t local_count = 0;
    dsm_get_local_pages(&local_pages, &local_count);
    
    /* Show global pages */
    dsm_global_page_t *global_pages = NULL;
    uint32_t global_count = 0;
    dsm_get_global_pages(&global_pages, &global_count);
    
    printf("\n--- LOCAL pages (already mapped) ---\n");
    if (local_count > 0) {
        for (uint32_t i = 0; i < local_count; i++) {
            printf("  L[%u] 0x%lx (%s, owner=%u)\n", i, 
                   (unsigned long)local_pages[i].global_addr,
                   dsm_page_state_to_string(local_pages[i].state),
                   local_pages[i].owner_id);
        }
    } else {
        printf("  (none)\n");
    }
    
    printf("\n--- GLOBAL pages (all cluster) ---\n");
    if (global_count > 0) {
        for (uint32_t i = 0; i < global_count; i++) {
            const char *loc = (global_pages[i].owner_id == dsm_get_node_id()) ? "LOCAL" : "REMOTE";
            printf("  G[%u] 0x%lx (owner=%u, %s)\n", i, 
                   (unsigned long)global_pages[i].global_addr,
                   global_pages[i].owner_id,
                   loc);
        }
    } else {
        printf("  (none)\n");
    }
    
    printf("\nEnter: L<idx> for local, G<idx> for global, or hex address: ");
    fflush(stdout);
    
    char input[64];
    if (fgets(input, sizeof(input), stdin) == NULL) {
        if (local_pages) free(local_pages);
        if (global_pages) free(global_pages);
        return;
    }
    
    uint64_t global_addr = 0;
    void *local_addr = NULL;
    
    /* Parse input */
    if (input[0] == 'L' || input[0] == 'l') {
        /* Local index */
        int idx = atoi(input + 1);
        if (idx >= 0 && (uint32_t)idx < local_count) {
            global_addr = local_pages[idx].global_addr;
            local_addr = local_pages[idx].local_addr;
        }
    } else if (input[0] == 'G' || input[0] == 'g') {
        /* Global index */
        int idx = atoi(input + 1);
        if (idx >= 0 && (uint32_t)idx < global_count) {
            global_addr = global_pages[idx].global_addr;
            /* Check if we have it locally */
            for (uint32_t i = 0; i < local_count; i++) {
                if (local_pages[i].global_addr == global_addr) {
                    local_addr = local_pages[i].local_addr;
                    break;
                }
            }
        }
    } else if (input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
        /* Hex address */
        sscanf(input, "%lx", (unsigned long *)&global_addr);
    } else {
        /* Try as plain number - assume local index */
        int idx = atoi(input);
        if (idx >= 0 && (uint32_t)idx < local_count) {
            global_addr = local_pages[idx].global_addr;
            local_addr = local_pages[idx].local_addr;
        } else {
            /* Try as hex without 0x prefix */
            sscanf(input, "%lx", (unsigned long *)&global_addr);
        }
    }
    
    if (local_pages) free(local_pages);
    if (global_pages) free(global_pages);
    
    if (global_addr == 0) {
        printf("Invalid address/index.\n");
        return;
    }
    
    /* If we don't have a local mapping, try to map it */
    if (local_addr == NULL) {
        printf("Mapping page at global address 0x%lx...\n", (unsigned long)global_addr);
        local_addr = dsm_map_remote(global_addr, PAGE_SIZE);
        if (local_addr == NULL) {
            printf("ERROR: Failed to map page.\n");
            return;
        }
        printf("Mapped to local address: %p\n", local_addr);
    }
    
    /* Read will trigger page fault if needed */
    printf("\n--- Reading page content (first 256 bytes as text) ---\n");
    
    volatile char *ptr = (volatile char *)local_addr;
    char buffer[257];
    
    /* Read byte by byte to trigger fault on first access */
    for (int i = 0; i < 256; i++) {
        buffer[i] = ptr[i];
    }
    buffer[256] = '\0';
    
    /* Print readable content */
    printf("Content: \"");
    for (int i = 0; i < 256 && buffer[i] != '\0'; i++) {
        if (buffer[i] >= 32 && buffer[i] < 127) {
            putchar(buffer[i]);
        } else if (buffer[i] == '\n') {
            printf("\\n");
        } else {
            printf("\\x%02x", (unsigned char)buffer[i]);
        }
    }
    printf("\"\n");
    
    /* Get updated page info */
    dsm_page_info_t info;
    if (dsm_get_page_info(local_addr, &info) == 0) {
        printf("\nPage State After Read:\n");
        printf("  Owner: %u\n", info.owner_id);
        printf("  State: %s\n", dsm_page_state_to_string(info.state));
    }
    
    printf("\n");
}

/* Write to a page by global address or index */
static void write_page(void) {
    printf("\n=== WRITE PAGE ===\n");
    
    /* Show local pages */
    dsm_local_page_t *local_pages = NULL;
    uint32_t local_count = 0;
    dsm_get_local_pages(&local_pages, &local_count);
    
    /* Show global pages */
    dsm_global_page_t *global_pages = NULL;
    uint32_t global_count = 0;
    dsm_get_global_pages(&global_pages, &global_count);
    
    printf("\n--- LOCAL pages (already mapped) ---\n");
    if (local_count > 0) {
        for (uint32_t i = 0; i < local_count; i++) {
            printf("  L[%u] 0x%lx (%s, owner=%u)\n", i, 
                   (unsigned long)local_pages[i].global_addr,
                   dsm_page_state_to_string(local_pages[i].state),
                   local_pages[i].owner_id);
        }
    } else {
        printf("  (none)\n");
    }
    
    printf("\n--- GLOBAL pages (all cluster) ---\n");
    if (global_count > 0) {
        for (uint32_t i = 0; i < global_count; i++) {
            const char *loc = (global_pages[i].owner_id == dsm_get_node_id()) ? "LOCAL" : "REMOTE";
            printf("  G[%u] 0x%lx (owner=%u, %s)\n", i, 
                   (unsigned long)global_pages[i].global_addr,
                   global_pages[i].owner_id,
                   loc);
        }
    } else {
        printf("  (none)\n");
    }
    
    printf("\nEnter: L<idx> for local, G<idx> for global, or hex address: ");
    fflush(stdout);
    
    char input[64];
    if (fgets(input, sizeof(input), stdin) == NULL) {
        if (local_pages) free(local_pages);
        if (global_pages) free(global_pages);
        return;
    }
    
    uint64_t global_addr = 0;
    void *local_addr = NULL;
    
    /* Parse input */
    if (input[0] == 'L' || input[0] == 'l') {
        /* Local index */
        int idx = atoi(input + 1);
        if (idx >= 0 && (uint32_t)idx < local_count) {
            global_addr = local_pages[idx].global_addr;
            local_addr = local_pages[idx].local_addr;
        }
    } else if (input[0] == 'G' || input[0] == 'g') {
        /* Global index */
        int idx = atoi(input + 1);
        if (idx >= 0 && (uint32_t)idx < global_count) {
            global_addr = global_pages[idx].global_addr;
            /* Check if we have it locally */
            for (uint32_t i = 0; i < local_count; i++) {
                if (local_pages[i].global_addr == global_addr) {
                    local_addr = local_pages[i].local_addr;
                    break;
                }
            }
        }
    } else if (input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
        /* Hex address */
        sscanf(input, "%lx", (unsigned long *)&global_addr);
    } else {
        /* Try as plain number - assume local index */
        int idx = atoi(input);
        if (idx >= 0 && (uint32_t)idx < local_count) {
            global_addr = local_pages[idx].global_addr;
            local_addr = local_pages[idx].local_addr;
        } else {
            /* Try as hex without 0x prefix */
            sscanf(input, "%lx", (unsigned long *)&global_addr);
        }
    }
    
    if (local_pages) free(local_pages);
    if (global_pages) free(global_pages);
    
    if (global_addr == 0) {
        printf("Invalid address/index.\n");
        return;
    }
    
    /* If we don't have a local mapping, try to map it */
    if (local_addr == NULL) {
        printf("Mapping page at global address 0x%lx...\n", (unsigned long)global_addr);
        local_addr = dsm_map_remote(global_addr, PAGE_SIZE);
        if (local_addr == NULL) {
            printf("ERROR: Failed to map page.\n");
            return;
        }
        printf("Mapped to local address: %p\n", local_addr);
    }
    
    printf("Enter text to write: ");
    fflush(stdout);
    
    char write_input[256];
    if (fgets(write_input, sizeof(write_input), stdin) == NULL) {
        printf("Read error.\n");
        return;
    }
    
    /* Remove trailing newline */
    size_t len = strlen(write_input);
    if (len > 0 && write_input[len-1] == '\n') {
        write_input[len-1] = '\0';
        len--;
    }
    
    printf("\nWriting \"%s\" to page at %p...\n", write_input, local_addr);
    printf("(This will trigger ownership transfer if we don't own it)\n");
    
    /* Write will trigger write fault and ownership transfer if needed */
    volatile char *ptr = (volatile char *)local_addr;
    
    /* Write byte by byte */
    for (size_t i = 0; i <= len; i++) {  /* Include null terminator */
        ptr[i] = write_input[i];
    }
    
    printf("Write complete!\n");
    
    /* Get updated page info */
    dsm_page_info_t info;
    if (dsm_get_page_info(local_addr, &info) == 0) {
        printf("\nPage State After Write:\n");
        printf("  Owner: %u (should be this node: %u)\n", 
               info.owner_id, dsm_get_node_id());
        printf("  State: %s (should be MODIFIED)\n", 
               dsm_page_state_to_string(info.state));
    }
    
    printf("\n");
}

/* Allocate a new page */
static void allocate_page(void) {
    printf("\n=== ALLOCATE NEW PAGE ===\n");
    
    printf("Enter initial text for the page (or empty for default): ");
    fflush(stdout);
    
    char alloc_input[256];
    if (fgets(alloc_input, sizeof(alloc_input), stdin) == NULL) {
        printf("Read error.\n");
        return;
    }
    
    /* Remove trailing newline */
    size_t len = strlen(alloc_input);
    if (len > 0 && alloc_input[len-1] == '\n') {
        alloc_input[len-1] = '\0';
        len--;
    }
    
    printf("Allocating new page...\n");
    
    void *addr = dsm_malloc(PAGE_SIZE);
    if (addr == NULL) {
        printf("ERROR: dsm_malloc failed!\n");
        return;
    }
    
    printf("Allocated at local address: %p\n", addr);
    
    /* Write content */
    if (len == 0) {
        snprintf((char *)addr, PAGE_SIZE, "Page allocated by node %u", 
                 dsm_get_node_id());
    } else {
        strncpy((char *)addr, alloc_input, PAGE_SIZE - 1);
        ((char *)addr)[PAGE_SIZE - 1] = '\0';
    }
    
    /* Get page info */
    dsm_page_info_t info;
    if (dsm_get_page_info(addr, &info) == 0) {
        printf("Global address: 0x%lx\n", (unsigned long)info.global_addr);
        printf("Owner: %u (this node)\n", info.owner_id);
        printf("State: %s\n", dsm_page_state_to_string(info.state));
    } else {
        printf("Warning: Could not get page info\n");
    }
    
    printf("Page allocated successfully.\n");
    printf("\n");
}

/* Manually map a remote page by global address */
static void add_remote_page(void) {
    printf("\n=== MAP REMOTE PAGE ===\n");
    printf("Enter global address (hex, e.g., 0x100a000): ");
    fflush(stdout);
    
    char addr_input[64];
    if (fgets(addr_input, sizeof(addr_input), stdin) == NULL) {
        printf("Read error.\n");
        return;
    }
    
    uint64_t global_addr;
    if (sscanf(addr_input, "%lx", (unsigned long *)&global_addr) != 1 &&
        sscanf(addr_input, "0x%lx", (unsigned long *)&global_addr) != 1) {
        printf("Invalid address format.\n");
        return;
    }
    
    printf("Mapping page at global address 0x%lx...\n", (unsigned long)global_addr);
    
    void *local_addr = dsm_map_remote(global_addr, PAGE_SIZE);
    if (local_addr == NULL) {
        printf("ERROR: Failed to map remote page.\n");
        return;
    }
    
    printf("Mapped to local address: %p\n", local_addr);
    printf("Use option 3 (read) or 4 (write) to access this page.\n\n");
}

/* Print menu */
static void print_menu(void) {
    printf("\n╔═══════════════════════════════════════════╗\n");
    printf("║         DSM EXPLORER MENU                 ║\n");
    printf("╠═══════════════════════════════════════════╣\n");
    printf("║  1) Show LOCAL pages (this node)          ║\n");
    printf("║  2) Show GLOBAL pages (all cluster)       ║\n");
    printf("║  3) Read a page                           ║\n");
    printf("║  4) Write to a page                       ║\n");
    printf("║  5) Allocate a new page                   ║\n");
    printf("║  6) Map remote page by address            ║\n");
    printf("║  7) Debug: dump internal state            ║\n");
    printf("║  q) Quit                                  ║\n");
    printf("╚═══════════════════════════════════════════╝\n");
    printf("Choice: ");
    fflush(stdout);
}

int main(int argc, char **argv) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                    DSM EXPLORER v1.0                             ║\n");
    printf("║           Interactive Distributed Shared Memory Explorer         ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    if (argc < 3) {
        printf("Usage: %s <port> <chunks>\n", argv[0]);
        printf("  port   - TCP port to listen on\n");
        printf("  chunks - Number of 4KB chunks for memory pool\n");
        printf("\nExample:\n");
        printf("  Terminal 1 (master): %s 9000 10\n", argv[0]);
        printf("  Terminal 2 (worker): %s 9001 10\n", argv[0]);
        printf("  Terminal 3 (worker): %s 9002 10\n", argv[0]);
        return 1;
    }
    
    printf("Initializing DSM...\n");
    
    if (dsm_init(argc, argv) != 0) {
        printf("ERROR: DSM initialization failed!\n");
        return 1;
    }
    
    printf("\n");
    printf("DSM initialized successfully!\n");
    printf("  Node ID: %u\n", dsm_get_node_id());
    printf("  Role: %s\n", dsm_is_master() ? "MASTER" : "WORKER");
    printf("  Node Count: %u\n", dsm_get_node_count());
    printf("\n");
    
    /* Main menu loop */
    char choice[16];
    
    while (1) {
        print_menu();
        
        if (fgets(choice, sizeof(choice), stdin) == NULL) {
            break;
        }
        
        switch (choice[0]) {
            case '1':
                show_local_pages();
                break;
                
            case '2':
                show_global_pages();
                break;
                
            case '3':
                read_page();
                break;
                
            case '4':
                write_page();
                break;
                
            case '5':
                allocate_page();
                break;
                
            case '6':
                add_remote_page();
                break;
                
            case '7':
                printf("\n--- DSM Internal State ---\n");
                dsm_debug_dump_state();
                printf("--- End of Debug State ---\n\n");
                break;
                
            case 'q':
            case 'Q':
                printf("\nShutting down DSM...\n");
                dsm_finalize();
                printf("Goodbye!\n");
                return 0;
                
            default:
                printf("Invalid option. Please choose 1-7 or q.\n");
                break;
        }
    }
    
    dsm_finalize();
    return 0;
}
