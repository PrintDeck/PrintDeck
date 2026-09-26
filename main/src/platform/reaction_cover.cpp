#include "printdeck/platform/reaction_cover.hpp"
#include "printdeck/core/compressed_resource.hpp"
#include "esp_heap_caps.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <atomic>
#include <memory>

namespace printdeck::platform {
namespace {
struct CoverSource { const char* id; const std::uint8_t* data; std::size_t size; };
#include "../../generated/reaction-catalog/reaction_device_previews.inc"
constexpr std::size_t kCoverBytes = 64 + kReactionCoverSize * kReactionCoverSize / 2;
struct FreeBuffer { void operator()(std::uint8_t* p) const { heap_caps_free(p); } };
using Buffer = std::unique_ptr<std::uint8_t, FreeBuffer>;
void* allocate(std::size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
std::atomic<int> requested{-1};
std::atomic<bool> changed{false};
int resident_id = -1, staged_id = -1;
std::uint64_t retry_after_ms = 0;
Buffer resident, staged;
lv_image_dsc_t descriptor{};
lv_obj_t* target = nullptr;  // Accessed only under the display lock.
}

lv_obj_t* create_reaction_cover(lv_obj_t* parent, std::string_view id) {
  if (target) return nullptr;
  if (id == "family:iris_palette" || id.starts_with("alloy_iris_")) id = "alloy_iris_green";
  int index = 0;
  for (const auto& cover : kReactionCovers) {
    if (id == cover.id) {
      target = lv_image_create(parent);
      lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(target, kReactionCoverSize, kReactionCoverSize);
      lv_obj_add_event_cb(target, [](lv_event_t* event) {
        if (target == lv_event_get_target_obj(event)) {
          target = nullptr; requested.store(-1); changed.store(true);
        }
      }, LV_EVENT_DELETE, nullptr);
      requested.store(index); changed.store(true);
      return target;
    }
    ++index;
  }
  return nullptr;
}

bool reaction_cover_work_pending(std::uint64_t now_ms) {
  return changed.load() || staged || (requested.load() != resident_id && now_ms >= retry_after_ms);
}

void release_reaction_cover() {
  if(target)lv_obj_add_flag(target,LV_OBJ_FLAG_HIDDEN);
  target=nullptr;requested.store(-1);changed.store(true);
}

void prepare_reaction_cover(std::uint64_t now_ms) {
  const int next = requested.load();
  if (next < 0 || next == resident_id || (staged && staged_id == next) || now_ms < retry_after_ms) return;
  staged.reset(); staged_id = -1;
  Buffer bytes{static_cast<std::uint8_t*>(allocate(kCoverBytes))};
  const auto& source = kReactionCovers[next];
  if (!bytes || !core::decompress_gzip_exact(source.data, source.size, bytes.get(),
                                             kCoverBytes, allocate, heap_caps_free)) {
    retry_after_ms = now_ms + 5000;
    return;
  }
  staged = std::move(bytes); staged_id = next; retry_after_ms = 0;
}

void publish_reaction_cover() {
  changed.store(false);
  const int next = requested.load();
  if (resident_id != next) {
    lv_image_cache_drop(&descriptor);
    resident.reset(); descriptor = {}; resident_id = -1;
  }
  if (staged && staged_id == next && target) {
    resident = std::move(staged); resident_id = next;
    descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf = LV_COLOR_FORMAT_I4;
    descriptor.header.w = descriptor.header.h = kReactionCoverSize;
    descriptor.header.stride = kReactionCoverSize / 2;
    descriptor.data_size = kCoverBytes; descriptor.data = resident.get();
  }
  staged.reset(); staged_id = -1;
  if (target && resident && resident_id == next) {
    lv_image_set_src(target, &descriptor);
    lv_obj_remove_flag(target, LV_OBJ_FLAG_HIDDEN);
  }
}
}
