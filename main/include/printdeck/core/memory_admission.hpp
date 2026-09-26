#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace printdeck::core {
enum class MemoryWork { cloud, thumbnail, probe };
enum class MemoryDeferral { none, update, foreground, budget, block };

// Called under the platform lock. Credits bound cooperating operations; actual
// heap checks still protect against allocations made by drivers and other tasks.
class MemoryAdmission {
 public:
  static constexpr std::size_t credit(MemoryWork work) {
    return work == MemoryWork::cloud ? 4096 : 8192;
  }
  MemoryDeferral acquire(MemoryWork work, std::size_t free, std::size_t block, std::int64_t now) {
    if (updating) return MemoryDeferral::update;
    if (work == MemoryWork::probe && (images || now < thumbnail_until)) return MemoryDeferral::foreground;
    if (free < 8192 + reserved_ + credit(work)) return MemoryDeferral::budget;
    if (block < (work == MemoryWork::thumbnail ? 8192U : 4096U)) return MemoryDeferral::block;
    reserved_ += credit(work);
    if (work == MemoryWork::thumbnail) thumbnail_until = 0;
    return MemoryDeferral::none;
  }
  void release(MemoryWork work) { reserved_ -= credit(work); }
  void prefer_thumbnail(std::int64_t now) { thumbnail_until = now + 1500; }
  std::size_t reserved() const { return reserved_; }
  bool updating = false;
  unsigned images = 0;
  std::int64_t thumbnail_until = 0;
 private:
  std::size_t reserved_ = 0;
};
} // namespace printdeck::core
