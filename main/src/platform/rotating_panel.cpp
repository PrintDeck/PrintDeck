#include "printdeck/platform/rotating_panel.hpp"

#include <cinttypes>
#include <cstdlib>
#include <cstring>

#include "bsp/display.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

namespace printdeck::platform {
namespace {

constexpr char kTag[] = "rotating_panel";
struct RotationPanel {
  esp_lcd_panel_t base{};
  esp_lcd_panel_handle_t target = nullptr;
  int rotation = 0;
  std::uint16_t* staging = nullptr;
  std::size_t staging_pixels = 0;
  std::uint16_t* rotated = nullptr;
  std::size_t rotated_pixels = 0;
  std::uint8_t staging_slot = 0;
};

RotationPanel g_panel;
esp_lcd_panel_io_handle_t g_panel_io = nullptr;

RotationPanel* context(esp_lcd_panel_t* panel) {
  return panel == nullptr ? nullptr : static_cast<RotationPanel*>(panel->user_data);
}

bool ensure_staging(RotationPanel* panel, std::size_t pixels) {
  if (panel->staging != nullptr && panel->staging_pixels >= pixels) return true;
  std::free(panel->staging);
  panel->staging = static_cast<std::uint16_t*>(heap_caps_malloc(
      pixels * sizeof(std::uint16_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  panel->staging_pixels = panel->staging == nullptr ? 0 : pixels;
  return panel->staging != nullptr;
}

bool ensure_rotated(RotationPanel* panel, std::size_t pixels) {
  if (panel->rotated != nullptr && panel->rotated_pixels >= pixels) return true;
  std::free(panel->rotated);
  panel->rotated = static_cast<std::uint16_t*>(heap_caps_malloc(
      pixels * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  panel->rotated_pixels = panel->rotated == nullptr ? 0 : pixels;
  return panel->rotated != nullptr;
}

void rotate_90(const std::uint16_t* source, std::uint16_t* destination,
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

void rotate_270(const std::uint16_t* source, std::uint16_t* destination,
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

esp_err_t draw_staged(RotationPanel* panel, int x_start, int y_start,
                      int x_end, int y_end, const std::uint16_t* source) {
  const int width = x_end - x_start;
  const int height = y_end - y_start;
  if (panel == nullptr || panel->target == nullptr || source == nullptr ||
      width <= 0 || height <= 0) {
    return ESP_ERR_INVALID_ARG;
  }

  // The LVGL adapter treats every panel-IO completion callback as completion
  // of the entire LVGL flush. Keep one partial flush as one QSPI transaction.
  const std::size_t slot_pixels = static_cast<std::size_t>(width) * height;
  if (!ensure_staging(panel, slot_pixels * 2U)) {
    ESP_LOGE(kTag, "Could not allocate %" PRIu32 " DMA staging pixels",
             static_cast<std::uint32_t>(slot_pixels * 2U));
    return ESP_ERR_NO_MEM;
  }

  std::uint16_t* staging =
      panel->staging + static_cast<std::size_t>(panel->staging_slot) * slot_pixels;
  panel->staging_slot ^= 1U;
  std::memcpy(staging, source, slot_pixels * sizeof(std::uint16_t));
  return esp_lcd_panel_draw_bitmap(panel->target, x_start, y_start, x_end, y_end,
                                   staging);
}

esp_err_t panel_reset(esp_lcd_panel_t* panel) {
  RotationPanel* state = context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_reset(state->target) : ESP_ERR_INVALID_STATE;
}

esp_err_t panel_init(esp_lcd_panel_t* panel) {
  RotationPanel* state = context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_init(state->target) : ESP_ERR_INVALID_STATE;
}

esp_err_t panel_delete(esp_lcd_panel_t* panel) {
  RotationPanel* state = context(panel);
  if (state == nullptr || state->target == nullptr) return ESP_ERR_INVALID_STATE;
  std::free(state->staging);
  std::free(state->rotated);
  state->staging = nullptr;
  state->rotated = nullptr;
  state->staging_pixels = 0;
  state->rotated_pixels = 0;
  return esp_lcd_panel_del(state->target);
}

esp_err_t panel_draw(esp_lcd_panel_t* panel, int x_start, int y_start,
                     int x_end, int y_end, const void* colors) {
  RotationPanel* state = context(panel);
  if (state == nullptr || state->target == nullptr || colors == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  const auto* source = static_cast<const std::uint16_t*>(colors);
  if (state->rotation != 90 && state->rotation != 270) {
    // LVGL buffers live in PSRAM on this board.  QSPI DMA must receive an
    // internal-memory staging buffer even when no software rotation is needed.
    if (esp_ptr_external_ram(colors)) {
      return draw_staged(state, x_start, y_start, x_end, y_end, source);
    }
    return esp_lcd_panel_draw_bitmap(state->target, x_start, y_start,
                                     x_end, y_end, colors);
  }

  const int source_width = x_end - x_start;
  const int source_height = y_end - y_start;
  if (source_width <= 0 || source_height <= 0) return ESP_ERR_INVALID_ARG;
  const std::size_t pixels =
      static_cast<std::size_t>(source_width) * source_height;
  if (!ensure_rotated(state, pixels)) return ESP_ERR_NO_MEM;

  int destination_x_start = 0;
  int destination_y_start = 0;
  int destination_x_end = 0;
  int destination_y_end = 0;
  if (state->rotation == 90) {
    rotate_90(source, state->rotated, source_width, source_height);
    destination_x_start = BSP_LCD_H_RES - y_end;
    destination_x_end = BSP_LCD_H_RES - y_start;
    destination_y_start = x_start;
    destination_y_end = x_end;
  } else {
    rotate_270(source, state->rotated, source_width, source_height);
    destination_x_start = y_start;
    destination_x_end = y_end;
    destination_y_start = BSP_LCD_V_RES - x_end;
    destination_y_end = BSP_LCD_V_RES - x_start;
  }
  return draw_staged(state, destination_x_start, destination_y_start,
                     destination_x_end, destination_y_end, state->rotated);
}

esp_err_t panel_mirror(esp_lcd_panel_t* panel, bool x, bool y) {
  RotationPanel* state = context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_mirror(state->target, x, y) : ESP_ERR_INVALID_STATE;
}

esp_err_t panel_swap(esp_lcd_panel_t* panel, bool swap) {
  RotationPanel* state = context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_swap_xy(state->target, swap) : ESP_ERR_INVALID_STATE;
}

esp_err_t panel_gap(esp_lcd_panel_t* panel, int x, int y) {
  RotationPanel* state = context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_set_gap(state->target, x, y) : ESP_ERR_INVALID_STATE;
}

esp_err_t panel_invert(esp_lcd_panel_t* panel, bool invert) {
  RotationPanel* state = context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_invert_color(state->target, invert) : ESP_ERR_INVALID_STATE;
}

esp_err_t panel_power(esp_lcd_panel_t* panel, bool enabled) {
  RotationPanel* state = context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_disp_on_off(state->target, enabled) : ESP_ERR_INVALID_STATE;
}

esp_err_t panel_sleep(esp_lcd_panel_t* panel, bool sleep) {
  RotationPanel* state = context(panel);
  return state != nullptr && state->target != nullptr
             ? esp_lcd_panel_disp_sleep(state->target, sleep) : ESP_ERR_INVALID_STATE;
}

}  // namespace

esp_lcd_panel_handle_t wrap_rotating_panel(esp_lcd_panel_handle_t target,
                                           esp_lcd_panel_io_handle_t io) {
  g_panel.base = {};
  g_panel.base.reset = panel_reset;
  g_panel.base.init = panel_init;
  g_panel.base.del = panel_delete;
  g_panel.base.draw_bitmap = panel_draw;
  g_panel.base.mirror = panel_mirror;
  g_panel.base.swap_xy = panel_swap;
  g_panel.base.set_gap = panel_gap;
  g_panel.base.invert_color = panel_invert;
  g_panel.base.disp_on_off = panel_power;
  g_panel.base.disp_sleep = panel_sleep;
  g_panel.base.user_data = &g_panel;
  g_panel.target = target;
  g_panel_io = io;
  g_panel.rotation = 0;
  g_panel.staging_slot = 0;
  return &g_panel.base;
}

esp_err_t set_rotating_panel_rotation(int degrees) {
  if (g_panel.target == nullptr || g_panel_io == nullptr) return ESP_ERR_INVALID_STATE;
  degrees = degrees == 90 ? 90 : degrees == 180 ? 180 : degrees == 270 ? 270 : 0;
  std::uint8_t madctl = 0x00;
  int x_gap = 6;
  if (degrees == 180) {
    madctl = 0xC0;
    x_gap = 8;
  }
  g_panel.rotation = degrees;
  ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(g_panel.target, x_gap, 0), kTag,
                      "set panel gap failed");
  std::uint32_t command = 0x36;
  command = (command & 0xffU) << 8U;
  command |= 0x02U << 24U;
  ESP_LOGI(kTag, "Rotation %d: software=%s MADCTL=0x%02x gap=%d,0",
           degrees, degrees == 90 || degrees == 270 ? "yes" : "no", madctl, x_gap);
  return esp_lcd_panel_io_tx_param(g_panel_io, command, &madctl, 1);
}

}  // namespace printdeck::platform
