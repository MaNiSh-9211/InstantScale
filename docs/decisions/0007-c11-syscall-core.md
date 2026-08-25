# 0007: C11 + raw syscalls for the core; scripts for orchestration

## Status: Accepted

## Context
The system's hot path is a contract between three parties: our structs, the
kernel's ABI (`struct uffdio_copy`, `struct uffdio_register`, wire frames),
and latency. GC, async runtimes, or abstraction layers would each add jitter
or layout drift.

## Decision
Phase cores are C11 (`-Wall -Wextra -Wpedantic`) against raw syscalls and
fixed-width wire structs. Orchestration lives in bash (Linux flows) and
PowerShell (Windows entry point); Rust remains the planned home for the
production PF-Daemon once formats stabilize.

## Consequences
Easier: struct layouts are auditable byte-for-byte; every syscall failure is
an explicit errno path; builds are one gcc line anywhere. Harder: manual
memory/buffer management — mitigated by caps, bounds checks, and the
fail-fast error contract; string handling exists only in orchestrators.
