#pragma once
#include "cbm.h"
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace cbm {
// Private, versioned, same-build temporary records; not a persistent index format.
// Trees and active extraction trackers must be released before encoding.
// On failure, encode clears output and decode returns nullptr with an error.
// Array capacities normalize to their counts; allocation layout is not retained.
// max_bytes bounds encoded size and each decoded payload request.
// max_arena_capacity additionally bounds decoded arena block/buffer capacity.
// Neither limit includes the result struct, allocator overhead, or process RSS.
bool encode_result_snapshot(const CBMFileResult &result, std::vector<std::byte> &output,
                            size_t max_bytes, std::string &error);
CBMFileResult *decode_result_snapshot(std::span<const std::byte> input, size_t max_bytes,
                                      std::string &error, size_t max_arena_capacity = SIZE_MAX);
// Owned projection for parallel registry and infrastructure passes only.
// Full resolution must use the stored complete result, never this projection.
CBMFileResult *make_registry_summary(const CBMFileResult &result, size_t max_bytes,
                                     std::string &error);
} // namespace cbm
