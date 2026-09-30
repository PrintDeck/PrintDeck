#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>

namespace printdeck::platform {
// BGRA pixels. Allocation failure is a normal decode failure, never an abort.
struct PreviewPixels {
  std::unique_ptr<std::uint8_t, decltype(&std::free)> storage{nullptr, std::free};
  std::size_t bytes = 0;
  std::uint16_t width = 0, height = 0;
  const std::uint8_t* data() const { return storage.get(); }
  std::size_t size() const { return bytes; }
  bool empty() const { return !storage || !bytes; }
};
// Limits include libpng allocations, one source row and output pixels. Never
// allocates a full source bitmap. Oversized interlaced PNGs fail safely.
bool decode_bounded_preview_png(std::span<const std::uint8_t> encoded,
    PreviewPixels& output, unsigned maximum_edge, std::size_t memory_budget);
}  // namespace printdeck::platform
