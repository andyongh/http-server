/**
 * hs_ae_impl.c
 *
 * fsae (https://github.com/andyongh/fsae) is an STB-style single-header
 * library.  Exactly ONE translation unit must define AE_IMPLEMENTATION
 * before including ae.h; all other TUs include ae.h without the macro.
 *
 * We also redirect fsae's internal allocator to jemalloc so every allocation
 * inside the event loop goes through our preferred allocator.
 */
#define _GNU_SOURCE

/* ── redirect fsae's allocator to jemalloc ─────────────────────────────── */
#include <jemalloc/jemalloc.h>
#define zmalloc(sz)        je_malloc(sz)
#define zrealloc(ptr, sz)  je_realloc(ptr, sz)
#define zfree(ptr)         je_free(ptr)

/* ── instantiate the implementation ────────────────────────────────────── */
#define AE_IMPLEMENTATION
#include "ae.h"
