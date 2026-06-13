/**
 * hs_buf.h  –  growable byte buffer for the WRITE path only.
 *
 * The receive path uses hs_ring_t (zero-malloc).
 * This buffer is used for serialising HTTP responses into conn->wbuf.
 * Backed by jemalloc.
 */
#pragma once
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <jemalloc/jemalloc.h>

typedef struct { char *data; size_t len; size_t cap; } hs_buf_t;

static inline void hs_buf_init(hs_buf_t *b, size_t cap)
{
    cap = cap ? cap : 64;
    b->data = (char *)je_malloc(cap);
    b->len  = 0;
    b->cap  = cap;
}
static inline void hs_buf_reset(hs_buf_t *b){ b->len = 0; }
static inline void hs_buf_free (hs_buf_t *b){ je_free(b->data); b->data=NULL; b->len=b->cap=0; }

static inline int hs_buf_grow(hs_buf_t *b, size_t need)
{
    if (b->len + need <= b->cap) return 0;
    size_t nc = b->cap; while (nc < b->len + need) nc *= 2;
    char *p = (char *)je_realloc(b->data, nc);
    if (!p) return -1;
    b->data = p; b->cap = nc; return 0;
}
static inline int hs_buf_append(hs_buf_t *b, const void *s, size_t n)
{
    if (hs_buf_grow(b, n)) return -1;
    memcpy(b->data + b->len, s, n); b->len += n; return 0;
}
static inline int hs_buf_append_str(hs_buf_t *b, const char *s)
{ return hs_buf_append(b, s, strlen(s)); }

static inline int hs_buf_appendf(hs_buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n < 0 || hs_buf_grow(b, (size_t)n + 1)) return -1;
    va_start(ap, fmt); vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap); va_end(ap);
    b->len += (size_t)n; return 0;
}
static inline void hs_buf_consume(hs_buf_t *b, size_t n)
{
    if (n >= b->len){ b->len = 0; return; }
    memmove(b->data, b->data + n, b->len - n); b->len -= n;
}
