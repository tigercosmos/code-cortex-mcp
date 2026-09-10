/*
 * arena.h — Bump allocator with block-based growth.
 *
 * All memory is freed at once via cbm_arena_destroy(). Individual frees are
 * not supported — this is by design for per-file extraction where all data
 * has the same lifetime.
 *
 * Restructured from internal/cbm/arena.h for the pure C rewrite.
 * New additions: cbm_arena_reset() for reuse without realloc.
 */
#ifndef CBM_ARENA_H
#define CBM_ARENA_H

#include <stddef.h>
#include <limits.h>
#include <stdarg.h>

/* Every growth at least doubles a block of at least 64 bytes. One slot per
 * size_t bit covers all representable capacities without a dynamic table. */
#define CBM_ARENA_MAX_BLOCKS ((int)(sizeof(size_t) * CHAR_BIT))
#define CBM_ARENA_DEFAULT_BLOCK_SIZE ((size_t)64 * 1024) /* 64KB */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *blocks[CBM_ARENA_MAX_BLOCKS];
    size_t block_sizes[CBM_ARENA_MAX_BLOCKS]; /* per-block sizes (for stats) */
    int nblocks;
    size_t block_size;                   /* current block capacity */
    size_t used;                         /* bytes used in current block */
    size_t total_alloc;                  /* cumulative bytes allocated (for stats) */
    struct CBMArenaResizable *resizable; /* separately owned growable buffers */
    size_t resizable_bytes;
} CBMArena;

/* Initialize arena with default block size. */
void cbm_arena_init(CBMArena *a);

/* Initialize arena with a custom initial block size. */
void cbm_arena_init_sized(CBMArena *a, size_t block_size);

/* Allocate n bytes (8-byte aligned). Returns NULL on OOM. */
void *cbm_arena_alloc(CBMArena *a, size_t n);

// Allocate only if total owned block/buffer capacity stays within max_capacity.
// The limit concerns owned storage, not allocator overhead or process RSS.
void *cbm_arena_alloc_bounded(CBMArena *a, size_t n, size_t max_capacity);

/* Allocate/grow an owned buffer, reclaiming its previous storage. Unlike bump
 * allocations, growth can invalidate pointers into this buffer. Pass NULL on
 * first allocation; subsequent pointers must come from this API and arena.
 * n must be positive and cannot shrink. Failure leaves the old buffer valid.
 * Reset/destroy frees these buffers along with the bump blocks. */
void *cbm_arena_grow_buffer(CBMArena *a, void *buffer, size_t n);

/* Whether p points inside a live bump block or resizable buffer. */
int cbm_arena_contains(const CBMArena *a, const void *p);

/* Allocate n bytes, zero-initialized. */
void *cbm_arena_calloc(CBMArena *a, size_t n);

/* Duplicate a NUL-terminated string. */
char *cbm_arena_strdup(CBMArena *a, const char *s);

/* Duplicate a string of known length, NUL-terminate. */
char *cbm_arena_strndup(CBMArena *a, const char *s, size_t len);

/* sprintf into arena memory. */
char *cbm_arena_sprintf(CBMArena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Reset arena for reuse: keeps first block, frees the rest. */
void cbm_arena_reset(CBMArena *a);

/* Free all blocks. Arena is zeroed after this. */
void cbm_arena_destroy(CBMArena *a);

/* Return total bytes allocated (for diagnostics). */
size_t cbm_arena_total(const CBMArena *a);

#ifdef __cplusplus
}
#endif

#endif /* CBM_ARENA_H */
