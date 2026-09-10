#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace printdeck::platform {
// Read one selected CTB image, never the complete job or full-resolution raster.
using CtbRead = std::function<bool(std::uint64_t, std::span<std::uint8_t>)>;
using CtbCancel = std::function<bool()>;
constexpr std::size_t kCtbImageLimit = 1024 * 1024;
struct CtbHeader {
  std::uint64_t size = 0, signature = 0;
  std::uint32_t table = 0, width = 0, height = 0, layers = 0, seed = 0;
  std::uint32_t model[2]{};
};
std::optional<std::string> ctb_preview_path(std::string_view path);
bool ctb_preview_header(const CtbRead&, std::uint64_t size, CtbHeader&);
// Output is a bounded top-down BGR24 BMP accepted by the existing image path.
std::vector<std::uint8_t> ctb_model_preview(const CtbRead&, const CtbHeader&, const CtbCancel&);
std::vector<std::uint8_t> ctb_layer_preview(const CtbRead&, const CtbHeader&, std::uint32_t index, const CtbCancel&);
bool ctb_preview_range(std::string_view header, std::uint64_t offset, std::size_t length, std::uint64_t size);
bool ctb_exposure_preview_matches(std::string_view source, std::string_view current,
    unsigned source_index, unsigned current_index, bool exposing,
    std::uint64_t status_at, std::uint64_t image_at, std::uint64_t now,
    std::uint64_t hold_until = 0);
}  // namespace printdeck::platform
