# Changelog

All notable changes. Reverse chronological.
ADRs referenced as `docs/decisions/NNNN-*.md`.

## [0.4.1] - 2026-08-25 - renamed to HotPod

### Changed
- Project renamed **InstantScale to HotPod**: GitHub repo, README branding, runner iscale.ps1 to hotpod.ps1, orchestrator phase3/iscale.sh to phase3/hotpod.sh, containers hotpod-src / network hotpod-net / image hotpod-devel, and all code banners + docs.
- Added project logo (assets/logo.png) and centered README banner.

## [0.4.0] - 2026-08-25 â€” CRIU unblocked on native Linux

### Added
- `criu-native` CI workflow: builds CRIU v4.1 from source on ubuntu-24.04;
  sleep dump/restore roundtrip passes; **demo_app migration with heartbeat-seq
  continuity verified in cloud CI** (`.github/workflows/criu.yml`).
- Lazy-pages experiment wired end-to-end (page-server + lazy dump/restore)
  with race-guard (restore waits for page-server metadata), rc-guarded calls,
  log-tail output, step-summary block, and downloadable diagnostics artifact.
- Capability recipe for hosted runners: `setcap cap_checkpoint_restore`,
  `kernel.yama.ptrace_scope=0`, `--unprivileged`.

### Changed
- CI `ci.yml`: enables Ubuntu universe pocket; CRIU probe degrades to a skip
  when the package is absent.

## [0.3.2] - 2026-08-25 â€” speculative run-merging (hydration invention)

### Added
- **Run-merging installer** (ADR-0004): response pages are grouped into
  maximal consecutive runs and installed via one ranged
  `ioctl(UFFDIO_COPY)` covering the faulting page *and* already-fetched
  successors. Measured on the 64 MB sweep: readiness 530 â†’ **146 ms**,
  throughput 107 â†’ **439 MB/s**, ~31 pages per syscall.

### Fixed
- Install-before-consume ordering bug: staged pointers referenced the socket
  reassembly buffer across its `memmove`, installing shifted bytes whenever
  frames arrived back-to-back (digest mismatch, caught by battery).
- Adaptive controller signal: speculative installs now count as prefetch hits;
  `last_fault_idx` advances past merged runs so piggyback windows stay ahead
  of execution (ADR-0010).

## [0.3.1] - 2026-08-25 â€” multi-host migration

### Added
- Phase 4 two-host demo: `phase4/multihost.ps1` + `source_node.sh` /
  `target_node.sh`; two containers on docker bridge `hotpod-net`.
- Measured: cross-network activation **2.45 ms**, continuity PASS,
  uniform final digest.
- Fan-out spike (`fanout.sh`): N replicas from one checkpoint concurrently â€”
  10 replicas all RUNNING in 21â€“33 ms wall, continuity 10/10.

### Fixed
- `is_tcp_connect` resolves hostnames via `getaddrinfo`
  (docker DNS names like `hotpod-src` previously failed `inet_pton`).

## [0.3.0] - 2026-08-25 â€” real-process lifecycle

### Added
- `demo_app` full lifecycle: cold-bootstrap tax, deterministic warm heap,
  heartbeats with monotonic seq + O(1) integrity probe, SIGUSR2
  self-checkpoint into ISIM image, eager (`--resume`) and lazy
  (`--resume-lazy-img`) resume paths (ADR-0008).
- Condensed PF-daemon `puller.h` used during lazy-resume.
- `battery.sh`: cold/eager/lazy A/B with continuity assertions
  (cold â‰ˆ 2.19 s Â· eager â‰ˆ 26 ms Â· lazy â‰ˆ 0.55 ms @ 64 MB; lazy stability 5/5).
- `analyze.c`: splits real CRIU image dirs into skeleton vs bulk with ratios.

### Fixed
- Teardown EBADF race replaced by eventfd-driven shutdown (ADR-0006).
- Ring-cache lookup deadlock: full-scan tag lookup replaces probe math;
  self-healing re-request covers evicted prefetches (ADR-0003, ADR-0005).

## [0.2.0] - 2026-08-25 â€” split-process wire

### Added
- ISIM image format + framed TCP protocol (`common.h`): META/PAGES/BYE,
  offset addressing (ADR-0002), per-page status, rolling CRC32 digest.
- epoll TCP `pageserver.c` serving pages from an mmap'd checkpoint image
  with partial-write resume and multi-client support.
- `restorer.c` daemon: userfaultfd registration, request batching (â‰¤64),
  adaptive prefetch ring + lookahead controller, latency histogram,
  LAZY-vs-EAGER harness (`demo.sh`): 256 MB heap RUNNING in 0.34 ms vs
  205 ms eager (611Ã—); prefetch serves 96.9 % of pages with zero RTT.

### Fixed
- epoll listener detection stored fd through the data union then tested
  `ptr == NULL` (segfault on first accept) â€” sentinel-tagged now.

## [0.1.0] - 2026-08-25 â€” kernel mechanics MVP

### Added
- Self-faulting prototype (`mvp/uffd_selffault.c`): mmap warm state â†’
  register MISSING â†’ `MADV_DONTNEED` wipe â†’ background epoll daemon traps
  faults, fabricates pages, installs via `UFFDIO_COPY`.
- Acceptance: first read issued **0.001 ms** after wipe; zero SIGSEGVs;
  all faults served; value + pattern integrity verified.
- Windows runner `hotpod.ps1` (privileged containers), root Makefile,
  docs skeleton, MIT license.
