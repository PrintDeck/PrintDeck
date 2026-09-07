#include "printdeck/platform/voice_model_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "model_path.h"
#include "printdeck/core/compressed_resource.hpp"
#include "zlib.h"
#include "../../generated/voice-models/model_layout.hpp"

namespace printdeck::platform {
namespace {

constexpr std::size_t kHeaderBytes = 40 + 3 * 40;
constexpr std::size_t kMaximumModelBytes = 3 * 1024 * 1024;
const std::uint8_t* mapped = nullptr;
esp_partition_mmap_handle_t mapping{};
std::uint8_t* active_image = nullptr;
srmodel_list_t model_list{};
srmodel_data_t model_data{};
std::array<char*, 3> file_names{};
std::array<char*, 3> file_data{};
std::array<int, 3> file_sizes{};
char* model_name = nullptr;
char* model_info = nullptr;
std::array<char, 256> info_text{};
srmodel_data_t* data_pointer = &model_data;

void* allocate(std::size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

std::uint32_t read_u32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         static_cast<std::uint32_t>(data[1]) << 8U |
         static_cast<std::uint32_t>(data[2]) << 16U |
         static_cast<std::uint32_t>(data[3]) << 24U;
}

bool matches_name(const std::uint8_t* input, const char* expected) {
  const auto length = std::strlen(expected);
  return length < 32 && std::memcmp(input, expected, length) == 0 &&
         input[length] == 0;
}

void release_active() {
  model_list = {};
  model_data = {};
  model_name = model_info = nullptr;
  file_names.fill(nullptr);
  file_data.fill(nullptr);
  file_sizes.fill(0);
  heap_caps_free(active_image);
  active_image = nullptr;
}

bool parse_image(const generated::VoiceModelRecord& record) {
  if (record.raw_bytes < kHeaderBytes || read_u32(active_image) != 1 ||
      !matches_name(active_image + 4, record.name) ||
      read_u32(active_image + 36) != 3) return false;
  std::size_t previous_end = kHeaderBytes;
  for (std::size_t i = 0; i < file_names.size(); ++i) {
    auto* entry = active_image + 40 + i * 40;
    if (!matches_name(entry, record.files[i])) return false;
    const std::size_t offset = read_u32(entry + 32);
    const std::size_t length = read_u32(entry + 36);
    if (offset < previous_end || offset % 16 != 0 || offset > record.raw_bytes ||
        length == 0 || length > record.raw_bytes - offset) return false;
    previous_end = offset + length;
    file_names[i] = reinterpret_cast<char*>(entry);
    file_data[i] = reinterpret_cast<char*>(active_image + offset);
    file_sizes[i] = static_cast<int>(length);
  }
  if (previous_end != record.raw_bytes ||
      static_cast<std::size_t>(file_sizes[0]) >= info_text.size()) return false;
  // The two pinned _MODEL_INFO_ files contain one information line, no comments.
  // Keep a terminated copy, as ESP-SR exposes it as a C string.
  const auto info_size = static_cast<std::size_t>(file_sizes[0]);
  std::memcpy(info_text.data(), file_data[0], info_size);
  info_text[info_size] = 0;
  for (std::size_t i = 0; i < info_size; ++i) {
    if (info_text[i] == '\n' || info_text[i] == '\r') { info_text[i] = 0; break; }
  }
  if (info_text[0] == 0 || info_text[0] == '#') return false;
  model_name = reinterpret_cast<char*>(active_image + 4);
  model_info = info_text.data();
  model_data.num = 3;
  model_data.files = file_names.data();
  model_data.data = file_data.data();
  model_data.sizes = file_sizes.data();
  model_list.num = 1;
  model_list.model_name = &model_name;
  model_list.model_info = &model_info;
  model_list.model_data = &data_pointer;
  return true;
}

}  // namespace

esp_err_t open_voice_models() {
  if (mapped != nullptr) return ESP_OK;
  const auto* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_models");
  if (partition == nullptr || partition->size < generated::kVoiceModelImageBytes)
    return ESP_ERR_NOT_FOUND;
  const void* address = nullptr;
  const esp_err_t result = esp_partition_mmap(partition, 0,
      generated::kVoiceModelImageBytes, ESP_PARTITION_MMAP_DATA, &address, &mapping);
  if (result != ESP_OK) return result;
  mapped = static_cast<const std::uint8_t*>(address);
  if (std::memcmp(mapped, generated::kVoiceModelMagic.data(),
                  generated::kVoiceModelMagic.size()) != 0) {
    close_voice_models();
    return ESP_ERR_INVALID_VERSION;
  }
  return ESP_OK;
}

bool activate_voice_model(const char* name) {
  if (mapped == nullptr || name == nullptr) return false;
  const generated::VoiceModelRecord* selected = nullptr;
  for (const auto& record : generated::kVoiceModels) {
    if (std::strcmp(name, record.name) == 0) selected = &record;
  }
  if (selected == nullptr) return false;
  // The caller has destroyed the old recognizer. Never retain two expanded
  // models or require one contiguous allocation for their combined size.
  release_active();
  const auto& record = *selected;
  if (record.raw_bytes > kMaximumModelBytes || record.offset < 8 ||
      record.offset > generated::kVoiceModelImageBytes ||
      record.packed_bytes > generated::kVoiceModelImageBytes - record.offset)
    return false;
  active_image = static_cast<std::uint8_t*>(allocate(record.raw_bytes));
  if (active_image == nullptr) return false;
  const bool valid = core::decompress_gzip_exact(mapped + record.offset,
      record.packed_bytes, active_image, record.raw_bytes, allocate, heap_caps_free) &&
      crc32(0, active_image, record.raw_bytes) == record.crc32 && parse_image(record);
  if (!valid) release_active();
  return valid;
}

void close_voice_models() {
  release_active();
  if (mapped != nullptr) esp_partition_munmap(mapping);
  mapped = nullptr;
  mapping = {};
}

srmodel_list_t* active_voice_model_list() {
  return model_list.num == 1 ? &model_list : nullptr;
}

}  // namespace printdeck::platform

// ESP-SR's coefficient reader consumes this documented structure. Only the
// AMOLED link wraps the accessors; managed ESP-SR libraries stay unchanged.
extern "C" srmodel_list_t* __wrap_get_static_srmodels() {
  return printdeck::platform::active_voice_model_list();
}
extern "C" char* __wrap_get_model_base_path() { return nullptr; }
