/**
 * hs_http.c  –  HTTP response building and wire serialisation
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hs_alloc.h"
#include "hs_http.h"
#include "hs_conn.h"
#include "httpserver.h"

const char *hs_http_status_str(int c)
{
    switch (c) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

hs_response_t *hs_http_response_new(struct hs_conn *conn)
{
    hs_response_t *r = (hs_response_t *)hs_calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->status = 200;
    r->conn   = conn;
    hs_buf_init(&r->body, 256);
    return r;
}

void hs_http_response_free(hs_response_t *r)
{
    if (!r) return;
    for (int i = 0; i < r->nheaders; i++) {
        hs_free(r->headers[i].name);
        hs_free(r->headers[i].value);
    }
    hs_buf_free(&r->body);
    hs_free(r);
}

/* ── public builder API (called from worker threads or reactor) ──────────── */
void hs_res_status(hs_response_t *r, int code)   { r->status = code; }

void hs_res_header(hs_response_t *r, const char *n, const char *v)
{
    if (r->nheaders >= HS_RES_MAX_HEADERS) return;
    int i = r->nheaders++;
    r->headers[i].name  = hs_strdup(n);
    r->headers[i].value = hs_strdup(v);
}

void hs_res_body(hs_response_t *r, const char *d, size_t l)
{
    hs_buf_reset(&r->body);
    hs_buf_append(&r->body, d, l);
}

void hs_res_body_str(hs_response_t *r, const char *s)
{
    hs_res_body(r, s, strlen(s));
}

/* hs_res_send() lives in hs_reactor.c (needs reactor->resp_queue + resp_efd) */

/* ── serialise into conn->wbuf (called on IO thread) ────────────────────── */
void hs_http_response_serialise(hs_response_t *res)
{
    hs_conn_t *conn = res->conn;
    hs_buf_t  *out  = &conn->wbuf;
    hs_buf_reset(out);

    hs_buf_appendf(out, "HTTP/1.1 %d %s\r\nServer: httpserver/0.4-lite\r\n",
                   res->status, hs_http_status_str(res->status));
    hs_buf_appendf(out, "Content-Length: %zu\r\n", res->body.len);
    hs_buf_append_str(out,
        conn->req.keep_alive ? "Connection: keep-alive\r\n"
                             : "Connection: close\r\n");

    for (int i = 0; i < res->nheaders; i++) {
        if (strcasecmp(res->headers[i].name, "content-length") == 0) continue;
        hs_buf_appendf(out, "%s: %s\r\n",
                       res->headers[i].name, res->headers[i].value);
    }
    hs_buf_append_str(out, "\r\n");
    if (res->body.len > 0)
        hs_buf_append(out, res->body.data, res->body.len);
}

/* ── inline error (no worker, no response object) ───────────────────────── */
void hs_http_error(struct hs_conn *conn, int status, const char *msg)
{
    hs_buf_t *out = &conn->wbuf;
    hs_buf_reset(out);
    hs_buf_appendf(out,
        "HTTP/1.1 %d %s\r\nServer: httpserver/0.4-lite\r\n"
        "Content-Type: text/plain\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        status, hs_http_status_str(status), strlen(msg), msg);
}
