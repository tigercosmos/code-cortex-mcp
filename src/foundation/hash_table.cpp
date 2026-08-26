/*
 * hash_table.cpp — CBMHashTable backed by std::unordered_map.
 *
 * Public API in hash_table.h is unchanged. The prior implementation used the
 * vendored Verstable open-addressing table; this is a pure-C++ port onto
 * std::unordered_map<const char*, void*> with content-based hashing and
 * comparison. Keys are BORROWED pointers — the table never copies or frees
 * them; the caller owns the key string for the lifetime of the entry.
 *
 * The const char* fast-path equality (a == b before strcmp) keeps the common
 * interned-key case cheap. cbm_ht_get remains a hot path (per-call registry
 * resolution); std::unordered_map is node-based rather than open-addressing,
 * so this trades some lookup locality for a dependency-free std-library impl.
 */
#include "hash_table.h"

#include <cstring>
#include <functional>
#include <string_view>
#include <unordered_map>

namespace {

/* Content-based hash/equality over borrowed C strings. */
struct CStrHash {
    size_t operator()(const char *s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};
struct CStrEq {
    bool operator()(const char *a, const char *b) const noexcept {
        return a == b || std::strcmp(a, b) == 0;
    }
};

using HashMap = std::unordered_map<const char *, void *, CStrHash, CStrEq>;

} // namespace

/* Opaque to callers; full definition lives here. */
struct CBMHashTable {
    HashMap map;
};

CBMHashTable *cbm_ht_create(uint32_t initial_capacity) {
    try {
        auto *ht = new CBMHashTable();
        if (initial_capacity > 0) {
            ht->map.reserve(initial_capacity);
        }
        return ht;
    } catch (...) {
        return nullptr;
    }
}

void cbm_ht_free(CBMHashTable *ht) {
    delete ht;
}

void *cbm_ht_set(CBMHashTable *ht, const char *key, void *value) {
    if (!ht || !key) {
        return nullptr;
    }
    try {
        auto it = ht->map.find(key);
        if (it != ht->map.end()) {
            void *prev = it->second;
            it->second = value; /* update value; keep the stored (borrowed) key */
            return prev;
        }
        ht->map.emplace(key, value);
        return nullptr; /* new key */
    } catch (...) {
        return nullptr; /* OOM: no-op insert (the C contract never throws) */
    }
}

void *cbm_ht_get(const CBMHashTable *ht, const char *key) {
    if (!ht || !key) {
        return nullptr;
    }
    auto it = ht->map.find(key);
    return it == ht->map.end() ? nullptr : it->second;
}

bool cbm_ht_has(const CBMHashTable *ht, const char *key) {
    if (!ht || !key) {
        return false;
    }
    return ht->map.find(key) != ht->map.end();
}

const char *cbm_ht_get_key(const CBMHashTable *ht, const char *key) {
    if (!ht || !key) {
        return nullptr;
    }
    auto it = ht->map.find(key);
    return it == ht->map.end() ? nullptr : it->first;
}

void *cbm_ht_delete(CBMHashTable *ht, const char *key) {
    if (!ht || !key) {
        return nullptr;
    }
    auto it = ht->map.find(key);
    if (it == ht->map.end()) {
        return nullptr;
    }
    void *prev = it->second;
    ht->map.erase(it);
    return prev;
}

uint32_t cbm_ht_count(const CBMHashTable *ht) {
    if (!ht) {
        return 0;
    }
    return static_cast<uint32_t>(ht->map.size());
}

void cbm_ht_foreach(const CBMHashTable *ht, cbm_ht_iter_fn fn, void *userdata) {
    if (!ht || !fn) {
        return;
    }
    for (const auto &kv : ht->map) {
        fn(kv.first, kv.second, userdata);
    }
}

void cbm_ht_clear(CBMHashTable *ht) {
    if (!ht) {
        return;
    }
    ht->map.clear();
}

size_t cbm_ht_memory_bytes(const CBMHashTable *ht) {
    if (!ht) {
        return 0;
    }
    /* libstdc++/libc++ both allocate one node per element (key, value, next)
     * and a bucket array of pointers. */
    const size_t node_bytes = sizeof(const char *) + sizeof(void *) + sizeof(void *);
    return (ht->map.bucket_count() * sizeof(void *)) + (ht->map.size() * node_bytes);
}
