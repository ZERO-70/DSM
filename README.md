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
| PAGE_REQUEST | Request page from owner |
| PAGE_DATA | Page content transfer |
| LOCK_REQUEST | Request distributed lock |
| LOCK_GRANT | Lock acquisition confirmed |
| BARRIER_ENTER | Node entering barrier |
| BARRIER_RELEASE | All nodes release barrier |

## Memory Model

- **Centralized Ownership**: Master tracks page ownership
- **Demand Paging**: Pages fetched on first access
- **SIGSEGV Handling**: Page faults trigger remote fetch
- **mmap with PROT_NONE**: Initial pages have no access permissions

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

- Single LAN only (no NAT traversal)
- No persistence (memory lost on restart)
- No deadlock detection for locks
- No automatic master failover

## Requirements

- Linux (tested on Ubuntu 22.04+)
- GCC with pthread support
- Network interface with broadcast support

## License

MIT License
# DSM
