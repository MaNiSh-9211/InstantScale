# 0005: Self-healing re-request for evicted prefetches

## Status: Accepted

## Context
A prefetched page can be evicted from the ring before its fault arrives.
Naive state tracking (`state == REQ` means "request in flight") then waits
for a response that was already consumed — an infinite hang. Two real
deadlocks traced to this class: ring lookup/placement skew (ADR-0003) and
random-stride eviction.

## Decision
On a fault with no ring hit: if `state == IDLE`, queue a request as usual.
If `state == REQ`, **queue the request again** (bounded by the batch cap).
Pages are immutable within a session, so duplicate responses are harmless:
the first resolves the waiter; a late twin simply parks into the ring.

## Consequences
Easier: no eviction path can deadlock; `hammer.sh` runs 10–20 consecutive
resumes clean. Harder: transient duplicate traffic under adversarial access
patterns; accepted because immutability makes dedup purely an optimization,
not a correctness requirement.
