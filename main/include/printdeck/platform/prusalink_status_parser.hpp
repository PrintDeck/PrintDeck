#pragma once

#include <optional>
#include <string_view>

#include "printdeck/core/device_state.hpp"

namespace printdeck::platform {

enum class PrusaLinkDialect : std::uint8_t { undetected, v1, legacy };

struct PrusaLinkIdentity {
  std::string api_version;
  std::string server_version;
  std::string firmware_version;
  std::string model;
};

struct PrusaLinkSample {
  core::PrinterSnapshot snapshot;
  std::optional<std::uint32_t> job_id;
  std::string preview_path;
  bool metadata_loaded = false;
};

std::optional<PrusaLinkIdentity> parse_prusalink_identity(std::string_view body);
std::optional<PrusaLinkSample> parse_prusalink_v1_status(
    std::string_view body, std::uint32_t profile_id, std::uint64_t now_ms);
std::optional<PrusaLinkSample> parse_prusalink_legacy_status(
    std::string_view printer_body, std::string_view job_body,
    std::uint32_t profile_id, std::uint64_t now_ms);
// Status, never optional metadata, owns the phase and current job identity.
bool apply_prusalink_v1_job(std::string_view body, PrusaLinkSample& sample);

}  // namespace printdeck::platform
