#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace printdeck::platform {

inline std::optional<std::uint16_t> printer_setup_port(std::string_view text) {
  if (text.empty()) return 0;
  if (text.size() > 5) return std::nullopt;
  unsigned value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  if (value == 0 || value > 65535) return std::nullopt;
  return static_cast<std::uint16_t>(value);
}

inline bool valid_printer_setup_host(std::string_view host) {
  if (host.empty() || host.size() > 63) return false;
  bool numeric = true;
  unsigned labels = 0;
  for (std::size_t begin = 0; begin < host.size();) {
    const auto end = host.find('.', begin);
    const auto label = host.substr(begin, end == host.npos ? host.size() - begin : end - begin);
    if (label.empty() || label.front() == '-' || label.back() == '-') return false;
    for (const char c : label) {
      const bool digit = c >= '0' && c <= '9';
      if (!digit && c != '-' && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')) return false;
      numeric &= digit;
    }
    ++labels;
    if (end == host.npos) break;
    begin = end + 1;
    if (begin == host.size()) return false;
  }
  if (!numeric) return true;
  if (labels != 4) return false;
  unsigned octet = 0;
  for (const char c : host) {
    if (c == '.') octet = 0;
    else { octet = octet * 10 + static_cast<unsigned>(c - '0'); if (octet > 255) return false; }
  }
  return true;
}
}  // namespace printdeck::platform
