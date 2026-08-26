#!/usr/bin/env bash
# HotPod v0.5 verification: zero-elision + metrics + auth + battery
set -uo pipefail
cd /src/phase2
make clean all >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
echo "BUILD OK"

echo "== T1: zero-page elision + metrics =="
./seeder /tmp/z.isim 4096 3 >/dev/null
stdbuf -oL -eL ./pageserver --port 46330 --image /tmp/z.isim --metrics-port 46331 >/tmp/ps.log 2>&1 &
SP=$!
sleep 0.6
grep -q "metrics on" /tmp/ps.log && echo "PASS metrics-thread-up" || echo "FAIL metrics-thread-up"
timeout 20 ./restorer --port 46330 2>&1 | grep -E "SUMMARY|zero pages" | head -3
sleep 0.3
curl -s -m 3 http://127.0.0.1:46331/metrics | grep -E "hotpod_(pages|zero|auth)" || echo "FAIL metrics-curl"
kill $SP 2>/dev/null; wait $SP 2>/dev/null

echo "== T2: auth suite =="
bash auth_test.sh 2>&1 | grep -E "^PASS|^FAIL|AUTH SUITE"

echo "== T3: battery (real cold work) =="
cd /src/phase3
make demo_app >/dev/null 2>&1
RUNS=2 bash battery.sh 2>&1 | tail -12
