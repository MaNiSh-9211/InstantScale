# 0003: Prefetch ring uses full-scan tag lookup, never index-probe lookup

## Status: Accepted

## Context
Prefetched pages land in a FIFO ring (`slot = cursor++ % SLOTS`). Faults
arrive in arbitrary order. A "direct-mapped" probe (`idx % SLOTS`,
`idx % SLOTS + 1`) missed pages whose storage slot came from the rotating
cursor — and a miss on an already-consumed prefetch made the restorer wait
forever for a response that had already been processed. This was an observed
deadlock (fixed in commit series around hammer.sh), reproduced as
`attempt N: rc=124` hangs.

## Decision
`ring_lookup` performs a **full scan** of validity flags + tags.
SLOTS=128–256 boolean checks cost ~nothing against a network RTT.

## Consequences
Easier: correctness is independent of arrival order and eviction pattern.
Harder: O(SLOTS) per fault; irrelevant at network speeds. The deeper rule
codified from this bug: *any* miss path must end in progress (see ADR-0005),
never in waiting on data already consumed.
