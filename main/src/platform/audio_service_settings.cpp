#include "printdeck/platform/audio_service.hpp"

#include <algorithm>

#include "printdeck/core/settings.hpp"

namespace printdeck::platform {

void AudioService::set_enabled(bool enabled) {
  enabled_.store(enabled);
  if (!enabled) playback_generation_.fetch_add(1);
}

void AudioService::set_volume(int percent) {
  volume_.store(std::clamp(percent, 0, 100));
  if (percent <= 0) playback_generation_.fetch_add(1);
}

void AudioService::set_preset(Preset preset) { preset_.store(preset); }

void AudioService::set_muted_events(std::uint16_t muted_events) {
  muted_events_.store(muted_events & core::kAudioEventMuteMask);
}

void AudioService::set_language(std::string_view language) {
  std::uint8_t selected = 0;
  if (language == "pl") selected = 1;
  else if (language == "es") selected = 2;
  else if (language == "fr") selected = 3;
  else if (language == "de") selected = 4;
  else if (language == "zh-CN") selected = 5;
  language_.store(selected);
}

bool AudioService::preset_from_id(std::string_view id, Preset& preset) {
  if (id == "modern") preset = Preset::modern;
  else if (id == "soft") preset = Preset::soft;
  else if (id == "oldschool") preset = Preset::oldschool;
  else if (id == "arcade") preset = Preset::arcade;
  else if (id == "scifi") preset = Preset::scifi;
  else if (id == "clean") preset = Preset::clean;
  else return false;
  return true;
}

}  // namespace printdeck::platform
