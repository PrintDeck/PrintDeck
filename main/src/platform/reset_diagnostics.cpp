#include "printdeck/platform/reset_diagnostics.hpp"

#include "esp_attr.h"

namespace printdeck::platform {
namespace {

constexpr uint32_t kCheckpointMagic = 0x504D5253U;  // "PMRS"
RTC_NOINIT_ATTR uint32_t s_checkpoint_magic;
RTC_NOINIT_ATTR uint32_t s_retained_checkpoint;
ResetCheckpoint s_previous_checkpoint = ResetCheckpoint::kNone;

bool valid_checkpoint(uint32_t value) {
  return value <= static_cast<uint32_t>(ResetCheckpoint::kPreviewDecode);
}

}  // namespace

void initialize_reset_diagnostics() {
  if (s_checkpoint_magic == kCheckpointMagic && valid_checkpoint(s_retained_checkpoint)) {
    s_previous_checkpoint = static_cast<ResetCheckpoint>(s_retained_checkpoint);
  } else {
    s_previous_checkpoint = ResetCheckpoint::kNone;
  }
  s_checkpoint_magic = kCheckpointMagic;
  s_retained_checkpoint = static_cast<uint32_t>(ResetCheckpoint::kBooting);
}

void mark_reset_checkpoint(ResetCheckpoint checkpoint) {
  s_checkpoint_magic = kCheckpointMagic;
  s_retained_checkpoint = static_cast<uint32_t>(checkpoint);
}

ResetCheckpoint previous_reset_checkpoint() { return s_previous_checkpoint; }

const char* reset_checkpoint_name(ResetCheckpoint checkpoint) {
  switch (checkpoint) {
    case ResetCheckpoint::kBooting: return "booting";
    case ResetCheckpoint::kRunning: return "running";
    case ResetCheckpoint::kPrintWake: return "print-wake";
    case ResetCheckpoint::kA1PreviewFetch: return "a1-preview-fetch";
    case ResetCheckpoint::kLvglLockRestart: return "lvgl-lock-restart";
    case ResetCheckpoint::kPrintWakeResume: return "print-wake-resume";
    case ResetCheckpoint::kPreviewDecode: return "preview-decode";
    case ResetCheckpoint::kNone:
    default: return "none";
  }
}

}  // namespace printdeck::platform
