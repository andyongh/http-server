/**
 * hs_lua_queue.h  –  Lua coroutine task queue
 *
 * A lightweight, single-threaded coroutine scheduler that integrates with
 * the reactor event loop (ae.h).
 *
 * Design:
 *   - Tasks are Lua coroutines (lua_State threads created via lua_newthread).
 *   - The queue holds up to HS_CORO_CAP coroutines in a ring buffer.
 *   - hs_coro_resume_all() is called periodically (e.g., from a before-sleep
 *     hook or an ae timer) to advance all runnable coroutines one step.
 *   - A coroutine that calls coroutine.yield() is re-queued for the next
 *     call to hs_coro_resume_all().
 *   - A coroutine that returns or errors is removed from the queue.
 *
 * Usage in Lua:
 *
 *   -- Push a task from your handler:
 *   local coro = coroutine.wrap(function()
 *       -- do something async
 *       coroutine.yield()   -- suspend, resume later
 *       -- continue...
 *   end)
 *   hs_queue_push(coro)   -- C function registered in Lua state
 *
 * Thread-safety:
 *   This queue is per lua_State and must only be used from ONE thread
 *   (the reactor thread or a single worker). For multi-thread use,
 *   create one hs_lua_coro_queue_t per thread.
 *
 * Integration with ae event loop:
 *   Call hs_coro_queue_tick(q) from a before-sleep hook:
 *
 *     aeSetBeforeSleepProc(loop, my_before_sleep);
 *
 *     void my_before_sleep(aeEventLoop *el) {
 *         hs_coro_queue_tick(global_queue);
 *     }
 */
#pragma once

#include <stddef.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "hs_log.h"

/* ── configuration ───────────────────────────────────────────────────────── */
#ifndef HS_CORO_CAP
#define HS_CORO_CAP 256   /* max simultaneous coroutines; must be power of 2 */
#endif

/* ── coroutine queue ─────────────────────────────────────────────────────── */
typedef struct {
    lua_State *L;            /* owner lua_State (for lua_tothread) */
    int        registry_refs[HS_CORO_CAP]; /* LUA_REGISTRYINDEX refs */
    int        head;         /* next pop index                     */
    int        tail;         /* next push index                    */
    int        count;        /* number of live coroutines          */
} hs_lua_coro_queue_t;

/**
 * Initialise the queue.  Must be called once per lua_State.
 */
static inline void hs_coro_queue_init(hs_lua_coro_queue_t *q, lua_State *L)
{
    memset(q, 0, sizeof(*q));
    q->L = L;
    for (int i = 0; i < HS_CORO_CAP; i++)
        q->registry_refs[i] = LUA_NOREF;
}

/**
 * Push a coroutine thread onto the queue.
 * The coroutine must be on top of the Lua stack (a lua_State * from
 * lua_newthread).
 *
 * Returns 0 on success, -1 if the queue is full.
 *
 * After calling, the caller may pop the thread from the stack (the queue
 * holds a registry reference to prevent GC).
 */
static inline int hs_coro_queue_push(hs_lua_coro_queue_t *q, lua_State *coro)
{
    if (q->count >= HS_CORO_CAP) {
        hs_log(HS_LOG_WARN, "[coro_queue] full (cap=%d)", HS_CORO_CAP);
        return -1;
    }

    /* Store the coroutine in the registry to prevent GC */
    lua_pushthread(coro);              /* push coro thread onto coro's stack */
    lua_xmove(coro, q->L, 1);         /* move to parent L                   */
    int ref = luaL_ref(q->L, LUA_REGISTRYINDEX);

    int idx = q->tail & (HS_CORO_CAP - 1);
    q->registry_refs[idx] = ref;
    q->tail++;
    q->count++;
    return 0;
}

/**
 * Resume all queued coroutines once.  Coroutines that yield are kept in
 * the queue.  Coroutines that return or error are removed.
 *
 * Call this from the before-sleep hook or a timer callback.
 *
 * Returns the number of coroutines still alive after the tick.
 */
static inline int hs_coro_queue_tick(hs_lua_coro_queue_t *q)
{
    if (q->count == 0) return 0;

    int processed = q->count;   /* snapshot: only process existing ones */
    int alive      = 0;

    for (int i = 0; i < processed; i++) {
        int idx = (q->head + i) & (HS_CORO_CAP - 1);
        int ref = q->registry_refs[idx];
        if (ref == LUA_NOREF) continue;

        /* Retrieve coroutine from registry */
        lua_rawgeti(q->L, LUA_REGISTRYINDEX, ref);
        lua_State *coro = lua_tothread(q->L, -1);
        lua_pop(q->L, 1);

        if (!coro) {
            luaL_unref(q->L, LUA_REGISTRYINDEX, ref);
            q->registry_refs[idx] = LUA_NOREF;
            continue;
        }

        /* Resume one step */
        int nres = 0;
#if LUA_VERSION_NUM >= 504
        int status = lua_resume(coro, q->L, 0, &nres);
#elif LUA_VERSION_NUM >= 502
        int status = lua_resume(coro, q->L, 0);
        nres = lua_gettop(coro);
#else
        int status = lua_resume(coro, 0);
        nres = lua_gettop(coro);
#endif
        lua_pop(coro, nres);   /* discard yielded/returned values */

        if (status == LUA_YIELD) {
            /* Coroutine yielded: keep in queue */
            alive++;
        } else {
            /* Coroutine finished (OK) or errored */
            if (status != LUA_OK) {
                hs_log(HS_LOG_WARN, "[coro_queue] coroutine error: %s",
                       lua_tostring(coro, -1));
            }
            luaL_unref(q->L, LUA_REGISTRYINDEX, ref);
            q->registry_refs[idx] = LUA_NOREF;
            q->count--;
        }
    }

    /* Compact the queue: remove consumed slots from head */
    while (q->head != q->tail &&
           q->registry_refs[q->head & (HS_CORO_CAP - 1)] == LUA_NOREF) {
        q->head++;
    }

    return alive;
}

/**
 * Destroy the queue, unreffing all remaining coroutines.
 */
static inline void hs_coro_queue_destroy(hs_lua_coro_queue_t *q)
{
    for (int i = 0; i < HS_CORO_CAP; i++) {
        if (q->registry_refs[i] != LUA_NOREF) {
            luaL_unref(q->L, LUA_REGISTRYINDEX, q->registry_refs[i]);
            q->registry_refs[i] = LUA_NOREF;
        }
    }
    q->count = q->head = q->tail = 0;
}

/**
 * C function to register in Lua as "hs_queue_push(coro)".
 * Registers a Lua coroutine thread into the queue.
 *
 * Usage:
 *   hs_coro_queue_t *q = ...;
 *   lua_pushlightuserdata(L, q);
 *   lua_pushcclosure(L, hs_lua_queue_push_fn, 1);
 *   lua_setglobal(L, "hs_queue_push");
 */
static inline int hs_lua_queue_push_fn(lua_State *L)
{
    hs_lua_coro_queue_t *q =
        (hs_lua_coro_queue_t *)lua_touserdata(L, lua_upvalueindex(1));
    luaL_checktype(L, 1, LUA_TTHREAD);
    lua_State *coro = lua_tothread(L, 1);
    int rc = hs_coro_queue_push(q, coro);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/**
 * Register the hs_queue_push function in the given Lua state.
 * Call once after creating a lua_State to expose the queue to Lua scripts.
 */
static inline void hs_coro_queue_register(hs_lua_coro_queue_t *q, lua_State *L)
{
    lua_pushlightuserdata(L, q);
    lua_pushcclosure(L, hs_lua_queue_push_fn, 1);
    lua_setglobal(L, "hs_queue_push");
}
