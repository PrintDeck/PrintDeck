#include "printdeck/platform/audio_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "printdeck/core/audio_sample.hpp"
#include "printdeck/core/compressed_resource.hpp"
#include "printdeck/platform/audio_assets.hpp"
#include "printdeck/core/settings.hpp"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/task_affinity.hpp"

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "audio";
constexpr int kSampleRate = 16000;
constexpr float kPi = 3.14159265358979323846F;
constexpr std::size_t kChunkSamples = 320;
struct Note {
  std::uint16_t frequency;
  std::uint16_t milliseconds;
};

struct Melody {
  const Note* notes;
  std::size_t count;
  int maximum_volume;
};

struct SoundStyle {
  int pitch_percent;
  int duration_percent;
  float amplitude;
  float second_harmonic;
  float third_harmonic;
  int envelope_samples;
  int maximum_volume;
};

using AdpcmSample = audio_assets::CompressedSample;

struct PlaybackControl {
  const std::atomic<std::uint32_t>& generation;
  std::uint32_t expected;
  bool cancelled() const { return generation.load() != expected; }
};

void* allocate_audio_resource(std::size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

#define PRINTDECK_ADPCM_SAMPLE(name) audio_assets::name
constexpr std::size_t kVoiceEventCount = 12;
#define PRINTDECK_VOICE_SAMPLE_SET(language)                                    \
  {{PRINTDECK_ADPCM_SAMPLE(voice_##language##_startup),                         \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_print_started),                   \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_progress_25),                     \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_progress_50),                     \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_progress_75),                     \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_print_paused),                    \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_print_finished),                  \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_print_error),                     \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_hms_alert),                       \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_filament_attention),              \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_shutdown),                        \
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_restarting)}}
const std::array<std::array<AdpcmSample, kVoiceEventCount>, 6> kVoiceSamples{{
    PRINTDECK_VOICE_SAMPLE_SET(en),
    PRINTDECK_VOICE_SAMPLE_SET(pl),
    PRINTDECK_VOICE_SAMPLE_SET(es),
    PRINTDECK_VOICE_SAMPLE_SET(fr),
    PRINTDECK_VOICE_SAMPLE_SET(de),
    PRINTDECK_VOICE_SAMPLE_SET(zh_cn),
}};
#undef PRINTDECK_VOICE_SAMPLE_SET

bool is_voice_event(AudioService::Event event) {
  switch (event) {
    case AudioService::Event::startup:
    case AudioService::Event::print_started:
    case AudioService::Event::progress_25:
    case AudioService::Event::progress_50:
    case AudioService::Event::progress_75:
    case AudioService::Event::print_paused:
    case AudioService::Event::print_finished:
    case AudioService::Event::print_error:
    case AudioService::Event::hms_alert:
    case AudioService::Event::filament_attention:
    case AudioService::Event::shutdown:
    case AudioService::Event::restarting:
      return true;
    case AudioService::Event::navigation:
    case AudioService::Event::orientation:
    case AudioService::Event::shutdown_countdown:
    case AudioService::Event::test:
      return false;
  }
  return false;
}

std::size_t voice_event_index(AudioService::Event event) {
  switch (event) {
    case AudioService::Event::startup: return 0;
    case AudioService::Event::print_started: return 1;
    case AudioService::Event::progress_25: return 2;
    case AudioService::Event::progress_50: return 3;
    case AudioService::Event::progress_75: return 4;
    case AudioService::Event::print_paused: return 5;
    case AudioService::Event::print_finished: return 6;
    case AudioService::Event::print_error: return 7;
    case AudioService::Event::hms_alert: return 8;
    case AudioService::Event::filament_attention: return 9;
    case AudioService::Event::shutdown: return 10;
    case AudioService::Event::restarting: return 11;
    case AudioService::Event::navigation:
    case AudioService::Event::orientation:
    case AudioService::Event::shutdown_countdown:
    case AudioService::Event::test:
    default: return 0;
  }
}

AdpcmSample voice_sample_for(std::uint8_t language, AudioService::Event event) {
  const std::size_t language_index =
      std::min<std::size_t>(language, kVoiceSamples.size() - 1);
  return kVoiceSamples[language_index][voice_event_index(event)];
}

AdpcmSample modern_sample_for(AudioService::Event event) {
  switch (event) {
    case AudioService::Event::startup: return PRINTDECK_ADPCM_SAMPLE(modern_startup);
    case AudioService::Event::navigation: return PRINTDECK_ADPCM_SAMPLE(modern_navigation);
    case AudioService::Event::orientation: return PRINTDECK_ADPCM_SAMPLE(modern_orientation);
    case AudioService::Event::print_started: return PRINTDECK_ADPCM_SAMPLE(modern_print_started);
    case AudioService::Event::progress_25:
    case AudioService::Event::progress_50:
    case AudioService::Event::progress_75:
      return PRINTDECK_ADPCM_SAMPLE(modern_test);
    case AudioService::Event::print_paused: return PRINTDECK_ADPCM_SAMPLE(modern_print_paused);
    case AudioService::Event::print_finished: return PRINTDECK_ADPCM_SAMPLE(modern_print_finished);
    case AudioService::Event::print_error: return PRINTDECK_ADPCM_SAMPLE(modern_print_error);
    case AudioService::Event::hms_alert: return PRINTDECK_ADPCM_SAMPLE(modern_hms_alert);
    case AudioService::Event::filament_attention:
      return PRINTDECK_ADPCM_SAMPLE(modern_filament_attention);
    case AudioService::Event::shutdown_countdown:
      return PRINTDECK_ADPCM_SAMPLE(modern_shutdown_countdown);
    case AudioService::Event::shutdown: return PRINTDECK_ADPCM_SAMPLE(modern_shutdown);
    case AudioService::Event::test: return PRINTDECK_ADPCM_SAMPLE(modern_test);
    case AudioService::Event::restarting: return PRINTDECK_ADPCM_SAMPLE(modern_test);
  }
  return PRINTDECK_ADPCM_SAMPLE(modern_test);
}

#define PRINTDECK_ADPCM_CASE(prefix, event) \
  case AudioService::Event::event: return PRINTDECK_ADPCM_SAMPLE(prefix##_##event)

AdpcmSample arcade_sample_for(AudioService::Event event) {
  switch (event) {
    PRINTDECK_ADPCM_CASE(arcade, startup);
    PRINTDECK_ADPCM_CASE(arcade, navigation);
    PRINTDECK_ADPCM_CASE(arcade, orientation);
    PRINTDECK_ADPCM_CASE(arcade, print_started);
    PRINTDECK_ADPCM_CASE(arcade, progress_25);
    PRINTDECK_ADPCM_CASE(arcade, progress_50);
    PRINTDECK_ADPCM_CASE(arcade, progress_75);
    PRINTDECK_ADPCM_CASE(arcade, print_paused);
    PRINTDECK_ADPCM_CASE(arcade, print_finished);
    PRINTDECK_ADPCM_CASE(arcade, print_error);
    PRINTDECK_ADPCM_CASE(arcade, hms_alert);
    PRINTDECK_ADPCM_CASE(arcade, filament_attention);
    PRINTDECK_ADPCM_CASE(arcade, shutdown_countdown);
    PRINTDECK_ADPCM_CASE(arcade, shutdown);
    PRINTDECK_ADPCM_CASE(arcade, test);
    case AudioService::Event::restarting: return PRINTDECK_ADPCM_SAMPLE(arcade_test);
  }
  return PRINTDECK_ADPCM_SAMPLE(arcade_test);
}

AdpcmSample scifi_sample_for(AudioService::Event event) {
  switch (event) {
    PRINTDECK_ADPCM_CASE(scifi, startup);
    PRINTDECK_ADPCM_CASE(scifi, navigation);
    PRINTDECK_ADPCM_CASE(scifi, orientation);
    PRINTDECK_ADPCM_CASE(scifi, print_started);
    PRINTDECK_ADPCM_CASE(scifi, progress_25);
    PRINTDECK_ADPCM_CASE(scifi, progress_50);
    PRINTDECK_ADPCM_CASE(scifi, progress_75);
    PRINTDECK_ADPCM_CASE(scifi, print_paused);
    PRINTDECK_ADPCM_CASE(scifi, print_finished);
    PRINTDECK_ADPCM_CASE(scifi, print_error);
    PRINTDECK_ADPCM_CASE(scifi, hms_alert);
    PRINTDECK_ADPCM_CASE(scifi, filament_attention);
    PRINTDECK_ADPCM_CASE(scifi, shutdown_countdown);
    PRINTDECK_ADPCM_CASE(scifi, shutdown);
    PRINTDECK_ADPCM_CASE(scifi, test);
    case AudioService::Event::restarting: return PRINTDECK_ADPCM_SAMPLE(scifi_test);
  }
  return PRINTDECK_ADPCM_SAMPLE(scifi_test);
}

AdpcmSample clean_sample_for(AudioService::Event event) {
  switch (event) {
    PRINTDECK_ADPCM_CASE(clean, navigation);
    PRINTDECK_ADPCM_CASE(clean, orientation);
    PRINTDECK_ADPCM_CASE(clean, shutdown_countdown);
    PRINTDECK_ADPCM_CASE(clean, test);
    case AudioService::Event::restarting: return PRINTDECK_ADPCM_SAMPLE(clean_test);
    default: return PRINTDECK_ADPCM_SAMPLE(clean_test);
  }
}

AdpcmSample embedded_sample_for(AudioService::Preset preset, AudioService::Event event) {
  switch (preset) {
    case AudioService::Preset::arcade: return arcade_sample_for(event);
    case AudioService::Preset::scifi: return scifi_sample_for(event);
    case AudioService::Preset::clean: return clean_sample_for(event);
    default: return modern_sample_for(event);
  }
}

#undef PRINTDECK_ADPCM_CASE
#undef PRINTDECK_ADPCM_SAMPLE

constexpr Note kStartup[]{{392, 150}, {523, 150}, {659, 190}, {0, 55},
                          {784, 170}, {988, 330}, {0, 45},  {784, 110},
                          {988, 280}};
constexpr Note kNavigation[]{{900, 75}};
constexpr Note kOrientation[]{{1500, 75}};
constexpr Note kPrintStarted[]{{523, 90}, {659, 90}, {784, 140}};
constexpr Note kProgress25[]{{659, 75}, {784, 75}, {988, 165}};
constexpr Note kProgress50[]{{523, 70}, {659, 70}, {784, 80}, {1047, 210}};
constexpr Note kProgress75[]{{784, 65}, {1047, 65}, {1319, 85}, {1568, 220},
                             {0, 45}, {1568, 110}};
constexpr Note kPrintPaused[]{{659, 100}, {0, 60}, {659, 100}};
constexpr Note kPrintFinished[]{{523, 90}, {659, 90}, {784, 120}, {1047, 230},
                                {784, 120}, {1047, 230}};
constexpr Note kPrintError[]{{220, 220}, {0, 80}, {220, 220}, {0, 80}, {165, 320}};
constexpr Note kHmsAlert[]{{1100, 120}, {0, 45}, {850, 160}, {0, 45}, {1100, 120}};
constexpr Note kFilament[]{{784, 110}, {988, 110}, {784, 110}, {988, 190}};
constexpr Note kShutdownCountdown[]{{1050, 55}};
constexpr Note kShutdown[]{{784, 150}, {0, 85}, {587, 170}, {0, 95}, {392, 230}};
constexpr Note kTest[]{{660, 110}, {0, 45}, {880, 150}};

template <std::size_t N>
constexpr Melody melody(const Note (&notes)[N], int maximum_volume = 100) {
  return {notes, N, maximum_volume};
}

Melody melody_for(AudioService::Event event) {
  switch (event) {
    case AudioService::Event::startup: return melody(kStartup);
    case AudioService::Event::navigation: return melody(kNavigation, 65);
    case AudioService::Event::orientation: return melody(kOrientation, 60);
    case AudioService::Event::print_started: return melody(kPrintStarted);
    case AudioService::Event::progress_25: return melody(kProgress25);
    case AudioService::Event::progress_50: return melody(kProgress50);
    case AudioService::Event::progress_75: return melody(kProgress75);
    case AudioService::Event::print_paused: return melody(kPrintPaused);
    case AudioService::Event::print_finished: return melody(kPrintFinished);
    case AudioService::Event::print_error: return melody(kPrintError);
    case AudioService::Event::hms_alert: return melody(kHmsAlert);
    case AudioService::Event::filament_attention: return melody(kFilament);
    case AudioService::Event::shutdown_countdown: return melody(kShutdownCountdown, 30);
    case AudioService::Event::shutdown: return melody(kShutdown);
    case AudioService::Event::test: return melody(kTest);
    case AudioService::Event::restarting: return melody(kTest);
  }
  return melody(kTest);
}

SoundStyle style_for(AudioService::Preset preset) {
  switch (preset) {
    case AudioService::Preset::oldschool:
      return {100, 100, 0.45F, 0.12F, 0.0F, 80, 100};
    case AudioService::Preset::soft:
      return {82, 125, 0.32F, 0.03F, 0.0F, 150, 72};
    case AudioService::Preset::modern:
      return {145, 72, 0.42F, 0.04F, 0.28F, 35, 100};
    case AudioService::Preset::arcade:
      return {100, 100, 1.0F, 0.0F, 0.0F, 1, 100};
    case AudioService::Preset::scifi:
      return {100, 100, 1.0F, 0.0F, 0.0F, 1, 100};
    case AudioService::Preset::clean:
      return {100, 100, 1.0F, 0.0F, 0.0F, 1, 100};
  }
  return style_for(AudioService::Preset::oldschool);
}

void write_silence(esp_codec_dev_handle_t codec, std::size_t samples) {
  static std::array<std::int16_t, kChunkSamples> silence{};
  while (samples > 0) {
    const std::size_t count = std::min(samples, silence.size());
    esp_codec_dev_write(codec, silence.data(), count * sizeof(silence[0]));
    samples -= count;
  }
}

bool write_adpcm_sample(esp_codec_dev_handle_t codec, AdpcmSample sample, int volume,
                        const PlaybackControl& control) {
  if (control.cancelled()) return true;
  const std::size_t size = static_cast<std::size_t>(sample.end - sample.begin);
  if (sample.decoded_size < 12 ||
      sample.decoded_size > core::kMaximumAudioSampleBytes ||
      size > core::kMaximumAudioSampleBytes + 1024) return false;
  std::unique_ptr<std::uint8_t, decltype(&heap_caps_free)> decoded(
      static_cast<std::uint8_t*>(allocate_audio_resource(sample.decoded_size)),
      &heap_caps_free);
  if (!decoded) return false;
  if (!core::decompress_gzip_exact(sample.begin, size, decoded.get(), sample.decoded_size,
                                  allocate_audio_resource, heap_caps_free)) return false;
  if (control.cancelled()) return true;
  core::AudioSampleDecoder reader;
  if (!reader.open(decoded.get(), sample.decoded_size)) return false;

  // Validate the full compressed stream before any clip samples reach I2S.
  // The complete PDIA buffer and zlib workspace use PSRAM, never the task stack.
  std::array<std::int16_t, kChunkSamples> output{};
  const int clamped_volume = std::clamp(volume, 0, 100);
  while (!reader.finished()) {
    if (control.cancelled()) return true;
    const std::size_t count = reader.read(output.data(), output.size());
    for (std::size_t index = 0; index < count; ++index) {
      output[index] = static_cast<std::int16_t>(
          static_cast<std::int32_t>(output[index]) * clamped_volume / 100);
    }
    if (esp_codec_dev_write(codec, output.data(), count * sizeof(output[0])) != ESP_OK) {
      return false;
    }
  }
  return true;
}

void write_note(esp_codec_dev_handle_t codec, Note note, int volume,
                const SoundStyle& style, const PlaybackControl& control) {
  const int milliseconds = std::max(
      1, static_cast<int>(note.milliseconds) * style.duration_percent / 100);
  if (note.frequency == 0) {
    write_silence(codec, static_cast<std::size_t>(kSampleRate * milliseconds / 1000));
    return;
  }
  const int total = std::max(1, kSampleRate * milliseconds / 1000);
  const float amplitude = style.amplitude *
                          static_cast<float>(std::clamp(volume, 0, 100)) / 100.0F;
  const float frequency = static_cast<float>(note.frequency) *
                          static_cast<float>(style.pitch_percent) / 100.0F;
  const float step = 2.0F * kPi * frequency /
                     static_cast<float>(kSampleRate);
  std::array<std::int16_t, kChunkSamples> samples{};
  int written = 0;
  while (written < total) {
    if (control.cancelled()) return;
    const int count = std::min<int>(samples.size(), total - written);
    for (int index = 0; index < count; ++index) {
      const int position = written + index;
      const int edge = std::min(position, total - position - 1);
      const float envelope = std::clamp(
          static_cast<float>(edge) / static_cast<float>(style.envelope_samples), 0.0F, 1.0F);
      const float fundamental = std::sin(step * static_cast<float>(position));
      const float harmonic =
          style.second_harmonic * std::sin(step * 2.0F * static_cast<float>(position)) +
          style.third_harmonic * std::sin(step * 3.0F * static_cast<float>(position));
      const float normalizer = 1.0F + style.second_harmonic + style.third_harmonic;
      samples[static_cast<std::size_t>(index)] = static_cast<std::int16_t>(
          32767.0F * amplitude * envelope * (fundamental + harmonic) / normalizer);
    }
    esp_codec_dev_write(codec, samples.data(), static_cast<std::size_t>(count) * sizeof(samples[0]));
    written += count;
  }
}

}  // namespace

esp_err_t AudioService::start(bool enabled, int volume_percent, std::string_view preset_id,
                              std::uint16_t muted_events) {
  if (task_ != nullptr) return ESP_OK;
  enabled_.store(enabled);
  volume_.store(std::clamp(volume_percent, 0, 100));
  Preset selected = Preset::modern;
  preset_from_id(preset_id, selected);
  preset_.store(selected);
  muted_events_.store(muted_events & core::kAudioEventMuteMask);
  codec_ = board_audio_codec_speaker_init();
  if (codec_ == nullptr) return ESP_FAIL;
  auto codec = static_cast<esp_codec_dev_handle_t>(codec_);
  esp_codec_dev_sample_info_t sample{};
  sample.bits_per_sample = 16;
  sample.channel = 1;
  sample.sample_rate = kSampleRate;
  esp_err_t result = esp_codec_dev_open(codec, &sample);
  if (result != ESP_OK) {
    codec_ = nullptr;
    return result;
  }
  result = esp_codec_dev_set_out_vol(codec, 100);
  if (result != ESP_OK) {
    esp_codec_dev_close(codec);
    codec_ = nullptr;
    return result;
  }
  queue_ = xQueueCreate(6, sizeof(Request));
  if (queue_ == nullptr) {
    esp_codec_dev_close(codec);
    codec_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreatePinnedToCore(task_entry, "printdeck_audio", 4096, this, 4, &task_,
                              kServiceCore) != pdPASS) {
    task_ = nullptr;
    vQueueDelete(queue_);
    queue_ = nullptr;
    esp_codec_dev_close(codec);
    codec_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(kLogTag, "Audio service ready");
  return ESP_OK;
}

bool AudioService::play(Event event) {
  return play(event, nullptr, nullptr);
}

bool AudioService::play(Event event, CompletionCallback completion, void* context) {
  const Preset preset = preset_.load();
  if (!enabled_.load() || volume_.load() == 0 || queue_ == nullptr) return false;
  const auto event_index = static_cast<std::uint8_t>(event);
  if (event_index < 14U &&
      (muted_events_.load() & (1U << event_index)) != 0) return false;
  const Request request{event, preset, volume_.load(), false, language_.load(),
                        completion, context, playback_generation_.load()};
  return xQueueSend(queue_, &request, 0) == pdTRUE;
}

bool AudioService::play(Event event, Preset preset) {
  if (!enabled_.load() || volume_.load() == 0 || queue_ == nullptr) return false;
  const auto event_index = static_cast<std::uint8_t>(event);
  if (event_index < 14U &&
      (muted_events_.load() & (1U << event_index)) != 0) return false;
  const Request request{event, preset, volume_.load(), false, language_.load(),
                        nullptr, nullptr, playback_generation_.load()};
  return xQueueSend(queue_, &request, 0) == pdTRUE;
}

bool AudioService::preview(Event event, Preset preset, int volume_percent) {
  if (volume_percent <= 0 || queue_ == nullptr) return false;
  bool expected = false;
  if (!preview_busy_.compare_exchange_strong(expected, true)) return false;
  const Request request{event, preset, std::clamp(volume_percent, 1, 100), true,
                        language_.load(), nullptr, nullptr, playback_generation_.load()};
  if (xQueueSend(queue_, &request, 0) == pdTRUE) return true;
  preview_busy_.store(false);
  return false;
}

void AudioService::task_entry(void* context) { static_cast<AudioService*>(context)->task_loop(); }

void AudioService::task_loop() {
  Request request{};
  while (true) {
    if (xQueueReceive(queue_, &request, pdMS_TO_TICKS(250)) == pdTRUE) {
      play_now(request.event, request.preset, request.volume, request.force,
               request.language, request.generation);
      if (request.force) preview_busy_.store(false);
      if (request.completion != nullptr) {
        request.completion(request.completion_context);
      }
    }
  }
}

void AudioService::play_now(Event event, Preset preset, int requested_volume, bool force,
                            std::uint8_t language, std::uint32_t generation) {
  const PlaybackControl control{playback_generation_, generation};
  if (control.cancelled()) return;
  if (!force && !enabled_.load()) return;
  auto codec = static_cast<esp_codec_dev_handle_t>(codec_);
  if (requested_volume <= 0) return;
  write_silence(codec, 320);
  // Restarting is a product lifecycle message, not a theme effect. Always use
  // the localized voice while still respecting the global audio switch and
  // volume selected by the user.
  if (event == Event::restarting) {
    if (write_adpcm_sample(codec, voice_sample_for(language, event),
                           std::clamp(requested_volume, 1, 100), control)) {
      write_silence(codec, 1024);
      return;
    }
    ESP_LOGE(kLogTag, "Restarting voice ADPCM asset is invalid");
    write_silence(codec, 1024);
    return;
  }
  const Melody selected = melody_for(event);
  const SoundStyle style = style_for(preset);
  const int sample_volume = std::min(requested_volume, style.maximum_volume);
  if (preset == Preset::clean && is_voice_event(event)) {
    if (write_adpcm_sample(codec, voice_sample_for(language, event), sample_volume, control)) {
      write_silence(codec, 1024);
      return;
    }
    ESP_LOGE(kLogTag, "Voice ADPCM asset is invalid");
  }
  const bool generated_progress = event == Event::progress_25 ||
                                  event == Event::progress_50 ||
                                  event == Event::progress_75;
  if (preset == Preset::modern && !generated_progress) {
    if (write_adpcm_sample(codec, modern_sample_for(event), sample_volume, control)) {
      write_silence(codec, 1024);
      return;
    }
    ESP_LOGE(kLogTag, "Modern ADPCM asset is invalid");
  }
  if (preset == Preset::arcade || preset == Preset::scifi || preset == Preset::clean) {
    const int effect_volume =
        preset == Preset::clean && event == Event::orientation
            ? std::max(1, sample_volume * 60 / 100)
            : sample_volume;
    if (write_adpcm_sample(codec, embedded_sample_for(preset, event), effect_volume, control)) {
      write_silence(codec, 1024);
      return;
    }
    ESP_LOGE(kLogTag, "Preset ADPCM asset is invalid");
  }
  const int volume = std::min(sample_volume, selected.maximum_volume);
  for (std::size_t index = 0; index < selected.count; ++index) {
    if (control.cancelled()) return;
    write_note(codec, selected.notes[index], volume, style, control);
  }
  write_silence(codec, 1024);
}

}  // namespace printdeck::platform
