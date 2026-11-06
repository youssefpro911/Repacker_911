#include "archive.hpp"
#include "chunker.hpp"
#include "scanner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <lzma.h>
#include <zstd.h>

namespace rpk {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {'R', 'P', 'K', 'G', '\r', '\n', 0x1a, '\n'};
constexpr std::uint32_t kFormatVersion = 4;
constexpr std::uint64_t kMaximumManifestSize = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumPathSize = 1024ULL * 1024ULL;
constexpr std::uint32_t kMaximumFileCount = 5'000'000;
constexpr std::uint32_t kMaximumBlockCount = 1'000'000;
constexpr std::size_t kIoBufferSize = 1024U * 1024U;

std::uint64_t checked_add(std::uint64_t a, std::uint64_t b, const char* message) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        throw std::runtime_error(message);
    }
    return a + b;
}

std::string path_for_error(const std::filesystem::path& path) {
    try {
        return path.u8string();
    } catch (...) {
        return path.string();
    }
}

bool file_is_read_only(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error("Cannot read Windows file attributes: " + path_for_error(path) +
                                 " (Win32 error " + std::to_string(GetLastError()) + ")");
    }
    return (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
    std::error_code ec;
    const auto permissions = std::filesystem::status(path, ec).permissions();
    if (ec) {
        throw std::runtime_error("Cannot read file permissions: " + path_for_error(path) +
                                 " (" + ec.message() + ")");
    }
    return (permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
#endif
}

void set_file_read_only(const std::filesystem::path& path, bool read_only) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error("Cannot read Windows file attributes: " + path_for_error(path) +
                                 " (Win32 error " + std::to_string(GetLastError()) + ")");
    }
    const DWORD updated = read_only ? (attributes | FILE_ATTRIBUTE_READONLY)
                                    : (attributes & ~FILE_ATTRIBUTE_READONLY);
    if (updated != attributes && !SetFileAttributesW(path.c_str(), updated)) {
        throw std::runtime_error("Cannot change Windows read-only attribute: " + path_for_error(path) +
                                 " (Win32 error " + std::to_string(GetLastError()) + ")");
    }
#else
    std::error_code ec;
    const auto mode = read_only ? std::filesystem::perm_options::remove
                                : std::filesystem::perm_options::add;
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, mode, ec);
    if (ec) {
        throw std::runtime_error("Cannot change file write permission: " + path_for_error(path) +
                                 " (" + ec.message() + ")");
    }
#endif
}

class ByteWriter {
public:
    void u8(std::uint8_t value) { data_.push_back(value); }
    void u32(std::uint32_t value) {
        for (unsigned int i = 0; i < 4; ++i) data_.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
    }
    void u64(std::uint64_t value) {
        for (unsigned int i = 0; i < 8; ++i) data_.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
    }
    void raw(const void* value, std::size_t size) {
        const auto* first = static_cast<const std::uint8_t*>(value);
        data_.insert(data_.end(), first, first + size);
    }
    void string(const std::string& value) {
        if (value.size() > kMaximumPathSize || value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("A path is too long to store in the archive");
        }
        u32(static_cast<std::uint32_t>(value.size()));
        raw(value.data(), value.size());
    }
    const std::vector<std::uint8_t>& data() const { return data_; }

private:
    std::vector<std::uint8_t> data_;
};

class ByteReader {
public:
    explicit ByteReader(const std::vector<std::uint8_t>& data) : data_(data) {}

    std::uint8_t u8() {
        require(1);
        return data_[position_++];
    }
    std::uint32_t u32() {
        require(4);
        std::uint32_t value = 0;
        for (unsigned int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(data_[position_++]) << (i * 8U);
        return value;
    }
    std::uint64_t u64() {
        require(8);
        std::uint64_t value = 0;
        for (unsigned int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(data_[position_++]) << (i * 8U);
        return value;
    }
    template <std::size_t N>
    std::array<std::uint8_t, N> fixed() {
        require(N);
        std::array<std::uint8_t, N> value{};
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(position_), N, value.begin());
        position_ += N;
        return value;
    }
    std::string string() {
        const std::uint32_t length = u32();
        if (length > kMaximumPathSize) throw std::runtime_error("Archive contains an excessively long path");
        require(length);
        std::string value(reinterpret_cast<const char*>(data_.data() + position_), length);
        position_ += length;
        return value;
    }
    bool finished() const { return position_ == data_.size(); }

private:
    void require(std::size_t count) const {
        if (count > data_.size() - position_) throw std::runtime_error("Archive manifest is truncated");
    }
    const std::vector<std::uint8_t>& data_;
    std::size_t position_ = 0;
};

void write_exact(std::ostream& output, const void* data, std::size_t size, const std::string& context) {
    if (size == 0) return;
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output) throw std::runtime_error("Write failed while " + context);
}

void read_exact(std::istream& input, void* data, std::size_t size, const std::string& context) {
    if (size == 0) return;
    input.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) throw std::runtime_error("Unexpected end of file while " + context);
}

bool valid_utf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c <= 0x7fU) {
            ++i;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t code = 0;
        if ((c & 0xe0U) == 0xc0U) { count = 2; code = c & 0x1fU; }
        else if ((c & 0xf0U) == 0xe0U) { count = 3; code = c & 0x0fU; }
        else if ((c & 0xf8U) == 0xf0U) { count = 4; code = c & 0x07U; }
        else return false;
        if (i + count > text.size()) return false;
        for (std::size_t j = 1; j < count; ++j) {
            const unsigned char next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0U) != 0x80U) return false;
            code = (code << 6U) | (next & 0x3fU);
        }
        if ((count == 2 && code < 0x80U) || (count == 3 && code < 0x800U) ||
            (count == 4 && code < 0x10000U) || code > 0x10ffffU ||
            (code >= 0xd800U && code <= 0xdfffU)) return false;
        i += count;
    }
    return true;
}

bool windows_reserved_name(const std::string& component) {
    std::string stem = component.substr(0, component.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    if (stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) && stem[3] >= '1' && stem[3] <= '9') return true;
    return false;
}

void validate_relative_path(const std::string& value) {
    if (value.empty() || value.size() > kMaximumPathSize || !valid_utf8(value)) {
        throw std::runtime_error("Archive contains an invalid UTF-8 path");
    }
    if (value.front() == '/' || value.find('\\') != std::string::npos || value.find(':') != std::string::npos) {
        throw std::runtime_error("Archive contains an unsafe path: " + value);
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte == '<' || byte == '>' || byte == '"' || byte == '|' || byte == '?' || byte == '*') {
            throw std::runtime_error("Archive path contains a character that is not portable to Windows: " + value);
        }
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find('/', start);
        const std::string component = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (component.empty() || component == "." || component == ".." || component.back() == '.' || component.back() == ' ' || windows_reserved_name(component)) {
            throw std::runtime_error("Archive contains a non-portable or unsafe path: " + value);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

std::filesystem::path from_utf8(const std::string& value) {
    return std::filesystem::u8path(value.begin(), value.end());
}

std::string relative_utf8(const std::filesystem::path& path, const std::filesystem::path& root) {
    const std::string value = path.lexically_relative(root).generic_u8string();
    validate_relative_path(value);
    return value;
}

std::filesystem::path unique_child(const std::filesystem::path& parent, const std::string& prefix) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto id = now ^ (++sequence * 0x9e3779b97f4a7c15ULL) ^ static_cast<std::uint64_t>(attempt);
        const auto candidate = parent / (prefix + std::to_string(id));
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(candidate, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found)) return candidate;
    }
    throw std::runtime_error("Unable to create a unique temporary path in: " + path_for_error(parent));
}

class TempDirectory {
public:
    explicit TempDirectory(const std::filesystem::path& path) : path_(path) {
        if (!std::filesystem::create_directory(path_)) throw std::runtime_error("Unable to create temporary directory: " + path_for_error(path_));
    }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

void compress_xz(const std::vector<std::uint8_t>& input,
                 std::uint32_t preset,
                 std::uint32_t dictionary_mib,
                 std::ofstream& output,
                 std::uint64_t& compressed_size) {
    lzma_options_lzma options{};
    if (lzma_lzma_preset(&options, preset)) throw std::runtime_error("Invalid LZMA2 preset");
    const std::uint64_t dictionary_bytes = static_cast<std::uint64_t>(dictionary_mib) * 1024ULL * 1024ULL;
    if (dictionary_bytes > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("LZMA2 dictionary size is too large");
    options.dict_size = static_cast<std::uint32_t>(dictionary_bytes);

    lzma_filter filters[2] = {
        {LZMA_FILTER_LZMA2, &options},
        {LZMA_VLI_UNKNOWN, nullptr}
    };
    lzma_stream stream = LZMA_STREAM_INIT;
    const lzma_ret init_result = lzma_stream_encoder(&stream, filters, LZMA_CHECK_CRC64);
    if (init_result != LZMA_OK) throw std::runtime_error("Unable to initialize LZMA2 encoder (liblzma error " + std::to_string(init_result) + ")");

    stream.next_in = input.data();
    stream.avail_in = input.size();
    std::vector<std::uint8_t> out_buffer(256U * 1024U);
    lzma_ret result = LZMA_OK;
    try {
        do {
            stream.next_out = out_buffer.data();
            stream.avail_out = out_buffer.size();
            result = lzma_code(&stream, LZMA_FINISH);
            const std::size_t produced = out_buffer.size() - stream.avail_out;
            if (produced != 0) {
                write_exact(output, out_buffer.data(), produced, "writing a compressed block");
                compressed_size = checked_add(compressed_size, produced, "Compressed archive size overflow");
            }
            if (result != LZMA_OK && result != LZMA_STREAM_END) {
                throw std::runtime_error("LZMA2 compression failed (liblzma error " + std::to_string(result) + ")");
            }
        } while (result != LZMA_STREAM_END);
    } catch (...) {
        lzma_end(&stream);
        throw;
    }
    lzma_end(&stream);
}

void compress_zstd_long(const std::vector<std::uint8_t>& input,
                        std::uint32_t preset,
                        std::uint64_t block_size,
                        std::ofstream& output,
                        std::uint64_t& compressed_size) {
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (context == nullptr) throw std::runtime_error("Unable to allocate a Zstandard compression context");
    const auto set_parameter = [context](ZSTD_cParameter parameter, int value, const char* name) {
        const std::size_t result = ZSTD_CCtx_setParameter(context, parameter, value);
        if (ZSTD_isError(result)) throw std::runtime_error(std::string("Unable to set Zstandard ") + name + ": " + ZSTD_getErrorName(result));
    };
    try {
        const int level = preset <= 2U ? 3 : (preset <= 6U ? 9 : 19);
        unsigned int window_log = 20;
        std::uint64_t window_bytes = 1ULL << window_log;
        while (window_bytes < block_size && window_log < 30U) {
            ++window_log;
            window_bytes <<= 1U;
        }
        set_parameter(ZSTD_c_compressionLevel, level, "compression level");
        set_parameter(ZSTD_c_enableLongDistanceMatching, 1, "long-distance matching");
        set_parameter(ZSTD_c_windowLog, static_cast<int>(window_log), "window log");
        set_parameter(ZSTD_c_contentSizeFlag, 1, "content-size flag");
        set_parameter(ZSTD_c_checksumFlag, 1, "frame checksum");

        ZSTD_inBuffer input_buffer{input.data(), input.size(), 0};
        std::vector<std::uint8_t> output_buffer(256U * 1024U);
        bool finished = false;
        while (!finished) {
            ZSTD_outBuffer out{output_buffer.data(), output_buffer.size(), 0};
            const ZSTD_EndDirective directive = input_buffer.pos == input_buffer.size ? ZSTD_e_end : ZSTD_e_continue;
            const std::size_t remaining = ZSTD_compressStream2(context, &out, &input_buffer, directive);
            if (ZSTD_isError(remaining)) throw std::runtime_error(std::string("Zstandard compression failed: ") + ZSTD_getErrorName(remaining));
            if (out.pos != 0) {
                write_exact(output, output_buffer.data(), out.pos, "writing a Zstandard block");
                compressed_size = checked_add(compressed_size, static_cast<std::uint64_t>(out.pos), "Compressed archive size overflow");
            }
            finished = directive == ZSTD_e_end && remaining == 0;
            if (!finished && input_buffer.pos == input_buffer.size && out.pos == 0 && remaining == 0 && directive == ZSTD_e_continue) {
                continue;
            }
        }
    } catch (...) {
        ZSTD_freeCCtx(context);
        throw;
    }
    ZSTD_freeCCtx(context);
}

struct EncodedBlockResult {
    Codec codec = Codec::Lzma2;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t compressed_size = 0;
    Sha256 checksum{};
    std::filesystem::path part_path;
};

EncodedBlockResult compress_block_to_file(std::vector<std::uint8_t> input,
                                          Codec codec,
                                          std::uint32_t preset,
                                          std::uint32_t dictionary_mib,
                                          std::uint64_t block_size,
                                          const std::filesystem::path& part_path) {
    EncodedBlockResult result;
    result.codec = codec;
    result.uncompressed_size = input.size();
    result.checksum = sha256_bytes(input.data(), input.size());
    result.part_path = part_path;
    std::ofstream output(part_path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create compressed worker block: " + path_for_error(part_path));
    if (codec == Codec::Lzma2) {
        compress_xz(input, preset, dictionary_mib, output, result.compressed_size);
    } else if (codec == Codec::ZstdLong) {
        compress_zstd_long(input, preset, block_size, output, result.compressed_size);
    } else if (codec == Codec::Store) {
        write_exact(output, input.data(), input.size(), "writing a stored block");
        result.compressed_size = input.size();
    } else {
        throw std::runtime_error("Internal request for an unknown compression codec");
    }
    output.flush();
    if (!output) throw std::runtime_error("Failed to flush compressed worker block: " + path_for_error(part_path));
    output.close();
    if (result.compressed_size == 0) throw std::runtime_error("Compression produced an empty block payload (codec " +
        std::to_string(static_cast<std::uint32_t>(codec)) + ", input " + std::to_string(result.uncompressed_size) + " bytes)");
    return result;
}

Codec select_codec(CodecMode mode, const std::string& classification) {
    switch (mode) {
    case CodecMode::Lzma2: return Codec::Lzma2;
    case CodecMode::ZstdLong: return Codec::ZstdLong;
    case CodecMode::Store: return Codec::Store;
    case CodecMode::Auto: break;
    default: throw std::runtime_error("Invalid codec mode");
    }
    if (classification == "Audio" || classification == "Video" ||
        classification == "Archive / package" || classification == "PNG image" ||
        classification == "JPEG image" || classification == "ZIP archive" ||
        classification == "7-Zip archive" || classification == "RAR archive" ||
        classification == "Gzip stream" || classification == "ISO media / MP4" ||
        classification == "Ogg audio" || classification == "MP3 audio" ||
        classification == "WAV audio") {
        return Codec::Store;
    }
    if (classification == "Text / script" || classification == "Text / unknown" ||
        classification == "PDF document" || classification == "Possible zlib / DEFLATE stream") {
        return Codec::ZstdLong;
    }
    return Codec::Lzma2;
}

std::string ascii_case_key(const std::string& value);

void decompress_lzma2_block(std::ifstream& archive,
                         const ArchiveInfo& info,
                         const BlockRecord& block,
                         const std::filesystem::path& raw_path) {
    const std::uint64_t absolute_offset = checked_add(info.payload_start, block.payload_offset, "Archive offset overflow");
    archive.clear();
    archive.seekg(static_cast<std::streamoff>(absolute_offset), std::ios::beg);
    if (!archive) throw std::runtime_error("Unable to seek to a compressed block in the archive");

    std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
    if (!raw) throw std::runtime_error("Unable to create temporary block file: " + path_for_error(raw_path));

    lzma_stream stream = LZMA_STREAM_INIT;
    const std::uint64_t decoder_memory_limit = static_cast<std::uint64_t>(info.dictionary_mib) * 1024ULL * 1024ULL + 128ULL * 1024ULL * 1024ULL;
    const lzma_ret init_result = lzma_stream_decoder(&stream, decoder_memory_limit, 0);
    if (init_result != LZMA_OK) throw std::runtime_error("Unable to initialize LZMA2 decoder (liblzma error " + std::to_string(init_result) + ")");

    std::vector<std::uint8_t> input_buffer(64U * 1024U);
    std::vector<std::uint8_t> output_buffer(256U * 1024U);
    std::uint64_t unread = block.compressed_size;
    std::uint64_t uncompressed = 0;
    Sha256Hasher hasher;
    bool ended = false;

    try {
        while (!ended) {
            if (stream.avail_in == 0 && unread > 0) {
                const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(unread, input_buffer.size()));
                read_exact(archive, input_buffer.data(), static_cast<std::size_t>(chunk), "reading a compressed block");
                stream.next_in = input_buffer.data();
                stream.avail_in = static_cast<std::size_t>(chunk);
                unread -= static_cast<std::uint64_t>(chunk);
            }

            stream.next_out = output_buffer.data();
            stream.avail_out = output_buffer.size();
            const lzma_action action = unread == 0 ? LZMA_FINISH : LZMA_RUN;
            const lzma_ret result = lzma_code(&stream, action);
            const std::size_t produced = output_buffer.size() - stream.avail_out;
            if (produced != 0) {
                write_exact(raw, output_buffer.data(), produced, "writing decompressed block data");
                hasher.update(output_buffer.data(), produced);
                uncompressed = checked_add(uncompressed, produced, "Uncompressed block size overflow");
                if (uncompressed > block.uncompressed_size) throw std::runtime_error("A block expands beyond its declared size");
            }
            if (result == LZMA_STREAM_END) {
                if (unread != 0 || stream.avail_in != 0) throw std::runtime_error("Compressed block contains trailing or unreferenced bytes");
                ended = true;
            } else if (result != LZMA_OK && result != LZMA_BUF_ERROR) {
                throw std::runtime_error("LZMA2 decompression failed (liblzma error " + std::to_string(result) + ")");
            } else if (result == LZMA_BUF_ERROR && unread == 0 && stream.avail_in == 0 && produced == 0) {
                throw std::runtime_error("Compressed block is truncated");
            }
        }
        raw.flush();
        if (!raw) throw std::runtime_error("Failed to flush a decompressed block to disk");
    } catch (...) {
        lzma_end(&stream);
        raw.close();
        throw;
    }
    lzma_end(&stream);
    raw.close();

    if (uncompressed != block.uncompressed_size) throw std::runtime_error("Decompressed block size does not match its manifest");
    if (!constant_time_equal(hasher.finish(), block.checksum)) throw std::runtime_error("SHA-256 mismatch in a decompressed block");
}

void decompress_zstd_block(std::ifstream& archive,
                           const ArchiveInfo& info,
                           const BlockRecord& block,
                           const std::filesystem::path& raw_path) {
    const std::uint64_t absolute_offset = checked_add(info.payload_start, block.payload_offset, "Archive offset overflow");
    archive.clear();
    archive.seekg(static_cast<std::streamoff>(absolute_offset), std::ios::beg);
    if (!archive) throw std::runtime_error("Unable to seek to a Zstandard block in the archive");

    std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
    if (!raw) throw std::runtime_error("Unable to create temporary block file: " + path_for_error(raw_path));
    ZSTD_DCtx* context = ZSTD_createDCtx();
    if (context == nullptr) throw std::runtime_error("Unable to allocate a Zstandard decompression context");
    const std::uint64_t max_window = std::max<std::uint64_t>(1ULL << 20U, std::min<std::uint64_t>(info.block_size, 1ULL << 30U));
    unsigned int decoder_window_log = 20;
    while ((1ULL << decoder_window_log) < max_window && decoder_window_log < 30U) ++decoder_window_log;
    const std::size_t window_result = ZSTD_DCtx_setParameter(context, ZSTD_d_windowLogMax, static_cast<int>(decoder_window_log));
    if (ZSTD_isError(window_result)) {
        ZSTD_freeDCtx(context);
        throw std::runtime_error(std::string("Unable to set Zstandard decoder memory limit: ") + ZSTD_getErrorName(window_result));
    }

    std::vector<std::uint8_t> input_buffer(64U * 1024U);
    std::vector<std::uint8_t> output_buffer(256U * 1024U);
    std::uint64_t unread = block.compressed_size;
    std::uint64_t uncompressed = 0;
    Sha256Hasher hasher;
    bool ended = false;
    try {
        while (!ended) {
            std::size_t available = 0;
            if (unread > 0) {
                const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(unread, input_buffer.size()));
                read_exact(archive, input_buffer.data(), static_cast<std::size_t>(chunk), "reading a Zstandard block");
                available = static_cast<std::size_t>(chunk);
                unread -= static_cast<std::uint64_t>(chunk);
            }
            ZSTD_inBuffer in{input_buffer.data(), available, 0};
            do {
                ZSTD_outBuffer out{output_buffer.data(), output_buffer.size(), 0};
                const std::size_t result = ZSTD_decompressStream(context, &out, &in);
                if (ZSTD_isError(result)) throw std::runtime_error(std::string("Zstandard decompression failed: ") + ZSTD_getErrorName(result));
                if (out.pos != 0) {
                    write_exact(raw, output_buffer.data(), out.pos, "writing decompressed Zstandard data");
                    hasher.update(output_buffer.data(), out.pos);
                    uncompressed = checked_add(uncompressed, static_cast<std::uint64_t>(out.pos), "Uncompressed block size overflow");
                    if (uncompressed > block.uncompressed_size) throw std::runtime_error("A Zstandard block expands beyond its declared size");
                }
                if (result == 0) {
                    if (unread != 0 || in.pos != in.size) throw std::runtime_error("Zstandard block contains trailing or unreferenced bytes");
                    ended = true;
                    break;
                }
                if (in.pos == in.size && unread == 0 && out.pos == 0) throw std::runtime_error("Zstandard block is truncated");
                if (in.pos == in.size) break;
            } while (!ended);
        }
        raw.flush();
        if (!raw) throw std::runtime_error("Failed to flush a decompressed Zstandard block to disk");
    } catch (...) {
        ZSTD_freeDCtx(context);
        raw.close();
        throw;
    }
    ZSTD_freeDCtx(context);
    raw.close();
    if (uncompressed != block.uncompressed_size) throw std::runtime_error("Decompressed Zstandard block size does not match its manifest");
    if (!constant_time_equal(hasher.finish(), block.checksum)) throw std::runtime_error("SHA-256 mismatch in a decompressed Zstandard block");
}

void decompress_store_block(std::ifstream& archive,
                            const ArchiveInfo& info,
                            const BlockRecord& block,
                            const std::filesystem::path& raw_path) {
    if (block.compressed_size != block.uncompressed_size) throw std::runtime_error("Stored block has inconsistent compressed and uncompressed sizes");
    const std::uint64_t absolute_offset = checked_add(info.payload_start, block.payload_offset, "Archive offset overflow");
    archive.clear();
    archive.seekg(static_cast<std::streamoff>(absolute_offset), std::ios::beg);
    if (!archive) throw std::runtime_error("Unable to seek to a stored block in the archive");
    std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
    if (!raw) throw std::runtime_error("Unable to create temporary block file: " + path_for_error(raw_path));
    std::vector<std::uint8_t> buffer(kIoBufferSize);
    std::uint64_t remaining = block.compressed_size;
    Sha256Hasher hasher;
    while (remaining > 0) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        read_exact(archive, buffer.data(), count, "reading a stored block");
        write_exact(raw, buffer.data(), count, "writing a stored block");
        hasher.update(buffer.data(), count);
        remaining -= count;
    }
    raw.flush();
    if (!raw) throw std::runtime_error("Failed to flush a stored block to disk");
    raw.close();
    if (!constant_time_equal(hasher.finish(), block.checksum)) throw std::runtime_error("SHA-256 mismatch in a stored block");
}

void decompress_block(std::ifstream& archive,
                      const ArchiveInfo& info,
                      const BlockRecord& block,
                      const std::filesystem::path& raw_path) {
    switch (block.codec) {
    case Codec::Lzma2: decompress_lzma2_block(archive, info, block, raw_path); return;
    case Codec::ZstdLong: decompress_zstd_block(archive, info, block, raw_path); return;
    case Codec::Store: decompress_store_block(archive, info, block, raw_path); return;
    default: throw std::runtime_error("Archive contains an unsupported block codec");
    }
}

std::string ascii_case_key(const std::string& value) {
    std::string key = value;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return c < 0x80U ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    });
    return key;
}

void validate_manifest(const ArchiveInfo& info) {
    if (info.version < 1 || info.version > kFormatVersion) throw std::runtime_error("Unsupported archive version: " + std::to_string(info.version));
    if (info.preset < 1 || info.preset > 9) throw std::runtime_error("Archive contains an invalid LZMA2 preset");
    if (info.dictionary_mib < 1 || info.dictionary_mib > 1024) throw std::runtime_error("Archive contains an invalid LZMA2 dictionary size");
    if (info.block_size < 1024ULL * 1024ULL || info.block_size > 1024ULL * 1024ULL * 1024ULL) throw std::runtime_error("Archive contains an invalid solid block size");
    if (info.files.size() > kMaximumFileCount || info.directories.size() > kMaximumFileCount || info.blocks.size() > kMaximumBlockCount) {
        throw std::runtime_error("Archive contains too many records");
    }

    if (info.version >= 3) {
        const bool has_precomp = std::any_of(info.files.begin(), info.files.end(), [](const FileRecord& file) { return !file.precomp.empty(); });
        if (has_precomp && info.precomp_zlib_version.empty()) throw std::runtime_error("Archive omits the zlib runtime version required by precompression metadata");
    }
    std::set<std::string> file_paths;
    std::set<std::string> directory_paths;
    std::set<std::string> case_insensitive_paths;
    for (const auto& file : info.files) {
        validate_relative_path(file.path);
        if (!file_paths.insert(file.path).second) throw std::runtime_error("Archive contains duplicate file paths: " + file.path);
        if (!case_insensitive_paths.insert(ascii_case_key(file.path)).second) throw std::runtime_error("Archive contains paths that collide on a case-insensitive filesystem: " + file.path);
        if (file.stored_size == 0 && file.size != 0) throw std::runtime_error("Archive contains an invalid stored file size: " + file.path);
        std::uint64_t expected_file_offset = 0;
        for (const auto& extent : file.extents) {
            if (extent.length == 0 || extent.file_offset != expected_file_offset || extent.block_index >= info.blocks.size()) {
                throw std::runtime_error("Archive contains an invalid extent for: " + file.path);
            }
            const auto& block = info.blocks[extent.block_index];
            if (extent.block_offset > block.uncompressed_size || extent.length > block.uncompressed_size - extent.block_offset) {
                throw std::runtime_error("Archive extent exceeds its solid block: " + file.path);
            }
            expected_file_offset = checked_add(expected_file_offset, extent.length, "File extent size overflow");
        }
        if (expected_file_offset != file.stored_size) throw std::runtime_error("Archive extents do not cover the stored file size: " + file.path);
        if (!file.precomp.empty() && file.stored_size > 256ULL * 1024ULL * 1024ULL) throw std::runtime_error("Archive precompressed file exceeds the safe reconstruction size: " + file.path);
        std::uint64_t previous_end = 0;
        for (const auto& record : file.precomp) {
            if (record.raw_size == 0 || record.encoded_size < 6 || record.transformed_offset < previous_end ||
                record.transformed_offset > file.stored_size || record.raw_size > file.stored_size - record.transformed_offset || record.level > 9) {
                throw std::runtime_error("Archive contains invalid zlib precompression metadata: " + file.path);
            }
            previous_end = record.transformed_offset + record.raw_size;
        }
        if (!file.precomp.empty() && info.version < 3) throw std::runtime_error("Legacy archive unexpectedly contains precompression metadata");
    }
    for (const auto& directory : info.directories) {
        validate_relative_path(directory);
        if (!directory_paths.insert(directory).second) throw std::runtime_error("Archive contains duplicate directory paths: " + directory);
        if (!case_insensitive_paths.insert(ascii_case_key(directory)).second) throw std::runtime_error("Archive contains paths that collide on a case-insensitive filesystem: " + directory);
        if (file_paths.count(directory) != 0) throw std::runtime_error("Archive path is both a file and directory: " + directory);
    }

    for (const auto& path : file_paths) {
        std::size_t slash = path.find('/');
        while (slash != std::string::npos) {
            const std::string parent = path.substr(0, slash);
            if (file_paths.count(parent) != 0) throw std::runtime_error("Archive file is used as a parent directory: " + parent);
            slash = path.find('/', slash + 1);
        }
    }
    for (const auto& path : directory_paths) {
        std::size_t slash = path.find('/');
        while (slash != std::string::npos) {
            const std::string parent = path.substr(0, slash);
            if (file_paths.count(parent) != 0) throw std::runtime_error("Archive file is used as a parent directory: " + parent);
            slash = path.find('/', slash + 1);
        }
    }

    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> ranges(info.blocks.size());
    for (const auto& file : info.files) {
        for (const auto& extent : file.extents) {
            ranges[extent.block_index].emplace_back(extent.block_offset, extent.block_offset + extent.length);
        }
    }
    for (std::size_t i = 0; i < info.blocks.size(); ++i) {
        const auto& block = info.blocks[i];
        if (block.uncompressed_size == 0 || block.uncompressed_size > info.block_size || block.compressed_size == 0) {
            throw std::runtime_error("Archive contains an invalid solid block record");
        }
        if (block.codec != Codec::Lzma2 && block.codec != Codec::ZstdLong && block.codec != Codec::Store) {
            throw std::runtime_error("Archive contains an unsupported solid block codec");
        }
        if (block.codec == Codec::Store && block.compressed_size != block.uncompressed_size) {
            throw std::runtime_error("Stored block has inconsistent compressed and uncompressed sizes");
        }
        auto& block_ranges = ranges[i];
        std::sort(block_ranges.begin(), block_ranges.end());
        block_ranges.erase(std::unique(block_ranges.begin(), block_ranges.end()), block_ranges.end());
        std::uint64_t cursor = 0;
        for (const auto& range : block_ranges) {
            if (range.first != cursor || range.second <= range.first) {
                throw std::runtime_error("Archive solid block has overlapping or missing unique data");
            }
            cursor = range.second;
        }
        if (cursor != block.uncompressed_size) throw std::runtime_error("Archive solid block contains unreferenced data");
    }
}

std::vector<std::uint8_t> serialize_manifest(const ArchiveInfo& info) {
    ByteWriter writer;
    if (info.files.size() > std::numeric_limits<std::uint32_t>::max() ||
        info.directories.size() > std::numeric_limits<std::uint32_t>::max() ||
        info.blocks.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Too many archive entries");
    }
    writer.u32(static_cast<std::uint32_t>(info.files.size()));
    writer.u32(static_cast<std::uint32_t>(info.directories.size()));
    writer.u32(static_cast<std::uint32_t>(info.blocks.size()));
    writer.u32(info.preset);
    writer.u32(info.dictionary_mib);
    writer.u64(info.block_size);
    if (info.version >= 3) writer.string(info.precomp_zlib_version);

    for (const auto& file : info.files) {
        writer.string(file.path);
        writer.u64(file.size);
        if (info.version >= 3) writer.u64(file.stored_size);
        writer.u8(file.read_only ? 1U : 0U);
        for (int i = 0; i < 7; ++i) writer.u8(0);
        writer.raw(file.checksum.data(), file.checksum.size());
        if (file.extents.size() > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("A file contains too many extents");
        writer.u32(static_cast<std::uint32_t>(file.extents.size()));
        for (const auto& extent : file.extents) {
            writer.u32(extent.block_index);
            writer.u64(extent.block_offset);
            writer.u64(extent.file_offset);
            writer.u64(extent.length);
        }
        if (info.version >= 3) {
            if (file.precomp.size() > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("A file contains too many precompression records");
            writer.u32(static_cast<std::uint32_t>(file.precomp.size()));
            for (const auto& record : file.precomp) {
                writer.u64(record.transformed_offset);
                writer.u64(record.raw_size);
                writer.u64(record.encoded_size);
                writer.u32(record.level);
                writer.raw(record.encoded_checksum.data(), record.encoded_checksum.size());
            }
        }
    }
    for (const auto& directory : info.directories) writer.string(directory);
    for (const auto& block : info.blocks) {
        writer.u64(block.payload_offset);
        writer.u64(block.compressed_size);
        writer.u64(block.uncompressed_size);
        if (info.version >= 4) {
            writer.u32(static_cast<std::uint32_t>(block.codec));
            writer.u32(0);
        }
        writer.raw(block.checksum.data(), block.checksum.size());
    }
    if (writer.data().size() > kMaximumManifestSize) throw std::runtime_error("Archive manifest exceeds the 512 MiB safety limit");
    return writer.data();
}

ArchiveInfo parse_manifest(const std::vector<std::uint8_t>& bytes, std::uint32_t version) {
    ByteReader reader(bytes);
    ArchiveInfo info;
    const std::uint32_t file_count = reader.u32();
    const std::uint32_t directory_count = reader.u32();
    const std::uint32_t block_count = reader.u32();
    info.preset = reader.u32();
    info.dictionary_mib = reader.u32();
    info.block_size = reader.u64();
    info.version = version;
    if (version >= 3) info.precomp_zlib_version = reader.string();
    if (file_count > kMaximumFileCount || directory_count > kMaximumFileCount || block_count > kMaximumBlockCount) {
        throw std::runtime_error("Archive declares an unreasonable number of records");
    }
    info.files.reserve(file_count);
    info.directories.reserve(directory_count);
    info.blocks.reserve(block_count);

    for (std::uint32_t i = 0; i < file_count; ++i) {
        FileRecord file;
        file.path = reader.string();
        file.size = reader.u64();
        file.stored_size = version >= 3 ? reader.u64() : file.size;
        const std::uint8_t flags = reader.u8();
        if (flags > 1) throw std::runtime_error("Archive contains unsupported file flags");
        file.read_only = flags != 0;
        for (int j = 0; j < 7; ++j) {
            if (reader.u8() != 0) throw std::runtime_error("Archive contains unknown reserved file flags");
        }
        file.checksum = reader.fixed<32>();
        const std::uint32_t extent_count = reader.u32();
        if (extent_count > kMaximumBlockCount) throw std::runtime_error("Archive declares too many extents for one file");
        file.extents.reserve(extent_count);
        for (std::uint32_t j = 0; j < extent_count; ++j) {
            Extent extent;
            extent.block_index = reader.u32();
            extent.block_offset = reader.u64();
            extent.file_offset = reader.u64();
            extent.length = reader.u64();
            file.extents.push_back(extent);
        }
        if (version >= 3) {
            const std::uint32_t precomp_count = reader.u32();
            if (precomp_count > 1'000'000U) throw std::runtime_error("Archive declares too many precompression records");
            file.precomp.reserve(precomp_count);
            for (std::uint32_t j = 0; j < precomp_count; ++j) {
                PrecompRecord record;
                record.transformed_offset = reader.u64();
                record.raw_size = reader.u64();
                record.encoded_size = reader.u64();
                record.level = reader.u32();
                record.encoded_checksum = reader.fixed<32>();
                file.precomp.push_back(record);
            }
        }
        info.files.push_back(std::move(file));
    }
    for (std::uint32_t i = 0; i < directory_count; ++i) info.directories.push_back(reader.string());
    for (std::uint32_t i = 0; i < block_count; ++i) {
        BlockRecord block;
        block.payload_offset = reader.u64();
        block.compressed_size = reader.u64();
        block.uncompressed_size = reader.u64();
        if (version >= 4) {
            const std::uint32_t codec = reader.u32();
            if (reader.u32() != 0) throw std::runtime_error("Archive contains unknown reserved block flags");
            if (codec < static_cast<std::uint32_t>(Codec::Lzma2) || codec > static_cast<std::uint32_t>(Codec::Store)) {
                throw std::runtime_error("Archive contains an unsupported block codec identifier");
            }
            block.codec = static_cast<Codec>(codec);
        } else {
            block.codec = Codec::Lzma2;
        }
        block.checksum = reader.fixed<32>();
        info.blocks.push_back(block);
    }
    if (!reader.finished()) throw std::runtime_error("Archive manifest contains unexpected trailing data");
    validate_manifest(info);
    return info;
}

std::filesystem::path safe_directory(const std::filesystem::path& root,
                                     const std::string& relative,
                                     bool create) {
    validate_relative_path(relative);
    std::filesystem::path current = root;
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t end = relative.find('/', start);
        const std::string component = relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
        current /= from_utf8(component);
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(current, ec);
        const bool absent = ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found);
        if (absent) {
            if (!create) throw std::runtime_error("Destination directory does not exist: " + path_for_error(current));
            ec.clear();
            if (!std::filesystem::create_directory(current, ec) && ec) throw std::runtime_error("Cannot create destination directory: " + path_for_error(current) + " (" + ec.message() + ")");
        } else {
            if (ec) throw std::runtime_error("Cannot inspect destination path: " + path_for_error(current) + " (" + ec.message() + ")");
            if (std::filesystem::is_symlink(status)) throw std::runtime_error("Refusing to extract through a symbolic link: " + path_for_error(current));
            if (!std::filesystem::is_directory(status)) throw std::runtime_error("A file blocks a destination directory: " + path_for_error(current));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return current;
}

void check_existing_parent_chain(const std::filesystem::path& root, const std::string& relative_parent) {
    if (relative_parent.empty() || relative_parent == ".") return;
    validate_relative_path(relative_parent);
    std::filesystem::path current = root;
    std::size_t start = 0;
    while (start <= relative_parent.size()) {
        const std::size_t end = relative_parent.find('/', start);
        const std::string component = relative_parent.substr(start, end == std::string::npos ? std::string::npos : end - start);
        current /= from_utf8(component);
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(current, ec);
        const bool absent = ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found);
        if (absent) return;
        if (ec) throw std::runtime_error("Cannot inspect destination path: " + path_for_error(current) + " (" + ec.message() + ")");
        if (std::filesystem::is_symlink(status)) throw std::runtime_error("Refusing to extract through a symbolic link: " + path_for_error(current));
        if (!std::filesystem::is_directory(status)) throw std::runtime_error("A file blocks a destination directory: " + path_for_error(current));
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

void check_existing_directory_target(const std::filesystem::path& root, const std::string& relative) {
    validate_relative_path(relative);
    const auto slash = relative.rfind('/');
    if (slash != std::string::npos) check_existing_parent_chain(root, relative.substr(0, slash));
    const auto target = root / from_utf8(relative);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(target, ec);
    const bool absent = ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found);
    if (absent) return;
    if (ec) throw std::runtime_error("Cannot inspect extraction directory: " + path_for_error(target) + " (" + ec.message() + ")");
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        throw std::runtime_error("Unsafe existing destination directory: " + path_for_error(target));
    }
}

void ensure_no_unsafe_existing_target(const std::filesystem::path& target, bool overwrite) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(target, ec);
    const bool absent = ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found);
    if (absent) return;
    if (ec) throw std::runtime_error("Cannot inspect extraction target: " + path_for_error(target) + " (" + ec.message() + ")");
    if (std::filesystem::is_symlink(status)) throw std::runtime_error("Refusing to overwrite a symbolic link: " + path_for_error(target));
    if (!std::filesystem::is_regular_file(status)) throw std::runtime_error("A non-file object blocks extraction: " + path_for_error(target));
    if (!overwrite) throw std::runtime_error("File already exists (use --overwrite to replace it): " + path_for_error(target));
}

void write_header(std::ostream& output, const std::vector<std::uint8_t>& manifest, std::uint32_t version) {
    write_exact(output, kMagic.data(), kMagic.size(), "writing archive header");
    const std::uint32_t flags = 0;
    const std::uint64_t manifest_size = manifest.size();
    const auto write_u32 = [&output](std::uint32_t value) {
        std::array<std::uint8_t, 4> bytes{};
        for (unsigned int i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
        write_exact(output, bytes.data(), bytes.size(), "writing archive header");
    };
    const auto write_u64 = [&output](std::uint64_t value) {
        std::array<std::uint8_t, 8> bytes{};
        for (unsigned int i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
        write_exact(output, bytes.data(), bytes.size(), "writing archive header");
    };
    write_u32(version);
    write_u32(flags);
    write_u64(manifest_size);
    const Sha256 digest = sha256_bytes(manifest.data(), manifest.size());
    write_exact(output, digest.data(), digest.size(), "writing manifest checksum");
    write_exact(output, manifest.data(), manifest.size(), "writing archive manifest");
}

void copy_stream(std::istream& input, std::ostream& output, const std::string& context) {
    std::vector<char> buffer(kIoBufferSize);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            output.write(buffer.data(), count);
            if (!output) throw std::runtime_error("Write failed while " + context);
        }
    }
    if (!input.eof()) throw std::runtime_error("Read failed while " + context);
}

} // namespace

void pack_directory(const std::filesystem::path& source,
                    const std::filesystem::path& archive_path,
                    const PackOptions& options,
                    ProgressCallback progress) {
    if (options.preset < 1 || options.preset > 9) throw std::runtime_error("Preset must be between 1 and 9");
    if (options.dictionary_mib < 1 || options.dictionary_mib > 1024) throw std::runtime_error("Dictionary size must be between 1 and 1024 MiB");
    if (options.block_size < 1024ULL * 1024ULL || options.block_size > 1024ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("Solid block size must be between 1 and 1024 MiB");
    }

    std::error_code ec;
    const auto source_root = std::filesystem::canonical(source, ec);
    if (ec || !std::filesystem::is_directory(source_root)) throw std::runtime_error("Source folder does not exist or is not a directory: " + path_for_error(source));

    auto output_abs = std::filesystem::absolute(archive_path).lexically_normal();
    const auto source_abs = source_root.lexically_normal();
    auto output_parent = output_abs.parent_path();
    if (output_parent.empty()) output_parent = std::filesystem::current_path();
    std::filesystem::create_directories(output_parent, ec);
    if (ec) throw std::runtime_error("Cannot create output folder: " + path_for_error(output_parent) + " (" + ec.message() + ")");
    output_abs = output_parent / output_abs.filename();
    const auto source_text = source_abs.generic_u8string();
    const auto output_text = output_abs.generic_u8string();
    if (output_text == source_text || (output_text.size() > source_text.size() && output_text.compare(0, source_text.size(), source_text) == 0 && output_text[source_text.size()] == '/')) {
        throw std::runtime_error("The archive output must be outside the source folder");
    }
    if (std::filesystem::exists(output_abs) && !options.overwrite) throw std::runtime_error("Output archive already exists (use --force to replace it): " + path_for_error(output_abs));

    constexpr std::size_t kEntropySampleLimit = 32U * 1024U;
    std::vector<std::pair<std::string, std::filesystem::path>> source_files;
    std::vector<std::string> directories;
    std::unordered_map<std::string, std::string> group_by_path;
    std::unordered_map<std::string, double> entropy_by_path;
    std::filesystem::recursive_directory_iterator iterator(source_root, std::filesystem::directory_options::none, ec);
    if (ec) throw std::runtime_error("Cannot scan source folder: " + ec.message());
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const auto entry_path = iterator->path();
        const auto status = iterator->symlink_status(ec);
        if (ec) throw std::runtime_error("Cannot inspect source entry: " + path_for_error(entry_path) + " (" + ec.message() + ")");
        if (std::filesystem::is_symlink(status)) throw std::runtime_error("Symbolic links are not supported: " + path_for_error(entry_path));
        const std::string relative = relative_utf8(entry_path, source_root);
        if (std::filesystem::is_directory(status)) {
            directories.push_back(relative);
        } else if (std::filesystem::is_regular_file(status)) {
            std::vector<std::uint8_t> sample(kEntropySampleLimit);
            std::ifstream sample_input(entry_path, std::ios::binary);
            if (!sample_input) throw std::runtime_error("Cannot sample source file: " + path_for_error(entry_path));
            sample_input.read(reinterpret_cast<char*>(sample.data()), static_cast<std::streamsize>(sample.size()));
            const auto sample_count = sample_input.gcount();
            if (sample_count < 0) throw std::runtime_error("Cannot sample source file: " + path_for_error(entry_path));
            sample.resize(static_cast<std::size_t>(sample_count));
            const std::string group = classify_file_type(entry_path.extension().u8string(), sample);
            group_by_path.emplace(relative, group);
            entropy_by_path.emplace(relative, estimate_entropy_bits_per_byte(sample));
            source_files.emplace_back(relative, entry_path);
        } else {
            throw std::runtime_error("Unsupported filesystem object in source folder: " + path_for_error(entry_path));
        }
        iterator.increment(ec);
        if (ec) throw std::runtime_error("Error while scanning source folder: " + ec.message());
    }
    std::sort(source_files.begin(), source_files.end(), [&](const auto& a, const auto& b) {
        const auto& group_a = group_by_path.at(a.first);
        const auto& group_b = group_by_path.at(b.first);
        return group_a == group_b ? a.first < b.first : group_a < group_b;
    });
    std::sort(directories.begin(), directories.end());
    if (source_files.size() > kMaximumFileCount || directories.size() > kMaximumFileCount) throw std::runtime_error("Source folder contains too many entries for this archive version");

    ArchiveInfo info;
    info.version = kFormatVersion;
    info.preset = options.preset;
    info.dictionary_mib = options.dictionary_mib;
    info.block_size = options.block_size;
    info.directories = std::move(directories);
    info.files.reserve(source_files.size());

    const auto spool_path = unique_child(output_parent, ".rpk-spool-");
    const auto raw_spool_path = unique_child(output_parent, ".rpk-dedup-");
    const auto final_temp_path = unique_child(output_parent, ".rpk-output-");
    const auto parts_directory = unique_child(output_parent, ".rpk-parts-");
    struct Cleanup {
        std::filesystem::path a;
        std::filesystem::path b;
        std::filesystem::path c;
        std::filesystem::path d;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(a, ec);
            ec.clear();
            std::filesystem::remove(b, ec);
            ec.clear();
            std::filesystem::remove(c, ec);
            ec.clear();
            std::filesystem::remove_all(d, ec);
        }
    } cleanup{spool_path, raw_spool_path, final_temp_path, parts_directory};
    if (!std::filesystem::create_directory(parts_directory, ec) || ec) {
        throw std::runtime_error("Cannot create temporary compression workspace: " + path_for_error(parts_directory));
    }

    std::ofstream spool(spool_path, std::ios::binary | std::ios::trunc);
    if (!spool) throw std::runtime_error("Cannot create temporary compressed-data file");
    std::fstream raw_spool;
    if (options.deduplicate) {
        raw_spool.open(raw_spool_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!raw_spool) throw std::runtime_error("Cannot create temporary deduplication data file");
    }

    struct DedupLocation {
        std::uint32_t block_index = 0;
        std::uint64_t block_offset = 0;
        std::uint64_t length = 0;
        std::uint64_t raw_offset = 0;
    };
    std::unordered_map<std::string, std::vector<DedupLocation>> dedup_index;
    std::uint64_t raw_spool_size = 0;
    std::uint64_t compressed_total = 0;
    std::uint64_t source_total = 0;
    std::uint64_t stored_total = 0;
    std::uint64_t unique_total = 0;
    std::vector<std::uint8_t> block;
    block.reserve(static_cast<std::size_t>(options.block_size));
    std::uint32_t created_block_count = 0;
    std::string active_group;
    Codec active_codec = Codec::Lzma2;

    if (options.codec != CodecMode::Auto && options.codec != CodecMode::Lzma2 &&
        options.codec != CodecMode::ZstdLong && options.codec != CodecMode::Store) {
        throw std::runtime_error("Invalid codec selection");
    }
    if (options.threads > 64U) throw std::runtime_error("Thread count must be between 1 and 64, or 0 for automatic selection");
    if (options.memory_limit_mib < 128U || options.memory_limit_mib > 65536U) {
        throw std::runtime_error("Memory limit must be between 128 and 65536 MiB");
    }
    const std::uint64_t memory_limit_bytes = static_cast<std::uint64_t>(options.memory_limit_mib) * 1024ULL * 1024ULL;
    const std::uint64_t estimated_worker_bytes = checked_add(
        checked_add(options.block_size, static_cast<std::uint64_t>(options.dictionary_mib) * 1024ULL * 1024ULL,
                    "Compression memory estimate overflow"),
        128ULL * 1024ULL * 1024ULL, "Compression memory estimate overflow");
    const std::uint64_t base_memory_bytes = options.block_size;
    if (memory_limit_bytes <= base_memory_bytes) {
        throw std::runtime_error("Memory limit is too low for the configured block size; increase --memory-mib or reduce --block-size-mib");
    }
    const std::uint64_t safe_workers = (memory_limit_bytes - base_memory_bytes) / estimated_worker_bytes;
    if (safe_workers == 0) {
        throw std::runtime_error("Memory limit cannot safely accommodate one compression worker; increase --memory-mib or reduce block/dictionary sizes");
    }
    const std::uint32_t hardware_threads = std::max<std::uint32_t>(1U, std::thread::hardware_concurrency());
    const std::uint32_t requested_threads = options.threads == 0 ? hardware_threads : options.threads;
    const std::size_t max_inflight = static_cast<std::size_t>(std::max<std::uint64_t>(1U,
        std::min<std::uint64_t>(requested_threads, safe_workers)));
    if (progress) {
        progress("Compression scheduler: " + std::to_string(max_inflight) + " worker(s), memory budget " +
                 std::to_string(options.memory_limit_mib) + " MiB");
    }

    struct PendingBlock {
        std::uint32_t index = 0;
        std::future<EncodedBlockResult> future;
    };
    std::deque<PendingBlock> pending_blocks;

    auto commit_ready_block = [&]() {
        if (pending_blocks.empty()) return;
        PendingBlock pending = std::move(pending_blocks.front());
        pending_blocks.pop_front();
        EncodedBlockResult encoded = pending.future.get();
        if (pending.index != info.blocks.size()) throw std::runtime_error("Internal compressed block ordering error");
        std::ifstream part(encoded.part_path, std::ios::binary);
        if (!part) throw std::runtime_error("Cannot reopen completed compression worker block");
        const std::uint64_t payload_offset = compressed_total;
        copy_stream(part, spool, "assembling compressed worker block");
        compressed_total = checked_add(compressed_total, encoded.compressed_size, "Compressed archive size overflow");
        part.close();
        std::filesystem::remove(encoded.part_path, ec);
        if (ec) throw std::runtime_error("Cannot remove completed compression worker block: " + ec.message());
        BlockRecord record;
        record.codec = encoded.codec;
        record.payload_offset = payload_offset;
        record.compressed_size = encoded.compressed_size;
        record.uncompressed_size = encoded.uncompressed_size;
        record.checksum = encoded.checksum;
        info.blocks.push_back(record);
        if (progress) progress("Compressed block " + std::to_string(info.blocks.size()) + " using " +
                               (record.codec == Codec::Lzma2 ? "LZMA2" : (record.codec == Codec::ZstdLong ? "Zstandard long-range" : "Store")) +
                               " (" + std::to_string(record.uncompressed_size / (1024ULL * 1024ULL)) + " MiB)");
    };

    auto flush_block = [&]() {
        if (block.empty()) return;
        if (created_block_count >= kMaximumBlockCount) throw std::runtime_error("Source requires too many solid blocks");
        const std::uint32_t block_index = created_block_count++;
        auto owned_block = std::move(block);
        block = {};
        block.reserve(static_cast<std::size_t>(options.block_size));
        const Codec codec = active_codec;
        const auto part_path = parts_directory / ("block-" + std::to_string(block_index) + ".part");
        pending_blocks.push_back(PendingBlock{block_index,
            std::async(std::launch::async, [owned = std::move(owned_block), codec, &options, part_path]() mutable {
                return compress_block_to_file(std::move(owned), codec, options.preset, options.dictionary_mib,
                                               options.block_size, part_path);
            })});
        if (pending_blocks.size() >= max_inflight) commit_ready_block();
    };

    auto append_chunk = [&](FileRecord& file,
                            std::uint64_t& file_offset,
                            const std::vector<std::uint8_t>& chunk) {
        if (chunk.empty()) return;
        const Sha256 digest = sha256_bytes(chunk.data(), chunk.size());
        const std::string key = sha256_hex(digest);
        bool reused = false;
        if (options.deduplicate) {
            const auto found = dedup_index.find(key);
            if (found != dedup_index.end()) {
                std::vector<std::uint8_t> comparison(chunk.size());
                for (const auto& candidate : found->second) {
                    if (candidate.length != chunk.size()) continue;
                    raw_spool.flush();
                    if (!raw_spool) throw std::runtime_error("Failed to flush deduplication comparison data");
                    raw_spool.clear();
                    raw_spool.seekg(static_cast<std::streamoff>(candidate.raw_offset), std::ios::beg);
                    if (!raw_spool) throw std::runtime_error("Failed to seek deduplication comparison data");
                    raw_spool.read(reinterpret_cast<char*>(comparison.data()), static_cast<std::streamsize>(comparison.size()));
                    if (raw_spool.gcount() != static_cast<std::streamsize>(comparison.size())) {
                        throw std::runtime_error("Temporary deduplication data is truncated");
                    }
                    if (comparison == chunk) {
                        if (file.extents.size() >= kMaximumBlockCount) throw std::runtime_error("File requires too many content extents for this archive version: " + file.path);
                        Extent extent;
                        extent.block_index = candidate.block_index;
                        extent.block_offset = candidate.block_offset;
                        extent.file_offset = file_offset;
                        extent.length = chunk.size();
                        file.extents.push_back(extent);
                        file_offset = checked_add(file_offset, chunk.size(), "Source file size exceeds the archive format limit");
                        reused = true;
                        break;
                    }
                }
            }
        }
        if (reused) return;

        if (!block.empty() && chunk.size() > options.block_size - block.size()) flush_block();
        if (chunk.size() > options.block_size) throw std::runtime_error("Chunk exceeds the configured solid block size");
        if (created_block_count >= kMaximumBlockCount) throw std::runtime_error("Source requires too many solid blocks");
        if (file.extents.size() >= kMaximumBlockCount) throw std::runtime_error("File requires too many content extents for this archive version: " + file.path);
        const auto block_index = created_block_count;
        const std::uint64_t block_offset = block.size();
        Extent extent;
        extent.block_index = block_index;
        extent.block_offset = block_offset;
        extent.file_offset = file_offset;
        extent.length = chunk.size();
        file.extents.push_back(extent);
        block.insert(block.end(), chunk.begin(), chunk.end());
        file_offset = checked_add(file_offset, chunk.size(), "Source file size exceeds the archive format limit");
        unique_total = checked_add(unique_total, chunk.size(), "Unique source size overflow");

        if (options.deduplicate) {
            DedupLocation location;
            location.block_index = block_index;
            location.block_offset = block_offset;
            location.length = chunk.size();
            location.raw_offset = raw_spool_size;
            raw_spool.clear();
            raw_spool.seekp(static_cast<std::streamoff>(raw_spool_size), std::ios::beg);
            if (!raw_spool) throw std::runtime_error("Failed to seek temporary deduplication data");
            raw_spool.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            if (!raw_spool) throw std::runtime_error("Failed to write temporary deduplication data");
            raw_spool_size = checked_add(raw_spool_size, chunk.size(), "Deduplication spool size overflow");
            dedup_index[key].push_back(location);
        }
        if (block.size() == options.block_size) flush_block();
    };

    constexpr std::uint64_t kPrecompInputLimit = 64ULL * 1024ULL * 1024ULL;
    std::vector<char> read_buffer(kIoBufferSize);
    for (const auto& item : source_files) {
        const std::string& file_group = group_by_path.at(item.first);
        if (file_group != active_group) {
            flush_block();
            active_group = file_group;
            active_codec = select_codec(options.codec, active_group);
            if (progress) progress("Codec group: " + active_group + " -> " +
                (active_codec == Codec::Lzma2 ? "LZMA2" : (active_codec == Codec::ZstdLong ? "Zstandard long-range" : "Store")));
        }
        FileRecord file;
        file.path = item.first;
        file.read_only = file_is_read_only(item.second);

        std::vector<std::uint8_t> entropy_sample;
        entropy_sample.reserve(kEntropySampleLimit);
        std::uint64_t file_offset = 0;
        ContentDefinedChunker chunker;
        std::error_code status_error;
        const auto file_bytes = std::filesystem::file_size(item.second, status_error);
        const bool can_precomp = options.precompress && !status_error && file_bytes <= kPrecompInputLimit;

        if (can_precomp) {
            std::ifstream input(item.second, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open source file: " + path_for_error(item.second));
            std::vector<std::uint8_t> original(static_cast<std::size_t>(file_bytes));
            if (!original.empty()) {
                input.read(reinterpret_cast<char*>(original.data()), static_cast<std::streamsize>(original.size()));
                if (input.gcount() != static_cast<std::streamsize>(original.size())) {
                    throw std::runtime_error("Source file changed or could not be fully read: " + path_for_error(item.second));
                }
            }
            if (input.peek() != std::char_traits<char>::eof()) {
                throw std::runtime_error("Source file grew while it was being packed: " + path_for_error(item.second));
            }
            file.size = original.size();
            file.checksum = sha256_bytes(original.data(), original.size());
            if (!original.empty()) {
                const std::size_t sample_size = std::min(kEntropySampleLimit, original.size());
                entropy_sample.assign(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(sample_size));
            }
            source_total = checked_add(source_total, file.size, "Total source size exceeds the archive format limit");

            const auto transformed = precompress_zlib_streams(original);
            file.precomp = transformed.records;
            file.stored_size = transformed.transformed.size();
            if (!file.precomp.empty()) {
                info.precomp_zlib_version = precomp_zlib_version();
                if (progress) progress("Precompressed " + std::to_string(file.precomp.size()) + " exact zlib stream(s) in " + item.first);
            }
            stored_total = checked_add(stored_total, file.stored_size, "Transformed source size overflow");
            if (!transformed.transformed.empty()) {
                chunker.push(transformed.transformed.data(), transformed.transformed.size(),
                             [&](std::vector<std::uint8_t>&& chunk) { append_chunk(file, file_offset, chunk); });
            }
        } else {
            std::ifstream input(item.second, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open source file: " + path_for_error(item.second));
            Sha256Hasher file_hasher;
            while (input) {
                input.read(read_buffer.data(), static_cast<std::streamsize>(read_buffer.size()));
                const std::streamsize read_count = input.gcount();
                if (read_count < 0) throw std::runtime_error("Invalid read count for: " + path_for_error(item.second));
                if (read_count == 0) break;
                const auto count = static_cast<std::size_t>(read_count);
                file_hasher.update(read_buffer.data(), count);
                if (entropy_sample.size() < kEntropySampleLimit) {
                    const std::size_t sample_take = std::min(kEntropySampleLimit - entropy_sample.size(), count);
                    const auto* bytes = reinterpret_cast<const std::uint8_t*>(read_buffer.data());
                    entropy_sample.insert(entropy_sample.end(), bytes, bytes + sample_take);
                }
                chunker.push(reinterpret_cast<const std::uint8_t*>(read_buffer.data()), count,
                             [&](std::vector<std::uint8_t>&& chunk) { append_chunk(file, file_offset, chunk); });
                file.size = checked_add(file.size, count, "Source file size exceeds the archive format limit");
                source_total = checked_add(source_total, count, "Total source size exceeds the archive format limit");
                stored_total = checked_add(stored_total, count, "Transformed source size overflow");
            }
            if (!input.eof()) throw std::runtime_error("Read error while packing: " + path_for_error(item.second));
            file.checksum = file_hasher.finish();
        }

        chunker.finish([&](std::vector<std::uint8_t>&& chunk) { append_chunk(file, file_offset, chunk); });
        if (!can_precomp) file.stored_size = file_offset;
        if (file_offset != file.stored_size) throw std::runtime_error("Internal transformed size mismatch while packing: " + file.path);
        const auto& classification = group_by_path.at(item.first);
        const double entropy = entropy_by_path.at(item.first);
        if (progress) {
            std::ostringstream message;
            message << "Scanned " << item.first << " | " << classification << " | entropy "
                    << std::fixed << std::setprecision(2) << entropy << " bits/byte";
            progress(message.str());
        }
        info.files.push_back(std::move(file));
    }
    flush_block();
    while (!pending_blocks.empty()) commit_ready_block();
    spool.flush();
    if (!spool) throw std::runtime_error("Failed to flush temporary compressed data");
    if (info.blocks.size() != created_block_count) throw std::runtime_error("Internal block table count mismatch after compression");
    spool.close();
    if (options.deduplicate) {
        raw_spool.flush();
        if (!raw_spool) throw std::runtime_error("Failed to flush temporary deduplication data");
        raw_spool.close();
    }

    validate_manifest(info);
    const auto manifest = serialize_manifest(info);
    std::ofstream output(final_temp_path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create temporary archive file");
    write_header(output, manifest, info.version);
    std::ifstream compressed_input(spool_path, std::ios::binary);
    if (!compressed_input) throw std::runtime_error("Cannot reopen temporary compressed data");
    copy_stream(compressed_input, output, "assembling final archive");
    output.flush();
    if (!output) throw std::runtime_error("Failed to finalize archive");
    output.close();

    if (std::filesystem::exists(output_abs)) {
        if (!options.overwrite) throw std::runtime_error("Output archive appeared during packing; refusing to replace it");
        std::filesystem::remove(output_abs, ec);
        if (ec) throw std::runtime_error("Cannot replace existing archive: " + path_for_error(output_abs) + " (" + ec.message() + ")");
    }
    std::filesystem::rename(final_temp_path, output_abs, ec);
    if (ec) throw std::runtime_error("Cannot move completed archive into place: " + path_for_error(output_abs) + " (" + ec.message() + ")");
    if (progress) {
        const std::uint64_t deduplicated = stored_total >= unique_total ? stored_total - unique_total : 0;
        progress("Precompression input: " + std::to_string(stored_total) + " bytes; deduplication removed " + std::to_string(deduplicated) + " transformed bytes");
        progress("Archive created: " + path_for_error(output_abs));
    }
}

ArchiveInfo read_archive(const std::filesystem::path& archive_path) {
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open archive: " + path_for_error(archive_path));
    input.seekg(0, std::ios::end);
    const auto end_position = input.tellg();
    if (end_position < 0) throw std::runtime_error("Cannot determine archive size");
    const auto archive_size = static_cast<std::uint64_t>(end_position);
    input.seekg(0, std::ios::beg);

    std::array<std::uint8_t, 8> magic{};
    read_exact(input, magic.data(), magic.size(), "reading archive signature");
    if (magic != kMagic) throw std::runtime_error("This file is not an RPK archive");

    auto read_u32 = [&input]() {
        std::array<std::uint8_t, 4> bytes{};
        read_exact(input, bytes.data(), bytes.size(), "reading archive header");
        std::uint32_t value = 0;
        for (unsigned int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[i]) << (i * 8U);
        return value;
    };
    auto read_u64 = [&input]() {
        std::array<std::uint8_t, 8> bytes{};
        read_exact(input, bytes.data(), bytes.size(), "reading archive header");
        std::uint64_t value = 0;
        for (unsigned int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
        return value;
    };

    const std::uint32_t version = read_u32();
    const std::uint32_t flags = read_u32();
    const std::uint64_t manifest_size = read_u64();
    const Sha256 expected_hash = [&input]() {
        Sha256 value{};
        read_exact(input, value.data(), value.size(), "reading manifest checksum");
        return value;
    }();
    if (version < 1 || version > kFormatVersion) throw std::runtime_error("Unsupported RPK archive version: " + std::to_string(version));
    if (flags != 0) throw std::runtime_error("Archive uses unsupported header flags");
    constexpr std::uint64_t header_size = 8ULL + 4ULL + 4ULL + 8ULL + 32ULL;
    if (manifest_size > kMaximumManifestSize || manifest_size > archive_size || header_size + manifest_size > archive_size) {
        throw std::runtime_error("Archive manifest size is invalid or exceeds the safety limit");
    }
    std::vector<std::uint8_t> manifest(static_cast<std::size_t>(manifest_size));
    read_exact(input, manifest.data(), manifest.size(), "reading archive manifest");
    const Sha256 actual_hash = sha256_bytes(manifest.data(), manifest.size());
    if (!constant_time_equal(expected_hash, actual_hash)) throw std::runtime_error("Archive manifest SHA-256 mismatch; the archive may be damaged");

    ArchiveInfo info = parse_manifest(manifest, version);
    info.version = version;
    info.payload_start = header_size + manifest_size;
    info.archive_size = archive_size;
    std::uint64_t payload_end = 0;
    for (const auto& block : info.blocks) {
        if (block.payload_offset != payload_end) throw std::runtime_error("Archive contains a gap or overlap in its block payload table");
        payload_end = checked_add(payload_end, block.compressed_size, "Archive payload size overflow");
    }
    if (checked_add(info.payload_start, payload_end, "Archive file size overflow") != archive_size) {
        throw std::runtime_error("Archive payload size does not match the file length");
    }
    return info;
}

void extract_archive(const std::filesystem::path& archive_path,
                     const std::filesystem::path& destination,
                     bool overwrite,
                     ProgressCallback progress) {
    const ArchiveInfo info = read_archive(archive_path);
    const bool has_precomp = std::any_of(info.files.begin(), info.files.end(), [](const FileRecord& file) { return !file.precomp.empty(); });
    if (has_precomp && info.precomp_zlib_version != precomp_zlib_version()) {
        throw std::runtime_error("This archive requires zlib " + info.precomp_zlib_version + " to rebuild embedded DEFLATE streams exactly; current runtime is " + precomp_zlib_version());
    }
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) throw std::runtime_error("Cannot create extraction folder: " + path_for_error(destination) + " (" + ec.message() + ")");
    const auto root = std::filesystem::canonical(destination, ec);
    if (ec) throw std::runtime_error("Cannot resolve extraction folder: " + path_for_error(destination));

    std::uint64_t required_space = 0;
    for (const auto& file : info.files) {
        required_space = checked_add(required_space, file.stored_size, "Total staged size exceeds the supported range");
        if (!file.precomp.empty()) required_space = checked_add(required_space, file.size, "Total reconstruction size exceeds the supported range");
    }
    std::uint64_t largest_block = 0;
    for (const auto& block : info.blocks) largest_block = std::max(largest_block, block.uncompressed_size);
    required_space = checked_add(required_space, largest_block, "Required extraction space exceeds the supported range");
    const auto space_info = std::filesystem::space(root, ec);
    if (!ec && space_info.available < required_space) {
        throw std::runtime_error("Insufficient free disk space: need approximately " + std::to_string(required_space / (1024ULL * 1024ULL)) +
                                 " MiB, available " + std::to_string(space_info.available / (1024ULL * 1024ULL)) + " MiB");
    }

    for (const auto& directory : info.directories) check_existing_directory_target(root, directory);
    for (const auto& file : info.files) {
        const auto target = root / from_utf8(file.path);
        const auto parent = target.parent_path();
        if (parent != root) check_existing_parent_chain(root, parent.lexically_relative(root).generic_u8string());
        ensure_no_unsafe_existing_target(target, overwrite);
    }

    TempDirectory stage(unique_child(root, ".rpk-stage-"));
    const auto work_path = unique_child(stage.path(), ".rpk-work-");
    std::filesystem::create_directory(work_path, ec);
    if (ec) throw std::runtime_error("Cannot create private extraction work folder: " + ec.message());
    for (const auto& directory : info.directories) {
        std::filesystem::create_directories(stage.path() / from_utf8(directory), ec);
        if (ec) throw std::runtime_error("Cannot create staging directory: " + directory + " (" + ec.message() + ")");
    }
    std::vector<std::filesystem::path> staged_files;
    staged_files.reserve(info.files.size());
    for (const auto& file : info.files) {
        const auto staged = stage.path() / from_utf8(file.path);
        std::filesystem::create_directories(staged.parent_path(), ec);
        if (ec) throw std::runtime_error("Cannot create staging folder for: " + file.path + " (" + ec.message() + ")");
        {
            std::ofstream create(staged, std::ios::binary | std::ios::trunc);
            if (!create) throw std::runtime_error("Cannot create staging file: " + file.path);
        }
        std::filesystem::resize_file(staged, file.stored_size, ec);
        if (ec) throw std::runtime_error("Cannot allocate staging file: " + file.path + " (" + ec.message() + ")");
        staged_files.push_back(staged);
    }

    std::vector<std::vector<std::pair<std::size_t, Extent>>> extents_by_block(info.blocks.size());
    for (std::size_t file_index = 0; file_index < info.files.size(); ++file_index) {
        for (const auto& extent : info.files[file_index].extents) {
            extents_by_block[extent.block_index].emplace_back(file_index, extent);
        }
    }

    std::ifstream archive(archive_path, std::ios::binary);
    if (!archive) throw std::runtime_error("Cannot reopen archive for extraction");
    const auto raw_block = work_path / "verified-block.tmp";
    for (std::size_t block_index = 0; block_index < info.blocks.size(); ++block_index) {
        decompress_block(archive, info, info.blocks[block_index], raw_block);
        std::ifstream raw(raw_block, std::ios::binary);
        if (!raw) throw std::runtime_error("Cannot read verified temporary block");
        for (const auto& item : extents_by_block[block_index]) {
            const std::size_t file_index = item.first;
            const Extent& extent = item.second;
            raw.clear();
            raw.seekg(static_cast<std::streamoff>(extent.block_offset), std::ios::beg);
            if (!raw) throw std::runtime_error("Cannot seek within a verified block");
            std::fstream output(staged_files[file_index], std::ios::binary | std::ios::in | std::ios::out);
            if (!output) throw std::runtime_error("Cannot open staging file for writing: " + info.files[file_index].path);
            output.seekp(static_cast<std::streamoff>(extent.file_offset), std::ios::beg);
            if (!output) throw std::runtime_error("Cannot seek within staging file: " + info.files[file_index].path);
            std::vector<char> buffer(kIoBufferSize);
            std::uint64_t remaining = extent.length;
            while (remaining > 0) {
                const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
                raw.read(buffer.data(), chunk);
                if (raw.gcount() != chunk) throw std::runtime_error("Verified block ended unexpectedly while restoring: " + info.files[file_index].path);
                output.write(buffer.data(), chunk);
                if (!output) throw std::runtime_error("Disk write failed while restoring: " + info.files[file_index].path);
                remaining -= static_cast<std::uint64_t>(chunk);
            }
        }
        raw.close();
        std::filesystem::remove(raw_block, ec);
        if (ec) throw std::runtime_error("Cannot remove temporary decompressed block: " + ec.message());
        if (progress) progress("Verified and restored solid block " + std::to_string(block_index + 1) + "/" + std::to_string(info.blocks.size()));
    }

    for (std::size_t i = 0; i < info.files.size(); ++i) {
        const auto& file = info.files[i];
        if (!file.precomp.empty()) {
            if (file.stored_size > 256ULL * 1024ULL * 1024ULL) throw std::runtime_error("Precompressed file exceeds the safe reconstruction memory limit: " + file.path);
            std::ifstream encoded_input(staged_files[i], std::ios::binary);
            if (!encoded_input) throw std::runtime_error("Cannot read staged precompressed file: " + file.path);
            std::vector<std::uint8_t> transformed(static_cast<std::size_t>(file.stored_size));
            if (!transformed.empty()) {
                encoded_input.read(reinterpret_cast<char*>(transformed.data()), static_cast<std::streamsize>(transformed.size()));
                if (encoded_input.gcount() != static_cast<std::streamsize>(transformed.size())) throw std::runtime_error("Staged precompressed file is truncated: " + file.path);
            }
            // Windows does not allow deleting/replacing a file while its input handle remains open.
            encoded_input.close();
            if (encoded_input.fail()) {
                throw std::runtime_error("Cannot close staged precompressed file: " + file.path);
            }
            const auto rebuilt = rebuild_zlib_streams(transformed, file.precomp);
            if (rebuilt.size() != file.size) throw std::runtime_error("Rebuilt file size does not match the original size: " + file.path);
            const auto rebuilt_path = work_path / ("precomp-rebuilt-" + std::to_string(i));
            {
                std::ofstream rebuilt_file(rebuilt_path, std::ios::binary | std::ios::trunc);
                if (!rebuilt_file) throw std::runtime_error("Cannot create precompression reconstruction file: " + file.path);
                write_exact(rebuilt_file, rebuilt.data(), rebuilt.size(), "writing reconstructed file");
                rebuilt_file.flush();
                if (!rebuilt_file) throw std::runtime_error("Failed to flush reconstructed file: " + file.path);
            }
            std::filesystem::remove(staged_files[i], ec);
            if (ec) throw std::runtime_error("Cannot replace staged precompressed file: " + file.path + " (" + ec.message() + ")");
            std::filesystem::rename(rebuilt_path, staged_files[i], ec);
            if (ec) throw std::runtime_error("Cannot install reconstructed file into staging: " + file.path + " (" + ec.message() + ")");
        } else if (file.stored_size != file.size) {
            throw std::runtime_error("Archive contains inconsistent stored/original sizes without precompression metadata: " + file.path);
        }
    }

    for (std::size_t i = 0; i < info.files.size(); ++i) {
        const Sha256 actual = sha256_file(staged_files[i]);
        if (!constant_time_equal(actual, info.files[i].checksum)) {
            throw std::runtime_error("File SHA-256 mismatch after extraction: " + info.files[i].path);
        }
    }

    for (const auto& directory : info.directories) safe_directory(root, directory, true);
    for (std::size_t i = 0; i < info.files.size(); ++i) {
        const auto& file = info.files[i];
        const auto target = root / from_utf8(file.path);
        const auto parent = target.parent_path();
        if (parent != root) safe_directory(root, parent.lexically_relative(root).generic_u8string(), true);
        ensure_no_unsafe_existing_target(target, overwrite);
        if (overwrite && std::filesystem::exists(target)) {
            set_file_read_only(target, false);
            std::filesystem::remove(target, ec);
            if (ec) throw std::runtime_error("Cannot replace existing file: " + path_for_error(target) + " (" + ec.message() + ")");
        }
        std::filesystem::rename(staged_files[i], target, ec);
        if (ec) throw std::runtime_error("Cannot install extracted file: " + file.path + " (" + ec.message() + ")");
        set_file_read_only(target, file.read_only);
        if (progress) progress("Extracted and hash-verified: " + file.path);
    }
}

void verify_archive(const std::filesystem::path& archive_path, ProgressCallback progress) {
    const auto verify_root = unique_child(std::filesystem::temp_directory_path(), "rpk-verify-");
    TempDirectory temporary(verify_root);
    extract_archive(archive_path, temporary.path(), false, std::move(progress));
}

} // namespace rpk
