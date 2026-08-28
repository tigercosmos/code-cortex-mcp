/*
 * test_main.c — Test runner entry point for pure C rewrite.
 *
 * Includes all test suites and runs them sequentially.
 */
/* Global test counters (declared extern in test_framework.h) */
int tf_pass_count = 0;
int tf_fail_count = 0;
int tf_skip_count = 0;

#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>
#if !defined(_WIN32)
#include "foundation/platform.h" /* cbm_resolve_self_exe_path — deleted-self probe */
#include <unistd.h>
#endif

/* #1204 deleted-self probe. Re-exec'd by the platform suite's driver from a
 * COPY of this binary, so the driver can rename over / unlink that copy while
 * this child is running and ask the resolver what it hands back. Runs before
 * any suite and exits; returns -1 when this is an ordinary test run. */
static int tf_maybe_run_deleted_self_probe(int argc, char **argv) {
#if defined(__linux__) || defined(__APPLE__)
    if (argc != 4 || strcmp(argv[1], "__cbm_deleted_self_probe") != 0) {
        return -1;
    }
    int ready_fd = atoi(argv[2]);
    int continue_fd = atoi(argv[3]);
    if (write(ready_fd, "R", 1) != 1) {
        return 41;
    }
    char go = '\0';
    if (read(continue_fd, &go, 1) != 1) {
        return 42;
    }
    char resolved[1024];
    bool ok = cbm_resolve_self_exe_path(NULL, resolved, sizeof(resolved));
#if defined(__linux__)
    /* Contract: after a rename-over the resolver hands back the
     * /proc/self/exe magic link — the in-memory OLD build, the only spawn the
     * worker's build-fingerprint gate accepts. Prove we really ARE in the
     * deleted state first, or the assertions pass vacuously on an intact
     * image. */
    char link_target[1024];
    ssize_t n = readlink("/proc/self/exe", link_target, sizeof(link_target) - 1);
    if (n <= 0) {
        return 45;
    }
    link_target[n] = '\0';
    if (strstr(link_target, " (deleted)") == NULL) {
        return 46;
    }
    if (!ok) {
        return 43;
    }
    return strcmp(resolved, "/proc/self/exe") == 0 && access(resolved, X_OK) == 0 ? 0 : 44;
#else
    /* macOS has no magic link: fail closed. Success here is the resolver
     * REFUSING, so the supervisor logs no_self_path and degrades in-process
     * instead of spawning a missing binary. */
    return ok ? 44 : 0;
#endif
#else
    (void)argc;
    (void)argv;
    return -1;
#endif
}

/* Forward declarations of suite functions */
extern void suite_arena(void);
extern void suite_hash_table(void);
extern void suite_dyn_array(void);
extern void suite_str_intern(void);
extern void suite_log(void);
extern void suite_str_util(void);
extern void suite_platform(void);
extern void suite_diagnostics(void);
extern void suite_extraction(void);
extern void suite_extraction_inheritance(void);
extern void suite_extraction_imports(void);
extern void suite_grammar_regression(void);
extern void suite_grammar_labels(void);
extern void suite_grammar_imports(void);
extern void suite_ac(void);
extern void suite_store_nodes(void);
extern void suite_store_edges(void);
extern void suite_store_search(void);
extern void suite_cypher(void);
extern void suite_mcp(void);
extern void suite_store_meta(void);
extern void suite_hook_fastpath(void);
extern void suite_tool_server(void);
extern void suite_language(void);
extern void suite_userconfig(void);
extern void suite_gitignore(void);
extern void suite_discover(void);
extern void suite_graph_buffer(void);
extern void suite_registry(void);
extern void suite_pipeline(void);
extern void suite_fqn(void);
extern void suite_path_alias(void);
extern void suite_watcher(void);
extern void suite_lz4(void);
extern void suite_zstd(void);
extern void suite_artifact(void);
extern void suite_sqlite_writer(void);
extern void suite_go_lsp(void);
extern void suite_c_lsp(void);
extern void suite_php_lsp(void);
extern void suite_cs_lsp(void);
extern void suite_cs_lsp_bench(void);
extern void suite_scope(void);
extern void suite_type_rep(void);
extern void suite_py_lsp(void);
extern void suite_py_lsp_bench(void);
extern void suite_py_lsp_stress(void);
extern void suite_py_lsp_scale(void);
extern void suite_ts_lsp(void);
extern void suite_java_lsp(void);
extern void suite_java_lsp_coverage(void);
extern void suite_kotlin_lsp(void);
extern void suite_rust_lsp(void);
extern void suite_store_arch(void);
extern void suite_store_bulk(void);
extern void suite_store_pragmas(void);
extern void suite_store_checkpoint(void);
extern void suite_configlink(void);
extern void suite_infrascan(void);
extern void suite_cli(void);
extern void suite_system_info(void);
extern void suite_worker_pool(void);
extern void suite_parallel(void);
extern void suite_mem(void);
extern void suite_security(void);
extern void suite_yaml(void);
extern void suite_integration(void);
extern void suite_lang_contract(void);
extern void suite_edge_imports(void);
extern void suite_edge_structural(void);
extern void suite_lsp_resolution_probe(void);
extern void suite_node_creation_probe(void);
extern void suite_edge_types_probe(void);
extern void suite_convergence_probe(void);
extern void suite_matrix_known_classes(void);
extern void suite_matrix_new_constructs(void);
extern void suite_grammar_probe_a(void);
extern void suite_grammar_probe_b(void);
extern void suite_grammar_probe_c(void);
extern void suite_grammar_probe_d(void);
extern void suite_grammar_probe_e(void);
extern void suite_grammar_probe_f(void);
extern void suite_grammar_probe_g(void);
extern void suite_incremental(void);
extern void suite_simhash(void);
extern void suite_stack_overflow(void);

/* Free the main thread's thread-local node-type bitset cache before exit so
 * LeakSanitizer (Linux x64) doesn't report it. Worker threads free their own
 * caches at thread teardown (pass_parallel.c). */
extern "C" void cbm_kind_in_set_free_cache(void);

int main(int argc, char **argv) {
    int deleted_self_rc = tf_maybe_run_deleted_self_probe(argc, argv);
    if (deleted_self_rc >= 0) {
        return deleted_self_rc;
    }

    printf("\n  code-cortex-mcp  C test suite\n");

    /* Foundation */
    RUN_SUITE(arena);
    RUN_SUITE(hash_table);
    RUN_SUITE(dyn_array);
    RUN_SUITE(str_intern);
    RUN_SUITE(log);
    RUN_SUITE(str_util);
    RUN_SUITE(platform);
    RUN_SUITE(diagnostics);

    /* Existing C code regression tests */
    RUN_SUITE(ac);
    RUN_SUITE(extraction);
    RUN_SUITE(extraction_inheritance);
    RUN_SUITE(extraction_imports);
    RUN_SUITE(grammar_regression);
    RUN_SUITE(grammar_labels);
    RUN_SUITE(grammar_imports);

    /* Store (M5) */
    RUN_SUITE(store_nodes);
    RUN_SUITE(store_edges);
    RUN_SUITE(store_search);
    RUN_SUITE(store_bulk);
    RUN_SUITE(store_pragmas);
    RUN_SUITE(store_checkpoint);

    /* Cypher (M6) */
    RUN_SUITE(cypher);

    /* MCP Server (M9) */
    RUN_SUITE(mcp);
    RUN_SUITE(store_meta);
    RUN_SUITE(hook_fastpath);
    RUN_SUITE(tool_server);

    /* Discover (M2) */
    RUN_SUITE(language);
    RUN_SUITE(userconfig);
    RUN_SUITE(gitignore);
    RUN_SUITE(discover);

    /* Graph Buffer (M7) */
    RUN_SUITE(graph_buffer);

    /* Pipeline (M8) */
    RUN_SUITE(registry);
    RUN_SUITE(pipeline);
    RUN_SUITE(fqn);
    RUN_SUITE(path_alias);

    /* Watcher (M10) */
    RUN_SUITE(watcher);

    /* LZ4 + zstd + SQLite writer */
    RUN_SUITE(lz4);
    RUN_SUITE(zstd);
    RUN_SUITE(sqlite_writer);

    /* Persistent artifact export/import */
    RUN_SUITE(artifact);

    /* LSP resolvers */
    RUN_SUITE(scope);
    RUN_SUITE(type_rep);
    RUN_SUITE(go_lsp);
    RUN_SUITE(c_lsp);
    RUN_SUITE(php_lsp);
    RUN_SUITE(cs_lsp);
    RUN_SUITE(cs_lsp_bench);
    RUN_SUITE(py_lsp);
    RUN_SUITE(kotlin_lsp);
    RUN_SUITE(rust_lsp);
    RUN_SUITE(py_lsp_bench);
    RUN_SUITE(py_lsp_stress);
    RUN_SUITE(py_lsp_scale);
    RUN_SUITE(ts_lsp);
    RUN_SUITE(java_lsp);
    RUN_SUITE(java_lsp_coverage);

    /* Architecture + ADR + Louvain */
    RUN_SUITE(store_arch);

    /* HTTP link */

    /* Traces helpers */

    /* Config link */
    RUN_SUITE(configlink);

    /* Infrastructure scanning */
    RUN_SUITE(infrascan);

    /* CLI (install, update, config) */
    RUN_SUITE(cli);

    /* System info + worker pool (parallelism) */
    RUN_SUITE(system_info);
    RUN_SUITE(worker_pool);

    /* Parallel pipeline */
    RUN_SUITE(parallel);

    /* mem + arena + slab integration */
    RUN_SUITE(mem);

    /* UI (config, embedded assets, layout) */

    /* UI HTTP server (transport + routing) */

    /* Security defenses */
    RUN_SUITE(security);

    /* YAML parser */
    RUN_SUITE(yaml);

    /* SimHash / SIMILAR_TO */
    RUN_SUITE(simhash);

    /* Stack overflow regression (GitHub #199) */
    RUN_SUITE(stack_overflow);

    /* Integration (end-to-end) */
    RUN_SUITE(integration);

    /* Per-language graph contracts (node/edge types, attribution, no-crash) */
    RUN_SUITE(lang_contract);
    RUN_SUITE(edge_imports);
    RUN_SUITE(edge_structural);
    RUN_SUITE(lsp_resolution_probe);
    RUN_SUITE(node_creation_probe);
    RUN_SUITE(edge_types_probe);
    RUN_SUITE(convergence_probe);
    RUN_SUITE(matrix_known_classes);
    RUN_SUITE(matrix_new_constructs);
    RUN_SUITE(grammar_probe_a);
    RUN_SUITE(grammar_probe_b);
    RUN_SUITE(grammar_probe_c);
    RUN_SUITE(grammar_probe_d);
    RUN_SUITE(grammar_probe_e);
    RUN_SUITE(grammar_probe_f);
    RUN_SUITE(grammar_probe_g);

    RUN_SUITE(incremental);

    /* Release process-lifetime caches so LeakSanitizer reports no leaks. */
    cbm_kind_in_set_free_cache();
    sqlite3_shutdown();
    TEST_SUMMARY();
}
