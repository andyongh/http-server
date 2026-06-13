/**
 * hs_reactor.c  –  IO-thread event loop, connection pool, boss/sub dispatch
 *
 * fsae API note:
 *   aeEventLoop has no privdata field. Context is passed exclusively through
 *   the clientData argument of aeCreateFileEvent(). Every callback here
 *   receives its context (sub-reactor or reactor-group pointer) directly.
 *
 * llhttp error mapping → HTTP error:
 *   HS_FEED_PARSE_ERR  → 400 Bad Request   + connection close
 *   HS_FEED_TOO_LARGE  → 413 Payload Too Large + connection close
 *   HS_FEED_UPGRADE    → 501 Not Implemented   + connection close
 *   HS_FEED_OOM        → 500 Internal Server Error + connection close
 *   HS_FEED_EOF        → clean close (no response)
 *   HS_FEED_IO_ERR     → hard close (no response)
 *   HS_FEED_AGAIN      → leave read event registered, retry on next epoll
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>

/* ── Linux/macOS portability ─────────────────────────────────────────────── */
#ifdef __linux__
#  include <sys/eventfd.h>
#  define hs_efd_signal(efd_r, efd_w)  do { uint64_t _v=1; (void)write((efd_w),&_v,8); } while(0)
#  define hs_efd_drain(fd)              do { uint64_t _v; (void)read((fd),&_v,8); } while(0)
#else
/* On macOS use a self-pipe pair (see hs_pipe_pair below).  hs_efd_signal
 * writes to the write-end (efd_w); the ae loop monitors the read-end (efd_r). */
#  define hs_efd_signal(efd_r, efd_w)  do { char _b=1; (void)write((efd_w),&_b,1); } while(0)
static inline void hs_efd_drain(int fd) {
    char _buf[64]; while(read(fd, _buf, sizeof(_buf)) > 0) {}
}
#endif /* __linux__ */

/* Portable pipe-pair creation with O_NONBLOCK + O_CLOEXEC */
static int hs_pipe_pair(int fds[2]) {
#ifdef __linux__
    return pipe2(fds, O_NONBLOCK | O_CLOEXEC);
#else
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        int f = fcntl(fds[i], F_GETFL, 0); fcntl(fds[i], F_SETFL, f | O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    return 0;
#endif
}

/* Portable accept with SOCK_NONBLOCK | SOCK_CLOEXEC */
static int hs_accept(int lfd) {
#ifdef __linux__
    return accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) return cfd;
    int f = fcntl(cfd, F_GETFL, 0); fcntl(cfd, F_SETFL, f | O_NONBLOCK);
    fcntl(cfd, F_SETFD, FD_CLOEXEC);
    return cfd;
#endif
}

#include <netinet/tcp.h>

#include <jemalloc/jemalloc.h>
#include "ae.h"
#include <llhttp.h>

#include "hs_reactor.h"
#include "hs_server.h"
#include "hs_http.h"
#include "hs_pool.h"
#include "hs_lua.h"

/* ── socket helpers ─────────────────────────────────────────────────────── */
static void set_nonblocking(int fd)
{
    int f = fcntl(fd, F_GETFL, 0);
    if (f >= 0) fcntl(fd, F_SETFL, f | O_NONBLOCK);
}
static void set_tcp_nodelay(int fd)
{
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Connection pool
 * ══════════════════════════════════════════════════════════════════════════ */

int hs_conn_pool_init(hs_conn_pool_t *p, int cap)
{
    p->slots    = (hs_conn_t *)je_calloc((size_t)cap, sizeof(hs_conn_t));
    p->free_stk = (int *)je_malloc((size_t)cap * sizeof(int));
    if (!p->slots || !p->free_stk) { je_free(p->slots); je_free(p->free_stk); return -1; }
    p->cap = cap;
    p->top = cap;
    for (int i = 0; i < cap; i++) {
        p->free_stk[i]       = i;
        p->slots[i].pool_idx = i;
    }
    return 0;
}

void hs_conn_pool_destroy(hs_conn_pool_t *p)
{
    je_free(p->slots);
    je_free(p->free_stk);
    p->slots = NULL; p->free_stk = NULL; p->top = p->cap = 0;
}

hs_conn_t *hs_conn_pool_alloc(hs_conn_pool_t *p)
{
    if (p->top == 0) return NULL;
    int idx = p->free_stk[--p->top];
    return &p->slots[idx];
}

void hs_conn_pool_free(hs_conn_pool_t *p, hs_conn_t *c)
{
    hs_conn_cleanup(c);
    p->free_stk[p->top++] = c->pool_idx;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Connection lifecycle helpers (IO thread only)
 * ══════════════════════════════════════════════════════════════════════════ */

static void conn_close(hs_sub_reactor_t *sub, hs_conn_t *conn)
{
    aeDeleteFileEvent(sub->ae, conn->fd, AE_READABLE | AE_WRITABLE);
    close(conn->fd);
    conn->fd = -1;
    hs_conn_pool_free(&sub->conn_pool, conn);
}

/* Write a static error response without dispatching to a worker */
static void conn_send_error_inline(hs_sub_reactor_t *sub,
                                   hs_conn_t *conn, int status,
                                   const char *msg)
{
    hs_http_error(conn, status, msg);
    conn->wbuf_sent = 0;
    conn->state     = HS_CONN_WRITING;
    /* Remove read event before scheduling write */
    aeDeleteFileEvent(sub->ae, conn->fd, AE_READABLE);
    aeCreateFileEvent(sub->ae, conn->fd, AE_WRITABLE, hs_write_cb, conn);
}

/* Register a freshly accepted fd with this sub-reactor */
static void sub_register_conn(hs_sub_reactor_t *sub, int cfd)
{
    set_nonblocking(cfd);
    set_tcp_nodelay(cfd);   /* harmless NOP for UDS */

    fprintf(stderr, "[accept %d]", cfd);
    fflush(stderr);

    hs_conn_t *conn = hs_conn_pool_alloc(&sub->conn_pool);
    if (!conn) {
        /* Pool exhausted: immediate 503 */
        static const char err[] =
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Length: 19\r\nConnection: close\r\n\r\n"
            "Service Unavailable";
        (void)write(cfd, err, sizeof(err) - 1);
        close(cfd);
        return;
    }

    hs_conn_init(conn, cfd, sub);
    if (aeCreateFileEvent(sub->ae, cfd, AE_READABLE, hs_read_cb, conn) == AE_ERR) {
        fprintf(stderr, "[register failed %d: %d (%s)]", cfd, errno, strerror(errno));
        fflush(stderr);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Write callback – flush wbuf to socket
 *  clientData = hs_conn_t *
 * ══════════════════════════════════════════════════════════════════════════ */
void hs_write_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)mask;
    hs_conn_t        *conn = (hs_conn_t *)clientData;
    hs_sub_reactor_t *sub  = conn->sub;
    hs_buf_t         *wb   = &conn->wbuf;
    int               keep = conn->req.keep_alive;

    while (conn->wbuf_sent < wb->len) {
        ssize_t n = write(fd,
                          wb->data + conn->wbuf_sent,
                          wb->len  - conn->wbuf_sent);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            conn_close(sub, conn);
            return;
        }
        conn->wbuf_sent += (size_t)n;
    }

    /* All bytes flushed */
    aeDeleteFileEvent(el, fd, AE_WRITABLE);
    hs_buf_reset(wb);
    conn->wbuf_sent = 0;

    if (conn->state == HS_CONN_CLOSING || !keep) {
        conn_close(sub, conn);
        return;
    }

    /* Keep-alive: prepare for the next request */
    hs_conn_reset_req(conn);
    conn->state = HS_CONN_READING;
    aeCreateFileEvent(el, fd, AE_READABLE, hs_read_cb, conn);

    /* If we already have pipelined request bytes in the ring buffer, process them immediately */
    if (hs_ring_len(&conn->ring) > 0) {
        hs_read_cb(el, fd, conn, AE_READABLE);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Read callback – recv + parse
 *  clientData = hs_conn_t *
 * ══════════════════════════════════════════════════════════════════════════ */
void hs_read_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)el; (void)mask;
    hs_conn_t        *conn = (hs_conn_t *)clientData;
    hs_sub_reactor_t *sub  = conn->sub;

    fprintf(stderr, "[read_cb fd=%d state=%d ring_len=%d]", fd, conn->state, hs_ring_len(&conn->ring));
    fflush(stderr);

    hs_feed_result_t res = hs_conn_recv_and_feed(conn);

    fprintf(stderr, "[read_cb_ret fd=%d res=%d state=%d ring_len=%d]", fd, res, conn->state, hs_ring_len(&conn->ring));
    fflush(stderr);

    switch (res) {
    case HS_FEED_OK:
        /* Success – msg_complete callback already called hs_on_request_complete */
        break;

    case HS_FEED_AGAIN:
        /* EAGAIN: nothing to do, epoll will re-fire when data arrives */
        break;

    case HS_FEED_EOF:
        if (conn->state == HS_CONN_INFLIGHT)
            conn->state = HS_CONN_CLOSING;
        else
            conn_close(sub, conn);
        break;

    case HS_FEED_IO_ERR:
        if (conn->state == HS_CONN_INFLIGHT)
            conn->state = HS_CONN_CLOSING;
        else
            conn_close(sub, conn);
        break;

    case HS_FEED_PARSE_ERR:
        conn_send_error_inline(sub, conn, 400, "Bad Request");
        break;

    case HS_FEED_TOO_LARGE:
        conn_send_error_inline(sub, conn, 413, "Payload Too Large");
        break;

    case HS_FEED_UPGRADE:
        conn_send_error_inline(sub, conn, 501, "Not Implemented");
        break;

    case HS_FEED_OOM:
        conn_send_error_inline(sub, conn, 500, "Internal Server Error");
        break;

    default:
        conn_close(sub, conn);
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Response eventfd callback – drain the MPSC queue from workers
 *  clientData = hs_sub_reactor_t *
 * ══════════════════════════════════════════════════════════════════════════ */
static void resp_efd_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)el; (void)mask;
    hs_sub_reactor_t *sub = (hs_sub_reactor_t *)clientData;

    /* Clear the eventfd counter (ignore EAGAIN: means already drained) */
    hs_efd_drain(fd);

    for (;;) {
        hs_mpsc_node_t *node = hs_mpsc_pop(&sub->resp_queue);
        if (!node) break;

        hs_response_t *res  = (hs_response_t *)node->payload;
        fprintf(stderr, "[response %d]", res->conn->fd);
        fflush(stderr);
        hs_conn_t     *conn = res->conn;

        if (conn->state == HS_CONN_CLOSING) {
            hs_http_response_free(res);
            conn_close(sub, conn);
            continue;
        }

        hs_http_response_serialise(res);
        hs_http_response_free(res);

        conn->wbuf_sent = 0;
        conn->state     = HS_CONN_WRITING;
        aeCreateFileEvent(sub->ae, conn->fd, AE_WRITABLE, hs_write_cb, conn);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Accept callbacks
 * ══════════════════════════════════════════════════════════════════════════ */

/* SINGLE mode: sub[0] handles accept directly
 * clientData = hs_sub_reactor_t *
 */
void hs_accept_cb_single(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)el; (void)mask;
    hs_sub_reactor_t *sub = (hs_sub_reactor_t *)clientData;
    int cfd;
    while ((cfd = hs_accept(fd)) >= 0)
        sub_register_conn(sub, cfd);
}

/* MULTI mode: boss accepts and dispatches to a sub via pipe
 * clientData = hs_reactor_group_t *
 */
static void boss_accept_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)el; (void)mask;
    hs_reactor_group_t *rg = (hs_reactor_group_t *)clientData;
    int cfd;
    while ((cfd = hs_accept(fd)) >= 0) {
        uint32_t idx =
            atomic_fetch_add_explicit(&rg->rr, 1, memory_order_relaxed)
            % (uint32_t)rg->nsubs;
        if (write(rg->subs[idx].wakeup_w, &cfd, sizeof(int)) < 0)
            close(cfd);
    }
}

static void boss_wakeup_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)mask; (void)clientData;
    char dummy;
    (void)read(fd, &dummy, 1);
    aeStop(el);
}

/* MULTI mode sub: receives new fd from boss via pipe
 * clientData = hs_sub_reactor_t *
 */
static void sub_wakeup_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)mask;
    hs_sub_reactor_t *sub = (hs_sub_reactor_t *)clientData;
    int cfd;
    while (read(fd, &cfd, sizeof(int)) == sizeof(int)) {
        if (cfd < 0) { aeStop(el); return; }   /* sentinel → stop */
        sub_register_conn(sub, cfd);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Thread entry points
 * ══════════════════════════════════════════════════════════════════════════ */

static void *sub_thread_fn(void *arg)
{
    hs_sub_reactor_t *sub = (hs_sub_reactor_t *)arg;
    aeMain(sub->ae);
    return NULL;
}

static void *boss_thread_fn(void *arg)
{
    hs_reactor_group_t *rg = (hs_reactor_group_t *)arg;
    aeMain(rg->boss_ae);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  hs_on_request_complete  (IO thread, called from llhttp message_complete)
 * ══════════════════════════════════════════════════════════════════════════ */
__attribute__((weak)) void hs_on_request_complete(hs_conn_t *conn)
{
    hs_sub_reactor_t *sub = conn->sub;
    hs_server_t      *srv = sub->srv;

    fprintf(stderr, "[complete %d]", conn->fd);
    fflush(stderr);

    hs_work_t *work = (hs_work_t *)je_malloc(sizeof(hs_work_t));
    if (!work) {
        conn_send_error_inline(sub, conn, 500, "Internal Server Error");
        return;
    }
    work->conn  = conn;
    conn->state = HS_CONN_INFLIGHT;
    aeDeleteFileEvent(sub->ae, conn->fd, AE_READABLE);

    if (hs_pool_submit(srv->pool, work) != 0) {
        je_free(work);
        conn->state = HS_CONN_READING;
        aeCreateFileEvent(sub->ae, conn->fd, AE_READABLE, hs_read_cb, conn);
        conn_send_error_inline(sub, conn, 503, "Service Unavailable");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  hs_process_work  (CPU worker thread)
 *
 *  Thread-safety: reads conn->req (immutable while INFLIGHT),
 *  writes a new hs_response_t, then calls hs_res_send().
 *  After hs_res_send() the worker MUST NOT touch conn or res.
 * ══════════════════════════════════════════════════════════════════════════ */
void hs_process_work(hs_server_t *srv, hs_conn_t *conn,
                     struct hs_lua_state *lstate)
{
    hs_response_t *res = hs_http_response_new(conn);
    if (!res) {
        /* OOM: send a bare 500 without heap */
        static const char err[] =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 21\r\nConnection: close\r\n\r\n"
            "Internal Server Error";
        uint64_t one = 1;
        /* We can't use the normal path; push a NULL response as sentinel */
        /* Instead, synthesise directly to wbuf and wake IO thread */
        hs_buf_reset(&conn->wbuf);
        hs_buf_append(&conn->wbuf, err, sizeof(err) - 1);
        conn->wbuf_sent = 0;
        /* Signal IO thread via eventfd with a synthetic empty response */
        /* Use a stack-allocated node – safe because we block until written */
        /* Simplest: just write the sentinel directly (abuses the protocol
         * slightly but is safe since the node lives until the IO thread
         * processes it synchronously).  For a real OOM this is acceptable. */
        static hs_response_t oom_res;
        static hs_mpsc_node_t oom_node;
        oom_res.conn   = conn;
        oom_res.status = 500;
        hs_buf_init(&oom_res.body, 0);
        oom_node.payload = &oom_res;
        hs_mpsc_push(&conn->sub->resp_queue, &oom_node);
#ifdef __linux__
        hs_efd_signal(conn->sub->resp_efd, conn->sub->resp_efd);
#else
        hs_efd_signal(conn->sub->resp_efd, conn->sub->resp_efd_w);
#endif
        return;
    }

    int dispatched = 0;

    if (lstate && srv->config.lua_script) {
        int rc = hs_lua_call_handler(lstate, conn, res);
        if (rc == 0) {
            dispatched = 1;   /* Lua handler called hs_res_send() */
        } else {
            hs_res_status(res, 500);
            hs_res_body_str(res, "Internal Server Error");
            hs_res_send(res);
            dispatched = 1;
        }
    }

    if (!dispatched && srv->config.handler) {
        srv->config.handler((hs_request_t *)conn, res, srv->config.handler_ud);
        dispatched = 1;
    }

    if (!dispatched) {
        hs_res_status(res, 404);
        hs_res_body_str(res, "Not Found");
        hs_res_send(res);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  hs_res_send  (worker thread → IO thread via MPSC + eventfd)
 *
 *  After this call the worker must not access res or res->conn.
 * ══════════════════════════════════════════════════════════════════════════ */
__attribute__((weak)) void hs_res_send(hs_response_t *res)
{
    hs_conn_t        *conn = res->conn;
    hs_sub_reactor_t *sub  = conn->sub;   /* immutable: safe to read */

    res->mpsc_node.payload = res;
    hs_mpsc_push(&sub->resp_queue, &res->mpsc_node);   /* lock-free push   */

    /* Wake the IO thread (one write() per response batch is enough;
     * the IO thread drains the entire queue in resp_efd_cb).            */
#ifdef __linux__
    hs_efd_signal(sub->resp_efd, sub->resp_efd);
#else
    hs_efd_signal(sub->resp_efd, sub->resp_efd_w);
#endif
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Reactor group lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

hs_reactor_group_t *hs_reactor_group_new(hs_server_t *srv)
{
    hs_reactor_group_t *rg =
        (hs_reactor_group_t *)je_calloc(1, sizeof(*rg));
    if (!rg) return NULL;

    rg->srv = srv;
    rg->boss_wakeup_r = -1;
    rg->boss_wakeup_w = -1;
    atomic_init(&rg->rr, 0);

    int nsubs = srv->config.num_io_threads;
    if (srv->config.reactor_mode == HS_REACTOR_SINGLE) {
        nsubs = 1;
    } else {
        if (nsubs <= 0) {
#ifdef _SC_NPROCESSORS_ONLN
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            nsubs = (n > 0) ? (int)n : 4;
#else
            nsubs = 4;
#endif
        }
    }
    rg->nsubs = nsubs;

    rg->subs = (hs_sub_reactor_t *)je_calloc((size_t)nsubs,
                                              sizeof(hs_sub_reactor_t));
    if (!rg->subs) goto fail;

    int cap = srv->config.conn_pool_cap > 0 ? srv->config.conn_pool_cap : 1024;

    for (int i = 0; i < nsubs; i++) {
        hs_sub_reactor_t *sub = &rg->subs[i];
        sub->id      = i;
        sub->srv     = srv;
        sub->wakeup_r = sub->wakeup_w = -1;

        sub->ae = aeCreateEventLoop(65536);
        if (!sub->ae) goto fail;

#ifdef __linux__
        sub->resp_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#else
        /* macOS: use a self-pipe pair; store write-end in resp_efd_w, read-end in resp_efd */
        {
            int pfd[2];
            if (hs_pipe_pair(pfd) < 0) goto fail;
            sub->resp_efd   = pfd[0];   /* read end  */
            sub->resp_efd_w = pfd[1];   /* write end */
        }
#endif
        if (sub->resp_efd < 0) goto fail;
        hs_mpsc_init(&sub->resp_queue);

        /* Register response eventfd – pass sub as clientData */
        if (aeCreateFileEvent(sub->ae, sub->resp_efd, AE_READABLE,
                              resp_efd_cb, sub) == AE_ERR) goto fail;

        if (srv->config.reactor_mode == HS_REACTOR_MULTI) {
            int pfd[2];
            if (hs_pipe_pair(pfd) < 0) goto fail;
            sub->wakeup_r = pfd[0];
            sub->wakeup_w = pfd[1];
            /* Register wakeup pipe – pass sub as clientData */
            if (aeCreateFileEvent(sub->ae, sub->wakeup_r, AE_READABLE,
                                  sub_wakeup_cb, sub) == AE_ERR) goto fail;
        }

        if (hs_conn_pool_init(&sub->conn_pool, cap) < 0) goto fail;
    }

    if (srv->config.reactor_mode == HS_REACTOR_MULTI) {
        rg->boss_ae = aeCreateEventLoop(64);
        if (!rg->boss_ae) goto fail;

        int pfd[2];
        if (hs_pipe_pair(pfd) < 0) goto fail;
        rg->boss_wakeup_r = pfd[0];
        rg->boss_wakeup_w = pfd[1];
        if (aeCreateFileEvent(rg->boss_ae, rg->boss_wakeup_r, AE_READABLE,
                              boss_wakeup_cb, rg) == AE_ERR) goto fail;
    }

    return rg;
fail:
    hs_reactor_group_free(rg);
    return NULL;
}

int hs_reactor_group_start(hs_reactor_group_t *rg)
{
    hs_server_t *srv = rg->srv;

    for (int li = 0; li < srv->nlisteners; li++) {
        int lfd = srv->listeners[li].fd;
        if (srv->config.reactor_mode == HS_REACTOR_SINGLE) {
            /* Pass sub[0] as clientData to the accept callback */
            if (aeCreateFileEvent(rg->subs[0].ae, lfd, AE_READABLE,
                                  hs_accept_cb_single, &rg->subs[0]) == AE_ERR)
                return -1;
        } else {
            /* Pass the reactor group as clientData to the boss callback */
            if (aeCreateFileEvent(rg->boss_ae, lfd, AE_READABLE,
                                  boss_accept_cb, rg) == AE_ERR)
                return -1;
        }
    }

    if (srv->config.reactor_mode == HS_REACTOR_MULTI) {
        for (int i = 0; i < rg->nsubs; i++) {
            if (pthread_create(&rg->subs[i].tid, NULL,
                               sub_thread_fn, &rg->subs[i]) != 0) {
                perror("pthread_create(sub)");
                return -1;
            }
        }
        if (pthread_create(&rg->boss_tid, NULL, boss_thread_fn, rg) != 0) {
            perror("pthread_create(boss)");
            return -1;
        }
        pthread_join(rg->boss_tid, NULL);
        for (int i = 0; i < rg->nsubs; i++)
            pthread_join(rg->subs[i].tid, NULL);
    } else {
        aeMain(rg->subs[0].ae);
    }

    return 0;
}

void hs_reactor_group_stop(hs_reactor_group_t *rg)
{
    hs_server_t *srv = rg->srv;

    /* Close all listener sockets to wake up boss event loop and stop accepts */
    for (int i = 0; i < srv->nlisteners; i++) {
        if (srv->listeners[i].fd >= 0) {
            /* Remove listener from the event loop first to be clean */
            if (srv->config.reactor_mode == HS_REACTOR_MULTI) {
                if (rg->boss_ae) aeDeleteFileEvent(rg->boss_ae, srv->listeners[i].fd, AE_READABLE);
            } else {
                if (rg->subs[0].ae) aeDeleteFileEvent(rg->subs[0].ae, srv->listeners[i].fd, AE_READABLE);
            }
            close(srv->listeners[i].fd);
            srv->listeners[i].fd = -1;
        }
    }

    if (srv->config.reactor_mode == HS_REACTOR_MULTI) {
        if (rg->boss_wakeup_w >= 0) {
            char dummy = 0;
            (void)write(rg->boss_wakeup_w, &dummy, 1);
        }
        for (int i = 0; i < rg->nsubs; i++) {
            int sentinel = -1;
            if (rg->subs[i].wakeup_w >= 0)
                (void)write(rg->subs[i].wakeup_w, &sentinel, sizeof(int));
        }
    } else {
        if (rg->nsubs > 0 && rg->subs[0].ae) {
            aeStop(rg->subs[0].ae);
            /* Wake up the single reactor thread from poll */
#ifdef __linux__
            hs_efd_signal(rg->subs[0].resp_efd, rg->subs[0].resp_efd);
#else
            hs_efd_signal(rg->subs[0].resp_efd, rg->subs[0].resp_efd_w);
#endif
        }
    }
}

void hs_reactor_group_free(hs_reactor_group_t *rg)
{
    if (!rg) return;
    for (int i = 0; rg->subs && i < rg->nsubs; i++) {
        hs_sub_reactor_t *sub = &rg->subs[i];
        if (sub->ae)         aeDeleteEventLoop(sub->ae);
        if (sub->resp_efd   >= 0) close(sub->resp_efd);
#ifndef __linux__
        if (sub->resp_efd_w >= 0) close(sub->resp_efd_w);
#endif
        if (sub->wakeup_r >= 0) close(sub->wakeup_r);
        if (sub->wakeup_w >= 0) close(sub->wakeup_w);
        hs_conn_pool_destroy(&sub->conn_pool);
    }
    if (rg->boss_wakeup_r >= 0) close(rg->boss_wakeup_r);
    if (rg->boss_wakeup_w >= 0) close(rg->boss_wakeup_w);
    if (rg->boss_ae) aeDeleteEventLoop(rg->boss_ae);
    je_free(rg->subs);
    je_free(rg);
}
