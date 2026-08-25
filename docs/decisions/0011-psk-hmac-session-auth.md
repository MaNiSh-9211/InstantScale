# 0011: PSK + HMAC-SHA256 session auth on the migration wire

## Status: Accepted

## Context
The pageserver happily served any connecting client — acceptable for a lab,
unacceptable for production where a checkpoint represents live service state.
Full TLS/mTLS is the long-term transport goal but adds certificate lifecycle
weight; a deployment-grade stopgap was needed immediately, without external
crypto dependencies in the C core.

## Decision
Pre-shared key (PSK) sessions with symmetric HMAC-SHA256 proof:
* Key distributed out-of-band (K8s Secret, token file, `HOTPOD_TOKEN` env).
* Client → `AUTH{nonce_c ‖ HMAC(token, nonce_c‖"C")}` must be the **first**
  frame; servers configured with a token reject anything else with an empty
  `RSP_AUTH` marker so tokenless clients get an actionable error.
* Server → `AUTH{nonce_s ‖ HMAC(token, nonce_s‖"S")}` proves server side.
* Constant-time tag comparison; nonces from `/dev/urandom`; replay guard
  (one AUTH per connection); SHA-256/HMAC implemented in-tree (no OpenSSL).

## Consequences
Easier: one secret to rotate (restart both ends); auth failures produce
distinct operator-actionable messages (`token rejected`, `requires a token`);
CI suite `auth_test.sh` covers accept / no-token / wrong-token / open-mode /
graceful-shutdown (8/8). Harder: plaintext transport — tokens authenticate
but do not encrypt page payloads; TLS or a WireGuard mesh is required before
any exposure beyond a private network (documented in PRODUCTION.md §9).
PSK sharing scales poorly past a handful of tenants — per-tenant keys or a
token-service belong on the roadmap with TLS.
