#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"

namespace printdeck::platform {

// Wraps the official Waveshare panel handle with the transfer staging and
// software quarter-turn rotation required by this QSPI AMOLED module.
esp_lcd_panel_handle_t wrap_rotating_panel(esp_lcd_panel_handle_t target,
                                           esp_lcd_panel_io_handle_t io);
esp_err_t set_rotating_panel_rotation(int degrees);

}  // namespace printdeck::platform
