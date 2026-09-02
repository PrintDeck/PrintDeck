#include "printdeck/platform/printer_discovery_service.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
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
#include "lwip/sockets.h"
#include "printdeck/platform/bambu_trust.hpp"
#include "printdeck/platform/bambu_model.hpp"
#include "printdeck/platform/printer_discovery_timing.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "sdkconfig.h"

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
constexpr std::size_t kProbeBatchSize = 5;
constexpr std::size_t kProbeBatchSizeWithSsdp = 3;
constexpr std::size_t kSocketOpenAttempts = 12;
constexpr std::size_t kMaximumResults = 24;
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

bool bambu_tls_identity(const std::string& host, std::uint64_t deadline_ms) {
  const std::uint64_t current = now_ms();
  if (current >= deadline_ms) return false;
  const char* anchors = bambu_trust_anchors();
  esp_tls_cfg_t config{};
  config.cacert_buf = reinterpret_cast<const unsigned char*>(anchors);
  config.cacert_bytes = static_cast<unsigned int>(std::strlen(anchors) + 1);
  config.skip_common_name = true;
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
                                         const core::DeviceSettings& settings) {
  if (!network.station_connected || !valid_ipv4(network.ipv4)) return ESP_ERR_INVALID_STATE;
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
    network_ = std::move(network);
    saved_ipv4_hosts_.clear();
    for (const auto& profile : settings.profiles) {
      std::string host = ipv4_from_endpoint(profile.endpoint);
      if (!host.empty()) saved_ipv4_hosts_.push_back(std::move(host));
    }
    if (++next_scan_id_ == 0) ++next_scan_id_;
    snapshot_ = {
        .state = PrinterDiscoveryState::scanning,
        .scan_id = next_scan_id_,
        .progress_percent = 0,
        .network_name = network_.station_name,
        .detail = "Starting local network search…",
        .printers = {},
    };
    cancel_requested_.store(false);
  }
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "printer_scan", 16384, this, 2, &task_,
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
  if (scan_id == 0 || snapshot_.state != PrinterDiscoveryState::scanning ||
      snapshot_.scan_id != scan_id) {
    return false;
  }
  cancel_requested_.store(true);
  return true;
}

PrinterDiscoverySnapshot PrinterDiscoveryService::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void PrinterDiscoveryService::task_entry(void* context) {
  static_cast<PrinterDiscoveryService*>(context)->run();
  vTaskDeleteWithCaps(nullptr);
}

void PrinterDiscoveryService::publish_progress(std::size_t completed, std::size_t total) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const int next = static_cast<int>((completed * 100U) / std::max<std::size_t>(total, 1));
  snapshot_.progress_percent = std::max(snapshot_.progress_percent, std::min(next, 99));
  snapshot_.detail = snapshot_.printers.empty()
                         ? "Looking for supported printers…"
                         : "Found " + std::to_string(snapshot_.printers.size()) +
                               (snapshot_.printers.size() == 1 ? " printer so far…"
                                                               : " printers so far…");
}

void PrinterDiscoveryService::add_result(DiscoveredPrinter result) {
  if (!valid_ipv4(result.host)) return;
  const std::lock_guard<std::mutex> lock(mutex_);
  if (std::find(saved_ipv4_hosts_.begin(), saved_ipv4_hosts_.end(), result.host) !=
      saved_ipv4_hosts_.end()) return;
  const auto existing = std::find_if(snapshot_.printers.begin(), snapshot_.printers.end(),
                                     [&result](const DiscoveredPrinter& value) {
    if (value.protocol != result.protocol) return false;
    if (!result.serial.empty() && !value.serial.empty()) return value.serial == result.serial;
    return value.host == result.host;
  });
  if (existing != snapshot_.printers.end()) {
    if (!result.name.empty()) existing->name = std::move(result.name);
    if (!result.model.empty()) existing->model = std::move(result.model);
    if (!result.serial.empty()) existing->serial = std::move(result.serial);
    if (existing->port == 0 || existing->port == 80) existing->port = result.port;
    return;
  }
  if (snapshot_.printers.size() >= kMaximumResults) return;
  if (result.name.empty()) {
    result.name = result.protocol == core::PrinterProtocol::bambu_lan
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
  const std::uint64_t started = now_ms();
  const std::uint64_t deadline = started + kMaximumDurationMs;
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
  for (std::size_t index = 0; index < 2; ++index) {
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
    destination.sin_addr.s_addr = multicast;
    for (const int socket_fd : ssdp_sockets) {
      if (socket_fd >= 0) sendto(socket_fd, kSearch, sizeof(kSearch) - 1, 0,
                                 reinterpret_cast<const sockaddr*>(&destination),
                                 sizeof(destination));
    }
  };
  const auto drain_ssdp = [&]() {
    for (const int socket_fd : ssdp_sockets) {
      if (socket_fd < 0) continue;
      while (true) {
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
               static_cast<unsigned>(snapshot().printers.size()));
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
  if ((host_bits & (host_bits + 1U)) != 0 || host_bits < 2 || host_bits > 255) {
    mask = 0xFFFFFF00U;
  }
  const std::uint32_t network = local & mask;
  const std::uint32_t broadcast = network | ~mask;
  struct Candidate { in_addr address{}; std::string host; };
  std::vector<Candidate> candidates;
  for (std::uint32_t address = network + 1; address < broadcast; ++address) {
    if (address == local) continue;
    in_addr candidate_address{.s_addr = htonl(address)};
    char text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &candidate_address, text, sizeof(text)) == nullptr) continue;
    const std::string host = text;
    if (std::find(saved_ipv4_hosts_.begin(), saved_ipv4_hosts_.end(), host) !=
        saved_ipv4_hosts_.end()) continue;
    candidates.push_back({candidate_address, host});
  }

  struct Pass {
    std::uint16_t port;
    core::PrinterProtocol protocol;
    bool verify_moonraker;
    std::uint32_t timeout_ms;
  };
  constexpr Pass kPasses[]{
      // A cold Wi-Fi ARP lookup can exceed a few hundred milliseconds. Give
      // Moonraker's primary port one full ARP window so the result does not
      // depend on an earlier failed scan having warmed the neighbor cache.
      {7125, core::PrinterProtocol::moonraker, false, 1000},
      {kBambuTlsPort, core::PrinterProtocol::bambu_lan, false,
       PrinterDiscoveryTiming::bambu_tcp_connect_timeout_ms},
      {4408, core::PrinterProtocol::moonraker, true, 1000},
      {4409, core::PrinterProtocol::moonraker, true, 1000},
      {80, core::PrinterProtocol::moonraker, true, 50},
  };
  struct Pending { int socket_fd; std::string host; std::uint16_t port;
                   core::PrinterProtocol protocol; bool verify_moonraker; };
  std::vector<Pending> pending;
  pending.reserve(kProbeBatchSize);
  const std::size_t total = std::max<std::size_t>(candidates.size() * std::size(kPasses), 1);
  std::size_t completed = 0;
  bool resource_pressure = false;

  const auto flush = [&](std::uint32_t timeout_ms) {
    if (pending.empty()) return;
    const std::uint64_t wait_deadline = std::min(deadline, now_ms() + timeout_ms);
    while (!pending.empty() && !cancel_requested_.load()) {
      fd_set writable;
      fd_set errors;
      FD_ZERO(&writable);
      FD_ZERO(&errors);
      int maximum_socket = -1;
      for (const auto& probe : pending) {
        FD_SET(probe.socket_fd, &writable);
        FD_SET(probe.socket_fd, &errors);
        maximum_socket = std::max(maximum_socket, probe.socket_fd);
      }
      const std::uint32_t remaining = PrinterDiscoveryTiming::bounded_wait_ms(
          now_ms(), wait_deadline, timeout_ms);
      if (remaining == 0) break;
      timeval timeout{.tv_sec = static_cast<time_t>(remaining / 1000),
                      .tv_usec = static_cast<suseconds_t>((remaining % 1000) * 1000)};
      const int ready = select(maximum_socket + 1, nullptr, &writable, &errors, &timeout);
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0) break;
      std::size_t waiting_count = 0;
      for (std::size_t index = 0; index < pending.size(); ++index) {
        auto& probe = pending[index];
        if (!FD_ISSET(probe.socket_fd, &writable) && !FD_ISSET(probe.socket_fd, &errors)) {
          if (waiting_count != index) pending[waiting_count] = std::move(probe);
          ++waiting_count;
          continue;
        }
        int socket_error = ECONNREFUSED;
        socklen_t error_size = sizeof(socket_error);
        bool accepted = getsockopt(probe.socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                                   &error_size) == 0 && socket_error == 0;
        if (accepted && probe.verify_moonraker) {
          accepted = moonraker_signature(probe.socket_fd, probe.host, deadline);
        } else if (accepted && probe.protocol == core::PrinterProtocol::bambu_lan) {
          close(probe.socket_fd);
          probe.socket_fd = -1;
          accepted = bambu_tls_identity(probe.host, deadline);
        }
        if (accepted) add_result({.protocol = probe.protocol,
                                  .name = {},
                                  .model = {},
                                  .host = probe.host,
                                  .serial = {},
                                  .port = probe.port});
        if (probe.socket_fd >= 0) close(probe.socket_fd);
      }
      pending.resize(waiting_count);
    }
    for (const auto& probe : pending) close(probe.socket_fd);
    pending.clear();
    service_ssdp();
  };

  for (const auto& pass : kPasses) {
    if (cancel_requested_.load() || now_ms() >= deadline) break;
    for (const auto& candidate : candidates) {
      if (cancel_requested_.load() || now_ms() >= deadline) break;
      service_ssdp();
      int socket_fd = -1;
      for (std::size_t attempt = 0;
           attempt < kSocketOpenAttempts && socket_fd < 0 && now_ms() < deadline;
           ++attempt) {
        socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_fd >= 0) break;
        // Web Config polling and the selected-printer connection also own a
        // few descriptors. Resolve our bounded batch, close it and retry so a
        // momentarily full table does not silently skip most of the subnet.
        flush(pass.timeout_ms);
        service_ssdp();
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if (socket_fd < 0) {
        resource_pressure = true;
      } else {
        fcntl(socket_fd, F_SETFL, O_NONBLOCK);
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(pass.port);
        target.sin_addr = candidate.address;
        const int result = connect(socket_fd, reinterpret_cast<const sockaddr*>(&target),
                                   sizeof(target));
        if (result == 0) {
          bool accepted = !pass.verify_moonraker;
          if (pass.verify_moonraker) {
            accepted = moonraker_signature(socket_fd, candidate.host, deadline);
          } else if (pass.protocol == core::PrinterProtocol::bambu_lan) {
            close(socket_fd);
            socket_fd = -1;
            accepted = bambu_tls_identity(candidate.host, deadline);
          }
          if (accepted) add_result({.protocol = pass.protocol,
                                    .name = {},
                                    .model = {},
                                    .host = candidate.host,
                                    .serial = {},
                                    .port = pass.port});
          if (socket_fd >= 0) close(socket_fd);
        } else if (errno == EINPROGRESS) {
          pending.push_back({socket_fd, candidate.host, pass.port, pass.protocol,
                             pass.verify_moonraker});
          const std::size_t batch_size =
              ssdp_active() ? kProbeBatchSizeWithSsdp : kProbeBatchSize;
          if (pending.size() >= batch_size) flush(pass.timeout_ms);
        } else {
          close(socket_fd);
        }
      }
      publish_progress(++completed, total);
    }
    flush(pass.timeout_ms);
  }

  while (!cancel_requested_.load() && now_ms() < deadline &&
         (now_ms() < started + kMinimumDurationMs || ssdp_active())) {
    service_ssdp();
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  service_ssdp();
  close_ssdp();

  bool timed_out = now_ms() >= deadline;
  bool cancelled = cancel_requested_.load();
  bool deduplication_complete = true;
  std::vector<DiscoveredPrinter> deduplicated;
  std::vector<std::vector<std::string>> moonraker_identities;
  if (!cancelled) {
    for (auto& printer : snapshot().printers) {
      if (cancel_requested_.load()) {
        cancelled = true;
        break;
      }
      if (now_ms() >= deadline) {
        timed_out = true;
        deduplication_complete = false;
        break;
      }
      if (printer.protocol != core::PrinterProtocol::moonraker) {
        deduplicated.push_back(std::move(printer));
        continue;
      }
      std::vector<std::string> identity =
          moonraker_interface_addresses(printer.host, printer.port, deadline);
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
    if (!cancelled && deduplication_complete) snapshot_.printers = std::move(deduplicated);
    std::sort(snapshot_.printers.begin(), snapshot_.printers.end(),
              [](const DiscoveredPrinter& left, const DiscoveredPrinter& right) {
      if (left.protocol != right.protocol) return left.protocol == core::PrinterProtocol::bambu_lan;
      return left.name < right.name;
    });
    if (cancelled) {
      snapshot_.state = PrinterDiscoveryState::idle;
      snapshot_.detail = "Network search stopped.";
    } else if (timed_out && snapshot_.printers.empty()) {
      snapshot_.state = PrinterDiscoveryState::failed;
      snapshot_.detail = "The 3-minute safety limit was reached before a supported printer responded.";
    } else if (timed_out) {
      snapshot_.state = PrinterDiscoveryState::complete;
      snapshot_.progress_percent = 100;
      snapshot_.detail = "Network search complete. Results found before the safety limit are shown.";
    } else if (resource_pressure && snapshot_.printers.empty()) {
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
      snapshot_.detail = snapshot_.printers.empty()
                             ? "No supported printers were found. You can still add one manually."
                             : "Network search complete.";
    }
    task_ = nullptr;
  }
  ESP_LOGI(kLogTag,
           "Network search finished: candidates=%u results=%u internal=%u, "
           "largest-internal=%u, largest-dma=%u",
           static_cast<unsigned>(candidates.size()),
           static_cast<unsigned>(snapshot().printers.size()),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(
               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)));
}

}  // namespace printdeck::platform
