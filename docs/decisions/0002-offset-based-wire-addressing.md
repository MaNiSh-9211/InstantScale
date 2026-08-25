# 0002: Address pages by region offset on the wire, never by virtual address

## Status: Accepted

## Context
After a real checkpoint/restore the target's ASLR layout will not match the
source's. Any protocol keyed on absolute virtual addresses would need
relocation metadata and would couple the wire to one capture format.

## Decision
All `PAGES_REQ` / page-header offsets are byte offsets within the logical
region (`offset = idx * page_size`). Both sides validate
`offset % ps == 0` and `offset < region_len`.

## Consequences
Easier: source and target map wherever `mmap` lands; seeder/checkpoint
formats stay runtime-agnostic; tests verify integrity via per-page sentinels
(`SENTINEL ^ idx`) independent of address. Harder: multi-region processes
(one image per region) are a future extension — the current lifecycle apps
expose exactly one heap region.
