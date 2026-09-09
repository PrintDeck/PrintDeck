#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include "printdeck/platform/elegoo_sdcp_parser.hpp"
#include "printdeck/core/resin_controls.hpp"

namespace printdeck::platform {

struct UniformationLayerExecution {
  std::uint64_t uptime_ms = 0;
  std::uint16_t layer = 0;
  std::uint32_t exposure_ms = 0;
};
std::optional<UniformationLayerExecution> uniformation_layer_execution(std::string_view log);

// GK3 Ultra uses the SDCP transport, with its own identity and resin layer cycle.
class UniformationSdcpParser {
 public:
  UniformationSdcpParser(std::uint32_t profile_id, std::string expected_id);
  ElegooSdcpMessage ingest(std::string_view message, std::uint64_t now_ms);
  bool ready(std::uint64_t now_ms) const;
  void snapshot_into(core::PrinterSnapshot& out, std::uint64_t now_ms) const;
  const ElegooIdentity& identity() const { return identity_; }
  const std::string& outer_id() const { return outer_id_; }
  ElegooError error() const { return error_; }
  std::uint64_t last_status_ms() const { return last_status_ms_; }
  const std::string& task_id() const { return task_id_; }
  bool task_details_known() const { return task_details_known_; }
  bool needs_layer_execution(std::uint64_t now_ms) const;
  void ingest_layer_execution(const UniformationLayerExecution&, std::uint64_t now_ms);
  bool active_layer_cycle() const;
  std::uint16_t current_layer() const { return snapshot_->job.current_layer; }
 private:
  void update_exposure();
  std::unique_ptr<core::PrinterSnapshot> snapshot_;
  ElegooIdentity identity_;
  std::string expected_id_, outer_id_;
  std::string task_id_, task_name_;
  core::ResinPrintSettings task_settings_;
  std::uint64_t last_status_ms_ = 0;
  std::optional<std::uint64_t> task_begin_ms_, printer_uptime_ms_, exposure_started_ms_;
  std::optional<UniformationLayerExecution> execution_;
  bool identity_known_ = false, status_known_ = false;
  bool task_details_known_ = false;
  ElegooError error_ = ElegooError::none;
};

std::optional<std::string> uniformation_sdcp_read_request(unsigned command,
    std::string_view mainboard_id, std::string_view outer_id,
    std::uint64_t request_id, std::uint64_t timestamp_seconds,
    std::string_view task_id = {});
std::optional<std::string> uniformation_sdcp_control_request(core::ResinControl action,
    std::string_view mainboard_id, std::string_view outer_id,
    std::uint64_t request_id, std::uint64_t timestamp_seconds);
bool uniformation_valid_task_id(std::string_view value);
// Decode only bounded, uncompressed printer BMP previews; output is BGRA.
bool uniformation_decode_preview_bmp(const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& pixels, std::uint16_t& width, std::uint16_t& height);
ElegooPollResult uniformation_sdcp_probe(const core::PrinterProfile& profile,
    std::uint64_t deadline_ms, const std::function<bool()>& cancelled,
    ElegooIdentity* identity = nullptr);

}  // namespace printdeck::platform
