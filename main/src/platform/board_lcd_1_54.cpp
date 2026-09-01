// Hardware mapping and initialization follow Waveshare's official
// ESP32-S3-Touch-LCD-1.54 ESP-IDF reference (Apache-2.0).
#include "printdeck/platform/board.hpp"

#include <algorithm>
#include <cstdlib>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "board_lcd_1_54";
constexpr spi_host_device_t kLcdSpiHost = SPI2_HOST;
constexpr gpio_num_t kLcdSclk = GPIO_NUM_38;
constexpr gpio_num_t kLcdMosi = GPIO_NUM_39;
constexpr gpio_num_t kLcdReset = GPIO_NUM_40;
constexpr gpio_num_t kLcdDc = GPIO_NUM_45;
constexpr gpio_num_t kLcdCs = GPIO_NUM_21;
constexpr gpio_num_t kLcdBacklight = GPIO_NUM_46;
constexpr gpio_num_t kTouchReset = GPIO_NUM_47;
constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_48;
constexpr gpio_num_t kI2cScl = GPIO_NUM_41;
constexpr gpio_num_t kI2cSda = GPIO_NUM_42;
constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;
constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr gpio_num_t kI2sMclk = GPIO_NUM_8;
constexpr gpio_num_t kI2sBclk = GPIO_NUM_9;
constexpr gpio_num_t kI2sLrclk = GPIO_NUM_10;
constexpr gpio_num_t kI2sDin = GPIO_NUM_11;
constexpr gpio_num_t kI2sDout = GPIO_NUM_12;
constexpr gpio_num_t kPowerAmplifier = GPIO_NUM_7;
constexpr gpio_num_t kBatteryHold = GPIO_NUM_2;
constexpr ledc_mode_t kBacklightMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;

i2c_master_bus_handle_t s_i2c = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;
esp_lcd_panel_io_handle_t s_panel_io = nullptr;
bool s_backlight_ready = false;
int s_brightness = 100;
i2s_chan_handle_t s_i2s_tx = nullptr;
i2s_chan_handle_t s_i2s_rx = nullptr;
const audio_codec_data_if_t* s_audio_data = nullptr;
DisplayDrawFailureCallback s_draw_failure_callback = nullptr;
void* s_draw_failure_context = nullptr;

struct LcdRotationPanel {
  esp_lcd_panel_t base{};
  esp_lcd_panel_handle_t target = nullptr;
  int software_rotation = 0;
  std::uint16_t* buffers = nullptr;
  std::size_t slot_pixels = 0;
  std::uint8_t slot = 0;
};

LcdRotationPanel s_rotation_panel;

esp_err_t report_draw_result(esp_err_t result) {
  if (result != ESP_OK && s_draw_failure_callback != nullptr) {
    s_draw_failure_callback(s_draw_failure_context);
  }
  return result;
}

LcdRotationPanel* rotation_context(esp_lcd_panel_t* panel) {
  return panel == nullptr ? nullptr
                          : static_cast<LcdRotationPanel*>(panel->user_data);
}

bool reserve_rotation_buffers(LcdRotationPanel* panel, std::size_t pixels) {
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
  LcdRotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_reset(state->target) : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_init(esp_lcd_panel_t* panel) {
  LcdRotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_init(state->target) : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_delete(esp_lcd_panel_t* panel) {
  LcdRotationPanel* state = rotation_context(panel);
  if (state == nullptr || state->target == nullptr) return ESP_ERR_INVALID_STATE;
  std::free(state->buffers);
  state->buffers = nullptr;
  state->slot_pixels = 0;
  return esp_lcd_panel_del(state->target);
}

esp_err_t rotation_panel_draw(esp_lcd_panel_t* panel, int x_start, int y_start,
                              int x_end, int y_end, const void* colors) {
  LcdRotationPanel* state = rotation_context(panel);
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
  LcdRotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_mirror(state->target, x, y) : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_swap(esp_lcd_panel_t* panel, bool swap) {
  LcdRotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_swap_xy(state->target, swap) : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_gap(esp_lcd_panel_t* panel, int x, int y) {
  LcdRotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_set_gap(state->target, x, y) : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_invert(esp_lcd_panel_t* panel, bool invert) {
  LcdRotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_invert_color(state->target, invert)
             : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_power(esp_lcd_panel_t* panel, bool enabled) {
  LcdRotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_disp_on_off(state->target, enabled)
             : ESP_ERR_INVALID_STATE;
}

esp_err_t rotation_panel_sleep(esp_lcd_panel_t* panel, bool sleep) {
  LcdRotationPanel* state = rotation_context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_disp_sleep(state->target, sleep)
             : ESP_ERR_INVALID_STATE;
}

esp_lcd_panel_handle_t wrap_lcd_rotation_panel(esp_lcd_panel_handle_t target) {
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

esp_err_t board_early_init() {
  const gpio_config_t power_hold = {
      .pin_bit_mask = 1ULL << kBatteryHold,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&power_hold), kLogTag,
                      "Battery power-hold pin initialization failed");
  return gpio_set_level(kBatteryHold, 1);
}

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
  // Reserve both asynchronous RGB565 transfer slots before SPI, networking,
  // audio or camera services can fragment the internal DMA-capable heap.  LVGL
  // is configured for 24 display lines, so this is the maximum region the
  // panel wrapper will ever receive.  Runtime rendering must never grow or
  // replace these buffers.
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
      .spi_mode = 3,
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
      kLogTag, "ST7789 panel IO initialization failed");
  s_panel_io = *panel_io;

  const esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = kLcdReset,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
      .bits_per_pixel = 16,
      .flags = {},
      .vendor_config = nullptr,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(*panel_io, &panel_config, panel),
                      kLogTag, "ST7789 panel initialization failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), kLogTag, "LCD reset failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), kLogTag, "LCD init failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(*panel, true), kLogTag,
                      "LCD inversion setup failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel, true), kLogTag,
                      "LCD enable failed");
  s_panel = *panel;
  ESP_LOGI(kLogTag, "ST7789 display ready at 240x240");
  return ESP_OK;
}

esp_lcd_panel_handle_t board_adapt_display_panel(esp_lcd_panel_handle_t panel,
                                                  esp_lcd_panel_io_handle_t) {
  s_panel = panel;
  return wrap_lcd_rotation_panel(panel);
}

esp_err_t board_display_set_rotation(int degrees) {
  if (s_panel == nullptr || s_panel_io == nullptr) return ESP_ERR_INVALID_STATE;
  degrees = degrees == 90 ? 90 : degrees == 180 ? 180 : degrees == 270 ? 270 : 0;
  // This 240x240 module corrupts its visible window in some rotated ST7789
  // address modes. Keep the controller in its proven native address mode and
  // rotate the small LVGL flush regions in software for every non-zero turn.
  const int previous_software_rotation = s_rotation_panel.software_rotation;
  s_rotation_panel.software_rotation = degrees;
  const std::uint8_t madctl = 0;
  const esp_err_t result =
      esp_lcd_panel_io_tx_param(s_panel_io, LCD_CMD_MADCTL, &madctl, 1);
  if (result != ESP_OK) {
    s_rotation_panel.software_rotation = previous_software_rotation;
    ESP_LOGE(kLogTag, "LCD rotation command failed: %s", esp_err_to_name(result));
    return result;
  }
  ESP_LOGI(kLogTag,
           "Physical display rotation applied: %d (software=%s MADCTL=0x%02x)",
           degrees, s_rotation_panel.software_rotation == 0 ? "no" : "yes",
           static_cast<unsigned>(madctl));
  return ESP_OK;
}

void board_display_set_draw_failure_callback(DisplayDrawFailureCallback callback,
                                             void* context) {
  s_draw_failure_callback = callback;
  s_draw_failure_context = context;
}

void board_auto_rotation_axes(float x, float, float z,
                              float* horizontal, float* vertical) {
  if (horizontal == nullptr || vertical == nullptr) return;
  // In the square enclosure the normal and upside-down resting positions put
  // gravity on the IMU Z axis, while the two side positions put it on X.
  // Project that physical X/Z rotation plane into the shared classifier.
  *horizontal = x;
  *vertical = z;
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
      .scl_speed_hz = 400 * 1000,
  };
  esp_lcd_panel_io_handle_t touch_io = nullptr;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_config, &touch_io),
                      kLogTag, "CST816 touch IO initialization failed");
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

void board_touch_transform(int degrees, bool* swap_xy, bool* mirror_x, bool* mirror_y) {
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
  const ledc_timer_config_t timer = {
      .speed_mode = kBacklightMode,
      .duty_resolution = LEDC_TIMER_10_BIT,
      .timer_num = kBacklightTimer,
      .freq_hz = 5000,
      .clk_cfg = LEDC_AUTO_CLK,
      .deconfigure = false,
  };
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kLogTag, "Backlight timer failed");
  const ledc_channel_config_t channel = {
      .gpio_num = kLcdBacklight,
      .speed_mode = kBacklightMode,
      .channel = kBacklightChannel,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = kBacklightTimer,
      .duty = 1023,
      .hpoint = 0,
      .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
      .flags = {.output_invert = 0},
  };
  ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), kLogTag, "Backlight channel failed");
  s_backlight_ready = true;
  return board_display_brightness_set(s_brightness);
}

esp_err_t board_display_brightness_set(int percent) {
  s_brightness = std::clamp(percent, 0, 100);
  if (!s_backlight_ready) return ESP_OK;
  const std::uint32_t duty = static_cast<std::uint32_t>(1023 * s_brightness / 100);
  ESP_RETURN_ON_ERROR(ledc_set_duty(kBacklightMode, kBacklightChannel, duty), kLogTag,
                      "Backlight duty failed");
  return ledc_update_duty(kBacklightMode, kBacklightChannel);
}

int board_display_brightness_get() { return s_brightness; }

bool board_touch_interrupt_active() { return gpio_get_level(kTouchInterrupt) == 0; }

esp_codec_dev_handle_t board_audio_codec_speaker_init() {
  if (s_audio_data == nullptr) {
    if (board_i2c_init() != ESP_OK) return nullptr;
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(kI2sPort, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    if (i2s_new_channel(&channel_config, &s_i2s_tx, &s_i2s_rx) != ESP_OK) return nullptr;
    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = kI2sMclk,
            .bclk = kI2sBclk,
            .ws = kI2sLrclk,
            .dout = kI2sDout,
            .din = kI2sDin,
            .invert_flags = {},
        },
    };
    if (i2s_channel_init_std_mode(s_i2s_tx, &i2s_config) != ESP_OK ||
        i2s_channel_init_std_mode(s_i2s_rx, &i2s_config) != ESP_OK ||
        i2s_channel_enable(s_i2s_tx) != ESP_OK ||
        i2s_channel_enable(s_i2s_rx) != ESP_OK) {
      return nullptr;
    }
    audio_codec_i2s_cfg_t data_config = {
        .port = kI2sPort,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    s_audio_data = audio_codec_new_i2s_data(&data_config);
    if (s_audio_data == nullptr) return nullptr;
  }

  const audio_codec_gpio_if_t* gpio = audio_codec_new_gpio();
  audio_codec_i2c_cfg_t control_config = {
      .port = kI2cPort,
      .addr = ES8311_CODEC_DEFAULT_ADDR,
      .bus_handle = s_i2c,
  };
  const audio_codec_ctrl_if_t* control = audio_codec_new_i2c_ctrl(&control_config);
  if (gpio == nullptr || control == nullptr) return nullptr;
  const esp_codec_dev_hw_gain_t gain = {
      .pa_voltage = 5.0F,
      .codec_dac_voltage = 3.3F,
  };
  es8311_codec_cfg_t codec_config = {
      .ctrl_if = control,
      .gpio_if = gpio,
      .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
      .pa_pin = kPowerAmplifier,
      .pa_reverted = false,
      .master_mode = false,
      .use_mclk = true,
      .digital_mic = false,
      .invert_mclk = false,
      .invert_sclk = false,
      .hw_gain = gain,
  };
  const audio_codec_if_t* codec = es8311_codec_new(&codec_config);
  if (codec == nullptr) return nullptr;
  esp_codec_dev_cfg_t device_config = {
      .dev_type = ESP_CODEC_DEV_TYPE_OUT,
      .codec_if = codec,
      .data_if = s_audio_data,
  };
  return esp_codec_dev_new(&device_config);
}

}  // namespace printdeck::platform
