#include "cbm.h"
#include "arena.h" // CBMArena, cbm_arena_init/alloc/strdup/destroy
#include "helpers.h"
#include "lang_specs.h"
#include "extract_unified.h"
#include "lsp/go_lsp.h"
#include "lsp/c_lsp.h"
#include "lsp/php_lsp.h"
#include "lsp/py_lsp.h"
#include "lsp/ts_lsp.h"
#include "lsp/cs_lsp.h"
#include "lsp/java_lsp.h"
#include "lsp/kotlin_lsp.h"
#include "lsp/rust_lsp.h"
#include "preprocessor.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"  // cbm_fopen — crash-supervisor per-file marker write
#include "foundation/hash_table.h" // CBMHashTable — crash-supervisor quarantine set
#include "tree_sitter/api.h" // TSParser, TSNode, TSTree, TSInput, TSLanguage, TSPoint, TSParseOptions, TSParseState
#include "foundation/constants.h"
#include <mimalloc.h> // mi_malloc/mi_calloc/mi_realloc/mi_free/mi_usable_size — bind 3rd-party allocators (#424)
#if defined(CBM_BIND_TS_ALLOCATOR) && CBM_BIND_TS_ALLOCATOR
#include <sqlite3.h> // sqlite3_mem_methods, sqlite3_config, SQLITE_CONFIG_MALLOC — bind sqlite to mimalloc
#endif
#include <stdint.h> // uint32_t, uint64_t, int64_t
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h> // struct timespec, CLOCK_MONOTONIC
#include <string>
#include <string_view>
#include <unordered_set>
#include <algorithm>
#include <vector>

#include "foundation/cbm_atomic.h"

// Keep source ranges only for the current raw or preprocessed walk. A known
// primitive range can discharge its own pending work once, never another site.
struct CPPPendingOperators {
    struct Site {
        uint64_t range;
        bool discharged;
    };
    CBMFileResult *result;
    std::vector<Site> sites;
    bool prepared = false;
    CPPPendingOperators(CBMFileResult *r, bool enabled) : result(enabled ? r : nullptr) {
        if (result)
            result->cpp_operator_tracker = this;
    }
    void prepare() {
        std::sort(sites.begin(), sites.end(),
                  [](const Site &a, const Site &b) { return a.range < b.range; });
        prepared = true;
    }
    void finish() {
        if (result && result->cpp_operator_tracker == this)
            result->cpp_operator_tracker = nullptr;
    }
    ~CPPPendingOperators() {
        finish();
    }
};

void cbm_track_deferred_cpp_operator(CBMFileResult *result, uint32_t start, uint32_t end) {
    ++result->pending_cpp_operator_count;
    if (auto *tracker = static_cast<CPPPendingOperators *>(result->cpp_operator_tracker))
        tracker->sites.push_back({((uint64_t)start << 32) | end, false});
}

void cbm_discharge_builtin_cpp_operator(CBMFileResult *result, uint32_t start, uint32_t end) {
    if (!result)
        return;
    auto *tracker = static_cast<CPPPendingOperators *>(result->cpp_operator_tracker);
    if (!tracker || !tracker->prepared)
        return;
    uint64_t key = ((uint64_t)start << 32) | end;
    auto found = std::lower_bound(
        tracker->sites.begin(), tracker->sites.end(), key,
        [](const CPPPendingOperators::Site &site, uint64_t range) { return site.range < range; });
    if (found != tracker->sites.end() && found->range == key && !found->discharged) {
        found->discharged = true;
        --result->pending_cpp_operator_count;
    }
}

int cbm_materialize_deferred_cpp_operators(CBMFileResult *result) {
    if (!result || result->deferred_cpp_operator_count == 0)
        return 0;
    std::unordered_set<std::string> seen;
    auto key = [](const char *caller, std::string_view name, uint32_t byte) {
        std::string value(caller ? caller : "");
        value.push_back('\0');
        value.append(name);
        value.push_back('\0');
        value.append(std::to_string(byte));
        return value;
    };
    for (int i = 0; i < result->calls.count; ++i) {
        const auto &call = result->calls.items[i];
        if (call.requires_typed_resolution && call.callee_name)
            seen.insert(key(call.enclosing_func_qn, call.callee_name, call.source_byte));
    }
    int added = 0;
    for (int i = 0; i < result->resolved_calls.count; ++i) {
        const auto &resolved = result->resolved_calls.items[i];
        if (!resolved.binary_operator_line || !resolved.source_byte || !resolved.caller_qn ||
            !resolved.callee_qn)
            continue;
        // C++ member QNs use dots, with occasional :: in namespace prefixes.
        const char *leaf = strrchr(resolved.callee_qn, '.');
        leaf = leaf ? leaf + 1 : resolved.callee_qn;
        for (const char *scope = strstr(leaf, "::"); scope; scope = strstr(leaf, "::"))
            leaf = scope + 2;
        if (strncmp(leaf, "operator", 8) != 0)
            continue;
        const char *suffix = strstr(leaf, "@overload_");
        std::string_view name(leaf, suffix ? (size_t)(suffix - leaf) : strlen(leaf));
        if (!seen.insert(key(resolved.caller_qn, name, resolved.source_byte)).second)
            continue;
        CBMCall call = {0};
        call.callee_name = cbm_arena_strndup(&result->arena, name.data(), name.size());
        call.enclosing_func_qn = resolved.caller_qn;
        call.start_line = (int)resolved.binary_operator_line;
        call.source_byte = resolved.source_byte;
        call.requires_typed_resolution = true;
        cbm_calls_push(&result->calls, &result->arena, call);
        ++added;
    }
    return added;
}

// Atomic counters for profiling parse vs extraction time (nanoseconds).
static std::atomic<uint64_t> total_parse_ns = 0;
static std::atomic<uint64_t> total_extract_ns = 0;
static std::atomic<uint64_t> total_lsp_ns = 0;
static std::atomic<uint64_t> total_preprocess_ns = 0;
static std::atomic<uint64_t> total_files_preprocessed = 0;

/* Arena bytes attributed to each stage of cbm_extract_file, so the pipeline's
 * memory profile can say WHICH stage fills the per-file result arena that the
 * whole run then holds. Read via cbm_get_arena_stage_bytes. */
static std::atomic<uint64_t> arena_bytes_parse = 0;    /* module QN, error strings */
static std::atomic<uint64_t> arena_bytes_extract = 0;  /* defs/imports/unified walk */
static std::atomic<uint64_t> arena_bytes_lsp = 0;      /* per-file LSP scratch (freed per file) */
static std::atomic<uint64_t> arena_bytes_lsp_kept = 0; /* what the LSP leaves on the result */
static std::atomic<uint64_t> arena_bytes_pp = 0;       /* C/C++ preprocessed second pass */

void cbm_get_arena_stage_bytes(uint64_t *parse, uint64_t *extract, uint64_t *lsp,
                               uint64_t *lsp_kept, uint64_t *pp) {
    if (parse) {
        *parse = atomic_load(&arena_bytes_parse);
    }
    if (extract) {
        *extract = atomic_load(&arena_bytes_extract);
    }
    if (lsp) {
        *lsp = atomic_load(&arena_bytes_lsp);
    }
    if (lsp_kept) {
        *lsp_kept = atomic_load(&arena_bytes_lsp_kept);
    }
    if (pp) {
        *pp = atomic_load(&arena_bytes_pp);
    }
}

static std::atomic<uint64_t> total_files = 0;

// C/C++ preprocessor #define macros are extracted as Macro nodes (#375). On a
// macro-dense codebase (e.g. the Linux kernel: ~2.4M macros, 49% of all nodes)
// this is the dominant extraction cost, so it is gated to the full/advanced
// index modes. Default ON to preserve behavior for direct callers/tests; the
// pipeline sets it from the index mode before extraction. Set once pre-extract,
// read-only during, so a relaxed atomic is sufficient.
static std::atomic<int> g_extract_macros = 1;
void cbm_set_macro_extraction(int enabled) {
    atomic_store_explicit(&g_extract_macros, enabled ? 1 : 0, memory_order_relaxed);
}
int cbm_macro_extraction_enabled(void) {
    return atomic_load_explicit(&g_extract_macros, memory_order_relaxed);
}

#define NSEC_PER_SEC 1000000000ULL
#define USEC_TO_NSEC 1000ULL
/* Use compat.h's cbm_clock_gettime which accepts CLOCK_MONOTONIC (value
 * varies by platform: 1 on Linux/Windows, 6 on macOS). We pass the
 * platform value via the compat.h fallback. */
#if defined(CLOCK_MONOTONIC)
#define CBM_CLOCK_MONO CLOCK_MONOTONIC
#elif defined(__APPLE__)
#define CBM_CLOCK_MONO 6
#else
#define CBM_CLOCK_MONO 1
#endif

static uint64_t now_ns(void) {
    struct timespec ts;
    cbm_clock_gettime(CBM_CLOCK_MONO, &ts);
    return ((uint64_t)ts.tv_sec * NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

// cbm_get_profile returns accumulated parse/extract times and file count.
void cbm_get_profile(cbm_profile_out_t out) {
    *out.parse_ns = atomic_load(&total_parse_ns);
    *out.extract_ns = atomic_load(&total_extract_ns);
    *out.files = atomic_load(&total_files);
}

uint64_t cbm_get_lsp_ns(void) {
    return atomic_load(&total_lsp_ns);
}

uint64_t cbm_get_preprocess_ns(void) {
    return atomic_load(&total_preprocess_ns);
}

uint64_t cbm_get_files_preprocessed(void) {
    return atomic_load(&total_files_preprocessed);
}

// cbm_reset_profile zeros the profiling counters.
void cbm_reset_profile(void) {
    atomic_store(&total_parse_ns, 0);
    atomic_store(&total_extract_ns, 0);
    atomic_store(&total_lsp_ns, 0);
    atomic_store(&total_preprocess_ns, 0);
    atomic_store(&total_files_preprocessed, 0);
    atomic_store(&total_files, 0);
}

// --- Growable array push functions ---

#define GROW_ARRAY(arr, arena)                                                                 \
    do {                                                                                       \
        if ((arr)->count >= (arr)->cap) {                                                      \
            int new_cap = (arr)->cap == 0 ? CBM_SZ_32 : (arr)->cap * PAIR_LEN;                 \
            void *new_items =                                                                  \
                (void *)cbm_arena_alloc((arena), (size_t)new_cap * sizeof(*(arr)->items));     \
            if (!new_items)                                                                    \
                return;                                                                        \
            if ((arr)->items && (arr)->count > 0) {                                            \
                memcpy(new_items, (arr)->items, (size_t)(arr)->count * sizeof(*(arr)->items)); \
            }                                                                                  \
            (arr)->items = (__typeof__((arr)->items))new_items;                                \
            (arr)->cap = new_cap;                                                              \
        }                                                                                      \
    } while (0)

void cbm_defs_push(CBMDefArray *arr, CBMArena *a, CBMDefinition def) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = def;
}

void cbm_calls_push(CBMCallArray *arr, CBMArena *a, CBMCall call) {
    // Call records dominate large-file extraction. Reclaim superseded array
    // buffers instead of retaining every geometric copy in the bump arena.
    if (arr->count >= arr->cap) {
        if (arr->cap > INT_MAX / PAIR_LEN) {
            return;
        }
        int new_cap = arr->cap == 0 ? CBM_SZ_32 : arr->cap * PAIR_LEN;
        if ((size_t)new_cap > SIZE_MAX / sizeof(*arr->items)) {
            return;
        }
        auto *items =
            (CBMCall *)cbm_arena_grow_buffer(a, arr->items, (size_t)new_cap * sizeof(*arr->items));
        if (!items) {
            // LSP passes can append through a different scratch arena, and
            // scratch reclamation can copy arrays back into bump storage.
            // Preserve that ownership-transfer path without reallocating a
            // buffer owned by another arena or a slice of a bump block.
            GROW_ARRAY(arr, a);
        } else {
            arr->items = items;
            arr->cap = new_cap;
        }
    }
    arr->items[arr->count++] = call;
}

void cbm_imports_push(CBMImportArray *arr, CBMArena *a, CBMImport imp) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = imp;
}

void cbm_usages_push(CBMUsageArray *arr, CBMArena *a, CBMUsage usage) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = usage;
}

void cbm_throws_push(CBMThrowArray *arr, CBMArena *a, CBMThrow thr) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = thr;
}

void cbm_rw_push(CBMRWArray *arr, CBMArena *a, CBMReadWrite rw) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = rw;
}

void cbm_typerefs_push(CBMTypeRefArray *arr, CBMArena *a, CBMTypeRef tr) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = tr;
}

void cbm_envaccess_push(CBMEnvAccessArray *arr, CBMArena *a, CBMEnvAccess ea) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ea;
}

void cbm_typeassign_push(CBMTypeAssignArray *arr, CBMArena *a, CBMTypeAssign ta) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ta;
}

void cbm_stringref_push(CBMStringRefArray *arr, CBMArena *a, CBMStringRef sr) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = sr;
}

void cbm_infrabinding_push(CBMInfraBindingArray *arr, CBMArena *a, CBMInfraBinding ib) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ib;
}

void cbm_impltrait_push(CBMImplTraitArray *arr, CBMArena *a, CBMImplTrait it) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = it;
}

void cbm_resolvedcall_push(CBMResolvedCallArray *arr, CBMArena *a, CBMResolvedCall rc) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = rc;
}

void cbm_channels_push(CBMChannelArray *arr, CBMArena *a, CBMChannel ch) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ch;
}

// --- String input reader (for parse_with_options) ---

typedef struct {
    const char *string;
    uint32_t length;
} CBMStringInput;

static const char *cbm_string_read(void *payload, uint32_t byte, TSPoint point,
                                   uint32_t *bytes_read) {
    (void)point;
    CBMStringInput *self = (CBMStringInput *)payload;
    if (byte >= self->length) {
        *bytes_read = 0;
        return "";
    }
    *bytes_read = self->length - byte;
    return self->string + byte;
}

// --- Parse timeout callback ---

/* Budget for the tree-sitter progress callback. The PRIMARY gate is per-thread
 * CPU time: a worker descheduled under contention burns WALL time but not CPU,
 * so a starved-but-parseable file must NOT be abandoned merely because the
 * budget elapsed in wall-clock against near-zero CPU. That false "parse
 * timeout" silently dropped a file's defs and, with them, every cross-file edge
 * those defs anchored. A generous WALL ceiling stays as a backstop so a
 * genuinely stuck/spinning parse still terminates in bounded time. */
enum { CBM_PARSE_WALL_CEILING_FACTOR = 12 }; /* ~60 s ceiling for the 5 s CPU budget */

typedef struct {
    uint64_t cpu_deadline_ns;   // trip once this thread's CPU time passes it
    uint64_t cpu_idle_until_ns; // wall time before which the CPU budget cannot be spent
    uint64_t wall_ceiling_ns;   // hard wall backstop for a spinning/stuck parse
} CBMParseBudget;

#ifdef CBM_ENABLE_TEST_SEAMS
/* Deterministic RED-repro seam (armed by CBM_TEST_WALL_STALL_ON): when set, the
 * timeout callback reads the WALL clock this many ns ahead of reality while CPU
 * time is untouched, emulating a worker descheduled long enough for a wall-only
 * budget to elapse. Thread-local so it cannot leak across worker threads. */
static thread_local uint64_t tl_parse_wall_seam_offset_ns = 0;
#endif

/* tree-sitter's TSProgressCallback mandates a non-const TSParseState*. */
// cppcheck-suppress constParameterCallback
static bool cbm_timeout_cb(TSParseState *state) {
    const CBMParseBudget *budget = (const CBMParseBudget *)state->payload;
    uint64_t real_wall = now_ns();
    uint64_t wall = real_wall;
#ifdef CBM_ENABLE_TEST_SEAMS
    wall += tl_parse_wall_seam_offset_ns;
#endif
    if (wall > budget->wall_ceiling_ns) {
        return true;
    }
    /* A thread cannot burn more CPU than the wall time that passed, so until the
     * budget has elapsed in real wall time the CPU clock (a syscall) need not be
     * read. The seam offset is deliberately not applied here: it fakes wall time,
     * not CPU time. */
    if (real_wall <= budget->cpu_idle_until_ns) {
        return false;
    }
    return cbm_thread_cpu_time_ns() > budget->cpu_deadline_ns;
}

// --- Thread-local parser pool ---
// TSParser is not thread-safe, but can be reused across files on the same thread.
// We keep one parser per thread, and just switch language as needed.
// This avoids ~70K ts_parser_new()/ts_parser_delete() cycles on large repos.

static CBM_TLS TSParser *tl_parser = NULL;
static CBM_TLS CBMLanguage tl_parser_lang = CBM_LANG_COUNT; // invalid sentinel

// Get or create a thread-local parser configured for the given language.
static TSParser *get_thread_parser(const TSLanguage *ts_lang, CBMLanguage lang) {
    if (!tl_parser) {
        tl_parser = ts_parser_new();
        if (!tl_parser) {
            return NULL;
        }
        tl_parser_lang = CBM_LANG_COUNT;
    }
    if (tl_parser_lang != lang) {
        ts_parser_set_language(tl_parser, ts_lang);
        tl_parser_lang = lang;
    }
    return tl_parser;
}

// --- Allocator binding (defense-in-depth, #424) ---

/* Bind tree-sitter and sqlite3 to mimalloc explicitly so a correct
 * binary does NOT depend on the fragile MI_OVERRIDE symbol override. Under
 * MI_OVERRIDE=1 — particularly the Windows static-MinGW link with
 * --allow-multiple-definition — `malloc`/`free` can resolve to DIFFERENT
 * allocators (mimalloc vs the CRT) inside third-party libs, so a block
 * allocated by mimalloc gets freed by the CRT (or vice-versa), corrupting the
 * heap freelist (#424). Binding each library through one explicit allocator
 * eliminates that mismatch class generically, on every platform.
 *
 * Guarded to the production build (CBM_BIND_TS_ALLOCATOR=1, which CFLAGS_PROD
 * defines alongside MI_OVERRIDE=1). The test build is CRT + ASan, where binding
 * to mimalloc would mismatch ASan/CRT frees — there these binds compile to
 * no-ops and the build stays unchanged. */

#if defined(CBM_BIND_TS_ALLOCATOR) && CBM_BIND_TS_ALLOCATOR
#include <assert.h>

/* sqlite3 mem methods backed by mimalloc. sqlite's xMalloc/xRealloc/xSize use
 * `int` sizes; wrap with size_t casts. xRoundup rounds to an 8-byte boundary
 * (sqlite requires 8-byte-aligned roundup, and mimalloc honors that alignment).
 * Field order matches struct sqlite3_mem_methods exactly:
 * xMalloc, xFree, xRealloc, xSize, xRoundup, xInit, xShutdown, pAppData. */
static void *cbm_sqlite_malloc(int n) {
    return mi_malloc((size_t)n);
}
static void cbm_sqlite_free(void *p) {
    mi_free(p);
}
static void *cbm_sqlite_realloc(void *p, int n) {
    return mi_realloc(p, (size_t)n);
}
static int cbm_sqlite_size(void *p) {
    return (int)mi_usable_size(p);
}
static int cbm_sqlite_roundup(int n) {
    return (n + 7) & ~7; /* round up to 8-byte boundary */
}
static int cbm_sqlite_meminit(void *appdata) {
    (void)appdata;
    return SQLITE_OK;
}
static void cbm_sqlite_memshutdown(void *appdata) {
    (void)appdata;
}
#endif /* CBM_BIND_TS_ALLOCATOR */

void cbm_alloc_init(void) {
#if defined(CBM_BIND_TS_ALLOCATOR) && CBM_BIND_TS_ALLOCATOR
    static int alloc_bound = 0; /* single-threaded startup; plain int is fine */
    if (alloc_bound) {
        return;
    }
    alloc_bound = 1;

    /* tree-sitter runtime (was previously bound in cbm_init; consolidated here). */
    ts_set_allocator(mi_malloc, mi_calloc, mi_realloc, mi_free);

    /* sqlite3. SQLITE_CONFIG_MALLOC MUST run before sqlite3_initialize / the
     * first sqlite3_open* — otherwise sqlite3_config returns SQLITE_MISUSE
     * silently and the binding is ignored. cbm_alloc_init() runs as the very
     * first statement of main(), before cbm_mcp_server_new → cbm_store_open*. */
    static sqlite3_mem_methods cbm_sqlite_mem = {
        cbm_sqlite_malloc,      /* xMalloc */
        cbm_sqlite_free,        /* xFree */
        cbm_sqlite_realloc,     /* xRealloc */
        cbm_sqlite_size,        /* xSize */
        cbm_sqlite_roundup,     /* xRoundup */
        cbm_sqlite_meminit,     /* xInit */
        cbm_sqlite_memshutdown, /* xShutdown */
        NULL,                   /* pAppData */
    };
    int sqlite_rc = sqlite3_config(SQLITE_CONFIG_MALLOC, &cbm_sqlite_mem);
    assert(sqlite_rc == SQLITE_OK && "SQLITE_CONFIG_MALLOC must run before sqlite3_initialize");
    (void)sqlite_rc;
#endif /* CBM_BIND_TS_ALLOCATOR */
}

// --- Init/Shutdown ---

static int cbm_initialized = 0;

int cbm_init(void) {
    if (cbm_initialized) {
        return 0;
    }
    enum { CBM_INIT_DONE = 1 };
    cbm_initialized = CBM_INIT_DONE;
    /* Defense-in-depth allocator binds (idempotent). main() calls cbm_alloc_init
     * first; this covers non-main entry points (pipeline passes call cbm_init).
     * For sqlite the SQLITE_CONFIG_MALLOC bind only takes effect if it runs
     * before sqlite initializes — main() guarantees that ordering; here it is a
     * best-effort idempotent re-assert for paths that never hit main(). */
    cbm_alloc_init();
    return 0;
}

void cbm_reset_thread_parser(void) {
    // Release parser's internal slab-allocated subtrees (stack, cached token).
    // Must be called BEFORE cbm_slab_reset_thread() to avoid corrupting
    // live slab chunks that the parser still references.
    if (tl_parser) {
        ts_parser_reset(tl_parser);
    }
}

void cbm_destroy_thread_parser(void) {
    // Full cleanup: delete the parser. Call on worker thread exit.
    if (tl_parser) {
        ts_parser_delete(tl_parser);
        tl_parser = NULL;
        tl_parser_lang = CBM_LANG_COUNT;
    }
}

void cbm_shutdown(void) {
    // Clean up thread-local parser for the calling thread.
    // Note: other threads' TLS parsers are freed when those threads exit.
    cbm_destroy_thread_parser();
    cbm_initialized = 0;
}

// --- Bottleneck call-name classification (language-agnostic heuristics) ---

// Case-insensitive equality for short callee names.
static bool name_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

static bool name_in_set(const char *name, const char *const *set) {
    for (const char *const *s = set; *s; s++) {
        if (name_ieq(name, *s)) {
            return true;
        }
    }
    return false;
}

// Linear-scan / membership calls: a hit inside a loop is the textbook hidden
// O(n^2) (cf. Olivo et al., PLDI'15) that syntactic loop-depth alone misses.
static bool is_linear_scan_name(const char *n) {
    static const char *const set[] = {"find",    "indexof",   "contains", "includes", "search",
                                      "lookup",  "strstr",    "strchr",   "strrchr",  "memchr",
                                      "find_if", "findindex", "count",    "index",    NULL};
    return name_in_set(n, set);
}

// Allocation / growable-append calls: repeated inside a loop is the classic
// accidental reallocation / string-concat O(n^2). Names are deliberately
// conservative; meaningless in some languages → simply never matches there.
static bool is_alloc_name(const char *n) {
    static const char *const set[] = {"malloc",  "calloc",    "realloc",      "strdup", "strndup",
                                      "append",  "push_back", "emplace_back", "concat", "strcat",
                                      "strncat", "push",      "pushback",     NULL};
    return name_in_set(n, set);
}

// Extract the receiver identifier from a def's receiver text — Go's
// "(s *Store)" / "(s Store)" → "s". Stores the identifier start in *out and
// returns its length; returns 0 for unnamed receivers ("(*Store)", "(Store)"),
// where no second token follows the identifier (a lone token is the TYPE, not
// a name — such methods have no receiver variable to call through anyway).
static size_t receiver_ident(const char *recv_text, const char **out) {
    const char *p = recv_text;
    if (*p == '(') {
        p++;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    const char *start = p;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
           *p == '_') {
        p++;
    }
    size_t len = (size_t)(p - start);
    if (len == 0) {
        return 0; // "(*Store)": leading '*', no identifier
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == ')' || *p == '\0') {
        return 0; // "(Store)": single token is the type, receiver unnamed
    }
    *out = start;
    return len;
}

// Whether a callee expression targets the same instance/class as the enclosing
// def, i.e. counts as genuine self-recursion rather than a same-named call on a
// different receiver. callee_name may be bare ("recur") or qualified
// ("self.recur", "this.recur", "super().save", "axios.get", "self.obj.recur").
//
// Bare names have no receiver → assume self-call (free function calling itself
// by bare name; preserves prior behavior). Qualified names: the receiver chain
// is everything before the LAST '.', and the WHOLE chain must name the same
// object — self/this/cls/@self, or the enclosing def's own receiver identifier
// (Go: `s` in `func (s *Store) save()`, from CBMDefinition.receiver). Matching
// the whole chain (not its first segment) keeps self.obj.recur() out: it
// targets self's FIELD obj, a different object. super() is the parent class and
// any other receiver (axios, console, ...) a different target. See #599.
static bool is_self_receiver(const char *callee_name, const char *def_receiver) {
    if (!callee_name || !callee_name[0]) {
        return false;
    }
    const char *dot = strrchr(callee_name, '.');
    if (!dot) {
        return true; // bare name → self-recursion candidate
    }
    size_t rlen = (size_t)(dot - callee_name);
    static const char *const self_receivers[] = {"self", "this", "cls", "@self", NULL};
    for (int i = 0; self_receivers[i]; i++) {
        size_t sl = strlen(self_receivers[i]);
        if (rlen == sl && strncmp(callee_name, self_receivers[i], sl) == 0) {
            return true;
        }
    }
    if (def_receiver) {
        const char *rid = NULL;
        size_t ril = receiver_ident(def_receiver, &rid);
        if (ril > 0 && ril == rlen && strncmp(callee_name, rid, ril) == 0) {
            return true; // call through the enclosing method's own receiver
        }
    }
    return false; // super() / axios / console / self.obj / any other receiver
}

// Count parameters from a signature string like "(int a, Foo* b, cb (*)(int,int))".
// Fallback for languages where param_names isn't populated (e.g. C keeps only the
// signature text). Counts commas at the top paren level; treats "()"/"(void)" as 0.
// Approximate by design (a structural smell, not an exact arity).
static int count_params_from_signature(const char *sig) {
    if (!sig) {
        return 0;
    }
    const char *p = sig;
    while (*p && *p != '(') {
        p++;
    }
    if (*p != '(') {
        return 0;
    }
    p++;
    const char *list = p;
    int depth = 0;
    int commas = 0;
    bool any = false;
    for (; *p; p++) {
        char ch = *p;
        if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
            depth++;
        } else if (ch == ')') {
            if (depth == 0) {
                break;
            }
            depth--;
        } else if (ch == ']' || ch == '}' || ch == '>') {
            if (depth > 0) {
                depth--;
            }
        } else if (ch == ',' && depth == 0) {
            commas++;
        } else if (!isspace((unsigned char)ch)) {
            any = true;
        }
    }
    if (!any) {
        return 0; /* "()" */
    }
    if (commas == 0) {
        while (*list == ' ' || *list == '\t') {
            list++;
        }
        if (strncmp(list, "void", 4) == 0 &&
            (list[4] == ')' || list[4] == ' ' || list[4] == '\0')) {
            return 0; /* C "(void)" */
        }
    }
    return commas + 1;
}

// --- Main extraction function ---

/* Test-only deterministic fault injection for the crash/hang supervisor tests.
 * Gated entirely behind env vars that are never set in production; a matching
 * rel_path either aborts (a fault signal the supervisor classifies as a crash)
 * or spins forever (an external-scanner infinite loop the quiet-timeout kills).
 * This gives an honest guard — green iff the supervisor actually contains a real
 * fault — instead of a fixture that may stop faulting once a root cause is fixed. */
/* Crash-supervisor per-file marker JOURNAL (Stage 3c skip-and-continue,
 * parallel-safe). Recovery re-runs are PARALLEL (there are no sequential
 * production runs), so a single overwrite-style marker would race across
 * workers and — worse — go stale during non-extract phases, blaming
 * whatever file was extracted LAST (that mis-quarantined four innocent
 * ms-typescript fixtures, one 15-minute retry at a time). Instead every
 * worker APPENDS one short line per event: "S <rel_path>" when it STARTS
 * work on a file, "D <rel_path>" when it finishes it. A single short
 * append of one line is atomic in practice on every target platform, and
 * the parent discards a torn final line by design. The parent's suspect
 * set after a crash/hang = files with an S but no D — exactly the
 * in-flight set; a file is only quarantined after appearing in the
 * suspect set of TWO CONSECUTIVE failed runs, so a stale or merely
 * unlucky in-flight file is never quarantined alone. The env var is set
 * solely by the supervisor during recovery — a no-op on normal runs. */
static void cbm_index_mark(const char *rel_path, char event) {
    const char *mf = getenv("CBM_INDEX_MARKER_FILE");
    if (!mf || !mf[0] || !rel_path || !rel_path[0]) {
        return;
    }
    FILE *f = cbm_fopen(mf, "ab");
    if (f) {
        (void)fprintf(f, "%c %s\n", event, rel_path);
        (void)fclose(f);
    }
}

void cbm_index_mark_start(const char *rel_path) {
    cbm_index_mark(rel_path, 'S');
}

void cbm_index_mark_done(const char *rel_path) {
    cbm_index_mark(rel_path, 'D');
}

/* ── Crash-quarantine set (Stage 3c skip-and-continue) ──────────────────────
 * After a crash the supervisor re-runs the worker single-threaded, passing
 * CBM_INDEX_QUARANTINE_FILE — a newline-delimited list of repo-relative paths
 * that already crashed the indexer and MUST NOT be extracted again. Owned here,
 * next to the other env-driven extract hooks (marker + fault injector), so the
 * single hard guard lives at the one choke point every pass funnels through
 * (cbm_extract_file): whether a pass re-extracts from disk on a cache miss
 * (sequential pass_calls/usages/semantic) or extracts fresh, a quarantined file
 * short-circuits to an empty result and never reaches the parser/crash. The
 * pipeline extract loops separately REPORT the skip as phase="crash" via
 * cbm_index_is_quarantined() so the crasher surfaces in the response skipped[].
 * Loaded once, lazily; read-only after load (safe for the parallel workers,
 * though recovery runs single-threaded). Unset env ⇒ empty set ⇒ cheap no-op. */
static CBMHashTable *g_quarantine_set = NULL;
enum { CBM_QSET_UNINIT = 0, CBM_QSET_INITING = 1, CBM_QSET_INITED = 2 };
static atomic_int g_quarantine_state = CBM_QSET_UNINIT;

static void cbm_quarantine_load(void) {
    const char *qf = getenv("CBM_INDEX_QUARANTINE_FILE");
    if (!qf || !qf[0]) {
        return; /* normal path: empty set */
    }
    FILE *f = cbm_fopen(qf, "rb");
    if (!f) {
        return;
    }
    CBMHashTable *set = cbm_ht_create(16);
    if (!set) {
        (void)fclose(f);
        return;
    }
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        /* Line format: "path\tphase" where phase is "crash" or "hang". A bare
         * "path" line (no tab) is tolerated and defaults to phase "crash" for
         * backward compatibility with older quarantine files. */
        char *tab = strchr(line, '\t');
        const char *phase = "crash";
        if (tab) {
            *tab = '\0';
            if (tab[1]) {
                phase = tab + 1;
            }
        }
        if (line[0] == '\0') {
            continue; /* empty path (line began with a tab) — skip */
        }
        /* The table borrows the key + value pointers, so dup both. Intentionally
         * never freed: the set lives for the whole (short-lived worker) process.
         * The value stores the phase so cbm_index_quarantine_phase() can report
         * "crash" vs "hang"; membership (cbm_index_is_quarantined) is value != NULL. */
        char *pval = cbm_strdup(phase);
        if (!pval) {
            continue;
        }
        if (cbm_ht_has(set, line)) {
            /* Duplicate path line: reuse the stored key (the table borrows key
             * pointers, so a fresh copy would leak on replace) and free the
             * value it displaces. */
            free(cbm_ht_set(set, line, (void *)pval));
        } else {
            char *key = cbm_strdup(line);
            if (key) {
                cbm_ht_set(set, key, (void *)pval);
            } else {
                /* Partial failure: don't leak the value copy. */
                free(pval);
            }
        }
    }
    (void)fclose(f);
    g_quarantine_set = set;
}

bool cbm_index_is_quarantined(const char *rel_path) {
    if (!rel_path || !rel_path[0]) {
        return false;
    }
    int state = atomic_load(&g_quarantine_state);
    if (state != CBM_QSET_INITED) {
        /* First caller wins the CAS and loads; racers spin until INITED.
         * Same once-init pattern as cbm_ui_log_init (http_server.c). */
        state = CBM_QSET_UNINIT;
        if (atomic_compare_exchange_strong(&g_quarantine_state, &state, CBM_QSET_INITING)) {
            cbm_quarantine_load();
            atomic_store(&g_quarantine_state, CBM_QSET_INITED);
        } else {
            while (atomic_load(&g_quarantine_state) != CBM_QSET_INITED) {
                cbm_usleep(1000); /* 1ms */
            }
        }
    }
    return g_quarantine_set && cbm_ht_has(g_quarantine_set, rel_path);
}

const char *cbm_index_quarantine_phase(const char *rel_path) {
    /* cbm_index_is_quarantined drives the lazy once-load and returns true only
     * when the set is loaded and holds rel_path — so on true, g_quarantine_set is
     * non-NULL and the stored value is the phase string ("crash"/"hang"). */
    if (!cbm_index_is_quarantined(rel_path)) {
        return NULL;
    }
    return (const char *)cbm_ht_get(g_quarantine_set, rel_path);
}

static void cbm_test_fault_inject(const char *rel_path) {
    if (!rel_path || !rel_path[0]) {
        return;
    }
    const char *crash_on = getenv("CBM_TEST_CRASH_ON");
    if (crash_on && crash_on[0] && strstr(rel_path, crash_on)) {
        abort(); /* SIGABRT → WIFSIGNALED → classified as a crash */
    }
    const char *hang_on = getenv("CBM_TEST_HANG_ON");
    if (hang_on && hang_on[0] && strstr(rel_path, hang_on)) {
        for (;;) {
            /* Busy-spin: the supervisor's quiet-timeout kills + reports us. */
        }
    }
    const char *exit_on = getenv("CBM_TEST_EXIT_ON");
    if (exit_on && exit_on[0] && strstr(rel_path, exit_on)) {
        exit(1); /* Nonzero exit code → CBM_PROC_EXIT_NONZERO → classified as "error" */
    }
}

static CBMFileResult *cbm_extract_file_impl(const char *source, int source_len,
                                            CBMLanguage language, const char *project,
                                            const char *rel_path, int64_t timeout_micros,
                                            const char **extra_defines, const char **include_paths,
                                            const CBMExtractOptions *options);

/* Best-effort parse-coverage collection (#963). Walks only the has_error paths
 * of the tree and records the 1-based line ranges of the TOP-MOST ERROR/MISSING
 * nodes (does not descend into an error subtree — one range per failed region).
 * Bounded by CBM_MAX_ERROR_REGIONS so pathological input can't blow up the
 * output. The ranges mark where constructs were dropped; they are a detection
 * aid, never a completeness proof.
 *
 * `dropped` counts the ranges the cap threw away, so a clipped list cannot read
 * as a complete one: cbm_error_ranges_str turns a non-zero count into a
 * trailing "+<N>" marker. The preprocessed-parse refinement splits one
 * whole-file range into many small ones, which pushes real files into a cap
 * that used to be unreachable (upstream measured cli.c and test_cli.c landing
 * on exactly 64), so the clip is live behaviour and the cap moves to 256. */
#define CBM_MAX_ERROR_REGIONS 256
typedef struct {
    uint32_t starts[CBM_MAX_ERROR_REGIONS];
    uint32_t ends[CBM_MAX_ERROR_REGIONS];
    int count;
    int dropped;
} cbm_error_regions_t;

static void cbm_error_regions_push(cbm_error_regions_t *acc, TSNode n) {
    TSPoint start = ts_node_start_point(n);
    TSPoint end = ts_node_end_point(n);
    uint32_t start_line = start.row + 1;
    uint32_t end_line = end.row + 1;

    /* A node that ends at column 0 stopped right after the previous line's
     * newline, so it holds no text on the row it points at. Counting that row
     * named a line past the end of the file whenever the region ran to EOF
     * (upstream: a 326-line PowerShell script reported "245-327"). */
    if (end.column == 0 && end.row > start.row) {
        end_line = end.row;
    }

    /* One line can carry several error nodes; repeating the same range says
     * nothing new. Only an EXACT repeat of the range just pushed is dropped —
     * never a merely overlapping one. Each range is judged separately later by
     * cbm_region_is_recovered, and merging 3-3 into 2-3 would hand the wider
     * range's covering definition to an error it does not explain, making a
     * real failure vanish (the Perl #1838 malformed fixture has exactly that
     * 2-3 / 3-3 shape upstream). Runs BEFORE the cap check, so a repeat is never
     * counted as a range the cap threw away. */
    if (acc->count > 0 && start_line == acc->starts[acc->count - 1] &&
        end_line == acc->ends[acc->count - 1]) {
        return;
    }

    if (acc->count >= CBM_MAX_ERROR_REGIONS) {
        acc->dropped++;
        return;
    }
    acc->starts[acc->count] = start_line;
    acc->ends[acc->count] = end_line;
    acc->count++;
}

/* A file that does not end with a newline leaves the grammar's mandatory line
 * terminator MISSING. That node is ZERO-WIDTH and sits at EOF.
 *
 * It is not a miss. The parser consumed no source for it — start_byte ==
 * end_byte — so by construction nothing was dropped: no construct can live in a
 * zero-byte span, and every real instruction above it parsed normally. This is a
 * property of the grammar's terminator rule, not of the file.
 *
 * Flagging it made the verdict arbitrary. Grammars whose terminator token is
 * VISIBLE (dockerfile, tcl, fish, gomod, hyprlang) reported parse_partial for a
 * missing final newline; grammars whose terminator is HIDDEN (ini, fsharp,
 * beancount, requirements, gitignore, sshconfig, kconfig) reported nothing for
 * exactly the same omission, because a hidden node is invisible to
 * ts_node_child(). Whether a user was told their file was partially parsed
 * depended on a grammar-authoring accident.
 *
 * The cost is not cosmetic: a phantom parse_partial writes a
 * "<project>::missed" shadow row into the project's own database.
 *
 * Deliberately narrow — ZERO-WIDTH AT EOF ONLY. A MISSING or ERROR node with
 * WIDTH still counts even at EOF (a Makefile whose last recipe line is
 * unterminated really does lose the recipe), and anything before EOF is
 * untouched.
 *
 * #1746: trailing blanks are extras owned by no node, so the Dockerfile grammar
 * parks that zero-width missing newline BEFORE a trailing blank run rather than
 * at raw EOF (`ENTRYPOINT ["a"] ` + EOF sits one byte short of source_len).
 * Treat "at EOF" as EOF modulo a trailing run of blanks — every blank except
 * newline, deliberately not a hand-picked subset, because a terminated final
 * line produces no MISSING terminator at all. The widening applies only when the
 * missing token is itself a newline; the exact-EOF rule above stays as broad as
 * it was. */
static bool cbm_is_blank_not_newline(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}

static bool cbm_is_eof_terminator_miss(TSNode n, const char *source, int source_len) {
    if (!ts_node_is_missing(n) || source_len < 0) {
        return false;
    }
    uint32_t start = ts_node_start_byte(n);
    uint32_t end = ts_node_end_byte(n);
    if (start != end || end > (uint32_t)source_len) {
        return false;
    }
    if (end == (uint32_t)source_len) {
        return true;
    }
    if (!source || strcmp(ts_node_type(n), "\n") != 0) {
        return false;
    }
    for (uint32_t i = end; i < (uint32_t)source_len; i++) {
        if (!cbm_is_blank_not_newline(source[i])) {
            return false;
        }
    }
    return true;
}

/* Walks to the end even after the cap is full, so `dropped` is the real number
 * of ranges lost rather than a lower bound. Cheap: the walk never descends into
 * an ERROR subtree, so it only visits the spine of nodes containing an error. */
template <typename Visit>
static void cbm_walk_error_nodes(TSNode n, const char *source, int source_len, const Visit &visit) {
    uint32_t k = ts_node_child_count(n);
    for (uint32_t i = 0; i < k; i++) {
        TSNode c = ts_node_child(n, i);
        if (ts_node_is_missing(c) || strcmp(ts_node_type(c), "ERROR") == 0) {
            if (cbm_is_eof_terminator_miss(c, source, source_len)) {
                continue; /* absent final newline only — nothing was dropped */
            }
            visit(c); /* top-most region; do not descend */
        } else if (ts_node_has_error(c)) {
            cbm_walk_error_nodes(c, source, source_len, visit);
        }
    }
}

static void cbm_collect_error_regions(TSNode n, cbm_error_regions_t *acc, const char *source,
                                      int source_len) {
    cbm_walk_error_nodes(n, source, source_len,
                         [acc](TSNode c) { cbm_error_regions_push(acc, c); });
}

/* 1-based line count of `src` where every newline starts a new line, so a
 * trailing newline opens an (empty) last line. */
static uint32_t cbm_newline_line_count(const char *src, int src_len) {
    uint32_t n = 1;
    for (int i = 0; i < src_len; i++) {
        if (src[i] == '\n') {
            n++;
        }
    }
    return n;
}

/* ── Preprocessed-parse line map (#963) ───────────────────────────────────
 *
 * The raw parse is preprocessor-blind. When an #ifdef splits a brace it sees
 * both branches at once, the braces do not balance, and the ERROR node swallows
 * the whole construct — at file scope the whole FILE. The second parse, on
 * preprocessed source, does not have that problem: the preprocessor already
 * picked one branch. So build one byte per ORIGINAL line and cut the raw
 * ranges down to the lines the second parse cannot vouch for. Lines in the
 * branch the preprocessor threw away never appear in the second parse, so they
 * stay flagged — they really are missing from the graph.
 *
 * CBM_LINE_PP_PARSED — the preprocessed parse covered this original line and
 *                      found no error on it.
 * CBM_LINE_NO_CODE   — blank, comment-only, or a preprocessor directive. A
 *                      reported range never begins or ends on one. Directives
 *                      are here because the preprocessor CONSUMES them, so the
 *                      second parse can never vouch for one; the known cost is
 *                      that a #define the raw parse really dropped no longer
 *                      shows up on its own. */
enum : uint8_t { CBM_LINE_PP_PARSED = 1u, CBM_LINE_NO_CODE = 2u };

static bool cbm_is_directive_line(const char *line, int len) {
    int i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    return i < len && line[i] == '#';
}

/* True when the line ends with a backslash continuation. */
static bool cbm_line_continues(const char *line, int len) {
    int end = len;
    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r')) {
        end--;
    }
    return end > 0 && line[end - 1] == '\\';
}

/* Set CBM_LINE_NO_CODE on every line of `src` that holds no construct. One
 * pass; carries block-comment and directive-continuation state across lines. */
static void cbm_mark_no_code_lines(const char *src, int src_len, uint8_t *map,
                                   uint32_t line_count) {
    bool in_block = false;
    bool in_directive = false;
    uint32_t line = 1;
    int i = 0;
    while (i <= src_len && line <= line_count) {
        int end = i;
        while (end < src_len && src[end] != '\n') {
            end++;
        }
        bool has_code = false;
        bool line_starts_in_block = in_block;
        for (int j = i; j < end; j++) {
            if (in_block) {
                if (src[j] == '*' && j + 1 < end && src[j + 1] == '/') {
                    in_block = false;
                    j++;
                }
                continue;
            }
            if (src[j] == '/' && j + 1 < end && src[j + 1] == '*') {
                in_block = true;
                j++;
                continue;
            }
            if (src[j] == '/' && j + 1 < end && src[j + 1] == '/') {
                break; /* rest of the line is a comment */
            }
            if (src[j] == '"' || src[j] == '\'') {
                /* A literal is code, and a slash-star comment marker inside it must
                 * not open a block: skip to the closing quote on this line. */
                char quote = src[j];
                for (j++; j < end && src[j] != quote; j++) {
                    if (src[j] == '\\' && j + 1 < end) {
                        j++;
                    }
                }
                has_code = true;
                continue;
            }
            if (src[j] != ' ' && src[j] != '\t' && src[j] != '\r') {
                has_code = true;
            }
        }
        bool directive =
            !line_starts_in_block && (in_directive || cbm_is_directive_line(src + i, end - i));
        if (!has_code || directive) {
            map[line] |= CBM_LINE_NO_CODE;
        }
        in_directive = directive && cbm_line_continues(src + i, end - i);
        line++;
        i = end + 1;
    }
}

/* Build the original-line map from the preprocessed parse. Returns NULL when
 * the expanded parse is itself a total loss (root is ERROR — it vouches for
 * nothing) or on allocation failure; the caller then keeps the raw ranges, so
 * OOM only costs precision, never correctness. The map lives in `a` so it
 * outlives the expanded source and its tree. */
static uint8_t *cbm_build_pp_line_map(CBMArena *a, const char *source, int source_len,
                                      const CBMPreprocessedSource *pp, TSNode pp_root,
                                      const char *expanded, int expanded_len, uint32_t *out_lines) {
    *out_lines = 0;
    if (!pp || !pp->original_line_by_expanded_line || !pp->belongs_to_main_file ||
        pp->expanded_line_count <= 0 || strcmp(ts_node_type(pp_root), "ERROR") == 0) {
        return nullptr;
    }
    uint32_t orig_lines = cbm_newline_line_count(source, source_len);
    auto *map = (uint8_t *)cbm_arena_alloc(a, (size_t)orig_lines + 2);
    int exp_lines = pp->expanded_line_count;
    auto *bad_rows = (uint8_t *)calloc((size_t)exp_lines + 2, 1);
    if (!map || !bad_rows) {
        free(bad_rows);
        return nullptr;
    }
    memset(map, 0, (size_t)orig_lines + 2);
    cbm_mark_no_code_lines(source, source_len, map, orig_lines);
    /* Mark the EXPANDED rows that sit under an ERROR/MISSING node of the
     * preprocessed tree. Not the region collector: its cap would stop marking. */
    cbm_walk_error_nodes(pp_root, expanded, expanded_len, [bad_rows, exp_lines](TSNode c) {
        uint32_t s = ts_node_start_point(c).row + 1;
        uint32_t e = ts_node_end_point(c).row + 1;
        for (uint32_t r = s; r <= e && r <= (uint32_t)exp_lines; r++) {
            bad_rows[r] = 1;
        }
    });
    /* An expanded line only vouches for its original line when it HAS text:
     * the preprocessor emits a blank line where it dropped a branch, and a blank
     * line proves nothing about the code that used to be there. */
    uint32_t eline = 1;
    bool eline_has_text = false;
    for (int ci = 0; ci <= expanded_len; ci++) {
        if (ci < expanded_len && expanded[ci] != '\n') {
            char ch = expanded[ci];
            if (ch != ' ' && ch != '\t' && ch != '\r') {
                eline_has_text = true;
            }
            continue;
        }
        if (eline_has_text && (int)eline <= exp_lines && !bad_rows[eline] &&
            pp->belongs_to_main_file[eline]) {
            uint32_t orig = pp->original_line_by_expanded_line[eline];
            if (orig >= 1 && orig <= orig_lines) {
                map[orig] |= CBM_LINE_PP_PARSED;
            }
        }
        eline++;
        eline_has_text = false;
    }
    free(bad_rows);
    *out_lines = orig_lines;
    return map;
}

/* Recovery subtraction (#963): tree-sitter error recovery plus the
 * ERROR-descending def walker often still extract constructs INSIDE a failed
 * region (verified: a function in an #ifdef-split ERROR region and even a
 * `def broken(:` both came back as defs). A region whose every line is
 * covered by definitions that START inside it is definitely recovered — its
 * constructs ARE in the graph — so flagging it would be a false miss.
 * Container defs (Module/Package) are ignored: a file-spanning Module node is
 * not evidence the region's constructs survived. Conservative: partially
 * covered regions stay flagged. */
static bool cbm_region_is_recovered(uint32_t rs, uint32_t re, const CBMDefArray *defs) {
    enum { MAX_COVER_DEFS = 256 };
    uint32_t starts[MAX_COVER_DEFS];
    uint32_t ends[MAX_COVER_DEFS];
    int n = 0;
    for (int i = 0; i < defs->count && n < MAX_COVER_DEFS; i++) {
        const CBMDefinition *d = &defs->items[i];
        if (!d->label || strcmp(d->label, "Module") == 0 || strcmp(d->label, "Package") == 0) {
            continue;
        }
        if (d->start_line < rs || d->start_line > re) {
            continue; /* recovery evidence must originate inside the region */
        }
        starts[n] = d->start_line;
        ends[n] = d->end_line < d->start_line ? d->start_line : d->end_line;
        n++;
    }
    if (n == 0) {
        return false;
    }
    /* Insertion-sort by start, then sweep for gaps in [rs, re]. */
    for (int i = 1; i < n; i++) {
        uint32_t s = starts[i];
        uint32_t e = ends[i];
        int j = i - 1;
        while (j >= 0 && starts[j] > s) {
            starts[j + 1] = starts[j];
            ends[j + 1] = ends[j];
            j--;
        }
        starts[j + 1] = s;
        ends[j + 1] = e;
    }
    uint32_t covered_to = rs - 1;
    for (int i = 0; i < n; i++) {
        if (starts[i] > covered_to + 1) {
            return false; /* uncovered gap */
        }
        if (ends[i] > covered_to) {
            covered_to = ends[i];
        }
    }
    return covered_to >= re;
}

/* #961: true when 1-based `line` of `src` contains `name` (used to verify a
 * def recovered from EXPANDED source really lives on that ORIGINAL line —
 * rejects header-inlined defs whose physical expanded lines alias unrelated
 * raw lines when compile_commands include paths are present). */
static bool cbm_line_contains(const char *src, int src_len, uint32_t line, const char *name) {
    if (!src || !name || !name[0] || line == 0) {
        return false;
    }
    uint32_t cur = 1;
    int i = 0;
    while (i < src_len && cur < line) {
        if (src[i] == '\n') {
            cur++;
        }
        i++;
    }
    if (cur != line) {
        return false;
    }
    int end = i;
    while (end < src_len && src[end] != '\n') {
        end++;
    }
    size_t nlen = strlen(name);
    for (int j = i; j + (int)nlen <= end; j++) {
        if (strncmp(src + j, name, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static bool cbm_identifier_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Verify that the mapped original span contains callable-definition syntax,
 * not merely the name at a macro invocation or call site. */
static bool cbm_span_contains_callable_def(const char *src, int src_len, uint32_t start_line,
                                           uint32_t end_line, const char *name) {
    if (!src || src_len <= 0 || !name || !name[0] || start_line == 0 || end_line < start_line) {
        return false;
    }
    int span_start = 0;
    uint32_t line = 1;
    while (span_start < src_len && line < start_line) {
        if (src[span_start++] == '\n') {
            line++;
        }
    }
    if (line != start_line) {
        return false;
    }
    int span_end = span_start;
    while (span_end < src_len && line <= end_line) {
        if (src[span_end++] == '\n') {
            line++;
        }
    }
    size_t name_len = strlen(name);
    for (int pos = span_start; pos + (int)name_len <= span_end; pos++) {
        if (strncmp(src + pos, name, name_len) != 0 ||
            (pos > 0 && cbm_identifier_char(src[pos - 1])) ||
            (pos + (int)name_len < src_len && cbm_identifier_char(src[pos + name_len]))) {
            continue;
        }
        int before = pos;
        while (before > span_start && isspace((unsigned char)src[before - 1])) {
            before--;
        }
        if (before > span_start && strchr("(,=!?[.", src[before - 1])) {
            continue;
        }
        int open = pos + (int)name_len;
        while (open < span_end && isspace((unsigned char)src[open])) {
            open++;
        }
        if (open >= span_end || src[open] != '(') {
            continue;
        }
        int depth = 0;
        int close = -1;
        for (int i = open; i < span_end; i++) {
            if (src[i] == '(') {
                depth++;
            } else if (src[i] == ')' && --depth == 0) {
                close = i;
                break;
            }
        }
        if (close < 0) {
            continue;
        }
        for (int i = close + 1; i < span_end; i++) {
            if (src[i] == '{') {
                return true;
            }
            if (src[i] == ';') {
                break;
            }
        }
    }
    return false;
}

// Published call locations refer to the original file, even when type resolution
// ran against expanded source. Header-owned and unmapped calls belong elsewhere.
static uint32_t cbm_preprocessed_main_call_line(const CBMPreprocessedSource *pp, uint32_t line,
                                                uint32_t original_lines) {
    if (!pp || !pp->original_line_by_expanded_line || !pp->belongs_to_main_file ||
        pp->expanded_line_count <= 0 || !line || line > (uint32_t)pp->expanded_line_count ||
        !pp->belongs_to_main_file[line]) {
        return 0;
    }
    uint32_t original = pp->original_line_by_expanded_line[line];
    return original && original <= original_lines ? original : 0;
}

static void cbm_remap_preprocessed_calls(CBMFileResult *result, int first_call, int first_resolved,
                                         const CBMPreprocessedSource *pp, const char *source,
                                         int source_len) {
    uint32_t original_lines = 1;
    for (int i = 0; i < source_len; ++i) {
        if (source[i] == '\n') {
            ++original_lines;
        }
    }
    int keep = first_call;
    for (int i = first_call; i < result->calls.count; ++i) {
        CBMCall call = result->calls.items[i];
        uint32_t original =
            cbm_preprocessed_main_call_line(pp, (uint32_t)call.start_line, original_lines);
        if (!original) {
            continue;
        }
        call.start_line = (int)original;
        // source_byte stays in the expanded view for its paired LSP lookup.
        // The line map cannot prove a raw byte offset; never invent one.
        result->calls.items[keep++] = call;
    }
    result->calls.count = keep;
    keep = first_resolved;
    for (int i = first_resolved; i < result->resolved_calls.count; ++i) {
        CBMResolvedCall resolved = result->resolved_calls.items[i];
        if (resolved.binary_operator_line) {
            uint32_t original =
                cbm_preprocessed_main_call_line(pp, resolved.binary_operator_line, original_lines);
            if (!original) {
                // Deferred counts describe syntactic candidates, not resolved
                // records. Keep them conservative instead of guessing a decrement.
                continue;
            }
            resolved.binary_operator_line = original;
        }
        result->resolved_calls.items[keep++] = resolved;
    }
    result->resolved_calls.count = keep;
}

/* Remap an expanded-source definition back to the original input file. Every
 * line in the definition must be attributable to the main file; generated
 * macro bodies, included headers, and ambiguous spans fail closed. */
static bool cbm_remap_preprocessed_def(CBMDefinition *def, const CBMPreprocessedSource *pp) {
    if (!def || !pp || !pp->original_line_by_expanded_line || !pp->belongs_to_main_file ||
        def->start_line == 0 || def->end_line < def->start_line ||
        def->end_line > (uint32_t)pp->expanded_line_count) {
        return false;
    }

    uint32_t original_start = pp->original_line_by_expanded_line[def->start_line];
    uint32_t original_end = pp->original_line_by_expanded_line[def->end_line];
    if (!original_start || !original_end || original_end < original_start) {
        return false;
    }
    for (uint32_t line = def->start_line; line <= def->end_line; line++) {
        if (!pp->belongs_to_main_file[line] || !pp->original_line_by_expanded_line[line]) {
            return false;
        }
    }

    def->start_line = original_start;
    def->end_line = original_end;
    def->lines = (int)(original_end - original_start + 1);
    return true;
}

static void cbm_subtract_recovered_regions(cbm_error_regions_t *regs, const CBMDefArray *defs) {
    int kept = 0;
    for (int i = 0; i < regs->count; i++) {
        if (!cbm_region_is_recovered(regs->starts[i], regs->ends[i], defs)) {
            regs->starts[kept] = regs->starts[i];
            regs->ends[kept] = regs->ends[i];
            kept++;
        }
    }
    regs->count = kept;
}

/* #1071: a function-like macro invocation whose argument is a type token
 * (e.g. ALLOC(int, n)) makes tree-sitter's C/C++ grammar emit an ERROR node — it
 * parses `int` in expression position — which would be recorded as a parse_partial
 * coverage gap. But the macro is #defined in THIS file, so nothing is actually
 * missing from the graph; it's a benign call the grammar can't parse without the
 * preprocessor. True if the [start_line, end_line] span contains a call `NAME(` to
 * a file-defined function-like macro (Macro label + a parameter signature). */
typedef struct {
    const char *name;
    int len;
} cbm_macro_name_t;

/* The names of this file's function-like macros, gathered once per file so the
 * per-line checks below do not rescan every definition. If the names could not
 * be copied, items is NULL while count stays non-zero, and the checks read the
 * definitions directly — slower, same answer. */
typedef struct {
    cbm_macro_name_t *items;
    int count;
    const CBMDefArray *defs;
} cbm_macro_names_t;

static bool cbm_is_function_like_macro(const CBMDefinition *d) {
    /* Function-like macros only: an object-like macro (#define PI 3.14) has no
     * parameter signature and can't be mistaken for a call. */
    return d->label && strcmp(d->label, "Macro") == 0 && d->signature && d->name && d->name[0];
}

static void cbm_collect_function_like_macros(CBMArena *arena, const CBMDefArray *defs,
                                             cbm_macro_names_t *out) {
    *out = {nullptr, 0, defs};
    int n = 0;
    for (int di = 0; di < defs->count; di++) {
        if (cbm_is_function_like_macro(&defs->items[di])) {
            n++;
        }
    }
    if (n == 0) {
        return;
    }
    out->count = n;
    out->items = (cbm_macro_name_t *)cbm_arena_alloc(arena, (size_t)n * sizeof(cbm_macro_name_t));
    if (!out->items) {
        return; /* checks read defs directly */
    }
    int k = 0;
    for (int di = 0; di < defs->count; di++) {
        const CBMDefinition *d = &defs->items[di];
        if (cbm_is_function_like_macro(d)) {
            out->items[k++] = {d->name, (int)strlen(d->name)};
        }
    }
}

/* True if [span_start, span_end) holds `NAME(` with NAME a whole identifier. */
static bool cbm_span_calls_name(const char *src, int src_len, int span_start, int span_end,
                                const char *name, int nlen) {
    for (int pos = span_start; pos + nlen <= span_end; pos++) {
        if (strncmp(src + pos, name, (size_t)nlen) != 0 ||
            (pos > 0 && cbm_identifier_char(src[pos - 1])) ||
            (pos + nlen < src_len && cbm_identifier_char(src[pos + nlen]))) {
            continue;
        }
        int open = pos + nlen;
        while (open < span_end && isspace((unsigned char)src[open])) {
            open++;
        }
        if (open < span_end && src[open] == '(') {
            return true; /* NAME( ... ) — an invocation of this file's macro */
        }
    }
    return false;
}

static bool cbm_byte_span_is_macro_invocation(const char *src, int src_len, int span_start,
                                              int span_end, const cbm_macro_names_t *macros) {
    if (!src || src_len <= 0 || macros->count == 0 || span_start < 0 || span_end > src_len ||
        span_start >= span_end) {
        return false;
    }
    if (macros->items) {
        for (int mi = 0; mi < macros->count; mi++) {
            if (cbm_span_calls_name(src, src_len, span_start, span_end, macros->items[mi].name,
                                    macros->items[mi].len)) {
                return true;
            }
        }
        return false;
    }
    for (int di = 0; di < macros->defs->count; di++) {
        const CBMDefinition *d = &macros->defs->items[di];
        if (cbm_is_function_like_macro(d) && cbm_span_calls_name(src, src_len, span_start, span_end,
                                                                 d->name, (int)strlen(d->name))) {
            return true;
        }
    }
    return false;
}

/* Byte offset where every 1-based line starts, so finding a line's span costs
 * one table read instead of a walk from byte 0. line_count + 2 entries: [L] is
 * where line L starts and the last entry is the end of the source. A line the
 * file never reaches starts at the end, so its span is empty — the same answer
 * the walk gives. line_count must be cbm_newline_line_count(src, src_len).
 * Returns NULL on allocation failure; callers fall back to the walk, so the
 * answer never changes. */
static int *cbm_build_line_offsets(CBMArena *arena, const char *src, int src_len,
                                   uint32_t line_count) {
    auto *offsets = (int *)cbm_arena_alloc(arena, ((size_t)line_count + 2) * sizeof(int));
    if (!offsets) {
        return nullptr;
    }
    for (uint32_t l = 0; l <= line_count + 1; l++) {
        offsets[l] = src_len;
    }
    offsets[0] = 0;
    offsets[1] = 0;
    uint32_t line = 1;
    for (int i = 0; i < src_len; i++) {
        if (src[i] != '\n') {
            continue;
        }
        line++;
        if (line > line_count + 1) {
            break;
        }
        offsets[line] = i + 1;
    }
    return offsets;
}

/* Same question by line number, for callers asking about one whole region.
 * Reads the span from line_offsets (built for line_count lines) when there is
 * one, and walks the source to find it otherwise. */
static bool cbm_span_is_macro_invocation(const char *src, int src_len, uint32_t start_line,
                                         uint32_t end_line, const cbm_macro_names_t *macros,
                                         const int *line_offsets, uint32_t line_count) {
    if (!src || src_len <= 0 || macros->count == 0 || start_line == 0 || end_line < start_line) {
        return false;
    }
    if (line_offsets) {
        if (start_line > line_count) {
            return false; /* the file never reaches the first line */
        }
        int span_end = end_line < line_count ? line_offsets[end_line + 1] : src_len;
        return cbm_byte_span_is_macro_invocation(src, src_len, line_offsets[start_line], span_end,
                                                 macros);
    }
    int span_start = 0;
    uint32_t line = 1;
    while (span_start < src_len && line < start_line) {
        if (src[span_start++] == '\n') {
            line++;
        }
    }
    if (line != start_line) {
        return false;
    }
    int span_end = span_start;
    while (span_end < src_len && line <= end_line) {
        if (src[span_end++] == '\n') {
            line++;
        }
    }
    return cbm_byte_span_is_macro_invocation(src, src_len, span_start, span_end, macros);
}

/* True if [rs, re] is fully enclosed by an extracted callable definition (a
 * Function/Method body). A macro invocation INSIDE a real function body is an
 * expression-level use where nothing is missing (#1071). A TOP-LEVEL invocation
 * is different: the macro may itself expand to a definition that the original
 * span doesn't contain (#949), which must stay flagged. Restricting the #1071
 * suppression to in-body calls keeps that #949 gap honest and fails safe. */
static bool cbm_region_inside_callable(uint32_t rs, uint32_t re, const CBMDefArray *defs) {
    for (int i = 0; i < defs->count; i++) {
        const CBMDefinition *d = &defs->items[i];
        if (!d->label) {
            continue;
        }
        if (strcmp(d->label, "Function") != 0 && strcmp(d->label, "Method") != 0 &&
            strcmp(d->label, "Constructor") != 0 && strcmp(d->label, "Destructor") != 0) {
            continue;
        }
        if (d->start_line <= rs && d->end_line >= re && d->end_line > d->start_line) {
            return true;
        }
    }
    return false;
}

static void cbm_subtract_macro_invocation_regions(cbm_error_regions_t *regs,
                                                  const CBMDefArray *defs,
                                                  const cbm_macro_names_t *macros, const char *src,
                                                  int src_len, const int *line_offsets,
                                                  uint32_t line_count) {
    if (macros->count == 0) {
        return; /* no function-like macro, so no region is a benign invocation */
    }
    int kept = 0;
    for (int i = 0; i < regs->count; i++) {
        bool benign = cbm_span_is_macro_invocation(src, src_len, regs->starts[i], regs->ends[i],
                                                   macros, line_offsets, line_count) &&
                      cbm_region_inside_callable(regs->starts[i], regs->ends[i], defs);
        if (!benign) {
            regs->starts[kept] = regs->starts[i];
            regs->ends[kept] = regs->ends[i];
            kept++;
        }
    }
    regs->count = kept;
}

/* Push [start, end] after trimming no-code lines off both ends. A run made
 * only of directives, comments or blank lines disappears — there was never a
 * construct on it to lose. */
static void cbm_push_trimmed_run(cbm_error_regions_t *out, uint32_t start, uint32_t end,
                                 const uint8_t *map, uint32_t line_count) {
    while (start <= end && start <= line_count && (map[start] & CBM_LINE_NO_CODE)) {
        start++;
    }
    while (end >= start && end <= line_count && (map[end] & CBM_LINE_NO_CODE)) {
        end--;
    }
    if (start > end) {
        return;
    }
    if (out->count >= CBM_MAX_ERROR_REGIONS) {
        out->dropped++;
        return;
    }
    out->starts[out->count] = start;
    out->ends[out->count] = end;
    out->count++;
}

/* #949: a TOP-LEVEL macro invocation is the one place a clean second parse
 * proves nothing — the macro can expand to a whole definition the recovery
 * walker deliberately refuses to adopt, so the line must stay flagged. An
 * in-body invocation is the benign #1071 case, handled later by
 * cbm_subtract_macro_invocation_regions. */
static bool cbm_line_is_toplevel_macro_call(const char *src, int src_len, uint32_t line,
                                            const int *line_offsets, uint32_t line_count,
                                            const CBMDefArray *defs,
                                            const cbm_macro_names_t *macros) {
    return macros->count > 0 &&
           cbm_span_is_macro_invocation(src, src_len, line, line, macros, line_offsets,
                                        line_count) &&
           !cbm_region_inside_callable(line, line, defs);
}

/* Cut every raw region down to the runs of lines the preprocessed parse could
 * not vouch for. This collapses a whole-file range on a file whose only real
 * problem is an #ifdef splitting a brace, but never clears a region outright:
 * the branch the preprocessor discarded is genuinely absent from the graph. */
static void cbm_refine_regions_with_pp_lines(cbm_error_regions_t *regs, const uint8_t *map,
                                             uint32_t line_count, const char *src, int src_len,
                                             const int *line_offsets, const CBMDefArray *defs,
                                             const cbm_macro_names_t *macros) {
    cbm_error_regions_t out = {};
    out.dropped = regs->dropped;
    for (int i = 0; i < regs->count; i++) {
        uint32_t run_start = 0;
        uint32_t run_end = 0;
        uint32_t end = regs->ends[i] < line_count ? regs->ends[i] : line_count;
        for (uint32_t line = regs->starts[i]; line <= end; line++) {
            if ((map[line] & CBM_LINE_PP_PARSED) &&
                !cbm_line_is_toplevel_macro_call(src, src_len, line, line_offsets, line_count, defs,
                                                 macros)) {
                if (run_start != 0) {
                    cbm_push_trimmed_run(&out, run_start, run_end, map, line_count);
                    run_start = 0;
                }
            } else {
                if (run_start == 0) {
                    run_start = line;
                }
                run_end = line;
            }
        }
        if (run_start != 0) {
            cbm_push_trimmed_run(&out, run_start, run_end, map, line_count);
        }
    }
    *regs = out;
}

/* Share of a file one range must cover before it stops being advice and
 * becomes noise ("look at lines 1 to 13047" of a 13046-line file). */
enum { CBM_UNUSABLE_PCT = 80 };

/* Number of 1-based lines in `src` given its cbm_newline_line_count: a final
 * newline closes the last line rather than opening another, and a missing one
 * still leaves a last line. */
static uint32_t cbm_count_lines(const char *src, int src_len, uint32_t newline_lines) {
    return src_len > 0 && src[src_len - 1] == '\n' ? newline_lines - 1 : newline_lines;
}

/* Serialize collected regions as "start-end,start-end,...", with a trailing
 * ",+<N>" when the cap threw N ranges away. The marker must stay a SUFFIX:
 * every reader stops at the first token that is not a range, so a marker in the
 * middle would silently hide everything after it. N can be non-zero while the
 * kept list is short (recovery and macro rules run after collection) — still
 * honest, because what the cap lost is unknown. */
static const char *cbm_error_ranges_str(CBMArena *a, const cbm_error_regions_t *regs) {
    if (regs->count <= 0 && regs->dropped <= 0) {
        return NULL;
    }
    enum { RANGE_MAX = 24 }; /* "4294967295-4294967295," */
    char *buf = (char *)cbm_arena_alloc(a, (size_t)(regs->count + 1) * RANGE_MAX);
    if (!buf) {
        return NULL;
    }
    buf[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < regs->count; i++) {
        off += (size_t)snprintf(buf + off, RANGE_MAX, "%s%u-%u", i ? "," : "", regs->starts[i],
                                regs->ends[i]);
    }
    if (regs->dropped > 0) {
        snprintf(buf + off, RANGE_MAX, "%s+%d", off ? "," : "", regs->dropped);
    }
    return buf;
}

/* Public entry: run the extraction and journal completion. The DONE mark on
 * every ordinary return (including error/timeout results) tells the crash
 * supervisor this file did NOT kill the worker — only a file whose S has no
 * D is a crash/hang suspect. */
/* ── Statement-macro blanking (C/C++) ─────────────────────────────
 *
 * A macro invocation whose arguments are statements —
 *     SC_DECL_SERIALIZABLE(register_member("id", m_id); register_member(...);)
 * — is not parseable by tree-sitter-cpp, and its error recovery then swallows
 * every declaration that follows in the file. Measured on modmesh: class
 * recall 91.7%, with headers keeping only their FIRST class (World.hpp lost
 * WorldState and World), and nothing but a parse_partial range to show for it.
 *
 * Before parsing, blank such invocations in a copy: IDENT(...) where IDENT
 * looks like a macro (letters/digits/underscore only, at least 3 chars,
 * contains an upper-case letter, no lower-case), the parentheses balance,
 * and the argument text contains ';'. Every byte is replaced by a space
 * except newlines, so byte offsets and line numbers of everything else stay
 * identical and node text can still be read from the ORIGINAL source.
 * String and character literals and comments are skipped so a ';' or '('
 * inside them cannot mislead the scan. Returns NULL when nothing changed. */
static bool blank_macro_ident_ok(const char *s, int n) {
    if (n < 3) {
        return false;
    }
    bool upper = false;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') {
            upper = true;
        } else if (c >= 'a' && c <= 'z') {
            return false;
        } else if (!(c == '_' || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return upper;
}

/* Advance past a literal or comment starting at src[i]; returns the index of
 * the first byte after it, or i when src[i] starts none. */
static int blank_skip_literal(const char *src, int len, int i) {
    /* C++ raw string: R"delim( ... )delim" (optionally u8R/uR/UR/LR). A '"'
     * or ';' inside it must not end the literal or count as a separator. */
    {
        int r = i;
        if ((src[r] == 'u' && r + 1 < len && src[r + 1] == '8')) {
            r += 2;
        } else if (src[r] == 'u' || src[r] == 'U' || src[r] == 'L') {
            r += 1;
        }
        if (r < len && src[r] == 'R' && r + 1 < len && src[r + 1] == '"' &&
            (i == 0 || !(isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_'))) {
            int d0 = r + 2;
            int d1 = d0;
            while (d1 < len && src[d1] != '(' && d1 - d0 < 16 && src[d1] != '"' &&
                   src[d1] != '\\' && src[d1] != '\n') {
                d1++;
            }
            if (d1 < len && src[d1] == '(') {
                int dlen = d1 - d0;
                for (int k = d1 + 1; k + dlen + 1 < len; k++) {
                    if (src[k] == ')' && memcmp(src + k + 1, src + d0, (size_t)dlen) == 0 &&
                        src[k + 1 + dlen] == '"') {
                        return k + dlen + 2;
                    }
                }
                return len; /* unterminated: nothing after it is safe to touch */
            }
        }
    }
    if (src[i] == '"' || src[i] == '\'') {
        char q = src[i];
        int j = i + 1;
        while (j < len && src[j] != q && src[j] != '\n') {
            if (src[j] == '\\') {
                j++;
            }
            j++;
        }
        return j < len ? j + 1 : len;
    }
    if (src[i] == '/' && i + 1 < len && src[i + 1] == '/') {
        int j = i + 2;
        while (j < len && src[j] != '\n') {
            j++;
        }
        return j;
    }
    if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
        int j = i + 2;
        while (j + 1 < len && !(src[j] == '*' && src[j + 1] == '/')) {
            j++;
        }
        return j + 1 < len ? j + 2 : len;
    }
    return i;
}

static char *cbm_blank_statement_macros(CBMArena *a, const char *src, int len) {
    char *out = NULL;
    int i = 0;
    while (i < len) {
        int skipped = blank_skip_literal(src, len, i);
        if (skipped != i) {
            i = skipped;
            continue;
        }
        unsigned char c = (unsigned char)src[i];
        bool ident_start = (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
        bool prev_ident =
            i > 0 && ((unsigned char)src[i - 1] == '_' || isalnum((unsigned char)src[i - 1]));
        if (!ident_start || prev_ident) {
            i++;
            continue;
        }
        int start = i;
        while (i < len && (src[i] == '_' || isalnum((unsigned char)src[i]))) {
            i++;
        }
        if (!blank_macro_ident_ok(src + start, i - start)) {
            continue;
        }
        int j = i;
        while (j < len && (src[j] == ' ' || src[j] == '\t')) {
            j++;
        }
        if (j >= len || src[j] != '(') {
            continue;
        }
        /* Balanced scan of the argument list. */
        int depth = 0;
        bool has_semicolon = false;
        int k = j;
        int close = -1;
        while (k < len) {
            int sk = blank_skip_literal(src, len, k);
            if (sk != k) {
                k = sk;
                continue;
            }
            if (src[k] == '(') {
                depth++;
            } else if (src[k] == ')') {
                depth--;
                if (depth == 0) {
                    close = k;
                    break;
                }
            } else if (src[k] == ';') {
                has_semicolon = true;
            } else if (src[k] == '{' || src[k] == '}') {
                /* A brace inside macro args is a lambda or a block-in-args
                 * macro; leave those to the parser. */
                break;
            } else if (src[k] == '#' && (k == 0 || src[k - 1] == '\n')) {
                /* A preprocessor line inside the argument list means the
                 * parentheses may balance differently per #if branch; do not
                 * blank across it. */
                break;
            }
            k++;
        }
        if (close < 0 || !has_semicolon) {
            i = close > 0 ? close + 1 : i;
            continue;
        }
        if (!out) {
            out = (char *)cbm_arena_alloc(a, (size_t)len + 1);
            if (!out) {
                return NULL;
            }
            memcpy(out, src, (size_t)len);
            out[len] = '\0';
        }
        for (int b = start; b <= close; b++) {
            if (out[b] != '\n') {
                out[b] = ' ';
            }
        }
        i = close + 1;
    }
    return out;
}

CBMFileResult *cbm_extract_file(const char *source, int source_len, CBMLanguage language,
                                const char *project, const char *rel_path, int64_t timeout_micros,
                                const char **extra_defines, const char **include_paths) {
    return cbm_extract_file_with_options(source, source_len, language, project, rel_path,
                                         timeout_micros, extra_defines, include_paths, nullptr);
}

CBMFileResult *cbm_extract_file_with_options(const char *source, int source_len,
                                             CBMLanguage language, const char *project,
                                             const char *rel_path, int64_t timeout_micros,
                                             const char **extra_defines, const char **include_paths,
                                             const CBMExtractOptions *options) {
    CBMFileResult *r = cbm_extract_file_impl(source, source_len, language, project, rel_path,
                                             timeout_micros, extra_defines, include_paths, options);
    cbm_index_mark_done(rel_path);
    return r;
}

/* ── Per-file LSP scratch arena ──────────────────────────────────── */

/* The per-file LSP resolvers build a whole type registry for the file — the
 * single largest thing in a full index's memory (etcd 891 MB of 1127, django
 * 1584 of 2531, rocksdb 762 of 2143, measured with CBM_MEM_PROFILE=1). All of
 * it used to land in result->arena, which the pipeline holds until the end of
 * the resolve phase, even though the only things the resolvers leave behind
 * are entries appended to result->resolved_calls, result->calls (Python and
 * Rust synthetic calls) and result->defs (the Python and Kotlin builtin
 * injections). Give them a scratch arena instead, copy those entries onto the
 * durable arena, and throw the registry away with the file.
 *
 * The copy is driven by pointer ownership rather than by knowing which
 * resolver wrote what: anything that points inside the scratch arena is
 * duplicated, anything else (durable-arena strings, source slices, string
 * literals) is left exactly as it is. */

static bool arena_owns(const CBMArena *a, const void *p) {
    return cbm_arena_contains(a, p) != 0;
}

/* Never returns the scratch pointer and never returns NULL for a non-NULL
 * input: on allocation failure it degrades to the empty string, which loses a
 * property rather than leaving a dangling read behind. */
static const char *reloc_str(CBMArena *dst, const CBMArena *scratch, const char *s) {
    if (!s || !arena_owns(scratch, s)) {
        return s;
    }
    const char *copy = cbm_arena_strdup(dst, s);
    return copy ? copy : "";
}

/* NULL-terminated arrays of strings (CBMDefinition::decorators and friends).
 * Both the array block and its elements can live in the scratch. */
static const char **reloc_str_array(CBMArena *dst, const CBMArena *scratch, const char **arr) {
    if (!arr) {
        return NULL;
    }
    int n = 0;
    while (arr[n]) {
        n++;
    }
    const char **out = arr;
    if (arena_owns(scratch, arr)) {
        out = (const char **)cbm_arena_alloc(dst, (size_t)(n + 1) * sizeof(*out));
        if (!out) {
            return NULL;
        }
        out[n] = NULL;
    }
    for (int i = 0; i < n; i++) {
        out[i] = reloc_str(dst, scratch, arr[i]);
    }
    return out;
}

static void reloc_definition(CBMArena *dst, const CBMArena *scratch, CBMDefinition *d) {
    d->name = reloc_str(dst, scratch, d->name);
    d->qualified_name = reloc_str(dst, scratch, d->qualified_name);
    d->label = reloc_str(dst, scratch, d->label);
    d->file_path = reloc_str(dst, scratch, d->file_path);
    d->signature = reloc_str(dst, scratch, d->signature);
    d->return_type = reloc_str(dst, scratch, d->return_type);
    d->receiver = reloc_str(dst, scratch, d->receiver);
    d->docstring = reloc_str(dst, scratch, d->docstring);
    d->parent_class = reloc_str(dst, scratch, d->parent_class);
    d->route_path = reloc_str(dst, scratch, d->route_path);
    d->route_method = reloc_str(dst, scratch, d->route_method);
    d->structural_profile = reloc_str(dst, scratch, d->structural_profile);
    d->body_tokens = reloc_str(dst, scratch, d->body_tokens);
    d->decorators = reloc_str_array(dst, scratch, d->decorators);
    d->base_classes = reloc_str_array(dst, scratch, d->base_classes);
    d->param_names = reloc_str_array(dst, scratch, d->param_names);
    d->param_types = reloc_str_array(dst, scratch, d->param_types);
    d->return_types = reloc_str_array(dst, scratch, d->return_types);
    if (d->fingerprint && d->fingerprint_k > 0 && arena_owns(scratch, d->fingerprint)) {
        size_t bytes = (size_t)d->fingerprint_k * sizeof(*d->fingerprint);
        uint32_t *copy = (uint32_t *)cbm_arena_alloc(dst, bytes);
        if (copy) {
            memcpy(copy, d->fingerprint, bytes);
            d->fingerprint = copy;
        } else {
            d->fingerprint = NULL;
            d->fingerprint_k = 0;
        }
    }
}

static void reloc_call(CBMArena *dst, const CBMArena *scratch, CBMCall *c) {
    c->callee_name = reloc_str(dst, scratch, c->callee_name);
    c->enclosing_func_qn = reloc_str(dst, scratch, c->enclosing_func_qn);
    c->first_string_arg = reloc_str(dst, scratch, c->first_string_arg);
    c->second_arg_name = reloc_str(dst, scratch, c->second_arg_name);
    if (c->args && c->arg_count > 0 && arena_owns(scratch, c->args)) {
        size_t bytes = (size_t)c->arg_count * sizeof(*c->args);
        auto *copy = (CBMCallArg *)cbm_arena_alloc(dst, bytes);
        if (copy) {
            memcpy(copy, c->args, bytes);
        } else {
            c->arg_count = 0;
        }
        c->args = copy;
    }
    for (int i = 0; i < c->arg_count; i++) {
        c->args[i].expr = reloc_str(dst, scratch, c->args[i].expr);
        c->args[i].value = reloc_str(dst, scratch, c->args[i].value);
        c->args[i].keyword = reloc_str(dst, scratch, c->args[i].keyword);
    }
}

#ifdef CBM_ENABLE_TEST_SEAMS
void cbm_test_relocate_call(CBMArena *dst, const CBMArena *scratch, CBMCall *call) {
    reloc_call(dst, scratch, call);
}
#endif

static void reloc_resolved_call(CBMArena *dst, const CBMArena *scratch, CBMResolvedCall *rc) {
    rc->caller_qn = reloc_str(dst, scratch, rc->caller_qn);
    rc->callee_qn = reloc_str(dst, scratch, rc->callee_qn);
    rc->strategy = reloc_str(dst, scratch, rc->strategy);
    rc->reason = reloc_str(dst, scratch, rc->reason);
}

/* A push that overflows the array reallocates the whole block from whichever
 * arena it was handed, so an array the LSP grew now lives in the scratch —
 * including the entries that were already there. Move the block first, then
 * fix up the entries the LSP added. */
#define RELOC_ARRAY_BLOCK(arr, dst, scratch)                                                  \
    do {                                                                                      \
        if ((arr)->items && (arr)->cap > 0 && arena_owns((scratch), (arr)->items)) {          \
            void *moved = cbm_arena_alloc((dst), (size_t)(arr)->cap * sizeof(*(arr)->items)); \
            if (moved) {                                                                      \
                memcpy(moved, (arr)->items, (size_t)(arr)->count * sizeof(*(arr)->items));    \
                (arr)->items = (__typeof__((arr)->items))moved;                               \
            } else {                                                                          \
                (arr)->count = 0; /* never leave the array pointing into the scratch */       \
                (arr)->cap = 0;                                                               \
                (arr)->items = NULL;                                                          \
            }                                                                                 \
        }                                                                                     \
    } while (0)

/* Move everything the per-file LSP left in `result` off `scratch` and onto
 * `dst`, so `scratch` can be destroyed. The *_before counts mark where each
 * array stood when the LSP started: entries below them were built by the
 * extractors on `dst` and need no field fix-up. */
static void lsp_scratch_reclaim(CBMFileResult *result, CBMArena *dst, const CBMArena *scratch,
                                int defs_before, int calls_before, int resolved_before) {
    RELOC_ARRAY_BLOCK(&result->defs, dst, scratch);
    RELOC_ARRAY_BLOCK(&result->calls, dst, scratch);
    RELOC_ARRAY_BLOCK(&result->resolved_calls, dst, scratch);

    for (int i = defs_before; i < result->defs.count; i++) {
        reloc_definition(dst, scratch, &result->defs.items[i]);
    }
    for (int i = calls_before; i < result->calls.count; i++) {
        reloc_call(dst, scratch, &result->calls.items[i]);
    }
    for (int i = resolved_before; i < result->resolved_calls.count; i++) {
        reloc_resolved_call(dst, scratch, &result->resolved_calls.items[i]);
    }
}

/* Initial block for the per-file traversal scratch arena. Upstream measured
 * arena_grow on a 14k-file TypeScript tree: it fires on one file in 12,000 at
 * both this size and at 1 MB, and on most files at 256 KB, where the two
 * channel walks alone are exactly 262144 bytes. 512 KB is also exactly
 * mimalloc's MI_LARGE_MAX_OBJ_SIZE, so the block is still bin-allocated. */
enum { CBM_EXTRACT_SCRATCH_BLOCK = CBM_SZ_512 * CBM_SZ_1K };

static CBMFileResult *extract_file_impl_body(const char *source, int source_len,
                                             CBMLanguage language, const char *project,
                                             const char *rel_path, int64_t timeout_micros,
                                             const char **extra_defines, const char **include_paths,
                                             const CBMExtractOptions *options, CBMArena *scratch) {
    // Allocate result on heap (arena inside for all string data)
    enum { SINGLE = 1 };
    CBMFileResult *result = (CBMFileResult *)calloc(SINGLE, sizeof(CBMFileResult));
    if (!result) {
        return NULL;
    }

    cbm_arena_init(&result->arena);
    CBMArena *a = &result->arena;

    /* Crash-quarantine hard guard (Stage 3c): a file the supervisor pinned as a
     * crasher must NEVER be parsed again. Return a clean empty result BEFORE the
     * marker write and fault injector so no pass (including sequential re-extract
     * passes that miss the result cache) can crash on it. The pipeline extract
     * loops separately record it as a phase="crash" skip. Checked before the
     * marker so quarantined files never overwrite it — the marker keeps pointing
     * at the real (non-quarantined) file being processed when a crash hits. */
    if (cbm_index_is_quarantined(rel_path)) {
        return result;
    }

    cbm_index_mark_start(rel_path);
    cbm_test_fault_inject(rel_path);

    // Get language spec
    const CBMLangSpec *spec = cbm_lang_spec(language);
    if (!spec) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, "unsupported language");
        return result;
    }

    // Get tree-sitter language
    const TSLanguage *ts_lang = cbm_ts_language(language);
    if (!ts_lang) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, "no tree-sitter grammar");
        return result;
    }

    // Get thread-local parser (reused across files on same thread)
    TSParser *parser = get_thread_parser(ts_lang, language);
    if (!parser) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, "parser alloc failed");
        return result;
    }

    // Reset parser state from any previous parse (cancellation flags etc.)
    ts_parser_reset(parser);

    uint64_t t0 = now_ns();

    // C/C++: parse a copy with statement-bearing macro invocations blanked
    // (same byte offsets), so one unparseable macro does not swallow every
    // declaration after it. See cbm_blank_statement_macros.
    const char *parse_source = source;
    if (language == CBM_LANG_C || language == CBM_LANG_CPP || language == CBM_LANG_CUDA) {
        char *blanked = cbm_blank_statement_macros(a, source, source_len);
        if (blanked) {
            parse_source = blanked;
        }
    }

    // Build string input + timeout options for parse_with_options
    CBMStringInput str_input = {parse_source, (uint32_t)source_len};
    TSInput ts_input = {
        &str_input,
        cbm_string_read,
        TSInputEncodingUTF8,
        NULL,
    };

    TSParseOptions opts = {0};
    CBMParseBudget budget = {0, 0, 0}; // cppcheck-suppress unreadVariable
    if (timeout_micros > 0) {
        uint64_t budget_ns = (uint64_t)timeout_micros * USEC_TO_NSEC;
        // Descheduling burns wall time but not CPU: gate on this thread's CPU
        // time so a starved-but-parseable file is not abandoned, with a generous
        // wall ceiling as a backstop against a genuinely spinning/stuck parse.
        budget.cpu_deadline_ns = cbm_thread_cpu_time_ns() + budget_ns;
        /* now_ns() >= t0, so CPU spent since the deadline was set is at most
         * wall elapsed since t0: the CPU deadline cannot pass before this. */
        budget.cpu_idle_until_ns = t0 + budget_ns;
        budget.wall_ceiling_ns = t0 + budget_ns * (uint64_t)CBM_PARSE_WALL_CEILING_FACTOR;
        opts.payload = &budget;
        opts.progress_callback = cbm_timeout_cb;
#ifdef CBM_ENABLE_TEST_SEAMS
        tl_parse_wall_seam_offset_ns = 0;
        const char *stall_on = getenv("CBM_TEST_WALL_STALL_ON");
        if (stall_on && stall_on[0] && rel_path && strstr(rel_path, stall_on)) {
            // Push the wall reading past the 1x budget (a wall-only budget
            // trips) but well under the ceiling (the CPU budget survives).
            tl_parse_wall_seam_offset_ns = budget_ns + (uint64_t)NSEC_PER_SEC;
        }
#endif
    }

    TSTree *tree = ts_parser_parse_with_options(parser, NULL, ts_input, opts);
    uint64_t t1 = now_ns();

    if (!tree) {
        result->has_error = true;
        result->error_msg =
            cbm_arena_strdup(a, timeout_micros > 0 ? "parse timeout" : "parse failed");
        return result;
    }

    TSNode root = ts_tree_root_node(tree);

    // Compute module QN. Java/Go derive the module from the CONTAINING
    // DIRECTORY (package semantics) rather than baking the filename stem in,
    // so def QNs, the LSP caller_qn, and the textual calls-enclosing QN all
    // agree (e.g. Outer.java -> module "proj", not "proj.Outer"). Other
    // languages are unchanged.
    result->module_qn = cbm_fqn_module_source_lang(a, project, rel_path, language);
    result->is_test_file = cbm_is_test_file(rel_path, language);

    // Build extraction context
    CBMExtractCtx ctx = {
        .arena = a,
        .scratch = scratch,
        .result = result,
        .source = source,
        .source_len = source_len,
        .language = language,
        .project = project,
        .rel_path = rel_path,
        .module_qn = result->module_qn,
        .root = root,
        .defer_cpp_operators = options && options->defer_cpp_operators,
        .deduplicate_usages = options && options->deduplicate_usages,
    };

    // Run extractors: defs + imports use separate walks (unique recursion patterns),
    // then a single unified cursor walk handles the remaining 7 extractors.
    cbm_extract_definitions(&ctx);
    cbm_extract_imports(&ctx);
    uint64_t arena_mark = a->total_alloc;
    atomic_fetch_add(&arena_bytes_parse, arena_mark);
    CPPPendingOperators raw_operators(result, ctx.defer_cpp_operators);
    cbm_extract_unified(&ctx);
    raw_operators.prepare();

    // Channel detection (Socket.IO / EventEmitter) — JS/TS only.
    cbm_extract_channels(&ctx);

    // K8s / Kustomize semantic pass (additional structured extraction for YAML-based infra files).
    if (ctx.language == CBM_LANG_KUSTOMIZE || ctx.language == CBM_LANG_K8S) {
        cbm_extract_k8s(&ctx);
    }

    // dbt lineage pass: a dbt model's dependencies live in Jinja ({{ ref(...) }}),
    // which the SQL grammar cannot read. Self-gated — SQL files only, and only
    // those carrying a real dbt builtin call.
    if (ctx.language == CBM_LANG_SQL) {
        cbm_extract_dbt(&ctx);
    }

    // LSP type-aware call/usage resolution (per-file). Runs in every mode;
    // refines the tree-sitter + textual-resolution graph with type info.
    atomic_fetch_add(&arena_bytes_extract, a->total_alloc - arena_mark);
    arena_mark = a->total_alloc;
    uint64_t lsp_start = now_ns();
    /* Scratch for the resolvers' type registries — see lsp_scratch_reclaim. */
    CBMArena lsp_scratch;
    cbm_arena_init(&lsp_scratch);
    CBMArena *la = &lsp_scratch;
    const int lsp_defs_before = result->defs.count;
    const int lsp_calls_before = result->calls.count;
    const int lsp_resolved_before = result->resolved_calls.count;
    {
        if (language == CBM_LANG_GO) {
            cbm_run_go_lsp(la, result, source, source_len, root);
        }
        if (language == CBM_LANG_C || language == CBM_LANG_CPP || language == CBM_LANG_CUDA) {
            cbm_run_c_lsp(la, result, source, source_len, root, language != CBM_LANG_C);
        }
        if (language == CBM_LANG_PHP) {
            cbm_run_php_lsp(la, result, source, source_len, root);
        }
        if (language == CBM_LANG_PYTHON) {
            cbm_run_py_lsp(la, result, source, source_len, root);
        }
        if (language == CBM_LANG_JAVASCRIPT || language == CBM_LANG_TYPESCRIPT ||
            language == CBM_LANG_TSX) {
            bool js_mode = (language == CBM_LANG_JAVASCRIPT);
            // jsx_mode: TSX always; .jsx in the JS bucket also enables it.
            bool jsx_mode = (language == CBM_LANG_TSX);
            if (language == CBM_LANG_JAVASCRIPT && rel_path) {
                size_t rl = strlen(rel_path);
                if (rl >= 4 && strcmp(rel_path + rl - 4, ".jsx") == 0)
                    jsx_mode = true;
            }
            // dts_mode: ".d.ts" suffix (TypeScript only).
            bool dts_mode = false;
            if (language == CBM_LANG_TYPESCRIPT && rel_path) {
                size_t rl = strlen(rel_path);
                if (rl >= 5 && strcmp(rel_path + rl - 5, ".d.ts") == 0)
                    dts_mode = true;
            }
            cbm_run_ts_lsp(la, result, source, source_len, root, js_mode, jsx_mode, dts_mode);
        }
        if (language == CBM_LANG_CSHARP) {
            cbm_run_cs_lsp(la, result, source, source_len, root);
        }
    }
    if (language == CBM_LANG_JAVA) {
        cbm_run_java_lsp(la, result, source, source_len, root);
    }
    if (language == CBM_LANG_KOTLIN) {
        cbm_run_kotlin_lsp(la, result, source, source_len, root);
    }
    if (language == CBM_LANG_RUST) {
        cbm_run_rust_lsp(la, result, source, source_len, root);
    }
    lsp_scratch_reclaim(result, a, &lsp_scratch, lsp_defs_before, lsp_calls_before,
                        lsp_resolved_before);
    raw_operators.finish();
    atomic_fetch_add(&arena_bytes_lsp, lsp_scratch.total_alloc);
    cbm_arena_destroy(&lsp_scratch);
    atomic_fetch_add(&total_lsp_ns, now_ns() - lsp_start);
    /* Only what survived the reclaim counts against the durable arena. */
    atomic_fetch_add(&arena_bytes_lsp_kept, a->total_alloc - arena_mark);
    arena_mark = a->total_alloc;

    // Calls extracted so far all carry ORIGINAL-source line numbers; the C/C++
    // preprocessor second pass below appends calls with EXPANDED-source lines,
    // which must not be used for the def line-range attribution of the bottleneck
    // metrics. Remember the boundary.
    int orig_calls_count = result->calls.count;
    int orig_resolved_count = result->resolved_calls.count;

    /* Preprocessed-parse line map (#963), built by the second pass below and
     * read by the parse-coverage block at the end. Stays NULL for every
     * language without a second pass, leaving their coverage signal as it was. */
    uint8_t *pp_line_map = nullptr;
    uint32_t pp_line_map_lines = 0;

    // Second pass: preprocess C/C++/CUDA and extract additional macro-hidden calls.
    // Defs keep original-source line numbers; only CALLS are extracted from expanded source.
    if (language == CBM_LANG_C || language == CBM_LANG_CPP || language == CBM_LANG_CUDA) {
        uint64_t pp_start = now_ns();
        CBMPreprocessedSource *preprocessed = cbm_preprocess_with_map(
            source, source_len, rel_path, extra_defines, include_paths, language != CBM_LANG_C);
        if (preprocessed && preprocessed->source) {
            char *expanded = preprocessed->source;
            int expanded_len = (int)strlen(expanded);
            // Record calls count before second pass
            int calls_before = result->calls.count;

            // Parse expanded source with fresh tree
            TSParser *pp_parser = get_thread_parser(ts_lang, language);
            if (pp_parser) {
                ts_parser_reset(pp_parser);
                CBMStringInput pp_input = {expanded, (uint32_t)expanded_len};
                TSInput pp_ts_input = {
                    &pp_input,
                    cbm_string_read,
                    TSInputEncodingUTF8,
                    NULL,
                };
                TSParseOptions pp_opts = {0};
                TSTree *pp_tree =
                    ts_parser_parse_with_options(pp_parser, NULL, pp_ts_input, pp_opts);
                if (pp_tree) {
                    TSNode pp_root = ts_tree_root_node(pp_tree);

                    // Build context for expanded source — extract only calls via unified extractor
                    CBMExtractCtx pp_ctx = {
                        .arena = a,
                        .scratch = scratch,
                        .result = result,
                        .source = expanded,
                        .source_len = expanded_len,
                        .language = language,
                        .project = project,
                        .rel_path = rel_path,
                        .module_qn = result->module_qn,
                        .root = pp_root,
                        .defer_cpp_operators = options && options->defer_cpp_operators,
                        .deduplicate_usages = options && options->deduplicate_usages,
                    };
                    // Re-run unified extraction on expanded source.
                    // This adds macro-expanded calls. Their locations are remapped
                    // below before original/expanded call evidence is combined.
                    CPPPendingOperators pp_operators(result, pp_ctx.defer_cpp_operators);
                    cbm_extract_unified(&pp_ctx);
                    pp_operators.prepare();

                    // Also run LSP on expanded source for additional type-resolved
                    // calls (language is already C/C++/CUDA — checked in enclosing
                    // block). Runs in every mode.
                    {
                        /* Same deal as the raw-source resolvers: the registry
                         * this builds over a fully expanded translation unit
                         * is the largest of the lot, and none of it outlives
                         * the file. */
                        CBMArena pp_lsp_scratch;
                        cbm_arena_init(&pp_lsp_scratch);
                        const int pp_defs_before = result->defs.count;
                        const int pp_calls_before = result->calls.count;
                        const int pp_resolved_before = result->resolved_calls.count;
                        uint64_t pp_kept_before = a->total_alloc;
                        cbm_run_c_lsp(&pp_lsp_scratch, result, expanded, expanded_len, pp_root,
                                      language != CBM_LANG_C);
                        lsp_scratch_reclaim(result, a, &pp_lsp_scratch, pp_defs_before,
                                            pp_calls_before, pp_resolved_before);
                        atomic_fetch_add(&arena_bytes_lsp, pp_lsp_scratch.total_alloc);
                        uint64_t pp_kept = a->total_alloc - pp_kept_before;
                        atomic_fetch_add(&arena_bytes_lsp_kept, pp_kept);
                        /* Charged to the LSP, so keep it out of the
                         * preprocessed-extraction total measured below. */
                        arena_mark += pp_kept;
                        cbm_arena_destroy(&pp_lsp_scratch);
                    }

                    /* #961: a def whose body braces are split across
                     * #ifdef/#else branches parses as an ERROR region on the
                     * RAW source (both branches present at once -> unbalanced
                     * braces), so the raw defs walk silently dropped it. The
                     * expanded tree parses clean (simplecpp picked one
                     * branch), so recover defs from it — adopting ONLY those
                     * that remap onto main-file original lines, intersect a
                     * raw ERROR region, have their name visible on the raw
                     * source line, carry real callable-definition syntax
                     * there, and whose QN the raw pass did not already
                     * extract. */
                    if (ts_node_has_error(root)) {
                        cbm_error_regions_t raw_regs = {};
                        cbm_collect_error_regions(root, &raw_regs, source, source_len);
                        if (raw_regs.count > 0) {
                            int defs_before = result->defs.count;
                            // Recover into a separate definition set. Overload preservation
                            // compares byte offsets; expanded-source offsets must never rename
                            // existing raw definitions whose calls already use their identities.
                            CBMFileResult recovered = {};
                            recovered.module_qn = result->module_qn;
                            recovered.is_test_file = result->is_test_file;
                            CBMExtractCtx recovery_ctx = pp_ctx;
                            recovery_ctx.result = &recovered;
                            cbm_extract_definitions(&recovery_ctx);
                            for (int i = 0; i < recovered.defs.count; i++) {
                                CBMDefinition *d = &recovered.defs.items[i];
                                bool adopt = false;
                                if (cbm_remap_preprocessed_def(d, preprocessed)) {
                                    for (int rj = 0; rj < raw_regs.count && !adopt; rj++) {
                                        if (d->start_line <= raw_regs.ends[rj] &&
                                            d->end_line >= raw_regs.starts[rj]) {
                                            adopt = true;
                                        }
                                    }
                                }
                                if (adopt && (!d->name ||
                                              !cbm_line_contains(source, source_len, d->start_line,
                                                                 d->name) ||
                                              !cbm_span_contains_callable_def(
                                                  source, source_len, d->start_line, d->end_line,
                                                  d->name))) {
                                    adopt = false;
                                }
                                for (int j = 0; j < defs_before && adopt; j++) {
                                    const CBMDefinition &raw = result->defs.items[j];
                                    const char *q = raw.qualified_name;
                                    bool same_qn =
                                        q && d->qualified_name && strcmp(q, d->qualified_name) == 0;
                                    // Real overloads can carry different raw/expanded byte
                                    // suffixes for the same source definition.
                                    bool same_definition =
                                        raw.start_line == d->start_line &&
                                        raw.end_line == d->end_line && raw.declaration_key &&
                                        d->declaration_key &&
                                        strcmp(raw.declaration_key, d->declaration_key) == 0;
                                    if (same_qn || same_definition) {
                                        adopt = false;
                                    }
                                }
                                if (adopt) {
                                    cbm_defs_push(&result->defs, a, *d);
                                }
                            }
                        }
                    }

                    // Capture which original lines the expanded parse vouches for
                    // before the expanded tree goes away (#963).
                    pp_line_map =
                        cbm_build_pp_line_map(a, source, source_len, preprocessed, pp_root,
                                              expanded, expanded_len, &pp_line_map_lines);

                    // Resolve against the expanded view first; publish only mapped
                    // main-file locations after definition recovery has finished.
                    cbm_remap_preprocessed_calls(result, calls_before, orig_resolved_count,
                                                 preprocessed, source, source_len);
                    ts_tree_delete(pp_tree);
                }
            }
            cbm_preprocessed_source_free(preprocessed);
            atomic_fetch_add(&total_files_preprocessed, 1);
            (void)calls_before; // used for future logging
        } else {
            cbm_preprocessed_source_free(preprocessed);
        }
        atomic_fetch_add(&total_preprocess_ns, now_ns() - pp_start);
        atomic_fetch_add(&arena_bytes_pp, a->total_alloc - arena_mark);
    }

    // Bottleneck call-context metrics. Each call is attributed to the INNERMOST
    // enclosing Function/Method def by source-line range (defs and calls in one
    // CBMFileResult share the same file). Range matching is used instead of
    // enclosing_func_qn string matching because some grammars (notably C, whose
    // function_definition has no "name" field) attribute the call's scope to the
    // module rather than the function — line ranges are unambiguous and
    // language-agnostic. Bounded per file (defs x calls), not a repo-scale scan.
    int def_count = result->defs.count;
    bool *has_self = def_count > 0 ? (bool *)calloc((size_t)def_count, sizeof(bool)) : NULL;
    bool *has_guarded = def_count > 0 ? (bool *)calloc((size_t)def_count, sizeof(bool)) : NULL;

    // param_count is a standalone structural smell (independent of calls). Prefer
    // the parsed param_names array; fall back to counting from the signature text
    // for languages (e.g. C) that populate only the signature.
    for (int di = 0; di < def_count; di++) {
        CBMDefinition *d = &result->defs.items[di];
        int pc = 0;
        if (d->param_names) {
            while (d->param_names[pc]) {
                pc++;
            }
        }
        if (pc == 0 && d->signature) {
            pc = count_params_from_signature(d->signature);
        }
        d->param_count = pc;
    }

    for (int ci = 0; ci < orig_calls_count; ci++) {
        const CBMCall *c = &result->calls.items[ci];
        if (!c->callee_name || c->start_line <= 0 || c->requires_typed_resolution) {
            continue;
        }
        // Innermost enclosing Function/Method def by line range (smallest span).
        int best = -1;
        int best_span = -1;
        for (int di = 0; di < def_count; di++) {
            const CBMDefinition *d = &result->defs.items[di];
            if (!d->name || !d->label ||
                (strcmp(d->label, "Function") != 0 && strcmp(d->label, "Method") != 0)) {
                continue;
            }
            if ((int)d->start_line <= c->start_line && c->start_line <= (int)d->end_line) {
                int span = (int)d->end_line - (int)d->start_line;
                if (best < 0 || span < best_span) {
                    best_span = span;
                    best = di;
                }
            }
        }
        if (best < 0) {
            continue;
        }
        CBMDefinition *d = &result->defs.items[best];
        // callee_name may be bare ("recur") or qualified ("self.recur",
        // "super().save", "axios.get"). A short-name match alone is not
        // self-recursion: the callee must also target the same object
        // (is_self_receiver), or super().save() inside save and axios.get
        // inside get are false positives (#599).
        const char *dot = strrchr(c->callee_name, '.');
        const char *callee_short = dot ? dot + 1 : c->callee_name;
        bool in_loop = c->loop_depth > 0;

        if (strcmp(callee_short, d->name) == 0 && is_self_receiver(c->callee_name, d->receiver)) {
            // Direct self-recursion. The call graph omits self-edges (pass_calls
            // skips source==target), so detect it here; seeds "recursive".
            d->is_recursive = true;
            if (has_self) {
                has_self[best] = true;
            }
            if (in_loop) {
                d->recursion_in_loop = true; // recursion compounded by a loop
            }
            if (c->branch_depth > 0 && has_guarded) {
                has_guarded[best] = true; // a self-call guarded by some conditional
            }
        }
        if (in_loop && is_linear_scan_name(callee_short)) {
            d->linear_scan_in_loop++; // hidden O(n^2): linear scan inside a loop
        }
        if (in_loop && is_alloc_name(callee_short)) {
            d->alloc_in_loop++; // repeated allocation/append inside a loop
        }
    }

    // An implicit binary operator is a self-call only when type resolution
    // targets the enclosing method. Textual operator names also describe
    // primitive arithmetic and calls to operators on different receiver types.
    for (int i = 0; i < orig_resolved_count; ++i) {
        const auto &call = result->resolved_calls.items[i];
        if (!call.binary_operator_line || !call.caller_qn || !call.callee_qn ||
            strcmp(call.caller_qn, call.callee_qn) != 0)
            continue;
        for (int di = 0; di < def_count; ++di) {
            auto &def = result->defs.items[di];
            if (def.qualified_name && strcmp(def.qualified_name, call.caller_qn) == 0) {
                def.is_recursive = true;
                if (has_self)
                    has_self[di] = true;
                break;
            }
        }
    }

    // Recursive with no self-call guarded by any conditional → no obvious base
    // case on the recursive path: a stronger "potentially unbounded" signal.
    for (int di = 0; di < def_count; di++) {
        if (has_self && has_self[di] && !(has_guarded && has_guarded[di])) {
            result->defs.items[di].unguarded_recursion = true;
        }
    }
    free(has_self);
    free(has_guarded);

    uint64_t t2 = now_ns();

    /* Best-effort parse-coverage signal (#963): flag files whose tree contains
     * ERROR/MISSING regions. Computed AFTER extraction so definite recovery is
     * subtracted first — a region fully re-extracted as definitions is not a
     * miss, and a fully recovered file is not flagged at all. Detection aid
     * only: the absence of this flag is NOT a completeness guarantee. */
    if (ts_node_has_error(root)) {
        cbm_error_regions_t regs = {};
        if (strcmp(ts_node_type(root), "ERROR") == 0) {
            cbm_error_regions_push(&regs, root); /* whole file unparseable */
        } else {
            cbm_collect_error_regions(root, &regs, source, source_len);
        }
        /* Recovery subtraction runs on the RAW ranges: its evidence is a whole
         * definition that STARTS inside the range, so it must be asked while the
         * range still matches the construct. */
        cbm_subtract_recovered_regions(&regs, &result->defs);
        /* Shared by the two macro-invocation rules below. The line count comes
         * from the pp line map when there is one (same definition), and 0 means
         * not counted yet. The offset table is built once, only when a
         * function-like macro can make the checks run: without it every check
         * walks the source from byte 0 — bytes x lines on a whole-file error. A
         * failed allocation leaves the table NULL, and the checks walk instead. */
        CBMArena *cov_arena = scratch ? scratch : a;
        uint32_t newline_lines = pp_line_map ? pp_line_map_lines : 0;
        cbm_macro_names_t macros = {};
        int *line_offsets = nullptr;
        cbm_collect_function_like_macros(cov_arena, &result->defs, &macros);
        if (macros.count > 0 && regs.count > 0) {
            if (newline_lines == 0) {
                newline_lines = cbm_newline_line_count(source, source_len);
            }
            line_offsets = cbm_build_line_offsets(cov_arena, source, source_len, newline_lines);
        }
        /* #963: cut what is left down to the lines the preprocessed parse could
         * not explain. */
        if (pp_line_map) {
            cbm_refine_regions_with_pp_lines(&regs, pp_line_map, pp_line_map_lines, source,
                                             source_len, line_offsets, &result->defs, &macros);
        }
        /* #1071: don't flag a benign function-like-macro call (defined in-file)
         * that tree-sitter can't parse without the preprocessor. Runs AFTER the
         * refinement — its evidence is per-line, so a narrow range points at the
         * call itself. */
        cbm_subtract_macro_invocation_regions(&regs, &result->defs, &macros, source, source_len,
                                              line_offsets, newline_lines);
        /* A file whose kept list is empty but whose cap still bound is NOT clean:
         * the dropped ranges were never judged by the rules above. */
        if (regs.count > 0 || regs.dropped > 0) {
            result->parse_incomplete = true;
            result->error_region_count = regs.count;
            result->error_ranges = cbm_error_ranges_str(a, &regs);
            /* One range over nearly the whole file is noise, not advice. */
            if (regs.count == 1 && regs.dropped == 0) {
                if (newline_lines == 0) {
                    newline_lines = cbm_newline_line_count(source, source_len);
                }
                uint32_t total = cbm_count_lines(source, source_len, newline_lines);
                uint32_t span = regs.ends[0] - regs.starts[0] + 1;
                if (total > 0 && (uint64_t)span * 100 >= (uint64_t)total * CBM_UNUSABLE_PCT) {
                    result->parse_unusable = true;
                }
            }
        }
    }

    result->imports_count = result->imports.count;

    // Accumulate profiling counters
    atomic_fetch_add(&total_parse_ns, t1 - t0);
    atomic_fetch_add(&total_extract_ns, t2 - t1);
    atomic_fetch_add(&total_files, 1);

    // Retain tree for cross-file LSP reuse (caller frees via cbm_free_tree)
    result->cached_tree = tree;
    result->cached_lang = language;
    return result;
}

/* Owns the traversal scratch arena for the whole of one file's extraction:
 * created here, handed to the body as ctx->scratch, destroyed on the way out.
 * The body has many early returns, so bracketing it in a wrapper keeps that to
 * one create and one destroy. If the arena cannot be created, the body is
 * handed NULL and the traversal stacks fall back to the result arena, which is
 * what shipped before #1997. Traversal stacks were previously cut from the
 * result arena, which the parallel pass retains for every file until the
 * result cache is freed (#2010). */
static CBMFileResult *cbm_extract_file_impl(const char *source, int source_len,
                                            CBMLanguage language, const char *project,
                                            const char *rel_path, int64_t timeout_micros,
                                            const char **extra_defines, const char **include_paths,
                                            const CBMExtractOptions *options) {
    CBMArena scratch;
    cbm_arena_init_sized(&scratch, CBM_EXTRACT_SCRATCH_BLOCK);
    CBMFileResult *result = extract_file_impl_body(source, source_len, language, project, rel_path,
                                                   timeout_micros, extra_defines, include_paths,
                                                   options, scratch.nblocks > 0 ? &scratch : NULL);
    cbm_arena_destroy(&scratch);
    return result;
}

void cbm_free_result(CBMFileResult *result) {
    if (!result) {
        return;
    }
    if (result->cached_tree) {
        ts_tree_delete(result->cached_tree);
        result->cached_tree = NULL;
    }
    cbm_arena_destroy(&result->arena);
    free(result);
}

void cbm_free_tree(CBMFileResult *result) {
    if (result && result->cached_tree) {
        ts_tree_delete(result->cached_tree);
        result->cached_tree = NULL;
    }
}

void cbm_free_tree_ptr(TSTree *tree) {
    if (tree) {
        ts_tree_delete(tree);
    }
}
