#pragma once
#include <mutex>
#include <string>
#include "esp_err.h"
#include "printdeck/platform/ctb_preview.hpp"
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
  void hide();
  struct VolumeResult {
    unsigned status = 503;
    std::uint32_t generation = 0;
    std::string cache_key;
    CtbHeader header;
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
  };
  // Cache/queue access only: never performs network I/O on the HTTP task.
  VolumeResult volume(std::uint32_t profile, std::string_view task, std::uint32_t generation,
      bool metadata, unsigned index, unsigned width, unsigned height);
  void update(const std::string& address, std::uint16_t port, const std::string& path,
      std::optional<std::uint64_t> begin, core::PrinterSnapshot& state, bool model_needed, bool visible);
 private:
  struct Request {
    std::string key, address, path, task;
    std::uint32_t profile = 0;
    std::uint16_t port = 3030, layer = 0, layers = 0;
    std::uint64_t status_at = 0;
    bool visible = false, model_needed = false, layer_needed = false, exposing = false;
  };
  static void task_entry(void*);
  void run();
  std::mutex mutex_;
  TaskHandle_t task_ = nullptr;
  Request request_;
  struct VolumeWork {
    bool pending = false, metadata = true, failed = false;
    unsigned index = 0, width = 0, height = 0;
    std::uint64_t until = 0;
    unsigned attempts = 0;
  } volume_work_;
  CtbHeader volume_header_;
  std::string volume_cache_key_;
  std::shared_ptr<std::vector<std::uint8_t>> volume_bytes_;
  std::uint32_t volume_generation_ = 1;
  std::uint64_t next_volume_ = 0;
  std::string result_key_;
  std::shared_ptr<std::vector<std::uint8_t>> model_, layer_;
  std::uint16_t layer_index_ = 0;
  std::uint64_t model_after_ = 0, layer_after_ = 0;
  std::shared_ptr<std::vector<std::uint8_t>> held_layer_;
  std::uint64_t hold_until_ = 0, held_after_ = 0;
  std::uint16_t held_index_ = 0;
};
}  // namespace printdeck::platform
