/**
 * hs_alloc.h  –  memory allocator shim
 *
 * pure-C version (system malloc/free directly)
 */
#pragma once

#include <stdlib.h>
#include <string.h>

#define hs_malloc(sz)        malloc(sz)
#define hs_calloc(n, sz)     calloc(n, sz)
#define hs_realloc(p, sz)    realloc(p, sz)
#define hs_free(p)           free(p)

static inline char *hs_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = (char *)malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}
