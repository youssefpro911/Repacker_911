#include "archive.hpp"
#include "scanner.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

std::string human_size(std::uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' ' << units[unit];
    return output.str();
}

void print_help() {
    std::cout <<
        "RPK - type-aware LZMA2 / Zstandard archive tool (Milestone 4)\n\n"
        "Usage:\n"
        "  rpk scan <source-folder>\n"
        "  rpk pack <source-folder> -o <archive.rpk> [options]\n"
        "  rpk list <archive.rpk>\n"
        "  rpk test <archive.rpk>\n"
        "  rpk extract <archive.rpk> -C <destination-folder> [--overwrite]\n\n"
        "Pack options:\n"
        "  --preset fast|balanced|max   LZMA preset (1, 6, or 9)\n"
        "  --dict-mib N                 LZMA2 dictionary size, 1..1024 MiB (default 64)\n"
        "  --block-size-mib N           Solid block size, 1..1024 MiB (default 64)\n"
        "  --codec auto|lzma2|zstd-long|store  Per-group codec selection (default auto)\n"
        "  --threads N|auto             Parallel block compression workers (default auto)\n"
        "  --memory-mib N               Approximate compression memory budget, 128..65536 MiB (default 1024)\n"
        "  --volume-size-mib N          Split output into .001, .002... parts; 0 disables (default 0)\n"
        "  --no-dedup                   Disable content-defined chunk deduplication\n"
        "  --no-precomp                 Disable exact-match embedded zlib precompression\n"
        "  --force                      Replace an existing output archive\n\n"
        "Extract options:\n"
        "  --overwrite                  Replace existing files after verification\n\n"
        "Examples:\n"
        "  rpk pack \"D:\\Games\\Example\" -o \"D:\\Backups\\Example.rpk\" --preset max --dict-mib 128\n"
        "  rpk test \"D:\\Backups\\Example.rpk\"\n"
        "  rpk extract \"D:\\Backups\\Example.rpk\" -C \"D:\\Games\\Restored\"\n";
}

std::uint64_t parse_unsigned(const std::string& value, const std::string& option) {
    if (value.empty() || value.front() == '-') throw std::runtime_error("Invalid value for " + option + ": " + value);
    std::size_t used = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(value, &used, 10);
    } catch (...) {
        throw std::runtime_error("Invalid value for " + option + ": " + value);
    }
    if (used != value.size()) throw std::runtime_error("Invalid value for " + option + ": " + value);
    return static_cast<std::uint64_t>(parsed);
}

std::uint32_t parse_preset(const std::string& value) {
    if (value == "fast") return 1;
    if (value == "balanced") return 6;
    if (value == "max") return 9;
    const auto numeric = parse_unsigned(value, "--preset");
    if (numeric < 1 || numeric > 9) throw std::runtime_error("--preset must be fast, balanced, max, or a number from 1 to 9");
    return static_cast<std::uint32_t>(numeric);
}

rpk::CodecMode parse_codec(const std::string& value) {
    if (value == "auto") return rpk::CodecMode::Auto;
    if (value == "lzma2") return rpk::CodecMode::Lzma2;
    if (value == "zstd-long") return rpk::CodecMode::ZstdLong;
    if (value == "store") return rpk::CodecMode::Store;
    throw std::runtime_error("--codec must be auto, lzma2, zstd-long, or store");
}

std::string codec_mode_name(rpk::CodecMode codec) {
    switch (codec) {
    case rpk::CodecMode::Auto: return "auto";
    case rpk::CodecMode::Lzma2: return "lzma2";
    case rpk::CodecMode::ZstdLong: return "zstd-long";
    case rpk::CodecMode::Store: return "store";
    }
    return "unknown";
}

std::filesystem::path path_from_arg(const std::string& value) {
    return std::filesystem::u8path(value.begin(), value.end());
}

int run(const std::vector<std::string>& args) {
    if (args.size() < 2 || args[1] == "--help" || args[1] == "-h" || args[1] == "help") {
        print_help();
        return args.size() < 2 ? 0 : 0;
    }

    const std::string command = args[1];
    if (command == "scan") {
        if (args.size() != 3) throw std::runtime_error("scan requires exactly one source folder");
        const auto report = rpk::scan_directory(path_from_arg(args[2]), [](const std::string& message) { std::cout << message << '\n'; });
        std::cout << "\nSource scan summary\n"
                  << "Files: " << report.total_files << " | Directories: " << report.total_directories
                  << " | Total size: " << human_size(report.total_bytes) << "\n\n";
        std::cout << std::left << std::setw(30) << "Classification" << std::right << std::setw(12) << "Files" << std::setw(16) << "Bytes" << '\n';
        for (const auto& item : report.files_by_type) {
            const auto byte_count = report.bytes_by_type.at(item.first);
            std::cout << std::left << std::setw(30) << item.first << std::right << std::setw(12) << item.second
                      << std::setw(16) << human_size(byte_count) << '\n';
        }
        if (!report.files.empty()) {
            std::cout << "\nFirst 20 file samples (entropy is estimated from the first 32 KiB):\n";
            const std::size_t limit = std::min<std::size_t>(20, report.files.size());
            for (std::size_t i = 0; i < limit; ++i) {
                const auto& file = report.files[i];
                std::cout << "  " << std::fixed << std::setprecision(2) << file.entropy_bits_per_byte
                          << " bits/byte | " << file.classification << " | " << file.path << '\n';
            }
        }
        return 0;
    }
    if (command == "pack") {
        if (args.size() < 4) throw std::runtime_error("pack requires a source folder and -o <archive>");
        const auto source = path_from_arg(args[2]);
        std::filesystem::path output;
        bool have_output = false;
        rpk::PackOptions options;
        for (std::size_t i = 3; i < args.size(); ++i) {
            const std::string& option = args[i];
            if (option == "-o" || option == "--output") {
                if (i + 1 >= args.size()) throw std::runtime_error(option + " requires a path");
                output = path_from_arg(args[++i]);
                have_output = true;
            } else if (option == "--preset") {
                if (i + 1 >= args.size()) throw std::runtime_error("--preset requires a value");
                options.preset = parse_preset(args[++i]);
            } else if (option == "--dict-mib") {
                if (i + 1 >= args.size()) throw std::runtime_error("--dict-mib requires a value");
                const auto value = parse_unsigned(args[++i], "--dict-mib");
                if (value < 1 || value > 1024) throw std::runtime_error("--dict-mib must be between 1 and 1024");
                options.dictionary_mib = static_cast<std::uint32_t>(value);
            } else if (option == "--block-size-mib") {
                if (i + 1 >= args.size()) throw std::runtime_error("--block-size-mib requires a value");
                const auto value = parse_unsigned(args[++i], "--block-size-mib");
                if (value < 1 || value > 1024) throw std::runtime_error("--block-size-mib must be between 1 and 1024");
                options.block_size = value * 1024ULL * 1024ULL;
            } else if (option == "--codec") {
                if (i + 1 >= args.size()) throw std::runtime_error("--codec requires a value");
                options.codec = parse_codec(args[++i]);
            } else if (option == "--threads") {
                if (i + 1 >= args.size()) throw std::runtime_error("--threads requires a value");
                const std::string value = args[++i];
                if (value == "auto") options.threads = 0;
                else {
                    const auto count = parse_unsigned(value, "--threads");
                    if (count < 1 || count > 64) throw std::runtime_error("--threads must be auto or between 1 and 64");
                    options.threads = static_cast<std::uint32_t>(count);
                }
            } else if (option == "--memory-mib") {
                if (i + 1 >= args.size()) throw std::runtime_error("--memory-mib requires a value");
                const auto value = parse_unsigned(args[++i], "--memory-mib");
                if (value < 128 || value > 65536) throw std::runtime_error("--memory-mib must be between 128 and 65536");
                options.memory_limit_mib = static_cast<std::uint32_t>(value);
            } else if (option == "--volume-size-mib") {
                if (i + 1 >= args.size()) throw std::runtime_error("--volume-size-mib requires a value");
                const auto value = parse_unsigned(args[++i], "--volume-size-mib");
                if (value > 4096) throw std::runtime_error("--volume-size-mib must be between 0 and 4096");
                options.volume_size_mib = static_cast<std::uint32_t>(value);
            } else if (option == "--no-dedup") {
                options.deduplicate = false;
            } else if (option == "--no-precomp") {
                options.precompress = false;
            } else if (option == "--force") {
                options.overwrite = true;
            } else {
                throw std::runtime_error("Unknown pack option: " + option);
            }
        }
        if (!have_output) throw std::runtime_error("pack requires -o <archive>");
        std::cout << "Packing with selected per-group codecs, preset " << options.preset
                  << ", dictionary " << options.dictionary_mib << " MiB, solid block "
                  << (options.block_size / (1024ULL * 1024ULL)) << " MiB"
                  << ", deduplication " << (options.deduplicate ? "enabled" : "disabled")
                  << ", codec " << codec_mode_name(options.codec)
                  << ", threads " << (options.threads == 0 ? "auto" : std::to_string(options.threads))
                  << ", memory budget " << options.memory_limit_mib << " MiB"
                  << ", volume size " << (options.volume_size_mib == 0 ? "single-file" : std::to_string(options.volume_size_mib) + " MiB")
                  << ", exact zlib precompression " << (options.precompress ? "enabled" : "disabled") << "\n";
        rpk::pack_directory(source, output, options, [](const std::string& message) { std::cout << message << '\n'; });
        return 0;
    }

    if (command == "list") {
        if (args.size() != 3) throw std::runtime_error("list requires exactly one archive path");
        const auto info = rpk::read_archive(path_from_arg(args[2]));
        std::uint64_t total_raw = 0;
        std::uint64_t total_compressed = 0;
        std::uint64_t stored_raw = 0;
        std::uint64_t unique_raw = 0;
        std::uint64_t precomp_streams = 0;
        std::uint64_t lzma_blocks = 0;
        std::uint64_t zstd_blocks = 0;
        std::uint64_t stored_blocks = 0;
        for (const auto& file : info.files) { total_raw += file.size; stored_raw += file.stored_size; precomp_streams += file.precomp.size(); }
        for (const auto& block : info.blocks) {
            total_compressed += block.compressed_size;
            unique_raw += block.uncompressed_size;
            if (block.codec == rpk::Codec::Lzma2) ++lzma_blocks;
            else if (block.codec == rpk::Codec::ZstdLong) ++zstd_blocks;
            else if (block.codec == rpk::Codec::Store) ++stored_blocks;
        }
        const std::uint64_t deduplicated = stored_raw >= unique_raw ? stored_raw - unique_raw : 0;
        std::cout << "RPK archive version: " << info.version << '\n'
                  << "Codecs: per-block LZMA2/XZ, Zstandard long-range, or Store\n"
                  << "Preset: " << info.preset << " | Dictionary: " << info.dictionary_mib << " MiB\n"
                  << "Solid block size: " << human_size(info.block_size) << '\n'
                  << "Files: " << info.files.size() << " | Directories: " << info.directories.size()
                  << " | Solid blocks: " << info.blocks.size() << "\n"
                  << "Block codecs: LZMA2 " << lzma_blocks << " | Zstd-long " << zstd_blocks << " | Store " << stored_blocks << '\n'
                  << "Original data: " << human_size(total_raw) << " | Post-precompression data: " << human_size(stored_raw)
                  << " | Unique transformed data: " << human_size(unique_raw) << " | Deduplicated: " << human_size(deduplicated)
                  << " | Exact zlib streams rebuilt: " << precomp_streams << "\n"
                  << "Compressed payload: " << human_size(total_compressed)
                  << " | Archive: " << human_size(info.archive_size) << "\n\n";
        for (const auto& directory : info.directories) std::cout << "[DIR]  " << directory << '\n';
        for (const auto& file : info.files) {
            std::cout << (file.read_only ? "[RO]   " : "[FILE] ") << std::setw(10) << human_size(file.size) << "  " << file.path << '\n';
        }
        return 0;
    }

    if (command == "test") {
        if (args.size() != 3) throw std::runtime_error("test requires exactly one archive path");
        rpk::verify_archive(path_from_arg(args[2]), [](const std::string& message) { std::cout << message << '\n'; });
        std::cout << "Archive OK: all solid blocks and restored file SHA-256 hashes match.\n";
        return 0;
    }

    if (command == "extract") {
        if (args.size() < 4) throw std::runtime_error("extract requires an archive and -C <destination-folder>");
        const auto archive = path_from_arg(args[2]);
        std::filesystem::path destination;
        bool have_destination = false;
        bool overwrite = false;
        for (std::size_t i = 3; i < args.size(); ++i) {
            const std::string& option = args[i];
            if (option == "-C" || option == "--directory") {
                if (i + 1 >= args.size()) throw std::runtime_error(option + " requires a path");
                destination = path_from_arg(args[++i]);
                have_destination = true;
            } else if (option == "--overwrite") {
                overwrite = true;
            } else {
                throw std::runtime_error("Unknown extract option: " + option);
            }
        }
        if (!have_destination) throw std::runtime_error("extract requires -C <destination-folder>");
        rpk::extract_archive(archive, destination, overwrite, [](const std::string& message) { std::cout << message << '\n'; });
        std::cout << "Extraction complete. Every restored file passed SHA-256 verification.\n";
        return 0;
    }

    if (command == "version" || command == "--version") {
        std::cout << "rpk " << "0.4.0" << " (archive format 4; reads formats 1, 2, and 3)\n";
        return 0;
    }

    throw std::runtime_error("Unknown command: " + command + " (use rpk --help)");
}

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* value) {
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) throw std::runtime_error("Unable to convert a Windows command-line argument to UTF-8");
    std::string result(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), needed, nullptr, nullptr) <= 0) {
        throw std::runtime_error("Unable to convert a Windows command-line argument to UTF-8");
    }
    result.pop_back();
    return result;
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    try {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) args.push_back(wide_to_utf8(argv[i]));
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char* argv[]) {
    try {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
#endif
