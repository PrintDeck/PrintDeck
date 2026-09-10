#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace printdeck::platform {

// A late first keyframe must get time to finish instead of being discarded by
// the connection timeout. Failed streams still reconnect within a fixed bound.
constexpr bool camera_first_frame_timed_out(std::int64_t now_us,
                                            std::int64_t started_us,
                                            std::int64_t candidate_us) {
  if (started_us <= 0) return false;
  const auto normal_deadline = started_us + 30'000'000;
  const auto decode_deadline = candidate_us > 0
      ? std::min(candidate_us + 8'000'000, normal_deadline + 8'000'000)
      : normal_deadline;
  return now_us >= std::max(normal_deadline, decode_deadline);
}

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

// Learn the phase of a slow, periodically replaced stock-camera JPEG. The
// midpoint between unchanged and fresh response headers bounds observation lag;
// downloading and decoding a large JPEG must not shift the learned phase.
// HTTP 304 checks remain cheap; decoding must happen only for a new image.
class StockSnapshotCadence {
 public:
  void reset() { *this = {}; }

  void observe(bool changed, std::int64_t received_us) {
    if (!changed) {
      last_unchanged_us_ = received_us;
      return;
    }
    if (learning_started_us_ == 0 || received_us - learning_started_us_ >= 20000000) {
      // Periodically sample the boundary again, even if every scheduled read
      // finds a new image. Otherwise a faster producer can remain invisible
      // when its intermediate frames fall between the old polling slots.
      count_ = 0;
      write_index_ = 0;
      learning_started_us_ = received_us;
    }
    const bool bracketed = last_unchanged_us_ > last_changed_us_;
    const std::int64_t estimate = bracketed
        ? last_unchanged_us_ + (received_us - last_unchanged_us_) / 2
        : received_us;
    if (last_changed_us_ != 0 && bracketed) {
      const std::int64_t interval = estimate - estimated_change_us_;
      if (interval >= 500000 && interval <= 5000000) {
        intervals_[write_index_] = interval;
        write_index_ = (write_index_ + 1) % intervals_.size();
        count_ = std::min(count_ + 1, intervals_.size());
      } else {
        // A stall, missed frames or a faster replacement service invalidates
        // the old prediction. Reacquire using bounded checks.
        count_ = 0;
        write_index_ = 0;
      }
    } else if (last_changed_us_ != 0 && received_us - last_changed_us_ > 5000000) {
      // A long observation gap invalidates the phase even without a bracket.
      count_ = 0;
      write_index_ = 0;
    }
    // A slightly late request can already find a new image. Do not erase a
    // learned period merely because no unchanged check preceded that image.
    // Nor add its transfer-dependent interval as a camera-period sample.
    estimated_change_us_ = estimate;
    last_changed_us_ = received_us;
  }

  std::int64_t period_us() const {
    if (count_ < 3) return 0;
    auto sorted = intervals_;
    const auto size = std::min(count_, sorted.size());
    for (std::size_t i = 1; i < size; ++i) {
      const auto value = sorted[i];
      std::size_t j = i;
      while (j > 0 && sorted[j - 1] > value) {
        sorted[j] = sorted[j - 1];
        --j;
      }
      sorted[j] = value;
    }
    return sorted[size / 2];
  }

  std::int64_t next_check_us(std::int64_t completed_us) const {
    // At most five checks per second, including failed or unchanged requests.
    const auto earliest = completed_us + 200000;
    const auto period = period_us();
    if (period == 0) return earliest;
    return std::max(earliest, estimated_change_us_ + period - 100000);
  }

  bool stalled(std::int64_t now_us) const {
    return last_changed_us_ != 0 && now_us - last_changed_us_ >= 15000000;
  }

 private:
  std::array<std::int64_t, 8> intervals_{};
  std::size_t count_ = 0;
  std::size_t write_index_ = 0;
  std::int64_t estimated_change_us_ = 0;
  std::int64_t last_changed_us_ = 0;
  std::int64_t last_unchanged_us_ = 0;
  std::int64_t learning_started_us_ = 0;
};

}  // namespace printdeck::platform
