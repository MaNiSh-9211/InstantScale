# 0004: Speculative run-merging — one ranged UFFDIO_COPY per consecutive run

## Status: Accepted

## Context
Per-page installation costs one ioctl plus one future minor fault per page.
When responses arrive for consecutive offsets (the sequential-sweep case),
most of those faults and syscalls are avoidable.

## Decision
In `process_responses`, stage each frame's pages, group them into maximal
consecutive runs, anchor at the first pending waiter, copy the whole run into
a contiguous staging buffer, and issue a **single ranged** `UFFDIO_COPY`
covering waiter + successors. Successor pages become PRESENT before their
first touch. Speculatively-installed pages count as prefetch hits for the
adaptive-lookahead controller, and `last_fault_idx` advances past merged runs
so the piggyback window stays ahead of execution.

## Consequences
Easier: measured hydration readiness 530 ms → **146 ms** (64 MB), throughput
107 → **439 MB/s**, ~31 pages per syscall. Harder: two new invariants —
(a) install strictly before consuming the frame from `ibuf` (staged pointers
reference frame bytes; an early `memmove` installed shifted garbage and broke
digests), (b) hit/miss accounting must treat speculative installs as hits or
the lookahead controller starves itself. Multi-threaded waiters inside one
run resolve together because the kernel wakes every sleeper in the range.
