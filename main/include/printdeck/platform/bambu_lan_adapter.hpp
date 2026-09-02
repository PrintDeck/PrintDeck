#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "printdeck/core/device_state.hpp"
#include "printdeck/platform/bambu_model.hpp"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {

class BambuLanAdapter {
 public:
  esp_err_t start(const core::PrinterProfile* selected_profile,
                  const NetworkService& network);
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }
  void configure(const core::PrinterProfile* selected_profile);
  core::PrinterSnapshot snapshot() const;
  void snapshot_into(core::PrinterSnapshot& destination) const;
  BambuPrinterModel detected_model() const { return model_.load(); }
  BambuModelCapabilities capabilities() const;
  bool request_chamber_light(bool enabled);

 private:
  static void task_entry(void* context);
  static void mqtt_entry(void* context, esp_event_base_t base, std::int32_t event_id,
                         void* event_data);
  void task_loop();
  void handle_mqtt(esp_mqtt_event_handle_t event);
  void handle_report(const char* payload, std::size_t length);
  esp_err_t begin_client();
  void publish_state(core::LinkState link, const char* detail);
  bool publish(const char* payload);
  bool publish_command(const char* section, const char* command);
  bool publish_chamber_light(bool enabled);
  void reset_session_health();
  void maintain_session(std::uint64_t now_ms);

  core::PrinterProfile profile_;
  core::PrinterProfile pending_profile_;
  std::mutex profile_mutex_;
  core::SnapshotStore snapshots_;
  const NetworkService* network_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  mutable std::mutex task_mutex_;
  esp_mqtt_client_handle_t client_ = nullptr;
  std::atomic<bool> connected_{false};
  std::atomic<bool> status_ready_{false};
  std::atomic<bool> reconfigure_requested_{false};
  std::atomic<int> pending_chamber_light_{-1};
  std::atomic<std::uint64_t> chamber_light_deadline_ms_{0};
  std::string report_topic_;
  std::string request_topic_;
  std::string client_id_;
  std::atomic<BambuPrinterModel> model_{BambuPrinterModel::unknown};
  std::atomic<bool> restricted_commands_{false};
  std::atomic<std::uint32_t> sequence_id_{1};
  std::atomic<std::uint64_t> connected_at_ms_{0};
  std::atomic<std::uint64_t> last_report_ms_{0};
  std::atomic<std::uint64_t> last_status_report_ms_{0};
  std::atomic<std::uint64_t> last_full_request_ms_{0};
  std::atomic<std::uint64_t> recovery_request_ms_{0};
  std::atomic<std::uint32_t> oversized_reports_{0};
  std::mutex incoming_mutex_;
  std::string incoming_topic_;
  std::vector<char> incoming_payload_;
};

}  // namespace printdeck::platform
