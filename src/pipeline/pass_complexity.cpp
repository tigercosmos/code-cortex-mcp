/*
 * pass_complexity.c — Interprocedural complexity propagation (Tier B).
 *
 * Tier A (in the extraction walk) stamps each Function/Method node with local
 * structural metrics: complexity (cyclomatic), cognitive, loop_count, loop_depth.
 * This pass propagates loop_depth along CALLS edges to estimate a worst-case
 * *transitive* nested-loop degree: a function with a depth-1 loop that calls an
 * O(n) helper is effectively O(n^2). The estimate assumes calls may occur inside
 * loops (an upper bound) — it is a queryable bottleneck *candidate* signal, not a
 * proof (true big-O is undecidable; cf. SPEED / Loopus). Cycles in the call graph
 * are broken and flagged via a `recursive` property.
 *
 * Writes two extra node properties: transitive_loop_depth, recursive.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/hash_table.h"
#include "cbm.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

enum { CBM_TLD_MAX_DEPTH = 256 }; /* recursion-depth cap (cycle/stack guard) */

/* Int → string for structured logging (thread-safe ring buffer). */
static const char *itoa_cx(int val) {
    enum { RING = 2, MASK = 1 };
    static CBM_TLS char bufs[RING][CBM_SZ_32];
    static CBM_TLS int idx = 0;
    int i = idx;
    idx = (idx + 1) & MASK;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", val);
    return bufs[i];
}

/* Parse an integer "key":N from a flat JSON object. Returns def if absent. */
static int json_get_int(const char *json, const char *key, int dflt) {
    if (!json) {
        return dflt;
    }
    char pat[CBM_SZ_64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return dflt;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return (int)strtol(p, NULL, CBM_DECIMAL_BASE);
}

/* Parse a boolean "key":true/false from a flat JSON object. */
static bool json_get_bool(const char *json, const char *key) {
    if (!json) {
        return false;
    }
    char pat[CBM_SZ_64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return *p == 't';
}

/* Append transitive_loop_depth + recursive to a node's properties JSON object. */
static void append_complexity_props(cbm_gbuf_node_t *node, int tld, bool recursive) {
    const char *old = node->properties_json ? node->properties_json : "{}";
    size_t olen = strlen(old);
    if (olen < 2 || old[olen - 1] != '}') {
        return; /* not a JSON object — leave untouched */
    }
    bool empty = (olen == 2); /* "{}" */
    char *neu = (char *)malloc(olen + CBM_SZ_64);
    if (!neu) {
        return;
    }
    memcpy(neu, old, olen - 1); /* copy without trailing '}' */
    int w =
        snprintf(neu + (olen - 1), CBM_SZ_64, "%s\"transitive_loop_depth\":%d,\"recursive\":%s}",
                 empty ? "" : ",", tld, recursive ? "true" : "false");
    if (w < 0) {
        free(neu);
        return;
    }
    free(node->properties_json);
    node->properties_json = neu;
}

/* Content-only node order: qualified_name, then file path and start line.
 * Never the temp id, and never the graph buffer's label-list order: extract
 * workers draw ids from one shared counter, so both follow worker scheduling
 * and differ between a sequential and a parallel index (and run to run). The
 * memoised DFS truncates a branch at the first back edge, so which member of a
 * call cycle it ENTERS first decides the transitive_loop_depth every other
 * member reads; both the seed order and the callee order must therefore be a
 * function of the inputs alone. A dangling target (no node for the id) has no
 * edges of its own, so where it sorts cannot move a result; it keys as empty
 * strings. */
enum { TLD_CMP_LESS = -1, TLD_CMP_GREATER = 1 };

static const char *str_or_empty(const char *s) {
    return s ? s : "";
}

static int cmp_node_canonical(const cbm_gbuf_node_t *a, const cbm_gbuf_node_t *b) {
    int r = strcmp(str_or_empty(a ? a->qualified_name : NULL),
                   str_or_empty(b ? b->qualified_name : NULL));
    if (r != 0) {
        return r;
    }
    r = strcmp(str_or_empty(a ? a->file_path : NULL), str_or_empty(b ? b->file_path : NULL));
    if (r != 0) {
        return r;
    }
    int la = a ? a->start_line : 0;
    int lb = b ? b->start_line : 0;
    if (la != lb) {
        return la < lb ? TLD_CMP_LESS : TLD_CMP_GREATER;
    }
    return 0;
}

static int cmp_seed_canonical(const void *pa, const void *pb) {
    return cmp_node_canonical(*(const cbm_gbuf_node_t *const *)pa,
                              *(const cbm_gbuf_node_t *const *)pb);
}

typedef struct {
    const cbm_gbuf_node_t *node; /* NULL for a dangling target id */
    int64_t id;
} tld_callee_t;

static int cmp_callee_canonical(const void *pa, const void *pb) {
    return cmp_node_canonical(((const tld_callee_t *)pa)->node, ((const tld_callee_t *)pb)->node);
}

/* Traversal state. `callees` is one bump stack shared by every DFS frame: a
 * frame takes its out-degree worth of slots, sorts them, recurses, then
 * releases them. Nodes on the recursion path are distinct (state 1 blocks
 * re-entry), so the live slots never exceed the CALLS edge count the stack is
 * sized for. `path` is the DFS path stack used to attribute a back edge to
 * every member of the cycle it closes. */
typedef struct {
    const cbm_gbuf_t *gb;
    int64_t maxid;
    int *loop_depth;
    int *tld;
    char *state;
    bool *recursive;
    int64_t *path;
    tld_callee_t *callees;
    int callee_top;
    int callee_cap;
} tld_ctx_t;

/* Memoized DFS: tld(id) = loop_depth(id) + max over CALLS-callees of tld(callee).
 * state: 0=unvisited, 1=in-progress (back-edge → cycle), 2=done. */
static int tld_dfs(tld_ctx_t *cx, int64_t id, int depth) {
    if (id < 1 || id > cx->maxid) {
        return 0;
    }
    if (cx->state[id] == 2) {
        return cx->tld[id];
    }
    if (cx->state[id] == 1) {
        /* Back edge: id is an ancestor on the current DFS path, so every node
         * from the current caller back up to id forms one call cycle (mutual
         * recursion A→B→A). Mark the whole cycle, not just id — otherwise the
         * caller is memoized done with recursive=false. */
        for (int k = depth - 1; k >= 0; k--) {
            cx->recursive[cx->path[k]] = true;
            if (cx->path[k] == id) {
                break;
            }
        }
        cx->recursive[id] = true; /* back edge → call-graph cycle */
        return 0;
    }
    if (depth > CBM_TLD_MAX_DEPTH) {
        return cx->loop_depth[id];
    }
    const cbm_gbuf_edge_t **edges = NULL;
    int ne = 0;
    cbm_gbuf_find_edges_by_source_type(cx->gb, id, "CALLS", &edges, &ne);
    if (ne > cx->callee_cap - cx->callee_top) {
        return cx->loop_depth[id]; /* unreachable by construction; same as the depth cap */
    }
    cx->state[id] = 1;
    cx->path[depth] = id; /* push onto the DFS path stack for cycle attribution */
    tld_callee_t *callees = cx->callees + cx->callee_top;
    int nc = 0;
    for (int i = 0; i < ne; i++) {
        int64_t c = edges[i]->target_id;
        if (c == id) {
            cx->recursive[id] = true; /* direct self-recursion */
            continue;
        }
        callees[nc].id = c;
        callees[nc].node = cbm_gbuf_find_by_id(cx->gb, c);
        nc++;
    }
    cx->callee_top += nc;
    qsort(callees, (size_t)nc, sizeof(*callees), cmp_callee_canonical);
    int best = 0;
    for (int i = 0; i < nc; i++) {
        int ct = tld_dfs(cx, callees[i].id, depth + 1);
        if (ct > best) {
            best = ct;
        }
    }
    cx->callee_top -= nc;
    cx->tld[id] = cx->loop_depth[id] + best;
    cx->state[id] = 2;
    return cx->tld[id];
}

/* Seed each Function/Method node's loop_depth and self_recursive flag, and
 * collect the node as a traversal seed (also the write-back target). The
 * self_recursive seed (set at extraction) feeds the final recursive flag;
 * tld_dfs additionally ORs in mutual recursion discovered as a call-graph
 * cycle. */
static void seed_loop_depths(tld_ctx_t *cx, const char *label, cbm_gbuf_node_t **seeds,
                             int *seed_count, int seed_cap) {
    const cbm_gbuf_node_t **nodes = NULL;
    int count = 0;
    if (cbm_gbuf_find_by_label(cx->gb, label, &nodes, &count) != 0) {
        return;
    }
    for (int i = 0; i < count && *seed_count < seed_cap; i++) {
        const cbm_gbuf_node_t *n = nodes[i];
        if (n->id >= 1 && n->id <= cx->maxid) {
            cx->loop_depth[n->id] = json_get_int(n->properties_json, "loop_depth", 0);
            cx->recursive[n->id] = json_get_bool(n->properties_json, "self_recursive");
            seeds[(*seed_count)++] = (cbm_gbuf_node_t *)n;
        }
    }
}

static int calls_edge_count(const cbm_gbuf_t *gb) {
    const cbm_gbuf_edge_t **edges = NULL;
    int count = 0;
    if (cbm_gbuf_find_edges_by_type(gb, "CALLS", &edges, &count) != 0) {
        return 0;
    }
    return count;
}

void cbm_pipeline_pass_complexity(cbm_pipeline_ctx_t *ctx) {
    cbm_gbuf_t *gb = ctx->gbuf;
    /* Node and edge IDs are drawn from one shared counter, so node IDs are NOT
     * contiguous 1..node_count — they interleave with edge IDs. Size the lookup
     * arrays by the id ceiling (next_id) so every node id is addressable. */
    int64_t maxid = cbm_gbuf_next_id(gb) - 1;
    if (maxid < 1) {
        return;
    }
    size_t sz = (size_t)maxid + 1;
    tld_ctx_t cx = {};
    cx.gb = gb;
    cx.maxid = maxid;
    cx.callee_cap = calls_edge_count(gb);
    cx.loop_depth = (int *)calloc(sz, sizeof(int));
    cx.tld = (int *)calloc(sz, sizeof(int));
    cx.state = (char *)calloc(sz, sizeof(char));
    cx.recursive = (bool *)calloc(sz, sizeof(bool));
    cx.path = (int64_t *)calloc(CBM_TLD_MAX_DEPTH + 1, sizeof(int64_t));
    cx.callees = (tld_callee_t *)calloc((size_t)cx.callee_cap + 1, sizeof(tld_callee_t));
    /* Seeds are bounded by the node-id ceiling; `written` guards a node listed
     * under both labels from being written back twice. */
    cbm_gbuf_node_t **seeds = (cbm_gbuf_node_t **)calloc(sz, sizeof(cbm_gbuf_node_t *));
    char *written = (char *)calloc(sz, sizeof(char));
    if (!cx.loop_depth || !cx.tld || !cx.state || !cx.recursive || !cx.path || !cx.callees ||
        !seeds || !written) {
        free(cx.loop_depth);
        free(cx.tld);
        free(cx.state);
        free(cx.recursive);
        free(cx.path);
        free(cx.callees);
        free(seeds);
        free(written);
        return;
    }

    int seed_count = 0;
    seed_loop_depths(&cx, "Function", seeds, &seed_count, (int)sz);
    seed_loop_depths(&cx, "Method", seeds, &seed_count, (int)sz);
    qsort(seeds, (size_t)seed_count, sizeof(*seeds), cmp_seed_canonical);

    int updated = 0;
    for (int i = 0; i < seed_count; i++) {
        cbm_gbuf_node_t *n = seeds[i];
        if (written[n->id]) {
            continue; /* already written (same node listed twice) */
        }
        if (cx.state[n->id] != 2) {
            tld_dfs(&cx, n->id, 0);
        }
        append_complexity_props(n, cx.tld[n->id], cx.recursive[n->id]);
        written[n->id] = 1;
        updated++;
    }

    cbm_log_info("pass.complexity", "functions", itoa_cx(updated));

    free(cx.loop_depth);
    free(cx.tld);
    free(cx.state);
    free(cx.recursive);
    free(cx.path);
    free(cx.callees);
    free(seeds);
    free(written);
}

/* ══════════════════════════════════════════════════════════════════════
 * Importance — index-time per-symbol score (weighted degree).
 *
 * Computes an importance score for every Function/Method/Class node and stores
 * it as a numeric "importance" key inside the node's EXISTING properties_json —
 * no new column, no schema change, no CBM_INDEX_FORMAT_VERSION bump.
 *
 *   importance = sqrt(num_refs) * priv * generic * distinct * test_penalty
 *
 *     num_refs      incoming CALLS + USAGE edges. 0 -> sqrt(0) = 0.
 *     priv     0.1  name is private (leading underscore)
 *     generic  0.1  the name is defined in >= 5 DISTINCT files
 *     distinct 10   name is snake_case or camelCase AND len >= 8
 *     test     0.1  target of an incoming TESTS edge, or lives in a test file
 *                   per cbm_is_test_path() (the classifier pass_tests uses)
 *
 * Cost: the generic multiplier needs |{files a name is defined in}|. Computed
 * per NODE by walking the whole same-name group and comparing paths pairwise,
 * that is O(k^3) per group of size k (upstream measured 85 s -> 481 s on a
 * 666 MB Java corpus). Here the count is memoized once per DISTINCT name and
 * paths are deduplicated through a hash set, so the total is linear.
 *
 * Ordering is load-bearing: see cbm_pipeline_pass_importance in
 * pipeline_internal.h. (Ported from DeusData/codebase-memory-mcp@e5f3aab0.)
 * ══════════════════════════════════════════════════════════════════════ */

static std::atomic<uint64_t> g_importance_name_visits{0};

uint64_t cbm_pipeline_importance_name_visits(void) {
    return g_importance_name_visits.load(std::memory_order_relaxed);
}

enum {
    CBM_IMPORTANCE_GENERIC_MIN_FILES = 5, /* name defined in >= N files -> generic */
    CBM_IMPORTANCE_DISTINCT_MIN_LEN = 8,  /* distinctive-identifier length floor */
    CBM_IMPORTANCE_NAME_MEMO_CAP = 4096,  /* initial buckets: one entry per distinct name */
    CBM_IMPORTANCE_PATH_SET_CAP = 256,
};
static const double CBM_IMPORTANCE_PRIV_MUL = 0.1;
static const double CBM_IMPORTANCE_GENERIC_MUL = 0.1;
static const double CBM_IMPORTANCE_DISTINCT_MUL = 10.0;
static const double CBM_IMPORTANCE_TEST_MUL = 0.1;

static const char *const CBM_IMPORTANCE_LABELS[] = {"Function", "Method", "Class"};

static bool imp_name_is_private(const char *name) {
    return name != nullptr && name[0] == '_';
}

/* snake_case is an embedded '_'; camelCase is a lower->upper hump. A plain
 * long lowercase word is NOT distinctive. */
static bool imp_name_is_distinctive(const char *name) {
    if (!name) {
        return false;
    }
    size_t len = strlen(name);
    if (len < (size_t)CBM_IMPORTANCE_DISTINCT_MIN_LEN) {
        return false;
    }
    if (strchr(name, '_') != nullptr) {
        return true;
    }
    for (size_t i = 1; i < len; i++) {
        if (islower((unsigned char)name[i - 1]) && isupper((unsigned char)name[i])) {
            return true;
        }
    }
    return false;
}

/* Pointer just past the JSON value starting at p, or NULL if unterminated. */
static const char *imp_value_end(const char *p) {
    if (*p == '"') {
        for (p++; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                continue;
            }
            if (*p == '"') {
                return p + 1;
            }
        }
        return nullptr;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        bool in_str = false;
        for (; *p; p++) {
            if (in_str) {
                if (*p == '\\' && p[1]) {
                    p++;
                } else if (*p == '"') {
                    in_str = false;
                }
            } else if (*p == '"') {
                in_str = true;
            } else if (*p == '{' || *p == '[') {
                depth++;
            } else if ((*p == '}' || *p == ']') && --depth == 0) {
                return p + 1;
            }
        }
        return nullptr;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') {
        p++;
    }
    return p;
}

/* `"key"` genuinely in KEY position — preceded by '{' or ',' and followed by
 * ':' (modulo whitespace). A bare strstr would also match text inside a string
 * VALUE. Returns the opening quote, or NULL. */
static const char *imp_find_key(const char *json, const char *key) {
    char pat[CBM_SZ_64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pat)) {
        return nullptr;
    }
    for (const char *p = json; (p = strstr(p, pat)) != nullptr; p += n) {
        const char *before = p;
        while (before > json && isspace((unsigned char)before[-1])) {
            before--;
        }
        char prev = (before > json) ? before[-1] : '\0';
        const char *after = p + n;
        while (isspace((unsigned char)*after)) {
            after++;
        }
        if ((prev == '{' || prev == ',') && *after == ':') {
            return p;
        }
    }
    return nullptr;
}

/* IDEMPOTENT BY CONTRACT: the incremental path rehydrates nodes that already
 * carry "importance"; a pure append would write {"importance":1,...,
 * "importance":2}. Overwrite in place; append only when absent. A blob that is
 * not a JSON object — or a non-finite score, which "%f" renders as the invalid
 * JSON token `nan`/`inf` — leaves the node untouched: properties feed generated
 * columns, and one malformed blob gets the whole database quarantined. */
void cbm_pipeline_importance_append_prop(cbm_gbuf_node_t *node, double score) {
    if (!node || !std::isfinite(score)) {
        return;
    }
    const char *old = node->properties_json ? node->properties_json : "{}";
    size_t olen = strlen(old);
    if (olen < 2 || old[0] != '{' || old[olen - 1] != '}') {
        return;
    }
    char val[CBM_SZ_32];
    int vn = snprintf(val, sizeof(val), "%.6f", score);
    if (vn < 0 || (size_t)vn >= sizeof(val)) {
        return;
    }

    const char *k = imp_find_key(old, "importance");
    if (k) {
        const char *v = strchr(k, ':');
        if (!v) {
            return;
        }
        v++;
        while (isspace((unsigned char)*v)) {
            v++;
        }
        const char *vend = imp_value_end(v);
        if (!vend) {
            return;
        }
        size_t head = (size_t)(v - old);
        size_t tail = strlen(vend);
        char *neu = (char *)malloc(head + (size_t)vn + tail + 1);
        if (!neu) {
            return;
        }
        memcpy(neu, old, head);
        memcpy(neu + head, val, (size_t)vn);
        memcpy(neu + head + (size_t)vn, vend, tail + 1);
        free(node->properties_json);
        node->properties_json = neu;
        return;
    }

    /* An object holding only whitespace ("{ }") takes no leading comma. */
    bool empty = true;
    for (size_t i = 1; i + 1 < olen; i++) {
        if (!isspace((unsigned char)old[i])) {
            empty = false;
            break;
        }
    }
    char frag[CBM_SZ_64];
    int fn = snprintf(frag, sizeof(frag), "%s\"importance\":%s}", empty ? "" : ",", val);
    if (fn < 0 || (size_t)fn >= sizeof(frag)) {
        return;
    }
    size_t keep = empty ? 1 : olen - 1; /* drop the trailing '}' (and any inner blanks) */
    char *neu = (char *)malloc(keep + (size_t)fn + 1);
    if (!neu) {
        return;
    }
    memcpy(neu, old, keep);
    memcpy(neu + keep, frag, (size_t)fn + 1);
    free(node->properties_json);
    node->properties_json = neu;
}

typedef struct {
    CBMHashTable *counts; /* name -> (void *)(intptr_t)(distinct_files + 1) */
    CBMHashTable *paths;  /* scratch path set, cleared between names */
} imp_name_index_t;

/* Distinct files a name is DEFINED in — once per distinct name, then a hash
 * hit. Keys are borrowed from gbuf nodes, which outlive the pass. */
static int imp_distinct_file_count(const cbm_gbuf_t *gb, const char *name, imp_name_index_t *ix) {
    if (!name || !*name || !ix->counts) {
        return 0;
    }
    void *cached = cbm_ht_get(ix->counts, name);
    if (cached) {
        return (int)((intptr_t)cached - 1);
    }
    const cbm_gbuf_node_t **nodes = nullptr;
    int count = 0;
    int distinct = 0;
    if (cbm_gbuf_find_by_name(gb, name, &nodes, &count) == 0 && count > 0) {
        g_importance_name_visits.fetch_add((uint64_t)count, std::memory_order_relaxed);
        if (ix->paths) {
            cbm_ht_clear(ix->paths);
            for (int i = 0; i < count; i++) {
                const char *fp = nodes[i]->file_path;
                if (!fp || cbm_ht_has(ix->paths, fp)) {
                    continue;
                }
                cbm_ht_set(ix->paths, fp, (void *)(intptr_t)1);
                distinct++;
            }
        }
        /* No scratch set (allocation failure): degrade to "never generic"
         * rather than a pairwise scan — a silent quadratic is worse than a
         * missing 0.1 multiplier. */
    }
    cbm_ht_set(ix->counts, name, (void *)(intptr_t)(distinct + 1));
    return distinct;
}

static int imp_incoming_edge_count(const cbm_gbuf_t *gb, int64_t id, const char *type) {
    const cbm_gbuf_edge_t **edges = nullptr;
    int ne = 0;
    if (cbm_gbuf_find_edges_by_target_type(gb, id, type, &edges, &ne) != 0) {
        return 0;
    }
    return ne;
}

void cbm_pipeline_pass_importance(cbm_pipeline_ctx_t *ctx) {
    if (!ctx || !ctx->gbuf) {
        return;
    }
    cbm_gbuf_t *gb = ctx->gbuf;
    /* Modest reservations, not sized from node_count: the memo holds one entry
     * per DISTINCT name, and sizing for every node would cost hundreds of MB of
     * transient peak on a multi-million-node graph. */
    imp_name_index_t ix = {cbm_ht_create(CBM_IMPORTANCE_NAME_MEMO_CAP),
                           cbm_ht_create(CBM_IMPORTANCE_PATH_SET_CAP)};
    if (!ix.counts) {
        cbm_ht_free(ix.paths);
        cbm_log_error("pass.importance", "msg", "name_index_alloc_failed");
        return;
    }

    int updated = 0;
    for (const char *label : CBM_IMPORTANCE_LABELS) {
        const cbm_gbuf_node_t **nodes = nullptr;
        int count = 0;
        if (cbm_gbuf_find_by_label(gb, label, &nodes, &count) != 0) {
            continue;
        }
        for (int i = 0; i < count; i++) {
            auto *n = (cbm_gbuf_node_t *)nodes[i];
            int num_refs = imp_incoming_edge_count(gb, n->id, "CALLS") +
                           imp_incoming_edge_count(gb, n->id, "USAGE");
            double score = std::sqrt((double)num_refs);
            if (imp_name_is_private(n->name)) {
                score *= CBM_IMPORTANCE_PRIV_MUL;
            }
            if (imp_distinct_file_count(gb, n->name, &ix) >= CBM_IMPORTANCE_GENERIC_MIN_FILES) {
                score *= CBM_IMPORTANCE_GENERIC_MUL;
            }
            if (imp_name_is_distinctive(n->name)) {
                score *= CBM_IMPORTANCE_DISTINCT_MUL;
            }
            if (cbm_is_test_path(n->file_path) || imp_incoming_edge_count(gb, n->id, "TESTS") > 0) {
                score *= CBM_IMPORTANCE_TEST_MUL;
            }
            cbm_pipeline_importance_append_prop(n, score);
            updated++;
        }
    }

    cbm_ht_free(ix.paths);
    cbm_ht_free(ix.counts);
    cbm_log_info("pass.importance", "symbols", itoa_cx(updated));
}
