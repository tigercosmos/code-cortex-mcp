/*
 * result_compact.cpp — copy a finished CBMFileResult into one exact-size arena.
 *
 * Why this exists: extraction writes every temporary into the result arena —
 * cbm_node_text copies, per-node QN sprintf, enclosing-QN strings — and
 * GROW_ARRAY leaves each previous generation of every record array dead behind
 * it. The maintainer's own profile found the index's RAM is ~95% retained
 * per-file extraction arenas, and the per-file LSP scratch arena (#1997, #2010)
 * only took the registries out of that; the dead array generations and the
 * node-text temporaries stayed, because the result owned the arena and the
 * pipeline holds every result until resolve ends.
 *
 * Compaction walks what is REACHABLE from the result, measures it, copies it
 * into a single block of exactly that size (strings interned by content within
 * the file), and hands the working arena back to this thread's pool.
 *
 * Three phases over one traversal:
 *   Count   — number of string references, to size the intern table
 *   Measure — bytes every allocation will take (aligned like the arena does)
 *   Copy    — the same allocations, for real, into the fresh arena
 * Measure and Copy issue identical allocation sequences, so the block fits
 * exactly. Every write lands on a copy of the result header, so any failure
 * leaves the caller's result untouched.
 *
 * The field lists below are destructured with structured bindings for the same
 * reason src/pipeline/result_snapshot.cpp does it: a new field on any of these
 * records is a COMPILE ERROR here until someone says what compaction should do
 * with it. That sibling walker serializes; this one relocates.
 */
#include "result_compact.h"

#include "foundation/constants.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {

enum { CR_ALIGN = 7, CR_MIN_TABLE = 64 };

/* An arena this big is not worth keeping mapped for the next file. */
constexpr size_t kWorkArenaKeepBytes = 4u * 1024u * 1024u;

enum class Phase { Count, Measure, Copy };

struct Slot {
    const char *src; /* NULL = empty slot */
    char *dst;       /* copy in the new arena; a booked marker during Measure */
    uint64_t hash;
    size_t len;
};

size_t cr_aligned(size_t n) {
    return (n + CR_ALIGN) & ~(size_t)CR_ALIGN;
}

uint64_t cr_hash(const char *s, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

template <class A, class T> void fields(A &archive, T &value);

struct Compactor {
    Phase phase = Phase::Count;
    size_t refs = 0;  /* Count: string references seen */
    size_t bytes = 0; /* Measure: total arena bytes */
    CBMArena *dst = nullptr;
    bool failed = false;
    Slot *slots = nullptr;
    size_t cap = 0; /* power of two */

    /* A raw allocation of `n` bytes in the new arena: Measure books it, Copy
     * makes it. Zero bytes is no allocation (the arena returns NULL too). */
    void *allocate(size_t n) {
        if (n == 0) {
            return nullptr;
        }
        if (phase == Phase::Measure) {
            if (n > SIZE_MAX - CR_ALIGN || cr_aligned(n) > SIZE_MAX - bytes) {
                failed = true;
                return nullptr;
            }
            bytes += cr_aligned(n);
            return nullptr;
        }
        if (phase == Phase::Copy) {
            void *p = cbm_arena_alloc(dst, n);
            if (!p) {
                failed = true;
            }
            return p;
        }
        return nullptr;
    }

    /* Find or insert the slot for s. The table is sized from Count at half
     * load and the same references come back in Copy, so it never fills;
     * returning NULL on a full table keeps the probe from spinning anyway. */
    Slot *slot_for(const char *s) {
        size_t len = std::strlen(s);
        uint64_t h = cr_hash(s, len);
        size_t mask = cap - SKIP_ONE;
        size_t i = (size_t)h & mask;
        for (size_t probes = 0; probes < cap; probes++) {
            Slot *slot = &slots[i];
            if (!slot->src) {
                slot->src = s;
                slot->hash = h;
                slot->len = len;
                slot->dst = nullptr;
                return slot;
            }
            if (slot->hash == h && slot->len == len &&
                (slot->src == s || std::memcmp(slot->src, s, len) == 0)) {
                return slot;
            }
            i = (i + SKIP_ONE) & mask;
        }
        return nullptr;
    }

    /* A string field: interned by content. Copy rewrites the field. */
    void string(const char *&field) {
        const char *s = field;
        if (!s) {
            return;
        }
        if (phase == Phase::Count) {
            refs++;
            return;
        }
        Slot *slot = slot_for(s);
        if (!slot) {
            failed = true;
            return;
        }
        if (phase == Phase::Measure) {
            if (!slot->dst) {
                slot->dst = const_cast<char *>(s); /* booked; cleared before Copy */
                if (slot->len > SIZE_MAX - SKIP_ONE) {
                    failed = true;
                    return;
                }
                (void)allocate(slot->len + SKIP_ONE);
            }
            return;
        }
        if (!slot->dst) {
            auto *copy = static_cast<char *>(allocate(slot->len + SKIP_ONE));
            if (!copy) {
                return;
            }
            std::memcpy(copy, slot->src, slot->len + SKIP_ONE);
            slot->dst = copy;
        }
        field = slot->dst;
    }

    /* A blob (fingerprint, retained source, a record array block): verbatim. */
    void blob(const void *&field, size_t n) {
        if (!field || n == 0) {
            return;
        }
        if (phase == Phase::Copy) {
            void *copy = allocate(n);
            if (!copy) {
                return;
            }
            std::memcpy(copy, field, n);
            field = copy;
            return;
        }
        (void)allocate(n);
    }

    /* A NULL-terminated list of strings: the pointer array plus each string. */
    void list(const char **&field) {
        const char **items = field;
        if (!items) {
            return;
        }
        size_t n = 0;
        while (items[n]) {
            if (n > SIZE_MAX / sizeof(char *) - PAIR_LEN) {
                failed = true;
                return;
            }
            n++;
        }
        const void *block = items;
        blob(block, (n + SKIP_ONE) * sizeof(char *));
        field = static_cast<const char **>(const_cast<void *>(block));
        const char **walk = field; /* the copy in Copy, the original otherwise */
        for (size_t i = 0; i < n && walk; i++) {
            string(walk[i]);
        }
    }

    template <class T> static void scalar(T &v) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        (void)v; /* scalars live in the record itself; nothing to relocate */
    }
    void value(const char *&s) {
        string(s);
    }
    void value(const char **&strings) {
        list(strings);
    }
    template <class T> void value(T &v) {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
            scalar(v);
        } else {
            fields(*this, v);
        }
    }
    template <class... T> void operator()(T &...v) {
        (value(v), ...);
    }

    /* A record array: copied at its exact count, then its fields are walked. */
    template <class T> void array(T *&items, int &count) {
        if (!items || count <= 0) {
            if (phase == Phase::Copy) {
                items = nullptr;
            }
            return;
        }
        if (static_cast<size_t>(count) > SIZE_MAX / sizeof(T)) {
            failed = true;
            return;
        }
        const void *block = items;
        blob(block, static_cast<size_t>(count) * sizeof(T));
        if (failed) {
            return;
        }
        items = static_cast<T *>(const_cast<void *>(block));
        for (int i = 0; i < count; ++i) {
            value(items[i]);
        }
    }
    /* Exact-count arrays: nothing may append into dead headroom afterwards. */
    template <class T> void records(T &v) {
        array(v.items, v.count);
        if (phase == Phase::Copy) {
            v.cap = v.count;
        }
    }
};

template <class A> void fields(A &a, CBMDefinition &v) {
    auto &[name, qualified_name, label, file_path, start_line, end_line, signature, return_type,
           receiver, docstring, parent_class, decorators, base_classes, param_names, param_types,
           return_types, route_path, route_method, complexity, cognitive, loop_count, loop_depth,
           is_recursive, param_count, max_access_depth, linear_scan_in_loop, alloc_in_loop,
           recursion_in_loop, unguarded_recursion, lines, fingerprint, fingerprint_k, is_exported,
           is_abstract, is_test, is_entry_point, structural_profile, body_tokens, declaration_key,
           definition_offset] = v;
    a(name, qualified_name, label, file_path, signature, return_type, receiver, docstring,
      parent_class, decorators, base_classes, param_names, param_types, return_types, route_path,
      route_method, structural_profile, body_tokens, declaration_key);
    a.array(fingerprint, fingerprint_k);
    a(start_line, end_line, complexity, cognitive, loop_count, loop_depth, is_recursive,
      param_count, max_access_depth, linear_scan_in_loop, alloc_in_loop, recursion_in_loop,
      unguarded_recursion, lines, is_exported, is_abstract, is_test, is_entry_point,
      definition_offset);
}
template <class A> void fields(A &a, CBMOverload &v) {
    auto &[byte_offset, qualified_name] = v;
    a(byte_offset, qualified_name);
}
template <class A> void fields(A &a, CBMCallArg &v) {
    auto &[expr, value, keyword, index] = v;
    a(expr, value, keyword, index);
}
template <class A> void fields(A &a, CBMCall &v) {
    auto &[callee_name, enclosing_func_qn, first_string_arg, second_arg_name, args, arg_count,
           loop_depth, branch_depth, start_line, source_byte, requires_typed_resolution, is_method,
           callee_is_locally_bound] = v;
    a(callee_name, enclosing_func_qn, first_string_arg, second_arg_name, loop_depth, branch_depth,
      start_line, source_byte, requires_typed_resolution, is_method, callee_is_locally_bound);
    a.array(args, arg_count);
}
template <class A> void fields(A &a, CBMImport &v) {
    auto &[local_name, module_path] = v;
    a(local_name, module_path);
}
template <class A> void fields(A &a, CBMUsage &v) {
    auto &[ref_name, enclosing_func_qn, is_member_access] = v;
    a(ref_name, enclosing_func_qn, is_member_access);
}
template <class A> void fields(A &a, CBMThrow &v) {
    auto &[exception_name, enclosing_func_qn] = v;
    a(exception_name, enclosing_func_qn);
}
template <class A> void fields(A &a, CBMReadWrite &v) {
    auto &[var_name, enclosing_func_qn, is_write, is_member_access] = v;
    a(var_name, enclosing_func_qn, is_write, is_member_access);
}
template <class A> void fields(A &a, CBMTypeRef &v) {
    auto &[type_name, enclosing_func_qn] = v;
    a(type_name, enclosing_func_qn);
}
template <class A> void fields(A &a, CBMEnvAccess &v) {
    auto &[env_key, enclosing_func_qn] = v;
    a(env_key, enclosing_func_qn);
}
template <class A> void fields(A &a, CBMTypeAssign &v) {
    auto &[var_name, type_name, enclosing_func_qn] = v;
    a(var_name, type_name, enclosing_func_qn);
}
template <class A> void fields(A &a, CBMStringRef &v) {
    auto &[value, enclosing_func_qn, key_path, kind] = v;
    a(value, enclosing_func_qn, key_path, kind);
}
template <class A> void fields(A &a, CBMInfraBinding &v) {
    auto &[source_name, target_url, broker] = v;
    a(source_name, target_url, broker);
}
template <class A> void fields(A &a, CBMChannel &v) {
    auto &[channel_name, transport, enclosing_func_qn, direction] = v;
    a(channel_name, transport, enclosing_func_qn, direction);
}
template <class A> void fields(A &a, CBMImplTrait &v) {
    auto &[trait_name, struct_name] = v;
    a(trait_name, struct_name);
}
template <class A> void fields(A &a, CBMResolvedCall &v) {
    auto &[caller_qn, callee_qn, strategy, confidence, reason, source_byte, binary_operator_line] =
        v;
    a(caller_qn, callee_qn, strategy, confidence, reason, source_byte, binary_operator_line);
}

/* The whole reachable graph. `arena` is the thing being replaced,
 * `cpp_operator_tracker` is a stack object of the extraction that just ended
 * (NULL by now), and `cached_tree` is tree-sitter's memory, never the
 * arena's — all three are carried across untouched. */
void walk(Compactor &a, CBMFileResult &v) {
    auto &[arena, defs, overloads, overload_count, calls, deferred_cpp_operator_count,
           pending_cpp_operator_count, cpp_operator_tracker, imports, usages, throws, rw, type_refs,
           env_accesses, type_assigns, impl_traits, resolved_calls, string_refs, infra_bindings,
           channels, module_qn, namespace_name, exports, constants, global_vars, macros, has_error,
           error_msg, parse_incomplete, parse_unusable, error_ranges, error_region_count,
           is_test_file, imports_count, cached_tree, lsp_skipped, walk_truncated, cached_lang,
           source, source_len] = v;
    (void)arena;
    (void)cpp_operator_tracker;
    (void)cached_tree;
    a(deferred_cpp_operator_count, pending_cpp_operator_count, has_error, parse_incomplete,
      parse_unusable, error_region_count, is_test_file, imports_count, lsp_skipped, walk_truncated,
      cached_lang, source_len);
    a.records(defs);
    a.records(calls);
    a.records(imports);
    a.records(usages);
    a.records(throws);
    a.records(rw);
    a.records(type_refs);
    a.records(env_accesses);
    a.records(type_assigns);
    a.records(impl_traits);
    a.records(resolved_calls);
    a.records(string_refs);
    a.records(infra_bindings);
    a.records(channels);
    a.array(overloads, overload_count);
    a(module_qn, namespace_name, error_msg, error_ranges);
    a(exports, constants, global_vars, macros);
    /* Source bytes the parallel pass retains are copied verbatim, NUL and all.
     * At the hook point (end of extraction) this is still NULL; the walk
     * handles it so compaction stays correct wherever it is called from. */
    const void *retained = source;
    a.blob(retained, source && source_len > 0 ? (size_t)source_len + SKIP_ONE : 0);
    source = static_cast<const char *>(retained);
}

size_t pow2_at_least(size_t n) {
    size_t cap = CR_MIN_TABLE;
    while (cap < n && cap <= SIZE_MAX / PAIR_LEN) {
        cap *= PAIR_LEN;
    }
    return cap;
}

/* ── Per-worker working arena ─────────────────────────────────────── */
thread_local CBMArena tl_work_arena;
thread_local bool tl_work_arena_live = false;

} // namespace

void cbm_work_arena_take(CBMArena *into) {
    if (!into) {
        return;
    }
    if (tl_work_arena_live) {
        *into = tl_work_arena;
        tl_work_arena_live = false;
        std::memset(&tl_work_arena, 0, sizeof(tl_work_arena));
        cbm_arena_rewind(into);
        return;
    }
    cbm_arena_init(into);
}

void cbm_work_arena_give(CBMArena *from) {
    if (!from || from->nblocks == 0) {
        return;
    }
    if (tl_work_arena_live || cbm_arena_capacity(from) > kWorkArenaKeepBytes) {
        cbm_arena_destroy(from);
        return;
    }
    tl_work_arena = *from;
    tl_work_arena_live = true;
    std::memset(from, 0, sizeof(*from));
}

void cbm_work_arena_release(void) {
    if (tl_work_arena_live) {
        cbm_arena_destroy(&tl_work_arena);
        tl_work_arena_live = false;
    }
}

void cbm_result_compact(CBMFileResult *result) {
    if (!result || result->arena.nblocks == 0 || result->cpp_operator_tracker) {
        return;
    }

    /* Every pointer rewrite lands on this copy of the header; the caller's
     * result is replaced only once everything has succeeded. */
    CBMFileResult tmp = *result;

    Compactor c;
    c.phase = Phase::Count;
    walk(c, tmp);
    if (c.failed || c.refs > SIZE_MAX / PAIR_LEN - CR_MIN_TABLE) {
        return;
    }

    c.cap = pow2_at_least(c.refs * PAIR_LEN + CR_MIN_TABLE);
    c.slots = static_cast<Slot *>(std::calloc(c.cap, sizeof(Slot)));
    if (!c.slots) {
        return;
    }

    c.phase = Phase::Measure;
    walk(c, tmp);
    for (size_t i = 0; i < c.cap; i++) {
        c.slots[i].dst = nullptr; /* Measure used dst as a booked marker */
    }
    if (c.failed) {
        std::free(c.slots);
        return;
    }

    CBMArena fresh;
    cbm_arena_init_exact(&fresh, c.bytes);
    if (fresh.nblocks == 0) {
        std::free(c.slots);
        return;
    }

    c.phase = Phase::Copy;
    c.dst = &fresh;
    walk(c, tmp);
    std::free(c.slots);
    if (c.failed) {
        cbm_arena_destroy(&fresh);
        return;
    }

    cbm_work_arena_give(&result->arena); /* kept for this thread's next file */
    tmp.arena = fresh;
    *result = tmp;
}
