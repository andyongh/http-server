/**
 * hs_lua_dir.h  –  Lua directory monitoring and hot-reload
 *
 * Monitors a directory of .lua files.  When any file changes, marks the
 * directory as needing reload.  Worker threads check this flag before
 * each request and reload their lua_State if necessary.
 *
 * The watcher integrates with the reactor's ae event loop:
 *   - Linux: inotify  (IN_MODIFY | IN_CREATE | IN_CLOSE_WRITE)
 *   - macOS: kqueue   (EVFILT_VNODE with NOTE_WRITE | NOTE_ATTRIB)
 *
 * Convention: the main handler script is "handler.lua" in lua_dir.
 * All other .lua files in the directory are available for require().
 */
#pragma once

#include <stdatomic.h>

struct hs_reactor;

/* ── opaque state ────────────────────────────────────────────────────────── */
typedef struct hs_lua_dir_watcher hs_lua_dir_watcher_t;

/**
 * Start monitoring lua_dir.  Registers a file event in the reactor's ae loop.
 * Returns 0 on success, -1 if the watcher cannot be created (non-fatal).
 */
int hs_lua_dir_watch_start(struct hs_reactor *r, const char *lua_dir);

/**
 * Returns 1 if a file in lua_dir changed since the last call to this
 * function on the calling thread (atomically clears the flag).
 * Thread-safe: multiple worker threads may call this concurrently.
 */
int hs_lua_dir_needs_reload(const char *lua_dir);

/**
 * Returns the canonical path to the main handler script in lua_dir.
 * The returned pointer is valid for the lifetime of the process.
 * Returns NULL if lua_dir is NULL.
 */
const char *hs_lua_dir_main_script(const char *lua_dir);
