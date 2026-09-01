// KNOMI2 hardware mapping follows the public BIGTREETECH KNOMI V2 pinout.
// PrintDeck uses ESP-IDF panel and touch components directly.
#include "printdeck/platform/board.hpp"

#include <algorithm>
#include <cstdlib>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_rom_sys.h"

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "board_knomi2";
constexpr spi_host_device_t kLcdSpiHost = SPI2_HOST;
constexpr gpio_num_t kLcdSclk = GPIO_NUM_18;
constexpr gpio_num_t kLcdMosi = GPIO_NUM_14;
constexpr gpio_num_t kLcdReset = GPIO_NUM_21;
constexpr gpio_num_t kLcdDc = GPIO_NUM_19;
constexpr gpio_num_t kLcdCs = GPIO_NUM_20;
constexpr gpio_num_t kLcdBacklight = GPIO_NUM_12;
constexpr gpio_num_t kTouchReset = GPIO_NUM_16;
constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_17;
constexpr gpio_num_t kI2cScl = GPIO_NUM_1;
constexpr gpio_num_t kI2cSda = GPIO_NUM_2;
constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;

i2c_master_bus_handle_t s_i2c = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;
bool s_backlight_ready = false;
int s_brightness = 100;
int s_backlight_step = -1;
DisplayDrawFailureCallback s_draw_failure_callback = nullptr;
void* s_draw_failure_context = nullptr;

struct RotationPanel {
  esp_lcd_panel_t base{};
  esp_lcd_panel_handle_t target = nullptr;
  int software_rotation = 0;
  std::uint16_t* buffers = nullptr;
  std::size_t slot_pixels = 0;
  std::uint8_t slot = 0;
};

RotationPanel s_rotation_panel;

esp_err_t report_draw_result(esp_err_t result) {
  if (result != ESP_OK && s_draw_failure_callback != nullptr) {
    s_draw_failure_callback(s_draw_failure_context);
  }
  return result;
}

RotationPanel* rotation_context(esp_lcd_panel_t* panel) {
  return panel == nullptr ? nullptr
                          : static_cast<RotationPanel*>(panel->user_data);
}

bool reserve_rotation_buffers(RotationPanel* panel, std::size_t pixels) {
  if (panel->buffers != nullptr && panel->slot_pixels >= pixels) return true;
  auto* buffers = static_cast<std::uint16_t*>(heap_caps_malloc(
      pixels * 2U * sizeof(std::uint16_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (buffers == nullptr) return false;
  std::free(panel->buffers);
  panel->buffers = buffers;
  panel->slot_pixels = pixels;
  panel->slot = 0;
  return true;
}

void rotate_region_90(const std::uint16_t* source, std::uint16_t* destination,
                      int source_width, int source_height) {
  for (int destination_y = 0; destination_y < source_width; ++destination_y) {
    const std::uint16_t* source_column = source + destination_y;
    std::uint16_t* destination_row =
        destination + static_cast<std::size_t>(destination_y) * source_height;
    for (int destination_x = 0; destination_x < source_height; ++destination_x) {
      destination_row[destination_x] =
          source_column[static_cast<std::size_t>(source_height - 1 - destination_x) *
                        source_width];
    }
  }
}

void rotate_region_180(const std::uint16_t* source, std::uint16_t* destination,
                       int source_width, int source_height) {
  const std::size_t pixels =
      static_cast<std::size_t>(source_width) * source_height;
  for (std::size_t index = 0; index < pixels; ++index) {
    destination[index] = source[pixels - 1U - index];
  }
}

void rotate_region_270(const std::uint16_t* source, std::uint16_t* destination,
                       int source_width, int source_height) {
  for (int destination_y = 0; destination_y < source_width; ++destination_y) {
    const int source_x = source_width - 1 - destination_y;
    std::uint16_t* destination_row =
        destination + static_cast<std::size_t>(destination_y) * source_height;
    for (int destination_x = 0; destination_x < source_height; ++destination_x) {
      destination_row[destination_x] =
          source[static_cast<std::size_t>(destination_x) * source_width + source_x];
    }
  }
}

esp_err_t rotation_panel_reset(esp_lcd_panel_t* panel) {
  RotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_reset(state->target) : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_init(esp_lcd_panel_t* panel) {
  RotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_init(state->target) : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_delete(esp_lcd_panel_t* panel) {
  RotationPanel* state = rotation_context(panel);
  if (state == nullptr || state->target == nullptr) return ESP_ERR_INVALID_STATE;
  std::free(state->buffers);
  state->buffers = nullptr;
  state->slot_pixels = 0;
  return esp_lcd_panel_del(state->target);
}

esp_err_t rotation_panel_draw(esp_lcd_panel_t* panel, int x_start, int y_start,
                              int x_end, int y_end, const void* colors) {
  RotationPanel* state = rotation_context(panel);
  if (state == nullptr || state->target == nullptr || colors == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (state->software_rotation == 0) {
    return report_draw_result(esp_lcd_panel_draw_bitmap(
        state->target, x_start, y_start, x_end, y_end, colors));
  }

  const int source_width = x_end - x_start;
  const int source_height = y_end - y_start;
  if (source_width <= 0 || source_height <= 0) return ESP_ERR_INVALID_ARG;
  const std::size_t pixels =
      static_cast<std::size_t>(source_width) * source_height;
  if (state->buffers == nullptr || state->slot_pixels < pixels) {
    ESP_LOGE(kLogTag,
             "Reserved LCD rotation buffers are unavailable or too small "
             "(need=%u px reserved=%u px)",
             static_cast<unsigned>(pixels),
             static_cast<unsigned>(state->slot_pixels));
    if (s_draw_failure_callback != nullptr) {
      s_draw_failure_callback(s_draw_failure_context);
    }
    return ESP_ERR_NO_MEM;
  }
  std::uint16_t* rotated =
      state->buffers + static_cast<std::size_t>(state->slot) * state->slot_pixels;
  state->slot ^= 1U;
  const auto* source = static_cast<const std::uint16_t*>(colors);

  int destination_x_start = 0;
  int destination_y_start = 0;
  int destination_x_end = 0;
  int destination_y_end = 0;
  if (state->software_rotation == 90) {
    rotate_region_90(source, rotated, source_width, source_height);
    destination_x_start = kDisplayWidth - y_end;
    destination_x_end = kDisplayWidth - y_start;
    destination_y_start = x_start;
    destination_y_end = x_end;
  } else if (state->software_rotation == 180) {
    rotate_region_180(source, rotated, source_width, source_height);
    destination_x_start = kDisplayWidth - x_end;
    destination_x_end = kDisplayWidth - x_start;
    destination_y_start = kDisplayHeight - y_end;
    destination_y_end = kDisplayHeight - y_start;
  } else {
    rotate_region_270(source, rotated, source_width, source_height);
    destination_x_start = y_start;
    destination_x_end = y_end;
    destination_y_start = kDisplayHeight - x_end;
    destination_y_end = kDisplayHeight - x_start;
  }
  return report_draw_result(esp_lcd_panel_draw_bitmap(
      state->target, destination_x_start, destination_y_start,
      destination_x_end, destination_y_end, rotated));
}

esp_err_t rotation_panel_mirror(esp_lcd_panel_t* panel, bool x, bool y) {
  RotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_mirror(state->target, x, y) : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_swap(esp_lcd_panel_t* panel, bool swap) {
  RotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_swap_xy(state->target, swap) : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_gap(esp_lcd_panel_t* panel, int x, int y) {
  RotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_set_gap(state->target, x, y) : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_invert(esp_lcd_panel_t* panel, bool invert) {
  RotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_invert_color(state->target, invert)
             : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_power(esp_lcd_panel_t* panel, bool enabled) {
  RotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_disp_on_off(state->target, enabled)
             : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_sleep(esp_lcd_panel_t* panel, bool sleep) {
  RotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_disp_sleep(state->target, sleep)
             : ESP_ERR_INVALID_STATE;
}

esp_lcd_panel_handle_t wrap_rotation_panel(esp_lcd_panel_handle_t target) {
  s_rotation_panel.base = {};
  s_rotation_panel.base.reset = rotation_panel_reset;
  s_rotation_panel.base.init = rotation_panel_init;
  s_rotation_panel.base.del = rotation_panel_delete;
  s_rotation_panel.base.draw_bitmap = rotation_panel_draw;
  s_rotation_panel.base.mirror = rotation_panel_mirror;
  s_rotation_panel.base.swap_xy = rotation_panel_swap;
  s_rotation_panel.base.set_gap = rotation_panel_gap;
  s_rotation_panel.base.invert_color = rotation_panel_invert;
  s_rotation_panel.base.disp_on_off = rotation_panel_power;
  s_rotation_panel.base.disp_sleep = rotation_panel_sleep;
  s_rotation_panel.base.user_data = &s_rotation_panel;
  s_rotation_panel.target = target;
  s_rotation_panel.software_rotation = 0;
  s_rotation_panel.slot = 0;
  return &s_rotation_panel.base;
}

}  // namespace

esp_err_t board_early_init() { return ESP_OK; }

esp_err_t board_display_new(int maximum_transfer_bytes,
                            esp_lcd_panel_handle_t* panel,
                            esp_lcd_panel_io_handle_t* panel_io) {
  if (panel == nullptr || panel_io == nullptr || maximum_transfer_bytes <= 0) {
    return ESP_ERR_INVALID_ARG;
  }
  const std::size_t maximum_transfer_pixels =
      static_cast<std::size_t>(maximum_transfer_bytes) / sizeof(std::uint16_t);
  if (maximum_transfer_pixels == 0 ||
      maximum_transfer_pixels * sizeof(std::uint16_t) !=
          static_cast<std::size_t>(maximum_transfer_bytes)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!reserve_rotation_buffers(&s_rotation_panel, maximum_transfer_pixels)) {
    ESP_LOGE(kLogTag,
             "Could not reserve %u-byte LCD DMA rotation buffer; free=%u "
             "largest=%u",
             static_cast<unsigned>(maximum_transfer_bytes * 2U),
             static_cast<unsigned>(heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)));
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(kLogTag,
           "Reserved %u-byte LCD DMA rotation buffer at startup; free=%u "
           "largest=%u",
           static_cast<unsigned>(maximum_transfer_bytes * 2U),
           static_cast<unsigned>(heap_caps_get_free_size(
               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(
               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)));

  const spi_bus_config_t bus_config = {
      .mosi_io_num = kLcdMosi,
      .miso_io_num = GPIO_NUM_NC,
      .sclk_io_num = kLcdSclk,
      .quadwp_io_num = GPIO_NUM_NC,
      .quadhd_io_num = GPIO_NUM_NC,
      .data4_io_num = GPIO_NUM_NC,
      .data5_io_num = GPIO_NUM_NC,
      .data6_io_num = GPIO_NUM_NC,
      .data7_io_num = GPIO_NUM_NC,
      .max_transfer_sz = maximum_transfer_bytes,
      .flags = SPICOMMON_BUSFLAG_MASTER,
      .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
      .intr_flags = 0,
  };
  ESP_RETURN_ON_ERROR(spi_bus_initialize(kLcdSpiHost, &bus_config, SPI_DMA_CH_AUTO),
                      kLogTag, "LCD SPI bus initialization failed");

  const esp_lcd_panel_io_spi_config_t io_config = {
      .cs_gpio_num = kLcdCs,
      .dc_gpio_num = kLcdDc,
      .spi_mode = 0,
      .pclk_hz = 40 * 1000 * 1000,
      .trans_queue_depth = 10,
      .on_color_trans_done = nullptr,
      .user_ctx = nullptr,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .cs_ena_pretrans = 0,
      .cs_ena_posttrans = 0,
      .flags = {},
  };
  ESP_RETURN_ON_ERROR(
      esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kLcdSpiHost),
                               &io_config, panel_io),
      kLogTag, "GC9A01 panel IO initialization failed");

  const esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = kLcdReset,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
      .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
      .bits_per_pixel = 16,
      .flags = {},
      .vendor_config = nullptr,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(*panel_io, &panel_config, panel),
                      kLogTag, "GC9A01 panel initialization failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), kLogTag, "LCD reset failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), kLogTag, "LCD init failed");
  // KNOMI2 mounts the panel with its native X direction reversed. Keep touch
  // coordinates in their native axis; mirroring touch makes horizontal swipes
  // feel backwards on the physical unit.
  ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(*panel, true, false), kLogTag,
                      "LCD horizontal mirror correction failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(*panel, true), kLogTag,
                      "LCD inversion setup failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel, true), kLogTag,
                      "LCD enable failed");
  s_panel = *panel;
  ESP_LOGI(kLogTag, "KNOMI2 GC9A01 display ready at 240x240");
  return ESP_OK;
}

esp_lcd_panel_handle_t board_adapt_display_panel(esp_lcd_panel_handle_t panel,
                                                  esp_lcd_panel_io_handle_t) {
  s_panel = panel;
  return wrap_rotation_panel(panel);
}

esp_err_t board_display_set_rotation(int degrees) {
  if (s_panel == nullptr) return ESP_ERR_INVALID_STATE;
  degrees = degrees == 90 ? 90 : degrees == 180 ? 180 : degrees == 270 ? 270 : 0;
  // Keep the GC9A01 in its verified address mode. Rotating the bounded LVGL
  // flush regions avoids changing the physical mirror correction.
  s_rotation_panel.software_rotation = degrees;
  ESP_LOGI(kLogTag, "Display rotation applied: %d (software=%s)", degrees,
           s_rotation_panel.software_rotation == 0 ? "no" : "yes");
  return ESP_OK;
}

void board_display_set_draw_failure_callback(DisplayDrawFailureCallback callback,
                                             void* context) {
  s_draw_failure_callback = callback;
  s_draw_failure_context = context;
}

void board_auto_rotation_axes(float x, float y, float,
                              float* horizontal, float* vertical) {
  if (horizontal == nullptr || vertical == nullptr) return;
  *horizontal = x;
  *vertical = y;
}

esp_err_t board_i2c_init() {
  if (s_i2c != nullptr) return ESP_OK;
  const i2c_master_bus_config_t config = {
      .i2c_port = kI2cPort,
      .sda_io_num = kI2cSda,
      .scl_io_num = kI2cScl,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags = {.enable_internal_pullup = 1, .allow_pd = 0},
  };
  return i2c_new_master_bus(&config, &s_i2c);
}

i2c_master_bus_handle_t board_i2c_handle() {
  return board_i2c_init() == ESP_OK ? s_i2c : nullptr;
}

esp_err_t board_touch_new(esp_lcd_touch_handle_t* touch) {
  if (touch == nullptr) return ESP_ERR_INVALID_ARG;
  ESP_RETURN_ON_ERROR(board_i2c_init(), kLogTag, "Touch I2C initialization failed");
  const esp_lcd_panel_io_i2c_config_t io_config = {
      .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
      .on_color_trans_done = nullptr,
      .user_ctx = nullptr,
      .control_phase_bytes = 1,
      .dc_bit_offset = 0,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .flags = {.dc_low_on_data = 0, .disable_control_phase = 1},
      .scl_speed_hz = 100 * 1000,
  };
  esp_lcd_panel_io_handle_t touch_io = nullptr;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_config, &touch_io),
                      kLogTag, "CST816S touch IO initialization failed");
  const esp_lcd_touch_config_t touch_config = {
      .x_max = kDisplayWidth,
      .y_max = kDisplayHeight,
      .rst_gpio_num = kTouchReset,
      .int_gpio_num = kTouchInterrupt,
      .levels = {.reset = 0, .interrupt = 0},
      .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
      .process_coordinates = nullptr,
      .interrupt_callback = nullptr,
      .user_data = nullptr,
      .driver_data = nullptr,
  };
  return esp_lcd_touch_new_i2c_cst816s(touch_io, &touch_config, touch);
}

void board_touch_transform(int degrees, bool* swap_xy, bool* mirror_x,
                           bool* mirror_y) {
  if (swap_xy == nullptr || mirror_x == nullptr || mirror_y == nullptr) return;
  *swap_xy = degrees == 90 || degrees == 270;
  *mirror_x = degrees == 90 || degrees == 180;
  *mirror_y = degrees == 180 || degrees == 270;
}

esp_err_t board_display_lock(std::uint32_t timeout_ms) {
  return esp_lv_adapter_lock(static_cast<std::int32_t>(timeout_ms));
}

void board_display_unlock() { esp_lv_adapter_unlock(); }

esp_err_t board_display_brightness_init() {
  if (s_backlight_ready) return ESP_OK;
  const gpio_config_t backlight = {
      .pin_bit_mask = 1ULL << kLcdBacklight,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&backlight), kLogTag,
                      "Backlight GPIO initialization failed");
  ESP_RETURN_ON_ERROR(gpio_set_level(kLcdBacklight, 0), kLogTag,
                      "Backlight reset failed");
  esp_rom_delay_us(3000);
  s_backlight_step = 0;
  s_backlight_ready = true;
  return board_display_brightness_set(s_brightness);
}

esp_err_t board_display_brightness_set(int percent) {
  s_brightness = std::clamp(percent, 0, 100);
  if (!s_backlight_ready) return ESP_OK;

  const int target_step = s_brightness == 0
                              ? 0
                              : 1 + ((s_brightness - 1) * 15) / 99;
  if (target_step == 0) {
    ESP_RETURN_ON_ERROR(gpio_set_level(kLcdBacklight, 0), kLogTag,
                        "Backlight off failed");
    esp_rom_delay_us(3000);
    s_backlight_step = 0;
    return ESP_OK;
  }

  if (s_backlight_step <= 0) {
    ESP_RETURN_ON_ERROR(gpio_set_level(kLcdBacklight, 1), kLogTag,
                        "Backlight wake failed");
    esp_rom_delay_us(25);
    s_backlight_step = 16;
  }
  if (s_backlight_step < target_step) s_backlight_step += 16;
  for (int pulse = s_backlight_step - target_step; pulse > 0; --pulse) {
    ESP_RETURN_ON_ERROR(gpio_set_level(kLcdBacklight, 0), kLogTag,
                        "Backlight pulse low failed");
    esp_rom_delay_us(1);
    ESP_RETURN_ON_ERROR(gpio_set_level(kLcdBacklight, 1), kLogTag,
                        "Backlight pulse high failed");
    esp_rom_delay_us(1);
  }
  s_backlight_step = target_step;
  return ESP_OK;
}

int board_display_brightness_get() { return s_brightness; }

bool board_touch_interrupt_active() {
  return gpio_get_level(kTouchInterrupt) == 0;
}

esp_codec_dev_handle_t board_audio_codec_speaker_init() { return nullptr; }

}  // namespace printdeck::platform
