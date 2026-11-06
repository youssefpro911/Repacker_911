#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace rpk {

struct ChunkerOptions {
    std::size_t minimum = 64U * 1024U;
    std::size_t target = 256U * 1024U;
    std::size_t maximum = 1024U * 1024U;
};

class ContentDefinedChunker {
public:
    using ChunkCallback = std::function<void(std::vector<std::uint8_t>&&)>;

    explicit ContentDefinedChunker(ChunkerOptions options = {});
    void push(const std::uint8_t* data, std::size_t size, const ChunkCallback& callback);
    void finish(const ChunkCallback& callback);
    void reset();

private:
    void emit(const ChunkCallback& callback);

    ChunkerOptions options_;
    std::uint64_t rolling_hash_ = 0;
    std::vector<std::uint8_t> pending_;
};

} // namespace rpk
