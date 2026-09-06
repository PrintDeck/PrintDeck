#include "printdeck/platform/prusalink_auth.hpp"

#include <array>
#include <cstdio>
#include <map>

namespace printdeck::platform {
namespace {

bool token(unsigned char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(ch) != std::string_view::npos;
}

std::string lower(std::string_view value) {
  std::string result(value);
  for (char& ch : result) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return result;
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
  return value;
}

std::string quote(std::string_view value) {
  std::string result = "\"";
  for (char ch : value) {
    if (ch == '\\' || ch == '"') result += '\\';
    result += ch;
  }
  return result + '"';
}

bool hex_digest(std::string_view value) {
  if (value.size() != 32) return false;
  for (char ch : value) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  return true;
}

}  // namespace

bool prusalink_credential_valid(std::string_view value, std::size_t maximum) {
  if (value.empty() || value.size() > maximum) return false;
  for (unsigned char ch : value) if (ch < 0x20 || ch == 0x7f) return false;
  return true;
}

std::optional<PrusaLinkDigestChallenge> parse_prusalink_digest(std::string_view header) {
  if (header.size() > 2048) return {};
  for (unsigned char ch : header) if ((ch < 0x20 && ch != '\t') || ch == 0x7f) return {};
  header = trim(header);
  const auto space = header.find_first_of(" \t");
  if (space == std::string_view::npos || lower(header.substr(0, space)) != "digest") return {};
  header = trim(header.substr(space));
  std::map<std::string, std::string> directives;
  while (!header.empty()) {
    std::size_t end = 0;
    while (end < header.size() && token(header[end])) ++end;
    if (end == 0) return {};
    std::string key = lower(header.substr(0, end));
    header = trim(header.substr(end));
    if (header.empty() || header.front() != '=') return {};
    header = trim(header.substr(1));
    if (header.empty()) return {};
    std::string value;
    if (header.front() == '"') {
      header.remove_prefix(1);
      bool closed = false;
      while (!header.empty()) {
        char ch = header.front();
        header.remove_prefix(1);
        if (ch == '"') { closed = true; break; }
        if (ch == '\\') {
          if (header.empty()) return {};
          ch = header.front();
          header.remove_prefix(1);
        }
        value += ch;
      }
      if (!closed) return {};
    } else {
      end = 0;
      while (end < header.size() && token(header[end])) ++end;
      if (end == 0) return {};
      value = header.substr(0, end);
      header.remove_prefix(end);
    }
    if (value.size() > 512 || directives.size() >= 16 ||
        !directives.emplace(std::move(key), std::move(value)).second) return {};
    header = trim(header);
    if (header.empty()) break;
    if (header.front() != ',') return {};
    header = trim(header.substr(1));
    if (header.empty()) return {};
  }
  PrusaLinkDigestChallenge result;
  result.realm = directives["realm"];
  result.nonce = directives["nonce"];
  if (!prusalink_credential_valid(result.realm, 256) ||
      !prusalink_credential_valid(result.nonce, 512)) return {};
  if (auto found = directives.find("opaque"); found != directives.end()) {
    result.opaque_present = true;
    result.opaque = found->second;
  }
  if (auto found = directives.find("algorithm"); found != directives.end()) {
    const auto algorithm = lower(found->second);
    if (algorithm != "md5" && algorithm != "md5-sess") return {};
    result.session_algorithm = algorithm == "md5-sess";
  }
  if (auto found = directives.find("qop"); found != directives.end()) {
    std::string_view choices(found->second);
    while (!choices.empty()) {
      auto comma = choices.find(',');
      const auto choice = trim(choices.substr(0, comma));
      if (lower(choice) == "auth") result.qop_auth = true;
      if (comma == std::string_view::npos) break;
      choices.remove_prefix(comma + 1);
    }
    if (!result.qop_auth) return {};
  }
  if (auto found = directives.find("stale"); found != directives.end()) {
    const auto stale = lower(found->second);
    if (stale != "true" && stale != "false") return {};
    result.stale = stale == "true";
  }
  if (auto found = directives.find("charset"); found != directives.end() &&
      lower(found->second) != "utf-8") return {};
  if (auto found = directives.find("userhash"); found != directives.end() &&
      lower(found->second) != "false") return {};
  return result;
}

std::optional<std::string> prusalink_digest_authorization(
    const PrusaLinkDigestChallenge& challenge, std::string_view username,
    std::string_view password, std::string_view request_target,
    std::string_view cnonce, std::uint32_t nonce_count, const PrusaLinkMd5& md5) {
  if (!md5 || !prusalink_credential_valid(username, 64) ||
      !prusalink_credential_valid(password, 128) ||
      !prusalink_credential_valid(challenge.realm, 256) ||
      !prusalink_credential_valid(challenge.nonce, 512) ||
      (!challenge.opaque.empty() && !prusalink_credential_valid(challenge.opaque, 512)) ||
      request_target.empty() || request_target.front() != '/' ||
      !prusalink_credential_valid(request_target, 512) ||
      request_target.starts_with("//") || request_target.find_first_of(" #\\") != std::string_view::npos ||
      ((challenge.qop_auth || challenge.session_algorithm) &&
       (!prusalink_credential_valid(cnonce, 64) || nonce_count == 0))) return {};
  std::string a1 = md5(std::string(username) + ':' + challenge.realm + ':' + std::string(password));
  if (!hex_digest(a1)) return {};
  if (challenge.session_algorithm) a1 = md5(a1 + ':' + challenge.nonce + ':' + std::string(cnonce));
  const std::string a2 = md5("GET:" + std::string(request_target));
  if (!hex_digest(a1) || !hex_digest(a2)) return {};
  std::array<char, 9> nc{};
  std::snprintf(nc.data(), nc.size(), "%08x", static_cast<unsigned>(nonce_count));
  std::string input = a1 + ':' + challenge.nonce + ':';
  if (challenge.qop_auth) input += std::string(nc.data()) + ':' + std::string(cnonce) + ":auth:";
  const auto response = md5(input + a2);
  if (!hex_digest(response)) return {};
  std::string header = "Digest username=" + quote(username) + ", realm=" + quote(challenge.realm) +
      ", nonce=" + quote(challenge.nonce) + ", uri=" + quote(request_target) +
      ", response=" + quote(response) + ", algorithm=" + (challenge.session_algorithm ? "MD5-sess" : "MD5");
  if (challenge.opaque_present) header += ", opaque=" + quote(challenge.opaque);
  if (challenge.qop_auth) header += ", qop=auth, nc=" + std::string(nc.data());
  if (challenge.qop_auth || challenge.session_algorithm) header += ", cnonce=" + quote(cnonce);
  if (header.size() > 2048) return {};
  return header;
}

}  // namespace printdeck::platform
