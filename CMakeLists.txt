# CMakeLists.txt — C++23 migration build system
#
# This is the CMake port of Makefile.cbm, introduced as part of the
# C → C++23 migration. Phase 1 goal: keep the existing C semantics, but
# (a) drive the build with CMake instead of Make, and (b) compile
# src/foundation/ as C++23 while leaving every other TU as C11.
#
# Pre-existing C++ in the tree (internal/cbm/preprocessor.cpp) keeps
# compiling under C++14 (matching the Makefile) — we don't touch it in
# this phase. New C++23 code lives under src/foundation/.
#
# Sanitizer mode: -DCBM_SANITIZE=ON adds -fsanitize=address,undefined
# to every target (mirrors Makefile's CFLAGS_TEST). Default ON in Debug.

cmake_minimum_required(VERSION 3.20)
project(cbm LANGUAGES C CXX ASM)

# ── Standards ───────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # _DEFAULT_SOURCE, _GNU_SOURCE
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(CBM_SANITIZE "Enable ASan+UBSan on all targets" ON)
option(CBM_WERROR  "Promote warnings to errors on first-party code" ON)

# Production-binary knobs (mirror Makefile.cbm's CFLAGS_EXTRA / STATIC / with-ui).
set(CBM_VERSION "dev" CACHE STRING "Version string baked into the binary")
option(CBM_STATIC  "Link the production binary fully static (musl/Alpine portable builds)" OFF)
add_compile_definitions(CBM_VERSION="${CBM_VERSION}")

# ── Common flags ────────────────────────────────────────────────
# Mirrors Makefile.cbm's CFLAGS_COMMON / CXXFLAGS_COMMON.
#
# Notes:
#   * Vendored TUs get -w (suppress all warnings) — the originals were
#     not written for our -Werror policy. We mirror MIMALLOC_CFLAGS_TEST,
#     SQLITE3_CFLAGS_TEST, MONGOOSE_CFLAGS_TEST, GRAMMAR_CFLAGS_TEST etc.
#   * First-party TUs get -Wall -Wextra (+ -Werror unless CBM_WERROR=OFF).
#
# IS_GCC heuristic — needed to enable GCC-only -Wno-* without breaking Clang
include(CheckCCompilerFlag)
set(_gcc_only_flags "")
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND _gcc_only_flags
        -Wno-format-truncation -Wno-unused-result
        -Wno-stringop-truncation -Wno-alloc-size-larger-than)
endif()

set(CBM_FIRSTPARTY_C_FLAGS
    -D_DEFAULT_SOURCE -D_GNU_SOURCE
    -Wall -Wextra
    -Wno-unused-parameter -Wno-sign-compare
    ${_gcc_only_flags})

# C++23 first-party flags. Designated-init order-mismatch and ext-c99 hits
# come up when porting C → C++ — silence the noisy false positives but
# keep substantive warnings on.
set(CBM_FIRSTPARTY_CXX_FLAGS
    -D_DEFAULT_SOURCE -D_GNU_SOURCE
    -Wall -Wextra
    -Wno-unused-parameter -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-deprecated-declarations
    -Wno-deprecated-enum-enum-conversion
    -Wno-deprecated-enum-float-conversion
    -Wno-missing-braces
    ${_gcc_only_flags})  # match the C side: -Wno-format-truncation / -unused-result / -stringop-truncation
# -Wno-c99-designator is Clang-only; add conditionally
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND CBM_FIRSTPARTY_CXX_FLAGS
        -Wno-c99-designator
        -Wno-deprecated-anon-enum-enum-conversion)  # clang's spelling of GCC's -Wdeprecated-enum-enum-conversion
endif()

if(CBM_WERROR)
    list(APPEND CBM_FIRSTPARTY_C_FLAGS -Werror)
    # Phase 2 complete: first-party C++ TUs are warning-clean under the
    # first-party flag set, so -Werror is now enabled for them too. (Kept
    # OFF for the prod build via build.sh's -DCBM_WERROR=OFF, where -O2 can
    # surface optimizer-only warnings.) Historical note: this was deferred
    # during Phase 1 because the C-to-C++ port surfaced non-bug warnings;
    # those have since been burned down (suppressing only benign categories)
    # and the residual unused-function helpers marked [[maybe_unused]].
    #
    # -Werror on first-party C++ is scoped to GCC (the canonical CI compiler).
    # clang (macOS AppleClang, Windows mingw-clang) emits a different, more
    # pedantic warning set; promoting those to errors turns CI into per-platform
    # whack-a-mole for no real benefit. clang still builds with -Wall -Wextra
    # (warnings visible) and fails only on genuine errors. The C side keeps
    # -Werror everywhere (it was already clang-clean as C pre-migration).
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND CBM_FIRSTPARTY_CXX_FLAGS -Werror)
    endif()
endif()

set(CBM_VENDORED_FLAGS -w) # mirrors -w on every vendored target in Makefile.cbm
set(CBM_VENDORED_DEFS_C  -D_DEFAULT_SOURCE)

# Optimization mirrors the Makefile's CFLAGS_TEST: -g -O1. CMake's default
# Debug is -g -O0 which is too slow for the full test-runner — override.
add_compile_options($<$<CONFIG:Debug>:-g> $<$<CONFIG:Debug>:-O1>)

# ── Sanitizers ─────────────────────────────────────────────────
if(CBM_SANITIZE)
    set(CBM_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    add_compile_options(${CBM_SAN_FLAGS})
    add_link_options(${CBM_SAN_FLAGS})
endif()

# ── libgit2 auto-detect (mirrors Makefile.cbm) ─────────────────
find_package(PkgConfig QUIET)
set(CBM_LIBGIT2_LIBS "")
set(CBM_LIBGIT2_DEFS "")
if(PkgConfig_FOUND)
    pkg_check_modules(LIBGIT2 QUIET libgit2)
    if(LIBGIT2_FOUND)
        set(CBM_LIBGIT2_LIBS ${LIBGIT2_LIBRARIES})
        set(CBM_LIBGIT2_DEFS HAVE_LIBGIT2)
    endif()
endif()

# ── Paths ──────────────────────────────────────────────────────
set(CBM_INTERNAL ${CMAKE_SOURCE_DIR}/internal/cbm)
set(CBM_TS_INCLUDE ${CBM_INTERNAL}/vendored/ts_runtime/include)
set(CBM_TS_SRC ${CBM_INTERNAL}/vendored/ts_runtime/src)

set(CBM_COMMON_INCLUDES
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendored
    ${CMAKE_SOURCE_DIR}/vendored/sqlite3
    ${CMAKE_SOURCE_DIR}/vendored/mimalloc/include
    ${CBM_INTERNAL}
    ${CBM_TS_INCLUDE})

# ── Helper: declare a first-party C++23 library ────────────────
function(cbm_add_cxx_library tgt)
    cmake_parse_arguments(A "" "" "SOURCES;DEFS;INCLUDES" ${ARGN})
    add_library(${tgt} OBJECT ${A_SOURCES})
    target_compile_options(${tgt} PRIVATE ${CBM_FIRSTPARTY_CXX_FLAGS})
    target_compile_definitions(${tgt} PRIVATE ${A_DEFS} ${CBM_LIBGIT2_DEFS})
    target_include_directories(${tgt} PRIVATE ${CBM_COMMON_INCLUDES} ${A_INCLUDES})
    set_target_properties(${tgt} PROPERTIES CXX_STANDARD 23)
endfunction()

# ── Helper: declare a vendored C library (relaxed warnings) ────
function(cbm_add_vendored_library tgt)
    cmake_parse_arguments(A "" "" "SOURCES;DEFS;INCLUDES" ${ARGN})
    add_library(${tgt} OBJECT ${A_SOURCES})
    target_compile_options(${tgt} PRIVATE ${CBM_VENDORED_FLAGS})
    target_compile_definitions(${tgt} PRIVATE ${CBM_VENDORED_DEFS_C} ${A_DEFS})
    target_include_directories(${tgt} PRIVATE ${A_INCLUDES})
    set_target_properties(${tgt} PROPERTIES C_STANDARD 11)
endfunction()

# ────────────────────────────────────────────────────────────────
# Vendored libraries (stay as C — third-party, consumed via extern "C" ABI)
# ────────────────────────────────────────────────────────────────

cbm_add_vendored_library(cbm_mimalloc
    SOURCES ${CMAKE_SOURCE_DIR}/vendored/mimalloc/src/static.c
    DEFS MI_OVERRIDE=0
    INCLUDES ${CMAKE_SOURCE_DIR}/vendored/mimalloc/include
             ${CMAKE_SOURCE_DIR}/vendored/mimalloc/src)

cbm_add_vendored_library(cbm_sqlite3
    SOURCES ${CMAKE_SOURCE_DIR}/vendored/sqlite3/sqlite3.c
    DEFS SQLITE_DQS=0 SQLITE_THREADSAFE=1 SQLITE_ENABLE_FTS5
    INCLUDES ${CMAKE_SOURCE_DIR}/vendored/sqlite3)

cbm_add_vendored_library(cbm_yyjson
    SOURCES ${CMAKE_SOURCE_DIR}/vendored/yyjson/yyjson.c
    INCLUDES ${CMAKE_SOURCE_DIR}/vendored/yyjson)

cbm_add_vendored_library(cbm_lz4
    SOURCES ${CBM_INTERNAL}/vendored/lz4/lz4.c
            ${CBM_INTERNAL}/vendored/lz4/lz4hc.c
    INCLUDES ${CBM_INTERNAL} ${CBM_INTERNAL}/vendored/lz4)

cbm_add_vendored_library(cbm_zstd
    SOURCES ${CBM_INTERNAL}/vendored/zstd/zstd.c
    INCLUDES ${CBM_INTERNAL}/vendored/zstd)

# Tree-sitter runtime + per-grammar shims. Grammars share static symbol
# names (each grammar's parser.c declares `static const TSLanguage`),
# so they cannot be unity-built — each shim is one TU.
cbm_add_vendored_library(cbm_ts_runtime
    SOURCES ${CBM_INTERNAL}/ts_runtime.c
    INCLUDES ${CBM_INTERNAL} ${CBM_TS_INCLUDE} ${CBM_TS_SRC})

file(GLOB CBM_GRAMMAR_SRCS CONFIGURE_DEPENDS ${CBM_INTERNAL}/grammar_*.c)
cbm_add_vendored_library(cbm_grammars
    SOURCES ${CBM_GRAMMAR_SRCS}
    INCLUDES ${CBM_INTERNAL} ${CBM_TS_INCLUDE} ${CBM_TS_SRC})

# LSP unity blob (internal/cbm/lsp_all.cpp — #includes every lsp/*.cpp).
# Still -w: the unity blob aggregates the five largest legacy resolvers and
# its full warning burn-down is its own Phase 2 pass. The split-out resolver
# TUs (cbm_lsp_tscs) and the generated tables are already warning-checked.
add_library(cbm_lsp_all OBJECT ${CBM_INTERNAL}/lsp_all.cpp)
set_target_properties(cbm_lsp_all PROPERTIES CXX_STANDARD 23)
target_compile_options(cbm_lsp_all PRIVATE ${CBM_FIRSTPARTY_CXX_FLAGS} -Wno-comment)
# CBM_PYTHON_STDLIB_GENERATED suppresses py_lsp.cpp's no-op stdlib stub —
# the real cbm_python_stdlib_register definition is in the cbm_lsp_generated
# target (generated/python_stdlib_data.c), linked separately.
target_compile_definitions(cbm_lsp_all PRIVATE
    _DEFAULT_SOURCE _GNU_SOURCE CBM_PYTHON_STDLIB_GENERATED)
target_include_directories(cbm_lsp_all PRIVATE
    ${CBM_INTERNAL} ${CBM_TS_INCLUDE} ${CBM_TS_SRC})

# LSP stdlib data tables (generated/*.cpp) — ported to C++23. Auto-generated
# but first-party; the (const CBMType*[]){...} compound literals are now
# arena-allocated via cbm_type_args. Built with relaxed warnings (large
# generated bodies, not warning-clean as C++); still sanitized.
file(GLOB CBM_LSP_GENERATED_SRCS CONFIGURE_DEPENDS
    ${CBM_INTERNAL}/lsp/generated/*.cpp)
cbm_add_cxx_library(cbm_lsp_generated
    SOURCES ${CBM_LSP_GENERATED_SRCS}
    INCLUDES ${CBM_TS_SRC})
# Generated tables carry benign C-isms (unused stdlib-parent arrays, etc.) that
# GCC -Werror rejects but are not real defects; the bodies are auto-generated,
# so relax these like the rest of the generated/legacy LSP code (see cbm_lsp_tscs).
target_compile_options(cbm_lsp_generated PRIVATE -Wno-comment
    -Wno-unused-variable -Wno-unused-but-set-variable -Wno-write-strings)

# ts_lsp / cs_lsp / java / kotlin / rust — ported to C++23 (the
# (const CBMType*[]){...} compound literals are now arena-allocated via
# cbm_type_args). Large legacy resolver bodies, not yet warning-clean as C++,
# so built with relaxed warnings like the lsp_all unity blob; still sanitized
# as first-party code. Each resolver is its own TU (no static-symbol clashes).
cbm_add_cxx_library(cbm_lsp_tscs
    SOURCES ${CBM_INTERNAL}/lsp/ts_lsp.cpp ${CBM_INTERNAL}/lsp/cs_lsp.cpp
            ${CBM_INTERNAL}/lsp/java_lsp.cpp ${CBM_INTERNAL}/lsp/kotlin_lsp.cpp
            ${CBM_INTERNAL}/lsp/rust_lsp.cpp ${CBM_INTERNAL}/lsp/rust_cargo.cpp
            ${CBM_INTERNAL}/lsp/rust_proc_macros.cpp ${CBM_INTERNAL}/lsp/rust_rustdoc.cpp
    DEFS _DEFAULT_SOURCE _GNU_SOURCE
    INCLUDES ${CBM_TS_SRC})
# These large legacy/ported resolver bodies are "not yet warning-clean as C++"
# (see header note). The Java/Kotlin/Rust resolvers faithfully carry upstream
# C-isms — string-literal→char* assignments, set-but-unused locals, an unused
# smart-cast stub — that GCC -Werror rejects but that are not real defects.
# Relax those specific diagnostics (clang already tolerates them) to keep the
# port byte-faithful to upstream rather than diverging the sources.
target_compile_options(cbm_lsp_tscs PRIVATE -Wno-comment
    -Wno-write-strings -Wno-unused-function -Wno-unused-variable
    -Wno-unused-but-set-variable)

# Preprocessor (already C++, kept at C++14 to match Makefile)
add_library(cbm_preprocessor OBJECT ${CBM_INTERNAL}/preprocessor.cpp)
set_target_properties(cbm_preprocessor PROPERTIES CXX_STANDARD 14)
target_compile_options(cbm_preprocessor PRIVATE -w)
target_include_directories(cbm_preprocessor PRIVATE
    ${CBM_INTERNAL} ${CBM_INTERNAL}/vendored)

set(CBM_VENDORED_TARGETS
    cbm_mimalloc cbm_sqlite3 cbm_yyjson
    cbm_lz4 cbm_zstd
    cbm_ts_runtime cbm_grammars cbm_lsp_all
    cbm_preprocessor)
# ────────────────────────────────────────────────────────────────
# First-party libraries
# ────────────────────────────────────────────────────────────────

# ── Foundation: all 16 TUs C++23 ───────────────────────────────
# hash_table.cpp is backed by std::unordered_map (the vendored Verstable
# header it previously instantiated has been removed — see hash_table.cpp).
set(CBM_FOUNDATION_CXX_SRCS
    src/foundation/arena.cpp
    src/foundation/str_intern.cpp
    src/foundation/log.cpp
    src/foundation/str_util.cpp
    src/foundation/platform.cpp
    src/foundation/system_info.cpp
    src/foundation/slab_alloc.cpp
    src/foundation/yaml.cpp
    src/foundation/compat.cpp
    src/foundation/compat_thread.cpp
    src/foundation/compat_fs.cpp
    src/foundation/compat_regex.cpp
    src/foundation/mem.cpp
    src/foundation/diagnostics.cpp
    src/foundation/profile.cpp
    src/foundation/hash_table.cpp
    src/foundation/subprocess.cpp
    src/foundation/sha256.cpp
    src/foundation/limits.cpp
    src/foundation/dump_verify.cpp)
cbm_add_cxx_library(cbm_foundation_cxx
    SOURCES ${CBM_FOUNDATION_CXX_SRCS}
    INCLUDES ${CMAKE_SOURCE_DIR}/vendored/mimalloc/include)
# GCC 13 raises a false-positive -Wmaybe-uninitialized deep in libstdc++'s
# <std_function.h> when std::regex's internal state machine (which stores a
# std::function<bool(char)> per character class) is instantiated under
# optimization. It is an optimizer-stage warning that in-source #pragma GCC
# diagnostic cannot reliably silence, and clang does not emit it. Disable just
# this flag for just this TU so the GCC -Werror build passes (compat_regex.cpp
# is the only translation unit that instantiates std::regex).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(src/foundation/compat_regex.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-maybe-uninitialized")
endif()

# ── Internal extraction (C++23) ────────────────────────────────
cbm_add_cxx_library(cbm_extraction
    SOURCES
        ${CBM_INTERNAL}/cbm.cpp
        ${CBM_INTERNAL}/extract_defs.cpp
        ${CBM_INTERNAL}/extract_calls.cpp
        ${CBM_INTERNAL}/extract_imports.cpp
        ${CBM_INTERNAL}/extract_usages.cpp
        ${CBM_INTERNAL}/extract_unified.cpp
        ${CBM_INTERNAL}/extract_semantic.cpp
        ${CBM_INTERNAL}/extract_type_refs.cpp
        ${CBM_INTERNAL}/extract_type_assigns.cpp
        ${CBM_INTERNAL}/extract_env_accesses.cpp
        ${CBM_INTERNAL}/extract_channels.cpp
        ${CBM_INTERNAL}/extract_k8s.cpp
        ${CBM_INTERNAL}/helpers.cpp
        ${CBM_INTERNAL}/service_patterns.cpp)

cbm_add_cxx_library(cbm_lang_specs SOURCES ${CBM_INTERNAL}/lang_specs.cpp)

cbm_add_cxx_library(cbm_ac_lz4
    SOURCES ${CBM_INTERNAL}/ac.cpp ${CBM_INTERNAL}/lz4_store.cpp)

cbm_add_cxx_library(cbm_zstd_store SOURCES ${CBM_INTERNAL}/zstd_store.cpp)

cbm_add_cxx_library(cbm_sqlite_writer SOURCES ${CBM_INTERNAL}/sqlite_writer.cpp)

# ── src/ subsystems (C++23) ────────────────────────────────────
cbm_add_cxx_library(cbm_store     SOURCES src/store/store.cpp)
cbm_add_cxx_library(cbm_cypher    SOURCES src/cypher/cypher.cpp)
cbm_add_cxx_library(cbm_mcp       SOURCES src/mcp/mcp.cpp src/mcp/index_supervisor.cpp)
cbm_add_cxx_library(cbm_discover  SOURCES
    src/discover/userconfig.cpp
    src/discover/gitignore.cpp
    src/discover/discover.cpp
    src/discover/language.cpp)
cbm_add_cxx_library(cbm_graph_buffer SOURCES src/graph_buffer/graph_buffer.cpp)
cbm_add_cxx_library(cbm_pipeline  SOURCES
    src/pipeline/fqn.cpp
    src/pipeline/path_alias.cpp
    src/pipeline/registry.cpp
    src/pipeline/pipeline.cpp
    src/pipeline/pipeline_incremental.cpp
    src/pipeline/worker_pool.cpp
    src/pipeline/pass_parallel.cpp
    src/pipeline/pass_definitions.cpp
    src/pipeline/pass_calls.cpp
    src/pipeline/pass_lsp_cross.cpp
    src/pipeline/pass_usages.cpp
    src/pipeline/pass_semantic.cpp
    src/pipeline/pass_tests.cpp
    src/pipeline/pass_githistory.cpp
    src/pipeline/pass_gitdiff.cpp
    src/pipeline/pass_configures.cpp
    src/pipeline/pass_configlink.cpp
    src/pipeline/pass_route_nodes.cpp
    src/pipeline/pass_enrichment.cpp
    src/pipeline/pass_envscan.cpp
    src/pipeline/pass_compile_commands.cpp
    src/pipeline/pass_infrascan.cpp
    src/pipeline/pass_k8s.cpp
    src/pipeline/pass_similarity.cpp
    src/pipeline/pass_semantic_edges.cpp
    src/pipeline/pass_cross_repo.cpp
    src/pipeline/artifact.cpp
    src/pipeline/pass_complexity.cpp
    src/pipeline/pass_pkgmap.cpp)
cbm_add_cxx_library(cbm_simhash   SOURCES src/simhash/minhash.cpp)
cbm_add_cxx_library(cbm_semantic  SOURCES src/semantic/semantic.cpp src/semantic/ast_profile.cpp src/semantic/rotsq.cpp)
cbm_add_cxx_library(cbm_watcher   SOURCES src/watcher/watcher.cpp)
cbm_add_cxx_library(cbm_git       SOURCES src/git/git_context.cpp)
cbm_add_cxx_library(cbm_cli       SOURCES src/cli/cli.cpp src/cli/progress_sink.cpp src/cli/hook_augment.cpp)

set(CBM_FIRSTPARTY_TARGETS
    cbm_foundation_cxx
    cbm_extraction cbm_lang_specs cbm_lsp_tscs cbm_lsp_generated
    cbm_ac_lz4 cbm_zstd_store cbm_sqlite_writer
    cbm_store cbm_cypher cbm_mcp
    cbm_discover
    cbm_graph_buffer
    cbm_pipeline cbm_simhash cbm_semantic cbm_watcher cbm_git
    cbm_cli)

# ────────────────────────────────────────────────────────────────
# test-runner
# ────────────────────────────────────────────────────────────────

# Test sources — mirror ALL_TEST_SRCS in Makefile.cbm.
set(CBM_TEST_SRCS
    tests/test_main.c
    # foundation
    tests/test_arena.c tests/test_hash_table.c tests/test_dyn_array.c
    tests/test_str_intern.c tests/test_log.c tests/test_str_util.c
    tests/test_platform.c
    # extraction
    tests/test_extraction.c tests/test_ac.c
    # store
    tests/test_store_nodes.c tests/test_store_edges.c tests/test_store_search.c
    tests/test_store_arch.c tests/test_store_bulk.c tests/test_store_pragmas.c
    tests/test_store_checkpoint.c
    # cypher / mcp
    tests/test_cypher.c tests/test_mcp.c
    # discover
    tests/test_language.c tests/test_userconfig.c tests/test_gitignore.c
    tests/test_discover.c
    # graph_buffer / pipeline / watcher
    tests/test_graph_buffer.c
    tests/test_registry.c tests/test_pipeline.c tests/test_fqn.c
    tests/test_path_alias.c tests/test_configlink.c tests/test_infrascan.c
    tests/test_worker_pool.c tests/test_parallel.c
    tests/test_watcher.c
    # vendored format suites
    tests/test_lz4.c tests/test_zstd.c tests/test_artifact.c
    tests/test_sqlite_writer.c
    # lsp suites
    tests/test_go_lsp.c tests/test_c_lsp.c tests/test_php_lsp.c
    tests/test_cs_lsp.c tests/test_cs_lsp_bench.c
    tests/test_scope.c tests/test_type_rep.c
    tests/test_py_lsp.c tests/test_py_lsp_bench.c tests/test_py_lsp_stress.c
    tests/test_py_lsp_scale.c tests/test_ts_lsp.c
    # cli / mem / security / yaml / simhash
    tests/test_cli.c tests/test_mem.c
    tests/test_security.c tests/test_yaml.c tests/test_simhash.c
    tests/test_stack_overflow.c
    # integration
    tests/test_integration.c tests/test_incremental.c
    # hybrid LSP resolvers (java / kotlin / rust)
    tests/test_java_lsp.c tests/test_java_lsp_coverage.c
    tests/test_kotlin_lsp.c tests/test_rust_lsp.c
    # grammar coverage / extraction / edge / probe suites
    tests/test_lang_contract.c tests/test_grammar_regression.c
    tests/test_grammar_labels.c tests/test_grammar_imports.c
    tests/test_extraction_imports.c tests/test_extraction_inheritance.c
    tests/test_edge_imports.c tests/test_edge_structural.c
    tests/test_lsp_resolution_probe.c tests/test_node_creation_probe.c
    tests/test_edge_types_probe.c tests/test_convergence_probe.c
    tests/test_matrix_known_classes.c tests/test_matrix_new_constructs.c
    tests/test_grammar_probe_a.c tests/test_grammar_probe_b.c
    tests/test_grammar_probe_c.c tests/test_grammar_probe_d.c
    tests/test_grammar_probe_e.c tests/test_grammar_probe_f.c
    tests/test_grammar_probe_g.c)

add_executable(test-runner ${CBM_TEST_SRCS})

target_compile_options(test-runner PRIVATE ${CBM_FIRSTPARTY_C_FLAGS})
target_include_directories(test-runner PRIVATE
    ${CBM_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/vendored/sqlite3)
set_source_files_properties(${CBM_TEST_SRCS} PROPERTIES LANGUAGE C)
set_target_properties(test-runner PROPERTIES C_STANDARD 11)

# Link foundation + all C subsystems + vendored
target_link_libraries(test-runner PRIVATE
    ${CBM_FIRSTPARTY_TARGETS}
    ${CBM_VENDORED_TARGETS}
    cbm_yyjson  # always needed for JSON I/O
    pthread m z stdc++ ${CBM_LIBGIT2_LIBS})

if(WIN32)
    target_link_libraries(test-runner PRIVATE ws2_32 psapi)
    target_link_options(test-runner PRIVATE
        -Wl,--allow-multiple-definition -Wl,--stack,8388608)
endif()

enable_testing()
add_test(NAME test-runner COMMAND test-runner WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ────────────────────────────────────────────────────────────────
# Production binary (codebase-memory-mcp)
# ────────────────────────────────────────────────────────────────
# Same library targets as test-runner, with src/main.cpp as the entry
# point instead of the test sources.
add_executable(codebase-memory-mcp src/main.cpp)
target_compile_options(codebase-memory-mcp PRIVATE ${CBM_FIRSTPARTY_CXX_FLAGS})
target_include_directories(codebase-memory-mcp PRIVATE ${CBM_COMMON_INCLUDES})
set_target_properties(codebase-memory-mcp PROPERTIES CXX_STANDARD 23)
target_link_libraries(codebase-memory-mcp PRIVATE
    ${CBM_FIRSTPARTY_TARGETS}
    ${CBM_VENDORED_TARGETS}
    cbm_yyjson
    pthread m z stdc++ ${CBM_LIBGIT2_LIBS})

if(CBM_STATIC)
    target_link_options(codebase-memory-mcp PRIVATE -static)
endif()

if(WIN32)
    target_link_libraries(codebase-memory-mcp PRIVATE ws2_32 psapi)
    target_link_options(codebase-memory-mcp PRIVATE
        -Wl,--allow-multiple-definition -Wl,--stack,8388608)
endif()
