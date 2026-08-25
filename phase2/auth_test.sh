#!/usr/bin/env bash
# Production security verification: PSK auth accept/reject + graceful shutdown.
set -u
cd "$(dirname "$0")"

make clean all >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
PORT=46311
TOKFILE=/tmp/hotpod_token
printf 's3cr3t-t0k3n' > "$TOKFILE"

pass=0; fail=0
check() { # name expected_substring
    if grep -q "$2" "$3" 2>/dev/null; then echo "PASS $1"; pass=$((pass+1));
    else echo "FAIL $1 (wanted '$2')"; fail=$((fail+1)); fi
}

echo "== 1. server WITH token + client WITH token =="
./seeder /tmp/at.isim 64 >/dev/null
HOTPOD_TOKEN=s3cr3t-t0k3n ./pageserver --port $PORT --image /tmp/at.isim \
    >/tmp/aps.log 2>&1 &
SP=$!; sleep 0.4
HOTPOD_TOKEN=s3cr3t-t0k3n timeout 15 ./restorer --port $PORT >/tmp/ac1.log 2>&1
RC=$?
grep -q "VERDICT.*PASS\|SUMMARY mode=lazy" /tmp/ac1.log && [ $RC -eq 0 ] \
    && { echo "PASS auth-ok roundtrip"; pass=$((pass+1)); } \
    || { echo "FAIL auth-ok roundtrip rc=$RC"; fail=$((fail+1)); tail -3 /tmp/ac1.log; }

echo "== 2. server WITH token + client WITHOUT token =="
timeout 15 ./restorer --port $PORT >/tmp/ac2.log 2>&1
RC=$?
check "reject-no-token" "rejected\|requires a token\|Permission denied" /tmp/ac2.log
[ $RC -ne 0 ] && { echo "PASS reject-no-token rc=$RC"; pass=$((pass+1)); } \
              || { echo "FAIL reject-no-token rc=0"; fail=$((fail+1)); }

echo "== 3. server WITH token + client WRONG token =="
HOTPOD_TOKEN=wrong-token timeout 15 ./restorer --port $PORT >/tmp/ac3.log 2>&1
check "reject-wrong-token" "rejected\|requires a token\|Permission denied" /tmp/ac3.log

echo "== 4. server WITHOUT token (open mode) + plain client =="
kill $SP 2>/dev/null; wait $SP 2>/dev/null
./pageserver --port $PORT --image /tmp/at.isim >/tmp/aps2.log 2>&1 &
SP=$!; sleep 0.4
grep -q "OPEN mode" /tmp/aps2.log && { echo "PASS open-mode warning"; pass=$((pass+1)); } \
                                  || { echo "FAIL open-mode warning"; fail=$((fail+1)); }
timeout 15 ./restorer --port $PORT >/tmp/ac4.log 2>&1
grep -q "SUMMARY mode=lazy" /tmp/ac4.log && { echo "PASS open-mode roundtrip"; pass=$((pass+1)); } \
                                        || { echo "FAIL open-mode roundtrip"; fail=$((fail+1)); }

echo "== 5. graceful shutdown on SIGTERM (stats + exit 0) =="
kill -TERM $SP; wait $SP 2>/dev/null; RC=$?
check "shutdown-stats" "stats: pages=" /tmp/aps2.log
[ $RC -eq 0 ] && { echo "PASS shutdown exit0"; pass=$((pass+1)); } \
              || { echo "FAIL shutdown exit=$RC"; fail=$((fail+1)); }

echo
echo "AUTH SUITE: pass=$pass fail=$fail"
[ $fail -eq 0 ]
