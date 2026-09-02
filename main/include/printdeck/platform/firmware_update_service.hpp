#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/platform/network_service.hpp"

namespace printdeck::platform {

enum class FirmwareUpdateState {
  idle,
  checking,
  current,
  unavailable,
  available,
  failed,
  downloading,
  rebooting,
};

struct FirmwareUpdateSnapshot {
  FirmwareUpdateState state = FirmwareUpdateState::idle;
  std::string current_version = PRINTDECK_VERSION;
  std::string latest_version;
  std::string detail = "Ready to check for updates.";
  int progress_percent = 0;
  bool update_available = false;
  bool busy = false;
};

class FirmwareUpdateService {
 public:
  using BackgroundActivityProbe = bool (*)(void* context);
  using RestartRequestedCallback = bool (*)(void* context);

  esp_err_t start(const NetworkService& network);
  void set_background_activity_probe(BackgroundActivityProbe probe, void* context);
  void set_restart_requested_callback(RestartRequestedCallback callback, void* context);
  bool request_check();
  bool request_install();
  bool request_url_install(std::string url);
  bool begin_manual_install();
  void update_manual_progress(int percent);
  void fail_manual_install(std::string detail);
  void finish_manual_install();
  FirmwareUpdateSnapshot snapshot() const;

 private:
  enum class WorkerOperation : std::uint8_t { none, check, install };

  static void scheduler_entry(void* context);
  static void task_entry(void* context);
  bool request_check(bool manual_request);
  void scheduler_loop();
  void poll();
  void task_loop();
  void check_release();
  void install_release();
  void record_successful_check();
  void fail(std::string detail);
  mutable std::mutex mutex_;
  const NetworkService* network_ = nullptr;
  FirmwareUpdateSnapshot snapshot_;
  std::string firmware_url_;
  std::optional<std::array<std::uint8_t, 32>> firmware_sha256_;
  std::atomic<bool> check_requested_{false};
  std::atomic<bool> install_requested_{false};
  std::atomic<bool> install_running_{false};
  std::atomic<bool> worker_finished_{false};
  std::atomic<WorkerOperation> worker_operation_{WorkerOperation::none};
  std::atomic<std::uint64_t> last_check_started_ms_{0};
  BackgroundActivityProbe background_activity_probe_ = nullptr;
  void* background_activity_context_ = nullptr;
  RestartRequestedCallback restart_requested_callback_ = nullptr;
  void* restart_requested_context_ = nullptr;
  std::atomic<bool> automatic_check_requested_{false};
  TaskHandle_t scheduler_task_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::uint64_t next_automatic_check_ms_ = 0;
  std::atomic<std::uint64_t> last_check_epoch_{0};
};

}  // namespace printdeck::platform
