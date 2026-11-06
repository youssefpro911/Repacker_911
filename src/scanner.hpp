#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rpk {

struct ScannedFile {
    std::string path;
    std::uint64_t size = 0;
    std::string extension;
    std::string classification;
    double entropy_bits_per_byte = 0.0;
    std::uint64_t sampled_bytes = 0;
};

struct ScanReport {
    std::uint64_t total_bytes = 0;
    std::uint64_t total_files = 0;
    std::uint64_t total_directories = 0;
    std::map<std::string, std::uint64_t> files_by_type;
    std::map<std::string, std::uint64_t> bytes_by_type;
    std::vector<ScannedFile> files;
    std::vector<std::string> directories;
};

using ScanProgressCallback = std::function<void(const std::string&)>;

ScanReport scan_directory(const std::filesystem::path& root,
                          ScanProgressCallback progress = {});
std::string classify_file_type(const std::string& extension,
                               const std::vector<std::uint8_t>& sample);
double estimate_entropy_bits_per_byte(const std::vector<std::uint8_t>& sample);

} // namespace rpk
