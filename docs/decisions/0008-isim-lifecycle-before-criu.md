# 0008: App-owned ISIM lifecycle before (and beside) CRIU integration

## Status: Accepted

## Context
CRIU-backed capture of arbitrary processes failed inside docker-desktop's
WSL2 VM kernel (`PTRACE_SECCOMP_GET_FILTER` → EPERM, even for `sleep`;
probe preserved in git history). Waiting on that environment would have
blocked all forward progress.

## Decision
Give first-party lifecycle apps their own capture path: `SIGUSR2` writes an
ISIM image `[is_img_hdr][heap pages][tail_meta{TAIL_MAGIC, seq, uptime_ms}]`
and exits; `--resume` / `--resume-lazy-img` restore from it through the same
wire the future CRIU bridge will use. CRIU itself is built from source in a
dedicated CI job on native Linux runners, where eager dump→restore with
heartbeat-seq continuity already passes.

## Consequences
Easier: every phase is testable today on Windows; the lazy wire, prefetcher,
and merger are validated by real processes rather than mocks. Harder:
third-party runtimes need the CRIU path (in progress); two capture formats
coexist until the CRIU parser fully replaces ISIM for external apps.
