#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace printdeck::core {

// Shared immutable pixels, including aligned decoder output without a copy.
// Ownership allocation is fallible even when C++ exceptions are disabled.
class CameraFrame {
 public:
  CameraFrame() = default;
  CameraFrame(std::shared_ptr<std::vector<std::uint8_t>> pixels)
      : vector_(std::move(pixels)) {}
  CameraFrame(const CameraFrame& other) : owner_(other.owner_), vector_(other.vector_) { retain(); }
  CameraFrame(CameraFrame&& other) noexcept
      : owner_(std::exchange(other.owner_, nullptr)), vector_(std::move(other.vector_)) {}
  CameraFrame& operator=(const CameraFrame& other) {
    if (this != &other) {
      CameraFrame replacement(other);
      std::swap(owner_, replacement.owner_);
      vector_.swap(replacement.vector_);
    }
    return *this;
  }
  CameraFrame& operator=(CameraFrame&& other) noexcept {
    if (this != &other) {
      reset();
      owner_ = std::exchange(other.owner_, nullptr);
      vector_ = std::move(other.vector_);
    }
    return *this;
  }
  ~CameraFrame() { reset(); }

  // Takes ownership also on failure; release must match the decoder allocator.
  static CameraFrame adopt(std::uint8_t* data, std::size_t size,
                           void (*release)(void*)) {
    CameraFrame frame;
    if (data == nullptr) return frame;
    if (size != 0) frame.owner_ = new (std::nothrow) Owner;
    if (frame.owner_ == nullptr) {
      release(data);
      return frame;
    }
    frame.owner_->data = data;
    frame.owner_->size = size;
    frame.owner_->release = release;
    return frame;
  }

  explicit operator bool() const { return owner_ != nullptr || vector_ != nullptr; }
  const CameraFrame* operator->() const { return this; }
  const std::uint8_t* get() const { return data(); }
  const std::uint8_t* data() const { return owner_ ? owner_->data : vector_ ? vector_->data() : nullptr; }
  std::size_t size() const { return owner_ ? owner_->size : vector_ ? vector_->size() : 0; }
  bool empty() const { return size() == 0; }
  void reset() {
    vector_.reset();
    Owner* owner = std::exchange(owner_, nullptr);
    if (owner && owner->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (owner->release) owner->release(const_cast<std::uint8_t*>(owner->data));
      delete owner;
    }
  }

 private:
  struct Owner {
    std::atomic<unsigned> references{1};
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    void (*release)(void*) = nullptr;
  };
  void retain() {
    if (owner_) owner_->references.fetch_add(1, std::memory_order_relaxed);
  }
  Owner* owner_ = nullptr;
  std::shared_ptr<std::vector<std::uint8_t>> vector_;
};

}  // namespace printdeck::core
