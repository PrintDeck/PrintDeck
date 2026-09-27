#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace printdeck::core {
struct CpuLoadSnapshot {
  bool available = false;
  unsigned percent = 0;
  std::array<unsigned, 2> cores{};
};

// Interval utilisation, averaged across both cores. Unsigned subtraction also
// handles the 32-bit microsecond runtime counter wrapping during normal use.
class CpuLoadMeter {
 public:
  CpuLoadSnapshot sample(std::uint32_t now, std::array<std::uint32_t, 2> idle) {
    if (!primed_) {
      primed_ = true; previous_ = now; idle_ = idle;
      return value_;
    }
    const auto elapsed = now - previous_;
    if (elapsed < 1'000'000) return value_;
    unsigned sum = 0;
    for (unsigned core = 0; core < 2; ++core) {
      const auto idle_time = std::min(elapsed, idle[core] - idle_[core]);
      value_.cores[core] = static_cast<unsigned>(
          (static_cast<std::uint64_t>(elapsed - idle_time) * 100 + elapsed / 2) / elapsed);
      sum += value_.cores[core];
    }
    value_.available = true;
    value_.percent = (sum + 1) / 2;
    previous_ = now; idle_ = idle;
    return value_;
  }
 private:
  bool primed_ = false;
  std::uint32_t previous_ = 0;
  std::array<std::uint32_t, 2> idle_{};
  CpuLoadSnapshot value_{};
};
}  // namespace printdeck::core
