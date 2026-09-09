#include "printdeck/platform/uniformation_connection_probe.hpp"

#include "esp_random.h"
#include "esp_timer.h"
#include "printdeck/platform/uniformation_sdcp_parser.hpp"
#include "printdeck/platform/task_affinity.hpp"

namespace printdeck::platform {
namespace {
std::uint64_t now_ms() { return static_cast<std::uint64_t>(esp_timer_get_time() / 1000); }
std::string check_token() {
  unsigned char bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}
}

esp_err_t UniformationConnectionProbe::start(core::PrinterProfile profile, const NetworkService& network) {
  if (profile.protocol != core::PrinterProtocol::uniformation_sdcp) return ESP_ERR_INVALID_ARG;
  const std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.running) return ESP_ERR_INVALID_STATE;
  profile_ = std::move(profile);
  network_ = &network;
  cancelled_ = false;
  verified_until_ = 0;
  snapshot_ = {.id = check_token(), .running = true};
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "uniformation_check", 49152, this, 4,
      nullptr, kServiceCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    snapshot_.running = false;
    snapshot_.error = ElegooError::unavailable;
    profile_ = {};
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void UniformationConnectionProbe::cancel(std::string_view id) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (id != snapshot_.id) return;
  cancelled_ = true;
  snapshot_.ready = false;
  verified_until_ = 0;
  if (!snapshot_.running) profile_ = {};
}

UniformationCheckSnapshot UniformationConnectionProbe::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

bool UniformationConnectionProbe::verified(const core::PrinterProfile& profile, std::string_view id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (cancelled_ || !snapshot_.ready || id != snapshot_.id || now_ms() >= verified_until_)
    return false;
  auto expected = profile_;
  expected.serial = snapshot_.identity.serial;
  return !expected.serial.empty() && core::same_printer_connection(expected, profile);
}

void UniformationConnectionProbe::task_entry(void* context) {
  static_cast<UniformationConnectionProbe*>(context)->run();
  vTaskDeleteWithCaps(nullptr);
}

void UniformationConnectionProbe::run() {
  core::PrinterProfile profile;
  { const std::lock_guard<std::mutex> lock(mutex_); profile = profile_; }
  ElegooIdentity identity;
  const auto cancelled = [&] { return cancelled_.load() || !network_->status().station_connected; };
  const auto deadline = now_ms() + 15000;
  auto result = uniformation_sdcp_probe(profile, deadline, cancelled, &identity);
  const std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.running = false;
  snapshot_.ready = result.snapshot && result.snapshot->link == core::LinkState::online &&
                    !identity.serial.empty() && !cancelled();
  snapshot_.error = cancelled() ? ElegooError::cancelled : result.error;
  snapshot_.identity = std::move(identity);
  verified_until_ = snapshot_.ready ? now_ms() + 300000 : 0;
  if (!snapshot_.ready) profile_ = {};
}

}  // namespace printdeck::platform
