#include "result_snapshot.h"
#include <cstring>
#include <cstdlib>
#include <limits>
#include <climits>
#include <stdexcept>
#include <type_traits>

namespace cbm {
namespace {
struct SnapshotError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
template <class A, class T> void fields(A &archive, T &value);

struct Writer {
    std::vector<std::byte> &data;
    size_t limit;
    void bytes(const void *p, size_t n) {
        if (n > limit - data.size())
            throw SnapshotError("snapshot byte limit");
        auto *b = static_cast<const std::byte *>(p);
        data.insert(data.end(), b, b + n);
    }
    template <class T> void scalar(T &v) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        if constexpr (std::is_same_v<T, bool>) {
            unsigned char b = v ? 1 : 0;
            bytes(&b, 1);
        } else
            bytes(&v, sizeof(v));
    }
    void value(const char *&s) {
        uint64_t n = s ? std::strlen(s) : UINT64_MAX;
        scalar(n);
        if (s)
            bytes(s, static_cast<size_t>(n));
    }
    void value(const char **&strings) {
        int count = -1;
        if (strings) {
            count = 0;
            while (strings[count]) {
                if (static_cast<size_t>(count) >= limit / sizeof(uint64_t) || count == INT_MAX)
                    throw SnapshotError("snapshot string list limit");
                ++count;
            }
        }
        scalar(count);
        for (int i = 0; i < count; ++i)
            value(strings[i]);
    }
    template <class T> void value(T &v) {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            scalar(v);
        else
            fields(*this, v);
    }
    template <class... T> void operator()(T &...v) {
        (value(v), ...);
    }
    template <class T> void array(T *&items, int &count) {
        if (count < 0 || (count && !items) || static_cast<size_t>(count) > limit / sizeof(T))
            throw SnapshotError("invalid snapshot array");
        scalar(count);
        for (int i = 0; i < count; ++i)
            value(items[i]);
    }
    template <class T> void records(T &v) {
        array(v.items, v.count);
    }
};

struct Reader {
    std::span<const std::byte> data;
    CBMArena &arena;
    size_t limit;
    size_t arena_capacity_limit;
    void bytes(void *p, size_t n) {
        if (n > data.size())
            throw SnapshotError("truncated snapshot");
        std::memcpy(p, data.data(), n);
        data = data.subspan(n);
    }
    void *allocate(size_t n) {
        if (n > limit || n > SIZE_MAX - 7)
            throw SnapshotError("snapshot allocation limit");
        void *p = cbm_arena_alloc_bounded(&arena, n, arena_capacity_limit);
        if (!p)
            throw SnapshotError("snapshot allocation failed");
        std::memset(p, 0, n);
        return p;
    }
    template <class T> void scalar(T &v) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        if constexpr (std::is_same_v<T, bool>) {
            unsigned char b;
            bytes(&b, 1);
            if (b > 1)
                throw SnapshotError("invalid snapshot boolean");
            v = b != 0;
        } else
            bytes(&v, sizeof(v));
    }
    void value(const char *&s) {
        uint64_t n;
        scalar(n);
        if (n == UINT64_MAX) {
            s = nullptr;
            return;
        }
        if (n >= SIZE_MAX || n > data.size())
            throw SnapshotError("invalid snapshot string");
        auto *p = static_cast<char *>(allocate(static_cast<size_t>(n) + 1));
        bytes(p, static_cast<size_t>(n));
        s = p;
        if (std::memchr(p, 0, static_cast<size_t>(n)))
            throw SnapshotError("embedded NUL in snapshot string");
    }
    void value(const char **&strings) {
        int count;
        scalar(count);
        if (count == -1) {
            strings = nullptr;
            return;
        }
        if (count < 0 || static_cast<size_t>(count) > data.size() / sizeof(uint64_t) ||
            static_cast<size_t>(count) >= limit / sizeof(char *))
            throw SnapshotError("invalid snapshot string list");
        strings =
            static_cast<const char **>(allocate((static_cast<size_t>(count) + 1) * sizeof(char *)));
        for (int i = 0; i < count; ++i) {
            value(strings[i]);
            if (!strings[i])
                throw SnapshotError("null snapshot list item");
        }
    }
    template <class T> void value(T &v) {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            scalar(v);
        else
            fields(*this, v);
    }
    template <class... T> void operator()(T &...v) {
        (value(v), ...);
    }
    template <class T> void array(T *&items, int &count) {
        scalar(count);
        if (count < 0 || static_cast<size_t>(count) > data.size() ||
            static_cast<size_t>(count) > limit / sizeof(T))
            throw SnapshotError("invalid snapshot array");
        items =
            count ? static_cast<T *>(allocate(static_cast<size_t>(count) * sizeof(T))) : nullptr;
        for (int i = 0; i < count; ++i)
            value(items[i]);
    }
    template <class T> void records(T &v) {
        array(v.items, v.count);
        v.cap = v.count;
    }
};

// Structured bindings make additions to these records a compile error until
// the snapshot schema explicitly handles the new field.
template <class A> void fields(A &a, CBMDefinition &v) {
    auto &[name, qualified_name, label, file_path, start_line, end_line, signature, return_type,
           receiver, docstring, parent_class, decorators, base_classes, param_names, param_types,
           return_types, route_path, route_method, complexity, cognitive, loop_count, loop_depth,
           is_recursive, param_count, max_access_depth, linear_scan_in_loop, alloc_in_loop,
           recursion_in_loop, unguarded_recursion, lines, fingerprint, fingerprint_k, is_exported,
           is_abstract, is_test, is_entry_point, structural_profile, body_tokens, declaration_key,
           definition_offset] = v;
    a(name, qualified_name, label, file_path, start_line, end_line, signature, return_type,
      receiver, docstring, parent_class, decorators, base_classes, param_names, param_types,
      return_types, route_path, route_method, complexity, cognitive, loop_count, loop_depth,
      is_recursive, param_count, max_access_depth, linear_scan_in_loop, alloc_in_loop,
      recursion_in_loop, unguarded_recursion, lines, is_exported, is_abstract, is_test,
      is_entry_point, structural_profile, body_tokens, declaration_key, definition_offset);
    a.array(fingerprint, fingerprint_k);
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
           loop_depth, branch_depth, start_line, source_byte, requires_typed_resolution,
           is_method] = v;
    a(callee_name, enclosing_func_qn, first_string_arg, second_arg_name, loop_depth, branch_depth,
      start_line, source_byte, requires_typed_resolution, is_method);
    a.array(args, arg_count);
}
template <class A> void fields(A &a, CBMImport &v) {
    auto &[local_name, module_path] = v;
    a(local_name, module_path);
}
template <class A> void fields(A &a, CBMUsage &v) {
    auto &[ref_name, enclosing_func_qn] = v;
    a(ref_name, enclosing_func_qn);
}
template <class A> void fields(A &a, CBMThrow &v) {
    auto &[exception_name, enclosing_func_qn] = v;
    a(exception_name, enclosing_func_qn);
}
template <class A> void fields(A &a, CBMReadWrite &v) {
    auto &[var_name, enclosing_func_qn, is_write] = v;
    a(var_name, enclosing_func_qn, is_write);
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
template <class A> void fields(A &a, CBMFileResult &v) {
    auto &[arena, defs, overloads, overload_count, calls, deferred_cpp_operator_count,
           pending_cpp_operator_count, cpp_operator_tracker, imports, usages, throws, rw, type_refs,
           env_accesses, type_assigns, impl_traits, resolved_calls, string_refs, infra_bindings,
           channels, module_qn, namespace_name, exports, constants, global_vars, macros, has_error,
           error_msg, parse_incomplete, error_ranges, error_region_count, is_test_file,
           imports_count, cached_tree, cached_lang, source, source_len] = v;
    a(deferred_cpp_operator_count, pending_cpp_operator_count, module_qn, namespace_name, exports,
      constants, global_vars, macros, has_error, error_msg, parse_incomplete, error_ranges,
      error_region_count, is_test_file, imports_count, cached_lang);
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
    a(source_len);
    if (source_len < 0)
        throw SnapshotError("invalid snapshot source length");
    bool has_source = source != nullptr;
    a(has_source);
    if constexpr (std::is_same_v<A, Reader>) {
        source = nullptr;
        if (has_source) {
            if (static_cast<size_t>(source_len) > a.data.size())
                throw SnapshotError("truncated snapshot source");
            auto *p = static_cast<char *>(a.allocate(static_cast<size_t>(source_len) + 1));
            a.bytes(p, static_cast<size_t>(source_len));
            source = p;
        }
    } else if (has_source)
        a.bytes(source, static_cast<size_t>(source_len));
    if (!has_source && source_len)
        throw SnapshotError("missing snapshot source");
}
uint64_t checksum(std::span<const std::byte> bytes) {
    uint64_t hash = 14695981039346656037ULL;
    for (std::byte b : bytes) {
        hash ^= std::to_integer<unsigned char>(b);
        hash *= 1099511628211ULL;
    }
    return hash;
}
constexpr uint64_t magic = 0x3150414e534d4243ULL; // CBMSNAP1, native scalar representation.
} // namespace

bool encode_result_snapshot(const CBMFileResult &result, std::vector<std::byte> &output,
                            size_t max_bytes, std::string &error) {
    output.clear();
    error.clear();
    try {
        if (result.cached_tree || result.cpp_operator_tracker)
            throw SnapshotError("release tree and extraction tracker before snapshot");
        Writer a{output, max_bytes};
        auto tag = magic;
        a(tag);
        auto copy = result;
        fields(a, copy);
        uint64_t digest = checksum(output);
        a(digest);
        return true;
    } catch (const std::exception &e) {
        output.clear();
        error = e.what();
        return false;
    }
}

static CBMFileResult *decode_impl(std::span<const std::byte> input, size_t max_bytes,
                                  std::string &error, size_t max_arena_capacity,
                                  size_t initial_capacity) {
    error.clear();
    CBMFileResult *result = nullptr;
    try {
        if (input.size() > max_bytes)
            throw SnapshotError("snapshot byte limit");
        if (input.size() < 2 * sizeof(uint64_t))
            throw SnapshotError("truncated snapshot");
        uint64_t digest;
        std::memcpy(&digest, input.data() + input.size() - sizeof(digest), sizeof(digest));
        input = input.first(input.size() - sizeof(digest));
        if (checksum(input) != digest)
            throw SnapshotError("snapshot checksum mismatch");
        result = static_cast<CBMFileResult *>(std::calloc(1, sizeof(*result)));
        if (!result)
            throw SnapshotError("snapshot allocation failed");
        if (max_arena_capacity < initial_capacity)
            throw SnapshotError("snapshot arena capacity limit");
        cbm_arena_init_sized(&result->arena, initial_capacity);
        Reader a{input, result->arena, max_bytes, max_arena_capacity};
        uint64_t tag;
        a(tag);
        if (tag != magic)
            throw SnapshotError("snapshot version mismatch");
        fields(a, *result);
        if (!a.data.empty())
            throw SnapshotError("trailing snapshot bytes");
        return result;
    } catch (const std::exception &e) {
        if (result)
            cbm_free_result(result);
        error = e.what();
        return nullptr;
    }
}
CBMFileResult *decode_result_snapshot(std::span<const std::byte> input, size_t max_bytes,
                                      std::string &error, size_t max_arena_capacity) {
    return decode_impl(input, max_bytes, error, max_arena_capacity, CBM_ARENA_DEFAULT_BLOCK_SIZE);
}

CBMFileResult *make_registry_summary(const CBMFileResult &result, size_t max_bytes,
                                     std::string &error) {
    try {
        CBMFileResult summary = {};
        std::vector<CBMDefinition> definitions;
        definitions.reserve(result.defs.count);
        for (int i = 0; i < result.defs.count; ++i) {
            const auto &src = result.defs.items[i];
            CBMDefinition def = {};
            def.name = src.name;
            def.qualified_name = src.qualified_name;
            def.label = src.label;
            def.parent_class = src.parent_class;
            def.return_type = src.return_type;
            def.declaration_key = src.declaration_key;
            def.param_types = src.param_types;
            def.base_classes = src.base_classes;
            definitions.push_back(def);
        }
        summary.defs = {definitions.data(), result.defs.count, result.defs.count};
        summary.module_qn = result.module_qn;
        summary.namespace_name = result.namespace_name;
        summary.imports = result.imports;
        summary.channels = result.channels;
        summary.env_accesses = result.env_accesses;
        summary.infra_bindings = result.infra_bindings;
        summary.string_refs = result.string_refs;
        std::vector<std::byte> bytes;
        if (!encode_result_snapshot(summary, bytes, max_bytes, error))
            return nullptr;
        return decode_impl(bytes, max_bytes, error, max_bytes, 4096);
    } catch (const std::exception &e) {
        error = e.what();
        return nullptr;
    }
}
} // namespace cbm
