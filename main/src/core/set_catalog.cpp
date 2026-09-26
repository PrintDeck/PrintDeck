#include "printdeck/core/set_catalog.hpp"
#include "printdeck/core/audio_pack.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include "cJSON.h"

namespace printdeck::core {
bool valid_set_id(std::string_view id) {
  return !id.empty() && id.size() <= 48 && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}
bool parse_set_catalog(std::string_view json, bool audio, std::vector<DownloadableSet>& sets) {
  if (json.empty() || json.size() > 64 * 1024 || json.find('\0') != json.npos) return false;
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_ParseWithLength(json.data(), json.size()), cJSON_Delete);
  if (!root) return false;
  const auto get = [](const cJSON* object, const char* name) { return cJSON_GetObjectItemCaseSensitive(object, name); };
  const auto text = [&](const cJSON* object, const char* name, std::size_t maximum) -> std::string {
    const auto* value = get(object, name);
    if (!cJSON_IsString(value) || !value->valuestring) return {};
    std::string result(value->valuestring);
    if (result.size() > maximum || std::any_of(result.begin(), result.end(), [](unsigned char c) {return c < 32;})) return {};
    return result;
  };
  const auto* schema = get(root.get(), "schema");
  const auto* entries = get(root.get(), "sets");
  const int count = cJSON_GetArraySize(entries);
  if (!cJSON_IsNumber(schema) || schema->valuedouble != 1 || !cJSON_IsArray(entries) || count < 1 || count > 32) return false;
  std::vector<DownloadableSet> next;
  next.reserve(count);
  for (int i = 0; i < count; ++i) {
    const auto* entry = cJSON_GetArrayItem(entries, i);
    DownloadableSet item;
    item.id = text(entry, "id", 48); item.name = text(entry, "name", 96); item.version = text(entry, "version", 32);
    if (!valid_set_id(item.id) || item.name.empty() || item.version.empty() ||
        std::any_of(next.begin(), next.end(), [&](const auto& previous) {return previous.id == item.id;})) return false;
    if (audio) {
      item.style = text(entry, "style", 16);
      constexpr std::array<std::string_view, 6> styles{"modern", "soft", "oldschool", "arcade", "scifi", "clean"};
      if (std::find(styles.begin(), styles.end(), item.style) == styles.end()) return false;
      const auto* languages = get(entry, "languages");
      if (!cJSON_IsObject(languages) || cJSON_GetArraySize(languages) != 6) return false;
      for (std::size_t locale = 0; locale < kSetLanguages.size(); ++locale) {
        const auto* package = get(languages, kSetLanguages[locale].data());
        auto& result = item.languages[locale];
        result.sha256 = text(package, "sha256", 64);
        result.file = text(package, "file", 96);
        const auto* bytes = get(package, "bytes");
        if (result.sha256.size() != 64 || result.sha256.find_first_not_of("0123456789abcdef") != std::string::npos ||
            result.file != "packages/" + result.sha256 + ".pda" || !cJSON_IsNumber(bytes) ||
            !std::isfinite(bytes->valuedouble) || bytes->valuedouble < 88 ||
            bytes->valuedouble > kAudioPackMaximumBytes || std::floor(bytes->valuedouble) != bytes->valuedouble) return false;
        result.bytes = static_cast<std::size_t>(bytes->valuedouble);
      }
    } else {
      item.family_id = text(entry, "family_id", 48); item.family_name = text(entry, "family_name", 96);
      item.variant_name = text(entry, "variant_name", 96);
      if (!valid_set_id(item.family_id) || item.family_name.empty() || item.variant_name.empty()) return false;
    }
    next.push_back(std::move(item));
  }
  sets = std::move(next);
  return true;
}
}  // namespace printdeck::core
