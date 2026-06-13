/**
 * tests/test_queue.c  –  MPSC and SPMC queue thread-safety tests
 *
 * Build standalone:
 *   gcc -std=c11 -Wall -g -D_GNU_SOURCE -Isrc \
 *       -I build/deps/jemalloc/include \
 *       build/deps/jemalloc/lib/libjemalloc.a \
 *       tests/test_queue.c -lpthread -o test_queue
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>

#include <jemalloc/jemalloc.h>
#include "hs_queue.h"

/* ── helper ─────────────────────────────────────────────────────────────── */
static int failures = 0;
#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr, "FAIL  %s:%d  %s\n",__FILE__,__LINE__,#expr);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* ══════════════════════════════════════════════════════════════════════════
 *  MPSC tests
 * ══════════════════════════════════════════════════════════════════════════ */

#define MPSC_PRODUCERS 8
#define MPSC_PER_PROD  10000
#define MPSC_TOTAL     (MPSC_PRODUCERS * MPSC_PER_PROD)

typedef struct {
    hs_mpsc_t        *q;
    int               producer_id;
    hs_mpsc_node_t   *node_pool;   /* pre-allocated nodes for this producer */
} mpsc_prod_arg_t;

static void *mpsc_producer(void *arg)
{
    mpsc_prod_arg_t *a = (mpsc_prod_arg_t *)arg;
    for (int i = 0; i < MPSC_PER_PROD; i++) {
        hs_mpsc_node_t *n = &a->node_pool[i];
        /* payload encodes (producer_id << 16) | seq */
        n->payload = (void *)(uintptr_t)((a->producer_id << 16) | i);
        hs_mpsc_push(a->q, n);
    }
    return NULL;
}

static void test_mpsc_concurrent(void)
{
    printf("test_mpsc_concurrent (%d producers × %d items) … ",
           MPSC_PRODUCERS, MPSC_PER_PROD);
    fflush(stdout);

    hs_mpsc_t q;
    hs_mpsc_init(&q);

    /* Pre-allocate nodes per producer */
    hs_mpsc_node_t *pools[MPSC_PRODUCERS];
    for (int i = 0; i < MPSC_PRODUCERS; i++)
        pools[i] = je_calloc(MPSC_PER_PROD, sizeof(hs_mpsc_node_t));

    pthread_t tids[MPSC_PRODUCERS];
    mpsc_prod_arg_t args[MPSC_PRODUCERS];
    for (int i = 0; i < MPSC_PRODUCERS; i++) {
        args[i].q           = &q;
        args[i].producer_id = i;
        args[i].node_pool   = pools[i];
        pthread_create(&tids[i], NULL, mpsc_producer, &args[i]);
    }
    for (int i = 0; i < MPSC_PRODUCERS; i++)
        pthread_join(tids[i], NULL);

    /* Drain all items as the single consumer */
    int counts[MPSC_PRODUCERS];
    memset(counts, 0, sizeof(counts));
    int total = 0;

    for (;;) {
        hs_mpsc_node_t *n = hs_mpsc_pop(&q);
        if (!n) break;
        uintptr_t v = (uintptr_t)n->payload;
        int prod = (int)(v >> 16);
        CHECK(prod >= 0 && prod < MPSC_PRODUCERS);
        if (prod >= 0 && prod < MPSC_PRODUCERS) counts[prod]++;
        total++;
        if (total == MPSC_TOTAL) break;
    }

    CHECK(total == MPSC_TOTAL);
    for (int i = 0; i < MPSC_PRODUCERS; i++)
        CHECK(counts[i] == MPSC_PER_PROD);

    for (int i = 0; i < MPSC_PRODUCERS; i++) je_free(pools[i]);
    printf("ok  (total=%d)\n", total);
}

static void test_mpsc_single(void)
{
    printf("test_mpsc_single … ");
    hs_mpsc_t q;
    hs_mpsc_init(&q);

    hs_mpsc_node_t nodes[4];
    for (int i = 0; i < 4; i++) {
        nodes[i].payload = (void *)(uintptr_t)(i + 1);
        hs_mpsc_push(&q, &nodes[i]);
    }

    int got = 0;
    for (;;) {
        hs_mpsc_node_t *n = hs_mpsc_pop(&q);
        if (!n) break;
        got++;
    }
    CHECK(got == 4);
    /* Queue should be empty now */
    CHECK(hs_mpsc_pop(&q) == NULL);
    printf("ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SPMC tests
 * ══════════════════════════════════════════════════════════════════════════ */

#define SPMC_CONSUMERS 8
#define SPMC_ITEMS     50000

typedef struct {
    hs_spmc_t    *q;
    atomic_int   *counter;   /* total items consumed across all consumers */
} spmc_cons_arg_t;

static void *spmc_consumer(void *arg)
{
    spmc_cons_arg_t *a = (spmc_cons_arg_t *)arg;
    for (;;) {
        void *item = hs_spmc_pop(a->q);
        if (!item) break;   /* queue closed and empty */
        atomic_fetch_add(a->counter, 1);
    }
    return NULL;
}

static void test_spmc_concurrent(void)
{
    printf("test_spmc_concurrent (1 producer × %d items, %d consumers) … ",
           SPMC_ITEMS, SPMC_CONSUMERS);
    fflush(stdout);

    hs_spmc_t q;
    hs_spmc_init(&q);

    atomic_int counter;
    atomic_init(&counter, 0);

    pthread_t tids[SPMC_CONSUMERS];
    spmc_cons_arg_t args[SPMC_CONSUMERS];
    for (int i = 0; i < SPMC_CONSUMERS; i++) {
        args[i].q       = &q;
        args[i].counter = &counter;
        pthread_create(&tids[i], NULL, spmc_consumer, &args[i]);
    }

    /* Single producer: push SPMC_ITEMS items */
    for (int i = 1; i <= SPMC_ITEMS; i++)
        hs_spmc_push(&q, (void *)(uintptr_t)i);

    /* Signal consumers to stop */
    hs_spmc_close(&q);

    for (int i = 0; i < SPMC_CONSUMERS; i++)
        pthread_join(tids[i], NULL);

    int total = atomic_load(&counter);
    CHECK(total == SPMC_ITEMS);
    printf("ok  (consumed=%d)\n", total);

    hs_spmc_destroy(&q);
}

typedef struct { hs_spmc_t *q; atomic_int *woken; } close_wake_arg_t;
static void *close_wake_worker(void *a_)
{
    close_wake_arg_t *a = (close_wake_arg_t *)a_;
    hs_spmc_pop(a->q);   /* blocks until closed */
    atomic_fetch_add(a->woken, 1);
    return NULL;
}

static void test_spmc_close_wakes_all(void)
{
    printf("test_spmc_close_wakes_all … ");
    hs_spmc_t q;
    hs_spmc_init(&q);

    atomic_int woken;
    atomic_init(&woken, 0);

    close_wake_arg_t arg = { &q, &woken };

    pthread_t tids[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&tids[i], NULL, close_wake_worker, &arg);

    struct timespec ts = { 0, 10000000 }; /* 10 ms */
    nanosleep(&ts, NULL);

    hs_spmc_close(&q);

    for (int i = 0; i < 4; i++) pthread_join(tids[i], NULL);
    CHECK(atomic_load(&woken) == 4);
    hs_spmc_destroy(&q);
    printf("ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("Queue thread-safety tests\n\n");

    test_mpsc_single();
    test_mpsc_concurrent();
    test_spmc_concurrent();
    test_spmc_close_wakes_all();

    printf("\n%s  (%d failure%s)\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
