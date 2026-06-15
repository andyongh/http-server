/**
 * hs_pool.c  –  CPU worker thread pool  (v0.4-lite)
 *
 * Changes from v0.3:
 *   - Uses hs_alloc.h shim (system malloc or jemalloc)
 *   - Removed reference to hs_sub_reactor (now hs_reactor)
 *   - Pool is fully optional: hs_server_new() skips pool creation
 *     when num_threads == 0 (inline dispatch)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "hs_alloc.h"
#include "hs_pool.h"
#include "hs_reactor.h"
#include "hs_server.h"
#include "hs_log.h"

typedef struct {
    int               id;
    pthread_t         tid;
    struct hs_server *srv;
} hs_worker_t;

struct hs_pool {
    hs_spmc_t    work_q;
    int          nthreads;
    hs_worker_t *workers;
};


static void *worker_fn(void *arg)
{
    hs_worker_t  *w   = (hs_worker_t *)arg;
    hs_server_t  *srv = w->srv;

    for (;;) {
        hs_conn_t *conn = (hs_conn_t *)hs_spmc_pop(&srv->pool->work_q);
        if (!conn) break;

        hs_process_work(srv, conn);
    }

    return NULL;
}


hs_pool_t *hs_pool_new(int nthreads, struct hs_server *srv)
{
    hs_pool_t *p = (hs_pool_t *)hs_calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->nthreads = nthreads;
    hs_spmc_init(&p->work_q);
    p->workers = (hs_worker_t *)hs_calloc(nthreads, sizeof(hs_worker_t));
    if (!p->workers) { hs_free(p); return NULL; }

    srv->pool = p;   /* wire back before threads start */

    for (int i = 0; i < nthreads; i++) {
        p->workers[i].id  = i;
        p->workers[i].srv = srv;
        if (pthread_create(&p->workers[i].tid, NULL, worker_fn,
                           &p->workers[i]) != 0) {
            hs_log(HS_LOG_ERROR, "pthread_create(worker): %s", strerror(errno));
            p->nthreads = i;
            break;
        }
    }
    return p;
}

int hs_pool_submit(hs_pool_t *p, hs_conn_t *conn)
{
    return hs_spmc_push(&p->work_q, conn);
}

void hs_pool_shutdown(hs_pool_t *p)
{
    if (!p) return;
    hs_spmc_close(&p->work_q);
    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->workers[i].tid, NULL);
    hs_spmc_destroy(&p->work_q);
    hs_free(p->workers);
    hs_free(p);
}
