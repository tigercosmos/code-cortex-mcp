/*
 * result_compact.h — compact a finished CBMFileResult into an exact-size
 * arena, and the per-worker working arena that makes the hand-back free.
 */
#ifndef CBM_RESULT_COMPACT_H
#define CBM_RESULT_COMPACT_H

#include "arena.h"
#include "cbm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copy everything reachable from `result` into ONE arena block of exactly the
 * measured size — strings interned by content within the file — and hand the
 * working arena back to this thread's pool. Call it at the END of a file's
 * extraction, BEFORE the result reaches any shared structure: compaction
 * relocates every string and every record array, so a pointer captured
 * earlier would dangle.
 *
 * Not touched: result->cached_tree (tree-sitter owns that memory, the arena
 * never did) and every scalar field. A result whose cpp_operator_tracker is
 * still live is left alone — the tracker holds source ranges for a walk that
 * has not finished.
 *
 * Best effort and atomic: any allocation failure leaves `result` exactly as
 * it was, with its working arena intact. Never throws. */
void cbm_result_compact(CBMFileResult *result);

/* Per-worker working arena. take() hands out this thread's parked arena
 * rewound to its first block (pages already mapped) or initializes a fresh
 * one; give() parks an arena instead of freeing it, unless one is already
 * parked or this one grew past CBM_WORK_ARENA_KEEP_BYTES; release() drops the
 * parked arena and must be called before the thread ends. */
void cbm_work_arena_take(CBMArena *into);
void cbm_work_arena_give(CBMArena *from);
void cbm_work_arena_release(void);

#ifdef __cplusplus
}
#endif

#endif /* CBM_RESULT_COMPACT_H */
