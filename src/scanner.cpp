#include "scanner.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace rpk {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string extension_of(const std::filesystem::path& path) {
    const auto value = path.extension().u8string();
    return lower_ascii(value);
}

std::vector<std::uint8_t> read_sample(const std::filesystem::path& path, std::uint64_t file_size) {
    constexpr std::size_t kSampleLimit = 256U * 1024U;
    constexpr std::size_t kHeadLimit = 32U * 1024U;
    std::vector<std::uint8_t> sample;
    const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(file_size, kSampleLimit));
    sample.resize(wanted);
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open file while scanning: " + path.u8string());
    if (wanted != 0) {
        const std::size_t head = std::min(wanted, kHeadLimit);
        input.read(reinterpret_cast<char*>(sample.data()), static_cast<std::streamsize>(head));
        const auto count = input.gcount();
        if (count < 0) throw std::runtime_error("Invalid sample read while scanning: " + path.u8string());
        sample.resize(static_cast<std::size_t>(count));
    }
    return sample;
}

bool starts_with(const std::vector<std::uint8_t>& bytes, std::initializer_list<std::uint8_t> signature) {
    if (bytes.size() < signature.size()) return false;
    return std::equal(signature.begin(), signature.end(), bytes.begin());
}

bool is_zlib_header(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 2) return false;
    const unsigned int cmf = bytes[0];
    const unsigned int flg = bytes[1];
    return (cmf & 0x0fU) == 8U && (cmf >> 4U) <= 7U && (((cmf << 8U) | flg) % 31U) == 0U;
}

std::string classify_magic(const std::vector<std::uint8_t>& bytes) {
    if (starts_with(bytes, {0x4d, 0x5a})) return "Windows executable / PE";
    if (starts_with(bytes, {0x7f, 'E', 'L', 'F'})) return "ELF executable";
    if (starts_with(bytes, {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a})) return "PNG image";
    if (starts_with(bytes, {0xff, 0xd8, 0xff})) return "JPEG image";
    if (starts_with(bytes, {'D', 'D', 'S', ' '})) return "DDS texture";
    if (starts_with(bytes, {0xab, 'K', 'T', 'X', ' ', '2', '0', 0xbb})) return "KTX texture";
    if (starts_with(bytes, {'R', 'I', 'F', 'F'}) && bytes.size() >= 12 &&
        std::equal(bytes.begin() + 8, bytes.begin() + 12, std::array<std::uint8_t, 4>{'W', 'A', 'V', 'E'}.begin())) return "WAV audio";
    if (starts_with(bytes, {'O', 'g', 'g', 'S'})) return "Ogg audio";
    if (starts_with(bytes, {'I', 'D', '3'})) return "MP3 audio";
    if (starts_with(bytes, {'P', 'K', 0x03, 0x04}) || starts_with(bytes, {'P', 'K', 0x05, 0x06})) return "ZIP archive";
    if (starts_with(bytes, {0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c})) return "7-Zip archive";
    if (starts_with(bytes, {'R', 'a', 'r', '!', 0x1a, 0x07})) return "RAR archive";
    if (starts_with(bytes, {0x1f, 0x8b})) return "Gzip stream";
    if (starts_with(bytes, {'%', 'P', 'D', 'F', '-'})) return "PDF document";
    if (bytes.size() >= 8 && std::equal(bytes.begin() + 4, bytes.begin() + 8,
                                       std::array<std::uint8_t, 4>{'f', 't', 'y', 'p'}.begin())) return "ISO media / MP4";
    if (is_zlib_header(bytes)) return "Possible zlib / DEFLATE stream";
    if (!bytes.empty()) {
        const auto printable = std::count_if(bytes.begin(), bytes.end(), [](std::uint8_t value) {
            return value == 9U || value == 10U || value == 13U || (value >= 32U && value < 127U) || value >= 0xc2U;
        });
        if (std::find(bytes.begin(), bytes.end(), 0U) == bytes.end() &&
            static_cast<double>(printable) / static_cast<double>(bytes.size()) > 0.92) return "Text / unknown";
    }
    return "Unknown binary";
}

} // namespace

double estimate_entropy_bits_per_byte(const std::vector<std::uint8_t>& sample) {
    if (sample.empty()) return 0.0;
    std::array<std::uint64_t, 256> counts{};
    for (const auto value : sample) ++counts[value];
    const double size = static_cast<double>(sample.size());
    double entropy = 0.0;
    for (const auto count : counts) {
        if (count == 0) continue;
        const double probability = static_cast<double>(count) / size;
        entropy -= probability * std::log2(probability);
    }
    return entropy;
}

std::string classify_file_type(const std::string& extension,
                               const std::vector<std::uint8_t>& sample) {
    const std::string ext = lower_ascii(extension);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".dds" ||
        ext == ".ktx" || ext == ".ktx2" || ext == ".tga" || ext == ".bmp" || ext == ".gif" || ext == ".astc") return "Image / texture";
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac" || ext == ".wem" ||
        ext == ".bank" || ext == ".opus" || ext == ".aac") return "Audio";
    if (ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" || ext == ".bik" || ext == ".bk2" || ext == ".mov") return "Video";
    if (ext == ".zip" || ext == ".7z" || ext == ".rar" || ext == ".pak" || ext == ".vpk" ||
        ext == ".rpf" || ext == ".bundle" || ext == ".assets" || ext == ".cab" || ext == ".xz") return "Archive / package";
    if (ext == ".txt" || ext == ".json" || ext == ".xml" || ext == ".yaml" || ext == ".yml" ||
        ext == ".ini" || ext == ".cfg" || ext == ".lua" || ext == ".cs" || ext == ".cpp" ||
        ext == ".h" || ext == ".hpp" || ext == ".py" || ext == ".js" || ext == ".toml") return "Text / script";
    if (ext == ".exe" || ext == ".dll" || ext == ".sys" || ext == ".com") return "Executable / binary";
    if (ext == ".ttf" || ext == ".otf" || ext == ".woff" || ext == ".woff2") return "Font";
    return classify_magic(sample);
}

ScanReport scan_directory(const std::filesystem::path& root, ScanProgressCallback progress) {
    std::error_code ec;
    const auto canonical_root = std::filesystem::canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonical_root)) {
        throw std::runtime_error("Source folder does not exist or is not a directory: " + root.u8string());
    }

    ScanReport report;
    std::filesystem::recursive_directory_iterator iterator(canonical_root, std::filesystem::directory_options::none, ec);
    if (ec) throw std::runtime_error("Cannot scan source folder: " + ec.message());
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const auto path = iterator->path();
        const auto status = iterator->symlink_status(ec);
        if (ec) throw std::runtime_error("Cannot inspect source entry: " + path.u8string() + " (" + ec.message() + ")");
        if (std::filesystem::is_symlink(status)) throw std::runtime_error("Symbolic links are not supported: " + path.u8string());
        const std::string relative = path.lexically_relative(canonical_root).generic_u8string();
        if (std::filesystem::is_directory(status)) {
            report.directories.push_back(relative);
            ++report.total_directories;
        } else if (std::filesystem::is_regular_file(status)) {
            const auto size = std::filesystem::file_size(path, ec);
            if (ec) throw std::runtime_error("Cannot read file size: " + path.u8string() + " (" + ec.message() + ")");
            const auto sample = read_sample(path, size);
            ScannedFile file;
            file.path = relative;
            file.size = size;
            file.extension = extension_of(path);
            file.classification = classify_file_type(file.extension, sample);
            file.entropy_bits_per_byte = estimate_entropy_bits_per_byte(sample);
            file.sampled_bytes = sample.size();
            report.files_by_type[file.classification]++;
            auto& type_bytes = report.bytes_by_type[file.classification];
            if (size > UINT64_MAX - type_bytes) throw std::runtime_error("Per-type source size overflow while scanning");
            type_bytes += size;
            if (size > UINT64_MAX - report.total_bytes) throw std::runtime_error("Total source size overflow while scanning");
            report.total_bytes += size;
            report.files.push_back(std::move(file));
            ++report.total_files;
        } else {
            throw std::runtime_error("Unsupported filesystem object: " + path.u8string());
        }
        iterator.increment(ec);
        if (ec) throw std::runtime_error("Error while scanning source folder: " + ec.message());
    }
    std::sort(report.files.begin(), report.files.end(), [](const ScannedFile& a, const ScannedFile& b) { return a.path < b.path; });
    std::sort(report.directories.begin(), report.directories.end());
    if (progress) progress("Scan complete: " + std::to_string(report.total_files) + " files, " + std::to_string(report.total_directories) + " directories");
    return report;
}

} // namespace rpk
