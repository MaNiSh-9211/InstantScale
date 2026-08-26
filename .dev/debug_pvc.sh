#!/usr/bin/env bash
# PVC visibility debug: what does /data look like from inside the cluster?
set -uo pipefail
cd "$(dirname "$0")/.."

KUBECTL=kubectl

echo "== engine/node recovery =="
if ! $KUBECTL get ns hotpod-demo >/dev/null 2>&1; then
    echo "api down -> restarting kind node container"
    docker start hotpod-control-plane >/dev/null 2>&1 || true
    for i in $(seq 60); do
        $KUBECTL get ns hotpod-demo >/dev/null 2>&1 && break
        sleep 3
    done
fi
$KUBECTL wait --for=condition=Ready node/hotpod-control-plane --timeout=120s || true

echo "== apply lsdebug =="
$KUBECTL apply -f deploy/kind/lsdebug.yaml
for i in $(seq 30); do
    ST=$($KUBECTL -n hotpod-demo get pod lsdebug -o jsonpath='{.status.phase}' 2>/dev/null)
    [ "$ST" = "Succeeded" ] || [ "$ST" = "Failed" ] && break
    sleep 2
done
echo "phase=$ST"
echo "== /data as seen inside cluster =="
$KUBECTL -n hotpod-demo logs lsdebug 2>&1
echo "== pageserver pod events (last) =="
POD=$($KUBECTL -n hotpod-demo get pods -l app=hotpod-pageserver \
       -o jsonpath='{.items[0].metadata.name}')
[ -n "$POD" ] && $KUBECTL -n hotpod-demo describe pod "$POD" | tail -n 12
