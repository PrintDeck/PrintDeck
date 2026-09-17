#pragma once

#include "printdeck/platform/prusalink_client.hpp"

namespace printdeck::platform {

// OctoPrint shares only the bounded, GET-only HTTP transport with PrusaLink.
// Identity and status parsing are specific to upstream OctoPrint's REST API.
bool octoprint_identity(std::string_view body, PrusaLinkIdentity* identity = nullptr);
std::optional<PrusaLinkSample> parse_octoprint_status(
    std::string_view printer_body, std::string_view job_body,
    std::uint32_t profile_id, std::uint64_t now_ms);

class OctoPrintClient {
 public:
  OctoPrintClient(PrusaLinkHttpTransport& transport, std::function<std::uint64_t()> now_ms)
      : transport_(transport), now_ms_(std::move(now_ms)) {}
  bool configure(std::string_view endpoint, std::string api_key,
                 std::uint32_t profile_id, bool allow_loopback = false);
  PrusaLinkPollResult poll(std::uint64_t deadline, const std::function<bool()>& cancelled);
  const PrusaLinkIdentity& identity() const { return identity_; }
 private:
  PrusaLinkHttpTransport& transport_;
  std::function<std::uint64_t()> now_ms_;
  std::string origin_;
  std::string api_key_;
  std::uint32_t profile_id_ = 0;
  PrusaLinkIdentity identity_;
  std::unique_ptr<PrusaLinkSample> previous_;
  std::uint64_t last_sample_ms_ = 0;
};

}  // namespace printdeck::platform
