#pragma once

#include <cstdint>
#include <atomic>
#include "printdeck/core/printer_address.hpp"
#include "printdeck/platform/printer_discovery_service.hpp"
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/device_state.hpp"
#include "printdeck/core/settings.hpp"
#include "printdeck/platform/network_service.hpp"
#include "printdeck/platform/inactive_printer_status.hpp"

namespace printdeck::platform {

// Performs one bounded local status probe for each profile that does not
// currently own a full live connection.
// Moonraker uses HTTP; Bambu uses a short authenticated LAN MQTT/TLS session
// that is closed before the next profile is checked.
class InactivePrinterPoller {
 public:
  static constexpr std::uint64_t kMinimumCheckSpacingMs = 10000;

  using RecoveryCallback = bool (*)(void*, const core::PrinterProfile&,
                                     const core::PrinterProfile&, const NetworkStatus&);
  void set_recovery_callback(RecoveryCallback callback, void* context) {
    recovery_callback_ = callback; recovery_context_ = context;
  }
  void set_discovery_service(PrinterDiscoveryService& service) { discovery_ = &service; }
  void set_recovery_allowed(bool allowed) { recovery_allowed_.store(allowed); }
  void observe_active(std::uint32_t id, bool online) {
    online_profile_.store(online ? id : 0);
  }
  esp_err_t start(const core::DeviceSettings& settings,
                  const NetworkService& network);
  void configure(const core::DeviceSettings& settings);
  void set_active_profile(std::uint32_t profile_id);
  void mark_offline(std::uint32_t profile_id);
  InactivePrinterSnapshot snapshot() const;
  bool check_in_progress(std::uint32_t profile_id) const;

 private:
  struct CheckAttempt {
    std::uint32_t profile_id = 0;
    std::uint64_t started_at_ms = 0;
    std::uint8_t consecutive_failures = 0;
    bool in_progress = false;
    core::PrinterRecoverySchedule recovery;
    std::uint64_t learn_after_ms = 0;
    std::size_t recovery_offset = 0;

  };

  static void task_entry(void* context);
  void task_loop();
  InactivePrinterStatus probe(const core::PrinterProfile& profile) const;
  bool begin_automatic_check(std::uint32_t profile_id,
                             std::uint32_t generation,
                             std::uint64_t now_ms);
  void finish_automatic_check(std::uint32_t profile_id,
                              std::uint64_t started_at_ms);
  void publish_automatic_result(std::uint32_t generation,
                                InactivePrinterStatus result);

  void recover_or_learn(const core::PrinterProfile& profile, std::uint32_t generation,
                        bool connected, const NetworkStatus& network, bool active = false);
  RecoveryCallback recovery_callback_ = nullptr;
  void* recovery_context_ = nullptr;
  std::atomic<bool> recovery_allowed_{true};
  std::atomic<std::uint32_t> online_profile_{0};
  std::uint64_t next_discovery_ms_ = 0;
  std::uint64_t next_name_query_ms_ = 0;
  struct RecoveryRun {
    core::PrinterProfile profile;
    NetworkStatus network;
    std::uint32_t generation = 0;
    std::uint32_t scan_id = 0;
  };
  std::optional<RecoveryRun> recovery_run_;
  PrinterDiscoveryService* discovery_ = nullptr;
  void finish_recovery();
  void reconcile_manual_search(const std::vector<core::PrinterProfile>& profiles,
                               std::uint32_t generation, const NetworkStatus& network);
  std::uint32_t manual_scan_id_ = 0;
  std::vector<std::uint32_t> reconciled_profiles_;
  mutable std::atomic<std::uint32_t> probing_profile_{0};
  mutable std::mutex mutex_;
  std::vector<core::PrinterProfile> profiles_;
  std::uint32_t active_profile_ = 0;
  std::uint32_t interval_s_ = 60;
  std::uint32_t config_generation_ = 0;
  std::vector<CheckAttempt> check_attempts_;
  InactivePrinterSnapshot snapshot_;
  const NetworkService* network_ = nullptr;
  TaskHandle_t task_ = nullptr;
};

}  // namespace printdeck::platform
