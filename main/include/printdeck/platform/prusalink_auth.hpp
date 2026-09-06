#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace printdeck::platform {

struct PrusaLinkDigestChallenge {
  std::string realm;
  std::string nonce;
  std::string opaque;
  bool opaque_present = false;
  bool session_algorithm = false;
  bool qop_auth = false;
  bool stale = false;
};

// The platform supplies an existing maintained MD5 implementation. Its result
// must be 32 lowercase hexadecimal characters, or empty on failure.
using PrusaLinkMd5 = std::function<std::string(std::string_view)>;

bool prusalink_credential_valid(std::string_view value, std::size_t maximum);
std::optional<PrusaLinkDigestChallenge> parse_prusalink_digest(std::string_view header);
std::optional<std::string> prusalink_digest_authorization(
    const PrusaLinkDigestChallenge& challenge, std::string_view username,
    std::string_view password, std::string_view request_target,
    std::string_view cnonce, std::uint32_t nonce_count, const PrusaLinkMd5& md5);

}  // namespace printdeck::platform
