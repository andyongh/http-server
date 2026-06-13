/**
 * hs_server.c  –  public API implementation
 *
 * The heavy lifting (IO loops, accept, read, write) lives in hs_reactor.c.
 * This file owns the public API surface and the server lifecycle.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <jemalloc/jemalloc.h>
#include <llhttp.h>

#include "hs_server.h"
#include "hs_pool.h"
#include "hs_http.h"
#include "hs_listener.h"
#include "hs_reactor.h"
#include "httpserver.h"

/* ── hs_config_init ──────────────────────────────────────────────────────── */
void hs_config_init(hs_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_flags    = HS_LISTEN_TCP;
    cfg->host            = "0.0.0.0";
    cfg->port            = 8080;
    cfg->backlog         = 1024;
    cfg->reactor_mode    = HS_REACTOR_SINGLE;
    cfg->num_io_threads  = 0;
    cfg->num_threads     = 0;
    cfg->conn_pool_cap   = 1024;
    cfg->max_body_size   = 4u * 1024u * 1024u;
    cfg->max_url_len     = 8192;
    cfg->max_headers     = HS_MAX_HEADERS;
}

/* ── hs_server_new ───────────────────────────────────────────────────────── */
hs_server_t *hs_server_new(const hs_config_t *cfg)
{
    hs_server_t *srv = (hs_server_t *)je_calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    srv->config = *cfg;
    atomic_init(&srv->running, 1);

    /* ── auto-detect CPU workers ── */
    if (srv->config.num_threads <= 0) {
#ifdef _SC_NPROCESSORS_ONLN
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        srv->config.num_threads = n > 0 ? (int)n : 4;
#else
        srv->config.num_threads = 4;
#endif
    }

    /* ── create listeners ── */
    srv->nlisteners = 0;

    if (cfg->listen_flags & HS_LISTEN_TCP) {
        hs_listener_t *l = &srv->listeners[srv->nlisteners];
        if (hs_listener_tcp(l, cfg->host, cfg->port, cfg->backlog) < 0)
            goto err;
        fprintf(stderr, "[server] TCP  %s:%d\n",
                cfg->host ? cfg->host : "0.0.0.0", cfg->port);
        srv->nlisteners++;
    }

    if (cfg->listen_flags & HS_LISTEN_UDS) {
        if (!cfg->uds_path || !*cfg->uds_path) {
            fprintf(stderr, "[server] HS_LISTEN_UDS set but uds_path is empty\n");
            goto err;
        }
        hs_listener_t *l = &srv->listeners[srv->nlisteners];
        if (hs_listener_uds(l, cfg->uds_path, cfg->backlog) < 0)
            goto err;
        fprintf(stderr, "[server] UDS  %s\n", cfg->uds_path);
        srv->nlisteners++;
    }

    if (srv->nlisteners == 0) {
        fprintf(stderr, "[server] No listen flags set\n");
        goto err;
    }

    /* ── reactor group ── */
    srv->rg = hs_reactor_group_new(srv);
    if (!srv->rg) goto err;

    /* ── CPU thread pool ── */
    srv->pool = hs_pool_new(srv->config.num_threads, srv);
    if (!srv->pool) goto err;

    fprintf(stderr, "[server] mode=%s  io_threads=%d  workers=%d\n",
            cfg->reactor_mode == HS_REACTOR_MULTI ? "MULTI" : "SINGLE",
            srv->rg->nsubs,
            srv->config.num_threads);

    return srv;

err:
    hs_server_free(srv);
    return NULL;
}

/* ── hs_server_run ───────────────────────────────────────────────────────── */
int hs_server_run(hs_server_t *srv)
{
    /* Ignore SIGPIPE globally (writes to closed sockets are checked via
     * return values). */
    signal(SIGPIPE, SIG_IGN);

    int rc = hs_reactor_group_start(srv->rg);

    /* Shutdown: drain and stop the worker pool */
    hs_pool_shutdown(srv->pool);
    srv->pool = NULL;

    return rc;
}

/* ── hs_server_stop ──────────────────────────────────────────────────────── */
void hs_server_stop(hs_server_t *srv)
{
    if (!atomic_exchange(&srv->running, 0)) return; /* already stopped */
    hs_reactor_group_stop(srv->rg);
}

/* ── hs_server_free ──────────────────────────────────────────────────────── */
void hs_server_free(hs_server_t *srv)
{
    if (!srv) return;
    if (srv->pool) { hs_pool_shutdown(srv->pool); srv->pool = NULL; }
    if (srv->rg)   { hs_reactor_group_free(srv->rg); srv->rg = NULL; }
    for (int i = 0; i < srv->nlisteners; i++)
        hs_listener_close(&srv->listeners[i]);
    je_free(srv);
}

/* ── request accessors ───────────────────────────────────────────────────── */

int hs_req_method(const hs_request_t *req)
{
    return ((const hs_conn_t *)req)->req.method;
}

const char *hs_req_method_str(const hs_request_t *req)
{
    return llhttp_method_name((llhttp_method_t)
                              ((const hs_conn_t *)req)->req.method);
}

const char *hs_req_url(const hs_request_t *req)
{
    return ((const hs_conn_t *)req)->req.url;
}

const char *hs_req_header(const hs_request_t *req, const char *name)
{
    const hs_conn_t     *conn = (const hs_conn_t *)req;
    const hs_parsed_req_t *r  = &conn->req;
    for (int i = 0; i < r->nheaders; i++)
        if (strcasecmp(r->headers[i].name, name) == 0)
            return r->headers[i].value;
    return NULL;
}

const char *hs_req_body(const hs_request_t *req, size_t *out_len)
{
    const hs_conn_t     *conn = (const hs_conn_t *)req;
    const hs_parsed_req_t *r  = &conn->req;

    if (out_len) *out_len = r->body_received;

    /* ── overflow path ── */
    if (r->overflow) {
        if (out_len) *out_len = r->overflow_len;
        return r->overflow;
    }

    /* ── zero-copy ring path ── */
    if (r->body_in_ring && r->body_ring_len > 0) {
        if (out_len) *out_len = r->body_ring_len;
        /* Safe: connection is INFLIGHT, no new writes to the ring */
        return conn->ring.data + r->body_ring_idx;
    }

    if (out_len) *out_len = 0;
    return "";
}

const char *hs_req_http_version(const hs_request_t *req)
{
    const hs_conn_t *conn = (const hs_conn_t *)req;
    return conn->req.http_minor == 1 ? "1.1" : "1.0";
}
