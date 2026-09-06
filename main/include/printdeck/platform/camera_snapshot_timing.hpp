#pragma once

#include <algorithm>
#include <cstdint>

namespace printdeck::platform {

// A requested snapshot cadence includes HTTP and decoding time. Scheduling
// from completion would add both to every interval. When processing overruns
// the interval, skip missed slots and still leave time for core-0 services.
constexpr std::int64_t next_snapshot_poll_us(std::int64_t started_us,
                                            std::int64_t completed_us,
                                            std::int64_t interval_us) {
  constexpr std::int64_t kMinimumIdleUs = 50000;
  return std::max(started_us + std::max(interval_us, kMinimumIdleUs),
                  completed_us + kMinimumIdleUs);
}

}  // namespace printdeck::platform
