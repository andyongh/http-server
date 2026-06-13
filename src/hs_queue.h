/**
 * hs_queue.h
 *
 *   hs_mpsc_t  –  lock-free MPSC linked list (Vyukov intrusive algorithm)
 *                 Workers → IO sub-reactor  (response path)
 *
 *   hs_spmc_t  –  mutex + condvar bounded ring
 *                 IO thread → Workers       (work-item path)
 */
#pragma once

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <jemalloc/jemalloc.h>

/* ╔══════════════════════════════════════════════════════════════════════════╗
 * ║  MPSC – lock-free  (multiple producers, single consumer)               ║
 * ╚══════════════════════════════════════════════════════════════════════════╝ */

typedef struct hs_mpsc_node {
    _Atomic(struct hs_mpsc_node *) next;
    void *payload;
} hs_mpsc_node_t;

typedef struct {
    _Atomic(hs_mpsc_node_t *) head;   /* producers: exchange here */
    hs_mpsc_node_t            stub;
    hs_mpsc_node_t           *tail;   /* consumer's private tail  */
} hs_mpsc_t;

static inline void hs_mpsc_init(hs_mpsc_t *q)
{
    atomic_store_explicit(&q->stub.next, NULL, memory_order_relaxed);
    q->stub.payload = NULL;
    atomic_store_explicit(&q->head, &q->stub, memory_order_relaxed);
    q->tail = &q->stub;
}

/** Thread-safe push.  node lifetime must exceed the paired pop. */
static inline void hs_mpsc_push(hs_mpsc_t *q, hs_mpsc_node_t *node)
{
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    hs_mpsc_node_t *prev =
        atomic_exchange_explicit(&q->head, node, memory_order_acq_rel);
    atomic_store_explicit(&prev->next, node, memory_order_release);
}

/** Single-consumer pop.  Returns NULL if empty. */
static inline hs_mpsc_node_t *hs_mpsc_pop(hs_mpsc_t *q)
{
    hs_mpsc_node_t *tail = q->tail;
    hs_mpsc_node_t *next =
        atomic_load_explicit(&tail->next, memory_order_acquire);

    if (tail == &q->stub) {
        if (!next) return NULL;
        q->tail = next;
        tail = next;
        next = atomic_load_explicit(&tail->next, memory_order_acquire);
    }
    if (next) { q->tail = next; return tail; }

    hs_mpsc_node_t *head =
        atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail != head) return NULL;

    hs_mpsc_push(q, &q->stub);
    next = atomic_load_explicit(&tail->next, memory_order_acquire);
    if (next) { q->tail = next; return tail; }
    return NULL;
}

/* ╔══════════════════════════════════════════════════════════════════════════╗
 * ║  SPMC – bounded ring (single producer / multiple consumers)             ║
 * ╚══════════════════════════════════════════════════════════════════════════╝ */

#define HS_SPMC_CAP  4096u   /* power of two */
#define HS_SPMC_MASK (HS_SPMC_CAP - 1u)

typedef struct {
    void           *ring[HS_SPMC_CAP];
    uint64_t        head;         /* producer writes here */
    uint64_t        tail;         /* consumers read here  */
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int             closed;
} hs_spmc_t;

static inline void hs_spmc_init(hs_spmc_t *q)
{
    q->head = q->tail = 0;
    q->closed = 0;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full,  NULL);
}

static inline void hs_spmc_destroy(hs_spmc_t *q)
{
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/** Producer push (blocks if full). -1 if closed. */
static inline int hs_spmc_push(hs_spmc_t *q, void *item)
{
    pthread_mutex_lock(&q->mu);
    while (!q->closed && q->head - q->tail >= HS_SPMC_CAP)
        pthread_cond_wait(&q->not_full, &q->mu);
    if (q->closed) { pthread_mutex_unlock(&q->mu); return -1; }
    q->ring[q->head & HS_SPMC_MASK] = item;
    q->head++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

/** Consumer pop (blocks until item or closed). NULL if closed+empty. */
static inline void *hs_spmc_pop(hs_spmc_t *q)
{
    pthread_mutex_lock(&q->mu);
    while (!q->closed && q->head == q->tail)
        pthread_cond_wait(&q->not_empty, &q->mu);
    if (q->head == q->tail) { pthread_mutex_unlock(&q->mu); return NULL; }
    void *item = q->ring[q->tail & HS_SPMC_MASK];
    q->tail++;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return item;
}

static inline void hs_spmc_close(hs_spmc_t *q)
{
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);
}
