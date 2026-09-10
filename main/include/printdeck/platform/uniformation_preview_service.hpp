#pragma once
#include <mutex>
#include <string>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/device_state.hpp"

namespace printdeck::platform {
// One bounded HTTP decoder for the selected resin printer. The SDCP worker
// continues receiving phase changes while this worker reads a single image.
class UniformationPreviewService {
 public:
  esp_err_t start();
  void clear();
  void update(const std::string& address, std::uint16_t port, const std::string& path,
      std::optional<std::uint64_t> begin, core::PrinterSnapshot& state, bool model_needed, bool visible);
 private:
  struct Request {
    std::string key, address, path, task;
    std::uint16_t port = 3030, layer = 0, layers = 0;
    std::uint64_t status_at = 0;
    bool visible = false, model_needed = false, layer_needed = false, exposing = false;
  };
  static void task_entry(void*);
  void run();
  std::mutex mutex_;
  TaskHandle_t task_ = nullptr;
  Request request_;
  std::string result_key_;
  std::shared_ptr<std::vector<std::uint8_t>> model_, layer_;
  std::uint16_t layer_index_ = 0;
  std::uint64_t model_after_ = 0, layer_after_ = 0;
  std::shared_ptr<std::vector<std::uint8_t>> held_layer_;
  std::uint64_t hold_until_ = 0, held_after_ = 0;
  std::uint16_t held_index_ = 0;
};
}  // namespace printdeck::platform
