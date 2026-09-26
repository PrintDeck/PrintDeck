#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/platform/bambu_local_connection.hpp"

namespace printdeck::platform {

struct BambuA1PreviewSnapshot {
  bool configured = false;
  bool fetching = false;
  std::string detail = "Print preview idle";
  std::string job_key;
  std::string model_title;
  std::string profile_title;
  std::shared_ptr<std::vector<uint8_t>> image;
};

// Fetches the current A1 job cover directly from the printer's local TLS file
// service. The worker requests the thumbnail and bounded title metadata; it never
// downloads the complete .gcode.3mf archive and never contacts Bambu Cloud.
class BambuA1PreviewClient {
 public:
  void configure(BambuLocalConnection connection);
  void set_network_ready(bool ready);
  void set_job(std::string file_hint, std::string job_name, std::string plate_hint,
               bool active, std::string source_job_id = {});
  static std::string job_key(const std::string& file_hint, const std::string& job_name,
                             const std::string& plate_hint, const std::string& source_job_id);
  esp_err_t start();
  void stop();
  void set_preview_requested(bool requested) {
    if (preview_requested_.exchange(requested) != requested && requested)
      fetch_requested_.store(true);
  }
  void set_metadata_requested(bool requested) {
    if (metadata_requested_.exchange(requested) != requested && requested)
      fetch_requested_.store(true);
  }
  bool running() const { return running_.load(std::memory_order_acquire); }
  BambuA1PreviewSnapshot snapshot() const;

 private:
  struct JobRequest {
    std::string file_hint;
    std::string job_name;
    std::string plate_hint;
    std::string key;
    bool active = false;
  };

  static void task_entry(void* context);
  void task_loop();
  BambuLocalConnection connection() const;
  JobRequest job_request() const;
  void publish_status(bool configured, bool fetching, const std::string& detail,
                      bool clear_image = false);
  void publish_image(const std::string& job_key,
                     std::shared_ptr<std::vector<uint8_t>> image,
                     std::string model_title, std::string profile_title);
  bool fetch(const BambuLocalConnection& connection, const JobRequest& job,
             std::shared_ptr<std::vector<uint8_t>>* image,
             std::string* model_title, std::string* profile_title);

  mutable std::mutex config_mutex_{};
  BambuLocalConnection connection_{};
  mutable std::mutex job_mutex_{};
  JobRequest job_{};
  mutable std::mutex snapshot_mutex_{};
  BambuA1PreviewSnapshot snapshot_{};
  TaskHandle_t task_handle_ = nullptr;
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> fetch_requested_{false};
  std::atomic<bool> preview_requested_{false};
  std::atomic<bool> metadata_requested_{false};
  std::atomic<bool> reconfigure_requested_{false};
  std::atomic<std::uint32_t> connection_generation_{0};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  mutable std::mutex task_mutex_{};
};

}  // namespace printdeck::platform
