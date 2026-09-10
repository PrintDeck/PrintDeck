#pragma once

#include <atomic>
#include <functional>
#include <mutex>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "printdeck/core/resin_controls.hpp"
#include "printdeck/platform/elegoo_sdcp_parser.hpp"
#include "printdeck/platform/network_service.hpp"
#include "printdeck/platform/uniformation_preview_service.hpp"

namespace printdeck::platform {

ElegooPollResult elegoo_sdcp_probe(const core::PrinterProfile& profile,
    std::uint64_t deadline, const std::function<bool()>& cancelled,
    ElegooIdentity* identity = nullptr);

class ElegooSdcpAdapter {
 public:
  esp_err_t start(const core::PrinterProfile* profile, const NetworkService& network);
  void configure(const core::PrinterProfile* profile);
  void stop();
  void set_preview_requested(bool requested, bool exposure_visible = false) {
    preview_requested_ = requested; exposure_visible_ = exposure_visible;
    if (!requested && !exposure_visible) previews_.clear();
  }
  bool request_control(const core::ResinControlRequest& request);
  bool running() const { return running_.load(); }
  core::PrinterSnapshot snapshot() const { return snapshots_.read(); }
  void snapshot_into(core::PrinterSnapshot& value) const { snapshots_.read_into(value); }
 private:
  static void task_entry(void* context);
  void run();
  mutable std::mutex mutex_;
  core::PrinterProfile profile_;
  core::SnapshotStore snapshots_;
  const NetworkService* network_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> preview_requested_{false};
  std::atomic<bool> exposure_visible_{false};
  UniformationPreviewService previews_;
  std::atomic<std::uint32_t> generation_{0};
  struct QueuedControl {
    core::ResinControlRequest request;
    std::uint64_t queued_at_ms = 0;
    std::uint32_t generation = 0;
  };
  QueueHandle_t controls_ = nullptr;
  std::atomic<bool> controls_live_{false};
};

}  // namespace printdeck::platform
