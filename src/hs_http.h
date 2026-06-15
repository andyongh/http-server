/**
 * hs_http.h  –  HTTP response object and serialisation
 */
#pragma once
#include <stddef.h>
#include "hs_buf.h"
#include "hs_queue.h"
#include "httpserver.h"

#define HS_RES_MAX_HEADERS 32

struct hs_response {
    int      status;
    struct { char *name; char *value; } headers[HS_RES_MAX_HEADERS];
    int      nheaders;
    hs_buf_t body;
    struct hs_conn *conn;
};

hs_response_t *hs_http_response_new(struct hs_conn *conn);
void           hs_http_response_serialise(hs_response_t *res);
void           hs_http_response_free(hs_response_t *res);
void           hs_http_error(struct hs_conn *conn, int status, const char *msg);
const char    *hs_http_status_str(int code);
