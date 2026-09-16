#include "printdeck/platform/printer_address_recovery.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "cJSON.h"
#include "esp_log.h"
#include "printdeck/platform/prusalink_http_transport.hpp"

namespace printdeck::platform {
namespace {
struct Service {
  std::string host, hostname, uuid;
  std::uint16_t port = 0;
  std::uint16_t https_port = 0;
  bool root_route = true;
};
// Serialize identity queries between the scanner, poller and adapter workers.
std::mutex query_mutex;
std::vector<Service> moonraker_services() {
  const std::lock_guard<std::mutex> lock(query_mutex);
  mdns_result_t* results = nullptr;
  std::vector<Service> services;
  if (mdns_query_ptr("_moonraker", "_tcp", 1500, 16, &results) != ESP_OK) {
    if (results) mdns_query_results_free(results);
    return services;
  }
  for (auto* r = results; r && services.size() < 32; r = r->next) {
    Service service;
    service.port = r->port;
    if (r->hostname) service.hostname = std::string(r->hostname) + ".local";
    for (std::size_t i = 0; i < r->txt_count; ++i) {
      const auto& txt = r->txt[i];
      if (!txt.key || !txt.value) continue;
      if (std::strcmp(txt.key, "uuid") == 0) service.uuid = txt.value;
      if (std::strcmp(txt.key, "route_prefix") == 0) service.root_route = txt.value[0] == '\0';
      if (std::strcmp(txt.key, "https_port") == 0) {
        unsigned port = 0;
        const std::string_view text(txt.value);
        if (text.size() <= 5) {
          for (char c : text) {
            if (c < '0' || c > '9') { port = 0; break; }
            port = port * 10 + c - '0';
          }
          if (port <= 65535) service.https_port = port;
        }
      }
    }
    if (!core::valid_moonraker_uuid(service.uuid) || !service.port || !service.root_route) continue;
    for (auto* a = r->addr; a && services.size() < 32; a = a->next) {
      if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
      char host[INET_ADDRSTRLEN];
      in_addr address{.s_addr = a->addr.u_addr.ip4.addr};
      if (!inet_ntop(AF_INET, &address, host, sizeof(host))) continue;
      service.host = host;
      services.push_back(service);
    }
  }
  if (results) mdns_query_results_free(results);
  return services;
}

bool port_matches(const core::PrinterAddress& address, const Service& service) {
  return address.port == (address.scheme == "https://" ? service.https_port : service.port);
}
bool same_host(const core::PrinterAddress& address, const Service& service) {
  if (address.host == service.host) return true;
  auto host = address.host;
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
  auto name = service.hostname;
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
  return host == name;
}
struct HostIdentities {
  std::string legacy;
  std::string product;
  std::string interfaces;
};

HostIdentities host_identities(const core::PrinterProfile& profile,
    const PrinterRecoveryCancel& cancelled) {
  if (cancelled()) return {};
  const auto address = core::printer_address(profile);
  if (!address) return {};
  const auto origin = (address->scheme.empty() ? "http://" : address->scheme) +
      address->host + ":" + std::to_string(address->port);
  PrusaLinkEspTransport transport;
  // Existing bounded HTTP transport: no credentials, redirects or cookies.
  const auto response = transport.get({.url = origin + "/machine/system_info",
      .maximum_body = 16384, .deadline_ms = prusalink_now_ms() + 1500}, cancelled);
  if (cancelled()) return {};
  if (response.error != PrusaLinkError::none || response.status != 200) {
    if (!profile.network_identity.empty())
      ESP_LOGW("printer_identity", "Host identity read failed: transport=%u HTTP=%d",
               static_cast<unsigned>(response.error), response.status);
    return {};
  }
  cJSON* document = cJSON_ParseWithLength(response.body.data(), response.body.size());
  if (!document) return {};
  struct JsonGuard { cJSON* value; ~JsonGuard() { cJSON_Delete(value); } } guard{document};
  const auto member = [](const cJSON* object, const char* key) {
    return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
  };
  const auto text = [&](const cJSON* object, const char* key) -> std::string {
    const auto* value = member(object, key);
    if (!cJSON_IsString(value) || !value->valuestring) return {};
    const std::string result(value->valuestring);
    if (result.empty() || result.size() > 128 ||
        std::any_of(result.begin(), result.end(), [](unsigned char c) { return c < 32 || c == 127; })) return {};
    return result;
  };
  const auto* info = member(member(document, "result"), "system_info");
  HostIdentities identities;
  const auto* product = member(info, "product_info");
  const auto model = text(product, "machine_type");
  const auto serial = text(product, "serial_number");
  const auto meaningful = [](const std::string& value) {
    std::string normalized;
    for (const unsigned char ch : value) {
      if (std::isalnum(ch)) normalized += static_cast<char>(std::tolower(ch));
    }
    return !normalized.empty() && normalized != "unknown" && normalized != "none" &&
        normalized != "null" && normalized != "na" &&
        normalized.find_first_not_of('0') != std::string::npos;
  };
  // Vendor product serials identify the printer across interface changes and
  // service restarts. Keep them private: only the digest is persisted.
  if (meaningful(model) && serial.size() >= 4 && meaningful(serial)) {
    const auto digest = prusalink_md5("moonraker-product-v1\n" + model + "\n" + serial);
    if (core::valid_moonraker_uuid(digest)) identities.product = "mp:" + digest;
  }
  const auto* instances = member(info, "instance_ids");
  const auto moonraker = text(instances, "moonraker");
  const auto klipper = text(instances, "klipper");
  if (moonraker.empty() || klipper.empty() || moonraker == "unknown" || klipper == "unknown") return identities;
  std::vector<std::string> macs;
  const auto* network = member(info, "network");
  const cJSON* interface = nullptr;
  cJSON_ArrayForEach(interface, network) {
    auto mac = text(interface, "mac_address");
    if (mac.size() != 17) continue;
    std::string normalized;
    bool valid = true;
    for (std::size_t i = 0; i < mac.size(); ++i) {
      const auto c = static_cast<unsigned char>(mac[i]);
      if (i % 3 == 2) { valid &= c == ':'; continue; }
      valid &= std::isxdigit(c) != 0;
      normalized += static_cast<char>(std::tolower(c));
    }
    if (!valid || normalized == "000000000000" || normalized == "ffffffffffff") continue;
    const auto first = std::strtoul(normalized.substr(0, 2).c_str(), nullptr, 16);
    if (first & 3U) continue; // multicast and locally generated virtual-interface identities
    macs.push_back(std::move(normalized));
  }
  std::sort(macs.begin(), macs.end());
  macs.erase(std::unique(macs.begin(), macs.end()), macs.end());
  if (macs.empty() || macs.size() > 8) return identities;
  std::vector<std::string> evidence;
  // Keep independent proofs: a disconnected Ethernet adapter must not invalidate
  // the still-matching Wi-Fi adapter. IPs and interface names are never identity.
  for (const auto& mac : macs) {
    const auto digest = prusalink_md5("moonraker-interface-v1\n" + moonraker + "\n" + klipper + "\n" + mac);
    if (!core::valid_moonraker_uuid(digest)) return identities;
    evidence.push_back(digest);
  }
  std::sort(evidence.begin(), evidence.end());
  identities.interfaces = "mh:";
  for (const auto& digest : evidence) {
    if (identities.interfaces.size() > 3) identities.interfaces += ',';
    identities.interfaces += digest;
  }
  // This digest is a compact local identity, not proof against a hostile LAN peer.
  std::string material = "moonraker-host-v1\n" + moonraker + "\n" + klipper;
  for (const auto& mac : macs) material += "\n" + mac;
  const auto digest = prusalink_md5(material);
  if (core::valid_moonraker_uuid(digest)) identities.legacy = "mr:" + digest;
  return identities;
}
}  // namespace

std::string moonraker_host_identity(const core::PrinterProfile& profile,
    const PrinterRecoveryCancel& cancelled) {
  const auto identities = host_identities(profile, cancelled);
  if (profile.network_identity.starts_with("mr:")) return identities.legacy;
  if (profile.network_identity.starts_with("mp:")) return identities.product;
  if (profile.network_identity.starts_with("mh:"))
    return core::moonraker_host_evidence_matches(profile.network_identity, identities.interfaces)
        ? profile.network_identity : std::string{};
  return identities.product.empty() ? identities.interfaces : identities.product;
}

std::optional<core::PrinterProfile> learn_moonraker_identity(
    const core::PrinterProfile& profile, const PrinterRecoveryCancel& cancelled) {
  if (profile.protocol != core::PrinterProtocol::moonraker || cancelled()) return {};
  if (profile.network_identity.starts_with("mr:")) {
    const auto identities = host_identities(profile, cancelled);
    if (cancelled() || identities.legacy != profile.network_identity) return {};
    auto upgraded = profile;
    upgraded.network_identity = identities.product.empty() ? identities.interfaces : identities.product;
    if (upgraded.network_identity.empty()) return {};
    return upgraded;
  }
  if (!profile.network_identity.empty() && !core::valid_moonraker_uuid(profile.network_identity)) return {};
  const auto address = core::printer_address(profile);
  if (!address) return {};
  core::UniquePrinterAddress identities;
  bool advertised = false;
  for (const auto& service : moonraker_services()) {
    if (port_matches(*address, service) && same_host(*address, service)) {
      advertised = true;
      identities.observe(service.uuid);
    }
  }
  const auto identity = identities.result();
  if (cancelled() || (advertised && !identity)) return {};
  if (!profile.network_identity.empty() && (!identity || *identity != profile.network_identity)) return {};
  const auto host = host_identities(profile, cancelled);
  auto learned = profile;
  // Prefer the same product identity whether or not this particular display
  // received the printer's multicast advertisement.
  learned.network_identity = !host.product.empty() ? host.product
      : !host.interfaces.empty() ? host.interfaces : identity ? *identity : std::string{};
  if (learned.network_identity == profile.network_identity) return {};
  if (learned.network_identity.empty() || cancelled()) return {};
  return learned;
}

bool moonraker_endpoint_identity_matches(const core::PrinterProfile& profile) {
  if (profile.protocol != core::PrinterProtocol::moonraker || profile.network_identity.empty()) return true;
  if (profile.network_identity.starts_with("mr:") || profile.network_identity.starts_with("mp:") ||
      profile.network_identity.starts_with("mh:")) {
    const auto identity = moonraker_host_identity(profile, [] { return false; });
    if (identity.empty()) {
      ESP_LOGW("printer_identity", "Saved host identity could not be verified");
    } else if (identity != profile.network_identity) {
      ESP_LOGW("printer_identity", "Saved host identity differs from the current host");
    }
    return identity == profile.network_identity;
  }
  const auto address = core::printer_address(profile);
  if (!address || !core::valid_moonraker_uuid(profile.network_identity)) return false;
  bool matched = false;
  for (const auto& service : moonraker_services()) {
    if (!port_matches(*address, service) || !same_host(*address, service)) continue;
    if (service.uuid != profile.network_identity) {
      ESP_LOGW("printer_identity", "Saved service identity differs from the current service");
      return false;
    }
    matched = true;
  }
  if (!matched) ESP_LOGW("printer_identity", "Saved service identity was not advertised");
  return matched;
}

std::vector<DiscoveredPrinter> discover_moonraker_identities() {
  std::vector<DiscoveredPrinter> result;
  for (const auto& service : moonraker_services()) {
    result.push_back({.protocol = core::PrinterProtocol::moonraker,
        .host = service.host, .port = service.port, .network_identity = service.uuid});
    if (service.https_port)
      result.push_back({.protocol = core::PrinterProtocol::moonraker,
          .host = service.host, .port = service.https_port, .network_identity = service.uuid});
  }
  return result;
}
}  // namespace printdeck::platform
