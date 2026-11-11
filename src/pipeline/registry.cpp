
/*
 * registry.c — Function/Method/Class registry for call resolution.
 *
 * Indexes all callable symbols by qualified name and simple name.
 * Resolution uses a prioritized strategy chain:
 *   1. Import map lookup
 *   2. Same-module match
 *   3. Unique name (single candidate project-wide)
 *   4. Suffix match with import distance scoring
 */
#include "foundation/constants.h"

enum { REG_INIT_CAP = 16, REG_MIN_CANDIDATES = 3, REG_RESOLVED = 1, REG_SUFFIX_ALLOC = 2 };
/* Names with more registered definitions than this are unresolvable by name
 * alone: candidate_count_penalty already floors their confidence to ~3/count
 * (<= 0.006 at 256), so the emitted edge is noise — while the candidate walk
 * (reachability + scoring per candidate, re-done per file) is the dominant
 * resolution cost on identifier-dense repos. On the Linux kernel, 274 names
 * exceed 256 candidates ("list_head" 7188, "flags" 5520, "dev" 4374, ...) and
 * accounted for ~900 s of the 987 s usage-resolution CPU. Bail out early. */
enum { REG_MAX_CANDIDATES = 256 };
#define REG_FULL_CONF 1.0
#define REG_HALF_PENALTY 0.5

#define DEFAULT_CONFIDENCE 0.5
#include "pipeline/pipeline.h"
#include "foundation/compat.h" /* CBM_TLS */
#include "foundation/hash_table.h"
#include "foundation/dyn_array.h"
#include "foundation/platform.h"

#include "cbm.h"               /* cbm_label_is_relation — the resolve-time relation veto */
#include "discover/discover.h" /* cbm_language_for_filename */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Confidence score → human-readable band label. */
#define CONF_BAND_HIGH 0.7
#define CONF_BAND_MEDIUM 0.45
#define CONF_BAND_SPECULATIVE 0.25

const char *cbm_confidence_band(double score) {
    if (score >= CONF_BAND_HIGH) {
        return "high";
    }
    if (score >= CONF_BAND_MEDIUM) {
        return "medium";
    }
    if (score >= CONF_BAND_SPECULATIVE) {
        return "speculative";
    }
    return "";
}

/* ── Resolution confidence scores ────────────────────────────────── */
/* Strategy 1: import_map — direct import → high confidence */
#define CONF_IMPORT_MAP 0.95
#define CONF_IMPORT_MAP_SUFFIX 0.85
/* Strategy 2: same_module — same file/package → high confidence */
#define CONF_SAME_MODULE 0.90
/* Strategy 3: unique_name — only one candidate project-wide */
#define CONF_UNIQUE_NAME 0.75
/* Strategy 4: suffix_match — multiple candidates, filtered */
#define CONF_SUFFIX_MATCH 0.55
/* Fuzzy fallback: lower confidence */
#define CONF_FUZZY_SINGLE 0.40
#define CONF_FUZZY_MULTI 0.30
/* Candidate count penalty cap */
#define CANDIDATE_PENALTY_CAP 3.0

/* ── Internal types ──────────────────────────────────────────────── */

/* Array of QN strings for byName index */
typedef CBM_DYN_ARRAY(char *) qn_array_t;

struct cbm_registry {
    /* Interned label strings (<=~30 distinct labels; owned here, freed in
     * _free). The exact map's VALUES point into this pool instead of one
     * strdup per registered definition (~8.5M strdups on the kernel). */
    char *label_pool[64];
    int label_pool_n;
    /* exact: qualifiedName → label string (heap-owned copies) */
    CBMHashTable *exact;

    /* byName: simpleName → qn_array_t* (heap-owned) */
    CBMHashTable *by_name;

    /* C# reference-assembly stubs (`.../ref/<Asm>.cs` definitions): a set of
     * QNs borrowed from `exact`'s keys. NULL until the first one registers,
     * so a repository without them pays nothing (candidate_outranks_on_tie). */
    CBMHashTable *csharp_ref_stubs;
};

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Extract the last path segment from a QN. Returns pointer into s.
 * Recognizes both '.' (most langs) and Rust/C++ '::' separators, so a
 * scoped callee like "lib::square" yields "square" rather than the whole
 * scoped path (which never matches the by-name index). */
static const char *simple_name(const char *qn) {
    const char *dot = strrchr(qn, '.');
    const char *seg = dot ? dot + SKIP_ONE : qn;
    /* Find the last "::" and, if it sits after the last '.', use the
     * segment following it. */
    const char *colons = NULL;
    for (const char *p = qn; (p = strstr(p, "::")) != NULL; p += 2) {
        colons = p;
    }
    if (colons && colons + 2 > seg) {
        seg = colons + 2;
    }
    return seg;
}

/* Extract everything before the last dot. Returns heap-allocated string. */

/* Count common dot-separated prefix segments. */
static int common_prefix_len(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
    int count = 0;
    while (*a && *b) {
        /* Find next segment in each */
        const char *adot = strchr(a, '.');
        const char *bdot = strchr(b, '.');
        size_t alen = adot ? (size_t)(adot - a) : strlen(a);
        size_t blen = bdot ? (size_t)(bdot - b) : strlen(b);
        if (alen != blen || memcmp(a, b, alen) != 0) {
            break;
        }
        count++;
        a += alen + (adot ? SKIP_ONE : 0);
        b += blen + (bdot ? SKIP_ONE : 0);
        if (!adot || !bdot) {
            break;
        }
    }
    return count;
}

enum { REG_TEST_PENALTY = 1000 };

/* Check if a qualified name looks like a test/mock path. */
static bool is_test_qn(const char *qn) {
    if (!qn) {
        return false;
    }
    return (strstr(qn, "Test") != NULL || strstr(qn, "test") != NULL ||
            strstr(qn, "Mock") != NULL || strstr(qn, "mock") != NULL ||
            strstr(qn, "Stub") != NULL || strstr(qn, "stub") != NULL ||
            strstr(qn, "Fake") != NULL || strstr(qn, "fake") != NULL ||
            strstr(qn, "Fixture") != NULL || strstr(qn, "spec") != NULL);
}

/* Score a candidate for tiebreaking. Higher = better.
 * Layer 1: Non-test code preferred over test code (+1000)
 * Layer 2: Namespace proximity via common prefix length (+plen) */
static int candidate_score(const char *candidate_qn, const char *module_qn) {
    int score = 0;
    if (!is_test_qn(candidate_qn)) {
        score += REG_TEST_PENALTY;
    }
    score += common_prefix_len(candidate_qn, module_qn);
    return score;
}

/* How plainly a candidate is something a call can target, lower = better: a
 * function or method; then any other definition (a class for a constructor
 * call, a macro, a type); then a constructor; then a candidate whose label is
 * unknown; last a variable or field, which a call reaches only when it holds a
 * function. */
enum {
    REG_RANK_CALLABLE = 0,
    REG_RANK_OTHER_DEF = 1,
    REG_RANK_CONSTRUCTOR = 2,
    REG_RANK_UNKNOWN = 3,
    REG_RANK_DATA = 4,
};

/* True when the QN's last segment repeats the one before it (`ns.Status.Status`,
 * `java...Logger.Logger`): a constructor named after its type, in line or out
 * of line (`monitoring.statistics.StatisticsImpl.StatisticsImpl` from
 * `StatisticsImpl::StatisticsImpl` in statistics.cc). */
static bool constructor_qn(const char *qn) {
    const char *last = strrchr(qn, '.');
    if (!last || last == qn) {
        return false;
    }
    const char *prev = last;
    while (prev > qn && prev[-1] != '.') {
        prev--;
    }
    size_t prev_len = (size_t)(last - prev);
    return prev_len > 0 && strlen(last + 1) == prev_len && memcmp(prev, last + 1, prev_len) == 0;
}

static int callable_rank(const cbm_registry_t *r, const char *qn) {
    const char *label = r ? (const char *)cbm_ht_get(r->exact, qn) : NULL;
    if (!label) {
        return REG_RANK_UNKNOWN;
    }
    if (strcmp(label, "Variable") == 0 || strcmp(label, "Field") == 0) {
        return REG_RANK_DATA;
    }
    bool callable = strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0;
    /* A constructor shares its type's name, so a bare `Status` (a base class,
     * a declaration, `Status(...)`) ties between the class and `Status.Status`.
     * The name means the type: ranking the constructor as a callable lost
     * rocksdb 145 INHERITS and ~9k class USAGE edges to constructors. It
     * ranks below every other definition, the QN order cannot pick it over
     * its own type either in line or out of line, and a real same-named
     * function (`BytewiseComparator()`) still wins. A constructor is a Method
     * (or its Declaration / OverloadSet): a free Function named after its
     * module (`parse` in parse.c, QN `parse.parse`) is not one. */
    if ((strcmp(label, "Method") == 0 || strcmp(label, "OverloadSet") == 0 ||
         strcmp(label, "Declaration") == 0) &&
        constructor_qn(qn)) {
        return REG_RANK_CONSTRUCTOR;
    }
    return callable ? REG_RANK_CALLABLE : REG_RANK_OTHER_DEF;
}

/* A .NET reference assembly (`src/libraries/<Asm>/ref/<Asm>.cs`) declares an
 * assembly's public surface with `throw null` bodies in one shallow file; a
 * call that could equally mean the implementation means the implementation.
 * C# only (registered through cbm_registry_add_lang): elsewhere a `ref`
 * directory is an ordinary name. */
static bool csharp_reference_stub(const cbm_registry_t *r, const char *qn) {
    return r && r->csharp_ref_stubs && cbm_ht_get(r->csharp_ref_stubs, qn) != NULL;
}

/* The reference-assembly layout itself: src/libraries/<Asm>/ref/<file>.cs,
 * where <Asm> is a dotted assembly name and the file's first segment is one
 * of its segments (`System.Runtime.ref.System.Runtime...`,
 * `System.Linq.Parallel.ref.System.Linq...`). A source folder or namespace
 * that is merely called `ref` (`app.ref.Helpers`) repeats nothing and is not
 * a stub. Only the CSHARP_REF_ASM_SEGMENTS segments before `ref` are asked. */
enum { CSHARP_REF_ASM_SEGMENTS = 4 };

static bool csharp_reference_assembly_qn(const char *qn) {
    static const char marker[] = ".ref.";
    const size_t marker_len = sizeof(marker) - SKIP_ONE;
    for (const char *hit = strstr(qn, marker); hit; hit = strstr(hit + SKIP_ONE, marker)) {
        const char *next = hit + marker_len;
        size_t next_len = strcspn(next, ".");
        const char *end = hit;
        for (int k = 0; k < CSHARP_REF_ASM_SEGMENTS && end > qn; k++) {
            const char *seg = end;
            while (seg > qn && seg[-1] != '.') {
                seg--;
            }
            if ((size_t)(end - seg) == next_len && next_len > 0 &&
                strncmp(seg, next, next_len) == 0) {
                return true;
            }
            end = seg > qn ? seg - SKIP_ONE : qn;
        }
    }
    return false;
}

/* Total order among candidates that tie on candidate_score. Bucket position
 * (registration order) must never decide (upstream's kernel probe:
 * `dev_name` flipping between twenty same-named struct fields, `sg_set_buf`
 * between include/linux and tools/virtio). The rule, a pure function of the
 * candidate set (O9):
 *   1. the more plainly callable candidate (`dev_name` the function over the
 *      struct fields; Kotlin `where(...)` the method over the property); a
 *      constructor ranks after its type (callable_rank);
 *   2. for C#, an implementation over a reference-assembly stub;
 *   3. the lexicographically smaller QN (include/linux over tools/virtio).
 * Nesting depth is deliberately not a criterion (upstream 51364158): "least
 * nested wins" sent django's `Model.objects.filter` to the template
 * `Library.filter`, one segment shallower than `QuerySet.filter`.
 * `best_rank` is callable_rank(r, best), cached by the caller so a tie costs
 * one label lookup. */
static bool candidate_outranks_on_tie(const cbm_registry_t *r, const char *candidate,
                                      int candidate_rank, const char *best, int best_rank) {
    if (candidate_rank != best_rank) {
        return candidate_rank < best_rank;
    }
    bool cs = csharp_reference_stub(r, candidate);
    bool bs = csharp_reference_stub(r, best);
    if (cs != bs) {
        return !cs;
    }
    return strcmp(candidate, best) < 0;
}

/* Pick candidate with highest composite score (test-deprioritization + namespace
 * proximity), ties settled by candidate_outranks_on_tie. */
static const char *best_by_import_distance(const cbm_registry_t *r, const char **candidates,
                                           int count, const char *module_qn) {
    const char *best = NULL;
    int best_score = CBM_NOT_FOUND;
    int best_rank = REG_RANK_UNKNOWN;
    bool best_rank_known = false;
    for (int i = 0; i < count; i++) {
        int score = candidate_score(candidates[i], module_qn);
        /* Score ties break on the candidates themselves (kind, C# stub, then
         * lexicographic QN), not array position — the candidate array order
         * reflects registry insertion order, and an order-sensitive pick let
         * equally-scored ambiguous names (e.g. two Makefiles both defining an
         * `echo` target) resolve differently between identical runs. The
         * kind is looked up only on a tie, and the best's only once. */
        if (score > best_score) {
            best_score = score;
            best = candidates[i];
            best_rank_known = false;
            continue;
        }
        if (score != best_score || !best) {
            continue;
        }
        if (!best_rank_known) {
            best_rank = callable_rank(r, best);
            best_rank_known = true;
        }
        int rank = callable_rank(r, candidates[i]);
        if (candidate_outranks_on_tie(r, candidates[i], rank, best, best_rank)) {
            best = candidates[i];
            best_rank = rank;
        }
    }
    return best;
}

/* ── Per-file is_import_reachable memoization cache ───────────────
 *
 * The hot path on kubernetes spends ~50% of resolve_calls CPU in
 * is_import_reachable's O(candidates × imports × strstr) scan. The
 * SAME candidate_qn is re-evaluated dozens of times per file because
 * the same callee_name appears in multiple call sites and each lookup
 * re-checks all candidates for that name.
 *
 * Cache life-cycle is per-FILE (because import_vals changes between
 * files). resolve_file_calls calls _begin at file entry and _end at
 * file exit. Thread-local so each worker has its own cache without
 * contention. */
static CBM_TLS CBMHashTable *_reach_cache = NULL;

/* Sentinels stored as values in the cache. NULL means "not cached".
 * We need two distinct non-NULL pointers to encode true/false. */
#define REACH_CACHE_TRUE ((void *)(uintptr_t)1)
#define REACH_CACHE_FALSE ((void *)(uintptr_t)2)

static void reach_cache_free_key(const char *key, void *val, void *ud) {
    (void)val;
    (void)ud;
    free((char *)key);
}

void cbm_registry_reach_cache_begin(int estimated_capacity) {
    if (_reach_cache) {
        /* Defensive: caller forgot to call _end. Clear and reuse. */
        cbm_ht_foreach(_reach_cache, reach_cache_free_key, NULL);
        cbm_ht_clear(_reach_cache);
        return;
    }
    if (estimated_capacity < 16)
        estimated_capacity = 16;
    _reach_cache = cbm_ht_create((uint32_t)estimated_capacity);
}

void cbm_registry_reach_cache_end(void) {
    if (!_reach_cache)
        return;
    cbm_ht_foreach(_reach_cache, reach_cache_free_key, NULL);
    cbm_ht_free(_reach_cache);
    _reach_cache = NULL;
}

/* ── Per-file import-map prefix → module-QN hash ──────────────────
 *
 * resolve_import_map does a linear strcmp scan over the per-file
 * import list for every call (and every usage, throw, rw, def). On
 * kubernetes typical files have 20-30 imports and ~200 calls. That's
 * 4000-6000 strcmps per file × 20K files = ~80M strcmps.
 *
 * Build a hash from prefix → module_qn ONCE per file at file entry.
 * Each lookup becomes O(1). Keys and values are BORROWED from the
 * caller's import_map — they outlive the cache scope. */
static CBM_TLS CBMHashTable *_import_map_cache = NULL;

void cbm_registry_import_map_cache_begin(const char **keys, const char **vals, int count) {
    if (_import_map_cache) {
        cbm_ht_free(_import_map_cache);
        _import_map_cache = NULL;
    }
    if (!keys || !vals || count <= 0)
        return;
    _import_map_cache = cbm_ht_create((uint32_t)count * 2u + 8u);
    if (!_import_map_cache)
        return;
    for (int i = 0; i < count; i++) {
        if (keys[i] && vals[i]) {
            cbm_ht_set(_import_map_cache, keys[i], (void *)(uintptr_t)vals[i]);
        }
    }
}

void cbm_registry_import_map_cache_end(void) {
    if (!_import_map_cache)
        return;
    /* Keys/values borrowed from caller — no free callback needed. */
    cbm_ht_free(_import_map_cache);
    _import_map_cache = NULL;
}

/* ── Per-file full-result cache for cbm_registry_resolve ──────────
 *
 * THE big one: 98.7% of resolve_calls CPU on kubernetes lives inside
 * cbm_registry_resolve's strategy chain (suffix_match iterating
 * hundreds of candidates per common name like "Get"/"Add"/"New"
 * × 1M LSP-miss calls = ~800s of work).
 *
 * Since module_qn is CONSTANT per file, the resolution for a given
 * callee_name within a file is also constant — same name resolves
 * to the same QN every time. Cache the full cbm_resolution_t result
 * keyed by callee_name; first lookup does the full chain, repeats
 * are O(1). On K8s a typical file has ~200 calls but only ~50
 * unique callee_names → ~75% hit rate → ~75% of the resolve cost
 * eliminated. */
typedef struct {
    cbm_resolution_t res;
    size_t scope_len;  /* raw and Python-scoped requests may share one file cache */
    bool skip_members; /* a C call and a C reference to the same name share it */
} resolve_cache_entry_t;

static CBM_TLS CBMHashTable *_resolve_cache = NULL;
/* Entries are malloc'd; keys are strdup'd. Both freed in _end. */

static void resolve_cache_free_entry(const char *key, void *val, void *ud) {
    (void)ud;
    free((char *)key);
    free(val);
}

void cbm_registry_resolve_cache_begin(int estimated_capacity) {
    if (_resolve_cache) {
        cbm_ht_foreach(_resolve_cache, resolve_cache_free_entry, NULL);
        cbm_ht_free(_resolve_cache);
        _resolve_cache = NULL;
    }
    if (estimated_capacity < 32)
        estimated_capacity = 32;
    _resolve_cache = cbm_ht_create((uint32_t)estimated_capacity);
}

void cbm_registry_resolve_cache_end(void) {
    if (!_resolve_cache)
        return;
    cbm_ht_foreach(_resolve_cache, resolve_cache_free_entry, NULL);
    cbm_ht_free(_resolve_cache);
    _resolve_cache = NULL;
}

/* Check if candidate's module prefix appears in import map values.
 * Uses stack buffer to avoid malloc/free per call in hot resolution loop.
 * Per-file memoization via TLS cache: repeated lookups of the same
 * candidate_qn (same name appears in many call sites) become O(1)
 * after the first computation. */
static bool is_import_reachable(const char *candidate_qn, const char **import_vals,
                                int import_count) {
    if (_reach_cache) {
        void *cached = (void *)cbm_ht_get(_reach_cache, candidate_qn);
        if (cached == REACH_CACHE_TRUE)
            return true;
        if (cached == REACH_CACHE_FALSE)
            return false;
    }

    char cand_mod[CBM_SZ_512];
    const char *last = strrchr(candidate_qn, '.');
    if (last) {
        size_t len = (size_t)(last - candidate_qn);
        if (len >= sizeof(cand_mod)) {
            len = sizeof(cand_mod) - SKIP_ONE;
        }
        memcpy(cand_mod, candidate_qn, len);
        cand_mod[len] = '\0';
    } else {
        snprintf(cand_mod, sizeof(cand_mod), "%s", candidate_qn);
    }
    bool reachable = false;
    for (int i = 0; i < import_count; i++) {
        if (strstr(cand_mod, import_vals[i]) || strstr(import_vals[i], cand_mod)) {
            reachable = true;
            break;
        }
    }

    if (_reach_cache) {
        char *kdup = strdup(candidate_qn);
        if (kdup) {
            cbm_ht_set(_reach_cache, kdup, reachable ? REACH_CACHE_TRUE : REACH_CACHE_FALSE);
        }
    }
    return reachable;
}

/* Scale confidence inversely with candidate count. */
static double candidate_count_penalty(double base, int count) {
    if (count <= REG_MIN_CANDIDATES) {
        return base;
    }
    return base * fmin(REG_FULL_CONF, CANDIDATE_PENALTY_CAP / (double)count);
}

static cbm_resolution_t empty_result(void) {
    cbm_resolution_t r = {0};
    return r;
}

/* ── Perl builtin guard (#459 follow-up: call-graph noise) ──────────
 * Curated subset of perlfunc core builtins. When a Perl CALL resolves
 * only by the generic short-name matcher (no LSP, no import, after the
 * same-module/name-lookup chain), a builtin name like `push`/`shift`/
 * `keys` must NOT be wired to a project sub that merely shares the name
 * — that is virtually always a false positive. A genuine intra-project
 * call is resolved by earlier (LSP/textual) stages before this guard.
 * MUST stay sorted ASCII-ascending for bsearch. */
static const char *const PERL_BUILTINS[] = {
    "abs",       "atan2",   "binmode", "bless",     "caller",   "chdir",    "chmod",   "chomp",
    "chop",      "chown",   "chr",     "chroot",    "close",    "closedir", "cos",     "defined",
    "delete",    "die",     "do",      "each",      "eof",      "eval",     "exec",    "exists",
    "exit",      "fork",    "gmtime",  "goto",      "grep",     "hex",      "index",   "int",
    "join",      "keys",    "last",    "lc",        "lcfirst",  "length",   "local",   "localtime",
    "log",       "lstat",   "map",     "mkdir",     "my",       "next",     "oct",     "open",
    "opendir",   "ord",     "our",     "pop",       "pos",      "print",    "printf",  "push",
    "quotemeta", "rand",    "read",    "readdir",   "readline", "redo",     "ref",     "rename",
    "require",   "return",  "reverse", "rindex",    "rmdir",    "say",      "scalar",  "seek",
    "shift",     "sin",     "sleep",   "sort",      "splice",   "split",    "sprintf", "sqrt",
    "srand",     "stat",    "substr",  "system",    "time",     "uc",       "ucfirst", "undef",
    "unlink",    "unshift", "values",  "wantarray", "warn",     "write",
};

static int perl_builtin_cmp(const void *key, const void *elem) {
    return strcmp((const char *)key, *(const char *const *)elem);
}

/* True if `name` is one of the curated Perl core builtins. Used to suppress
 * generic-resolver CALLS edges from Perl builtin invocations to project subs
 * that happen to share the builtin's name. Perl-scoped: callers gate on the
 * file language so no other language's resolution is affected. */
bool cbm_perl_is_builtin(const char *name) {
    if (!name || !name[0]) {
        return false;
    }
    return bsearch(name, PERL_BUILTINS, sizeof(PERL_BUILTINS) / sizeof(PERL_BUILTINS[0]),
                   sizeof(PERL_BUILTINS[0]), perl_builtin_cmp) != NULL;
}

/* Decide whether a *resolved* Perl call edge is generic-resolver noise that
 * should be suppressed (#476). Returns true only for Perl, only for a builtin
 * invocation or a method call, and only when the registry landed the match via
 * a WEAK short-name strategy. High-confidence import/same-module strategies
 * (same_module, import_map, import_map_suffix) are KEPT so a genuine same-file
 * or imported call to a builtin-named sub still resolves — only the weak
 * short-name guesses (suffix_match, unique_name) are dropped. `strategy` is the
 * cbm_resolution_t.strategy of a non-empty match;
 * NULL/empty (no match) returns false. Pure + side-effect-free so the
 * suppression contract is unit-testable without a full pipeline. */
bool cbm_perl_suppress_generic_match(bool is_perl, bool is_method, const char *callee_name,
                                     const char *strategy) {
    if (!is_perl) {
        return false;
    }
    if (!(is_method || cbm_perl_is_builtin(callee_name))) {
        return false;
    }
    if (!strategy || !strategy[0]) {
        return false;
    }
    if (strcmp(strategy, "same_module") == 0 || strcmp(strategy, "import_map") == 0 ||
        strcmp(strategy, "import_map_suffix") == 0) {
        return false; /* high-confidence import/same-module match — keep the genuine edge */
    }
    return true; /* weak short-name match (suffix_match / unique_name / …) → drop */
}

/* The weak short-name strategies that actually reach the call-resolution
 * guards: the registry's suffix_match / unique_name and the parallel
 * field_type_hint. "fuzzy" is listed as defensive insurance only —
 * cbm_registry_fuzzy_resolve is not wired into the sequential/parallel resolvers
 * today, so it never reaches these helpers, but naming it keeps a future wiring
 * from silently reintroducing the noise. Everything else — same_module /
 * import_map / import_map_suffix / qualified_suffix / callee_suffix /
 * service_pattern / lsp_* — is a receiver- or import-aware match and is KEPT.
 *
 * Shared by BOTH weak-call guards below so the drop-list exists exactly once: a
 * list that drifted between the member guard and the local-binding guard would
 * make the two disagree about what "weak" means. */
bool cbm_weak_short_name_strategy(const char *strategy) {
    if (!strategy || !strategy[0]) {
        return false;
    }
    return strcmp(strategy, "suffix_match") == 0 || strcmp(strategy, "unique_name") == 0 ||
           strcmp(strategy, "field_type_hint") == 0 || strcmp(strategy, "fuzzy") == 0;
}

bool cbm_go_suppress_textual_field_call(bool is_go, const char *target_label,
                                        const char *strategy) {
    /* Companion of the #1942 reference guard, for CALLS. Once Go struct fields
     * are graph nodes (#1935) the short-name registry offers them as call
     * targets, so `ctx.Err()` bound a project struct's `Err` field and
     * `strings.Count()` a `Count` field by unique_name / suffix_match /
     * field_type_hint. A Go call can reach a field only through a selector on
     * a func-typed field, and the type-aware Go LSP resolves that shape; every
     * textual (non-lsp_*) strategy is a bare-name guess here. Go only: other
     * languages keep their existing member-call guards. */
    if (!is_go || !target_label || strcmp(target_label, "Field") != 0) {
        return false;
    }
    return !strategy || strncmp(strategy, "lsp_", 4) != 0;
}

bool cbm_suppress_bare_field_ref(CBMLanguage lang, bool is_member_access,
                                 const char *target_label) {
    /* #1942/#1962: a bare Go identifier can never denote a struct field --
     * field access is always a selector expression (x.f). The extractor
     * strips the receiver before the resolver runs (resolve_lhs_write_name
     * records the trailing field name; is_reference_node records the inner
     * field_identifier), so the reference TEXT cannot carry the distinction --
     * the recorded is_member_access shape can. Only a reference that was never
     * the member half of a selector is refused a Field bind.
     *
     * #2666: C has the same rule. A struct member is named only as the member
     * half of `a.b`, `a->b` or a designator `.b`, all recorded as
     * field_identifier; a bare C identifier (a macro argument such as
     * RUN_SELECTED_SUITE(mcp), a stray name in an unexpanded macro body) is
     * never one. The call path already drops a bare C call onto a Field
     * (cbm_call_onto_field_policy).
     *
     * Go- and C-gated: a C#/Java/C++/Python method body legitimately
     * references its own members bare, so a global veto would break those
     * languages. A `.h` is parsed as C++ and keeps that behavior. */
    if ((lang != CBM_LANG_GO && lang != CBM_LANG_C) || is_member_access || !target_label) {
        return false;
    }
    return strcmp(target_label, "Field") == 0;
}

/* Dynamic-language analogue of the Perl guard above (#592/#606/#1276
 * direction; precedent #477). A member call `x.foo()` reaches the weak textual
 * cascade ONLY when the language's LSP could not resolve the receiver type —
 * type-resolved calls win via lsp_* strategies before the registry runs.
 * Binding such a call to a project symbol by a weak short-name strategy
 * fabricates a CALLS edge (`re.test()` -> SalesforceRestClient.test,
 * `accelerator.print()` -> MockAccelerator.print). Drop ONLY the weak
 * strategies; keep import/same-module/qualified-tail matches and every lsp_*
 * strategy. Uses an EXPLICIT drop-list (not keep-list + default-drop) because
 * the parallel resolver runs lsp_* strategies through the same guard variable —
 * a default-drop would silently kill lsp_ts_method. Pure + side-effect-free so
 * the contract is unit-testable without a full pipeline.
 *
 * `enabled` is the per-language gate; cbm_suppress_weak_call below owns the
 * language set, so both resolvers read it from one place. */
bool cbm_suppress_weak_member_match(bool enabled, bool is_method, const char *strategy) {
    if (!enabled || !is_method) {
        return false;
    }
    return cbm_weak_short_name_strategy(strategy);
}

/* Bare-call counterpart of the member guard above. A Python call `foo()` whose
 * callee identifier is bound as a parameter of an enclosing scope cannot be the
 * module-level `foo`: the parameter shadows it for the whole body. Binding such
 * a call to a project Function/Method by a weak short-name strategy fabricates
 * the edge by construction (`def _run_with_heavy_slot(run): run()` ->
 * SatoriLive.run).
 *
 * This is deliberately NOT keyed on the callee's spelling. A list of
 * "generic-looking" names (get / run / execute) asserts that certain spellings
 * are usually noise, which is a claim about corpus fashion rather than about
 * what the resolver knew — and it ages invisibly. A parameter binding is a fact
 * about THIS file's scope, decidable outright.
 *
 * `enabled` is the per-language gate, set by cbm_suppress_weak_call below.
 * Pure; unit-tested in test_registry.cpp. */
bool cbm_suppress_weak_local_binding_call(bool enabled, bool callee_is_locally_bound,
                                          const char *strategy) {
    if (!enabled || !callee_is_locally_bound) {
        return false;
    }
    return cbm_weak_short_name_strategy(strategy);
}

/* Import-binding counterpart of the parameter guard above (#2127). An import
 * binds the identifier for the whole module; when that import is external it
 * never materializes, and the chain falls through to project-wide short-name
 * strategies (`from unittest.mock import patch; patch()` -> a REST view's
 * `PkgConfigView.patch`). The caller decides whether the binding contradicts
 * the target (cbm_python_import_binding_contradicts: a SCOPE FACT about this
 * file, not a spelling list); only weak strategies are dropped, so import_map /
 * same_module / lsp_* edges are untouched. Pure; unit-tested in test_registry.c. */
bool cbm_suppress_weak_import_bound_call(bool enabled, bool import_binding_contradicts,
                                         const char *strategy) {
    if (!enabled || !import_binding_contradicts) {
        return false;
    }
    return cbm_weak_short_name_strategy(strategy);
}

/* ── Python builtin-type members ─────────────────────────────────────────
 * Methods of Python's builtin types (str, bytes, list, dict, set, file objects)
 * plus the builtin functions that appear as attribute calls. Like PERL_BUILTINS
 * this is a fact about the LANGUAGE, not about corpus fashion: a member call
 * whose name is a builtin's own method — `parts.extend(...)`, `line.strip()`,
 * `d.items()` — is that builtin's method whenever the receiver is untyped, so a
 * project symbol that merely shares the spelling must not bind. MUST stay
 * sorted ASCII-ascending for bsearch. */
static const char *const PYTHON_BUILTIN_MEMBERS[] = {
    "add",        "append",       "capitalize",
    "casefold",   "center",       "clear",
    "close",      "copy",         "count",
    "decode",     "difference",   "discard",
    "encode",     "endswith",     "expandtabs",
    "extend",     "fileno",       "find",
    "flush",      "format",       "get",
    "index",      "insert",       "intersection",
    "isalnum",    "isalpha",      "isdecimal",
    "isdigit",    "isidentifier", "islower",
    "isnumeric",  "isprintable",  "isspace",
    "issubset",   "issuperset",   "istitle",
    "isupper",    "items",        "join",
    "keys",       "ljust",        "lower",
    "lstrip",     "partition",    "pop",
    "popitem",    "print",        "read",
    "readable",   "readline",     "readlines",
    "remove",     "replace",      "reverse",
    "rfind",      "rindex",       "rjust",
    "rpartition", "rsplit",       "rstrip",
    "seek",       "setdefault",   "sort",
    "split",      "splitlines",   "startswith",
    "strip",      "swapcase",     "symmetric_difference",
    "tell",       "title",        "truncate",
    "union",      "update",       "upper",
    "values",     "writable",     "write",
    "writelines", "zfill",
};

static int python_builtin_member_cmp(const void *key, const void *elem) {
    return strcmp((const char *)key, *(const char *const *)elem);
}

bool cbm_python_is_builtin_member(const char *name) {
    if (!name || !name[0]) {
        return false;
    }
    return bsearch(name, PYTHON_BUILTIN_MEMBERS,
                   sizeof(PYTHON_BUILTIN_MEMBERS) / sizeof(PYTHON_BUILTIN_MEMBERS[0]),
                   sizeof(PYTHON_BUILTIN_MEMBERS[0]), python_builtin_member_cmp) != NULL;
}

/* The member guard's one exemption. A Python member call whose receiver could
 * not be typed still carries real evidence when three facts line up: the callee
 * name has exactly ONE definition in the whole project (strategy unique_name),
 * the receiver is an attribute chain rooted at self/cls — an object the class
 * owns, not a parameter handed in — and the name is not a builtin type's own
 * method. `self.compiler.apply_converters()` names the only apply_converters in
 * django and nothing else it could mean exists (upstream measured 800 such
 * self-rooted edges dropped on django, every sampled one a real call). A bare
 * parameter receiver (`accelerator.backward()`, #1276's own case) carries no
 * ownership evidence and stays suppressed, as does every builtin-member name
 * (`self.parts.extend()`). Python only. */
bool cbm_weak_member_unique_name_exempt(bool is_python, bool receiver_is_self_attribute,
                                        const char *callee_name, const char *strategy) {
    if (!is_python || !receiver_is_self_attribute) {
        return false;
    }
    if (!strategy || strcmp(strategy, "unique_name") != 0) {
        return false;
    }
    if (!callee_name || !callee_name[0]) {
        return false;
    }
    return !cbm_python_is_builtin_member(simple_name(callee_name));
}

/* The weak-call guards above, with their language set in ONE place. Both the
 * sequential (pass_calls.cpp) and parallel (pass_parallel.cpp) resolvers call
 * this, so a language added here reaches both and they cannot diverge. The
 * member guard covers Python and the JS/TS family; the bare-call local-binding
 * guard is Python-only because only Python extraction sets
 * callee_is_locally_bound. */
bool cbm_suppress_weak_call(CBMLanguage lang, const CBMCall *call, const char *strategy) {
    bool member_lang = lang == CBM_LANG_PYTHON || lang == CBM_LANG_JAVASCRIPT ||
                       lang == CBM_LANG_TYPESCRIPT || lang == CBM_LANG_TSX ||
                       /* Files whose calls are embedded JS/TS (<script> bodies):
                        * the calls carry the JS receiver flag but the FILE
                        * language gated them out, so generated Dokka pages
                        * bound localStorage.getItem to a docs bundle (upstream
                        * probe: 4,207 junk edges on JetBrains/Exposed). */
                       lang == CBM_LANG_HTML || lang == CBM_LANG_VUE || lang == CBM_LANG_SVELTE ||
                       lang == CBM_LANG_ASTRO;
    bool local_binding_lang = lang == CBM_LANG_PYTHON;
    bool weak_member = cbm_suppress_weak_member_match(member_lang, call->is_method, strategy) &&
                       !cbm_weak_member_unique_name_exempt(lang == CBM_LANG_PYTHON,
                                                           call->receiver_is_self_attribute,
                                                           call->callee_name, strategy);
    return weak_member || cbm_suppress_weak_local_binding_call(
                              local_binding_lang, call->callee_is_locally_bound, strategy);
}

/* ── Lifecycle ──────────────────────────────────────────────────── */

cbm_registry_t *cbm_registry_new(void) {
    cbm_registry_t *r = (cbm_registry_t *)calloc(CBM_ALLOC_ONE, sizeof(cbm_registry_t));
    if (!r) {
        return NULL;
    }
    r->exact = cbm_ht_create(CBM_SZ_1K);
    r->by_name = cbm_ht_create(CBM_SZ_512);
    return r;
}

static void free_label(const char *key, void *value, void *ud) {
    (void)ud;
    (void)value; /* interned in the registry's label_pool */
    free((void *)key);
}

static void free_qn_array(const char *key, void *value, void *ud) {
    (void)ud;
    qn_array_t *arr = (qn_array_t *)value;
    if (arr) {
        /* items borrow the exact map's keys — freed there, not here */
        cbm_da_free(arr);
        free(arr);
    }
    free((void *)key);
}

void cbm_registry_free(cbm_registry_t *r) {
    if (!r) {
        return;
    }
    /* by_name first: its items borrow exact's keys. */
    cbm_ht_foreach(r->by_name, free_qn_array, NULL);
    cbm_ht_free(r->by_name);
    if (r->csharp_ref_stubs) {
        cbm_ht_free(r->csharp_ref_stubs); /* keys borrowed from exact */
    }
    cbm_ht_foreach(r->exact, free_label, NULL);
    cbm_ht_free(r->exact);
    for (int i = 0; i < r->label_pool_n; i++) {
        free(r->label_pool[i]);
    }
    free(r);
}

/* ── Registration ────────────────────────────────────────────────── */

void cbm_registry_add(cbm_registry_t *r, const char *name, const char *qualified_name,
                      const char *label) {
    cbm_registry_add_lang(r, name, qualified_name, label, CBM_LANG_COUNT);
}

void cbm_registry_add_lang(cbm_registry_t *r, const char *name, const char *qualified_name,
                           const char *label, CBMLanguage lang) {
    (void)name;
    if (!r || !qualified_name || !label) {
        return;
    }
    // Name-only resolution can identify a family, not an individual overload.
    // Type-aware resolution uses its separate registry and retains these QNs.
    if (strstr(qualified_name, "@overload_"))
        return;

    /* Check for duplicate */
    if (cbm_ht_get(r->exact, qualified_name)) {
        return;
    }

    /* Intern the label (bounded set; linear scan is fine at this size). */
    const char *interned = NULL;
    for (int i = 0; i < r->label_pool_n; i++) {
        if (strcmp(r->label_pool[i], label) == 0) {
            interned = r->label_pool[i];
            break;
        }
    }
    if (!interned && r->label_pool_n < (int)(sizeof(r->label_pool) / sizeof(r->label_pool[0]))) {
        r->label_pool[r->label_pool_n] = strdup(label);
        interned = r->label_pool[r->label_pool_n];
        r->label_pool_n++;
    }
    if (!interned) {
        return; /* pool exhausted (cannot happen with sane label sets) */
    }

    /* Store in exact map: QN → interned label. The key is the registry's ONE
     * owned copy of the QN; by_name below borrows it (same lifetime) instead
     * of a second strdup — this pair of copies was ~280 MB on the kernel. */
    cbm_ht_set(r->exact, strdup(qualified_name), (void *)interned);
    const char *owned_qn = cbm_ht_get_key(r->exact, qualified_name);

    /* Index by simple name.
     * No array dedup needed: exact-map check above guarantees uniqueness. */
    const char *simple = simple_name(qualified_name);
    qn_array_t *arr = (qn_array_t *)cbm_ht_get(r->by_name, simple);
    if (!arr) {
        arr = (__typeof__(arr))calloc(CBM_ALLOC_ONE, sizeof(qn_array_t));
        cbm_ht_set(r->by_name, strdup(simple), arr);
    }
    cbm_da_push(arr, (char *)owned_qn);

    if (lang == CBM_LANG_CSHARP && csharp_reference_assembly_qn(owned_qn)) {
        if (!r->csharp_ref_stubs) {
            r->csharp_ref_stubs = cbm_ht_create(REG_INIT_CAP);
        }
        if (r->csharp_ref_stubs) {
            cbm_ht_set(r->csharp_ref_stubs, owned_qn, (void *)owned_qn);
        }
    }
}

/* ── Lookup ──────────────────────────────────────────────────────── */

bool cbm_registry_exists(const cbm_registry_t *r, const char *qn) {
    if (!r || !qn) {
        return false;
    }
    return cbm_ht_get(r->exact, qn) != NULL;
}

const char *cbm_registry_label_of(const cbm_registry_t *r, const char *qn) {
    if (!r || !qn) {
        return NULL;
    }
    return (const char *)cbm_ht_get(r->exact, qn);
}

int cbm_registry_find_by_name(const cbm_registry_t *r, const char *name, const char ***out,
                              int *count) {
    if (!r || !out || !count) {
        return CBM_NOT_FOUND;
    }
    qn_array_t *arr = (qn_array_t *)cbm_ht_get(r->by_name, name);
    if (arr && arr->count > 0) {
        *out = (const char **)arr->items;
        *count = arr->count;
    } else {
        *out = NULL;
        *count = 0;
    }
    return 0;
}

int cbm_registry_size(const cbm_registry_t *r) {
    return r ? (int)cbm_ht_count(r->exact) : 0;
}

/* ── Resolution ──────────────────────────────────────────────────── */

/* Callback context for import_map_suffix scan */
/* Strategy 1: Import map lookup (exact → suffix fallback) */
static cbm_resolution_t resolve_import_map(const cbm_registry_t *r, const char *prefix,
                                           const char *suffix, const char **keys, const char **vals,
                                           int map_count) {
    if (!keys || !vals || map_count <= 0) {
        return empty_result();
    }

    /* Find prefix in import map keys. Prefer the per-file TLS hash
     * cache (O(1)) over the linear scan when available. */
    const char *resolved = NULL;
    if (_import_map_cache) {
        resolved = (const char *)cbm_ht_get(_import_map_cache, prefix);
    } else {
        for (int i = 0; i < map_count; i++) {
            if (strcmp(keys[i], prefix) == 0) {
                resolved = vals[i];
                break;
            }
        }
    }
    if (!resolved) {
        return empty_result();
    }

    /* Build candidate: resolved.suffix or resolved.prefix.
     * When the callee has a dot ("pkg.Func"), prefix="pkg" is the import key
     * and suffix="Func" is the function name, so the target QN is
     * resolved.Func. When the callee is bare ("requireAdmin"), prefix IS the
     * function name and suffix is NULL, so the target QN must be
     * resolved.requireAdmin — not just resolved, which would point at the
     * module node and miss the function entirely. */
    /* Direct hit ONLY for suffix-less callees (an aliased direct-symbol
     * import called bare: `from m import f as g; g()` — #875/#979). With a
     * suffix present (`imported.method()`), returning the bare base here
     * would swallow the suffix and bind the call to the imported symbol's
     * own node (a Variable/Class/module) instead of base.method — exactly
     * the mis-resolution the comment above warns about. That regressed
     * django-scale graphs by ~11K CALLS/TESTS edges (Signal.send calls
     * degraded to edges onto the signal variables themselves). #1000 */
    if (!suffix || !suffix[0]) {
        const char *direct = cbm_ht_get_key(r->exact, resolved);
        if (direct) {
            return (cbm_resolution_t){direct, "import_map", CONF_IMPORT_MAP, REG_RESOLVED};
        }
    }
    char candidate[CBM_SZ_512];
    if (suffix && suffix[0]) {
        snprintf(candidate, sizeof(candidate), "%s.%s", resolved, suffix);
    } else {
        snprintf(candidate, sizeof(candidate), "%s.%s", resolved, prefix);
    }
    /* Use cbm_ht_get_key to get the persistent heap-owned key string */
    const char *stored_key = cbm_ht_get_key(r->exact, candidate);
    if (stored_key) {
        return (cbm_resolution_t){stored_key, "import_map", CONF_IMPORT_MAP, REG_RESOLVED};
    }

    /* import_map_suffix fallback: find a QN starting with resolved+"." and
     * ending with "."+suffix. Any such QN's last segment equals the last
     * segment of suffix, so probe the by_name index and tail-check the (few)
     * candidates instead of cbm_ht_foreach over the WHOLE exact table — that
     * scan ran per unresolved call and dominated elasticsearch's resolve
     * phase (94% of samples: 700k-entry foreach + strlen per entry). */
    if (suffix && suffix[0]) {
        char resolved_dot[CBM_SZ_512];
        char dot_suffix[CBM_SZ_256];
        snprintf(resolved_dot, sizeof(resolved_dot), "%s.", resolved);
        snprintf(dot_suffix, sizeof(dot_suffix), ".%s", suffix);
        qn_array_t *arr = (qn_array_t *)cbm_ht_get(r->by_name, simple_name(suffix));
        if (arr) {
            size_t rd_len = strlen(resolved_dot);
            size_t ds_len = strlen(dot_suffix);
            for (int i = 0; i < arr->count; i++) {
                const char *qn = arr->items[i];
                size_t klen = strlen(qn);
                if (klen >= rd_len + ds_len && strncmp(qn, resolved_dot, rd_len) == 0 &&
                    strcmp(qn + klen - ds_len, dot_suffix) == 0) {
                    return (cbm_resolution_t){qn, "import_map_suffix", CONF_IMPORT_MAP_SUFFIX,
                                              REG_RESOLVED};
                }
            }
        }
    }
    return empty_result();
}

/* Exact same-module lookup without ever hashing a truncated candidate. The
 * long path uses the canonical short-name bucket, not heuristic name limits. */
static const char *same_module_candidate(const cbm_registry_t *r, const char *module_qn,
                                         size_t scope_len, const char *name) {
    if (!module_qn || !name) {
        return NULL;
    }
    size_t name_len = strlen(name);
    char candidate[CBM_SZ_512];
    if (scope_len < sizeof(candidate) && name_len < sizeof(candidate) - scope_len - SKIP_ONE) {
        memcpy(candidate, module_qn, scope_len);
        candidate[scope_len] = '.';
        memcpy(candidate + scope_len + SKIP_ONE, name, name_len + SKIP_ONE);
        return cbm_ht_get_key(r->exact, candidate);
    }
    const qn_array_t *arr = (const qn_array_t *)cbm_ht_get(r->by_name, simple_name(name));
    if (!arr) {
        return NULL;
    }
    for (int i = 0; i < arr->count; i++) {
        const char *qn = arr->items[i];
        if (!qn) {
            continue;
        }
        size_t qn_len = strlen(qn);
        if (qn_len > scope_len && qn_len - scope_len - SKIP_ONE == name_len &&
            memcmp(qn, module_qn, scope_len) == 0 && qn[scope_len] == '.' &&
            memcmp(qn + scope_len + SKIP_ONE, name, name_len) == 0) {
            return qn;
        }
    }
    return NULL;
}

/* Strategy 2: Same-module match. Python package files (`pkg/__init__.py`)
 * look their symbols up under the package scope (upstream 132ded8c); the
 * scoring strategies elsewhere keep the raw file module QN. */
static cbm_resolution_t resolve_same_module(const cbm_registry_t *r, const char *callee_name,
                                            const char *suffix, const char *module_qn,
                                            size_t scope_len) {
    const char *stored_key = same_module_candidate(r, module_qn, scope_len, callee_name);
    if (!stored_key && suffix && suffix[0]) {
        stored_key = same_module_candidate(r, module_qn, scope_len, suffix);
    }
    if (stored_key) {
        return (cbm_resolution_t){stored_key, "same_module", CONF_SAME_MODULE, REG_RESOLVED};
    }
    return empty_result();
}

/* Strategy 4: multiple candidates with import filtering. */
static cbm_resolution_t resolve_multi_with_imports(const cbm_registry_t *r, const qn_array_t *arr,
                                                   const char *module_qn, const char **import_vals,
                                                   int import_count) {
    const char *filtered[CBM_SZ_256];
    int fcount = 0;
    for (int i = 0; i < arr->count && fcount < CBM_SZ_256; i++) {
        if (is_import_reachable(arr->items[i], import_vals, import_count)) {
            filtered[fcount++] = arr->items[i];
        }
    }
    if (fcount == SKIP_ONE) {
        double conf = candidate_count_penalty(CONF_SUFFIX_MATCH, arr->count);
        return (cbm_resolution_t){filtered[0], "suffix_match", conf, arr->count};
    }
    if (fcount > SKIP_ONE) {
        const char *best = best_by_import_distance(r, filtered, fcount, module_qn);
        if (best) {
            double conf = candidate_count_penalty(CONF_SUFFIX_MATCH, fcount);
            return (cbm_resolution_t){best, "suffix_match", conf, fcount};
        }
    }
    /* No import-reachable — use all candidates with penalty */
    const char *best = best_by_import_distance(r, (const char **)arr->items, arr->count, module_qn);
    if (best) {
        double conf = candidate_count_penalty(CONF_SUFFIX_MATCH * REG_HALF_PENALTY, arr->count);
        return (cbm_resolution_t){best, "suffix_match", conf, arr->count};
    }
    return empty_result();
}

/* Confidence for a full qualified-tail match (Strategy 3.5). A package- or
 * namespace-qualified callee that uniquely matches one candidate's full tail is
 * as trustworthy as a same-module hit. */
#define CONF_QUALIFIED_SUFFIX 0.90

/* Copy callee_name into out (capacity cap, always NUL-terminated, truncated to
 * fit), normalizing "::" to "." so a qualified callee composes with dotted
 * candidate QNs. Returns the written length. */
static size_t dotted_callee(const char *callee_name, char *out, size_t cap) {
    size_t w = 0;
    for (const char *s = callee_name; *s && w + SKIP_ONE < cap;) {
        if (s[0] == ':' && s[1] == ':') {
            out[w++] = '.';
            s += 2;
        } else {
            out[w++] = *s++;
        }
    }
    out[w] = '\0';
    return w;
}

/* When a callee is package/namespace-qualified (Foo::Bar::sub or Foo.Bar.sub),
 * disambiguate among same-simple-name candidates by matching the FULL qualified
 * tail against each candidate QN at a segment boundary. Returns the sole
 * candidate whose QN equals or ends with ".<dotted-callee>", or NULL when zero
 * or several candidates match (the caller then falls back to bare-name scoring).
 *
 * Fixes qualified cross-file calls collapsing onto one namespace when the bare
 * symbol name is defined in several — e.g. Perl's Foo::Bar::run and Foo::Baz::run
 * both reduce to "run", so the bare-name scorer would route every caller to a
 * single winner. Language agnostic: callees with no separator return NULL and
 * leave behavior unchanged. */
static const char *qualified_suffix_match(const qn_array_t *arr, const char *callee_name) {
    char dotted[CBM_SZ_512];
    size_t w = dotted_callee(callee_name, dotted, sizeof(dotted));
    /* Must be qualified (contain a '.') — a bare name matches every candidate
     * and carries no disambiguating signal. */
    if (!strchr(dotted, '.')) {
        return NULL;
    }
    const char *match = NULL;
    for (int i = 0; i < arr->count; i++) {
        const char *qn = arr->items[i];
        size_t qlen = strlen(qn);
        if (qlen < w) {
            continue;
        }
        const char *tail = qn + (qlen - w);
        if (strcmp(tail, dotted) != 0) {
            continue;
        }
        /* Segment boundary: tail is the whole QN or is preceded by '.'. */
        if (tail != qn && tail[-1] != '.') {
            continue;
        }
        if (match) {
            return NULL; /* ambiguous — more than one qualified tail matches */
        }
        match = qn;
    }
    return match;
}

/* A dotted callee whose FIRST segment starts upper-case names a type — URLSession,
 * Calendar, JSONEncoder. That receiver chain is evidence the bare-name scorers
 * throw away, and throwing it away binds Foundation's URLSession.shared.data to
 * a project's own PickedFile.data: high confidence, and nothing in the graph
 * shows it is wrong. Require instead that the candidate's own parent segment
 * appears somewhere in the chain. Calendar.utcGregorian.startOfDayUTC resolving
 * to AuthDTOs.Calendar.startOfDayUTC passes, because Calendar is in the chain.
 *
 * Only an upper-case first segment is guarded. A lower-case root names a value
 * (vm.load, http.Get, os.path.join) whose declared type the chain does not
 * show, so the chain proves nothing there and the call passes through
 * unchanged. A callee with no separator passes through as well.
 *
 * Language agnostic by design: the registry holds no language, and every
 * language that writes receiver chains gains the same protection. */
static bool receiver_chain_admits(const char *callee_name, const char *candidate_qn) {
    /* Cheap rejects before the copy. A root that is not an upper-case letter
     * (including a leading "::", which normalizes to '.') names a value, not a
     * type. */
    if (callee_name[0] < 'A' || callee_name[0] > 'Z') {
        return true;
    }
    if (!strchr(callee_name, '.') && !strstr(callee_name, "::")) {
        return true; /* bare name — no receiver chain to judge */
    }
    char dotted[CBM_SZ_512];
    dotted_callee(callee_name, dotted, sizeof(dotted));
    const char *last_dot = strrchr(dotted, '.');
    if (!last_dot) {
        return true; /* the separator fell past the truncation point */
    }
    /* A name written in capitals with underscores is a constant holding a
     * value, not a type: ISO_4217_URL.lower is a string's own method. JSON and
     * URL carry no underscore and stay guarded. */
    bool has_underscore = false;
    bool all_caps = true;
    for (const char *c = dotted; c < last_dot && *c != '.'; c++) {
        if (*c == '_') {
            has_underscore = true;
        } else if (*c >= 'a' && *c <= 'z') {
            all_caps = false;
            break;
        }
    }
    if (all_caps && has_underscore) {
        return true;
    }

    /* The candidate's ancestry: every segment before its final name. */
    const char *cand_last = strrchr(candidate_qn, '.');
    if (!cand_last || cand_last == candidate_qn) {
        return true; /* top-level candidate — no ancestry to look for */
    }

    /* Walk the chain — every segment before the final callee name. A trailing
     * "()" is dropped so JSONEncoder().encode reads as JSONEncoder. A chain
     * segment admits the candidate when it names ANY segment of the candidate's
     * ancestry, not only the immediate parent: Settings.builder().putList binds
     * settings.Settings.Builder.putList because Settings owns Builder, and
     * RequestOptions.DEFAULT.toBuilder().setWarningsHandler binds
     * RequestOptions.Builder.setWarningsHandler the same way (upstream measured
     * 9,473 refused unique-name calls on elasticsearch, most of them these
     * factory chains into a nested Builder). A foreign root still finds nothing
     * in the ancestry — Base64.getEncoder does not admit
     * DocOffsetsCodec.getEncoder, URLSession.shared.data does not admit
     * PickedFile.data. */
    for (const char *seg = dotted; seg < last_dot;) {
        const char *end = strchr(seg, '.');
        size_t len = (size_t)(end - seg);
        if (len >= 2 && seg[len - 2] == '(' && seg[len - 1] == ')') {
            len -= 2; /* an empty "()" — JSONEncoder().encode names JSONEncoder */
        }
        for (const char *anc = candidate_qn; anc < cand_last;) {
            const char *anc_end = strchr(anc, '.');
            size_t anc_len = (size_t)(anc_end - anc);
            if (anc_len == len && len > 0 && strncmp(seg, anc, len) == 0) {
                return true;
            }
            anc = anc_end + SKIP_ONE;
        }
        seg = end + SKIP_ONE;
    }
    return false;
}

/* The same-name candidates without the members -- Field and Method nodes --
 * in `view`, backed by the caller's buffer of at most REG_MAX_CANDIDATES
 * entries. */
static void candidates_without_members(const cbm_registry_t *r, const qn_array_t *arr,
                                       qn_array_t *view, char **items) {
    int n = 0;
    for (int i = 0; i < arr->count; i++) {
        const char *label = cbm_registry_label_of(r, arr->items[i]);
        if (label && (strcmp(label, "Field") == 0 || strcmp(label, "Method") == 0)) {
            continue;
        }
        items[n++] = arr->items[i];
    }
    memset(view, 0, sizeof(*view));
    view->items = items;
    view->count = n;
    view->cap = n;
}

/* Strategy 3+4: Name lookup + suffix match. skip_members: the callee cannot
 * name a member (a bare C call: C has no methods, and a struct member is only
 * ever reached through its object), so Field and Method nodes do not compete.
 * With a Field in the bucket, a unique header function became a suffix_match
 * that the #725 guard then refused (.h is C++): kernel `put_page(page)`
 * 345 -> 9 callers (upstream 2026-10-13 graph audit). Dropping only the
 * Fields made libc `read()` the unique name of a C++ adapter's Method. */
static cbm_resolution_t resolve_name_lookup(const cbm_registry_t *r, const char *callee_name,
                                            const char *module_qn, const char **import_vals,
                                            int import_count, bool skip_members) {
    const char *lookup = simple_name(callee_name);
    qn_array_t *arr = (qn_array_t *)cbm_ht_get(r->by_name, lookup);
    if (!arr || arr->count == 0) {
        return empty_result();
    }
    if (arr->count > REG_MAX_CANDIDATES) {
        return empty_result(); /* unresolvably ambiguous — see REG_MAX_CANDIDATES */
    }
    qn_array_t view;
    // cppcheck-suppress variableScope ; view.items points here
    char *view_items[REG_MAX_CANDIDATES];
    if (skip_members) {
        candidates_without_members(r, arr, &view, view_items);
        if (view.count == 0) {
            return empty_result();
        }
        arr = &view;
    }

    /* Strategy 3.5: a qualified callee disambiguates among multiple same-name
     * candidates by full qualified tail, before bare-name scoring collapses
     * them onto a single winner. */
    if (arr->count > 1) {
        const char *q = qualified_suffix_match(arr, callee_name);
        if (q) {
            return (cbm_resolution_t){q, "qualified_suffix", CONF_QUALIFIED_SUFFIX, REG_RESOLVED};
        }
    }

    /* Strategy 3: unique name */
    if (arr->count == SKIP_ONE) {
        if (!receiver_chain_admits(callee_name, arr->items[0])) {
            return empty_result();
        }
        double conf = CONF_UNIQUE_NAME;
        if (import_vals && import_count > 0 &&
            !is_import_reachable(arr->items[0], import_vals, import_count)) {
            conf *= DEFAULT_CONFIDENCE;
        }
        return (cbm_resolution_t){arr->items[0], "unique_name", conf, REG_RESOLVED};
    }

    /* Strategy 4: multiple candidates */
    if (import_vals && import_count > 0) {
        return resolve_multi_with_imports(r, arr, module_qn, import_vals, import_count);
    }
    const char *best = best_by_import_distance(r, (const char **)arr->items, arr->count, module_qn);
    if (best) {
        if (!receiver_chain_admits(callee_name, best)) {
            return empty_result();
        }
        double conf = candidate_count_penalty(CONF_SUFFIX_MATCH, arr->count);
        return (cbm_resolution_t){best, "suffix_match", conf, arr->count};
    }
    return empty_result();
}

/* The strategy chain shared by both public resolve variants. No caching here —
 * cbm_registry_resolve owns the per-file cache. */
static cbm_resolution_t registry_resolve_chain(const cbm_registry_t *r, const char *callee_name,
                                               const char *module_qn, const char **import_map_keys,
                                               const char **import_map_vals, int import_map_count,
                                               size_t scope_len, bool skip_members) {
    /* Split callee at the first path separator: "pkg.Func" → prefix="pkg",
     * suffix="Func".  Rust/C++ use "::" rather than ".", so honor whichever
     * separator appears first ("lib::square" → prefix="lib", suffix="square").
     * Both separators are unambiguous, so handling "::" never affects "."-only
     * callees. */
    char prefix[CBM_SZ_256] = {0};
    const char *suffix = NULL;
    const char *dot = strchr(callee_name, '.');
    const char *colons = strstr(callee_name, "::");
    const char *sep = dot;
    size_t sep_len = SKIP_ONE; /* length of '.' */
    if (colons && (!sep || colons < sep)) {
        sep = colons;
        sep_len = 2; /* length of "::" */
    }
    if (sep) {
        size_t plen = sep - callee_name;
        if (plen >= sizeof(prefix)) {
            plen = sizeof(prefix) - SKIP_ONE;
        }
        memcpy(prefix, callee_name, plen);
        prefix[plen] = '\0';
        suffix = sep + sep_len;
    } else {
        snprintf(prefix, sizeof(prefix), "%s", callee_name);
    }

    /* Strategy 1: import map */
    cbm_resolution_t res =
        resolve_import_map(r, prefix, suffix, import_map_keys, import_map_vals, import_map_count);
    if (!(res.qualified_name && res.qualified_name[0])) {
        /* Strategy 2: same module */
        res = resolve_same_module(r, callee_name, suffix, module_qn, scope_len);
    }
    if (!(res.qualified_name && res.qualified_name[0])) {
        /* Strategy 3+4: name lookup */
        res = resolve_name_lookup(r, callee_name, module_qn, import_map_vals, import_map_count,
                                  skip_members);
    }

    return res;
}

static cbm_resolution_t registry_resolve_scoped(const cbm_registry_t *r, const char *callee_name,
                                                const char *module_qn, const char **import_map_keys,
                                                const char **import_map_vals, int import_map_count,
                                                size_t scope_len, bool skip_members) {
    if (!r || !callee_name) {
        return empty_result();
    }

    /* Per-file cache: same callee_name in N call sites → 1 chain walk
     * + N-1 O(1) hash hits. module_qn is constant per file so the
     * cache key only needs callee_name (plus the effective scope and the member choice). */
    resolve_cache_entry_t *cached =
        _resolve_cache ? (resolve_cache_entry_t *)cbm_ht_get(_resolve_cache, callee_name) : NULL;
    if (cached && cached->scope_len == scope_len && cached->skip_members == skip_members) {
        return cached->res;
    }

    cbm_resolution_t res =
        registry_resolve_chain(r, callee_name, module_qn, import_map_keys, import_map_vals,
                               import_map_count, scope_len, skip_members);

    /* Data relations (Table/View) are LINEAGE-ONLY registry members. Common
     * table names (users, orders, config) collide with code identifiers in
     * every language, so no CALLS/USAGE/READS/WRITES/THROWS/handler/decorator
     * consumer may bind them. The SQL lineage path opts in explicitly through
     * cbm_registry_resolve_lineage. */
    if (res.qualified_name && res.qualified_name[0] &&
        cbm_label_is_relation(cbm_registry_label_of(r, res.qualified_name))) {
        res = empty_result();
    }

    /* Cache the result (including empty — caching the negative answer
     * is just as valuable; same name asks the same question). */
    if (cached) {
        /* Same key, different effective scope or member choice: reuse its owned entry/key. */
        cached->res = res;
        cached->scope_len = scope_len;
        cached->skip_members = skip_members;
    } else if (_resolve_cache) {
        resolve_cache_entry_t *e = (resolve_cache_entry_t *)malloc(sizeof(*e));
        if (e) {
            e->res = res;
            e->scope_len = scope_len;
            e->skip_members = skip_members;
            char *kdup = strdup(callee_name);
            if (kdup) {
                cbm_ht_set(_resolve_cache, kdup, e);
            } else {
                free(e);
            }
        }
    }
    return res;
}

cbm_resolution_t cbm_registry_resolve(const cbm_registry_t *r, const char *callee_name,
                                      const char *module_qn, const char **import_map_keys,
                                      const char **import_map_vals, int import_map_count) {
    return registry_resolve_scoped(r, callee_name, module_qn, import_map_keys, import_map_vals,
                                   import_map_count, module_qn ? strlen(module_qn) : 0, false);
}

cbm_resolution_t cbm_registry_resolve_lang(const cbm_registry_t *r, const char *callee_name,
                                           const char *module_qn, const char **import_map_keys,
                                           const char **import_map_vals, int import_map_count,
                                           CBMLanguage lang) {
    size_t scope_len = lang == CBM_LANG_PYTHON ? cbm_fqn_symbol_scope_len(module_qn)
                                               : (module_qn ? strlen(module_qn) : 0);
    return registry_resolve_scoped(r, callee_name, module_qn, import_map_keys, import_map_vals,
                                   import_map_count, scope_len, false);
}

cbm_resolution_t cbm_registry_resolve_call_lang(const cbm_registry_t *r, const char *callee_name,
                                                const char *module_qn, const char **import_map_keys,
                                                const char **import_map_vals, int import_map_count,
                                                CBMLanguage lang) {
    /* A bare C call never names a member: a struct member is reached through
     * its object (cbm_call_onto_field_policy refuses every Field it lands on)
     * and C has no methods. So its Field and Method namesakes do not compete
     * for the name; a C member call and every other language keep them. */
    bool skip_members = lang == CBM_LANG_C && callee_name && !strchr(callee_name, '.') &&
                        !strstr(callee_name, "->");
    size_t scope_len = lang == CBM_LANG_PYTHON ? cbm_fqn_symbol_scope_len(module_qn)
                                               : (module_qn ? strlen(module_qn) : 0);
    return registry_resolve_scoped(r, callee_name, module_qn, import_map_keys, import_map_vals,
                                   import_map_count, scope_len, skip_members);
}

cbm_resolution_t cbm_registry_resolve_lineage(const cbm_registry_t *r, const char *callee_name,
                                              const char *module_qn, const char **import_map_keys,
                                              const char **import_map_vals, int import_map_count) {
    if (!r || !callee_name) {
        return empty_result();
    }
    /* Relation-PERMITTING variant, for SQL FROM/JOIN lineage usages only.
     * Deliberately uncached: the per-file cache is keyed by bare callee_name and
     * stores the relation-VETOED answer of the default variant, so sharing it
     * would poison one variant with the other's semantics. A SQL file holds few
     * distinct relation refs, so the chain walk stays cheap. */
    return registry_resolve_chain(r, callee_name, module_qn, import_map_keys, import_map_vals,
                                  import_map_count, module_qn ? strlen(module_qn) : 0, false);
}

/* ── Fuzzy Resolve ──────────────────────────────────────────────── */

/* Filter candidates by import reachability. Returns count of reachable. */
static int filter_import_reachable(const char **candidates, int count, const char **import_vals,
                                   int import_count, const char **out, int max_out) {
    int n = 0;
    for (int i = 0; i < count && n < max_out; i++) {
        if (is_import_reachable(candidates[i], import_vals, import_count)) {
            out[n++] = candidates[i];
        }
    }
    return n;
}

cbm_fuzzy_result_t cbm_registry_fuzzy_resolve(const cbm_registry_t *r, const char *callee_name,
                                              const char *module_qn, const char **import_map_keys,
                                              const char **import_map_vals, int import_map_count) {
    (void)import_map_keys;
    cbm_fuzzy_result_t no_match = {{0}, false};
    if (!r || !callee_name) {
        return no_match;
    }

    /* Extract simple name (last segment after dots) */
    const char *lookup = simple_name(callee_name);
    qn_array_t *arr = (qn_array_t *)cbm_ht_get(r->by_name, lookup);
    if (!arr || arr->count == 0) {
        return no_match;
    }
    if (arr->count > REG_MAX_CANDIDATES) {
        return no_match; /* unresolvably ambiguous — see REG_MAX_CANDIDATES */
    }

    bool have_imports = (import_map_vals && import_map_count > 0);

    /* Single candidate */
    if (arr->count == SKIP_ONE) {
        double conf = CONF_FUZZY_SINGLE;
        if (have_imports &&
            !is_import_reachable(arr->items[0], import_map_vals, import_map_count)) {
            conf *= DEFAULT_CONFIDENCE;
        }
        return (cbm_fuzzy_result_t){{arr->items[0], "fuzzy", conf, REG_RESOLVED}, true};
    }

    /* Multiple candidates: filter by import reachability */
    const char *filtered[CBM_SZ_256];
    int fcount = arr->count;
    const char **fptr = (const char **)arr->items;

    if (have_imports) {
        fcount = filter_import_reachable((const char **)arr->items, arr->count, import_map_vals,
                                         import_map_count, filtered, CBM_SZ_256);
        fptr = filtered;
    }

    if (fcount == 0) {
        /* No import-reachable — use originals with penalty */
        const char *best =
            best_by_import_distance(r, (const char **)arr->items, arr->count, module_qn);
        if (!best) {
            return no_match;
        }
        return (cbm_fuzzy_result_t){
            {best, "fuzzy",
             candidate_count_penalty(CONF_FUZZY_MULTI * REG_HALF_PENALTY, arr->count), arr->count},
            true};
    }
    if (fcount == SKIP_ONE) {
        return (cbm_fuzzy_result_t){
            {fptr[0], "fuzzy", candidate_count_penalty(CONF_FUZZY_SINGLE, arr->count), arr->count},
            true};
    }
    const char *best = best_by_import_distance(r, fptr, fcount, module_qn);
    if (!best) {
        return no_match;
    }
    return (cbm_fuzzy_result_t){
        {best, "fuzzy", candidate_count_penalty(CONF_FUZZY_MULTI, fcount), fcount}, true};
}

/* ── FindEndingWith ─────────────────────────────────────────────── */

struct few_ctx {
    const char *target; /* ".suffix" */
    size_t target_len;
    const char **results;
    int count;
    int cap;
};

static void few_scan(const char *key, void *value, void *ud) {
    (void)value;
    struct few_ctx *ctx = (few_ctx *)ud;
    size_t klen = strlen(key);
    if (klen >= ctx->target_len && strcmp(key + klen - ctx->target_len, ctx->target) == 0) {
        if (ctx->count >= ctx->cap) {
            ctx->cap = ctx->cap ? ctx->cap * PAIR_LEN : REG_INIT_CAP;
            ctx->results = (__typeof__(ctx->results))safe_realloc(ctx->results, (size_t)ctx->cap *
                                                                                    sizeof(char *));
        }
        ctx->results[ctx->count++] = key;
    }
}

int cbm_registry_find_ending_with(const cbm_registry_t *r, const char *suffix, const char ***out) {
    if (!r || !suffix || !out) {
        if (out) {
            *out = NULL;
        }
        return 0;
    }

    /* Build ".suffix" target */
    size_t slen = strlen(suffix);
    char *target = (char *)malloc(slen + REG_SUFFIX_ALLOC);
    target[0] = '.';
    memcpy(target + SKIP_ONE, suffix, slen + SKIP_ONE);

    struct few_ctx ctx = {target, slen + SKIP_ONE, NULL, 0, 0};
    cbm_ht_foreach(r->exact, few_scan, &ctx);

    free(target);
    *out = ctx.results;
    return ctx.count;
}

/* Public wrapper for is_import_reachable (testing). */
bool cbm_registry_is_import_reachable(const char *candidate_qn, const char **import_vals,
                                      int import_count) {
    return is_import_reachable(candidate_qn, import_vals, import_count);
}

/* #725: drop a suffix_match CALLS edge when the caller language and the target
 * file's language disagree.
 *
 * suffix_match is language-AGNOSTIC: two same-named symbols in different
 * languages collapse onto one winner chosen by import distance, so a Python
 * Store.commit() call attaches to a JS function named commit, and a Bash main
 * to a Python main. get_architecture then reports hotspots whose in-degree is
 * inherited from another language entirely.
 *
 * unique_name (candidates == 1) is a different strategy and is deliberately
 * left alone; so are same_module / import_map / lsp_*, which all have real
 * evidence behind them. JS/TS/TSX are ONE family, so a .ts helper calling a
 * .tsx function is not dropped. Pure; unit-tested in test_registry.cpp. */
static bool cbm_js_ts_family(CBMLanguage lang) {
    return lang == CBM_LANG_JAVASCRIPT || lang == CBM_LANG_TYPESCRIPT || lang == CBM_LANG_TSX;
}

static const char *cbm_reg_path_basename(const char *path) {
    if (!path || !path[0]) {
        return path;
    }
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
#endif
    return slash ? slash + 1 : path;
}

/* C and C++ are one family for cross-language reference checks: .h maps to
 * CBM_LANG_CPP in the extension table, so a .c file referencing a symbol
 * declared in its own header would otherwise read as a language boundary. */
static bool cbm_c_cpp_family(CBMLanguage lang) {
    return lang == CBM_LANG_C || lang == CBM_LANG_CPP;
}

/* True when a and b count as one language for the cross-language guards. The
 * JS/TS/TSX family is one language for both guards. C and C++ are one family
 * only when c_cpp_is_one_family is set (the reference guard): .h maps to
 * CBM_LANG_CPP, so a .c file referencing its own header would otherwise read as
 * a boundary. */
static bool same_language_family(CBMLanguage a, CBMLanguage b, bool c_cpp_is_one_family) {
    if (a == b) {
        return true;
    }
    if (cbm_js_ts_family(a) && cbm_js_ts_family(b)) {
        return true;
    }
    return c_cpp_is_one_family && cbm_c_cpp_family(a) && cbm_c_cpp_family(b);
}

/* Language of a target file, from its basename. cbm_language_for_filename
 * scans the special-filename and extension tables (hundreds of strcmp calls),
 * and the guards run once per resolved edge. Consecutive edges from one file
 * mostly land in the same few target files, so a caller-owned memo of the last
 * basename skips the table scan. The answer is identical: the lookup is a pure
 * function of the basename for the lifetime of one per-file loop. memo may be
 * NULL. */
static CBMLanguage target_file_language(const char *target_file_path, cbm_lang_memo_t *memo) {
    const char *base = cbm_reg_path_basename(target_file_path);
    if (!memo) {
        return cbm_language_for_filename(base);
    }
    if (memo->valid && strcmp(memo->base, base) == 0) {
        return memo->lang;
    }
    CBMLanguage lang = cbm_language_for_filename(base);
    size_t len = strlen(base);
    if (len < sizeof(memo->base)) {
        memcpy(memo->base, base, len + SKIP_ONE);
        memo->lang = lang;
        memo->valid = true;
    }
    return lang;
}

/* Build and configuration languages have no cross-language call semantics: a
 * Makefile's `$(eval ...)` or a CMake `function(...)` names nothing in a C
 * file, so a bare-name bind into another language is always a collision
 * (upstream's kernel probe: Makefile targets bound to `sk_psock.eval`). */
static bool build_config_language(CBMLanguage lang) {
    return lang == CBM_LANG_MAKEFILE || lang == CBM_LANG_CMAKE || lang == CBM_LANG_YAML ||
           lang == CBM_LANG_TOML || lang == CBM_LANG_JSON || lang == CBM_LANG_INI ||
           lang == CBM_LANG_DOCKERFILE;
}

bool cbm_suppress_cross_language_suffix_match(CBMLanguage caller_lang, const char *target_file_path,
                                              const char *strategy) {
    /* unique_name is the candidates==1 case and is not this guard — except for
     * a build or configuration caller, where even a unique match into another
     * language is a collision by construction. */
    if (!strategy) {
        return false;
    }
    bool config_caller = build_config_language(caller_lang);
    if (strcmp(strategy, "suffix_match") != 0 &&
        !(config_caller && strcmp(strategy, "unique_name") == 0)) {
        return false;
    }
    if (caller_lang == CBM_LANG_COUNT || !target_file_path || !target_file_path[0]) {
        return false;
    }
    CBMLanguage target_lang = target_file_language(target_file_path, NULL);
    return target_lang != CBM_LANG_COUNT &&
           !same_language_family(caller_lang, target_lang, /*c_cpp_is_one_family=*/false);
}

/* #1928: USAGE / WRITES / READS analog of the CALLS guard above. A variable or
 * field reference resolved by the short-name registry must not cross a
 * language boundary: unlike CALLS, a reference edge carries no import-closure
 * evidence at all -- a Go test's local `event` and an eBPF C probe's automatic
 * `event` share nothing but the spelling, so EVERY registry strategy is a
 * bare-name guess here and none is exempt. The JS/TS family keeps its
 * exemption, and C/C++ count as one family (.h maps to CBM_LANG_CPP). */
static bool cross_language_ref(CBMLanguage caller_lang, const char *target_file_path,
                               cbm_lang_memo_t *memo) {
    if (caller_lang == CBM_LANG_COUNT || !target_file_path || !target_file_path[0]) {
        return false;
    }
    CBMLanguage target_lang = target_file_language(target_file_path, memo);
    return target_lang != CBM_LANG_COUNT &&
           !same_language_family(caller_lang, target_lang, /*c_cpp_is_one_family=*/true);
}

bool cbm_suppress_cross_language_ref(CBMLanguage caller_lang, const char *target_file_path) {
    return cross_language_ref(caller_lang, target_file_path, NULL);
}

bool cbm_suppress_ref_edge(CBMLanguage lang, bool is_member_access, const cbm_gbuf_node_t *target,
                           cbm_lang_memo_t *memo) {
    return cross_language_ref(lang, target->file_path, memo) ||
           cbm_suppress_bare_field_ref(lang, is_member_access, target->label);
}

cbm_field_call_policy_t cbm_call_onto_field_policy(CBMLanguage caller_lang, const char *callee_text,
                                                   const char *target_label,
                                                   const char *target_file_path) {
    /* The short-name registry holds Field nodes, so a call spelled like some
     * struct member can resolve onto that Field. Three shapes are decided here:
     *   - a caller in another language: nothing but the spelling connects a
     *     Bash `command` or a Python `sorted()` to a C struct member (the
     *     #1928 reference rule, with its C/C++ family exemption);
     *   - a bare C call (`socket(...)`, a function-pointer parameter
     *     `want(x)`): a bare C name is never a struct member;
     *   - a C or C++ member call (`cb.close(ctx)`, `text.size()`): the type of
     *     the object decides which struct's member is called, never the
     *     member name alone. The C LSP publishes owners in both modes.
     * A bare C++ call keeps its resolution (a method body reaches its own
     * members without a selector), and so does every other language. */
    if (!target_label || strcmp(target_label, "Field") != 0) {
        return CBM_FIELD_CALL_KEEP;
    }
    if (cbm_suppress_cross_language_ref(caller_lang, target_file_path)) {
        return CBM_FIELD_CALL_DROP;
    }
    if (caller_lang != CBM_LANG_C && caller_lang != CBM_LANG_CPP) {
        return CBM_FIELD_CALL_KEEP;
    }
    if (callee_text && (strchr(callee_text, '.') || strstr(callee_text, "->"))) {
        return CBM_FIELD_CALL_BY_OWNER;
    }
    return caller_lang == CBM_LANG_C ? CBM_FIELD_CALL_DROP : CBM_FIELD_CALL_KEEP;
}

bool cbm_c_member_binds_by_owner(bool is_c, bool is_member_access) {
    /* The member half of `a.b` / `a->b` is not a name in any C scope: only the
     * type of `a` gives `b` a meaning. Handing the bare `b` to the short-name
     * registry bound whichever Field (or function, or variable) was called
     * `b`. Such a reference binds through the C LSP's field-owner rows; an
     * object the LSP could not type binds only a member name that is unique
     * in the project (cbm_registry_unique_field_qn), never one of several.
     *
     * C-gated like the Go rule above: a C++ or Objective-C method body
     * reaches its own members without a selector, and their member accesses
     * keep the existing resolution until their LSPs publish owners too. */
    return is_c && is_member_access;
}

const char *cbm_registry_unique_field_qn(const cbm_registry_t *r, const char *member_name) {
    /* A member name that exactly one Field in the whole project carries names
     * that Field without any choice being made. This is what an untyped C
     * member access may still bind: the defect in bare-name member resolution
     * was the arbitrary pick among SEVERAL same-named members. Only Fields are
     * candidates -- a function or variable of the same name can never be what
     * `a.b` means -- and two of them bind nothing. */
    const char **qns = NULL;
    int count = 0;
    if (!r || !member_name || cbm_registry_find_by_name(r, member_name, &qns, &count) != 0 ||
        !qns) {
        return NULL;
    }
    const char *only = NULL;
    for (int i = 0; i < count; i++) {
        const char *label = qns[i] ? cbm_registry_label_of(r, qns[i]) : NULL;
        if (!label || strcmp(label, "Field") != 0) {
            continue;
        }
        if (only && strcmp(only, qns[i]) != 0) {
            return NULL;
        }
        only = qns[i];
    }
    return only;
}

bool cbm_registry_field_name_ambiguous(const cbm_registry_t *r, const char *member_name) {
    const char **qns = NULL;
    int count = 0;
    if (!r || !member_name || cbm_registry_find_by_name(r, member_name, &qns, &count) != 0 ||
        !qns) {
        return false;
    }
    const char *first = NULL;
    for (int i = 0; i < count; i++) {
        const char *label = qns[i] ? cbm_registry_label_of(r, qns[i]) : NULL;
        if (!label || strcmp(label, "Field") != 0) {
            continue;
        }
        if (first && strcmp(first, qns[i]) != 0) {
            return true;
        }
        first = qns[i];
    }
    return false;
}

bool cbm_suppress_call_target(CBMLanguage lang, const cbm_gbuf_node_t *target,
                              const char *strategy) {
    return cbm_suppress_cross_language_suffix_match(lang, target->file_path, strategy) ||
           cbm_go_suppress_textual_field_call(lang == CBM_LANG_GO, target->label, strategy);
}
