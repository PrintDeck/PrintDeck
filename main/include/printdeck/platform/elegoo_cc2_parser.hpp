#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "printdeck/platform/elegoo_types.hpp"

namespace printdeck::platform {

inline constexpr std::size_t kElegooCc2MaximumMessageBytes = 32768;
inline constexpr std::uint64_t kElegooCc2FreshnessMs = 15000;
inline constexpr std::uint64_t kElegooCc2TerminalHoldMs = 2000;

bool elegoo_cc2_serial_valid(std::string_view serial);
// The saved endpoint is a local hostname/IPv4, optionally followed by :1883.
// Loopback is accepted only by explicitly opted-in private host fixtures.
std::optional<std::string> elegoo_cc2_host(std::string_view endpoint,
                                         bool allow_loopback = false);
bool elegoo_cc2_endpoint_valid(std::string_view endpoint);

struct ElegooCc2Discovery {
  ElegooIdentity identity;
  bool lan_mode = false;
  bool access_code_required = false;
};

std::optional<ElegooCc2Discovery> parse_elegoo_cc2_discovery(std::string_view body);
bool elegoo_cc2_registration_accepted(std::string_view body,
                                     std::string_view client_id,
                                     bool& capacity_error);
bool elegoo_cc2_pong(std::string_view body);

enum class ElegooCc2Update {
  ignored, invalid, attributes, status, need_baseline, identity_mismatch,
};

// Single session, owned by the MQTT ingestion worker. The transport separately
// verifies exact topics, retained flags and connection generation. Only a
// correlated 1002 response establishes a full baseline; early deltas cannot.
class ElegooCc2Reducer {
 public:
  void configure(std::uint32_t profile_id, const ElegooIdentity& identity);
  ElegooCc2Update apply(std::string_view body, std::uint64_t now_ms,
                        std::uint32_t expected_response_id = 0);
  core::PrinterSnapshot snapshot(std::uint64_t now_ms) const;
  void snapshot_into(core::PrinterSnapshot& destination, std::uint64_t now_ms) const;
  bool baseline_ready() const { return baseline_ready_; }
  bool needs_baseline() const { return needs_baseline_; }
  const ElegooIdentity& identity() const { return identity_; }
  std::uint64_t last_status_ms() const { return last_status_ms_; }

 private:
  ElegooIdentity identity_;
  core::PrinterSnapshot current_;
  std::optional<core::PrinterSnapshot> terminal_;
  std::string task_id_;
  std::array<std::uint64_t, 7> field_times_{};
  std::uint64_t last_status_ms_ = 0;
  std::uint64_t terminal_until_ms_ = 0;
  std::uint32_t last_sequence_ = 0;
  int machine_state_ = -1;
  int substate_ = -1;
  bool have_sequence_ = false;
  bool baseline_ready_ = false;
  bool needs_baseline_ = true;
  bool has_exception_ = false;
};

}  // namespace printdeck::platform
