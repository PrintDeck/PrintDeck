#pragma once

#include <mutex>
#include <chrono>
#include "printdeck/core/printer_connection_pool.hpp"

#include "printdeck/platform/prusalink_client.hpp"

namespace printdeck::platform {

// Independent endpoints can proceed together; status and previews for the
// same endpoint remain serialized. One extra slot serves selected telemetry.
class PrinterTransactionLock {
 public:
  explicit PrinterTransactionLock(std::string endpoint);
  bool try_lock_for(std::chrono::milliseconds wait);
  bool owns_lock() const { return lease_.has_value(); }
  void unlock() { lease_.reset(); }
 private:
  std::string endpoint_;
  std::optional<core::PrinterConnectionPool::Lease> lease_;
};
std::uint64_t prusalink_now_ms();
std::string prusalink_resolved_ipv4(std::string host, std::uint64_t deadline,
    const std::function<bool()>& cancelled);
std::string prusalink_md5(std::string_view input);
std::string prusalink_random_cnonce();

class PrusaLinkEspTransport final : public PrusaLinkHttpTransport {
 public:
  PrusaLinkHttpResponse get(const PrusaLinkHttpRequest& request,
                            const std::function<bool()>& cancelled) override;
};

}  // namespace printdeck::platform
