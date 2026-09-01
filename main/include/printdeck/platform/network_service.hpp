#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_event_base.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "printdeck/core/settings.hpp"

namespace printdeck::platform {

struct NetworkStatus {
  bool station_connecting = false;
  bool station_connected = false;
  bool station_connection_failed = false;
  bool recovery_ap_active = false;
  std::string station_name;
  std::string ipv4;
  std::string netmask;
  std::string local_hostname;
  std::string setup_network_name;
};

class NetworkService {
 public:
  esp_err_t start(const core::DeviceSettings& settings);
  NetworkStatus status() const;
  std::vector<std::string> scan_visible_networks();
  esp_err_t test_station_connection(const std::string& network_name,
                                    const std::string& password);
  void accept_tested_station(const std::string& network_name,
                             const std::string& password);
  void cancel_tested_station();

 private:
  static void event_entry(void* context, esp_event_base_t base, std::int32_t id,
                          void* event_data);
  static void recovery_timer_entry(void* context);
  void handle_event(esp_event_base_t base, std::int32_t id, void* event_data);
  esp_err_t enable_setup_access_point();
  void arm_recovery_timer(std::int64_t delay_us);
  void ensure_station_timeout();
  esp_err_t restore_saved_station();
  esp_err_t start_mdns();
  esp_err_t enable_captive_portal();
  void disable_captive_portal();
  esp_err_t start_captive_dns(std::uint32_t access_point_ipv4);
  static void captive_dns_entry(void* context);
  void run_captive_dns();

  mutable std::mutex mutex_;
  std::mutex scan_mutex_;
  std::mutex station_test_mutex_;
  NetworkStatus status_;
  esp_timer_handle_t recovery_timer_ = nullptr;
  EventGroupHandle_t station_test_events_ = nullptr;
  std::string saved_station_name_;
  std::string saved_station_password_;
  std::string setup_network_name_;
  std::string captive_portal_uri_;
  esp_netif_t* access_point_netif_ = nullptr;
  std::uint8_t setup_client_count_ = 0;
  bool station_test_active_ = false;
  bool station_test_retry_enabled_ = false;
  std::atomic<bool> mdns_started_{false};
  std::atomic<bool> captive_dns_running_{false};
  std::atomic<bool> captive_dns_stop_requested_{false};
  std::atomic<int> captive_dns_socket_{-1};
  std::atomic<std::uint32_t> captive_dns_ipv4_{0};
  bool started_ = false;
};

}  // namespace printdeck::platform
