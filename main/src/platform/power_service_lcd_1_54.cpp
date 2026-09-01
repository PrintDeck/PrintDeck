#include "printdeck/platform/power_service.hpp"

#include <algorithm>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
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

constexpr char kLogTag[] = "power_lcd_1_54";
constexpr gpio_num_t kBatteryAdcPin = GPIO_NUM_1;
constexpr gpio_num_t kBatteryHoldPin = GPIO_NUM_2;
constexpr gpio_num_t kChargingPin = GPIO_NUM_3;
constexpr gpio_num_t kPowerButtonPin = GPIO_NUM_5;
constexpr std::uint64_t kButtonDebounceMs = 40;
constexpr float kBatteryMinimumVoltage = 2.8F;
constexpr float kBatteryMaximumVoltage = 4.5F;
constexpr float kChargeTerminationVoltage = 4.10F;
constexpr float kUsbLatchReleaseDrop = 0.05F;
constexpr float kChargerFloatVoltage = 4.15F;
constexpr float kChargerFloatTransitionVoltage = 4.18F;
constexpr float kBatteryRemovalVoltageJump = 0.035F;
constexpr float kBatteryReinsertVoltage = 4.12F;

adc_oneshot_unit_handle_t s_adc = nullptr;
adc_cali_handle_t s_calibration = nullptr;
adc_channel_t s_adc_channel = ADC_CHANNEL_0;
bool s_calibrated = false;

struct PowerDetectionState {
  bool battery_absent_latched = false;
  bool charger_power_latched = false;
  float charger_reference_voltage = 0.0F;
  bool sample_initialized = false;
  float previous_voltage = 0.0F;
  bool previous_charge_pin_active = false;
  bool previous_battery_present = false;
  bool previous_usb_present = false;
  bool log_initialized = false;
  bool last_battery_present = false;
  bool last_usb_present = false;
  bool last_charge_pin_active = false;
  bool last_usb_host_connected = false;
};

PowerDetectionState s_power_detection;

float battery_voltage() {
  if (s_adc == nullptr) return 0.0F;
  int total = 0;
  constexpr int kSamples = 8;
  for (int sample = 0; sample < kSamples; ++sample) {
    int raw = 0;
    if (adc_oneshot_read(s_adc, s_adc_channel, &raw) != ESP_OK) return 0.0F;
    total += raw;
  }
  const int average = total / kSamples;
  int millivolts = 0;
  if (s_calibrated &&
      adc_cali_raw_to_voltage(s_calibration, average, &millivolts) == ESP_OK) {
    return static_cast<float>(millivolts) * 3.0F / 1000.0F;
  }
  return static_cast<float>(average) * 3.3F * 3.0F / 4095.0F;
}

std::uint8_t battery_percent(float voltage) {
  struct Point { float voltage; int percent; };
  constexpr Point points[] = {
      {3.30F, 0}, {3.52F, 10}, {3.64F, 25}, {3.76F, 45},
      {3.88F, 65}, {4.00F, 82}, {4.12F, 96}, {4.20F, 100},
  };
  if (voltage <= points[0].voltage) return 0;
  constexpr std::size_t point_count = sizeof(points) / sizeof(points[0]);
  for (std::size_t index = 1; index < point_count; ++index) {
    if (voltage <= points[index].voltage) {
      const Point lower = points[index - 1];
      const Point upper = points[index];
      const float ratio = (voltage - lower.voltage) / (upper.voltage - lower.voltage);
      return static_cast<std::uint8_t>(std::clamp(
          lower.percent + static_cast<int>((upper.percent - lower.percent) * ratio),
          0, 100));
    }
  }
  return 100;
}

}  // namespace

esp_err_t PowerService::start() {
  if (ready_) return ESP_OK;
  const gpio_config_t output = {
      .pin_bit_mask = 1ULL << kBatteryHoldPin,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&output), kLogTag, "Battery hold pin failed");
  ESP_RETURN_ON_ERROR(gpio_set_level(kBatteryHoldPin, 1), kLogTag,
                      "Battery power hold failed");
  const gpio_config_t inputs = {
      .pin_bit_mask = (1ULL << kChargingPin) | (1ULL << kPowerButtonPin),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&inputs), kLogTag, "Power inputs failed");

  const adc_oneshot_unit_init_cfg_t unit_config = {
      .unit_id = ADC_UNIT_1,
      .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };
  ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &s_adc), kLogTag,
                      "Battery ADC initialization failed");
  adc_unit_t mapped_unit = ADC_UNIT_1;
  ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(kBatteryAdcPin, &mapped_unit, &s_adc_channel),
                      kLogTag, "Battery ADC pin mapping failed");
  ESP_RETURN_ON_FALSE(mapped_unit == ADC_UNIT_1, ESP_ERR_INVALID_STATE, kLogTag,
                      "Battery ADC pin is not on ADC unit 1");
  const adc_oneshot_chan_cfg_t channel_config = {
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_adc_channel, &channel_config),
                      kLogTag, "Battery ADC channel failed");
  const adc_cali_curve_fitting_config_t calibration_config = {
      .unit_id = ADC_UNIT_1,
      .chan = s_adc_channel,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  s_calibrated = adc_cali_create_scheme_curve_fitting(&calibration_config,
                                                       &s_calibration) == ESP_OK;
  ready_ = true;
  ESP_LOGI(kLogTag, "GPIO/ADC power service ready; calibrated=%s",
           s_calibrated ? "yes" : "no");
  return ESP_OK;
}

PowerButtonAction PowerService::poll_button() {
  if (!ready_) return PowerButtonAction::none;
  const std::uint64_t now = static_cast<std::uint64_t>(esp_timer_get_time() / 1000ULL);
  const bool pressed_now = gpio_get_level(kPowerButtonPin) == 0;
  if (pressed_now && !button_pressed_) {
    button_pressed_ = true;
    button_pressed_at_ms_ = now;
    release_pending_ = false;
    release_candidate_at_ms_ = 0;
    countdown_stage_ = 0;
    return PowerButtonAction::wake;
  }
  if (!pressed_now && button_pressed_) {
    if (!release_pending_) {
      release_pending_ = true;
      release_candidate_at_ms_ = now;
      return PowerButtonAction::none;
    }
    if (now - release_candidate_at_ms_ < kButtonDebounceMs) return PowerButtonAction::none;
    const std::uint64_t held = release_candidate_at_ms_ - button_pressed_at_ms_;
    const bool countdown_visible = countdown_stage_ != 0;
    button_pressed_ = false;
    button_pressed_at_ms_ = 0;
    release_pending_ = false;
    release_candidate_at_ms_ = 0;
    countdown_stage_ = 0;
    switch (core::power_button_release_action(held, countdown_visible)) {
      case core::PowerButtonReleaseAction::home: return PowerButtonAction::home;
      case core::PowerButtonReleaseAction::cancel_shutdown:
        return PowerButtonAction::cancel;
      case core::PowerButtonReleaseAction::none: return PowerButtonAction::none;
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
  if (stage == 0 || stage == countdown_stage_) return PowerButtonAction::none;
  countdown_stage_ = stage;
  return action;
}

PowerSnapshot PowerService::sample() const {
  PowerSnapshot result;
  if (!ready_) return result;
  const float voltage = battery_voltage();
  const bool charge_pin_active = gpio_get_level(kChargingPin) == 0;
  // GPIO19/GPIO20 are connected directly to the ESP32-S3 native USB port.
  // SOF packets therefore provide an exact host-presence signal even after a
  // full battery stops drawing charge current.
  const bool usb_host_connected = usb_serial_jtag_is_connected();
  const bool plausible_battery_voltage =
      voltage >= kBatteryMinimumVoltage && voltage <= kBatteryMaximumVoltage;

  result.available = true;
  // With USB connected and no cell installed, the charger can float BAT near
  // its 4.2 V regulation point and assert CHG. At runtime, unplugging a real
  // cell produces the same float level as a step rather than a normal charge
  // curve. A USB connection creates the same step with a nearly-full cell, so
  // never use that first sample as evidence of battery removal. Only accept a
  // float transition after external power was already present and stable.
  const bool external_power_signal = usb_host_connected || charge_pin_active;
  const bool external_power_just_connected =
      s_power_detection.sample_initialized &&
      !s_power_detection.previous_usb_present && external_power_signal;
  const bool charge_started_at_float =
      s_power_detection.sample_initialized &&
      s_power_detection.previous_battery_present &&
      !external_power_just_connected &&
      !s_power_detection.previous_charge_pin_active && charge_pin_active &&
      voltage >= kChargerFloatTransitionVoltage;
  const bool jumped_to_charger_float =
      s_power_detection.sample_initialized &&
      s_power_detection.previous_battery_present && charge_pin_active &&
      !external_power_just_connected &&
      voltage >= kChargerFloatVoltage &&
      voltage >= s_power_detection.previous_voltage + kBatteryRemovalVoltageJump;

  if (!plausible_battery_voltage) {
    s_power_detection.battery_absent_latched = true;
  } else if (charge_started_at_float || jumped_to_charger_float) {
    s_power_detection.battery_absent_latched = true;
  } else if (s_power_detection.battery_absent_latched) {
    const bool looks_like_inserted_battery =
        voltage < kBatteryReinsertVoltage ||
        (!charge_pin_active && voltage < kChargerFloatTransitionVoltage);
    if (looks_like_inserted_battery) {
      s_power_detection.battery_absent_latched = false;
    }
  }

  result.battery_present =
      plausible_battery_voltage && !s_power_detection.battery_absent_latched;
  // The schematic exposes CHG_STAT but not VBUS as a GPIO. Latch charger power
  // only when charging reaches the termination region; this covers data-less
  // power supplies after the charge LED turns off. If USB is then removed, the
  // resting battery voltage drop releases the latch. Below the termination
  // region a disappearing CHG signal means that external power was removed.
  if (charge_pin_active) {
    s_power_detection.charger_reference_voltage = std::max(
        s_power_detection.charger_reference_voltage, voltage);
    s_power_detection.charger_power_latched =
        s_power_detection.charger_reference_voltage >= kChargeTerminationVoltage;
  } else if (s_power_detection.charger_power_latched) {
    const bool below_termination = voltage < kChargeTerminationVoltage;
    const bool voltage_dropped =
        voltage <= s_power_detection.charger_reference_voltage - kUsbLatchReleaseDrop;
    if (!plausible_battery_voltage || below_termination || voltage_dropped) {
      s_power_detection.charger_power_latched = false;
      s_power_detection.charger_reference_voltage = 0.0F;
    }
  } else {
    s_power_detection.charger_reference_voltage = 0.0F;
  }

  result.usb_present =
      usb_host_connected || charge_pin_active || s_power_detection.charger_power_latched;
  // On this board "charging" is a user-facing external-power state. Show it
  // only for the meaningful combination: a battery is installed and USB is
  // present. USB-only operation has its own header icon.
  result.charging = result.usb_present && result.battery_present;
  result.battery_percent = result.battery_present ? battery_percent(voltage) : 0;

  if (!s_power_detection.log_initialized ||
      s_power_detection.last_battery_present != result.battery_present ||
      s_power_detection.last_usb_present != result.usb_present ||
      s_power_detection.last_charge_pin_active != charge_pin_active ||
      s_power_detection.last_usb_host_connected != usb_host_connected) {
    ESP_LOGI(kLogTag,
             "Power: battery=%s %u%%, usb=%s (host=%s, charge_pin=%s, "
             "usb_latched=%s, battery_absent_latched=%s), "
             "voltage=%.3fV",
             result.battery_present ? "yes" : "no",
             static_cast<unsigned>(result.battery_percent),
             result.usb_present ? "yes" : "no",
             usb_host_connected ? "yes" : "no",
             charge_pin_active ? "active" : "inactive",
             s_power_detection.charger_power_latched ? "yes" : "no",
             s_power_detection.battery_absent_latched ? "yes" : "no",
             static_cast<double>(voltage));
    s_power_detection.log_initialized = true;
    s_power_detection.last_battery_present = result.battery_present;
    s_power_detection.last_usb_present = result.usb_present;
    s_power_detection.last_charge_pin_active = charge_pin_active;
    s_power_detection.last_usb_host_connected = usb_host_connected;
  }
  s_power_detection.sample_initialized = true;
  s_power_detection.previous_voltage = voltage;
  s_power_detection.previous_charge_pin_active = charge_pin_active;
  s_power_detection.previous_battery_present = result.battery_present;
  s_power_detection.previous_usb_present = result.usb_present;
  return result;
}

esp_err_t PowerService::power_off() {
  if (!ready_) return ESP_ERR_INVALID_STATE;
  ESP_RETURN_ON_ERROR(gpio_set_level(kBatteryHoldPin, 0), kLogTag,
                      "Battery power release failed");

  // BAT_EN can physically disconnect the battery rail, but the board's USB
  // path feeds VSYS independently.  If execution continues, USB is still
  // powering the ESP32 (or the battery rail did not fall), so finish shutdown
  // in deep sleep instead of leaving the UI frozen on POWERING OFF.
  ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(kPowerButtonPin, 0), kLogTag,
                      "POWER deep-sleep wake configuration failed");

  while (gpio_get_level(kPowerButtonPin) == 0) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  vTaskDelay(pdMS_TO_TICKS(kButtonDebounceMs));
  if (gpio_get_level(kPowerButtonPin) == 0) {
    while (gpio_get_level(kPowerButtonPin) == 0) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

  ESP_LOGI(kLogTag, "Power rail remains active; entering POWER-button deep sleep");
  board_display_brightness_set(0);
  vTaskDelay(pdMS_TO_TICKS(30));
  esp_deep_sleep_start();
  return ESP_FAIL;
}

}  // namespace printdeck::platform
