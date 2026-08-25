#!/usr/bin/env bash
# InstantScale Phase 3 orchestrator — the autoscaling story, end to end:
#
#   cold   : fresh process incl. simulated runtime bootstrap (--init-ms)
#   eager  : warm instance checkpoints itself (SIGUSR2) -> image copied to
#            "target" -> --resume loads ALL pages before heartbeat #1
#   lazy   : same checkpoint -> phase2 pageserver streams it ->
#            --resume-lazy activates INSTANTLY, pages arrive behind execution
#
# Continuity proof: heartbeats carry a monotonic seq. The first post-resume
# heartbeat must continue at exactly last_seq+1 — the process RESUMED.
# Final integrity: SIGTERM triggers a full-heap CRC sweep ("FINAL digest=").
set -euo pipefail
cd "$(dirname "$0")"

MODE=${1:-all}
WARM_MB=${WARM_MB:-32}
INIT_MS=${INIT_MS:-2000}
PORT=${PORT:-46200}
WORK=$(mktemp -d /tmp/iscale3.XXXXXX)
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT

now_ms() { date +%s%3N; }

start_fresh() { # $1=hb file -> echoes pid (cold instance)
    ./demo_app --init-ms "$INIT_MS" --warm-mb "$WARM_MB" >>"$1" 2>&1 &
    local p=$!
    PIDS+=("$p")
    echo "$p"
}

wait_line() { # $1=file $2=regex
    for _ in $(seq 1 1200); do
        grep -q "$2" "$1" 2>/dev/null && return 0
        sleep 0.02
    done
    echo "TIMEOUT waiting for '$2' in $1" >&2
    tail -n 8 "$1" >&2 || true
    return 1
}

lines_of()  { wc -l <"$1" 2>/dev/null | tr -d ' ' || echo 0; }
last_seq()  { awk '$1 == "HB" { split($2, a, "="); s = a[2] } END { print s + 0 }' "$1"; }
first_seq() { awk '$1 == "HB" { split($2, a, "="); print a[2]; exit }' "$1"; }

wait_activation() { # $1=hb $2=baseline_lines $3=t0_ms [$4=regex] -> ms
    local pat=${4:-.} n
    for _ in $(seq 1 3000); do
        # NOTE: plain n=$(grep -c ...) aborts under set -e when grep exits 1
        n=$(grep -c "$pat" "$1" 2>/dev/null || true)
        n=${n:-0}
        # grep -c exits 1 on zero matches even after printing "0"
        if [ "${n%%$'\n'*}" -gt "$2" ]; then
            echo $(( $(now_ms) - $3 ))
            return 0
        fi
        sleep 0.004
    done
    echo TIMEOUT; return 1
}

app_reported_ms() { # parse "activated_in=<ms>" (token is 13 chars)
    awk 'match($0, /activated_in=[0-9.]+/) {
             print substr($0, RSTART + 13, RLENGTH - 13); exit }' "$1"
}

checkpoint_instance() { # $1=pid $2=hbfile -> waits for CKPT line
    kill -USR2 "$1"
    wait_line "$2" '^CKPT '
}

run_cold() {
    local hb="$WORK/cold.log" t0 pid act
    : >"$hb"; t0=$(now_ms)
    pid=$(start_fresh "$hb")
    act=$(wait_activation "$hb" 0 "$t0")
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "RESULT mode=cold  activation_ms=$act workdir=$WORK"
}

# Shared prep for both resume modes: run instance A, checkpoint it, return
# the target-side image path via global $IMG_T and the pre-seq via $S1.
prepare_checkpoint() {
    local hbA="$WORK/a.log"
    : >"$hbA"
    local pidA
    pidA=$(start_fresh "$hbA")
    wait_line "$hbA" '^READY'
    sleep 0.45                       # accumulate a few heartbeats
    S1=$(last_seq "$hbA")

    checkpoint_instance "$pidA" "$hbA"
    wait "$pidA" 2>/dev/null || true # SIGUSR2 => snapshot & exit

    cp /tmp/demo_app.isim "$WORK/app.isim"
    IMG_T="$WORK/target.isim"        # the "network copy" of the checkpoint
    cp "$WORK/app.isim" "$IMG_T"
}

run_eager_resume() {
    prepare_checkpoint
    local hb="$WORK/e.log" t0 act s2 final
    : >"$hb"; t0=$(now_ms)
    ./demo_app --resume "$IMG_T" >>"$hb" 2>&1 &
    local pidB=$!
    PIDS+=("$pidB")
    wait_activation "$hb" 0 "$t0" '^RESUMED-EAGER' >/dev/null
    act=$(app_reported_ms "$hb")
    wait_line "$hb" '^HB '
    s2=$(first_seq "$hb")
    sleep 0.5
    kill -TERM "$pidB" 2>/dev/null || true; wait "$pidB" 2>/dev/null || true
    final=$(grep '^FINAL' "$hb" || echo "FINAL MISSING")
    echo "RESULT mode=eager activation_ms=$act pre_seq=$S1 post_seq=$s2 $final workdir=$WORK"
}

run_lazy_resume() {
    [ -x ../phase2/pageserver ] || make -C ../phase2 pageserver >/dev/null
    prepare_checkpoint
    ../phase2/pageserver --port "$PORT" --image "$IMG_T" \
        >"$WORK/ps.log" 2>&1 &
    local ps_pid=$!
    PIDS+=("$ps_pid")
    sleep 0.25

    local hb="$WORK/l.log" t0 act s2 final img_bytes
    img_bytes=$(stat -c%s "$IMG_T")
    : >"$hb"; t0=$(now_ms)
    ./demo_app --resume-lazy-img "$IMG_T" --host 127.0.0.1 \
               --port "$PORT" >>"$hb" 2>&1 &
    local pidB=$!
    PIDS+=("$pidB")
    wait_activation "$hb" 0 "$t0" '^RESUMED-LAZY' >/dev/null
    act=$(app_reported_ms "$hb")
    wait_line "$hb" '^HB '
    s2=$(first_seq "$hb")
    sleep 1.5                        # let hydration stream behind execution
    kill -TERM "$pidB" 2>/dev/null || true; wait "$pidB" 2>/dev/null || true
    final=$(grep '^FINAL' "$hb" || echo "FINAL MISSING")

    echo "RESULT mode=lazy  activation_ms=$act image_bytes=$img_bytes pre_seq=$S1 post_seq=$s2 $final workdir=$WORK"
}

case "$MODE" in
    cold)  run_cold ;;
    eager) run_eager_resume ;;
    lazy)  run_lazy_resume ;;
    all)
        run_cold
        run_eager_resume
        run_lazy_resume
        echo
        echo "workdir: $WORK"
        ;;
    *) echo "usage: $0 [cold|eager|lazy|all]"; exit 2 ;;
esac
