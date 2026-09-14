#include "printdeck/platform/embedded_resources.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
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
constexpr std::uint64_t kRetryDelayMs = 5000;
void* allocate_resource(std::size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
struct ReleaseBuffer { void operator()(std::uint8_t* data) const { heap_caps_free(data); } };
using Buffer = std::unique_ptr<std::uint8_t, ReleaseBuffer>;
Buffer latin, cjk, terminal, logos, staged_cjk;
std::atomic<bool> cjk_requested{false}, cjk_published{false};
std::uint64_t cjk_retry_after_ms = 0;
std::array<lv_image_dsc_t, kSmallLogoNames.size()> logo_descriptors{};

Buffer decode(const std::uint8_t* begin, const std::uint8_t* end, std::size_t size) {
  Buffer buffer{static_cast<std::uint8_t*>(allocate_resource(size))};
  if (!buffer || !core::decompress_gzip_exact(begin, static_cast<std::size_t>(end - begin),
                                             buffer.get(), size, allocate_resource,
                                             heap_caps_free)) return {};
  return buffer;
}

void describe_image(lv_image_dsc_t& image, std::uint32_t width, std::uint32_t height,
                    std::size_t bytes, const std::uint8_t* data) {
  image = {};
  image.header.magic = LV_IMAGE_HEADER_MAGIC;
  image.header.cf = LV_COLOR_FORMAT_I4;
  image.header.w = width;
  image.header.h = height;
  image.header.stride = width / 2;
  image.data_size = bytes;
  image.data = data;
}

#if defined(PRINTDECK_BOARD_AMOLED_1_75)
constexpr std::size_t kBootImage = kLargeLogoNames.size();
constexpr std::size_t kImageCount = kBootImage + 1;
constexpr std::size_t kNoImage = kImageCount;
static_assert(kImageCount <= 32);
static_assert(kLargeLogoBytes * kLargeLogoNames.size() == kLargeLogosBytes);
static_assert(kLargeLogosBytes + kBootLogoBytes == kAmoledImagesBytes);
// The active screen owns one brand image (FDM or resin), or the boot wordmark.
// The second slot permits overlap; one staged decode is the only extra buffer.
// These descriptors keep stable identities even after a slot is reused.
std::array<lv_image_dsc_t, kImageCount> large_descriptors{};
struct ImageSlot { std::size_t image = kNoImage; Buffer bytes; };
std::array<ImageSlot, 2> image_slots{};
struct ImageBinding {
  lv_obj_t* object = nullptr;
  std::size_t image = kNoImage;
  const void* source = nullptr;
};
std::array<ImageBinding, 4> image_bindings{};
std::atomic<std::uint32_t> requested_images{0}, resident_images{0};
std::atomic<bool> image_maintenance_dirty{false};
Buffer staged_image;
bool staged_image_needs_publication = false;
std::size_t staged_image_id = kNoImage;
std::array<std::uint64_t, kImageCount> image_retry_after_ms{};
bool boot_initialized = false;
lv_timer_t* image_lease_timer = nullptr;

void audit_image_leases(lv_timer_t* timer) {
  // Already in the LVGL task/lock: at most four pointer comparisons, without
  // allocation, decoding, cache retirement or application display-lock calls.
  bool live = false;
  for (const auto& binding : image_bindings) {
    if (!binding.object) continue;
    live = true;
    if (lv_image_get_src(binding.object) != binding.source)
      image_maintenance_dirty.store(true, std::memory_order_release);
  }
  if (!live) lv_timer_pause(timer);
}

std::size_t image_bytes(std::size_t image) {
  return image == kBootImage ? kBootLogoBytes : kLargeLogoBytes;
}

Buffer decode_image(std::size_t image) {
  const auto bytes = image_bytes(image);
  Buffer result{static_cast<std::uint8_t*>(allocate_resource(bytes))};
  const auto offset = image == kBootImage ? kLargeLogosBytes : image * kLargeLogoBytes;
  if (!result || !core::decompress_gzip_slice(large_logos_start,
        static_cast<std::size_t>(large_logos_end - large_logos_start),
        kAmoledImagesBytes, offset, result.get(), bytes, allocate_resource, heap_caps_free)) return {};
  return result;
}

void image_deleted(lv_event_t* event) {
  auto* binding = static_cast<ImageBinding*>(lv_event_get_user_data(event));
  if (binding && binding->object == lv_event_get_target_obj(event)) {
    *binding = {};
    image_maintenance_dirty.store(true, std::memory_order_release);
  }
  // Leave cache retirement to the application pass after the LVGL draw has
  // completed. The deletion callback performs neither inflate nor heap work.
}

lv_obj_t* create_image(lv_obj_t* parent, std::size_t image) {
  const auto found = std::find_if(image_bindings.begin(), image_bindings.end(),
                                  [](const auto& binding) { return !binding.object; });
  if (found == image_bindings.end()) return nullptr;
  if (!image_lease_timer) image_lease_timer = lv_timer_create(audit_image_leases, 1000, nullptr);
  if (!image_lease_timer) return nullptr;
  auto* object = lv_image_create(parent);
  if (!object) return nullptr;
  const auto* source = large_descriptors[image].data ? &large_descriptors[image] :
      image < kBootImage && logos ? &logo_descriptors[image] : nullptr;
  *found = {object, image, source};
  if (source) lv_image_set_src(object, source);
  // A small lossless brand mask is the bounded placeholder while the large
  // version loads. The final 112px/boot geometry is reserved immediately.
  lv_obj_set_size(object, image == kBootImage ? kBootLogoWidth : 112,
                          image == kBootImage ? kBootLogoHeight : 112);
  // Keep the small placeholder at its native size. The pinned partial I4
  // decoder does not safely support stretching it to the larger bounds.
  lv_image_set_inner_align(object, LV_IMAGE_ALIGN_CENTER);
  lv_obj_add_event_cb(object, image_deleted, LV_EVENT_DELETE, &*found);
  requested_images.fetch_or(std::uint32_t{1} << image, std::memory_order_release);
  image_maintenance_dirty.store(true, std::memory_order_release);
  lv_timer_resume(image_lease_timer);
  return object;
}
#endif
}  // namespace

void initialize_embedded_resources() {
  if (!latin) {
    auto next_latin = decode(latin_start, latin_end, kLatinFontBytes);
    auto next_terminal = decode(terminal_start, terminal_end, kTerminalFontBytes);
    if (next_latin && next_terminal) {
      latin = std::move(next_latin);
      terminal = std::move(next_terminal);
      ESP_LOGI(kLogTag, "Latin and terminal resources verified and decoded into PSRAM");
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
        describe_image(logo_descriptors[index], 48, 48, kSmallLogoBytes,
                        logos.get() + index * kSmallLogoBytes);
      }
    }
  }
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  // Boot is the only large image needed before the first application pass.
  // Decode just its range, still verifying every byte and CRC of the bundle.
  if (!boot_initialized) {
    auto boot = decode_image(kBootImage);
    if (boot) {
      describe_image(large_descriptors[kBootImage], kBootLogoWidth, kBootLogoHeight,
                      kBootLogoBytes, boot.get());
      image_slots[0] = {kBootImage, std::move(boot)};
      boot_initialized = true;
      resident_images.store(std::uint32_t{1} << kBootImage, std::memory_order_release);
      image_maintenance_dirty.store(true, std::memory_order_release);
    } else {
      ESP_LOGW(kLogTag, "Boot image unavailable; using text mark");
    }
  }
#endif
}

void request_embedded_cjk_glyph(std::uint32_t codepoint) {
  // Match the complete shipped cmap, including punctuation and fullwidth forms.
  // Unsupported symbols must not cause a large, pointless allocation.
  if (!cjk_published.load(std::memory_order_acquire) &&
      std::binary_search(kCjkCodepoints.begin(), kCjkCodepoints.end(), codepoint)) {
    cjk_requested.store(true, std::memory_order_release);
  }
}

bool embedded_resources_need_preparation(std::uint64_t now_ms) {
  if (!cjk_published.load(std::memory_order_acquire) && !staged_cjk &&
      cjk_requested.load(std::memory_order_acquire) && now_ms >= cjk_retry_after_ms) return true;
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  if (!staged_image) {
    const auto pending = requested_images.load(std::memory_order_acquire) &
                         ~resident_images.load(std::memory_order_acquire);
    for (std::size_t image = 0; image < kImageCount; ++image)
      if ((pending & (std::uint32_t{1} << image)) && now_ms >= image_retry_after_ms[image]) return true;
  }
#endif
  return false;
}

bool embedded_resources_need_publication() {
  if (staged_cjk) return true;
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  return staged_image_needs_publication || image_maintenance_dirty.load(std::memory_order_acquire);
#else
  return false;
#endif
}

void prepare_requested_embedded_resources(std::uint64_t now_ms) {
  if (!cjk_published.load(std::memory_order_acquire) && !staged_cjk &&
      cjk_requested.load(std::memory_order_acquire) && now_ms >= cjk_retry_after_ms) {
    staged_cjk = decode(cjk_start, cjk_end, kCjkFontBytes);
    if (!staged_cjk) {
      cjk_retry_after_ms = now_ms + kRetryDelayMs;
      ESP_LOGW(kLogTag, "CJK resource unavailable; retry deferred");
    }
  }
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  if (staged_image) return;
  const auto pending = requested_images.load(std::memory_order_acquire) &
                       ~resident_images.load(std::memory_order_acquire);
  for (std::size_t image = 0; image < kImageCount; ++image) {
    if (!(pending & (std::uint32_t{1} << image)) || now_ms < image_retry_after_ms[image]) continue;
    staged_image = decode_image(image);
    if (staged_image) {
      staged_image_id = image;
      staged_image_needs_publication = true;
    }
    else image_retry_after_ms[image] = now_ms + kRetryDelayMs;
    break;
  }
#endif
}

void publish_prepared_embedded_resources() {
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  // Consume only while holding the display lock, before any LVGL callbacks.
  // New work raised during publication must survive for the next service pass.
  image_maintenance_dirty.exchange(false, std::memory_order_acq_rel);
  staged_image_needs_publication = false;
#endif
  if (staged_cjk) {
    cjk = std::move(staged_cjk);
    cjk_published.store(true, std::memory_order_release);
    ESP_LOGI(kLogTag, "Requested CJK resource verified and decoded into PSRAM");
  }
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  std::uint32_t needed = 0;
  for (auto& binding : image_bindings) {
    if (!binding.object) continue;
    if (lv_image_get_src(binding.object) != binding.source) {
      // A caller replaced the image without deleting its object. Remove this
      // lease and its callback before this stable binding slot can be reused.
      lv_obj_remove_event_cb_with_user_data(binding.object, image_deleted, &binding);
      binding = {};
      continue;
    }
    needed |= std::uint32_t{1} << binding.image;
  }
  requested_images.store(needed, std::memory_order_release);
  if (!needed && image_lease_timer) lv_timer_pause(image_lease_timer);
  std::uint32_t resident = 0;
  for (auto& slot : image_slots) {
    if (slot.image == kNoImage) continue;
    if (!(needed & (std::uint32_t{1} << slot.image))) {
      auto& descriptor = large_descriptors[slot.image];
      lv_image_cache_drop(&descriptor);
      lv_image_header_cache_drop(&descriptor);
      descriptor.data = nullptr;
      slot = {};
    } else {
      resident |= std::uint32_t{1} << slot.image;
    }
  }
  if (staged_image && !(needed & (std::uint32_t{1} << staged_image_id))) {
    staged_image.reset();
    staged_image_id = kNoImage;
  }
  if (staged_image) {
    auto slot = std::find_if(image_slots.begin(), image_slots.end(),
                             [](const auto& item) { return item.image == kNoImage; });
    if (slot != image_slots.end()) {
      const auto id = staged_image_id;
      describe_image(large_descriptors[id], id == kBootImage ? kBootLogoWidth : 112,
                      id == kBootImage ? kBootLogoHeight : 112, image_bytes(id), staged_image.get());
      *slot = {id, std::move(staged_image)};
      resident |= std::uint32_t{1} << id;
      staged_image_id = kNoImage;
    }
  }
  resident_images.store(resident, std::memory_order_release);
  for (auto& binding : image_bindings) {
    if (!binding.object || !(resident & (std::uint32_t{1} << binding.image))) continue;
    const auto* source = &large_descriptors[binding.image];
    if (binding.source != source) {
      lv_image_set_src(binding.object, source);
      binding.source = source;
      lv_obj_invalidate(binding.object);
    }
  }
#endif
}

EmbeddedFont embedded_latin_font() { return {latin.get(), latin ? kLatinFontBytes : 0}; }
EmbeddedFont embedded_cjk_font() { return {cjk.get(), cjk ? kCjkFontBytes : 0}; }
EmbeddedFont embedded_terminal_font() { return {terminal.get(), terminal ? kTerminalFontBytes : 0}; }

const lv_image_dsc_t* embedded_small_brand_logo(std::string_view name) {
  if (!logos) return nullptr;
  for (std::size_t index = 0; index < kSmallLogoNames.size(); ++index) {
    if (name == kSmallLogoNames[index]) return &logo_descriptors[index];
  }
  return nullptr;
}

lv_obj_t* create_embedded_large_brand_logo(lv_obj_t* parent, std::string_view name) {
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  for (std::size_t index = 0; index < kLargeLogoNames.size(); ++index) {
    if (name == kLargeLogoNames[index]) return create_image(parent, index);
  }
#else
  (void)parent; (void)name;
#endif
  return nullptr;
}

lv_obj_t* create_embedded_boot_logo(lv_obj_t* parent) {
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  // Preserve the boot text fallback if initial allocation/verification failed.
  if (large_descriptors[kBootImage].data) return create_image(parent, kBootImage);
#else
  (void)parent;
#endif
  return nullptr;
}

EmbeddedResourceUsage embedded_resource_usage() {
  EmbeddedResourceUsage usage{};
  usage.cjk_bytes = cjk ? kCjkFontBytes : 0;
  usage.staged_bytes = staged_cjk ? kCjkFontBytes : 0;
#if defined(PRINTDECK_BOARD_AMOLED_1_75)
  for (const auto& slot : image_slots) {
    if (slot.bytes) usage.large_image_bytes += image_bytes(slot.image);
  }
  if (staged_image) usage.staged_bytes += image_bytes(staged_image_id);
  for (const auto& binding : image_bindings) if (binding.object) ++usage.image_leases;
#endif
  return usage;
}
}  // namespace printdeck::platform
