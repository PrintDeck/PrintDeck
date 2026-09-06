#pragma once

#include <mutex>

#include "printdeck/platform/prusalink_client.hpp"

namespace printdeck::platform {

// One bounded transaction across selected polling, verification and inactive
// probes. Acquire on a worker, never while holding settings/display locks.
std::timed_mutex& prusalink_transaction_mutex();
std::uint64_t prusalink_now_ms();
std::string prusalink_md5(std::string_view input);
std::string prusalink_random_cnonce();

class PrusaLinkEspTransport final : public PrusaLinkHttpTransport {
 public:
  PrusaLinkHttpResponse get(const PrusaLinkHttpRequest& request,
                            const std::function<bool()>& cancelled) override;
};

}  // namespace printdeck::platform
