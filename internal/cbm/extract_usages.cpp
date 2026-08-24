#include "cbm.h"
#include "helpers.h"
#include "lang_specs.h"
#include "extract_unified.h"
#include "tree_sitter/api.h" // TSNode, ts_node_*
#include "foundation/constants.h"
#include "extract_node_stack.h"

enum { MAX_PARENT_DEPTH = 10 };
#include <stdint.h> // uint32_t
#include <string.h>
#include <ctype.h>

// Forward declaration
static void walk_usages(CBMExtractCtx *ctx, TSNode root, const CBMLangSpec *spec);

// Is this an identifier-like node that represents a reference?
static bool is_reference_node(TSNode node, CBMLanguage lang) {
    const char *kind = ts_node_type(node);

    // Common identifier types across languages
    if (strcmp(kind, "identifier") == 0 || strcmp(kind, "simple_identifier") == 0 ||
        strcmp(kind, "type_identifier") == 0) {
        return true;
    }

    // Language-specific reference types
    switch (lang) {
    case CBM_LANG_GO:
        return strcmp(kind, "field_identifier") == 0 || strcmp(kind, "package_identifier") == 0;
    case CBM_LANG_PYTHON:
        return strcmp(kind, "attribute") == 0;
    case CBM_LANG_RUST:
        return strcmp(kind, "field_identifier") == 0 || strcmp(kind, "scoped_identifier") == 0;
    case CBM_LANG_HASKELL:
        return strcmp(kind, "variable") == 0 || strcmp(kind, "constructor") == 0;
    case CBM_LANG_OCAML:
        return strcmp(kind, "value_path") == 0 || strcmp(kind, "constructor_path") == 0;
    case CBM_LANG_ERLANG:
        return strcmp(kind, "atom") == 0 || strcmp(kind, "var") == 0;
    default:
        return false;
    }
}

// Check if a reference node is a definition name (the "name" field of its parent).
static bool is_definition_name(TSNode node) {
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent)) {
        return false;
    }
    TSNode name_field = ts_node_child_by_field_name(parent, TS_FIELD("name"));
    return !ts_node_is_null(name_field) &&
           ts_node_start_byte(name_field) == ts_node_start_byte(node) &&
           ts_node_end_byte(name_field) == ts_node_end_byte(node);
}

// Try to emit a usage for a reference node. Returns early if the node should be skipped.
static void try_emit_usage(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec,
                           bool inside_call, bool inside_import) {
    if (!is_reference_node(node, ctx->language)) {
        return;
    }
    if (inside_call || inside_import) {
        return;
    }
    if (is_definition_name(node)) {
        return;
    }
    char *name = cbm_node_text(ctx->arena, node, ctx->source);
    if (name && name[0] && !cbm_is_keyword(name, ctx->language)) {
        CBMUsage usage;
        usage.ref_name = name;
        usage.enclosing_func_qn = cbm_enclosing_func_qn_cached(ctx, node);
        cbm_usages_push(&ctx->result->usages, ctx->arena, usage);
    }
}

// Iterative usage walker — explicit stack, pre-order, children in source order.
//
// The call/import ancestry that gates usage emission is carried ON the walk as
// the tree depth of the NEAREST enclosing call / import node, instead of a
// per-node ancestor re-walk. ts_node_parent RE-DESCENDS from the root scanning
// siblings, so the ancestor-climbing predicates this replaces cost
// O(depth x sibling-position) for EVERY reference node and went quadratic on
// wide nodes — upstream measured 92% of extract time in those walks on
// dotnet/runtime's JIT torture tests (490 s for one 147 KB file, 6.8 s after).
// Both gates are now O(1).
//
// Semantics are preserved exactly, INCLUDING this tree's MAX_PARENT_DEPTH bound,
// which upstream does not have (its predicates climbed to the root, so its own
// port could use plain enter/exit counters). The predicates examined STRICT
// ancestors at distance 1..MAX_PARENT_DEPTH, so the nearest enclosing
// call/import counts only while it is within that distance; nearest is
// sufficient, because if the nearest is out of range every farther one is too. A
// node is gated BEFORE its own kind updates the trackers, so a call node is not
// itself "inside a call".
//
// The frame stack is sized by tree DEPTH rather than the old node stack's
// breadth, so it also allocates far less on wide trees.
/* Is the nearest enclosing call/import node close enough to gate this node?
 * -1 means "no such ancestor".
 *
 * Kept out of line so the two call sites share one suppression. cppcheck reads
 * the range test as constant because its value flow cannot follow the frame
 * stack growing inside walk_usages' loop: it only ever sees the root frame
 * (top == 1, hence depth == 0, hence nearest_depth == 0), and concludes
 * 0 - 0 <= 10 is a tautology. At every deeper frame the comparison is exactly
 * the MAX_PARENT_DEPTH bound the old ancestor climb enforced, and dropping it
 * would change which usages are emitted. */
static bool usage_ancestor_within_reach(int nearest_depth, int node_depth) {
    // cppcheck-suppress knownConditionTrueFalse
    return nearest_depth >= 0 && node_depth - nearest_depth <= MAX_PARENT_DEPTH;
}

static void walk_usages(CBMExtractCtx *ctx, TSNode root, const CBMLangSpec *spec) {
    typedef struct {
        TSNode node;
        uint32_t next_child;
        int saved_call;   /* nearest-call depth to restore when this frame exits */
        int saved_import; /* likewise for imports */
    } UsageFrame;
    int cap = 256;
    UsageFrame *frames = (UsageFrame *)cbm_arena_alloc(ctx->arena, (size_t)cap * sizeof(*frames));
    if (!frames) {
        return;
    }
    int nearest_call = -1; /* tree depth of the nearest enclosing call node */
    int nearest_import = -1;
    int top = 0;
    frames[top++] = (UsageFrame){root, 0, -1, -1};
    bool entering = true;

    while (top > 0) {
        UsageFrame *f = &frames[top - 1];
        const int depth = top - 1;
        if (entering) {
            bool inside_call = usage_ancestor_within_reach(nearest_call, depth);
            bool inside_import = usage_ancestor_within_reach(nearest_import, depth);
            try_emit_usage(ctx, f->node, spec, inside_call, inside_import);
            f->saved_call = nearest_call;
            f->saved_import = nearest_import;
            if (cbm_kind_in_set(f->node, spec->call_node_types)) {
                nearest_call = depth;
            }
            if (cbm_kind_in_set(f->node, spec->import_node_types)) {
                nearest_import = depth;
            }
        }
        uint32_t count = ts_node_child_count(f->node);
        if (f->next_child < count) {
            TSNode child = ts_node_child(f->node, f->next_child);
            f->next_child++;
            if (top == cap) {
                int new_cap = cap * 2;
                UsageFrame *grown =
                    (UsageFrame *)cbm_arena_alloc(ctx->arena, (size_t)new_cap * sizeof(*grown));
                if (!grown) {
                    return;
                }
                memcpy(grown, frames, (size_t)cap * sizeof(*frames));
                frames = grown;
                cap = new_cap;
            }
            /* `f` is deliberately not touched after a growth — it may dangle. */
            frames[top++] = (UsageFrame){child, 0, -1, -1};
            entering = true;
        } else {
            nearest_call = f->saved_call;
            nearest_import = f->saved_import;
            top--;
            entering = false;
        }
    }
}

void cbm_extract_usages(CBMExtractCtx *ctx) {
    const CBMLangSpec *spec = cbm_lang_spec(ctx->language);
    if (!spec) {
        return;
    }

    walk_usages(ctx, ctx->root, spec);
}

// --- Unified handler: called once per node by the cursor walk ---
// Uses WalkState flags instead of parent-chain walks for O(1) context checks.

void handle_usages(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state) {
    (void)spec;
    if (!is_reference_node(node, ctx->language)) {
        return;
    }

    // Skip if inside a call (already counted as CALLS edge) — O(1) via state
    if (state->inside_call) {
        return;
    }
    // Skip if inside an import
    if (state->inside_import) {
        return;
    }

    // Skip if it's a definition name (left side of assignment, function name)
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent)) {
        TSNode name_field = ts_node_child_by_field_name(parent, TS_FIELD("name"));
        if (!ts_node_is_null(name_field) &&
            ts_node_start_byte(name_field) == ts_node_start_byte(node) &&
            ts_node_end_byte(name_field) == ts_node_end_byte(node)) {
            return;
        }
    }

    char *name = cbm_node_text(ctx->arena, node, ctx->source);
    if (name && name[0] && !cbm_is_keyword(name, ctx->language)) {
        CBMUsage usage;
        usage.ref_name = name;
        usage.enclosing_func_qn = state->enclosing_func_qn;
        cbm_usages_push(&ctx->result->usages, ctx->arena, usage);
    }
}
