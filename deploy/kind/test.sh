#!/usr/bin/env bash
# HotPod vs normal autoscaling on a REAL kind cluster.
# Measures wall-clock from "scale decision" (Job applied) to "all replicas
# done serving their heartbeats", plus per-pod self-reported evidence.
#
# Prereqs: kind cluster running, image built & loaded (test.sh does both):
#   docker build -f deploy/Dockerfile -t hotpod:test .
#   kind load docker-image hotpod:test --name <cluster>
set -uo pipefail

NS=hotpod-demo
N=${N:-5}
CTX=$(kubectl config current-context)
CLUSTER=${CTX#kind-}

say() { echo "[kind-test] $*"; }

say "context=$CTX cluster=$CLUSTER replicas=$N"
kind load docker-image hotpod:test --name "$CLUSTER" 2>/dev/null \
  || say "(kind load skipped — assuming image already in cluster)"

kubectl apply -f deploy/kind/00-ns-pvc-secret.yaml >/dev/null
kubectl -n $NS delete job checkpoint-producer cold-scale hot-scale \
        --ignore-not-found >/dev/null 2>&1

# --- 1. one-time warm cost: produce the checkpoint -------------------------
say "producing warm checkpoint (this is the one-time cost)..."
kubectl apply -f deploy/kind/10-checkpoint-job.yaml >/dev/null
kubectl -n $NS wait job/checkpoint-producer \
        --for=condition=complete --timeout=300s >/dev/null \
  || { say "producer FAILED"; kubectl -n $NS logs job/checkpoint-producer; exit 1; }
CKPT_MS=$(kubectl -n $NS logs job/checkpoint-producer \
          | grep -oE 'took=[0-9.]+ ms' | head -1 | grep -oE '[0-9.]+')
[ -n "$CKPT_MS" ] || { say "producer produced no timing — aborting"; exit 1; }

# --- 2. pageserver ----------------------------------------------------------
kubectl apply -f deploy/kind/20-pageserver.yaml >/dev/null
kubectl -n $NS wait deploy/hotpod-pageserver \
        --for=condition=available --timeout=180s >/dev/null
say "pageserver ready"

# --- 3. the A/B race ---------------------------------------------------------
run_scale() { # $1=job-name $2=manifest -> echoes wall ms
    kubectl -n $NS delete job "$1" --ignore-not-found >/dev/null 2>&1
    kubectl apply -f "$2" >/dev/null
    local t0=$(date +%s%3N)
    kubectl -n $NS wait "job/$1" --for=condition=complete \
            --timeout=300s >/dev/null \
      || { say "$1 FAILED"; kubectl -n $NS logs "job/$1" --tail=-1; exit 1; }
    local t1=$(date +%s%3N)
    echo $((t1 - t0))
}

say "NORMAL autoscaling: $N fresh pods, full cold start each..."
COLD_WALL=$(run_scale cold-scale deploy/kind/30-scale-cold-job.yaml)

say "HOTPOD autoscaling: $N pods lazy-resuming one checkpoint..."
HOT_WALL=$(run_scale hot-scale deploy/kind/31-scale-hot-job.yaml)

# --- 4. evidence -------------------------------------------------------------
echo
echo "per-pod evidence (cold):"
kubectl -n $NS logs job/cold-scale | grep -E "INIT real-work|READY" | head -12
echo
echo "per-pod evidence (hot):"
kubectl -n $NS logs job/hot-scale | grep -E "RESUMED-LAZY|FINAL" | head -12

COLD_ACT=$(kubectl -n $NS logs job/cold-scale \
           | grep -oE 'loaded\+verified [0-9]+ MB in [0-9.]+ ms' \
           | grep -oE '[0-9.]+$' | sort -n | head -1)
HOT_ACT=$(kubectl -n $NS logs job/hot-scale \
          | grep -oE 'activated_in=[0-9.]+' | grep -oE '[0-9.]+' \
          | sort -n | head -1)
HOT_MAX=$(kubectl -n $NS logs job/hot-scale \
          | grep -oE 'activated_in=[0-9.]+' | grep -oE '[0-9.]+' \
          | sort -n | tail -1)

fhb() { # $1=job -> "min/max ms" of FIRST_HB after_start across ALL pods
    for p in $(kubectl -n $NS get pods -l "job-name=$1" -o jsonpath='{.items[*].metadata.name}'); do
        kubectl -n $NS logs "$p" 2>/dev/null \
          | grep -oE 'FIRST_HB after_start=[0-9.]+'
    done | grep -oE '[0-9.]+' | sort -n \
      | awk 'NR==1{mn=$1} {mx=$1} END{if(mn=="")mn="NA";printf "%s / %s", mn, mx}'
}
COLD_FHB=$(fhb cold-scale)
HOT_FHB=$(fhb hot-scale)

SPEEDUP=$(awk -v c="${COLD_FHB%% *}" -v h="${HOT_FHB%% *}" \
          'BEGIN{if (h+0 > 0) printf "%.0f", c/h; else print "?"}')

echo
echo "================= KIND A/B RESULT (N=$N replicas) ================="
printf "%-42s %12s %12s\n" "metric" "NORMAL" "HOTPOD"
printf "%-42s %12s %12s\n" "------------------------------------------" "------" "------"
printf "%-42s %10s ms %10s ms\n" "wall: scale decision -> ALL completed" "$COLD_WALL" "$HOT_WALL"
printf "%-42s %16s %16s\n" "readiness: start -> first HB (min/max)" "$COLD_FHB" "$HOT_FHB"
printf "%-42s %10s ms %10s ms\n" "fastest replica self-reported activation" "$COLD_ACT" "$HOT_ACT"
printf "%-42s %12s %12s\n" "state at first heartbeat" "cold" "warm (seq N+1)"
echo "-------------------------------------------------------------------"
printf "HOTPOD replicas were %sx faster to first heartbeat\n" "$SPEEDUP"
printf "(checkpoint one-time cost: %s ms; hot wall includes full\n lazy hydration + CRC verify before pod exit — cold exits early)\n" \
       "$CKPT_MS"
echo "==================================================================="
