/**
 * hs_reactor.c  –  Single-Reactor IO event loop + connection pool
 *
 * v0.4-lite changes:
 *   - Multi-Reactor (boss/sub) removed entirely
 *   - hs_sub_reactor_t → hs_reactor_t  (single struct)
 *   - hs_reactor_group_t removed
 *   - Dispatch modes: INLINE (handler in reactor thread) vs WORKER (thread pool)
 *   - jemalloc optional (HS_USE_JEMALLOC=0 → system malloc)
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

/* ── memory allocator shim (jemalloc optional) ───────────────────────────── */
#include "hs_alloc.h"

/* ── Linux/macOS portability ─────────────────────────────────────────────── */
#ifdef __linux__
#  include <sys/eventfd.h>
#  define hs_efd_signal(efd_r, efd_w)  do { uint64_t _v=1; (void)write((efd_w),&_v,8); } while(0)
#  define hs_efd_drain(fd)              do { uint64_t _v; (void)read((fd),&_v,8); } while(0)
#else
/* On macOS use a self-pipe pair.  hs_efd_signal writes to the write-end (efd_w);
 * the ae loop monitors the read-end (efd_r). */
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

#include "ae.h"
#include <llhttp.h>

#include "hs_reactor.h"
#include "hs_server.h"
#include "hs_http.h"
#include "hs_pool.h"
#include "hs_lua.h"
#include "hs_lua_dir.h"
#include "hs_log.h"

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
    p->slots    = (hs_conn_t *)hs_calloc((size_t)cap, sizeof(hs_conn_t));
    p->free_stk = (int *)hs_malloc((size_t)cap * sizeof(int));
    if (!p->slots || !p->free_stk) { hs_free(p->slots); hs_free(p->free_stk); return -1; }
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
    hs_free(p->slots);
    hs_free(p->free_stk);
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

static void conn_close(hs_reactor_t *r, hs_conn_t *conn)
{
    aeDeleteFileEvent(r->ae, conn->fd, AE_READABLE | AE_WRITABLE);
    close(conn->fd);
    conn->fd = -1;
    hs_conn_pool_free(&r->conn_pool, conn);
}

/* Write a static error response without dispatching to a worker */
static void conn_send_error_inline(hs_reactor_t *r,
                                   hs_conn_t *conn, int status,
                                   const char *msg)
{
    hs_http_error(conn, status, msg);
    conn->wbuf_sent = 0;
    conn->state     = HS_CONN_WRITING;
    aeDeleteFileEvent(r->ae, conn->fd, AE_READABLE);
    aeCreateFileEvent(r->ae, conn->fd, AE_WRITABLE, hs_write_cb, conn);
}

/* Register a freshly accepted fd with this reactor */
static void reactor_register_conn(hs_reactor_t *r, int cfd)
{
    set_nonblocking(cfd);
    set_tcp_nodelay(cfd);

    hs_log(HS_LOG_DEBUG, "accept fd=%d", cfd);

    hs_conn_t *conn = hs_conn_pool_alloc(&r->conn_pool);
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

    hs_conn_init(conn, cfd, r);
    if (aeCreateFileEvent(r->ae, cfd, AE_READABLE, hs_read_cb, conn) == AE_ERR) {
        hs_log(HS_LOG_ERROR, "register failed fd=%d: %d (%s)", cfd, errno, strerror(errno));
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Write callback – flush wbuf to socket
 *  clientData = hs_conn_t *
 * ══════════════════════════════════════════════════════════════════════════ */
void hs_write_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)mask;
    hs_conn_t    *conn = (hs_conn_t *)clientData;
    hs_reactor_t *r    = conn->reactor;
    hs_buf_t     *wb   = &conn->wbuf;
    int           keep = conn->req.keep_alive;

    while (conn->wbuf_sent < wb->len) {
        ssize_t n = write(fd,
                          wb->data + conn->wbuf_sent,
                          wb->len  - conn->wbuf_sent);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            conn_close(r, conn);
            return;
        }
        conn->wbuf_sent += (size_t)n;
    }

    /* All bytes flushed */
    aeDeleteFileEvent(el, fd, AE_WRITABLE);
    hs_buf_reset(wb);
    conn->wbuf_sent = 0;

    if (conn->state == HS_CONN_CLOSING || !keep) {
        conn_close(r, conn);
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
    hs_conn_t    *conn = (hs_conn_t *)clientData;
    hs_reactor_t *r    = conn->reactor;

    hs_log(HS_LOG_DEBUG, "read_cb fd=%d state=%d ring_len=%d",
           fd, conn->state, hs_ring_len(&conn->ring));

    hs_feed_result_t res = hs_conn_recv_and_feed(conn);

    hs_log(HS_LOG_DEBUG, "read_cb_ret fd=%d res=%d state=%d ring_len=%d",
           fd, res, conn->state, hs_ring_len(&conn->ring));

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
            conn_close(r, conn);
        break;

    case HS_FEED_IO_ERR:
        if (conn->state == HS_CONN_INFLIGHT)
            conn->state = HS_CONN_CLOSING;
        else
            conn_close(r, conn);
        break;

    case HS_FEED_PARSE_ERR:
        conn_send_error_inline(r, conn, 400, "Bad Request");
        break;

    case HS_FEED_TOO_LARGE:
        conn_send_error_inline(r, conn, 413, "Payload Too Large");
        break;

    case HS_FEED_UPGRADE:
        conn_send_error_inline(r, conn, 501, "Not Implemented");
        break;

    case HS_FEED_OOM:
        conn_send_error_inline(r, conn, 500, "Internal Server Error");
        break;

    default:
        conn_close(r, conn);
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Response eventfd/pipe callback – drain the MPSC queue from workers
 *  clientData = hs_reactor_t *
 * ══════════════════════════════════════════════════════════════════════════ */
static void resp_efd_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)el; (void)mask;
    hs_reactor_t *r = (hs_reactor_t *)clientData;

    hs_efd_drain(fd);

    for (;;) {
        hs_mpsc_node_t *node = hs_mpsc_pop(&r->resp_queue);
        if (!node) break;

        hs_response_t *res  = (hs_response_t *)node->payload;
        hs_log(HS_LOG_DEBUG, "response fd=%d", res->conn->fd);
        hs_conn_t     *conn = res->conn;

        if (conn->state == HS_CONN_CLOSING) {
            hs_http_response_free(res);
            conn_close(r, conn);
            continue;
        }

        hs_http_response_serialise(res);
        hs_http_response_free(res);

        conn->wbuf_sent = 0;
        conn->state     = HS_CONN_WRITING;
        aeCreateFileEvent(r->ae, conn->fd, AE_WRITABLE, hs_write_cb, conn);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Accept callback
 *  clientData = hs_reactor_t *
 * ══════════════════════════════════════════════════════════════════════════ */
void hs_accept_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)el; (void)mask;
    hs_reactor_t *r = (hs_reactor_t *)clientData;
    int cfd;
    while ((cfd = hs_accept(fd)) >= 0)
        reactor_register_conn(r, cfd);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Inline response helper (used by INLINE dispatch path in hs_process_work)
 *  Called from the reactor thread: serialise immediately, no eventfd needed.
 * ══════════════════════════════════════════════════════════════════════════ */
void hs_res_send_inline(hs_response_t *res)
{
    hs_conn_t    *conn = res->conn;
    hs_reactor_t *r    = conn->reactor;

    hs_http_response_serialise(res);
    hs_http_response_free(res);

    conn->wbuf_sent = 0;
    conn->state     = HS_CONN_WRITING;
    aeCreateFileEvent(r->ae, conn->fd, AE_WRITABLE, hs_write_cb, conn);
}

static hs_dispatch_mode_t hs_check_dispatch_mode(hs_server_t *srv, hs_conn_t *conn, hs_lua_state_t *lstate, hs_response_t *res)
{
    /* Lua handler takes priority */
    if (lstate && (srv->config.lua_script || srv->config.lua_dir)) {
        int mode = hs_lua_call_handler(lstate, conn, res);
        if (mode < 0) return HS_DISPATCH_INLINE; /* error path: run inline to return 500 */
        return (hs_dispatch_mode_t)mode;
    }

    if (srv->config.handler) {
        return srv->config.handler((hs_request_t *)conn, res, srv->config.handler_ud);
    }

    return HS_DISPATCH_INLINE;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  hs_on_request_complete  (IO thread, called from llhttp message_complete)
 *
 *  Determines dispatch mode:
 *    - No pool (num_threads == 0) → always INLINE
 *    - Pool available → call C handler once to get mode; if WORKER, push
 *      to pool and suspend; if INLINE, respond immediately.
 * ══════════════════════════════════════════════════════════════════════════ */
__attribute__((weak)) void hs_on_request_complete(hs_conn_t *conn)
{
    hs_reactor_t *r   = conn->reactor;
    hs_server_t  *srv = r->srv;

    hs_log(HS_LOG_DEBUG, "complete fd=%d", conn->fd);

    /* No pool → inline dispatch always */
    if (!srv->pool) {
        conn->in_worker = 0;
        hs_process_work(srv, conn, r->lstate);
        return;
    }

    /* Pool exists → decide dispatch mode */
    conn->in_worker = 0;
    hs_response_t *res = hs_http_response_new(conn);
    if (!res) {
        conn_send_error_inline(r, conn, 500, "Internal Server Error");
        return;
    }

    /* Check reload for reactor's lstate */
    if (r->lstate && srv->config.lua_dir &&
        hs_lua_dir_needs_reload(srv->config.lua_dir)) {
        hs_lua_state_free(r->lstate);
        const char *new_path = hs_lua_dir_main_script(srv->config.lua_dir);
        r->lstate = new_path ? hs_lua_state_new(new_path) : NULL;
    }

    hs_dispatch_mode_t mode = hs_check_dispatch_mode(srv, conn, r->lstate, res);

    if (mode == HS_DISPATCH_INLINE) {
        /* Inline. The C/Lua handler already called hs_res_send(res) internally,
         * which pushed to the MPSC resp_queue. So we do not free res here. */
    } else {
        /* Worker. Free the temporary response and delegate to worker pool. */
        hs_http_response_free(res);

        hs_work_t *work = (hs_work_t *)hs_malloc(sizeof(hs_work_t));
        if (!work) {
            conn_send_error_inline(r, conn, 500, "Internal Server Error");
            return;
        }
        work->conn  = conn;
        conn->state = HS_CONN_INFLIGHT;
        aeDeleteFileEvent(r->ae, conn->fd, AE_READABLE);

        if (hs_pool_submit(srv->pool, work) != 0) {
            hs_free(work);
            conn->state = HS_CONN_READING;
            aeCreateFileEvent(r->ae, conn->fd, AE_READABLE, hs_read_cb, conn);
            conn_send_error_inline(r, conn, 503, "Service Unavailable");
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  hs_process_work  (called from worker thread OR reactor thread for INLINE)
 *
 *  Thread-safety:
 *    Worker thread: reads conn->req (immutable while INFLIGHT),
 *    writes a new hs_response_t, then calls hs_res_send().
 *
 *    Reactor thread (INLINE / no pool): same, but hs_res_send calls
 *    hs_res_send_inline() which directly schedules the write event.
 * ══════════════════════════════════════════════════════════════════════════ */
void hs_process_work(hs_server_t *srv, hs_conn_t *conn,
                     struct hs_lua_state *lstate)
{
    hs_response_t *res = hs_http_response_new(conn);
    if (!res) {
        /* OOM: best-effort bare 500 */
        static const char err[] =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 21\r\nConnection: close\r\n\r\n"
            "Internal Server Error";
        (void)write(conn->fd, err, sizeof(err) - 1);
        conn->state = HS_CONN_CLOSING;
        return;
    }

    if (srv->pool) {
        conn->in_worker = 1;
    } else {
        conn->in_worker = 0;
    }

    int dispatched = 0;

    /* Lua handler takes priority */
    if (lstate && (srv->config.lua_script || srv->config.lua_dir)) {
        int rc = hs_lua_call_handler(lstate, conn, res);
        if (rc >= 0) {
            dispatched = 1;
        } else {
            hs_res_status(res, 500);
            hs_res_body_str(res, "Internal Server Error");
            hs_res_send(res);
            dispatched = 1;
        }
    }

    if (!dispatched && srv->config.handler) {
        (void)srv->config.handler((hs_request_t *)conn, res, srv->config.handler_ud);
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
 *  When called from the reactor thread itself (no-pool / inline case),
 *  the conn->reactor->resp_efd write wakes the epoll which is already
 *  blocked in aeMain – this is harmless and the drain happens immediately
 *  on the next event loop iteration.
 *
 *  After this call the caller must not access res or res->conn.
 * ══════════════════════════════════════════════════════════════════════════ */
__attribute__((weak)) void hs_res_send(hs_response_t *res)
{
    hs_conn_t    *conn = res->conn;
    hs_reactor_t *r    = conn->reactor;   /* immutable: safe to read */

    if (!conn->in_worker) {
        hs_res_send_inline(res);
    } else {
        res->mpsc_node.payload = res;
        hs_mpsc_push(&r->resp_queue, &res->mpsc_node);   /* lock-free push   */

        /* Wake the IO thread */
#ifdef __linux__
        hs_efd_signal(r->resp_efd, r->resp_efd);
#else
        hs_efd_signal(r->resp_efd, r->resp_efd_w);
#endif
    }
}

static long long reactor_tick_cb(struct aeEventLoop *el, long long id, void *clientData)
{
    (void)el; (void)id;
    hs_reactor_t *r = (hs_reactor_t *)clientData;
    if (r->lstate) {
        hs_lua_state_tick(r->lstate);
    }
    return 10; /* run again in 10ms */
}

hs_reactor_t *hs_reactor_new(hs_server_t *srv)
{
    hs_reactor_t *r = (hs_reactor_t *)hs_calloc(1, sizeof(*r));
    if (!r) return NULL;

    r->srv = srv;
    r->id  = 0;

    r->ae = aeCreateEventLoop(65536);
    if (!r->ae) goto fail;

    r->resp_efd = -1;
#ifndef __linux__
    r->resp_efd_w = -1;
#endif

    if (srv->config.num_threads > 0) {
#ifdef __linux__
        r->resp_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#else
        {
            int pfd[2];
            if (hs_pipe_pair(pfd) < 0) goto fail;
            r->resp_efd   = pfd[0];   /* read end  */
            r->resp_efd_w = pfd[1];   /* write end */
        }
#endif
        if (r->resp_efd < 0) goto fail;

        hs_mpsc_init(&r->resp_queue);

        if (aeCreateFileEvent(r->ae, r->resp_efd, AE_READABLE,
                              resp_efd_cb, r) == AE_ERR) goto fail;
    }

    int cap = srv->config.conn_pool_cap > 0 ? srv->config.conn_pool_cap : 1024;
    if (hs_conn_pool_init(&r->conn_pool, cap) < 0) goto fail;

    /* Initialize Lua state for inline dispatch */
    const char *lua_path = srv->config.lua_dir
        ? hs_lua_dir_main_script(srv->config.lua_dir)
        : srv->config.lua_script;

    if (lua_path) {
        r->lstate = hs_lua_state_new(lua_path);
        if (!r->lstate) {
            hs_log(HS_LOG_ERROR, "reactor: Lua init failed");
        } else {
            aeCreateTimeEvent(r->ae, 10, reactor_tick_cb, r, NULL);
        }
    }

    return r;

fail:
    hs_reactor_free(r);
    return NULL;
}

int hs_reactor_start(hs_reactor_t *r)
{
    hs_server_t *srv = r->srv;

    /* Register all listener sockets */
    for (int i = 0; i < srv->nlisteners; i++) {
        int lfd = srv->listeners[i].fd;
        if (aeCreateFileEvent(r->ae, lfd, AE_READABLE,
                              hs_accept_cb, r) == AE_ERR)
            return -1;
    }

    /* Run the event loop in the calling thread */
    aeMain(r->ae);
    return 0;
}

void hs_reactor_stop(hs_reactor_t *r)
{
    if (!r || !r->ae) return;
    aeStop(r->ae);
    /* Wake the event loop from poll if efd was initialized */
    if (r->resp_efd >= 0) {
#ifdef __linux__
        hs_efd_signal(r->resp_efd, r->resp_efd);
#else
        hs_efd_signal(r->resp_efd, r->resp_efd_w);
#endif
    }
}

void hs_reactor_free(hs_reactor_t *r)
{
    if (!r) return;
    if (r->lstate) {
        hs_lua_state_free(r->lstate);
    }
    if (r->ae)       aeDeleteEventLoop(r->ae);
    if (r->resp_efd  >= 0) close(r->resp_efd);
#ifndef __linux__
    if (r->resp_efd_w >= 0) close(r->resp_efd_w);
#endif
    hs_conn_pool_destroy(&r->conn_pool);
    hs_free(r);
}
