#pragma once

#include <mutex>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/device_state.hpp"

namespace printdeck::platform {

enum class MoonrakerProbeState {
  idle,
  connecting,
  ready,
  authorization_required,
  unavailable,
};

struct MoonrakerProbeSnapshot {
  MoonrakerProbeState state = MoonrakerProbeState::idle;
  int progress_percent = 0;
  std::string detail = "Ready to test";
  std::string version;
  std::string klipper_state;
  std::string manufacturer;
  std::string model;
  std::string brand;
  std::string evidence;
  bool running = false;
};

class MoonrakerConnectionProbe {
 public:
  esp_err_t start(core::PrinterProfile profile);
  MoonrakerProbeSnapshot snapshot() const;
  // A deliberate connection check followed by Save may re-enrol this exact
  // endpoint. Failed, expired or different connection checks cannot rebind it.
  bool verified(const core::PrinterProfile& profile) const;

 private:
  static void task_entry(void* context);
  void run();
  void finish(MoonrakerProbeState state, std::string detail,
              std::string version = {}, std::string klipper_state = {},
              std::string manufacturer = {}, std::string model = {},
              std::string brand = {}, std::string evidence = {});

  mutable std::mutex mutex_;
  core::PrinterProfile pending_profile_;
  MoonrakerProbeSnapshot snapshot_;
  TaskHandle_t task_ = nullptr;
  std::uint64_t verified_at_ms_ = 0;
};

}  // namespace printdeck::platform
