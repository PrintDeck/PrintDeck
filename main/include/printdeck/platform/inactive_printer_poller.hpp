#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/device_state.hpp"
#include "printdeck/core/settings.hpp"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {

struct InactivePrinterStatus {
  std::uint32_t profile_id = 0;
  bool available = false;
  bool connected = false;
  bool checking = false;
  core::JobPhase phase = core::JobPhase::unknown;
  core::JobKind kind = core::JobKind::print;
  std::string job_name;
  std::uint32_t remaining_seconds = 0;
};

struct InactivePrinterSnapshot {
  std::vector<InactivePrinterStatus> printers;
  std::uint32_t revision = 0;
};

// Performs one bounded local status probe for each profile that does not
// currently own a full live connection.
// Moonraker uses HTTP; Bambu uses a short authenticated LAN MQTT/TLS session
// that is closed before the next profile is checked.
class InactivePrinterPoller {
 public:
  static constexpr std::uint64_t kMinimumCheckSpacingMs = 10000;

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
