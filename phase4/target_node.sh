#!/usr/bin/env bash
# HotPod Phase 4 â€” TARGET NODE (runs inside the "Host B" container)
#
# Activates a lazily-restored instance whose pages live on Host A, across the
# docker bridge network. Prints machine-readable RESULT lines for the
# Windows orchestrator to aggregate.
#
# usage: target_node.sh <source-hostname> [port] [mode: lazy|eager]
set -euo pipefail
cd /src

HOST=${1:?source hostname required}
PORT=${2:-46100}
MODE=${3:-lazy}
IMG=/src/artifacts/app.isim

# Always rebuild: cheap and immune to stale bind-mounted binaries.
make -C phase3 demo_app >/dev/null

for _ in $(seq 1 100); do
    [ -f "$IMG" ] && break
    sleep 0.1
done

HB=/tmp/target_hb.log
: >"$HB"

if [ "$MODE" = "eager" ]; then
    ./phase3/demo_app --resume "$IMG" >>"$HB" 2>&1 &
else
    ./phase3/demo_app --resume-lazy-img "$IMG" \
                      --host "$HOST" --port "$PORT" --interval-ms 50 \
                      >>"$HB" 2>&1 &
fi
B=$!

for _ in $(seq 1 1500); do
    grep -q '^RESUMED' "$HB" && break
    sleep 0.01
done

sleep 6              # serve traffic while hydration streams in the background
kill -TERM "$B" 2>/dev/null || true
wait "$B" 2>/dev/null || true

PRE=$(cat /src/artifacts/pre_seq.txt 2>/dev/null || echo '?')
POST=$(awk '$1=="HB"{split($2,a,"=");print a[2]; exit}' "$HB")
ACT=$(awk 'match($0,/activated_in=[0-9.]+/){print substr($0,RSTART+13,RLENGTH-13);exit}' "$HB")
HBS=$(grep -c '^HB ' "$HB" || true)
FIN=$(grep '^FINAL' "$HB" || echo "FINAL MISSING")

echo "RESULT mode=$MODE activation_ms=${ACT:-NA} pre_seq=$PRE post_seq=$POST heartbeats=$HBS $FIN"
