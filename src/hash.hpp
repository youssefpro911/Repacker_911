#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace rpk {

using Sha256 = std::array<std::uint8_t, 32>;

class Sha256Hasher {
public:
    Sha256Hasher();
    ~Sha256Hasher();
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;
    Sha256Hasher(Sha256Hasher&&) noexcept;
    Sha256Hasher& operator=(Sha256Hasher&&) noexcept;

    void update(const void* data, std::size_t size);
    Sha256 finish();

private:
    void* context_ = nullptr;
    bool finished_ = false;
};

Sha256 sha256_bytes(const void* data, std::size_t size);
Sha256 sha256_file(const std::filesystem::path& path);
std::string sha256_hex(const Sha256& hash);
bool constant_time_equal(const Sha256& a, const Sha256& b) noexcept;

} // namespace rpk
