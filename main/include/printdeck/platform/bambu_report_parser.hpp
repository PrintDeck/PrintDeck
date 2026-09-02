#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "printdeck/core/device_state.hpp"

namespace printdeck::platform {

struct BambuReportParseResult {
  bool parsed = false;
  bool status_report = false;
  bool identity_report = false;
  bool restricted_commands = false;
  bool chamber_light_confirmed = false;
  std::string product_name;
  core::PrinterSnapshot snapshot;
};

BambuReportParseResult parse_bambu_report(const char* payload, std::size_t length,
                                           const core::PrinterSnapshot& previous,
                                           std::uint32_t profile_id,
                                           std::uint64_t updated_at_ms);

}  // namespace printdeck::platform
