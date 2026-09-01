#include "printdeck/platform/power_service.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "printdeck/core/power_button.hpp"
#include "printdeck/platform/board.hpp"

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "power_knomi2";
// BIGTREETECH's KNOMI2 reference firmware names the active-low BOOT switch
// GPIO0 and enables its internal pull-up. It is a normal GPIO input, not a
// PMIC key or an I2C button controller.
constexpr gpio_num_t kPowerButtonPin = GPIO_NUM_0;
constexpr std::uint64_t kButtonDebounceMs = 40;

}  // namespace

esp_err_t PowerService::start() {
  if (ready_) return ESP_OK;
  const gpio_config_t input = {
      .pin_bit_mask = 1ULL << kPowerButtonPin,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&input), kLogTag,
                      "GPIO0 POWER button initialization failed");
  ready_ = true;
  ESP_LOGI(kLogTag,
           "GPIO0 POWER button ready; graceful shutdown uses deep sleep");
  return ESP_OK;
}

PowerSnapshot PowerService::sample() const { return {}; }

PowerButtonAction PowerService::poll_button() {
  if (!ready_) return PowerButtonAction::none;
  const std::uint64_t now =
      static_cast<std::uint64_t>(esp_timer_get_time() / 1000ULL);
  const bool pressed_now = gpio_get_level(kPowerButtonPin) == 0;

  if (pressed_now && !button_pressed_) {
    button_pressed_ = true;
    button_pressed_at_ms_ = now;
    release_pending_ = false;
    release_candidate_at_ms_ = 0;
    countdown_stage_ = 0;
    ESP_LOGI(kLogTag, "POWER pressed");
    return PowerButtonAction::wake;
  }

  if (!pressed_now && button_pressed_) {
    if (!release_pending_) {
      release_pending_ = true;
      release_candidate_at_ms_ = now;
      return PowerButtonAction::none;
    }
    if (now - release_candidate_at_ms_ < kButtonDebounceMs) {
      return PowerButtonAction::none;
    }
    const std::uint64_t held =
        release_candidate_at_ms_ - button_pressed_at_ms_;
    const bool countdown_visible = countdown_stage_ != 0;
    button_pressed_ = false;
    button_pressed_at_ms_ = 0;
    release_pending_ = false;
    release_candidate_at_ms_ = 0;
    countdown_stage_ = 0;
    ESP_LOGI(kLogTag, "POWER released after %llu ms", held);
    switch (core::power_button_release_action(held, countdown_visible)) {
      case core::PowerButtonReleaseAction::home:
        return PowerButtonAction::home;
      case core::PowerButtonReleaseAction::cancel_shutdown:
        return PowerButtonAction::cancel;
      case core::PowerButtonReleaseAction::none:
        return PowerButtonAction::none;
    }
  }

  if (pressed_now) {
    release_pending_ = false;
    release_candidate_at_ms_ = 0;
  }
  if (!button_pressed_) return PowerButtonAction::none;

  const std::uint64_t held = now - button_pressed_at_ms_;
  const std::uint8_t stage = core::power_button_shutdown_stage(held);
  PowerButtonAction action = PowerButtonAction::none;
  switch (stage) {
    case 1: action = PowerButtonAction::show_3; break;
    case 2: action = PowerButtonAction::show_2; break;
    case 3: action = PowerButtonAction::show_1; break;
    case 4: action = PowerButtonAction::shutdown; break;
    default: break;
  }
  if (stage == 0 || stage == countdown_stage_) {
    return PowerButtonAction::none;
  }
  countdown_stage_ = stage;
  return action;
}

esp_err_t PowerService::power_off() {
  if (!ready_) return ESP_ERR_INVALID_STATE;
  ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(kPowerButtonPin, 0),
                      kLogTag, "POWER deep-sleep wake configuration failed");

  // The countdown completes while the switch is still held. Wait for a clean
  // release before sleeping, otherwise the active-low wake source would wake
  // the ESP32 immediately.
  while (gpio_get_level(kPowerButtonPin) == 0) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  vTaskDelay(pdMS_TO_TICKS(kButtonDebounceMs));
  if (gpio_get_level(kPowerButtonPin) == 0) {
    while (gpio_get_level(kPowerButtonPin) == 0) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(kButtonDebounceMs));
  }

  ESP_LOGI(kLogTag, "Entering POWER-button deep sleep");
  ESP_RETURN_ON_ERROR(board_display_brightness_set(0), kLogTag,
                      "Display backlight shutdown failed");
  vTaskDelay(pdMS_TO_TICKS(30));
  esp_deep_sleep_start();
  return ESP_FAIL;
}

}  // namespace printdeck::platform
