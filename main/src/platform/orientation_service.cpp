#include "printdeck/platform/orientation_service.hpp"
#include "printdeck/platform/task_affinity.hpp"

#include <cmath>

#include "esp_log.h"
#include "esp_timer.h"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/display_shell.hpp"
#include "qmi8658.h"

namespace printdeck::platform {
namespace {
constexpr char kLogTag[] = "orientation";
constexpr int kAutomaticMode = -1;
constexpr std::uint64_t kOrientationStableMs = 1000;
constexpr std::uint64_t kOrientationChangeCooldownMs = 2000;
constexpr float kMinimumPlanarGravity = 3.5F;
constexpr float kMinimumTotalGravity = 5.0F;
constexpr float kMaximumTotalGravity = 14.0F;
constexpr std::uint32_t kInvalidSamplesBeforeRecovery = 8;
constexpr std::uint64_t kRecoveryCooldownMs = 15000;
constexpr std::uint32_t kMaximumRecoveryAttempts = 3;

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time() / 1000ULL);
}

int rotation_mode(const std::string& mode) {
  if (mode == "auto") return kAutomaticMode;
  return mode == "90" ? 90 : mode == "180" ? 180 : mode == "270" ? 270 : 0;
}

esp_err_t restore_accelerometer(qmi8658_dev_t* sensor) {
  esp_err_t result = qmi8658_reset(sensor);
  if (result == ESP_OK) vTaskDelay(pdMS_TO_TICKS(30));
  if (result == ESP_OK) result = qmi8658_write_register(sensor, QMI8658_CTRL1, 0x60);
  if (result == ESP_OK) result = qmi8658_set_accel_range(sensor, QMI8658_ACCEL_RANGE_8G);
  // The board's QMI8658 revision produces stale/implausible register frames
  // at the 31.25 Hz setting while the shared bus is active. Generate at
  // 500 Hz and keep the application poll at 4 Hz, matching the proven board
  // configuration and ensuring every poll sees a fresh complete sample.
  if (result == ESP_OK) result = qmi8658_set_accel_odr(sensor, QMI8658_ACCEL_ODR_500HZ);
  if (result == ESP_OK) result = qmi8658_enable_sensors(sensor, QMI8658_ENABLE_ACCEL);
  qmi8658_set_accel_unit_mps2(sensor, true);
  return result;
}
}

esp_err_t OrientationService::start(DisplayShell& display, const std::string& mode,
                                    int initial_auto_rotation,
                                    RotationFeedback feedback, void* feedback_context) {
  if (task_ != nullptr || sensor_ != nullptr) return ESP_ERR_INVALID_STATE;
  display_ = &display;
  feedback_ = feedback;
  feedback_context_ = feedback_context;
  const int requested_mode = rotation_mode(mode);
  const int initial_rotation = requested_mode == kAutomaticMode
                                   ? (initial_auto_rotation == 90 ? 90
                                      : initial_auto_rotation == 180 ? 180
                                      : initial_auto_rotation == 270 ? 270 : 0)
                                   : requested_mode;
  {
    const std::lock_guard<std::mutex> lock(mode_mutex_);
    mode_.store(requested_mode, std::memory_order_release);
    applied_.store(initial_rotation, std::memory_order_release);
    display.set_rotation(initial_rotation);
  }
  return requested_mode == kAutomaticMode ? start_auto_tracking() : ESP_OK;
}

esp_err_t OrientationService::configure(const std::string& mode) {
  if (display_ == nullptr) return ESP_ERR_INVALID_STATE;
  const int requested_mode = rotation_mode(mode);
  if (requested_mode != kAutomaticMode) {
    const std::lock_guard<std::mutex> lock(mode_mutex_);
    mode_.store(requested_mode, std::memory_order_release);
    if (!display_->set_rotation(requested_mode)) return ESP_FAIL;
    applied_.store(requested_mode, std::memory_order_release);
    return ESP_OK;
  }

  mode_.store(kAutomaticMode, std::memory_order_release);
  return task_ == nullptr ? start_auto_tracking() : ESP_OK;
}

esp_err_t OrientationService::start_auto_tracking() {
  if (task_ != nullptr) return ESP_OK;
  if (sensor_ != nullptr) return ESP_ERR_INVALID_STATE;
  auto* sensor = new qmi8658_dev_t{};
  esp_err_t result = qmi8658_init(sensor, board_i2c_handle(), QMI8658_ADDRESS_HIGH);
  if (result == ESP_OK) result = qmi8658_set_accel_range(sensor, QMI8658_ACCEL_RANGE_8G);
  if (result == ESP_OK) result = qmi8658_set_accel_odr(sensor, QMI8658_ACCEL_ODR_500HZ);
  if (result != ESP_OK) {
    delete sensor;
    ESP_LOGW(kLogTag, "Motion sensor unavailable: %s", esp_err_to_name(result));
    return result;
  }
  qmi8658_set_accel_unit_mps2(sensor, true);
  sensor_ = sensor;
  if (xTaskCreatePinnedToCore(task_entry, "orientation", 3072, this, 3, &task_,
                              kServiceCore) != pdPASS) {
    delete sensor;
    sensor_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void OrientationService::task_entry(void* context) {
  static_cast<OrientationService*>(context)->task_loop();
}

void OrientationService::task_loop() {
  auto* sensor = static_cast<qmi8658_dev_t*>(sensor_);
  int candidate = applied_.load(std::memory_order_acquire);
  std::uint64_t candidate_since_ms = now_ms();
  std::uint64_t last_change_ms = 0;
  std::uint32_t invalid_samples = 0;
  std::uint32_t recovery_attempts = 0;
  std::uint64_t last_recovery_ms = 0;
  std::uint64_t last_read_error_log_ms = 0;
  while (true) {
    if (mode_.load(std::memory_order_acquire) != kAutomaticMode) {
      candidate = applied_.load(std::memory_order_acquire);
      candidate_since_ms = now_ms();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    float x = 0, y = 0, z = 0;
    const esp_err_t read_result = qmi8658_read_accel(sensor, &x, &y, &z);
    if (read_result == ESP_OK) {
      const float total = std::sqrt(x * x + y * y + z * z);
      float gx = 0;
      float gy = 0;
      board_auto_rotation_axes(x, y, z, &gx, &gy);
      const std::uint64_t current_ms = now_ms();
      if (total < kMinimumTotalGravity || total > kMaximumTotalGravity) {
        ++invalid_samples;
        const bool cooldown_elapsed =
            last_recovery_ms == 0 || current_ms - last_recovery_ms >= kRecoveryCooldownMs;
        if (invalid_samples >= kInvalidSamplesBeforeRecovery &&
            recovery_attempts < kMaximumRecoveryAttempts && cooldown_elapsed) {
          ++recovery_attempts;
          invalid_samples = 0;
          last_recovery_ms = current_ms;
          ESP_LOGW(kLogTag, "Implausible motion data; restoring QMI8658 (%u/%u)",
                   static_cast<unsigned>(recovery_attempts),
                   static_cast<unsigned>(kMaximumRecoveryAttempts));
          const esp_err_t recovery_result = restore_accelerometer(sensor);
          ESP_LOGI(kLogTag, "QMI8658 recovery result: %s",
                   esp_err_to_name(recovery_result));
        } else if (invalid_samples >= kInvalidSamplesBeforeRecovery &&
                   recovery_attempts >= kMaximumRecoveryAttempts) {
          ESP_LOGE(kLogTag,
                   "QMI8658 remains implausible; auto-rotation is disabled until restart");
          vTaskSuspend(nullptr);
        }
      } else {
        invalid_samples = 0;
      }
      if (total >= kMinimumTotalGravity && total <= kMaximumTotalGravity &&
          std::hypot(gx, gy) >= kMinimumPlanarGravity) {
        const int screen_relative = std::fabs(gx) > std::fabs(gy)
                                        ? (gx > 0 ? 270 : 90)
                                        : (gy > 0 ? 0 : 180);
        const int suggested = screen_relative;
        if (suggested != candidate) {
          candidate = suggested;
          candidate_since_ms = current_ms;
        }
        const bool stable = current_ms >= candidate_since_ms + kOrientationStableMs;
        const bool cooldown = last_change_ms == 0 ||
                              current_ms >= last_change_ms + kOrientationChangeCooldownMs;
        if (stable && cooldown &&
            candidate != applied_.load(std::memory_order_acquire)) {
          // Do not acknowledge a pose until the renderer and the physical
          // panel have both accepted it. A busy full-frame refresh can defer a
          // rotation briefly; leaving `applied_` unchanged makes the service
          // retry instead of playing a misleading sound and getting stuck.
          const std::lock_guard<std::mutex> lock(mode_mutex_);
          if (mode_.load(std::memory_order_acquire) == kAutomaticMode &&
              display_->set_rotation(candidate)) {
            applied_.store(candidate, std::memory_order_release);
            if (feedback_ != nullptr) feedback_(feedback_context_, candidate);
            last_change_ms = current_ms;
          }
        }
      }
    } else {
      const std::uint64_t current_ms = now_ms();
      if (last_read_error_log_ms == 0 || current_ms - last_read_error_log_ms >= 5000) {
        ESP_LOGW(kLogTag, "QMI8658 read failed: %s", esp_err_to_name(read_result));
        last_read_error_log_ms = current_ms;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

}  // namespace printdeck::platform
