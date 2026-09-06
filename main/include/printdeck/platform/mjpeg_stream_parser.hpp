#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace printdeck::platform {

// The U1 camera stream declares each JPEG's length. Never scan image payloads
// for MIME boundaries: a JPEG may itself contain boundary-like bytes.
class MjpegStreamParser {
 public:
  explicit MjpegStreamParser(std::string boundary) : boundary_(std::move(boundary)) {}
  static constexpr std::size_t kMaximumFrameBytes = 1024U * 1024U;
  static constexpr std::size_t kMaximumHeaderBytes = 2048;

  template <typename FrameReady>
  bool feed(std::span<const std::uint8_t> bytes, FrameReady&& ready) {
    if (failed_) return false;
    while (!bytes.empty()) {
      if (remaining_ == 0) {
        headers_.push_back(static_cast<char>(bytes.front()));
        bytes = bytes.subspan(1);
        if (headers_.size() > kMaximumHeaderBytes) return fail();
        if (!headers_.ends_with("\r\n\r\n")) continue;
        if (!parse_headers()) return fail();
        headers_.clear();
        frame_.clear();
        frame_.reserve(remaining_);
      } else {
        const std::size_t count = std::min(remaining_, bytes.size());
        frame_.insert(frame_.end(), bytes.begin(), bytes.begin() + count);
        bytes = bytes.subspan(count);
        remaining_ -= count;
        if (remaining_ == 0) {
          if (frame_[0] != 0xff || frame_[1] != 0xd8 ||
              frame_[frame_.size() - 2] != 0xff || frame_.back() != 0xd9) return fail();
          ready(std::move(frame_));
        }
      }
    }
    return true;
  }

 private:
  bool fail() { failed_ = true; return false; }
  bool parse_headers() {
    std::string_view lines(headers_);
    if (lines.starts_with("\r\n")) lines.remove_prefix(2);
    const auto end = lines.find("\r\n");
    if (boundary_.empty() || boundary_.size() > 70 ||
        lines.substr(0, end) != "--" + boundary_) return false;
    lines.remove_prefix(end + 2);
    bool jpeg = false;
    bool length_seen = false;
    while (!lines.starts_with("\r\n")) {
      const auto next = lines.find("\r\n");
      if (next == std::string_view::npos) return false;
      const auto line = lines.substr(0, next);
      lines.remove_prefix(next + 2);
      const auto colon = line.find(':');
      if (colon == std::string_view::npos) return false;
      std::string key(line.substr(0, colon));
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      auto value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
      if (key == "content-type") jpeg = value == "image/jpeg";
      if (key == "content-length") {
        if (length_seen) return false;
        length_seen = true;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), remaining_);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            remaining_ < 4 || remaining_ > kMaximumFrameBytes) return false;
      }
    }
    return jpeg && length_seen;
  }
  std::string boundary_;
  std::string headers_;
  std::vector<std::uint8_t> frame_;
  std::size_t remaining_ = 0;
  bool failed_ = false;
};

}  // namespace printdeck::platform
