/**
 * hs_conn.c  –  connection management, llhttp callbacks, ring+overflow body
 *
 * llhttp error taxonomy (all cases handled):
 *
 *   HPE_OK              → HS_FEED_OK
 *   HPE_PAUSED          → after message_complete: normal keep-alive pause
 *                         before message_complete: treat as HS_FEED_PARSE_ERR
 *   HPE_PAUSED_UPGRADE  → HS_FEED_UPGRADE (501 Not Implemented)
 *   HPE_USER            → body_413 set → HS_FEED_TOO_LARGE (413)
 *                         otherwise (OOM in overflow) → HS_FEED_OOM (500)
 *   any other HPE_*     → HS_FEED_PARSE_ERR (400 Bad Request)
 *
 * Ring + overflow body rules:
 *   First on_body call: record body_ring_idx = (at - ring.data)
 *   Subsequent calls in same ring revolution: just grow body_ring_len
 *   If body_ring_idx + body_ring_len > RING_SIZE: wrap detected
 *     → allocate overflow, copy prior bytes, append current chunk
 *   If overflow pre-allocated (Content-Length > RING_SIZE): always copy
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#include "hs_alloc.h"
#include <llhttp.h>

#include "hs_conn.h"
#include "hs_log.h"
#include "hs_reactor.h"   /* hs_reactor_t, hs_on_request_complete */
#include "hs_server.h"

/* ─────────────────────── forward declarations ────────────────────────── */
static void req_commit_header(hs_conn_t *c);
static int  overflow_ensure(hs_parsed_req_t *r, size_t cap);
static int  overflow_append(hs_parsed_req_t *r, const char *d, size_t n);

/* ═══════════════════════════════════════════════════════════════════════
 *  llhttp callbacks (all run on the IO thread)
 * ═══════════════════════════════════════════════════════════════════════ */

static int cb_url(llhttp_t *p, const char *at, size_t len)
{
    hs_conn_t *c = (hs_conn_t *)p->data;
    size_t cur   = strlen(c->req.url);
    size_t room  = sizeof(c->req.url) - cur - 1;
    size_t cp    = len < room ? len : room;
    memcpy(c->req.url + cur, at, cp);
    c->req.url[cur + cp] = '\0';
    return HPE_OK;
}

static int cb_url_complete(llhttp_t *p)
{
    ((hs_conn_t *)p->data)->req.url_complete = 1;
    return HPE_OK;
}

static int cb_header_field(llhttp_t *p, const char *at, size_t len)
{
    hs_conn_t       *c = (hs_conn_t *)p->data;
    hs_parsed_req_t *r = &c->req;

    if (r->_hdr_field_ready)
        req_commit_header(c);   /* previous value is complete, commit it */

    size_t cur  = strlen(r->_hdr_field);
    size_t room = sizeof(r->_hdr_field) - cur - 1;
    size_t cp   = len < room ? len : room;
    memcpy(r->_hdr_field + cur, at, cp);
    r->_hdr_field[cur + cp] = '\0';
    return HPE_OK;
}

static int cb_header_value(llhttp_t *p, const char *at, size_t len)
{
    hs_conn_t       *c = (hs_conn_t *)p->data;
    hs_parsed_req_t *r = &c->req;
    size_t cur  = strlen(r->_hdr_value);
    size_t room = sizeof(r->_hdr_value) - cur - 1;
    size_t cp   = len < room ? len : room;
    memcpy(r->_hdr_value + cur, at, cp);
    r->_hdr_value[cur + cp] = '\0';
    r->_hdr_field_ready = 1;
    return HPE_OK;
}

static int cb_headers_complete(llhttp_t *p)
{
    hs_conn_t       *c = (hs_conn_t *)p->data;
    hs_parsed_req_t *r = &c->req;

    if (r->_hdr_field_ready)
        req_commit_header(c);

    r->method      = llhttp_get_method(p);
    r->http_major  = llhttp_get_http_major(p);
    r->http_minor  = llhttp_get_http_minor(p);
    r->keep_alive  = llhttp_should_keep_alive(p);
    r->headers_complete = 1;

    /* Detect Upgrade / CONNECT (we return 501 for these) */
    for (int i = 0; i < r->nheaders; i++) {
        if (strcasecmp(r->headers[i].name, "upgrade") == 0 ||
            r->method == HTTP_CONNECT) {
            r->body_upgrade = 1;
            return HPE_USER;   /* triggers HS_FEED_UPGRADE in feed fn */
        }
    }

    /* Extract Content-Length */
    for (int i = 0; i < r->nheaders; i++) {
        if (strcasecmp(r->headers[i].name, "content-length") == 0) {
            r->content_length = (size_t)strtoull(r->headers[i].value,NULL,10);
            break;
        }
    }

    /* ── 413 early-out on declared Content-Length ── */
    size_t max = c->reactor->srv->config.max_body_size;
    if (r->content_length > max) {
        r->body_413 = 1;
        return HPE_USER;
    }

    /* Pre-allocate overflow if body is known to exceed the ring */
    if (r->content_length > 0 && r->content_length > HS_RING_SIZE) {
        if (overflow_ensure(r, r->content_length) < 0) {
            r->body_413 = 0;   /* not 413, but OOM → triggers HS_FEED_OOM */
            return HPE_USER;
        }
    }

    return HPE_OK;
}

static int cb_body(llhttp_t *p, const char *at, size_t len)
{
    hs_conn_t       *c = (hs_conn_t *)p->data;
    hs_parsed_req_t *r = &c->req;

    r->body_received += len;

    /* ── streaming 413 check (chunked or no Content-Length) ── */
    if (r->body_received > c->reactor->srv->config.max_body_size) {
        r->body_413 = 1;
        return HPE_USER;
    }

    /* ── overflow path ── */
    if (r->overflow) {
        return overflow_append(r, at, len) == 0 ? HPE_OK : HPE_USER;
    }

    /* ── ring path ── */
    if (r->body_ring_len == 0) {
        /* First body byte: record physical index in ring.data */
        r->body_ring_idx = (uint32_t)(at - c->ring.data);
        r->body_in_ring  = 1;
    } else {
        /* Check for gap (non-contiguous write, e.g. chunked request) */
        const char *expected = c->ring.data + ((r->body_ring_idx + r->body_ring_len) % HS_RING_SIZE);
        if (at != expected) {
            size_t cap = r->content_length ? r->content_length : r->body_received;
            if (overflow_ensure(r, cap < r->body_received ? r->body_received : cap) < 0)
                return HPE_USER;   /* OOM */

            if (r->body_ring_len > 0) {
                uint32_t idx    = r->body_ring_idx;
                uint32_t remain = r->body_ring_len;
                uint32_t to_end = HS_RING_SIZE - idx;
                uint32_t p1     = remain < to_end ? remain : to_end;
                memcpy(r->overflow,      c->ring.data + idx, p1);
                memcpy(r->overflow + p1, c->ring.data,       remain - p1);
                r->overflow_len = remain;
            }
            r->body_in_ring  = 0;
            r->body_ring_len = 0;

            return overflow_append(r, at, len) == 0 ? HPE_OK : HPE_USER;
        }
    }

    uint32_t new_len = r->body_ring_len + (uint32_t)len;

    if (r->body_ring_idx + new_len > HS_RING_SIZE) {
        /*
         * Wrap detected: body straddles the physical end of the ring.
         * Allocate overflow and copy everything received so far.
         */
        size_t cap = r->content_length
                     ? r->content_length
                     : r->body_received;
        if (overflow_ensure(r, cap < r->body_received ? r->body_received : cap) < 0)
            return HPE_USER;   /* OOM */

        /* Copy already-accumulated ring portion */
        if (r->body_ring_len > 0) {
            uint32_t idx    = r->body_ring_idx;
            uint32_t remain = r->body_ring_len;
            uint32_t to_end = HS_RING_SIZE - idx;
            uint32_t p1     = remain < to_end ? remain : to_end;
            memcpy(r->overflow,      c->ring.data + idx, p1);
            memcpy(r->overflow + p1, c->ring.data,       remain - p1);
            r->overflow_len = remain;
        }
        r->body_in_ring  = 0;
        r->body_ring_len = 0;

        return overflow_append(r, at, len) == 0 ? HPE_OK : HPE_USER;
    }

    /* ── still contiguous in ring ── */
    r->body_ring_len = new_len;
    return HPE_OK;
}

static int cb_message_complete(llhttp_t *p)
{
    hs_conn_t       *c = (hs_conn_t *)p->data;
    hs_parsed_req_t *r = &c->req;
    c->req.msg_complete = 1;

    /*
     * Final contiguity check: if body_in_ring is set but the physical span
     * wraps (shouldn't normally reach here due to cb_body detection, but
     * guard defensively).
     */
    if (r->body_in_ring && r->body_ring_len > 0 &&
        !hs_ring_is_contiguous(r->body_ring_idx, r->body_ring_len)) {

        if (overflow_ensure(r, r->body_ring_len) == 0) {
            uint32_t idx    = r->body_ring_idx;
            uint32_t remain = r->body_ring_len;
            uint32_t to_end = HS_RING_SIZE - idx;
            uint32_t p1     = remain < to_end ? remain : to_end;
            memcpy(r->overflow,      c->ring.data + idx, p1);
            memcpy(r->overflow + p1, c->ring.data,       remain - p1);
            r->overflow_len  = remain;
            r->body_in_ring  = 0;
            r->body_ring_len = 0;
        }
    }

    /* Hand to the server – still on the IO thread */
    hs_on_request_complete(c);
    return HPE_PAUSED;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Helpers
 * ══════════════════════════════════════════════════════════════════════ */

static void req_commit_header(hs_conn_t *c)
{
    hs_parsed_req_t *r = &c->req;
    if (!r->_hdr_field_ready || r->nheaders >= HS_MAX_HEADERS) {
        r->_hdr_field[0] = r->_hdr_value[0] = '\0';
        r->_hdr_field_ready = 0;
        return;
    }
    int i = r->nheaders++;
    r->headers[i].name  = hs_strdup(r->_hdr_field);
    r->headers[i].value = hs_strdup(r->_hdr_value);
    r->_hdr_field[0] = r->_hdr_value[0] = '\0';
    r->_hdr_field_ready = 0;
}

static int overflow_ensure(hs_parsed_req_t *r, size_t cap)
{
    if (r->overflow) return 0;   /* already allocated */
    if (cap == 0) cap = HS_RING_SIZE;
    r->overflow = (char *)hs_malloc(cap);
    if (!r->overflow) return -1;
    r->overflow_cap = cap;
    r->overflow_len = 0;
    return 0;
}

static int overflow_append(hs_parsed_req_t *r, const char *d, size_t n)
{
    if (r->overflow_len + n > r->overflow_cap) {
        size_t nc = r->overflow_cap ? r->overflow_cap * 2 : n * 2;
        while (nc < r->overflow_len + n) nc *= 2;
        char *p = (char *)hs_realloc(r->overflow, nc);
        if (!p) return -1;
        r->overflow = p;
        r->overflow_cap = nc;
    }
    memcpy(r->overflow + r->overflow_len, d, n);
    r->overflow_len += n;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Parser init
 * ══════════════════════════════════════════════════════════════════════ */

static void parser_init(hs_conn_t *c)
{
    llhttp_settings_t *s = &c->parser_settings;
    llhttp_settings_init(s);
    s->on_url              = cb_url;
    s->on_url_complete     = cb_url_complete;
    s->on_header_field     = cb_header_field;
    s->on_header_value     = cb_header_value;
    s->on_headers_complete = cb_headers_complete;
    s->on_body             = cb_body;
    s->on_message_complete = cb_message_complete;
    llhttp_init(&c->parser, HTTP_REQUEST, s);
    c->parser.data = c;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════ */

void hs_conn_init(hs_conn_t *conn, int fd, struct hs_reactor *reactor)
{
    hs_buf_init(&conn->wbuf, 4096);
    conn->fd        = fd;
    conn->state     = HS_CONN_READING;
    conn->reactor   = reactor;
    conn->wbuf_sent = 0;
    hs_ring_init(&conn->ring);
    memset(&conn->req, 0, sizeof(conn->req));
    parser_init(conn);

    /* Initialize embedded response */
    conn->res.status = 200;
    conn->res.conn = conn;
    conn->res.nheaders = 0;
    hs_buf_init(&conn->res.body, 256);
}

void hs_conn_reset_req(hs_conn_t *conn)
{
    hs_parsed_req_t *r = &conn->req;
    for (int i = 0; i < r->nheaders; i++) {
        hs_free(r->headers[i].name);
        hs_free(r->headers[i].value);
    }
    hs_free(r->overflow);
    memset(r, 0, sizeof(*r));
    llhttp_reset(&conn->parser);

    /* Reset embedded response but keep allocated body capacity */
    for (int i = 0; i < conn->res.nheaders; i++) {
        hs_free(conn->res.headers[i].name);
        hs_free(conn->res.headers[i].value);
    }
    conn->res.nheaders = 0;
    conn->res.status = 200;
    hs_buf_reset(&conn->res.body);
}

void hs_conn_cleanup(hs_conn_t *conn)
{
    hs_parsed_req_t *r = &conn->req;
    for (int i = 0; i < r->nheaders; i++) {
        hs_free(r->headers[i].name);
        hs_free(r->headers[i].value);
    }
    hs_free(r->overflow);
    hs_buf_free(&conn->wbuf);

    /* Cleanup embedded response resources */
    for (int i = 0; i < conn->res.nheaders; i++) {
        hs_free(conn->res.headers[i].name);
        hs_free(conn->res.headers[i].value);
    }
    hs_buf_free(&conn->res.body);
}

/*
 * hs_conn_recv_and_feed
 * ─────────────────────
 * 1. readv() into the ring (two-iovec, handles wrap-around).
 * 2. Feed all available ring data to llhttp in up to two segments
 *    (before and after the physical wrap point), so every `at` pointer
 *    in a callback points directly into ring.data[].
 * 3. Map every possible llhttp outcome to a hs_feed_result_t.
 */
hs_feed_result_t hs_conn_recv_and_feed(hs_conn_t *conn)
{
    /* ── Step 1: receive ── */
    ssize_t rc = hs_ring_recv(&conn->ring, conn->fd);

    if (rc == -1) {
        if (hs_ring_len(&conn->ring) == 0) {
            return HS_FEED_AGAIN;   /* EAGAIN / EWOULDBLOCK        */
        }
    } else if (rc == 0) {
        if (hs_ring_len(&conn->ring) == 0) {
            return HS_FEED_EOF;     /* clean peer close            */
        }
    } else if (rc == -2) {
        return HS_FEED_IO_ERR;  /* hard error (errno preserved) */
    } else if (rc == -3) {
        conn->req.body_413 = 1;
        return HS_FEED_TOO_LARGE;
    }

    /* ── Step 2: feed ring segments to llhttp ── */
    const char *ptr;
    uint32_t    seg_len;
    uint32_t    total_fed = 0;
    llhttp_errno_t err    = HPE_OK;

    /* First contiguous segment */
    hs_ring_peek(&conn->ring, &ptr, &seg_len);
    if (seg_len == 0) return HS_FEED_OK;

    err = llhttp_execute(&conn->parser, ptr, (size_t)seg_len);
    if (err == HPE_OK) {
        total_fed += seg_len;

        /* Second segment (wrap-around remainder) – only if first was clean */
        uint32_t remaining = hs_ring_len(&conn->ring) - seg_len;
        if (remaining > 0) {
            /* Wrapped data always starts at physical index 0 */
            err = llhttp_execute(&conn->parser, conn->ring.data,
                                 (size_t)remaining);
            if (err == HPE_OK) {
                total_fed += remaining;
            } else {
                const char *error_pos = llhttp_get_error_pos(&conn->parser);
                if (error_pos && error_pos >= conn->ring.data && error_pos <= conn->ring.data + remaining) {
                    total_fed += (uint32_t)(error_pos - conn->ring.data);
                }
            }
        }
    } else {
        const char *error_pos = llhttp_get_error_pos(&conn->parser);
        if (error_pos && error_pos >= ptr && error_pos <= ptr + seg_len) {
            total_fed += (uint32_t)(error_pos - ptr);
        }
    }

    hs_ring_drain(&conn->ring, total_fed);

    /* ── Step 3: map llhttp result ── */
    if (err == HPE_OK) {
        return HS_FEED_OK;
    }

    if (err == HPE_PAUSED) {
        return conn->req.msg_complete ? HS_FEED_OK : HS_FEED_PARSE_ERR;
    }

    if (err == HPE_PAUSED_UPGRADE) {
        conn->req.body_upgrade = 1;
        return HS_FEED_UPGRADE;
    }

    if (conn->req.body_upgrade) {
        return HS_FEED_UPGRADE;
    }
    if (conn->req.body_413) {
        return HS_FEED_TOO_LARGE;
    }

    if (err == HPE_USER || err == HPE_CB_HEADERS_COMPLETE) {
        /* Must be OOM in overflow allocation */
        return HS_FEED_OOM;
    }

    /* All remaining HPE_* values are syntax errors → 400 */
    hs_log(HS_LOG_WARN, "fd=%d llhttp error: %s – %s",
            conn->fd,
            llhttp_errno_name(err),
            llhttp_get_error_reason(&conn->parser));
    return HS_FEED_PARSE_ERR;
}
