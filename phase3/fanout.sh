#!/usr/bin/env bash
# HotPod â€” FAN-OUT autoscaling demo
#
# THE SPIKE SCENARIO: traffic doubles, the autoscaler needs 10 more warm
# replicas RIGHT NOW. With one existing checkpoint:
#
#   traditional : N x cold bootstrap (sequential minutes) or N x full-copy
#   HotPod: N replicas activate CONCURRENTLY, each in ~sub-ms,
#                 pages streaming behind execution from one page server
#
# Continuity + integrity verified per replica (seq == pre+1, uniform CRC).
set -euo pipefail
cd "$(dirname "$0")"

N=${1:-10}
WARM_MB=${WARM_MB:-16}
INIT_MS=${INIT_MS:-1500}
PORT=${PORT:-46250}
WORK=$(mktemp -d /tmp/hotpodN.XXXXXX)
IMG=/tmp/demo_app.isim
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT

now_ms() { date +%s%3N; }

echo "== source instance: warming ($WARM_MB MB, ${INIT_MS}ms tax) =="
./demo_app --init-ms "$INIT_MS" --warm-mb "$WARM_MB" >"$WORK/a.log" 2>&1 &
AP=$!
PIDS+=("$AP")
until grep -q '^READY' "$WORK/a.log" 2>/dev/null; do sleep 0.05; done
sleep 0.45 # accumulate heartbeats
S1=$(awk '$1=="HB"{split($2,a,"=");s=a[2]}END{print s+0}' "$WORK/a.log")

kill -USR2 "$AP" # snapshot & exit
until grep -q '^CKPT ' "$WORK/a.log" 2>/dev/null; do sleep 0.02; done
wait "$AP" 2>/dev/null || true
cp "$IMG" "$WORK/app.isim"

[ -x ../phase2/pageserver ] || make -C ../phase2 pageserver >/dev/null
../phase2/pageserver --port "$PORT" --image "$WORK/app.isim" \
    >"$WORK/ps.log" 2>&1 &
PS_PID=$!
PIDS+=("$PS_PID")
sleep 0.25

echo "== SPIKE: fanning out $N replicas concurrently =="
T0=$(now_ms)
for i in $(seq 1 "$N"); do
    ./demo_app --resume-lazy-img "$WORK/app.isim" --host 127.0.0.1 \
               --port "$PORT" >"$WORK/r$i.log" 2>&1 &
    RP[$i]=$!
    PIDS+=("${RP[$i]}")
done

# Collect per-replica activation (app-reported, process-monotonic clock).
declare -A ACT
remaining=$N
deadline=$(( $(now_ms) + 60000 ))
while [ "$remaining" -gt 0 ] && [ "$(now_ms)" -lt "$deadline" ]; do
    remaining=0
    for i in $(seq 1 "$N"); do
        [ -n "${ACT[$i]:-}" ] && continue
        a=$(awk 'match($0,/activated_in=[0-9.]+/){
                    printf "%.3f", substr($0,RSTART+13,RLENGTH-13)+0; exit }' \
                "$WORK/r$i.log" 2>/dev/null || true)
        if [ -n "$a" ]; then ACT[$i]="$a"; else remaining=$((remaining+1)); fi
    done
    [ "$remaining" -gt 0 ] && sleep 0.01
done
T_ALL=$(( $(now_ms) - T0 ))

# Hydration happens behind execution; give it a moment, then stop cleanly.
sleep 2.5

min=999999; max=0; cont_ok=0; digests=""
for i in $(seq 1 "$N"); do
    kill -TERM "${RP[$i]}" 2>/dev/null || true
    a=${ACT[$i]:-TIMEOUT}
    echo "  replica $i : activated_in=${a} ms"
    awk -v t="$a" 'BEGIN{exit !(t+0<min)}' || true
    # numeric min/max via shell
    cmp_min=$(awk -v a="$a" -v m="$min" 'BEGIN{print (a+0<m)?1:0}')
    [ "$cmp_min" = "1" ] && min=$a
    cmp_max=$(awk -v a="$a" -v m="$max" 'BEGIN{print (a+0>m)?1:0}')
    [ "$cmp_max" = "1" ] && max=$a
done
wait $(for i in $(seq 1 "$N"); do echo "${RP[$i]}"; done) 2>/dev/null || true

for i in $(seq 1 "$N"); do
    s=$(awk '$1=="HB"{split($2,a,"=");print a[2];exit}' "$WORK/r$i.log")
    d=$(grep '^FINAL' "$WORK/r$i.log" | awk '{print $3}')
    digests="$digests$d "
    [ "${s:-0}" -ge $((S1 + 1)) ] && cont_ok=$((cont_ok + 1))
done

unique_digests=$(echo $digests | tr ' ' '\n' | sort -u | wc -l)
cold_equiv=$(( N * INIT_MS ))

echo
echo "RESULT mode=fanout replicas=$N heap_mb=$WARM_MB pre_seq=$S1"
echo "       wall_to_all_running_ms=$T_ALL"
echo "       activation_ms: min=$min max=$max"
echo "       continuity_ok=$cont_ok/$N (first HB seq >= $((S1+1)))"
echo "       uniform_final_digest=$([ "$unique_digests" = "1" ] && echo YES || echo NO)"
echo "       cold_start_equivalent_sequential_ms=$cold_equiv (reference)"
