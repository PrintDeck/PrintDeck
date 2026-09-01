#include "printdeck/platform/bambu_a1_camera_client.hpp"
#include "printdeck/platform/task_affinity.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/idf_additions.h"
#include "printdeck/platform/bambu_trust.hpp"

namespace printdeck::platform {
namespace {

constexpr char kTag[] = "printdeck.a1cam";
constexpr uint16_t kPort = 6000;
constexpr size_t kLoginBytes = 80;
constexpr size_t kHeaderBytes = 16;
constexpr uint32_t kMinimumJpegBytes = 4;
constexpr uint32_t kMaximumJpegBytes = 256U * 1024U;
constexpr uint16_t kOutputWidth = 400;
constexpr uint16_t kOutputHeight = 224;
constexpr int64_t kRefreshIntervalUs = 2000000;
constexpr int kFrameAttempts = 3;

uint32_t decode_u32_le(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8U) |
         (static_cast<uint32_t>(bytes[2]) << 16U) |
         (static_cast<uint32_t>(bytes[3]) << 24U);
}

void encode_u32_le(uint8_t* bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
  bytes[2] = static_cast<uint8_t>(value >> 16U);
  bytes[3] = static_cast<uint8_t>(value >> 24U);
}

std::array<uint8_t, kLoginBytes> build_login_message(const BambuLocalConnection& connection) {
  std::array<uint8_t, kLoginBytes> message{};
  encode_u32_le(message.data(), 0x40U);
  encode_u32_le(message.data() + 4, 0x3000U);

  constexpr size_t kFieldBytes = 32;
  const std::string username = connection.mqtt_username.empty() ? "bblp" : connection.mqtt_username;
  std::memcpy(message.data() + 16, username.data(), std::min(username.size(), kFieldBytes));
  std::memcpy(message.data() + 48, connection.access_code.data(),
              std::min(connection.access_code.size(), kFieldBytes));
  return message;
}

bool write_complete(esp_tls_t* tls, const uint8_t* bytes, size_t length) {
  size_t sent = 0;
  const int64_t deadline = esp_timer_get_time() + 2500000;
  while (sent < length && esp_timer_get_time() < deadline) {
    const ssize_t result = esp_tls_conn_write(tls, bytes + sent, length - sent);
    if (result > 0) {
      sent += static_cast<size_t>(result);
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return sent == length;
}

bool read_complete(esp_tls_t* tls, uint8_t* bytes, size_t length) {
  size_t received = 0;
  const int64_t deadline = esp_timer_get_time() + 4500000;
  while (received < length && esp_timer_get_time() < deadline) {
    const ssize_t result = esp_tls_conn_read(tls, bytes + received, length - received);
    if (result > 0) {
      received += static_cast<size_t>(result);
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return received == length;
}

bool is_complete_jpeg(const std::vector<uint8_t>& data) {
  return data.size() >= kMinimumJpegBytes && data[0] == 0xFFU && data[1] == 0xD8U &&
         data[data.size() - 2] == 0xFFU && data[data.size() - 1] == 0xD9U;
}

bool decode_rgb565(const std::vector<uint8_t>& jpeg,
                   std::shared_ptr<std::vector<uint8_t>>* frame, uint16_t* width,
                   uint16_t* height) {
  if (frame == nullptr || width == nullptr || height == nullptr) {
    return false;
  }

  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  config.scale.width = kOutputWidth;
  config.scale.height = kOutputHeight;

  jpeg_dec_handle_t decoder = nullptr;
  if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || decoder == nullptr) {
    return false;
  }

  jpeg_dec_io_t io{};
  jpeg_dec_header_info_t header{};
  io.inbuf = const_cast<uint8_t*>(jpeg.data());
  io.inbuf_len = static_cast<int>(jpeg.size());
  void* decoded = nullptr;
  bool success = false;

  do {
    if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK) {
      break;
    }
    int decoded_bytes = 0;
    if (jpeg_dec_get_outbuf_len(decoder, &decoded_bytes) != JPEG_ERR_OK || decoded_bytes <= 0) {
      break;
    }
    decoded = jpeg_calloc_align(static_cast<size_t>(decoded_bytes), 16);
    if (decoded == nullptr) {
      break;
    }
    io.outbuf = static_cast<uint8_t*>(decoded);
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) {
      break;
    }

    auto output = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(decoded_bytes));
    std::memcpy(output->data(), decoded, output->size());
    *frame = std::move(output);
    *width = config.scale.width == 0 ? header.width : config.scale.width;
    *height = config.scale.height == 0 ? header.height : config.scale.height;
    success = true;
  } while (false);

  if (decoded != nullptr) {
    jpeg_free_align(decoded);
  }
  jpeg_dec_close(decoder);
  return success;
}

}  // namespace

void BambuA1CameraClient::configure(BambuLocalConnection connection) {
  if (connection.mqtt_username.empty()) {
    connection.mqtt_username = "bblp";
  }
  const bool configured = connection.is_ready();
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    connection_ = std::move(connection);
  }
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_.supported = false;
  }
  reconfigure_requested_.store(true);
  refresh_requested_.store(configured);
  publish_status(configured, false, false,
                 configured ? "Camera ready" : "Camera not configured", true);
}

void BambuA1CameraClient::set_network_ready(bool ready) {
  network_ready_.store(ready);
}

void BambuA1CameraClient::set_enabled(bool enabled) {
  const bool was_enabled = enabled_.exchange(enabled);
  if (enabled && !was_enabled) {
    refresh_requested_.store(true);
  }
  if (enabled != was_enabled) {
    TaskHandle_t task = nullptr;
    {
      const std::lock_guard<std::mutex> lock(task_mutex_);
      task = task_handle_;
    }
    if (task != nullptr) xTaskNotifyGive(task);
  }
}

void BambuA1CameraClient::request_refresh() {
  refresh_requested_.store(true);
}

esp_err_t BambuA1CameraClient::start() {
  const std::lock_guard<std::mutex> lock(task_mutex_);
  if (running_.load(std::memory_order_acquire)) {
    return stop_requested_.load(std::memory_order_acquire) ? ESP_ERR_INVALID_STATE
                                                           : ESP_OK;
  }
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      &BambuA1CameraClient::task_entry, "a1_camera", 12288, this, 4, &task_handle_,
      kServiceCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (created != pdPASS) {
    task_handle_ = nullptr;
    running_.store(false, std::memory_order_release);
  }
  return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void BambuA1CameraClient::stop() {
  enabled_.store(false, std::memory_order_release);
  refresh_requested_.store(false, std::memory_order_release);
  stop_requested_.store(true, std::memory_order_release);
  TaskHandle_t task = nullptr;
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    task = task_handle_;
  }
  if (task != nullptr) xTaskNotifyGive(task);
  publish_status(connection().is_ready(), false, false, "Camera off", true);
}

BambuA1CameraSnapshot BambuA1CameraClient::snapshot() const {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return snapshot_;
}

BambuLocalConnection BambuA1CameraClient::connection() const {
  std::lock_guard<std::mutex> lock(config_mutex_);
  return connection_;
}

void BambuA1CameraClient::publish_status(bool configured, bool enabled, bool connected,
                                         const char* detail, bool clear_frame) {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_.configured = configured;
  snapshot_.enabled = enabled;
  snapshot_.connected = connected;
  snapshot_.detail = detail == nullptr ? "" : detail;
  if (clear_frame) {
    snapshot_.frame.reset();
    snapshot_.width = 0;
    snapshot_.height = 0;
  }
}

void BambuA1CameraClient::publish_frame(std::shared_ptr<std::vector<uint8_t>> frame,
                                        uint16_t width, uint16_t height) {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_.configured = true;
  snapshot_.enabled = true;
  snapshot_.supported = true;
  snapshot_.connected = true;
  snapshot_.detail = "Camera image updated";
  snapshot_.frame = std::move(frame);
  snapshot_.width = width;
  snapshot_.height = height;
}

bool BambuA1CameraClient::capture(const BambuLocalConnection& connection) {
  esp_tls_cfg_t tls_config{};
  tls_config.timeout_ms = 7000;
  tls_config.skip_common_name = true;
  tls_config.cacert_buf = reinterpret_cast<const unsigned char*>(bambu_trust_anchors());
  tls_config.cacert_bytes =
      static_cast<unsigned int>(std::strlen(bambu_trust_anchors()) + 1U);
  tls_config.addr_family = ESP_TLS_AF_INET;
  tls_config.tls_version = ESP_TLS_VER_TLS_1_2;
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
  tls_config.esp_tls_dyn_buf_strategy = ESP_TLS_DYN_BUF_RX_STATIC;
#endif

  esp_tls_t* tls = esp_tls_init();
  if (tls == nullptr) {
    return false;
  }

  bool success = false;
  do {
    if (esp_tls_conn_new_sync(connection.host.c_str(), static_cast<int>(connection.host.size()),
                              kPort, &tls_config, tls) != 1) {
      break;
    }
    const auto login = build_login_message(connection);
    if (!write_complete(tls, login.data(), login.size())) {
      break;
    }

    for (int attempt = 0; attempt < kFrameAttempts && enabled_.load(); ++attempt) {
      std::array<uint8_t, kHeaderBytes> header{};
      if (!read_complete(tls, header.data(), header.size())) {
        break;
      }
      const uint32_t size = decode_u32_le(header.data());
      const uint32_t reserved_a = decode_u32_le(header.data() + 4);
      const uint32_t marker = decode_u32_le(header.data() + 8);
      const uint32_t reserved_b = decode_u32_le(header.data() + 12);
      if (size < kMinimumJpegBytes || size > kMaximumJpegBytes || reserved_a != 0 ||
          reserved_b != 0 || marker > 1) {
        break;
      }

      std::vector<uint8_t> jpeg(size);
      if (!read_complete(tls, jpeg.data(), jpeg.size())) {
        break;
      }
      if (!is_complete_jpeg(jpeg)) {
        continue;
      }
      if (!enabled_.load()) break;

      std::shared_ptr<std::vector<uint8_t>> frame;
      uint16_t width = 0;
      uint16_t height = 0;
      if (decode_rgb565(jpeg, &frame, &width, &height) && enabled_.load()) {
        publish_frame(std::move(frame), width, height);
        success = true;
        break;
      }
    }
  } while (false);

  // Never monopolize the A1's single local camera session between snapshots.
  esp_tls_conn_destroy(tls);
  return success;
}

void BambuA1CameraClient::task_entry(void* context) {
  auto* camera = static_cast<BambuA1CameraClient*>(context);
  camera->task_loop();
  {
    const std::lock_guard<std::mutex> lock(camera->task_mutex_);
    camera->task_handle_ = nullptr;
  }
  camera->running_.store(false, std::memory_order_release);
  vTaskDeleteWithCaps(nullptr);
}

void BambuA1CameraClient::task_loop() {
  int64_t last_capture_us = 0;
  uint32_t failure_count = 0;
  while (!stop_requested_.load(std::memory_order_acquire)) {
    if (reconfigure_requested_.exchange(false)) {
      last_capture_us = 0;
      failure_count = 0;
    }

    const BambuLocalConnection configured_connection = connection();
    if (!configured_connection.is_ready()) {
      publish_status(false, false, false, "Camera not configured", true);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }
    if (!network_ready_.load()) {
      publish_status(true, enabled_.load(), false, "Camera waiting for Wi-Fi", true);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      continue;
    }
    if (!enabled_.load()) {
      publish_status(true, false, false, "Camera off", true);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
      continue;
    }

    const int64_t now_us = esp_timer_get_time();
    const bool automatically_due =
        last_capture_us == 0 || now_us - last_capture_us >= kRefreshIntervalUs;
    if (!refresh_requested_.exchange(false) && !automatically_due) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
      continue;
    }

    publish_status(true, true, false, "Loading camera image");
    const bool captured = capture(configured_connection);
    last_capture_us = esp_timer_get_time();
    if (captured) {
      failure_count = 0;
      ESP_LOGD(kTag, "A1 camera snapshot updated");
      continue;
    }
    if (!enabled_.load()) {
      continue;
    }

    ++failure_count;
    publish_status(true, true, false, "No camera detected", false);
    const uint32_t retry_ms = failure_count <= 2 ? 2000U : failure_count <= 5 ? 5000U : 10000U;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(retry_ms));
  }
  publish_status(connection().is_ready(), false, false, "Camera off", true);
}

}  // namespace printdeck::platform
