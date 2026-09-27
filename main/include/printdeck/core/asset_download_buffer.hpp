#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace printdeck::core {

// Bounded download storage. Complete files retained here do not need temporary
// disk space; remaining files stream to disk. Allocation failure degrades safely.
class AssetDownloadBuffer {
 public:
  using Allocate = void* (*)(std::size_t);
  using Free = void (*)(void*);
  AssetDownloadBuffer() = default;
  AssetDownloadBuffer(const AssetDownloadBuffer&) = delete;
  AssetDownloadBuffer& operator=(const AssetDownloadBuffer&) = delete;
  ~AssetDownloadBuffer() { reset(); }

  void prepare(std::size_t total, std::size_t smallest_file,
               std::size_t free_bytes, std::size_t largest_block,
               std::size_t reserve, Allocate allocate, Free release) {
    reset();
    if (!allocate || !release || !total || !smallest_file ||
        smallest_file > total || free_bytes < reserve) return;
    const auto available = free_bytes - reserve;
    auto candidate = std::min({total, available, largest_block});
    while (candidate >= smallest_file) {
      data_ = static_cast<std::uint8_t*>(allocate(candidate));
      if (data_) {
        size_ = candidate;
        release_ = release;
        whole_set_ = size_ == total;
        break;
      }
      if (candidate == smallest_file) break;
      candidate = std::max(smallest_file, candidate / 2);
    }
  }

  std::span<std::uint8_t> file(std::size_t offset, std::size_t bytes) const {
    if (!data_ || offset > size_ || bytes > size_ - offset) return {};
    return {data_ + offset, bytes};
  }
  bool whole_set() const { return whole_set_; }
  std::size_t size() const { return size_; }
  void reset() {
    if (data_) release_(data_);
    data_ = nullptr;
    size_ = 0;
    whole_set_ = false;
    release_ = nullptr;
  }

 private:
  std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  bool whole_set_ = false;
  Free release_ = nullptr;
};

}  // namespace printdeck::core
