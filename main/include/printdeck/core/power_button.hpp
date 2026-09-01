#pragma once

#include <cstdint>

namespace printdeck::core {

constexpr std::uint64_t kPowerButtonShortPressLimitMs = 1000;
constexpr std::uint64_t kPowerButtonShutdownCountdownStartMs = 1000;
constexpr std::uint64_t kPowerButtonShutdownCountdownStepMs = 600;

constexpr std::uint8_t power_button_shutdown_stage(std::uint64_t held_ms) {
  if (held_ms >=
      kPowerButtonShutdownCountdownStartMs + 3 * kPowerButtonShutdownCountdownStepMs) {
    return 4;
  }
  if (held_ms >=
      kPowerButtonShutdownCountdownStartMs + 2 * kPowerButtonShutdownCountdownStepMs) {
    return 3;
  }
  if (held_ms >= kPowerButtonShutdownCountdownStartMs + kPowerButtonShutdownCountdownStepMs) {
    return 2;
  }
  return held_ms >= kPowerButtonShutdownCountdownStartMs ? 1 : 0;
}

enum class PowerButtonReleaseAction : std::uint8_t {
  none,
  home,
  cancel_shutdown,
};

constexpr PowerButtonReleaseAction power_button_release_action(
    std::uint64_t held_ms, bool shutdown_countdown_visible) {
  if (shutdown_countdown_visible) return PowerButtonReleaseAction::cancel_shutdown;
  return held_ms < kPowerButtonShortPressLimitMs ? PowerButtonReleaseAction::home
                                                 : PowerButtonReleaseAction::none;
}

}  // namespace printdeck::core
