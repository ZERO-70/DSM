// master.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dsm.h"

#define PORT 9000
#define CHUNKS 8          // number of 4KB chunks this node donates
#define PAGE_SIZE 4096
#define GLOBAL_ADDR 0x100000000ULL  // chosen global DSM address

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (dsm_init_with_params(PORT, CHUNKS) != 0) {
        fprintf(stderr, "dsm_init failed\n");
        return 1;
    }

    dsm_set_log_level(1); // INFO

    if (!dsm_is_master()) {
        fprintf(stderr, "This node is not master (run master on a node that becomes master)\n");
        dsm_finalize();
        return 1;
    }

    printf("[MASTER] Node ID: %u  Node Count: %u\n", dsm_get_node_id(), dsm_get_node_count());
    printf("[MASTER] Allocating one page at global address 0x%llx\n", (unsigned long long)GLOBAL_ADDR);

    void *local = dsm_malloc_at(GLOBAL_ADDR, PAGE_SIZE);
    if (!local) {
        fprintf(stderr, "[MASTER] dsm_malloc_at failed\n");
        dsm_finalize();
        return 1;
    }

    printf("[MASTER] local pointer: %p\n", local);

    // Query owner and page info before any modification
    int owner_before = dsm_get_page_owner(local);
    dsm_page_info_t info_before;
    if (dsm_get_page_info(local, &info_before) == 0) {
        printf("[MASTER] BEFORE write: owner=%u, state=%s, version=%llu, dirty=%d\n",
               info_before.owner_id,
               dsm_page_state_to_string(info_before.state),
               (unsigned long long)info_before.version,
               info_before.dirty);
    } else {
        printf("[MASTER] BEFORE write: dsm_get_page_info failed\n");
    }

    // Write something into the page (mark it modified)
    const char *msg = "Hello from MASTER - shared page test!\n";
    strncpy((char *)local, msg, PAGE_SIZE - 1);
    // optionally write a pattern across page
    for (size_t i = strlen(msg); i < 128; ++i) {
        ((char *)local)[i] = (char)('A' + (i % 26));
    }

    // Query owner and page info after modification
    dsm_page_info_t info_after;
    if (dsm_get_page_info(local, &info_after) == 0) {
        printf("[MASTER] AFTER write: owner=%u, state=%s, version=%llu, dirty=%d\n",
               info_after.owner_id,
               dsm_page_state_to_string(info_after.state),
               (unsigned long long)info_after.version,
               info_after.dirty);
    } else {
        printf("[MASTER] AFTER write: dsm_get_page_info failed\n");
    }

    // Let worker map/read the remote address.
    // Synchronize: barrier will block until worker also arrives.
    printf("[MASTER] Barrier: waiting for worker to map & read...\n");
    dsm_barrier();

    // After barrier: worker should have fetched/seen the page (depending on protocol)
    dsm_page_info_t info_post_barrier;
    if (dsm_get_page_info(local, &info_post_barrier) == 0) {
        printf("[MASTER] POST-BARRIER: owner=%u, state=%s, version=%llu, dirty=%d\n",
               info_post_barrier.owner_id,
               dsm_page_state_to_string(info_post_barrier.state),
               (unsigned long long)info_post_barrier.version,
               info_post_barrier.dirty);
    } else {
        printf("[MASTER] POST-BARRIER: dsm_get_page_info failed\n");
    }

    // Print first 128 bytes to confirm content still present
    printf("[MASTER] First 128 bytes of page:\n");
    fwrite(local, 1, 128, stdout);
    printf("\n");

    dsm_finalize();
    printf("[MASTER] finalize done\n");
    return 0;
}
