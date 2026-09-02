#include "printdeck/core/device_state.hpp"

#include <algorithm>
#include <cstdlib>
#include <new>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace printdeck::core {

const PrinterProfile* DeviceState::selected() const {
  const auto found = std::find_if(profiles.begin(), profiles.end(),
                                  [this](const PrinterProfile& profile) {
                                    return profile.id == selected_profile;
                                  });
  return found == profiles.end() ? nullptr : &*found;
}

bool DeviceState::select(std::uint32_t profile_id) {
  if (profile_id == 0) {
    selected_profile = 0;
    return true;
  }
  const bool exists = std::any_of(profiles.begin(), profiles.end(),
                                  [profile_id](const PrinterProfile& profile) {
                                    return profile.id == profile_id;
                                  });
  if (exists) selected_profile = profile_id;
  return exists;
}

bool dashboard_available(std::uint32_t selected_profile,
                         const PrinterSnapshot* snapshot) {
  return selected_profile != 0 && snapshot != nullptr &&
         snapshot->profile_id == selected_profile &&
         snapshot->link == LinkState::online;
}

bool printer_selection_unavailable(std::uint32_t selected_profile,
                                   const PrinterSnapshot* snapshot,
                                   bool connection_grace_elapsed) {
  return connection_grace_elapsed && selected_profile != 0 && snapshot != nullptr &&
         snapshot->profile_id == selected_profile &&
         snapshot->link != LinkState::online;
}

bool retain_last_known_job_during_reconnect(PrinterSnapshot& current,
                                            const PrinterSnapshot& last_known) {
  if (current.profile_id == 0 || current.profile_id != last_known.profile_id ||
      current.link == LinkState::online || current.job.phase != JobPhase::unknown ||
      last_known.link != LinkState::online ||
      last_known.job.phase == JobPhase::unknown) {
    return false;
  }
  current.job.phase = last_known.job.phase;
  current.job.kind = last_known.job.kind;
  current.job.activity = last_known.job.activity;
  current.job.name = last_known.job.name;
  current.job.completion = last_known.job.completion;
  current.job.remaining_seconds = last_known.job.remaining_seconds;
  current.job.reachable = last_known.job.reachable;
  return true;
}

bool printer_selectable(bool selected, PrinterReachability reachability) {
  return !selected || reachability == PrinterReachability::online;
}

bool printer_check_allowed(std::uint64_t now_ms,
                           std::uint64_t last_attempt_ms,
                           std::uint64_t cooldown_ms) {
  if (last_attempt_ms == 0) return true;
  return now_ms >= last_attempt_ms &&
         now_ms - last_attempt_ms >= cooldown_ms;
}

AutomaticProbeDecision automatic_probe_decision(
    bool connected, std::uint8_t previous_consecutive_failures) {
  constexpr std::uint8_t kFailureThreshold = 2;
  if (connected) return {.consecutive_failures = 0, .publish = true};
  const std::uint8_t failures =
      std::min<std::uint8_t>(kFailureThreshold,
                             static_cast<std::uint8_t>(
                                 previous_consecutive_failures +
                                 (previous_consecutive_failures < kFailureThreshold ? 1 : 0)));
  return {.consecutive_failures = failures,
          .publish = failures >= kFailureThreshold};
}

bool same_printer_connection(const PrinterProfile& first,
                             const PrinterProfile& second) {
  return first.id == second.id && first.protocol == second.protocol &&
         first.endpoint == second.endpoint && first.api_key == second.api_key &&
         first.serial == second.serial && first.access_code == second.access_code;
}

SnapshotStore::SnapshotStore() {
#ifdef ESP_PLATFORM
  void* storage = heap_caps_malloc(sizeof(PrinterSnapshot),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (storage == nullptr) {
    storage = heap_caps_malloc(sizeof(PrinterSnapshot),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (storage == nullptr) std::abort();
  value_ = ::new (storage) PrinterSnapshot();
#else
  value_ = new PrinterSnapshot();
#endif
}

SnapshotStore::~SnapshotStore() {
#ifdef ESP_PLATFORM
  if (value_ != nullptr) {
    value_->~PrinterSnapshot();
    heap_caps_free(value_);
  }
#else
  delete value_;
#endif
  value_ = nullptr;
}

PrinterSnapshot SnapshotStore::read() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return *value_;
}

void SnapshotStore::read_into(PrinterSnapshot& destination) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  destination = *value_;
}

void SnapshotStore::replace(PrinterSnapshot next) {
  next.job.normalize();
  const std::lock_guard<std::mutex> lock(mutex_);
  *value_ = std::move(next);
}

}  // namespace printdeck::core
