#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

namespace printdeck::platform {

#if defined(PRINTDECK_BOARD_KNOMI2)
inline constexpr int kDisplayWidth = 240;
inline constexpr int kDisplayHeight = 240;
inline constexpr bool kDisplayIsRound = true;
inline constexpr bool kDisplayUsesCompactLayout = true;
inline constexpr bool kDisplayRequiresEvenInvalidation = false;
inline constexpr bool kBoardHasAudio = false;
inline constexpr char kBoardVariant[] = "knomi2";
inline constexpr char kFirmwareStableChannel[] = "knomi2";
inline constexpr char kFirmwareOtaAssetPrefix[] = "printdeck_knomi2_ota-";
inline constexpr char kFirmwareFullAssetPrefix[] = "printdeck_knomi2_full-";
inline constexpr char kLegacyFirmwareOtaAssetPrefix[] = "";
inline constexpr char kLegacyFirmwareFullAssetPrefix[] = "";
#elif defined(PRINTDECK_BOARD_LCD_1_54)
inline constexpr int kDisplayWidth = 240;
inline constexpr int kDisplayHeight = 240;
inline constexpr bool kDisplayIsRound = false;
inline constexpr bool kDisplayUsesCompactLayout = true;
inline constexpr bool kDisplayRequiresEvenInvalidation = false;
inline constexpr bool kBoardHasAudio = true;
inline constexpr char kBoardVariant[] = "lcd_1_54";
// Keep this flash layout on its own update channel and release asset family so
// an application-only image is never offered to an incompatible partition map.
inline constexpr char kFirmwareStableChannel[] = "lcd_1_54_layout_2";
inline constexpr char kFirmwareOtaAssetPrefix[] =
    "printdeck_lcd_1_54_layout_2_ota-";
inline constexpr char kFirmwareFullAssetPrefix[] = "printdeck_lcd_1_54_full-";
inline constexpr char kLegacyFirmwareOtaAssetPrefix[] = "";
inline constexpr char kLegacyFirmwareFullAssetPrefix[] = "";
#else
inline constexpr int kDisplayWidth = 466;
inline constexpr int kDisplayHeight = 466;
inline constexpr bool kDisplayIsRound = true;
inline constexpr bool kDisplayUsesCompactLayout = false;
inline constexpr bool kDisplayRequiresEvenInvalidation = true;
inline constexpr bool kBoardHasAudio = true;
inline constexpr char kBoardVariant[] = "amoled_1_75";
// Keep this flash layout on its own update channel and release asset family so
// an application-only image is never offered to an incompatible partition map.
inline constexpr char kFirmwareStableChannel[] = "amoled_1_75_layout_2";
inline constexpr char kFirmwareOtaAssetPrefix[] =
    "printdeck_amoled_1_75_layout_2_ota-";
inline constexpr char kFirmwareFullAssetPrefix[] = "printdeck_amoled_1_75_full-";
inline constexpr char kLegacyFirmwareOtaAssetPrefix[] = "";
inline constexpr char kLegacyFirmwareFullAssetPrefix[] = "";
#endif

inline constexpr bool kDisplayUsesLargeLayout = !kDisplayUsesCompactLayout;
inline constexpr bool kDisplayUsesCompactRoundLayout =
    kDisplayUsesCompactLayout && kDisplayIsRound;

// The application owns LVGL and the esp_lvgl_adapter.  Board implementations
// provide only the physical panel, touch, shared buses and peripheral hooks so
// navigation and printer behavior remain common to all products.
esp_err_t board_early_init();
esp_err_t board_display_new(int maximum_transfer_bytes,
                            esp_lcd_panel_handle_t* panel,
                            esp_lcd_panel_io_handle_t* panel_io);
esp_lcd_panel_handle_t board_adapt_display_panel(esp_lcd_panel_handle_t panel,
                                                  esp_lcd_panel_io_handle_t panel_io);
esp_err_t board_display_set_rotation(int degrees);
using DisplayDrawFailureCallback = void (*)(void* context);
// The callback is invoked only when a panel draw could not be submitted.  It
// lets the shared UI roll back a newly opened full-screen overlay instead of
// leaving an invisible object tree consuming touch input.
void board_display_set_draw_failure_callback(DisplayDrawFailureCallback callback,
                                             void* context);
// Project the board-specific IMU mounting onto the two screen-orientation
// axes consumed by the shared automatic-rotation classifier.
void board_auto_rotation_axes(float x, float y, float z,
                              float* horizontal, float* vertical);
esp_err_t board_touch_new(esp_lcd_touch_handle_t* touch);
void board_touch_transform(int degrees, bool* swap_xy, bool* mirror_x, bool* mirror_y);

esp_err_t board_display_lock(std::uint32_t timeout_ms);
void board_display_unlock();
esp_err_t board_display_brightness_init();
esp_err_t board_display_brightness_set(int percent);
int board_display_brightness_get();
bool board_touch_interrupt_active();

esp_err_t board_i2c_init();
i2c_master_bus_handle_t board_i2c_handle();
esp_codec_dev_handle_t board_audio_codec_speaker_init();

}  // namespace printdeck::platform
