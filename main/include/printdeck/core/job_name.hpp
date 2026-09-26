#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace printdeck::core {

// Keep the existing portable-state name budget without splitting UTF-8.
inline std::string bounded_job_name(std::string_view name, std::size_t limit = 192) {
  if (name.size() <= limit) return std::string(name);
  if (limit < 3) return std::string(limit, '.');
  std::size_t end = limit - 3;
  while (end && (static_cast<unsigned char>(name[end]) & 0xc0U) == 0x80U) --end;
  return std::string(name.substr(0, end)) + "...";
}

inline std::string combined_job_name(std::string_view title, std::string_view profile) {
  const auto trim = [](std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
  };
  title = trim(title);
  profile = trim(profile);
  const auto key = [](std::string_view value) {
    std::string result;
    for (unsigned char c : value) {
      if (c == '_' || std::isspace(c)) c = ' ';
      if (c != ' ' || result.empty() || result.back() != ' ') result += static_cast<char>(std::tolower(c));
    }
    for (const std::string_view suffix : {".gcode.3mf", ".3mf", ".gcode"}) {
      if (result.ends_with(suffix)) { result.resize(result.size()-suffix.size()); break; }
    }
    return result;
  };
  if (title.empty()) return bounded_job_name(profile);
  if (profile.empty() || key(title) == key(profile)) return bounded_job_name(title);
  return bounded_job_name(std::string(title) + " - " + std::string(profile));
}

}  // namespace printdeck::core
