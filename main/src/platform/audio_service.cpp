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

extern "C" {
#if defined(PRINTDECK_LOCAL_VOICE)
#define PRINTDECK_VOICE_ASSET(name)                                                \
  extern const std::uint8_t voice_##name##_start[]                                \
      asm("_binary_voice_" #name "_adpcm_gz_start");                                \
  extern const std::uint8_t voice_##name##_end[]                                  \
      asm("_binary_voice_" #name "_adpcm_gz_end")
PRINTDECK_VOICE_ASSET(yes);
PRINTDECK_VOICE_ASSET(print_is);
PRINTDECK_VOICE_ASSET(percent_complete);
PRINTDECK_VOICE_ASSET(printing_for);
PRINTDECK_VOICE_ASSET(remaining);
PRINTDECK_VOICE_ASSET(estimated_completion);
PRINTDECK_VOICE_ASSET(no_active_print);
PRINTDECK_VOICE_ASSET(printer_unavailable);
PRINTDECK_VOICE_ASSET(timing_unavailable);
PRINTDECK_VOICE_ASSET(and);
PRINTDECK_VOICE_ASSET(hour);
PRINTDECK_VOICE_ASSET(hours);
PRINTDECK_VOICE_ASSET(minute);
PRINTDECK_VOICE_ASSET(minutes);
PRINTDECK_VOICE_ASSET(am);
PRINTDECK_VOICE_ASSET(pm);
PRINTDECK_VOICE_ASSET(oh);
PRINTDECK_VOICE_ASSET(zero);
PRINTDECK_VOICE_ASSET(one);
PRINTDECK_VOICE_ASSET(two);
PRINTDECK_VOICE_ASSET(three);
PRINTDECK_VOICE_ASSET(four);
PRINTDECK_VOICE_ASSET(five);
PRINTDECK_VOICE_ASSET(six);
PRINTDECK_VOICE_ASSET(seven);
PRINTDECK_VOICE_ASSET(eight);
PRINTDECK_VOICE_ASSET(nine);
PRINTDECK_VOICE_ASSET(ten);
PRINTDECK_VOICE_ASSET(eleven);
PRINTDECK_VOICE_ASSET(twelve);
PRINTDECK_VOICE_ASSET(thirteen);
PRINTDECK_VOICE_ASSET(fourteen);
PRINTDECK_VOICE_ASSET(fifteen);
PRINTDECK_VOICE_ASSET(sixteen);
PRINTDECK_VOICE_ASSET(seventeen);
PRINTDECK_VOICE_ASSET(eighteen);
PRINTDECK_VOICE_ASSET(nineteen);
PRINTDECK_VOICE_ASSET(twenty);
PRINTDECK_VOICE_ASSET(thirty);
PRINTDECK_VOICE_ASSET(forty);
PRINTDECK_VOICE_ASSET(fifty);
PRINTDECK_VOICE_ASSET(sixty);
PRINTDECK_VOICE_ASSET(seventy);
PRINTDECK_VOICE_ASSET(eighty);
PRINTDECK_VOICE_ASSET(ninety);
PRINTDECK_VOICE_ASSET(hundred);
#undef PRINTDECK_VOICE_ASSET
#endif
}

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

#if defined(PRINTDECK_LOCAL_VOICE)
struct VoiceSample {
  const std::uint8_t* begin;
  const std::uint8_t* end;
};

#define PRINTDECK_VOICE_SAMPLE(name) VoiceSample{voice_##name##_start, voice_##name##_end}
constexpr std::array<int, 8> kImaIndexTable{-1, -1, -1, -1, 2, 4, 6, 8};
constexpr std::array<int, 89> kImaStepTable{
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,
    21,    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,
    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,
    173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,
    494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,
    1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,
    4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};

constexpr std::array<VoiceSample, 20> kSmallNumbers{
    PRINTDECK_VOICE_SAMPLE(zero),      PRINTDECK_VOICE_SAMPLE(one),
    PRINTDECK_VOICE_SAMPLE(two),       PRINTDECK_VOICE_SAMPLE(three),
    PRINTDECK_VOICE_SAMPLE(four),      PRINTDECK_VOICE_SAMPLE(five),
    PRINTDECK_VOICE_SAMPLE(six),       PRINTDECK_VOICE_SAMPLE(seven),
    PRINTDECK_VOICE_SAMPLE(eight),     PRINTDECK_VOICE_SAMPLE(nine),
    PRINTDECK_VOICE_SAMPLE(ten),       PRINTDECK_VOICE_SAMPLE(eleven),
    PRINTDECK_VOICE_SAMPLE(twelve),    PRINTDECK_VOICE_SAMPLE(thirteen),
    PRINTDECK_VOICE_SAMPLE(fourteen),  PRINTDECK_VOICE_SAMPLE(fifteen),
    PRINTDECK_VOICE_SAMPLE(sixteen),   PRINTDECK_VOICE_SAMPLE(seventeen),
    PRINTDECK_VOICE_SAMPLE(eighteen),  PRINTDECK_VOICE_SAMPLE(nineteen)};

constexpr std::array<VoiceSample, 8> kTens{
    PRINTDECK_VOICE_SAMPLE(twenty), PRINTDECK_VOICE_SAMPLE(thirty),
    PRINTDECK_VOICE_SAMPLE(forty),  PRINTDECK_VOICE_SAMPLE(fifty),
    PRINTDECK_VOICE_SAMPLE(sixty),  PRINTDECK_VOICE_SAMPLE(seventy),
    PRINTDECK_VOICE_SAMPLE(eighty), PRINTDECK_VOICE_SAMPLE(ninety)};
#undef PRINTDECK_VOICE_SAMPLE
#endif


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

#if defined(PRINTDECK_LOCAL_VOICE)
void write_voice_sample(esp_codec_dev_handle_t codec, VoiceSample sample, int volume, const PlaybackControl& control) {
  if (control.cancelled()) return;
  const std::size_t packed_size = static_cast<std::size_t>(sample.end - sample.begin);
  if (packed_size < 18 || packed_size > core::kMaximumAudioSampleBytes + 1024) return;
  const auto* trailer = sample.end - 4;
  const std::size_t raw_size = static_cast<std::uint32_t>(trailer[0]) |
      static_cast<std::uint32_t>(trailer[1]) << 8U |
      static_cast<std::uint32_t>(trailer[2]) << 16U |
      static_cast<std::uint32_t>(trailer[3]) << 24U;
  if (raw_size == 0 || raw_size > core::kMaximumAudioSampleBytes) return;
  std::unique_ptr<std::uint8_t, decltype(&heap_caps_free)> decoded(
      static_cast<std::uint8_t*>(allocate_audio_resource(raw_size)), &heap_caps_free);
  if (!decoded || !core::decompress_gzip_exact(sample.begin, packed_size,
          decoded.get(), raw_size, allocate_audio_resource, heap_caps_free)) return;
  if (control.cancelled()) return;
  sample = {decoded.get(), decoded.get() + raw_size};
  std::array<std::int16_t, kChunkSamples> output{};
  std::size_t output_count = 0;
  int predictor = 0;
  int step_index = 0;
  const auto flush = [&]() {
    if (output_count == 0) return;
    esp_codec_dev_write(codec, output.data(), output_count * sizeof(output[0]));
    output_count = 0;
  };
  for (const std::uint8_t* input = sample.begin; input < sample.end; ++input) {
    if (control.cancelled()) return;
    for (int shift : {4, 0}) {
      const int code = (*input >> shift) & 0x0f;
      const int step = kImaStepTable[static_cast<std::size_t>(step_index)];
      int delta = step >> 3;
      if ((code & 4) != 0) delta += step;
      if ((code & 2) != 0) delta += step >> 1;
      if ((code & 1) != 0) delta += step >> 2;
      predictor += (code & 8) != 0 ? -delta : delta;
      predictor = std::clamp(predictor, -32768, 32767);
      step_index += kImaIndexTable[static_cast<std::size_t>(code & 7)];
      step_index = std::clamp(step_index, 0, 88);
      output[output_count++] = static_cast<std::int16_t>(
          predictor * std::clamp(volume, 0, 100) / 100);
      if (output_count == output.size()) flush();
    }
  }
  flush();
}

void write_voice_number(esp_codec_dev_handle_t codec, unsigned value, int volume, const PlaybackControl& control) {
  value = std::min(value, 999U);
  if (value >= 100) {
    write_voice_sample(codec, kSmallNumbers[value / 100], volume, control);
    write_voice_sample(codec, {voice_hundred_start, voice_hundred_end}, volume, control);
    value %= 100;
    if (value == 0) return;
  }
  if (value < kSmallNumbers.size()) {
    write_voice_sample(codec, kSmallNumbers[value], volume, control);
    return;
  }
  write_voice_sample(codec, kTens[value / 10 - 2], volume, control);
  if (value % 10 != 0) write_voice_sample(codec, kSmallNumbers[value % 10], volume, control);
}

void write_voice_duration(esp_codec_dev_handle_t codec, std::uint32_t seconds, int volume, const PlaybackControl& control) {
  const unsigned total_minutes = std::min<std::uint32_t>(seconds / 60U + (seconds % 60U >= 30U ? 1U : 0U), 59999U);
  const unsigned hours = total_minutes / 60U;
  const unsigned minutes = total_minutes % 60U;
  if (hours > 0) {
    write_voice_number(codec, hours, volume, control);
    write_voice_sample(codec, hours == 1 ? VoiceSample{voice_hour_start, voice_hour_end}
                                         : VoiceSample{voice_hours_start, voice_hours_end},
                       volume, control);
    if (minutes > 0) {
      write_voice_sample(codec, {voice_and_start, voice_and_end}, volume, control);
    }
  }
  if (minutes > 0 || hours == 0) {
    write_voice_number(codec, minutes, volume, control);
    write_voice_sample(codec, minutes == 1 ? VoiceSample{voice_minute_start, voice_minute_end}
                                           : VoiceSample{voice_minutes_start, voice_minutes_end},
                       volume, control);
  }
}
#endif


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

#if defined(PRINTDECK_LOCAL_VOICE)
std::uint32_t AudioService::acknowledge_voice_command() {
  return queue_voice(RequestKind::voice_acknowledgement, {});
}

std::uint32_t AudioService::speak_print_status(const SpokenPrintStatus& status,
                                               SpokenResponse response) {
  RequestKind kind = RequestKind::voice_status;
  if (response == SpokenResponse::remaining_time) {
    kind = RequestKind::voice_remaining_time;
  } else if (response == SpokenResponse::completion_time) {
    kind = RequestKind::voice_completion_time;
  }
  return queue_voice(kind, status);
}

bool AudioService::voice_request_complete(std::uint32_t ticket) const {
  return ticket != 0 && completed_voice_ticket_.load() == ticket;
}

std::uint32_t AudioService::queue_voice(RequestKind kind, const SpokenPrintStatus& status) {
#if defined(PRINTDECK_LOCAL_VOICE)
  if (!enabled_.load() || volume_.load() == 0 || queue_ == nullptr) return 0;
  std::uint32_t ticket = next_voice_ticket_.fetch_add(1);
  if (ticket == 0) ticket = next_voice_ticket_.fetch_add(1);
  Request request{Event::test, preset_.load(), volume_.load(), false, language_.load(),
                  nullptr, nullptr, playback_generation_.load()};
  request.kind = kind;
  request.ticket = ticket;
  request.status = status;
  return xQueueSend(queue_, &request, 0) == pdTRUE ? ticket : 0;
#else
  (void)kind;
  (void)status;
  return 0;
#endif
}

#endif

void AudioService::task_entry(void* context) { static_cast<AudioService*>(context)->task_loop(); }

void AudioService::task_loop() {
  Request request{};
  while (true) {
    if (xQueueReceive(queue_, &request, pdMS_TO_TICKS(250)) == pdTRUE) {
#if defined(PRINTDECK_LOCAL_VOICE)
      playback_active_.store(true);
      if (request.kind != RequestKind::event) {
        play_voice_now(request.kind, request.status, request.volume, request.generation);
      } else
#endif
      {
        play_now(request.event, request.preset, request.volume, request.force,
                 request.language, request.generation);
      }
#if defined(PRINTDECK_LOCAL_VOICE)
      playback_active_.store(false);
      if (request.ticket != 0) completed_voice_ticket_.store(request.ticket);
#endif
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
  if (!force) requested_volume = requested_volume * display_volume_scale_.load() / 100;
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

#if defined(PRINTDECK_LOCAL_VOICE)
void AudioService::play_voice_now(RequestKind kind, const SpokenPrintStatus& status, int volume,
                                  std::uint32_t generation) {
#if defined(PRINTDECK_LOCAL_VOICE)
  const PlaybackControl control{playback_generation_, generation};
  if (control.cancelled() || !enabled_.load()) return;
  volume = volume * display_volume_scale_.load() / 100;
  auto codec = static_cast<esp_codec_dev_handle_t>(codec_);
  if (codec == nullptr || volume <= 0) return;
  const auto say = [&](const std::uint8_t* begin, const std::uint8_t* end) {
    write_voice_sample(codec, {begin, end}, volume, control);
  };
  write_silence(codec, 320);
  if (kind == RequestKind::voice_acknowledgement) {
    say(voice_yes_start, voice_yes_end);
    write_silence(codec, 640);
    return;
  }
  if (!status.printer_available) {
    say(voice_printer_unavailable_start, voice_printer_unavailable_end);
    write_silence(codec, 1024);
    return;
  }
  if (!status.print_active) {
    say(voice_no_active_print_start, voice_no_active_print_end);
    write_silence(codec, 1024);
    return;
  }
  if (kind == RequestKind::voice_status) {
    say(voice_print_is_start, voice_print_is_end);
    write_voice_number(codec, status.completion_percent, volume, control);
    say(voice_percent_complete_start, voice_percent_complete_end);
    write_silence(codec, 1024);
    return;
  }
  if (kind == RequestKind::voice_remaining_time) {
    if (!status.timing_available || status.remaining_seconds == 0) {
      say(voice_timing_unavailable_start, voice_timing_unavailable_end);
      write_silence(codec, 1024);
      return;
    }
    write_voice_duration(codec, status.remaining_seconds, volume, control);
    say(voice_remaining_start, voice_remaining_end);
    write_silence(codec, 1024);
    return;
  }
  if (kind == RequestKind::voice_completion_time) {
    if (!status.eta_available) {
      say(voice_timing_unavailable_start, voice_timing_unavailable_end);
      write_silence(codec, 1024);
      return;
    }
    say(voice_estimated_completion_start, voice_estimated_completion_end);
    unsigned hour = status.eta_hour % 12U;
    if (hour == 0) hour = 12;
    write_voice_number(codec, hour, volume, control);
    if (status.eta_minute > 0) {
      if (status.eta_minute < 10) say(voice_oh_start, voice_oh_end);
      write_voice_number(codec, status.eta_minute, volume, control);
    }
    if (status.eta_hour < 12) say(voice_am_start, voice_am_end);
    else say(voice_pm_start, voice_pm_end);
    write_silence(codec, 1024);
    return;
  }
  // Every voice request kind is handled above. Keep malformed requests silent.
  write_silence(codec, 1024);
#else
  (void)kind;
  (void)status;
  (void)volume;
#endif
}

#endif

}  // namespace printdeck::platform
