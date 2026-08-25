#!/usr/bin/env bash
# Hammer the lazy restorer until it hangs (timeout=3s per attempt).
set -u
cd "$(dirname "$0")"

./seeder /tmp/w.isim "${1:-64}" >/dev/null
stdbuf -oL ./pageserver --port 46100 --image /tmp/w.isim >/tmp/srv.log 2>&1 &
SRV=$!
sleep 0.4

for attempt in $(seq 1 "${2:-20}"); do
    IS_DBG=1 timeout 3 stdbuf -oL ./restorer --port 46100 >/tmp/res.log 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "attempt $attempt: PASS"
    else
        echo "attempt $attempt: rc=$rc  <-- capturing"
        echo "--- last 30 trace lines ---"
        tail -n 30 /tmp/res.log
        break
    fi
done
kill $SRV 2>/dev/null
tail -n 3 /tmp/srv.log
