#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "printdeck/core/job_state.hpp"

namespace printdeck::core {

// Observations, not printer-specific identifiers, define local print sessions.
// The journal is bounded and volatile. A new stream establishes a quiet baseline.
inline constexpr std::size_t kPrintEventHistory = 8;
enum class PrintEventType : std::uint8_t {
  started, paused, resumed, completed, failed, cancelled, milestone,
  attention, attention_cleared,
};
inline const char* print_event_type_id(PrintEventType type) {
  constexpr const char* names[] = {"started", "paused", "resumed", "completed", "failed",
      "cancelled", "milestone", "attention", "attention_cleared"};
  return names[static_cast<unsigned>(type)];
}
inline std::string print_stream_id(std::uint64_t stream) {
  char text[17]{};
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(stream));
  return text;
}
inline std::string print_job_id(std::uint64_t stream, std::uint32_t session) {
  if (!session) return {};
  return print_stream_id(stream) + "-" + std::to_string(session);
}
struct PrintEvent {
  std::uint32_t sequence = 0;
  std::uint32_t session = 0;
  std::uint64_t observed_at_ms = 0;
  PrintEventType type = PrintEventType::started;
  JobKind kind = JobKind::print;
  PrinterCondition condition = PrinterCondition::unknown;
  float progress = 0;
  bool progress_known = false;
  std::uint8_t milestone = 0;
};
struct PrintEventHistory {
  std::uint64_t stream = 0;
  std::uint32_t sequence = 0;
  std::uint32_t session = 0;
  std::array<PrintEvent, kPrintEventHistory> events{};
  std::size_t count = 0;
};

class PrintEventTracker {
 public:
  explicit PrintEventTracker(std::uint64_t stream = 0) { history_.stream = stream; }
  const PrintEventHistory& history() const { return history_; }

  void observe(const JobState& job, std::uint64_t sample_ms, bool stale,
               std::uint64_t now_ms, std::uint64_t maximum_gap_ms) {
    if (sample_ms && sample_ms < sample_ms_) return;
    if (stale || !sample_ms || job.phase == JobPhase::unknown) {
      baseline_ = false;
      return;
    }
    // Concurrent HTTP/MQTT readers must not regress an already observed sample.
    if (baseline_ && sample_ms == sample_ms_) return;
    const bool continuous = baseline_ && now_ms >= observed_ms_ &&
        now_ms - observed_ms_ <= maximum_gap_ms;
    const bool active = is_active(job.phase);
    const bool known_progress = job.completion_known && std::isfinite(job.completion);
    const float progress = std::clamp(job.completion, 0.0F, 100.0F);
    if (!continuous) {
      // A print found in progress gets an ID, never a synthetic start/resume.
      history_.session = active ? ++session_counter_ : 0;
      thresholds_ = known_progress ? thresholds(progress) : 0;
      progress_baseline_ = known_progress;
    } else {
      const bool new_job = active && (!is_active(phase_) ||
          (job.phase == JobPhase::preparing && phase_ == JobPhase::printing));
      if (new_job) {
        history_.session = ++session_counter_;
        thresholds_ = known_progress ? thresholds(progress) : 0;
        progress_baseline_ = known_progress;
        emit(PrintEventType::started, job, now_ms);
      } else if (is_active(phase_)) {
        if (job.phase == JobPhase::paused && phase_ != JobPhase::paused)
          emit(PrintEventType::paused, job, now_ms);
        else if (phase_ == JobPhase::paused && job.phase == JobPhase::printing)
          emit(PrintEventType::resumed, job, now_ms);
        else if (job.phase == JobPhase::completed) emit(PrintEventType::completed, job, now_ms);
        else if (job.phase == JobPhase::failed) emit(PrintEventType::failed, job, now_ms);
        else if (job.phase == JobPhase::cancelled) emit(PrintEventType::cancelled, job, now_ms);
      }
      if (!new_job && active && known_progress) {
        const auto reached = thresholds(progress);
        if (progress_baseline_ && job.phase == JobPhase::printing) {
          for (unsigned index = 0; index < 3; ++index)
            if ((reached & (1U << index)) && !(thresholds_ & (1U << index)))
              emit(PrintEventType::milestone, job, now_ms, (index + 1) * 25);
        }
        thresholds_ |= reached;
      }
      progress_baseline_ = known_progress;
      if (job.condition != PrinterCondition::unknown && condition_ != PrinterCondition::unknown) {
        if (needs_attention(job.condition) && job.condition != condition_)
          emit(PrintEventType::attention, job, now_ms);
        else if (!needs_attention(job.condition) && needs_attention(condition_))
          emit(PrintEventType::attention_cleared, job, now_ms);
      }
    }
    phase_ = job.phase;
    condition_ = job.condition;
    sample_ms_ = sample_ms;
    observed_ms_ = now_ms;
    baseline_ = true;
  }

 private:
  static bool is_active(JobPhase phase) {
    return phase == JobPhase::preparing || phase == JobPhase::printing || phase == JobPhase::paused;
  }
  static bool needs_attention(PrinterCondition value) {
    return value == PrinterCondition::attention || value == PrinterCondition::error;
  }
  static unsigned thresholds(float value) {
    return (value >= 25 ? 1U : 0U) | (value >= 50 ? 2U : 0U) | (value >= 75 ? 4U : 0U);
  }
  void emit(PrintEventType type, const JobState& job, std::uint64_t now_ms, unsigned milestone = 0) {
    if (history_.count == kPrintEventHistory) {
      std::move(history_.events.begin() + 1, history_.events.end(), history_.events.begin());
      --history_.count;
    }
    history_.events[history_.count++] = {
        ++history_.sequence, history_.session, now_ms, type, job.kind, job.condition,
        std::clamp(job.completion, 0.0F, 100.0F),
        job.completion_known && std::isfinite(job.completion), static_cast<std::uint8_t>(milestone)};
  }
  PrintEventHistory history_;
  JobPhase phase_ = JobPhase::unknown;
  PrinterCondition condition_ = PrinterCondition::unknown;
  std::uint32_t session_counter_ = 0;
  std::uint64_t sample_ms_ = 0, observed_ms_ = 0;
  unsigned thresholds_ = 0;
  bool baseline_ = false, progress_baseline_ = false;
};

}  // namespace printdeck::core
