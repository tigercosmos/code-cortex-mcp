#ifndef CBM_EXTRACT_UNIFIED_H
#define CBM_EXTRACT_UNIFIED_H

#include "cbm.h"
#include "lang_specs.h"

// Scope kinds for the walk state stack.
#define SCOPE_FUNC 1
#define SCOPE_CLASS 2
#define SCOPE_CALL 3
#define SCOPE_IMPORT 4
#define SCOPE_LOOP 5
#define SCOPE_BRANCH 6
#define SCOPE_PARAMS 7 // parameter-binding frame for a Python lambda (no other scope opens there)

#define MAX_SCOPES 64
#define INLINE_PY_PARAM_SLOTS 64
#define INLINE_PY_PARAM_STACK 64

// WalkState tracks scope context during the unified cursor walk.
// Replaces parent-chain walks for enclosing_func_qn, inside_call, etc.

#ifdef __cplusplus
extern "C" {
#endif

/* One name bound as a function/lambda parameter by a scope currently OPEN on
 * the walk stack. A count, not a flag: `def outer(run): def inner(run):` binds
 * the same name twice and the inner pop must not unbind the outer. */
/* A parameter name as a span of the file's source bytes. The walk never copies
 * names: the source outlives the walk, and a copy per parameter would land in
 * the per-file result arena for the whole index. */
typedef struct {
    const char *ptr;
    uint32_t len;
} CBMParamName;

typedef struct {
    CBMParamName name;
    uint32_t hash; // 0 marks an empty slot
    int count;
} CBMParamSlot;

typedef struct {
    const char *enclosing_func_qn;  // current function QN (module_qn at top level)
    const char *enclosing_class_qn; // current class QN (NULL outside class)
    bool inside_call;               // within a call_node_types subtree
    bool inside_import;             // within an import_node_types subtree
    int loop_depth;                 // count of enclosing loop scopes (for bottleneck metrics)
    int branch_depth;               // count of enclosing branch scopes

    struct {
        const char *qn;
        uint32_t depth;
        uint8_t kind;
        int prev_py_param_stack_count; // py_param_stack height on push; pop unwinds to it
    } scopes[MAX_SCOPES];
    int scope_top;

    /* Python bare-call shadowing (upstream #1912). A name bound as a parameter
     * by ANY enclosing function or lambda shadows every project function, so a
     * bare `run()` under `def outer(run)` cannot honestly resolve by short name.
     *
     * Maintained as a live name->count map pushed and popped BY THE WALK, not
     * recomputed per call. Ascending the tree (ts_node_parent or a cursor) is
     * O(depth) per call, and since every level of f(f(f(...))) is itself a bare
     * call that is quadratic across the file. Lookup here is O(1), so no hop cap
     * (and no fail-open past one) is needed. */
    CBMParamSlot *py_param_slots;
    CBMParamSlot inline_py_param_slots[INLINE_PY_PARAM_SLOTS];
    int py_param_slot_capacity;
    int py_param_slot_used;
    CBMParamName *py_param_stack;
    CBMParamName inline_py_param_stack[INLINE_PY_PARAM_STACK];
    int py_param_stack_capacity;
    int py_param_stack_count;
    /* Allocation failure: stop tracking and answer "not bound" forever after,
     * which can only cost a suppression, never a true edge. */
    bool py_param_tracking_failed;
} WalkState;

/* Is the `len`-byte name at `name` (not NUL-terminated) bound as a parameter by
 * a Python function or lambda scope currently open on the walk stack? O(1).
 * Answers false on any failure, so a caller can only ever lose a suppression,
 * never a true edge. */
bool cbm_walk_python_param_is_bound(const WalkState *state, const char *name, uint32_t len);

// Per-node handler prototypes. Each is called once per node during the
// unified cursor walk, replacing the old recursive walk_* functions.
void handle_calls(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state);
void handle_usages(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state);
bool cbm_usage_dedup_begin(CBMExtractCtx *ctx);
void cbm_usage_dedup_end(CBMExtractCtx *ctx, bool owned);
void handle_throws(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state);
void handle_readwrites(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state);
void handle_type_refs(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state);
void handle_env_accesses(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec,
                         WalkState *state);
void handle_type_assigns(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec,
                         WalkState *state);

// Single-pass extraction using TSTreeCursor. Visits every node once,
// dispatching to all handlers per node. Replaces the 7 separate walk_*
// functions for calls/usages/throws/readwrites/type_refs/env_accesses/type_assigns.
// Definitions and imports stay as separate passes (different recursion patterns).
void cbm_extract_unified(CBMExtractCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif // CBM_EXTRACT_UNIFIED_H
