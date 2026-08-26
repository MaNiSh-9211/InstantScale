#!/usr/bin/env bash
# HotPod end-to-end on kind: node up -> build -> load -> A/B scale test.
set -uo pipefail
cd "$(dirname "$0")/.."

KIND="$(cygpath -u "$USERPROFILE")/tools/kind.exe"
N=${N:-5}
say(){ echo "[e2e] $*"; }

# 0. kind node container up + Ready
if ! docker ps --format '{{.Names}}' | grep -q '^hotpod-control-plane$'; then
    say "starting kind node container..."
    docker start hotpod-control-plane >/dev/null 2>&1 || true
fi
say "waiting for node Ready..."
kubectl wait --for=condition=Ready node/hotpod-control-plane --timeout=240s

# 1. build production image
say "building hotpod:test..."
docker build -f deploy/Dockerfile -t hotpod:test . 2>&1 | tail -2

# 2. load into kind
say "loading image into kind..."
"$KIND" load docker-image hotpod:test --name hotpod 2>&1 | tail -1

# 3. A/B test
say "A/B test: N=$N replicas, real cold work vs lazy resume"
N=$N bash deploy/kind/test.sh
