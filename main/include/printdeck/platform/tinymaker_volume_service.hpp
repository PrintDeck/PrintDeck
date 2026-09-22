#pragma once

#include <mutex>
#include "printdeck/core/device_state.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace printdeck::platform {
// Fetches only the RAM-backed live stack. SD model/image endpoints must never
// be used while TinyMaker is printing. Geometry stays in the browser.
class TinyMakerVolumeService {
 public:
  struct Result {
    unsigned status = 503;
    std::string reason = "waiting", key;
    std::uint32_t generation = 0;
    unsigned layers = 0, slots = 0, captured = 0;
    float layer_mm = 0;
    std::shared_ptr<std::string> bytes;
  };
  void update(const core::PrinterProfile*, const core::PrinterSnapshot&, bool suspended);
  Result request(std::uint32_t profile, bool metadata, std::uint32_t generation, unsigned since);
 private:
  static void entry(void*);
  void run();
  std::mutex mutex_;
  TaskHandle_t task_ = nullptr;
  std::string origin_, identity_;
  std::uint32_t profile_ = 0, last_elapsed_ = 0, last_layer_ = 0, last_uptime_ = 0;
  std::uint64_t status_at_ = 0, lease_ = 0, next_ = 0;
  bool online_ = false, supported_ = false, suspended_ = false, pending_ = false;
  unsigned since_ = 0, attempts_ = 0;
  Result source_, result_;
};
}  // namespace printdeck::platform
