/**
 * tests/test_integration.c  –  end-to-end HTTP integration tests
 */
#define _GNU_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
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
#include <time.h>

#include "httpserver.h"

static int PORT_BASE = 18180;

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

/* read headers byte-by-byte, then read body exactly according to Content-Length */
static int recv_response(int fd, char *buf, size_t bufsz)
{
    size_t got = 0;
    /* 1. Read headers 1 byte at a time until we see \r\n\r\n */
    while (got < bufsz - 1) {
        ssize_t n = read(fd, buf + got, 1);
        if (n <= 0) break;
        got++;
        buf[got] = '\0';
        if (got >= 4 && strcmp(buf + got - 4, "\r\n\r\n") == 0) {
            break;
        }
    }

    /* 2. Parse Content-Length and read exactly that many bytes */
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl) {
        size_t clen = (size_t)atoi(cl + 15);
        size_t body_read = 0;
        while (body_read < clen && got < bufsz - 1) {
            ssize_t n = read(fd, buf + got, clen - body_read);
            if (n <= 0) break;
            got += (size_t)n;
            body_read += (size_t)n;
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
static hs_dispatch_mode_t test_handler(hs_request_t *req, hs_response_t *res, void *ud)
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
    return HS_DISPATCH_INLINE;
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
 *  Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_get_200(void)
{
    printf("  test_get_200 … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = PORT_BASE;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const char req[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        write(fd, req, strlen(req));

        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);

        CHECK_EQ(parse_status(buf), 200);
        const char *body = parse_body(buf);
        CHECK(body && strcmp(body, "Hello!") == 0);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

static void test_post_echo(void)
{
    printf("  test_post_echo … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = PORT_BASE + 1;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE + 1);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const char body[] = "ping-pong-test-123";
        char req[512];
        int rlen = snprintf(req, sizeof(req),
            "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
        write(fd, req, (size_t)rlen);

        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);

        CHECK_EQ(parse_status(buf), 200);
        const char *rb = parse_body(buf);
        CHECK(rb && strcmp(rb, body) == 0);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

static void test_404(void)
{
    printf("  test_404 … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = PORT_BASE + 2;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE + 2);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const char req[] = "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n";
        write(fd, req, strlen(req));
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);
        CHECK_EQ(parse_status(buf), 404);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

static void test_400(void)
{
    printf("  test_400 … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = PORT_BASE + 3;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE + 3);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const char req[] = "BADREQUEST !!!\r\n\r\n";
        write(fd, req, strlen(req));
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);
        CHECK_EQ(parse_status(buf), 400);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

static void test_413(void)
{
    printf("  test_413 … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port          = PORT_BASE + 4;
    a.cfg.num_threads   = 2;
    a.cfg.handler       = test_handler;
    a.cfg.max_body_size = 64;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE + 4);
    CHECK(fd >= 0);
    if (fd >= 0) {
        char req[512];
        int rlen = snprintf(req, sizeof(req),
            "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Length: 1024\r\n\r\n");
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

static void test_keepalive(void)
{
    printf("  test_keepalive … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = PORT_BASE + 5;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE + 5);
    CHECK(fd >= 0);
    if (fd >= 0) {
        /* Request 1: keepalive */
        const char r1[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
        write(fd, r1, strlen(r1));
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        CHECK_EQ(parse_status(buf), 200);

        /* Request 2: close */
        const char r2[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
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

static void test_pipelining(void)
{
    printf("  test_pipelining … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = PORT_BASE + 6;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE + 6);
    CHECK(fd >= 0);
    if (fd >= 0) {
        /* Write two requests back-to-back in a single write call */
        const char reqs[] =
            "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
            "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        write(fd, reqs, strlen(reqs));

        /* Read both responses sequentially */
        char buf1[4096] = {0};
        recv_response(fd, buf1, sizeof(buf1));
        CHECK_EQ(parse_status(buf1), 200);
        const char *body1 = parse_body(buf1);
        CHECK(body1 && strcmp(body1, "Hello!") == 0);

        char buf2[4096] = {0};
        recv_response(fd, buf2, sizeof(buf2));
        CHECK_EQ(parse_status(buf2), 200);
        const char *body2 = parse_body(buf2);
        CHECK(body2 && strcmp(body2, "Hello!") == 0);

        close(fd);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

static void test_http10_close(void)
{
    printf("  test_http10_close … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = PORT_BASE + 7;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE + 7);
    CHECK(fd >= 0);
    if (fd >= 0) {
        /* HTTP/1.0 defaults to close */
        const char req[] = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
        write(fd, req, strlen(req));

        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        CHECK_EQ(parse_status(buf), 200);

        /* Verify connection is closed by checking if read returns EOF */
        char dummy;
        ssize_t n = read(fd, &dummy, 1);
        CHECK(n == 0);  /* Clean peer close */

        close(fd);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

static void test_http10_keepalive(void)
{
    printf("  test_http10_keepalive … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = PORT_BASE + 8;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE + 8);
    CHECK(fd >= 0);
    if (fd >= 0) {
        /* HTTP/1.0 with Connection: keep-alive */
        const char r1[] = "GET / HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
        write(fd, r1, strlen(r1));
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        CHECK_EQ(parse_status(buf), 200);

        /* Request 2: close */
        const char r2[] = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
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

static void test_chunked_request(void)
{
    printf("  test_chunked_request … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port         = PORT_BASE + 9;
    a.cfg.num_threads  = 2;
    a.cfg.handler      = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE + 9);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const char req[] =
            "POST /echo HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Transfer-Encoding: chunked\r\n\r\n"
            "4\r\nwiki\r\n"
            "5\r\npedia\r\n"
            "0\r\n\r\n";
        write(fd, req, strlen(req));

        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);

        CHECK_EQ(parse_status(buf), 200);
        const char *body = parse_body(buf);
        CHECK(body && strcmp(body, "wikipedia") == 0);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

/* ── concurrent load: 8 threads × 50 requests ──────────────────────────── */
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
        
        /* Mix keep-alive and connection close requests */
        const char *req = (i % 2 == 0)
            ? "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
            : "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            
        write(fd, req, strlen(req));
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        
        if (i % 2 == 0) {
            /* If we kept it alive, make one more request on it, then close */
            const char r2[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            write(fd, r2, strlen(r2));
            memset(buf, 0, sizeof(buf));
            recv_response(fd, buf, sizeof(buf));
        }
        
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
    a.cfg.port         = PORT_BASE + 10;
    a.cfg.num_threads  = 4;
    a.cfg.handler      = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);

    pthread_t ctids[CONC_THREADS];
    conc_arg_t cargs[CONC_THREADS];
    for (int i = 0; i < CONC_THREADS; i++) {
        cargs[i] = (conc_arg_t){ .port = PORT_BASE + 10, .ok = 0, .fail = 0 };
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

static void test_multi_reactor_get(void)
{
    printf("  test_multi_reactor_get … ");
    fflush(stdout);

    srv_thread_arg_t a;
    hs_config_init(&a.cfg);
    a.cfg.port           = PORT_BASE + 11;
    a.cfg.num_threads    = 4;
    a.cfg.handler        = test_handler;

    pthread_t tid;
    hs_server_t *srv = start_server(&a, &tid);
    if (!srv) { printf("SKIP\n"); return; }

    struct timespec ts = {0, 30000000}; nanosleep(&ts, NULL);

    int fd = tcp_connect(PORT_BASE + 11);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const char req[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        write(fd, req, strlen(req));
        char buf[4096] = {0};
        recv_response(fd, buf, sizeof(buf));
        close(fd);
        CHECK_EQ(parse_status(buf), 200);
    }

    hs_server_stop(srv);
    pthread_join(tid, NULL);
    printf("ok\n");
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    PORT_BASE = 18180;

    printf("Integration tests (PORT_BASE=%d)\n\n", PORT_BASE);

    test_get_200();
    test_post_echo();
    test_404();
    test_400();
    test_413();
    test_keepalive();
    test_pipelining();
    test_http10_close();
    test_http10_keepalive();
    test_chunked_request();
    test_concurrent();
    test_multi_reactor_get();

    printf("\n%s  (%d failure%s)\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
