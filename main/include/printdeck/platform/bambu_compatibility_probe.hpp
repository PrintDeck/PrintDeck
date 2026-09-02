#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "esp_err.h"
#include "mqtt_client.h"
#include "printdeck/platform/bambu_local_connection.hpp"

namespace printdeck::platform {

enum class BambuCompatibilityState : uint8_t {
  kIdle,
  kConnecting,
  kCollecting,
  kProbingServices,
  kComplete,
  kFailed,
  kCancelled,
};

struct BambuCompatibilitySnapshot {
  BambuCompatibilityState state = BambuCompatibilityState::kIdle;
  int progress_percent = 0;
  std::string detail;
  std::string model;
  size_t mqtt_messages = 0;
  bool active_print_observed = false;
  bool report_ready = false;
};

struct BambuNumericFieldObservation {
  double minimum = 0.0;
  double maximum = 0.0;
  size_t samples = 0;
};

// Runs an explicitly requested, read-only LAN probe against one BambuLab
// printer. Credentials exist only for the lifetime of the worker; the report
// contains an allow-listed identity summary plus JSON field names/types, never
// raw MQTT values or connection details.
class BambuCompatibilityProbe {
 public:
  struct ModuleIdentity {
    std::string name;
    std::string product_name;
    std::string project_name;
    std::string hardware_version;
    std::string software_version;
  };

  esp_err_t start(BambuLocalConnection connection);
  void cancel();
  BambuCompatibilitySnapshot snapshot() const;
  std::string report_json() const;

 private:
  static void task_entry(void* context);
  static void mqtt_event_handler(void* context, esp_event_base_t base,
                                 int32_t event_id, void* event_data);

  void task_loop(BambuLocalConnection connection);
  void handle_mqtt_event(esp_mqtt_event_handle_t event);
  void consume_report(const char* payload, size_t length);
  void set_status(BambuCompatibilityState state, int progress,
                  const std::string& detail);
  bool probe_tls_service(const BambuLocalConnection& connection, uint16_t port) const;
  void finish_report(const BambuLocalConnection& connection, bool mqtt_available,
                     bool service_6000, bool service_322, bool service_990,
                     const std::string& terminal_detail);
  void stop_mqtt();

  mutable std::mutex mutex_{};
  BambuCompatibilitySnapshot snapshot_{};
  BambuLocalConnection pending_connection_{};
  std::string report_json_{};
  std::set<std::string> schema_fields_{};
  std::map<std::string, BambuNumericFieldObservation> numeric_fields_{};
  std::vector<ModuleIdentity> modules_{};
  std::string detected_model_{};
  size_t maximum_payload_bytes_ = 0;
  size_t maximum_live_extruders_ = 0;
  size_t maximum_ams_units_ = 0;
  size_t maximum_virtual_slots_ = 0;
  bool restricted_commands_observed_ = false;
  esp_mqtt_client_handle_t mqtt_client_ = nullptr;
  std::string report_topic_{};
  std::string request_topic_{};
  std::string mqtt_client_id_{};
  std::string incoming_topic_{};
  std::vector<char> incoming_payload_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_requested_{false};
  std::atomic<bool> mqtt_connected_{false};
  std::atomic<bool> mqtt_connected_observed_{false};
  std::atomic<bool> first_report_received_{false};
};

const char* to_string(BambuCompatibilityState state);

}  // namespace printdeck::platform
