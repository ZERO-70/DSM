/**
 * @file test_dsm.c
 * @brief Test program for the Distributed Shared Memory system
 * 
 * This test program demonstrates:
 *   1. DSM initialization and discovery
 *   2. Memory allocation
 *   3. Distributed locks
 *   4. Barrier synchronization
 *   5. Page fault handling (when accessing remote memory)
 * 
 * Usage:
 *   # Start first node (becomes master):
 *   ./test_dsm 8080 10
 * 
 *   # Start second node (becomes worker):
 *   ./test_dsm 8081 10
 * 
 *   Both nodes must be on the same LAN.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "dsm.h"

/* Global flag for clean shutdown */
static volatile int g_running = 1;

/* Signal handler for clean shutdown */
static void signal_handler(int sig) {
    (void)sig;
    printf("\nReceived signal, shutting down...\n");
    g_running = 0;
}

/* Test 1: Basic memory allocation */
static void test_memory_allocation(void) {
    printf("\n=== Test 1: Memory Allocation ===\n");
    
    /* Allocate some memory */
    size_t size = 4096 * 4; /* 4 pages */
    printf("Allocating %zu bytes...\n", size);
    
    int *data = (int*)dsm_malloc(size);
    if (!data) {
        printf("FAILED: dsm_malloc returned NULL\n");
        return;
    }
    printf("Allocated at address: %p\n", (void*)data);
    
    /* Write to the memory */
    printf("Writing data...\n");
    for (size_t i = 0; i < size / sizeof(int); i++) {
        data[i] = i * 2;
    }
    
    /* Read it back */
    printf("Verifying data...\n");
    int errors = 0;
    for (size_t i = 0; i < size / sizeof(int); i++) {
        if (data[i] != (int)(i * 2)) {
            errors++;
        }
    }
    
    if (errors == 0) {
        printf("SUCCESS: Memory read/write test passed\n");
    } else {
        printf("FAILED: %d errors found\n", errors);
    }
    
    /* Free the memory */
    printf("Freeing memory...\n");
    dsm_free(data);
    printf("Memory freed\n");
}

/* Test 2: Distributed locks */
static void test_locks(void) {
    printf("\n=== Test 2: Distributed Locks ===\n");
    
    int lock_id = 0;
    
    printf("Acquiring lock %d...\n", lock_id);
    dsm_lock(lock_id);
    printf("Lock %d acquired\n", lock_id);
    
    /* Do some work */
    printf("Doing critical section work...\n");
    sleep(2);
    
    printf("Releasing lock %d...\n", lock_id);
    dsm_unlock(lock_id);
    printf("Lock %d released\n", lock_id);
}

/* Test 3: Barrier synchronization */
static void test_barrier(void) {
    printf("\n=== Test 3: Barrier Synchronization ===\n");
    
    printf("Waiting at barrier...\n");
    printf("(Other nodes must also reach the barrier)\n");
    dsm_barrier();
    printf("Passed barrier!\n");
}

/* Test 6: TRUE Distributed Shared Memory Test
 * 
 * This test demonstrates actual distributed memory sharing:
 * - Master allocates memory at a KNOWN global address and writes data
 * - Worker maps the SAME global address and reads data (triggers page fault)
 * 
 * Run sequence:
 *   1. Press '6' on MASTER first - it allocates and writes "HELLO DSM!"
 *   2. Press '6' on WORKER - it reads from master's memory via page fault
 */
static void test_true_dsm(void) {
    printf("\n=== Test 6: TRUE Distributed Shared Memory ===\n");
    
    /* Use a fixed global address that both nodes know */
    /* This is the base of the shared address space */
    uint64_t shared_global_addr = 0x2000000;  /* 32MB mark */
    size_t size = 4096;  /* One page */
    dsm_page_info_t page_info;
    
    if (dsm_is_master()) {
        printf("[MASTER] Allocating shared page at global addr 0x%lx\n", shared_global_addr);
        
        /* Master allocates and owns this page */
        char *data = (char*)dsm_malloc_at(shared_global_addr, size);
        if (!data) {
            printf("[MASTER] FAILED: Could not allocate at global address\n");
            return;
        }
        
        /* Show initial page ownership */
        printf("\n[MASTER] --- Page Info BEFORE Write ---\n");
        if (dsm_get_page_info(data, &page_info) == 0) {
            printf("[MASTER]   Owner Node:    %u %s\n", page_info.owner_id,
                   page_info.owner_id == dsm_get_node_id() ? "(THIS NODE - we own it)" : "(REMOTE)");
            printf("[MASTER]   State:         %s\n", dsm_page_state_to_string(page_info.state));
            printf("[MASTER]   Global Addr:   0x%lx\n", page_info.global_addr);
            printf("[MASTER]   Local Addr:    %p\n", page_info.local_addr);
            printf("[MASTER]   Version:       %lu\n", page_info.version);
            printf("[MASTER]   Dirty:         %s\n", page_info.dirty ? "YES" : "NO");
        } else {
            printf("[MASTER]   ERROR: Could not get page info!\n");
        }
        
        printf("\n[MASTER] Writing test data: 'HELLO DSM! From Master Node'\n");
        strcpy(data, "HELLO DSM! From Master Node");
        
        /* Write more data to prove the whole page works */
        for (int i = 0; i < 100; i++) {
            data[100 + i] = 'A' + (i % 26);
        }
        data[200] = '\0';
        
        /* Show page ownership after write */
        printf("\n[MASTER] --- Page Info AFTER Write ---\n");
        if (dsm_get_page_info(data, &page_info) == 0) {
            printf("[MASTER]   Owner Node:    %u %s\n", page_info.owner_id,
                   page_info.owner_id == dsm_get_node_id() ? "(THIS NODE - we own it)" : "(REMOTE)");
            printf("[MASTER]   State:         %s\n", dsm_page_state_to_string(page_info.state));
            printf("[MASTER]   Version:       %lu\n", page_info.version);
            printf("[MASTER]   Dirty:         %s\n", page_info.dirty ? "YES" : "NO");
        }
        
        printf("\n[MASTER] Data written. Page is ready for workers to fetch.\n");
        printf("[MASTER] Now press '6' on WORKER to read this data via page fault!\n");
        printf("[MASTER] Local address: %p\n", (void*)data);
        
    } else {
        printf("[WORKER] Attempting to read shared page at global addr 0x%lx\n", shared_global_addr);
        printf("[WORKER] This will trigger a PAGE FAULT and fetch from master...\n");
        
        /* Worker maps the same global address - this triggers page fault */
        char *data = (char*)dsm_map_remote(shared_global_addr, size);
        if (!data) {
            printf("[WORKER] FAILED: Could not map remote address\n");
            printf("[WORKER] Make sure MASTER pressed '6' first!\n");
            return;
        }
        
        /* Show page info BEFORE reading (should be INVALID) */
        printf("\n[WORKER] --- Page Info BEFORE Read ---\n");
        if (dsm_get_page_info(data, &page_info) == 0) {
            printf("[WORKER]   Owner Node:    %u (Node %u owns this page, we will fetch from them)\n", 
                   page_info.owner_id, page_info.owner_id);
            printf("[WORKER]   State:         %s (page not yet fetched)\n", dsm_page_state_to_string(page_info.state));
            printf("[WORKER]   Global Addr:   0x%lx\n", page_info.global_addr);
            printf("[WORKER]   Local Addr:    %p\n", page_info.local_addr);
            printf("[WORKER]   Version:       %lu\n", page_info.version);
        } else {
            printf("[WORKER]   ERROR: Could not get page info!\n");
        }
        
        printf("\n[WORKER] Reading data from remote page (this triggers page fault)...\n");
        
        /* This read will trigger SIGSEGV → page fault handler → fetch from master */
        printf("[WORKER] Message from master: '%s'\n", data);
        printf("[WORKER] Additional data: '%s'\n", data + 100);
        
        /* Show page info AFTER reading (should be SHARED) */
        printf("\n[WORKER] --- Page Info AFTER Read ---\n");
        if (dsm_get_page_info(data, &page_info) == 0) {
            printf("[WORKER]   Owner Node:    %u (data was fetched from Node %u)\n", 
                   page_info.owner_id, page_info.owner_id);
            printf("[WORKER]   State:         %s (page now cached locally)\n", dsm_page_state_to_string(page_info.state));
            printf("[WORKER]   Version:       %lu\n", page_info.version);
        }
        
        printf("\n[WORKER] SUCCESS! Read data from Node %d's memory via page fault!\n", 
               dsm_get_page_owner(data));
    }
}

/* Interactive menu */
static void show_menu(void) {
    printf("\n");
    printf("==================================\n");
    printf("   DSM Test Menu\n");
    printf("==================================\n");
    printf("  1. Test memory allocation\n");
    printf("  2. Test distributed locks\n");
    printf("  3. Test barrier sync\n");
    printf("  4. Show node info\n");
    printf("  5. Run all tests\n");
    printf("  6. TRUE DSM test (page fault)\n");
    printf("  q. Quit\n");
    printf("==================================\n");
    printf("Choice: ");
}

static void show_node_info(void) {
    printf("\n=== Node Information ===\n");
    printf("Node ID:    %u\n", dsm_get_node_id());
    printf("Role:       %s\n", dsm_is_master() ? "MASTER" : "WORKER");
    printf("Node Count: %u\n", dsm_get_node_count());
    printf("Total Mem:  %zu bytes\n", dsm_get_total_memory());
}

int main(int argc, char **argv) {
    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("========================================\n");
    printf("  Distributed Shared Memory Test\n");
    printf("========================================\n");
    
    if (argc < 3) {
        printf("Usage: %s <port> <chunks>\n", argv[0]);
        printf("  port   - TCP port to listen on (e.g., 8080)\n");
        printf("  chunks - Number of 4KB chunks to donate (e.g., 10)\n");
        return 1;
    }
    
    /* Set log level to INFO */
    dsm_set_log_level(1);
    
    /* Initialize DSM */
    printf("\nInitializing DSM system...\n");
    printf("Port: %s, Chunks: %s\n", argv[1], argv[2]);
    
    if (dsm_init(argc, argv) != 0) {
        fprintf(stderr, "Failed to initialize DSM\n");
        return 1;
    }
    
    /* Show initial node info */
    show_node_info();
    
    /* Main loop */
    char choice[16];
    while (g_running) {
        show_menu();
        
        if (fgets(choice, sizeof(choice), stdin) == NULL) {
            break;
        }
        
        switch (choice[0]) {
            case '1':
                test_memory_allocation();
                break;
            case '2':
                test_locks();
                break;
            case '3':
                test_barrier();
                break;
            case '4':
                show_node_info();
                break;
            case '5':
                test_memory_allocation();
                test_locks();
                /* Only test barrier if multiple nodes */
                if (dsm_get_node_count() > 1) {
                    test_barrier();
                } else {
                    printf("\nSkipping barrier test (need multiple nodes)\n");
                }
                break;
            case '6':
                if (dsm_get_node_count() < 2) {
                    printf("\nNeed 2 nodes for TRUE DSM test. Start another node first.\n");
                } else {
                    test_true_dsm();
                }
                break;
            case 'q':
            case 'Q':
                g_running = 0;
                break;
            default:
                printf("Invalid choice\n");
        }
    }
    
    /* Cleanup */
    printf("\nShutting down DSM...\n");
    dsm_finalize();
    printf("DSM finalized. Goodbye!\n");
    
    return 0;
}
