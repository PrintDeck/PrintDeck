#pragma once

#include <cstddef>
#include <cstdint>

#include "printdeck/core/device_state.hpp"

namespace printdeck::platform {

struct BambuReportParseResult {
  bool parsed = false;
  bool chamber_light_confirmed = false;
  core::PrinterSnapshot snapshot;
};

BambuReportParseResult parse_bambu_report(const char* payload, std::size_t length,
                                           const core::PrinterSnapshot& previous,
                                           std::uint32_t profile_id,
                                           std::uint64_t updated_at_ms);

}  // namespace printdeck::platform
