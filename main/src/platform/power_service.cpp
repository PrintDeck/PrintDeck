#include "printdeck/platform/power_service.hpp"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include <algorithm>
#include <array>

#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "printdeck/core/power_button.hpp"

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "power";
constexpr std::uint32_t kI2cTimeoutMs = 1000;
constexpr std::uint8_t kPressStableSamples = 3;
constexpr std::uint8_t kReleaseStableSamples = 10;
constexpr std::uint64_t kButtonErrorLogIntervalMs = 5000;
constexpr std::uint32_t kPowerButtonPin = IO_EXPANDER_PIN_NUM_4;

XPowersPMU s_pmu;
i2c_master_dev_handle_t s_pmu_device = nullptr;
esp_io_expander_handle_t s_expander = nullptr;

esp_err_t read_register_bytes(std::uint8_t address, std::uint8_t* data, std::uint8_t length) {
  if (s_pmu_device == nullptr) return ESP_ERR_INVALID_STATE;
  return i2c_master_transmit_receive(s_pmu_device, &address, 1, data, length, kI2cTimeoutMs);
}

esp_err_t write_register_bytes(std::uint8_t address, const std::uint8_t* data,
                               std::uint8_t length) {
  if (s_pmu_device == nullptr || length > 16) return ESP_ERR_INVALID_ARG;
  std::array<std::uint8_t, 17> buffer{};
  buffer[0] = address;
  std::copy_n(data, length, buffer.begin() + 1);
  return i2c_master_transmit(s_pmu_device, buffer.data(), length + 1, kI2cTimeoutMs);
}

int read_register(std::uint8_t, std::uint8_t address, std::uint8_t* data, std::uint8_t length) {
  return read_register_bytes(address, data, length) == ESP_OK ? 0 : -1;
}

int write_register(std::uint8_t, std::uint8_t address, std::uint8_t* data,
                   std::uint8_t length) {
  return write_register_bytes(address, data, length) == ESP_OK ? 0 : -1;
}

}  // namespace

esp_err_t PowerService::start() {
  if (ready_) return ESP_OK;
  ESP_RETURN_ON_ERROR(bsp_i2c_init(), kLogTag, "Board I2C initialization failed");
  i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
  if (bus == nullptr) return ESP_FAIL;
  if (s_pmu_device == nullptr) {
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = AXP2101_SLAVE_ADDRESS;
    config.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &s_pmu_device), kLogTag,
                        "Could not attach AXP2101");
  }
  if (!s_pmu.begin(AXP2101_SLAVE_ADDRESS, read_register, write_register)) return ESP_FAIL;

  s_expander = bsp_io_expander_init();
  if (s_expander == nullptr ||
      esp_io_expander_set_dir(s_expander, kPowerButtonPin, IO_EXPANDER_INPUT) != ESP_OK) {
    ESP_LOGE(kLogTag, "Conditioned power-key level unavailable; software POWER disabled");
    s_expander = nullptr;
  }

  s_pmu.enableVbusVoltageMeasure();
  s_pmu.enableBattVoltageMeasure();
  s_pmu.enableSystemVoltageMeasure();
  s_pmu.enableTemperatureMeasure();
  s_pmu.disableTSPinMeasure();
  s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  s_pmu.clearIrqStatus();
  const bool emergency_time_configured =
      s_pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_6S);
  s_pmu.setLongPressPowerOFF();
  s_pmu.enableLongPressShutdown();
  std::uint8_t power_off_enable = 0;
  std::uint8_t power_key_timing = 0;
  const bool emergency_verified = emergency_time_configured &&
      read_register_bytes(XPOWERS_AXP2101_PWROFF_EN, &power_off_enable, 1) == ESP_OK &&
      read_register_bytes(XPOWERS_AXP2101_IRQ_OFF_ON_LEVEL_CTRL, &power_key_timing, 1) == ESP_OK &&
      (power_off_enable & 0x03U) == 0x02U &&
      ((power_key_timing >> 2U) & 0x03U) == XPOWERS_POWEROFF_6S;
  if (!emergency_verified) {
    ESP_LOGW(kLogTag, "Could not verify the 6-second emergency power-off configuration");
  }
  s_pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
  s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
  s_pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
  s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  ready_ = true;
  ESP_LOGI(
      kLogTag,
      "AXP2101 ready; POWER uses stable EXIO4 samples, graceful off at %llu ms, "
      "emergency at 6 seconds",
      static_cast<unsigned long long>(
          core::kPowerButtonShutdownCountdownStartMs +
          3 * core::kPowerButtonShutdownCountdownStepMs));
  return ESP_OK;
}

PowerButtonAction PowerService::poll_button() {
  if (!ready_ || s_expander == nullptr) return PowerButtonAction::none;
  const std::uint64_t now_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000ULL);
  std::uint32_t levels = 0;
  const esp_err_t read_result =
      esp_io_expander_get_level(s_expander, kPowerButtonPin, &levels);
  if (read_result != ESP_OK) {
    ++button_read_error_count_;
    transition_candidate_valid_ = false;
    transition_candidate_samples_ = 0;
    release_pending_ = false;
    if (last_button_read_error_log_ms_ == 0 ||
        now_ms - last_button_read_error_log_ms_ >= kButtonErrorLogIntervalMs) {
      ESP_LOGW(kLogTag, "PWR EXIO4 read failed: %s (total errors: %lu); keeping state",
               esp_err_to_name(read_result),
               static_cast<unsigned long>(button_read_error_count_));
      last_button_read_error_log_ms_ = now_ms;
    }
    return PowerButtonAction::none;
  }

  const bool raw_pressed = (levels & kPowerButtonPin) != 0;
  if (raw_pressed == button_pressed_) {
    if (transition_candidate_valid_) {
      if (button_pressed_ && !transition_candidate_pressed_) {
        const std::uint64_t dropout_ms = now_ms - transition_candidate_at_ms_;
        button_pressed_at_ms_ += dropout_ms;
        ESP_LOGI(kLogTag, "Ignored %llu ms POWER contact dropout (%u sample(s))", dropout_ms,
                 transition_candidate_samples_);
      } else {
        ESP_LOGD(kLogTag, "Rejected POWER transition after %u sample(s)",
                 transition_candidate_samples_);
      }
    }
    transition_candidate_valid_ = false;
    transition_candidate_samples_ = 0;
    transition_candidate_at_ms_ = 0;
    release_pending_ = false;
  } else {
    const std::uint8_t required_samples =
        raw_pressed ? kPressStableSamples : kReleaseStableSamples;
    if (!transition_candidate_valid_ || transition_candidate_pressed_ != raw_pressed) {
      transition_candidate_valid_ = true;
      transition_candidate_pressed_ = raw_pressed;
      transition_candidate_samples_ = 1;
      transition_candidate_at_ms_ = now_ms;
    } else if (transition_candidate_samples_ < required_samples) {
      ++transition_candidate_samples_;
    }

    release_pending_ = button_pressed_ && !raw_pressed;
    if (transition_candidate_samples_ >= required_samples) {
      const std::uint64_t transition_at_ms = transition_candidate_at_ms_;
      transition_candidate_valid_ = false;
      transition_candidate_samples_ = 0;
      transition_candidate_at_ms_ = 0;
      release_pending_ = false;

      if (raw_pressed) {
        button_pressed_ = true;
        button_pressed_at_ms_ = transition_at_ms;
        countdown_stage_ = 0;
        ESP_LOGI(kLogTag, "POWER pressed (stable EXIO4)");
        return PowerButtonAction::wake;
      }

      const std::uint64_t held_ms = transition_at_ms - button_pressed_at_ms_;
      const bool visible = countdown_stage_ != 0;
      button_pressed_ = false;
      button_pressed_at_ms_ = 0;
      countdown_stage_ = 0;
      ESP_LOGI(kLogTag, "POWER released after %llu ms (stable EXIO4)", held_ms);
      switch (core::power_button_release_action(held_ms, visible)) {
        case core::PowerButtonReleaseAction::home: return PowerButtonAction::home;
        case core::PowerButtonReleaseAction::cancel_shutdown:
          return PowerButtonAction::cancel;
        case core::PowerButtonReleaseAction::none: return PowerButtonAction::none;
      }
    }
  }

  if (!button_pressed_ || release_pending_) return PowerButtonAction::none;
  const std::uint64_t held_ms = now_ms - button_pressed_at_ms_;
  const std::uint8_t stage = core::power_button_shutdown_stage(held_ms);
  PowerButtonAction action = PowerButtonAction::none;
  switch (stage) {
    case 1: action = PowerButtonAction::show_3; break;
    case 2: action = PowerButtonAction::show_2; break;
    case 3: action = PowerButtonAction::show_1; break;
    case 4: action = PowerButtonAction::shutdown; break;
    default: break;
  }
  if (stage == 0 || stage == countdown_stage_) return PowerButtonAction::none;
  countdown_stage_ = stage;
  return action;
}

PowerSnapshot PowerService::sample() const {
  PowerSnapshot result;
  if (!ready_) return result;
  result.available = true;
  result.battery_present = s_pmu.isBatteryConnect();
  result.usb_present = s_pmu.isVbusIn();
  result.charging = s_pmu.isCharging();
  result.temperature_c = s_pmu.getTemperature();
  if (result.battery_present) {
    result.battery_percent = static_cast<std::uint8_t>(s_pmu.getBatteryPercent());
  }
  return result;
}

esp_err_t PowerService::power_off() {
  if (!ready_) return ESP_ERR_INVALID_STATE;
  esp_err_t last_result = ESP_FAIL;
  for (int attempt = 1; attempt <= 3; ++attempt) {
    std::uint8_t common_config = 0;
    last_result =
        read_register_bytes(XPOWERS_AXP2101_COMMON_CONFIG, &common_config, 1);
    if (last_result == ESP_OK) {
      common_config |= 0x01U;
      last_result =
          write_register_bytes(XPOWERS_AXP2101_COMMON_CONFIG, &common_config, 1);
      if (last_result == ESP_OK) return ESP_OK;
    }
    ESP_LOGW(kLogTag, "AXP2101 shutdown attempt %d failed: %s", attempt,
             esp_err_to_name(last_result));
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return last_result;
}

}  // namespace printdeck::platform
