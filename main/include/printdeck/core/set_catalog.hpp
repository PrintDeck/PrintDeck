#pragma once
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace printdeck::core {
inline constexpr std::array<std::string_view, 6> kSetLanguages{"en", "pl", "es", "fr", "de", "zh-CN"};
struct AudioPackageDefinition {
  std::string file, sha256;
  std::size_t bytes = 0;
};
struct DownloadableSet {
  std::string id, name, version, family_id, family_name, variant_name, style;
  std::array<AudioPackageDefinition, 6> languages{};
};
// Catalogs only name files below fixed product origins, never arbitrary URLs.
// Validation is all-or-nothing so a bad refresh cannot replace the last good list.
bool parse_set_catalog(std::string_view json, bool audio, std::vector<DownloadableSet>& sets);
bool valid_set_id(std::string_view id);
}  // namespace printdeck::core
