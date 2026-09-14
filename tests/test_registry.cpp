/*
 * test_registry.c — Tests for function registry and FQN helpers.
 *
 * RED phase: Define expected behavior for symbol registration,
 * resolution strategies, and qualified name computation.
 */
#include "test_framework.h"
#include "pipeline/pipeline.h"

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
    ASSERT_TRUE(cbm_go_suppress_bare_field_ref(true, false, "Field"));
    /* The member half of a selector may bind a field. */
    ASSERT_FALSE(cbm_go_suppress_bare_field_ref(true, true, "Field"));
    /* Bare references to non-fields are untouched. */
    ASSERT_FALSE(cbm_go_suppress_bare_field_ref(true, false, "Variable"));
    ASSERT_FALSE(cbm_go_suppress_bare_field_ref(true, false, "Function"));
    /* Other languages reference their own members bare inside methods. */
    ASSERT_FALSE(cbm_go_suppress_bare_field_ref(false, false, "Field"));
    /* Degenerate input -> nothing to judge. */
    ASSERT_FALSE(cbm_go_suppress_bare_field_ref(true, false, NULL));
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
     * guard and the Go bare-field guard, with or without the language memo. */
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
                                cbm_go_suppress_bare_field_ref(langs[l] == CBM_LANG_GO, member != 0,
                                                               labels[b]);
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

SUITE(registry) {
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
    RUN_TEST(go_textual_call_never_binds_field);
    RUN_TEST(dynamic_suppress_drops_weak_method_matches);
    RUN_TEST(dynamic_suppress_keeps_high_confidence_and_non_methods);
    RUN_TEST(local_binding_suppress_drops_weak_shadowed_bare_calls);
    RUN_TEST(local_binding_suppress_keeps_unshadowed_and_strong_strategies);
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
}
