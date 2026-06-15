/**
 * hs_reactor.h  –  Single-Reactor: IO event loop + connection pool
 *
 * v0.4-lite: Multi-Reactor removed. Only one reactor runs in the calling
 * thread (or, with HS_DISPATCH_WORKER, a shared CPU thread pool is used).
 *
 * fsae note: aeEventLoop has no privdata field.  Every callback receives
 * its context exclusively via the clientData argument passed to
 * aeCreateFileEvent().
 *
 * Thread-safety contract
 * ──────────────────────
 *  hs_conn_t        owned by the single reactor IO thread only.
 *                   Workers receive a read-only pointer once INFLIGHT.
 *
 *  hs_conn_pool_t   owned by the reactor; no locking needed.
 *
 *  hs_mpsc_t        Vyukov lock-free; safe for N writers, 1 reader.
 *
 *  hs_spmc_t        mutex+condvar; safe for 1 writer, N readers.
 *
 *  reactor->resp_efd  Linux eventfd / macOS pipe; N-writer-safe.
 *
 *  srv->config        read-only after hs_server_new(); no locking needed.
 *
 *  srv->running       atomic_int; signal-handler safe.
 */
#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "ae.h"       /* fsae single-header – no AE_IMPLEMENTATION here */

#include "hs_queue.h"
#include "hs_conn.h"

struct hs_server;
struct hs_pool;


/* ── per-reactor connection pool (IO-thread-owned, lock-free) ──────────── */
typedef struct {
    hs_conn_t *slots;       /* calloc'd array                             */
    int       *free_stk;    /* malloc'd free-index stack                  */
    int        top;
    int        cap;
} hs_conn_pool_t;

int        hs_conn_pool_init(hs_conn_pool_t *p, int cap);
void       hs_conn_pool_destroy(hs_conn_pool_t *p);
hs_conn_t *hs_conn_pool_alloc(hs_conn_pool_t *p);
void       hs_conn_pool_free(hs_conn_pool_t *p, hs_conn_t *c);

/* ── reactor (single IO thread) ─────────────────────────────────────────── */
typedef struct hs_reactor {
    int               id;            /* always 0 in single-reactor mode    */
    aeEventLoop      *ae;

    /* Per-reactor response queue: workers push, IO thread pops via eventfd */
    int               resp_efd;    /* read end (eventfd on Linux, pipe on macOS) */
#ifndef __linux__
    int               resp_efd_w;  /* write end of self-pipe (macOS only)       */
#endif
    hs_mpsc_t         resp_queue;

    /* Connection pool – IO-thread-owned, no locking */
    hs_conn_pool_t    conn_pool;

    struct hs_server *srv;        /* back-pointer to the server config etc. */
} hs_reactor_t;

/* ── lifecycle ───────────────────────────────────────────────────────────── */
hs_reactor_t *hs_reactor_new(struct hs_server *srv);
int           hs_reactor_start(hs_reactor_t *r);   /* blocks until stopped */
void          hs_reactor_stop(hs_reactor_t *r);
void          hs_reactor_free(hs_reactor_t *r);

/* ── callbacks declared here so hs_server.h can reference them ─────────── */
void hs_read_cb(aeEventLoop *el, int fd, void *clientData, int mask);
void hs_write_cb(aeEventLoop *el, int fd, void *clientData, int mask);
void hs_accept_cb(aeEventLoop *el, int fd, void *clientData, int mask);

/* ── cross-unit internal API ─────────────────────────────────────────────── */
void hs_on_request_complete(hs_conn_t *conn);
void hs_process_work(struct hs_server *srv, hs_conn_t *conn);
