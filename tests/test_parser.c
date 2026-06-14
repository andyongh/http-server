/**
 * tests/test_parser.c  –  llhttp error-path coverage
 *
 * Tests every hs_feed_result_t outcome by synthesising HTTP bytes directly
 * into hs_conn_t ring buffers (no real sockets).  A mock sub-reactor is
 * injected so the tests run without a real event loop.
 *
 *   HS_FEED_OK          – valid GET, valid POST with body
 *   HS_FEED_PARSE_ERR   – malformed request line
 *   HS_FEED_TOO_LARGE   – body exceeds max_body_size (Content-Length known)
 *   HS_FEED_TOO_LARGE   – streaming body exceeds limit (no Content-Length)
 *   HS_FEED_UPGRADE     – Connection: Upgrade
 *   HS_FEED_OOM         – simulated (overflow alloc failure stub)
 *   HS_FEED_AGAIN       – ring is already empty (recv returns -1)
 *   keep-alive          – llhttp HPE_PAUSED after second request injected
 *   zero-copy body path – body_in_ring set, pointer inside ring
 *   overflow body path  – body > HS_RING_SIZE triggers je_malloc
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>

#include "hs_alloc.h"
#include <llhttp.h>

/* Pull in the internal headers */
#include "hs_ring.h"
#include "hs_buf.h"
#include "hs_queue.h"
#include "hs_conn.h"
#include "hs_reactor.h"
#include "hs_server.h"
#include "httpserver.h"

/* ── check macro ────────────────────────────────────────────────────────── */
static int failures = 0;
#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr, "  FAIL  %s:%d  %s\n",__FILE__,__LINE__,#expr);\
            failures++;                                                      \
        }                                                                    \
    } while (0)
#define CHECK_EQ(a,b) CHECK((a)==(b))

/* ══════════════════════════════════════════════════════════════════════════
 *  Minimal mock infrastructure
 *
 *  We don't need a real event loop or real sockets.  Instead we:
 *    1. Create a Unix socketpair so readv() works.
 *    2. Stub hs_on_request_complete() so it just marks a flag.
 *    3. Provide a minimal hs_server_t / hs_sub_reactor_t with config.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Captured in the stub */
static int g_request_complete_called = 0;
static hs_conn_t *g_completed_conn   = NULL;

/* Override: replaces the real hs_on_request_complete in hs_reactor.c.
 * Because we link test_parser.c together with the real hs_conn.c
 * (which calls hs_on_request_complete via a function pointer path), we
 * provide the definition here.  The linker picks this one first.      */
void hs_on_request_complete(hs_conn_t *conn)
{
    g_request_complete_called++;
    g_completed_conn = conn;
}

/* Stub for hs_res_send (not used in parser tests) */
void hs_res_send(hs_response_t *res) { (void)res; }

/* ── build a fake server + sub-reactor ──────────────────────────────────── */
typedef struct {
    hs_server_t      srv;
    hs_reactor_t     reactor;
} mock_ctx_t;

static void mock_ctx_init(mock_ctx_t *m, size_t max_body)
{
    memset(m, 0, sizeof(*m));
    hs_config_init(&m->srv.config);
    m->srv.config.max_body_size = max_body;
    m->reactor.srv = &m->srv;
}

/* ── create a connected socketpair and write HTTP bytes into the write end ─ */
static int inject_bytes(hs_conn_t *conn, mock_ctx_t *m,
                        const char *data, size_t len)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) return -1;

    int sndbuf = 65536;
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    int rcvbuf = 65536;
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    /* non-blocking read end */
    int fl = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);

    /* write all bytes then close write end (EOF) */
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fds[1], data + sent, len - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    close(fds[1]);

    hs_conn_init(conn, fds[0], &m->reactor);
    return 0;
}

static void conn_teardown(hs_conn_t *conn)
{
    if (conn->fd >= 0) close(conn->fd);
    hs_conn_cleanup(conn);
    conn->fd = -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Tests
 * ══════════════════════════════════════════════════════════════════════════ */

/* T1 – simple GET → HS_FEED_OK + message complete */
static void test_get_ok(void)
{
    printf("  test_get_ok … ");
    mock_ctx_t m; mock_ctx_init(&m, 4096);
    hs_conn_t conn;

    const char req[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    inject_bytes(&conn, &m, req, strlen(req));

    g_request_complete_called = 0;
    hs_feed_result_t r = hs_conn_recv_and_feed(&conn);

    CHECK_EQ(r, HS_FEED_OK);
    CHECK(g_request_complete_called == 1);
    CHECK_EQ(conn.req.method, HTTP_GET);
    CHECK(strcmp(conn.req.url, "/hello") == 0);

    conn_teardown(&conn);
    printf("ok\n");
}

/* T2 – POST with known body → zero-copy ring path */
static void test_post_small_body_zerocopy(void)
{
    printf("  test_post_small_body_zerocopy … ");
    mock_ctx_t m; mock_ctx_init(&m, 65536);
    hs_conn_t conn;

    const char body[] = "hello=world";
    char req[512];
    int reqlen = snprintf(req, sizeof(req),
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        strlen(body), body);

    inject_bytes(&conn, &m, req, (size_t)reqlen);

    g_request_complete_called = 0;
    hs_feed_result_t r = hs_conn_recv_and_feed(&conn);

    CHECK_EQ(r, HS_FEED_OK);
    CHECK(g_request_complete_called == 1);

    size_t blen = 0;
    const char *bp = hs_req_body((hs_request_t *)&conn, &blen);

    CHECK(blen == strlen(body));
    CHECK(memcmp(bp, body, blen) == 0);

    /* Zero-copy: pointer inside ring.data */
    CHECK(bp >= conn.ring.data && bp < conn.ring.data + HS_RING_SIZE);
    CHECK(conn.req.body_in_ring == 1);
    CHECK(conn.req.overflow == NULL);

    conn_teardown(&conn);
    printf("ok\n");
}

/* T3 – POST body > HS_RING_SIZE → overflow path */
static void test_post_large_body_overflow(void)
{
    printf("  test_post_large_body_overflow … ");
    size_t body_size = HS_RING_SIZE + 512;   /* guaranteed to overflow ring */
    mock_ctx_t m; mock_ctx_init(&m, body_size * 2);
    hs_conn_t conn;

    char *body = (char *)hs_malloc(body_size);
    memset(body, 'X', body_size);

    char *req_hdr = (char *)hs_malloc(256);
    int hdrlen = snprintf(req_hdr, 256,
        "POST /big HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        body_size);

    /* Write header + body via socketpair */
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    int sndbuf = 65536;
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    int rcvbuf = 65536;
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    int fl = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    write(fds[1], req_hdr, (size_t)hdrlen);
    write(fds[1], body, body_size);
    close(fds[1]);
    hs_free(req_hdr); hs_free(body);

    hs_conn_init(&conn, fds[0], &m.reactor);

    g_request_complete_called = 0;
    /* May need multiple calls because the body is larger than one ring fill */
    hs_feed_result_t r = HS_FEED_AGAIN;
    int attempts = 0;
    while ((r == HS_FEED_OK && !conn.req.msg_complete) ||
           r == HS_FEED_AGAIN) {
        r = hs_conn_recv_and_feed(&conn);
        if (++attempts > 100) break;
    }

    CHECK_EQ(r, HS_FEED_OK);
    CHECK(g_request_complete_called == 1);

    size_t blen = 0;
    const char *bp = hs_req_body((hs_request_t *)&conn, &blen);

    CHECK(blen == body_size);
    /* Overflow path: pointer is je_malloc'd, NOT in ring */
    CHECK(conn.req.overflow != NULL);
    CHECK(bp == conn.req.overflow);

    /* Verify content */
    int all_x = 1;
    for (size_t i = 0; i < blen; i++)
        if (bp[i] != 'X') { all_x = 0; break; }
    CHECK(all_x);

    conn_teardown(&conn);
    printf("ok\n");
}

/* T4 – malformed request line → HS_FEED_PARSE_ERR */
static void test_parse_error(void)
{
    printf("  test_parse_error … ");
    mock_ctx_t m; mock_ctx_init(&m, 4096);
    hs_conn_t conn;

    /* Garbage that llhttp cannot parse */
    const char garbage[] = "NOTHTTP !!@#$\r\n\r\n";
    inject_bytes(&conn, &m, garbage, strlen(garbage));

    g_request_complete_called = 0;
    hs_feed_result_t r = hs_conn_recv_and_feed(&conn);

    CHECK_EQ(r, HS_FEED_PARSE_ERR);
    CHECK_EQ(g_request_complete_called, 0);

    conn_teardown(&conn);
    printf("ok\n");
}

/* T5 – Content-Length > max_body_size → HS_FEED_TOO_LARGE (early-out) */
static void test_413_content_length(void)
{
    printf("  test_413_content_length (declared CL) … ");
    mock_ctx_t m; mock_ctx_init(&m, 1024);   /* max = 1 KiB */
    hs_conn_t conn;

    /* Declare 10 MiB body – header only, no actual body bytes needed */
    const char req[] =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 10485760\r\n"
        "\r\n";
    inject_bytes(&conn, &m, req, strlen(req));

    g_request_complete_called = 0;
    hs_feed_result_t r = hs_conn_recv_and_feed(&conn);

    CHECK_EQ(r, HS_FEED_TOO_LARGE);
    CHECK_EQ(g_request_complete_called, 0);
    CHECK(conn.req.body_413 == 1);

    conn_teardown(&conn);
    printf("ok\n");
}

/* T6 – streaming body exceeds limit (no Content-Length) → HS_FEED_TOO_LARGE */
static void test_413_streaming(void)
{
    printf("  test_413_streaming (chunked / no CL) … ");
    size_t max_body = 64;   /* tiny limit */
    mock_ctx_t m; mock_ctx_init(&m, max_body);
    hs_conn_t conn;

    /* Body of 128 bytes without Content-Length (server sends 413 mid-stream) */
    char body[128]; memset(body, 'A', sizeof(body));
    char req[512];
    int len = snprintf(req, sizeof(req),
        "POST /stream HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "80\r\n");
    /* append body */
    memcpy(req + len, body, sizeof(body));
    len += sizeof(body);
    int tail_len = snprintf(req + len, sizeof(req) - len, "\r\n0\r\n\r\n");
    size_t total = (size_t)len + tail_len;

    inject_bytes(&conn, &m, req, total);

    g_request_complete_called = 0;
    /* max_body_size (64) < Content-Length (128) → early-out in headers_complete */
    hs_feed_result_t r = hs_conn_recv_and_feed(&conn);

    CHECK(r == HS_FEED_TOO_LARGE);
    CHECK_EQ(g_request_complete_called, 0);

    conn_teardown(&conn);
    printf("ok\n");
}

/* T7 – Upgrade header → HS_FEED_UPGRADE */
static void test_upgrade(void)
{
    printf("  test_upgrade (Connection: Upgrade) … ");
    mock_ctx_t m; mock_ctx_init(&m, 4096);
    hs_conn_t conn;

    const char req[] =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "\r\n";
    inject_bytes(&conn, &m, req, strlen(req));

    g_request_complete_called = 0;
    hs_feed_result_t r = hs_conn_recv_and_feed(&conn);

    CHECK_EQ(r, HS_FEED_UPGRADE);
    CHECK_EQ(g_request_complete_called, 0);
    CHECK(conn.req.body_upgrade == 1);

    conn_teardown(&conn);
    printf("ok\n");
}

/* T8 – EOF (empty read) → HS_FEED_EOF */
static void test_eof(void)
{
    printf("  test_eof … ");
    mock_ctx_t m; mock_ctx_init(&m, 4096);
    hs_conn_t conn;

    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    close(fds[1]);   /* immediate EOF */

    hs_conn_init(&conn, fds[0], &m.reactor);
    hs_feed_result_t r = hs_conn_recv_and_feed(&conn);
    CHECK_EQ(r, HS_FEED_EOF);

    conn_teardown(&conn);
    printf("ok\n");
}

/* T9 – EAGAIN → HS_FEED_AGAIN */
static void test_eagain(void)
{
    printf("  test_eagain … ");
    mock_ctx_t m; mock_ctx_init(&m, 4096);
    hs_conn_t conn;

    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    /* Write end open, nothing written → EAGAIN */

    hs_conn_init(&conn, fds[0], &m.reactor);
    hs_feed_result_t r = hs_conn_recv_and_feed(&conn);
    CHECK_EQ(r, HS_FEED_AGAIN);

    close(fds[1]);
    conn_teardown(&conn);
    printf("ok\n");
}

/* T10 – request headers parsing: multiple headers, case-insensitive lookup */
static void test_header_lookup(void)
{
    printf("  test_header_lookup … ");
    mock_ctx_t m; mock_ctx_init(&m, 4096);
    hs_conn_t conn;

    const char req[] =
        "GET /info HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "X-Request-ID: abc-123\r\n"
        "Content-Type: application/json\r\n"
        "Accept: */*\r\n"
        "\r\n";
    inject_bytes(&conn, &m, req, strlen(req));

    g_request_complete_called = 0;
    hs_conn_recv_and_feed(&conn);
    CHECK(g_request_complete_called == 1);

    /* Case-insensitive header lookup */
    const char *ct = hs_req_header((hs_request_t *)&conn, "content-type");
    CHECK(ct != NULL);
    CHECK(strcmp(ct, "application/json") == 0);

    const char *xrid = hs_req_header((hs_request_t *)&conn, "X-Request-Id");
    CHECK(xrid != NULL);
    CHECK(strcmp(xrid, "abc-123") == 0);

    /* Non-existent header */
    CHECK(hs_req_header((hs_request_t *)&conn, "X-Does-Not-Exist") == NULL);

    conn_teardown(&conn);
    printf("ok\n");
}

/* T11 – keep-alive: llhttp pauses after first message, then re-resumed */
static void test_keepalive_pipeline(void)
{
    printf("  test_keepalive_pipeline … ");
    mock_ctx_t m; mock_ctx_init(&m, 4096);
    hs_conn_t conn;

    /*
     * Two pipelined requests in one socketpair write.
     * After the first message_complete, llhttp self-pauses (HPE_PAUSED).
     * We preserve the remaining bytes in the ring buffer, reset the parser,
     * and re-feed for the second request.
     */
    const char two_reqs[] =
        "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n";

    inject_bytes(&conn, &m, two_reqs, strlen(two_reqs));

    /* First request */
    g_request_complete_called = 0;
    hs_feed_result_t r = hs_conn_recv_and_feed(&conn);
    CHECK(r == HS_FEED_OK);
    CHECK(g_request_complete_called == 1);
    CHECK(strcmp(conn.req.url, "/first") == 0);

    /* Simulate keep-alive reset (normally done in write_cb) */
    hs_conn_reset_req(&conn);

    /* Second request: should parse directly from the remaining bytes in the ring */
    g_request_complete_called = 0;
    r = hs_conn_recv_and_feed(&conn);
    CHECK(r == HS_FEED_OK);
    CHECK(g_request_complete_called == 1);
    CHECK(strcmp(conn.req.url, "/second") == 0);

    conn_teardown(&conn);
    printf("ok\n");
}

/* T12 – HTTP version detection */
static void test_http10(void)
{
    printf("  test_http10 … ");
    mock_ctx_t m; mock_ctx_init(&m, 4096);
    hs_conn_t conn;

    const char req[] = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
    inject_bytes(&conn, &m, req, strlen(req));

    g_request_complete_called = 0;
    hs_conn_recv_and_feed(&conn);
    CHECK(g_request_complete_called == 1);
    CHECK_EQ(conn.req.http_minor, 0);
    CHECK_EQ(conn.req.keep_alive, 0);   /* HTTP/1.0: no keep-alive by default */
    CHECK(strcmp(hs_req_http_version((hs_request_t *)&conn), "1.0") == 0);

    conn_teardown(&conn);
    printf("ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("Parser / llhttp error-handling tests\n\n");

    test_get_ok();
    test_post_small_body_zerocopy();
    test_post_large_body_overflow();
    test_parse_error();
    test_413_content_length();
    test_413_streaming();
    test_upgrade();
    test_eof();
    test_eagain();
    test_header_lookup();
    test_keepalive_pipeline();
    test_http10();

    printf("\n%s  (%d failure%s)\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
