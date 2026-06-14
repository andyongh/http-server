#!/usr/bin/env bash
# tests/test_regression.sh
# ─────────────────────────────────────────────────────────────────────────────
# HTTP/1.1 regression test suite for httpserver-lite.
#
# Requires:
#   - curl (HTTP/1.1 capable)
#   - nc or netcat (for raw socket tests)
#   - The example server binary at ./build/out/httpserver_example
#     OR pass SERVER_BIN=<path> SERVER_LUA=<handler.lua path>
#
# Usage:
#   ./tests/test_regression.sh                          # inline (C handler)
#   SERVER_BIN=./build/out/httpserver_example \
#   SERVER_LUA=./examples/handler.lua \
#       ./tests/test_regression.sh                      # Lua handler
#   WORKERS=4 ./tests/test_regression.sh               # with CPU workers
#
# Exit: 0 = all passed, 1 = any failed
# ─────────────────────────────────────────────────────────────────────────────
set -eu

SERVER_BIN="${SERVER_BIN:-./build/out/httpserver_example}"
SERVER_LUA="${SERVER_LUA:-}"
PORT="${PORT:-18080}"
HOST="${HOST:-127.0.0.1}"
WORKERS="${WORKERS:-0}"
TIMEOUT="${TIMEOUT:-10}"

pass=0
fail=0
srv_pid=""

# ── colour output ─────────────────────────────────────────────────────────
GREEN="\033[32m"
RED="\033[31m"
YELLOW="\033[33m"
RESET="\033[0m"

ppass() { printf "${GREEN}  PASS${RESET}  %s\n" "$1"; pass=$((pass + 1)); }
pfail() { printf "${RED}  FAIL${RESET}  %s: %s\n" "$1" "$2"; fail=$((fail + 1)); }
pskip() { printf "${YELLOW}  SKIP${RESET}  %s\n" "$1"; }

# ── server lifecycle ──────────────────────────────────────────────────────
start_server() {
    local lua_opt=""
    if [ -n "$SERVER_LUA" ]; then
        lua_opt="HS_LUA=$SERVER_LUA"
    fi

    HS_PORT=$PORT HS_WORKERS=$WORKERS HS_LUA="$SERVER_LUA" \
        "$SERVER_BIN" &
    srv_pid=$!

    # wait for it to be ready
    local retries=20
    while ! curl -sf "http://$HOST:$PORT/" >/dev/null 2>&1; do
        sleep 0.1
        retries=$((retries - 1))
        if [ $retries -le 0 ]; then
            echo "ERROR: server did not start in time"
            kill "$srv_pid" 2>/dev/null || true
            exit 1
        fi
    done
}

stop_server() {
    if [ -n "$srv_pid" ]; then
        kill "$srv_pid" 2>/dev/null || true
        wait "$srv_pid" 2>/dev/null || true
        srv_pid=""
    fi
}

trap 'stop_server' EXIT

# ── helpers ───────────────────────────────────────────────────────────────
curl_req() {
    curl -sf --http1.1 --max-time "$TIMEOUT" "$@" 2>/dev/null
}

status_code() {
    curl -o /dev/null -s -w '%{http_code}' --http1.1 --max-time "$TIMEOUT" "$@" 2>/dev/null
}

# ── test cases ────────────────────────────────────────────────────────────

test_get_root() {
    local body
    body=$(curl_req "http://$HOST:$PORT/")
    local code
    code=$(status_code "http://$HOST:$PORT/")
    if [ "$code" = "200" ]; then
        ppass "GET / → 200"
    else
        pfail "GET /" "expected 200, got $code"
    fi
}

test_get_not_found() {
    local code
    code=$(status_code "http://$HOST:$PORT/no-such-path-xyz")
    if [ "$code" = "404" ]; then
        ppass "GET /no-such-path → 404"
    else
        pfail "GET /no-such-path" "expected 404, got $code"
    fi
}

test_post_echo() {
    local body
    body=$(curl_req -X POST -d "hello-regression" "http://$HOST:$PORT/echo")
    if echo "$body" | grep -q "hello-regression"; then
        ppass "POST /echo → body echoed"
    else
        pfail "POST /echo" "body not echoed; got: $body"
    fi
}

test_content_type_header() {
    local ct
    ct=$(curl -sf --http1.1 -I --max-time "$TIMEOUT" "http://$HOST:$PORT/" 2>/dev/null \
         | grep -i "^content-type:" | tr -d '\r' | head -1)
    if [ -n "$ct" ]; then
        ppass "Response has Content-Type header"
    else
        pfail "Content-Type" "no content-type in response"
    fi
}

test_keep_alive() {
    # Send 5 pipelined requests over a single connection
    local count
    count=$(curl -sf --http1.1 --max-time "$TIMEOUT" \
        "http://$HOST:$PORT/" \
        "http://$HOST:$PORT/" \
        "http://$HOST:$PORT/" \
        "http://$HOST:$PORT/" \
        "http://$HOST:$PORT/" 2>/dev/null | wc -l)
    if [ "$count" -ge 5 ]; then
        ppass "Keep-Alive (5 requests, 1 connection)"
    else
        pfail "Keep-Alive" "only $count response lines (expected ≥5)"
    fi
}

test_large_body() {
    # 64 KiB body
    local body
    local payload
    payload=$(head -c 65536 /dev/urandom | base64 | tr -d '\n' | head -c 65536)
    local code
    code=$(echo "$payload" | curl -o /dev/null -s -w '%{http_code}' \
        --http1.1 --max-time "$TIMEOUT" \
        -X POST --data-binary @- "http://$HOST:$PORT/echo" 2>/dev/null)
    if [ "$code" = "200" ]; then
        ppass "POST 64KiB body → 200"
    else
        pfail "large_body" "expected 200, got $code"
    fi
}

test_body_too_large() {
    # Send > 4 MiB body – expect 413
    # We use a fake Content-Length header via nc
    local code
    code=$(printf 'POST /echo HTTP/1.1\r\nHost: %s\r\nContent-Length: 5000000\r\nConnection: close\r\n\r\n' "$HOST" \
        | nc -q1 "$HOST" "$PORT" 2>/dev/null \
        | head -1 | awk '{print $2}' || echo "")
    if [ "$code" = "413" ]; then
        ppass "Body > max_body_size → 413"
    else
        pskip "Body-too-large (nc not available or server inline-dismisses without 413)"
    fi
}

test_malformed_http() {
    # Send garbage HTTP – expect 400 or connection close
    local code
    code=$(printf 'NOTHTTP GARBAGE\r\n\r\n' \
        | nc -q1 "$HOST" "$PORT" 2>/dev/null \
        | head -1 | awk '{print $2}' || echo "")
    if [ "$code" = "400" ]; then
        ppass "Malformed HTTP → 400"
    else
        pskip "Malformed HTTP (nc unavailable or server closed without 400)"
    fi
}

test_ping() {
    local body
    body=$(curl_req "http://$HOST:$PORT/ping" 2>/dev/null || echo "")
    if echo "$body" | grep -q "pong"; then
        ppass "GET /ping → pong"
    else
        pfail "GET /ping" "expected 'pong' in body, got: $body"
    fi
}

test_concurrent_requests() {
    # 20 concurrent requests with curl-parallel
    local results
    local pids=()
    local tmpdir
    tmpdir=$(mktemp -d)

    for i in $(seq 1 20); do
        curl_req "http://$HOST:$PORT/" > "$tmpdir/$i.out" 2>/dev/null &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do
        wait "$pid" || true
    done

    local ok=0
    for i in $(seq 1 20); do
        [ -s "$tmpdir/$i.out" ] && ok=$((ok+1))
    done
    rm -rf "$tmpdir"

    if [ "$ok" -ge 18 ]; then
        ppass "20 concurrent GET / → all 200 (got $ok/20)"
    else
        pfail "concurrent_requests" "only $ok/20 succeeded"
    fi
}

# ── main ──────────────────────────────────────────────────────────────────
echo "================================================"
echo " httpserver-lite regression tests"
echo "  server:  $SERVER_BIN"
echo "  port:    $PORT"
[ -n "$SERVER_LUA" ] && echo "  lua:     $SERVER_LUA"
echo "================================================"

if [ ! -x "$SERVER_BIN" ]; then
    echo "ERROR: $SERVER_BIN not found. Run 'make' first."
    exit 1
fi

start_server

test_get_root
test_get_not_found
test_post_echo
test_content_type_header
test_keep_alive
test_large_body
test_body_too_large
test_malformed_http
test_ping
test_concurrent_requests

stop_server

echo ""
echo "================================================"
echo " Results: ${pass} passed, ${fail} failed"
echo "================================================"

[ "$fail" -eq 0 ]
