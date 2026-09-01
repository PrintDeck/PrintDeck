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

}  // namespace printdeck::core
