#include "sfx.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace rpk {
namespace {

std::filesystem::path make_temporary_path(const std::filesystem::path& parent) {
    static std::atomic<std::uint64_t> sequence{0};
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto path = parent / (".rpk-sfx-" + std::to_string(stamp ^ ++sequence) + ".tmp");
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) && !ec) return path;
    }
    throw std::runtime_error("Cannot create a temporary SFX output path");
}

void copy_exact(std::istream& input, std::ostream& output,
                std::uint64_t bytes, const char* description) {
    std::vector<char> buffer(1024U * 1024U);
    while (bytes != 0) {
        const auto request = static_cast<std::streamsize>(
            std::min<std::uint64_t>(bytes, buffer.size()));
        input.read(buffer.data(), request);
        if (input.gcount() != request) {
            throw std::runtime_error(std::string("Unexpected end while reading ") + description);
        }
        output.write(buffer.data(), request);
        if (!output) {
            throw std::runtime_error(std::string("Write failed while copying ") + description);
        }
        bytes -= static_cast<std::uint64_t>(request);
    }
}

void write_u64(std::ostream& output, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (unsigned int i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>((value >> (i * 8U)) & 0xffU);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Cannot write SFX footer");
}

} // namespace

void create_sfx_installer(const std::filesystem::path& archive_path,
                          const std::filesystem::path& stub_path,
                          const std::filesystem::path& output_path,
                          bool overwrite,
                          ProgressCallback progress) {
    if (!std::filesystem::is_regular_file(archive_path)) {
        throw std::runtime_error("SFX creation requires a single-file RPK archive");
    }
    if (!std::filesystem::is_regular_file(stub_path)) {
        throw std::runtime_error("RPK setup stub was not found: " + stub_path.u8string());
    }

    (void)read_archive(archive_path);
    std::error_code ec;
    const auto archive_size = std::filesystem::file_size(archive_path, ec);
    if (ec || archive_size == 0) throw std::runtime_error("Cannot determine source archive size");
    const auto stub_size = std::filesystem::file_size(stub_path, ec);
    if (ec || stub_size == 0) throw std::runtime_error("Cannot determine setup stub size");
    if (archive_size > std::numeric_limits<std::uint64_t>::max() - stub_size - kSfxFooterSize) {
        throw std::runtime_error("SFX installer size exceeds the supported range");
    }

    const auto archive_abs = std::filesystem::absolute(archive_path).lexically_normal();
    const auto stub_abs = std::filesystem::absolute(stub_path).lexically_normal();
    const auto output_abs = std::filesystem::absolute(output_path).lexically_normal();
    if (output_abs == archive_abs || output_abs == stub_abs) {
        throw std::runtime_error("SFX output must be different from the source archive and setup stub");
    }
    if (std::filesystem::exists(output_abs) && !overwrite) {
        throw std::runtime_error("SFX output already exists; choose another name or enable overwrite");
    }

    auto parent = output_abs.parent_path();
    if (parent.empty()) parent = std::filesystem::current_path();
    std::filesystem::create_directories(parent, ec);
    if (ec) throw std::runtime_error("Cannot create SFX output folder: " + ec.message());

    const auto temporary = make_temporary_path(parent);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); }
    } cleanup{temporary};

    std::ifstream stub(stub_path, std::ios::binary);
    std::ifstream archive(archive_path, std::ios::binary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!stub || !archive || !output) {
        throw std::runtime_error("Cannot open an SFX input or temporary output file");
    }

    copy_exact(stub, output, stub_size, "setup stub");
    copy_exact(archive, output, archive_size, "RPK archive");
    output.write(kSfxFooterMagic.data(), static_cast<std::streamsize>(kSfxFooterMagic.size()));
    if (!output) throw std::runtime_error("Cannot write SFX signature");
    write_u64(output, stub_size);
    write_u64(output, archive_size);
    output.flush();
    if (!output) throw std::runtime_error("Cannot finalize SFX installer");
    output.close();
    stub.close();
    archive.close();

    if (std::filesystem::exists(output_abs)) {
        if (!overwrite) throw std::runtime_error("SFX output appeared during creation; refusing to replace it");
        std::filesystem::remove(output_abs, ec);
        if (ec) throw std::runtime_error("Cannot replace existing SFX output: " + ec.message());
    }
    std::filesystem::rename(temporary, output_abs, ec);
    if (ec) throw std::runtime_error("Cannot move completed SFX installer into place: " + ec.message());

    if (progress) progress("Self-extracting setup created: " + output_abs.u8string());
}

} // namespace rpk
