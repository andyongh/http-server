#!/usr/bin/env bash
# tests/bench_wrk.sh
# ─────────────────────────────────────────────────────────────────────────────
# wrk benchmark script for httpserver-lite.
#
# Requirements:
#   - wrk (https://github.com/wg/wrk)
#   - The example server binary built and running OR auto-start mode
#
# Usage:
#   ./tests/bench_wrk.sh                    # auto-start with C handler
#   SERVER_RUNNING=1 PORT=8080 \
#       ./tests/bench_wrk.sh               # use already-running server
#   SERVER_LUA=examples/handler.lua \
#       ./tests/bench_wrk.sh               # auto-start with Lua handler
#   WORKERS=4 ./tests/bench_wrk.sh        # with CPU worker pool
#
# Environment variables:
#   SERVER_BIN      Path to the server binary  (default: ./build/out/httpserver_example)
#   SERVER_LUA      Lua handler path           (default: none = C handler)
#   PORT            Bind port                  (default: 18080)
#   WORKERS         CPU worker threads         (default: 0 = inline)
#   WRK_DURATION    Benchmark duration         (default: 10s)
#   WRK_CONNS       Concurrent connections     (default: 100)
#   WRK_THREADS     wrk thread count           (default: 4)
#   SERVER_RUNNING  Set to 1 to skip start     (default: 0)
#
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SERVER_BIN="${SERVER_BIN:-./build/out/httpserver_example}"
SERVER_LUA="${SERVER_LUA:-}"
PORT="${PORT:-18080}"
HOST="${HOST:-127.0.0.1}"
WORKERS="${WORKERS:-0}"
WRK_DURATION="${WRK_DURATION:-10s}"
WRK_CONNS="${WRK_CONNS:-100}"
WRK_THREADS="${WRK_THREADS:-4}"
SERVER_RUNNING="${SERVER_RUNNING:-0}"

srv_pid=""

# ── prerequisites check ───────────────────────────────────────────────────
if ! command -v wrk >/dev/null 2>&1; then
    echo "ERROR: 'wrk' not found. Install it:"
    echo "  macOS: brew install wrk"
    echo "  Linux: apt install wrk  OR  build from source"
    exit 1
fi

# ── server lifecycle ──────────────────────────────────────────────────────
start_server() {
    if [ "$SERVER_RUNNING" = "1" ]; then
        echo "[bench] using already-running server on $HOST:$PORT"
        return
    fi

    if [ ! -x "$SERVER_BIN" ]; then
        echo "ERROR: $SERVER_BIN not found. Run 'make' first."
        exit 1
    fi

    local lua_opt=""
    if [ -n "$SERVER_LUA" ]; then
        lua_opt="HS_LUA=$SERVER_LUA"
    fi

    HS_PORT=$PORT HS_WORKERS=$WORKERS $lua_opt "$SERVER_BIN" &
    srv_pid=$!

    echo "[bench] started server PID=$srv_pid port=$PORT workers=$WORKERS"

    local retries=30
    while ! curl -sf "http://$HOST:$PORT/" >/dev/null 2>&1; do
        sleep 0.1
        retries=$((retries - 1))
        if [ $retries -le 0 ]; then
            echo "ERROR: server did not start"
            kill "$srv_pid" 2>/dev/null || true
            exit 1
        fi
    done
    echo "[bench] server ready"
}

stop_server() {
    if [ -n "$srv_pid" ]; then
        kill "$srv_pid" 2>/dev/null || true
        wait "$srv_pid" 2>/dev/null || true
        echo "[bench] server stopped"
    fi
}

trap 'stop_server' EXIT

# ── benchmark runner ──────────────────────────────────────────────────────
run_wrk() {
    local label="$1"
    local url="$2"
    local method="${3:-GET}"
    local extra="${4:-}"

    echo ""
    echo "──────────────────────────────────────────"
    printf "  %-30s %s\n" "$label" "$url"
    echo "  threads=$WRK_THREADS  conns=$WRK_CONNS  duration=$WRK_DURATION"
    echo "──────────────────────────────────────────"

    # shellcheck disable=SC2086
    wrk -t"$WRK_THREADS" -c"$WRK_CONNS" -d"$WRK_DURATION" \
        -H "Connection: keep-alive" \
        $extra \
        "$url" 2>&1

    echo ""
}

# ── wrk Lua script for POST /echo ─────────────────────────────────────────
WRK_POST_SCRIPT=$(mktemp /tmp/wrk_post_XXXXXX.lua)
cat > "$WRK_POST_SCRIPT" <<'EOF'
wrk.method = "POST"
wrk.body   = "hello from wrk benchmark"
wrk.headers["Content-Type"] = "text/plain"
EOF

# ── main ──────────────────────────────────────────────────────────────────
echo "================================================"
echo " httpserver-lite wrk benchmark"
echo "================================================"
echo "  target:   http://$HOST:$PORT"
echo "  server:   $SERVER_BIN"
[ -n "$SERVER_LUA" ] && echo "  lua:      $SERVER_LUA"
echo "  workers:  $WORKERS"
echo "  wrk:      -t$WRK_THREADS -c$WRK_CONNS -d$WRK_DURATION"
echo "================================================"

start_server

# Warmup
echo "[bench] warming up..."
curl -sf "http://$HOST:$PORT/" >/dev/null

# ── Benchmarks ────────────────────────────────────────────────────────────
run_wrk "GET / (Hello)" \
    "http://$HOST:$PORT/"

run_wrk "GET /ping" \
    "http://$HOST:$PORT/ping"

run_wrk "POST /echo (24B body)" \
    "http://$HOST:$PORT/echo" \
    "POST" \
    "-s $WRK_POST_SCRIPT"

# ── Summary ───────────────────────────────────────────────────────────────
echo "================================================"
echo " Benchmark complete"
echo " Hint: increase WORKERS=N for CPU-bound handlers"
echo "================================================"

rm -f "$WRK_POST_SCRIPT"
