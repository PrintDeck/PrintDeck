#include "printdeck/platform/bambu_a1_preview_client.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "printdeck/platform/image_workspace.hpp"
#include "printdeck/platform/bambu_job_metadata.hpp"
#include "printdeck/core/job_name.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/idf_additions.h"
#include "printdeck/platform/bambu_trust.hpp"
#include "printdeck/platform/reset_diagnostics.hpp"

namespace printdeck::platform {
namespace {

constexpr char kTag[] = "printdeck.a1preview";
constexpr uint16_t kFtpsPort = 990;
constexpr size_t kMaximumArchivePrefixBytes = 1024U * 1024U;
constexpr size_t kMaximumPreviewBytes = 512U * 1024U;
constexpr int64_t kRetryIntervalUs = 15000000;
constexpr uint8_t kMaximumAttemptsPerJob = 4;

uint32_t decode_u32_be(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24U) |
         (static_cast<uint32_t>(bytes[1]) << 16U) |
         (static_cast<uint32_t>(bytes[2]) << 8U) |
         static_cast<uint32_t>(bytes[3]);
}

bool has_png_signature(const uint8_t* bytes, size_t size) {
  constexpr std::array<uint8_t, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  return size >= signature.size() && std::equal(signature.begin(), signature.end(), bytes);
}

std::string plate_png_name(const std::string& hint) {
  size_t marker = hint.find("plate_");
  while (marker != std::string::npos) {
    const size_t digits = marker + 6;
    size_t end = digits;
    while (end < hint.size() && std::isdigit(static_cast<unsigned char>(hint[end]))) ++end;
    if (end > digits) return "Metadata/plate_" + hint.substr(digits, end - digits) + ".png";
    marker = hint.find("plate_", digits);
  }
  return {};
}

std::string active_plate_png_from_archive(const std::vector<uint8_t>& bytes) {
  constexpr char marker[] = "Metadata/plate_";
  constexpr char gcode_suffix[] = ".gcode";
  std::string selected_plate;
  auto cursor = bytes.begin();
  while (cursor != bytes.end()) {
    cursor = std::search(cursor, bytes.end(), std::begin(marker), std::end(marker) - 1);
    if (cursor == bytes.end()) break;
    const size_t start = static_cast<size_t>(cursor - bytes.begin());
    size_t digits = start + sizeof(marker) - 1;
    size_t end = digits;
    while (end < bytes.size() && std::isdigit(static_cast<unsigned char>(bytes[end]))) ++end;
    const size_t suffix_end = end + sizeof(gcode_suffix) - 1;
    if (end > digits && suffix_end < bytes.size() &&
        std::equal(std::begin(gcode_suffix), std::end(gcode_suffix) - 1,
                   bytes.begin() + static_cast<ptrdiff_t>(end)) &&
        bytes[suffix_end] != '.') {
      const std::string plate(bytes.begin() + static_cast<ptrdiff_t>(digits),
                              bytes.begin() + static_cast<ptrdiff_t>(end));
      if (!selected_plate.empty() && selected_plate != plate) return {};
      selected_plate = plate;
    }
    cursor += sizeof(marker) - 1;
  }
  return selected_plate.empty() ? std::string{}
                                : "Metadata/plate_" + selected_plate + ".png";
}

std::shared_ptr<std::vector<uint8_t>> find_complete_png(const std::vector<uint8_t>& bytes,
                                                        const std::string& target_name) {
  constexpr std::array<uint8_t, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  auto cursor = bytes.begin();
  const std::string resolved_target =
      target_name.empty() ? active_plate_png_from_archive(bytes) : target_name;
  if (resolved_target.empty()) return {};
  if (!resolved_target.empty()) {
    cursor = std::search(bytes.begin(), bytes.end(), resolved_target.begin(),
                         resolved_target.end());
    if (cursor == bytes.end()) return {};
    cursor += static_cast<ptrdiff_t>(resolved_target.size());
  }
  while (cursor != bytes.end()) {
    cursor = std::search(cursor, bytes.end(), signature.begin(), signature.end());
    if (cursor == bytes.end()) return {};
    const size_t start = static_cast<size_t>(cursor - bytes.begin());
    size_t offset = start + signature.size();
    while (offset + 12 <= bytes.size()) {
      const uint32_t chunk_size = decode_u32_be(bytes.data() + offset);
      if (chunk_size > kMaximumPreviewBytes || offset + 12U + chunk_size > bytes.size()) break;
      const uint8_t* type = bytes.data() + offset + 4;
      offset += 12U + chunk_size;
      if (std::memcmp(type, "IEND", 4) == 0) {
        if (offset - start > kMaximumPreviewBytes) return {};
        return std::make_shared<std::vector<uint8_t>>(bytes.begin() + start,
                                                      bytes.begin() + offset);
      }
    }
    ++cursor;
  }
  return {};
}

std::string basename(std::string value) {
  while (!value.empty() && (value.back() == '/' || value.back() == '\\')) value.pop_back();
  const size_t slash = value.find_last_of("/\\");
  if (slash != std::string::npos) value.erase(0, slash + 1);
  if (value.empty() || value.size() > 192U || value == "." || value == "..") return {};
  if (std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
      })) return {};
  return value;
}

bool ends_with_case_insensitive(const std::string& value, const char* suffix) {
  const size_t suffix_size = std::strlen(suffix);
  if (value.size() < suffix_size) return false;
  const size_t start = value.size() - suffix_size;
  for (size_t i = 0; i < suffix_size; ++i) {
    if (std::tolower(static_cast<unsigned char>(value[start + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

void add_unique(std::vector<std::string>* values, std::string value) {
  if (values == nullptr || value.empty()) return;
  if (std::find(values->begin(), values->end(), value) == values->end()) {
    values->push_back(std::move(value));
  }
}

void add_archive_variants(std::vector<std::string>* names, std::string name) {
  name = basename(std::move(name));
  if (name.empty()) return;
  if (ends_with_case_insensitive(name, ".gcode.3mf")) {
    add_unique(names, std::move(name));
  } else if (ends_with_case_insensitive(name, ".3mf")) {
    add_unique(names, name);
    name.erase(name.size() - 4);
    add_unique(names, name + ".gcode.3mf");
  } else {
    add_unique(names, name + ".3mf");
    add_unique(names, name + ".gcode.3mf");
  }
}

std::vector<std::string> archive_paths(const std::string& file_hint,
                                       const std::string& job_name,
                                       const std::string& plate_hint) {
  std::vector<std::string> names;
  add_archive_variants(&names, file_hint);
  if (ends_with_case_insensitive(plate_hint, ".3mf")) add_archive_variants(&names, plate_hint);
  add_archive_variants(&names, job_name);
  std::vector<std::string> paths;
  for (const std::string& name : names) {
    add_unique(&paths, "/cache/" + name);
    add_unique(&paths, "/model/" + name);
    add_unique(&paths, "/" + name);
  }
  return paths;
}

esp_tls_cfg_t make_tls_config(const BambuLocalConnection& connection) {
  esp_tls_cfg_t config{};
  config.timeout_ms = 8000;
  config.skip_common_name = false;
  config.common_name = connection.serial.c_str();
  config.cacert_buf = reinterpret_cast<const unsigned char*>(bambu_trust_anchors());
  config.cacert_bytes = static_cast<unsigned int>(std::strlen(bambu_trust_anchors()) + 1U);
  config.addr_family = ESP_TLS_AF_INET;
  config.tls_version = ESP_TLS_VER_TLS_1_2;
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
  config.esp_tls_dyn_buf_strategy = ESP_TLS_DYN_BUF_RX_STATIC;
#endif
  return config;
}

bool tls_write_all(esp_tls_t* tls, const uint8_t* bytes, size_t size) {
  size_t written = 0;
  const int64_t deadline = esp_timer_get_time() + 5000000;
  while (written < size && esp_timer_get_time() < deadline) {
    const ssize_t count = esp_tls_conn_write(tls, bytes + written, size - written);
    if (count > 0) {
      written += static_cast<size_t>(count);
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  return written == size;
}

bool read_ftp_line(esp_tls_t* tls, std::string* line) {
  if (tls == nullptr || line == nullptr) return false;
  line->clear();
  const int64_t deadline = esp_timer_get_time() + 8000000;
  while (line->size() < 1024 && esp_timer_get_time() < deadline) {
    uint8_t byte = 0;
    const ssize_t count = esp_tls_conn_read(tls, &byte, 1);
    if (count == 1) {
      if (byte == '\n') return true;
      if (byte != '\r') line->push_back(static_cast<char>(byte));
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  return false;
}

bool read_ftp_response(esp_tls_t* tls, int* code, std::string* final_line = nullptr) {
  std::string line;
  if (!read_ftp_line(tls, &line) || line.size() < 3 ||
      !std::isdigit(static_cast<unsigned char>(line[0])) ||
      !std::isdigit(static_cast<unsigned char>(line[1])) ||
      !std::isdigit(static_cast<unsigned char>(line[2]))) {
    return false;
  }
  const int parsed = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
  if (line.size() >= 4 && line[3] == '-') {
    const std::string terminator = line.substr(0, 3) + " ";
    do {
      if (!read_ftp_line(tls, &line)) return false;
    } while (line.rfind(terminator, 0) != 0);
  }
  if (code != nullptr) *code = parsed;
  if (final_line != nullptr) *final_line = std::move(line);
  return true;
}

bool ftp_command(esp_tls_t* tls, const std::string& command, int expected_a,
                 int expected_b = -1, std::string* response = nullptr) {
  const std::string wire = command + "\r\n";
  if (!tls_write_all(tls, reinterpret_cast<const uint8_t*>(wire.data()), wire.size())) {
    return false;
  }
  int code = 0;
  const bool received = read_ftp_response(tls, &code, response);
  const bool accepted = received && (code == expected_a || code == expected_b);
  if (!accepted) ESP_LOGW(kTag, "FTPS %.4s failed (reply=%d, received=%d)", command.c_str(), code, received);
  return accepted;
}

bool parse_pasv_port(const std::string& response, uint16_t* port) {
  if (port == nullptr) return false;
  const size_t open = response.find('(');
  const size_t close = response.find(')', open == std::string::npos ? 0 : open + 1);
  if (open == std::string::npos || close == std::string::npos) return false;
  std::array<unsigned, 6> values{};
  if (std::sscanf(response.substr(open + 1, close - open - 1).c_str(), "%u,%u,%u,%u,%u,%u",
                  &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6 ||
      values[4] > 255 || values[5] > 255) return false;
  *port = static_cast<uint16_t>((values[4] << 8U) | values[5]);
  return *port != 0;
}

bool open_ftps_control(const BambuLocalConnection& connection, esp_tls_t** result) {
  if (result == nullptr) return false;
  *result = esp_tls_init();
  if (*result == nullptr) return false;
  esp_tls_cfg_t config = make_tls_config(connection);
  if (esp_tls_conn_new_sync(connection.host.c_str(), static_cast<int>(connection.host.size()),
                            kFtpsPort, &config, *result) != 1) {
    esp_tls_conn_destroy(*result);
    *result = nullptr;
    return false;
  }
  int code = 0;
  bool logged_in = false;
  if (read_ftp_response(*result, &code) && code == 220) {
    const std::string user = "USER bblp\r\n";
    if (tls_write_all(*result, reinterpret_cast<const uint8_t*>(user.data()), user.size()) &&
        read_ftp_response(*result, &code)) {
      logged_in = code == 230 ||
                  (code == 331 && ftp_command(*result, "PASS " + connection.access_code, 230));
    }
  }
  if (!logged_in || !ftp_command(*result, "PBSZ 0", 200) ||
      !ftp_command(*result, "PROT P", 200) || !ftp_command(*result, "TYPE I", 200)) {
    esp_tls_conn_destroy(*result);
    *result = nullptr;
    return false;
  }
  return true;
}

bool fetch_archive_png(const BambuLocalConnection& connection, const std::string& path,
                       const std::string& target_name,
                       std::shared_ptr<std::vector<uint8_t>>* image) {
  esp_tls_t* control = nullptr;
  if (image == nullptr || !open_ftps_control(connection, &control)) return false;
  bool success = false;
  do {
    std::string pasv;
    if (!ftp_command(control, "PASV", 227, -1, &pasv)) break;
    uint16_t data_port = 0;
    if (!parse_pasv_port(pasv, &data_port) ||
        !ftp_command(control, "RETR " + path, 125, 150)) break;

    esp_tls_t* data = esp_tls_init();
    if (data == nullptr) break;
    esp_tls_cfg_t config = make_tls_config(connection);
    if (esp_tls_conn_new_sync(connection.host.c_str(), static_cast<int>(connection.host.size()),
                              data_port, &config, data) != 1) {
      esp_tls_conn_destroy(data);
      break;
    }

    std::vector<uint8_t> prefix;
    prefix.reserve(128U * 1024U);
    std::array<uint8_t, 8192> buffer{};
    const int64_t deadline = esp_timer_get_time() + 12000000;
    while (prefix.size() < kMaximumArchivePrefixBytes && esp_timer_get_time() < deadline) {
      const size_t remaining = kMaximumArchivePrefixBytes - prefix.size();
      const ssize_t count = esp_tls_conn_read(data, buffer.data(), std::min(buffer.size(), remaining));
      if (count > 0) {
        prefix.insert(prefix.end(), buffer.begin(), buffer.begin() + count);
        auto found = find_complete_png(prefix, target_name);
        if (found && has_png_signature(found->data(), found->size())) {
          *image = std::move(found);
          success = true;
          break;
        }
      } else if (count == 0) {
        break;
      } else {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
    if (!success) ESP_LOGW(kTag, "Archive preview missing (read=%u bytes, plate hint=%d)",
                          unsigned(prefix.size()), !target_name.empty());
    esp_tls_conn_destroy(data);
  } while (false);
  esp_tls_conn_destroy(control);
  return success;
}

BambuJobMetadata fetch_metadata_prefix(const BambuLocalConnection& connection,
    const std::string& path, const std::function<bool()>& stopped) {
  using Tls = std::unique_ptr<esp_tls_t, decltype(&esp_tls_conn_destroy)>;
  esp_tls_t* initial = nullptr;
  if (stopped() || !open_ftps_control(connection, &initial)) return {};
  Tls control(initial, esp_tls_conn_destroy);
  std::string response;
  uint16_t port = 0;
  if (stopped() || !ftp_command(control.get(), "PASV", 227, -1, &response) ||
      !parse_pasv_port(response, &port) ||
      !ftp_command(control.get(), "RETR " + path, 125, 150)) return {};
  Tls data(esp_tls_init(), esp_tls_conn_destroy);
  auto config = make_tls_config(connection);
  if (stopped() || !data || esp_tls_conn_new_sync(connection.host.c_str(),
      connection.host.size(), port, &config, data.get()) != 1) return {};
  std::string prefix;
  prefix.reserve(128 * 1024);
  std::array<char, 2048> buffer{};
  while (!stopped() && prefix.size() < kMaximumArchivePrefixBytes) {
    const auto n = esp_tls_conn_read(data.get(), buffer.data(),
        std::min(buffer.size(), kMaximumArchivePrefixBytes - prefix.size()));
    if (n == 0) break;
    if (n < 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    prefix.append(buffer.data(), static_cast<std::size_t>(n));
    auto metadata = bambu_metadata::read_prefix(prefix);
    if (!metadata.title.empty()) return stopped() ? BambuJobMetadata{} : metadata;
  }
  ESP_LOGW(kTag, "Bounded Bambu metadata prefix has no complete title (read=%u)",
           unsigned(prefix.size()));
  return {};
}

BambuJobMetadata fetch_archive_metadata(const BambuLocalConnection& connection,
    const std::string& path, const std::function<bool()>& cancelled) {
  const int64_t deadline = esp_timer_get_time() + 15000000;
  const auto stopped = [&] { return cancelled() || esp_timer_get_time() >= deadline; };
  using Tls = std::unique_ptr<esp_tls_t, decltype(&esp_tls_conn_destroy)>;
  esp_tls_t* initial = nullptr;
  if (stopped() || !open_ftps_control(connection, &initial)) return {};
  Tls control(initial, esp_tls_conn_destroy);
  std::string response;
  if (!ftp_command(control.get(), "SIZE " + path, 213, -1, &response) || response.size() < 5) return {};
  std::uint64_t size = 0;
  for (std::size_t i = 4; i < response.size(); ++i) {
    const char c = response[i];
    if (c < '0' || c > '9' || size > 512ULL * 1024 * 1024 / 10) return {};
    size = size * 10 + c - '0';
  }
  if (size < 22 || size > 512ULL * 1024 * 1024 ||
      !ftp_command(control.get(), "MDTM " + path, 213, -1, &response)) return {};
  const auto modified = response;
  if (modified.size() < 18 || modified.size() > 28) return {};
  std::string cache;
  std::uint64_t cache_offset = 0;
  unsigned reads = 0;
  bool rest_unsupported = false;
  const auto reader = [&](std::uint64_t offset, std::size_t length, std::string& out) {
    if (stopped()) return false;
    if (offset >= cache_offset && offset-cache_offset <= cache.size() &&
        length <= cache.size()-(offset-cache_offset)) {
      out = cache.substr(offset-cache_offset, length);
      return true;
    }
    if (++reads > 6) return false;
    if (!control) {
      esp_tls_t* next = nullptr;
      if (!open_ftps_control(connection, &next)) return false;
      control.reset(next);
      if (!ftp_command(control.get(), "MDTM " + path, 213, -1, &response) || response != modified) return false;
    }
    if (stopped() || !ftp_command(control.get(), "PASV", 227, -1, &response)) return false;
    uint16_t port = 0;
    if (!parse_pasv_port(response, &port)) return false;
    // Offset zero is already the default. A1 firmware can reject REST entirely.
    if (offset != 0 && !ftp_command(control.get(), "REST " + std::to_string(offset),
                                   350, -1, &response)) {
      rest_unsupported = response.starts_with("500 ") || response.starts_with("502 ") ||
                         response.starts_with("504 ");
      return false;
    }
    if (!ftp_command(control.get(), "RETR " + path, 125, 150) || stopped()) return false;
    Tls data(esp_tls_init(), esp_tls_conn_destroy);
    auto config = make_tls_config(connection);
    if (!data || esp_tls_conn_new_sync(connection.host.c_str(), connection.host.size(), port, &config, data.get()) != 1) return false;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size-offset, std::max<std::size_t>(length,4096)));
    cache.assign(count, '\0');
    cache_offset = offset;
    std::size_t received = 0;
    while (received < count && !stopped()) {
      const auto n = esp_tls_conn_read(data.get(), cache.data()+received, count-received);
      if (n > 0) received += n;
      else if (n == 0) break;
      else vTaskDelay(pdMS_TO_TICKS(10));
    }
    // End this bounded RETR without leaving a partial-transfer response queued.
    data.reset(); control.reset();
    if (received != count || stopped()) { cache.clear(); return false; }
    out = cache.substr(0,length);
    return true;
  };
  auto metadata = bambu_metadata::read(size, reader);
  control.reset();
  if (metadata.title.empty() && rest_unsupported && !stopped()) {
    ESP_LOGI(kTag, "Bambu FTPS ranges unavailable; reading bounded metadata prefix");
    return fetch_metadata_prefix(connection, path, stopped);
  }
  return metadata;
}

}  // namespace

void BambuA1PreviewClient::configure(BambuLocalConnection connection) {
  if (connection.mqtt_username.empty()) connection.mqtt_username = "bblp";
  const bool configured = connection.is_ready();
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    connection_ = std::move(connection);
    connection_generation_.fetch_add(1);
  }
  reconfigure_requested_.store(true);
  fetch_requested_.store(configured);
  publish_status(configured, false,
                 configured ? "Print preview ready" : "Print preview not configured", true);
}

void BambuA1PreviewClient::set_network_ready(bool ready) { network_ready_.store(ready); }

std::string BambuA1PreviewClient::job_key(const std::string& file_hint,
    const std::string& job_name, const std::string& plate_hint, const std::string& source_job_id) {
  std::string key;
  for (const auto* part : {&file_hint, &job_name, &plate_hint, &source_job_id})
    key += std::to_string(part->size()) + ":" + *part;
  return key;
}

void BambuA1PreviewClient::set_job(std::string file_hint, std::string job_name,
                                   std::string plate_hint, bool active, std::string source_job_id) {
  const std::string key = active ? job_key(file_hint, job_name, plate_hint, source_job_id)
                                 : std::string{};
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(job_mutex_);
    changed = job_.active != active || job_.key != key;
    if (changed) {
      job_.file_hint = std::move(file_hint);
      job_.job_name = std::move(job_name);
      job_.plate_hint = std::move(plate_hint);
      job_.key = key;
      job_.active = active;
    }
  }
  if (changed) {
    fetch_requested_.store(active);
    publish_status(connection().is_ready(), false,
                   active ? "Waiting for local print preview" : "Print preview idle", true);
  }
}

esp_err_t BambuA1PreviewClient::start() {
  const std::lock_guard<std::mutex> lock(task_mutex_);
  if (running_.load(std::memory_order_acquire)) {
    return stop_requested_.load(std::memory_order_acquire) ? ESP_ERR_INVALID_STATE
                                                           : ESP_OK;
  }
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      &BambuA1PreviewClient::task_entry, "a1_preview", 20480, this, 3, &task_handle_,
      kServiceCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (created != pdPASS) {
    task_handle_ = nullptr;
    running_.store(false, std::memory_order_release);
  }
  return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void BambuA1PreviewClient::stop() {
  stop_requested_.store(true, std::memory_order_release);
  fetch_requested_.store(false, std::memory_order_release);
  TaskHandle_t task = nullptr;
  {
    const std::lock_guard<std::mutex> lock(task_mutex_);
    task = task_handle_;
  }
  if (task != nullptr) xTaskNotifyGive(task);
  publish_status(connection().is_ready(), false, "Print preview idle");
}

BambuA1PreviewSnapshot BambuA1PreviewClient::snapshot() const {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return snapshot_;
}

BambuLocalConnection BambuA1PreviewClient::connection() const {
  std::lock_guard<std::mutex> lock(config_mutex_);
  return connection_;
}

BambuA1PreviewClient::JobRequest BambuA1PreviewClient::job_request() const {
  std::lock_guard<std::mutex> lock(job_mutex_);
  return job_;
}

void BambuA1PreviewClient::publish_status(bool configured, bool fetching,
                                          const std::string& detail, bool clear_image) {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_.configured = configured;
  snapshot_.fetching = fetching;
  snapshot_.detail = detail;
  if (clear_image) {
    snapshot_.image.reset();
    snapshot_.job_key.clear();
    snapshot_.model_title.clear();
    snapshot_.profile_title.clear();
  }
}

void BambuA1PreviewClient::publish_image(
    const std::string& job_key, std::shared_ptr<std::vector<uint8_t>> image,
    std::string model_title, std::string profile_title) {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_.configured = true;
  snapshot_.fetching = false;
  snapshot_.detail = "Local print preview loaded";
  snapshot_.job_key = job_key;
  snapshot_.image = std::move(image);
  snapshot_.model_title = std::move(model_title);
  snapshot_.profile_title = std::move(profile_title);
}

bool BambuA1PreviewClient::fetch(const BambuLocalConnection& connection, const JobRequest& job,
                                 std::shared_ptr<std::vector<uint8_t>>* image,
                                 std::string* model_title, std::string* profile_title) {
  const std::vector<std::string> paths = archive_paths(job.file_hint, job.job_name, job.plate_hint);
  const std::string target_name = plate_png_name(job.plate_hint);
  if (paths.empty() || image == nullptr) return false;
  ESP_LOGI(kTag, "Fetching a bounded local Bambu print preview");
  for (const std::string& path : paths) {
    if (stop_requested_.load(std::memory_order_acquire) || !network_ready_.load() ||
        (!preview_requested_.load() && !metadata_requested_.load()) || job_request().key != job.key) break;
    const bool found_image = *image || fetch_archive_png(connection, path, target_name, image);
    if (stop_requested_.load() || job_request().key != job.key) break;
    if (model_title->empty()) {
      const auto metadata = fetch_archive_metadata(connection, path, [&] {
        return stop_requested_.load() || !network_ready_.load() ||
            (!preview_requested_.load() && !metadata_requested_.load()) || job_request().key != job.key;
      });
      *model_title = core::bounded_job_name(metadata.title);
      *profile_title = core::bounded_job_name(metadata.profile);
    }
    if (found_image || !model_title->empty()) {
      ESP_LOGI(kTag, "Loaded local Bambu job data (preview=%d, title=%d)",
               found_image, !model_title->empty());
      return true;
    }
  }
  ESP_LOGW(kTag, "Bambu print preview was not found for the active local job");
  return false;
}

void BambuA1PreviewClient::task_entry(void* context) {
  auto* preview = static_cast<BambuA1PreviewClient*>(context);
  preview->task_loop();
  {
    const std::lock_guard<std::mutex> lock(preview->task_mutex_);
    preview->task_handle_ = nullptr;
  }
  preview->running_.store(false, std::memory_order_release);
  vTaskDeleteWithCaps(nullptr);
}

void BambuA1PreviewClient::task_loop() {
  auto& attempted_job = attempted_job_;
  auto& attempts = attempts_;
  auto& last_attempt_us = last_attempt_us_;
  bool memory_deferred = false;
  while (!stop_requested_.load(std::memory_order_acquire)) {
    const auto connection_generation = connection_generation_.load();
    const BambuLocalConnection current_connection = connection();
    const JobRequest job = job_request();
    if (reconfigure_requested_.exchange(false)) {
      attempted_job.clear();
      attempts = 0;
      last_attempt_us = 0;
    }
    if ((!preview_requested_.load() && !metadata_requested_.load()) || !job.active || !current_connection.is_ready()) {
      publish_status(current_connection.is_ready(), false, "Print preview idle");
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
      continue;
    }
    if (job.key != attempted_job) {
      attempted_job = job.key;
      attempts = 0;
      last_attempt_us = 0;
      fetch_requested_.store(true);
    }
    const int64_t now_us = esp_timer_get_time();
    const bool retry_due = attempts > 0 && attempts < kMaximumAttemptsPerJob &&
                           now_us - last_attempt_us >= kRetryIntervalUs;
    if (!network_ready_.load() || (!fetch_requested_.exchange(false) && !retry_due) ||
        attempts >= kMaximumAttemptsPerJob) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
      continue;
    }
    // FTPS needs control and data TLS sessions beside the printer connection.
    // Wait for internal headroom without spending this job's bounded retries.
    const auto internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const auto internal_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal_free < 16 * 1024 || internal_block < 8 * 1024) {
      if (!memory_deferred) ESP_LOGW(kTag, "Print preview waiting for memory (free=%u, largest=%u)",
                                    unsigned(internal_free), unsigned(internal_block));
      memory_deferred = true;
      fetch_requested_.store(true);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }
    memory_deferred = false;
    ImageWorkspaceLock workspace(50);
    if (!workspace || (!preview_requested_.load() && !metadata_requested_.load())) {
      fetch_requested_.store(true);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
      continue;
    }
    ++attempts;
    ESP_LOGI(kTag, "Print preview attempt %u/%u", unsigned(attempts), unsigned(kMaximumAttemptsPerJob));
    last_attempt_us = now_us;
    publish_status(true, true, "Loading local print preview");
    mark_reset_checkpoint(ResetCheckpoint::kA1PreviewFetch);
    const auto cached = snapshot();
    const bool same_job = cached.job_key == job.key;
    auto image = same_job ? cached.image : nullptr;
    std::string model_title = same_job ? cached.model_title : "";
    std::string profile_title = same_job ? cached.profile_title : "";
    if (fetch(current_connection, job, &image, &model_title, &profile_title) &&
        !stop_requested_.load(std::memory_order_acquire) && (preview_requested_.load() || metadata_requested_.load()) &&
        connection_generation_.load() == connection_generation &&
        job_request().key == job.key) {
      const bool complete = image && !model_title.empty();
      publish_image(job.key, std::move(image), std::move(model_title), std::move(profile_title));
      // Retry only missing data. Successful cover/title reads survive worker
      // suspension and never spend another TLS session for the same job.
      if (complete) attempts = kMaximumAttemptsPerJob;
    } else if (job_request().key == job.key) {
      publish_status(true, false, attempts < kMaximumAttemptsPerJob
                                      ? "Print preview not ready; retrying"
                                      : "Print preview unavailable");
    }
    mark_reset_checkpoint(ResetCheckpoint::kRunning);
  }
  publish_status(connection().is_ready(), false, "Print preview idle");
}

}  // namespace printdeck::platform
