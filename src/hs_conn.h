/**
 * hs_conn.h  –  per-connection state
 *
 * Receive path: zero-malloc hs_ring_t (embedded, no heap alloc).
 *
 * Body handling:
 *   Content-Length > max_body_size  → HPE_USER → HS_FEED_TOO_LARGE (0 alloc)
 *   body fits ring, contiguous      → zero-copy ptr into ring->data
 *   body wraps ring or > RING_SIZE  → one je_malloc overflow buffer
 *
 * llhttp error taxonomy exposed via hs_feed_result_t:
 *   HS_FEED_OK           normal / message complete
 *   HS_FEED_AGAIN        EAGAIN from socket (no data yet)
 *   HS_FEED_PARSE_ERR    bad HTTP syntax → 400
 *   HS_FEED_TOO_LARGE    body > max_body_size → 413
 *   HS_FEED_UPGRADE      Upgrade / CONNECT not supported → 501
 *   HS_FEED_EOF          peer closed cleanly
 *   HS_FEED_IO_ERR       hard read() error → close
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <jemalloc/jemalloc.h>

static inline char *je_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = (char *)je_malloc(len);
    if (dup) {
        memcpy(dup, s, len);
    }
    return dup;
}

#include <llhttp.h>

#include "hs_ring.h"
#include "hs_buf.h"
#include "hs_queue.h"

/* ── llhttp parse outcome ──────────────────────────────────────────────── */
typedef enum {
    HS_FEED_OK         = 0,   /* message complete OR need more data        */
    HS_FEED_AGAIN      = 1,   /* EAGAIN from readv – caller retries later  */
    HS_FEED_PARSE_ERR  = 2,   /* bad HTTP syntax                → 400      */
    HS_FEED_TOO_LARGE  = 3,   /* body exceeded max_body_size    → 413      */
    HS_FEED_UPGRADE    = 4,   /* Upgrade / CONNECT              → 501      */
    HS_FEED_EOF        = 5,   /* peer closed connection cleanly            */
    HS_FEED_IO_ERR     = 6,   /* hard read error                → close    */
    HS_FEED_OOM        = 7,   /* overflow body alloc failed     → 500      */
} hs_feed_result_t;

#define HS_MAX_HEADERS 64

typedef struct { char *name; char *value; } hs_header_t;

/* ── parsed request (embedded in hs_conn_t) ────────────────────────────── */
typedef struct {
    int      method;
    int      http_major, http_minor;
    int      keep_alive;

    char     url[8192];

    hs_header_t headers[HS_MAX_HEADERS];
    int         nheaders;

    /* llhttp accumulator temporaries */
    char     _hdr_field[256];
    char     _hdr_value[8192];
    int      _hdr_field_ready;

    /* body accounting */
    size_t   body_received;
    size_t   content_length;   /* from Content-Length (0 = unknown/absent) */

    /*
     * Zero-copy body in ring:
     *   body_in_ring == 1 && overflow == NULL
     *   → ring.data[body_ring_idx .. +body_ring_len] is valid while INFLIGHT
     *     (no new reads happen on an INFLIGHT connection)
     */
    uint32_t body_ring_idx;
    uint32_t body_ring_len;
    int      body_in_ring;

    /* Overflow body (wrapping or large body) */
    char    *overflow;
    size_t   overflow_len;
    size_t   overflow_cap;

    /* parser state flags */
    unsigned url_complete     : 1;
    unsigned headers_complete : 1;
    unsigned msg_complete     : 1;
    unsigned body_413         : 1;   /* set when body limit exceeded        */
    unsigned body_upgrade     : 1;   /* set on Upgrade/CONNECT request      */
} hs_parsed_req_t;

/* ── connection state machine ──────────────────────────────────────────── */
typedef enum {
    HS_CONN_READING  = 0,
    HS_CONN_INFLIGHT,           /* request is with a worker                 */
    HS_CONN_WRITING,            /* flushing wbuf                            */
    HS_CONN_CLOSING,            /* peer EOF or error while INFLIGHT         */
} hs_conn_state_t;

struct hs_sub_reactor;

/* ── connection ─────────────────────────────────────────────────────────── */
typedef struct hs_conn {
    int               fd;
    hs_conn_state_t   state;

    struct hs_sub_reactor *sub;   /* owning IO thread (immutable after init) */

    /* HTTP/1.x parser */
    llhttp_t          parser;
    llhttp_settings_t parser_settings;

    /* ZERO-MALLOC receive ring (embedded, part of this struct) */
    hs_ring_t         ring;

    hs_parsed_req_t   req;

    /* Response write buffer (heap; allocated once, reused for keep-alive) */
    hs_buf_t          wbuf;
    size_t            wbuf_sent;

    /* MPSC node for the response queue */
    hs_mpsc_node_t    resp_node;

    /* Index in the owning sub-reactor's connection pool */
    int               pool_idx;
} hs_conn_t;

/* ── lifecycle ──────────────────────────────────────────────────────────── */
void hs_conn_init(hs_conn_t *conn, int fd, struct hs_sub_reactor *sub);
void hs_conn_reset_req(hs_conn_t *conn);
void hs_conn_cleanup(hs_conn_t *conn);

/**
 * hs_conn_recv_and_feed
 *
 * Read from the socket into the ring buffer, then feed all available ring
 * data into the llhttp parser (two-segment-aware for ring wrap-around).
 *
 * Returns one of the hs_feed_result_t values.
 *
 * On HS_FEED_OK with conn->req.msg_complete == 1 the caller should dispatch
 * the request to a worker thread.
 *
 * On any error value the caller is responsible for sending the appropriate
 * error response (or closing the connection).
 */
hs_feed_result_t hs_conn_recv_and_feed(hs_conn_t *conn);
