#include "printdeck/platform/printer_discovery_service.hpp"
#include "printdeck/platform/printer_address_recovery.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <utility>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/idf_additions.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "printdeck/platform/bambu_trust.hpp"
#include "printdeck/platform/bambu_model.hpp"
#include "printdeck/platform/printer_discovery_timing.hpp"
#include "printdeck/platform/printer_discovery_schedule.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "sdkconfig.h"
#include "mdns.h"
#include "printdeck/platform/prusalink_discovery.hpp"
#include "printdeck/platform/tinymaker_client.hpp"
#include "printdeck/platform/prusalink_http_transport.hpp"
#include "printdeck/platform/elegoo_sdcp_parser.hpp"
#include "printdeck/platform/uniformation_sdcp_parser.hpp"
#include "printdeck/platform/elegoo_cc2_parser.hpp"
#include "printdeck/platform/device_discovery_policy.hpp"
#include "printdeck/platform/printer_setup_address.hpp"

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "printer_discovery";
constexpr std::uint64_t kMinimumDurationMs = 3000;
// Temporarily allow a full three-minute pass so hardware testing can separate
// a slow network from an incomplete discovery implementation.
constexpr std::uint64_t kMaximumDurationMs = 180000;
// The HTTP listener, browser polling, the selected-printer client and both SSDP
// listeners share LWIP's global descriptor table with discovery. Refuse stale
// local sdkconfig files instead of silently building firmware that can starve
// Web Config and randomly skip the subnet under descriptor pressure.
#if !defined(CONFIG_LWIP_MAX_SOCKETS) || CONFIG_LWIP_MAX_SOCKETS < 16
#error "PrintDeck requires CONFIG_LWIP_MAX_SOCKETS >= 16; regenerate sdkconfig from sdkconfig.defaults"
#endif
// Keep discovery at five simultaneous descriptors. While the two Bambu SSDP
// listeners are open, at most three TCP probes run; after their complete retry
// and MX response window closes, restore the original five-probe throughput.
// This preserves reliable, spaced SSDP discovery without returning to the old
// seven-descriptor peak or increasing the global socket table.
constexpr std::size_t kProbeBatchSize = PrinterDiscoverySchedule::capacity;
constexpr std::size_t kProbeBatchSizeWithSsdp = 3;
constexpr std::size_t kSocketOpenAttempts = 12;
constexpr std::size_t kMaximumResults = 24;
constexpr std::uint64_t kRecentResultLifetimeMs = 5 * 60 * 1000;
constexpr std::uint16_t kBambuTlsPort = 8883;
constexpr char kSsdpGroup[] = "239.255.255.250";

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
}

std::string trim(std::string value) {
  const auto printable = [](unsigned char character) { return !std::isspace(character); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), printable));
  value.erase(std::find_if(value.rbegin(), value.rend(), printable).base(), value.end());
  return value;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

bool valid_ipv4(const std::string& host) {
  in_addr address{};
  return inet_pton(AF_INET, host.c_str(), &address) == 1;
}

std::string ipv4_from_endpoint(std::string endpoint) {
  if (endpoint.rfind("http://", 0) == 0) endpoint.erase(0, 7);
  else if (endpoint.rfind("https://", 0) == 0) endpoint.erase(0, 8);
  const std::size_t slash = endpoint.find('/');
  if (slash != std::string::npos) endpoint.resize(slash);
  const std::size_t colon = endpoint.find(':');
  if (colon != std::string::npos) endpoint.resize(colon);
  return valid_ipv4(endpoint) ? endpoint : std::string{};
}

std::string discovery_network_key(const NetworkStatus& network) {
  return network.station_name + "|" + network.ipv4 + "|" + network.netmask;
}

bool valid_bambu_serial(const std::string& serial) {
  return serial.size() >= 8 && serial.size() <= 32 &&
         std::all_of(serial.begin(), serial.end(), [](unsigned char character) {
           return std::isalnum(character) != 0;
         });
}

std::string header_value(const std::string& packet, const char* wanted) {
  const std::string wanted_lower = lower(wanted);
  std::size_t cursor = 0;
  while (cursor < packet.size()) {
    const std::size_t end = packet.find('\n', cursor);
    std::string line = packet.substr(cursor, end == std::string::npos ? packet.size() - cursor
                                                                      : end - cursor);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::size_t separator = line.find(':');
    if (separator != std::string::npos && lower(trim(line.substr(0, separator))) == wanted_lower) {
      return trim(line.substr(separator + 1));
    }
    if (end == std::string::npos) break;
    cursor = end + 1;
  }
  return {};
}

std::string ssdp_host(std::string location, const std::string& source) {
  location = trim(std::move(location));
  const std::string normalized = lower(location);
  if (normalized.rfind("http://", 0) == 0) location.erase(0, 7);
  else if (normalized.rfind("https://", 0) == 0) location.erase(0, 8);
  const std::size_t path = location.find('/');
  if (path != std::string::npos) location.resize(path);
  const std::size_t port = location.find(':');
  if (port != std::string::npos) location.resize(port);
  location = trim(std::move(location));
  return valid_ipv4(location) ? location : source;
}

bool moonraker_signature(int socket_fd, const std::string& host, std::uint64_t deadline_ms) {
  const std::uint64_t current = now_ms();
  if (current >= deadline_ms) return false;
  const std::uint64_t response_deadline = std::min(deadline_ms, current + 250);
  fcntl(socket_fd, F_SETFL, 0);
  timeval timeout{};
  timeout.tv_usec = static_cast<suseconds_t>((response_deadline - current) * 1000);
  setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  const std::string request =
      "GET /server/info HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
  if (send(socket_fd, request.data(), request.size(), 0) < 0) return false;
  std::string response;
  response.reserve(4096);
  char buffer[768]{};
  while (response.size() < 4096 && now_ms() < response_deadline) {
    const int received = recv(socket_fd, buffer, sizeof(buffer), 0);
    if (received <= 0) break;
    response.append(buffer, static_cast<std::size_t>(received));
    if (response.find("moonraker_version") != std::string::npos ||
        response.find("klippy_connected") != std::string::npos) return true;
  }
  return false;
}

std::vector<std::string> moonraker_interface_addresses(const std::string& host,
                                                        std::uint16_t port,
                                                        std::uint64_t deadline_ms) {
  std::vector<std::string> addresses;
  const std::uint64_t current = now_ms();
  const std::uint32_t timeout_ms =
      PrinterDiscoveryTiming::bounded_wait_ms(current, deadline_ms, 1500);
  if (timeout_ms == 0) return addresses;
  const int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_fd < 0) return addresses;
  timeval timeout{.tv_sec = static_cast<time_t>(timeout_ms / 1000),
                  .tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000)};
  setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &target.sin_addr) != 1 ||
      connect(socket_fd, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0) {
    close(socket_fd);
    return addresses;
  }
  const std::string request = "GET /machine/system_info HTTP/1.0\r\nHost: " + host +
                              "\r\nConnection: close\r\n\r\n";
  if (send(socket_fd, request.data(), request.size(), 0) < 0) {
    close(socket_fd);
    return addresses;
  }
  std::string response;
  response.reserve(8192);
  char buffer[1024]{};
  while (response.size() < 16384 && now_ms() < deadline_ms) {
    const int received = recv(socket_fd, buffer, sizeof(buffer), 0);
    if (received <= 0) break;
    response.append(buffer, static_cast<std::size_t>(received));
  }
  close(socket_fd);
  const std::size_t body_offset = response.find("\r\n\r\n");
  if (body_offset == std::string::npos) return addresses;
  cJSON* document = cJSON_ParseWithLength(response.data() + body_offset + 4,
                                           response.size() - body_offset - 4);
  if (document == nullptr) return addresses;
  const cJSON* result = cJSON_GetObjectItemCaseSensitive(document, "result");
  const cJSON* system_info = cJSON_GetObjectItemCaseSensitive(result, "system_info");
  const cJSON* network = cJSON_GetObjectItemCaseSensitive(system_info, "network");
  if (cJSON_IsObject(network)) {
    const cJSON* interface = nullptr;
    cJSON_ArrayForEach(interface, network) {
      const cJSON* ip_addresses = cJSON_GetObjectItemCaseSensitive(interface, "ip_addresses");
      const cJSON* entry = nullptr;
      cJSON_ArrayForEach(entry, ip_addresses) {
        const cJSON* family = cJSON_GetObjectItemCaseSensitive(entry, "family");
        const cJSON* address = cJSON_GetObjectItemCaseSensitive(entry, "address");
        if (cJSON_IsString(family) && cJSON_IsString(address) &&
            family->valuestring != nullptr && address->valuestring != nullptr &&
            std::strcmp(family->valuestring, "ipv4") == 0 && valid_ipv4(address->valuestring)) {
          addresses.emplace_back(address->valuestring);
        }
      }
    }
  }
  cJSON_Delete(document);
  std::sort(addresses.begin(), addresses.end());
  addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
  return addresses;
}

bool bambu_tls_identity(const std::string& host, std::uint64_t deadline_ms, const char* serial = nullptr) {
  const std::uint64_t current = now_ms();
  if (current >= deadline_ms) return false;
  const char* anchors = bambu_trust_anchors();
  esp_tls_cfg_t config{};
  config.cacert_buf = reinterpret_cast<const unsigned char*>(anchors);
  config.cacert_bytes = static_cast<unsigned int>(std::strlen(anchors) + 1);
  config.skip_common_name = serial == nullptr;
  config.common_name = serial;
  config.timeout_ms = static_cast<int>(PrinterDiscoveryTiming::bounded_wait_ms(
      current, deadline_ms, PrinterDiscoveryTiming::bambu_tls_handshake_timeout_ms));
  config.addr_family = ESP_TLS_AF_INET;
  config.tls_version = ESP_TLS_VER_TLS_1_2;
  esp_tls_t* tls = esp_tls_init();
  if (tls == nullptr) return false;
  const std::uint64_t handshake_started = now_ms();
  const bool accepted = esp_tls_conn_new_sync(host.c_str(), host.size(), kBambuTlsPort,
                                               &config, tls) == 1;
  esp_tls_conn_destroy(tls);
  ESP_LOGI(kLogTag, "Bambu TLS identity %s after %u ms",
           accepted ? "accepted" : "rejected",
           static_cast<unsigned>(now_ms() - handshake_started));
  return accepted;
}

}  // namespace

esp_err_t PrinterDiscoveryService::start(NetworkStatus network,
                                         const core::DeviceSettings& settings,
                                         std::string target_host, std::uint16_t target_port,
                                         std::optional<core::PrinterProfile> recovery, std::uint32_t reserved_id,
                                         std::size_t recovery_offset) {
  if ((!target_host.empty() && !valid_printer_setup_host(target_host)) ||
      (target_host.empty() && target_port != 0)) return ESP_ERR_INVALID_ARG;
  if (!network.station_connected || !valid_ipv4(network.ipv4)) return ESP_ERR_INVALID_STATE;
  if (recovery && !core::printer_address_recoverable(*recovery)) return ESP_ERR_INVALID_ARG;
  if (recovery && now_ms() < manual_priority_until_ms_.load()) return ESP_ERR_INVALID_STATE;
  if (!recovery) manual_priority_until_ms_.store(now_ms() + 10000);
  if (!recovery && recovering()) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load() && recovery_mode_.load()) {
      if (++next_scan_id_ == 0) ++next_scan_id_;
      pending_manual_ = ManualRequest{std::move(network), settings, std::move(target_host), target_port, next_scan_id_};
      cancel_requested_.store(true);
      return ESP_OK; // Manual search owns a stable ID while the background worker closes sockets.
    }
  }
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return ESP_ERR_INVALID_STATE;
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.state == PrinterDiscoveryState::scanning) {
      running_.store(false, std::memory_order_release);
      return ESP_ERR_INVALID_STATE;
    }
    const std::uint64_t started_at_ms = now_ms();
    const std::string network_key = discovery_network_key(network);
    if (cache_network_key_ != network_key) last_manual_snapshot_.reset();
    if (recovery && !recovery_mode_.load() &&
        (snapshot_.state == PrinterDiscoveryState::complete || snapshot_.state == PrinterDiscoveryState::failed) &&
        snapshot_.network_name == network.station_name && snapshot_.network_ipv4 == network.ipv4 &&
        snapshot_.network_netmask == network.netmask)
      last_manual_snapshot_ = snapshot_;
    std::vector<DiscoveredPrinter> recent = !recovery && recovery_mode_.load()
        ? (last_manual_snapshot_ ? last_manual_snapshot_->printers : std::vector<DiscoveredPrinter>{})
        : std::move(snapshot_.printers);
    if (recovery || !target_host.empty() || cache_network_key_ != network_key) {
      recent.clear();
    } else {
      recent.erase(
          std::remove_if(recent.begin(), recent.end(), [started_at_ms](const auto& printer) {
            return printer.retain_until_ms == 0 || printer.retain_until_ms <= started_at_ms;
          }),
          recent.end());
      for (auto& printer : recent) printer.seen_in_current_scan = false;
    }
    cache_network_key_ = target_host.empty() ? network_key : std::string{};
    recovery_offset_ = recovery_offset;
    recovery_profile_ = std::move(recovery);
    recovery_mode_.store(recovery_profile_.has_value());
    network_ = std::move(network);
    target_host_ = std::move(target_host);
    target_port_ = target_port;
    saved_ipv4_hosts_.clear();
    saved_prusa_origins_.clear();
    for (const auto& profile : settings.profiles) {
      if (recovery_profile_ || !target_host_.empty()) break;
      if (profile.protocol == core::PrinterProtocol::prusalink) {
        if (const auto origin = prusalink_origin(profile.endpoint)) saved_prusa_origins_.push_back(*origin);
        continue;
      }
      std::string host = ipv4_from_endpoint(profile.endpoint);
      if (!host.empty()) saved_ipv4_hosts_.push_back(std::move(host));
    }
    if (++next_scan_id_ == 0) ++next_scan_id_;
    snapshot_ = {
        .state = PrinterDiscoveryState::scanning,
        .scan_id = reserved_id ? reserved_id : next_scan_id_,
        .progress_percent = 0,
        .network_name = network_.station_name,
        .detail = "Starting local network search…",
        .printers = std::move(recent),
        .network_ipv4 = network_.ipv4,
        .network_netmask = network_.netmask,
    };
    cancel_requested_.store(false);
    pause_requested_.store(false);
  }
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "printer_scan", 16384U, this, recovery_profile_ ? 1 : 2, &task_,
                                     kServiceCore,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = PrinterDiscoveryState::failed;
    snapshot_.detail = "PrintDeck could not start the network search. Please try again.";
    task_ = nullptr;
    running_.store(false, std::memory_order_release);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

bool PrinterDiscoveryService::cancel(std::uint32_t scan_id) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (pending_manual_ && pending_manual_->id == scan_id) {
    pending_manual_.reset();
    return true;
  }
  if (scan_id == 0 || snapshot_.state != PrinterDiscoveryState::scanning ||
      snapshot_.scan_id != scan_id) {
    return false;
  }
  cancel_requested_.store(true);
  return true;
}

bool PrinterDiscoveryService::set_paused(std::uint32_t scan_id, bool paused) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!scan_id || snapshot_.scan_id != scan_id ||
      snapshot_.state != PrinterDiscoveryState::scanning || recovery_mode_.load() ||
      cancel_requested_.load()) return false;
  pause_requested_.store(paused);
  return true;
}

PrinterDiscoverySnapshot PrinterDiscoveryService::snapshot(bool include_recovery) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!include_recovery && pending_manual_) {
    return {.state = PrinterDiscoveryState::scanning, .scan_id = pending_manual_->id,
            .network_name = pending_manual_->network.station_name, .detail = "Starting local network search…"};
  }
  // A background pass must not erase completed manual results from Web Config
  // or from the profile reconciliation worker.
  PrinterDiscoverySnapshot result = !include_recovery && recovery_mode_.load()
      ? last_manual_snapshot_.value_or(PrinterDiscoverySnapshot{}) : snapshot_;
  if (result.state != PrinterDiscoveryState::scanning) {
    const std::uint64_t current_ms = now_ms();
    result.printers.erase(
        std::remove_if(result.printers.begin(), result.printers.end(),
                       [current_ms](const auto& printer) {
                         return printer.retain_until_ms != 0 &&
                                printer.retain_until_ms <= current_ms;
                       }),
        result.printers.end());
  }
  return result;
}

void PrinterDiscoveryService::task_entry(void* context) {
  auto* service = static_cast<PrinterDiscoveryService*>(context);
  service->run();
  std::optional<ManualRequest> pending;
  {
    const std::lock_guard<std::mutex> lock(service->mutex_);
    pending.swap(service->pending_manual_);
  }
  if (pending) service->start(std::move(pending->network), pending->settings,
      std::move(pending->host), pending->port, {}, pending->id);
  vTaskDeleteWithCaps(nullptr);
}

void PrinterDiscoveryService::publish_progress(std::size_t completed, std::size_t total) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const int next = static_cast<int>((completed * 100U) / std::max<std::size_t>(total, 1));
  snapshot_.progress_percent = std::max(snapshot_.progress_percent, std::min(next, 99));
  const std::size_t current_results = static_cast<std::size_t>(std::count_if(
      snapshot_.printers.begin(), snapshot_.printers.end(),
      [](const auto& printer) { return printer.seen_in_current_scan; }));
  snapshot_.detail = current_results == 0
                         ? "Looking for supported printers…"
                         : "Found " + std::to_string(current_results) +
                               (current_results == 1 ? " printer so far…"
                                                     : " printers so far…");
}

void PrinterDiscoveryService::add_result(DiscoveredPrinter result) {
  if (!valid_ipv4(result.host)) return;
  if (!recovery_profile_ && result.protocol == core::PrinterProtocol::moonraker) {
    bool known_identity = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const auto known = std::find_if(snapshot_.printers.begin(), snapshot_.printers.end(), [&](const auto& value) {
        return value.seen_in_current_scan && value.protocol == result.protocol && value.host == result.host && value.port == result.port;
      });
      if (known != snapshot_.printers.end() && !known->network_identity.empty()) {
        result.network_identity = known->network_identity;
        known_identity = true;
      }
    }
    if (!known_identity) {
      core::PrinterProfile endpoint;
      endpoint.protocol = core::PrinterProtocol::moonraker;
      endpoint.endpoint = "http://" + result.host + ":" + std::to_string(result.port);
      const auto identity = moonraker_host_identity(endpoint, [&] { return cancel_requested_.load(); });
      if (result.network_identity.empty() || identity.starts_with("mp:") || identity.starts_with("mh:"))
        result.network_identity = identity;
    }
  }
  if (recovery_profile_) {
    if (!valid_device_peer_ipv4(ntohl(inet_addr(result.host.c_str())), ntohl(inet_addr(network_.ipv4.c_str())),
                               ntohl(inet_addr(network_.netmask.c_str())))) return;
    const auto address = core::printer_address(*recovery_profile_);
    if (!address || result.protocol != recovery_profile_->protocol || result.port != address->port) return;
    if (result.protocol == core::PrinterProtocol::moonraker) {
      if (result.network_identity.empty() || recovery_profile_->network_identity.starts_with("mr:") ||
          recovery_profile_->network_identity.starts_with("mp:") ||
          recovery_profile_->network_identity.starts_with("mh:")) {
        auto candidate = core::printer_at_address(*recovery_profile_, result.host);
        if (!candidate || !moonraker_endpoint_identity_matches(*candidate)) return;
        result.network_identity = recovery_profile_->network_identity;
      }
      if (result.network_identity != recovery_profile_->network_identity) return;
    } else if (result.serial != recovery_profile_->serial) return;
  }
  if (!target_host_.empty() && (result.host != target_host_ ||
      (target_port_ != 0 && result.port != target_port_))) return;
  const std::lock_guard<std::mutex> lock(mutex_);
  result.last_seen_ms = now_ms();
  result.retain_until_ms = 0;
  result.seen_in_current_scan = true;
  if (result.protocol == core::PrinterProtocol::prusalink) {
    const auto origin = prusalink_origin(result.host + ":" + std::to_string(result.port));
    if (origin && std::find(saved_prusa_origins_.begin(), saved_prusa_origins_.end(), *origin) !=
                      saved_prusa_origins_.end()) return;
  } else if (target_host_.empty() && std::find(saved_ipv4_hosts_.begin(), saved_ipv4_hosts_.end(), result.host) !=
             saved_ipv4_hosts_.end()) return;
  const auto existing = std::find_if(snapshot_.printers.begin(), snapshot_.printers.end(),
                                     [this, &result](const DiscoveredPrinter& value) {
    if (value.protocol != result.protocol) return false;
    if (recovery_profile_) return value.host == result.host && value.port == result.port;
    if (!result.network_identity.empty() && !value.network_identity.empty())
      return value.network_identity == result.network_identity && value.host == result.host && value.port == result.port;
    if (result.protocol == core::PrinterProtocol::moonraker &&
        (!result.network_identity.empty() || !value.network_identity.empty()))
      return value.host == result.host && value.port == result.port;
    if (!result.serial.empty() && !value.serial.empty())
      return value.serial == result.serial && (!value.seen_in_current_scan || value.host == result.host);
    return value.host == result.host && ((result.protocol != core::PrinterProtocol::prusalink && result.protocol != core::PrinterProtocol::tinymaker) || value.port == result.port);
  });
  if (existing != snapshot_.printers.end()) {
    existing->host = result.host;
    if (!result.name.empty()) existing->name = std::move(result.name);
    if (!result.model.empty()) existing->model = std::move(result.model);
    if (!result.serial.empty()) existing->serial = std::move(result.serial);
    if (!result.network_identity.empty()) existing->network_identity = std::move(result.network_identity);
    if (existing->port == 0 || existing->port == 80) existing->port = result.port;
    existing->last_seen_ms = result.last_seen_ms;
    existing->retain_until_ms = 0;
    existing->seen_in_current_scan = true;
    return;
  }
  if (snapshot_.printers.size() >= kMaximumResults) return;
  if (result.name.empty()) {
    result.name = result.protocol == core::PrinterProtocol::prusalink ? "Prusa" : result.protocol == core::PrinterProtocol::bambu_lan
                      ? "Bambu Lab printer"
                      : "Klipper printer";
  }
  snapshot_.printers.push_back(std::move(result));
}

void PrinterDiscoveryService::run() {
  struct RunningGuard {
    std::atomic<bool>& running;
    ~RunningGuard() { running.store(false, std::memory_order_release); }
  } running_guard{running_};
  std::uint64_t started = now_ms();
  const bool targeted = !target_host_.empty();
  std::uint64_t deadline = started + (targeted ? 35000 : kMaximumDurationMs);
  // Resolve only the user-entered host, on the worker, then enforce the actual
  // station subnet before opening printer sockets. DNS never selects a remote
  // HTTP destination or expands a targeted request into a subnet scan.
  if (targeted) {
    in_addr address{};
    if (inet_pton(AF_INET, target_host_.c_str(), &address) != 1) {
      const auto hostname = lower(target_host_);
      if (hostname.ends_with(".local")) {
        esp_ip4_addr_t resolved{};
        if (mdns_query_a(hostname.substr(0, hostname.size() - 6).c_str(), 2000, &resolved) == ESP_OK)
          address.s_addr = resolved.addr;
      } else {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* resolved = nullptr;
        if (getaddrinfo(hostname.c_str(), nullptr, &hints, &resolved) == 0 && resolved)
          address = reinterpret_cast<sockaddr_in*>(resolved->ai_addr)->sin_addr;
        if (resolved) freeaddrinfo(resolved);
      }
    }
    if (cancel_requested_.load() || now_ms() >= deadline ||
        !valid_device_peer_ipv4(ntohl(address.s_addr), ntohl(inet_addr(network_.ipv4.c_str())),
                               ntohl(inet_addr(network_.netmask.c_str())))) {
      const std::lock_guard<std::mutex> lock(mutex_);
      snapshot_.state = cancel_requested_.load() ? PrinterDiscoveryState::idle : PrinterDiscoveryState::failed;
      snapshot_.detail = "No supported printers were found. You can still add one manually.";
      task_ = nullptr;
      return;
    }
    char host[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address, host, sizeof(host));
    target_host_ = host;
  }
  const auto target_found = [&] { return targeted && !snapshot(true).printers.empty(); };
  ESP_LOGI(kLogTag,
           "Network search started; internal=%u, largest-internal=%u, "
           "largest-dma=%u",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(
               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)));

  constexpr std::uint16_t kSsdpListenPorts[]{1990, 2021};
  int ssdp_sockets[]{-1, -1};
  const in_addr_t multicast = inet_addr(kSsdpGroup);
  constexpr char kSearch[] =
      "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
      "MAN: \"ssdp:discover\"\r\nMX: 2\r\n"
      "ST: urn:bambulab-com:device:3dprinter:1\r\n\r\n";
  for (std::size_t index = 0; index < 2 &&
       (!recovery_profile_ || recovery_profile_->protocol == core::PrinterProtocol::bambu_lan); ++index) {
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) continue;
    int reuse = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    fcntl(socket_fd, F_SETFL, O_NONBLOCK);
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(kSsdpListenPorts[index]);
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    ip_mreq membership{};
    membership.imr_multiaddr.s_addr = multicast;
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&bind_address),
             sizeof(bind_address)) != 0 ||
        setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                   sizeof(membership)) != 0) {
      close(socket_fd);
      continue;
    }
    ssdp_sockets[index] = socket_fd;
  }

  const auto send_ssdp = [&]() {
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(1900);
    destination.sin_addr.s_addr = targeted ? inet_addr(target_host_.c_str()) : multicast;
    for (const int socket_fd : ssdp_sockets) {
      if (socket_fd >= 0) sendto(socket_fd, kSearch, sizeof(kSearch) - 1, 0,
                                 reinterpret_cast<const sockaddr*>(&destination),
                                 sizeof(destination));
    }
  };
  const auto drain_ssdp = [&]() {
    for (const int socket_fd : ssdp_sockets) {
      if (socket_fd < 0) continue;
      for (unsigned packet_index = 0; packet_index < 12 &&
           !cancel_requested_.load() && now_ms() < deadline; ++packet_index) {
        char buffer[1537]{};
        sockaddr_in source{};
        socklen_t source_size = sizeof(source);
        const int received = recvfrom(socket_fd, buffer, sizeof(buffer) - 1, 0,
                                      reinterpret_cast<sockaddr*>(&source), &source_size);
        if (received <= 0) break;
        char source_host[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &source.sin_addr, source_host, sizeof(source_host)) == nullptr) {
          continue;
        }
        const std::string packet(buffer, static_cast<std::size_t>(received));
        const std::string serial = trim(header_value(packet, "USN"));
        if (!valid_bambu_serial(serial)) continue;
        std::string model = trim(header_value(packet, "DevModel.bambu.com"));
        const BambuPrinterModel detected_model = bambu_model_from_identity(model);
        if (detected_model != BambuPrinterModel::unknown) {
          model = bambu_model_name(detected_model);
        }
        std::string name = trim(header_value(packet, "DevName.bambu.com"));
        if (name.size() > 48) name.resize(48);
        add_result({.protocol = core::PrinterProtocol::bambu_lan,
                    .name = std::move(name),
                    .model = std::move(model),
                    .host = ssdp_host(header_value(packet, "Location"), source_host),
                    .serial = serial,
                    .port = kBambuTlsPort});
      }
    }
  };
  const auto ssdp_active = [&]() {
    return ssdp_sockets[0] >= 0 || ssdp_sockets[1] >= 0;
  };
  const auto close_ssdp = [&]() {
    for (int& socket_fd : ssdp_sockets) {
      if (socket_fd >= 0) close(socket_fd);
      socket_fd = -1;
    }
  };
  std::size_t ssdp_search_round = 0;
  std::uint64_t next_ssdp_search_ms = started;
  std::uint64_t last_ssdp_search_ms = 0;
  const auto service_ssdp = [&]() {
    const std::uint64_t current = now_ms();
    if (ssdp_active() && !cancel_requested_.load() && current < deadline &&
        PrinterDiscoveryTiming::bambu_ssdp_should_send(
                             ssdp_search_round, current, next_ssdp_search_ms)) {
      send_ssdp();
      last_ssdp_search_ms = current;
      next_ssdp_search_ms = current +
                            PrinterDiscoveryTiming::bambu_ssdp_search_interval_ms;
      ++ssdp_search_round;
    }
    drain_ssdp();
    if (ssdp_active() && PrinterDiscoveryTiming::bambu_ssdp_responses_complete(
                             ssdp_search_round, current, last_ssdp_search_ms)) {
      // The final spaced M-SEARCH has retained its complete MX=2 response
      // window. Continue with the TLS fallback without keeping two UDP
      // descriptors alive for the remainder of the subnet scan.
      close_ssdp();
      ESP_LOGI(kLogTag, "Bambu SSDP window finished: rounds=%u results=%u",
               static_cast<unsigned>(ssdp_search_round),
               static_cast<unsigned>(snapshot(true).printers.size()));
    }
  };
  service_ssdp();

  const in_addr_t station_address = inet_addr(network_.ipv4.c_str());
  const in_addr_t netmask_address = inet_addr(network_.netmask.c_str());
  if (station_address == INADDR_NONE) {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = PrinterDiscoveryState::failed;
    snapshot_.detail = "PrintDeck could not determine the current Wi-Fi network.";
    task_ = nullptr;
    for (const int socket_fd : ssdp_sockets) if (socket_fd >= 0) close(socket_fd);
    return;
  }
  const std::uint32_t local = ntohl(station_address);
  std::uint32_t mask = netmask_address == INADDR_NONE ? 0xFFFFFF00U : ntohl(netmask_address);
  const std::uint32_t host_bits = ~mask;
  if (recovery_profile_ && ((host_bits & (host_bits + 1U)) != 0 || host_bits < 2 || host_bits > 4095)) {
    close_ssdp();
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = PrinterDiscoveryState::failed;
    task_ = nullptr;
    return;
  }
  if ((host_bits & (host_bits + 1U)) != 0 || host_bits < 2 || (!targeted && !recovery_profile_ && host_bits > 255)) {
    mask = 0xFFFFFF00U;
  }
  const std::uint32_t network = local & mask;
  const std::uint32_t broadcast = network | ~mask;
  // One shared UDP descriptor for the two proven Elegoo discovery dialects.
  // Its lifetime and retry count are bounded independently of the subnet pass.
  const bool want_elegoo = !recovery_profile_ || recovery_profile_->protocol == core::PrinterProtocol::elegoo_sdcp ||
                           recovery_profile_->protocol == core::PrinterProtocol::elegoo_cc2;
  int elegoo_socket = want_elegoo ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : -1;
  struct ElegooSocketGuard {
    int& descriptor;
    ~ElegooSocketGuard() { if (descriptor >= 0) close(descriptor); }
  } elegoo_socket_guard{elegoo_socket};
  if (elegoo_socket >= 0) {
    int enabled = 1;
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr.s_addr = station_address;
    if (setsockopt(elegoo_socket, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) != 0 ||
        bind(elegoo_socket, reinterpret_cast<const sockaddr*>(&bind_address), sizeof(bind_address)) != 0 ||
        fcntl(elegoo_socket, F_SETFL, O_NONBLOCK) < 0) {
      close(elegoo_socket); elegoo_socket = -1;
    }
  }
  auto elegoo_started = now_ms();
  unsigned elegoo_round = 0;
  const auto service_elegoo = [&] {
    if (elegoo_socket < 0) return;
    const auto current = now_ms();
    if (cancel_requested_.load() || current >= deadline || current >= elegoo_started + 8000) {
      close(elegoo_socket); elegoo_socket = -1; return;
    }
    if (elegoo_round < 2 && current >= elegoo_started + elegoo_round * 3000) {
      sockaddr_in destination{};
      destination.sin_family = AF_INET;
      destination.sin_addr.s_addr = targeted ? inet_addr(target_host_.c_str()) : htonl(broadcast);
      const std::pair<std::uint16_t, std::string_view> requests[] = {
          {3000, "M99999"}, {52700, "{\"id\":0,\"method\":7000}"}};
      for (const auto& [port, body] : requests) {
        destination.sin_port = htons(port);
        sendto(elegoo_socket, body.data(), body.size(), 0,
               reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
      }
      ++elegoo_round;
    }
    for (unsigned packet = 0; packet < 12 && !cancel_requested_.load(); ++packet) {
      char bytes[4097];
      sockaddr_in source{};
      socklen_t size = sizeof(source);
      const int count = recvfrom(elegoo_socket, bytes, sizeof(bytes), 0,
          reinterpret_cast<sockaddr*>(&source), &size);
      if (count <= 0) break;
      if (count >= static_cast<int>(sizeof(bytes))) continue;
      const auto address = ntohl(source.sin_addr.s_addr);
      if ((address & mask) != network || address == local || address == network || address == broadcast) continue;
      char host[INET_ADDRSTRLEN]{};
      if (!inet_ntop(AF_INET, &source.sin_addr, host, sizeof(host))) continue;
      const std::string_view body(bytes, static_cast<std::size_t>(count));
      ElegooIdentity identity;
      if (ntohs(source.sin_port) == 3000 && elegoo_sdcp_decode_discovery(body, identity)) {
        add_result({.protocol = core::PrinterProtocol::elegoo_sdcp, .name = "ELEGOO " + identity.model,
                    .model = identity.model, .host = host, .serial = identity.serial, .port = 3030});
      } else if (ntohs(source.sin_port) == 52700) {
        const auto cc2 = parse_elegoo_cc2_discovery(body);
        if (cc2) add_result({.protocol = core::PrinterProtocol::elegoo_cc2,
                            .name = "ELEGOO " + cc2->identity.model, .model = cc2->identity.model,
                            .host = host, .serial = cc2->identity.serial, .port = 1883});
      }
    }
  };
  service_elegoo();
  const auto uniformation_identity = [&](const std::string& host, std::uint16_t port) {
    core::PrinterProfile profile;
    profile.protocol = core::PrinterProtocol::uniformation_sdcp;
    profile.endpoint = host + ":" + std::to_string(port);
    ElegooIdentity identity;
    const auto cancelled = [&] { return cancel_requested_.load() || now_ms() >= deadline; };
    const auto result = uniformation_sdcp_probe(profile, std::min(deadline, now_ms() + 3500), cancelled, &identity);
    if (!cancelled() && result.snapshot && result.snapshot->link == core::LinkState::online)
      add_result({.protocol = core::PrinterProtocol::uniformation_sdcp, .name = "UniFormation " + identity.model,
                  .model = identity.model, .host = host, .serial = identity.serial, .port = port});
  };
  const auto prusa_identity = [&](const std::string& host, std::uint16_t port, bool advertised = false) {
    const auto cancelled = [&] { return cancel_requested_.load() || now_ms() >= deadline; };
    std::unique_lock<std::timed_mutex> transaction(prusalink_transaction_mutex(), std::defer_lock);
    // A short bounded wait avoids dropping an advertised printer merely
    // because an authenticated status poll currently owns the transport.
    const auto lock_deadline = std::min(deadline, now_ms() + 1200);
    while (!transaction.try_lock_for(std::chrono::milliseconds(20)))
      if (cancelled() || now_ms() >= lock_deadline) return false;
    if (cancelled()) return false;
    const std::string origin = "http://" + host + (port == 80 ? "" : ":" + std::to_string(port));
    PrusaLinkEspTransport transport;
    const auto budget = std::min(deadline, now_ms() + 1800);
    const auto version = transport.get({.url = origin + "/api/version", .maximum_body = 16384, .deadline_ms = budget}, cancelled);
    if (targeted) ESP_LOGI(kLogTag, "HTTP identity check: transport=%u status=%d bytes=%u",
        static_cast<unsigned>(version.error), version.status, static_cast<unsigned>(version.body.size()));
    if (version.error == PrusaLinkError::none && version.status == 200 &&
        (version.content_encoding.empty() || version.content_encoding == "identity") && tinymaker_identity(version.body)) {
      TinyMakerClient client(transport, now_ms);
      if (client.configure(origin, 0) && client.identify(version.body)) {
        const auto result = client.poll(std::min(deadline, now_ms() + 3500), cancelled);
        ESP_LOGI(kLogTag, "TinyMaker status verification: result=%u ready=%s",
            static_cast<unsigned>(result.error), result.sample ? "yes" : "no");
        if (result.sample && !cancelled())
          add_result({.protocol = core::PrinterProtocol::tinymaker, .name = "TinyMaker",
                      .model = "TinyMaker", .host = host, .port = port});
      }
      return false;
    }
    if (prusalink_discovery_identity(version, nullptr, advertised)) {
      if (const auto identity = parse_prusalink_identity(version.body))
        add_result({.protocol = core::PrinterProtocol::prusalink, .name = "Prusa",
                    .model = identity->model, .host = host, .port = port});
      return true;
    }
    if (version.status != 401 || cancelled()) return false;
    const auto root = transport.get({.url = origin + "/", .maximum_body = 65536, .deadline_ms = budget}, cancelled);
    return !cancelled() && prusalink_discovery_identity(version, &root);
  };
  if (!targeted && (!recovery_profile_ || recovery_profile_->protocol == core::PrinterProtocol::moonraker)) {
    for (auto& printer : discover_moonraker_identities()) {
      if (cancel_requested_.load() || now_ms() >= deadline) break;
      if (valid_device_peer_ipv4(ntohl(inet_addr(printer.host.c_str())), local, mask)) add_result(std::move(printer));
    }
  }
  mdns_result_t* mdns_results = nullptr;
  unsigned prusa_services = 0;
  unsigned prusa_addresses = 0;
  unsigned prusa_verified = 0;
  const auto check_prusa_address = [&](in_addr native, std::uint16_t port) {
    const auto ipv4 = ntohl(native.s_addr);
    if ((ipv4 & mask) != network || ipv4 == local || ipv4 == network || ipv4 == broadcast) return;
    ++prusa_addresses;
    char host[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &native, host, sizeof(host)) && prusa_identity(host, port, true)) {
      ++prusa_verified;
      add_result({.protocol = core::PrinterProtocol::prusalink, .name = "Prusa", .host = host, .port = port});
    }
  };
  // Cover the responder's retry interval, including Wi-Fi multicast delivery.
  // The query remains bounded, and cancellation is checked before HTTP work.
  const esp_err_t prusa_mdns_result = (targeted || recovery_profile_ || cancel_requested_.load()) ? ESP_ERR_INVALID_STATE
      : mdns_query_ptr("_prusalink", "_tcp", 2400, 10, &mdns_results);
  if (prusa_mdns_result == ESP_OK) {
    for (const auto* result = mdns_results; result && !cancel_requested_.load(); result = result->next) {
      ++prusa_services;
      if (!result->port) continue;
      bool has_ipv4 = false;
      for (const auto* address = result->addr; address; address = address->next) {
        if (address->addr.type != ESP_IPADDR_TYPE_V4) continue;
        has_ipv4 = true;
        check_prusa_address({.s_addr = address->addr.u_addr.ip4.addr}, result->port);
      }
      // DNS-SD responders may send SRV/TXT without an additional A record.
      // Resolve only that advertised local hostname and reapply subnet bounds.
      if (!has_ipv4 && result->hostname && result->hostname[0] &&
          !cancel_requested_.load() && now_ms() + 700 < deadline) {
        esp_ip4_addr_t resolved{};
        if (mdns_query_a(result->hostname, 700, &resolved) == ESP_OK)
          check_prusa_address({.s_addr = resolved.addr}, result->port);
      }
    }
  }
  if (mdns_results) mdns_query_results_free(mdns_results);
  ESP_LOGI(kLogTag, "Prusa mDNS: result=%s services=%u local-addresses=%u verified=%u",
           esp_err_to_name(prusa_mdns_result), prusa_services, prusa_addresses, prusa_verified);
  service_elegoo();
  struct Candidate { in_addr address{}; std::string host; };
  std::vector<Candidate> candidates;
  const auto first_address = targeted ? ntohl(inet_addr(target_host_.c_str())) : network + 1;
  const auto last_address = targeted ? first_address + 1 : broadcast;
  for (std::uint32_t address = first_address; address < last_address; ++address) {
    if (address == local) continue;
    in_addr candidate_address{.s_addr = htonl(address)};
    char text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &candidate_address, text, sizeof(text)) == nullptr) continue;
    const std::string host = text;
    candidates.push_back({candidate_address, host});
  }

  if (recovery_profile_ && !candidates.empty()) {
    const auto address = core::printer_address(*recovery_profile_);
    std::size_t origin = 0;
    if (address) {
      const auto previous = std::find_if(candidates.begin(), candidates.end(),
          [&](const auto& value) { return value.host == address->host; });
      if (previous != candidates.end()) origin = std::distance(candidates.begin(), previous) + 1;
    }
    const auto offset = (origin + recovery_offset_) % candidates.size();
    std::rotate(candidates.begin(), candidates.begin() + offset, candidates.end());
    if (candidates.size() > 32) candidates.resize(32);
  }

  struct Pass {
    std::uint16_t port;
    core::PrinterProtocol protocol;
    bool verify_moonraker;
    std::uint32_t timeout_ms;
  };
  // Complete the known services for each of a few concurrent addresses.
  // HTTP goes first and gets the cold-neighbor timeout; later ports reuse ARP.
  std::vector<Pass> passes{
      {80, core::PrinterProtocol::moonraker, true,
       PrinterDiscoveryTiming::http_tcp_connect_timeout_ms},
      {3030, core::PrinterProtocol::uniformation_sdcp, false, 250},
      {7125, core::PrinterProtocol::moonraker, false, 250},
      {kBambuTlsPort, core::PrinterProtocol::bambu_lan, false,
       PrinterDiscoveryTiming::bambu_tcp_connect_timeout_ms},
      {4408, core::PrinterProtocol::moonraker, true, 250},
      {4409, core::PrinterProtocol::moonraker, true, 250},
  };
  if (targeted && target_port_ != 0) {
    passes = {{target_port_, target_port_ == 3030 ? core::PrinterProtocol::uniformation_sdcp
                : target_port_ == kBambuTlsPort ? core::PrinterProtocol::bambu_lan
                : core::PrinterProtocol::moonraker, target_port_ != kBambuTlsPort && target_port_ != 3030, 1200}};
    // CC2 identifies itself through UDP; never send HTTP to its MQTT port.
    if (target_port_ == 1883) passes.clear();
  } else if (targeted) {
    for (auto& pass : passes) if (pass.protocol == core::PrinterProtocol::moonraker) pass.verify_moonraker = true;
  }
  if (recovery_profile_) {
    const auto address = core::printer_address(*recovery_profile_);
    passes.erase(std::remove_if(passes.begin(), passes.end(), [&](const auto& pass) {
      return !address || pass.port != address->port;
    }), passes.end());
    if (passes.empty() && address && recovery_profile_->protocol != core::PrinterProtocol::elegoo_cc2 &&
        recovery_profile_->protocol != core::PrinterProtocol::elegoo_sdcp)
      passes.push_back({address->port, recovery_profile_->protocol, recovery_profile_->protocol == core::PrinterProtocol::moonraker, 1000});
  }
  struct Pending {
    int socket_fd;
    PrinterDiscoverySchedule::Probe probe;
    std::uint64_t deadline_ms;
  };
  PrinterDiscoverySchedule schedule(candidates.size(), passes.size());
  std::vector<Pending> pending;
  pending.reserve(kProbeBatchSize);
  const std::size_t total = std::max<std::size_t>(candidates.size() * passes.size(), 1);
  std::size_t completed = 0;
  bool resource_pressure = false;
  const auto stopped = [&] {
    return cancel_requested_.load() || now_ms() >= deadline || target_found();
  };
  const auto complete = [&](PrinterDiscoverySchedule::Probe probe) {
    schedule.complete(probe);
    publish_progress(++completed, total);
    if (recovery_profile_) {
      for (unsigned pause = 0; pause < 3 && !stopped(); ++pause) vTaskDelay(pdMS_TO_TICKS(50));
    }
  };
  const auto verify = [&](Pending& pending_probe) {
    const auto& pass = passes[pending_probe.probe.service];
    const auto& host = candidates[pending_probe.probe.address].host;
    auto& socket_fd = pending_probe.socket_fd;
    int socket_error = ECONNREFUSED;
    socklen_t error_size = sizeof(socket_error);
    bool accepted = getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                               &error_size) == 0 && socket_error == 0;
    if (accepted && pass.protocol == core::PrinterProtocol::uniformation_sdcp) {
      close(socket_fd); socket_fd = -1;
      uniformation_identity(host, pass.port);
      accepted = false;
    } else if (accepted && pass.verify_moonraker) {
      accepted = moonraker_signature(socket_fd, host, deadline);
      if (!accepted && (pass.port == 80 || targeted)) {
        close(socket_fd); socket_fd = -1;
        if (prusa_identity(host, pass.port))
          add_result({.protocol = core::PrinterProtocol::prusalink, .name = "Prusa", .host = host, .port = pass.port});
      }
    } else if (accepted && pass.protocol == core::PrinterProtocol::bambu_lan) {
      close(socket_fd); socket_fd = -1;
      accepted = bambu_tls_identity(host, deadline, recovery_profile_ ? recovery_profile_->serial.c_str() : nullptr);
    }
    if (accepted) add_result({.protocol = pass.protocol,
                              .name = {}, .model = {}, .host = host,
                              .serial = recovery_profile_ && pass.protocol == core::PrinterProtocol::bambu_lan ? recovery_profile_->serial : std::string{},
                              .port = pass.port});
  };
  // Resolve ready or individually expired probes, then immediately refill free
  // lanes. A slow address never holds completed addresses behind a batch fence.
  const auto service_pending = [&] {
    if (pending.empty()) return;
    fd_set writable;
    fd_set errors;
    FD_ZERO(&writable);
    FD_ZERO(&errors);
    int maximum_socket = -1;
    auto wait_deadline = std::min(deadline, now_ms() + 50);
    for (const auto& probe : pending) {
      FD_SET(probe.socket_fd, &writable);
      FD_SET(probe.socket_fd, &errors);
      maximum_socket = std::max(maximum_socket, probe.socket_fd);
      wait_deadline = std::min(wait_deadline, probe.deadline_ms);
    }
    const auto remaining = PrinterDiscoveryTiming::bounded_wait_ms(now_ms(), wait_deadline, 50);
    timeval timeout{.tv_sec = 0, .tv_usec = static_cast<suseconds_t>(remaining * 1000)};
    const int ready = select(maximum_socket + 1, nullptr, &writable, &errors, &timeout);
    if (ready < 0 && errno == EINTR) return;
    const auto observed_at = now_ms();
    std::size_t waiting_count = 0;
    for (std::size_t index = 0; index < pending.size(); ++index) {
      auto& probe = pending[index];
      const bool ready_now = ready > 0 &&
          (FD_ISSET(probe.socket_fd, &writable) || FD_ISSET(probe.socket_fd, &errors));
      if (ready >= 0 && !ready_now && observed_at < probe.deadline_ms && !stopped()) {
        if (waiting_count != index) pending[waiting_count] = std::move(probe);
        ++waiting_count;
        continue;
      }
      if (ready < 0) resource_pressure = true;
      if (ready_now && !stopped()) verify(probe);
      if (probe.socket_fd >= 0) close(probe.socket_fd);
      complete(probe.probe);
    }
    pending.resize(waiting_count);
  };

  // Acknowledge only at a quiet boundary, after every TCP probe has closed.
  // The worker keeps its schedule/results and sends no traffic while paused.
  const auto pause_at_boundary = [&] {
    if (!pause_requested_.load() || cancel_requested_.load()) return;
    while (!pending.empty() && !stopped()) service_pending();
    if (stopped()) return;
    const auto pause_started = now_ms();
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      snapshot_.paused = true;
    }
    while (pause_requested_.load() && !cancel_requested_.load()) {
      // An abandoned browser must not reserve the worker forever.
      if (now_ms() - pause_started >= 300000) {
        cancel_requested_.store(true);
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    const auto elapsed = now_ms() - pause_started;
    deadline += elapsed;
    started += elapsed;
    elegoo_started += elapsed;
    next_ssdp_search_ms += elapsed;
    if (last_ssdp_search_ms) last_ssdp_search_ms += elapsed;
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.paused = false;
  };

  while (!schedule.done() && !stopped()) {
    pause_at_boundary();
    if (stopped()) break;
    service_ssdp();
    service_elegoo();
    const std::size_t batch_size = recovery_profile_ ? 1 :
        (ssdp_active() ? kProbeBatchSizeWithSsdp : kProbeBatchSize) - (elegoo_socket >= 0 ? 1 : 0);
    while (pending.size() < batch_size && !stopped() && !pause_requested_.load()) {
      const auto probe = schedule.next();
      if (!probe) break;
      const auto& pass = passes[probe->service];
      const auto& candidate = candidates[probe->address];
      int socket_fd = -1;
      for (std::size_t attempt = 0;
           attempt < kSocketOpenAttempts && socket_fd < 0 && !stopped(); ++attempt) {
        socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_fd >= 0) break;
        service_pending();
        service_ssdp();
        service_elegoo();
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if (socket_fd < 0) {
        resource_pressure = true;
        complete(*probe);
        continue;
      }
      if (stopped()) { close(socket_fd); complete(*probe); break; }
      fcntl(socket_fd, F_SETFL, O_NONBLOCK);
      sockaddr_in target{};
      target.sin_family = AF_INET;
      target.sin_port = htons(pass.port);
      target.sin_addr = candidate.address;
      const auto probe_deadline = std::min(deadline, now_ms() +
          PrinterDiscoveryTiming::tcp_connect_timeout_ms(probe->service, pass.timeout_ms));
      const int result = connect(socket_fd, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
      if (result == 0 || errno == EINPROGRESS) {
        pending.push_back({socket_fd, *probe, probe_deadline});
      } else {
        close(socket_fd);
        complete(*probe);
      }
    }
    service_pending();
  }
  for (const auto& probe : pending) close(probe.socket_fd);
  pending.clear();

  while (!cancel_requested_.load() && now_ms() < deadline && !target_found() &&
         (now_ms() < started + kMinimumDurationMs || ssdp_active() || elegoo_socket >= 0)) {
    pause_at_boundary();
    if (stopped()) break;
    service_ssdp();
    service_elegoo();
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  service_ssdp();
  service_elegoo();
  close_ssdp();

  bool timed_out = now_ms() >= deadline;
  bool cancelled = cancel_requested_.load();
  bool deduplication_complete = true;
  std::vector<DiscoveredPrinter> deduplicated;
  std::vector<std::vector<std::string>> moonraker_identities;
  if (!cancelled) {
    for (auto& printer : snapshot(true).printers) {
      pause_at_boundary();
      if (cancel_requested_.load()) {
        cancelled = true;
        break;
      }
      if (now_ms() >= deadline) {
        timed_out = true;
        deduplication_complete = false;
        break;
      }
      if (targeted || !printer.seen_in_current_scan ||
          printer.protocol != core::PrinterProtocol::moonraker) {
        deduplicated.push_back(std::move(printer));
        continue;
      }
      // Reuse the existing interface reconciliation for recovery too: Ethernet
      // and Wi-Fi addresses reported by the same host are one printer. Include
      // the service port and learned identity so separate instances stay apart.
      auto identity = moonraker_interface_addresses(printer.host, printer.port, deadline);
      if (std::find(identity.begin(), identity.end(), printer.host) == identity.end()) identity.clear();
      if (!identity.empty()) {
        identity.push_back("port:" + std::to_string(printer.port));
        identity.push_back("identity:" + printer.network_identity);
      }
      if (!identity.empty() &&
          std::find(moonraker_identities.begin(), moonraker_identities.end(), identity) !=
              moonraker_identities.end()) {
        ESP_LOGI(kLogTag, "Merged Moonraker interface %s into an existing printer result",
                 printer.host.c_str());
        continue;
      }
      if (!identity.empty()) moonraker_identities.push_back(std::move(identity));
      deduplicated.push_back(std::move(printer));
    }
  }
  timed_out = timed_out || now_ms() >= deadline;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    cancelled = cancel_requested_.exchange(false) || cancelled;
    snapshot_.paused = false;
    pause_requested_.store(false);
    if (!cancelled && deduplication_complete) snapshot_.printers = std::move(deduplicated);
    const std::uint64_t completed_at_ms = now_ms();
    for (auto& printer : snapshot_.printers) {
      if (printer.seen_in_current_scan) {
        printer.retain_until_ms = completed_at_ms + kRecentResultLifetimeMs;
      }
    }
    snapshot_.printers.erase(
        std::remove_if(snapshot_.printers.begin(), snapshot_.printers.end(),
                       [completed_at_ms](const auto& printer) {
                         return !printer.seen_in_current_scan &&
                                (printer.retain_until_ms == 0 ||
                                 printer.retain_until_ms <= completed_at_ms);
                       }),
        snapshot_.printers.end());
    std::sort(snapshot_.printers.begin(), snapshot_.printers.end(),
              [](const DiscoveredPrinter& left, const DiscoveredPrinter& right) {
      if (left.seen_in_current_scan != right.seen_in_current_scan) {
        return left.seen_in_current_scan;
      }
      if (left.protocol != right.protocol) return left.protocol == core::PrinterProtocol::bambu_lan;
      return left.name < right.name;
    });
    const bool found_current = std::any_of(
        snapshot_.printers.begin(), snapshot_.printers.end(),
        [](const auto& printer) { return printer.seen_in_current_scan; });
    if (cancelled) {
      snapshot_.state = PrinterDiscoveryState::idle;
      snapshot_.detail = "Network search stopped.";
    } else if (timed_out && !found_current) {
      snapshot_.state = PrinterDiscoveryState::failed;
      snapshot_.detail = "The 3-minute safety limit was reached before a supported printer responded.";
    } else if (timed_out) {
      snapshot_.state = PrinterDiscoveryState::complete;
      snapshot_.progress_percent = 100;
      snapshot_.detail = "Network search complete. Results found before the safety limit are shown.";
    } else if (resource_pressure && !found_current) {
      snapshot_.state = PrinterDiscoveryState::failed;
      snapshot_.detail = "The network was busy, so the search could not be completed. Try again.";
      snapshot_.progress_percent = 100;
    } else if (resource_pressure) {
      snapshot_.state = PrinterDiscoveryState::complete;
      snapshot_.detail = "Network search complete. Available printers are shown.";
      snapshot_.progress_percent = 100;
    } else {
      snapshot_.state = PrinterDiscoveryState::complete;
      snapshot_.progress_percent = 100;
      snapshot_.detail = !found_current
                             ? "No supported printers were found. You can still add one manually."
                             : "Network search complete.";
    }
    task_ = nullptr;
  }
  ESP_LOGI(kLogTag,
           "Network search finished: candidates=%u results=%u internal=%u, "
           "largest-internal=%u, largest-dma=%u, stack high-water=%u",
           static_cast<unsigned>(candidates.size()),
           static_cast<unsigned>(snapshot(true).printers.size()),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(
               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

}  // namespace printdeck::platform
