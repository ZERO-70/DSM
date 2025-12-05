// worker.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dsm.h"

#define PORT 9001
#define CHUNKS 4          // number of 4KB chunks this node donates
#define PAGE_SIZE 4096
#define GLOBAL_ADDR 0x100000000ULL  // must match master's address

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (dsm_init_with_params(PORT, CHUNKS) != 0) {
        fprintf(stderr, "dsm_init failed\n");
        return 1;
    }

    dsm_set_log_level(1); // INFO

    if (dsm_is_master()) {
        fprintf(stderr, "This node became master; intended role is worker. Re-run so roles are correct.\n");
        dsm_finalize();
        return 1;
    }

    printf("[WORKER] Node ID: %u  Node Count: %u\n", dsm_get_node_id(), dsm_get_node_count());
    printf("[WORKER] Mapping remote page at global address 0x%llx\n", (unsigned long long)GLOBAL_ADDR);

    void *local = dsm_map_remote(GLOBAL_ADDR, PAGE_SIZE);
    if (!local) {
        fprintf(stderr, "[WORKER] dsm_map_remote failed\n");
        dsm_finalize();
        return 1;
    }

    printf("[WORKER] local pointer for mapped remote page: %p\n", local);

    // Query owner and page info before any access
    int owner_before = dsm_get_page_owner(local);
    dsm_page_info_t info_before;
    if (dsm_get_page_info(local, &info_before) == 0) {
        printf("[WORKER] BEFORE read: owner=%u, state=%s, version=%llu, dirty=%d\n",
               info_before.owner_id,
               dsm_page_state_to_string(info_before.state),
               (unsigned long long)info_before.version,
               info_before.dirty);
    } else {
        printf("[WORKER] BEFORE read: dsm_get_page_info failed\n");
    }

    // Access (read) the mapped memory to trigger page fetch
    printf("[WORKER] Reading first 128 bytes of the mapped page (this will fault & fetch if needed):\n");
    fwrite(local, 1, 128, stdout);
    printf("\n");

    // Query owner and page info after read
    dsm_page_info_t info_after;
    if (dsm_get_page_info(local, &info_after) == 0) {
        printf("[WORKER] AFTER read: owner=%u, state=%s, version=%llu, dirty=%d\n",
               info_after.owner_id,
               dsm_page_state_to_string(info_after.state),
               (unsigned long long)info_after.version,
               info_after.dirty);
    } else {
        printf("[WORKER] AFTER read: dsm_get_page_info failed\n");
    }

    // Synchronize with master so master can inspect post-read ownership
    dsm_barrier();

    dsm_finalize();
    printf("[WORKER] finalize done\n");
    return 0;
}
