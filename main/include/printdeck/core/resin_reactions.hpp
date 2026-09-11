#pragma once

#include <algorithm>
#include <cmath>
#include "printdeck/core/job_state.hpp"

namespace printdeck::core {

// Small procedural scenes; their motion illustrates a reported phase, not Z telemetry.
enum class ResinReaction : std::uint8_t {
  standby, homing, lowering, exposing, lifting, printing, waiting, paused,
  stopped, complete, error, unavailable,
};

enum class ResinReadout : std::uint8_t { remaining, layers, end_at, elapsed };

inline ResinReadout resin_reaction_readout(std::uint64_t elapsed_ms,
                                           bool finish_known, bool elapsed_known) {
  std::array<ResinReadout, 4> fields{ResinReadout::remaining, ResinReadout::layers};
  std::size_t count = 2;
  if (finish_known) fields[count++] = ResinReadout::end_at;
  if (elapsed_known) fields[count++] = ResinReadout::elapsed;
  return fields[(elapsed_ms / 5000U) % count];
}

inline ResinReaction resin_reaction(const JobState& job) {
  if (!job.reachable) return ResinReaction::unavailable;
  if (job.condition == PrinterCondition::error || job.phase == JobPhase::failed)
    return ResinReaction::error;
  if (job.phase == JobPhase::cancelled) return ResinReaction::stopped;
  if (job.phase == JobPhase::completed) return ResinReaction::complete;
  if (job.phase == JobPhase::paused) return ResinReaction::paused;
  switch (job.resin_stage) {
    case ResinStage::standby: return ResinReaction::standby;
    case ResinStage::homing: return ResinReaction::homing;
    case ResinStage::lowering: return ResinReaction::lowering;
    case ResinStage::exposing: case ResinStage::exposure_test: return ResinReaction::exposing;
    case ResinStage::lifting: return ResinReaction::lifting;
    case ResinStage::pausing: case ResinStage::paused: return ResinReaction::paused;
    case ResinStage::stopping: case ResinStage::stopped: return ResinReaction::stopped;
    case ResinStage::completed: return ResinReaction::complete;
    case ResinStage::checking_file: case ResinStage::transferring_file:
    case ResinStage::device_test: case ResinStage::finishing: return ResinReaction::waiting;
    default: break;
  }
  if (job.phase == JobPhase::printing) return ResinReaction::printing;
  if (job.phase == JobPhase::preparing) return ResinReaction::waiting;
  return ResinReaction::standby;
}

inline std::optional<std::uint32_t> resin_reaction_remaining(const JobState& job) {
  const auto reaction = resin_reaction(job);
  if (reaction == ResinReaction::complete) return 0;
  if (reaction == ResinReaction::unavailable || reaction == ResinReaction::error ||
      reaction == ResinReaction::standby || reaction == ResinReaction::stopped)
    return std::nullopt;
  const bool active_job = job.phase == JobPhase::preparing ||
                         job.phase == JobPhase::printing || job.phase == JobPhase::paused;
  if (!active_job || !job.remaining_known) return std::nullopt;
  return job.remaining_seconds;
}

inline unsigned resin_reaction_progress(const JobState& job) {
  if (!job.reachable || job.phase == JobPhase::idle || job.phase == JobPhase::unknown) return 0;
  if (job.phase == JobPhase::completed) return 100;
  if (!job.completion_known || !std::isfinite(job.completion)) return 0;
  // Match the displayed whole-percent progress: a job at 0% has no fill.
  return static_cast<unsigned>(std::clamp(job.completion, 0.0F, 100.0F));
}

inline bool resin_reaction_animated(ResinReaction reaction) {
  return reaction == ResinReaction::homing || reaction == ResinReaction::lowering ||
         reaction == ResinReaction::lifting || reaction == ResinReaction::exposing ||
         reaction == ResinReaction::printing || reaction == ResinReaction::waiting;
}

struct ResinReactionFrame {
  int platform_y = 49;
  std::uint8_t light_opacity = 0;
  std::uint8_t dot = 0;
};

inline ResinReactionFrame resin_reaction_frame(ResinReaction reaction, std::uint32_t elapsed_ms) {
  ResinReactionFrame frame;
  const int movement = static_cast<int>(std::min<std::uint32_t>(elapsed_ms, 1800U) * 23U / 1800U);
  if (reaction == ResinReaction::lifting) frame.platform_y = 72 - movement;
  else if (reaction == ResinReaction::lowering || reaction == ResinReaction::homing)
    frame.platform_y = 49 + movement;
  else if (reaction == ResinReaction::exposing) {
    frame.platform_y = 72;
    const auto phase = elapsed_ms % 1600U;
    const auto triangle = phase <= 800U ? phase : 1600U - phase;
    frame.light_opacity = static_cast<std::uint8_t>(96U + triangle * 159U / 800U);
  } else if (reaction == ResinReaction::printing || reaction == ResinReaction::waiting) {
    frame.platform_y = 60;
    frame.dot = static_cast<std::uint8_t>((elapsed_ms / 300U) % 3U);
  }
  return frame;
}

}  // namespace printdeck::core
