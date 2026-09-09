#include "result_store.h"
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace cbm {
namespace {
bool seek(FILE *stream, uint64_t offset) {
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return false;
#ifdef _WIN32
    return _fseeki64(stream, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        return false;
    return fseeko(stream, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
} // namespace
struct ResultStore::State {
    struct FileCloser {
        void operator()(FILE *stream) const {
            if (stream)
                std::fclose(stream);
        }
    };
    using File = std::unique_ptr<FILE, FileCloser>;
    struct Entry {
        uint64_t offset;
        size_t bytes;
    };
    File stream;
    Limits limits;
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, Entry> entries;
    size_t live = 0;
    uint64_t end = 0;
    bool writes_failed = false;
    State(File file, Limits value) : stream(std::move(file)), limits(value) {}
};

ResultStore::Lease::Lease(std::shared_ptr<State> state, CBMFileResult *result)
    : state_(std::move(state)), result_(result) {}
ResultStore::Lease::Lease(Lease &&other) noexcept
    : state_(std::move(other.state_)), result_(std::exchange(other.result_, nullptr)) {}
ResultStore::Lease &ResultStore::Lease::operator=(Lease &&other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        result_ = std::exchange(other.result_, nullptr);
    }
    return *this;
}
void ResultStore::Lease::release() {
    if (!result_)
        return;
    cbm_free_result(result_);
    result_ = nullptr;
    {
        std::lock_guard lock(state_->mutex);
        --state_->live;
    }
    state_.reset();
}
ResultStore::Lease::~Lease() {
    release();
}

std::unique_ptr<ResultStore> ResultStore::create(Limits limits, std::string &error) {
    return adopt(std::tmpfile(), limits, error);
}
std::unique_ptr<ResultStore> ResultStore::adopt(FILE *stream, Limits limits, std::string &error) {
    error.clear();
    State::File owned(stream);
    try {
        if (!stream) {
            error = "cannot create temporary result store";
            return {};
        }
        if (limits.max_record_bytes < 16 ||
            limits.max_arena_capacity < CBM_ARENA_DEFAULT_BLOCK_SIZE ||
            limits.max_live_leases == 0 || limits.max_disk_bytes == 0 ||
            limits.max_disk_bytes > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            error = "invalid result store limits";
            return {};
        }
        if (!seek(stream, 0)) {
            error = "result store is not seekable";
            return {};
        }
        return std::unique_ptr<ResultStore>(
            new ResultStore(std::make_shared<State>(std::move(owned), limits)));
    } catch (const std::exception &e) {
        error = e.what();
        return {};
    }
}

bool ResultStore::put(uint64_t id, const CBMFileResult &result, std::string &error) {
    error.clear();
    std::lock_guard lock(state_->mutex);
    auto &s = *state_;
    if (s.writes_failed) {
        error = "result store writes disabled after I/O failure";
        return false;
    }
    try {
        std::vector<std::byte> encoded;
        if (!encode_result_snapshot(result, encoded, s.limits.max_record_bytes, error))
            return false;
        if (encoded.size() > s.limits.max_disk_bytes - s.end) {
            error = "result store disk limit";
            return false;
        }
        // Allocate map storage before writing, so allocation failure cannot occur
        // between successful I/O and publication. Keep previous entries intact.
        auto [slot, inserted] = s.entries.try_emplace(id, State::Entry{0, 0});
        const uint64_t offset = s.end;
        FILE *file = s.stream.get();
        std::clearerr(file);
        if (!seek(file, offset) ||
            std::fwrite(encoded.data(), 1, encoded.size(), file) != encoded.size() ||
            std::fflush(file) != 0) {
            if (inserted)
                s.entries.erase(slot);
            s.writes_failed = true;
            error = "cannot publish complete result snapshot";
            return false;
        }
        slot->second = {offset, encoded.size()};
        s.end += encoded.size();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

ResultStore::Lease ResultStore::acquire(uint64_t id, std::string &error) {
    error.clear();
    std::lock_guard lock(state_->mutex);
    auto &s = *state_;
    auto entry = s.entries.find(id);
    if (entry == s.entries.end()) {
        error = "result snapshot not found";
        return {};
    }
    if (s.live >= s.limits.max_live_leases) {
        error = "result store lease limit";
        return {};
    }
    try {
        std::vector<std::byte> encoded(entry->second.bytes);
        FILE *file = s.stream.get();
        std::clearerr(file);
        if (!seek(file, entry->second.offset) ||
            std::fread(encoded.data(), 1, encoded.size(), file) != encoded.size()) {
            error = "cannot read complete result snapshot";
            return {};
        }
        auto *result = decode_result_snapshot(encoded, s.limits.max_record_bytes, error,
                                              s.limits.max_arena_capacity);
        if (!result)
            return {};
        ++s.live;
        return Lease(state_, result);
    } catch (const std::exception &e) {
        error = e.what();
        return {};
    }
}
ResultStore::Stats ResultStore::stats() const {
    std::lock_guard lock(state_->mutex);
    return {state_->entries.size(), state_->live, state_->end, state_->writes_failed};
}
} // namespace cbm
