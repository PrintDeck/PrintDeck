#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace printdeck::platform {

// One bounded PNG/BMP per printer/job identity. The caller owns flash-safe I/O.
class PrintPreviewCache {
 public:
  static constexpr std::size_t kMaximumBytes = 1024 * 1024;
  explicit PrintPreviewCache(std::string directory = "/assets/print-previews")
      : directory_(std::move(directory)) {}
  std::size_t size(std::string_view key) const;
  bool read(std::string_view key, std::span<std::uint8_t> bytes) const;
  bool write(std::string_view key, std::span<const std::uint8_t> bytes) const;
  void remove(std::string_view key) const;

 private:
  std::string path(std::string_view key) const;
  std::string directory_;
};

}  // namespace printdeck::platform
