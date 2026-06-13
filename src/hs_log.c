/**
 * hs_log.c  –  production logger with structured events
 *
 * Features:
 *   • Atomic level gate: messages below the configured minimum are
 *     dropped before vsnprintf, so DEBUG in the hot path is free.
 *   • Structured hs_log_event_t with timestamp, file:line, thread ID.
 *   • Pluggable callback: users receive the full event struct.
 *   • Default stderr formatter: ISO-8601 timestamp, level, file:line, message.
 *   • Thread-safe registration (lock-free atomics, no mutex on hot path).
 */
#define _GNU_SOURCE
#include "hs_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>

/* ── portable thread ID ──────────────────────────────────────────────── */
#ifdef __APPLE__
#  include <pthread.h>
static inline unsigned long hs_gettid(void) {
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return (unsigned long)tid;
}
#elif defined(__linux__)
#  include <sys/syscall.h>
#  include <unistd.h>
static inline unsigned long hs_gettid(void) {
    return (unsigned long)syscall(SYS_gettid);
}
#else
#  include <pthread.h>
static inline unsigned long hs_gettid(void) {
    return (unsigned long)(uintptr_t)pthread_self();
}
#endif

/* ── globals (atomic, lock-free) ─────────────────────────────────────── */
static _Atomic(hs_log_cb)     g_log_cb    = NULL;
static _Atomic(void *)        g_log_ud    = NULL;
static _Atomic(hs_log_level_t) g_min_level = HS_LOG_INFO;

void hs_log_register(hs_log_cb cb, void *user_data)
{
    atomic_store_explicit(&g_log_cb, cb,        memory_order_release);
    atomic_store_explicit(&g_log_ud, user_data,  memory_order_release);
}

void hs_log_set_level(hs_log_level_t level)
{
    atomic_store_explicit(&g_min_level, level, memory_order_release);
}

/* ── level strings ───────────────────────────────────────────────────── */
static const char *level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static const char *level_name(hs_log_level_t lvl)
{
    if (lvl >= 0 && lvl <= HS_LOG_FATAL) return level_names[lvl];
    return "???";
}

/* ── strip path to basename ──────────────────────────────────────────── */
static const char *basename_of(const char *path)
{
    const char *p = path;
    for (const char *s = path; *s; s++)
        if (*s == '/') p = s + 1;
    return p;
}

/* ── default stderr formatter ────────────────────────────────────────── */
/*
 * Output format:
 *   2026-06-13T20:14:05.123  INFO  [tid:12345] hs_server.c:70  message text
 */
static void default_stderr(const hs_log_event_t *ev)
{
    struct tm tm;
    localtime_r(&ev->ts.tv_sec, &tm);

    char ts_buf[32];
    int n = (int)strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(ts_buf + n, sizeof(ts_buf) - (size_t)n,
             ".%03ld", ev->ts.tv_nsec / 1000000);

    fprintf(stderr, "%s  %-5s  [tid:%lu] %s:%d  %s\n",
            ts_buf,
            level_name(ev->level),
            ev->tid,
            basename_of(ev->file),
            ev->line,
            ev->msg);
}

/* ── core implementation (called by hs_log macro) ────────────────────── */
void hs_log_impl(hs_log_level_t level,
                 const char *file, int line,
                 const char *fmt, ...)
{
    /* Fast-path level gate — no work at all for filtered messages */
    hs_log_level_t min = atomic_load_explicit(&g_min_level,
                                              memory_order_relaxed);
    if (level < min) return;

    /* Format the message */
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Build the event struct */
    hs_log_event_t ev = {
        .level = level,
        .file  = file,
        .line  = line,
        .msg   = buf,
        .tid   = hs_gettid(),
    };
    clock_gettime(CLOCK_REALTIME, &ev.ts);

    /* Dispatch */
    hs_log_cb cb = atomic_load_explicit(&g_log_cb, memory_order_acquire);
    if (cb) {
        cb(&ev, atomic_load_explicit(&g_log_ud, memory_order_acquire));
    } else {
        default_stderr(&ev);
    }
}
