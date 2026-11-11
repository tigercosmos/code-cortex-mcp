/*
 * pipeline.h — Indexing pipeline orchestrator.
 *
 * Orchestrates multi-pass indexing of a repository:
 *   1. Structure: Project/Folder/Package/File nodes
 *   2. Definitions: Extract + write nodes + build registry
 *   3. Imports: Resolve import edges
 *   4. Calls: Call resolution (registry + LSP)
 *   5. Usages: Usage/type_ref edges
 *   6. Semantic: Inherits/decorates/implements
 *   7. Post: Tests, communities, HTTP links, config, git history
 *
 * Depends on: foundation, extraction, lsp, store, graph_buffer, discover
 */
#ifndef CBM_PIPELINE_H
#define CBM_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "discover/discover.h"         /* cbm_ignored_file_t (#963) */
#include "foundation/constants.h"      /* CBM_SZ_512 */
#include "graph_buffer/graph_buffer.h" /* cbm_gbuf_node_t */
#include "foundation/hash_table.h"     /* CBMHashTable (#1355 package lookups) */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct cbm_store cbm_store_t;
typedef struct cbm_gbuf cbm_gbuf_t;

/* ── Opaque handle ──────────────────────────────────────────────── */

typedef struct cbm_pipeline cbm_pipeline_t;

/* ── Index mode ─────────────────────────────────────────────────── */

#ifndef CBM_INDEX_MODE_T_DEFINED
#define CBM_INDEX_MODE_T_DEFINED
typedef enum {
    /* All modes run the LSP type-aware call/usage resolution (per-file +
     * cross-file). The mode only controls file discovery breadth and whether
     * SIMILAR_TO / SEMANTICALLY_RELATED edges are computed. */
    CBM_MODE_FULL = 0,     /* Full: everything including SIMILAR_TO + SEMANTICALLY_RELATED */
    CBM_MODE_MODERATE = 1, /* Moderate: fast discovery + SIMILAR_TO + SEMANTICALLY_RELATED */
    CBM_MODE_FAST = 2,     /* Fast: skip non-essential files, no similarity/semantic edges */
} cbm_index_mode_t;
#endif

/* ── Pipeline lifecycle ─────────────────────────────────────────── */

/* Create a new pipeline. Caller owns the result. */
cbm_pipeline_t *cbm_pipeline_new(const char *repo_path, const char *db_path, cbm_index_mode_t mode);

/* Enable persistent artifact export (.code-cortex/graph.db.zst).
 * When enabled, the pipeline writes a compressed artifact after indexing. */
void cbm_pipeline_set_persistence(cbm_pipeline_t *p, bool enabled);

/* Free a pipeline and all its internal state. NULL-safe. */
void cbm_pipeline_free(cbm_pipeline_t *p);

/* Claim this pipeline's DB before any artifact installation or rebuild.
 * Nonblocking: 0 acquired, CBM_INDEX_BUSY held elsewhere, -1 error. Reentrant
 * only for this pipeline object. run() releases on every exit; free() releases
 * an unused claim. The claim freezes the canonical DB target/project name. */
int cbm_pipeline_claim_index(cbm_pipeline_t *p);

/* Run the full indexing pipeline. Returns 0, CBM_INDEX_BUSY, or -1 on error.
 * Discovers files, extracts, resolves, and dumps to SQLite. */
int cbm_pipeline_run(cbm_pipeline_t *p);

/* Callback run() calls once a run has its database in place, before it
 * releases the index lease: what must happen before another process can
 * take the database (the cache sweep judges it under that lease) goes here,
 * not after run() returns. It also runs when a requested artifact export
 * fails the run after the database was committed. Runs on run()'s thread;
 * NULL clears. Set before run. */
typedef void (*cbm_pipeline_indexed_fn)(const cbm_pipeline_t *p, void *ud);
void cbm_pipeline_set_on_indexed(cbm_pipeline_t *p, cbm_pipeline_indexed_fn fn, void *ud);

/* Request cancellation of a running pipeline (thread-safe). */
void cbm_pipeline_cancel(cbm_pipeline_t *p);

/* Get the project name derived from repo_path. Returned string is
 * owned by the pipeline. Valid until cbm_pipeline_free(). */
const char *cbm_pipeline_project_name(const cbm_pipeline_t *p);

/* Override the derived project name with a sanitized user-provided label. */
bool cbm_pipeline_set_project_name(cbm_pipeline_t *p, const char *name);

/* Get the index mode (CBM_MODE_FULL, CBM_MODE_MODERATE, CBM_MODE_FAST). */
int cbm_pipeline_get_mode(const cbm_pipeline_t *p);

/* Get the list of directory subtrees skipped during discovery (#411).
 * *out receives a borrowed array of rel-path strings (owned by the pipeline,
 * valid until cbm_pipeline_free()); *count receives its length. Both are set
 * to NULL/0 when p is NULL or nothing was excluded. Do not free. */
void cbm_pipeline_get_excluded(const cbm_pipeline_t *p, char ***out, int *count);

/* Committed node/edge counts captured at dump time (-1 when dump did not run).
 * Nodes are the #334 plausibility-gate axis; edges are informational only. */
/* True when this run rebuilt the index because the stored format identity did
 * not match CBM_INDEX_FORMAT_VERSION (#769). Surfaced by index_repository. */
bool cbm_pipeline_had_format_migration(const cbm_pipeline_t *p);

/* The artifact export error of the most recent cbm_pipeline_run on p, or ""
 * when that run did not fail in the post-publish artifact export (upstream
 * #1665). Owned by p. */
const char *cbm_pipeline_export_error(const cbm_pipeline_t *p);

/* The run failed because the database's branch could not be recorded after
 * it was built (busy or read-only cache). */
bool cbm_pipeline_branch_stamp_failed(const cbm_pipeline_t *p);

void cbm_pipeline_get_committed_counts(const cbm_pipeline_t *p, int *nodes, int *edges);

/* Per-branch databases (pipeline/branch_slots.h): how many parked branch
 * databases to keep for this project. < 1 turns parking off, so a branch
 * switch reindexes the one database in place. Default
 * CBM_BRANCH_SLOTS_DEFAULT_KEEP, or 0 when CBM_BRANCH_INDEX=0. Set before run. */
void cbm_pipeline_set_branch_keep(cbm_pipeline_t *p, int keep);
/* Total bytes the parked databases of this checkout may take on disk (<= 0:
 * no size cap); the least recently parked go first. Default
 * CBM_BRANCH_SLOTS_DEFAULT_BUDGET, or CBM_BRANCH_INDEX_BUDGET. Set before run. */
void cbm_pipeline_set_branch_budget(cbm_pipeline_t *p, int64_t budget);

/* After run: what the branch switch did ("none", "restored", "seeded",
 * "in_place"), the slug the database had been built for (NULL unknown), the
 * slug this run indexed (NULL outside git), and how many source files the run
 * re-extracted (-1 unknown). Borrowed strings, valid until free. Any out
 * pointer may be NULL. */
void cbm_pipeline_get_branch_switch(const cbm_pipeline_t *p, const char **action, const char **from,
                                    const char **to, int *files_reparsed);

/* ── Per-file indexing failures (Stage 2 / Track B) ─────────────── */

/* One source file that was skipped during indexing. All strings are owned by
 * the pipeline (copied on record, freed in cbm_pipeline_free). A skip is the
 * expected, handled outcome of a bad/oversized file — indexing continues and
 * the run still reports status "indexed"; these are surfaced (not errors that
 * fail the run) via MCP `skipped[]` / the CLI / a per-run logfile. */
typedef struct {
    char *path;   /* repo-relative path of the skipped file */
    char *reason; /* human-readable cause (e.g. "oversized (712 MB > 512 MB)",
                   * "parse timeout", "read failed"). For phase "parse_partial"
                   * this carries the 1-based line-range list ("12-40,88-90")
                   * of the unparseable regions. */
    char *phase;  /* "read" | "extract" | "oversized" | "parse_partial" |
                   * "parse_unusable" (a parse_partial whose one range covers
                   * 80%+ of the file — also indexed, also not a skip).
                   * "parse_partial" (#963) is NOT a skip: the file WAS indexed
                   * but contains tree-sitter ERROR/MISSING regions whose
                   * constructs are absent from the graph (best-effort signal —
                   * absence of the flag is NOT a completeness guarantee). The
                   * MCP layer reports it separately from skipped[].
                   * "walk_truncated" / "lsp_skipped" are likewise indexed,
                   * not skips: the unified walk stopped at its budget, or the
                   * file ran without its LSP walks (reason says how far /
                   * which rule — cbm_walk_coverage_entry). "cross_lsp"
                   * is a RESERVED phase string for Track C's crash-attribution
                   * signal and is intentionally NOT emitted today (the
                   * cross-LSP passes are best-effort/void with no genuine
                   * per-file failure). */
} cbm_file_error_t;

/* Record a skipped file. path/reason/phase are copied. NULL-safe on p.
 *
 * NOT thread-safe: call it from the sequential extraction pass, or from the
 * parallel merge step (never from inside a parallel worker — workers collect
 * into per-worker lists and merge sequentially). */
void cbm_pipeline_add_file_error(cbm_pipeline_t *p, const char *path, const char *reason,
                                 const char *phase);

/* Borrowed accessor for the recorded skips (owned by the pipeline, valid until
 * cbm_pipeline_free()). out and count are set to NULL and 0 when p is NULL or
 * nothing was skipped. Do not free. */
void cbm_pipeline_get_file_errors(const cbm_pipeline_t *p, cbm_file_error_t **out, int *count);

/* Borrowed accessor for the individually-ignored files captured during
 * discovery (#963 "purposely not indexed" — by design, not failures). count
 * is the stored (capped) length, total the uncapped number seen. Do not
 * free. */
void cbm_pipeline_get_ignored(const cbm_pipeline_t *p, cbm_ignored_file_t **out, int *count,
                              int *total);

/* ── Process-local gate (global extraction state and server threads) ── */

/* Try to acquire the process-local gate. Returns true if acquired,
 * false if another pipeline is already running (non-blocking).
 * Use this in the watcher — skip reindex if busy. */
bool cbm_pipeline_try_lock(void);

/* Acquire the global index lock, blocking until available.
 * Use this in MCP handler and autoindex — wait for busy watcher to finish. */
void cbm_pipeline_lock(void);

/* Release the global index lock. */
void cbm_pipeline_unlock(void);

/* ── FQN helpers (used by passes and external callers) ──────────── */

/* Compute a qualified name: project.dir.parts.name
 * Strips extension, converts / to ., drops __init__ and index.
 * Caller must free() the returned string. */
char *cbm_pipeline_fqn_compute(const char *project, const char *rel_path, const char *name);

/* Module QN: project.dir.parts (no name). Caller must free(). */
char *cbm_pipeline_fqn_module(const char *project, const char *rel_path);

/* Language-aware module QN. When `module_is_dir` is true (Java/Go package
 * semantics) the module is derived from the CONTAINING DIRECTORY (the filename
 * stem is dropped), so it agrees with the extraction-side def QNs; when false
 * it is exactly cbm_pipeline_fqn_module(). Caller must free(). */
char *cbm_pipeline_fqn_module_dir(const char *project, const char *rel_path, bool module_is_dir);

/* Folder QN: project.dir.parts. Caller must free(). */
char *cbm_pipeline_fqn_folder(const char *project, const char *rel_dir);

/* Resolve an import specifier that uses a relative path (./foo, ../bar, .foo,
 * or an unqualified local name like "foo.h") against the importing file's
 * path.  Returns a malloc'd normalized relative path without extension
 * (e.g. "src/api/helpers") suitable for passing to cbm_pipeline_fqn_module,
 * or NULL if the specifier is not a relative path (bare module names like
 * "lodash", "django", "github.com/foo/bar" return NULL — the caller should
 * treat those as external/unresolvable). Handles ".", "..", and leading
 * dot-only segments used by Python relative imports. */
char *cbm_pipeline_resolve_relative_import(const char *source_rel, const char *module_path);

/* Join the importing file's directory with a slash-separated relative path,
 * collapsing "." and ".." segments.  Language-neutral: the extension is kept
 * (e.g. "src/a/b.c" + "../x.h" → "src/x.h").  Returns a malloc'd path, or NULL
 * on overflow or for a path that climbs above the repository root. */
char *cbm_pipeline_normalize_relative_path(const char *source_rel, const char *rel_path);

/* Derive project name from an absolute path.
 * Replaces / and : with -, collapses --, trims leading -.
 * Caller must free() the returned string. */
char *cbm_project_name_from_path(const char *abs_path);

/* ── Function Registry ──────────────────────────────────────────── */

typedef struct cbm_registry cbm_registry_t;

typedef struct {
    const char *qualified_name; /* borrowed from registry */
    const char *strategy;       /* resolution strategy name */
    double confidence;          /* 0.0–1.0 */
    int candidate_count;
} cbm_resolution_t;

/* Create/free a function registry. */
cbm_registry_t *cbm_registry_new(void);
void cbm_registry_free(cbm_registry_t *r);

/* Register a function/method/class. All strings are copied. */
void cbm_registry_add(cbm_registry_t *r, const char *name, const char *qualified_name,
                      const char *label);

/* cbm_registry_add with the definition's language (CBM_LANG_COUNT: unknown).
 * The language settles one suffix_match tie: a C# reference-assembly stub
 * (`.ref.` in its QN) loses to an equally good implementation. */
void cbm_registry_add_lang(cbm_registry_t *r, const char *name, const char *qualified_name,
                           const char *label, CBMLanguage lang);

/* Resolve a callee name using prioritized strategies.
 * import_map: NULL-terminated array of {local_name, resolved_qn} pairs, or NULL.
 * Returns result with qualified_name="" if unresolved. */

/* #725: true when a suffix_match CALLS edge crosses a language boundary and
 * should be dropped. unique_name (candidates == 1) is left alone; JS/TS/TSX are
 * one family. Pure — unit-tested in test_registry.cpp. */
bool cbm_suppress_cross_language_suffix_match(CBMLanguage caller_lang, const char *target_file_path,
                                              const char *strategy);

/* #1928: USAGE/WRITES/READS analog of the CALLS guard above. Reference edges
 * resolved by the short-name registry carry no import-closure evidence, so a
 * cross-language binding is a bare-name collision for EVERY strategy -- drop
 * it whenever the caller's language and the target file's language disagree
 * (JS/TS family members and the C/C++ header family excepted). Pure;
 * unit-tested in test_registry.cpp. */
bool cbm_suppress_cross_language_ref(CBMLanguage caller_lang, const char *target_file_path);

/* #1942/#1962/#2666: a bare Go or C identifier can never denote a struct
 * field -- field access is always a selector expression (or, in C, a
 * designator). Drops a READS/WRITES/USAGE bind whose target is a Field unless
 * the reference was recorded as the member half of a selector
 * (CBMUsage/CBMReadWrite.is_member_access). Go and C only: other OO languages
 * (and C++, which every `.h` is parsed as) reference their own members bare
 * inside method bodies. Pure; unit-tested in test_registry.cpp. */
bool cbm_suppress_bare_field_ref(CBMLanguage lang, bool is_member_access, const char *target_label);

/* CALLS companion of the above: a Go CALLS edge onto a Field node is kept only
 * when a type-aware lsp_* strategy produced it. Textual registry strategies
 * (unique_name, suffix_match, field_type_hint, same_module, ...) bind a call to
 * a same-named struct field by spelling alone. Pure; unit-tested in
 * test_registry.cpp. */
bool cbm_go_suppress_textual_field_call(bool is_go, const char *target_label, const char *strategy);

/* Caller-owned memo of the last target file's language, for the per-edge
 * reference guard. Zero-initialize one per file loop ({}); never share it
 * across files or threads. */
typedef struct {
    char base[CBM_SZ_64];
    CBMLanguage lang;
    bool valid;
} cbm_lang_memo_t;

/* One guard for a resolved USAGE/READS/WRITES edge: true when the edge crosses
 * a language boundary (cbm_suppress_cross_language_ref) or binds a bare Go or
 * C reference to a Field (cbm_suppress_bare_field_ref). Shared by the
 * sequential (pass_usages.cpp) and parallel (pass_parallel.cpp) resolvers.
 * memo may be NULL; it changes cost, never the answer. */
bool cbm_suppress_ref_edge(CBMLanguage lang, bool is_member_access, const cbm_gbuf_node_t *target,
                           cbm_lang_memo_t *memo);

/* What to do with a call whose registry resolution landed on a Field node.
 * Pure; unit-tested in test_registry.cpp. */
typedef enum {
    CBM_FIELD_CALL_KEEP = 0, /* not a Field target, or a language this rule leaves alone */
    CBM_FIELD_CALL_DROP,     /* a Field can never be this call's target */
    CBM_FIELD_CALL_BY_OWNER, /* C/C++ member call: the object's type names the Field */
} cbm_field_call_policy_t;
cbm_field_call_policy_t cbm_call_onto_field_policy(CBMLanguage caller_lang, const char *callee_text,
                                                   const char *target_label,
                                                   const char *target_file_path);

/* True when a reference is the member half of a C `a.b` / `a->b`. Such a
 * reference binds through the C LSP's field-owner rows (the type of `a`); with
 * no typed owner it binds only cbm_registry_unique_field_qn(). C only. Pure;
 * unit-tested in test_registry.cpp. */
bool cbm_c_member_binds_by_owner(bool is_c, bool is_member_access);

/* The QN of the Field `member_name` names when exactly one Field in the
 * registry carries that name (symbols of other labels are not candidates for
 * a member); NULL when none or several do. */
const char *cbm_registry_unique_field_qn(const cbm_registry_t *r, const char *member_name);

/* True when two or more distinct Fields in the registry carry `member_name`:
 * only then does binding the member depend on the type of its object. */
bool cbm_registry_field_name_ambiguous(const cbm_registry_t *r, const char *member_name);

/* One guard for a resolved CALLS target: true when a suffix_match edge crosses
 * a language boundary (cbm_suppress_cross_language_suffix_match) or a textual
 * Go match binds a Field (cbm_go_suppress_textual_field_call). Shared by
 * pass_calls.cpp and pass_parallel.cpp. */
bool cbm_suppress_call_target(CBMLanguage lang, const cbm_gbuf_node_t *target,
                              const char *strategy);

/* Relation-permitting resolve, for SQL FROM/JOIN lineage ONLY. The default
 * cbm_registry_resolve vetoes Table/View results because common relation names
 * collide with code identifiers in every language. Uncached by design — see the
 * implementation. */
cbm_resolution_t cbm_registry_resolve_lineage(const cbm_registry_t *r, const char *callee_name,
                                              const char *module_qn, const char **import_map_keys,
                                              const char **import_map_vals, int import_map_count);

cbm_resolution_t cbm_registry_resolve(const cbm_registry_t *r, const char *callee_name,
                                      const char *module_qn, const char **import_map_keys,
                                      const char **import_map_vals, int import_map_count);

/* Explicit-language resolution (upstream 132ded8c): a Python package file
 * (`pkg/__init__.py`) resolves same-module names under its package scope
 * (`proj.pkg`); every other language behaves exactly as cbm_registry_resolve. */
cbm_resolution_t cbm_registry_resolve_lang(const cbm_registry_t *r, const char *callee_name,
                                           const char *module_qn, const char **import_map_keys,
                                           const char **import_map_vals, int import_map_count,
                                           CBMLanguage lang);

/* The CALLS variant: as cbm_registry_resolve_lang, except that a bare C call (no
 * `.` / `->`) does not consider Field or Method nodes, which it can never
 * call. Shares the per-file cache, keyed on that choice too. */
cbm_resolution_t cbm_registry_resolve_call_lang(const cbm_registry_t *r, const char *callee_name,
                                                const char *module_qn, const char **import_map_keys,
                                                const char **import_map_vals, int import_map_count,
                                                CBMLanguage lang);

/* Per-file memoization cache for is_import_reachable. Thread-local —
 * each resolve worker owns its own cache. Call _begin at the start
 * of resolve_file_calls (or any per-file resolve loop) and _end at
 * the end. The cache MUST be invalidated between files because
 * is_import_reachable's truth depends on the file's import_vals. */
void cbm_registry_reach_cache_begin(int estimated_capacity);
void cbm_registry_reach_cache_end(void);

/* Per-file import-map prefix → module-QN hash. Turns the linear
 * strcmp scan inside resolve_import_map into O(1). Keys/values are
 * BORROWED — caller must keep the import_map arrays alive for the
 * cache lifetime. Invalidate between files via _end. */
void cbm_registry_import_map_cache_begin(const char **keys, const char **vals, int count);
void cbm_registry_import_map_cache_end(void);

/* Per-file full-result cache for cbm_registry_resolve. The same
 * callee_name appears in many call sites within a file; module_qn
 * is constant per file so each name resolves identically. First
 * lookup does the full strategy chain; repeats are O(1) hash hits.
 * This eliminates ~75% of the resolve-chain work on K8s where the
 * same names ("Get", "Add", "New", etc) appear hundreds of times. */
void cbm_registry_resolve_cache_begin(int estimated_capacity);
void cbm_registry_resolve_cache_end(void);

/* Check if a qualified name exists in the registry. */
bool cbm_registry_exists(const cbm_registry_t *r, const char *qn);

/* True if `name` is one of the curated Perl core builtins (perlfunc). Used by
 * the call-resolution passes to suppress generic-resolver CALLS edges from Perl
 * builtin invocations (push/shift/keys/...) to project subs that merely share
 * the name. Perl-scoped: callers gate on the file language. */
bool cbm_perl_is_builtin(const char *name);

/* Decide whether a resolved Perl call edge is generic-resolver noise to drop
 * (#476): true only for Perl, only for a builtin/method call, and only when the
 * match used a weak short-name strategy — high-confidence same_module/import_map
 * matches are kept. Pure; unit-tested in test_registry.c. */
bool cbm_perl_suppress_generic_match(bool is_perl, bool is_method, const char *callee_name,
                                     const char *strategy);

/* True for the registry strategies that are pure short-name guesses
 * (suffix_match / unique_name / field_type_hint / fuzzy). NULL/empty is false.
 * The single drop-list shared by every weak-match guard (the two call guards
 * below and the cross-LSP base-class resolution), so they cannot disagree about
 * what "weak" means. */
bool cbm_weak_short_name_strategy(const char *strategy);

/* Decide whether a resolved member-call edge is weak-strategy noise to drop
 * (#592/#606/#1276): true only when the CALLER's per-language gate says the
 * guard applies (`enabled`), only for a member call with an unresolved receiver
 * (is_method), and only when the match used a weak short-name strategy.
 * Explicit drop-list keeps every lsp_* / import / same-module / qualified match.
 * The language set lives in cbm_suppress_weak_call. Pure; unit-tested in
 * test_registry.cpp. */
bool cbm_suppress_weak_member_match(bool enabled, bool is_method, const char *strategy);

/* Go analog of the member guard (upstream #1906): a selector call whose
 * receiver the Go LSP could not type must not be bound by a receiver-blind
 * short-name strategy. Drops suffix_match / field_type_hint / fuzzy always, and
 * unique_name only when its confidence carries the import-unreachability
 * penalty (the stdlib/vendor hijack shape). Composed next to
 * cbm_suppress_weak_call at the same seam of BOTH resolvers. Pure;
 * unit-tested in test_registry.cpp. */
bool cbm_go_suppress_weak_method_match(bool is_go, bool is_method, const char *strategy,
                                       double confidence);

/* A bare Go `init` (a call or reference that is not the member half of a
 * selector) is always a local name: Go forbids referring to a package init
 * function, and a method or field needs a selector. True when such a name
 * resolved onto a Function (every Go Function named init is a package init;
 * any registry strategy) or onto a Method or Field found by a weak short-name
 * strategy. lsp_* answers are kept. Go only. Pure; unit-tested in
 * test_registry.cpp; wired at the CALLS, USAGE and READS/WRITES seams of both
 * resolvers (pass_calls.cpp, pass_usages.cpp, pass_parallel.cpp). */
bool cbm_go_suppress_bare_init_bind(bool is_go, bool is_member, const char *name,
                                    const char *strategy, const char *target_label);

/* Bare-call counterpart of the guard above. True when a resolved BARE call edge
 * binds a callee that is shadowed by an enclosing parameter, and the match came
 * from a weak short-name strategy — so the edge is fabricated by construction
 * (`def f(run): run()` must not bind an unrelated `SatoriLive.run`). Keyed on
 * the SCOPE FACT, not on the callee's spelling. The language gate comes from
 * cbm_suppress_weak_call. Pure; unit-tested in test_registry.cpp. */
bool cbm_suppress_weak_local_binding_call(bool enabled, bool callee_is_locally_bound,
                                          const char *strategy);

/* Import-binding counterpart (#2127). True when the callee's root identifier
 * is bound by an import of this file whose module chain contradicts the
 * resolved target (`from unittest.mock import patch; patch()` must not bind a
 * project `PkgConfigView.patch`), and the match came from a weak short-name
 * strategy. Same drop-list as the guards above; the language set lives at the
 * call sites and must be identical in pass_calls.c and pass_parallel.c.
 * Pure; unit-tested in test_registry.c. */
bool cbm_suppress_weak_import_bound_call(bool enabled, bool import_binding_contradicts,
                                         const char *strategy);

/* #1355: drop a project-wide same-name guess (suffix_match / unique_name /
 * field_type_hint / fuzzy) for a BARE call whose name the calling file binds to
 * a NON-RELATIVE (package) import that resolved to nothing in the graph —
 * `import { eq } from "drizzle-orm"` must not make `eq(...)` a CALLS edge to an
 * unrelated project `eq`. A name the import map does bind, a relative
 * specifier, a member/qualified callee, and every import-/receiver-aware
 * strategy are all kept. `indexed_packages` is the pipeline package map: a
 * specifier naming a package the tree's own manifests declare (a workspace
 * sibling) counts as in-tree. `declared_packages` is the pipeline namespace
 * map and does the same for package-path specifiers (`import
 * org.example.util.f` is kept when an indexed file declares `package
 * org.example.util`). NULL disables either check. The language gate lives in
 * cbm_suppress_external_import_call. Pure; unit-tested in test_registry.cpp. */
bool cbm_suppress_external_import_shadow(const char *callee_name, const char *strategy,
                                         const CBMImportArray *file_imports,
                                         const char **import_map_keys, int import_map_count,
                                         const CBMHashTable *indexed_packages,
                                         const CBMHashTable *declared_packages);

/* #1128: true when a JS/TS-family member call resolved by `same_module` only
 * through the lookup that drops its receiver (`resolved_qn` does not end in the
 * whole callee), while that receiver is bound to an import of the file
 * (`file_imports`: ES import or CommonJS require, not a synthesized local), to
 * an AMD dependency (`receiver_is_amd_dependency`, the extractor's per-call
 * fact), or is a builtin global of the JS runtime (`console`, `Math`, `JSON`,
 * ...): `axios.get()` must not bind the `get` this file happens to define. Any
 * other receiver keeps the edge (`context.push()` onto an object-literal method
 * stored at module level). The language list is inside the helper, so
 * pass_calls.cpp and pass_parallel.cpp cannot drift. Only the plain-CALLS
 * fall-through is dropped. Pure; unit-tested in test_registry.cpp. */
bool cbm_suppress_same_module_bound_receiver(CBMLanguage lang, bool is_method,
                                             const char *callee_name, const char *resolved_qn,
                                             const char *strategy,
                                             const CBMImportArray *file_imports,
                                             bool receiver_is_amd_dependency);

/* True if `name` is a method of a Python builtin type (str/bytes/list/dict/set/
 * file) or a builtin function seen as an attribute call. A language fact, kept
 * as a sorted table like the Perl builtins. */
bool cbm_python_is_builtin_member(const char *name);

/* The member guard's one exemption: a Python member call whose receiver is an
 * attribute chain rooted at self/cls (an object the class owns), whose callee
 * has exactly one project definition (strategy unique_name) and is not a
 * builtin type's own method keeps its edge. cbm_suppress_weak_call applies it,
 * so the sequential and parallel resolvers agree. Pure; unit-tested in
 * test_registry.cpp. */
bool cbm_weak_member_unique_name_exempt(bool is_python, bool receiver_is_self_attribute,
                                        const char *callee_name, const char *strategy);

/* True when a resolved call's plain CALLS edge is weak-strategy noise: the
 * member guard (Python, JS/TS/TSX, and the HTML/Vue/Svelte/Astro hosts of
 * embedded scripts) less its Python self-attribute exemption, or the bare-call
 * local-binding guard (Python). The one owner of both language sets; the
 * sequential and parallel resolvers both call it. */
bool cbm_suppress_weak_call(CBMLanguage lang, const CBMCall *call, const char *strategy);

/* Get the label of a qualified name, or NULL if not found. */
const char *cbm_registry_label_of(const cbm_registry_t *r, const char *qn);

/* Find all QNs with a given simple name. Sets *out and *count.
 * Caller does NOT free the array (owned by registry). */
int cbm_registry_find_by_name(const cbm_registry_t *r, const char *name, const char ***out,
                              int *count);

/* Return total number of entries. */
int cbm_registry_size(const cbm_registry_t *r);

/* Find all qualified names ending with ".suffix".
 * Sets *out to heap-allocated array of borrowed string pointers.
 * Caller must free(*out) but NOT the individual strings.
 * Returns count of matches. */
int cbm_registry_find_ending_with(const cbm_registry_t *r, const char *suffix, const char ***out);

/* Check if candidate QN's module prefix is reachable via any import value. */
bool cbm_registry_is_import_reachable(const char *candidate_qn, const char **import_vals,
                                      int import_count);

/* Fuzzy resolve: match callee by bare function name (last segment after dots).
 * Returns result with ok=true if found, ok=false if not.
 * Lower confidence than Resolve (0.40 single, 0.30 multiple). */
typedef struct {
    cbm_resolution_t result;
    bool ok;
} cbm_fuzzy_result_t;

cbm_fuzzy_result_t cbm_registry_fuzzy_resolve(const cbm_registry_t *r, const char *callee_name,
                                              const char *module_qn, const char **import_map_keys,
                                              const char **import_map_vals, int import_map_count);

const char *cbm_confidence_band(double score);

/* ── Git diff hunks (pass_gitdiff.cpp) ────────────────────────────
 * Public (unlike the rest of pipeline_internal.h) because detect_changes
 * (src/mcp/mcp.cpp) scopes seed detection to changed line ranges, not just
 * changed files. */

typedef struct {
    char path[CBM_SZ_512];
    int start_line;
    int end_line;
} cbm_changed_hunk_t;

/* Parse `git diff --unified=0` output into per-hunk (path, start_line,
 * end_line) entries — end_line is the last new-side line the hunk touches.
 * Returns count written to out (capped at max_out). */
int cbm_parse_hunks(const char *output, cbm_changed_hunk_t *out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* CBM_PIPELINE_H */
