/**
 * hs_pool.h  –  CPU worker thread pool
 */
#pragma once
#include "hs_queue.h"

struct hs_server;
struct hs_conn;
struct hs_lua_state;

typedef struct hs_pool hs_pool_t;

hs_pool_t *hs_pool_new(int nthreads, struct hs_server *srv);
int        hs_pool_submit(hs_pool_t *pool, struct hs_conn *conn);
void       hs_pool_shutdown(hs_pool_t *pool);
