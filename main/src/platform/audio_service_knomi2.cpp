#include "printdeck/platform/audio_service.hpp"

namespace printdeck::platform {

esp_err_t AudioService::start(bool enabled, int volume_percent, std::string_view preset_id,
                              std::uint16_t muted_events) {
  set_enabled(enabled);
  set_volume(volume_percent);
  Preset selected = Preset::modern;
  preset_from_id(preset_id, selected);
  set_preset(selected);
  set_muted_events(muted_events);
  // KNOMI2 has no audio output. Keep the shared settings contract without
  // allocating an audio task, codec, queue or embedded sound resources.
  return ESP_ERR_NOT_SUPPORTED;
}

bool AudioService::play(Event) { return false; }

bool AudioService::play(Event, CompletionCallback, void*) { return false; }

bool AudioService::play(Event, Preset) { return false; }

bool AudioService::preview(Event, Preset, int) { return false; }

}  // namespace printdeck::platform
