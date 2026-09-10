/*
 * arena.c — Bump allocator implementation.
 *
 * Restructured from internal/cbm/arena.c with additions:
 *   - cbm_arena_init_sized() for custom block sizes
 *   - cbm_arena_calloc() for zero-initialized allocations
 *   - cbm_arena_reset() for reuse without full destroy
 *   - cbm_arena_total() for allocation tracking
 *
 * Arena blocks use malloc/free (= mimalloc in production builds).
 */
#include "arena.h"
#include "foundation/constants.h"

enum { ARENA_ALIGN = 7, ARENA_GROW_OK = 1 };
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

struct CBMArenaResizable {
    void *data;
    size_t size;
    CBMArenaResizable *next;
};

int cbm_arena_contains(const CBMArena *a, const void *p) {
    if (!a || !p) {
        return 0;
    }
    uintptr_t address = (uintptr_t)p;
    for (int i = 0; i < a->nblocks; i++) {
        uintptr_t base = (uintptr_t)a->blocks[i];
        if (address >= base && address - base < a->block_sizes[i]) {
            return 1;
        }
    }
    for (const CBMArenaResizable *entry = a->resizable; entry; entry = entry->next) {
        uintptr_t base = (uintptr_t)entry->data;
        if (address >= base && address - base < entry->size) {
            return 1;
        }
    }
    return 0;
}

void *cbm_arena_grow_buffer(CBMArena *a, void *buffer, size_t n) {
    if (!a || a->nblocks == 0 || n == 0) {
        return NULL;
    }
    CBMArenaResizable *entry = a->resizable;
    if (buffer) {
        while (entry && entry->data != buffer) {
            entry = entry->next;
        }
        if (!entry || n < entry->size || n - entry->size > SIZE_MAX - a->total_alloc) {
            return NULL;
        }
        void *data = realloc(buffer, n);
        if (!data) {
            return NULL;
        }
        a->total_alloc += n - entry->size;
        a->resizable_bytes += n - entry->size;
        entry->data = data;
        entry->size = n;
        return data;
    }
    if (n > SIZE_MAX - a->total_alloc) {
        return NULL;
    }
    entry = (CBMArenaResizable *)malloc(sizeof(*entry));
    if (!entry) {
        return NULL;
    }
    entry->data = malloc(n);
    if (!entry->data) {
        free(entry);
        return NULL;
    }
    entry->size = n;
    entry->next = a->resizable;
    a->resizable = entry;
    a->total_alloc += n;
    a->resizable_bytes += n;
    return entry->data;
}

static void arena_free_buffers(CBMArena *a) {
    while (a->resizable) {
        CBMArenaResizable *entry = a->resizable;
        a->resizable = entry->next;
        free(entry->data);
        free(entry);
    }
    a->resizable_bytes = 0;
}

void cbm_arena_init(CBMArena *a) {
    cbm_arena_init_sized(a, CBM_ARENA_DEFAULT_BLOCK_SIZE);
}

void cbm_arena_init_sized(CBMArena *a, size_t block_size) {
    memset(a, 0, sizeof(*a));
    if (block_size < CBM_SZ_64) {
        block_size = CBM_SZ_64; /* minimum sanity */
    }
    a->block_size = block_size;
    a->blocks[0] = (char *)malloc(block_size);
    if (a->blocks[0]) {
        a->block_sizes[0] = block_size;
        a->nblocks = SKIP_ONE;
    }
}

static int arena_grow(CBMArena *a, size_t min_size) {
    if (a->nblocks >= CBM_ARENA_MAX_BLOCKS || a->block_size > SIZE_MAX / PAIR_LEN) {
        return 0;
    }
    size_t new_size = a->block_size * PAIR_LEN;
    if (new_size < min_size) {
        new_size = min_size;
    }
    char *block = (char *)malloc(new_size);
    if (!block) {
        return 0;
    }
    a->blocks[a->nblocks] = block;
    a->block_sizes[a->nblocks] = new_size;
    a->nblocks++;
    a->block_size = new_size;
    a->used = 0;
    return ARENA_GROW_OK;
}

void *cbm_arena_alloc(CBMArena *a, size_t n) {
    if (!a || n == 0 || n > SIZE_MAX - ARENA_ALIGN) {
        return NULL;
    }
    /* 8-byte alignment */
    n = (n + ARENA_ALIGN) & ~(size_t)ARENA_ALIGN;
    if (a->nblocks == 0 || n > SIZE_MAX - a->total_alloc || a->used > a->block_size) {
        return NULL;
    }
    if (n > a->block_size - a->used) {
        if (!arena_grow(a, n)) {
            return NULL;
        }
    }
    char *ptr = a->blocks[a->nblocks - SKIP_ONE] + a->used;
    a->used += n;
    a->total_alloc += n;
    return ptr;
}

void *cbm_arena_alloc_bounded(CBMArena *a, size_t n, size_t max_capacity) {
    if (!a || n == 0 || n > SIZE_MAX - ARENA_ALIGN || a->nblocks == 0)
        return NULL;
    size_t aligned = (n + ARENA_ALIGN) & ~(size_t)ARENA_ALIGN;
    size_t capacity = a->resizable_bytes;
    for (int i = 0; i < a->nblocks; ++i) {
        if (a->block_sizes[i] > SIZE_MAX - capacity)
            return NULL;
        capacity += a->block_sizes[i];
    }
    if (capacity > max_capacity || a->used > a->block_size)
        return NULL;
    if (aligned > a->block_size - a->used) {
        if (a->block_size > SIZE_MAX / PAIR_LEN)
            return NULL;
        size_t growth = a->block_size * PAIR_LEN;
        if (growth < aligned)
            growth = aligned;
        if (growth > max_capacity - capacity)
            return NULL;
    }
    return cbm_arena_alloc(a, n);
}

void *cbm_arena_calloc(CBMArena *a, size_t n) {
    void *p = (void *)cbm_arena_alloc(a, n);
    if (p) {
        memset(p, 0, n);
    }
    return p;
}

char *cbm_arena_strdup(CBMArena *a, const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *dst = (char *)cbm_arena_alloc(a, len + SKIP_ONE);
    if (dst) {
        memcpy(dst, s, len + SKIP_ONE);
    }
    return dst;
}

char *cbm_arena_strndup(CBMArena *a, const char *s, size_t len) {
    if (!s || len == SIZE_MAX) {
        return NULL;
    }
    char *dst = (char *)cbm_arena_alloc(a, len + SKIP_ONE);
    if (dst) {
        memcpy(dst, s, len);
        dst[len] = '\0';
    }
    return dst;
}

char *cbm_arena_sprintf(CBMArena *a, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        return NULL;
    }

    char *dst = (char *)cbm_arena_alloc(a, (size_t)needed + SKIP_ONE);
    if (!dst) {
        return NULL;
    }

    va_start(args, fmt);
    vsnprintf(dst, (size_t)needed + SKIP_ONE, fmt, args);
    va_end(args);
    return dst;
}

void cbm_arena_reset(CBMArena *a) {
    arena_free_buffers(a);
    /* Keep first block, free the rest */
    for (int i = SKIP_ONE; i < a->nblocks; i++) {
        free(a->blocks[i]);
        a->blocks[i] = NULL;
        a->block_sizes[i] = 0;
    }
    if (a->nblocks > SKIP_ONE) {
        a->nblocks = SKIP_ONE;
    }
    a->used = 0;
    a->total_alloc = 0;
    /* Reset block_size to match surviving block — prevents overflow if
     * block_size grew during previous allocations (e.g., CBM_SZ_128 → CBM_SZ_256). */
    if (a->nblocks == SKIP_ONE) {
        a->block_size = a->block_sizes[0];
    }
}

void cbm_arena_destroy(CBMArena *a) {
    arena_free_buffers(a);
    for (int i = 0; i < a->nblocks; i++) {
        free(a->blocks[i]);
    }
    memset(a, 0, sizeof(*a));
}

size_t cbm_arena_total(const CBMArena *a) {
    return a->total_alloc;
}
