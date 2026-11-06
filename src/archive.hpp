#pragma once

#include "hash.hpp"
#include "precomp.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rpk {

enum class Codec : std::uint32_t {
    Lzma2 = 1,
    ZstdLong = 2,
    Store = 3
};

enum class CodecMode : std::uint32_t {
    Auto = 0,
    Lzma2 = 1,
    ZstdLong = 2,
    Store = 3
};

struct PackOptions {
    std::uint32_t preset = 6;
    std::uint32_t dictionary_mib = 64;
    std::uint64_t block_size = 64ULL * 1024ULL * 1024ULL;
    std::uint32_t threads = 0; // 0 = automatically select a safe count
    std::uint32_t memory_limit_mib = 1024;
    CodecMode codec = CodecMode::Auto;
    bool overwrite = false;
    bool deduplicate = true;
    bool precompress = true;
};

struct Extent {
    std::uint32_t block_index = 0;
    std::uint64_t block_offset = 0;
    std::uint64_t file_offset = 0;
    std::uint64_t length = 0;
};

struct FileRecord {
    std::string path;
    std::uint64_t size = 0;
    std::uint64_t stored_size = 0;
    std::vector<PrecompRecord> precomp;
    bool read_only = false;
    Sha256 checksum{};
    std::vector<Extent> extents;
};

struct BlockRecord {
    Codec codec = Codec::Lzma2;
    std::uint64_t payload_offset = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    Sha256 checksum{};
};

struct ArchiveInfo {
    std::uint32_t version = 1;
    std::uint32_t preset = 6;
    std::uint32_t dictionary_mib = 64;
    std::uint64_t block_size = 0;
    std::uint64_t payload_start = 0;
    std::uint64_t archive_size = 0;
    std::string precomp_zlib_version;
    std::vector<FileRecord> files;
    std::vector<std::string> directories;
    std::vector<BlockRecord> blocks;
};

using ProgressCallback = std::function<void(const std::string&)>;

void pack_directory(const std::filesystem::path& source,
                    const std::filesystem::path& archive_path,
                    const PackOptions& options,
                    ProgressCallback progress = {});

ArchiveInfo read_archive(const std::filesystem::path& archive_path);
void extract_archive(const std::filesystem::path& archive_path,
                     const std::filesystem::path& destination,
                     bool overwrite = false,
                     ProgressCallback progress = {});
void verify_archive(const std::filesystem::path& archive_path,
                    ProgressCallback progress = {});

} // namespace rpk
