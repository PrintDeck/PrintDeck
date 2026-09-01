#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/platform/bambu_local_connection.hpp"

namespace printdeck::platform {

struct BambuA1CameraSnapshot {
  bool configured = false;
  bool enabled = false;
  bool supported = false;
  bool connected = false;
  std::string detail = "Camera off";
  std::shared_ptr<std::vector<uint8_t>> frame;
  uint16_t width = 0;
  uint16_t height = 0;
};

// Local-only still-image client for the Bambu Lab A1 camera endpoint. The
// worker owns all socket and JPEG work; the application loop only reads the
// cached snapshot. A connection is held for one frame and then released so
// other local viewers are not blocked while this page is open.
class BambuA1CameraClient {
 public:
  void configure(BambuLocalConnection connection);
  void set_network_ready(bool ready);
  void set_enabled(bool enabled);
  void request_refresh();
  esp_err_t start();
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }
  BambuA1CameraSnapshot snapshot() const;

 private:
  static void task_entry(void* context);
  void task_loop();
  BambuLocalConnection connection() const;
  void publish_status(bool configured, bool enabled, bool connected, const char* detail,
                      bool clear_frame = false);
  void publish_frame(std::shared_ptr<std::vector<uint8_t>> frame, uint16_t width,
                     uint16_t height);
  bool capture(const BambuLocalConnection& connection);

  mutable std::mutex config_mutex_{};
  BambuLocalConnection connection_{};
  mutable std::mutex snapshot_mutex_{};
  BambuA1CameraSnapshot snapshot_{};
  TaskHandle_t task_handle_ = nullptr;
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> enabled_{false};
  std::atomic<bool> refresh_requested_{false};
  std::atomic<bool> reconfigure_requested_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  mutable std::mutex task_mutex_{};
};

}  // namespace printdeck::platform
