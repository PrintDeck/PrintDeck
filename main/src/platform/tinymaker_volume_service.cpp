#include "printdeck/platform/tinymaker_volume_service.hpp"
#include "printdeck/platform/prusalink_http_transport.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "mbedtls/sha256.h"
#include <array>
#include <chrono>
#include <cmath>

namespace printdeck::platform {
namespace {
std::string key_for(std::string_view value) {
  std::array<unsigned char, 32> hash{};
  if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash.data(), 0)) return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : hash) { out += hex[byte >> 4]; out += hex[byte & 15]; }
  return out;
}
}

void TinyMakerVolumeService::update(const core::PrinterProfile* profile,
    const core::PrinterSnapshot& state, bool suspended) {
  const std::lock_guard lock(mutex_);
  const bool valid = profile && profile->id == state.profile_id && profile->protocol == core::PrinterProtocol::tinymaker;
  const auto& stack = state.job.resin_live_slices;
  const auto uptime = stack ? stack->uptime_seconds : 0;
  const auto identity = valid ? std::to_string(profile->id) + "\n" + profile->endpoint + "\n" +
      state.job.gcode_file + "\n" + std::to_string(state.job.total_layers) + "\n" +
      std::to_string(state.job.resin_settings.layer_height_mm.value_or(0)) + "\n" +
      std::to_string(stack ? stack->revision : 0) : "";
  const bool changed = identity != identity_ || (last_uptime_ && uptime < last_uptime_) ||
      state.job.elapsed_seconds + 5 < last_elapsed_ || state.job.current_layer < last_layer_ ||
      (stack && stack->captured < source_.captured && stack->slots > 0);
  if (changed) {
    identity_ = identity;
    profile_ = valid ? profile->id : 0;
    origin_ = valid ? prusalink_origin(profile->endpoint).value_or("") : "";
    source_ = {}; source_.generation = esp_random() | 1U;
    // TinyMaker has no stable print-task identifier. Limit persisted reuse to
    // this observed job generation, never a similarly named later print.
    source_.key = key_for(identity + "\n" + std::to_string(source_.generation));
    result_ = {}; pending_ = false; attempts_ = 0; lease_ = next_ = 0;
  }
  last_elapsed_ = state.job.elapsed_seconds; last_layer_ = state.job.current_layer; last_uptime_ = uptime;
  source_.layers = state.job.total_layers;
  source_.layer_mm = state.job.resin_settings.layer_height_mm.value_or(0);
  source_.slots = stack ? stack->slots : 0; source_.captured = stack ? stack->captured : 0;
  supported_ = stack.has_value();
  online_ = valid && !origin_.empty() && state.link == core::LinkState::online &&
      (state.job.phase == core::JobPhase::printing || state.job.phase == core::JobPhase::paused ||
       state.job.phase == core::JobPhase::preparing);
  status_at_ = state.updated_at_ms; suspended_ = suspended;
  if (!online_ || suspended_) { pending_ = false; result_.bytes.reset(); lease_ = 0; }
}

TinyMakerVolumeService::Result TinyMakerVolumeService::request(std::uint32_t profile,
    bool metadata, std::uint32_t generation, unsigned since) {
  const std::lock_guard lock(mutex_);
  const auto now = prusalink_now_ms();
  if (profile != profile_ || !online_ || now < status_at_ || now - status_at_ > 10000)
    return {.status = 404, .reason = "no_job"};
  if (!supported_) return {.status = 422, .reason = "firmware"};
  if (suspended_) return {.status = 429, .reason = "busy"};
  if (!source_.layers || !std::isfinite(source_.layer_mm) || source_.layer_mm <= 0 || source_.layer_mm > 1)
    return {.status = 422, .reason = "metadata"};
  if (!metadata && generation != source_.generation) return {.status = 409, .reason = "changed"};
  if (!metadata && since > source_.captured) return {.status = 400, .reason = "range"};
  lease_ = now + 6000;
  if (metadata) { auto result = source_; result.status = 200; result.reason = "ready"; return result; }
  if (!source_.slots) return {.status = 202, .reason = "waiting"};
  if (since == since_ && result_.bytes && result_.captured == source_.captured && result_.slots == source_.slots)
    return result_;
  if (pending_) return {.status = 202, .reason = "pending"};
  if (now < next_) return result_.status == 503 && result_.reason != "waiting"
      ? result_ : Result{.status = 429, .reason = "busy"};
  if (!task_ && xTaskCreatePinnedToCoreWithCaps(entry, "tiny_volume", 12288, this, 1, &task_,
      kServiceCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    return {.status = 503, .reason = "busy"};
  if (since != since_) attempts_ = 0;
  since_ = since; pending_ = true; result_ = {};
  xTaskNotifyGive(task_);
  return {.status = 202, .reason = "pending"};
}

void TinyMakerVolumeService::entry(void* self) { static_cast<TinyMakerVolumeService*>(self)->run(); }
void TinyMakerVolumeService::run() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    std::string origin; Result source; unsigned since;
    {
      const std::lock_guard lock(mutex_);
      if (prusalink_now_ms() >= lease_) { pending_ = false; result_.bytes.reset(); }
      if (!pending_ || !online_ || suspended_) continue;
      origin = origin_; source = source_; since = since_;
    }
    const auto cancelled = [&] {
      const std::lock_guard lock(mutex_); const auto now = prusalink_now_ms();
      return source.generation != source_.generation || !online_ || suspended_ ||
          now >= lease_ || now < status_at_ || now - status_at_ > 10000;
    };
    const auto internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const auto internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const auto psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // The body and worker stack use PSRAM. Plain HTTP needs small socket
    // allocations, not a contiguous 32 KiB TLS workspace. Keep a total
    // internal reserve as well as a contiguous allocation floor; HTTPS
    // retains the larger floor for its TLS handshake.
    const bool secure = origin.starts_with("https://");
    const auto internal_floor = secure ? 32 * 1024 : 4 * 1024;
    const auto internal_budget = secure ? 32 * 1024 : 12 * 1024;
    if (internal_free < internal_budget || internal < internal_floor || psram < 256 * 1024) {
      const std::lock_guard lock(mutex_);
      if (source.generation == source_.generation) {
        pending_ = false; next_ = prusalink_now_ms() + 5000;
        ESP_LOGD("tiny_volume", "Memory wait: free=%u block=%u psram=%u",
            unsigned(internal_free), unsigned(internal), unsigned(psram));
        result_ = {.status = 503, .reason = "memory"};
      }
      continue;
    }
    PrinterTransactionLock transaction(origin);
    if (!transaction.try_lock_for(std::chrono::milliseconds(20))) continue;
    PrusaLinkEspTransport transport;
    auto response = transport.get({.url = origin + "/api/live/slices?since=" + std::to_string(since),
        .maximum_body = 131072, .deadline_ms = prusalink_now_ms() + 2500}, cancelled);
    transaction.unlock();
    if (cancelled()) continue;
    const std::lock_guard lock(mutex_);
    if (source.generation != source_.generation) continue;
    pending_ = false; next_ = prusalink_now_ms() + 750;
    const bool valid = response.error == PrusaLinkError::none && response.status == 200 &&
        (response.content_encoding.empty() || response.content_encoding == "identity") &&
        !response.body.empty() && response.body.size() <= 131072;
    if (!valid) {
      if (++attempts_ >= 3) { next_ += 30000; attempts_ = 0; }
      else next_ += 2000;
      const char* reason = response.error == PrusaLinkError::timeout ? "timeout" :
          response.error == PrusaLinkError::unsupported_response ? "response" : "transport";
      ESP_LOGW("tiny_volume", "Read failed: error=%u status=%d bytes=%u", unsigned(response.error), response.status, unsigned(response.body.size()));
      result_ = {.status = 503, .reason = reason};
      continue;
    }
    attempts_ = 0; result_ = source; result_.status = 200; result_.reason = "ready";
    result_.bytes = std::make_shared<std::string>(std::move(response.body));
  }
}
}  // namespace printdeck::platform
