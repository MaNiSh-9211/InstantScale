# 0010: Adaptive lookahead window with bounded growth/shrink

## Status: Accepted

## Context
Fixed prefetch depth is wrong everywhere: too small on sequential sweeps
(every K-th touch still pays an RTT), too large under random access (wasted
bandwidth and ring churn). The controller also had two real bugs: a growth
condition demanding zero misses (unsatisfiable — every window boundary is a
miss by construction) and, after speculative run-merging landed, ring-hits
disappeared so the old hit-rate signal starved the controller.

## Decision
Every ≥32 samples, compare misses to samples: `misses*4 <= total` → double
lookahead (cap 32); `misses*2 > total` → halve (floor 1). Speculatively-
installed pages count as hits — they are exactly the pages that will never
fault. `last_fault_idx` advances past merged runs so piggyback requests stay
ahead of execution.

## Consequences
Easier: sequential sweeps converge to ~1 fault per ≤32 pages; random strides
settle at minimal waste. Harder: controller state must be updated from every
install path (ring hit, merged run) or it silently mis-tunes; covered by the
64 MB battery regression (`net` faults drop to hundreds).
