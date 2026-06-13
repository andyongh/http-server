# httpserver v0.3

High-performance HTTP/1.1 server library for Linux and macOS. Powered by `fsae`, `llhttp`, `LuaJIT`, and `jemalloc`, with support for both C and Lua APIs.

---

## What changed in v0.3

| | v0.2 | v0.3 |
|--|------|------|
| Event library | fsae (multi-file) | **fsae** (single-header, STB-style) |
| Reactor | single only | **single + multi** |
| Transport | TCP + UDS | TCP + UDS (unchanged) |
| Receive buffer | ring buffer | **zero-malloc ring + overflow** |
| llhttp errors | partial | **all HPE_* codes mapped** |
| Sanitizer | opt-in cmake | **ASAN/TSan/UBSan via `make`** |
| Tests | ring only | **4 suites: ring, queue, parser, integration** |
| Build | CMake only | **CMake + Makefile** |

---

## Architecture

```
Single-Reactor (HS_REACTOR_SINGLE)
───────────────────────────────────
  Calling thread
  ┌─────────────────────────────────────────────────────────┐
  │  fsae epoll loop                                        │
  │  accept()  ──► conn_pool_alloc  ──► AE_READABLE        │
  │  read_cb   ──► hs_ring_recv (readv)                     │
  │            ──► hs_conn_recv_and_feed                    │
  │            ──► llhttp → message_complete                │
  │            ──► hs_spmc_push(work_queue)                 │
  │  resp_efd  ──► hs_mpsc_pop(resp_queue)                  │
  │            ──► serialise → AE_WRITABLE → write()       │
  └─────────────────────────────────────────────────────────┘
                   │ (SPMC, mutex+condvar)
        ┌──────────┼──────────┐
        ▼          ▼          ▼
    worker[0]  worker[1]  worker[N]   (CPU thread pool)
    per-thread lua_State (JIT-compiled)
        │
        └──► hs_mpsc_push(resp_queue) + write(eventfd)

Multi-Reactor (HS_REACTOR_MULTI)
─────────────────────────────────
  Boss thread (accept only)
  ┌─────────────────────────────────────────────────────────┐
  │  fsae epoll – accept4() → write(pipe[i], client_fd)    │
  └──────────────┬──────────────────┬───────────────────────┘
                 │ pipe             │ pipe (round-robin)
         sub[0] ─┘                  └─ sub[1]  ...  sub[N]
         (IO thread)                   (IO thread)
          own conn_pool (zero-malloc)
          own resp_queue + eventfd
                 │
                 └──► shared CPU thread pool
```

---

## fsae migration notes

[fsae](https://github.com/andyongh/fsae) is an STB-style single-header event library with the same Redis `ae` API. Key differences from the original Redis `ae`:

- `aeEventLoop` has **no `privdata` field**.  Every callback receives context
  exclusively via the `clientData` argument of `aeCreateFileEvent()`.
- Define `AE_IMPLEMENTATION` in exactly **one** TU before `#include "ae.h"`.
  We do this in `src/hs_ae_impl.c`.  All other TUs include `ae.h` without
  the macro.
- Internal allocator redirected to `je_malloc` / `je_free` via `#define`
  in `hs_ae_impl.c`.

---

## llhttp error handling

Every possible `llhttp_errno_t` is mapped to a `hs_feed_result_t` value in
`hs_conn_recv_and_feed()`:

| llhttp error | hs_feed_result_t | HTTP response |
|---|---|---|
| `HPE_OK` | `HS_FEED_OK` | – |
| `HPE_PAUSED` (after message_complete) | `HS_FEED_OK` | – |
| `HPE_PAUSED` (unexpected) | `HS_FEED_PARSE_ERR` | 400 |
| `HPE_PAUSED_UPGRADE` | `HS_FEED_UPGRADE` | 501 |
| `HPE_USER` + `body_413` | `HS_FEED_TOO_LARGE` | 413 + close |
| `HPE_USER` + `body_upgrade` | `HS_FEED_UPGRADE` | 501 + close |
| `HPE_USER` (OOM) | `HS_FEED_OOM` | 500 + close |
| any other `HPE_*` | `HS_FEED_PARSE_ERR` | 400 + close |
| `readv` → `EAGAIN` | `HS_FEED_AGAIN` | – |
| `readv` → `n == 0` | `HS_FEED_EOF` | close |
| `readv` → hard error | `HS_FEED_IO_ERR` | close |
| ring full | `HS_FEED_TOO_LARGE` | 413 + close |

413 early-out (zero alloc): if `Content-Length > max_body_size` the parser
returns `HPE_USER` in `on_headers_complete` – before any body bytes arrive.

---

## Zero-malloc receive ring

```
hs_ring_t (embedded in hs_conn_t – no heap allocation)
│
├── readv(2 iovecs)  one syscall, fills both sides of the wrap
│     iov[0]: ring.data[w_idx .. RING_SIZE-1]
│     iov[1]: ring.data[0 .. free-iov0]   (only if free straddles wrap)
│
└── hs_conn_recv_and_feed
      ├── segment-1: llhttp_execute(ring.data+r_idx, min(avail,to_end))
      └── segment-2: llhttp_execute(ring.data, remaining)  ← wrap half

Body decision tree:
  CL > max_body_size           → HPE_USER → 413 + close   (0 allocs)
  body fits ring, contiguous   → body_in_ring=1, zero-copy pointer
  body wraps ring              → 1× je_malloc(overflow)
  CL > HS_RING_SIZE            → 1× je_malloc(overflow, CL) upfront
```

`HS_RING_SIZE` defaults to 8192. Override: `cmake -DHS_RING_SIZE=32768` or
`make HS_RING_SIZE=32768`.

---

## Thread-safety model

| Object | Owner | Access rule |
|--------|-------|-------------|
| `hs_conn_t` | IO sub-reactor | IO thread only; workers read request fields (immutable while INFLIGHT) |
| `hs_conn_pool_t` | IO sub-reactor | IO thread only; O(1) stack push/pop |
| `hs_mpsc_t` | One sub-reactor | Lock-free: N workers push, 1 IO thread pops |
| `hs_spmc_t` | Shared | 1 IO thread pushes, N workers pop (mutex+condvar) |
| `srv->config` | Read-only after `hs_server_new()` | No locking needed |
| `srv->running` | Atomic | Signal-handler safe |
| `eventfd` | One sub-reactor | N workers write, 1 IO thread reads |
| `lua_State` | One worker | Per-thread; no sharing |

---

## Cross-Platform Compatibility

`httpserver` v0.3 is fully compatible with both **Linux** and **macOS** (including Apple Silicon / ARM64). Key portability adaptations include:
- **Event Loop Signaling**: Emulates Linux `eventfd` using a self-pipe pair on macOS to wake up the `fsae` reactor.
- **Socket Flags**: Provides fallback shims for non-blocking and close-on-exec configurations on systems lacking `accept4` or `SOCK_CLOEXEC` support.
- **LuaJIT Memory Mapping**: Integrates special linker flags (`pagezero_size` and `image_base`) on Apple Silicon to ensure LuaJIT can map JIT executable memory above the 32-bit boundary.
- **Sanitizers**: AddressSanitizer (ASAN), UBSan, and ThreadSanitizer (TSan) are configured for both platforms, with leak detection disabled on macOS (since LSAN is not supported by Apple Clang).

---

## Getting started

```bash
git clone --recurse-submodules https://github.com/andyongh/http-server
cd http-server

# Build all deps (jemalloc, llhttp, LuaJIT)
chmod +x scripts/bootstrap.sh && ./scripts/bootstrap.sh

# Build (Release)
make

# Build with ASAN + UBSan
make asan

# Run all tests
make test

# Run tests under ASAN
make test-asan

# Run tests under ThreadSanitizer
make test-tsan

# CMake workflow
cmake -S . -B build -DSANITIZE=asan && cmake --build build
ctest --test-dir build -V
```

---

## Project layout

```
httpserver/
├── .gitmodules
├── CMakeLists.txt
├── Makefile
├── scripts/
│   └── bootstrap.sh
├── deps/
│   ├── fsae/          single-header event library (replaces Redis `ae`)
│   ├── llhttp/
│   ├── luajit/
│   └── jemalloc/
├── include/
│   └── httpserver.h   public API
├── src/
│   ├── hs_ae_impl.c   defines AE_IMPLEMENTATION → instantiates fsae
│   ├── hs_ring.h      zero-malloc ring (header-only)
│   ├── hs_queue.h     MPSC lock-free + SPMC ring (header-only)
│   ├── hs_buf.h       growable write buffer (header-only)
│   ├── hs_listener.h/c  TCP + UDS socket creation
│   ├── hs_conn.h/c    connection state, all llhttp callbacks,
│   │                  zero-copy + overflow body, hs_feed_result_t
│   ├── hs_reactor.h/c  fsae clientData API, conn pool,
│   │                   boss/sub MULTI dispatch
│   ├── hs_http.h/c    response object + serialisation
│   ├── hs_pool.h/c    CPU thread pool
│   ├── hs_lua.h/c     per-thread LuaJIT state
│   ├── hs_server.h    internal server struct
│   └── hs_server.c    public API + request accessors
├── examples/
│   ├── main.c
│   └── handler.lua
└── tests/
    ├── test_ring.c        ring buffer (64 B ring, no deps)
    ├── test_queue.c       MPSC/SPMC thread safety (8 × 10K MPSC, 50K SPMC)
    ├── test_parser.c      12 llhttp error-path tests (mock sub-reactor)
    └── test_integration.c 8 end-to-end tests over TCP (400/413/keep-alive/concurrent)
```

---

## API

```c
#include "httpserver.h"

hs_config_t cfg;
hs_config_init(&cfg);
cfg.listen_flags   = HS_LISTEN_TCP | HS_LISTEN_UDS;
cfg.host           = "0.0.0.0";
cfg.port           = 8080;
cfg.uds_path       = "/tmp/myapp.sock";
cfg.reactor_mode   = HS_REACTOR_MULTI;
cfg.num_io_threads = 4;
cfg.num_threads    = 8;
cfg.max_body_size  = 4 * 1024 * 1024;
cfg.handler        = my_handler;

hs_server_t *srv = hs_server_new(&cfg);
hs_server_run(srv);   /* blocks */
hs_server_free(srv);
```

---

## License

MIT
