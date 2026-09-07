#include "printdeck/core/firmware_channel.hpp"

#include <cctype>
#include <limits>

#include "cJSON.h"

namespace printdeck::core {
namespace {

std::optional<std::uint32_t> parse_version_part(std::string_view value) {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) return std::nullopt;
  std::uint32_t result = 0;
  for (const char character : value) {
    if (!std::isdigit(static_cast<unsigned char>(character))) return std::nullopt;
    const auto digit = static_cast<std::uint32_t>(character - '0');
    if (result > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
      return std::nullopt;
    }
    result = result * 10U + digit;
  }
  return result;
}

int hex_value(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

bool only_trailing_whitespace(const char* cursor, const char* end) {
  while (cursor != nullptr && cursor < end) {
    if (!std::isspace(static_cast<unsigned char>(*cursor))) return false;
    ++cursor;
  }
  return cursor == end;
}

bool valid_https_url(const cJSON* item) {
  if (!cJSON_IsString(item) || item->valuestring == nullptr) return false;
  const std::string_view url = item->valuestring;
  if (url.size() < 9 || url.size() > 512 || url.rfind("https://", 0) != 0) {
    return false;
  }
  for (const char character : url) {
    if (std::isspace(static_cast<unsigned char>(character)) ||
        std::iscntrl(static_cast<unsigned char>(character))) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::optional<FirmwareVersion> parse_firmware_version(std::string_view value) {
  const std::size_t first_dot = value.find('.');
  if (first_dot == std::string_view::npos) return std::nullopt;
  const std::size_t second_dot = value.find('.', first_dot + 1);
  if (second_dot == std::string_view::npos ||
      value.find('.', second_dot + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  const auto major = parse_version_part(value.substr(0, first_dot));
  const auto minor = parse_version_part(
      value.substr(first_dot + 1, second_dot - first_dot - 1));
  const auto patch = parse_version_part(value.substr(second_dot + 1));
  if (!major || !minor || !patch) return std::nullopt;
  return FirmwareVersion{*major, *minor, *patch};
}

std::optional<FirmwareChannel> parse_firmware_channel(
    std::string_view json, std::string_view expected_target) {
  const char* parse_end = nullptr;
  cJSON* root = cJSON_ParseWithLengthOpts(
      json.data(), json.size(), &parse_end, false);
  const char* json_end = json.data() + json.size();
  if (root == nullptr || !only_trailing_whitespace(parse_end, json_end) ||
      !cJSON_IsObject(root)) {
    if (root != nullptr) cJSON_Delete(root);
    return std::nullopt;
  }

  const cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
  const cJSON* channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
  const cJSON* target = cJSON_GetObjectItemCaseSensitive(root, "target");
  const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
  const cJSON* url = cJSON_GetObjectItemCaseSensitive(root, "url");
  const cJSON* sha256 = cJSON_GetObjectItemCaseSensitive(root, "sha256");
  const bool fields_valid = cJSON_IsNumber(schema) && schema->valuedouble == 1.0 &&
                            cJSON_IsString(channel) &&
                            std::string_view(channel->valuestring) == "stable" &&
                            cJSON_IsString(target) &&
                            std::string_view(target->valuestring) == expected_target &&
                            cJSON_IsString(version) &&
                            parse_firmware_version(version->valuestring).has_value() &&
                            (url == nullptr || valid_https_url(url)) &&
                            cJSON_IsString(sha256) &&
                            std::string_view(sha256->valuestring).size() == 64;
  if (!fields_valid) {
    cJSON_Delete(root);
    return std::nullopt;
  }

  FirmwareChannel result;
  result.version = version->valuestring;
  if (url != nullptr) result.url = url->valuestring;
  const std::string_view checksum = sha256->valuestring;
  for (std::size_t index = 0; index < result.sha256.size(); ++index) {
    const int high = hex_value(checksum[index * 2]);
    const int low = hex_value(checksum[index * 2 + 1]);
    if (high < 0 || low < 0) {
      cJSON_Delete(root);
      return std::nullopt;
    }
    result.sha256[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  cJSON_Delete(root);
  return result;
}


std::optional<FirmwareOffer> select_firmware_offer(
    std::string_view json, std::string_view target,
    std::string_view installed_version, std::string_view installed_layout) {
  const auto current = parse_firmware_version(installed_version);
  if (!current || json.size() > 16384) return std::nullopt;
  // Bound parser recursion before cJSON allocates or uses the OTA worker stack.
  int depth = 0;
  bool quoted = false, escaped = false;
  for (const char c : json) {
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') { if (++depth > 4) return std::nullopt; }
    else if (c == '}' || c == ']') { if (--depth < 0) return std::nullopt; }
  }
  if (depth != 0 || quoted) return std::nullopt;
  const char* end = nullptr;
  cJSON* root = cJSON_ParseWithLengthOpts(json.data(), json.size(), &end, false);
  struct Cleanup { cJSON* root; ~Cleanup() { cJSON_Delete(root); } } cleanup{root};
  auto field = [](const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
  };
  auto string = [&](const cJSON* object, const char* key) -> std::string_view {
    const auto* value = field(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
  };
  auto digest = [](std::string_view value) {
    if (value.size() != 64) return false;
    for (char c : value) if (hex_value(c) < 0) return false;
    return true;
  };
  auto unique_fields = [](const cJSON* object) {
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON* a = object->child; a; a = a->next)
      for (const cJSON* b = a->next; b; b = b->next)
        if (std::string_view(a->string) == b->string) return false;
    return true;
  };
  const auto* schema = field(root, "schema");
  const auto* latest = field(root, "latest");
  const auto* boundaries = field(root, "factory_releases");
  const auto latest_version = parse_firmware_version(string(latest, "version"));
  if (!unique_fields(root) || !unique_fields(latest) || !only_trailing_whitespace(end, json.data() + json.size()) ||
      !cJSON_IsNumber(schema) || schema->valuedouble != 2 ||
      string(root, "target") != target || !latest_version ||
      !digest(string(latest, "layout")) || !digest(string(latest, "sha256")) ||
      !valid_https_url(field(latest, "url")) || !cJSON_IsArray(boundaries) ||
      cJSON_GetArraySize(boundaries) > 32) return std::nullopt;
  FirmwareOffer offer;
  offer.release.version = string(latest, "version");
  offer.release.url = string(latest, "url");
  offer.layout = string(latest, "layout");
  const auto hash = string(latest, "sha256");
  for (std::size_t i = 0; i < 32; ++i)
    offer.release.sha256[i] = (hex_value(hash[i * 2]) << 4) | hex_value(hash[i * 2 + 1]);
  const cJSON* selected = nullptr;
  const cJSON* last_boundary = nullptr;
  std::optional<FirmwareVersion> previous;
  const cJSON* item = nullptr;
  cJSON_ArrayForEach(item, boundaries) {
    const auto version = parse_firmware_version(string(item, "version"));
    if (!unique_fields(item) || !version || *version > *latest_version || (previous && *version <= *previous) ||
        !digest(string(item, "layout")) ||
        !valid_https_url(field(item, "install_manifest"))) return std::nullopt;
    previous = version;
    last_boundary = item;
    if (*version > *current) selected = item;
  }
  if (last_boundary && string(last_boundary, "layout") != offer.layout)
    return std::nullopt;
  // A development/newer installed build must never be offered a downgrade.
  if (*current > *latest_version) return offer;
  if (!selected && offer.layout != installed_layout) selected = last_boundary;
  if (selected) {
    offer.release.version = string(selected, "version");
    offer.layout = string(selected, "layout");
    offer.release.url.clear();
    offer.release.sha256 = {};
    offer.factory_required = true;
  } else if (offer.layout != installed_layout) {
    // Incomplete migration metadata must never enable OTA.
    return std::nullopt;
  }
  return offer;
}

}  // namespace printdeck::core
