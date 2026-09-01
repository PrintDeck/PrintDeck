#include "printdeck/platform/audio_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "esp_codec_dev.h"
#include "esp_log.h"
#include "printdeck/core/settings.hpp"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/task_affinity.hpp"

extern "C" {
#define PRINTDECK_AUDIO_ASSET(name)                                             \
  extern const std::uint8_t name##_start[] asm("_binary_" #name "_adpcm_start"); \
  extern const std::uint8_t name##_end[] asm("_binary_" #name "_adpcm_end")
PRINTDECK_AUDIO_ASSET(modern_startup);
PRINTDECK_AUDIO_ASSET(modern_navigation);
PRINTDECK_AUDIO_ASSET(modern_orientation);
PRINTDECK_AUDIO_ASSET(modern_print_started);
PRINTDECK_AUDIO_ASSET(modern_print_paused);
PRINTDECK_AUDIO_ASSET(modern_print_finished);
PRINTDECK_AUDIO_ASSET(modern_print_error);
PRINTDECK_AUDIO_ASSET(modern_hms_alert);
PRINTDECK_AUDIO_ASSET(modern_filament_attention);
PRINTDECK_AUDIO_ASSET(modern_shutdown_countdown);
PRINTDECK_AUDIO_ASSET(modern_shutdown);
PRINTDECK_AUDIO_ASSET(modern_test);
#define PRINTDECK_PRESET_AUDIO_ASSETS(prefix)             \
  PRINTDECK_AUDIO_ASSET(prefix##_startup);                \
  PRINTDECK_AUDIO_ASSET(prefix##_navigation);             \
  PRINTDECK_AUDIO_ASSET(prefix##_orientation);            \
  PRINTDECK_AUDIO_ASSET(prefix##_print_started);          \
  PRINTDECK_AUDIO_ASSET(prefix##_progress_25);            \
  PRINTDECK_AUDIO_ASSET(prefix##_progress_50);            \
  PRINTDECK_AUDIO_ASSET(prefix##_progress_75);            \
  PRINTDECK_AUDIO_ASSET(prefix##_print_paused);           \
  PRINTDECK_AUDIO_ASSET(prefix##_print_finished);         \
  PRINTDECK_AUDIO_ASSET(prefix##_print_error);            \
  PRINTDECK_AUDIO_ASSET(prefix##_hms_alert);              \
  PRINTDECK_AUDIO_ASSET(prefix##_filament_attention);     \
  PRINTDECK_AUDIO_ASSET(prefix##_shutdown_countdown);     \
  PRINTDECK_AUDIO_ASSET(prefix##_shutdown);               \
  PRINTDECK_AUDIO_ASSET(prefix##_test)
PRINTDECK_PRESET_AUDIO_ASSETS(arcade);
PRINTDECK_PRESET_AUDIO_ASSETS(scifi);
#undef PRINTDECK_PRESET_AUDIO_ASSETS
PRINTDECK_AUDIO_ASSET(clean_navigation);
PRINTDECK_AUDIO_ASSET(clean_orientation);
PRINTDECK_AUDIO_ASSET(clean_shutdown_countdown);
PRINTDECK_AUDIO_ASSET(clean_test);
#define PRINTDECK_VOICE_AUDIO_ASSETS(language)                 \
  PRINTDECK_AUDIO_ASSET(voice_##language##_startup);           \
  PRINTDECK_AUDIO_ASSET(voice_##language##_print_started);     \
  PRINTDECK_AUDIO_ASSET(voice_##language##_progress_25);       \
  PRINTDECK_AUDIO_ASSET(voice_##language##_progress_50);       \
  PRINTDECK_AUDIO_ASSET(voice_##language##_progress_75);       \
  PRINTDECK_AUDIO_ASSET(voice_##language##_print_paused);      \
  PRINTDECK_AUDIO_ASSET(voice_##language##_print_finished);    \
  PRINTDECK_AUDIO_ASSET(voice_##language##_print_error);       \
  PRINTDECK_AUDIO_ASSET(voice_##language##_hms_alert);         \
  PRINTDECK_AUDIO_ASSET(voice_##language##_filament_attention); \
  PRINTDECK_AUDIO_ASSET(voice_##language##_shutdown)
PRINTDECK_VOICE_AUDIO_ASSETS(en);
PRINTDECK_VOICE_AUDIO_ASSETS(pl);
PRINTDECK_VOICE_AUDIO_ASSETS(es);
PRINTDECK_VOICE_AUDIO_ASSETS(fr);
PRINTDECK_VOICE_AUDIO_ASSETS(de);
PRINTDECK_VOICE_AUDIO_ASSETS(zh_cn);
#undef PRINTDECK_VOICE_AUDIO_ASSETS
#undef PRINTDECK_AUDIO_ASSET
}

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "audio";
constexpr int kSampleRate = 16000;
constexpr float kPi = 3.14159265358979323846F;
constexpr std::size_t kChunkSamples = 320;
constexpr std::size_t kAdpcmHeaderSize = 12;

constexpr std::array<std::int16_t, 89> kAdpcmStepTable{
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767,
};
constexpr std::array<std::int8_t, 8> kAdpcmIndexChange{-1, -1, -1, -1,
                                                       2,  4,  6,  8};

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

struct AdpcmSample {
  const std::uint8_t* begin;
  const std::uint8_t* end;
};

#define PRINTDECK_ADPCM_SAMPLE(name) AdpcmSample{name##_start, name##_end}
constexpr std::size_t kVoiceEventCount = 11;
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
    PRINTDECK_ADPCM_SAMPLE(voice_##language##_shutdown)}}
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
  }
  return PRINTDECK_ADPCM_SAMPLE(scifi_test);
}

AdpcmSample clean_sample_for(AudioService::Event event) {
  switch (event) {
    PRINTDECK_ADPCM_CASE(clean, navigation);
    PRINTDECK_ADPCM_CASE(clean, orientation);
    PRINTDECK_ADPCM_CASE(clean, shutdown_countdown);
    PRINTDECK_ADPCM_CASE(clean, test);
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

std::uint32_t read_le_u32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         static_cast<std::uint32_t>(bytes[1]) << 8U |
         static_cast<std::uint32_t>(bytes[2]) << 16U |
         static_cast<std::uint32_t>(bytes[3]) << 24U;
}

int read_le_i16(const std::uint8_t* bytes) {
  const int value = static_cast<int>(bytes[0]) |
                    static_cast<int>(bytes[1]) << 8;
  return value >= 0x8000 ? value - 0x10000 : value;
}

bool write_adpcm_sample(esp_codec_dev_handle_t codec, AdpcmSample sample, int volume) {
  const std::size_t size = static_cast<std::size_t>(sample.end - sample.begin);
  if (size < kAdpcmHeaderSize || sample.begin[0] != 'P' || sample.begin[1] != 'D' ||
      sample.begin[2] != 'I' || sample.begin[3] != 'A') {
    return false;
  }
  const std::uint32_t sample_count = read_le_u32(sample.begin + 4);
  int predictor = read_le_i16(sample.begin + 8);
  int step_index = sample.begin[10];
  if (sample_count == 0 || step_index >= static_cast<int>(kAdpcmStepTable.size()) ||
      sample.begin[11] != 0 ||
      size != kAdpcmHeaderSize + static_cast<std::size_t>(sample_count) / 2U) {
    return false;
  }

  std::array<std::int16_t, kChunkSamples> output{};
  std::size_t buffered = 0;
  const int clamped_volume = std::clamp(volume, 0, 100);
  const auto append = [&](int decoded) {
    output[buffered++] = static_cast<std::int16_t>(
        static_cast<std::int32_t>(decoded) * clamped_volume / 100);
    if (buffered == output.size()) {
      esp_codec_dev_write(codec, output.data(), buffered * sizeof(output[0]));
      buffered = 0;
    }
  };

  append(predictor);
  const std::uint8_t* payload = sample.begin + kAdpcmHeaderSize;
  for (std::uint32_t index = 1; index < sample_count; ++index) {
    const std::uint32_t code_index = index - 1;
    const std::uint8_t packed = payload[code_index / 2U];
    const int code = (code_index & 1U) == 0 ? packed & 0x0F : packed >> 4U;
    const int step = kAdpcmStepTable[static_cast<std::size_t>(step_index)];
    int delta = step >> 3;
    if ((code & 4) != 0) delta += step;
    if ((code & 2) != 0) delta += step >> 1;
    if ((code & 1) != 0) delta += step >> 2;
    predictor = std::clamp(predictor + ((code & 8) != 0 ? -delta : delta),
                           -32768, 32767);
    step_index = std::clamp(
        step_index + kAdpcmIndexChange[static_cast<std::size_t>(code & 7)], 0, 88);
    append(predictor);
  }
  if (buffered > 0) {
    esp_codec_dev_write(codec, output.data(), buffered * sizeof(output[0]));
  }
  return true;
}

void write_note(esp_codec_dev_handle_t codec, Note note, int volume,
                const SoundStyle& style) {
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

void AudioService::set_enabled(bool enabled) { enabled_.store(enabled); }

void AudioService::set_volume(int percent) { volume_.store(std::clamp(percent, 0, 100)); }

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

bool AudioService::play(Event event) {
  return play(event, preset_.load());
}

bool AudioService::play(Event event, Preset preset) {
  if (!enabled_.load() || volume_.load() == 0 || queue_ == nullptr) return false;
  const auto event_index = static_cast<std::uint8_t>(event);
  if (event_index < 14U &&
      (muted_events_.load() & (1U << event_index)) != 0) return false;
  const Request request{event, preset, volume_.load(), false, language_.load()};
  return xQueueSend(queue_, &request, 0) == pdTRUE;
}

bool AudioService::preview(Event event, Preset preset, int volume_percent) {
  if (volume_percent <= 0 || queue_ == nullptr) return false;
  const Request request{event, preset, std::clamp(volume_percent, 1, 100), true,
                        language_.load()};
  return xQueueSend(queue_, &request, 0) == pdTRUE;
}

void AudioService::task_entry(void* context) { static_cast<AudioService*>(context)->task_loop(); }

void AudioService::task_loop() {
  Request request{};
  while (true) {
    if (xQueueReceive(queue_, &request, portMAX_DELAY) == pdTRUE) {
      play_now(request.event, request.preset, request.volume, request.force,
               request.language);
    }
  }
}

void AudioService::play_now(Event event, Preset preset, int requested_volume, bool force,
                            std::uint8_t language) {
  if (!force && !enabled_.load()) return;
  auto codec = static_cast<esp_codec_dev_handle_t>(codec_);
  const Melody selected = melody_for(event);
  const SoundStyle style = style_for(preset);
  const int sample_volume = std::min(requested_volume, style.maximum_volume);
  if (sample_volume <= 0) return;
  write_silence(codec, 320);
  if (preset == Preset::clean && is_voice_event(event)) {
    if (write_adpcm_sample(codec, voice_sample_for(language, event), sample_volume)) {
      write_silence(codec, 1024);
      return;
    }
    ESP_LOGE(kLogTag, "Voice ADPCM asset is invalid");
  }
  const bool generated_progress = event == Event::progress_25 ||
                                  event == Event::progress_50 ||
                                  event == Event::progress_75;
  if (preset == Preset::modern && !generated_progress) {
    if (write_adpcm_sample(codec, modern_sample_for(event), sample_volume)) {
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
    if (write_adpcm_sample(codec, embedded_sample_for(preset, event), effect_volume)) {
      write_silence(codec, 1024);
      return;
    }
    ESP_LOGE(kLogTag, "Preset ADPCM asset is invalid");
  }
  const int volume = std::min(sample_volume, selected.maximum_volume);
  for (std::size_t index = 0; index < selected.count; ++index) {
    write_note(codec, selected.notes[index], volume, style);
  }
  write_silence(codec, 1024);
}

}  // namespace printdeck::platform
