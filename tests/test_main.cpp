#include "archive.hpp"
#include "chunker.hpp"
#include "scanner.hpp"
#include "hash.hpp"
#include "precomp.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TestDirectory {
public:
    TestDirectory() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("rpk-test-" + std::to_string(tick));
        if (!std::filesystem::create_directories(path_)) throw std::runtime_error("Cannot create test directory");
    }
    ~TestDirectory() {
        std::error_code ec;
        if (std::filesystem::exists(path_, ec)) {
            for (const auto& item : std::filesystem::recursive_directory_iterator(path_, ec)) {
                if (item.is_regular_file(ec)) {
                    std::filesystem::permissions(item.path(), std::filesystem::perms::owner_write,
                                                 std::filesystem::perm_options::add, ec);
                    ec.clear();
                }
            }
        }
        ec.clear();
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot write test file: " + path.u8string());
    if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Test file write failed: " + path.u8string());
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read test file: " + path.u8string());
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) throw std::runtime_error("Cannot get test file size");
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof()) throw std::runtime_error("Test file read failed");
    return bytes;
}

std::vector<std::uint8_t> patterned(std::size_t size, std::uint32_t seed) {
    std::vector<std::uint8_t> bytes(size);
    std::uint32_t state = seed;
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1664525U + 1013904223U;
        bytes[i] = static_cast<std::uint8_t>((state >> 16U) & 0xffU);
    }
    return bytes;
}


std::vector<std::uint8_t> zlib_compress(const std::vector<std::uint8_t>& raw, int level) {
    z_stream stream{};
    if (deflateInit2(&stream, level, Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("Test zlib init failed");
    }
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(deflateBound(&stream, static_cast<uLong>(raw.size()))));
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(raw.data()));
    stream.avail_in = static_cast<uInt>(raw.size());
    stream.next_out = encoded.data();
    stream.avail_out = static_cast<uInt>(encoded.size());
    const int status = deflate(&stream, Z_FINISH);
    const auto produced = static_cast<std::size_t>(stream.total_out);
    deflateEnd(&stream);
    if (status != Z_STREAM_END) throw std::runtime_error("Test zlib compression failed");
    encoded.resize(produced);
    return encoded;
}

void test_precomp_roundtrip() {
    std::vector<std::uint8_t> raw(200U * 1024U);
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::uint8_t>((i / 19U) % 17U);
    const auto compressed = zlib_compress(raw, 6);
    std::vector<std::uint8_t> original{'P','R','E','F','I','X'};
    original.insert(original.end(), compressed.begin(), compressed.end());
    original.insert(original.end(), {'S','U','F','F','I','X'});

    const auto packed = rpk::precompress_zlib_streams(original);
    require(packed.records.size() == 1, "Precompressor did not detect the valid embedded zlib stream");
    require(packed.transformed.size() > original.size(), "Precompressor should replace the zlib payload with raw data");
    const auto rebuilt = rpk::rebuild_zlib_streams(packed.transformed, packed.records);
    require(rebuilt == original, "Precompressor round-trip was not byte-identical");

    auto invalid = packed.records;
    invalid[0].level = (invalid[0].level + 1U) % 10U;
    bool rejected = false;
    try { (void)rpk::rebuild_zlib_streams(packed.transformed, invalid); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "Precompressor accepted an incorrect re-encoding parameter");
}

void test_sha256() {
    const std::string input = "abc";
    const auto digest = rpk::sha256_bytes(input.data(), input.size());
    require(rpk::sha256_hex(digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known-answer test failed");
}

void compare_trees(const std::filesystem::path& source, const std::filesystem::path& restored) {
    std::vector<std::string> source_files;
    std::vector<std::string> restored_files;
    std::vector<std::string> source_dirs;
    std::vector<std::string> restored_dirs;
    for (const auto& item : std::filesystem::recursive_directory_iterator(source)) {
        const auto rel = item.path().lexically_relative(source).generic_u8string();
        if (item.is_directory()) source_dirs.push_back(rel);
        else if (item.is_regular_file()) source_files.push_back(rel);
    }
    for (const auto& item : std::filesystem::recursive_directory_iterator(restored)) {
        const auto rel = item.path().lexically_relative(restored).generic_u8string();
        if (item.is_directory()) restored_dirs.push_back(rel);
        else if (item.is_regular_file()) restored_files.push_back(rel);
    }
    std::sort(source_files.begin(), source_files.end());
    std::sort(restored_files.begin(), restored_files.end());
    std::sort(source_dirs.begin(), source_dirs.end());
    std::sort(restored_dirs.begin(), restored_dirs.end());
    require(source_files == restored_files, "Restored file list differs from source");
    require(source_dirs == restored_dirs, "Restored directory list differs from source");
    for (const auto& relative : source_files) {
        const auto left = read_bytes(source / std::filesystem::u8path(relative));
        const auto right = read_bytes(restored / std::filesystem::u8path(relative));
        require(left == right, "Restored bytes differ for: " + relative);
        require(rpk::constant_time_equal(rpk::sha256_file(source / std::filesystem::u8path(relative)),
                                         rpk::sha256_file(restored / std::filesystem::u8path(relative))),
                "Restored SHA-256 differs for: " + relative);
    }
}


void test_chunker_determinism() {
    const auto input = patterned(5U * 1024U * 1024U + 731U, 0x12345678U);
    auto collect = [&](std::size_t stride) {
        rpk::ContentDefinedChunker chunker;
        std::vector<std::size_t> lengths;
        std::uint64_t total = 0;
        for (std::size_t offset = 0; offset < input.size();) {
            const std::size_t count = std::min(stride, input.size() - offset);
            chunker.push(input.data() + offset, count, [&](std::vector<std::uint8_t>&& chunk) {
                require(!chunk.empty(), "Chunker emitted an empty chunk");
                require(chunk.size() <= 1024U * 1024U, "Chunker exceeded its maximum size");
                lengths.push_back(chunk.size());
                total += chunk.size();
            });
            offset += count;
        }
        chunker.finish([&](std::vector<std::uint8_t>&& chunk) {
            require(!chunk.empty(), "Chunker emitted an empty final chunk");
            lengths.push_back(chunk.size());
            total += chunk.size();
        });
        require(total == input.size(), "Chunker lost or duplicated input bytes");
        return lengths;
    };
    require(collect(4096) == collect(1024U * 1024U), "Chunk boundaries depend on input read-buffer size");
}

void test_scanner() {
    TestDirectory temporary;
    const auto source = temporary.path() / "scan-source";
    std::filesystem::create_directories(source / "textures");
    std::vector<std::uint8_t> dds(128U * 1024U, 0x24U);
    dds[0] = 'D'; dds[1] = 'D'; dds[2] = 'S'; dds[3] = ' ';
    write_bytes(source / "textures" / "sample.bin", dds);
    write_bytes(source / "readme.txt", std::vector<std::uint8_t>{'H','e','l','l','o',' ','R','P','K','!'});
    const auto report = rpk::scan_directory(source);
    require(report.total_files == 2, "Scanner returned the wrong file count");
    require(report.total_directories == 1, "Scanner returned the wrong directory count");
    require(report.total_bytes == dds.size() + 10U, "Scanner returned the wrong source size");
    bool detected_dds = false;
    bool detected_text = false;
    for (const auto& file : report.files) {
        if (file.path == "textures/sample.bin") detected_dds = file.classification == "DDS texture";
        if (file.path == "readme.txt") detected_text = file.classification == "Text / script";
    }
    require(detected_dds, "Scanner did not classify a DDS magic signature");
    require(detected_text, "Scanner did not classify a known text extension");
    const double entropy = rpk::estimate_entropy_bits_per_byte(std::vector<std::uint8_t>(1024, 0x5aU));
    require(entropy < 0.001, "Entropy estimator should report near-zero entropy for a constant sample");
}

void test_pack_extract_and_corruption() {
    TestDirectory temporary;
    const auto source = temporary.path() / "source";
    const auto archive = temporary.path() / "sample.rpk";
    const auto restored = temporary.path() / "restored";
    std::filesystem::create_directories(source / "empty-folder");
    write_bytes(source / "hello.txt", std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o', '\n'});
    write_bytes(source / "zero.dat", {});
    write_bytes(source / std::filesystem::u8path("nested/π-文件.bin"), patterned(2U * 1024U * 1024U + 153U, 17U));
    write_bytes(source / "nested" / "second.bin", patterned(700U * 1024U, 99U));
    const std::vector<std::uint8_t> repeated(400U * 1024U, 0x41U);
    write_bytes(source / "nested" / "repeat.bin", repeated);
    write_bytes(source / "nested" / "repeat-copy.bin", repeated);
    std::vector<std::uint8_t> precomp_raw(180U * 1024U);
    for (std::size_t i = 0; i < precomp_raw.size(); ++i) precomp_raw[i] = static_cast<std::uint8_t>((i / 23U) % 13U);
    const auto precomp_stream = zlib_compress(precomp_raw, 6);
    std::vector<std::uint8_t> embedded{'H','E','A','D'};
    embedded.insert(embedded.end(), precomp_stream.begin(), precomp_stream.end());
    embedded.insert(embedded.end(), {'T','A','I','L'});
    write_bytes(source / "nested" / "embedded-zlib.dat", embedded);
    std::vector<std::uint8_t> media(160U * 1024U, 0x31U);
    media[4] = 'f'; media[5] = 't'; media[6] = 'y'; media[7] = 'p';
    write_bytes(source / "sample.mp4", media);

    std::error_code ec;
    std::filesystem::permissions(source / "hello.txt", std::filesystem::perms::owner_write, std::filesystem::perm_options::remove, ec);
    require(!ec, "Could not mark test file read-only");

    rpk::PackOptions options;
    options.preset = 1;
    options.dictionary_mib = 16;
    options.block_size = 1024ULL * 1024ULL;
    rpk::pack_directory(source, archive, options);

    const auto info = rpk::read_archive(archive);
    require(info.version == 4, "New archives should use container format version 4");
    require(info.files.size() == 8, "Unexpected number of files in archive");
    require(info.directories.size() == 2, "Unexpected number of directories in archive");
    require(info.blocks.size() >= 3, "Expected multiple solid blocks");
    bool saw_lzma = false;
    bool saw_zstd = false;
    bool saw_store = false;
    for (const auto& block : info.blocks) {
        saw_lzma = saw_lzma || block.codec == rpk::Codec::Lzma2;
        saw_zstd = saw_zstd || block.codec == rpk::Codec::ZstdLong;
        saw_store = saw_store || block.codec == rpk::Codec::Store;
    }
    require(saw_lzma && saw_zstd && saw_store, "Automatic type-aware selection did not exercise all block codecs");
    std::uint64_t raw_bytes = 0;
    std::uint64_t stored_bytes = 0;
    for (const auto& file : info.files) raw_bytes += file.size;
    for (const auto& block : info.blocks) stored_bytes += block.uncompressed_size;
    require(stored_bytes < raw_bytes, "Duplicate chunks were not removed from the stored block data");
    bool found_read_only = false;
    for (const auto& file : info.files) {
        if (file.path == "hello.txt") found_read_only = file.read_only;
    }
    require(found_read_only, "Read-only attribute was not recorded");
    bool found_precomp = false;
    for (const auto& file : info.files) if (file.path == "nested/embedded-zlib.dat") found_precomp = !file.precomp.empty();
    require(found_precomp, "Archive did not record the exact-match embedded zlib transform");

    rpk::extract_archive(archive, restored, false);
    compare_trees(source, restored);
    const auto restored_permissions = std::filesystem::status(restored / "hello.txt").permissions();
    require((restored_permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none,
            "Read-only attribute was not restored");

    const auto corrupt = temporary.path() / "corrupt.rpk";
    std::filesystem::copy_file(archive, corrupt);
    const auto corrupt_info = rpk::read_archive(corrupt);
    require(!corrupt_info.blocks.empty(), "Corruption test archive has no blocks");
    {
        std::fstream file(corrupt, std::ios::binary | std::ios::in | std::ios::out);
        require(static_cast<bool>(file), "Cannot open archive for corruption test");
        const auto position = static_cast<std::streamoff>(corrupt_info.payload_start + corrupt_info.blocks.front().compressed_size / 2U);
        file.seekg(position);
        char byte = 0;
        file.read(&byte, 1);
        require(static_cast<bool>(file), "Cannot read byte for corruption test");
        byte = static_cast<char>(byte ^ 0x5a);
        file.seekp(position);
        file.write(&byte, 1);
        file.flush();
        require(static_cast<bool>(file), "Cannot corrupt archive test byte");
    }
    bool rejected = false;
    try {
        rpk::verify_archive(corrupt);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Corrupted archive was not rejected");

    const auto no_dedup_archive = temporary.path() / "no-dedup.rpk";
    rpk::PackOptions no_dedup_options = options;
    no_dedup_options.deduplicate = false;
    no_dedup_options.precompress = false;
    rpk::pack_directory(source, no_dedup_archive, no_dedup_options);
    const auto no_dedup_info = rpk::read_archive(no_dedup_archive);
    std::uint64_t no_dedup_stored = 0;
    for (const auto& block : no_dedup_info.blocks) no_dedup_stored += block.uncompressed_size;
    std::uint64_t no_precomp_expected = 0;
    for (const auto& file : no_dedup_info.files) no_precomp_expected += file.stored_size;
    require(no_dedup_stored == no_precomp_expected, "Disabling deduplication should store all transformed file bytes");
    require(no_dedup_info.version == 4, "New archives without transforms should still use format version 4");
    rpk::verify_archive(no_dedup_archive);
}

void test_legacy_archive_compatibility() {
    const std::filesystem::path fixture_dir = RPK_TEST_FIXTURE_DIR;
    TestDirectory temporary;
    for (const auto& item : std::vector<std::pair<std::string, std::uint32_t>>{
             {"legacy-v1.rpk", 1U}, {"legacy-v2.rpk", 2U}, {"legacy-v3.rpk", 3U}}) {
        const auto fixture = fixture_dir / item.first;
        require(std::filesystem::exists(fixture), "Missing legacy archive fixture: " + item.first);
        const auto local = temporary.path() / item.first;
        std::filesystem::copy_file(fixture, local);
        const auto info = rpk::read_archive(local);
        require(info.version == item.second, "Legacy archive version was not parsed correctly: " + item.first);
        if (item.second < 3U || info.precomp_zlib_version == rpk::precomp_zlib_version()) {
            rpk::verify_archive(local);
        }
    }
}

} // namespace

int main() {
    try {
        test_sha256();
        test_precomp_roundtrip();
        test_chunker_determinism();
        test_scanner();
        test_pack_extract_and_corruption();
        test_legacy_archive_compatibility();
        std::cout << "All RPK tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
