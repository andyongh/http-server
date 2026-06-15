/**
 * examples/main.c  –  httpserver-lite v0.4-lite  Single-Reactor example
 *
 * Features demonstrated:
 *   1. TCP + UDS dual listener
 *   2. INLINE dispatch (handler in reactor thread, num_threads=0)
 *   3. WORKER dispatch (handler in CPU pool, HS_WORKERS=N env var)
 *   4. Lua handler via lua_dir (auto hot-reload)
 *   5. Zero-malloc ring buffer (transparent to the handler)
 *
 * Build:
 *   make
 *   make HS_USE_JEMALLOC=1   # with jemalloc
 *
 * Run:
 *   ./build/out/httpserver_example          # C handler, inline dispatch
 *   HS_WORKERS=4 ./build/out/...           # C handler, 4 CPU workers
 *   HS_LUA=examples/handler.lua ./build/out/...  # Lua handler
 *   HS_LUA_DIR=examples/lua ./build/out/...      # Lua dir with hot-reload
 *   REACTOR=multi ./build/out/...          # (removed in v0.4-lite – NOOP)
 *
 * Test:
 *   curl http://localhost:8080/
 *   curl -X POST http://localhost:8080/echo -d "hello"
 *   curl http://localhost:8080/ping
 *   curl --unix-socket /tmp/httpserver.sock http://localhost/
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include "httpserver.h"

static hs_server_t *g_srv = NULL;

static void on_signal(int sig)
{
    (void)sig;
    if (g_srv) hs_server_stop(g_srv);
}

/* ── C handler ───────────────────────────────────────────────────────────── */
static hs_dispatch_mode_t handle(hs_request_t *req, hs_response_t *res,
                                 void *ud)
{
    (void)ud;
    const char *url    = hs_req_url(req);
    const char *method = hs_req_method_str(req);
    hs_log(HS_LOG_INFO, "[%s] %s", method, url);

    if (strcmp(url, "/") == 0) {
        hs_res_status(res, 200);
        hs_res_header(res, "Content-Type", "text/plain");
        hs_res_body_str(res, "Hello from httpserver v0.4-lite!\n");

    } else if (strncmp(url, "/echo", 5) == 0) {
        size_t len = 0;
        const char *body = hs_req_body(req, &len);
        hs_res_status(res, 200);
        hs_res_header(res, "Content-Type", "text/plain");
        hs_res_body(res, len ? body : "(empty)\n", len ? len : 8);

    } else if (strcmp(url, "/ping") == 0) {
        hs_res_status(res, 200);
        hs_res_header(res, "Content-Type", "text/plain");
        hs_res_body_str(res, "pong\n");

    } else if (strcmp(url, "/headers") == 0) {
        const char *ct = hs_req_header(req, "Content-Type");
        const char *ua = hs_req_header(req, "User-Agent");
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "Content-Type : %s\nUser-Agent   : %s\n",
                 ct ? ct : "(none)", ua ? ua : "(none)");
        hs_res_status(res, 200);
        hs_res_header(res, "Content-Type", "text/plain");
        hs_res_body_str(res, buf);

    } else {
        hs_res_status(res, 404);
        hs_res_header(res, "Content-Type", "text/plain");
        hs_res_body_str(res, "Not Found\n");
    }

    hs_res_send(res);
    return HS_DISPATCH_INLINE;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* Parse and set minimum log level */
    const char *log_lvl_str = getenv("HS_LOG_LEVEL");
    if (log_lvl_str) {
        if (strcasecmp(log_lvl_str, "DEBUG") == 0) hs_log_set_level(HS_LOG_DEBUG);
        else if (strcasecmp(log_lvl_str, "INFO") == 0) hs_log_set_level(HS_LOG_INFO);
        else if (strcasecmp(log_lvl_str, "WARN") == 0) hs_log_set_level(HS_LOG_WARN);
        else if (strcasecmp(log_lvl_str, "ERROR") == 0) hs_log_set_level(HS_LOG_ERROR);
        else if (strcasecmp(log_lvl_str, "FATAL") == 0) hs_log_set_level(HS_LOG_FATAL);
        else if (strcasecmp(log_lvl_str, "OFF") == 0) hs_log_set_level(HS_LOG_OFF);
    }

    hs_config_t cfg;
    hs_config_init(&cfg);


    /* ── listen: TCP + optional UDS ── */
    const char *uds = getenv("HS_UDS");
    if (uds && *uds) {
        cfg.listen_flags = HS_LISTEN_TCP | HS_LISTEN_UDS;
        cfg.uds_path     = uds;
    } else {
        cfg.listen_flags = HS_LISTEN_TCP;
    }

    const char *host = getenv("HS_HOST");
    cfg.host = host && *host ? host : "0.0.0.0";

    const char *port_str = getenv("HS_PORT");
    cfg.port = port_str ? (uint16_t)atoi(port_str) : 8080;

    cfg.backlog       = 1024;
    cfg.conn_pool_cap = 2048;
    cfg.max_body_size = 4u * 1024u * 1024u;  /* 4 MiB */

    /* ── dispatch: 0 = inline, N = N worker threads, -1 = auto ── */
    const char *workers_str = getenv("HS_WORKERS");
    cfg.num_threads = workers_str ? atoi(workers_str) : 0;

    cfg.handler = handle;
    printf("[main] C handler (INLINE dispatch)\n");


    printf("[main] listening on %s:%d  workers=%d\n",
           cfg.host, cfg.port, cfg.num_threads);

    g_srv = hs_server_new(&cfg);
    if (!g_srv) { fprintf(stderr, "server_new failed\n"); return 1; }

    int rc = hs_server_run(g_srv);
    hs_server_free(g_srv);
    return rc == 0 ? 0 : 1;
}
