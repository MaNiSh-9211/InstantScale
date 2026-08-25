#!/usr/bin/env bash
# Fan-out battery: 1 warm checkpoint -> N concurrent instant replicas
set -u
cd "$(dirname "$0")"

echo "=== fanout x10 (16MB heap) ==="
bash fanout.sh 10 || true

echo
echo "=== fanout x25 (32MB heap) ==="
WARM_MB=32 bash fanout.sh 25 || true
