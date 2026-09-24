#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace printdeck::core {

// Bounded ownership shared by workers. A lease survives settings changes until
// its actual connection is closed; a new selection must wait for that lease.
class PrinterConnectionPool {
 public:
  static constexpr std::size_t kCapacity = 6;
  explicit PrinterConnectionPool(std::size_t limit) : limit_(limit > kCapacity ? kCapacity : limit) {}
  class Lease {
   public:
    Lease(Lease&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), slot_(other.slot_) {}
    Lease& operator=(Lease&&) = delete;
    Lease(const Lease&) = delete;
    ~Lease() { if (owner_) owner_->release(slot_); }
   private:
    friend class PrinterConnectionPool;
    Lease(PrinterConnectionPool& owner, std::size_t slot) : owner_(&owner), slot_(slot) {}
    PrinterConnectionPool* owner_;
    std::size_t slot_;
  };

  std::optional<Lease> acquire(std::uint32_t profile, std::string_view endpoint,
                               std::size_t admission_limit = kCapacity) {
    const std::lock_guard lock(mutex_);
    std::size_t free = limit_;
    std::size_t occupied = 0;
    for (std::size_t i = 0; i < limit_; ++i) {
      if (!slots_[i].used) { if (free == limit_) free = i; continue; }
      ++occupied;
      if ((profile && slots_[i].profile == profile) || slots_[i].endpoint == endpoint) return {};
    }
    if (free == limit_ || occupied >= admission_limit) return {};
    slots_[free] = {true, profile, std::string(endpoint)};
    return Lease(*this, free);
  }
  bool contains(std::uint32_t profile) const {
    const std::lock_guard lock(mutex_);
    for (const auto& slot : slots_) if (slot.used && slot.profile == profile) return true;
    return false;
  }
  std::size_t size() const {
    const std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& slot : slots_) count += slot.used;
    return count;
  }
 private:
  void release(std::size_t slot) {
    const std::lock_guard lock(mutex_);
    slots_[slot] = {};
  }
  struct Slot { bool used = false; std::uint32_t profile = 0; std::string endpoint; };
  std::size_t limit_;
  mutable std::mutex mutex_;
  std::array<Slot, kCapacity> slots_{};
};
}  // namespace printdeck::core
