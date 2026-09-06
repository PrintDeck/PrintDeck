#include "printdeck/platform/prusalink_discovery.hpp"

#include <array>
#include "zlib.h"

namespace printdeck::platform {
bool prusalink_discovery_identity(const PrusaLinkHttpResponse& version,
                                 const PrusaLinkHttpResponse* root,
                                 bool advertised_prusalink_service) {
  if (version.error != PrusaLinkError::none || version.body.size() > 16384) return false;
  if (version.status == 200 && (version.content_encoding.empty() || version.content_encoding == "identity"))
    return parse_prusalink_identity(version.body).has_value();
  if (version.status != 401 || version.challenges.size() > 8) return false;
  bool printer_api = false;
  bool digest_present = false;
  for (const auto& header : version.challenges) {
    if (header.size() > 2048) return false;
    const auto digest = parse_prusalink_digest(header);
    digest_present |= digest.has_value();
    printer_api |= digest && digest->realm == "Printer API";
  }
  // Standalone PrusaLink advertises this service but uses a configurable
  // Digest realm and may protect its root too. This is only a discovery
  // candidate: authenticated identity and status remain mandatory to save.
  if (advertised_prusalink_service && digest_present) return true;
  if (!root || root->error != PrusaLinkError::none || root->status != 200 ||
      root->body.size() > 65536) return false;
  if (!printer_api) return false;
  std::string html;
  if (root->content_encoding == "gzip") {
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return false;
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(root->body.data()));
    stream.avail_in = root->body.size();
    std::array<char, 2048> buffer{};
    int result = Z_OK;
    while (result == Z_OK && html.size() <= 256 * 1024) {
      stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
      stream.avail_out = buffer.size();
      result = inflate(&stream, Z_NO_FLUSH);
      html.append(buffer.data(), buffer.size() - stream.avail_out);
    }
    const bool valid = result == Z_STREAM_END && stream.avail_in == 0 && html.size() <= 256 * 1024;
    inflateEnd(&stream);
    if (!valid) return false;
  } else if (root->content_encoding.empty() || root->content_encoding == "identity") html = root->body;
  else return false;
  return html.find("<title>PrusaLink</title>") != std::string::npos &&
      html.find("id=\"telemetry-wrapper\"") != std::string::npos &&
      html.find("href=\"/#dashboard\" class=\"brand-logo\"") != std::string::npos;
}
}
