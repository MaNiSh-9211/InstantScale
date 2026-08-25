# InstantScale

[![ci](https://github.com/MaNiSh-9211/InstantScale/actions/workflows/ci.yml/badge.svg)](https://github.com/MaNiSh-9211/InstantScale/actions/workflows/ci.yml)

> **Instant autoscaling.** Activate pre-warmed processes on new hosts in
> **sub-millisecond** time by separating *process activation* from *memory
> transfer* — measured, reproducible, from your Windows desktop.

InstantScale checkpoints a warm application into a compact image, starts the
restored instance **immediately with 0% of its heap present**, and streams
memory pages on demand via the Linux [`userfaultfd`](https://www.kernel.org/doc/html/latest/admin-guide/mm/userfaultfd.html)
subsystem while the process is already serving traffic.

## Why it matters

| Strategy | Time to RUNNING (64 MB heap) |
|---|---|
| Cold start (simulated runtime bootstrap) | ~2,200 ms |
| Eager migration (copy full snapshot first) | ~26–28 ms |
| **Lazy migration (InstantScale)** | **0.17 – 0.55 ms** |

That is **~4,000–12,000× faster than cold start** and **~50–160× faster than
eager copy** — and the restored process *continues* its monotonic sequence
number (`seq N → N+1`) with byte-identical memory digests: it resumed, it did
not restart.

> **vs GraalVM/Quarkus AOT?** AOT removes JIT boot tax but still births a
> *cold* instance (empty pools/caches, rebuild per release). InstantScale
> resumes an already-warm process in sub-ms with full state.
> Full analysis: [docs/AOT_COMPARISON.md](docs/AOT_COMPARISON.md)

## Measured results (Windows dev box → Docker Desktop → WSL2 5.15 kernel)

| Test | Result |
|---|---|
| Phase 1 kernel trap/inject | PASS · first read issued **0.001 ms** after wipe |
| Phase 2 lazy vs eager A/B (256 MB) | RUNNING in **0.34 ms vs 205 ms** (611×) |
| Phase 2 prefetch efficiency | **96.9 %** of pages served with zero network RTT |
| Phase 3 lifecycle (64 MB heap) | cold 2187 ms · eager 25.8 ms · **lazy 0.55 ms** |
| Phase 3 stability | lazy resume **5/5 runs** clean, continuity + digest OK |
| Phase 4 **multi-host** (two containers, real network) | **activated in 2.45 ms**, seq continuity + CRC match |

## Testing on Windows (primary workflow)

Everything targets Linux syscalls, but the whole stack runs on your Windows
machine through Docker Desktop privileged containers:

```powershell
powershell -ExecutionPolicy Bypass -File iscale.ps1 all     # every phase, green in one shot
powershell -ExecutionPolicy Bypass -File iscale.ps1 mvp     # Phase 1: kernel mechanics proof
powershell -ExecutionPolicy Bypass -File iscale.ps1 p2      # Phase 2: LAZY vs EAGER wire demo
powershell -ExecutionPolicy Bypass -File iscale.ps1 p3      # Phase 3: scale-out lifecycle battery
powershell -ExecutionPolicy Bypass -File iscale.ps1 mh      # Phase 4: TWO-HOST migration over a real network
powershell -ExecutionPolicy Bypass -File iscale.ps1 hammer  # deadlock/stress suite
```

The runner auto-starts Docker Desktop if needed and builds the `iscale-devel`
image (gcc + CRIU) on first use.

## Repository layout

```
InstantScale/
├── iscale.ps1              # ONE-COMMAND Windows runner for every phase
├── Makefile                # CI/Linux entry points (same tests)
├── INSTRUCTIONS.local.md   # project brief (git-ignored, local only)
├── .dev/Dockerfile         # devel image: gcc + CRIU + python tooling
├── docs/
│   ├── ARCHITECTURE.md     # system design & kernel mechanics
│   └── ROADMAP.md          # phased delivery plan + status
├── mvp/                    # Phase 1: self-faulting prototype (C)
│   └── uffd_selffault.c    # trap/inject mechanics, fully annotated
├── phase2/                 # Phase 2: split-process page service over TCP
│   ├── common.h            # ISIM image format + wire protocol + helpers
│   ├── seeder.c            # deterministic warm-heap image builder
│   ├── pageserver.c        # epoll TCP page server (source host)
│   ├── restorer.c          # uffd client: batching + adaptive prefetch ring
│   ├── demo.sh             # LAZY vs EAGER comparison harness
│   └── hammer.sh           # stability/deadlock stress (10×)
└── phase3/                 # Phase 3: real-process instant scale-out
    ├── demo_app.c          # stateful service w/ full InstantScale lifecycle
    ├── puller.h            # condensed PF-daemon used at lazy-resume
    ├── analyze.c           # CRIU-image skeleton/bulk splitter & reporter
    ├── iscale.sh           # orchestrator: cold/eager/lazy migrations
    └── battery.sh          # validation battery (stability included)
└── phase4/                 # Phase 4: multi-host live migration
    ├── source_node.sh      # Host A: warm -> checkpoint -> serve pages
    ├── target_node.sh      # Host B: lazy resume over the wire
    └── multihost.ps1       # two-container orchestrator (docker bridge)
```

## How the magic works

1. A warm instance snapshots itself (`SIGUSR2`) into an ISIM image:
   `[header][heap pages][tail {seq, uptime}]` — kilobytes of metadata plus
   bulk pages, deliberately separated.
2. The target maps an **empty** region and arms `userfaultfd(MISSING)` —
   activation requires only the header/tail (~100 bytes).
3. Execution resumes instantly. Every touched page traps into the kernel,
   wakes the PF-Daemon (epoll), which fetches exactly that 4 KB page over
   TCP and installs it atomically with `ioctl(UFFDIO_COPY)` — the blocked
   instruction simply completes. No SIGSEGV, no polling.
4. Sequential-touch detection + an adaptive prefetch window mean most pages
   are already local when referenced (96.9 % RTT-free in our sweeps).

## Requirements

- Windows 10/11 + Docker Desktop (WSL2 backend), or any Linux host with
  kernel ≥ 5.11 and gcc/make.
- No WSL distro setup needed — containers provide the Linux environment.

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md). Next: CRIU integration on native
Linux CI (blocked only inside docker-desktop's VM kernel today), then
multi-host orchestration and Kubernetes CRD wiring.

## License

MIT — see [LICENSE](LICENSE).
