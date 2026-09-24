#pragma once

#include <cstdint>
#include "printdeck/core/device_state.hpp"

namespace printdeck::core {

// Upload user-visible transitions promptly, including stages within a print.
// Ordinary temperature/progress updates still use the periodic feed.
inline bool cloud_printer_state_changed(const PrinterSnapshot& previous,
                                        const PrinterSnapshot& current) {
  return previous.profile_id != current.profile_id || previous.link != current.link ||
      previous.job.phase != current.job.phase || previous.job.condition != current.job.condition ||
      previous.job.name != current.job.name || previous.job.kind != current.job.kind ||
      effective_printer_activity(previous.job) != effective_printer_activity(current.job);
}

struct CloudFeedRevision {
  std::uint32_t selected = 0;
  std::uint32_t inactive = 0;
  bool operator==(const CloudFeedRevision&) const = default;
};

// Coalesce changes into the existing snapshot exchange. A failed exchange keeps
// them pending, but must never bypass the transport's retry backoff.
class CloudFeedChanges {
 public:
  bool due(std::int64_t now, CloudFeedRevision revision, unsigned failures) const {
    return revision != acknowledged_ && failures == 0 && now >= not_before_ &&
        (!attempted_ || now - last_attempt_ >= 3000);
  }
  void attempted(std::int64_t now) { last_attempt_ = now; attempted_ = true; }
  void acknowledge(CloudFeedRevision revision) { acknowledged_ = revision; }
  void defer_until(std::int64_t deadline) { not_before_ = deadline; }

 private:
  CloudFeedRevision acknowledged_;
  std::int64_t last_attempt_ = 0;
  std::int64_t not_before_ = 0;
  bool attempted_ = false;
};

}  // namespace printdeck::core
