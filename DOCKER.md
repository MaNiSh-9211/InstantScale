# Docker / container operations

All commands assume the repository root. Windows PowerShell examples are
primary; bash equivalents are listed per section. The toolchain image is
`iscale-devel` (gcc 14 + CRIU v4.1 source-build capability), built from
[`.dev/Dockerfile`](.dev/Dockerfile).

Privileged mode is **required**: Docker's default seccomp profile blocks
`userfaultfd` (ADR-0009).

## One-command stack (multi-host migration)

| Action | Windows PowerShell | Linux / macOS bash |
|---|---|---|
| Up + run target | `docker compose up --build -d; docker compose logs -f target` | same |
| Down | `docker compose down` | same |
| Status | `docker compose ps` | same |

Compose services: `source` (`iscale-src`: warm app → checkpoint → pageserver
on 46100) and `target` (lazy resume over the bridge). Heartbeats and the
checkpoint appear under `./artifacts/`.

## Per-phase suites via the runner

| Suite | PowerShell | bash |
|---|---|---|
| Phases 1–4 full matrix | `powershell -ExecutionPolicy Bypass -File iscale.ps1 all` | `make test-mvp test-p2 test-p3` |
| Phase 1 kernel prototype | `iscale.ps1 mvp` | `make -C mvp clean all && ./mvp/uffd_selffault` |
| Phase 2 LAZY-vs-EAGER demo | `iscale.ps1 p2` | `make -C phase2 clean all && bash phase2/demo.sh` |
| Phase 3 lifecycle battery | `iscale.ps1 p3` | `bash phase3/battery.sh` |
| Two-host migration | `iscale.ps1 mh` | see compose above or `phase4/multihost.ps1` |
| Stress hammer | `iscale.ps1 hammer` | `bash phase2/hammer.sh 64 10` |

## Manual build & run (per binary, inside a container)

```powershell
# build everything
docker run --rm --privileged -v "${PWD}:/src" -w /src iscale-devel `
  bash -c 'make -C mvp clean all && make -C phase2 clean all && make -C phase3 clean all'

# run pageserver as a named host on the bridge (terminal 1)
docker run -d --name iscale-src --privileged --network iscale-net `
  -v "${PWD}:/src" -w /src iscale-devel `
  ./phase2/pageserver --port 46100 --image /src/artifacts/app.isim

# run restorer against it (terminal 2)
docker run --rm --privileged --network iscale-net -v "${PWD}:/src" -w /src `
  iscale-devel ./phase3/demo_app --resume-lazy-img artifacts/app.isim `
  --host iscale-src --port 46100
```

```bash
# bash equivalents
docker run --rm --privileged -v "$PWD:/src" -w /src iscale-devel \
  bash -c 'make -C mvp clean all && make -C phase2 clean all && make -C phase3 clean all'

docker run -d --name iscale-src --privileged --network iscale-net \
  -v "$PWD:/src" -w /src iscale-devel \
  ./phase2/pageserver --port 46100 --image /src/artifacts/app.isim

docker run --rm --privileged --network iscale-net -v "$PWD:/src" -w /src \
  iscale-devel ./phase3/demo_app --resume-lazy-img artifacts/app.isim \
  --host iscale-src --port 46100
```

## Logs

| What | Command |
|---|---|
| compose target stream | `docker compose logs -f target` |
| compose source stream | `docker compose logs -f source` |
| orchestrator workdir logs | last `RESULT ... workdir=` line names the mktemp dir; inspect `*.log` inside |
| fan-out replica logs | `/tmp/iscaleN.*/r*.log` inside the container |
| CI diagnostics | Actions → run → artifact `criu-lazy-logs` (criu.yml) |

## Health probes

| Component | Probe (inside its network/container) |
|---|---|
| pageserver listening | `bash -c 'exec 3<>/dev/tcp/127.0.0.1/46100'` → exit 0 |
| checkpoint produced | `[ -f artifacts/checkpoint.ready ]` and non-empty `artifacts/app.isim` |
| restored instance alive & hydrated | heartbeat file gains `HB seq=` lines; on SIGTERM prints `FINAL digest=0x…` matching source |
| CRIU present | `criu --version` exits 0 |

## Observability verification (30-second smoke)

```powershell
powershell -File iscale.ps1 p3     # expect: RESULT mode=lazy activation_ms < 5, FINAL digest=0x… present
```

The battery prints: activation time, first-touch latency histogram (8 buckets),
network-fault count, prefetch-cache hits, merged-install stats
(`merged=N merge_runs=M`), hydration MB/s, and the end-to-end CRC verdict.
Any `digest mismatch` is fatal by design — treat as a bug report.
