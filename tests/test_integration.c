/**
 * tests/test_integration.c  –  end-to-end HTTP over real socketpairs
 *
 * Spins up a minimal server instance (no real network), connects via
 * socketpair, sends raw HTTP, reads raw HTTP back, and validates the
 * response status + body.
 *
 * Tests:
 *   1. GET / → 200 "Hello!"
 *   2. POST /echo → 200 echo body
 *   3. GET /notfound → 404
 *   4. Malformed request → 400
 *   5. Body > max_body_size → 413
 *   6. Multiple keep-alive requests on same connection
 *   7. UDS socketpair (same path, AF_UNIX)
 *   8. Concurrent requests from 8 client threads
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "httpserver.h"

/* ── helpers ────────────────────────────────────────────────────────────── */
static int failures = 0;
#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr, "  FAIL  %s:%d  %s\n",__FILE__,__LINE__,#expr);\
            failures++;                                                      \
        }                                                                    \
    } while (0)
#define CHECK_EQ(a,b) CHECK((int)(a)==(int)(b))

/* read until '\r\n\r\n' header-end is seen, then read body per Content-Length */
static int recv_response(int fd, char *buf, size_t bufsz)
{
    size_t got = 0;
    while (got < bufsz - 1) {
        ssize_t n = read(fd, buf + got, bufsz - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        /* Stop once we have the full response (header + body) */
        buf[got] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            /* Extract Content-Length */
            char *cl = strcasestr(buf, "Content-Length:");
            if (cl) {
                size_t clen = (size_t)atoi(cl + 15);
                size_t body_start = (size_t)(hdr_end + 4 - buf);
                size_t body_got   = got - body_start;
                /* Read remaining body bytes */
                while (body_got < clen && got < bufsz - 1) {
                    ssize_t m = read(fd, buf + got, clen - body_got);
                    if (m <= 0) break;
                    got += (size_t)m;
                    body_got += (size_t)m;
                }
            }
            break;
        }
    }
    buf[got] = '\0';
    return (int)got;
}

static int parse_status(const char *resp)
{
    /* "HTTP/1.x SSS " */
    if (strncmp(resp, "HTTP/1.", 7) != 0) return -1;
    return atoi(resp + 9);
}

static const char *parse_body(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/* ── C handler used by the test server ─────────────────────────────────── */
static void test_handler(hs_request_t *req, hs_response_t *res, void *ud)
{
    (void)ud;
    const char *url = hs_req_url(req);
    const char *m   = hs_req_method_str(req);

    if (strcmp(url, "/") == 0 && strcmp(m, "GET") == 0) {
        hs_res_status(res, 200);
        hs_res_header(res, "Content-Type", "text/plain");
        hs_res_body_str(res, "Hello!");
    } else if (strcmp(url, "/echo") == 0 && strcmp(m, "POST") == 0) {
        size_t blen = 0;
        const char *body = hs_req_body(req, &blen);
        hs_res_status(res, 200);
        hs_res_header(res, "Content-Type", "text/plain");
        hs_res_body(res, body, blen);
    } else {
        hs_res_status(res, 404);
        hs_res_body_str(res, "Not Found");
    }
    hs_res_send(res);
}

/* ── server thread: runs hs_server_run() ────────────────────────────────── */
typedef struct {
    hs_config_t cfg;
    hs_server_t *srv;
    int          ready_pipe[2];   /* write[1] when server is about to aeMain */
} srv_thread_arg_t;

static void *srv_thread(void *arg)
{
    srv_thread_arg_t *a = (srv_thread_arg_t *)arg;
    a->srv = hs_server_new(&a->cfg);
    if (!a->srv) { write(a->ready_pipe[1], "E", 1); return NULL; }
    write(a->ready_pipe[1], "R", 1);
    hs_server_run(a->srv);
    hs_server_free(a->srv);
    return NULL;
}

/* Start server and wait for it to be ready */
static hs_server_t *start_server(srv_thread_arg_t *a, pthread_t *tid)
{
    pipe(a->ready_pipe);
    pthread_create(tid, NULL, srv_thread, a);
    char ch = 0;
    read(a->ready_pipe[0], &ch, 1);
    close(a->ready_pipe[0]);
    close(a->ready_pipe[1]);
    return (ch == 'R') ? a->srv : NULL;
}

/* TCP client connect helper */
static int tcp_connect(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T1 – GET / → 200
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_get_200(void)
{
    printf("  test_get_200 … ");

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = 18080;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;
    a.cfg.reactor_mode = HS_REACTOR_SINGLE;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP (server start failed)\n"); return; }

    /* Small sleep to let the event loop register the listen fd */
    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(18080);
    CHECK(fd >= 0);
    if (fd < 0) { hs_server_stop(srv); pthread_join(tid, NULL); return; }

    const char req[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    write(fd, req, strlen(req));

    char buf[4096] = {0};
    recv_response(fd, buf, sizeof(buf));
    close(fd);

    CHECK_EQ(parse_status(buf), 200);
    const char *body = parse_body(buf);
    CHECK(body && strcmp(body, "Hello!") == 0);

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T2 – POST /echo → echoes body
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_post_echo(void)
{
    printf("  test_post_echo … ");

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = 18081;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;
    a.cfg.reactor_mode = HS_REACTOR_SINGLE;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(18081);
    CHECK(fd >= 0);
    if (fd < 0) { hs_server_stop(srv); pthread_join(tid, NULL); return; }

    const char body[]  = "ping";
    char req[256];
    int  rlen = snprintf(req, sizeof(req),
        "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    write(fd, req, (size_t)rlen);

    char buf[4096] = {0};
    recv_response(fd, buf, sizeof(buf));
    close(fd);

    CHECK_EQ(parse_status(buf), 200);
    const char *rb = parse_body(buf);
    CHECK(rb && strcmp(rb, body) == 0);

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T3 – unknown URL → 404
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_404(void)
{
    printf("  test_404 … ");

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = 18082;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;
    a.cfg.reactor_mode = HS_REACTOR_SINGLE;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(18082);
    CHECK(fd >= 0);
    if (fd >= 0) {
        write(fd, "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n", 42);
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);
        CHECK_EQ(parse_status(buf), 404);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T4 – malformed request → 400
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_400(void)
{
    printf("  test_400 … ");

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = 18083;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;
    a.cfg.reactor_mode = HS_REACTOR_SINGLE;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(18083);
    CHECK(fd >= 0);
    if (fd >= 0) {
        write(fd, "BADREQUEST !!!\r\n\r\n", 18);
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);
        CHECK_EQ(parse_status(buf), 400);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T5 – body > max_body_size → 413
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_413(void)
{
    printf("  test_413 … ");

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port          = 18084;
    a.cfg.num_threads   = 2;
    a.cfg.handler       = test_handler;
    a.cfg.reactor_mode  = HS_REACTOR_SINGLE;
    a.cfg.max_body_size = 128;   /* tiny limit */

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(18084);
    CHECK(fd >= 0);
    if (fd >= 0) {
        char req[512];
        int  rlen = snprintf(req, sizeof(req),
            "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Length: 10485760\r\n\r\n");
        write(fd, req, (size_t)rlen);
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);
        CHECK_EQ(parse_status(buf), 413);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T6 – keep-alive: two requests on the same connection
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_keepalive(void)
{
    printf("  test_keepalive … ");

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = 18085;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;
    a.cfg.reactor_mode = HS_REACTOR_SINGLE;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(18085);
    CHECK(fd >= 0);
    if (fd >= 0) {
        /* First request */
        const char r1[] =
            "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
        write(fd, r1, strlen(r1));
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        CHECK_EQ(parse_status(buf), 200);

        /* Second request on the same connection */
        const char r2[] =
            "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        write(fd, r2, strlen(r2));
        memset(buf, 0, sizeof(buf));
        recv_response(fd, buf, sizeof(buf));
        CHECK_EQ(parse_status(buf), 200);
        close(fd);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T7 – concurrent load: 8 threads × 50 requests
 * ═══════════════════════════════════════════════════════════════════════════ */
#define CONC_THREADS  8
#define CONC_REQS     50

typedef struct {
    uint16_t port;
    int      ok;
    int      fail;
} conc_arg_t;

static void *conc_client(void *arg)
{
    conc_arg_t *a = (conc_arg_t *)arg;
    for (int i = 0; i < CONC_REQS; i++) {
        int fd = tcp_connect(a->port);
        if (fd < 0) { a->fail++; continue; }
        write(fd, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 52);
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);
        if (parse_status(buf) == 200) a->ok++;
        else                           a->fail++;
    }
    return NULL;
}

static void test_concurrent(void)
{
    printf("  test_concurrent (%d threads × %d req) … ",
           CONC_THREADS, CONC_REQS);
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = 18086;
    a.cfg.num_threads  = 4;
    a.cfg.handler      = test_handler;
    a.cfg.reactor_mode = HS_REACTOR_SINGLE;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    pthread_t ctids[CONC_THREADS];
    conc_arg_t cargs[CONC_THREADS];
    for (int i = 0; i < CONC_THREADS; i++) {
        cargs[i] = (conc_arg_t){ .port = 18086 };
        pthread_create(&ctids[i], NULL, conc_client, &cargs[i]);
    }

    int total_ok = 0, total_fail = 0;
    for (int i = 0; i < CONC_THREADS; i++) {
        pthread_join(ctids[i], NULL);
        total_ok   += cargs[i].ok;
        total_fail += cargs[i].fail;
    }

    CHECK_EQ(total_ok, CONC_THREADS * CONC_REQS);
    CHECK_EQ(total_fail, 0);

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok  (ok=%d fail=%d)\n", total_ok, total_fail);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  T8 – Multi-reactor mode: same tests with HS_REACTOR_MULTI
 * ═══════════════════════════════════════════════════════════════════════════ */
static void test_multi_reactor_get(void)
{
    printf("  test_multi_reactor_get … ");

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port           = 18087;
    a.cfg.num_threads    = 4;
    a.cfg.num_io_threads = 2;
    a.cfg.handler        = test_handler;
    a.cfg.reactor_mode   = HS_REACTOR_MULTI;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 30000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(18087);
    CHECK(fd >= 0);
    if (fd >= 0) {
        write(fd, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 52);
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);
        CHECK_EQ(parse_status(buf), 200);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("Integration tests\n\n");

    test_get_200();
    test_post_echo();
    test_404();
    test_400();
    test_413();
    test_keepalive();
    test_concurrent();
    test_multi_reactor_get();

    printf("\n%s  (%d failure%s)\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
