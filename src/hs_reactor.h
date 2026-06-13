/**
 * hs_reactor.h  –  sub-reactor (IO thread) + boss + connection pool
 *
 * fsae note: aeEventLoop has no privdata field.  Every callback receives
 * its context exclusively via the clientData argument passed to
 * aeCreateFileEvent().  We never touch el->apidata or any internal field.
 *
 * Thread-safety contract
 * ──────────────────────
 *  hs_conn_t          owned by ONE sub-reactor IO thread only.
 *                     Workers receive a pointer that becomes read-only
 *                     (req fields) once the conn is INFLIGHT.
 *
 *  hs_conn_pool_t     owned by ONE sub-reactor; no locking needed.
 *
 *  hs_mpsc_t          Vyukov lock-free; safe for N writers, 1 reader.
 *
 *  hs_spmc_t          mutex+condvar; safe for 1 writer, N readers.
 *
 *  sub->resp_efd      Linux eventfd; safe for N concurrent writers.
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
struct hs_lua_state;

/* ── per-reactor connection pool (IO-thread-owned, lock-free) ──────────── */
typedef struct {
    hs_conn_t *slots;       /* je_calloc'd array                          */
    int       *free_stk;    /* je_malloc'd free-index stack               */
    int        top;
    int        cap;
} hs_conn_pool_t;

int        hs_conn_pool_init(hs_conn_pool_t *p, int cap);
void       hs_conn_pool_destroy(hs_conn_pool_t *p);
hs_conn_t *hs_conn_pool_alloc(hs_conn_pool_t *p);
void       hs_conn_pool_free(hs_conn_pool_t *p, hs_conn_t *c);

/* ── sub-reactor ────────────────────────────────────────────────────────── */
typedef struct hs_sub_reactor {
    int               id;
    pthread_t         tid;       /* 0 in SINGLE mode (calling thread)       */
    aeEventLoop      *ae;

    /*
     * Boss → sub pipe (MULTI mode only; -1 in SINGLE mode).
     * Boss writes sizeof(int) bytes = new client fd.
     * Sentinel fd = -1 signals the sub to call aeStop().
     */
    int               wakeup_r;
    int               wakeup_w;

    /* Per-reactor response queue: workers push, IO thread pops via eventfd */
    int               resp_efd;    /* read end (eventfd on Linux, pipe on macOS) */
#ifndef __linux__
    int               resp_efd_w;  /* write end of self-pipe (macOS only)       */
#endif
    hs_mpsc_t         resp_queue;

    /* Connection pool – IO-thread-owned, no locking */
    hs_conn_pool_t    conn_pool;

    struct hs_server *srv;        /* back-pointer to the server config etc. */
} hs_sub_reactor_t;

/* ── reactor group ──────────────────────────────────────────────────────── */
typedef struct {
    hs_sub_reactor_t *subs;
    int               nsubs;

    aeEventLoop      *boss_ae;   /* MULTI mode only; NULL in SINGLE mode    */
    pthread_t         boss_tid;

    _Atomic uint32_t  rr;        /* round-robin dispatch counter            */

    struct hs_server *srv;
} hs_reactor_group_t;

/* ── lifecycle ───────────────────────────────────────────────────────────── */
hs_reactor_group_t *hs_reactor_group_new(struct hs_server *srv);
int                 hs_reactor_group_start(hs_reactor_group_t *rg);
void                hs_reactor_group_stop(hs_reactor_group_t *rg);
void                hs_reactor_group_free(hs_reactor_group_t *rg);

/* ── callbacks declared here so hs_server.h can reference them ────────── */
void hs_read_cb(aeEventLoop *el, int fd, void *clientData, int mask);
void hs_write_cb(aeEventLoop *el, int fd, void *clientData, int mask);
void hs_accept_cb_single(aeEventLoop *el, int fd, void *clientData, int mask);

/* ── cross-unit internal API ────────────────────────────────────────────── */
void hs_on_request_complete(hs_conn_t *conn);
void hs_process_work(struct hs_server *srv, hs_conn_t *conn,
                     struct hs_lua_state *lstate);
