/*
 * pass_lsp_cross.c — Cross-file LSP type-aware call resolution pass.
 *
 * See pass_lsp_cross.h for the high-level contract. This file is the
 * pipeline glue that converts the existing per-file extraction state
 * (CBMDefinition / CBMImport / IMPORTS-edge gbuf state) into the input
 * shape each language LSP's cbm_run_X_lsp_cross expects, then merges
 * the resulting CBMResolvedCall entries back into per-file results.
 *
 * The pass is a no-op for any file whose CBMFileResult is missing or
 * whose language has no cross-file LSP entry registered (e.g. Rust /
 * Java today). Per-LSP emit functions dedup against entries already in
 * resolved_calls, so this pass is also idempotent — safe to invoke
 * multiple times if the pipeline gains a re-run path later.
 */
#include "pipeline/pass_lsp_cross.h"
#include "pipeline/pipeline_internal.h"
#include "pipeline/lsp_resolve.h"
#include "lsp/go_lsp.h"
#include "lsp/c_lsp.h"
#include "lsp/py_lsp.h"
#include "lsp/ts_lsp.h"
#include "lsp/php_lsp.h"
#include "lsp/java_lsp.h"
#include "lsp/kotlin_lsp.h"
#include "lsp/rust_lsp.h"
#include "lsp/rust_cargo.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/constants.h"
#include "foundation/hash_table.h"
#include "foundation/log.h"
#include "foundation/compat_fs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* ── Constants ─────────────────────────────────────────────────── */

enum {
    PXC_MAX_FILE_BYTES_FACTOR = 100, /* same cap pass_calls.c uses for source size */
    PXC_ITOA_BUF = 16,
};

/* Format an int into a thread-local rotating buffer for log key=value emission.
 * Mirrors the itoa_log helper in pass_calls.c — kept local so passes don't
 * grow a foundation-wide formatting API just for log output. */
static const char *itoa_buf(int val) {
    static thread_local char bufs[PXC_ITOA_BUF][PXC_ITOA_BUF];
    static thread_local int slot = 0;
    char *out = bufs[slot];
    slot = (slot + 1) & (PXC_ITOA_BUF - 1);
    snprintf(out, PXC_ITOA_BUF, "%d", val);
    return out;
}

/* ── Local helpers ─────────────────────────────────────────────── */

/* True for languages whose module QN is derived from the CONTAINING DIRECTORY
 * (Java package, Go package) rather than the filename stem. MUST match the
 * extraction-side cbm_lang_module_is_dir() in internal/cbm/helpers.c so the
 * cross-file LSP caller_qn agrees with the def-node QN (the lsp_resolve join
 * keys on exact equality). */
static bool pxc_module_is_dir(CBMLanguage lang) {
    return lang == CBM_LANG_JAVA || lang == CBM_LANG_GO;
}

/* Slurp a file into a malloc'd, NUL-terminated buffer. Mirrors the
 * read_file helper in pass_calls.c / pass_parallel.c (kept local so the
 * pipeline doesn't grow a public read-file API just for this pass). */
static char *pxc_read_file(const char *path, int *out_len) {
    FILE *f = cbm_fopen(path, "rb");
    if (!f)
        return NULL;
    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (long)PXC_MAX_FILE_BYTES_FACTOR * (long)CBM_SZ_1K * (long)CBM_SZ_1K) {
        (void)fclose(f);
        return NULL;
    }
    /* +pad: tree-sitter lexer lookahead reads past EOF; keep it in-bounds */
    enum { CBM_TS_LOOKAHEAD_PAD = 16 };
    char *buf = (char *)malloc((size_t)size + CBM_TS_LOOKAHEAD_PAD);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    (void)fclose(f);
    if (nread > (size_t)size)
        nread = (size_t)size;
    memset(buf + nread, 0, CBM_TS_LOOKAHEAD_PAD);
    *out_len = (int)nread;
    return buf;
}

/* Map a CBMDefinition.label to a CBMLSPDef.label. Per-language LSP registrars
 * only care about type-like containers (Class/Struct/Interface/Trait/Enum/Type)
 * plus Protocol/Function/Method — variables, modules, decorators, etc. are
 * skipped. Struct passes through so Rust/Go struct type-registration via the
 * cross-file LSP path is not dropped. */
static const char *pxc_map_label(const char *label) {
    if (!label)
        return NULL;
    if (cbm_label_is_type_like(label) || strcmp(label, "Protocol") == 0 ||
        strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0) {
        return label;
    }
    return NULL;
}

/* Build the embedded_types "|"-separated string from base_classes[].
 * Returns NULL when there are no bases. Allocated in the supplied arena. */
static const char *pxc_join_pipe(CBMArena *arena, const char *const *items) {
    if (!items || !items[0])
        return NULL;
    int count = 0;
    size_t total = 0;
    for (int i = 0; items[i]; i++) {
        count++;
        total += strlen(items[i]);
    }
    if (count == 0)
        return NULL;
    /* count - 1 separators + NUL. */
    size_t bufsz = total + (size_t)(count - 1) + 1;
    char *buf = (char *)cbm_arena_alloc(arena, bufsz);
    if (!buf)
        return NULL;
    char *p = buf;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(items[i]);
        memcpy(p, items[i], n);
        p += n;
        if (i + 1 < count)
            *p++ = '|';
    }
    *p = '\0';
    return buf;
}

static bool pxc_is_jvm_lang(CBMLanguage lang);

static const char *pxc_last_component(const char *qn) {
    if (!qn) {
        return NULL;
    }
    const char *dot = strrchr(qn, '.');
    return dot ? dot + 1 : qn;
}

static const char *pxc_jvm_type_qn(CBMArena *arena, const char *namespace_name,
                                   const char *type_qn_or_name) {
    if (!arena || !namespace_name || !namespace_name[0] || !type_qn_or_name) {
        return type_qn_or_name;
    }
    const char *short_name = pxc_last_component(type_qn_or_name);
    if (!short_name || !short_name[0]) {
        return type_qn_or_name;
    }
    return cbm_arena_sprintf(arena, "%s.%s", namespace_name, short_name);
}

static const char *pxc_jvm_def_qn(CBMArena *arena, const CBMDefinition *src,
                                  const char *namespace_name, const char *label) {
    if (!arena || !src || !namespace_name || !namespace_name[0]) {
        return src ? src->qualified_name : NULL;
    }
    if (strcmp(label, "Method") == 0 || strcmp(label, "Function") == 0 ||
        strcmp(label, "Constructor") == 0) {
        if (src->parent_class && src->parent_class[0]) {
            return cbm_arena_sprintf(arena, "%s.%s.%s", namespace_name,
                                     pxc_last_component(src->parent_class), src->name);
        }
        return cbm_arena_sprintf(arena, "%s.%s", namespace_name, src->name);
    }
    return cbm_arena_sprintf(arena, "%s.%s", namespace_name, src->name);
}

static const char *pxc_infer_jvm_namespace(CBMArena *arena, const char *rel_path,
                                           CBMLanguage lang) {
    if (!arena || !rel_path || !pxc_is_jvm_lang(lang)) {
        return NULL;
    }
    const char *root = NULL;
    const char *lang_root = lang == CBM_LANG_KOTLIN ? "kotlin/" : "java/";
    if (strncmp(rel_path, "src/main/", 9) == 0 &&
        strncmp(rel_path + 9, lang_root, strlen(lang_root)) == 0) {
        root = rel_path + 9 + strlen(lang_root);
    } else if (strncmp(rel_path, "src/test/", 9) == 0 &&
               strncmp(rel_path + 9, lang_root, strlen(lang_root)) == 0) {
        root = rel_path + 9 + strlen(lang_root);
    } else {
        const char *needle = lang == CBM_LANG_KOTLIN ? "/kotlin/" : "/java/";
        root = strstr(rel_path, needle);
        if (root) {
            root += strlen(needle);
        } else if (strncmp(rel_path, "src/", 4) == 0) {
            root = rel_path + 4;
        } else {
            root = strstr(rel_path, "/src/");
            if (root) {
                root += strlen("/src/");
            }
        }
    }
    if (!root || !root[0]) {
        return NULL;
    }
    if (strncmp(root, "main/", 5) == 0 || strncmp(root, "test/", 5) == 0) {
        root += 5;
    }
    if (strncmp(root, "java/", 5) == 0) {
        root += 5;
    } else if (strncmp(root, "kotlin/", 7) == 0) {
        root += 7;
    }
    const char *slash = strrchr(root, '/');
    if (!slash || slash <= root) {
        return NULL;
    }
    size_t len = (size_t)(slash - root);
    char *ns = (char *)cbm_arena_alloc(arena, len + 1);
    if (!ns) {
        return NULL;
    }
    memcpy(ns, root, len);
    ns[len] = '\0';
    for (size_t i = 0; i < len; i++) {
        if (ns[i] == '/') {
            ns[i] = '.';
        }
    }
    return ns;
}

/* Convert one CBMDefinition into a CBMLSPDef. Returns 0 on success, -1
 * to skip (unsupported label or missing required field). dst gets borrowed
 * pointers into src and into `arena` for synthesised composites. */
static int pxc_build_lsp_def(CBMArena *arena, const CBMDefinition *src, const char *module_qn,
                             const char *namespace_name, CBMLanguage lang, CBMLSPDef *dst) {
    const char *label = pxc_map_label(src->label);
    if (!label && src->label && strcmp(src->label, "Declaration") == 0 &&
        (lang == CBM_LANG_CPP || lang == CBM_LANG_CUDA))
        label = "Declaration";
    if (!label || !src->qualified_name || !src->name)
        return -1;
    memset(dst, 0, sizeof(*dst));
    if (pxc_is_jvm_lang(lang) && namespace_name && namespace_name[0]) {
        dst->qualified_name = pxc_jvm_def_qn(arena, src, namespace_name, label);
        dst->receiver_type = pxc_jvm_type_qn(arena, namespace_name, src->parent_class);
    } else {
        dst->qualified_name = src->qualified_name;
        dst->receiver_type = src->parent_class;
    }
    dst->short_name = src->name;
    dst->label = label;
    dst->def_module_qn = module_qn;
    dst->namespace_name = namespace_name;
    dst->is_interface = (strcmp(label, "Interface") == 0 || strcmp(label, "Protocol") == 0);
    /* Single return-type string. The per-language registrars split on '|'
     * for multi-return languages (Go); single-return languages just see one
     * piece, which is what's already stored. */
    dst->return_types = src->return_type;
    if (lang == CBM_LANG_C || lang == CBM_LANG_CPP || lang == CBM_LANG_CUDA)
        dst->parameter_types = src->param_types;
    dst->embedded_types = pxc_join_pipe(arena, src->base_classes);
    dst->lang = lang;
    dst->cpp_declaration_key = src->declaration_key;
    return 0;
}

/* Carry C/C++ field types across the file-result -> cross-LSP boundary.
 * Field nodes are not independent cross-LSP symbols; their exact parent QN
 * owns the field_defs payload. Two linear passes avoid scanning every field
 * once per class. Result-store summaries retain the same four source fields. */
static void pxc_attach_cpp_fields(CBMFileResult *result, CBMLSPDef *defs, int count) {
    struct FieldText {
        size_t size;
        size_t used;
        char *data;
    };
    CBMHashTable *groups = NULL;
    for (int i = 0; i < result->defs.count; ++i) {
        const CBMDefinition *field = &result->defs.items[i];
        if (!field->label || strcmp(field->label, "Field") != 0 || !field->parent_class ||
            !field->name || !field->return_type || !field->return_type[0])
            continue;
        if (!groups)
            groups = cbm_ht_create(0);
        if (!groups)
            return;
        auto *group = static_cast<FieldText *>(cbm_ht_get(groups, field->parent_class));
        if (!group) {
            group = static_cast<FieldText *>(cbm_arena_alloc(&result->arena, sizeof(FieldText)));
            if (!group) {
                cbm_ht_free(groups);
                return;
            }
            memset(group, 0, sizeof(*group));
            cbm_ht_set(groups, field->parent_class, group);
        }
        size_t name_size = strlen(field->name), type_size = strlen(field->return_type);
        if (name_size > SIZE_MAX - type_size) {
            cbm_ht_free(groups);
            return;
        }
        size_t text_size = name_size + type_size;
        // Reserve separators and the final terminator without size_t wrap.
        if (text_size > SIZE_MAX - 3 || group->size > SIZE_MAX - 3 - text_size) {
            cbm_ht_free(groups);
            return;
        }
        group->size += text_size + 2;
    }
    if (!groups)
        return;
    for (int i = 0; i < result->defs.count; ++i) {
        const CBMDefinition *field = &result->defs.items[i];
        if (!field->label || strcmp(field->label, "Field") != 0 || !field->parent_class ||
            !field->name || !field->return_type || !field->return_type[0])
            continue;
        auto *group = static_cast<FieldText *>(cbm_ht_get(groups, field->parent_class));
        if (!group)
            continue;
        if (!group->data) {
            group->data = static_cast<char *>(cbm_arena_alloc(&result->arena, group->size + 1));
            if (!group->data)
                continue;
        }
        if (group->used)
            group->data[group->used++] = '|';
        size_t n = strlen(field->name);
        memcpy(group->data + group->used, field->name, n);
        group->used += n;
        group->data[group->used++] = ':';
        n = strlen(field->return_type);
        memcpy(group->data + group->used, field->return_type, n);
        group->used += n;
        group->data[group->used] = '\0';
    }
    for (int i = 0; i < count; ++i) {
        if (!cbm_label_is_type_like(defs[i].label))
            continue;
        auto *group = static_cast<FieldText *>(cbm_ht_get(groups, defs[i].qualified_name));
        if (group)
            defs[i].field_defs = group->data;
    }
    cbm_ht_free(groups);
}

/* C++ types are file-qualified in the graph. Join a method implementation to
 * a declaring header only through an exact declaration key and include path.
 * Keep all graph QNs unchanged; the C resolver receives lookup aliases only. */
static void pxc_attach_cpp_identity(CBMFileResult **cache, const cbm_file_info_t *files,
                                    int file_count, CBMLSPDef *defs, int def_count,
                                    const std::vector<int> &def_files) {
    struct Owner {
        std::string semantic;
        const char *qn;
        int file;
    };
    std::vector<Owner> owners;
    std::vector<std::vector<int>> owner_files(file_count);
    std::unordered_map<std::string, std::vector<int>> declarations, implementations;
    std::vector<std::vector<int>> classes(file_count), per_file(file_count);
    bool has_declarations = false;
    for (int i = 0; i < def_count; ++i) {
        if (defs[i].lang != CBM_LANG_CPP && defs[i].lang != CBM_LANG_CUDA)
            continue;
        per_file[def_files[i]].push_back(i);
        if (cbm_label_is_type_like(defs[i].label))
            classes[def_files[i]].push_back(i);
        if (strcmp(defs[i].label, "Declaration") == 0)
            has_declarations = true;
        if (strcmp(defs[i].label, "Method") == 0 && defs[i].cpp_declaration_key)
            implementations[defs[i].cpp_declaration_key].push_back(i);
    }
    if (!has_declarations)
        return;
    std::unordered_map<std::string, int> owner_ids;
    std::unordered_map<std::string, std::string> owner_semantics;
    std::unordered_set<std::string> ambiguous_owners;
    for (int i = 0; i < def_count; ++i) {
        auto &d = defs[i];
        if (strcmp(d.label, "Declaration") != 0 || !d.cpp_declaration_key)
            continue;
        const char *marker = strstr(d.qualified_name, ".__decl_");
        const char *semantic = marker ? strchr(marker + 1, '.') : nullptr;
        if (!semantic)
            continue;
        ++semantic;
        const char *method = strrchr(semantic, '.');
        if (!method)
            continue; // A free function has no declaring class.
        std::string owner_name(semantic, method);
        auto dot = owner_name.rfind('.');
        std::string leaf = owner_name.substr(dot == std::string::npos ? 0 : dot + 1);
        const char *owner_qn = nullptr;
        bool ambiguous = false;
        for (int ci : classes[def_files[i]]) {
            if (leaf != defs[ci].short_name)
                continue;
            if (owner_qn && strcmp(owner_qn, defs[ci].qualified_name) != 0)
                ambiguous = true;
            owner_qn = defs[ci].qualified_name;
        }
        if (!owner_qn || ambiguous)
            continue;
        auto [semantic_it, new_owner] = owner_semantics.emplace(owner_qn, owner_name);
        if (!new_owner && semantic_it->second != owner_name)
            ambiguous_owners.insert(owner_qn);
        std::string key = std::to_string(def_files[i]) + ":" + owner_name;
        auto [it, inserted] = owner_ids.emplace(key, (int)owners.size());
        if (inserted) {
            owner_files[def_files[i]].push_back((int)owners.size());
            owners.push_back({owner_name, owner_qn, def_files[i]});
        }
        d.cpp_declaring_type = owner_qn;
        declarations[d.cpp_declaration_key].push_back(i);
    }
    if (owners.empty())
        return;

    // Resolve source-authored includes: exact relative path first, then an
    // unambiguous path suffix. Multiple suffix matches never select a winner.
    std::unordered_map<std::string, int> paths;
    std::unordered_map<std::string, std::vector<int>> suffixes;
    for (int fi = 0; fi < file_count; ++fi) {
        if (!cache[fi] ||
            (files[fi].language != CBM_LANG_CPP && files[fi].language != CBM_LANG_CUDA))
            continue;
        std::string path =
            std::filesystem::path(files[fi].rel_path).lexically_normal().generic_string();
        paths[path] = fi;
        size_t start = 0;
        for (int segments = 0; segments < 32; ++segments) {
            suffixes[path.substr(start)].push_back(fi);
            auto slash = path.find('/', start);
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
    }
    struct Visibility {
        std::vector<int> files;
        bool ambiguous = false;
    };
    std::vector<Visibility> visible(file_count);
    constexpr size_t max_headers = 256, max_bindings = 4096;
    for (int fi = 0; fi < file_count; ++fi) {
        if (per_file[fi].empty())
            continue;
        auto &v = visible[fi];
        v.files.push_back(fi);
        std::unordered_set<int> seen{fi};
        for (size_t pos = 0; pos < v.files.size() && !v.ambiguous; ++pos) {
            int from = v.files[pos];
            for (int ii = 0; ii < cache[from]->imports.count; ++ii) {
                const char *raw = cache[from]->imports.items[ii].module_path;
                if (!raw || !raw[0])
                    continue;
                std::filesystem::path include(raw);
                if (include.is_absolute())
                    continue;
                std::string relative =
                    (std::filesystem::path(files[from].rel_path).parent_path() / include)
                        .lexically_normal()
                        .generic_string();
                int target = -1;
                if (auto exact = paths.find(relative); exact != paths.end())
                    target = exact->second;
                else if (auto suffix = suffixes.find(include.lexically_normal().generic_string());
                         suffix != suffixes.end()) {
                    if (suffix->second.size() != 1) {
                        v.ambiguous = true;
                        break;
                    }
                    target = suffix->second[0];
                }
                if (target < 0 || seen.count(target))
                    continue;
                if (v.files.size() == max_headers) {
                    v.ambiguous = true;
                    break;
                }
                seen.insert(target);
                v.files.push_back(target);
            }
        }
        std::unordered_map<std::string, const char *> names;
        auto bind = [&](const std::string &name, const char *qn) {
            if (v.ambiguous)
                return;
            if (names.size() == max_bindings && !names.count(name)) {
                v.ambiguous = true;
                return;
            }
            auto [it, inserted] = names.emplace(name, qn);
            if (!inserted && (!it->second || !qn || strcmp(it->second, qn) != 0))
                it->second = nullptr;
        };
        for (int included : v.files) {
            if (v.ambiguous)
                break;
            for (int oi : owner_files[included]) {
                if (v.ambiguous)
                    break;
                const auto &owner = owners[oi];
                const char *target = ambiguous_owners.count(owner.qn) ? nullptr : owner.qn;
                bind(owner.semantic, target);
                auto dot = owner.semantic.rfind('.');
                bind(owner.semantic.substr(dot == std::string::npos ? 0 : dot + 1), target);
            }
        }
        // A file-level marker blocks the old short-name fallback even when
        // the closure/name budget stops us before enumerating every type.
        if (v.ambiguous) {
            names.clear();
            names.emplace("__blocked__", nullptr);
        }
        if (names.empty())
            continue;
        bool ambiguous_names = v.ambiguous;
        for (const auto &[name, qn] : names)
            if (!qn)
                ambiguous_names = true;
        if (ambiguous_names) {
            // One additional control entry, beyond the bounded name payload.
            names.emplace("__no_weak_receiver__", nullptr);
        }
        auto *bindings = static_cast<CBMCVisibleType *>(
            cbm_arena_alloc(&cache[fi]->arena, names.size() * sizeof(CBMCVisibleType)));
        if (!bindings)
            continue;
        int count = 0;
        for (const auto &[name, qn] : names) {
            bindings[count++] = {cbm_arena_sprintf(&cache[fi]->arena, "%s.__cpp_visible.%s",
                                                   defs[per_file[fi][0]].def_module_qn,
                                                   name.c_str()),
                                 v.ambiguous ? nullptr : qn};
        }
        defs[per_file[fi][0]].cpp_visible_types = bindings;
        defs[per_file[fi][0]].cpp_visible_type_count = count;
    }

    for (const auto &[key, candidates] : implementations) {
        auto found = declarations.find(key);
        if (found == declarations.end())
            continue;
        // Multiple distinct definitions of the same semantic signature are
        // ambiguous; do not choose one based on traversal or file order.
        std::unordered_set<std::string> impl_qns;
        for (int i : candidates)
            impl_qns.insert(defs[i].qualified_name);
        if (impl_qns.size() != 1)
            continue;
        for (int i : candidates) {
            const auto &v = visible[def_files[i]];
            if (v.ambiguous)
                continue;
            const char *owner = nullptr;
            bool ambiguous = false;
            for (int di : found->second) {
                if (std::find(v.files.begin(), v.files.end(), def_files[di]) == v.files.end())
                    continue;
                const char *qn = defs[di].cpp_declaring_type;
                if (ambiguous_owners.count(qn)) {
                    ambiguous = true;
                    continue;
                }
                if (owner && strcmp(owner, qn) != 0)
                    ambiguous = true;
                owner = qn;
            }
            if (owner && !ambiguous)
                defs[i].cpp_declaring_type = owner;
        }
    }
}

/* Collect a project-wide CBMLSPDef[] from all cached results. Returns a
 * malloc'd array (caller frees) of length *out_count. String fields are
 * borrowed from cache[i]->arena and from def_modules[i] (also borrowed). */
CBMLSPDef *cbm_pxc_collect_all_defs(CBMFileResult **cache, const cbm_file_info_t *files,
                                    int file_count, const char *project_name, char **def_modules,
                                    int *out_count) {
    int total = 0;
    for (int i = 0; i < file_count; i++) {
        if (cache[i])
            total += cache[i]->defs.count;
    }
    if (total == 0) {
        *out_count = 0;
        return NULL;
    }
    CBMLSPDef *defs = (CBMLSPDef *)calloc((size_t)total, sizeof(CBMLSPDef));
    if (!defs) {
        *out_count = 0;
        return NULL;
    }
    int idx = 0;
    std::vector<int> def_files;
    def_files.reserve(total);
    for (int fi = 0; fi < file_count; fi++) {
        if (!cache[fi])
            continue;
        if (!def_modules[fi]) {
            def_modules[fi] = cbm_pipeline_fqn_module_dir(project_name, files[fi].rel_path,
                                                          pxc_module_is_dir(files[fi].language));
        }
        const char *namespace_name = cache[fi]->namespace_name;
        if ((!namespace_name || !namespace_name[0]) && files[fi].rel_path) {
            namespace_name =
                pxc_infer_jvm_namespace(&cache[fi]->arena, files[fi].rel_path, files[fi].language);
            if (namespace_name && namespace_name[0]) {
                cache[fi]->namespace_name = namespace_name;
            }
        }
        int first_def = idx;
        for (int di = 0; di < cache[fi]->defs.count; di++) {
            if (pxc_build_lsp_def(&cache[fi]->arena, &cache[fi]->defs.items[di], def_modules[fi],
                                  namespace_name, files[fi].language, &defs[idx]) == 0) {
                def_files.push_back(fi);
                idx++;
            }
        }
        if (files[fi].language == CBM_LANG_C || files[fi].language == CBM_LANG_CPP ||
            files[fi].language == CBM_LANG_CUDA)
            pxc_attach_cpp_fields(cache[fi], defs + first_def, idx - first_def);
    }
    pxc_attach_cpp_identity(cache, files, file_count, defs, idx, def_files);
    *out_count = idx;
    return defs;
}

/* Build per-file import map (local_name -> resolved module QN) from gbuf
 * IMPORTS edges. Mirrors build_import_map() in pass_parallel.c. Returns 0
 * with *out_count = 0 when the file has no IMPORTS edges. Caller frees keys
 * with pxc_free_import_map. */
static int pxc_build_import_map(const cbm_gbuf_t *gbuf, const char *project_name,
                                const char *rel_path, const char ***out_keys,
                                const char ***out_vals, int *out_count) {
    *out_keys = NULL;
    *out_vals = NULL;
    *out_count = 0;

    char *file_qn = cbm_pipeline_fqn_compute(project_name, rel_path, "__file__");
    if (!file_qn)
        return 0;
    const cbm_gbuf_node_t *file_node = cbm_gbuf_find_by_qn(gbuf, file_qn);
    free(file_qn);
    if (!file_node)
        return 0;

    const cbm_gbuf_edge_t **edges = NULL;
    int edge_count = 0;
    int rc =
        cbm_gbuf_find_edges_by_source_type(gbuf, file_node->id, "IMPORTS", &edges, &edge_count);
    if (rc != 0 || edge_count == 0)
        return 0;

    const char **keys = (const char **)calloc((size_t)edge_count, sizeof(const char *));
    const char **vals = (const char **)calloc((size_t)edge_count, sizeof(const char *));
    if (!keys || !vals) {
        free(keys);
        free(vals);
        return 0;
    }
    int count = 0;
    for (int i = 0; i < edge_count; i++) {
        const cbm_gbuf_edge_t *e = edges[i];
        const cbm_gbuf_node_t *target = cbm_gbuf_find_by_id(gbuf, e->target_id);
        if (!target || !e->properties_json)
            continue;
        const char *start = strstr(e->properties_json, "\"local_name\":\"");
        if (!start)
            continue;
        start += strlen("\"local_name\":\"");
        const char *end = strchr(start, '"');
        if (!end || end <= start)
            continue;
        size_t n = (size_t)(end - start);
        char *local = (char *)malloc(n + 1);
        if (!local)
            continue;
        memcpy(local, start, n);
        local[n] = '\0';
        keys[count] = local;
        vals[count] = target->qualified_name; /* borrowed from gbuf */
        count++;
    }
    *out_keys = keys;
    *out_vals = vals;
    *out_count = count;
    return 0;
}

static void pxc_free_import_map(const char **keys, const char **vals, int count) {
    if (keys) {
        for (int i = 0; i < count; i++)
            free((void *)keys[i]);
        free((void *)keys);
    }
    free((void *)vals); /* vals strings borrowed from gbuf — don't free elements */
}

/* Detect TS dialect flags from a relative path. */
void cbm_pxc_ts_modes(CBMLanguage lang, const char *rel_path, bool *out_js, bool *out_jsx,
                      bool *out_dts) {
    *out_js = (lang == CBM_LANG_JAVASCRIPT);
    *out_jsx = (lang == CBM_LANG_TSX);
    *out_dts = false;
    if (!rel_path)
        return;
    size_t rl = strlen(rel_path);
    if (lang == CBM_LANG_JAVASCRIPT && rl >= 4 && strcmp(rel_path + rl - 4, ".jsx") == 0) {
        *out_jsx = true;
    }
    if (lang == CBM_LANG_TYPESCRIPT && rl >= 5 && strcmp(rel_path + rl - 5, ".d.ts") == 0) {
        *out_dts = true;
    }
}

/* Returns true when this language has a cross-file LSP wired up. */
bool cbm_pxc_has_cross_lsp(CBMLanguage lang) {
    switch (lang) {
    case CBM_LANG_GO:
    case CBM_LANG_C:
    case CBM_LANG_CPP:
    case CBM_LANG_CUDA:
    case CBM_LANG_PYTHON:
    case CBM_LANG_JAVASCRIPT:
    case CBM_LANG_TYPESCRIPT:
    case CBM_LANG_TSX:
    case CBM_LANG_PHP:
    case CBM_LANG_CSHARP: /* tier-2 prebuilt registry path (pass_parallel.c) */
    case CBM_LANG_JAVA:   /* tier-2 prebuilt registry path (pass_parallel.c) */
    case CBM_LANG_KOTLIN: /* fallback cbm_pxc_run_one path */
    case CBM_LANG_RUST:   /* fallback cbm_pxc_run_one path (manifest-aware) */
        return true;
    default:
        return false;
    }
}

/* Append cross-file results from `src_out` (allocated in a scratch arena
 * about to be destroyed) into `dst_calls` (lives in cache_entry->arena),
 * copying every string field into dst_arena. Skips entries whose
 * (caller_qn, callee_qn) is already present — avoids inflating the array with
 * cross-file duplicates of per-file LSP output.
 *
 * Dedup uses a hash set keyed on "caller\x1fcallee", giving O(1) membership.
 * The previous linear strcmp scan made each append O(n), so a file that
 * resolved very many cross-calls turned the whole append into O(n^2) and could
 * peg a core for minutes (observed: an index hung in pxc_append_results/strcmp).
 * The key strings live in a scratch arena that is destroyed after the table. */
static void pxc_append_results(CBMArena *dst_arena, CBMResolvedCallArray *dst_calls,
                               const CBMResolvedCallArray *src_out) {
    if (!dst_calls || !src_out)
        return;

    CBMArena keys;
    cbm_arena_init(&keys);
    CBMHashTable *seen = cbm_ht_create((uint32_t)(dst_calls->count + src_out->count + 1));

    for (int i = 0; i < dst_calls->count; i++) {
        const CBMResolvedCall *rc = &dst_calls->items[i];
        if (rc->caller_qn && rc->callee_qn) {
            char *k = cbm_arena_sprintf(&keys, "%s\x1f%s", rc->caller_qn, rc->callee_qn);
            if (k) {
                cbm_ht_set(seen, k, (void *)(uintptr_t)1);
            }
        }
    }

    for (int j = 0; j < src_out->count; j++) {
        const CBMResolvedCall *src = &src_out->items[j];
        if (!src->caller_qn || !src->callee_qn)
            continue;
        char *k = cbm_arena_sprintf(&keys, "%s\x1f%s", src->caller_qn, src->callee_qn);
        if (k && cbm_ht_has(seen, k))
            continue;
        if (k) {
            cbm_ht_set(seen, k, (void *)(uintptr_t)1);
        }
        CBMResolvedCall dst = {};
        dst.caller_qn = cbm_arena_strdup(dst_arena, src->caller_qn);
        dst.callee_qn = cbm_arena_strdup(dst_arena, src->callee_qn);
        dst.strategy = src->strategy ? cbm_arena_strdup(dst_arena, src->strategy) : NULL;
        dst.confidence = src->confidence;
        dst.reason = src->reason ? cbm_arena_strdup(dst_arena, src->reason) : NULL;
        cbm_resolvedcall_push(dst_calls, dst_arena, dst);
    }

    cbm_ht_free(seen);
    cbm_arena_destroy(&keys);
}

/* ── Rust workspace manifest (Cargo.toml) for cross-CRATE resolution ──
 *
 * cbm_pxc_run_one's signature is shared with the parallel pass
 * (pass_parallel.c) and cannot grow a manifest parameter without touching
 * that file. We therefore pass the parsed workspace manifest to the Rust
 * cross-file resolver through a file-static borrowed pointer that the
 * sequential driver (cbm_pipeline_pass_lsp_cross, below) sets up once per
 * pass run from the project's root Cargo.toml. The manifest's strings are
 * owned by `g_pxc_rust_manifest_arena`; the pointer is borrowed (NULL when
 * the project has no Cargo.toml — single-crate / non-workspace projects,
 * where in-file resolution needs no workspace metadata). */
static thread_local const CBMCargoManifest *g_pxc_rust_manifest = NULL;

void cbm_pxc_set_rust_manifest(const CBMCargoManifest *m) {
    g_pxc_rust_manifest = m;
}

const struct CBMCargoManifest *cbm_pxc_get_rust_manifest(void) {
    return g_pxc_rust_manifest;
}

/* Convert a CBMLSPDef array (the pipeline's lingua franca, go_lsp.h:73)
 * into a CBMRustLSPDef array (rust_lsp.h) inside `arena`. The two structs
 * share their first 9 string fields; CBMRustLSPDef adds `trait_qn` before
 * `is_interface` whereas CBMLSPDef has `is_interface` followed by `lang`,
 * so a memcpy is unsafe — copy field-by-field. trait_qn is left NULL
 * because the pipeline's collect-all-defs step does not carry the
 * impl-Trait-for-Type linkage; the resolver still recovers trait dispatch
 * from the in-file walk (the cross-file path only needs receiver_type). */
static CBMRustLSPDef *pxc_lspdefs_to_rust(CBMArena *arena, const CBMLSPDef *defs, int def_count) {
    if (!defs || def_count <= 0)
        return NULL;
    CBMRustLSPDef *out =
        (CBMRustLSPDef *)cbm_arena_alloc(arena, (size_t)def_count * sizeof(CBMRustLSPDef));
    if (!out)
        return NULL;
    for (int i = 0; i < def_count; i++) {
        out[i].qualified_name = defs[i].qualified_name;
        out[i].short_name = defs[i].short_name;
        out[i].label = defs[i].label;
        out[i].receiver_type = defs[i].receiver_type;
        out[i].def_module_qn = defs[i].def_module_qn;
        out[i].return_types = defs[i].return_types;
        out[i].embedded_types = defs[i].embedded_types;
        out[i].field_defs = defs[i].field_defs;
        out[i].method_names_str = defs[i].method_names_str;
        out[i].trait_qn = NULL;
        out[i].is_interface = defs[i].is_interface;
    }
    return out;
}

/* Run cross-file LSP for a single file inside a scratch arena that gets
 * freed when the call returns. The LSP would otherwise allocate a fresh
 * type registry + stdlib + all project defs into the supplied arena, and
 * that adds up to O(N×project_size) memory if we used cache[i]->arena
 * directly across N files (test_incremental.c saw 3.5 GB peak on a
 * 1100-file repo before this fix). Output gets copied into the file's own
 * arena and merged into result->resolved_calls. */
void cbm_pxc_run_one(CBMLanguage lang, CBMFileResult *r, const char *source, int source_len,
                     const char *module_qn, CBMLSPDef *defs, int def_count, const char **imp_names,
                     const char **imp_qns, int imp_count) {
    TSTree *tree = r->cached_tree; /* may be NULL — LSP re-parses then */

    CBMArena scratch;
    cbm_arena_init(&scratch);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));

    switch (lang) {
    case CBM_LANG_GO:
        cbm_run_go_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names,
                             imp_qns, imp_count, tree, &out);
        break;
    case CBM_LANG_C:
    case CBM_LANG_CPP:
    case CBM_LANG_CUDA: {
        bool cpp_mode = (lang != CBM_LANG_C);
        /* C/C++ cross LSP takes include_paths/include_ns_qns instead of
         * imports — the existing pipeline doesn't carry C-style include
         * resolution as a separate map, so pass NULL/0 and let the LSP
         * fall back to its own #include scan. */
        cbm_run_c_lsp_cross(&scratch, source, source_len, module_qn, cpp_mode, defs, def_count,
                            NULL, NULL, 0, tree, &out);
        break;
    }
    case CBM_LANG_PYTHON:
        cbm_run_py_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names,
                             imp_qns, imp_count, tree, &out);
        break;
    case CBM_LANG_PHP:
        cbm_run_php_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names,
                              imp_qns, imp_count, tree, &out);
        break;
    case CBM_LANG_JAVA:
        cbm_run_java_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count, imp_names,
                               imp_qns, imp_count, tree, &out);
        break;
    case CBM_LANG_KOTLIN:
        cbm_run_kotlin_lsp_cross(&scratch, source, source_len, module_qn, defs, def_count,
                                 imp_names, imp_qns, imp_count, tree, &out);
        break;
    case CBM_LANG_RUST: {
        /* The Rust resolver wants CBMRustLSPDef (rust_lsp.h), not the
         * pipeline's CBMLSPDef — the structs share their first 9 fields
         * but diverge after, so convert into the scratch arena. The
         * workspace manifest (set once by the sequential driver) lets
         * `crate_a::foo` route across the crate boundary (#56). */
        CBMRustLSPDef *rdefs = pxc_lspdefs_to_rust(&scratch, defs, def_count);
        cbm_run_rust_lsp_cross_with_manifest(&scratch, source, source_len, module_qn, rdefs,
                                             def_count, imp_names, imp_qns, imp_count, tree,
                                             g_pxc_rust_manifest, &out);
        break;
    }
    default:
        break;
    }

    pxc_append_results(&r->arena, &r->resolved_calls, &out);
    cbm_arena_destroy(&scratch);
}

/* Variant of cbm_pxc_run_one for TS/JS/JSX/TSX with explicit dialect
 * flags. Same scratch-arena lifecycle as cbm_pxc_run_one. */
void cbm_pxc_run_one_ts(CBMFileResult *r, const char *source, int source_len, const char *module_qn,
                        CBMLSPDef *defs, int def_count, const char **imp_names,
                        const char **imp_qns, int imp_count, bool js_mode, bool jsx_mode,
                        bool dts_mode) {
    CBMArena scratch;
    cbm_arena_init(&scratch);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));

    cbm_run_ts_lsp_cross(&scratch, source, source_len, module_qn, js_mode, jsx_mode, dts_mode, defs,
                         def_count, imp_names, imp_qns, imp_count, r->cached_tree, &out);

    pxc_append_results(&r->arena, &r->resolved_calls, &out);
    cbm_arena_destroy(&scratch);
}

/* Per-file cross-LSP dispatch, shared by the PARALLEL resolve worker and the
 * SEQUENTIAL driver. One code path = one semantics: filter the global defs
 * down to the file's own+imported modules via the module-def index, resolve
 * through the shared prebuilt registry when the language has one (per-file
 * OVERLAY pattern — no registry build, no finalize), and only fall back to
 * the per-file registry build (with the FILTERED defs) for languages without
 * a shared-registry variant. Before this helper existed the sequential
 * driver fed the FULL def list into full per-file registry builds —
 * O(files x defs), which ground an 81k-file TS corpus for hours.
 *
 * `rust_shared_get` supplies the lazily-built shared Rust all-defs registry
 * (the parallel resolver owns its once-guard); NULL means "no shared rust
 * registry available" and rust NULL-filter files take the per-file build. */
/* Per-file registry-build cost counters. Surfaced by pass_parallel at the end of
 * resolve: per-file registry build cost is driven by defs_per_file, so if that
 * tracks the CORPUS rather than the file's own module plus imports, the module
 * filter is not containing the work and cross-file LSP is O(files x corpus_defs)
 * — the exact shape the in-run scaling probe cannot see. */
static cbm_atomic_uint64 g_pxc_defs_registered{0};
static cbm_atomic_uint64 g_pxc_build_files{0};
static cbm_atomic_uint64 g_pxc_filter_files{0};
static cbm_atomic_uint64 g_pxc_filter_failed{0};

void cbm_pxc_filter_stats(uint64_t *defs_registered, uint64_t *build_files, uint64_t *filter_files,
                          uint64_t *filter_failed) {
    if (defs_registered) {
        *defs_registered = atomic_load_explicit(&g_pxc_defs_registered, memory_order_relaxed);
    }
    if (build_files) {
        *build_files = atomic_load_explicit(&g_pxc_build_files, memory_order_relaxed);
    }
    if (filter_files) {
        *filter_files = atomic_load_explicit(&g_pxc_filter_files, memory_order_relaxed);
    }
    if (filter_failed) {
        *filter_failed = atomic_load_explicit(&g_pxc_filter_failed, memory_order_relaxed);
    }
}

// Ambiguity applies only to receiver diagnostics from this authored-source
// dispatch. Older/preprocessed offsets can collide with unrelated raw sites.
// Nested calls can also share an offset, so match caller AND callee leaf.
static void pxc_guard_ambiguous_cpp_receivers(CBMFileResult *result, int first_new_record) {
    constexpr size_t max_chunk_records = 4096;
    std::unordered_multimap<uint32_t, const CBMResolvedCall *> unresolved_receivers;
    auto apply_chunk = [&]() {
        for (int ci = 0; ci < result->calls.count; ++ci) {
            auto &call = result->calls.items[ci];
            if (!call.enclosing_func_qn || !call.callee_name)
                continue;
            auto range = unresolved_receivers.equal_range(call.source_byte);
            for (auto it = range.first; it != range.second; ++it) {
                const auto &diagnostic = *it->second;
                if (strcmp(diagnostic.caller_qn, call.enclosing_func_qn) == 0 &&
                    strcmp(cbm_lsp_bare_segment(diagnostic.callee_qn),
                           cbm_lsp_bare_segment(call.callee_name)) == 0) {
                    call.requires_typed_resolution = true;
                    break;
                }
            }
        }
        unresolved_receivers.clear();
    };
    // Bound scratch independently of file size. Full chunks are applied before
    // continuing; overflow never disables unrelated free-function fallback.
    for (int ri = std::max(0, first_new_record); ri < result->resolved_calls.count; ++ri) {
        const auto &call = result->resolved_calls.items[ri];
        if (!call.source_byte || !call.caller_qn || !call.callee_qn || !call.strategy ||
            strcmp(call.strategy, "lsp_unresolved") != 0 || !call.reason)
            continue;
        if (strcmp(call.reason, "unknown_receiver_type") != 0 &&
            strcmp(call.reason, "method_not_found") != 0)
            continue;
        unresolved_receivers.emplace(call.source_byte, &call);
        if (unresolved_receivers.size() == max_chunk_records)
            apply_chunk();
    }
    if (!unresolved_receivers.empty())
        apply_chunk();
}

#ifdef CBM_ENABLE_TEST_SEAMS
extern "C" void cbm_test_guard_ambiguous_cpp_receivers(CBMFileResult *result,
                                                       int first_new_record) {
    pxc_guard_ambiguous_cpp_receivers(result, first_new_record);
}
#endif

void cbm_pxc_dispatch_file(CBMLanguage lang, CBMFileResult *result, const char *source,
                           int source_len, const char *rel, const char *def_module,
                           const CBMCrossLspRegistries *cross_registries,
                           const CBMModuleDefIndex *module_def_index, CBMLSPDef *all_defs,
                           int all_def_count, const char **imp_keys, const char **imp_vals,
                           int imp_count, CBMTypeRegistry *(*rust_shared_get)(void *),
                           void *rust_shared_ctx) {
    if (!result) {
        return;
    }
    const int raw_cross_start = result->resolved_calls.count;
    bool used_prebuilt = false;
    CBMTypeRegistry *prebuilt =
        cross_registries ? cbm_pxc_registry_for_lang(cross_registries, lang) : NULL;
    if (prebuilt) {
        switch (lang) {
        case CBM_LANG_GO:
            /* Tier 3 (metadata-driven): pure lookup over the Tier-1
             * lsp_unresolved entries — no parse, no AST walk. */
            cbm_go_fast_resolve_qualified_calls(result, prebuilt, imp_keys, imp_vals, imp_count);
            used_prebuilt = true;
            break;
        case CBM_LANG_PYTHON:
            cbm_run_py_lsp_cross_with_registry(&result->arena, source, source_len, def_module,
                                               prebuilt, imp_keys, imp_vals, imp_count,
                                               result->cached_tree, &result->resolved_calls);
            used_prebuilt = true;
            break;
        case CBM_LANG_C:
        case CBM_LANG_CPP:
        case CBM_LANG_CUDA:
            cbm_run_c_lsp_cross_with_registry(
                &result->arena, source, source_len, def_module, (lang != CBM_LANG_C), prebuilt,
                imp_keys, imp_vals, imp_count, result->cached_tree, &result->resolved_calls);
            // Snapshot-backed workers load the original call records after
            // registry construction, so apply the same ambiguity guard here.
            if (cbm_registry_lookup_type(prebuilt,
                                         cbm_arena_sprintf(&result->arena,
                                                           "%s.__cpp_visible.__no_weak_receiver__",
                                                           def_module))) {
                pxc_guard_ambiguous_cpp_receivers(result, raw_cross_start);
            }
            used_prebuilt = true;
            break;
        case CBM_LANG_CSHARP:
            cbm_run_cs_lsp_cross_with_registry(&result->arena, source, source_len, def_module,
                                               prebuilt, imp_vals, imp_count, result->cached_tree,
                                               &result->resolved_calls);
            used_prebuilt = true;
            break;
        case CBM_LANG_JAVA:
            /* Own-FILE defs go into a per-file overlay; imports, same-package
             * siblings and stdlib all resolve through the shared base. */
            cbm_run_java_lsp_cross_with_registry(
                &result->arena, result, source, source_len, def_module, prebuilt, imp_keys,
                imp_vals, imp_count, result->cached_tree, &result->resolved_calls);
            used_prebuilt = true;
            break;
        case CBM_LANG_JAVASCRIPT:
        case CBM_LANG_TYPESCRIPT:
        case CBM_LANG_TSX: {
            /* TS: per-file OVERLAY chained to the shared base. Filter to
             * own+imports so the overlay builder can pick out own-module
             * defs without scanning the whole project. */
            bool js;
            bool jsx;
            bool dts;
            cbm_pxc_ts_modes(lang, rel, &js, &jsx, &dts);
            CBMLSPDef *ts_defs = all_defs;
            int ts_def_count = all_def_count;
            CBMLSPDef *ts_filtered = NULL;
            if (module_def_index) {
                int fc = 0;
                ts_filtered = cbm_pxc_filter_defs_for_file(module_def_index, all_defs, lang,
                                                           result->namespace_name, def_module,
                                                           imp_vals, imp_count, &fc);
                if (ts_filtered) {
                    ts_defs = ts_filtered;
                    ts_def_count = fc;
                }
            }
            cbm_run_ts_lsp_cross_with_registry(&result->arena, source, source_len, def_module, js,
                                               jsx, dts, prebuilt, ts_defs, ts_def_count, imp_keys,
                                               imp_vals, imp_count, result->cached_tree,
                                               &result->resolved_calls);
            free(ts_filtered);
            used_prebuilt = true;
            break;
        }
        /* PHP falls through to the per-file build path below until its
         * overlay variant lands. */
        default:
            break;
        }
    }

    if (used_prebuilt) {
        return;
    }
    /* Fallback: gopls per-file filter + per-file registry build. RUST is
     * exempt from the module filter: its resolution is Cargo-manifest-aware
     * and a `crate_a::foo` reference routes to defs in ANOTHER workspace
     * crate — a module that is in neither own_module nor the import map, so
     * the filter starves cross-crate resolution (#56 repro red). Rust
     * therefore always resolves against the FULL def universe: the lazily
     * built shared registry when available, else a full per-file build. */
    CBMLSPDef *filtered = NULL;
    CBMLSPDef *file_defs = all_defs;
    int file_def_count = all_def_count;
    if (module_def_index && lang != CBM_LANG_RUST) {
        int filtered_count = 0;
        filtered =
            cbm_pxc_filter_defs_for_file(module_def_index, all_defs, lang, result->namespace_name,
                                         def_module, imp_vals, imp_count, &filtered_count);
        atomic_fetch_add_explicit(&g_pxc_filter_files, 1, memory_order_relaxed);
        if (filtered) {
            file_defs = filtered;
            file_def_count = filtered_count;
        } else {
            atomic_fetch_add_explicit(&g_pxc_filter_failed, 1, memory_order_relaxed);
        }
    }
    /* Charged whichever path runs: a failed filter means the file registered the
     * WHOLE corpus, which is precisely what this number exists to reveal. */
    atomic_fetch_add_explicit(&g_pxc_defs_registered, (uint64_t)file_def_count,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_pxc_build_files, 1, memory_order_relaxed);
    if (lang == CBM_LANG_RUST) {
        CBMTypeRegistry *shared = rust_shared_get ? rust_shared_get(rust_shared_ctx) : NULL;
        if (shared) {
            cbm_run_rust_lsp_cross_with_registry(&result->arena, source, source_len, def_module,
                                                 shared, imp_keys, imp_vals, imp_count,
                                                 result->cached_tree, cbm_pxc_get_rust_manifest(),
                                                 &result->resolved_calls,
                                                 /*result=*/NULL);
        } else {
            cbm_pxc_run_one(lang, result, source, source_len, def_module, file_defs, file_def_count,
                            imp_keys, imp_vals, imp_count);
        }
    } else if (lang == CBM_LANG_JAVASCRIPT || lang == CBM_LANG_TYPESCRIPT || lang == CBM_LANG_TSX) {
        bool js;
        bool jsx;
        bool dts;
        cbm_pxc_ts_modes(lang, rel, &js, &jsx, &dts);
        cbm_pxc_run_one_ts(result, source, source_len, def_module, file_defs, file_def_count,
                           imp_keys, imp_vals, imp_count, js, jsx, dts);
    } else {
        cbm_pxc_run_one(lang, result, source, source_len, def_module, file_defs, file_def_count,
                        imp_keys, imp_vals, imp_count);
    }
    if (lang == CBM_LANG_CPP || lang == CBM_LANG_CUDA) {
        const char *marker =
            cbm_arena_sprintf(&result->arena, "%s.__cpp_visible.__no_weak_receiver__", def_module);
        bool ambiguous = false;
        for (int di = 0; di < file_def_count && !ambiguous; ++di) {
            for (int vi = 0; vi < file_defs[di].cpp_visible_type_count; ++vi) {
                if (strcmp(file_defs[di].cpp_visible_types[vi].name, marker) == 0) {
                    ambiguous = true;
                    break;
                }
            }
        }
        if (ambiguous)
            pxc_guard_ambiguous_cpp_receivers(result, raw_cross_start);
    }
    free(filtered);
}

/* Parse the project's root Cargo.toml (if present) into `out_m`, using
 * `marena` for the manifest's owned strings. Returns true when a manifest
 * was parsed (a workspace root or any [package]/[dependencies]); false when
 * there is no readable Cargo.toml, leaving *out_m untouched. The resulting
 * manifest feeds cross-CRATE Rust resolution (#56): its [workspace].members
 * map lets `crate_a::foo` route to the member crate's def. */
static bool pxc_build_rust_manifest(const cbm_pipeline_ctx_t *ctx, CBMArena *marena,
                                    CBMCargoManifest *out_m) {
    if (!ctx || !ctx->repo_path || !marena || !out_m)
        return false;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/Cargo.toml", ctx->repo_path);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;
    int toml_len = 0;
    char *toml = pxc_read_file(path, &toml_len);
    if (!toml || toml_len <= 0) {
        free(toml);
        return false;
    }
    memset(out_m, 0, sizeof(*out_m));
    cbm_cargo_parse(marena, toml, toml_len, out_m);
    free(toml); /* cargo parser copies into marena */
    return true;
}

int cbm_pipeline_pass_lsp_cross(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                                int file_count, CBMFileResult **cache) {
    if (!ctx || !files || file_count <= 0 || !cache)
        return 0;

    cbm_log_info("pass.start", "pass", "lsp_cross", "files", itoa_buf(file_count));

    /* Build the Rust workspace manifest once (only when the project has at
     * least one Rust file, to avoid an unconditional Cargo.toml read).
     * The manifest's strings live in `cargo_arena`; the resolver borrows
     * the pointer through the file-static set below. */
    bool have_rust = false;
    for (int i = 0; i < file_count; i++) {
        if (cache[i] && files[i].language == CBM_LANG_RUST) {
            have_rust = true;
            break;
        }
    }
    CBMArena cargo_arena;
    CBMCargoManifest cargo_manifest;
    bool have_manifest = false;
    if (have_rust) {
        cbm_arena_init(&cargo_arena);
        have_manifest = pxc_build_rust_manifest(ctx, &cargo_arena, &cargo_manifest);
        cbm_pxc_set_rust_manifest(have_manifest ? &cargo_manifest : NULL);
    }

    /* Per-file module QN cache so we don't recompute it once per def + once
     * per call. cbm_pipeline_fqn_module mallocs; freed at end. */
    char **def_modules = (char **)calloc((size_t)file_count, sizeof(char *));
    if (!def_modules) {
        cbm_log_error("pass.err", "pass", "lsp_cross", "phase", "alloc");
        return 0;
    }

    int def_count = 0;
    CBMLSPDef *all_defs = cbm_pxc_collect_all_defs(cache, files, file_count, ctx->project_name,
                                                   def_modules, &def_count);

    /* Shared prepare (mirrors run_parallel_pipeline): inverted module-def
     * index + per-language shared registries, built ONCE for the whole pass.
     * The per-file loop below then dispatches through the SAME helper the
     * parallel resolve worker uses — previously this driver handed the FULL
     * def list to full per-file registry builds (O(files x defs); the
     * ms-typescript sequential crawl). The registries live in the
     * CALLER-OWNED ctx->seq_cross_arena: resolved_calls may borrow registry
     * strings that the later calls pass still reads, so the arena must
     * outlive this pass (run_sequential_pipeline destroys it after all
     * passes; freeing here was a pass_calls use-after-free). */
    CBMModuleDefIndex *module_def_index =
        all_defs ? cbm_pxc_build_module_def_index(all_defs, def_count) : NULL;
    CBMCrossLspRegistries cross_registries = {};
    if (all_defs) {
        CBMArena *xa = &ctx->seq_cross_arena;
        if (!ctx->seq_cross_arena_live) {
            cbm_arena_init(xa);
            ctx->seq_cross_arena_live = true;
        }
        cross_registries.go = cbm_go_build_cross_registry(xa, all_defs, def_count);
        cross_registries.python = cbm_py_build_cross_registry(xa, all_defs, def_count);
        cross_registries.c = cbm_c_build_cross_registry(xa, all_defs, def_count);
        cross_registries.cs = cbm_cs_build_cross_registry(xa, all_defs, def_count);
        cross_registries.ts = cbm_ts_build_cross_registry(xa, all_defs, def_count);
    }

    int processed = 0;
    int skipped_no_lsp = 0;
    int skipped_no_source = 0;
    int per_lang_calls = 0;

    for (int i = 0; i < file_count; i++) {
        if (!cache[i])
            continue;
        CBMLanguage lang = files[i].language;
        if ((lang == CBM_LANG_CPP || lang == CBM_LANG_CUDA) &&
            cbm_pipeline_cpp_primitives_complete(cache[i])) {
            continue;
        }
        if (!cbm_pxc_has_cross_lsp(lang)) {
            skipped_no_lsp++;
            continue;
        }

        int source_len = 0;
        char *source = pxc_read_file(files[i].path, &source_len);
        if (!source || source_len <= 0) {
            free(source);
            skipped_no_source++;
            continue;
        }

        if (!def_modules[i]) {
            def_modules[i] = cbm_pipeline_fqn_module_dir(ctx->project_name, files[i].rel_path,
                                                         pxc_module_is_dir(files[i].language));
        }

        const char **imp_keys = NULL;
        const char **imp_vals = NULL;
        int imp_count = 0;
        pxc_build_import_map(ctx->gbuf, ctx->project_name, files[i].rel_path, &imp_keys, &imp_vals,
                             &imp_count);

        /* Journal around the resolve: a hang here must be attributed to THIS
         * file, not to a stale extraction marker (the innocent-quarantine
         * failure mode). */
        cbm_index_mark_start(files[i].rel_path);
        cbm_pxc_dispatch_file(lang, cache[i], source, source_len, files[i].rel_path, def_modules[i],
                              &cross_registries, module_def_index, all_defs, def_count, imp_keys,
                              imp_vals, imp_count, NULL, NULL);
        cbm_index_mark_done(files[i].rel_path);
        per_lang_calls++;
        processed++;

        pxc_free_import_map(imp_keys, imp_vals, imp_count);
        free(source);
    }

    cbm_pxc_free_module_def_index(module_def_index);
    free(all_defs);
    /* The module-QN strings are borrowed by the shared cross registries in
     * ctx->seq_cross_arena, which deliberately outlive this pass so that
     * pass_calls can read borrowed registry strings. Freeing the strings here
     * while keeping the registries alive was a use-after-free (pass_calls
     * strcmp on a freed module QN, caught by ASan on the kernel corpus tier).
     * Ownership transfers to the ctx; released beside the arena. */
    ctx->seq_cross_def_modules = def_modules;
    ctx->seq_cross_def_module_count = file_count;

    /* Drop the borrowed manifest pointer before its arena dies, so a later
     * pass (or a stale thread-local) can never read freed manifest memory. */
    if (have_rust) {
        cbm_pxc_set_rust_manifest(NULL);
        cbm_arena_destroy(&cargo_arena);
    }
    (void)have_manifest;

    cbm_log_info("pass.done", "pass", "lsp_cross", "files_processed", itoa_buf(processed),
                 "files_skipped_no_lsp", itoa_buf(skipped_no_lsp), "files_skipped_no_source",
                 itoa_buf(skipped_no_source), "defs_total", itoa_buf(def_count), "lsp_calls",
                 itoa_buf(per_lang_calls));
    return 0;
}

/* ── Per-module def index (gopls "package summary" pattern) ──── */

typedef struct {
    int count;
    int cap;
    int *indices; /* malloc'd; indices into the caller's all_defs[] */
} pxc_module_entry_t;

struct CBMModuleDefIndex {
    CBMHashTable *ht;           /* module_qn → pxc_module_entry_t* */
    CBMHashTable *namespace_ht; /* declared package/namespace → pxc_module_entry_t* */
    int def_count;              /* total entries in the all_defs[] array */
};

/* cbm_ht_foreach callback: free each pxc_module_entry_t. */
static void pxc_module_entry_free_cb(const char *key, void *value, void *userdata) {
    (void)key;
    (void)userdata;
    pxc_module_entry_t *e = (pxc_module_entry_t *)value;
    if (!e)
        return;
    free(e->indices);
    free(e);
}
static pxc_module_entry_t *pxc_module_entry_get_or_create(CBMHashTable *ht, const char *key) {
    if (!ht || !key || !key[0]) {
        return NULL;
    }
    pxc_module_entry_t *e = (pxc_module_entry_t *)cbm_ht_get(ht, key);
    if (e) {
        return e;
    }
    e = (pxc_module_entry_t *)calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    e->cap = 8;
    e->indices = (int *)calloc((size_t)e->cap, sizeof(*e->indices));
    if (!e->indices) {
        free(e);
        return NULL;
    }
    cbm_ht_set(ht, key, e);
    return e;
}

static void pxc_module_entry_add_index(pxc_module_entry_t *e, int index) {
    if (!e) {
        return;
    }
    if (e->count >= e->cap) {
        int new_cap = e->cap * 2;
        int *new_indices = (int *)realloc(e->indices, (size_t)new_cap * sizeof(*new_indices));
        if (!new_indices) {
            return;
        }
        e->indices = new_indices;
        e->cap = new_cap;
    }
    e->indices[e->count++] = index;
}

static bool pxc_def_lang_matches(CBMLanguage caller_lang, CBMLanguage def_lang);

static int pxc_mark_entry_defs(bool *selected, const pxc_module_entry_t *e,
                               const CBMLSPDef *all_defs, CBMLanguage caller_lang) {
    if (!selected || !e) {
        return 0;
    }
    int added = 0;
    for (int j = 0; j < e->count; j++) {
        int idx = e->indices[j];
        const CBMLSPDef *def = &all_defs[idx];
        if (!pxc_def_lang_matches(caller_lang, def->lang) || selected[idx]) {
            continue;
        }
        selected[idx] = true;
        added++;
    }
    return added;
}

static bool pxc_is_jvm_lang(CBMLanguage lang) {
    return lang == CBM_LANG_JAVA || lang == CBM_LANG_KOTLIN;
}

static bool pxc_def_lang_matches(CBMLanguage caller_lang, CBMLanguage def_lang) {
    if (pxc_is_jvm_lang(caller_lang)) {
        return pxc_is_jvm_lang(def_lang);
    }
    return true;
}

static void pxc_mark_module_defs(const CBMModuleDefIndex *idx, bool *selected,
                                 const CBMLSPDef *all_defs, CBMLanguage caller_lang,
                                 const char *module_qn, int *total) {
    if (!idx || !idx->ht || !module_qn || !module_qn[0]) {
        return;
    }
    pxc_module_entry_t *e = (pxc_module_entry_t *)cbm_ht_get(idx->ht, module_qn);
    int added = pxc_mark_entry_defs(selected, e, all_defs, caller_lang);
    if (total) {
        *total += added;
    }
}

CBMModuleDefIndex *cbm_pxc_build_module_def_index(CBMLSPDef *all_defs, int def_count) {
    if (!all_defs || def_count <= 0) {
        return NULL;
    }

    CBMHashTable *ht = cbm_ht_create(64);
    CBMHashTable *namespace_ht = cbm_ht_create(64);
    if (!ht || !namespace_ht) {
        cbm_ht_free(ht);
        cbm_ht_free(namespace_ht);
        return NULL;
    }

    /* Single pass: index each def by file module and by declared package.
     * JVM mixed roots (`src/main/java` + `src/main/kotlin`) share the
     * declared package, not the path-derived module prefix. */
    for (int i = 0; i < def_count; i++) {
        pxc_module_entry_add_index(pxc_module_entry_get_or_create(ht, all_defs[i].def_module_qn),
                                   i);
        pxc_module_entry_add_index(
            pxc_module_entry_get_or_create(namespace_ht, all_defs[i].namespace_name), i);
    }

    CBMModuleDefIndex *idx = (CBMModuleDefIndex *)calloc(1, sizeof(*idx));
    if (!idx) {
        cbm_ht_foreach(ht, pxc_module_entry_free_cb, NULL);
        cbm_ht_free(ht);
        cbm_ht_foreach(namespace_ht, pxc_module_entry_free_cb, NULL);
        cbm_ht_free(namespace_ht);
        return NULL;
    }
    idx->ht = ht;
    idx->namespace_ht = namespace_ht;
    idx->def_count = def_count;
    return idx;
}

void cbm_pxc_free_module_def_index(CBMModuleDefIndex *idx) {
    if (!idx) {
        return;
    }
    if (idx->ht) {
        cbm_ht_foreach(idx->ht, pxc_module_entry_free_cb, NULL);
        cbm_ht_free(idx->ht);
    }
    if (idx->namespace_ht) {
        cbm_ht_foreach(idx->namespace_ht, pxc_module_entry_free_cb, NULL);
        cbm_ht_free(idx->namespace_ht);
    }
    free(idx);
}

CBMLSPDef *cbm_pxc_filter_defs_for_file(const CBMModuleDefIndex *idx, CBMLSPDef *all_defs,
                                        CBMLanguage caller_lang, const char *caller_namespace,
                                        const char *own_module, const char *const *imp_qns,
                                        int imp_count, int *out_count) {
    if (out_count) {
        *out_count = 0;
    }
    if (!idx || !idx->ht || !all_defs || !out_count || idx->def_count <= 0) {
        return NULL;
    }

    bool *selected = (bool *)calloc((size_t)idx->def_count, sizeof(*selected));
    if (!selected) {
        return NULL;
    }

    int total = 0;
    pxc_mark_module_defs(idx, selected, all_defs, caller_lang, own_module, &total);
    for (int i = 0; i < imp_count; i++) {
        pxc_mark_module_defs(idx, selected, all_defs, caller_lang, imp_qns[i], &total);
    }
    if (pxc_is_jvm_lang(caller_lang) && caller_namespace && caller_namespace[0] &&
        idx->namespace_ht) {
        pxc_module_entry_t *e =
            (pxc_module_entry_t *)cbm_ht_get(idx->namespace_ht, caller_namespace);
        total += pxc_mark_entry_defs(selected, e, all_defs, caller_lang);
    }

    if (total == 0) {
        free(selected);
        return NULL;
    }

    CBMLSPDef *out = (CBMLSPDef *)malloc((size_t)total * sizeof(CBMLSPDef));
    if (!out) {
        free(selected);
        return NULL;
    }

    int n = 0;
    for (int i = 0; i < idx->def_count; i++) {
        if (selected[i]) {
            out[n++] = all_defs[i];
        }
    }
    *out_count = n;
    free(selected);
    return out;
}
