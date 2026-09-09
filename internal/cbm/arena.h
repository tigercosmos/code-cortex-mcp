#ifndef CBM_ARENA_H
#define CBM_ARENA_H

#include <stddef.h>
#include <limits.h>

// CBMArena is a simple bump allocator that allocates from fixed-size blocks.
// All memory is freed at once via cbm_arena_destroy(). Individual frees are not
// supported — this is by design for per-file extraction where all data has the
// same lifetime.
/* Every growth at least doubles a block of at least 64 bytes. One slot per
 * size_t bit covers all representable capacities without a dynamic table. */
#define CBM_ARENA_MAX_BLOCKS ((int)(sizeof(size_t) * CHAR_BIT))
#define CBM_ARENA_DEFAULT_BLOCK_SIZE (64 * 1024) // 64KB initial

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *blocks[CBM_ARENA_MAX_BLOCKS];
    size_t block_sizes[CBM_ARENA_MAX_BLOCKS]; // per-block sizes (for stats)
    int nblocks;
    size_t block_size;
    size_t used;        // bytes used in current block
    size_t total_alloc; // cumulative bytes allocated (for stats)
    struct CBMArenaResizable *resizable; // separately owned growable buffers
    size_t resizable_bytes;
} CBMArena;

// Initialize an arena with the default block size.
void cbm_arena_init(CBMArena *a);
void cbm_arena_init_sized(CBMArena *a, size_t block_size);

// Allocate n bytes from the arena. Returns NULL on OOM or block exhaustion.
// All returned pointers are 8-byte aligned.
void *cbm_arena_alloc(CBMArena *a, size_t n);

// Allocate only if total owned block/buffer capacity stays within max_capacity.
// The limit concerns owned storage, not allocator overhead or process RSS.
void *cbm_arena_alloc_bounded(CBMArena *a, size_t n, size_t max_capacity);

// Allocate/grow arena-owned storage. Growth can invalidate pointers into the
// buffer. Only pass NULL or a buffer from this API and arena. No shrinking;
// failure preserves the old allocation. Reset/destroy frees all buffers.
void *cbm_arena_grow_buffer(CBMArena *a, void *buffer, size_t n);

// Whether p points inside a live bump block or resizable buffer.
int cbm_arena_contains(const CBMArena *a, const void *p);

// Duplicate a string into arena memory. Returns arena-owned copy.
char *cbm_arena_strdup(CBMArena *a, const char *s);

// Duplicate a string of known length into arena memory. NUL-terminates.
char *cbm_arena_strndup(CBMArena *a, const char *s, size_t len);

// sprintf into arena memory. Returns arena-owned string.
char *cbm_arena_sprintf(CBMArena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Free all blocks. Arena is invalid after this call.
void cbm_arena_destroy(CBMArena *a);

#ifdef __cplusplus
}
#endif

#endif // CBM_ARENA_H
