#pragma once

#include <mutex>
#include <string>
#include <cstdio>

namespace printdeck::platform {

// Mounts, destination changes and complete set transactions share this lock.
// Playback uses immutable memory, so neither LVGL nor audio waits for an SD write.
// Callers acquire it before any service filesystem/state mutex.
struct SdSetStorage {
  std::recursive_mutex mutex;
  std::string root;
};
inline SdSetStorage& sd_set_storage() {
  static SdSetStorage storage;
  return storage;
}

// Allow FAT allocation rounding and metadata as well as the payload itself.
inline constexpr std::size_t kSdSetReserveBytes = 2 * 1024 * 1024;

// The board deliberately uses FAT 8.3 names, without long-name heap buffers.
inline std::string sd_reaction_filename(std::size_t index) {
  char name[13];
  std::snprintf(name, sizeof(name), "r%02u.gif", static_cast<unsigned>(index));
  return name;
}

}  // namespace printdeck::platform
