#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace printdeck::platform {
// Network payload growth must report allocation failure in no-exception builds.
class BoundedResponseBuffer {
 public:
  using Reallocate = void* (*)(void*, std::size_t);
  explicit BoundedResponseBuffer(std::size_t maximum, Reallocate allocate = std::realloc)
      : maximum_(maximum), allocate_(allocate) {}
  ~BoundedResponseBuffer() { std::free(data_); }
  BoundedResponseBuffer(const BoundedResponseBuffer&) = delete;
  BoundedResponseBuffer& operator=(const BoundedResponseBuffer&) = delete;
  bool append(const char* data, std::size_t bytes) {
    if (bytes > maximum_ - size_ || (bytes && !data)) return false;
    const std::size_t needed = size_ + bytes;
    if (needed > capacity_) {
      const std::size_t capacity = std::min(maximum_, std::max(needed,
          capacity_ > maximum_ / 2 ? maximum_ : std::max<std::size_t>(2048, capacity_ * 2)));
      auto* next = static_cast<char*>(allocate_(data_, capacity));
      if (!next) return false;
      data_ = next;
      capacity_ = capacity;
    }
    if (bytes) std::memcpy(data_ + size_, data, bytes);
    size_ = needed;
    return true;
  }
  const char* data() const { return data_; }
  std::size_t size() const { return size_; }
 private:
  char* data_ = nullptr;
  std::size_t size_ = 0, capacity_ = 0, maximum_;
  Reallocate allocate_;
};
}  // namespace printdeck::platform
