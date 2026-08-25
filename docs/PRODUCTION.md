# HotPod Production Runbook

Target reader: DevOps engineers deploying HotPod for instant scale-out.

## 1. What you are deploying

| Component | What it does | Runs as |
|---|---|---|
| `pageserver` | Serves 4 KB pages from a checkpoint image over TCP :46100. PSK/HMAC auth enforced when a token is configured. | Deployment (k8s) / systemd service (VM) |
| `demo_app --resume-lazy-img` | Reference consumer: activates with an empty heap, pulls pages on demand. Your production workload links the same puller (see `phase3/puller.h`). | Job / pod / process |
| checkpoint image (ISIM) | `[hdr][pages][tail{seq,uptime}]`, CRC32-digested. Produced by `SIGUSR2` to a running HotPod-lifecycle app, or by `seeder`. | file on shared storage |

## 2. Prerequisites

- Linux kernel ≥ 5.11 with `userfaultfd`. Either:
  - `sysctl vm.unprivileged_userfaultfd=1` (recommended; see systemd unit), or
  - run containers `privileged: true` (k8s manifests shipped this way), or
  - grant `CAP_SYS_PTRACE`.
- Image storage reachable by both sides (hostPath, NFS, S3-mounted, …).
- Network path target → pageserver with ≤ 1 ms RTT for best numbers
  (cross-region works, activation becomes ≈ RTT).
- Docker ≥ 20.10 / k8s ≥ 1.25.

## 3. Build & distribute

```bash
docker build -f deploy/Dockerfile -t ghcr.io/<org>/hotpod:v0.5.0 .
docker push ghcr.io/<org>/hotpod:v0.5.0
```

## 4. Secrets

```bash
kubectl -n hotpod create secret generic hotpod-token \
  --from-literal=HOTPOD_TOKEN="$(openssl rand -hex 32)"
```

VM path: token file at `/etc/hotpod/token`, `chmod 600`, owned by service user.
**Rotate** by restarting pageserver + consumers with the new token
(sessions are short-lived; rotation is a restart, not a migration).

## 5. Producing a checkpoint (the "warm" step)

1. Start your service from the HotPod image with the lifecycle entry point
   (see `phase3/demo_app.c` for the reference integration: warm-up →
   heartbeats → SIGUSR2 handler).
2. Trigger: `kill -USR2 <pid>` → writes `/tmp/demo_app.isim` (or
   `--ckpt-path`), then keeps running (SIGUSR1) or exits (SIGUSR2).
3. Move the image to shared storage; pageserver picks it up read-only.

## 6. Activating replicas

Kubernetes: apply the migration Job (swap the command for your workload
binary). VM: `demo_app --resume-lazy-img /data/app.isim --host <ps> --port 46100`.

Expected logs on the target:

```
RESUMED-LAZY seq=<N> activated_in=<0.2–5> ms (0% of <pages> pages present)
READY ... digest=PENDING(lazy)
HB seq=<N+1> ...          ← continuity proof
```

On SIGTERM the instance prints `FINAL digest=…` — compare with the source
checkpoint digest (`seeder`/CKPT log). Equal = integrity proven.

## 7. Monitoring

* **Activation**: parse `activated_in=<ms>` from target stdout; alert > 100 ms
  intra-region.
* **Hydration**: pageserver stats line on shutdown
  (`stats: pages=… bytes=… conns_total=… live=…`); ship stdout.
* **Integrity**: absence of `FINAL digest=` before process exit = abnormal.
* **Auth failures**: pageserver logs `AUTH failed` / `unauthenticated frame`
  — alert on any (token skew or intrusion attempt).

## 8. Failure playbook

| Symptom | Likely cause | Action |
|---|---|---|
| target: `token rejected by pageserver` | secret skew | re-create secret, restart both |
| target: `pageserver requires a token` | client missing HOTPOD_TOKEN | fix env/secret |
| target: `recv(EOF mid-frame) ECONNRESET` | pageserver died | check pageserver logs/disk; Restart policy handles k8s |
| activation OK but heartbeats stall | network partition mid-flow | verify Service/endpoints; restart target |
| `digest mismatch` at FINAL | memory corruption — **file a bug** | do not ignore; capture both logs |
| pageserver: `client cap 256 hit` | too many concurrent resumes | scale pageserver Deployments per checkpoint |

## 9. Security model

* PSK + HMAC-SHA256 challenge-response, both directions, constant-time tag
  compare (ADR-0011). No token ⇒ pageserver refuses everything except AUTH.
* Nonces from `/dev/urandom`; replay guard (one AUTH per connection).
* Transport is plaintext TCP today — deploy inside a private network /
  NetworkPolicy / WireGuard mesh. TLS is on the roadmap; do not expose
  :46100 publicly.
* Containers run privileged solely to permit `userfaultfd`; prefer the
  sysctl route to drop privilege when your nodes allow it.

## 10. Known limitations (beta)

* CRIU `--lazy-pages` for *arbitrary third-party* processes is experimental
  (eager CRIU migration is CI-verified). First-party lifecycle apps are GA.
* One heap region per checkpoint.
* Checkpoint images are immutable per session; live re-sync (dirty-page
  tracking) is on the roadmap.

## 11. Upgrade path

Images are version-tagged; pageserver and consumers must share the wire
version (`IS_PROTO_VER`). Deploy new pageserver first (it is
backward-compatible within a major), then roll consumers.
