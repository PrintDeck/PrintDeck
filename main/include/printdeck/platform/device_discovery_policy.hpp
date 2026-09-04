#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace printdeck::platform {

inline constexpr std::uint64_t kDeviceDiscoveryCacheMs = 60'000;
inline constexpr std::uint64_t kDeviceDiscoveryRetryMs = 10'000;
inline constexpr std::size_t kDeviceDiscoveryLimit = 24;

// Only the shared entry hostname redirects. Never reflect an arbitrary Host
// header into Location or redirect an IP, setup address, or device alias.
inline bool is_printdeck_entry_host(std::string_view host) {
  if (host.ends_with(":80")) host.remove_suffix(3);
  if (host.ends_with('.')) host.remove_suffix(1);
  constexpr std::string_view entry = "printdeck.local";
  return host.size() == entry.size() &&
      std::equal(host.begin(), host.end(), entry.begin(), [](char actual, char expected) {
        if (actual >= 'A' && actual <= 'Z') actual += 'a' - 'A';
        return actual == expected;
      });
}

// Access under NetworkService's discovery mutex. An in-flight query keeps its
// slot even after a Wi-Fi change; its results are discarded on completion.
struct DeviceDiscoveryPolicy {
  bool running = false;
  bool complete = false;
  bool failed = false;
  std::uint32_t epoch = 0;
  std::uint32_t scan_id = 0;
  std::uint64_t expires_ms = 0;

  bool begin(std::uint64_t now, std::uint32_t network_epoch) {
    if (running || (epoch == network_epoch && now < expires_ms)) return false;
    epoch = network_epoch;
    running = true;
    complete = false;
    failed = false;
    ++scan_id;
    return true;
  }

  void finish(std::uint64_t now, bool success) {
    running = false;
    complete = success;
    failed = !success;
    expires_ms = now + (success ? kDeviceDiscoveryCacheMs : kDeviceDiscoveryRetryMs);
  }
};

inline bool valid_printdeck_id(std::string_view id) {
  return id.size() == 22 && id.starts_with("printdeck-") &&
      std::all_of(id.begin() + 10, id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
}

inline bool valid_printdeck_hostname(std::string_view name) {
  return name.size() >= 9 && name.size() <= 63 &&
      (name == "printdeck" || name.starts_with("printdeck-")) &&
      name.back() != '-' && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
      });
}

// IPv4 values are in host byte order. Never turn discovery data into a link
// outside the station subnet, to a broadcast, or to the local device itself.
inline bool valid_device_peer_ipv4(std::uint32_t peer, std::uint32_t local,
                                  std::uint32_t mask) {
  const bool private_ip = (peer >> 24) == 10 || (peer >> 20) == 0xac1 ||
                          (peer >> 16) == 0xc0a8 || (peer >> 16) == 0xa9fe;
  return private_ip && mask != 0 && peer != local &&
      (peer & mask) == (local & mask) && (peer & ~mask) != 0 &&
      (peer & ~mask) != ~mask;
}

}  // namespace printdeck::platform
