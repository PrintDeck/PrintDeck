#include "printdeck/platform/embedded_resources.hpp"

#include <array>
#include <memory>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "printdeck/core/compressed_resource.hpp"
#include "../../generated/compressed-device-assets/resource_sizes.hpp"

extern const std::uint8_t latin_start[] asm("_binary_device_latin_ttf_gz_start");
extern const std::uint8_t latin_end[] asm("_binary_device_latin_ttf_gz_end");
extern const std::uint8_t cjk_start[] asm("_binary_device_cjk_ttf_gz_start");
extern const std::uint8_t cjk_end[] asm("_binary_device_cjk_ttf_gz_end");
extern const std::uint8_t terminal_start[] asm("_binary_device_terminal_ttf_gz_start");
extern const std::uint8_t terminal_end[] asm("_binary_device_terminal_ttf_gz_end");
extern const std::uint8_t logos_start[] asm("_binary_device_small_logos_bin_gz_start");
extern const std::uint8_t logos_end[] asm("_binary_device_small_logos_bin_gz_end");
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
extern const std::uint8_t large_logos_start[] asm("_binary_device_large_logos_bin_gz_start");
extern const std::uint8_t large_logos_end[] asm("_binary_device_large_logos_bin_gz_end");
#endif

namespace printdeck::platform {
namespace {
using namespace embedded_assets;
constexpr char kLogTag[] = "display_resources";
void* allocate_resource(std::size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
using Buffer = std::unique_ptr<std::uint8_t, decltype(&heap_caps_free)>;
Buffer latin{nullptr, heap_caps_free};
Buffer cjk{nullptr, heap_caps_free};
Buffer terminal{nullptr, heap_caps_free};
Buffer logos{nullptr, heap_caps_free};
std::array<lv_image_dsc_t, kSmallLogoNames.size()> logo_descriptors{};
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
Buffer amoled_images{nullptr, heap_caps_free};
std::array<lv_image_dsc_t, kLargeLogoNames.size()> large_logo_descriptors{};
lv_image_dsc_t boot_logo_descriptor{};
#endif

Buffer decode(const std::uint8_t* begin, const std::uint8_t* end, std::size_t size) {
  Buffer buffer{static_cast<std::uint8_t*>(allocate_resource(size)), heap_caps_free};
  if (!buffer || !core::decompress_gzip_exact(begin, static_cast<std::size_t>(end - begin),
                                             buffer.get(), size, allocate_resource,
                                             heap_caps_free)) return Buffer{nullptr, heap_caps_free};
  return buffer;
}
}  // namespace

void initialize_embedded_resources() {
  if (!latin) {
    auto next_latin = decode(latin_start, latin_end, kLatinFontBytes);
    auto next_cjk = decode(cjk_start, cjk_end, kCjkFontBytes);
    auto next_terminal = decode(terminal_start, terminal_end, kTerminalFontBytes);
    if (next_latin && next_cjk && next_terminal) {
      latin = std::move(next_latin);
      cjk = std::move(next_cjk);
      terminal = std::move(next_terminal);
      ESP_LOGI(kLogTag, "Font resources verified and decoded into PSRAM");
    } else {
      ESP_LOGW(kLogTag, "Font resources unavailable; using built-in English fonts");
    }
  }
  if (!logos) {
    logos = decode(logos_start, logos_end, kSmallLogosBytes);
    if (!logos) {
      ESP_LOGW(kLogTag, "Small brand images unavailable");
    } else {
      for (std::size_t index = 0; index < logo_descriptors.size(); ++index) {
        auto& image = logo_descriptors[index];
        image.header.magic = LV_IMAGE_HEADER_MAGIC;
        image.header.cf = LV_COLOR_FORMAT_I4;
        image.header.w = 48;
        image.header.h = 48;
        image.header.stride = 24;
        image.data_size = kSmallLogoBytes;
        image.data = logos.get() + index * kSmallLogoBytes;
      }
      ESP_LOGI(kLogTag, "Small brand images verified and decoded into PSRAM");
    }
  }
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  if (!amoled_images) {
    auto next = decode(large_logos_start, large_logos_end, kAmoledImagesBytes);
    if (!next) {
      ESP_LOGW(kLogTag, "Large brand and boot images unavailable; using text marks");
      return;
    }
    static_assert(kLargeLogoBytes * kLargeLogoNames.size() == kLargeLogosBytes);
    static_assert(kLargeLogosBytes + kBootLogoBytes == kAmoledImagesBytes);
    for (std::size_t index = 0; index < large_logo_descriptors.size(); ++index) {
      auto& image = large_logo_descriptors[index];
      image.header.magic = LV_IMAGE_HEADER_MAGIC;
      image.header.cf = LV_COLOR_FORMAT_I4;
      image.header.w = 112;
      image.header.h = 112;
      image.header.stride = 56;
      image.data_size = kLargeLogoBytes;
      image.data = next.get() + index * kLargeLogoBytes;
    }
    boot_logo_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    boot_logo_descriptor.header.cf = LV_COLOR_FORMAT_I4;
    boot_logo_descriptor.header.w = kBootLogoWidth;
    boot_logo_descriptor.header.h = kBootLogoHeight;
    boot_logo_descriptor.header.stride = kBootLogoStride;
    boot_logo_descriptor.data_size = kBootLogoBytes;
    boot_logo_descriptor.data = next.get() + kLargeLogosBytes;
    // Publish only complete, verified descriptors; their backing buffer remains
    // alive for every screen and subsequent initialization attempt.
    amoled_images = std::move(next);
    ESP_LOGI(kLogTag, "Large brand and boot images verified and decoded into PSRAM");
  }
#endif
}

EmbeddedFont embedded_latin_font() { return {latin.get(), kLatinFontBytes}; }
EmbeddedFont embedded_cjk_font() { return {cjk.get(), kCjkFontBytes}; }
EmbeddedFont embedded_terminal_font() { return {terminal.get(), kTerminalFontBytes}; }

const lv_image_dsc_t* embedded_small_brand_logo(std::string_view name) {
  if (!logos) return nullptr;
  for (std::size_t index = 0; index < kSmallLogoNames.size(); ++index) {
    if (name == kSmallLogoNames[index]) return &logo_descriptors[index];
  }
  return nullptr;
}

const lv_image_dsc_t* embedded_large_brand_logo(std::string_view name) {
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  if (!amoled_images) return nullptr;
  for (std::size_t index = 0; index < kLargeLogoNames.size(); ++index) {
    if (name == kLargeLogoNames[index]) return &large_logo_descriptors[index];
  }
#else
  (void)name;
#endif
  return nullptr;
}

const lv_image_dsc_t* embedded_boot_logo() {
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  return amoled_images ? &boot_logo_descriptor : nullptr;
#else
  return nullptr;
#endif
}
}  // namespace printdeck::platform
