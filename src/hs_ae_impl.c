/**
 * hs_ae_impl.c
 *
 * fsae (https://github.com/andyongh/fsae) is an STB-style single-header
 * library.  Exactly ONE translation unit must define AE_IMPLEMENTATION
 * before including ae.h; all other TUs include ae.h without the macro.
 *
 * We redirect fsae's internal allocator through hs_alloc.h so the same
 * allocator choice (system malloc or jemalloc) is used throughout.
 */
#define _GNU_SOURCE

/* ── redirect fsae's allocator through hs_alloc shim ───────────────────── */
#include "hs_alloc.h"
#define zmalloc(sz)        hs_malloc(sz)
#define zrealloc(ptr, sz)  hs_realloc(ptr, sz)
#define zfree(ptr)         hs_free(ptr)

/* ── instantiate the implementation ────────────────────────────────────── */
#define AE_IMPLEMENTATION
#include "ae.h"
