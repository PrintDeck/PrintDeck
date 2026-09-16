#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "printdeck/core/settings.hpp"

namespace printdeck::core {

inline bool valid_moonraker_uuid(std::string_view value) {
  return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

inline bool valid_moonraker_identity(std::string_view value) {
  if (value.starts_with("mh:")) {
    value.remove_prefix(3);
    std::string_view previous;
    unsigned count = 0;
    while (!value.empty()) {
      const auto end = value.find(',');
      const auto digest = value.substr(0, end);
      if (++count > 8 || !valid_moonraker_uuid(digest) ||
          (!previous.empty() && digest <= previous)) return false;
      previous = digest;
      if (end == value.npos) return true;
      value.remove_prefix(end + 1);
    }
    return false;
  }
  return valid_moonraker_uuid(value) ||
      ((value.starts_with("mr:") || value.starts_with("mp:")) &&
       valid_moonraker_uuid(value.substr(3)));
}

inline bool moonraker_host_evidence_matches(std::string_view saved, std::string_view current) {
  if (!saved.starts_with("mh:") || !current.starts_with("mh:") ||
      !valid_moonraker_identity(saved) || !valid_moonraker_identity(current)) return false;
  for (std::size_t i = 3; i < saved.size(); i += 33)
    for (std::size_t j = 3; j < current.size(); j += 33)
      if (saved.substr(i, 32) == current.substr(j, 32)) return true;
  return false;
}

struct PrinterAddress {
  std::string host;
  std::string scheme;
  std::uint16_t port = 0;
};

inline std::optional<PrinterAddress> printer_address(const PrinterProfile& profile) {
  if (!is_local_printer_endpoint(profile.endpoint, profile.protocol)) return {};
  std::string_view value = profile.endpoint;
  PrinterAddress result;
  if (value.starts_with("http://")) { result.scheme = "http://"; value.remove_prefix(7); }
  else if (value.starts_with("https://")) { result.scheme = "https://"; value.remove_prefix(8); }
  const auto colon = value.find(':');
  result.host = value.substr(0, colon);
  if (colon != value.npos) {
    unsigned port = 0;
    for (char c : value.substr(colon + 1)) port = port * 10 + (c - '0');
    result.port = port;
  } else {
    switch (profile.protocol) {
      case PrinterProtocol::bambu_lan: result.port = 8883; break;
      case PrinterProtocol::elegoo_sdcp: case PrinterProtocol::uniformation_sdcp: result.port = 3030; break;
      case PrinterProtocol::elegoo_cc2: result.port = 1883; break;
      default: result.port = result.scheme == "https://" ? 443 : 80; break;
    }
  }
  return result;
}

inline bool printer_address_recoverable(const PrinterProfile& profile) {
  switch (profile.protocol) {
    case PrinterProtocol::moonraker: return valid_moonraker_identity(profile.network_identity);
    case PrinterProtocol::bambu_lan:
    case PrinterProtocol::elegoo_sdcp:
    case PrinterProtocol::elegoo_cc2:
    case PrinterProtocol::uniformation_sdcp: return !profile.serial.empty();
    default: return false; // No proven, discoverable identity: retain manual editing.
  }
}

// No rewrite of scheme or service port when only the DHCP address changed.
inline std::optional<PrinterProfile> printer_at_address(const PrinterProfile& profile,
                                                       std::string_view host) {
  const auto address = printer_address(profile);
  if (!address) return {};
  auto result = profile;
  result.endpoint = address->scheme + std::string(host);
  // Preserve explicit ports, including reverse-proxy ports.
  const auto start = profile.endpoint.find("://");
  if (profile.endpoint.find(':', start == std::string::npos ? 0 : start + 3) != std::string::npos)
    result.endpoint += ":" + std::to_string(address->port);
  if (!is_local_printer_endpoint(result.endpoint, result.protocol)) return {};
  return result;
}

// Never choose the first of two different endpoints advertising the same identity.
class UniquePrinterAddress {
 public:
  void observe(std::string host) {
    if (host.empty()) return;
    if (host_.empty()) host_ = std::move(host);
    else if (host_ != host) ambiguous_ = true;
  }
  std::optional<std::string> result() const {
    if (ambiguous_ || host_.empty()) return {};
    return host_;
  }
 private:
  std::string host_;
  bool ambiguous_ = false;
};

struct PrinterRecoverySchedule {
  unsigned failures = 0;
  unsigned attempts = 0;
  std::uint64_t retry_at_ms = 0;
  void observe(bool connected) {
    if (connected) { failures = 0; attempts = 0; retry_at_ms = 0; }
    else failures = std::min(failures + 1, 2U);
  }
  bool due(std::uint64_t now) const { return failures >= 2 && now >= retry_at_ms; }
  void attempted(std::uint64_t now) {
    const auto delay = std::min<std::uint64_t>(30000ULL << std::min(attempts, 5U), 900000);
    retry_at_ms = now + delay;
    attempts = std::min(attempts + 1, 6U);
  }
};

// Compare against the authoritative current profile, then patch only learned fields.
inline bool merge_printer_address(DeviceSettings& settings, const PrinterProfile& expected,
                                   const PrinterProfile& recovered) {
  auto unchanged = recovered;
  unchanged.endpoint = expected.endpoint;
  unchanged.network_identity = expected.network_identity;
  if (!same_printer_connection(expected, unchanged)) return false;
  const auto current = std::find_if(settings.profiles.begin(), settings.profiles.end(),
      [&](const auto& p) { return same_printer_connection(p, expected); });
  if (current == settings.profiles.end()) return false;
  if (expected.endpoint == recovered.endpoint && expected.network_identity == recovered.network_identity) return false;
  if (expected.endpoint != recovered.endpoint &&
      (!printer_address_recoverable(expected) || expected.network_identity != recovered.network_identity)) return false;
  // The worker may upgrade freshly matched legacy evidence at the same endpoint.
  // Never combine an identity upgrade with an address change.
  const bool identity_upgrade = expected.endpoint == recovered.endpoint &&
      (expected.network_identity.starts_with("mr:") || valid_moonraker_uuid(expected.network_identity)) &&
      (recovered.network_identity.starts_with("mp:") || recovered.network_identity.starts_with("mh:"));
  if (expected.network_identity != recovered.network_identity &&
      (expected.protocol != PrinterProtocol::moonraker ||
       (!expected.network_identity.empty() && !identity_upgrade) ||
       !valid_moonraker_identity(recovered.network_identity))) return false;
  const auto before = printer_address(expected);
  const auto after = printer_address(recovered);
  if (!before || !after || before->scheme != after->scheme || before->port != after->port) return false;
  current->endpoint = recovered.endpoint;
  current->network_identity = recovered.network_identity;
  return true;
}
}  // namespace printdeck::core
