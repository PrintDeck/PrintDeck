#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/companion_camera.hpp"
#include "printdeck/core/camera_frame.hpp"
#include "printdeck/platform/network_service.hpp"
namespace printdeck::platform {
struct CompanionCameraStatus {
  bool scanning=false;
  int progress=0;
  std::string message;
  std::vector<core::CompanionCamera> cameras;
  bool refreshing=false;
  core::CameraFrame frame;
  std::string frame_camera_id;
  std::uint16_t width=0,height=0;
};
class CompanionCameraService {
 public:
  esp_err_t start();
  void configure(const std::vector<core::CompanionCamera>& known);
  void search(const NetworkStatus& network, bool web = false);
  void maintain_search(bool device_page);
  void keep_web_search_alive();
  void forget(const std::string& id);
  void cancel();
  void view(const core::CompanionCamera* camera, int fps);
  bool receiving() const { return receiving_.load(); }
  CompanionCameraStatus snapshot() const;
  void message(const char* text);
 private:
  static void entry(void* context);
  void run();
  void scan();
  bool identify(const std::string& host, core::CompanionCamera& camera);
  void capture(const core::CompanionCamera& camera, std::uint32_t generation);
  void add(core::CompanionCamera camera);
  mutable std::mutex mutex_;
  CompanionCameraStatus status_;
  NetworkStatus network_;
  core::CompanionCamera viewed_;
  int fps_=1;
  std::int64_t view_started_ms_=0;
  bool first_frame_pending_=false;
  std::atomic<std::uint32_t> generation_{0};
  std::atomic<bool> scan_requested_{false}, cancel_requested_{false}, receiving_{false};
  std::atomic<std::int64_t> web_search_deadline_{0};
  TaskHandle_t task_=nullptr;
};
}
