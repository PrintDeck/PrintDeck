#include "printdeck/platform/board.hpp"

#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "printdeck/platform/rotating_panel.hpp"

namespace printdeck::platform {
namespace {

// The QSPI panel is write-only: LVGL can still render and capture its scene
// without a connected screen. Keep a polling input for Live View when the
// detached screen's CST9217 does not acknowledge its address after reset.
esp_lcd_touch_t remote_touch{};
bool remote_touch_active = false;

esp_err_t remote_touch_read(esp_lcd_touch_handle_t) { return ESP_OK; }

bool remote_touch_get_xy(esp_lcd_touch_handle_t, std::uint16_t*, std::uint16_t*,
                         std::uint16_t*, std::uint8_t* count, std::uint8_t) {
  *count = 0;
  return false;
}

}  // namespace

esp_err_t board_early_init() { return ESP_OK; }

esp_err_t board_display_new(int maximum_transfer_bytes,
                            esp_lcd_panel_handle_t* panel,
                            esp_lcd_panel_io_handle_t* panel_io) {
  const bsp_display_config_t config = {
      .max_transfer_sz = maximum_transfer_bytes,
  };
  return bsp_display_new(&config, panel, panel_io);
}

esp_lcd_panel_handle_t board_adapt_display_panel(esp_lcd_panel_handle_t panel,
                                                  esp_lcd_panel_io_handle_t panel_io) {
  return wrap_rotating_panel(panel, panel_io);
}

esp_err_t board_display_set_rotation(int degrees) {
  return set_rotating_panel_rotation(degrees);
}

void board_display_set_draw_failure_callback(DisplayDrawFailureCallback, void*) {
  // The round BSP owns its QSPI staging path.  The callback is currently used
  // by the Square LCD wrapper, whose internal-DMA allocation can report a
  // synchronous draw failure.
}

void board_auto_rotation_axes(float x, float y, float,
                              float* horizontal, float* vertical) {
  if (horizontal == nullptr || vertical == nullptr) return;
  *horizontal = -y;
  *vertical = x;
}

esp_err_t board_touch_new(esp_lcd_touch_handle_t* touch) {
  if (touch == nullptr) return ESP_ERR_INVALID_ARG;
  *touch = nullptr;
  const esp_err_t bus_result = bsp_i2c_init();
  if (bus_result != ESP_OK) return bus_result;
  const esp_lcd_touch_config_t config = {
      .x_max = BSP_LCD_H_RES,
      .y_max = BSP_LCD_V_RES,
      .rst_gpio_num = BSP_LCD_TOUCH_RST,
      .int_gpio_num = BSP_LCD_TOUCH_INT,
      .levels = {.reset = 0, .interrupt = 0},
      .flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1},
  };
  esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
  io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
  esp_lcd_panel_io_handle_t io = nullptr;
  const esp_err_t io_result = esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(), &io_config, &io);
  if (io_result != ESP_OK) return io_result;

  // Retain the driver's normal reset and identification sequence. A bus error,
  // allocation failure or an unrecognized responding device is not absence.
  const esp_err_t result = esp_lcd_touch_new_i2c_cst9217(io, &config, touch);
  if (result == ESP_OK) return ESP_OK;
  esp_lcd_panel_io_del(io);
  if (result != ESP_FAIL && result != ESP_ERR_NOT_FOUND) return result;
  const esp_err_t probe = i2c_master_probe(
      bsp_i2c_get_handle(), ESP_LCD_TOUCH_IO_I2C_CST9217_ADDRESS, 50);
  if (probe != ESP_ERR_NOT_FOUND) return result;

  remote_touch.config.x_max = BSP_LCD_H_RES;
  remote_touch.config.y_max = BSP_LCD_V_RES;
  remote_touch.config.rst_gpio_num = GPIO_NUM_NC;
  remote_touch.config.int_gpio_num = GPIO_NUM_NC;
  remote_touch.read_data = remote_touch_read;
  remote_touch.get_xy = remote_touch_get_xy;
  remote_touch_active = true;
  *touch = &remote_touch;
  ESP_LOGW("amoled_board", "Touch controller absent; continuing with Live View input only");
  return ESP_OK;
}

void board_touch_transform(int degrees, bool* swap_xy, bool* mirror_x, bool* mirror_y) {
  if (swap_xy == nullptr || mirror_x == nullptr || mirror_y == nullptr) return;
  *swap_xy = degrees == 90 || degrees == 270;
  *mirror_x = degrees == 0 || degrees == 270;
  *mirror_y = degrees == 0 || degrees == 90;
}

esp_err_t board_display_lock(std::uint32_t timeout_ms) {
  return bsp_display_lock(timeout_ms);
}

void board_display_unlock() { bsp_display_unlock(); }

esp_err_t board_display_brightness_init() { return bsp_display_brightness_init(); }

esp_err_t board_display_brightness_set(int percent) {
  return bsp_display_brightness_set(percent);
}

int board_display_brightness_get() { return bsp_display_brightness_get(); }

bool board_touch_interrupt_active() {
  return !remote_touch_active && gpio_get_level(BSP_LCD_TOUCH_INT) == 0;
}

esp_err_t board_i2c_init() { return bsp_i2c_init(); }

i2c_master_bus_handle_t board_i2c_handle() { return bsp_i2c_get_handle(); }

esp_codec_dev_handle_t board_audio_codec_speaker_init() {
  return bsp_audio_codec_speaker_init();
}

esp_codec_dev_handle_t board_audio_codec_microphone_init() {
  return bsp_audio_codec_microphone_init();
}

}  // namespace printdeck::platform
