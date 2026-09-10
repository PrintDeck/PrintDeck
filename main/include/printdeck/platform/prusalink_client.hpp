#pragma once

#include <functional>
#include <vector>

#include "printdeck/platform/prusalink_auth.hpp"
#include "printdeck/platform/prusalink_status_parser.hpp"

namespace printdeck::platform {

using PrusaLinkAuthMode = core::HttpAuthMode;
enum class PrusaLinkError : std::uint8_t {
  none, invalid_configuration, cancelled, timeout, unavailable,
  authorization, unsupported_authentication, unsupported_response, service_not_ready,
};

struct PrusaLinkCredentials {
  PrusaLinkAuthMode mode = PrusaLinkAuthMode::digest;
  std::string username;
  std::string password;
  std::string api_key;
};

struct PrusaLinkHttpRequest {
  std::string url;
  std::string header_name;
  std::string header_value;
  std::size_t maximum_body = 32 * 1024;
  std::uint64_t deadline_ms = 0;
};

struct PrusaLinkHttpResponse {
  PrusaLinkError error = PrusaLinkError::none;
  int status = 0;
  std::string body;
  std::vector<std::string> challenges;
  std::string content_encoding;
  std::string content_type;
};

// Implementations perform GET only, with no redirects, cookies, proxy or native
// authentication. They must enforce body/header caps and the monotonic deadline,
// validate HTTPS certificates, poll cancellation, and never log header values.
struct PrusaLinkHttpTransport {
  virtual ~PrusaLinkHttpTransport() = default;
  virtual PrusaLinkHttpResponse get(const PrusaLinkHttpRequest& request,
                                    const std::function<bool()>& cancelled) = 0;
};

std::optional<std::string> prusalink_origin(std::string_view endpoint,
                                          bool allow_loopback = false);
std::optional<std::string> prusalink_preview_target(std::string_view reference,
                                                   std::string_view origin);
bool prusalink_preview_png(std::string_view bytes);

struct PrusaLinkPollResult {
  PrusaLinkError error = PrusaLinkError::none;
  std::optional<PrusaLinkSample> sample;
};

// Single-worker-owned session. Replacing it invalidates dialect, nonce and job
// metadata together. Callers must reject results from retired generations.
class PrusaLinkClient {
 public:
  PrusaLinkClient(PrusaLinkHttpTransport& transport, PrusaLinkMd5 md5,
                  std::function<std::uint64_t()> now_ms,
                  std::function<std::string()> random_cnonce);
  bool configure(std::string_view endpoint, PrusaLinkCredentials credentials,
                 std::uint32_t profile_id, bool allow_loopback = false);
  PrusaLinkPollResult poll(std::uint64_t deadline_ms,
                          const std::function<bool()>& cancelled,
                          bool include_metadata = true,
                          const std::function<bool()>& want_preview = {});
  const PrusaLinkIdentity& identity() const { return identity_; }
  PrusaLinkDialect dialect() const { return dialect_; }
  std::uint64_t session_revision() const { return session_revision_; }

 private:
  PrusaLinkPollResult poll_once(std::uint64_t deadline_ms,
                               const std::function<bool()>& cancelled,
                               bool include_metadata, const std::function<bool()>& want_preview);
  PrusaLinkHttpResponse get(std::string_view path, std::size_t maximum_body,
                            std::uint64_t deadline_ms,
                            const std::function<bool()>& cancelled);
  PrusaLinkHttpTransport& transport_;
  PrusaLinkMd5 md5_;
  std::function<std::uint64_t()> now_ms_;
  std::function<std::string()> random_cnonce_;
  std::string origin_;
  PrusaLinkCredentials credentials_;
  std::uint32_t profile_id_ = 0;
  PrusaLinkIdentity identity_;
  PrusaLinkDialect dialect_ = PrusaLinkDialect::undetected;
  std::optional<PrusaLinkDigestChallenge> challenge_;
  std::string cnonce_;
  std::uint32_t nonce_count_ = 0;
  std::unique_ptr<PrusaLinkSample> previous_;
  std::uint64_t session_revision_ = 0;
};

}  // namespace printdeck::platform
