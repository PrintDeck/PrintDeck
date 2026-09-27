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
  bool available = true;
  AudioPackageDefinition package;
  // SHA-256 of raw BGRA palette + packed I4 pixels, at 192 and 88 pixels.
  std::array<std::string, 2> device_previews{};
  std::array<std::string, 6> descriptions{};
};
// Catalogs only name files below fixed product origins, never arbitrary URLs.
// Validation is all-or-nothing so a bad refresh cannot replace the last good list.
bool parse_set_catalog(std::string_view json, bool audio, std::vector<DownloadableSet>& sets);
bool valid_set_id(std::string_view id);
enum class CatalogAction { keep, remove, update };
CatalogAction reaction_catalog_action(const std::vector<DownloadableSet>& sets,
    std::string_view installed, std::string_view version);
}  // namespace printdeck::core
