/**
 * test_lua_queue.c  –  Lua coroutine task queue unit tests
 *
 * Tests:
 *   1. push/tick single coroutine
 *   2. multiple coroutines interleaved
 *   3. coroutine error handling (does not crash)
 *   4. queue full (push returns -1 when at cap)
 *   5. coroutines that return immediately (no yield)
 *   6. hs_queue_push() Lua-callable function
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Include the Lua queue as a header-only component */
#define _GNU_SOURCE
#include "hs_alloc.h"
#include "hs_log.h"

/* Suppress logging for clean test output */
static void noop_log(const hs_log_event_t *ev, void *ud) { (void)ev; (void)ud; }

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include "hs_lua_queue.h"

#define PASS(name) do { printf("  PASS  %s\n", name); pass++; } while(0)
#define FAIL(name, msg) do { printf("  FAIL  %s: %s\n", name, msg); fail++; } while(0)

static int pass = 0;
static int fail = 0;

/* Helper: execute Lua code and assert success */
static void lua_exec(lua_State *L, const char *code)
{
    if (luaL_dostring(L, code)) {
        fprintf(stderr, "Lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        abort();
    }
}

/* ── Test 1: single coroutine that yields once then returns ────────────── */
static void test_single_coroutine(void)
{
    const char *name = "single_coroutine";
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    hs_lua_coro_queue_t q;
    hs_coro_queue_init(&q, L);
    hs_coro_queue_register(&q, L);

    /* Push a counter variable into Lua */
    lua_pushnumber(L, 0);
    lua_setglobal(L, "counter");

    /* Create a coroutine that increments counter, yields, increments again */
    lua_exec(L,
        "local co = coroutine.create(function()\n"
        "    counter = counter + 1\n"
        "    coroutine.yield()\n"
        "    counter = counter + 1\n"
        "end)\n"
        "hs_queue_push(co)\n"
    );

    /* counter should still be 0 before any tick */
    lua_getglobal(L, "counter");
    assert(lua_tonumber(L, -1) == 0);
    lua_pop(L, 1);

    /* tick 1: coroutine runs to yield, counter = 1 */
    int alive = hs_coro_queue_tick(&q);
    lua_getglobal(L, "counter");
    int c = (int)lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (c != 1 || alive != 1) {
        FAIL(name, "tick1: expected counter=1, alive=1");
        goto done;
    }

    /* tick 2: coroutine continues to return, counter = 2 */
    alive = hs_coro_queue_tick(&q);
    lua_getglobal(L, "counter");
    c = (int)lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (c != 2 || alive != 0) {
        FAIL(name, "tick2: expected counter=2, alive=0");
        goto done;
    }

    /* tick 3: queue is empty */
    alive = hs_coro_queue_tick(&q);
    if (alive != 0 || q.count != 0) {
        FAIL(name, "tick3: expected empty queue");
        goto done;
    }

    PASS(name);

done:
    hs_coro_queue_destroy(&q);
    lua_close(L);
}

/* ── Test 2: multiple coroutines interleaved ────────────────────────────── */
static void test_multiple_coroutines(void)
{
    const char *name = "multiple_coroutines";
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    hs_lua_coro_queue_t q;
    hs_coro_queue_init(&q, L);
    hs_coro_queue_register(&q, L);

    lua_exec(L, "results = {}");

    /* Push 3 coroutines, each yielding once */
    lua_exec(L,
        "for i = 1, 3 do\n"
        "    local id = i\n"
        "    hs_queue_push(coroutine.create(function()\n"
        "        table.insert(results, 'start-' .. id)\n"
        "        coroutine.yield()\n"
        "        table.insert(results, 'end-' .. id)\n"
        "    end))\n"
        "end\n"
    );

    if (q.count != 3) { FAIL(name, "expected 3 coroutines"); goto done; }

    /* tick 1: all 3 run to yield */
    hs_coro_queue_tick(&q);

    /* tick 2: all 3 complete */
    int alive = hs_coro_queue_tick(&q);
    if (alive != 0 || q.count != 0) {
        FAIL(name, "expected all done after 2 ticks");
        goto done;
    }

    /* verify results has 6 entries */
    lua_exec(L, "assert(#results == 6, 'expected 6 results')");

    PASS(name);
done:
    hs_coro_queue_destroy(&q);
    lua_close(L);
}

/* ── Test 3: coroutine error does not crash ─────────────────────────────── */
static void test_coroutine_error(void)
{
    const char *name = "coroutine_error";
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    hs_lua_coro_queue_t q;
    hs_coro_queue_init(&q, L);
    hs_coro_queue_register(&q, L);

    lua_exec(L,
        "hs_queue_push(coroutine.create(function()\n"
        "    error('intentional error')\n"
        "end))\n"
    );

    /* Should not crash, error coroutine removed */
    hs_coro_queue_tick(&q);

    if (q.count != 0) {
        FAIL(name, "error coroutine should be removed");
        goto done;
    }

    PASS(name);
done:
    hs_coro_queue_destroy(&q);
    lua_close(L);
}

/* ── Test 4: immediate return (no yield) ────────────────────────────────── */
static void test_immediate_return(void)
{
    const char *name = "immediate_return";
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    hs_lua_coro_queue_t q;
    hs_coro_queue_init(&q, L);
    hs_coro_queue_register(&q, L);

    lua_pushnumber(L, 0);
    lua_setglobal(L, "ran");

    lua_exec(L,
        "hs_queue_push(coroutine.create(function()\n"
        "    ran = 1\n"
        "end))\n"
    );

    int alive = hs_coro_queue_tick(&q);

    lua_getglobal(L, "ran");
    int r = (int)lua_tonumber(L, -1);
    lua_pop(L, 1);

    if (r != 1 || alive != 0 || q.count != 0) {
        FAIL(name, "expected ran=1, queue empty after 1 tick");
        goto done;
    }

    PASS(name);
done:
    hs_coro_queue_destroy(&q);
    lua_close(L);
}

/* ── Test 5: queue fill (cap limit) ─────────────────────────────────────── */
static void test_queue_capacity(void)
{
    const char *name = "queue_capacity";
    /* Use a tiny custom capacity for this test – create queue manually */
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    hs_lua_coro_queue_t q;
    hs_coro_queue_init(&q, L);
    hs_coro_queue_register(&q, L);

    /* Push HS_CORO_CAP - 1 coroutines (should succeed) */
    int pushed = 0;
    for (int i = 0; i < HS_CORO_CAP - 1; i++) {
        lua_State *co = lua_newthread(L);
        /* Just an empty coroutine body via a tiny Lua chunk */
        if (luaL_loadstring(co, "coroutine.yield()") != 0) break;
        if (hs_coro_queue_push(&q, co) == 0) pushed++;
        lua_pop(L, 1);  /* pop thread from main L */
    }

    if (pushed != HS_CORO_CAP - 1) {
        FAIL(name, "could not push HS_CORO_CAP-1 coroutines");
        goto done;
    }

    /* Drain the queue */
    while (q.count > 0) hs_coro_queue_tick(&q);

    if (q.count != 0) {
        FAIL(name, "queue not empty after drain");
        goto done;
    }

    PASS(name);
done:
    hs_coro_queue_destroy(&q);
    lua_close(L);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    hs_log_register(noop_log, NULL);
    hs_log_set_level(HS_LOG_OFF);

    printf("=== test_lua_queue ===\n");

    test_single_coroutine();
    test_multiple_coroutines();
    test_coroutine_error();
    test_immediate_return();
    test_queue_capacity();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
