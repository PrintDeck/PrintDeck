#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace printdeck::platform {

class DisplayShell;

class OrientationService {
 public:
  using RotationFeedback = void (*)(void* context, int degrees);
  esp_err_t start(DisplayShell& display, const std::string& mode,
                  int initial_auto_rotation,
                  RotationFeedback feedback = nullptr, void* feedback_context = nullptr);
  esp_err_t configure(const std::string& mode);

 private:
  static void task_entry(void* context);
  void task_loop();
  esp_err_t start_auto_tracking();

  DisplayShell* display_ = nullptr;
  void* sensor_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<int> mode_{0};
  std::atomic<int> applied_{0};
  std::mutex mode_mutex_;
  RotationFeedback feedback_ = nullptr;
  void* feedback_context_ = nullptr;
};

}  // namespace printdeck::platform
