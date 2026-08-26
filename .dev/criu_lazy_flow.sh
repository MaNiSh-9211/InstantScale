#!/usr/bin/env bash
# CRIU lazy-pages roundtrip — RUN AS ROOT (sudo bash criu_lazy_flow.sh)
# Producer app -> page-server -> lazy dump -> lazy restore -> continuity.
set -uo pipefail

CR=/tmp/criu-src/criu/criu
W=/tmp/lazy
rm -rf "$W"; mkdir -p "$W/ps" "$W/dump"
HB=$W/hb.log; : > "$HB"

/src/phase3/demo_app --init-work-mb 64 --warm-mb 16 \
    --interval-ms 50 --hb-count 40 >> "$HB" 2>&1 &
APP=$!
until grep -q "^READY" "$HB"; do sleep 0.05; done
until grep -q "^HB seq=2" "$HB"; do sleep 0.02; done
PRE=$(awk '$1=="HB"{split($2,a,"=");s=a[2]}END{print s+0}' "$HB")

timeout -k 5 60 $CR page-server --images $W/ps --address 127.0.0.1 \
     --port 46333 --daemon --pidfile $W/ps.pid \
     -o $W/ps.log -v1 || echo "[dbg] page-server rc=$?"

timeout -k 5 180 $CR dump -D $W/dump --lazy-pages \
     --page-server --address 127.0.0.1 --port 46333 \
     -t $APP --shell-job -o $W/dump.log -v1 &
DJ=$!
for _ in $(seq 300); do
    [ -f $W/ps/pstree.img ] && break
    sleep 0.1
done

timeout -k 5 180 $CR restore --lazy-pages --page-server \
     --address 127.0.0.1 --port 46333 --images $W/ps \
     --shell-job -d --pidfile $W/r.pid \
     -o $W/res.log -v1 || echo "[dbg] restore rc=$?"
wait $DJ 2>/dev/null || true

sleep 2
POST=$(awk '$1=="HB"{split($2,a,"=");s=a[2]}END{print s+0}' "$HB")
kill $(cat $W/r.pid 2>/dev/null) 2>/dev/null || true
kill $(cat $W/ps.pid 2>/dev/null) 2>/dev/null || true

echo "lazy pre_seq=$PRE post_seq=$POST"
echo "--- res.log tail ---"; tail -n 8 $W/res.log 2>/dev/null || true
echo "--- dump.log tail ---"; tail -n 8 $W/dump.log 2>/dev/null || true
mkdir -p /tmp/lazy-logs
cp $W/*.log "$HB" /tmp/lazy-logs/ 2>/dev/null || true
chmod -R a+rX /tmp/lazy-logs 2>/dev/null || true

if [ -n "$POST" ] && [ "$POST" -gt "$PRE" ]; then
    echo "CRIU LAZY PAGES OK"
    exit 0
fi
echo "CRIU LAZY PAGES: continuity not observed"
exit 1
