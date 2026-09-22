#pragma once

#include <algorithm>
#include <optional>
#include <string_view>
#include "printdeck/core/settings.hpp"

namespace printdeck::core {

// Match the existing profile's 48-byte storage limit without splitting UTF-8.
inline bool valid_bambu_printer_name(std::string_view name) {
  if (name.empty() || name.size() > 48) return false;
  for (std::size_t i = 0; i < name.size();) {
    const auto first = static_cast<unsigned char>(name[i++]);
    if (first < 0x20 || first == 0x7f) return false;
    if (first < 0x80) continue;
    unsigned count = first >= 0xc2 && first <= 0xdf ? 1 :
        first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
    if (!count || i + count > name.size()) return false;
    const auto second = static_cast<unsigned char>(name[i]);
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
        (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return false;
    while (count--) if ((static_cast<unsigned char>(name[i++]) & 0xc0) != 0x80) return false;
  }
  return true;
}

// SSDP is display metadata only. The caller also checks the source address;
// serial identity must match the saved, authenticated printer connection.
inline std::optional<std::string> bambu_advertised_name(
    std::string_view packet, std::string_view serial) {
  if (packet.size() > 1536 || serial.empty()) return {};
  const auto line_end = packet.find("\r\n");
  const auto first = packet.substr(0, line_end);
  if (first != "NOTIFY * HTTP/1.1" && first != "HTTP/1.1 200 OK") return {};
  if (line_end == packet.npos) return {};
  packet.remove_prefix(line_end + 2);
  std::string_view name, identity, type;
  bool has_name = false, has_identity = false, has_type = false;
  const auto equal = [](std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](unsigned char a, unsigned char b) { return (a >= 'A' && a <= 'Z' ? a + 32 : a) == b; });
  };
  bool complete = false;
  while (!packet.empty()) {
    const auto end = packet.find("\r\n");
    if (end == packet.npos) return {};
    const auto line = packet.substr(0, end);
    packet.remove_prefix(end + 2);
    if (line.empty()) { complete = true; break; }
    const auto colon = line.find(':');
    if (colon == line.npos) return {};
    const auto key = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    if (equal(key, "devname.bambu.com")) { if (has_name) return {}; has_name = true; name = value; }
    if (equal(key, "usn")) { if (has_identity) return {}; has_identity = true; identity = value; }
    if (equal(key, "st") || equal(key, "nt")) { if (has_type) return {}; has_type = true; type = value; }
  }
  if (!complete || identity != serial || type != "urn:bambulab-com:device:3dprinter:1" ||
      !valid_bambu_printer_name(name)) return {};
  return std::string(name);
}

inline bool merge_bambu_printer_name(DeviceSettings& settings,
    const PrinterProfile& expected, const PrinterProfile& learned) {
  if (expected.protocol != PrinterProtocol::bambu_lan ||
      !same_printer_connection(expected, learned) ||
      !valid_bambu_printer_name(learned.display_name) || expected.display_name == learned.display_name) return false;
  const auto current = std::find_if(settings.profiles.begin(), settings.profiles.end(),
      [&](const auto& p) { return same_printer_connection(p, expected); });
  // A queued observation cannot overwrite an intervening edit or deletion.
  if (current == settings.profiles.end() || current->display_name != expected.display_name) return false;
  current->display_name = learned.display_name;
  return true;
}
}  // namespace printdeck::core
