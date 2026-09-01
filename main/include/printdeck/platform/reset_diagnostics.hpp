#pragma once

#include <cstdint>

#include "esp_system.h"

namespace printdeck::platform {

enum class ResetCheckpoint : uint32_t {
  kNone = 0,
  kBooting,
  kRunning,
  kPrintWake,
  kA1PreviewFetch,
  kLvglLockRestart,
  kPrintWakeResume,
  kPreviewDecode,
};

void initialize_reset_diagnostics();
void mark_reset_checkpoint(ResetCheckpoint checkpoint);
ResetCheckpoint previous_reset_checkpoint();
const char* reset_checkpoint_name(ResetCheckpoint checkpoint);

inline const char* reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external-pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power-glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
    case ESP_RST_UNKNOWN:
    default: return "unknown";
  }
}

}  // namespace printdeck::platform
