# 0009: Windows-first development via privileged Docker containers

## Status: Accepted

## Context
The product is Linux-only (userfaultfd, CRIU), but the primary developer
machine is Windows with Docker Desktop (WSL2 backend). Requiring a Linux
workstation would slow iteration; dual-booting fragments the loop.

## Decision
Every test path runs inside privileged `iscale-devel` / `gcc:14` containers
(bind-mounted repo, bash entry points). A single PowerShell runner,
`iscale.ps1`, exposes phases (`mvp|p2|p3|mh|hammer|all`), auto-starts
Docker Desktop, and builds the toolchain image. CI mirrors the same commands
on native ubuntu-24.04 runners. Known limitation: docker-desktop's VM kernel
blocks CRIU syscalls, so CRIU flows run only in CI/native Linux.

## Consequences
Easier: zero-friction verification from Windows; identical commands locally
and in CI; artifacts from failed cloud runs are downloadable for local
inspection. Harder: `--privileged` is required (default seccomp blocks
userfaultfd) — acceptable for a dev/test boundary; production guidance keeps
the daemon behind explicit capability grants instead.
