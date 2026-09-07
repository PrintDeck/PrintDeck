#pragma once

#include "esp_err.h"

namespace printdeck::platform {

// Single voice-worker owner. Destroy the active recognizer before switching
// or closing: ESP-SR may retain pointers into the current PSRAM image.
esp_err_t open_voice_models();
bool activate_voice_model(const char* name);
void close_voice_models();

}  // namespace printdeck::platform
