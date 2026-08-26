#!/usr/bin/env bash
set -uo pipefail
echo "== docker containers (all) =="
docker ps -a --format '{{.Names}} | {{.Image}} | {{.Status}}'
echo "== kubectl current context =="
kubectl config current-context
echo "== kubeconfig server for kind-hotpod =="
kubectl config view --minify -o jsonpath='{.clusters[0].cluster.server}' 2>/dev/null || true
echo
echo "== nodes =="
kubectl get nodes -o wide 2>&1 | head -5
kubectl get node -o jsonpath='{.items[0].metadata.creationTimestamp}' 2>/dev/null; echo
echo "== namespaces =="
kubectl get ns 2>&1 | head -10
echo "== deployments in hotpod-demo =="
kubectl -n hotpod-demo get deploy 2>&1
echo "== listening ports on kind node container (if exists) =="
CID=$(docker ps -aq --filter name=control-plane)
if [ -n "$CID" ]; then
  for c in $CID; do
    echo "container $c: $(docker inspect -f '{{.State.Status}} started={{.State.StartedAt}}' $c)"
    docker port "$c" 2>/dev/null || true
  done
else
  echo "(no control-plane container found!)"
fi
