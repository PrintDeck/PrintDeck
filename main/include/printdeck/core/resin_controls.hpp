#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <string_view>
#include "printdeck/core/device_state.hpp"

namespace printdeck::core {

enum class ResinControl : std::uint8_t { pause, resume, stop };

struct ResinControlRequest {
  std::uint32_t profile_id = 0;
  std::array<char, 37> task{};
  ResinControl action = ResinControl::pause;
};

inline bool resin_control_available(ResinControl action, const PrinterSnapshot& state,
                                    std::uint64_t now_ms) {
  const auto& job = state.job;
  if (!state.profile_id || state.link != LinkState::online || !job.reachable ||
      now_ms < state.updated_at_ms || now_ms - state.updated_at_ms > 5000 ||
      job.preview_hint.size() != 36 || job.condition == PrinterCondition::error ||
      job.resin_stage == ResinStage::finishing) return false;
  switch (action) {
    case ResinControl::pause:
      return job.phase == JobPhase::printing || job.phase == JobPhase::preparing;
    case ResinControl::resume:
      return job.phase == JobPhase::paused && job.resin_stage == ResinStage::paused;
    case ResinControl::stop:
      return job.phase == JobPhase::printing || job.phase == JobPhase::preparing ||
             job.phase == JobPhase::paused;
  }
  return false;
}

inline bool resin_control_matches(const ResinControlRequest& request,
                                  const PrinterSnapshot& state, std::uint64_t now_ms) {
  return request.task.back() == '\0' && state.profile_id == request.profile_id &&
         state.job.preview_hint == std::string_view(request.task.data(), 36) &&
         resin_control_available(request.action, state, now_ms);
}

inline bool resin_control_completed(ResinControl action, const JobState& job) {
  switch (action) {
    case ResinControl::pause: return job.resin_stage == ResinStage::paused;
    case ResinControl::resume: return job.phase == JobPhase::printing || job.phase == JobPhase::preparing;
    case ResinControl::stop: return job.resin_stage == ResinStage::stopped;
  }
  return false;
}

class ResinControlConfirmation {
 public:
  enum class Result { blocked, next, confirmed, cancelled };
  bool begin(ResinControl action, const PrinterSnapshot& state, std::uint64_t now_ms) {
    cancel();
    if (!resin_control_available(action, state, now_ms)) return false;
    request_.profile_id = state.profile_id;
    std::copy_n(state.job.preview_hint.data(), 36, request_.task.data());
    request_.action = action;
    step_ = 1; started_ms_ = now_ms;
    return true;
  }
  bool valid(const PrinterSnapshot& state, std::uint64_t now_ms) const {
    return step_ != 0 && now_ms >= started_ms_ && now_ms - started_ms_ <= 60000 &&
           resin_control_matches(request_, state, now_ms);
  }
  unsigned remaining(std::uint64_t now_ms) const {
    if (!step_ || now_ms < started_ms_) return 5;
    const auto elapsed = now_ms - started_ms_;
    return elapsed >= 5000 ? 0 : static_cast<unsigned>((5000 - elapsed + 999) / 1000);
  }
  Result advance(const PrinterSnapshot& state, std::uint64_t now_ms) {
    if (!valid(state, now_ms)) { cancel(); return Result::cancelled; }
    if (remaining(now_ms)) return Result::blocked;
    if (step_ == 1 && request_.action != ResinControl::pause) {
      step_ = 2; started_ms_ = now_ms; return Result::next;
    }
    step_ = 0;
    return Result::confirmed;
  }
  void cancel() { step_ = 0; request_ = {}; started_ms_ = 0; }
  unsigned step() const { return step_; }
  const ResinControlRequest& request() const { return request_; }
 private:
  ResinControlRequest request_;
  std::uint64_t started_ms_ = 0;
  unsigned step_ = 0;
};

}  // namespace printdeck::core
