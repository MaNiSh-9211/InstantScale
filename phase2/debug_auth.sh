#!/usr/bin/env bash
# inspect reject-path client log (dev only)
cd "$(dirname "$0")"
./seeder /tmp/at.isim 64 >/dev/null
HOTPOD_TOKEN=s3cr3t-t0k3n ./pageserver --port 46311 --image /tmp/at.isim >/tmp/aps.log 2>&1 &
SP=$!
sleep 0.4
timeout 10 ./restorer --port 46311 >/tmp/ac2.log 2>&1
echo "client rc=$?"
echo "--- ac2.log ---"
cat /tmp/ac2.log
echo "--- aps tail ---"
tail -n 4 /tmp/aps.log
kill $SP 2>/dev/null
