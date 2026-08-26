#!/usr/bin/env bash
# HotPod A/B battery — run everything N times, print a stats table.
# No simulation: cold start performs real work (256 MB load + CRC verify).
#
# Usage:  RUNS=3 WARM_MB=32 bash battery.sh
set -uo pipefail
cd "$(dirname "$0")"

RUNS=${RUNS:-3}
WARM_MB=${WARM_MB:-32}
INIT_WORK_MB=${INIT_WORK_MB:-256}
export WARM_MB INIT_WORK_MB

make -C ../phase2 pageserver seeder >/dev/null 2>&1
make -C ../phase3 demo_app >/dev/null 2>&1

declare -a COLD_ACT EAGER_ACT LAZY_ACT
declare -a COLD_OK EAGER_OK LAZY_OK

parse_act() { # extract activation_ms from a RESULT line
    awk -F'activation_ms=' '{split($2,a," "); print a[1]+0}'
}

continuity_ok() { # $1=RESULT line -> 1 if seq continued + digest present
    echo "$1" | grep -q "FINAL digest=" || return 1
    local pre post
    pre=$(echo "$1"  | grep -oE 'pre_seq=[0-9]+'  | grep -oE '[0-9]+')
    post=$(echo "$1" | grep -oE 'post_seq=[0-9]+' | grep -oE '[0-9]+')
    [ -n "$pre" ] && [ -n "$post" ] && [ "$post" -gt "$pre" ]
}

run_mode() { # $1=mode
    local mode=$1 arr=$2 okarr=$3
    for i in $(seq 1 "$RUNS"); do
        local line
        line=$(WARM_MB=$WARM_MB bash hotpod.sh "$mode" 2>&1 | grep '^RESULT' || true)
        local act
        act=$(echo "$line" | parse_act)
        if [ -z "$act" ]; then
            echo "  run $i/$RUNS [$mode]: FAILED"; continue
        fi
        echo "  run $i/$RUNS [$mode]: ${act} ms"
        eval "$arr+=(\$act)"
        if [ "$mode" = "cold" ]; then
            eval "$okarr+=(2)"          # 2 = n/a (fresh start, nothing to continue)
        elif continuity_ok "$line"; then
            eval "$okarr+=(1)"
        else
            eval "$okarr+=(0)"
        fi
    done
}

stats() { # $1=array-name -> prints "min median max"
    local -a v=()
    eval 'v=(${'"$1"'[@]})'
    [ ${#v[@]} -eq 0 ] && { echo "NA NA NA"; return; }
    local sorted=$(printf '%s\n' "${v[@]}" | sort -n)
    local min=$(echo "$sorted" | head -1)
    local max=$(echo "$sorted" | tail -1)
    local med=$(echo "$sorted" | awk '{a[NR]=$1} END{print (NR%2)?a[int(NR/2)+1]:(a[NR/2]+a[NR/2+1])/2}')
    echo "$min $med $max"
}

okcount() { eval 'v=(${'"$1"'[@]})'; local n=0; for x in "${v[@]:-}"; do [ "${x:-0}" != "0" ] && n=$((n+1)); done; echo "$n"; }

echo "=== HotPod A/B battery: RUNS=$RUNS heap=${WARM_MB}MB real-cold-work=${INIT_WORK_MB}MB ==="
echo "-- cold (real 256MB load+verify each start) --"; run_mode cold  COLD_ACT  COLD_OK
echo "-- eager (full copy before start)           --"; run_mode eager EAGER_ACT EAGER_OK
echo "-- lazy  (HotPod: 0% heap at start)         --"; run_mode lazy  LAZY_ACT  LAZY_OK

COLD_S=$(stats COLD_ACT);  EAGER_S=$(stats EAGER_ACT);  LAZY_S=$(stats LAZY_ACT)
COLD_MIN=$(echo $COLD_S  | awk '{print $1}'); COLD_MED=$(echo $COLD_S  | awk '{print $2}')
EAGER_MIN=$(echo $EAGER_S| awk '{print $1}'); EAGER_MED=$(echo $EAGER_S| awk '{print $2}')
LAZY_MIN=$(echo $LAZY_S  | awk '{print $1}'); LAZY_MED=$(echo $LAZY_S  | awk '{print $2}')
COLD_OK_N=$(okcount COLD_OK); EAGER_OK_N=$(okcount EAGER_OK); LAZY_OK_N=$(okcount LAZY_OK)

SPEED_COLD=$(awk -v c="$COLD_MED" -v l="$LAZY_MED" 'BEGIN{if(l+0>0)printf "%.0f", c/l; else print "?"}')
SPEED_EAGER=$(awk -v e="$EAGER_MED" -v l="$LAZY_MED" 'BEGIN{if(l+0>0)printf "%.1f", e/l; else print "?"}')

echo
echo "================ A/B RESULTS (median of $RUNS) ================"
printf "%-8s %6s %10s %10s %10s %8s\n" "mode" "runs" "min_ms" "median_ms" "max_ms" "continuity"
printf "%-8s %6s %10s %10s %10s %8s\n" "------" "----" "------" "---------" "------" "-----"
printf "%-8s %6s %10s %10s %10s %4s/%s\n" "cold"  "$RUNS" "$COLD_MIN"  "$COLD_MED"  "$(echo $COLD_S|awk '{print $3}')"  "$COLD_OK_N" "$RUNS"
printf "%-8s %6s %10s %10s %10s %4s/%s\n" "eager" "$RUNS" "$EAGER_MIN" "$EAGER_MED" "$(echo $EAGER_S|awk '{print $3}')" "$EAGER_OK_N" "$RUNS"
printf "%-8s %6s %10s %10s %10s %4s/%s\n" "lazy"  "$RUNS" "$LAZY_MIN"  "$LAZY_MED"  "$(echo $LAZY_S|awk '{print $3}')"  "$LAZY_OK_N" "$RUNS"
echo "---------------------------------------------------------------"
echo "HotPod lazy is ${SPEED_COLD}x faster than cold start and"
echo "${SPEED_EAGER}x faster than eager full-copy (median, this machine)."
echo "==============================================================="
