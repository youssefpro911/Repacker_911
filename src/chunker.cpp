#include "chunker.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace rpk {
namespace {

std::uint64_t splitmix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

const std::array<std::uint64_t, 256>& gear_table() {
    static const std::array<std::uint64_t, 256> table = [] {
        std::array<std::uint64_t, 256> result{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = splitmix64(static_cast<std::uint64_t>(i) + 0x52504b434443ULL);
        }
        return result;
    }();
    return table;
}

bool is_power_of_two(std::size_t value) {
    return value != 0 && (value & (value - 1U)) == 0;
}

} // namespace

ContentDefinedChunker::ContentDefinedChunker(ChunkerOptions options) : options_(options) {
    if (options_.minimum == 0 || !is_power_of_two(options_.target) ||
        options_.minimum > options_.target || options_.target > options_.maximum ||
        options_.maximum > 64U * 1024U * 1024U) {
        throw std::invalid_argument("Invalid content-defined chunker bounds");
    }
    pending_.reserve(options_.maximum);
}

void ContentDefinedChunker::push(const std::uint8_t* data,
                                 std::size_t size,
                                 const ChunkCallback& callback) {
    if (size != 0 && data == nullptr) throw std::invalid_argument("Chunker received a null data pointer");
    const auto& gear = gear_table();
    const std::uint64_t boundary_mask = static_cast<std::uint64_t>(options_.target - 1U);
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t value = data[i];
        pending_.push_back(value);
        rolling_hash_ = (rolling_hash_ << 1U) + gear[value];
        if (pending_.size() >= options_.minimum &&
            (((rolling_hash_ & boundary_mask) == 0) || pending_.size() >= options_.maximum)) {
            emit(callback);
        }
    }
}

void ContentDefinedChunker::finish(const ChunkCallback& callback) {
    if (!pending_.empty()) emit(callback);
}

void ContentDefinedChunker::reset() {
    pending_.clear();
    rolling_hash_ = 0;
}

void ContentDefinedChunker::emit(const ChunkCallback& callback) {
    if (pending_.empty()) return;
    callback(std::move(pending_));
    pending_ = std::vector<std::uint8_t>();
    pending_.reserve(options_.maximum);
    rolling_hash_ = 0;
}

} // namespace rpk
