/**
 * hs_lua_dir.c  –  Lua directory monitoring and hot-reload
 *
 * Platform support:
 *   Linux: inotify (IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO)
 *   macOS: kqueue  (EVFILT_VNODE on dir fd with NOTE_WRITE)
 *
 * Design:
 *   - A single global atomic counter tracks the "generation" of the dir.
 *   - When any Lua file changes, the watcher increments the generation.
 *   - Each worker thread caches the last generation it loaded.
 *     On each request it compares; if different, it reloads.
 *   - hs_lua_dir_needs_reload() uses thread-local storage to store
 *     the per-thread generation.
 */
#define _GNU_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "hs_alloc.h"
#include "hs_lua_dir.h"
#include "hs_reactor.h"
#include "hs_log.h"

/* ── global reload state ─────────────────────────────────────────────────── */
_Atomic uint64_t g_lua_dir_generation = 0;
static _Thread_local uint64_t tl_last_generation = UINT64_MAX;

/* ── main script path (constructed once) ────────────────────────────────── */
static char g_main_script[4096] = {0};

const char *hs_lua_dir_main_script(const char *lua_dir)
{
    if (!lua_dir) return NULL;
    if (g_main_script[0] == '\0') {
        snprintf(g_main_script, sizeof(g_main_script),
                 "%s/handler.lua", lua_dir);
    }
    return g_main_script;
}

int hs_lua_dir_needs_reload(const char *lua_dir)
{
    (void)lua_dir;
    uint64_t gen = atomic_load_explicit(&g_lua_dir_generation,
                                        memory_order_acquire);
    if (gen != tl_last_generation) {
        tl_last_generation = gen;
        return 1;
    }
    return 0;
}

/* ── signal reload (called from watcher callback) ────────────────────────── */
static void signal_reload(void)
{
    atomic_fetch_add_explicit(&g_lua_dir_generation, 1, memory_order_release);
    hs_log(HS_LOG_INFO, "[lua_dir] change detected, reload generation=%llu",
           (unsigned long long)
           atomic_load_explicit(&g_lua_dir_generation, memory_order_relaxed));
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Platform-specific watchers
 * ══════════════════════════════════════════════════════════════════════════ */

#ifdef __linux__
/* ─── Linux: inotify ────────────────────────────────────────────────────── */
#include <sys/inotify.h>
#include "ae.h"

#define INOTIFY_BUF_LEN (1024 * (sizeof(struct inotify_event) + NAME_MAX + 1))

struct hs_lua_dir_watcher {
    int ifd;   /* inotify fd */
    int wd;    /* watch descriptor */
};

static hs_lua_dir_watcher_t g_watcher;

static void inotify_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)el; (void)clientData; (void)mask;
    char buf[INOTIFY_BUF_LEN] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;
    int triggered = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        char *p = buf;
        while (p < buf + n) {
            struct inotify_event *ev = (struct inotify_event *)p;
            /* Only care about .lua files */
            if (ev->len > 0 && ev->name[0] != '\0') {
                size_t nlen = strlen(ev->name);
                if (nlen >= 4 &&
                    strcmp(ev->name + nlen - 4, ".lua") == 0) {
                    triggered = 1;
                }
            } else {
                /* Directory-level event (no name) – signal anyway */
                triggered = 1;
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }

    if (triggered) signal_reload();
}

int hs_lua_dir_watch_start(struct hs_reactor *r, const char *lua_dir)
{
    hs_lua_dir_main_script(lua_dir);   /* prime g_main_script */

    g_watcher.ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_watcher.ifd < 0) {
        hs_log(HS_LOG_WARN, "[lua_dir] inotify_init1 failed: %s", strerror(errno));
        return -1;
    }

    g_watcher.wd = inotify_add_watch(g_watcher.ifd, lua_dir,
                                      IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO);
    if (g_watcher.wd < 0) {
        hs_log(HS_LOG_WARN, "[lua_dir] inotify_add_watch(%s) failed: %s",
               lua_dir, strerror(errno));
        close(g_watcher.ifd);
        g_watcher.ifd = -1;
        return -1;
    }

    if (aeCreateFileEvent(r->ae, g_watcher.ifd, AE_READABLE,
                          inotify_cb, NULL) == AE_ERR) {
        hs_log(HS_LOG_WARN, "[lua_dir] aeCreateFileEvent failed");
        inotify_rm_watch(g_watcher.ifd, g_watcher.wd);
        close(g_watcher.ifd);
        return -1;
    }

    hs_log(HS_LOG_INFO, "[lua_dir] watching %s (inotify fd=%d)", lua_dir, g_watcher.ifd);
    return 0;
}

#else
/* ─── macOS: kqueue ─────────────────────────────────────────────────────── */
#include <sys/event.h>
#include "ae.h"

struct hs_lua_dir_watcher {
    int kq;     /* kqueue fd */
    int dirfd;  /* open directory fd */
};

static hs_lua_dir_watcher_t g_watcher;

static void kqueue_cb(aeEventLoop *el, int fd, void *clientData, int mask)
{
    (void)el; (void)fd; (void)clientData; (void)mask;
    struct kevent ev;
    struct timespec ts = {0, 0};

    /* Drain all pending events */
    while (kevent(g_watcher.kq, NULL, 0, &ev, 1, &ts) > 0)
        signal_reload();
}

int hs_lua_dir_watch_start(struct hs_reactor *r, const char *lua_dir)
{
    hs_lua_dir_main_script(lua_dir);

    g_watcher.kq = kqueue();
    if (g_watcher.kq < 0) {
        hs_log(HS_LOG_WARN, "[lua_dir] kqueue() failed: %s", strerror(errno));
        return -1;
    }

    g_watcher.dirfd = open(lua_dir, O_RDONLY | O_EVTONLY);
    if (g_watcher.dirfd < 0) {
        hs_log(HS_LOG_WARN, "[lua_dir] open(%s) failed: %s", lua_dir, strerror(errno));
        close(g_watcher.kq);
        return -1;
    }

    struct kevent change;
    EV_SET(&change, g_watcher.dirfd, EVFILT_VNODE,
           EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME | NOTE_REVOKE,
           0, NULL);

    if (kevent(g_watcher.kq, &change, 1, NULL, 0, NULL) < 0) {
        hs_log(HS_LOG_WARN, "[lua_dir] kevent registration failed: %s", strerror(errno));
        close(g_watcher.dirfd);
        close(g_watcher.kq);
        return -1;
    }

    /* Set kqueue fd non-blocking */
    {
        int f = fcntl(g_watcher.kq, F_GETFL, 0);
        fcntl(g_watcher.kq, F_SETFL, f | O_NONBLOCK);
    }

    if (aeCreateFileEvent(r->ae, g_watcher.kq, AE_READABLE,
                          kqueue_cb, NULL) == AE_ERR) {
        hs_log(HS_LOG_WARN, "[lua_dir] aeCreateFileEvent failed");
        close(g_watcher.dirfd);
        close(g_watcher.kq);
        return -1;
    }

    hs_log(HS_LOG_INFO, "[lua_dir] watching %s (kqueue fd=%d)", lua_dir, g_watcher.kq);
    return 0;
}

#endif /* __linux__ */
