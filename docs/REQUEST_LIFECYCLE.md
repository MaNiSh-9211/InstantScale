# Request lifecycle: one page fault under lazy resume

The representative "request" in InstantScale is not HTTP — it is a **missing
page**: the restored application touches memory that has not arrived yet.
Below is the full journey of page `idx = 4096` (offset 16 MiB) during the
64 MB battery run, with costs measured from that run
(`phase3/battery.sh`, WSL2 5.15, loopback).

## Participants

```mermaid
sequenceDiagram
    autonumber
    participant App as App thread (RUNNING)
    participant K as Kernel (uffd queue)
    participant D as PF-daemon (epoll)
    participant PS as pageserver (TCP :46100)

    Note over App: uptime t, first touch of idx 4096
    App->>K: load → PTE absent → sleep in handle_userfault
    K->>D: uffd_msg{PAGEFAULT, addr} (queued instantly)
    D->>D: pend[] += {idx, aligned_addr, t0}
    D->>PS: REQ frame {hdr, offsets...} (batched w/ piggyback)
    PS->>PS: bounds check → memcpy from mmap'd image
    PS-->>D: RSP frame {page_hdr+4KB}...
    D->>D: run-merge consecutive pages → staging buffer
    D->>K: ioctl(UFFDIO_COPY) ranged install
    K-->>App: wake; the original load completes
    App->>App: continue heartbeat loop
```

## Timeline with measured costs

| # | Step | Layer | Cost (this run) | Notes |
|---|---|---|---|---|
| 0 | connect + META handshake + register region | control plane | ≈ 0.30 ms cumulative to `RUNNING` | activation gate; only KBs moved |
| 1 | app executes `*(volatile u64*)(base+off)` | app | ~ns | compiler-volatile read |
| 2 | kernel page-walk misses → park thread | kernel | sub-µs to enqueue | `wchan=handle_userfault` observed |
| 3 | daemon epoll returns EPOLLIN(uffd), reads `uffd_msg` | daemon | µs-class | non-blocking drain loop |
| 4 | ring lookup miss → `pend[]` append + sendq append | daemon | ns–µs | dedup by state machine |
| 5 | adaptive lookahead appends successors N+1..N+k | daemon policy | ns | k ∈ [1..32], hit-rate driven |
| 6 | staged frame sent (`send`, MSG_NOSIGNAL, NODELAY) | wire | µs-class | partial-write resume armed |
| 7 | server parses, bounds-checks, memcpys from mmap'd image, stages reply | pageserver | <50–100 µs bucket for most faults | zero pread; image is mapped |
| 8 | response reassembled; consecutive run detected (waiter + successors) | daemon | µs | run length up to batch cap |
| 9 | single ranged `UFFDIO_COPY(dst, staging, k·4096)` | kernel | µs-class per syscall | ~31 pages/syscall typical |
| 10 | kernel maps pages, wakes ALL waiters in range | kernel | — | app resumes exactly at its load |
| 11 | latency sample recorded into histogram | daemon | — | buckets in report |

Aggregate for the full 64 MB sweep (same run): 16,384 pages hydrated via
**440** network-fault batches and **440** ranged installs covering
**13,703** speculatively-installed pages; overall readiness **146 ms**
(**439 MB/s**); final CRC identical to source checkpoint.

## Failure paths

| Breaks | Detected by | Response | Recovery |
|---|---|---|---|
| pageserver killed mid-request | `recv()==0` → `ECONNRESET` | daemon `is_die` (process exits with exact errno) | orchestrator-level restart of the pair |
| socket backpressure (EAGAIN on send) | send loop short write | stage remainder, resume on EPOLLOUT | transparent |
| ioctl UFFDIO_COPY returns EAGAIN/EINTR | errno / `.copy == -EAGAIN` | retry with cursor advance on partial copy | transparent |
| prefetched page evicted before its fault | fault finds no ring entry, state==REQ | idempotent re-request (ADR-0005) | one extra RTT for that page |
| duplicate response arrives late | `pend_find` miss | parked into ring (immutable data) | none |
| peer sends OOB offset / bad magic | header+bounds validation both sides | drop connection / `EPROTO` fail-fast | misbehaving peer isolated |
| response buffer would overflow | cap check before recv | `EPROTO` fail-fast | protocol bug surfaced immediately |
| stop signal during hydration | eventfd EPOLLIN | daemon drains, joins cleanly | graceful teardown |

## Cache behaviour (mapped to classic patterns)

| Classic pattern | InstantScale equivalent | Mechanism |
|---|---|---|
| cache stampede / herd | one request per missing page | `pend[]` + `state[]` guarantee a single in-flight request per idx (single-flight) |
| cache penetration | fault for never-requested page | self-healing re-request path still resolves it (never a silent hang) |
| cache penetration (bogus key) | OOB/misaligned offset | rejected at server and client; connection dropped |
| avalanche (mass simultaneous misses) | sequential sweep burst | adaptive lookahead widens window (≤32) and run-merging coalesces installs (~31 pages/ioctl) |
| poisoning | corrupted payload | rolling CRC32 over every page verified against seeder/checkpoint digest; mismatch is fatal (`EPROTO`) |
| eviction storm | ring pressure | FIFO overwrite is safe: worst case falls back to demand fetch (re-request) |
