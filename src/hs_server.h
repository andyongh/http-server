/**
 * hs_server.h  –  internal server struct (shared across translation units)
 *
 * Not part of the public API – users only include include/httpserver.h
 */
#pragma once

#include <stdatomic.h>
#include "ae.h"           /* fsae single-header: no AE_IMPLEMENTATION here */
#include "hs_listener.h"
#include "hs_reactor.h"
#include "httpserver.h"

struct hs_pool;

struct hs_server {
    hs_config_t          config;

    hs_listener_t        listeners[HS_MAX_LISTENERS];
    int                  nlisteners;

    hs_reactor_t        *reactor;    /* single reactor (IO thread) */
    struct hs_pool       *pool;      /* optional CPU thread pool   */

    atomic_int            running;
};

typedef struct hs_server hs_server_t;
