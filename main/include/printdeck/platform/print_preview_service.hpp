#pragma once
#include <atomic>
#include <mutex>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/device_state.hpp"
#include "printdeck/platform/print_preview_cache.hpp"

namespace printdeck::platform {
class PrintPreviewService {
 public:
  PrintPreviewService() = default;
  explicit PrintPreviewService(PrintPreviewCache cache) : cache_(std::move(cache)) {}
  struct Snapshot {
    std::string key;
    std::shared_ptr<std::vector<std::uint8_t>> image;
    bool fetch_needed = false;
  };
  static std::string key(const core::PrinterProfile&, const core::JobState&);
  esp_err_t start();
  void update(const core::PrinterProfile*, const core::PrinterSnapshot&, bool visible);
  Snapshot snapshot() const;
 private:
  struct Request {
    std::string key, printer;
    core::JobPhase phase = core::JobPhase::unknown;
    bool online = false, visible = false;
    std::shared_ptr<std::vector<std::uint8_t>> image;
  };
  static void task_entry(void*);
  void run();
  mutable std::mutex mutex_;
  Request request_;
  Snapshot snapshot_;
  std::uint32_t revision_ = 0;
  TaskHandle_t task_ = nullptr;
  PrintPreviewCache cache_;
};
}  // namespace printdeck::platform
