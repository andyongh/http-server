/**
 * httpserver.h  –  public API
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  SINGLE-REACTOR  (HS_REACTOR_SINGLE)                                    │
 * │                                                                         │
 * │   Calling thread ── fsae loop ── accept/read/write ── work_queue      │
 * │                                                         │               │
 * │                              CPU pool ◄────────────────┘               │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  MULTI-REACTOR  (HS_REACTOR_MULTI)                                      │
 * │                                                                         │
 * │   Boss thread ── fsae ── accept ──► pipe[i] ──► Sub-reactor[i]        │
 * │   (round-robin dispatch)             read/write                         │
 * │                                          │                              │
 * │                              CPU pool ◄──┘  (shared, M threads)        │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * RECEIVE PATH – zero-malloc ring buffer
 * ──────────────────────────────────────
 *   Each connection owns a pre-allocated hs_ring_t (HS_RING_SIZE bytes,
 *   embedded in hs_conn_t, no heap allocation).  readv() fills it with up
 *   to two iovecs to handle the wrap-around naturally.
 *
 *   Body handling:
 *     Content-Length > max_body_size  → 413 + close (no alloc)
 *     body fits in ring, contiguous   → zero-copy pointer into ring->data
 *     body wraps ring OR > ring size  → one je_malloc("overflow buffer")
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ── opaque handles ──────────────────────────────────────────────────────── */
typedef struct hs_server   hs_server_t;
typedef struct hs_request  hs_request_t;
typedef struct hs_response hs_response_t;

/* ── listen flags (OR-able) ─────────────────────────────────────────────── */
typedef enum {
    HS_LISTEN_TCP  = 0x01,   /* bind to host:port   */
    HS_LISTEN_UDS  = 0x02,   /* AF_UNIX socket path */
} hs_listen_flags_t;

/* ── reactor model ──────────────────────────────────────────────────────── */
typedef enum {
    HS_REACTOR_SINGLE = 0,   /* one IO thread  (calling thread)      */
    HS_REACTOR_MULTI  = 1,   /* boss + N sub-reactor IO threads      */
} hs_reactor_mode_t;

/* ── handler signature ──────────────────────────────────────────────────── */
typedef void (*hs_handler_fn)(hs_request_t *req, hs_response_t *res,
                              void *user_data);

/* ── server configuration ───────────────────────────────────────────────── */
typedef struct {
    /* ── listen ────────────────────────────────────────────────────────── */
    hs_listen_flags_t listen_flags;  /* default: HS_LISTEN_TCP            */
    const char       *host;          /* TCP bind addr, default "0.0.0.0"  */
    uint16_t          port;          /* TCP port, default 8080             */
    int               backlog;       /* listen() backlog, default 1024     */
    const char       *uds_path;      /* UDS socket path (HS_LISTEN_UDS)   */

    /* ── reactor ────────────────────────────────────────────────────────── */
    hs_reactor_mode_t reactor_mode;  /* default: HS_REACTOR_SINGLE        */
    int               num_io_threads;/* sub-reactors (0=auto, MULTI only) */

    /* ── CPU workers ────────────────────────────────────────────────────── */
    int               num_threads;   /* 0 = nproc                          */

    /* ── per-reactor connection pool ────────────────────────────────────── */
    int               conn_pool_cap; /* connections per reactor, def 1024  */

    /* ── limits ─────────────────────────────────────────────────────────── */
    size_t            max_body_size; /* default 4 MiB; 413 if exceeded     */
    size_t            max_url_len;   /* default 8192                       */
    int               max_headers;   /* default 64                         */

    /* ── C handler (mutually exclusive with lua_script) ─────────────────── */
    hs_handler_fn     handler;
    void             *handler_ud;

    /* ── Lua handler (takes priority over C handler when set) ───────────── */
    const char       *lua_script;
} hs_config_t;

void hs_config_init(hs_config_t *cfg);   /* fill with sane defaults */

/* ── server lifecycle ───────────────────────────────────────────────────── */
hs_server_t *hs_server_new(const hs_config_t *cfg);
int          hs_server_run(hs_server_t *srv);   /* blocks                  */
void         hs_server_stop(hs_server_t *srv);  /* signal-handler safe     */
void         hs_server_free(hs_server_t *srv);

/* ── request accessors (valid until hs_res_send()) ──────────────────────── */
int         hs_req_method(const hs_request_t *req);        /* llhttp int   */
const char *hs_req_method_str(const hs_request_t *req);
const char *hs_req_url(const hs_request_t *req);
const char *hs_req_header(const hs_request_t *req, const char *name);
const char *hs_req_body(const hs_request_t *req, size_t *out_len);
const char *hs_req_http_version(const hs_request_t *req);

/* ── response builder ───────────────────────────────────────────────────── */
void hs_res_status(hs_response_t *res, int code);
void hs_res_header(hs_response_t *res, const char *name, const char *value);
void hs_res_body(hs_response_t *res, const char *data, size_t len);
void hs_res_body_str(hs_response_t *res, const char *str);
void hs_res_send(hs_response_t *res);   /* MUST be called exactly once     */

/* ── logging ────────────────────────────────────────────────────────────── */
typedef enum {
    HS_LOG_DEBUG = 0,
    HS_LOG_INFO  = 1,
    HS_LOG_WARN  = 2,
    HS_LOG_ERROR = 3,
    HS_LOG_FATAL = 4,
    HS_LOG_OFF   = 5     /* disable all output */
} hs_log_level_t;

typedef struct {
    hs_log_level_t  level;
    const char     *file;       /* source file  (__FILE__)          */
    int             line;       /* source line  (__LINE__)          */
    const char     *msg;        /* formatted message                */
    struct timespec ts;         /* CLOCK_REALTIME, ns precision     */
    unsigned long   tid;        /* OS thread id                     */
} hs_log_event_t;

typedef void (*hs_log_cb)(const hs_log_event_t *ev, void *user_data);

/**
 * Register a log callback.  Pass NULL to restore the default stderr logger.
 * Thread-safe (atomic store).
 */
void hs_log_register(hs_log_cb cb, void *user_data);

/**
 * Set the minimum log level.  Messages below this level are dropped
 * before any formatting work is done.  Default: HS_LOG_INFO.
 * Thread-safe (atomic store).
 */
void hs_log_set_level(hs_log_level_t level);

/**
 * hs_log() is a macro that captures __FILE__ and __LINE__.
 * Usage:  hs_log(HS_LOG_INFO, "listening on %s:%d", host, port);
 */
void hs_log_impl(hs_log_level_t level,
                 const char *file, int line,
                 const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
;

#define hs_log(level, ...) \
    hs_log_impl((level), __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
