/**
 * examples/main.c  –  demonstrate all three new features:
 *   1. TCP + UDS dual listener
 *   2. Single vs Multi reactor (toggle via -m flag)
 *   3. Zero-malloc ring buffer (transparent to the handler)
 *
 * Build:  cmake -S .. -B ../build && cmake --build ../build
 *
 * Test TCP:  curl http://localhost:8080/
 *            curl -X POST http://localhost:8080/echo -d "hello"
 *
 * Test UDS:  curl --unix-socket /tmp/httpserver.sock http://localhost/
 *
 * Toggle multi-reactor:  REACTOR=multi ./build/example_hello
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "httpserver.h"

static hs_server_t *g_srv = NULL;

static void on_signal(int sig)
{
    (void)sig;
    if (g_srv) hs_server_stop(g_srv);
}

/* ── handler ─────────────────────────────────────────────────────────────── */
static void handle(hs_request_t *req, hs_response_t *res, void *ud)
{
    (void)ud;
    const char *url    = hs_req_url(req);
    const char *method = hs_req_method_str(req);
    printf("[%s] %s\n", method, url);

    if (strcmp(url, "/") == 0) {
        hs_res_status(res, 200);
        hs_res_header(res, "Content-Type", "text/plain");
        hs_res_body_str(res, "Hello from httpserver v2!\n");

    } else if (strncmp(url, "/echo", 5) == 0) {
        size_t len = 0;
        const char *body = hs_req_body(req, &len);
        hs_res_status(res, 200);
        hs_res_header(res, "Content-Type", "text/plain");
        hs_res_body(res, len ? body : "(empty)\n", len ? len : 8);

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
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    hs_config_t cfg;
    hs_config_init(&cfg);

    /* ── dual listener: TCP + UDS ── */
    cfg.listen_flags = HS_LISTEN_TCP | HS_LISTEN_UDS;
    cfg.host         = "0.0.0.0";
    cfg.port         = 8080;
    cfg.uds_path     = "/tmp/httpserver.sock";
    cfg.backlog      = 1024;

    /* ── reactor model: set REACTOR=multi in env to use MULTI ── */
    const char *mode = getenv("REACTOR");
    if (mode && strcmp(mode, "multi") == 0) {
        cfg.reactor_mode   = HS_REACTOR_MULTI;
        cfg.num_io_threads = 0;   /* auto = nproc */
        printf("[main] reactor mode: MULTI\n");
    } else {
        cfg.reactor_mode = HS_REACTOR_SINGLE;
        printf("[main] reactor mode: SINGLE\n");
    }

    cfg.num_threads  = 0;             /* CPU workers: auto */
    cfg.conn_pool_cap = 2048;
    cfg.max_body_size = 1 * 1024 * 1024;  /* 1 MiB */

    cfg.handler = handle;

    g_srv = hs_server_new(&cfg);
    if (!g_srv) { fprintf(stderr, "server_new failed\n"); return 1; }

    int rc = hs_server_run(g_srv);
    hs_server_free(g_srv);
    return rc == 0 ? 0 : 1;
}
