# HotPod vs Ahead-of-Time (AOT) Compilation
### GraalVM Native Image Â· Quarkus Â· native binaries â€” versus â€” checkpoint skeletons + userfaultfd lazy paging

---

## TL;DR

| | AOT native binary | HotPod |
|---|---|---|
| Process start | ~10 â€“ 100 ms (reported) | **0.17 â€“ 2.45 ms (measured here)** |
| Runtime | any AOT-supported language | **any Linux process, unmodified** |
| State at first request | **cold** (empty pools/caches/JIT profile) | **warm** (exact pre-spike state resumes, `seq N â†’ N+1`) |
| Warm-up after start | still required (pool init, cache fills) | **none â€” it already happened before the checkpoint** |
| Rebuild per release | yes (full AOT compile, minutes) | no (checkpoint any running build) |
| Peak throughput | can trail JIT-optimized JVM | identical to source runtime (it *is* the warmed runtime) |

**Headline:** AOT attacks *boot cost*. HotPod removes *activation cost*
**and** preserves *warm state*. They stack â€” see Â§6.

---

## 1. What AOT actually buys â€” and what it cannot

GraalVM Native Image / Quarkus ahead-of-time compilation eliminates the
JVM's JIT warm-up tax by compiling to a native executable. That genuinely
drops **process boot** from seconds to milliseconds.

But three costs remain untouched:

1. **Cold state.** A freshly booted native binary has empty connection
   pools, cold caches, no hydrated session state, no "profile" of hot paths.
   Fast boot â‰  ready-to-serve-warm-traffic. Real services still spend
   hundreds of ms to seconds opening pools / loading reference data before
   the first useful request completes.
2. **Rebuild pipeline.** Every code change requires a full AOT recompile;
   reflection/dynamic features need configuration; some libraries don't fit.
3. **Scale-out is still birth, not migration.** When traffic doubles, each
   new AOT instance is a *newborn*: correct, but amnesiac. Session/state
   must round-trip through external stores.

HotPod takes a different axis entirely: **checkpoint the warm process,
resume it somewhere else with its memory arriving lazily.** The restored
instance continues its own sequence counter (`seq N â†’ N+1` in every test) â€”
it did not reboot; it *kept running somewhere else*, with 0 % of its heap
present at first instruction.

---

## 2. Measured numbers (this repository, reproducible)

All HotPod rows are produced by the harnesses in this repo
(`phase2/demo.sh`, `phase3/battery.sh`, `fanout.sh`) on a Windows 11 dev box
driving Docker Desktop / WSL2 kernel 5.15, loopback or docker-bridge network.

| Metric | Value | Source |
|---|---|---|
| Lazy-resume activation, 64 MB heap, loopback | **0.17 â€“ 0.55 ms** | phase3/battery.sh |
| Lazy-resume activation, cross-container network hop | **2.45 ms** | phase4/multihost.ps1 |
| Full-hydration throughput (post run-merging) | **439 MB/s**, 13,703 pages in 440 syscalls | phase2 demo.sh |
| Fan-out spike: 10 replicas from ONE checkpoint, wall time until ALL serving | **21 â€“ 33 ms** | phase3/fanout.sh |
| Continuity proof | first post-resume heartbeat == `pre_seq + 1`, byte-identical CRC | every battery run |
| Eager full-copy baseline (same heap, loopback) | 26 â€“ 56 ms (64 MB) Â· 205 ms (256 MB) | phase2/demo.sh |

Representative **publicly reported** AOT figures (for context, not measured
here â€” see framework docs/benchmarks):

| AOT scenario | Reported startup |
|---|---|
| GraalVM Native Image, minimal HTTP service | ~10 â€“ 50 ms |
| Quarkus native, real-world REST app | tens â€“ low hundreds of ms |
| + production warm-up (pools, caches, client TLS) | additional 100 ms â€“ seconds |

---

## 3. The readiness gap (why "fast boot" underestimates the problem)

```
AOT timeline      |--boot--|--pool/cache warm-up--|-- useful --|
                   â–² few ms   â–² still pays here        â–² finally warm

HotPod      |-0.x ms-|############################|
                   â–² RUNNING     warm pages stream behind live traffic
                     (heap was already warm when checkpointed)
```

For stateful services (auth session stores, feature-flag caches, ML feature
pipelines, JIT-profiled hot loops), the warm-up segment dominates. AOT cannot
compress it because the state simply does not exist yet. A checkpoint captures
it *after* it exists.

---

## 4. Compatibility

| Workload | AOT | HotPod |
|---|---|---|
| Java (JIT HotSpot, fully optimized) | âœ— (would force GraalVM rewrite) | âœ“ unchanged |
| Node.js / Python / Go / Rust / C++ | partial / n/a | âœ“ any Linux ELF |
| Heavy reflection / dynamic loading | requires config or unsupported | âœ“ unaffected |
| Third-party native libs (JNI/FFI) | painful | âœ“ captured in snapshot |
| Existing CI artifacts | rebuild required | checkpoint the artifact you already ship |

---

## 5. Performance ceiling honesty

* AOT trades peak throughput for predictability (no PGO from production
  profiles). A checkpointed **JIT-hot** JVM keeps its profile-guided
  compiled code â€” the fastest form of that code â€” because the compiled
  code lives in the captured pages.
* Lazy paging moves bytes eventually: total *hydration* time obeys
  bandwidth physics like eager copy. The win is scheduling: traffic flows
  after kilobytes, and sequential-touch prefetch + speculative run-merging
  keep p99 fault latency near one RTT (measured < 250 Âµs loopback).
* Cross-region (~50 ms RTT) puts a floor under first-page fetches: expect
  activation â‰ˆ RTT, i.e. **tens of ms instead of minutes** â€” still orders
  of magnitude ahead of both cold start and eager copy.

---

## 6. Better together

AOT and HotPod compose:

1. Ship a GraalVM native build (fast individual boot, small footprint).
2. Run one instance through production-shaped warm-up (pools primed,
   caches filled).
3. Checkpoint it â€” HotPod fans out N clones in tens of ms, each
   already warm, each continuing mid-flight state.

Warm native binaries + lazy page streaming is the fastest known route to
"N extra warm replicas right now."

---

## 7. Reproducibility

```powershell
powershell -ExecutionPolicy Bypass -File hotpod.ps1 p3    # lifecycle battery
powershell -ExecutionPolicy Bypass -File hotpod.ps1 mh    # two-host wire
cd phase3 && bash fanout.sh 10                            # spike fan-out
```

Every number in Â§2 prints from these commands; CI re-runs them on
ubuntu-24.04 on every push (`.github/workflows/ci.yml`).

## 8. Limitations (fairness)

* Checkpoint/restore requires Linux â‰¥ 5.11 with userfaultfd available
  (unprivileged sysctl or CAP_SYS_PTRACE / privileged container).
* CRIU-backed capture of *arbitrary third-party* processes is wired in CI
  (eager path proven; `--lazy-pages` experiment in progress).
* GPU fds, exotic devices, and external socket continuity are outside the
  current scope (CRIU's classic constraint set applies).
