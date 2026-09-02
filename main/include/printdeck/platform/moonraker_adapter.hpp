#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/device_state.hpp"
#include "printdeck/platform/moonraker_status_parser.hpp"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {

class MoonrakerAdapter {
 public:
  esp_err_t start(const core::PrinterProfile* selected_profile,
                  const NetworkService& network);
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }
  void configure(const core::PrinterProfile* selected_profile);
  core::PrinterSnapshot snapshot() const;
  void snapshot_into(core::PrinterSnapshot& destination) const;
  bool request_chamber_light(bool enabled);

 private:
  static void task_entry(void* context);
  void task_loop();
  core::PrinterProfile profile() const;
  void publish(core::LinkState link, const char* detail);
  bool discover_printer(const core::PrinterProfile& profile);
  bool poll(const core::PrinterProfile& profile);
  bool send_chamber_light(const core::PrinterProfile& profile, bool enabled);
  void refresh_job_metadata(const core::PrinterProfile& profile,
                            const std::string& filename);

  mutable std::mutex profile_mutex_;
  core::PrinterProfile profile_;
  core::SnapshotStore snapshots_;
  const NetworkService* network_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  mutable std::mutex task_mutex_;
  std::string cached_metadata_filename_;
  std::shared_ptr<std::vector<std::uint8_t>> cached_preview_;
  std::uint32_t cached_estimated_seconds_ = 0;
  std::uint16_t cached_total_layers_ = 0;
  std::uint32_t active_profile_id_ = 0;
  std::vector<std::string> tool_objects_;
  std::string chamber_sensor_object_;
  MoonrakerLightDescriptor chamber_light_;
  std::uint8_t chamber_light_channels_ = 0;
  std::atomic<int> pending_chamber_light_{-1};
  std::atomic<std::uint64_t> chamber_light_deadline_ms_{0};
  bool has_print_task_config_ = false;
};

}  // namespace printdeck::platform
