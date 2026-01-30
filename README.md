# DSM - Distributed Shared Memory System

A demo implementation of a distributed shared memory system in C with page-level coherence.

## Features

- **Single binary** that runs as server (master) or client (worker)
- **Page-level coherence protocol** with three states:
  - `ABSENT`: Page not present locally
  - `SHARED`: Page present, read-only (can be replicated)
  - `EXCLUSIVE`: Page present, read-write (single owner)
- **Safe page fault handling**: Uses setjmp/longjmp pattern for reliable fault recovery
- **TCP-based communication** for reliable page transfer
- **Fixed memory mapping** at consistent address across processes
- **Platform-aware page size**: 16KB on Apple Silicon, 4KB on x86

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        SERVER (Master)                      │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ Global Page │  │   Network   │  │   DSM Region        │  │
│  │  Directory  │  │   Handler   │  │   (16MB at 0x7...)  │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
└────────────────────────────┬────────────────────────────────┘
                             │ TCP
        ┌────────────────────┼────────────────────┐
        │                    │                    │
┌───────▼───────┐    ┌───────▼───────┐    ┌───────▼───────┐
│   CLIENT 0    │    │   CLIENT 1    │    │   CLIENT N    │
│ ┌───────────┐ │    │ ┌───────────┐ │    │ ┌───────────┐ │
│ │Local Page │ │    │ │Local Page │ │    │ │Local Page │ │
│ │  Table    │ │    │ │  Table    │ │    │ │  Table    │ │
│ └───────────┘ │    │ └───────────┘ │    │ └───────────┘ │
│ ┌───────────┐ │    │ ┌───────────┐ │    │ ┌───────────┐ │
│ │  SIGSEGV  │ │    │ │  SIGSEGV  │ │    │ │  SIGSEGV  │ │
│ │  Handler  │ │    │ │  Handler  │ │    │ │  Handler  │ │
│ └───────────┘ │    │ └───────────┘ │    │ └───────────┘ │
│ ┌───────────┐ │    │ ┌───────────┐ │    │ ┌───────────┐ │
│ │DSM Region │ │    │ │DSM Region │ │    │ │DSM Region │ │
│ └───────────┘ │    │ └───────────┘ │    │ └───────────┘ │
└───────────────┘    └───────────────┘    └───────────────┘
```

## Coherence Protocol

```
          ┌─────────────────┐
          │     ABSENT      │
          │  (PROT_NONE)    │
          └────────┬────────┘
                   │
       ┌───────────┴───────────┐
       │ read fault            │ write fault
       ▼                       ▼
┌──────────────┐        ┌──────────────┐
│    SHARED    │───────▶│  EXCLUSIVE   │
│ (PROT_READ)  │ upgrade│(PROT_READ|   │
└──────────────┘        │ PROT_WRITE)  │
       ▲                └──────┬───────┘
       │                       │
       │ invalidate            │ invalidate
       └───────────────────────┘
```

## Building

```bash
make
```

On Linux, the binary is fully statically linked.
On macOS, static linking is limited; run with `DYLD_NO_PIE=1`.

## Usage

### Start Server
```bash
./dsm --server --port 9000 --clients 2 -v
```

### Start Clients (in separate terminals)
```bash
./dsm --client --host 127.0.0.1 --port 9000 -v
```

### Options
- `--server, -s`: Run as server (master)
- `--client, -c`: Run as client (worker)  
- `--host HOST, -h`: Server hostname (required for client)
- `--port PORT, -p`: Port number (default: 9000)
- `--clients N, -n`: Number of clients to wait for (server only, default: 1)
- `--verbose, -v`: Enable debug output showing page transitions

## Demo: Distributed Vector Sum

The demo computes the sum of a vector of 1M integers distributed across clients:

1. Server initializes vector with `vector[i] = i % 100`
2. Server distributes work ranges to connected clients
3. Each client computes partial sum, triggering page faults as needed
4. Page faults are handled by requesting pages from server via TCP
5. Results are collected and verified

## How It Works

1. **Memory Region**: A 16MB region is `mmap`'d at a fixed address (`0x700000000000`) with `MAP_FIXED` to ensure consistent addresses across processes.

2. **Page Protection**: Initially all client pages are `PROT_NONE`. The server initializes data with `PROT_READ|PROT_WRITE`.

3. **Fault Handling**: When a client accesses a page, `SIGSEGV` is triggered. The handler:
   - Determines if it's a read or write fault
   - Sends a page request to the server
   - Receives page data and updates local `mprotect`

4. **Page Transfer**: The server manages a global directory tracking:
   - Which client owns each page exclusively
   - Which clients have shared copies
   - When to invalidate cached copies

## Files

- `dsm.h` - Main header with structures and constants
- `dsm.c` - Entry point, server/client main loops
- `memory.c/h` - Memory region management (mmap, mprotect)
- `network.c/h` - TCP communication
- `fault_handler.c/h` - SIGSEGV handler
- `coherence.c/h` - Page state transitions
- `worker.c/h` - Demo computation (vector sum)

## Limitations

- **Page size**: 16KB on Apple Silicon (macOS ARM64), 4KB on x86/Linux
- **Max clients**: 4 (configurable in `dsm.h`)
- **Region size**: 16MB fixed (configurable)
- **No persistence**: Data is lost when server exits
- **Read-only demo**: Current demo only reads data; write coherence is implemented but not exercised
