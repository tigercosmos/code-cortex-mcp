/*
 * lsp_resolve.h — Shared LSP-override resolver for the call-edge pipeline.
 *
 * Both pipeline paths (sequential cbm_pipeline_pass_calls and parallel
 * cbm_parallel_extract → resolve_file_calls) need to look up an
 * LSP-resolved call for a given (caller, callee) pair before falling back
 * to the registry's name-based resolver. Before this header existed, each
 * pipeline carried its own copy of that lookup with divergent confidence
 * floors and slightly different match semantics — most production
 * indexing went through the parallel path with a 0.5 floor while the
 * sequential path used 0.6, so the same project produced different
 * CALLS-edge attributions depending on which pipeline mode kicked in.
 *
 * Centralising the lookup here means both pipelines admit exactly the
 * same set of LSP overrides. Each pipeline still owns its own edge
 * emission (sequential uses emit_classified_edge, parallel uses
 * emit_service_edge) — this header only does the matching.
 *
 * Inline-only: no .c file needed.
 */
#ifndef CBM_PIPELINE_LSP_RESOLVE_H
#define CBM_PIPELINE_LSP_RESOLVE_H

#include "cbm.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/constants.h"
#include "service_patterns.h" /* cbm_service_pattern_match for cbm_pipeline_service_name */

#include <stdio.h>
#include <stdlib.h> /* qsort, malloc: the field-owner index */
#include <string.h>

#include <algorithm>
#include <iterator>

/* Confidence floor below which LSP-resolved calls are ignored and the
 * registry resolver is consulted instead. Locked at 0.6 per the v1
 * Python-LSP integration plan; revisit when telemetry justifies a knob.
 * Applies to every language whose LSP populates result->resolved_calls
 * (Go, C/C++, Python, PHP). */
#define CBM_LSP_CONFIDENCE_FLOOR 0.6f

#ifdef __cplusplus
extern "C" {
#endif

static inline int cbm_pipeline_lsp_key(char *buffer, size_t capacity, const char *caller,
                                       const char *leaf, uint32_t source_byte) {
    const char *suffix = strstr(leaf, "@overload_");
    size_t length = suffix ? (size_t)(suffix - leaf) : strlen(leaf);
    return snprintf(buffer, capacity, "%s|%.*s|%u", caller, (int)length, leaf, source_byte);
}

/* Bare last segment of a (possibly qualified) name, splitting on the LAST
 * member/scope separator. C++ textual callees carry `::` (Class::method,
 * Ns::f) and `->` (p->run), while the LSP records dotted internal QNs
 * (Class.method). Splitting only on '.' (strrchr) leaves `Math::square`
 * and `p->run` intact, so they never match the LSP's `square`/`run` short
 * name and the type-aware strategy is silently dropped to the textual
 * registry. Treat '.', ':' and '>' as terminal separators so the bare
 * method name is recovered on BOTH the QN side (dotted, occasionally `::`
 * for template/alias scopes) and the textual side (`.`/`::`/`->`). Other
 * languages' callee names contain none of `::`/`->`, so this is a no-op
 * for them. */
static inline const char *cbm_lsp_bare_segment(const char *name) {
    if (!name) {
        return name;
    }
    const char *seg = name;
    for (const char *p = name; *p; p++) {
        /* '.' (dotted QN / Java-style member) and ':' (C++ `::`, last colon
         * wins) are member/scope separators. '>' is only a separator when it
         * closes the `->` arrow (preceded by '-'); a bare '>' closes a template
         * argument list ("identity<int>") and must NOT split, else the segment
         * would be the empty string after the trailing '>'. */
        if (*p == '.' || *p == ':' || (*p == '>' && p != name && p[-1] == '-')) {
            seg = p + SKIP_ONE;
        }
    }
    return seg;
}

/* Tail helper: return the start of the final two dot-separated segments
 * ("Class.method") or NULL when the QN is too short. */
static inline const char *cbm_pipeline_qn_class_method_tail(const char *qn) {
    if (!qn) {
        return NULL;
    }
    const char *last = strrchr(qn, '.');
    if (!last || last == qn) {
        return NULL;
    }
    const char *second = last;
    while (second > qn) {
        second--;
        if (*second == '.') {
            if (second == qn) {
                return qn;
            }
            return second + 1;
        }
    }
    return qn;
}

static inline const char *cbm_pipeline_call_callee_leaf(const char *callee_name) {
    return cbm_lsp_bare_segment(callee_name);
}

/* Strategies for which the call site's TEXTUAL callee name is not the resolved
 * callee_qn's tail, and the LSP therefore stashed the textual name in `reason`
 * for the join:
 *   - lsp_func_ptr / lsp_dll_resolve — callee is the pointer name (`fp`);
 *   - lsp_destructor — the only anchor is the deleted operand (`p` vs `T.~T`);
 *   - lsp_dict_dispatch — the dispatch table variable name;
 *   - lsp_method_ref_ctor{,_synth} — Java constructor method refs;
 *   - php_method_dynamic — the dynamic method name;
 *   - lsp_import_alias — `from m import f as g; g()`: textual `g`, QN tail `f`.
 *
 * Both join venues MUST agree on this set. The matcher below consults it as a
 * fallback when the tails differ; the parallel path's O(1) per-file index
 * (resolve_file_calls in pass_parallel.cpp) must therefore ALSO index these
 * entries under their `reason` name, because that index is treated as
 * authoritative — a miss there does NOT fall back to the linear scan. Adding a
 * strategy here without adding the alias key there silently drops the whole
 * strategy on the parallel path (the default above 50 files). */
static inline bool cbm_lsp_strategy_matches_on_reason(const char *strategy) {
    return strategy &&
           (strcmp(strategy, "lsp_func_ptr") == 0 || strcmp(strategy, "lsp_dll_resolve") == 0 ||
            strcmp(strategy, "lsp_method_ref_ctor") == 0 ||
            strcmp(strategy, "lsp_method_ref_ctor_synth") == 0 ||
            strcmp(strategy, "lsp_dict_dispatch") == 0 || strcmp(strategy, "lsp_destructor") == 0 ||
            strcmp(strategy, "php_method_dynamic") == 0 ||
            strcmp(strategy, "lsp_import_alias") == 0);
}

/* Gate for the unique-`Class.method`-tail fallbacks below. Tail-matching by
 * leaf is safe where class-per-file package semantics hold — the JVM
 * languages (Java/Kotlin): the declared `package` is ground truth, a class
 * name is unique within a package, and mixed Gradle/Maven source roots
 * (`src/main/java` + `src/main/kotlin`) legitimately produce path-derived
 * module QNs that disagree with the package-shaped QNs the LSP emits, so
 * the tail is the only reliable join key. In other languages the same-name
 * guarantee does not exist (Python/TS re-export shims, Go internal clones,
 * C++ template instantiations), and a single wrong-module coincidence
 * would fabricate a CALLS edge — so the fallbacks stay off there. */
static inline bool cbm_pipeline_lsp_allow_tail_match(CBMLanguage lang) {
    return lang == CBM_LANG_JAVA || lang == CBM_LANG_KOTLIN;
}

static inline int cbm_pipeline_qn_class_method_tail_eq(const char *qn, const char *tail) {
    const char *qt = cbm_pipeline_qn_class_method_tail(qn);
    return qt && tail && strcmp(qt, tail) == 0;
}

/* Look up the highest-confidence LSP-resolved call entry whose caller QN
 * matches the textual call's enclosing function and whose callee QN
 * short-name matches the textual callee. Returns a pointer into `arr`
 * or NULL if no qualifying entry exists.
 *
 * Match rule:
 *   1. exact caller_qn + callee short-name match wins first;
 *   2. if no exact caller match exists AND allow_tail_match is set
 *      (JVM callers only, see cbm_pipeline_lsp_allow_tail_match), a
 *      unique Class.method tail match between rc->caller_qn and
 *      call->enclosing_func_qn may win;
 *   3. ambiguous tails return NULL so the registry fallback stays in
 *      control.
 *
 * The pointer returned aliases into `arr` and stays valid as long as the
 * underlying CBMFileResult is alive. */
static inline const CBMResolvedCall *cbm_pipeline_find_lsp_resolution(
    const CBMResolvedCallArray *arr, const CBMCall *call, bool allow_tail_match) {
    if (!arr || arr->count == 0 || !call) {
        return NULL;
    }
    if (!call->enclosing_func_qn || !call->callee_name) {
        return NULL;
    }

    const CBMResolvedCall *best_exact = NULL;
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (!rc->caller_qn || !rc->callee_qn) {
            continue;
        }
        if (rc->confidence < CBM_LSP_CONFIDENCE_FLOOR) {
            continue;
        }
        if (strcmp(rc->caller_qn, call->enclosing_func_qn) != 0) {
            continue;
        }
        if (rc->source_byte && call->source_byte && rc->source_byte != call->source_byte) {
            continue;
        }
        const char *short_name = cbm_lsp_bare_segment(rc->callee_qn);
        /* The call's callee_name is receiver-qualified for method/qualified
         * calls ("c.inc", "A.Helper", "Math::square", "p->run"); the LSP
         * records the resolved class-qualified callee_qn ("Class.inc"). Compare
         * the bare last segment on BOTH sides so method-dispatch resolutions
         * join — the LSP already did the receiver->type resolution, and matching
         * the full "c.inc" against "inc" would always miss, silently dropping the
         * type-aware LSP strategy to the weaker textual registry. Free-function
         * calls (bare callee_name) are unaffected. */
        const char *call_short = cbm_lsp_bare_segment(call->callee_name);
        const char *overload_suffix = strstr(short_name, "@overload_");
        bool same_name =
            overload_suffix ? strlen(call_short) == (size_t)(overload_suffix - short_name) &&
                                  strncmp(short_name, call_short, overload_suffix - short_name) == 0
                            : strcmp(short_name, call_short) == 0;
        if (!same_name) {
            /* Indirect/implicit resolution: the textual callee differs from the
             * resolved callee_qn's short name. A function-pointer / DLL call's
             * callee is the pointer name (`fp`); a C++ destructor's only textual
             * anchor is the deleted operand (`p`, vs. the `T.~T` callee QN). In
             * both the LSP stashed the original textual name in `reason`. Match
             * the call site on that name, gated to those strategies so `reason`
             * is never misread as an unresolved-call diagnostic. */
            if (!(rc->reason && cbm_lsp_strategy_matches_on_reason(rc->strategy) &&
                  strcmp(cbm_lsp_bare_segment(rc->reason), call_short) == 0)) {
                continue;
            }
        }
        if (!best_exact || (rc->source_byte && !best_exact->source_byte) ||
            ((rc->source_byte != 0) == (best_exact->source_byte != 0) &&
             rc->confidence > best_exact->confidence)) {
            best_exact = rc;
        }
    }
    if (best_exact) {
        return best_exact;
    }
    if (!allow_tail_match) {
        return NULL;
    }

    const char *call_tail = cbm_pipeline_qn_class_method_tail(call->enclosing_func_qn);
    if (!call_tail) {
        return NULL;
    }

    const CBMResolvedCall *best_tail = NULL;
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (!rc->caller_qn || !rc->callee_qn) {
            continue;
        }
        if (rc->confidence < CBM_LSP_CONFIDENCE_FLOOR) {
            continue;
        }
        const char *short_name = strrchr(rc->callee_qn, '.');
        short_name = short_name ? short_name + SKIP_ONE : rc->callee_qn;
        const char *call_leaf = cbm_pipeline_call_callee_leaf(call->callee_name);
        if (!call_leaf || strcmp(short_name, call_leaf) != 0) {
            continue;
        }
        if (!cbm_pipeline_qn_class_method_tail_eq(rc->caller_qn, call_tail)) {
            continue;
        }
        if (best_tail) {
            return NULL;
        }
        best_tail = rc;
    }
    return best_tail;
}

/* Resolve an LSP-emitted callee_qn to a graph-buffer node.
 *
 * Per-file LSPs sometimes emit `callee_qn` as the raw package-shaped
 * import path the source code uses rather than the project-qualified QN
 * the gbuf actually stores. The fallback rule is:
 *   1. try the LSP-emitted QN as-is;
 *   2. retry with `<project>.<callee_qn>` when needed;
 *   3. if both fail AND allow_tail_match is set (JVM callers only, see
 *      cbm_pipeline_lsp_allow_tail_match), use the exact node-name index
 *      to narrow candidates by short method name and accept exactly one
 *      Function/Method whose qualified_name has the same Class.method
 *      tail.
 *
 * Returns the matching node, or NULL if neither lookup hits. */
static inline const cbm_gbuf_node_t *cbm_pipeline_lsp_target_node(const cbm_gbuf_t *gbuf,
                                                                  const char *project_name,
                                                                  const char *callee_qn,
                                                                  bool allow_tail_match) {
    if (!gbuf || !callee_qn) {
        return NULL;
    }
    const cbm_gbuf_node_t *direct = cbm_gbuf_find_by_qn(gbuf, callee_qn);
    if (direct) {
        return direct;
    }
    /* Exact retries, in tie-rule order (CBM_MACRO_QN_SUFFIX): the definition
     * first -- the QN as given, then with the project prefix -- and only when
     * neither names a node the C-preprocessor macro of that name, in the same
     * two spellings. The macro case is a prototype the C resolver knows whose
     * only node in the graph is its rename macro (`extern T f(...);` plus
     * `#define f f_v2`). */
    bool add_prefix = false;
    if (project_name && project_name[0]) {
        size_t proj_len = strlen(project_name);
        add_prefix =
            !(strncmp(callee_qn, project_name, proj_len) == 0 && callee_qn[proj_len] == '.');
    }
    char buf[CBM_SZ_1K];
    if (add_prefix) {
        int written = snprintf(buf, sizeof(buf), "%s.%s", project_name, callee_qn);
        if (written > 0 && (size_t)written < sizeof(buf)) {
            const cbm_gbuf_node_t *prefixed = cbm_gbuf_find_by_qn(gbuf, buf);
            if (prefixed) {
                return prefixed;
            }
        }
    }
    int mwritten = snprintf(buf, sizeof(buf), "%s" CBM_MACRO_QN_SUFFIX, callee_qn);
    if (mwritten > 0 && (size_t)mwritten < sizeof(buf)) {
        const cbm_gbuf_node_t *macro = cbm_gbuf_find_by_qn(gbuf, buf);
        if (macro) {
            return macro;
        }
    }
    if (add_prefix) {
        mwritten = snprintf(buf, sizeof(buf), "%s.%s" CBM_MACRO_QN_SUFFIX, project_name, callee_qn);
        if (mwritten > 0 && (size_t)mwritten < sizeof(buf)) {
            const cbm_gbuf_node_t *macro = cbm_gbuf_find_by_qn(gbuf, buf);
            if (macro) {
                return macro;
            }
        }
    }
    if (!allow_tail_match) {
        return NULL;
    }

    const char *short_name = strrchr(callee_qn, '.');
    short_name = short_name ? short_name + SKIP_ONE : callee_qn;
    const char *callee_tail = cbm_pipeline_qn_class_method_tail(callee_qn);
    if (!callee_tail) {
        return NULL;
    }
    const cbm_gbuf_node_t **hits = NULL;
    int hit_count = 0;
    if (cbm_gbuf_find_by_name(gbuf, short_name, &hits, &hit_count) != 0 || hit_count == 0) {
        return NULL;
    }

    const cbm_gbuf_node_t *match = NULL;
    for (int i = 0; i < hit_count; i++) {
        const cbm_gbuf_node_t *cand = hits[i];
        if (!cand || !cand->label || !cand->qualified_name) {
            continue;
        }
        if (strcmp(cand->label, "Function") != 0 && strcmp(cand->label, "Method") != 0) {
            continue;
        }
        if (!cbm_pipeline_qn_class_method_tail_eq(cand->qualified_name, callee_tail)) {
            continue;
        }
        if (match) {
            return NULL;
        }
        match = cand;
    }
    return match;
}

/* Rust (#2053): true when the Rust LSP positively resolved a call to a
 * registered symbol that lives OUTSIDE the project — a std/core/alloc seed
 * method (`root.join()` on `root: &Path` -> std.path.Path.join) or a seeded
 * crate API. Such a row has no graph node, and the callers then fell back to
 * the textual registry, which bound the call to whatever project method
 * shares the leaf name (EvidenceTier::join, unique_name).
 *
 * The evidence is the strategy plus the QN's namespace. The listed strategies
 * emit the QN of a function the LSP registry actually holds; project defs are
 * registered under their project-prefixed QNs, so a registered QN outside the
 * project prefix is an embedded seed by construction. A project-prefixed QN
 * that merely failed the gbuf lookup is NOT external and keeps the #1085
 * registry fallback. Rust-only: suppressors stay per-language. */
static inline bool cbm_pipeline_rust_external_target(CBMLanguage lang, const char *strategy,
                                                     const char *callee_qn,
                                                     const char *project_name) {
    if (lang != CBM_LANG_RUST || !strategy || !callee_qn || !callee_qn[0] || !project_name ||
        !project_name[0]) {
        return false;
    }
    static const char *const registered_target_strategies[] = {
        "lsp_method_dispatch", "lsp_trait_dispatch", "lsp_deref_dispatch", "lsp_bound_dispatch",
        "lsp_direct",          "lsp_ufcs",           "lsp_constructor",
    };
    bool registered_target = std::any_of(
        std::begin(registered_target_strategies), std::end(registered_target_strategies),
        [strategy](const char *s) { return strcmp(strategy, s) == 0; });
    if (!registered_target) {
        return false;
    }
    size_t proj_len = strlen(project_name);
    return !(strncmp(callee_qn, project_name, proj_len) == 0 && callee_qn[proj_len] == '.');
}

/* A resolver row that proves the call's receiver is a type OUTSIDE the project:
 * PHP `$r->m()` / `R::m()` where `R` is imported from a vendor namespace
 * (#1186). When its target has no graph node, it rules out every name-only
 * fallback: the method of a vendor class is not a same-named project method. */
static inline bool cbm_pipeline_lsp_external_receiver(const CBMResolvedCall *resolved) {
    return resolved && resolved->strategy &&
           strcmp(resolved->strategy, "php_external_receiver") == 0;
}

/* registry.cpp (declared in pipeline.h too): a project class-like symbol
 * with this simple name exists. Declared here so this header stays free of
 * pipeline.h. */
typedef struct cbm_registry cbm_registry_t;
bool cbm_registry_has_type_named(const cbm_registry_t *r, const char *name);

/* #606 (upstream 5fd9e4f3): a sub-floor `php_method_typed_unindexed` row still
 * proves the receiver is not a project class, so a weak short-name fallback
 * must not bind the call: `$file->getUid()` on a vendor File became a CALLS
 * edge to App\Item::getUid. A project class with the same simple name keeps
 * the fallback, since the PHP LSP does not follow every cross-file type.
 *
 * The row is matched to the call by caller QN, call site (source_byte, when
 * both carry one) and callee leaf -- the same join cbm_pipeline_find_lsp_resolution
 * uses, without its confidence floor. (Upstream matches through
 * cbm_pipeline_invocation_site_rank / _leaf_matches, which this fork lacks.)
 * Both resolvers call this, so pass_calls.cpp and pass_parallel.cpp agree. */
static inline bool cbm_pipeline_php_receiver_type_unindexed(const CBMResolvedCallArray *arr,
                                                            const CBMCall *call,
                                                            const cbm_registry_t *registry) {
    if (!arr || !call || !call->enclosing_func_qn || !call->callee_name) {
        return false;
    }
    const char *call_leaf = cbm_pipeline_call_callee_leaf(call->callee_name);
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (!rc->caller_qn || !rc->callee_qn || !rc->strategy ||
            strcmp(rc->strategy, "php_method_typed_unindexed") != 0 ||
            strcmp(rc->caller_qn, call->enclosing_func_qn) != 0) {
            continue;
        }
        if (rc->source_byte && call->source_byte && rc->source_byte != call->source_byte) {
            continue;
        }
        const char *method_dot = strrchr(rc->callee_qn, '.');
        if (!method_dot || method_dot == rc->callee_qn || !call_leaf ||
            strcmp(method_dot + SKIP_ONE, call_leaf) != 0) {
            continue;
        }
        char class_qn[CBM_SZ_512];
        size_t class_len = (size_t)(method_dot - rc->callee_qn);
        if (class_len >= sizeof(class_qn)) {
            continue;
        }
        memcpy(class_qn, rc->callee_qn, class_len);
        class_qn[class_len] = '\0';
        return !cbm_registry_has_type_named(registry, cbm_lsp_bare_segment(class_qn));
    }
    return false;
}

/* The service-pattern name of a call: its callee text, or, when that names no
 * service and the LSP proved the receiver a vendor class, the vendor method
 * (`GuzzleClient::get` -> `GuzzleHttp.Client.get`). The extractor names a
 * scoped call by its method alone, and such a call never reaches the
 * registry, so the vendor class is its only service evidence (#1186). */
static inline const char *cbm_pipeline_service_name(const CBMCall *call,
                                                    const CBMResolvedCall *external,
                                                    cbm_svc_kind_t *kind) {
    *kind = cbm_service_pattern_match(call->callee_name);
    if (*kind == CBM_SVC_NONE && cbm_pipeline_lsp_external_receiver(external) &&
        external->callee_qn && external->callee_qn[0]) {
        *kind = cbm_service_pattern_match(external->callee_qn);
        if (*kind != CBM_SVC_NONE) {
            return external->callee_qn;
        }
    }
    return call->callee_name;
}

/* ── C member access: bind a field through its owner ─────────────────
 *
 * The member half of `a.b` / `a->b` is not a name in any scope: only the type
 * of `a` gives `b` a meaning. The C LSP publishes that as rows in
 * result->field_refs, one per (enclosing function, owner field). The usage and
 * read/write resolvers join them by enclosing function and member name, which
 * is exactly the granularity of the function -> Field edge they emit. */

/* One indexed row with its member name (the callee QN's bare segment),
 * computed once instead of in every comparison. */
typedef struct {
    const CBMResolvedCall *row;
    const char *member;
} cbm_pipeline_lsp_field_entry_t;

/* field_refs rows sorted by (caller, member name, target). A NULL `rows` with
 * a nonzero source array means the allocation failed and lookups scan the
 * array instead, so no typed edge is ever lost to an index. */
typedef struct {
    cbm_pipeline_lsp_field_entry_t *rows;
    int count;
} cbm_pipeline_lsp_field_index_t;

static inline bool cbm_pipeline_lsp_field_row_usable(const CBMResolvedCall *row) {
    return row->caller_qn && row->callee_qn && row->confidence >= CBM_LSP_CONFIDENCE_FLOOR;
}

static inline int cbm_pipeline_lsp_field_key_cmp(const CBMResolvedCall *row, const char *caller_qn,
                                                 const char *member) {
    int by_caller = strcmp(row->caller_qn, caller_qn);
    return by_caller != 0 ? by_caller : strcmp(cbm_lsp_bare_segment(row->callee_qn), member);
}

static inline int cbm_pipeline_lsp_field_entry_key_cmp(const cbm_pipeline_lsp_field_entry_t *entry,
                                                       const char *caller_qn, const char *member) {
    int by_caller = strcmp(entry->row->caller_qn, caller_qn);
    return by_caller != 0 ? by_caller : strcmp(entry->member, member);
}

static inline int cbm_pipeline_lsp_field_row_cmp(const void *left_ptr, const void *right_ptr) {
    const cbm_pipeline_lsp_field_entry_t *left = (const cbm_pipeline_lsp_field_entry_t *)left_ptr;
    const cbm_pipeline_lsp_field_entry_t *right = (const cbm_pipeline_lsp_field_entry_t *)right_ptr;
    int by_key = cbm_pipeline_lsp_field_entry_key_cmp(left, right->row->caller_qn, right->member);
    return by_key != 0 ? by_key : strcmp(left->row->callee_qn, right->row->callee_qn);
}

/* False means only allocation failure; lookups then take the linear path. */
static inline bool cbm_pipeline_lsp_field_index_build(const CBMResolvedCallArray *arr,
                                                      cbm_pipeline_lsp_field_index_t *index) {
    index->rows = NULL;
    index->count = 0;
    int field_count = 0;
    for (int i = 0; arr && i < arr->count; i++) {
        if (cbm_pipeline_lsp_field_row_usable(&arr->items[i])) {
            field_count++;
        }
    }
    if (field_count == 0) {
        return true;
    }
    cbm_pipeline_lsp_field_entry_t *rows =
        (cbm_pipeline_lsp_field_entry_t *)malloc((size_t)field_count * sizeof(*rows));
    if (!rows) {
        return false;
    }
    int next = 0;
    for (int i = 0; i < arr->count; i++) {
        if (cbm_pipeline_lsp_field_row_usable(&arr->items[i])) {
            rows[next].row = &arr->items[i];
            rows[next].member = cbm_lsp_bare_segment(arr->items[i].callee_qn);
            next++;
        }
    }
    qsort(rows, (size_t)field_count, sizeof(*rows), cbm_pipeline_lsp_field_row_cmp);
    index->rows = rows;
    index->count = field_count;
    return true;
}

static inline void cbm_pipeline_lsp_field_index_free(cbm_pipeline_lsp_field_index_t *index) {
    free(index->rows);
    index->rows = NULL;
    index->count = 0;
}

/* Walks the owner rows of one (function, member name). `index` NULL selects
 * the linear scan over `arr`. */
typedef struct {
    const cbm_pipeline_lsp_field_index_t *index;
    const CBMResolvedCallArray *arr;
    const char *caller_qn;
    const char *member;
    int next;
} cbm_pipeline_lsp_field_cursor_t;

static inline cbm_pipeline_lsp_field_cursor_t cbm_pipeline_lsp_field_cursor(
    const CBMResolvedCallArray *arr, const cbm_pipeline_lsp_field_index_t *index,
    const char *caller_qn, const char *member) {
    cbm_pipeline_lsp_field_cursor_t cursor = {index, arr, caller_qn, member, 0};
    if (index && caller_qn && member) {
        int low = 0;
        int high = index->count;
        while (low < high) {
            int middle = low + ((high - low) / 2);
            if (cbm_pipeline_lsp_field_entry_key_cmp(&index->rows[middle], caller_qn, member) < 0) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        cursor.next = low;
    }
    return cursor;
}

static inline const CBMResolvedCall *cbm_pipeline_lsp_field_next(
    cbm_pipeline_lsp_field_cursor_t *cursor) {
    if (!cursor->caller_qn || !cursor->member) {
        return NULL;
    }
    if (cursor->index) {
        if (cursor->next >= cursor->index->count) {
            return NULL;
        }
        const cbm_pipeline_lsp_field_entry_t *entry = &cursor->index->rows[cursor->next];
        if (cbm_pipeline_lsp_field_entry_key_cmp(entry, cursor->caller_qn, cursor->member) != 0) {
            return NULL;
        }
        cursor->next++;
        return entry->row;
    }
    while (cursor->arr && cursor->next < cursor->arr->count) {
        const CBMResolvedCall *row = &cursor->arr->items[cursor->next++];
        if (cbm_pipeline_lsp_field_row_usable(row) &&
            cbm_pipeline_lsp_field_key_cmp(row, cursor->caller_qn, cursor->member) == 0) {
            return row;
        }
    }
    return NULL;
}

/* The graph node of an owner row, or NULL when the owner is not a project
 * Field (a libc struct, say). Exact QN only: a name fallback here would undo
 * the typing the row carries. */
static inline const cbm_gbuf_node_t *cbm_pipeline_lsp_field_node(const cbm_gbuf_t *gbuf,
                                                                 const char *project_name,
                                                                 const CBMResolvedCall *row) {
    const cbm_gbuf_node_t *node =
        cbm_pipeline_lsp_target_node(gbuf, project_name, row->callee_qn, false);
    return node && node->label && strcmp(node->label, "Field") == 0 ? node : NULL;
}

/* The Field a C/C++ member call (`cb.close(ctx)`) lands on, decided by the
 * type of its object: among the owner rows of (enclosing function, member
 * name) that name a project Field, the one with the smallest QN, so the
 * sequential scan and the sorted index agree. *typed says whether the LSP
 * typed the object at all; NULL with *typed set means "typed, and no project
 * Field is meant", NULL without it means the caller may try the member name. */
static inline const cbm_gbuf_node_t *cbm_pipeline_c_member_call_field(
    const CBMResolvedCallArray *arr, const cbm_pipeline_lsp_field_index_t *index,
    const cbm_gbuf_t *gbuf, const char *project_name, const char *enclosing_func_qn,
    const char *callee_text, bool *typed) {
    cbm_pipeline_lsp_field_cursor_t owners = cbm_pipeline_lsp_field_cursor(
        arr, index, enclosing_func_qn, cbm_lsp_bare_segment(callee_text));
    const cbm_gbuf_node_t *best = NULL;
    const CBMResolvedCall *owner;
    *typed = false;
    while ((owner = cbm_pipeline_lsp_field_next(&owners)) != NULL) {
        *typed = true;
        const cbm_gbuf_node_t *field = cbm_pipeline_lsp_field_node(gbuf, project_name, owner);
        if (field && (!best || strcmp(field->qualified_name, best->qualified_name) < 0)) {
            best = field;
        }
    }
    return best;
}

#ifdef __cplusplus
}
#endif

// A bounded proof for the extra work formerly caused by primitive operator
// candidates. Unknown/external/method calls and larger files keep the full pass.
static inline bool cbm_pipeline_cpp_primitives_complete(const CBMFileResult *result) {
    if (!result || result->deferred_cpp_operator_count == 0 ||
        result->pending_cpp_operator_count != 0 || result->overload_count != 0 ||
        result->calls.count > 128 || result->resolved_calls.count > 256 || result->defs.count > 512)
        return false;
    for (int i = 0; i < result->calls.count; ++i) {
        const CBMCall *call = &result->calls.items[i];
        const CBMResolvedCall *resolved =
            cbm_pipeline_find_lsp_resolution(&result->resolved_calls, call, false);
        if (!resolved || !resolved->strategy || strcmp(resolved->strategy, "lsp_direct") != 0 ||
            !resolved->callee_qn)
            return false;
        bool local_definition = false;
        for (int j = 0; j < result->defs.count; ++j) {
            const CBMDefinition *def = &result->defs.items[j];
            if (def->label && def->qualified_name && strcmp(def->label, "Function") == 0 &&
                strcmp(def->qualified_name, resolved->callee_qn) == 0) {
                local_definition = true;
                break;
            }
        }
        if (!local_definition)
            return false;
    }
    return true;
}

#endif /* CBM_PIPELINE_LSP_RESOLVE_H */
