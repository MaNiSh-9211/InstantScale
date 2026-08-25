# InstantScale — Architecture

## 1. Design thesis

Autoscaling latency is dominated by two costs:

1. **Runtime warm-up** (JIT, caches, pools) — solved by *pre-warming + CRIU checkpoint*.
2. **State transfer** (multi-GB memory images over the network) — solved by
   *lazy on-demand paging via `userfaultfd`*.

InstantScale decouples **activation** from **hydration**:

```
SOURCE HOST A                                TARGET HOST B
─────────────                                ─────────────
warm app process
      │
CRIU dump ──► image splitter ──┬─ skeleton (registers, VMAs, ptables ~KBs)
                               │        │  (fast path: gRPC/TCP)
                               │        ▼
                               │  uffd-restore: map empty VMAs,
                               │  register with userfaultfd(MISSING),
                               │  load registers → process RUNNING ◄── milliseconds
                               │        │
                               └─ bulk page server ◄── page requests on fault
                                        │  (4 KB pulls, prefetched, deduped)
                                        ▼
                                   ioctl(UFFDIO_COPY) per fault
```

The target process becomes schedulable after receiving only kilobytes of state.
Every subsequent page touch is a kernel→daemon event and a single round-trip fetch.

## 2. Components

### 2.1 Orchestrator (`iscale` CLI + daemon) — Rust
- Checkpoint trigger: drives CRIU (via C API bindings or `criu` RPC over protobuf socket).
- Image splitting: parses CRIU image files (`pages-*.img`, `core-*.img`, `mm-*.img`),
  separates page tables/registers (skeleton) from bulk pages.
- Session control: handshake, auth (mTLS), lease of page-server endpoints, teardown.

### 2.2 PF-Daemon (Page Fault Service) — Rust, no GC, async runtime-free hot path
- Owns the `userfaultfd` fd; integrates it into an `epoll(7)` loop (edge-triggered).
- Fault handling: reads `struct uffd_msg`, resolves address → page request.
- Transport: raw TCP with zero-copy receive into a staging buffer aligned to
  `sysconf(_SC_PAGESIZE)`; optional RDMA/io_uring backends behind a trait.
- Injection: `ioctl(fd, UFFDIO_COPY, &uffdio_copy)` — atomic, wakes the faulting thread.
- Policy engine: demand-zero shortcuts, batching (`UFFDIO_COPY` multi-page), read-ahead
  heuristics (sequential-touch detection), LRU prefetch of neighbor pages, write-protect
  mode (`UFFDIO_REGISTER_MODE_WP`) for live re-sync later.

### 2.3 Source-side page server
- Streams pages out of the CRIU image (memory-mapped, `O_DIRECT` friendly).
- Post-checkpoint *dirty tracking* (soft-dirty bit / `UFFDIO_REGISTER_MODE_WP` on source)
  to serve freshest versions for long-lived sessions.

## 3. Kernel primitives (contract)

| Primitive | Role |
|---|---|
| `userfaultfd(2)` | create fault-notification channel (`O_CLOEXEC \| O_NONBLOCK`) |
| `UFFDIO_REGISTER` (MODE_MISSING) | mark VMA ranges as "pages not present — ask me" |
| `epoll` on uffd fd | async fault pump without blocking other work |
| `struct uffd_msg` | carries `address`, flags, reason (`UFFD_EVENT_PAGEFAULT`) |
| `ioctl(UFFDIO_COPY)` | atomically install a page + wake the blocked thread |
| `MADV_DONTNEED` | drop backing pages → simulate "not yet transferred" |
| CRIU images | authoritative source of registers, VMAs, and page contents |

Failure semantics: every syscall error is mapped to an explicit errno
(`ENOENT` unknown range, `EINVAL` misaligned/unregistered, `EAGAIN` non-blocking queue full,
`ENOMEM` injection buffer exhausted). No blind casts; all addresses page-aligned.

## 4. Performance model & budgets (target < 50 ms activation)

- skeleton transfer: ≤ 1 RTT, tens of KB → ~1–5 ms LAN
- restore + first schedule: ~5–15 ms
- steady-state page service: p99 < 200 µs/page intra-host, bounded by RTT inter-host
- mitigations for cold-touch bursts: prefetch windows, batched UFFDIO_COPY,
  background hydration thread that walks the heap while the process serves traffic.

## 5. Security model

- `vm.unprivileged_userfaultfd` sysctl considerations; prefer CAP_SYS_PTRACE-scoped helper.
- mTLS between orchestrator/PF-daemon/page-server; per-session tokens bound to VMA ranges.
- Page integrity: xxhash64 per 4 KB chunk end-to-end; CRC fallback for debug builds.

## 6. Test strategy

1. **Self-fault MVP (Phase 1)** — `mvp/`: proves trap/inject mechanics.
2. **Two-process loopback** — Phase 2: page server in one process, restorer
   in another over TCP; adaptive prefetch ring; A/B eager harness.
3. **Real-process scale-out** — Phase 3: an app-owned checkpoint/resume
   lifecycle (SIGUSR2 → ISIM image → lazy resume with seq continuity).
   This path is kernel-portable and runs everywhere, including inside
   Docker Desktop on Windows dev machines.
4. **CRIU-backed migration** — same flow with CRIU-produced images on
   native Linux (blocked only under docker-desktop's VM kernel today);
   `analyze.c` already splits/quantifies real CRIU image directories.
5. **Chaos/perf harness** — packet loss, jitter, fault storms;
   activation p50/p99 and total-hydration time vs eager baseline.
