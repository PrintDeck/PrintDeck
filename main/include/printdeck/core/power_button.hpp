#pragma once

#include <cstdint>

namespace printdeck::core {

constexpr std::uint64_t kPowerButtonShortPressLimitMs = 1000;
constexpr std::uint64_t kPowerButtonShutdownCountdownStartMs = 1000;
constexpr std::uint64_t kPowerButtonShutdownCountdownStepMs = 600;
constexpr std::uint64_t kPowerButtonDoubleClickMs = 400;
constexpr std::uint64_t kPowerButtonSleepFollowupMs = 5000;

enum class PowerButtonClick { none, single, double_click };

// Feed debounced edges only. A hold cancels both clicks in a pending pair.
class PowerButtonClicks {
 public:
  void press(std::uint64_t now, bool wake_only) {
    pressed_ = true;
    consumed_ = wake_only;
    second_ = pending_ && now - released_at_ <= kPowerButtonDoubleClickMs;
    pending_ = false;
  }
  PowerButtonClick release(std::uint64_t now) {
    pressed_ = false;
    if (consumed_) { cancel(); return PowerButtonClick::none; }
    if (second_) { cancel(); return PowerButtonClick::double_click; }
    pending_ = true;
    released_at_ = now;
    return PowerButtonClick::none;
  }
  PowerButtonClick poll(std::uint64_t now) {
    if (!pressed_ && pending_ && now - released_at_ > kPowerButtonDoubleClickMs) {
      cancel();
      return PowerButtonClick::single;
    }
    return PowerButtonClick::none;
  }
  void cancel() { pending_ = false; second_ = false; consumed_ = true; }

 private:
  std::uint64_t released_at_ = 0;
  bool pending_ = false;
  bool pressed_ = false;
  bool second_ = false;
  bool consumed_ = false;
};

// Pack the start time and stage together so display wake can atomically clear
// a manual sequence across the POWER, display and touch tasks. Zero is inactive.
class ManualDisplaySleep {
 public:
  explicit constexpr ManualDisplaySleep(std::uint64_t state = 0) : state_(state) {}
  constexpr std::uint64_t state() const { return state_; }
  constexpr bool active() const { return state_ != 0; }
  constexpr bool followup_allowed(std::uint64_t now) const {
    return (state_ & 3U) == 3 && now >= started_at() &&
           now - started_at() <= kPowerButtonSleepFollowupMs;
  }
  constexpr ManualDisplaySleep double_click(std::uint64_t now, bool saver_enabled) const {
    return ManualDisplaySleep((now << 2U) | (followup_allowed(now) || !saver_enabled ? 2U : 3U));
  }
  constexpr int mode_after(std::uint64_t now, std::uint32_t saver_seconds,
                           std::uint32_t until_wake) const {
    if (!active()) return 0;
    if ((state_ & 3U) == 2) return 2;
    return saver_seconds != until_wake && now >= started_at() &&
                   now - started_at() >= 1000ULL * saver_seconds ? 2 : 3;
  }

 private:
  constexpr std::uint64_t started_at() const { return state_ >> 2U; }
  std::uint64_t state_;
};

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
