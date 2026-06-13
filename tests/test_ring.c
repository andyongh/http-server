/**
 * tests/test_ring.c  –  standalone unit tests for hs_ring_t
 *
 * Build independently (no deps needed):
 *   gcc -std=c11 -I../src -DHS_RING_SIZE=64 -o test_ring test_ring.c && ./test_ring
 *
 * HS_RING_SIZE=64 keeps the ring tiny so wrap-around is easy to trigger.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

/* Override jemalloc symbols so this file links without jemalloc */
#define je_malloc  malloc
#define je_free    free
#define je_realloc realloc
#define je_strdup  strdup
#define je_calloc  calloc

#include "hs_ring.h"

/* ── tiny helpers ─────────────────────────────────────────────────────────── */
#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);\
            failures++;                                                     \
        }                                                                   \
    } while (0)

static int failures = 0;

/* ── helper: create a blocking socket pair ────────────────────────────────── */
static void make_pair(int fds[2]) {
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
}

/* ══════════════════════════════════════════════════════════════════════════ */
/* TEST 1 – basic recv + peek + drain                                         */
/* ══════════════════════════════════════════════════════════════════════════ */
static void test_basic(void)
{
    printf("test_basic … ");

    int fds[2]; make_pair(fds);
    const char msg[] = "hello";
    write(fds[1], msg, 5);
    close(fds[1]);

    /* Make read-end non-blocking */
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    hs_ring_t ring; hs_ring_init(&ring);

    ssize_t n = hs_ring_recv(&ring, fds[0]);
    CHECK(n == 5);
    CHECK(hs_ring_len(&ring) == 5);

    const char *ptr; uint32_t len;
    hs_ring_peek(&ring, &ptr, &len);
    CHECK(len == 5);
    CHECK(memcmp(ptr, "hello", 5) == 0);

    hs_ring_drain(&ring, 5);
    CHECK(hs_ring_empty(&ring));

    /* EOF */
    n = hs_ring_recv(&ring, fds[0]);
    CHECK(n == 0);

    close(fds[0]);
    printf("ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════ */
/* TEST 2 – wrap-around: fill ring past physical end, peek in two segments    */
/* ══════════════════════════════════════════════════════════════════════════ */
static void test_wrap(void)
{
    printf("test_wrap … ");

    hs_ring_t ring; hs_ring_init(&ring);

    /* Advance r and w by HS_RING_SIZE - 4 to put w near the physical end */
    ring.r = ring.w = HS_RING_SIZE - 4;

    /* Write 10 bytes that straddle the physical end:
     *   ring.data[60..63]  (4 bytes)  + ring.data[0..5] (6 bytes) */
    int fds[2]; make_pair(fds);
    char buf[10];
    for (int i = 0; i < 10; i++) buf[i] = (char)('A' + i);
    write(fds[1], buf, 10);
    close(fds[1]);

    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    ssize_t n = hs_ring_recv(&ring, fds[0]);
    CHECK(n == 10);
    CHECK(hs_ring_len(&ring) == 10);

    /* First peek: up to the physical end (4 bytes) */
    const char *p1; uint32_t l1;
    hs_ring_peek(&ring, &p1, &l1);
    CHECK(l1 == 4);
    CHECK(memcmp(p1, "ABCD", 4) == 0);
    hs_ring_drain(&ring, l1);

    /* Second peek: the wrapped portion (6 bytes) */
    const char *p2; uint32_t l2;
    hs_ring_peek(&ring, &p2, &l2);
    CHECK(l2 == 6);
    CHECK(memcmp(p2, "EFGHIJ", 6) == 0);
    hs_ring_drain(&ring, l2);

    CHECK(hs_ring_empty(&ring));
    close(fds[0]);
    printf("ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════ */
/* TEST 3 – hs_ring_is_contiguous                                             */
/* ══════════════════════════════════════════════════════════════════════════ */
static void test_contiguity(void)
{
    printf("test_contiguity … ");

    /* body starting at physical index 60, length 4 → fits before end (64) */
    CHECK(hs_ring_is_contiguous(60, 4) == 1);

    /* body starting at physical index 60, length 5 → wraps (60+5 > 64) */
    CHECK(hs_ring_is_contiguous(60, 5) == 0);

    /* body starting at 0, full ring → contiguous */
    CHECK(hs_ring_is_contiguous(0, HS_RING_SIZE) == 1);

    /* body starting at 1, full ring → wraps */
    CHECK(hs_ring_is_contiguous(1, HS_RING_SIZE) == 0);

    printf("ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════ */
/* TEST 4 – hs_ring_linearise: copy wrapped segment into flat buffer          */
/* ══════════════════════════════════════════════════════════════════════════ */
static void test_linearise(void)
{
    printf("test_linearise … ");

    hs_ring_t ring; hs_ring_init(&ring);

    /* Manually write 10 bytes straddling the physical end at index 60 */
    const char src[] = "0123456789";
    /* First 4 bytes at index 60..63 */
    memcpy(ring.data + 60, src, 4);
    /* Next 6 bytes at index 0..5 */
    memcpy(ring.data, src + 4, 6);

    char dst[10];
    /* abs_pos such that physical index = 60: abs_pos = 60 (r=0, so r+60 idx) */
    hs_ring_linearise(&ring, 60, 10, dst);
    CHECK(memcmp(dst, "0123456789", 10) == 0);

    printf("ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════ */
/* TEST 5 – ring full: recv returns -3                                        */
/* ══════════════════════════════════════════════════════════════════════════ */
static void test_full(void)
{
    printf("test_full … ");

    hs_ring_t ring; hs_ring_init(&ring);

    /* Simulate ring completely full */
    ring.w = ring.r + HS_RING_SIZE;
    CHECK(hs_ring_full(&ring));

    int fds[2]; make_pair(fds);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    write(fds[1], "X", 1);
    close(fds[1]);

    ssize_t n = hs_ring_recv(&ring, fds[0]);
    CHECK(n == -3);   /* ring full */

    close(fds[0]);
    printf("ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════ */
/* TEST 6 – two-iovec recv fills both segments in one readv call              */
/* ══════════════════════════════════════════════════════════════════════════ */
static void test_two_iov_recv(void)
{
    printf("test_two_iov_recv … ");

    hs_ring_t ring; hs_ring_init(&ring);

    /*
     * Choose r=6, w=60 so that:
     *   w_idx  = 60 & (RING-1) = 60
     *   to_end = RING - 60     =  4   (bytes to physical end)
     *   free   = RING - (w-r)  = 64-54 = 10
     * Since to_end(4) < free(10) → readv will use TWO iovecs.
     */
    ring.r = 6;
    ring.w = 60;
    uint32_t w_idx  = ring.w & (HS_RING_SIZE - 1);
    uint32_t to_end = HS_RING_SIZE - w_idx;
    uint32_t free_b = hs_ring_free(&ring);
    /* Verify two-iov case */
    CHECK(free_b == 10);
    CHECK(to_end < free_b);   /* straddles wrap */

    /* Write exactly 10 bytes into a socket */
    int fds[2]; make_pair(fds);
    char payload[10];
    for (int i = 0; i < 10; i++) payload[i] = (char)('a' + i);
    write(fds[1], payload, 10);
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    ssize_t n = hs_ring_recv(&ring, fds[0]);
    CHECK(n == 10);
    CHECK(hs_ring_len(&ring) == (uint32_t)(HS_RING_SIZE - 10) + 10);

    /* Drain old data, then verify the new 10 bytes are correct */
    hs_ring_drain(&ring, HS_RING_SIZE - 10);
    const char *ptr; uint32_t len;
    char flat[10]; uint32_t collected = 0;
    while (collected < 10) {
        hs_ring_peek(&ring, &ptr, &len);
        if (!len) break;
        memcpy(flat + collected, ptr, len);
        collected += len;
        hs_ring_drain(&ring, len);
    }
    CHECK(collected == 10);
    CHECK(memcmp(flat, "abcdefghij", 10) == 0);

    close(fds[0]);
    printf("ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("hs_ring tests  (HS_RING_SIZE=%u)\n\n", HS_RING_SIZE);

    test_basic();
    test_wrap();
    test_contiguity();
    test_linearise();
    test_full();
    test_two_iov_recv();

    printf("\n%s  (%d failure%s)\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
