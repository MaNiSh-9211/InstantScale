# InstantScale — Roadmap

Goal: autoscaling that is **instant**, i.e. activation < 50 ms, measured end-to-end
(checkpoint → process serving traffic on target), orders of magnitude faster than cold start.

## Phase 0 — Environment & Foundations (day 1)
- [x] Repo, docs, brief (git-ignored local file)
- [ ] Linux dev environment: WSL2 Ubuntu or Docker-based toolchain
- [ ] CI: build + run MVP on Linux runners (kernel ≥ 5.11)
- [ ] Decide kernel floor: require `userfaultfd` unprivileged (5.11+) or set sysctl

## Phase 1 — MVP: single-process self-faulting prototype (week 1)
Prove the kernel mechanics. No network, no CRIU.
- [x] Design + annotated C implementation (`mvp/uffd_selffault.c`)
- [ ] Build & run on Linux; capture logs
- [ ] Acceptance:
  - [ ] main thread resumes < 20 ms after `madvise(MADV_DONTNEED)`
  - [ ] zero SIGSEGV; deterministic trap on first touch
  - [ ] handler prints fault addresses matching the mmap region
  - [ ] `UFFDIO_COPY` injects payload; loop continues correctly
- [ ] Stretch: Rust port using `nix` (this becomes the PF-Daemon core)

## Phase 2 — Split-process page service (weeks 2–3) ✅ COMPLETE
Simulates the wire without CRIU yet.
- [x] Page-server process serves pages from a checkpoint image over TCP
      (`phase2/pageserver.c`: mmap'd image, epoll, partial-write safe)
- [x] Wire protocol: framed `META_REQ/PAGES_REQ/BYE` + offset addressing
      (source/target ASLR-independent), per-page status + rolling CRC32
- [x] Restorer registers uffd *before* touching memory; instant activation
- [x] Request batching (multi-page frames, cap 64)
- [x] **Adaptive prefetch ring** — lookahead auto-tunes 1→32 pages from the
      observed hit rate; 96.9 % of faults served with zero network RTT
- [x] Self-healing re-request path (ring-evicted prefetches can't deadlock)
- [x] Metrics: activation time, per-fault latency histogram (p99 < 250 µs
      loopback), hydration throughput, cache hit rate, digest verification
- [x] A/B harness: `IS_EAGER=1` same binary = traditional full-copy migration;
      measured **229–452× faster to RUNNING** depending on heap size
- [x] Stability hammer (`hammer.sh`): 10/10 clean passes after fixing two
      real deadlocks (epoll data-union misuse; ring lookup/placement skew)

## Phase 3 — Real-process instant scale-out (weeks 4–6) ✅ CORE COMPLETE
Goal: migrate a real, stateful process with proof of continuity.

**Delivered (app-owned lifecycle — works everywhere incl. Windows/Docker):**
- [x] `demo_app`: stateful warm service; cold-bootstrap tax is configurable
      (`--init-ms`), heap self-verifies every heartbeat
- [x] Self-checkpointing (`SIGUSR2` → ISIM image with `[hdr][pages][tail]`,
      tail carries seq + uptime → the continuity anchors)
- [x] Eager resume (`--resume`): full heap load before heartbeat #1
- [x] Lazy resume (`--resume-lazy-img`): uffd armed before first touch,
      pages streamed from a phase2 pageserver behind live execution
- [x] Continuity proof: first post-resume HB == pre_seq+1 on every run;
      SIGTERM triggers full-heap CRC sweep ("FINAL digest=")
- [x] `battery.sh`: cold 2187ms / eager 25.8ms / **lazy 0.55ms** @64MB;
      lazy stability 5/5 runs (0.118–0.226ms @32MB); digests match eager

**Measured (Windows box → Docker Desktop → WSL2 5.15):**

| mode  | 64 MB activation | notes |
|-------|------------------|-------|
| cold  | ~2190 ms         | simulated runtime bootstrap |
| eager | 25.8 ms          | all bytes before start |
| lazy  | **0.171–0.551 ms** | 0% pages present at start |

**CRIU integration status:**
- [x] `analyze.c` splits/quantifies CRIU image dirs (skeleton vs bulk ratio,
      physical split into `-split/skeleton|pages`)
- [x] Devel image ships CRIU 4.1.1 (`criu check` = "Looks good")
- [ ] Live CRIU dump/restore: **blocked inside docker-desktop's VM kernel** —
      `PTRACE_SECCOMP_GET_FILTER` returns EPERM even for `sleep`
      (probe: phase3/probe_criu.sh history). Kernel configs report
      CHECKPOINT_RESTORE=y, so this is a docker-desktop/WSL2 quirk.
      Action: run CRIU flows on native Linux CI (GitHub Actions ubuntu-24.04)
      or bare-metal/WSL2 custom kernel; orchestrator flags already drafted in
      iscale.sh git history.

## Phase 4 — Distributed live-migration (weeks 7–10) 🚧 IN PROGRESS
- [x] **Multi-host demo over a real network**: `phase4/multihost.ps1` boots two
      containers ("hosts") on a docker bridge; Host A checkpoints+serves,
      Host B lazy-resumes across the wire.
      Measured: **activation 2.45 ms cross-network**, seq continuity PASS,
      full-heap CRC match, orchestrator exit-code enforced.
      Run it: `powershell -File iscale.ps1 mh`
- [ ] Prefetch engine v2 (heap-walk hydration thread alongside sequential detector)
- [ ] Batched multi-page UFFDIO_COPY; optional io_uring transport backend
- [ ] Kubernetes prototype: CRD `InstantScaleSession`, warm pool of
      checkpoints ("seed pods") per service class

**CI (GitHub Actions, ubuntu-24.04 native kernel):**
- [x] Full matrix green: Phase 1 prototype, Phase 2 demo+hammer,
      Phase 3 battery, **fan-out spike ×10 replicas**
- [x] Badge + topics on repo; runs on every push/PR
- [x] **CRIU probe SOLVED**: criu-native job builds CRIU v4.1 from source
      (`setcap cap_checkpoint_restore`, yama ptrace_scope=0, `--unprivileged`)
      and passes: sleep dump/restore roundtrip **+ demo_app migration with
      heartbeat-seq continuity verified in CI**
- [ ] CRIU `--lazy-pages` roundtrip: experiment wired, not yet passing
      (continue-on-error); debug lres.log/ldump.log next session

## Phase 5 — Productization
- [ ] Container runtime integrations (runc shim, containerd snapshotter)
- [ ] Dirty-page re-sync for repeated scale events (WP-mode uffd on source)
- [ ] Multi-arch notes (x86_64 first, aarch64 next)
- [ ] Paper/blogpost: "Autoscaling at memory-access speed" + reproducible benchmarks

## Risk register
| Risk | Mitigation |
|---|---|
| Fault storms on huge heaps saturate single uffd queue | batching, multiple handler threads, io_uring |
| First-touch working set larger than RTT budget | prefetch heuristics, background hydrator |
| Seccomp blocks userfaultfd in containers | document profile patches; privileged sidecar mode |
| CRIU image format drift | pin CRIU version; versioned parser with fuzz tests |
