#include "printdeck/platform/prusalink_client.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace printdeck::platform {
namespace {

bool uint_value(std::string_view text, unsigned& value) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool local_host(std::string_view host, bool loopback) {
  if (host.empty() || host.size() > 128) return false;
  const bool numeric = std::all_of(host.begin(), host.end(), [](char ch) {
    return (ch >= '0' && ch <= '9') || ch == '.';
  });
  if (numeric) {
    unsigned parts[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      const auto dot = host.find('.');
      if ((i == 3) != (dot == std::string_view::npos)) return false;
      const auto part = host.substr(0, dot);
      if ((part.size() > 1 && part.front() == '0') || !uint_value(part, parts[i]) || parts[i] > 255) return false;
      if (i != 3) host.remove_prefix(dot + 1);
    }
    return parts[0] == 10 || (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) ||
           (parts[0] == 192 && parts[1] == 168) || (parts[0] == 169 && parts[1] == 254) ||
           (loopback && parts[0] == 127);
  }
  if (host == "localhost") return loopback;
  if (host.starts_with("0x") && std::all_of(host.begin() + 2, host.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      })) return false;
  if (host.find('.') != std::string_view::npos && !host.ends_with(".local")) return false;
  while (!host.empty()) {
    const auto dot = host.find('.');
    const auto label = host.substr(0, dot);
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
    for (char ch : label) if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
    if (dot == std::string_view::npos) return true;
    host.remove_prefix(dot + 1);
  }
  return false;
}

PrusaLinkError status_error(const PrusaLinkHttpResponse& response) {
  if (response.error != PrusaLinkError::none) return response.error;
  if (response.status == 401 || response.status == 403) return PrusaLinkError::authorization;
  if (response.status == 429 || response.status >= 500) return PrusaLinkError::service_not_ready;
  if (response.status != 200) return PrusaLinkError::unsupported_response;
  return PrusaLinkError::none;
}

}  // namespace

std::optional<std::string> prusalink_origin(std::string_view endpoint, bool allow_loopback) {
  if (endpoint.empty() || endpoint.size() > 128) return {};
  std::string normalized(endpoint);
  for (char& ch : normalized) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  std::string_view authority(normalized);
  bool tls = false;
  if (authority.starts_with("https://")) { tls = true; authority.remove_prefix(8); }
  else if (authority.starts_with("http://")) authority.remove_prefix(7);
  if (authority.ends_with('/')) authority.remove_suffix(1);
  std::string_view host = authority;
  unsigned port = tls ? 443 : 80;
  if (const auto colon = authority.find(':'); colon != std::string_view::npos) {
    host = authority.substr(0, colon);
    if (!uint_value(authority.substr(colon + 1), port) || port == 0 || port > 65535) return {};
  }
  if (!local_host(host, allow_loopback)) return {};
  std::string result = tls ? "https://" : "http://";
  result += host;
  if (port != (tls ? 443U : 80U)) result += ':' + std::to_string(port);
  return result;
}

PrusaLinkClient::PrusaLinkClient(PrusaLinkHttpTransport& transport, PrusaLinkMd5 md5,
    std::function<std::uint64_t()> now_ms, std::function<std::string()> random_cnonce)
    : transport_(transport), md5_(std::move(md5)), now_ms_(std::move(now_ms)),
      random_cnonce_(std::move(random_cnonce)) {}

std::optional<std::string> prusalink_preview_target(std::string_view reference, std::string_view origin) {
  if (reference.starts_with(origin) && reference.size() > origin.size() && reference[origin.size()] == '/')
    reference.remove_prefix(origin.size());
  if (reference.size() > 512 || reference.find_first_of("?#\\\r\n") != std::string_view::npos ||
      (!reference.starts_with("/thumb/l/usb/") && !reference.starts_with("/thumb/s/usb/") &&
       !reference.starts_with("/api/thumbnails/"))) return {};
  std::string decoded;
  const auto nibble = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' :
      ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; };
  for (std::size_t i = 0; i < reference.size(); ++i) {
    unsigned char ch = reference[i];
    if (ch == '%') {
      if (i + 2 >= reference.size() || nibble(reference[i+1]) < 0 || nibble(reference[i+2]) < 0) return {};
      ch = nibble(reference[i+1]) * 16 + nibble(reference[i+2]);
      i += 2;
      if (ch == '/' || ch == '%' || ch == '\\') return {};
    } else if (ch == ' ') return {};
    if (ch < 32 || ch == 127 || ch == '\\') return {};
    decoded += static_cast<char>(ch);
  }
  if (decoded.find("/../") != std::string::npos || decoded.ends_with("/..") ||
      decoded.find("/./") != std::string::npos || decoded.find("//") != std::string::npos) return {};
  return std::string(reference);
}

bool prusalink_preview_png(std::string_view bytes) {
  if (bytes.size() < 33 || bytes.size() > 512 * 1024 ||
      bytes.substr(0, 8) != std::string_view("\x89PNG\r\n\x1a\n", 8) || bytes.substr(12, 4) != "IHDR") return false;
  const auto integer = [&](std::size_t at) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      value = (value << 8) | static_cast<unsigned char>(bytes[at+i]);
    }
    return value;
  };
  return integer(8) == 13 && integer(16) > 0 && integer(16) <= 512 && integer(20) > 0 && integer(20) <= 512;
}

bool PrusaLinkClient::configure(std::string_view endpoint, PrusaLinkCredentials credentials,
                              std::uint32_t profile_id, bool allow_loopback) {
  origin_.clear();
  credentials_ = {};
  identity_ = {};
  dialect_ = PrusaLinkDialect::undetected;
  challenge_.reset();
  cnonce_.clear();
  nonce_count_ = 0;
  previous_.reset();
  ++session_revision_;
  profile_id_ = profile_id;
  const auto origin = prusalink_origin(endpoint, allow_loopback);
  if (!origin || !now_ms_ || !random_cnonce_ || !md5_) return false;
  switch (credentials.mode) {
    case PrusaLinkAuthMode::api_key:
      if (!prusalink_credential_valid(credentials.api_key, 128)) return false;
      credentials.username.clear();
      credentials.password.clear();
      break;
    case PrusaLinkAuthMode::digest:
      if (!prusalink_credential_valid(credentials.username, 64) ||
          !prusalink_credential_valid(credentials.password, 128)) return false;
      credentials.api_key.clear();
      break;
    default: return false;
  }
  origin_ = *origin;
  credentials_ = std::move(credentials);
  return true;
}

PrusaLinkHttpResponse PrusaLinkClient::get(std::string_view path, std::size_t maximum_body,
    std::uint64_t deadline_ms, const std::function<bool()>& cancelled) {
  bool refreshed = false;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    if (cancelled && cancelled()) return {.error = PrusaLinkError::cancelled};
    if (now_ms_() >= deadline_ms) return {.error = PrusaLinkError::timeout};
    PrusaLinkHttpRequest request;
    request.url = origin_ + std::string(path);
    request.maximum_body = maximum_body;
    request.deadline_ms = deadline_ms;
    bool authenticated = false;
    if (credentials_.mode == PrusaLinkAuthMode::api_key) {
      request.header_name = "X-Api-Key";
      request.header_value = credentials_.api_key;
      authenticated = true;
    } else if (challenge_) {
      if (nonce_count_ == std::numeric_limits<std::uint32_t>::max()) {
        challenge_.reset();
        continue;
      }
      const auto auth = prusalink_digest_authorization(*challenge_, credentials_.username,
          credentials_.password, path, cnonce_, ++nonce_count_, md5_);
      if (!auth) return {.error = PrusaLinkError::unsupported_authentication};
      request.header_name = "Authorization";
      request.header_value = *auth;
      authenticated = true;
    }
    auto response = transport_.get(request, cancelled);
    if (cancelled && cancelled()) return {.error = PrusaLinkError::cancelled};
    if (now_ms_() >= deadline_ms) return {.error = PrusaLinkError::timeout};
    if (response.error != PrusaLinkError::none) return response;
    std::size_t header_bytes = 0;
    for (const auto& header : response.challenges) header_bytes += header.size();
    if (response.body.size() > maximum_body || header_bytes > 2048 || response.challenges.size() > 8 ||
        (!response.content_encoding.empty() && response.content_encoding != "identity"))
      return {.error = PrusaLinkError::unsupported_response};
    if (response.status != 401 || credentials_.mode != PrusaLinkAuthMode::digest) {
      if (credentials_.mode == PrusaLinkAuthMode::digest && !authenticated &&
          response.status >= 200 && response.status < 300)
        return {.error = PrusaLinkError::unsupported_authentication};
      return response;
    }
    std::optional<PrusaLinkDigestChallenge> next;
    for (const auto& header : response.challenges) {
      if (auto parsed = parse_prusalink_digest(header)) { next = std::move(parsed); break; }
    }
    if (!next) return {.error = PrusaLinkError::unsupported_authentication};
    if (authenticated && (!next->stale || refreshed)) {
      challenge_.reset();
      return {.error = PrusaLinkError::authorization};
    }
    if (authenticated) refreshed = true;
    if (challenge_ && challenge_->nonce != next->nonce) {
      // A replaced nonce may indicate a reboot, not merely expiration. Do not
      // join status and metadata across that ambiguity, even with reused IDs.
      previous_.reset();
      ++session_revision_;
    }
    challenge_ = std::move(next);
    cnonce_ = random_cnonce_();
    nonce_count_ = 0;
  }
  return {.error = PrusaLinkError::authorization};
}

PrusaLinkPollResult PrusaLinkClient::poll(std::uint64_t deadline_ms,
    const std::function<bool()>& cancelled, bool include_metadata) {
  auto result = poll_once(deadline_ms, cancelled, include_metadata);
  if (!result.sample) {
    // Reconnect is a new evidence baseline; never carry a prior job's metadata
    // through an unavailable/rebooted endpoint just because its ID is reused.
    previous_.reset();
    identity_ = {};
    dialect_ = PrusaLinkDialect::undetected;
    challenge_.reset();
    nonce_count_ = 0;
    ++session_revision_;
    return result;
  }
  auto& current = *result.sample;
  if (previous_) {
    const auto& before = previous_->snapshot.job;
    auto& job = current.snapshot.job;
    const bool terminal = job.phase == core::JobPhase::completed || job.phase == core::JobPhase::cancelled;
    const bool previous_job = before.phase == core::JobPhase::printing || before.phase == core::JobPhase::paused ||
                              before.phase == core::JobPhase::completed || before.phase == core::JobPhase::cancelled;
    const bool same_id = current.job_id && current.job_id == previous_->job_id;
    if (previous_job && ((same_id && job.phase != core::JobPhase::idle) || (terminal && !current.job_id))) {
      if (job.name.empty()) job.name = before.name;
      if (job.gcode_file.empty()) job.gcode_file = before.gcode_file;
      if (same_id) {
        if (!job.preview) job.preview = before.preview;
        if (current.preview_path.empty()) current.preview_path = previous_->preview_path;
        current.metadata_loaded |= previous_->metadata_loaded;
      }
    }
  }
  previous_ = std::make_unique<PrusaLinkSample>(current);
  return result;
}

PrusaLinkPollResult PrusaLinkClient::poll_once(std::uint64_t deadline_ms,
    const std::function<bool()>& cancelled, bool include_metadata) {
  if (origin_.empty()) return {.error = PrusaLinkError::invalid_configuration};
  if (identity_.api_version.empty()) {
    auto response = get("/api/version", 16 * 1024, deadline_ms, cancelled);
    if (auto error = status_error(response); error != PrusaLinkError::none) return {.error = error};
    const auto parsed = parse_prusalink_identity(response.body);
    if (!parsed) return {.error = PrusaLinkError::unsupported_response};
    identity_ = *parsed;
  }
  std::optional<PrusaLinkSample> next;
  if (dialect_ != PrusaLinkDialect::legacy) {
    auto response = get("/api/v1/status", 32 * 1024, deadline_ms, cancelled);
    if (dialect_ == PrusaLinkDialect::undetected && response.error == PrusaLinkError::none &&
        (response.status == 404 || response.status == 405 || response.status == 501)) {
      // Commit legacy only after both required legacy documents validate.
    } else {
      if (auto error = status_error(response); error != PrusaLinkError::none) return {.error = error};
      next = parse_prusalink_v1_status(response.body, profile_id_, now_ms_());
      if (!next) return {.error = PrusaLinkError::unsupported_response};
      dialect_ = PrusaLinkDialect::v1;
    }
  }
  if (!next) {
    auto printer = get("/api/printer", 32 * 1024, deadline_ms, cancelled);
    if (auto error = status_error(printer); error != PrusaLinkError::none) return {.error = error};
    auto job = get("/api/job", 32 * 1024, deadline_ms, cancelled);
    if (auto error = status_error(job); error != PrusaLinkError::none) return {.error = error};
    next = parse_prusalink_legacy_status(printer.body, job.body, profile_id_, now_ms_());
    if (!next) return {.error = PrusaLinkError::unsupported_response};
    dialect_ = PrusaLinkDialect::legacy;
  }
  if (previous_ && next->job_id == previous_->job_id) {
    const auto& before = previous_->snapshot.job;
    const auto& after = next->snapshot.job;
    const bool restarted = (before.phase == core::JobPhase::completed || before.phase == core::JobPhase::cancelled) &&
        (after.phase == core::JobPhase::printing || after.phase == core::JobPhase::paused || after.phase == core::JobPhase::preparing);
    if (restarted || (before.elapsed_known && after.elapsed_known && after.elapsed_seconds < before.elapsed_seconds))
      previous_.reset();
  }
  if (dialect_ == PrusaLinkDialect::v1 && include_metadata && next->job_id && next->snapshot.job.phase != core::JobPhase::idle &&
             !(previous_ && previous_->job_id == next->job_id && previous_->metadata_loaded)) {
    const auto status_revision = session_revision_;
    auto response = get("/api/v1/job", 32 * 1024, deadline_ms, cancelled);
    if (status_revision != session_revision_) return {.error = PrusaLinkError::unavailable};
    if (response.error == PrusaLinkError::cancelled) return {.error = response.error};
    if (response.error == PrusaLinkError::none && response.status == 200)
      apply_prusalink_v1_job(response.body, *next);
    if (const auto target = prusalink_preview_target(next->preview_path, origin_);
        target && next->metadata_loaded && now_ms_() + 500 < deadline_ms) {
      const auto preview_revision = session_revision_;
      auto preview = get(*target, 512 * 1024, deadline_ms, cancelled);
      if (preview.error == PrusaLinkError::none && preview.status == 200 && prusalink_preview_png(preview.body)) {
        // A job may change during a thumbnail transfer. Never attach an image
        // until fresh status confirms the same job in the same auth session.
        const auto confirmation = get("/api/v1/status", 32 * 1024, deadline_ms, cancelled);
        const auto confirmed = confirmation.error == PrusaLinkError::none && confirmation.status == 200
            ? parse_prusalink_v1_status(confirmation.body, profile_id_, now_ms_()) : std::nullopt;
        if (preview_revision == session_revision_ && confirmed && confirmed->job_id == next->job_id)
          next->snapshot.job.preview = std::make_shared<std::vector<std::uint8_t>>(preview.body.begin(), preview.body.end());
      }
      if (preview_revision != session_revision_) return {.error = PrusaLinkError::unavailable};
    }
    // Empty 204 or unavailable metadata cannot turn a valid status offline.
  }
  if (cancelled && cancelled()) return {.error = PrusaLinkError::cancelled};
  return {.sample = std::move(next)};
}

}  // namespace printdeck::platform
