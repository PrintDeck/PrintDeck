#pragma once
#include "printdeck/platform/prusalink_client.hpp"

namespace printdeck::platform {
bool tinymaker_identity(std::string_view body);
class TinyMakerClient {
 public:
  TinyMakerClient(PrusaLinkHttpTransport& transport, std::function<std::uint64_t()> now_ms)
      : transport_(transport), now_ms_(std::move(now_ms)) {}
  bool configure(std::string_view endpoint, std::uint32_t profile_id, bool allow_loopback = false);
  bool identify(std::string_view version);
  PrusaLinkPollResult poll(std::uint64_t deadline, const std::function<bool()>& cancelled);
  const PrusaLinkIdentity& identity() const { return identity_; }
 private:
  PrusaLinkHttpTransport& transport_;
  std::function<std::uint64_t()> now_ms_;
  std::string origin_;
  std::uint32_t profile_id_ = 0;
  PrusaLinkIdentity identity_;
  std::unique_ptr<PrusaLinkSample> previous_;
  std::uint64_t last_sample_ms_ = 0;
  std::optional<std::uint32_t> uptime_;
  std::optional<core::ResinPrintSettings> settings_;
  std::uint64_t next_settings_attempt_ms_ = 0;
};
}  // namespace printdeck::platform
