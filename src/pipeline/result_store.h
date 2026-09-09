#pragma once
#include "result_snapshot.h"
#include <cstdio>
#include <cstdint>
#include <memory>

namespace cbm {
// Run-local temporary storage. Metadata and registries are outside lease bounds.
class ResultStore {
    struct State;

  public:
    struct Limits {
        size_t max_record_bytes;
        size_t max_arena_capacity;
        size_t max_live_leases;
        uint64_t max_disk_bytes;
    };
    class Lease {
      public:
        Lease() = default;
        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;
        ~Lease();
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        CBMFileResult *get() const {
            return result_;
        }
        explicit operator bool() const {
            return result_ != nullptr;
        }

      private:
        friend class ResultStore;
        Lease(std::shared_ptr<State> state, CBMFileResult *result);
        void release();
        std::shared_ptr<State> state_;
        CBMFileResult *result_ = nullptr;
    };
    struct Stats {
        size_t records;
        size_t live_leases;
        uint64_t published_bytes;
        bool writes_failed;
    };
    static std::unique_ptr<ResultStore> create(Limits limits, std::string &error);
    // Takes ownership even on failure. Stream must be a fresh, seekable binary
    // update stream, never shared with other readers/writers after adoption.
    static std::unique_ptr<ResultStore> adopt(FILE *stream, Limits limits, std::string &error);
    // Replacements publish only after a complete write and fflush. Not durable:
    // no fsync, no recovery after process exit. Failed writes disable more puts.
    bool put(uint64_t id, const CBMFileResult &result, std::string &error);
    // Slot exhaustion returns an error instead of blocking a caller holding a lease.
    // Mutations to a loaded result are private until an explicit put.
    Lease acquire(uint64_t id, std::string &error);
    Stats stats() const;

  private:
    explicit ResultStore(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};
} // namespace cbm
