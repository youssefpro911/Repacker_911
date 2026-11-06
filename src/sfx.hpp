#pragma once

#include "archive.hpp"

#include <array>
#include <cstdint>
#include <filesystem>

namespace rpk {
inline constexpr std::array<char, 8> kSfxFooterMagic = {'R','P','K','S','F','X','1','!'};
inline constexpr std::uint64_t kSfxFooterSize = 24;

void create_sfx_installer(const std::filesystem::path& archive_path,
                          const std::filesystem::path& stub_path,
                          const std::filesystem::path& output_path,
                          bool overwrite = false,
                          ProgressCallback progress = {});
} // namespace rpk
