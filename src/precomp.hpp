#pragma once

#include "hash.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rpk {

struct PrecompRecord {
    std::uint64_t transformed_offset = 0;
    std::uint64_t raw_size = 0;
    std::uint64_t encoded_size = 0;
    std::uint32_t level = 6;
    Sha256 encoded_checksum{};
};

struct PrecompResult {
    std::vector<std::uint8_t> transformed;
    std::vector<PrecompRecord> records;
    std::uint32_t rejected_candidates = 0;
};

// Only RFC 1950 zlib-wrapped DEFLATE streams that can be re-encoded byte-for-byte
// by the current zlib runtime are transformed. Everything else is kept verbatim.
PrecompResult precompress_zlib_streams(const std::vector<std::uint8_t>& input,
                                      std::uint64_t maximum_expanded_bytes = 128ULL * 1024ULL * 1024ULL);

std::vector<std::uint8_t> rebuild_zlib_streams(const std::vector<std::uint8_t>& transformed,
                                               const std::vector<PrecompRecord>& records);

std::string precomp_zlib_version();

} // namespace rpk
