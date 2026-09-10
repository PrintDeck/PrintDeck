#pragma once

#include "printdeck/core/device_state.hpp"

namespace printdeck::platform {

struct InactivePrinterStatus {
  std::uint32_t profile_id = 0;
  bool available = false;
  bool connected = false;
  bool checking = false;
  core::JobPhase phase = core::JobPhase::unknown;
  core::JobKind kind = core::JobKind::print;
  std::string job_name;
  float completion = 0.0F;
  bool completion_known = false;
  std::uint32_t elapsed_seconds = 0;
  bool elapsed_known = false;
  std::uint32_t remaining_seconds = 0;
  bool remaining_known = false;
  core::PrinterCondition condition = core::PrinterCondition::normal;
  std::uint64_t updated_at_ms = 0;

  core::PrinterSnapshot printer_snapshot() const {
    core::PrinterSnapshot snapshot;
    snapshot.profile_id = profile_id;
    snapshot.link = !available || checking ? core::LinkState::connecting
        : connected ? core::LinkState::online : core::LinkState::failed;
    snapshot.link_detail = !available ? "Waiting for printer status probe"
        : checking ? "Checking printer status"
        : connected ? "Printer available" : "Printer unavailable";
    snapshot.job.reachable = connected;
    snapshot.job.phase = phase;
    snapshot.job.kind = kind;
    snapshot.job.name = job_name;
    snapshot.job.completion = completion;
    snapshot.job.completion_known = completion_known;
    snapshot.job.elapsed_seconds = elapsed_seconds;
    snapshot.job.elapsed_known = elapsed_known;
    snapshot.job.remaining_seconds = remaining_seconds;
    snapshot.job.remaining_known = remaining_known;
    snapshot.job.condition = condition;
    snapshot.updated_at_ms = updated_at_ms;
    return snapshot;
  }
};

struct InactivePrinterSnapshot {
  std::vector<InactivePrinterStatus> printers;
  std::uint32_t revision = 0;
};

}  // namespace printdeck::platform
