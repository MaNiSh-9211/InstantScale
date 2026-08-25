# Diagrams — visual reference

One diagram per concept. All render natively on GitHub.

## 1. Security defense-in-depth

```mermaid
graph TD
    L1["Layer 1 - container boundary<br/>privileged only where userfaultfd needed"]
    L2["Layer 2 - kernel sysctls<br/>vm.unprivileged_userfaultfd, yama ptrace_scope"]
    L3["Layer 3 - capability scoping<br/>setcap cap_checkpoint_restore (CRIU path)"]
    L4["Layer 4 - fd hygiene<br/>O_CLOEXEC on uffd/epoll/eventfd/sockets"]
    L5["Layer 5 - protocol validation<br/>magic + count caps + offset bounds both sides"]
    L6["Layer 6 - integrity<br/>CRC32 over every page vs checkpoint digest"]
    L7["Layer 7 - fail-fast error contract<br/>errno-exact is_die on every syscall"]
    NET["planned: mTLS session tokens bound to VMA ranges"]
    L1 --> L2 --> L3 --> L4 --> L5 --> L6 --> L7 --> NET
```

## 2. Checkpoint / state distribution pipeline

```mermaid
flowchart LR
    A["SIGUSR2 to warm process"] --> B["freeze-free snapshot:<br/>write hdr+pages+tail, fsync"]
    B --> C[("ISIM file<br/>artifacts/app.isim")]
    C --> D{"distribution"}
    D -->|shared volume / object store| E[("target-side copy")]
    E --> F["pageserver mmap"]
    F --> G["serve 4KB pages on demand"]
    H["tail {seq, uptime}"] -.->|"skeleton: the ONLY<br/>pre-activation bytes"| I["restored instance metadata"]
```

## 3. Single-flight / deduplication

```mermaid
flowchart TD
    F["page fault for idx X"] --> S{"state[X]?"}
    S -->|LOCAL| W0["impossible: page present,<br/>no fault would occur"]
    S -->|REQ in-flight| P["pend[] += fault record<br/>(no duplicate request)"]
    S -->|IDLE| Q["state=REQ, queue offset once"]
    Q --> R["batched REQ frame"]
    P --> RESP
    R --> RESP["response pages arrive"]
    RESP --> J{"waiter inside run?"}
    J -->|yes| M["ranged UFFDIO_COPY run<br/>resolve ALL matched pend entries"]
    J -->|no| K["park into tagged ring"]
    K --> LATER["later fault: ring hit -><br/>zero-RTT inject"]
```

## 4. Backpressure / throttling decision flow

```mermaid
flowchart TD
    START["new work item"] --> C1{"sendq < IS_MAX_BATCH (64)?"}
    C1 -->|no| SKIP["skip enqueue:<br/>state machine already covers item"]
    C1 -->|yes| ENQ["sendq append"]
    ENQ --> C2{"npend == MAX_PEND (512)?"}
    C2 -->|overflow| DIE["ENOMEM fail-fast<br/>(protocol bug, surfaced loudly)"]
    C2 -->|ok| PENDD["pend append with t_fault"]
    PENDD --> FLUSH{"out frame staged?"}
    FLUSH -->|complete| SND["send() loop;<br/>EAGAIN -> resume via EPOLLOUT"]
    FLUSH -->|partial| RESUME["keep cursor;<br/>EPOLLOUT resumes flush"]
    SND --> ADAPT{"window stats >= 32 samples?"}
    ADAPT -->|misses <= 25 percent| GROW["lookahead *= 2 (cap 32)"]
    ADAPT -->|misses > 50 percent| SHRINK["lookahead /= 2 (floor 1)"]
```

## 5. Page state machine (resilience core)

```mermaid
stateDiagram-v2
    [*] --> IDLE
    IDLE --> REQ : fault/prefetch queues offset
    REQ --> LOCAL : response matches pend entry<br/>UFFDIO_COPY wakes sleeper
    REQ --> LOCAL : later ring hit serves it
    REQ --> REQ : evicted prefetch -> idempotent re-request
    LOCAL --> [*]
    note right of REQ
        ring_valid[tag] is orthogonal:
        cached data may exist while REQ
        (consumed on next touch)
    end note
```

## 6. Data layout — ISIM image and wire frames

```mermaid
flowchart LR
    subgraph IMG["checkpoint image (.isim)"]
        direction LR
        H["is_img_hdr 64B<br/>magic ver ps np len digest name"]
        PG["pages...<br/>np x 4096B"]
        T["tail_meta 24B<br/>TAIL_MAGIC seq uptime_ms"]
        H --> PG --> T
    end
    subgraph WIRE["TCP frames"]
        direction LR
        WH["wire_hdr 16B<br/>magic type count pad"]
        MO["wire_off 8B x N"]
        PH["wire_page_hdr 16B<br/>offset status data_len"]
        PD["data 4096B"]
    end
    IMG -->|"seeder / write_checkpoint"| DISK[("disk")]
    DISK -->|"pageserver mmap"| WIRE
```

## 7. Deployment topology

```mermaid
flowchart TB
    subgraph WIN["Windows dev box"]
        PS1["iscale.ps1"]
        DD["Docker Desktop<br/>(WSL2 VM, kernel 5.15+)"]
        PS1 --> DD
        subgraph DDN["docker bridge iscale-net"]
            C1["container iscale-src<br/>demo_app + pageserver"]
            C2["container target<br/>lazy resume"]
            C1 <-->|"TCP 46100<br/>pages over wire"| C2
        end
        ART[("artifacts/<br/>checkpoint + heartbeats")] --- C1
        ART --- C2
    end
    subgraph GH["GitHub Actions ubuntu-24.04"]
        CI["ci.yml: phases 1-4 matrix"]
        CC["criu.yml: source-built CRIU v4.1<br/>eager migration continuity OK<br/>lazy-pages experiment"]
    end
    DD -.->|"push triggers"| GH
```
