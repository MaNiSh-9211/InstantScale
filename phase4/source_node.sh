#!/usr/bin/env bash
# InstantScale Phase 4 — SOURCE NODE (runs inside the "Host A" container)
#
# Lifecycle:
#   1. build binaries (bind-mounted repo)
#   2. boot a warm instance of demo_app (cold tax configurable via env)
#   3. let it serve a few heartbeats, then SIGUSR2 => checkpoint & exit
#   4. signal readiness to the Windows orchestrator via artifacts/checkpoint.ready
#   5. exec the pageserver: Host B will pull 4 KB pages over the docker network
set -euo pipefail
cd /src

INIT_MS=${INIT_MS:-1200}
HEAP_MB=${HEAP_MB:-32}
PORT=${PORT:-46100}

mkdir -p artifacts
rm -f artifacts/*

# Always rebuild: cheap (<1s) and immune to stale bind-mounted binaries.
make -C phase3 all       >/dev/null
make -C phase2 pageserver >/dev/null

echo "[source] booting warm instance (init=${INIT_MS}ms heap=${HEAP_MB}MB)"
./phase3/demo_app --init-ms "$INIT_MS" --warm-mb "$HEAP_MB" \
    --ckpt artifacts/app.isim \
    >>artifacts/src_hb.log 2>&1 &
APP=$!

for _ in $(seq 1 400); do
    grep -q '^READY' artifacts/src_hb.log 2>/dev/null && break
    sleep 0.05
done
grep -q '^READY' artifacts/src_hb.log || { echo "[source] app never became READY"; exit 1; }
sleep 0.5   # accumulate heartbeats -> continuity anchor seq

PRE_SEQ=$(awk '$1=="HB"{split($2,a,"=");s=a[2]}END{print s+0}' artifacts/src_hb.log)
echo "[source] pre-migration seq=$PRE_SEQ — checkpointing"

kill -USR2 "$APP"
wait "$APP" 2>/dev/null || true

grep -q '^CKPT ' artifacts/src_hb.log
[ -f artifacts/app.isim ] || { echo "[source] checkpoint file missing!"; exit 1; }
echo "[source] checkpoint written: $(stat -c%s artifacts/app.isim) bytes"
echo "$PRE_SEQ" > artifacts/pre_seq.txt
touch artifacts/checkpoint.ready      # Windows orchestrator polls this

echo "[source] serving pages on :$PORT — target may activate now"
exec ./phase2/pageserver --port "$PORT" --image artifacts/app.isim
