#include "printdeck/platform/network_service.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "sdkconfig.h"

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "network";
constexpr char kSetupNetworkPrefix[] = "PrintDeck";
constexpr char kMdnsHostname[] = "printdeck";
constexpr char kMdnsInstanceName[] = "PrintDeck";
constexpr char kMdnsApiService[] = "_printdeck";
static_assert(CONFIG_MDNS_MAX_SERVICES >= 2,
              "PrintDeck requires separate HTTP and Unified API mDNS services");
constexpr std::int64_t kStationConnectTimeoutUs = 20'000'000;
constexpr std::int64_t kFailureMessageDurationUs = 4'000'000;
constexpr std::int64_t kRecoveryRetryIntervalUs = 30'000'000;
constexpr EventBits_t kStationTestConnectedBit = BIT0;
constexpr std::uint16_t kDnsPort = 53;
constexpr std::size_t kDnsHeaderSize = 12;
constexpr std::size_t kDnsMaximumPacketSize = 512;
constexpr std::uint32_t kDnsAnswerTtlSeconds = 60;
constexpr int kServiceCore = 0;

std::uint16_t read_network_u16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                    static_cast<std::uint16_t>(bytes[1]));
}

void write_network_u16(std::uint8_t* bytes, std::uint16_t value) {
  bytes[0] = static_cast<std::uint8_t>(value >> 8U);
  bytes[1] = static_cast<std::uint8_t>(value & 0xFFU);
}

void write_network_u32(std::uint8_t* bytes, std::uint32_t value) {
  bytes[0] = static_cast<std::uint8_t>(value >> 24U);
  bytes[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  bytes[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  bytes[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

std::size_t captive_dns_response(const std::uint8_t* request, std::size_t request_size,
                                 std::uint32_t access_point_ipv4,
                                 std::uint8_t* response, std::size_t response_capacity) {
  if (request == nullptr || response == nullptr || request_size < kDnsHeaderSize ||
      request_size > response_capacity || read_network_u16(request + 4) == 0) {
    return 0;
  }
  const std::uint16_t request_flags = read_network_u16(request + 2);
  if ((request_flags & 0x8000U) != 0 || (request_flags & 0x7800U) != 0) return 0;

  std::size_t offset = kDnsHeaderSize;
  bool name_complete = false;
  while (offset < request_size) {
    const std::uint8_t label_size = request[offset++];
    if (label_size == 0) {
      name_complete = true;
      break;
    }
    if ((label_size & 0xC0U) != 0 || label_size > 63 ||
        offset + label_size > request_size) {
      return 0;
    }
    offset += label_size;
  }
  if (!name_complete || offset + 4 > request_size) return 0;

  const std::uint16_t query_type = read_network_u16(request + offset);
  const std::uint16_t query_class = read_network_u16(request + offset + 2);
  const std::size_t question_end = offset + 4;
  const bool answer_ipv4 = query_type == 1 && query_class == 1;
  const std::size_t answer_size = answer_ipv4 ? 16 : 0;
  if (question_end + answer_size > response_capacity) return 0;

  std::memcpy(response, request, question_end);
  write_network_u16(response + 2,
                    static_cast<std::uint16_t>(0x8080U | (request_flags & 0x0100U)));
  write_network_u16(response + 4, 1);
  write_network_u16(response + 6, answer_ipv4 ? 1 : 0);
  write_network_u16(response + 8, 0);
  write_network_u16(response + 10, 0);
  if (!answer_ipv4) return question_end;

  std::size_t answer_offset = question_end;
  write_network_u16(response + answer_offset, 0xC00CU);
  answer_offset += 2;
  write_network_u16(response + answer_offset, 1);
  answer_offset += 2;
  write_network_u16(response + answer_offset, 1);
  answer_offset += 2;
  write_network_u32(response + answer_offset, kDnsAnswerTtlSeconds);
  answer_offset += 4;
  write_network_u16(response + answer_offset, 4);
  answer_offset += 2;
  std::memcpy(response + answer_offset, &access_point_ipv4, sizeof(access_point_ipv4));
  return answer_offset + sizeof(access_point_ipv4);
}

template <std::size_t N>
void copy_wifi_text(std::uint8_t (&destination)[N], const std::string& source) {
  const std::size_t bytes = std::min(source.size(), N);
  std::memset(destination, 0, N);
  std::memcpy(destination, source.data(), bytes);
}

wifi_config_t station_config(const std::string& network_name, const std::string& password) {
  wifi_config_t station{};
  copy_wifi_text(station.sta.ssid, network_name);
  copy_wifi_text(station.sta.password, password);
  // An all-channel scan intentionally keeps BSSID unset. The ESP-IDF driver
  // can then rank every access point advertising this SSID and move to the
  // next candidate when one of them cannot complete the connection.
  station.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  station.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  station.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  station.sta.pmf_cfg.capable = true;
  station.sta.pmf_cfg.required = false;
  station.sta.rm_enabled = true;
  station.sta.btm_enabled = true;
  return station;
}

}  // namespace

esp_err_t NetworkService::start(const core::DeviceSettings& settings) {
  if (started_) return ESP_ERR_INVALID_STATE;

  esp_err_t result = esp_netif_init();
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
  result = esp_event_loop_create_default();
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;

  if (esp_netif_create_default_wifi_sta() == nullptr) return ESP_ERR_NO_MEM;
  access_point_netif_ = esp_netif_create_default_wifi_ap();
  if (access_point_netif_ == nullptr) return ESP_ERR_NO_MEM;
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  result = esp_wifi_init(&init);
  if (result != ESP_OK) return result;
  std::array<std::uint8_t, 6> access_point_mac{};
  result = esp_read_mac(access_point_mac.data(), ESP_MAC_WIFI_SOFTAP);
  if (result != ESP_OK) return result;
  std::array<std::uint8_t, 6> station_mac{};
  result = esp_read_mac(station_mac.data(), ESP_MAC_WIFI_STA);
  if (result != ESP_OK) return result;
  std::array<char, 23> device_id{};
  std::snprintf(device_id.data(), device_id.size(), "printdeck-%02x%02x%02x%02x%02x%02x",
                station_mac[0], station_mac[1], station_mac[2], station_mac[3],
                station_mac[4], station_mac[5]);
  device_id_ = device_id.data();
  std::array<char, 33> setup_network{};
  std::snprintf(setup_network.data(), setup_network.size(), "%s-%02X%02X%02X",
                kSetupNetworkPrefix, access_point_mac[3], access_point_mac[4],
                access_point_mac[5]);
  setup_network_name_ = setup_network.data();
  // PrintDeck owns persistence through SettingsStore. Candidate onboarding
  // credentials must stay volatile until a complete connection, including
  // DHCP, has been verified.
  result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (result != ESP_OK) return result;
  result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_entry, this);
  if (result != ESP_OK) return result;
  result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_entry, this);
  if (result != ESP_OK) return result;

  const esp_timer_create_args_t timer_args = {
      .callback = recovery_timer_entry,
      .arg = this,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_recovery",
      .skip_unhandled_events = true,
  };
  result = esp_timer_create(&timer_args, &recovery_timer_);
  if (result != ESP_OK) return result;
  station_test_events_ = xEventGroupCreate();
  if (station_test_events_ == nullptr) return ESP_ERR_NO_MEM;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    saved_station_name_ = settings.wifi_name;
    saved_station_password_ = settings.wifi_password;
    status_.station_name = settings.wifi_name;
    status_.station_connecting = !settings.wifi_name.empty();
    status_.setup_network_name = setup_network_name_;
    status_.device_id = device_id_;
  }

  const esp_err_t mdns_result = start_mdns();
  if (mdns_result != ESP_OK) {
    ESP_LOGW(kLogTag, "mDNS unavailable; Web Config remains reachable by IP: %s",
             esp_err_to_name(mdns_result));
  }

  if (settings.wifi_name.empty()) {
    result = enable_setup_access_point();
  } else {
    wifi_config_t station = station_config(settings.wifi_name, settings.wifi_password);
    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_STA, &station);
    if (result == ESP_OK) result = esp_wifi_start();
    if (result == ESP_OK) arm_recovery_timer(kStationConnectTimeoutUs);
  }
  if (result == ESP_OK) started_ = true;
  return result;
}

NetworkStatus NetworkService::status() const {
  NetworkStatus current;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    current = status_;
  }
  if (mdns_started_.load(std::memory_order_acquire)) {
    char hostname[MDNS_NAME_BUF_LEN]{};
    if (mdns_hostname_get(hostname) == ESP_OK && hostname[0] != '\0') {
      current.local_hostname = std::string(hostname) + ".local";
    }
  }
  return current;
}

esp_err_t NetworkService::start_mdns() {
  esp_err_t result = mdns_init();
  if (result != ESP_OK) return result;

  result = mdns_hostname_set(kMdnsHostname);
  if (result == ESP_OK) result = mdns_instance_name_set(kMdnsInstanceName);
  if (result == ESP_OK) {
    result = mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
  }
  if (result != ESP_OK) {
    mdns_free();
    return result;
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    status_.local_hostname = std::string(kMdnsHostname) + ".local";
  }
  mdns_started_.store(true, std::memory_order_release);

  mdns_txt_item_t api_txt[] = {
      {.key = "id", .value = device_id_.c_str()},
      {.key = "api", .value = "v1"},
      {.key = "path", .value = "/v1"},
      {.key = "auth", .value = "bearer"},
  };
  const esp_err_t api_result = mdns_service_add(
      kMdnsInstanceName, kMdnsApiService, "_tcp", 80, api_txt, std::size(api_txt));
  if (api_result != ESP_OK) {
    ESP_LOGW(kLogTag,
             "Home Assistant mDNS discovery unavailable; Web Config remains at "
             "http://%s.local/: %s",
             kMdnsHostname, esp_err_to_name(api_result));
    return ESP_OK;
  }

  ESP_LOGI(kLogTag, "Web Config and Unified API advertised at http://%s.local/",
           kMdnsHostname);
  return ESP_OK;
}

esp_err_t NetworkService::enable_captive_portal() {
  if (access_point_netif_ == nullptr) return ESP_ERR_INVALID_STATE;

  esp_netif_ip_info_t ip_info{};
  esp_err_t result = esp_netif_get_ip_info(access_point_netif_, &ip_info);
  if (result != ESP_OK) return result;

  std::array<char, 32> portal_uri{};
  std::snprintf(portal_uri.data(), portal_uri.size(), "http://" IPSTR "/",
                IP2STR(&ip_info.ip));

  const esp_err_t stop_result = esp_netif_dhcps_stop(access_point_netif_);
  if (stop_result != ESP_OK &&
      stop_result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
    return stop_result;
  }
  captive_portal_uri_ = portal_uri.data();
  const esp_err_t option_result = esp_netif_dhcps_option(
      access_point_netif_, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
      captive_portal_uri_.data(), captive_portal_uri_.size());
  const esp_err_t start_result = esp_netif_dhcps_start(access_point_netif_);
  if (option_result != ESP_OK) return option_result;
  if (start_result != ESP_OK &&
      start_result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
    return start_result;
  }

  result = start_captive_dns(ip_info.ip.addr);
  if (result == ESP_OK) {
    ESP_LOGI(kLogTag, "Captive Wi-Fi setup enabled");
  }
  return result;
}

void NetworkService::disable_captive_portal() {
  captive_dns_stop_requested_.store(true, std::memory_order_release);
  const int socket = captive_dns_socket_.load(std::memory_order_acquire);
  if (socket >= 0) shutdown(socket, SHUT_RDWR);
}

esp_err_t NetworkService::start_captive_dns(std::uint32_t access_point_ipv4) {
  captive_dns_ipv4_.store(access_point_ipv4, std::memory_order_release);
  bool expected = false;
  if (!captive_dns_running_.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return ESP_OK;
  }
  captive_dns_stop_requested_.store(false, std::memory_order_release);
  if (xTaskCreatePinnedToCore(captive_dns_entry, "captive_dns", 4096, this, 4,
                             nullptr, kServiceCore) != pdPASS) {
    captive_dns_running_.store(false, std::memory_order_release);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void NetworkService::captive_dns_entry(void* context) {
  auto* service = static_cast<NetworkService*>(context);
  if (service != nullptr) service->run_captive_dns();
  vTaskDelete(nullptr);
}

void NetworkService::run_captive_dns() {
  const int socket = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (socket < 0) {
    ESP_LOGW(kLogTag, "Captive DNS socket could not be created: errno %d", errno);
    captive_dns_running_.store(false, std::memory_order_release);
    return;
  }
  captive_dns_socket_.store(socket, std::memory_order_release);

  const int reuse_address = 1;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));
  const timeval receive_timeout = {.tv_sec = 0, .tv_usec = 250'000};
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(kDnsPort);
  address.sin_addr.s_addr = captive_dns_ipv4_.load(std::memory_order_acquire);
  if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    ESP_LOGW(kLogTag, "Captive DNS socket could not bind: errno %d", errno);
    close(socket);
    captive_dns_socket_.store(-1, std::memory_order_release);
    captive_dns_running_.store(false, std::memory_order_release);
    return;
  }

  std::array<std::uint8_t, kDnsMaximumPacketSize> request{};
  std::array<std::uint8_t, kDnsMaximumPacketSize> response{};
  while (!captive_dns_stop_requested_.load(std::memory_order_acquire)) {
    sockaddr_storage source{};
    socklen_t source_size = sizeof(source);
    const int received = recvfrom(socket, request.data(), request.size(), 0,
                                  reinterpret_cast<sockaddr*>(&source), &source_size);
    if (received < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      if (!captive_dns_stop_requested_.load(std::memory_order_acquire)) {
        ESP_LOGW(kLogTag, "Captive DNS receive failed: errno %d", errno);
      }
      break;
    }
    const std::size_t response_size = captive_dns_response(
        request.data(), static_cast<std::size_t>(received),
        captive_dns_ipv4_.load(std::memory_order_acquire), response.data(),
        response.size());
    if (response_size == 0) continue;
    sendto(socket, response.data(), response_size, 0,
           reinterpret_cast<const sockaddr*>(&source), source_size);
  }

  shutdown(socket, SHUT_RDWR);
  close(socket);
  captive_dns_socket_.store(-1, std::memory_order_release);
  captive_dns_running_.store(false, std::memory_order_release);
  ESP_LOGI(kLogTag, "Captive Wi-Fi setup disabled");
}

std::vector<std::string> NetworkService::scan_visible_networks() {
  const std::lock_guard<std::mutex> scan_lock(scan_mutex_);
  const NetworkStatus current = status();
  if (current.station_name.empty()) {
    const esp_err_t mode_result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (mode_result != ESP_OK) return {};
  }

  wifi_scan_config_t config{};
  config.show_hidden = false;
  config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  config.scan_time.active.min = 80;
  config.scan_time.active.max = 180;
  if (esp_wifi_scan_start(&config, true) != ESP_OK) return {};

  std::uint16_t count = 0;
  if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) return {};
  count = std::min<std::uint16_t>(count, 32);
  std::vector<wifi_ap_record_t> records(count);
  if (count > 0 && esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) return {};

  std::sort(records.begin(), records.begin() + count,
            [](const wifi_ap_record_t& left, const wifi_ap_record_t& right) {
              return left.rssi > right.rssi;
            });
  std::vector<std::string> names;
  names.reserve(count);
  for (std::uint16_t index = 0; index < count; ++index) {
    const char* text = reinterpret_cast<const char*>(records[index].ssid);
    std::string name(text, strnlen(text, sizeof(records[index].ssid)));
    if (name.empty() || std::find(names.begin(), names.end(), name) != names.end()) continue;
    names.push_back(std::move(name));
  }
  return names;
}

esp_err_t NetworkService::test_station_connection(const std::string& network_name,
                                                  const std::string& password) {
  const std::lock_guard<std::mutex> test_lock(station_test_mutex_);
  if (!started_ || station_test_events_ == nullptr || !status().recovery_ap_active) {
    return ESP_ERR_INVALID_STATE;
  }

  xEventGroupClearBits(station_test_events_, kStationTestConnectedBit);
  if (recovery_timer_ != nullptr) esp_timer_stop(recovery_timer_);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    station_test_active_ = true;
    station_test_retry_enabled_ = false;
    status_.station_name = network_name;
    status_.station_connecting = true;
    status_.station_connected = false;
    status_.station_connection_failed = false;
    status_.ipv4.clear();
    status_.netmask.clear();
  }

  esp_err_t result = esp_wifi_set_mode(WIFI_MODE_APSTA);
  wifi_config_t candidate = station_config(network_name, password);
  if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_STA, &candidate);
  if (result == ESP_OK) {
    const esp_err_t disconnect_result = esp_wifi_disconnect();
    if (disconnect_result != ESP_OK && disconnect_result != ESP_ERR_WIFI_NOT_CONNECT) {
      result = disconnect_result;
    }
  }
  if (result == ESP_OK) result = esp_wifi_connect();
  if (result == ESP_OK) {
    const std::lock_guard<std::mutex> lock(mutex_);
    station_test_retry_enabled_ = true;
  }
  if (result != ESP_OK) {
    cancel_tested_station();
    return result;
  }

  ESP_LOGI(kLogTag, "Testing Wi-Fi credentials while setup access point remains active");
  const EventBits_t bits = xEventGroupWaitBits(
      station_test_events_, kStationTestConnectedBit, pdTRUE, pdFALSE,
      pdMS_TO_TICKS(kStationConnectTimeoutUs / 1000));
  if ((bits & kStationTestConnectedBit) == 0) {
    ESP_LOGW(kLogTag, "Wi-Fi credential test timed out");
    cancel_tested_station();
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

void NetworkService::accept_tested_station(const std::string& network_name,
                                           const std::string& password) {
  const std::lock_guard<std::mutex> test_lock(station_test_mutex_);
  const std::lock_guard<std::mutex> lock(mutex_);
  saved_station_name_ = network_name;
  saved_station_password_ = password;
  status_.station_name = network_name;
  station_test_active_ = false;
  station_test_retry_enabled_ = false;
}

void NetworkService::cancel_tested_station() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    station_test_active_ = false;
    station_test_retry_enabled_ = false;
    status_.station_connected = false;
    status_.station_connecting = false;
    status_.station_connection_failed = false;
    status_.station_name = saved_station_name_;
    status_.ipv4.clear();
    status_.netmask.clear();
  }
  const esp_err_t result = restore_saved_station();
  if (result != ESP_OK) {
    ESP_LOGW(kLogTag, "Could not restore the saved Wi-Fi configuration: %s",
             esp_err_to_name(result));
  }
}

void NetworkService::event_entry(void* context, esp_event_base_t base, std::int32_t id,
                                 void* event_data) {
  static_cast<NetworkService*>(context)->handle_event(base, id, event_data);
}

void NetworkService::recovery_timer_entry(void* context) {
  auto* service = static_cast<NetworkService*>(context);
  bool setup_client_connected = false;
  {
    const std::lock_guard<std::mutex> lock(service->mutex_);
    if (service->station_test_active_) return;
    setup_client_connected = service->setup_client_count_ > 0;
  }
  if (setup_client_connected) {
    // Keep the setup page on a stable SoftAP channel while somebody is using
    // it. An explicit credential test may still use STA and channel switching.
    service->arm_recovery_timer(kRecoveryRetryIntervalUs);
    return;
  }
  NetworkStatus current = service->status();
  if (current.station_connected) return;

  if (!current.recovery_ap_active) {
    {
      const std::lock_guard<std::mutex> lock(service->mutex_);
      service->status_.station_connecting = false;
      service->status_.station_connection_failed = true;
    }
    const esp_err_t result = service->enable_setup_access_point();
    if (result != ESP_OK) {
      ESP_LOGE(kLogTag, "Recovery access point failed: %s", esp_err_to_name(result));
      return;
    }
    const esp_err_t disconnect_result = esp_wifi_disconnect();
    if (disconnect_result != ESP_OK && disconnect_result != ESP_ERR_WIFI_NOT_CONNECT) {
      ESP_LOGW(kLogTag, "Could not pause station retries: %s",
               esp_err_to_name(disconnect_result));
    }
    ESP_LOGW(kLogTag, "Saved Wi-Fi timed out; showing recovery message");
    service->arm_recovery_timer(kFailureMessageDurationUs);
    return;
  }

  if (current.station_connection_failed) {
    {
      const std::lock_guard<std::mutex> lock(service->mutex_);
      service->status_.station_connection_failed = false;
    }
    service->arm_recovery_timer(kRecoveryRetryIntervalUs);
    return;
  }

  const esp_err_t result = esp_wifi_connect();
  if (result != ESP_OK) {
    ESP_LOGW(kLogTag, "Background Wi-Fi retry could not start: %s",
             esp_err_to_name(result));
  } else {
    ESP_LOGI(kLogTag, "Background Wi-Fi retry started while setup remains available");
  }
  service->arm_recovery_timer(kRecoveryRetryIntervalUs);
}

void NetworkService::handle_event(esp_event_base_t base, std::int32_t id, void* event_data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
    bool pause_background_retry = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (setup_client_count_ < UINT8_MAX) ++setup_client_count_;
      pause_background_retry = status_.recovery_ap_active && !station_test_active_;
    }
    if (pause_background_retry) {
      const esp_err_t result = esp_wifi_disconnect();
      if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(kLogTag, "Could not pause background Wi-Fi retry: %s",
                 esp_err_to_name(result));
      }
      arm_recovery_timer(kRecoveryRetryIntervalUs);
    }
    ESP_LOGI(kLogTag, "Setup client connected");
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
    bool resume_background_retry = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (setup_client_count_ > 0) --setup_client_count_;
      resume_background_retry = setup_client_count_ == 0 && status_.recovery_ap_active &&
                                !station_test_active_;
    }
    if (resume_background_retry) arm_recovery_timer(kRecoveryRetryIntervalUs);
    ESP_LOGI(kLogTag, "Setup client disconnected");
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (status_.station_name.empty() || station_test_active_) return;
    }
    ESP_LOGI(kLogTag, "Connecting to saved Wi-Fi");
    esp_wifi_connect();
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(event_data);
    NetworkStatus current;
    bool station_test_active = false;
    bool station_test_retry_enabled = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      current = status_;
      station_test_active = station_test_active_;
      station_test_retry_enabled = station_test_retry_enabled_;
      status_.station_connected = false;
      status_.station_connecting = station_test_active || !current.recovery_ap_active;
      status_.ipv4.clear();
      status_.netmask.clear();
    }
    if (event != nullptr) {
      ESP_LOGW(kLogTag, "Wi-Fi disconnected (reason %u, RSSI %d)",
               static_cast<unsigned>(event->reason), static_cast<int>(event->rssi));
    }
    if (station_test_active && station_test_retry_enabled) {
      const esp_err_t retry_result = esp_wifi_connect();
      if (retry_result != ESP_OK) {
        ESP_LOGW(kLogTag, "Wi-Fi credential test retry could not start: %s",
                 esp_err_to_name(retry_result));
      }
    } else if (!current.recovery_ap_active) {
      esp_wifi_connect();
      ensure_station_timeout();
    }
    return;
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);
    char address[IP4ADDR_STRLEN_MAX]{};
    char netmask[IP4ADDR_STRLEN_MAX]{};
    esp_ip4addr_ntoa(&event->ip_info.ip, address, sizeof(address));
    esp_ip4addr_ntoa(&event->ip_info.netmask, netmask, sizeof(netmask));
    bool station_test_active = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      station_test_active = station_test_active_;
      status_.station_connected = true;
      status_.station_connecting = false;
      status_.station_connection_failed = false;
      status_.ipv4 = address;
      status_.netmask = netmask;
    }
    if (recovery_timer_ != nullptr) esp_timer_stop(recovery_timer_);
    if (station_test_active && station_test_events_ != nullptr) {
      xEventGroupSetBits(station_test_events_, kStationTestConnectedBit);
    } else if (status().recovery_ap_active) {
      const esp_err_t mode_result = esp_wifi_set_mode(WIFI_MODE_STA);
      if (mode_result == ESP_OK) {
        disable_captive_portal();
        const std::lock_guard<std::mutex> lock(mutex_);
        status_.recovery_ap_active = false;
      } else {
        ESP_LOGW(kLogTag, "Could not hide recovery setup network: %s",
                 esp_err_to_name(mode_result));
      }
    }
    ESP_LOGI(kLogTag, "Wi-Fi connected");
  }
}

esp_err_t NetworkService::enable_setup_access_point() {
  wifi_config_t access_point{};
  copy_wifi_text(access_point.ap.ssid, setup_network_name_);
  access_point.ap.ssid_len = setup_network_name_.size();
  access_point.ap.channel = 1;
  access_point.ap.authmode = WIFI_AUTH_OPEN;
  access_point.ap.max_connection = 4;
  access_point.ap.beacon_interval = 100;

  const NetworkStatus current = status();
  const wifi_mode_t mode = current.station_name.empty() ? WIFI_MODE_AP : WIFI_MODE_APSTA;
  esp_err_t result = esp_wifi_set_mode(mode);
  if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_AP, &access_point);
  if (result == ESP_OK && !started_) result = esp_wifi_start();
  if (result == ESP_OK) {
    const std::lock_guard<std::mutex> lock(mutex_);
    status_.recovery_ap_active = true;
    if (status_.station_name.empty()) status_.station_connecting = false;
    ESP_LOGI(kLogTag, "Setup access point enabled");
  }
  if (result == ESP_OK) {
    const esp_err_t captive_result = enable_captive_portal();
    if (captive_result != ESP_OK) {
      ESP_LOGW(kLogTag, "Captive setup could not start; QR and local address remain available: %s",
               esp_err_to_name(captive_result));
    }
  }
  return result;
}

void NetworkService::arm_recovery_timer(std::int64_t delay_us) {
  if (recovery_timer_ == nullptr) return;
  esp_timer_stop(recovery_timer_);
  esp_timer_start_once(recovery_timer_, delay_us);
}

void NetworkService::ensure_station_timeout() {
  if (recovery_timer_ == nullptr || esp_timer_is_active(recovery_timer_)) return;
  esp_timer_start_once(recovery_timer_, kStationConnectTimeoutUs);
}

esp_err_t NetworkService::restore_saved_station() {
  std::string network_name;
  std::string password;
  bool setup_client_connected = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    network_name = saved_station_name_;
    password = saved_station_password_;
    setup_client_connected = setup_client_count_ > 0;
  }

  const esp_err_t disconnect_result = esp_wifi_disconnect();
  if (disconnect_result != ESP_OK && disconnect_result != ESP_ERR_WIFI_NOT_CONNECT) {
    return disconnect_result;
  }
  if (network_name.empty()) return esp_wifi_set_mode(WIFI_MODE_AP);

  esp_err_t result = esp_wifi_set_mode(WIFI_MODE_APSTA);
  wifi_config_t station = station_config(network_name, password);
  if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_STA, &station);
  if (result == ESP_OK && !setup_client_connected) result = esp_wifi_connect();
  if (result == ESP_OK) arm_recovery_timer(kRecoveryRetryIntervalUs);
  return result;
}

}  // namespace printdeck::platform
