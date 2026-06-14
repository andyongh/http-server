/**
 * hs_alloc.h  –  memory allocator shim
 *
 * Build with HS_USE_JEMALLOC=1 (default: 0 = system malloc).
 *
 *   make                    → system malloc/free
 *   make HS_USE_JEMALLOC=1  → jemalloc (requires deps/jemalloc built)
 */
#pragma once

#include <stdlib.h>
#include <string.h>

#ifdef HS_USE_JEMALLOC
#  include <jemalloc/jemalloc.h>
#  define hs_malloc(sz)        je_malloc(sz)
#  define hs_calloc(n, sz)     je_calloc(n, sz)
#  define hs_realloc(p, sz)    je_realloc(p, sz)
#  define hs_free(p)           je_free(p)
static inline char *hs_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = (char *)je_malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}
#else
#  define hs_malloc(sz)        malloc(sz)
#  define hs_calloc(n, sz)     calloc(n, sz)
#  define hs_realloc(p, sz)    realloc(p, sz)
#  define hs_free(p)           free(p)
static inline char *hs_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = (char *)malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}
#endif /* HS_USE_JEMALLOC */
