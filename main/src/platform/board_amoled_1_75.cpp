#include "printdeck/platform/board.hpp"

#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "printdeck/platform/rotating_panel.hpp"

namespace printdeck::platform {

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
  bsp_display_cfg_t config = {
      .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
      .rotation = ESP_LV_ADAPTER_ROTATE_0,
      .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
      .touch_flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1},
  };
  return bsp_touch_new(&config, touch);
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

bool board_touch_interrupt_active() { return gpio_get_level(BSP_LCD_TOUCH_INT) == 0; }

esp_err_t board_i2c_init() { return bsp_i2c_init(); }

i2c_master_bus_handle_t board_i2c_handle() { return bsp_i2c_get_handle(); }

esp_codec_dev_handle_t board_audio_codec_speaker_init() {
  return bsp_audio_codec_speaker_init();
}

}  // namespace printdeck::platform
