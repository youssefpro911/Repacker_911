#include "precomp.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <zlib.h>

namespace rpk {
namespace {

bool plausible_zlib_header(const std::vector<std::uint8_t>& input, std::size_t offset) {
    if (offset + 2 > input.size()) return false;
    const unsigned int cmf = input[offset];
    const unsigned int flg = input[offset + 1];
    if ((cmf & 0x0fU) != 8U || (cmf >> 4U) > 7U) return false;
    return (((cmf << 8U) | flg) % 31U) == 0U;
}

bool inflate_zlib_candidate(const std::vector<std::uint8_t>& input,
                            std::size_t offset,
                            std::uint64_t maximum_output,
                            std::vector<std::uint8_t>& raw,
                            std::size_t& encoded_size) {
    if (input.size() - offset > std::numeric_limits<uInt>::max()) return false;
    z_stream stream{};
    if (inflateInit2(&stream, MAX_WBITS) != Z_OK) throw std::runtime_error("Unable to initialize zlib inflater");

    const std::size_t available = std::min<std::size_t>(input.size() - offset, 16U * 1024U * 1024U);
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data() + offset));
    stream.avail_in = static_cast<uInt>(available);
    std::array<std::uint8_t, 64U * 1024U> out{};
    raw.clear();
    int result = Z_OK;
    try {
        while (result == Z_OK) {
            stream.next_out = out.data();
            stream.avail_out = static_cast<uInt>(out.size());
            result = inflate(&stream, Z_NO_FLUSH);
            const std::size_t produced = out.size() - stream.avail_out;
            if (produced > maximum_output - raw.size()) {
                inflateEnd(&stream);
                raw.clear();
                return false;
            }
            raw.insert(raw.end(), out.data(), out.data() + produced);
            if (result == Z_BUF_ERROR || (result == Z_OK && stream.avail_in == 0 && produced == 0)) break;
        }
        encoded_size = static_cast<std::size_t>(stream.total_in);
    } catch (...) {
        inflateEnd(&stream);
        throw;
    }
    inflateEnd(&stream);
    if (result != Z_STREAM_END || encoded_size < 6 || raw.empty()) {
        raw.clear();
        return false;
    }
    return true;
}

bool deflate_zlib(const std::vector<std::uint8_t>& raw, int level, std::vector<std::uint8_t>& encoded) {
    if (raw.size() > std::numeric_limits<uInt>::max()) return false;
    z_stream stream{};
    if (deflateInit2(&stream, level, Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("Unable to initialize zlib deflater");
    }
    const uLong bound = deflateBound(&stream, static_cast<uLong>(raw.size()));
    if (bound > std::numeric_limits<std::size_t>::max()) {
        deflateEnd(&stream);
        throw std::runtime_error("zlib output size exceeds addressable memory");
    }
    encoded.resize(static_cast<std::size_t>(bound));
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(raw.data()));
    stream.avail_in = static_cast<uInt>(raw.size());
    stream.next_out = encoded.data();
    stream.avail_out = static_cast<uInt>(std::min<std::size_t>(encoded.size(), std::numeric_limits<uInt>::max()));
    const int result = deflate(&stream, Z_FINISH);
    const bool ok = result == Z_STREAM_END;
    const std::size_t produced = static_cast<std::size_t>(stream.total_out);
    deflateEnd(&stream);
    if (!ok) {
        encoded.clear();
        return false;
    }
    encoded.resize(produced);
    return true;
}

} // namespace

PrecompResult precompress_zlib_streams(const std::vector<std::uint8_t>& input,
                                       std::uint64_t maximum_expanded_bytes) {
    PrecompResult result;
    result.transformed.reserve(input.size());
    std::vector<std::uint8_t> raw;
    std::vector<std::uint8_t> encoded;
    std::size_t source_cursor = 0;

    std::uint32_t candidate_attempts = 0;
    std::uint64_t expanded_total = 0;
    for (std::size_t candidate = 0; candidate + 6 <= input.size();) {
        if (!plausible_zlib_header(input, candidate)) {
            ++candidate;
            continue;
        }
        if (++candidate_attempts > 8U || expanded_total >= maximum_expanded_bytes) break;
        std::size_t encoded_size = 0;
        const std::uint64_t remaining_budget = maximum_expanded_bytes - expanded_total;
        const std::uint64_t candidate_limit = std::min<std::uint64_t>(remaining_budget, 16ULL * 1024ULL * 1024ULL);
        if (!inflate_zlib_candidate(input, candidate, candidate_limit, raw, encoded_size)) {
            ++candidate;
            if (result.rejected_candidates != std::numeric_limits<std::uint32_t>::max()) ++result.rejected_candidates;
            continue;
        }

        expanded_total += raw.size();
        int matching_level = -1;
        for (int level = 0; level <= 9; ++level) {
            if (!deflate_zlib(raw, level, encoded)) continue;
            if (encoded.size() == encoded_size &&
                std::equal(encoded.begin(), encoded.end(), input.begin() + static_cast<std::ptrdiff_t>(candidate))) {
                matching_level = level;
                break;
            }
        }
        if (matching_level < 0) {
            candidate += encoded_size;
            if (result.rejected_candidates != std::numeric_limits<std::uint32_t>::max()) ++result.rejected_candidates;
            continue;
        }

        result.transformed.insert(result.transformed.end(),
                                  input.begin() + static_cast<std::ptrdiff_t>(source_cursor),
                                  input.begin() + static_cast<std::ptrdiff_t>(candidate));
        PrecompRecord record;
        record.transformed_offset = result.transformed.size();
        record.raw_size = raw.size();
        record.encoded_size = encoded_size;
        record.level = static_cast<std::uint32_t>(matching_level);
        record.encoded_checksum = sha256_bytes(input.data() + candidate, encoded_size);
        result.transformed.insert(result.transformed.end(), raw.begin(), raw.end());
        result.records.push_back(record);

        source_cursor = candidate + encoded_size;
        candidate = source_cursor;
    }

    result.transformed.insert(result.transformed.end(),
                              input.begin() + static_cast<std::ptrdiff_t>(source_cursor), input.end());
    if (result.records.empty()) result.transformed = input;
    return result;
}

std::vector<std::uint8_t> rebuild_zlib_streams(const std::vector<std::uint8_t>& transformed,
                                              const std::vector<PrecompRecord>& records) {
    std::vector<std::uint8_t> output;
    std::uint64_t cursor = 0;
    std::vector<std::uint8_t> raw;
    std::vector<std::uint8_t> encoded;
    for (const auto& record : records) {
        if (record.transformed_offset < cursor ||
            record.transformed_offset > transformed.size() ||
            record.raw_size > transformed.size() - record.transformed_offset ||
            record.level > 9) {
            throw std::runtime_error("Invalid zlib precompression record");
        }
        output.insert(output.end(), transformed.begin() + static_cast<std::ptrdiff_t>(cursor),
                      transformed.begin() + static_cast<std::ptrdiff_t>(record.transformed_offset));
        const auto begin = transformed.begin() + static_cast<std::ptrdiff_t>(record.transformed_offset);
        raw.assign(begin, begin + static_cast<std::ptrdiff_t>(record.raw_size));
        if (!deflate_zlib(raw, static_cast<int>(record.level), encoded)) {
            throw std::runtime_error("Unable to rebuild an embedded zlib stream");
        }
        if (encoded.size() != record.encoded_size ||
            !constant_time_equal(sha256_bytes(encoded.data(), encoded.size()), record.encoded_checksum)) {
            throw std::runtime_error("Embedded zlib stream cannot be reproduced exactly with this zlib runtime");
        }
        output.insert(output.end(), encoded.begin(), encoded.end());
        cursor = record.transformed_offset + record.raw_size;
    }
    output.insert(output.end(), transformed.begin() + static_cast<std::ptrdiff_t>(cursor), transformed.end());
    return output;
}

std::string precomp_zlib_version() {
    return zlibVersion();
}

} // namespace rpk
