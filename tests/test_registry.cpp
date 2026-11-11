/*
 * test_registry.c — Tests for function registry and FQN helpers.
 *
 * RED phase: Define expected behavior for symbol registration,
 * resolution strategies, and qualified name computation.
 */
#include "test_framework.h"
#include "pipeline/pipeline.h"
#include "cbm.h"
#include "pipeline/pipeline_internal.h" /* cbm_python_import_binding_contradicts (#2127) */

#include <stdlib.h>
#include <string.h>

/* ── FQN computation ──────────────────────────────────────────────── */

TEST(fqn_simple) {
    char *qn = cbm_pipeline_fqn_compute("myproj", "cmd/server/main.go", "HandleRequest");
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "myproj.cmd.server.main.HandleRequest");
    free(qn);
    PASS();
}

TEST(fqn_no_name) {
    char *qn = cbm_pipeline_fqn_compute("myproj", "pkg/service.go", NULL);
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "myproj.pkg.service");
    free(qn);
    PASS();
}

TEST(fqn_python_init) {
    /* __init__.py should be stripped */
    char *qn = cbm_pipeline_fqn_compute("myproj", "pkg/__init__.py", "Foo");
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "myproj.pkg.Foo");
    free(qn);
    PASS();
}

TEST(fqn_js_index) {
    /* index.js should be stripped */
    char *qn = cbm_pipeline_fqn_compute("myproj", "src/index.ts", "App");
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "myproj.src.App");
    free(qn);
    PASS();
}

TEST(fqn_module) {
    char *qn = cbm_pipeline_fqn_module("myproj", "cmd/server/main.go");
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "myproj.cmd.server.main");
    free(qn);
    PASS();
}

TEST(fqn_folder) {
    char *qn = cbm_pipeline_fqn_folder("myproj", "cmd/server");
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "myproj.cmd.server");
    free(qn);
    PASS();
}

TEST(fqn_root_file) {
    char *qn = cbm_pipeline_fqn_compute("proj", "main.go", "main");
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "proj.main.main");
    free(qn);
    PASS();
}

/* ── FQN collision regression tests ──────────────────────────────── */
/* Bug: __init__.py Module QN collided with Folder QN, causing Folder
 * nodes to be overwritten during extraction. Symbols inside __init__.py
 * must still get clean package QNs (no __init__ in their QN). */

TEST(fqn_init_module_distinct_from_folder) {
    /* Module QN for __init__.py must differ from Folder QN for same dir */
    char *mod_qn = cbm_pipeline_fqn_module("proj", "pkg/__init__.py");
    char *folder_qn = cbm_pipeline_fqn_folder("proj", "pkg");
    ASSERT_NOT_NULL(mod_qn);
    ASSERT_NOT_NULL(folder_qn);
    /* These MUST be different — the old bug was they were both "proj.pkg" */
    ASSERT_STR_NEQ(mod_qn, folder_qn);
    /* Module should contain __init__ as disambiguator */
    ASSERT_NOT_NULL(strstr(mod_qn, "__init__"));
    /* Folder should NOT contain __init__ */
    ASSERT_EQ(strstr(folder_qn, "__init__"), NULL);
    free(mod_qn);
    free(folder_qn);
    PASS();
}

TEST(fqn_init_nested_module_distinct) {
    /* Same collision test for deeply nested __init__.py */
    char *mod_qn =
        cbm_pipeline_fqn_module("proj", "docker-images/cloud-runs/bq-sync-api/__init__.py");
    char *folder_qn = cbm_pipeline_fqn_folder("proj", "docker-images/cloud-runs/bq-sync-api");
    ASSERT_NOT_NULL(mod_qn);
    ASSERT_NOT_NULL(folder_qn);
    ASSERT_STR_NEQ(mod_qn, folder_qn);
    free(mod_qn);
    free(folder_qn);
    PASS();
}

TEST(fqn_index_ts_module_distinct_from_folder) {
    /* Same collision for JS/TS index.ts */
    char *mod_qn = cbm_pipeline_fqn_module("proj", "src/components/index.ts");
    char *folder_qn = cbm_pipeline_fqn_folder("proj", "src/components");
    ASSERT_NOT_NULL(mod_qn);
    ASSERT_NOT_NULL(folder_qn);
    ASSERT_STR_NEQ(mod_qn, folder_qn);
    free(mod_qn);
    free(folder_qn);
    PASS();
}

TEST(fqn_init_symbols_get_clean_package_qn) {
    /* Symbols inside __init__.py must NOT have __init__ in their QN.
     * "proj.pkg.Foo" not "proj.pkg.__init__.Foo" */
    char *sym_qn = cbm_pipeline_fqn_compute("proj", "pkg/__init__.py", "Foo");
    ASSERT_NOT_NULL(sym_qn);
    ASSERT_STR_EQ(sym_qn, "proj.pkg.Foo");
    ASSERT_EQ(strstr(sym_qn, "__init__"), NULL);
    free(sym_qn);
    PASS();
}

TEST(fqn_index_symbols_get_clean_qn) {
    /* Symbols inside index.ts must NOT have index in their QN */
    char *sym_qn = cbm_pipeline_fqn_compute("proj", "src/index.ts", "App");
    ASSERT_NOT_NULL(sym_qn);
    ASSERT_STR_EQ(sym_qn, "proj.src.App");
    free(sym_qn);
    PASS();
}

TEST(fqn_init_file_node_distinct) {
    /* File node QN (name="__file__") for __init__.py must be distinct from Folder */
    char *file_qn = cbm_pipeline_fqn_compute("proj", "pkg/__init__.py", "__file__");
    char *folder_qn = cbm_pipeline_fqn_folder("proj", "pkg");
    ASSERT_NOT_NULL(file_qn);
    ASSERT_NOT_NULL(folder_qn);
    ASSERT_STR_NEQ(file_qn, folder_qn);
    free(file_qn);
    free(folder_qn);
    PASS();
}

TEST(fqn_regular_module_unchanged) {
    /* Non-init modules should be unaffected by the fix */
    char *qn = cbm_pipeline_fqn_module("proj", "pkg/utils.py");
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "proj.pkg.utils");
    free(qn);
    PASS();
}

TEST(project_name_from_path) {
    char *name = cbm_project_name_from_path("/Users/dev/project");
    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name, "Users-dev-project");
    free(name);
    PASS();
}

TEST(project_name_from_root) {
    char *name = cbm_project_name_from_path("/");
    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name, "root");
    free(name);
    PASS();
}

/* ── Registry lifecycle ───────────────────────────────────────────── */

TEST(registry_create_free) {
    cbm_registry_t *r = cbm_registry_new();
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(cbm_registry_size(r), 0);
    cbm_registry_free(r);
    PASS();
}

TEST(registry_name_lookup_keeps_overload_family_only) {
    auto *registry = cbm_registry_new();
    ASSERT_NOT_NULL(registry);
    cbm_registry_add(registry, "blend", "p.api.blend@overload_10", "Function");
    cbm_registry_add(registry, "blend", "p.api.blend@overload_50", "Function");
    cbm_registry_add(registry, "blend", "p.api.blend", "OverloadSet");
    ASSERT_EQ(cbm_registry_size(registry), 1);
    ASSERT_TRUE(cbm_registry_exists(registry, "p.api.blend"));
    ASSERT_STR_EQ(cbm_registry_label_of(registry, "p.api.blend"), "OverloadSet");
    auto resolved = cbm_registry_resolve(registry, "blend", "p.api", nullptr, nullptr, 0);
    ASSERT_STR_EQ(resolved.qualified_name, "p.api.blend");
    cbm_registry_free(registry);
    PASS();
}

TEST(registry_free_null) {
    cbm_registry_free(NULL); /* should not crash */
    PASS();
}

TEST(registry_add_and_exists) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "main", "proj.cmd.main", "Function");
    ASSERT_EQ(cbm_registry_size(r), 1);
    ASSERT_TRUE(cbm_registry_exists(r, "proj.cmd.main"));
    ASSERT_FALSE(cbm_registry_exists(r, "proj.cmd.other"));
    cbm_registry_free(r);
    PASS();
}

TEST(registry_label_of) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Foo", "proj.pkg.Foo", "Class");
    cbm_registry_add(r, "bar", "proj.pkg.bar", "Function");

    ASSERT_STR_EQ(cbm_registry_label_of(r, "proj.pkg.Foo"), "Class");
    ASSERT_STR_EQ(cbm_registry_label_of(r, "proj.pkg.bar"), "Function");
    ASSERT_NULL(cbm_registry_label_of(r, "proj.pkg.baz"));

    cbm_registry_free(r);
    PASS();
}

TEST(registry_find_by_name) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "main", "proj.cmd.main", "Function");
    cbm_registry_add(r, "main", "proj.srv.main", "Function");
    cbm_registry_add(r, "helper", "proj.util.helper", "Function");

    const char **out = NULL;
    int count = 0;
    cbm_registry_find_by_name(r, "main", &out, &count);
    ASSERT_EQ(count, 2);

    cbm_registry_find_by_name(r, "helper", &out, &count);
    ASSERT_EQ(count, 1);

    cbm_registry_find_by_name(r, "nonexistent", &out, &count);
    ASSERT_EQ(count, 0);

    cbm_registry_free(r);
    PASS();
}

TEST(registry_no_duplicates) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "main", "proj.cmd.main", "Function");
    cbm_registry_add(r, "main", "proj.cmd.main", "Function"); /* duplicate */
    ASSERT_EQ(cbm_registry_size(r), 1);

    const char **out = NULL;
    int count = 0;
    cbm_registry_find_by_name(r, "main", &out, &count);
    ASSERT_EQ(count, 1); /* no duplicate in byName list */

    cbm_registry_free(r);
    PASS();
}

/* ── Resolution strategies ────────────────────────────────────────── */

TEST(resolve_same_module) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "helper", "proj.pkg.service.helper", "Function");

    /* Call "helper" from the same module → should resolve */
    cbm_resolution_t res = cbm_registry_resolve(r, "helper", "proj.pkg.service", NULL, NULL, 0);
    ASSERT_STR_EQ(res.qualified_name, "proj.pkg.service.helper");
    ASSERT_STR_EQ(res.strategy, "same_module");
    ASSERT_TRUE(res.confidence >= 0.85);

    cbm_registry_free(r);
    PASS();
}

TEST(resolve_import_map) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Process", "proj.pkg.worker.Process", "Function");

    /* Import map: "worker" → "proj.pkg.worker" */
    const char *keys[] = {"worker"};
    const char *vals[] = {"proj.pkg.worker"};

    /* Call "worker.Process" → should resolve via import map */
    cbm_resolution_t res =
        cbm_registry_resolve(r, "worker.Process", "proj.cmd.main", keys, vals, 1);
    ASSERT_STR_EQ(res.qualified_name, "proj.pkg.worker.Process");
    ASSERT_STR_EQ(res.strategy, "import_map");
    ASSERT_TRUE(res.confidence >= 0.90);

    cbm_registry_free(r);
    PASS();
}

/* Bare function call (no dot) routed through import_map. The candidate QN
 * must be module_qn.callee, not module_qn — otherwise lookups fall through
 * to name-based resolution and pick a same-named function from a different
 * file. Regression for the @/lib/auth-style import case. */
TEST(resolve_import_map_bare_function) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "requireAdmin", "proj.lib.authorization.requireAdmin", "Function");
    /* Same name in another module — without the fix this is what gets picked. */
    cbm_registry_add(r, "requireAdmin", "proj.lib.users.requireAdmin", "Function");

    const char *keys[] = {"requireAdmin"};
    const char *vals[] = {"proj.lib.authorization"};

    cbm_resolution_t res =
        cbm_registry_resolve(r, "requireAdmin", "proj.actions.settings", keys, vals, 1);
    ASSERT_STR_EQ(res.qualified_name, "proj.lib.authorization.requireAdmin");
    ASSERT_STR_EQ(res.strategy, "import_map");

    cbm_registry_free(r);
    PASS();
}

TEST(resolve_unique_name) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "UniqueFunc", "proj.deep.path.UniqueFunc", "Function");

    /* Call "UniqueFunc" — only one candidate project-wide */
    cbm_resolution_t res =
        cbm_registry_resolve(r, "UniqueFunc", "proj.other.module", NULL, NULL, 0);
    ASSERT_STR_EQ(res.qualified_name, "proj.deep.path.UniqueFunc");
    ASSERT_STR_EQ(res.strategy, "unique_name");

    cbm_registry_free(r);
    PASS();
}

TEST(resolve_unresolved) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "foo", "proj.pkg.foo", "Function");

    /* Call "nonexistent" — not in registry */
    cbm_resolution_t res = cbm_registry_resolve(r, "nonexistent", "proj.other", NULL, NULL, 0);
    ASSERT_TRUE(res.qualified_name == NULL || res.qualified_name[0] == '\0');

    cbm_registry_free(r);
    PASS();
}

TEST(resolve_many_nodes) {
    cbm_registry_t *r = cbm_registry_new();
    /* Add 500 functions */
    for (int i = 0; i < 500; i++) {
        char name[32], qn[64];
        snprintf(name, sizeof(name), "func_%d", i);
        snprintf(qn, sizeof(qn), "proj.pkg.func_%d", i);
        cbm_registry_add(r, name, qn, "Function");
    }
    ASSERT_EQ(cbm_registry_size(r), 500);

    /* Resolve one */
    cbm_resolution_t res = cbm_registry_resolve(r, "func_250", "proj.pkg", NULL, NULL, 0);
    ASSERT_STR_EQ(res.qualified_name, "proj.pkg.func_250");

    cbm_registry_free(r);
    PASS();
}

/* ── Confidence band ───────────────────────────────────────────── */

TEST(confidence_band_high) {
    ASSERT_STR_EQ(cbm_confidence_band(0.95), "high");
    ASSERT_STR_EQ(cbm_confidence_band(0.70), "high");
    PASS();
}

TEST(confidence_band_medium) {
    ASSERT_STR_EQ(cbm_confidence_band(0.55), "medium");
    ASSERT_STR_EQ(cbm_confidence_band(0.45), "medium");
    PASS();
}

TEST(confidence_band_speculative) {
    ASSERT_STR_EQ(cbm_confidence_band(0.40), "speculative");
    ASSERT_STR_EQ(cbm_confidence_band(0.25), "speculative");
    ASSERT_STR_EQ(cbm_confidence_band(0.20), "");
    ASSERT_STR_EQ(cbm_confidence_band(0.0), "");
    PASS();
}

/* ── Suffix match resolution ──────────────────────────────────── */

TEST(resolve_suffix_match) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Process", "proj.svcA.Process", "Function");
    cbm_registry_add(r, "Process", "proj.svcB.Process", "Function");

    /* Caller in svcA — should prefer svcA via import distance */
    cbm_resolution_t res = cbm_registry_resolve(r, "Process", "proj.svcA.caller", NULL, NULL, 0);
    ASSERT_STR_EQ(res.qualified_name, "proj.svcA.Process");
    ASSERT_STR_EQ(res.strategy, "suffix_match");
    ASSERT_TRUE(res.confidence >= 0.50 && res.confidence <= 0.60);

    cbm_registry_free(r);
    PASS();
}

/* A name with more than REG_MAX_CANDIDATES (256) registered definitions is
 * unresolvable by name alone: the candidate penalty floors its confidence to
 * ~3/count (noise), while walking the candidate array per file dominated
 * usage-resolution CPU on the Linux kernel ("flags"/"dev"/"list_head" have
 * 4-7k definitions each). resolve must bail out with an empty result instead
 * of scanning and emitting a near-zero-confidence edge. */
TEST(resolve_caps_unresolvably_ambiguous_names) {
    cbm_registry_t *r = cbm_registry_new();
    for (int i = 0; i < 300; i++) {
        char qn[64];
        snprintf(qn, sizeof(qn), "proj.mod%d.flags", i);
        cbm_registry_add(r, "flags", qn, "Variable");
    }
    cbm_resolution_t res = cbm_registry_resolve(r, "flags", "proj.other.caller", NULL, NULL, 0);
    ASSERT_TRUE(res.qualified_name == NULL || res.qualified_name[0] == '\0');

    /* Same-module resolution still wins regardless of candidate count. */
    res = cbm_registry_resolve(r, "flags", "proj.mod7", NULL, NULL, 0);
    ASSERT_STR_EQ(res.qualified_name, "proj.mod7.flags");
    ASSERT_STR_EQ(res.strategy, "same_module");

    cbm_registry_free(r);
    PASS();
}

/* ── Import map suffix resolution ─────────────────────────────── */

/* `from M import execute as bridge_execute` + `bridge_execute()` — the IMPORTS
 * target is ALREADY the def QN, so the suffix-less direct-hit path must return
 * it rather than appending the local alias onto the module. Adversarial pin: the
 * behaviour is already correct here, and this is what keeps it that way while
 * the pkgmap/pass_calls side of the aliased-from-import fix moves around it. */
/* #725: two same-named symbols in different languages. suffix_match is the
 * strategy that collapses them onto one winner by import distance, so a Python
 * Store.commit() call lands on a JS `commit` and get_architecture reports a
 * hotspot whose in-degree came from another language. unique_name
 * (candidates == 1) is a different strategy and must survive, as must
 * same_module / import_map / lsp_* — those have real evidence behind them. */
TEST(cross_language_ref_drops_go_vs_c) {
    /* #1928: the USAGE/WRITES/READS analog of #725. Reference edges carry no
     * import-closure evidence, so EVERY registry strategy is a bare-name
     * guess -- the predicate takes no strategy at all. */
    ASSERT_TRUE(cbm_suppress_cross_language_ref(CBM_LANG_GO, "bpf/probe.c"));
    ASSERT_TRUE(cbm_suppress_cross_language_ref(CBM_LANG_GO, "driver/mock.hpp"));
    ASSERT_TRUE(cbm_suppress_cross_language_ref(CBM_LANG_C, "pkg/events/event.go"));
    ASSERT_TRUE(cbm_suppress_cross_language_ref(CBM_LANG_PYTHON, "web/src/pages/Editor.js"));
    /* Same language -> keep. */
    ASSERT_FALSE(cbm_suppress_cross_language_ref(CBM_LANG_GO, "pkg/state/state.go"));
    /* C and C++ are one family: .h maps to CBM_LANG_CPP. */
    ASSERT_FALSE(cbm_suppress_cross_language_ref(CBM_LANG_C, "bpf/probe.h"));
    ASSERT_FALSE(cbm_suppress_cross_language_ref(CBM_LANG_CPP, "driver/compat.c"));
    /* ...but Go into a header is still a boundary. */
    ASSERT_TRUE(cbm_suppress_cross_language_ref(CBM_LANG_GO, "bpf/probe.h"));
    /* JS/TS/TSX are one family. */
    ASSERT_FALSE(cbm_suppress_cross_language_ref(CBM_LANG_JAVASCRIPT, "lib/util.ts"));
    ASSERT_FALSE(cbm_suppress_cross_language_ref(CBM_LANG_TYPESCRIPT, "ui/Panel.tsx"));
    /* Unknown caller/target language or no path -> nothing to judge -> keep. */
    ASSERT_FALSE(cbm_suppress_cross_language_ref(CBM_LANG_COUNT, "store.py"));
    ASSERT_FALSE(cbm_suppress_cross_language_ref(CBM_LANG_GO, NULL));
    ASSERT_FALSE(cbm_suppress_cross_language_ref(CBM_LANG_GO, ""));
    ASSERT_FALSE(cbm_suppress_cross_language_ref(CBM_LANG_GO, "Makefile.inc.unknownext"));
    PASS();
}

TEST(go_bare_ref_never_binds_field) {
    /* #1942/#1962: a bare Go identifier can never denote a struct field. The
     * extractor strips the receiver before the resolver runs, so the
     * selector-vs-bare distinction arrives as the recorded is_member_access
     * signal, never as a dot in the reference text. */
    ASSERT_TRUE(cbm_suppress_bare_field_ref(CBM_LANG_GO, false, "Field"));
    /* The member half of a selector may bind a field. */
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_GO, true, "Field"));
    /* Bare references to non-fields are untouched. */
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_GO, false, "Variable"));
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_GO, false, "Function"));
    /* Other languages reference their own members bare inside methods. */
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_CSHARP, false, "Field"));
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_JAVA, false, "Field"));
    /* Degenerate input -> nothing to judge. */
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_GO, false, NULL));
    PASS();
}

TEST(c_bare_ref_never_binds_field) {
    /* #2666: a C struct member is named only as the member half of `a.b`,
     * `a->b` or a designator `.b` (field_identifier -> is_member_access). A
     * bare C identifier -- a macro argument like RUN_SELECTED_SUITE(mcp) -- is
     * never one, so it must not bind a same-named Field. */
    ASSERT_TRUE(cbm_suppress_bare_field_ref(CBM_LANG_C, false, "Field"));
    /* The member half still binds. */
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_C, true, "Field"));
    /* A bare C name keeps binding globals, functions and types. */
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_C, false, "Variable"));
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_C, false, "Function"));
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_C, false, "Type"));
    /* C++ (and every `.h`, parsed as C++): a method body names its own
     * members bare, so the bind is kept. */
    ASSERT_FALSE(cbm_suppress_bare_field_ref(CBM_LANG_CPP, false, "Field"));
    PASS();
}

TEST(go_textual_call_never_binds_field) {
    /* Textual strategies cannot bind a Go call to a struct Field. */
    ASSERT_TRUE(cbm_go_suppress_textual_field_call(true, "Field", "unique_name"));
    ASSERT_TRUE(cbm_go_suppress_textual_field_call(true, "Field", "suffix_match"));
    ASSERT_TRUE(cbm_go_suppress_textual_field_call(true, "Field", "field_type_hint"));
    ASSERT_TRUE(cbm_go_suppress_textual_field_call(true, "Field", "same_module"));
    ASSERT_TRUE(cbm_go_suppress_textual_field_call(true, "Field", NULL));
    /* The type-aware LSP may (a call through a func-typed field). */
    ASSERT_FALSE(cbm_go_suppress_textual_field_call(true, "Field", "lsp_type_dispatch"));
    /* Non-field targets and other languages are untouched. */
    ASSERT_FALSE(cbm_go_suppress_textual_field_call(true, "Method", "unique_name"));
    ASSERT_FALSE(cbm_go_suppress_textual_field_call(true, NULL, "unique_name"));
    ASSERT_FALSE(cbm_go_suppress_textual_field_call(false, "Field", "unique_name"));
    PASS();
}

TEST(cross_language_suffix_match_drops_py_vs_js) {
    ASSERT_TRUE(cbm_suppress_cross_language_suffix_match(CBM_LANG_PYTHON, "web/src/pages/Editor.js",
                                                         "suffix_match"));
    ASSERT_TRUE(
        cbm_suppress_cross_language_suffix_match(CBM_LANG_JAVASCRIPT, "store.py", "suffix_match"));
    ASSERT_TRUE(
        cbm_suppress_cross_language_suffix_match(CBM_LANG_BASH, "cli/main.py", "suffix_match"));
    /* Same language: keep. */
    ASSERT_FALSE(
        cbm_suppress_cross_language_suffix_match(CBM_LANG_PYTHON, "store.py", "suffix_match"));
    /* Other strategies: keep, whatever the languages. */
    ASSERT_FALSE(cbm_suppress_cross_language_suffix_match(
        CBM_LANG_PYTHON, "web/src/pages/Editor.js", "unique_name"));
    ASSERT_FALSE(cbm_suppress_cross_language_suffix_match(
        CBM_LANG_PYTHON, "web/src/pages/Editor.js", "same_module"));
    ASSERT_FALSE(cbm_suppress_cross_language_suffix_match(CBM_LANG_PYTHON,
                                                          "web/src/pages/Editor.js", "import_map"));
    /* JS/TS/TSX are ONE family. */
    ASSERT_FALSE(cbm_suppress_cross_language_suffix_match(CBM_LANG_JAVASCRIPT, "lib/util.ts",
                                                          "suffix_match"));
    ASSERT_FALSE(cbm_suppress_cross_language_suffix_match(CBM_LANG_TYPESCRIPT, "ui/Panel.tsx",
                                                          "suffix_match"));
    /* Unknown either side: keep — never guess. */
    ASSERT_FALSE(cbm_suppress_cross_language_suffix_match(CBM_LANG_PYTHON, NULL, "suffix_match"));
    ASSERT_FALSE(
        cbm_suppress_cross_language_suffix_match(CBM_LANG_COUNT, "store.py", "suffix_match"));
    PASS();
}

TEST(resolve_import_map_aliased_from_import) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "execute", "proj.services.satori_bridge.gate.execute", "Function");
    /* The alias ghost must not win even if something registered it. */
    cbm_registry_add(r, "bridge_execute", "proj.services.satori_bridge.gate.bridge_execute",
                     "Function");

    const char *keys[] = {"bridge_execute"};
    const char *vals[] = {"proj.services.satori_bridge.gate.execute"};

    cbm_resolution_t res =
        cbm_registry_resolve(r, "bridge_execute", "proj.services.yui_core.router", keys, vals, 1);
    ASSERT_STR_EQ(res.qualified_name, "proj.services.satori_bridge.gate.execute");
    ASSERT_STR_EQ(res.strategy, "import_map");

    cbm_registry_free(r);
    PASS();
}

TEST(resolve_import_map_suffix) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Foo", "proj.other.sub.Foo", "Function");

    const char *keys[] = {"other"};
    const char *vals[] = {"proj.other"};

    /* "other.Foo" → exact "proj.other.Foo" not found →
     * suffix scan finds "proj.other.sub.Foo" */
    cbm_resolution_t res = cbm_registry_resolve(r, "other.Foo", "proj.pkg", keys, vals, 1);
    ASSERT_STR_EQ(res.qualified_name, "proj.other.sub.Foo");
    ASSERT_STR_EQ(res.strategy, "import_map_suffix");
    ASSERT_TRUE(res.confidence >= 0.80 && res.confidence <= 0.90);

    cbm_registry_free(r);
    PASS();
}

/* ── Import reachability tests ────────────────────────────────── */

TEST(resolve_is_import_reachable) {
    /* Test import reachability through unique_name confidence penalty.
     * is_import_reachable is static, so we test it indirectly. */
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Helper", "proj.shared.utils.Helper", "Function");

    /* With import covering the module → full confidence */
    const char *keys1[] = {"utils"};
    const char *vals1[] = {"proj.shared.utils"};
    cbm_resolution_t res = cbm_registry_resolve(r, "Helper", "proj.caller", keys1, vals1, 1);
    ASSERT_STR_EQ(res.strategy, "unique_name");
    ASSERT_TRUE(res.confidence >= 0.70); /* 0.75, not halved */

    /* With import NOT covering the module → halved */
    const char *keys2[] = {"other"};
    const char *vals2[] = {"proj.other"};
    res = cbm_registry_resolve(r, "Helper", "proj.caller", keys2, vals2, 1);
    ASSERT_STR_EQ(res.strategy, "unique_name");
    ASSERT_TRUE(res.confidence <= 0.40); /* 0.75 * 0.5 = 0.375 */

    cbm_registry_free(r);
    PASS();
}

TEST(resolve_import_reachable_prefix) {
    /* "proj.handler.sub.Process" should be reachable via import "proj.handler" */
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Process", "proj.handler.sub.Process", "Function");

    const char *keys[] = {"handler"};
    const char *vals[] = {"proj.handler"};
    cbm_resolution_t res = cbm_registry_resolve(r, "Process", "proj.caller", keys, vals, 1);
    ASSERT_STR_EQ(res.strategy, "unique_name");
    ASSERT_TRUE(res.confidence >= 0.70); /* reachable → no penalty */

    cbm_registry_free(r);
    PASS();
}

/* ── Negative import evidence ─────────────────────────────────── */

TEST(negative_import_rejects_unimported) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Process", "proj.billing.Process", "Function");
    cbm_registry_add(r, "Process", "proj.handler.Process", "Function");

    /* Import only handler's module — suffix_match should prefer handler */
    const char *keys[] = {"handler"};
    const char *vals[] = {"proj.handler"};
    cbm_resolution_t res = cbm_registry_resolve(r, "Process", "proj.caller", keys, vals, 1);
    ASSERT_STR_EQ(res.qualified_name, "proj.handler.Process");

    cbm_registry_free(r);
    PASS();
}

/* ── Fuzzy resolve ────────────────────────────────────────────── */

TEST(fuzzy_resolve_single_candidate) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "CreateOrder", "svcA.handlers.CreateOrder", "Function");
    cbm_registry_add(r, "ValidateOrder", "svcB.validators.ValidateOrder", "Function");

    /* FuzzyResolve should find by simple name even with unknown prefix */
    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(r, "unknownPkg.CreateOrder", "svcC.caller", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_STR_EQ(fr.result.qualified_name, "svcA.handlers.CreateOrder");

    cbm_registry_free(r);
    PASS();
}

TEST(fuzzy_resolve_nonexistent) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "CreateOrder", "svcA.handlers.CreateOrder", "Function");

    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(r, "NonExistent", "svcC.caller", NULL, NULL, 0);
    ASSERT_FALSE(fr.ok);

    cbm_registry_free(r);
    PASS();
}

TEST(fuzzy_resolve_multiple_best_by_distance) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Process", "svcA.handlers.Process", "Function");
    cbm_registry_add(r, "Process", "svcB.handlers.Process", "Function");

    /* Caller in svcA — should prefer svcA */
    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(r, "unknown.Process", "svcA.other", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_STR_EQ(fr.result.qualified_name, "svcA.handlers.Process");

    /* Caller in svcB — should prefer svcB */
    fr = cbm_registry_fuzzy_resolve(r, "unknown.Process", "svcB.other", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_STR_EQ(fr.result.qualified_name, "svcB.handlers.Process");

    cbm_registry_free(r);
    PASS();
}

TEST(fuzzy_resolve_deep_name_extraction) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "DoWork", "myproject.utils.DoWork", "Function");

    /* Deeply qualified callee — should extract "DoWork" */
    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(r, "some.deep.module.DoWork", "myproject.caller", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_STR_EQ(fr.result.qualified_name, "myproject.utils.DoWork");

    cbm_registry_free(r);
    PASS();
}

TEST(fuzzy_resolve_empty_registry) {
    cbm_registry_t *r = cbm_registry_new();

    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(r, "SomeFunc", "myproject.caller", NULL, NULL, 0);
    ASSERT_FALSE(fr.ok);

    cbm_registry_free(r);
    PASS();
}

TEST(fuzzy_resolve_confidence_single) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Handler", "proj.svc.Handler", "Function");

    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(r, "unknownPkg.Handler", "proj.caller", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_TRUE(fr.result.confidence >= 0.35 && fr.result.confidence <= 0.45);
    ASSERT_STR_EQ(fr.result.strategy, "fuzzy");

    cbm_registry_free(r);
    PASS();
}

TEST(fuzzy_resolve_confidence_distance) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Process", "proj.svcA.Process", "Function");
    cbm_registry_add(r, "Process", "proj.svcB.Process", "Function");

    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(r, "unknownPkg.Process", "proj.svcA.other", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_TRUE(fr.result.confidence >= 0.25 && fr.result.confidence <= 0.35);
    ASSERT_STR_EQ(fr.result.strategy, "fuzzy");

    cbm_registry_free(r);
    PASS();
}

TEST(fuzzy_penalty_unreachable_import) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Handler", "proj.billing.Handler", "Function");

    /* Import for different module → confidence halved */
    const char *keys[] = {"other"};
    const char *vals[] = {"proj.other"};
    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(r, "unknown.Handler", "proj.caller", keys, vals, 1);
    ASSERT_TRUE(fr.ok);
    /* 0.40 * 0.5 = 0.20 */
    ASSERT_TRUE(fr.result.confidence >= 0.15 && fr.result.confidence <= 0.25);

    cbm_registry_free(r);
    PASS();
}

TEST(fuzzy_no_import_map_passthrough) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Handler", "proj.billing.Handler", "Function");

    /* nil import map → full confidence */
    cbm_fuzzy_result_t fr =
        cbm_registry_fuzzy_resolve(r, "unknown.Handler", "proj.caller", NULL, NULL, 0);
    ASSERT_TRUE(fr.ok);
    ASSERT_TRUE(fr.result.confidence >= 0.35 && fr.result.confidence <= 0.45);

    cbm_registry_free(r);
    PASS();
}

TEST(dynamic_suppress_drops_weak_method_matches) {
    /* #592/#606/#1276: a member call whose receiver the LSP could not type, that
     * landed via a WEAK short-name strategy, is generic-resolver noise → drop.
     * "fuzzy" is covered defensively (cbm_registry_fuzzy_resolve is not wired
     * into the resolvers today) so a future wiring cannot silently reintroduce it. */
    ASSERT_TRUE(cbm_suppress_weak_member_match(true, true, "suffix_match"));
    ASSERT_TRUE(cbm_suppress_weak_member_match(true, true, "unique_name"));
    ASSERT_TRUE(cbm_suppress_weak_member_match(true, true, "field_type_hint"));
    ASSERT_TRUE(cbm_suppress_weak_member_match(true, true, "fuzzy"));
    PASS();
}

TEST(dynamic_suppress_keeps_high_confidence_and_non_methods) {
    /* Keep every receiver-/import-aware strategy. The PARALLEL resolver runs
     * lsp_* strategies through this same guard variable, so an explicit
     * drop-list must never touch them. */
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, "same_module"));
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, "import_map"));
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, "import_map_suffix"));
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, "qualified_suffix"));
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, "callee_suffix"));
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, "service_pattern"));
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, "lsp_ts_method"));
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, "lsp_cross"));
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, "lsp_ts_local"));
    /* A bare call (is_method=false) is a free-function call → never suppressed. */
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, false, "unique_name"));
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, false, "suffix_match"));
    /* Languages outside the caller's guard set are never affected. */
    ASSERT_FALSE(cbm_suppress_weak_member_match(false, true, "suffix_match"));
    /* No match (NULL/empty strategy) → nothing to suppress. */
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, NULL));
    ASSERT_FALSE(cbm_suppress_weak_member_match(true, true, ""));
    PASS();
}

/* Upstream's kernel probe: a Makefile target bound the C function
 * `sk_psock.eval` through unique_name. Build and configuration languages have
 * no cross-language call semantics, so for them a unique match into another
 * language is a collision by construction; a code caller keeps #1572. */
TEST(cross_language_config_caller_drops_unique_name_too) {
    ASSERT_TRUE(cbm_suppress_cross_language_suffix_match(CBM_LANG_MAKEFILE, "include/linux/skmsg.h",
                                                         "unique_name"));
    ASSERT_TRUE(
        cbm_suppress_cross_language_suffix_match(CBM_LANG_CMAKE, "src/main.c", "unique_name"));
    ASSERT_TRUE(
        cbm_suppress_cross_language_suffix_match(CBM_LANG_YAML, "app/models.py", "unique_name"));
    /* A code caller's unique_name into another language is still #1572. */
    ASSERT_FALSE(cbm_suppress_cross_language_suffix_match(
        CBM_LANG_PYTHON, "web/src/pages/Editor.js", "unique_name"));
    /* Same-language config targets are untouched. */
    ASSERT_FALSE(
        cbm_suppress_cross_language_suffix_match(CBM_LANG_MAKEFILE, "lib/Makefile", "unique_name"));
    /* Receiver-aware strategies are never this guard's business. */
    ASSERT_FALSE(
        cbm_suppress_cross_language_suffix_match(CBM_LANG_MAKEFILE, "src/main.c", "same_module"));
    PASS();
}

/* Same-named candidates that tie on test-status and namespace proximity are
 * settled by the candidates themselves: the more plainly callable candidate
 * (the function over the fields), then the smaller QN, whichever order the
 * registry was built in (O9). */
TEST(registry_tie_break_is_independent_of_registration_order) {
    const char *cands[] = {
        "proj.drivers.media.cec.i2c.ch7322.ch7322_conn_match.dev_name", /* struct field */
        "proj.sound.soc.codecs.tas2783-sdw.tas2783_prv.dev_name",       /* struct field */
        "proj.include.linux.device.dev_name",                           /* the function */
        "proj.arch.um.drivers.pty.pty_chan.dev_name",                   /* struct field */
    };
    const char *labels[] = {"Field", "Field", "Function", "Field"};
    const int n = 4;

    cbm_registry_t *forward = cbm_registry_new();
    cbm_registry_t *backward = cbm_registry_new();
    for (int i = 0; i < n; i++) {
        cbm_registry_add(forward, "dev_name", cands[i], labels[i]);
        cbm_registry_add(backward, "dev_name", cands[n - 1 - i], labels[n - 1 - i]);
    }

    /* A far-away caller with no imports: every candidate scores the same. */
    cbm_resolution_t f =
        cbm_registry_resolve(forward, "dev_name", "proj.kernel.irq.msi", NULL, NULL, 0);
    cbm_resolution_t b =
        cbm_registry_resolve(backward, "dev_name", "proj.kernel.irq.msi", NULL, NULL, 0);
    ASSERT_NOT_NULL(f.qualified_name);
    ASSERT_NOT_NULL(b.qualified_name);
    ASSERT_STR_EQ(f.qualified_name, "proj.include.linux.device.dev_name");
    ASSERT_STR_EQ(b.qualified_name, "proj.include.linux.device.dev_name");
    ASSERT_STR_EQ(f.strategy, "suffix_match");
    ASSERT_STR_EQ(b.strategy, "suffix_match");

    /* Equal kind: the smaller QN, from either order. */
    cbm_registry_t *lex_a = cbm_registry_new();
    cbm_registry_t *lex_b = cbm_registry_new();
    cbm_registry_add(lex_a, "sg_next", "proj.tools.virtio.scatterlist.sg_next", "Function");
    cbm_registry_add(lex_a, "sg_next", "proj.include.linux.scatterlist.sg_next", "Function");
    cbm_registry_add(lex_b, "sg_next", "proj.include.linux.scatterlist.sg_next", "Function");
    cbm_registry_add(lex_b, "sg_next", "proj.tools.virtio.scatterlist.sg_next", "Function");
    cbm_resolution_t la =
        cbm_registry_resolve(lex_a, "sg_next", "proj.drivers.scsi.arm_scsi", NULL, NULL, 0);
    cbm_resolution_t lb =
        cbm_registry_resolve(lex_b, "sg_next", "proj.drivers.scsi.arm_scsi", NULL, NULL, 0);
    ASSERT_STR_EQ(la.qualified_name, "proj.include.linux.scatterlist.sg_next");
    ASSERT_STR_EQ(lb.qualified_name, "proj.include.linux.scatterlist.sg_next");

    /* Proximity outranks the tie order: the sibling wins over a shallower stranger. */
    cbm_registry_t *near = cbm_registry_new();
    cbm_registry_add(near, "vnic_rq_free", "proj.lib.vnic_rq_free", "Function");
    cbm_registry_add(near, "vnic_rq_free", "proj.drivers.scsi.fnic.vnic_rq.vnic_rq_free",
                     "Function");
    cbm_resolution_t nr = cbm_registry_resolve(near, "vnic_rq_free",
                                               "proj.drivers.scsi.fnic.fnic_res", NULL, NULL, 0);
    ASSERT_STR_EQ(nr.qualified_name, "proj.drivers.scsi.fnic.vnic_rq.vnic_rq_free");

    cbm_registry_free(forward);
    cbm_registry_free(backward);
    cbm_registry_free(lex_a);
    cbm_registry_free(lex_b);
    cbm_registry_free(near);
    PASS();
}

/* A call names something callable. When same-named candidates tie, a method
 * must not lose to a property or field that merely shares its name. Exposed
 * (Kotlin): `cities.selectAll().where { ... }` bound to the property
 * `AbstractQuery.where` instead of the jdbc `Query.where` function, because the
 * two QNs have the same depth and "exposed-core" sorts first (upstream
 * 51364158: 785 -> 1,648 Kotlin CALLS edges onto Variable/Field targets). */
TEST(registry_tie_prefers_callable_over_property) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(
        r, "where",
        "proj.exposed-core.src.main.kotlin.org.jetbrains.exposed.v1.core.AbstractQuery.where",
        "Variable");
    cbm_registry_add(r, "where",
                     "proj.exposed-jdbc.src.main.kotlin.org.jetbrains.exposed.v1.jdbc.Query.where",
                     "Method");
    /* A deeper method still beats a shallower field: kind decides before order. */
    cbm_registry_add(r, "limit", "proj.exposed-core.Table.limit", "Field");
    cbm_registry_add(r, "limit", "proj.exposed-jdbc.src.main.kotlin.jdbc.Query.limit", "Method");

    const char *caller =
        "proj.exposed-tests.src.test.kotlin.org.jetbrains.exposed.v1.tests.SelectTests";
    cbm_resolution_t w = cbm_registry_resolve(r, "where", caller, NULL, NULL, 0);
    cbm_resolution_t l = cbm_registry_resolve(r, "limit", caller, NULL, NULL, 0);
    ASSERT_STR_EQ(w.qualified_name,
                  "proj.exposed-jdbc.src.main.kotlin.org.jetbrains.exposed.v1.jdbc.Query.where");
    ASSERT_STR_EQ(l.qualified_name, "proj.exposed-jdbc.src.main.kotlin.jdbc.Query.limit");
    cbm_registry_free(r);
    PASS();
}

/* Nesting depth says nothing about which same-named method a call means.
 * django: `Model.objects.filter(...)` from tests/ tied between
 * `QuerySet.filter` and the template `Library.filter`; "least nested wins"
 * picked Library (one segment shallower) for most of the ~5,600 `objects.X`
 * calls (upstream: QuerySet share 57 % -> 24 %). Equal-kind ties now fall to
 * the QN order alone, still a pure function of the candidate set (O9). */
TEST(registry_tie_ignores_nesting_depth) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "filter", "proj.django.template.library.Library.filter", "Method");
    cbm_registry_add(r, "filter", "proj.django.db.models.query.QuerySet.filter", "Method");
    cbm_resolution_t f =
        cbm_registry_resolve(r, "filter", "proj.tests.lookup.tests", NULL, NULL, 0);
    ASSERT_STR_EQ(f.qualified_name, "proj.django.db.models.query.QuerySet.filter");
    ASSERT_STR_EQ(f.strategy, "suffix_match");
    cbm_registry_free(r);
    PASS();
}

/* A constructor is named after its type, so a bare type name (`: public
 * Logger`, `Slice key;`) ties between the class and its constructor. Before
 * 51364158 the class won as the shallower QN; ranking every Method above a
 * Class sent the name to the constructor, and resolve_as_class then dropped
 * the base (rocksdb: INHERITS 1794 -> 1650, OVERRIDE 2740 -> 2610, USAGE of
 * classes -9k). A constructor ranks below the type; a real callable still
 * wins over a type, and a constructor in another language (`java...Snapshot.
 * Snapshot`) or out of line (`StatisticsImpl::StatisticsImpl`, whose QN sorts
 * first) does not outrank the class either. */
TEST(registry_tie_prefers_class_over_its_constructor) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "Logger", "proj.include.rocksdb.env.Logger", "Class");
    cbm_registry_add(r, "Logger", "proj.include.rocksdb.env.Logger.Logger", "Method");
    cbm_registry_add(r, "Snapshot", "proj.include.rocksdb.snapshot.Snapshot", "Class");
    cbm_registry_add(r, "Snapshot", "proj.java.src.main.java.org.rocksdb.Snapshot.Snapshot",
                     "Method");
    cbm_registry_add(r, "Slice", "proj.include.rocksdb.slice.Slice", "Class");
    cbm_registry_add(r, "Slice", "proj.include.rocksdb.slice.Slice.Slice", "OverloadSet");
    cbm_registry_add(r, "Slice", "proj.util.slice.Slice.Slice", "Method");
    /* Out of line, the constructor's QN sorts before the class's. */
    cbm_registry_add(r, "StatisticsImpl",
                     "proj.monitoring.statistics.StatisticsImpl.StatisticsImpl", "Method");
    cbm_registry_add(r, "StatisticsImpl", "proj.monitoring.statistics_impl.StatisticsImpl",
                     "Class");
    /* Not a constructor: a function that merely shares a type's name still wins. */
    cbm_registry_add(r, "Key", "proj.db.forward_iterator_bench.Key", "Class");
    cbm_registry_add(r, "Key", "proj.db.range_del_aggregator_bench.Key", "Function");

    const char *caller = "proj.logging.auto_roll_logger";
    ASSERT_STR_EQ(cbm_registry_resolve(r, "Logger", caller, NULL, NULL, 0).qualified_name,
                  "proj.include.rocksdb.env.Logger");
    ASSERT_STR_EQ(cbm_registry_resolve(r, "Snapshot", caller, NULL, NULL, 0).qualified_name,
                  "proj.include.rocksdb.snapshot.Snapshot");
    ASSERT_STR_EQ(cbm_registry_resolve(r, "Slice", caller, NULL, NULL, 0).qualified_name,
                  "proj.include.rocksdb.slice.Slice");
    ASSERT_STR_EQ(cbm_registry_resolve(r, "StatisticsImpl", caller, NULL, NULL, 0).qualified_name,
                  "proj.monitoring.statistics_impl.StatisticsImpl");
    ASSERT_STR_EQ(cbm_registry_resolve(r, "Key", caller, NULL, NULL, 0).qualified_name,
                  "proj.db.range_del_aggregator_bench.Key");
    cbm_registry_free(r);
    PASS();
}

/* .NET reference assemblies (`src/libraries/<Asm>/ref/<Asm>.cs`) declare every
 * public member of an assembly with a `throw null` body in one shallow file.
 * A call that ties between such a stub and the implementation must reach the
 * implementation: upstream measured 14.1 % of dotnet/runtime's CALLS on ref/
 * stubs once ties preferred the shallower, then the smaller ("ref" < "src")
 * QN. The preference is C#'s alone: elsewhere a `ref` directory is an
 * ordinary name. */
TEST(registry_tie_prefers_csharp_implementation_over_ref_stub) {
    cbm_registry_t *r = cbm_registry_new();
    /* Shallower stub vs deeper implementation (MemoryStream). */
    cbm_registry_add_lang(
        r, "MemoryStream",
        "proj.src.libraries.System.Runtime.ref.System.Runtime.MemoryStream.MemoryStream", "Method",
        CBM_LANG_CSHARP);
    cbm_registry_add_lang(
        r, "MemoryStream",
        "proj.src.libraries.System.Private.CoreLib.src.System.IO.MemoryStream.MemoryStream",
        "Method", CBM_LANG_CSHARP);
    /* Equal depth, "ref" sorts before "src" (WithCancellation). */
    cbm_registry_add_lang(r, "WithCancellation",
                          "proj.src.libraries.System.Linq.Parallel.ref.System.Linq."
                          "ParallelEnumerable.WithCancellation",
                          "Method", CBM_LANG_CSHARP);
    cbm_registry_add_lang(r, "WithCancellation",
                          "proj.src.libraries.System.Linq.Parallel.src.System.Linq."
                          "ParallelEnumerable.WithCancellation",
                          "Method", CBM_LANG_CSHARP);
    /* Not C#: a `ref` directory gets no special treatment. */
    cbm_registry_add_lang(r, "resolve", "proj.lib.ref.Resolver.resolve", "Method", CBM_LANG_JAVA);
    cbm_registry_add_lang(r, "resolve", "proj.lib.src.Resolver.resolve", "Method", CBM_LANG_JAVA);

    const char *caller = "proj.src.libraries.System.Security.Cryptography.src.CapiHelper";
    cbm_resolution_t ms = cbm_registry_resolve(r, "MemoryStream", caller, NULL, NULL, 0);
    cbm_resolution_t wc = cbm_registry_resolve(r, "WithCancellation", caller, NULL, NULL, 0);
    cbm_resolution_t jv = cbm_registry_resolve(r, "resolve", "proj.app.Main", NULL, NULL, 0);
    ASSERT_STR_EQ(
        ms.qualified_name,
        "proj.src.libraries.System.Private.CoreLib.src.System.IO.MemoryStream.MemoryStream");
    ASSERT_STR_EQ(wc.qualified_name, "proj.src.libraries.System.Linq.Parallel.src.System.Linq."
                                     "ParallelEnumerable.WithCancellation");
    ASSERT_STR_EQ(jv.qualified_name, "proj.lib.ref.Resolver.resolve");
    cbm_registry_free(r);
    PASS();
}

/* Only a Method is a constructor: a C function named after its file
 * (`parse` in parse.c, QN `...parse.parse`) keeps its callable rank, and a C#
 * folder merely named `ref` is not a reference assembly. */
TEST(registry_tie_same_stem_function_and_plain_ref_folder) {
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "parse", "proj.c.parse.parse", "Function");
    cbm_registry_add(r, "parse", "proj.a.parse", "Class");
    cbm_registry_add_lang(r, "Run", "proj.app.ref.Helpers.Run", "Method", CBM_LANG_CSHARP);
    cbm_registry_add_lang(r, "Run", "proj.app.src.Helpers.Run", "Method", CBM_LANG_CSHARP);
    cbm_resolution_t fn = cbm_registry_resolve(r, "parse", "proj.z.main", NULL, NULL, 0);
    cbm_resolution_t run = cbm_registry_resolve(r, "Run", "proj.z.Main", NULL, NULL, 0);
    ASSERT_STR_EQ(fn.qualified_name, "proj.c.parse.parse");
    /* No stub among them: the plain QN order decides ("ref" < "src"). */
    ASSERT_STR_EQ(run.qualified_name, "proj.app.ref.Helpers.Run");
    cbm_registry_free(r);
    PASS();
}

TEST(python_builtin_member_table_matches_builtin_type_methods) {
    /* Ends and middle of the sorted table, so a mis-sorted insert shows up. */
    ASSERT_TRUE(cbm_python_is_builtin_member("add"));
    ASSERT_TRUE(cbm_python_is_builtin_member("extend"));
    ASSERT_TRUE(cbm_python_is_builtin_member("items"));
    ASSERT_TRUE(cbm_python_is_builtin_member("print"));
    ASSERT_TRUE(cbm_python_is_builtin_member("startswith"));
    ASSERT_TRUE(cbm_python_is_builtin_member("zfill"));
    /* The is* run: upstream's table listed istitle/isupper before issubset,
     * which bsearch cannot find past. Every entry of the run must resolve. */
    ASSERT_TRUE(cbm_python_is_builtin_member("isspace"));
    ASSERT_TRUE(cbm_python_is_builtin_member("issubset"));
    ASSERT_TRUE(cbm_python_is_builtin_member("issuperset"));
    ASSERT_TRUE(cbm_python_is_builtin_member("istitle"));
    ASSERT_TRUE(cbm_python_is_builtin_member("isupper"));
    /* Project-specific spellings are not builtin members. */
    ASSERT_FALSE(cbm_python_is_builtin_member("apply_converters"));
    ASSERT_FALSE(cbm_python_is_builtin_member("lazy_model_operation"));
    ASSERT_FALSE(cbm_python_is_builtin_member("describe"));
    ASSERT_FALSE(cbm_python_is_builtin_member(NULL));
    ASSERT_FALSE(cbm_python_is_builtin_member(""));
    PASS();
}

TEST(weak_member_unique_name_exempt_is_python_self_rooted_unique_and_specific_only) {
    /* The exemption: Python, self/cls-rooted receiver, unique_name, callee not
     * a builtin member. */
    ASSERT_TRUE(cbm_weak_member_unique_name_exempt(true, true, "self.compiler.apply_converters",
                                                   "unique_name"));
    ASSERT_TRUE(cbm_weak_member_unique_name_exempt(true, true, "cls.registry.lazy_model_operation",
                                                   "unique_name"));
    /* A bare parameter/local receiver carries no ownership evidence: #1276's
     * accelerator.backward() and c.describe() stay suppressed. */
    ASSERT_FALSE(
        cbm_weak_member_unique_name_exempt(true, false, "accelerator.backward", "unique_name"));
    /* A builtin type's own method stays suppressed even on self. */
    ASSERT_FALSE(
        cbm_weak_member_unique_name_exempt(true, true, "self.parts.extend", "unique_name"));
    ASSERT_FALSE(cbm_weak_member_unique_name_exempt(true, true, "self.out.print", "unique_name"));
    /* Only unique_name carries the one-definition evidence. */
    ASSERT_FALSE(cbm_weak_member_unique_name_exempt(true, true, "self.compiler.apply_converters",
                                                    "suffix_match"));
    ASSERT_FALSE(cbm_weak_member_unique_name_exempt(true, true, "self.compiler.apply_converters",
                                                    "field_type_hint"));
    /* Python only: the JS/TS family keeps its recorded trade. */
    ASSERT_FALSE(
        cbm_weak_member_unique_name_exempt(false, true, "this.compiler.apply", "unique_name"));
    /* Degenerate inputs never exempt. */
    ASSERT_FALSE(cbm_weak_member_unique_name_exempt(true, true, NULL, "unique_name"));
    ASSERT_FALSE(cbm_weak_member_unique_name_exempt(true, true, "", "unique_name"));
    ASSERT_FALSE(cbm_weak_member_unique_name_exempt(true, true, "self.x.y", NULL));

    /* Wired through the shared guard both resolvers call. */
    CBMCall call = {};
    call.callee_name = "self.compiler.apply_converters";
    call.is_method = true;
    call.receiver_is_self_attribute = true;
    ASSERT_FALSE(cbm_suppress_weak_call(CBM_LANG_PYTHON, &call, "unique_name"));
    ASSERT_TRUE(cbm_suppress_weak_call(CBM_LANG_PYTHON, &call, "suffix_match"));
    call.receiver_is_self_attribute = false;
    ASSERT_TRUE(cbm_suppress_weak_call(CBM_LANG_PYTHON, &call, "unique_name"));
    /* Embedded-script hosts carry the JS member guard. */
    call.callee_name = "localStorage.getItem";
    ASSERT_TRUE(cbm_suppress_weak_call(CBM_LANG_HTML, &call, "unique_name"));
    ASSERT_TRUE(cbm_suppress_weak_call(CBM_LANG_VUE, &call, "unique_name"));
    PASS();
}

TEST(local_binding_suppress_drops_weak_shadowed_bare_calls) {
    /* A bare `run()` whose callee is a parameter of an enclosing scope cannot be
     * the module-level `run`, so a weak short-name match fabricates the edge. */
    ASSERT_TRUE(cbm_suppress_weak_local_binding_call(true, true, "suffix_match"));
    ASSERT_TRUE(cbm_suppress_weak_local_binding_call(true, true, "unique_name"));
    ASSERT_TRUE(cbm_suppress_weak_local_binding_call(true, true, "field_type_hint"));
    ASSERT_TRUE(cbm_suppress_weak_local_binding_call(true, true, "fuzzy"));
    PASS();
}

TEST(local_binding_suppress_keeps_unshadowed_and_strong_strategies) {
    /* THE RECALL PIN. A bare call to a genuine module-level function is NOT
     * locally bound, so it is never suppressed — whatever the callee is spelled.
     * A name-keyed guard (get/run/execute) would fail exactly this. */
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, false, "suffix_match"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, false, "unique_name"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, false, "field_type_hint"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, false, "fuzzy"));
    /* Every receiver-/import-aware strategy is kept even when shadowed. */
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, "same_module"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, "import_map"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, "import_map_suffix"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, "qualified_suffix"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, "callee_suffix"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, "service_pattern"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, "lsp_cross"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, "lsp_py_method"));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, "lsp_direct"));
    /* Languages outside the caller's gate are never affected. */
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(false, true, "suffix_match"));
    /* No match (NULL/empty strategy) → nothing to suppress. */
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, NULL));
    ASSERT_FALSE(cbm_suppress_weak_local_binding_call(true, true, ""));
    PASS();
}

/* #2127: the import-binding guard drops only weak strategies, and only when the
 * caller established that the file's import contradicts the target. */
TEST(import_binding_suppress_drops_only_weak_contradicted_calls) {
    ASSERT_TRUE(cbm_suppress_weak_import_bound_call(true, true, "unique_name"));
    ASSERT_TRUE(cbm_suppress_weak_import_bound_call(true, true, "suffix_match"));
    ASSERT_TRUE(cbm_suppress_weak_import_bound_call(true, true, "fuzzy"));
    ASSERT_FALSE(cbm_suppress_weak_import_bound_call(true, false, "unique_name"));
    ASSERT_FALSE(cbm_suppress_weak_import_bound_call(false, true, "unique_name"));
    ASSERT_FALSE(cbm_suppress_weak_import_bound_call(true, true, "import_map"));
    ASSERT_FALSE(cbm_suppress_weak_import_bound_call(true, true, "same_module"));
    ASSERT_FALSE(cbm_suppress_weak_import_bound_call(true, true, "lsp_py_method"));
    ASSERT_FALSE(cbm_suppress_weak_import_bound_call(true, true, NULL));
    PASS();
}

/* #2127: when does a Python import binding contradict a resolved target? */
TEST(python_import_binding_contradicts_only_foreign_chains) {
    CBMImport items[] = {
        {.local_name = "patch", .module_path = "unittest.mock.patch"},
        {.local_name = "f", .module_path = "pkg.f"},
        {.local_name = "app", .module_path = "app.util"},
        {.local_name = "h", .module_path = "app.util.helper"},
        {.local_name = "rel", .module_path = ".views.rel"},
        {.local_name = "mock", .module_path = "unittest.mock"},
        {.local_name = "Author", .module_path = ".models.Author"},
        {.local_name = "copy", .module_path = "copy.copy"},
    };
    CBMImportArray imps = {.items = items, .count = 8, .cap = 8};
    /* `from copy import copy` is a from-import (leaf == local), not a root
     * package import: its chain `copy` must appear in the target. */
    ASSERT_TRUE(cbm_python_import_binding_contradicts(
        &imps, "copy", "proj.forms.utils.ErrorList.copy", NULL, NULL, NULL));
    /* Member call through an external module import: foreign → dropped. */
    ASSERT_TRUE(cbm_python_import_binding_contradicts(
        &imps, "mock.patch", "proj.views.generic.RedirectView.patch", NULL, NULL, NULL));
    /* Member reached THROUGH an imported class may live on any type: only the
     * import's own chain (`models`) is compared, so the manager call stays. */
    ASSERT_FALSE(cbm_python_import_binding_contradicts(
        &imps, "Author.objects.create", "proj.db.models.query.QuerySet.create", NULL, NULL, NULL));
    /* The report: external mock.patch must not be a project REST handler. */
    ASSERT_TRUE(cbm_python_import_binding_contradicts(
        &imps, "patch", "proj.app.views.PkgConfigView.patch", NULL, NULL, NULL));
    /* Re-export / src layout: the chain `pkg` is present → keep. */
    ASSERT_FALSE(
        cbm_python_import_binding_contradicts(&imps, "f", "proj.src.pkg.core.f", NULL, NULL, NULL));
    /* `import app.util` binds the root package; the callee spells its path. */
    ASSERT_FALSE(cbm_python_import_binding_contradicts(&imps, "app.util.helper",
                                                       "proj.app.util.helper", NULL, NULL, NULL));
    /* Aliased from-import: consistent target kept, foreign target dropped. */
    ASSERT_FALSE(cbm_python_import_binding_contradicts(&imps, "h", "proj.app.util.helper", NULL,
                                                       NULL, NULL));
    ASSERT_TRUE(
        cbm_python_import_binding_contradicts(&imps, "h", "proj.other.helper", NULL, NULL, NULL));
    /* Relative import: leading dots carry no segment; `views` must appear. */
    ASSERT_FALSE(cbm_python_import_binding_contradicts(&imps, "rel", "proj.app.views.rel", NULL,
                                                       NULL, NULL));
    ASSERT_TRUE(cbm_python_import_binding_contradicts(&imps, "rel", "proj.app.models.rel", NULL,
                                                      NULL, NULL));
    /* Not import-bound → never a contradiction (the recall pin). */
    ASSERT_FALSE(
        cbm_python_import_binding_contradicts(&imps, "helper", "proj.x.helper", NULL, NULL, NULL));
    ASSERT_FALSE(
        cbm_python_import_binding_contradicts(NULL, "patch", "proj.x.patch", NULL, NULL, NULL));
    PASS();
}

/* Base-class twin: the django shapes. Every spelling of the stdlib base must
 * contradict the same-named project class; project bases never do, and the
 * gate is Python-only. */
TEST(python_external_base_contradicts_django_shapes) {
    CBMImport items[] = {
        {.local_name = "unittest", .module_path = "unittest"},
        {.local_name = "ut", .module_path = "unittest"},
        {.local_name = "TestCase", .module_path = "unittest.TestCase"},
        {.local_name = "pkg", .module_path = "pkg.test.models"},
    };
    CBMImportArray imps = {.items = items, .count = 4, .cap = 4};
    const char *project_tc = "proj.django.test.testcases.TestCase";
    ASSERT_TRUE(cbm_python_external_base_contradicts(CBM_LANG_PYTHON, &imps, "unittest.TestCase",
                                                     project_tc, NULL, NULL, NULL));
    ASSERT_TRUE(cbm_python_external_base_contradicts(CBM_LANG_PYTHON, &imps, "TestCase", project_tc,
                                                     NULL, NULL, NULL));
    /* `import unittest as ut`: `ut.TestCase` spells `unittest.TestCase`. */
    ASSERT_TRUE(cbm_python_external_base_contradicts(CBM_LANG_PYTHON, &imps, "ut.TestCase",
                                                     project_tc, NULL, NULL, NULL));
    /* A target that does spell the chain (vendored/src-layout unittest). */
    ASSERT_FALSE(cbm_python_external_base_contradicts(
        CBM_LANG_PYTHON, &imps, "ut.TestCase", "proj.src.unittest.TestCase", NULL, NULL, NULL));
    /* Dotted project base through `import pkg.test.models`. */
    ASSERT_FALSE(
        cbm_python_external_base_contradicts(CBM_LANG_PYTHON, &imps, "pkg.test.models.Base",
                                             "proj.pkg.test.models.Base", NULL, NULL, NULL));
    /* Not import-bound: a same-module base is never judged. */
    ASSERT_FALSE(cbm_python_external_base_contradicts(CBM_LANG_PYTHON, &imps, "SimpleTestCase",
                                                      "proj.django.test.testcases.SimpleTestCase",
                                                      NULL, NULL, NULL));
    /* Python only. */
    ASSERT_FALSE(cbm_python_external_base_contradicts(CBM_LANG_JAVA, &imps, "unittest.TestCase",
                                                      project_tc, NULL, NULL, NULL));
    PASS();
}

TEST(weak_call_guards_share_one_drop_list) {
    /* The member guard, the local-binding guard and the cross-LSP base-class
     * resolution must agree on what "weak" means; all read one predicate. */
    static const char *const strategies[] = {"suffix_match",
                                             "unique_name",
                                             "field_type_hint",
                                             "fuzzy",
                                             "same_module",
                                             "import_map",
                                             "import_map_suffix",
                                             "qualified_suffix",
                                             "callee_suffix",
                                             "service_pattern",
                                             "lsp_cross",
                                             "lsp_ts_method",
                                             "lsp_py_method",
                                             "lsp_direct",
                                             "",
                                             NULL};
    for (int i = 0; strategies[i] != NULL; i++) {
        bool weak = cbm_weak_short_name_strategy(strategies[i]);
        ASSERT_EQ(cbm_suppress_weak_member_match(true, true, strategies[i]), weak);
        ASSERT_EQ(cbm_suppress_weak_local_binding_call(true, true, strategies[i]), weak);
    }
    PASS();
}

TEST(weak_call_guard_owns_language_set) {
    /* cbm_suppress_weak_call is the one owner of both weak-call language sets,
     * shared by the sequential and parallel resolvers. */
    CBMCall member = {};
    member.is_method = true;
    ASSERT_TRUE(cbm_suppress_weak_call(CBM_LANG_PYTHON, &member, "suffix_match"));
    ASSERT_TRUE(cbm_suppress_weak_call(CBM_LANG_JAVASCRIPT, &member, "unique_name"));
    ASSERT_TRUE(cbm_suppress_weak_call(CBM_LANG_TYPESCRIPT, &member, "field_type_hint"));
    ASSERT_TRUE(cbm_suppress_weak_call(CBM_LANG_TSX, &member, "suffix_match"));
    ASSERT_FALSE(cbm_suppress_weak_call(CBM_LANG_GO, &member, "suffix_match"));
    ASSERT_FALSE(cbm_suppress_weak_call(CBM_LANG_PYTHON, &member, "lsp_py_method"));

    CBMCall bound = {};
    bound.callee_is_locally_bound = true;
    ASSERT_TRUE(cbm_suppress_weak_call(CBM_LANG_PYTHON, &bound, "unique_name"));
    /* The local-binding guard is Python-only. */
    ASSERT_FALSE(cbm_suppress_weak_call(CBM_LANG_JAVASCRIPT, &bound, "unique_name"));

    CBMCall plain = {};
    ASSERT_FALSE(cbm_suppress_weak_call(CBM_LANG_PYTHON, &plain, "suffix_match"));
    PASS();
}

TEST(ref_edge_guard_matches_its_parts) {
    /* cbm_suppress_ref_edge is exactly the OR of the cross-language reference
     * guard and the Go/C bare-field guard, with or without the language memo. */
    static const CBMLanguage langs[] = {CBM_LANG_GO,     CBM_LANG_C,          CBM_LANG_CPP,
                                        CBM_LANG_PYTHON, CBM_LANG_JAVASCRIPT, CBM_LANG_COUNT};
    static const char *const paths[] = {"pkg/a.go",    "pkg/b.go",  "bpf/probe.c", "bpf/probe.h",
                                        "web/x.ts",    "web/y.tsx", "Makefile",    "",
                                        "a.blade.php", "b.php",     "pkg/a.go"};
    static const char *const labels[] = {"Field", "Variable"};
    for (size_t l = 0; l < sizeof(langs) / sizeof(langs[0]); l++) {
        cbm_lang_memo_t memo = {};
        for (size_t p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
            for (size_t b = 0; b < sizeof(labels) / sizeof(labels[0]); b++) {
                for (int member = 0; member < 2; member++) {
                    cbm_gbuf_node_t node = {};
                    node.file_path = (char *)paths[p];
                    node.label = (char *)labels[b];
                    bool want = cbm_suppress_cross_language_ref(langs[l], paths[p]) ||
                                cbm_suppress_bare_field_ref(langs[l], member != 0, labels[b]);
                    ASSERT_EQ(cbm_suppress_ref_edge(langs[l], member != 0, &node, &memo), want);
                    ASSERT_EQ(cbm_suppress_ref_edge(langs[l], member != 0, &node, NULL), want);
                }
            }
        }
    }
    PASS();
}

TEST(call_target_guard_matches_its_parts) {
    cbm_gbuf_node_t js = {};
    js.file_path = (char *)"web/src/pages/Editor.js";
    js.label = (char *)"Function";
    ASSERT_TRUE(cbm_suppress_call_target(CBM_LANG_PYTHON, &js, "suffix_match"));
    ASSERT_FALSE(cbm_suppress_call_target(CBM_LANG_PYTHON, &js, "unique_name"));
    ASSERT_FALSE(cbm_suppress_call_target(CBM_LANG_TYPESCRIPT, &js, "suffix_match"));

    cbm_gbuf_node_t field = {};
    field.file_path = (char *)"pkg/state/state.go";
    field.label = (char *)"Field";
    ASSERT_TRUE(cbm_suppress_call_target(CBM_LANG_GO, &field, "unique_name"));
    ASSERT_FALSE(cbm_suppress_call_target(CBM_LANG_GO, &field, "lsp_type_dispatch"));
    ASSERT_FALSE(cbm_suppress_call_target(CBM_LANG_JAVA, &field, "unique_name"));
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────── */

/* Python package scope is an explicit resolver input, not a property inferred
 * from a QN shared by other languages. Keep these fixtures separate from the
 * raw public-API tests above. */
static bool registry_scope_match(cbm_resolution_t result, const char *qn, const char *strategy) {
    return qn && strategy && result.qualified_name && result.strategy &&
           strcmp(result.qualified_name, qn) == 0 && strcmp(result.strategy, strategy) == 0 &&
           result.candidate_count == 1;
}

TEST(python_symbol_scope_len_preserves_file_identity) {
    const char *inputs[] = {"proj.pkg.__init__",
                            "proj.__init__",
                            "proj.pkg.__init__.__init__",
                            "proj.pkg.mod",
                            "proj.pkg.x__init__",
                            "__init__",
                            ".__init__",
                            "proj.pkg.index",
                            "",
                            NULL};
    const char *scopes[] = {"proj.pkg",
                            "proj",
                            "proj.pkg.__init__",
                            "proj.pkg.mod",
                            "proj.pkg.x__init__",
                            "__init__",
                            ".__init__",
                            "proj.pkg.index",
                            "",
                            ""};
    bool lengths = true;
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        lengths = cbm_fqn_symbol_scope_len(inputs[i]) == strlen(scopes[i]) && lengths;
    }
    char *module = cbm_pipeline_fqn_module("proj", "pkg/__init__.py");
    char *symbol = cbm_pipeline_fqn_compute("proj", "pkg/__init__.py", "Field");
    bool identities = module && symbol && strcmp(module, "proj.pkg.__init__") == 0 &&
                      strcmp(symbol, "proj.pkg.Field") == 0 &&
                      cbm_fqn_symbol_scope_len(module) == strlen("proj.pkg");
    free(module);
    free(symbol);
    ASSERT_TRUE(lengths && identities);
    PASS();
}

TEST(registry_python_package_scope_is_explicit) {
    cbm_registry_t *r = cbm_registry_new();
    ASSERT_NOT_NULL(r);
    cbm_registry_add(r, "Field", "proj.forms.fields.Field", "Class");
    cbm_registry_add(r, "Field", "proj.db.fields.Field", "Class");
    cbm_registry_add(r, "Field", "proj.db.fields.__init__.Field", "Class");
    cbm_registry_add(r, "Field", "proj.db.plain.Field", "Class");
    cbm_registry_add(r, "Field", "proj.db.index.Field", "Class");
    const char *keys[] = {"forms"};
    const char *vals[] = {"proj.forms"};
    const char *module = "proj.db.fields.__init__";
    bool seeded = cbm_registry_size(r) == 5;
    cbm_resolution_t package =
        cbm_registry_resolve_lang(r, "Field", module, keys, vals, 1, CBM_LANG_PYTHON);
    bool python = registry_scope_match(package, "proj.db.fields.Field", "same_module");
    cbm_resolution_t plain =
        cbm_registry_resolve_lang(r, "Field", "proj.db.plain", keys, vals, 1, CBM_LANG_PYTHON);
    bool normal = registry_scope_match(plain, "proj.db.plain.Field", "same_module");
    cbm_resolution_t raw = cbm_registry_resolve(r, "Field", module, keys, vals, 1);
    bool preserved = registry_scope_match(raw, "proj.db.fields.__init__.Field", "same_module");
    const CBMLanguage raw_languages[] = {CBM_LANG_JAVA, CBM_LANG_GO, CBM_LANG_JAVASCRIPT,
                                         (CBMLanguage)-1, CBM_LANG_COUNT};
    for (size_t i = 0; i < sizeof(raw_languages) / sizeof(raw_languages[0]); i++) {
        cbm_resolution_t result =
            cbm_registry_resolve_lang(r, "Field", module, keys, vals, 1, raw_languages[i]);
        preserved = registry_scope_match(result, raw.qualified_name, "same_module") &&
                    result.confidence == raw.confidence && preserved;
    }
    cbm_resolution_t index =
        cbm_registry_resolve_lang(r, "Field", "proj.db.index", keys, vals, 1, CBM_LANG_JAVASCRIPT);
    bool index_raw = registry_scope_match(index, "proj.db.index.Field", "same_module");
    cbm_registry_free(r);
    ASSERT_TRUE(seeded && python && normal && preserved && index_raw);
    PASS();
}

TEST(registry_python_scope_keeps_import_precedence_and_raw_scoring) {
    cbm_registry_t *r = cbm_registry_new();
    ASSERT_NOT_NULL(r);
    cbm_registry_add(r, "Field", "proj.vendor.Field", "Class");
    cbm_registry_add(r, "Field", "proj.pkg.Field", "Class");
    cbm_registry_add(r, "Field", "proj.pkg.__init__.Field", "Class");
    cbm_registry_add(r, "choose", "proj.pkg.__init__.child.choose", "Function");
    cbm_registry_add(r, "choose", "proj.pkg.peer.choose", "Function");
    const char *keys[] = {"Field"};
    const char *vals[] = {"proj.vendor.Field"};
    bool seeded = cbm_registry_size(r) == 5;
    cbm_resolution_t imported =
        cbm_registry_resolve_lang(r, "Field", "proj.pkg.__init__", keys, vals, 1, CBM_LANG_PYTHON);
    bool precedence = registry_scope_match(imported, "proj.vendor.Field", "import_map");
    cbm_resolution_t raw = cbm_registry_resolve(r, "choose", "proj.pkg.__init__", NULL, NULL, 0);
    cbm_resolution_t python =
        cbm_registry_resolve_lang(r, "choose", "proj.pkg.__init__", NULL, NULL, 0, CBM_LANG_PYTHON);
    bool scoring =
        raw.qualified_name && python.qualified_name && raw.strategy && python.strategy &&
        strcmp(raw.qualified_name, "proj.pkg.__init__.child.choose") == 0 &&
        strcmp(python.qualified_name, raw.qualified_name) == 0 &&
        strcmp(raw.strategy, "suffix_match") == 0 && strcmp(python.strategy, raw.strategy) == 0 &&
        python.confidence == raw.confidence && python.candidate_count == raw.candidate_count;
    cbm_registry_free(r);
    ASSERT_TRUE(seeded && precedence && scoring);
    PASS();
}

TEST(call_onto_field_follows_language_and_shape) {
    /* A call the registry resolved onto a Field node. */
    /* Another language: only the spelling connects them. */
    ASSERT_EQ(cbm_call_onto_field_policy(CBM_LANG_BASH, "command", "Field", "src/probe.h"),
              CBM_FIELD_CALL_DROP);
    ASSERT_EQ(cbm_call_onto_field_policy(CBM_LANG_PYTHON, "sorted", "Field", "src/pool.c"),
              CBM_FIELD_CALL_DROP);
    /* A bare C call is never a struct member. */
    ASSERT_EQ(cbm_call_onto_field_policy(CBM_LANG_C, "socket", "Field", "tests/t.c"),
              CBM_FIELD_CALL_DROP);
    /* A C member call is decided by its object's type, in a .c or a .h. */
    ASSERT_EQ(cbm_call_onto_field_policy(CBM_LANG_C, "cb.close", "Field", "src/cb.h"),
              CBM_FIELD_CALL_BY_OWNER);
    ASSERT_EQ(cbm_call_onto_field_policy(CBM_LANG_C, "w->svc->app.cancel", "Field", "src/a.c"),
              CBM_FIELD_CALL_BY_OWNER);
    /* C++ reaches its own members bare, so a bare call keeps its resolution;
     * a member call is typed like C's (`text.size()` is not some struct's
     * `size` member). */
    ASSERT_EQ(cbm_call_onto_field_policy(CBM_LANG_CPP, "handler", "Field", "src/a.h"),
              CBM_FIELD_CALL_KEEP);
    ASSERT_EQ(cbm_call_onto_field_policy(CBM_LANG_CPP, "text.size", "Field", "src/a.h"),
              CBM_FIELD_CALL_BY_OWNER);
    /* The same language as the Field keeps its resolution too. */
    ASSERT_EQ(cbm_call_onto_field_policy(CBM_LANG_GO, "s.fn", "Field", "pkg/s.go"),
              CBM_FIELD_CALL_KEEP);
    /* Not a Field target: not this rule's business. */
    ASSERT_EQ(cbm_call_onto_field_policy(CBM_LANG_BASH, "main", "Function", "src/main.c"),
              CBM_FIELD_CALL_KEEP);
    ASSERT_EQ(cbm_call_onto_field_policy(CBM_LANG_C, "socket", NULL, "tests/t.c"),
              CBM_FIELD_CALL_KEEP);
    PASS();
}

TEST(c_member_access_binds_by_owner_only) {
    /* The member half of a C `a.b` / `a->b` means nothing without the type of
     * `a`: it binds through the C LSP's field-owner rows and never through a
     * bare-name lookup. The selector shape arrives as is_member_access. */
    ASSERT_TRUE(cbm_c_member_binds_by_owner(true, true));
    /* A bare C identifier keeps the ordinary resolution. */
    ASSERT_FALSE(cbm_c_member_binds_by_owner(true, false));
    /* Other languages keep theirs: a C++ or Objective-C method body reaches its
     * own members without a selector, and Go has its own rule above. */
    ASSERT_FALSE(cbm_c_member_binds_by_owner(false, true));
    ASSERT_FALSE(cbm_c_member_binds_by_owner(false, false));
    PASS();
}


TEST(unique_field_qn_needs_exactly_one_field_of_that_name) {
    /* What an untyped C member access may still bind: a name exactly one
     * Field carries. Two Fields would be a choice; a function or variable of
     * the same name is not a candidate for a member at all. */
    cbm_registry_t *r = cbm_registry_new();
    ASSERT_NOT_NULL(r);
    cbm_registry_add(r, "only_here", "proj.a.Alpha.only_here", "Field");
    cbm_registry_add(r, "count", "proj.a.Alpha.count", "Field");
    cbm_registry_add(r, "count", "proj.b.Beta.count", "Field");
    cbm_registry_add(r, "close", "proj.cb.Callbacks.close", "Field");
    cbm_registry_add(r, "close", "proj.io.close", "Function");
    cbm_registry_add(r, "lonely_fn", "proj.io.lonely_fn", "Function");

    const char *only = cbm_registry_unique_field_qn(r, "only_here");
    ASSERT_NOT_NULL(only);
    ASSERT_STR_EQ(only, "proj.a.Alpha.only_here");
    /* Two fields share the name: no pick. */
    ASSERT_NULL(cbm_registry_unique_field_qn(r, "count"));
    /* One field and one function share the name: the function is not a
     * candidate for a member, so the field is still the only one. */
    const char *member = cbm_registry_unique_field_qn(r, "close");
    ASSERT_NOT_NULL(member);
    ASSERT_STR_EQ(member, "proj.cb.Callbacks.close");
    /* Unique, but not a field. */
    ASSERT_NULL(cbm_registry_unique_field_qn(r, "lonely_fn"));
    /* Unknown name and degenerate input. */
    ASSERT_NULL(cbm_registry_unique_field_qn(r, "absent"));
    ASSERT_NULL(cbm_registry_unique_field_qn(r, NULL));
    ASSERT_NULL(cbm_registry_unique_field_qn(NULL, "count"));
    cbm_registry_free(r);
    PASS();
}

TEST(field_name_ambiguous_needs_two_fields_of_that_name) {
    /* The cross-file pass is worth running for an untyped C member only when
     * two or more Fields carry its name: with one the unique-name fallback
     * binds it, with none nothing binds either way. */
    cbm_registry_t *r = cbm_registry_new();
    ASSERT_NOT_NULL(r);
    cbm_registry_add(r, "only_here", "proj.a.Alpha.only_here", "Field");
    cbm_registry_add(r, "count", "proj.a.Alpha.count", "Field");
    cbm_registry_add(r, "count", "proj.b.Beta.count", "Field");
    cbm_registry_add(r, "close", "proj.cb.Callbacks.close", "Field");
    cbm_registry_add(r, "close", "proj.io.close", "Function");

    ASSERT_TRUE(cbm_registry_field_name_ambiguous(r, "count"));
    ASSERT_FALSE(cbm_registry_field_name_ambiguous(r, "only_here"));
    /* A function namesake is not a second candidate. */
    ASSERT_FALSE(cbm_registry_field_name_ambiguous(r, "close"));
    ASSERT_FALSE(cbm_registry_field_name_ambiguous(r, "absent"));
    ASSERT_FALSE(cbm_registry_field_name_ambiguous(r, NULL));
    ASSERT_FALSE(cbm_registry_field_name_ambiguous(NULL, "count"));
    cbm_registry_free(r);
    PASS();
}


TEST(c_bare_call_does_not_weigh_field_namesakes) {
    /* 2026-10-13 kernel audit: a struct member named like a header function
     * made the bare name two candidates, the answer a suffix_match, and the
     * #725 guard (.h is C++) refused it -- `put_page(page)` 345 -> 9 callers.
     * A bare C call never names a member, so its Field namesakes do not
     * compete: the function is the unique name it was in v0.11.0. */
    cbm_registry_t *r = cbm_registry_new();
    cbm_registry_add(r, "put_page", "proj.include.linux.mm.put_page", "Function");
    cbm_registry_add(r, "put_page",
                          "proj.arch.arm64.include.asm.kvm_pgtable.kvm_pgtable_mm_ops.put_page",
                          "Field");
    const char *mod = "proj.arch.arm64.kernel.elfcore";
    cbm_resolution_t res =
        cbm_registry_resolve_call_lang(r, "put_page", mod, NULL, NULL, 0, CBM_LANG_C);
    ASSERT_STR_EQ(res.qualified_name, "proj.include.linux.mm.put_page");
    ASSERT_STR_EQ(res.strategy, "unique_name");
    /* The reference resolver and C++ (which reaches its own members bare) still
     * weigh the member: two candidates. */
    res = cbm_registry_resolve(r, "put_page", mod, NULL, NULL, 0);
    ASSERT_STR_EQ(res.strategy, "suffix_match");
    res = cbm_registry_resolve_call_lang(r, "put_page", mod, NULL, NULL, 0, CBM_LANG_CPP);
    ASSERT_STR_EQ(res.strategy, "suffix_match");
    /* A name only members carry is no bare C call target at all -- neither a
     * struct member nor a C++ method: redis's libc `read()` became the unique
     * name of a Qt adapter's `read` Method once only Fields were set aside. */
    cbm_registry_add(r, "on_close", "proj.net.sock.sock_ops.on_close", "Field");
    res = cbm_registry_resolve_call_lang(r, "on_close", mod, NULL, NULL, 0, CBM_LANG_C);
    ASSERT_TRUE(res.qualified_name == NULL || res.qualified_name[0] == '\0');
    cbm_registry_add(r, "read", "proj.deps.hiredis.adapters.qt.RedisQtAdapter.read", "Method");
    cbm_registry_add(r, "read", "proj.src.connection.ConnectionType.read", "Field");
    res = cbm_registry_resolve_call_lang(r, "read", mod, NULL, NULL, 0, CBM_LANG_C);
    ASSERT_TRUE(res.qualified_name == NULL || res.qualified_name[0] == '\0');
    /* C++ still reaches the method by name. */
    res = cbm_registry_resolve_call_lang(r, "read", mod, NULL, NULL, 0, CBM_LANG_CPP);
    ASSERT_TRUE(res.qualified_name && res.qualified_name[0] != '\0');
    cbm_registry_free(r);
    PASS();
}


SUITE(registry) {
    RUN_TEST(python_symbol_scope_len_preserves_file_identity);
    RUN_TEST(registry_python_package_scope_is_explicit);
    RUN_TEST(registry_python_scope_keeps_import_precedence_and_raw_scoring);
    RUN_TEST(registry_name_lookup_keeps_overload_family_only);
    /* FQN */
    RUN_TEST(fqn_simple);
    RUN_TEST(fqn_no_name);
    RUN_TEST(fqn_python_init);
    RUN_TEST(fqn_js_index);
    RUN_TEST(fqn_module);
    RUN_TEST(fqn_folder);
    RUN_TEST(fqn_root_file);
    /* FQN collision regression (Folder vs __init__.py Module) */
    RUN_TEST(fqn_init_module_distinct_from_folder);
    RUN_TEST(fqn_init_nested_module_distinct);
    RUN_TEST(fqn_index_ts_module_distinct_from_folder);
    RUN_TEST(fqn_init_symbols_get_clean_package_qn);
    RUN_TEST(fqn_index_symbols_get_clean_qn);
    RUN_TEST(fqn_init_file_node_distinct);
    RUN_TEST(fqn_regular_module_unchanged);
    RUN_TEST(project_name_from_path);
    RUN_TEST(project_name_from_root);
    /* Registry lifecycle */
    RUN_TEST(registry_create_free);
    RUN_TEST(registry_free_null);
    RUN_TEST(registry_add_and_exists);
    RUN_TEST(registry_label_of);
    RUN_TEST(registry_find_by_name);
    RUN_TEST(registry_no_duplicates);
    /* Resolution */
    RUN_TEST(resolve_same_module);
    RUN_TEST(resolve_import_map);
    RUN_TEST(resolve_import_map_bare_function);
    RUN_TEST(resolve_unique_name);
    RUN_TEST(resolve_unresolved);
    RUN_TEST(resolve_many_nodes);
    /* Confidence band */
    RUN_TEST(confidence_band_high);
    RUN_TEST(confidence_band_medium);
    RUN_TEST(confidence_band_speculative);
    /* Suffix match + import map suffix */
    RUN_TEST(resolve_suffix_match);
    RUN_TEST(resolve_caps_unresolvably_ambiguous_names);
    RUN_TEST(cross_language_suffix_match_drops_py_vs_js);
    RUN_TEST(cross_language_ref_drops_go_vs_c);
    RUN_TEST(go_bare_ref_never_binds_field);
    RUN_TEST(c_bare_ref_never_binds_field);
    RUN_TEST(go_textual_call_never_binds_field);
    RUN_TEST(dynamic_suppress_drops_weak_method_matches);
    RUN_TEST(dynamic_suppress_keeps_high_confidence_and_non_methods);
    RUN_TEST(cross_language_config_caller_drops_unique_name_too);
    RUN_TEST(registry_tie_break_is_independent_of_registration_order);
    RUN_TEST(registry_tie_prefers_callable_over_property);
    RUN_TEST(registry_tie_ignores_nesting_depth);
    RUN_TEST(registry_tie_prefers_class_over_its_constructor);
    RUN_TEST(registry_tie_same_stem_function_and_plain_ref_folder);
    RUN_TEST(registry_tie_prefers_csharp_implementation_over_ref_stub);
    RUN_TEST(python_builtin_member_table_matches_builtin_type_methods);
    RUN_TEST(weak_member_unique_name_exempt_is_python_self_rooted_unique_and_specific_only);
    RUN_TEST(local_binding_suppress_drops_weak_shadowed_bare_calls);
    RUN_TEST(local_binding_suppress_keeps_unshadowed_and_strong_strategies);
    RUN_TEST(import_binding_suppress_drops_only_weak_contradicted_calls);
    RUN_TEST(python_import_binding_contradicts_only_foreign_chains);
    RUN_TEST(python_external_base_contradicts_django_shapes);
    RUN_TEST(weak_call_guards_share_one_drop_list);
    RUN_TEST(weak_call_guard_owns_language_set);
    RUN_TEST(ref_edge_guard_matches_its_parts);
    RUN_TEST(call_target_guard_matches_its_parts);
    RUN_TEST(resolve_import_map_aliased_from_import);
    RUN_TEST(resolve_import_map_suffix);
    /* Import reachability */
    RUN_TEST(resolve_is_import_reachable);
    RUN_TEST(resolve_import_reachable_prefix);
    /* Negative import evidence */
    RUN_TEST(negative_import_rejects_unimported);
    /* Fuzzy resolve */
    RUN_TEST(fuzzy_resolve_single_candidate);
    RUN_TEST(fuzzy_resolve_nonexistent);
    RUN_TEST(fuzzy_resolve_multiple_best_by_distance);
    RUN_TEST(fuzzy_resolve_deep_name_extraction);
    RUN_TEST(fuzzy_resolve_empty_registry);
    RUN_TEST(fuzzy_resolve_confidence_single);
    RUN_TEST(fuzzy_resolve_confidence_distance);
    RUN_TEST(fuzzy_penalty_unreachable_import);
    RUN_TEST(fuzzy_no_import_map_passthrough);
    RUN_TEST(call_onto_field_follows_language_and_shape);
    RUN_TEST(unique_field_qn_needs_exactly_one_field_of_that_name);
    RUN_TEST(field_name_ambiguous_needs_two_fields_of_that_name);
    RUN_TEST(c_member_access_binds_by_owner_only);
    RUN_TEST(c_bare_call_does_not_weigh_field_namesakes);
}
