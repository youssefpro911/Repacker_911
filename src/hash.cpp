#include "hash.hpp"

#include <fstream>
#include <memory>
#include <stdexcept>

#include <openssl/evp.h>

namespace rpk {

Sha256Hasher::Sha256Hasher() {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        throw std::runtime_error("Unable to allocate SHA-256 context");
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("Unable to initialize SHA-256");
    }
    context_ = ctx;
}

Sha256Hasher::~Sha256Hasher() {
    if (context_ != nullptr) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(context_));
    }
}

Sha256Hasher::Sha256Hasher(Sha256Hasher&& other) noexcept
    : context_(other.context_), finished_(other.finished_) {
    other.context_ = nullptr;
    other.finished_ = true;
}

Sha256Hasher& Sha256Hasher::operator=(Sha256Hasher&& other) noexcept {
    if (this != &other) {
        if (context_ != nullptr) {
            EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(context_));
        }
        context_ = other.context_;
        finished_ = other.finished_;
        other.context_ = nullptr;
        other.finished_ = true;
    }
    return *this;
}

void Sha256Hasher::update(const void* data, std::size_t size) {
    if (finished_ || context_ == nullptr) {
        throw std::logic_error("SHA-256 context is no longer active");
    }
    if (size != 0 && EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(context_), data, size) != 1) {
        throw std::runtime_error("Unable to update SHA-256");
    }
}

Sha256 Sha256Hasher::finish() {
    if (finished_ || context_ == nullptr) {
        throw std::logic_error("SHA-256 context was already finalized");
    }
    Sha256 result{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(context_), result.data(), &length) != 1 || length != result.size()) {
        throw std::runtime_error("Unable to finalize SHA-256");
    }
    finished_ = true;
    return result;
}

Sha256 sha256_bytes(const void* data, std::size_t size) {
    Sha256Hasher hasher;
    hasher.update(data, size);
    return hasher.finish();
}

Sha256 sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open file for hashing: " + path.string());
    }

    Sha256Hasher hasher;
    char buffer[1024 * 1024];
    while (input) {
        input.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            hasher.update(buffer, static_cast<std::size_t>(count));
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Read error while hashing: " + path.string());
    }
    return hasher.finish();
}

std::string sha256_hex(const Sha256& hash) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.resize(hash.size() * 2);
    for (std::size_t i = 0; i < hash.size(); ++i) {
        output[i * 2] = digits[(hash[i] >> 4U) & 0x0fU];
        output[i * 2 + 1] = digits[hash[i] & 0x0fU];
    }
    return output;
}

bool constant_time_equal(const Sha256& a, const Sha256& b) noexcept {
    unsigned int difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned int>(a[i] ^ b[i]);
    }
    return difference == 0;
}

} // namespace rpk
