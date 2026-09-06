#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "printdeck/platform/elegoo_types.hpp"

namespace printdeck::platform {

inline constexpr std::size_t kElegooSdcpMaximumMessage = 16384;
inline constexpr std::uint64_t kElegooSdcpStatusLifetimeMs = 10000;

struct ElegooSdcpEndpoint {
  std::string host;
  std::uint16_t port = 3030;
};

// Only local IPv4 addresses and local host names. Saved endpoints have no scheme/path.
std::optional<ElegooSdcpEndpoint> elegoo_sdcp_endpoint(std::string_view endpoint);
bool elegoo_sdcp_valid_mainboard_id(std::string_view value);
bool elegoo_sdcp_decode_discovery(std::string_view packet, ElegooIdentity& identity,
                                  std::string* outer_id = nullptr);

// The command argument is checked here, not just by callers. No arbitrary SDCP commands.
std::optional<std::string> elegoo_sdcp_read_request(unsigned command,
    std::string_view mainboard_id, std::uint64_t request_id, std::uint64_t timestamp_ms,
    std::string_view outer_id = {});
bool elegoo_sdcp_valid_upgrade(std::string_view headers, std::string_view expected_accept);

enum class ElegooSdcpMessage { ignored, attributes, status, invalid };

class ElegooSdcpParser {
 public:
  explicit ElegooSdcpParser(std::uint32_t profile_id, std::string mainboard_id);
  ~ElegooSdcpParser();
  bool seed_identity(const ElegooIdentity& identity);
  ElegooSdcpMessage ingest(std::string_view message, std::uint64_t now_ms);
  bool ready(std::uint64_t now_ms) const;
  // Large snapshots stay in the caller's storage; never on the WS callback stack.
  void snapshot_into(core::PrinterSnapshot& out, std::uint64_t now_ms) const;
  const ElegooIdentity& identity() const { return identity_; }
  ElegooError error() const { return error_; }
  std::uint64_t last_status_ms() const { return last_status_ms_; }
 private:
  void update_phase(std::uint64_t now_ms);
  std::unique_ptr<core::PrinterSnapshot> snapshot_;
  ElegooIdentity identity_;
  std::string expected_id_;
  std::string task_id_;
  // Some CC1 firmware omits TaskId. Epochs follow observed job boundaries,
  // independently of filename and the optional printer-supplied identifier.
  std::uint64_t job_epoch_ = 1;
  std::uint64_t completed_epoch_ = 0;
  int machine_state_ = -1;
  int print_state_ = -1;
  bool identity_known_ = false;
  bool baseline_known_ = false;
  bool received_status_ = false;
  bool total_ticks_known_ = false;
  double total_ticks_ = 0;
  std::uint64_t last_status_ms_ = 0;
  std::uint64_t last_state_ms_ = 0;
  std::uint64_t completed_until_ms_ = 0;
  ElegooError error_ = ElegooError::none;
};

// Understands both IDF chunks within a frame and WebSocket continuation frames.
// Control frames may interrupt a fragmented text message without discarding it.
class ElegooSdcpFrames {
 public:
  enum class Result { incomplete, complete, ignored, invalid };
  Result append(unsigned opcode, bool final, std::size_t frame_size,
                std::size_t offset, std::string_view chunk);
  const std::string& message() const { return message_; }
  std::string take();
  void reset();
 private:
  std::string message_;
  std::size_t frame_size_ = 0;
  std::size_t frame_offset_ = 0;
  unsigned frame_opcode_ = 0;
  bool frame_final_ = false;
  bool in_frame_ = false;
  bool fragmented_ = false;
  bool complete_ = false;
};

}  // namespace printdeck::platform
