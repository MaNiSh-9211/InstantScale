#!/usr/bin/env bash
# HotPod Phase 2 demo â€” the whole pitch in ~10 seconds:
#   1. seed a deterministic "warm heap" checkpoint (like a CRIU image)
#   2. start the page server (source host holding the memory)
#   3. LAZY  restore: target activates instantly, pages stream on demand
#   4. EAGER restore: traditional full-copy migration, for contrast
# Both runs prove identical integrity via the rolling CRC32 digest.
set -euo pipefail
cd "$(dirname "$0")"

PORT=${PORT:-46100}
PAGES=${PAGES:-65536}          # 65536 Ã— 4 KB = 256 MB heap
TMP=$(mktemp -d)
SRV=""
cleanup() { [[ -n "$SRV" ]] && kill "$SRV" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT

echo "â”€â”€ seeding warm heap (${PAGES} pages) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€"
./seeder "$TMP/warm.isim" "$PAGES"

echo "â”€â”€ starting page server on :$PORT â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€"
./pageserver --port "$PORT" --image "$TMP/warm.isim" >"$TMP/server.log" 2>&1 &
SRV=$!
for _ in $(seq 1 100); do
    if (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then exec 3>&-; break; fi
    sleep 0.05
done

echo
echo "â•â•â•â• RUN 1: LAZY â€” HotPod activation â•â•â•â•"
IS_EAGER=0 IS_PREFETCH=4 ./restorer --port "$PORT" | tee "$TMP/lazy.log"

echo
echo "â•â•â•â• RUN 2: EAGER â€” today's copy-everything migration â•â•â•â•"
IS_EAGER=1 ./restorer --port "$PORT" | tee "$TMP/eager.log"

echo
LAZY_LINE=$(grep '^SUMMARY' "$TMP/lazy.log")
EAGER_LINE=$(grep '^SUMMARY' "$TMP/eager.log")
awk -v l="$LAZY_LINE" -v e="$EAGER_LINE" 'BEGIN {
  split(l, L); split(e, E)
  for (i in L) { n = index(L[i], "="); if (n) K[substr(L[i], 1, n-1)] = substr(L[i], n+1) }
  for (i in E) { n = index(E[i], "="); if (n) J[substr(E[i], 1, n-1)] = substr(E[i], n+1) }
  printf "\n%-16s %16s %16s   %s\n", "metric", "LAZY", "EAGER", "verdict"
  printf "%-16s %16s %16s   %s\n", "----", "----", "----", "-------"
  m = split("activate_ms ready_ms hydrate_mbps hits net", keys, " ")
  for (ki = 1; ki <= m; ki++) {
    k = keys[ki]
    verdict = ""
    if (k == "activate_ms" && K[k]+0 > 0)
      verdict = sprintf("lazy is %.0fx faster to RUNNING", J[k] / K[k])
    printf "%-16s %16s %16s   %s\n", k, K[k], J[k], verdict
  }
}'

echo
echo "server-side totals:"; grep -E "connection closed" "$TMP/server.log" || true
