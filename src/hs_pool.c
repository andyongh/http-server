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
#include "hs_lua.h"
#include "hs_lua_dir.h"
#include "hs_server.h"
#include "hs_log.h"

typedef struct {
    int               id;
    pthread_t         tid;
    struct hs_server *srv;
    hs_lua_state_t   *lstate;
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

    /* Prefer lua_dir, fall back to lua_script */
    const char *lua_path = srv->config.lua_dir
        ? hs_lua_dir_main_script(srv->config.lua_dir)
        : srv->config.lua_script;

    if (lua_path) {
        w->lstate = hs_lua_state_new(lua_path);
        if (!w->lstate)
            hs_log(HS_LOG_ERROR, "worker %d: Lua init failed", w->id);
    }

    for (;;) {
        hs_work_t *work = (hs_work_t *)hs_spmc_pop(&srv->pool->work_q);
        if (!work) break;

        /* Check if Lua dir has changed; if so, reload the Lua state */
        if (w->lstate && srv->config.lua_dir &&
            hs_lua_dir_needs_reload(srv->config.lua_dir)) {
            hs_lua_state_free(w->lstate);
            const char *new_path = hs_lua_dir_main_script(srv->config.lua_dir);
            w->lstate = new_path ? hs_lua_state_new(new_path) : NULL;
        }

        hs_process_work(srv, work->conn, w->lstate);
        hs_free(work);
    }

    if (w->lstate) { hs_lua_state_free(w->lstate); w->lstate = NULL; }
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

int hs_pool_submit(hs_pool_t *p, hs_work_t *work)
{
    return hs_spmc_push(&p->work_q, work);
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
