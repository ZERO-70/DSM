# Distributed Shared Memory (DSM) System

A distributed shared memory system implementation in C for nodes on a Local Area Network (LAN).

## Features

- **Automatic Discovery**: UDP broadcast-based node discovery
- **Master/Worker Architecture**: Automatic master election (first node becomes master)
- **Transparent Memory Sharing**: mmap-based memory with SIGSEGV page fault handling
- **Distributed Synchronization**: Locks and barriers for coordination
- **Demand Paging**: Pages fetched from remote nodes on access

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Application                             │
│              dsm_init() / dsm_malloc() / etc                │
├─────────────────────────────────────────────────────────────┤
│                      DSM API Layer                          │
│                        (dsm.h)                              │
├──────────┬──────────┬──────────┬──────────┬─────────────────┤
│   Init   │ Network  │  Memory  │   Sync   │   Page Fault    │
│ dsm_init │ dsm_tcp  │dsm_memory│ dsm_sync │ dsm_pagefault   │
│          │ dsm_udp  │          │          │                 │
├──────────┴──────────┴──────────┴──────────┴─────────────────┤
│                    Operating System                          │
│            sockets / mmap / signals / pthreads              │
└─────────────────────────────────────────────────────────────┘
```

## Building

```bash
# Build everything
make

# Build with debug symbols
make debug

# Clean build artifacts
make clean
```

## Usage

### Running Locally (Same Machine, Two Terminals)

This is the easiest way to test. Open two terminal windows:

```bash
# Terminal 1 - Start master (wait for it to initialize)
cd ~/Documents/dsm
LD_LIBRARY_PATH=./bin ./bin/test_dsm 9000 10

# Terminal 2 - Start worker (run after master is ready)
cd ~/Documents/dsm
LD_LIBRARY_PATH=./bin ./bin/test_dsm 9001 10
```

### Running on Multiple Machines (Same LAN/WiFi)

Both machines must be connected to the **same WiFi network** or LAN.

**On Machine A (will become MASTER):**
```bash
cd ~/Documents/dsm
LD_LIBRARY_PATH=./bin ./bin/test_dsm 9000 10
```

**On Machine B (will become WORKER):**
```bash
cd ~/Documents/dsm
LD_LIBRARY_PATH=./bin ./bin/test_dsm 9000 10
```

> **Note**: You can use the same port (9000) on different machines since they have different IPs.

### Firewall Configuration (if needed)

If nodes can't discover each other, open the required ports:

```bash
# Ubuntu/Debian
sudo ufw allow 9999/udp   # Discovery port
sudo ufw allow 9000/tcp   # Your chosen TCP port

# Fedora/RHEL
sudo firewall-cmd --add-port=9999/udp --permanent
sudo firewall-cmd --add-port=9000/tcp --permanent
sudo firewall-cmd --reload
```

### Parameters

| Parameter | Description | Example |
|-----------|-------------|---------|
| `port` | TCP port for this node | 9000 |
| `chunks` | Number of 4KB pages to donate to shared pool | 10 |

### Quick Test Sequence

Once both nodes are running, try these tests in order:

1. **Press `4`** on both → Shows node info (verify connection)
2. **Press `1`** on both → Test memory allocation
3. **Press `2`** on both → Test distributed locks
4. **Press `3`** on both → Test barrier (both must press to continue)
5. **Press `q`** on both → Clean shutdown

### API Usage

```c
#include "dsm.h"

int main(int argc, char **argv) {
    // Initialize DSM (port and chunks from command line)
    if (dsm_init(argc, argv) != 0) {
        return 1;
    }
    
    // Allocate shared memory
    int *data = (int*)dsm_malloc(4096);
    
    // Use distributed lock
    dsm_lock(0);
    data[0] = 42;  // Protected by lock
    dsm_unlock(0);
    
    // Barrier synchronization
    dsm_barrier();
    
    // Cleanup
    dsm_free(data);
    dsm_finalize();
    
    return 0;
}
```

## File Structure

```
dsm/
├── include/
│   ├── dsm.h           # Public API
│   ├── dsm_types.h     # Type definitions and constants
│   ├── dsm_network.h   # Network subsystem interface
│   ├── dsm_memory.h    # Memory management interface
│   └── dsm_internal.h  # Internal structures
├── src/
│   ├── dsm_init.c      # Initialization and cleanup
│   ├── dsm_udp.c       # UDP broadcast discovery
│   ├── dsm_tcp.c       # TCP connection management
│   ├── dsm_network.c   # Node table management
│   ├── dsm_memory.c    # mmap-based memory
│   ├── dsm_pagefault.c # SIGSEGV handler
│   └── dsm_sync.c      # Locks and barriers
├── tests/
│   └── test_dsm.c      # Test program
├── Makefile
└── README.md
```

## Protocol Messages

| Message Type | Description |
|-------------|-------------|
| DISCOVER_DSM | UDP broadcast for node discovery |
| JOIN_RESPONSE | Master assigns node ID |
| NODE_TABLE | Full node table synchronization |
| PAGE_REQUEST | Request page from owner (read) |
| PAGE_DATA | Page content transfer |
| PAGE_WRITE_REQ | Request page for writing (ownership transfer) |
| PAGE_INVALIDATE | Invalidate remote copies |
| INVALIDATE_ACK | Acknowledge invalidation |
| OWNERSHIP_XFER | Transfer ownership with page data |
| LOCK_REQUEST | Request distributed lock |
| LOCK_GRANT | Lock acquisition confirmed |
| BARRIER_ENTER | Node entering barrier |
| BARRIER_RELEASE | All nodes release barrier |

## Memory Model: Invalidate/Exclusive Ownership Protocol

This is a **Full Read/Write DSM** with automatic ownership transfer:

### Page States
- **INVALID**: No local copy - access will trigger page fault
- **SHARED**: Read-only local copy (may exist on multiple nodes)
- **MODIFIED/EXCLUSIVE**: Exclusive write access (only this node has valid copy)
- **PENDING**: Waiting for data transfer

### Protocol Flow

**Read Access (non-owner)**:
1. Node reads page → SIGSEGV (page is INVALID)
2. Page fault handler requests page from owner
3. Owner sends page data, receiver gets SHARED copy
4. If owner had EXCLUSIVE, downgrades to SHARED

**Write Access (non-owner)**:
1. Node writes to SHARED page → SIGSEGV (no write permission)
2. Fault handler sends WRITE_REQUEST to master
3. Master broadcasts INVALIDATE to all copy holders
4. Nodes invalidate copies and send ACKs
5. Current owner transfers ownership + data to requester
6. Requester becomes EXCLUSIVE owner

**Write Access (owner upgrading)**:
1. Owner writes to SHARED page → SIGSEGV
2. Master invalidates all other copies
3. Owner upgrades to EXCLUSIVE (MODIFIED) state

### Memory Access Rules

| Operation | Behavior |
|-----------|----------|
| **Read remote page** | ✅ Works - Page fetched automatically, becomes SHARED |
| **Write to own page** | ✅ Works - If SHARED, upgrades to MODIFIED |
| **Write to remote page** | ✅ Works - Ownership transferred automatically |

### Test 7: Demonstrating Ownership Transfer

```
# Run sequence:
1. Master writes "MASTER WROTE THIS"
2. Worker reads (gets SHARED copy via page fault)  
3. Worker writes "WORKER WROTE THIS" (triggers ownership transfer!)
4. Master reads back (sees worker's changes)
```

**Best Practices:**
- For shared read-only data: Allocate once, read from all nodes
- For shared writable data: Use `dsm_lock()` + `dsm_unlock()` around modifications
- For producer/consumer: Producer allocates and writes, consumers read
- For parallel computation: Partition data so each node owns its working set

## Synchronization

### Locks
- Master serializes all lock requests
- FIFO ordering for waiters
- Deadlock detection not implemented

### Barriers
- All nodes must participate
- Master collects arrivals and releases

## Logging

Set log level with `dsm_set_log_level()`:
- 0 = DEBUG
- 1 = INFO (default)
- 2 = WARN
- 3 = ERROR

## Limitations

- **Read-Replication Model**: Only the allocating node can write to a page (by design)
- Single LAN only (no NAT traversal)
- No persistence (memory lost on restart)
- No deadlock detection for locks
- No automatic master failover
- No dynamic ownership transfer

## Requirements

- Linux (tested on Ubuntu 22.04+)
- GCC with pthread support
- Network interface with broadcast support

## License

MIT License
