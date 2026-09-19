#include "printdeck/platform/uniformation_preview_service.hpp"
#include "printdeck/platform/ctb_preview.hpp"
#include "printdeck/platform/uniformation_sdcp_parser.hpp"
#include "printdeck/platform/image_workspace.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include <cmath>
#include "freertos/idf_additions.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace printdeck::platform {
namespace {
std::uint64_t now_ms() { return esp_timer_get_time() / 1000; }
// Stable across selection and PD restarts; never expose printer paths or endpoints.
std::string volume_cache_key(std::uint32_t profile, const std::string& job,
    const CtbHeader& header, const std::string& etag) {
  const auto identity = "volume-mask-v1\n" + std::to_string(profile) + "\n" + job + "\n" +
      std::to_string(header.size) + "\n" + std::to_string(header.signature) + "\n" + etag;
  std::array<unsigned char, 32> digest{};
  if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(identity.data()), identity.size(), digest.data(), 0)) return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string result; result.reserve(64);
  for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}
class RangeReader {
 public:
  RangeReader(std::string url, const CtbCancel& cancel) : url_(std::move(url)), cancel_(cancel), deadline_(now_ms() + 5000) {}
  ~RangeReader() { if (client_) esp_http_client_cleanup(client_); }
  bool open() {
    esp_http_client_config_t config{};
    config.url = url_.c_str(); config.timeout_ms = 350; config.buffer_size = 2048;
    config.disable_auto_redirect = true; config.method = HTTP_METHOD_HEAD;
    config.event_handler = headers; config.user_data = this;
    client_ = esp_http_client_init(&config);
    if (!client_ || !begin(HTTP_METHOD_HEAD)) return false;
    const auto length = esp_http_client_fetch_headers(client_);
    size = length > 0 ? length : 0; etag = response_etag_;
    const bool ok = esp_http_client_get_status_code(client_) == 200 && size >= 368 &&
        size <= 16ULL * 1024 * 1024 * 1024 && !bad_headers_ && !cancelled();
    esp_http_client_close(client_); return ok;
  }
  bool read(std::uint64_t offset, std::span<std::uint8_t> bytes) {
    if (bytes.empty() || offset > size || bytes.size() > size - offset ||
        bytes.size() > kCtbImageLimit || transferred_ + bytes.size() > kCtbImageLimit + 4096 || cancelled()) return false;
    const auto range = "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + bytes.size() - 1);
    esp_http_client_set_header(client_, "Range", range.c_str());
    if (!begin(HTTP_METHOD_GET)) return false;
    const auto length = esp_http_client_fetch_headers(client_);
    // Reject ignored ranges before consuming a potentially enormous CTB body.
    if (esp_http_client_get_status_code(client_) != 206 || length != std::int64_t(bytes.size()) ||
        !ctb_preview_range(content_range_, offset, bytes.size(), size) ||
        (!etag.empty() && etag != response_etag_) || bad_headers_) return false;
    std::size_t received = 0;
    while (received < bytes.size() && !cancelled()) {
      const int count = esp_http_client_read(client_, reinterpret_cast<char*>(bytes.data() + received),
          std::min<std::size_t>(8192, bytes.size() - received));
      if (count <= 0) return false;
      received += count; vTaskDelay(1);
    }
    transferred_ += received;
    const bool ok = received == bytes.size() && esp_http_client_is_complete_data_received(client_) && !cancelled();
    esp_http_client_close(client_); return ok;
  }
  bool cancelled() const { return now_ms() >= deadline_ || cancel_(); }
  std::uint64_t size = 0;
  std::string etag;
 private:
  bool begin(esp_http_client_method_t method) {
    content_range_.clear(); response_etag_.clear(); bad_headers_ = false;
    if (cancelled()) return false;
    esp_http_client_set_method(client_, method);
    esp_http_client_set_header(client_, "Connection", "close");
    return esp_http_client_open(client_, 0) == ESP_OK;
  }
  static esp_err_t headers(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    auto& self = *static_cast<RangeReader*>(event->user_data);
    std::string* target = nullptr;
    if (!strcasecmp(event->header_key, "Content-Range")) target = &self.content_range_;
    else if (!strcasecmp(event->header_key, "ETag")) target = &self.response_etag_;
    if (target) {
      const auto length = strnlen(event->header_value, 129);
      if (!target->empty() || length > 128) self.bad_headers_ = true;
      else target->assign(event->header_value, length);
    }
    return ESP_OK;
  }
  std::string url_, content_range_, response_etag_;
  CtbCancel cancel_;
  std::uint64_t deadline_;
  std::size_t transferred_ = 0;
  bool bad_headers_ = false;
  esp_http_client_handle_t client_ = nullptr;
};

std::vector<std::uint8_t> history_preview(const std::string& origin, const std::string& task, const CtbCancel& cancel) {
  if (!uniformation_valid_task_id(task) || cancel()) return {};
  const auto url = origin + "/media/emmc0/history_image/" + task + ".bmp";
  esp_http_client_config_t config{}; config.url = url.c_str(); config.timeout_ms = 350;
  config.disable_auto_redirect = true; config.buffer_size = 2048;
  auto* client = esp_http_client_init(&config); if (!client) return {};
  std::unique_ptr<std::remove_pointer_t<esp_http_client_handle_t>, decltype(&esp_http_client_cleanup)> guard(client, esp_http_client_cleanup);
  esp_http_client_set_header(client, "Connection", "close");
  const auto deadline = now_ms() + 3000;
  if (esp_http_client_open(client, 0) != ESP_OK) return {};
  const auto size = esp_http_client_fetch_headers(client);
  if (esp_http_client_get_status_code(client) != 200 || size < 54 || size > 1048576) return {};
  std::vector<std::uint8_t> bytes(size); std::size_t read = 0;
  while (read < bytes.size() && now_ms() < deadline && !cancel()) {
    const auto count = esp_http_client_read(client, reinterpret_cast<char*>(bytes.data() + read), std::min<std::size_t>(8192, bytes.size() - read));
    if (count <= 0) return {};
    read += count; vTaskDelay(1);
  }
  std::vector<std::uint8_t> pixels; std::uint16_t w = 0, h = 0;
  if (read != bytes.size() || now_ms() >= deadline || cancel() ||
      !uniformation_decode_preview_bmp(bytes, pixels, w, h)) return {};
  unsigned visible = 0;
  for (std::size_t i = 0; i < pixels.size(); i += 4)
    if (pixels[i] > 8 || pixels[i + 1] > 8 || pixels[i + 2] > 8) ++visible;
  return visible > unsigned(w) * h / 100 ? bytes : std::vector<std::uint8_t>{};
}
}

esp_err_t UniformationPreviewService::start() {
  const std::lock_guard lock(mutex_);
  if (task_) return ESP_OK;
  volume_generation_ = esp_random() | 1U;
  // Idle worker retains only its bounded PSRAM stack. No socket or image is
  // kept when this printer/view stops being selected.
  return xTaskCreatePinnedToCoreWithCaps(task_entry, "resin_preview", 12288, this, 2,
      &task_, kServiceCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
void UniformationPreviewService::hide() {
  const std::lock_guard lock(mutex_);
  request_.visible = false; model_.reset(); layer_.reset(); held_layer_.reset(); hold_until_ = 0;
  volume_work_ = {}; volume_bytes_.reset();
  if (task_) xTaskNotifyGive(task_);
}
void UniformationPreviewService::clear() {
  const std::lock_guard lock(mutex_);
  if (!request_.key.empty()) ++volume_generation_;
  volume_work_ = {}; volume_header_ = {}; volume_cache_key_.clear(); volume_bytes_.reset();
  request_ = {}; model_.reset(); layer_.reset(); result_key_.clear();
  held_layer_.reset(); hold_until_ = 0;
  if (task_) xTaskNotifyGive(task_);
}
void UniformationPreviewService::update(const std::string& address, std::uint16_t port, const std::string& path,
    std::optional<std::uint64_t> begin, core::PrinterSnapshot& state, bool model_needed, bool visible) {
  auto& job = state.job;
  Request request;
  if (state.link == core::LinkState::online && uniformation_valid_task_id(job.preview_hint) &&
      (job.phase == core::JobPhase::printing || job.phase == core::JobPhase::preparing || job.phase == core::JobPhase::paused)) {
    request.profile = state.profile_id;
    request.address = address; request.port = port; request.task = job.preview_hint;
    request.path = begin ? path : ""; request.layer = job.current_layer; request.layers = job.total_layers;
    request.key = address + ":" + std::to_string(port) + "\n" + request.task + "\n" + request.path + "\n" +
        (begin ? std::to_string(*begin) : "") + "\n" + std::to_string(request.layers);
    request.visible = visible; request.model_needed = model_needed; request.status_at = state.updated_at_ms;
    request.exposing = job.resin_stage == core::ResinStage::exposing && job.phase == core::JobPhase::printing;
    request.layer_needed = job.phase == core::JobPhase::printing && job.current_layer < job.total_layers &&
        (job.resin_stage == core::ResinStage::lowering || job.resin_stage == core::ResinStage::exposing);
  }
  const std::lock_guard lock(mutex_);
  const bool moving = job.phase == core::JobPhase::printing &&
      (job.resin_stage == core::ResinStage::lifting || job.resin_stage == core::ResinStage::lowering);
  if (request.key != request_.key || !request.visible || (!moving && !request.exposing) || request.exposing) {
    held_layer_.reset(); hold_until_ = 0;
  } else if (request_.exposing && moving && result_key_ == request.key && layer_ &&
      layer_index_ == request_.layer && request_.status_at > layer_after_) {
    // Retain exactly the just-finished exposure, even if telemetry advances
    // its current index during lifting. Never carry it into another exposure.
    held_layer_ = layer_; held_index_ = layer_index_; held_after_ = layer_after_;
    hold_until_ = state.updated_at_ms + 2000;
  }
  if (now_ms() >= hold_until_) held_layer_.reset();
  const bool changed = request.key != request_.key || request.layer != request_.layer ||
      request.layer_needed != request_.layer_needed || request.model_needed != request_.model_needed;
  if (request.key != request_.key) { ++volume_generation_; volume_work_ = {}; volume_header_ = {}; volume_cache_key_.clear(); volume_bytes_.reset(); }
  request_ = std::move(request);
  if (changed && task_) xTaskNotifyGive(task_);
  if (request_.visible && request_.key == result_key_ && now_ms() >= state.updated_at_ms && now_ms() - state.updated_at_ms <= 2000) {
    if (state.updated_at_ms > model_after_) job.preview = model_;
    // The latest status must postdate the download; never attach an old layer
    // to a newer exposure, a paused print or a status that went stale.
    if (ctb_exposure_preview_matches(result_key_, request_.key, layer_index_, request_.layer,
        job.resin_stage == core::ResinStage::exposing && request_.layer_needed,
        state.updated_at_ms, layer_after_, now_ms())) {
      job.exposure_preview = layer_;
      job.exposure_preview_until_ms = state.updated_at_ms + 2000;
    }
    else if (held_layer_ && moving && ctb_exposure_preview_matches(result_key_, request_.key,
        held_index_, request_.layer, false, state.updated_at_ms, held_after_, now_ms(), hold_until_))
      { job.exposure_preview = held_layer_;
        job.exposure_preview_until_ms = std::min(hold_until_, state.updated_at_ms + 2000); }
  }
}
UniformationPreviewService::VolumeResult UniformationPreviewService::volume(std::uint32_t profile,
    std::string_view task, std::uint32_t generation, bool metadata, unsigned index, unsigned width, unsigned height) {
  const std::lock_guard lock(mutex_);
  VolumeResult out;
  const auto now = now_ms();
  if (!request_.visible || request_.path.empty() || request_.profile != profile || request_.task != task ||
      now < request_.status_at || now - request_.status_at > 2500) return out;
  out.generation = volume_generation_; out.header = volume_header_; out.cache_key = volume_cache_key_;
  if (metadata && volume_header_.size) { out.status = 200; return out; }
  if (!metadata && (!generation || generation != volume_generation_)) { out.status = 409; return out; }
  if (!metadata && (!volume_header_.size || index >= volume_header_.layers || !width || !height ||
      width > 1536 || height > 1536 || width * height > 1048576 ||
      width > volume_header_.width || height > volume_header_.height)) { out.status = 400; return out; }
  const bool same = metadata == volume_work_.metadata && (metadata ||
      (index == volume_work_.index && width == volume_work_.width && height == volume_work_.height));
  if (same && !volume_work_.pending && volume_bytes_) { out.bytes = volume_bytes_; out.status = 200; return out; }
  if (same && volume_work_.failed) {
    if (volume_work_.attempts >= 3) { out.status = 422; return out; }
    if (now < next_volume_) return out;
  }
  if (volume_work_.pending && now < volume_work_.until && !same) { out.status = 429; return out; }
  const auto attempts = same ? volume_work_.attempts : 0;
  volume_work_ = {true, metadata, false, index, width, height, now + 6000, attempts};
  volume_bytes_.reset(); out.status = 202;
  if (task_) xTaskNotifyGive(task_);
  return out;
}
void UniformationPreviewService::task_entry(void* context) { static_cast<UniformationPreviewService*>(context)->run(); }
void UniformationPreviewService::run() {
  std::string key, etag;
  CtbHeader header;
  std::uint64_t next_model = 0, next_layer = 0;
  unsigned model_attempts = 0;
  std::optional<std::uint16_t> attempted_layer;
  while (true) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
    Request request;
    bool want_model, want_layer, want_volume;
    VolumeWork volume_work; std::uint32_t volume_generation;
    {
      const std::lock_guard lock(mutex_); request = request_;
      if (request.key != key) {
        key = request.key; etag.clear(); header = {}; model_attempts = 0; next_model = next_layer = 0; attempted_layer.reset();
        model_.reset(); layer_.reset(); result_key_ = key;
      }
      if (now_ms() >= volume_work_.until) { volume_work_.pending = false; volume_bytes_.reset(); }
      volume_work = volume_work_; volume_generation = volume_generation_;
      want_volume = volume_work.pending && now_ms() >= next_volume_;
      want_model = request.model_needed && !model_ && model_attempts < 3 && now_ms() >= next_model;
      want_layer = request.layer_needed && !request.path.empty() && attempted_layer != request.layer && now_ms() >= next_layer;
    }
    if (!request.visible || (!want_model && !want_layer && !want_volume)) continue;
    // Fetch the model first; later requests read at most one current layer.
    const bool model = want_model;
    const bool volume = !want_model && !want_layer && want_volume;
    const auto cancel = [&] {
      const std::lock_guard lock(mutex_);
      return !request_.visible || request_.key != request.key || now_ms() < request_.status_at ||
          now_ms() - request_.status_at > 2500 ||
          (volume ? (!volume_work_.pending || now_ms() >= volume_work_.until || volume_generation_ != volume_generation ||
              volume_work_.metadata != volume_work.metadata || volume_work_.index != volume_work.index ||
              volume_work_.width != volume_work.width || volume_work_.height != volume_work.height)
                  : (!model && (!request_.layer_needed || request_.layer != request.layer)));
    };
    if (cancel()) continue;
    ImageWorkspaceLock workspace(20);
    if (!workspace || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < kCtbImageLimit + 512 * 1024) continue;
    if (model) { ++model_attempts; next_model = now_ms() + 30000; }
    else if (!volume) { attempted_layer = request.layer; next_layer = now_ms() + 5000; }
    const auto origin = "http://" + request.address + ":" + std::to_string(request.port);
    std::vector<std::uint8_t> image;
    bool volume_header_ok = false;
    if (!request.path.empty()) {
      RangeReader reader(origin + request.path, cancel);
      if (reader.open()) {
        const CtbRead read = [&](std::uint64_t offset, std::span<std::uint8_t> bytes) { return reader.read(offset, bytes); };
        if (!header.size || header.size != reader.size || etag.empty() || etag != reader.etag) {
          header = {}; if (ctb_preview_header(read, reader.size, header)) etag = reader.etag;
        }
        if (header.size && header.layers == request.layers) {
          auto yielded = now_ms();
          const auto stop = [&] {
            if (now_ms() - yielded >= 20) { vTaskDelay(1); yielded = now_ms(); }
            return reader.cancelled();
          };
          if (volume) {
            volume_header_ok = std::isfinite(header.layer_mm) && header.layer_mm > 0 && header.layer_mm <= 1;
            for (const auto mm : header.size_mm) volume_header_ok &= std::isfinite(mm) && mm > 0 && mm <= 2000;
            if (volume_header_ok && !volume_work.metadata)
              image = ctb_layer_mask(read, header, volume_work.index, volume_work.width, volume_work.height, stop);
          } else image = model ? ctb_model_preview(read, header, stop) : ctb_layer_preview(read, header, request.layer, stop);
        }
      }
    }
    if (model && image.empty() && !cancel()) image = history_preview(origin, request.task, cancel);
    if (cancel()) continue;
    if (volume) {
      const std::lock_guard lock(mutex_);
      if (request_.key == request.key && volume_generation_ == volume_generation) {
        next_volume_ = now_ms() + 750;
        volume_work_.pending = false;
        volume_work_.failed = !volume_header_ok || (!volume_work.metadata && image.empty());
        if (volume_work_.failed) { ++volume_work_.attempts; next_volume_ = now_ms() + 5000; }
        if (!volume_work_.failed) {
          volume_header_ = header;
          volume_cache_key_ = volume_cache_key(request.profile, request.key, header, etag);
          if (!volume_work.metadata) volume_bytes_ = std::make_shared<std::vector<std::uint8_t>>(std::move(image));
        }
      }
      continue;
    }
    if (image.empty()) { ESP_LOGD("resin_preview", "Selected %s preview unavailable", model ? "model" : "layer"); continue; }
    const auto finished = now_ms();
    auto result = std::make_shared<std::vector<std::uint8_t>>(std::move(image));
    const std::lock_guard lock(mutex_);
    if (request_.visible && request_.key == request.key && (model || (request_.layer == request.layer && request_.layer_needed))) {
      if (model) { model_ = std::move(result); model_after_ = finished; }
      else { layer_ = std::move(result); layer_index_ = request.layer; layer_after_ = finished; }
      ESP_LOGI("resin_preview", "%s preview ready%s", model ? "Model" : "Layer", model ? "" : " for current index");
      if (model) xTaskNotifyGive(task_);
    }
  }
}
}  // namespace printdeck::platform
