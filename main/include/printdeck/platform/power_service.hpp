#pragma once

#include <cstdint>

#include "esp_err.h"

namespace printdeck::platform {

struct PowerSnapshot {
  bool available = false;
  bool battery_present = false;
  std::uint8_t battery_percent = 0;
  bool charging = false;
  bool usb_present = false;
  float temperature_c = 0.0F;
};

enum class PowerButtonAction : std::uint8_t {
  none,
  wake,
  home,
  show_3,
  show_2,
  show_1,
  cancel,
  shutdown,
};

class PowerService {
 public:
  esp_err_t start();
  PowerSnapshot sample() const;
  PowerButtonAction poll_button();
  esp_err_t power_off();
  bool ready() const { return ready_; }

 private:
  bool ready_ = false;
  bool button_pressed_ = false;
  std::uint64_t button_pressed_at_ms_ = 0;
  bool release_pending_ = false;
  bool transition_candidate_valid_ = false;
  bool transition_candidate_pressed_ = false;
  std::uint8_t transition_candidate_samples_ = 0;
  std::uint64_t transition_candidate_at_ms_ = 0;
  std::uint64_t release_candidate_at_ms_ = 0;
  std::uint32_t button_read_error_count_ = 0;
  std::uint64_t last_button_read_error_log_ms_ = 0;
  std::uint8_t countdown_stage_ = 0;
};

}  // namespace printdeck::platform
