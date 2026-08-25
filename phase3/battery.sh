#!/usr/bin/env bash
# Phase 3 full validation battery (dev/CI entry point)
set -u
cd "$(dirname "$0")"

echo "=== all (64MB heap, 2s cold tax) ==="
WARM_MB=64 bash hotpod.sh all 2>&1 | grep -E 'RESULT|ERROR'

echo
echo "=== lazy stability x5 (32MB) ==="
for i in 1 2 3 4 5; do
    WARM_MB=32 bash hotpod.sh lazy >/tmp/r$i.log 2>&1
    rc=$?
    res=$(grep -m1 RESULT /tmp/r$i.log || true)
    if [ -n "$res" ]; then
        echo "run$i rc=$rc $res"
    else
        echo "run$i rc=$rc FAILED â€” last lines:"
        tail -n 4 /tmp/r$i.log
    fi
done

echo
echo "=== eager activation parse check ==="
WARM_MB=16 bash hotpod.sh eager >/tmp/re.log 2>&1
grep -E 'RESULT|ERROR' /tmp/re.log || tail -n 6 /tmp/re.log
