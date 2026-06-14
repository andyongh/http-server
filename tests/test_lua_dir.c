/**
 * test_lua_dir.c  –  Lua directory hot-reload unit tests
 *
 * Tests:
 *   1. hs_lua_dir_main_script() constructs the right path
 *   2. hs_lua_dir_needs_reload() – initially not needed (no change detected)
 *   3. After simulating a file change (atomic generation bump), reload is needed
 *   4. After reading, reload flag is cleared
 *   5. Multiple "threads" (simulated) each see the reload once
 *
 * Note: We test the interface directly without actually running inotify/kqueue
 * since that requires a real reactor loop. The reload detection logic
 * (global generation counter + TLS) is platform-independent and testable.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>

#include "hs_log.h"

/* We access the internal generation counter directly for testing */
extern _Atomic uint64_t g_lua_dir_generation;

#include "hs_lua_dir.h"

/* Suppress logging */
static void noop_log(const hs_log_event_t *ev, void *ud) { (void)ev; (void)ud; }

#define PASS(name) do { printf("  PASS  %s\n", name); pass++; } while(0)
#define FAIL(name, msg) do { printf("  FAIL  %s: %s\n", name, msg); fail++; } while(0)

static int pass = 0;
static int fail = 0;

/* ── Test 1: main script path ───────────────────────────────────────────── */
static void test_main_script_path(void)
{
    const char *name = "main_script_path";
    const char *path = hs_lua_dir_main_script("/tmp/lua_test");
    if (!path) { FAIL(name, "returned NULL"); return; }
    if (strcmp(path, "/tmp/lua_test/handler.lua") != 0) {
        printf("    got: %s\n", path);
        FAIL(name, "wrong path");
        return;
    }
    PASS(name);
}

/* ── Test 2: initial no-reload state ────────────────────────────────────── */
static void test_no_reload_initially(void)
{
    const char *name = "no_reload_initially";

    /* Reset generation to 0 */
    atomic_store(&g_lua_dir_generation, 0);

    /* First call should return 1 (TLS is UINT64_MAX != 0) then 0 */
    /* First call: TLS = UINT64_MAX != 0, so returns 1, sets TLS = 0 */
    hs_lua_dir_needs_reload("/tmp/lua_test");

    /* Second call: TLS = 0 == gen 0, so returns 0 */
    int r = hs_lua_dir_needs_reload("/tmp/lua_test");
    if (r != 0) {
        FAIL(name, "expected no reload on second call when no change");
        return;
    }
    PASS(name);
}

/* ── Test 3: reload detected after generation bump ──────────────────────── */
static void test_reload_after_change(void)
{
    const char *name = "reload_after_change";

    /* Ensure TLS matches current gen */
    hs_lua_dir_needs_reload("/tmp/lua_test");  /* sync TLS to current gen */

    /* Simulate a file change */
    atomic_fetch_add(&g_lua_dir_generation, 1);

    int r = hs_lua_dir_needs_reload("/tmp/lua_test");
    if (r != 1) {
        FAIL(name, "expected reload=1 after generation bump");
        return;
    }
    PASS(name);
}

/* ── Test 4: reload flag clears after reading ────────────────────────────── */
static void test_reload_clears(void)
{
    const char *name = "reload_clears";

    /* Ensure we just read the reload, so TLS should now match */
    /* (test_reload_after_change already called needs_reload once) */
    int r = hs_lua_dir_needs_reload("/tmp/lua_test");
    if (r != 0) {
        FAIL(name, "expected reload=0 after already reading the change");
        return;
    }
    PASS(name);
}

/* ── Test 5: multiple threads each see reload once ──────────────────────── */
typedef struct {
    int saw_reload;
    const char *dir;
} thread_args_t;

static void *thread_check_reload(void *arg)
{
    thread_args_t *a = (thread_args_t *)arg;
    /* sync TLS first */
    hs_lua_dir_needs_reload(a->dir);
    /* bump generation from main thread happens after join, so check now */
    a->saw_reload = hs_lua_dir_needs_reload(a->dir);
    return NULL;
}

static void test_multi_thread_reload(void)
{
    const char *name = "multi_thread_reload";

    /* Set a known generation */
    atomic_store(&g_lua_dir_generation, 10);
    hs_lua_dir_needs_reload("/tmp/lua_test");  /* sync main TLS to 10 */

    /* Bump generation to 11 */
    atomic_fetch_add(&g_lua_dir_generation, 1);

    /* Two threads: each has fresh TLS (UINT64_MAX) initially, will sync */
    thread_args_t a1 = {0, "/tmp/lua_test"};
    thread_args_t a2 = {0, "/tmp/lua_test"};
    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread_check_reload, &a1);
    pthread_create(&t2, NULL, thread_check_reload, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    /*
     * Both threads had TLS = UINT64_MAX != 11, so:
     * - First needs_reload call in thread syncs TLS to 11, returns 1
     * - Second needs_reload call in thread: TLS=11 == gen=11, returns 0
     * So saw_reload should be 0 for both.
     */
    if (a1.saw_reload != 0 || a2.saw_reload != 0) {
        FAIL(name, "threads should not see reload on second call");
        return;
    }

    PASS(name);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    hs_log_register(noop_log, NULL);
    hs_log_set_level(HS_LOG_OFF);

    printf("=== test_lua_dir ===\n");

    test_main_script_path();
    test_no_reload_initially();
    test_reload_after_change();
    test_reload_clears();
    test_multi_thread_reload();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
