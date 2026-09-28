#include "printdeck/platform/voice_service.hpp"
#include "printdeck/platform/image_workspace.hpp"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_timer.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "printdeck/platform/voice_model_store.hpp"

// ESP-SR 2.5.1 exports these exact interface tables. Selecting the two
// packaged models directly avoids retaining unrelated WakeNet10 and
// MultiNet6/7 engines through the generic runtime dispatchers.
extern "C" {
extern const esp_wn_iface_t printdeck_custom_wake_word;
extern const esp_wn_iface_t esp_sr_wakenet9_quantized;
extern const esp_mn_iface_t esp_sr_multinet5_quantized8;
}

// MultiNet5 also calls the generic dispatcher internally while creating its
// command grammar. Keep that callback on the single packaged English model,
// so it cannot retain MultiNet6/7 and their unused decoders. The upstream
// archives remain unmodified; this binding is scoped to the voice-enabled targets.
extern "C" esp_mn_iface_t* __wrap_esp_mn_handle_from_name(char* model_name) {
  if (model_name == nullptr || std::strcmp(model_name, "mn5q8_en") != 0) return nullptr;
  return const_cast<esp_mn_iface_t*>(&esp_sr_multinet5_quantized8);
}

namespace printdeck::platform {
namespace {
constexpr std::size_t kRecordingCapacity = 65536;
constexpr std::uint32_t kRecordingBytes = 16000 * 2 * 180;
struct RecordingBuffer {
  std::mutex mutex;
  std::uint8_t* data = nullptr;
  std::string token;
  std::size_t head = 0, size = 0;
  std::uint32_t total = 0, delivered = 0, state = 0;
  std::int64_t polled = 0, started = 0;
  bool available = false;
  void expire() {
    const auto now = esp_timer_get_time();
    if (state == 1 && now - started >= 180000000) state = 2;
    if (state == 1 && now - polled > 3000000) state = 5;
    if (data && now - polled > 10000000) {
      std::free(data); data = nullptr; size = 0; token.clear(); state = 0;
    }
  }
};
RecordingBuffer recording;
}

bool VoiceRecording::start(std::string_view token) {
  const std::lock_guard<std::mutex> lock(recording.mutex);
  recording.expire();
  if (!recording.available || (recording.state == 1 || recording.size != 0) || token.size() != 32) return false;
  if (!recording.data) recording.data = static_cast<std::uint8_t*>(
      heap_caps_malloc(kRecordingCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!recording.data) return false;
  recording.token = token;
  recording.head = recording.size = recording.total = recording.delivered = 0;
  recording.started = recording.polled = esp_timer_get_time();
  recording.state = 1;
  return true;
}
bool VoiceRecording::stop(std::string_view token) {
  const std::lock_guard<std::mutex> lock(recording.mutex);
  recording.expire();
  if (token.empty() || token != recording.token) return false;
  if (recording.state == 1) recording.state = 2;
  return true;
}
void VoiceRecording::available(bool value) {
  const std::lock_guard<std::mutex> lock(recording.mutex);
  recording.available = value;
  if (!value && recording.state == 1) recording.state = 3;
  recording.expire();
}
bool VoiceRecording::capture(const std::int16_t* samples, std::size_t count) {
  const std::lock_guard<std::mutex> lock(recording.mutex);
  recording.expire();
  if (recording.state != 1) return false;
  const auto bytes = std::min(count * sizeof(std::int16_t),
                              static_cast<std::size_t>(kRecordingBytes - recording.total));
  if (bytes > kRecordingCapacity - recording.size) { recording.state = 4; return false; }
  const auto* source = reinterpret_cast<const std::uint8_t*>(samples);
  for (std::size_t i = 0; i < bytes; ++i)
    recording.data[(recording.head + recording.size + i) % kRecordingCapacity] = source[i];
  recording.size += bytes; recording.total += bytes;
  if (recording.total == kRecordingBytes) recording.state = 2;
  return true;
}
bool VoiceRecording::read(std::string_view token, std::string& packet) {
  const std::lock_guard<std::mutex> lock(recording.mutex);
  recording.expire();
  if (token.empty() || token != recording.token) return false;
  recording.polled = esp_timer_get_time();
  const auto bytes = std::min(recording.size, std::size_t{8192});
  packet.resize(16 + bytes);
  const std::uint32_t fields[] = {0x31524450, recording.state, recording.delivered,
                                static_cast<std::uint32_t>(recording.size - bytes)};
  for (unsigned i = 0; i < 4; ++i)
    for (unsigned j = 0; j < 4; ++j) packet[i*4+j] = static_cast<char>(fields[i] >> (j*8));
  for (std::size_t i = 0; i < bytes; ++i)
    packet[16+i] = recording.data[(recording.head+i) % kRecordingCapacity];
  recording.head = (recording.head+bytes) % kRecordingCapacity;
  recording.size -= bytes; recording.delivered += bytes;
  if (recording.state != 1 && !recording.size && recording.data) {
    std::free(recording.data); recording.data = nullptr;
  }
  return true;
}

namespace {

constexpr char kLogTag[] = "voice";
constexpr char kCommandModel[] = "mn5q8_en";
constexpr int kSampleRate = 16000;
constexpr int kCommandTimeoutMs = 7000;
constexpr float kMicrophoneGainDb = 36.0F;
constexpr det_mode_t kWakeDetectionMode = DET_MODE_95;
constexpr int kStatusCommand = 1;
constexpr int kRemainingTimeCommand = 2;
constexpr int kCompletionTimeCommand = 3;

struct CommandAlias {
  int command_id;
  const char* phrase;
  const char* phonemes;
};

// MultiNet5 Q8 consumes Espressif's compact phoneme alphabet. These values
// were generated by esp-sr/tool/multinet_g2p.py; several natural phrases map
// to the three concise application intents.
constexpr std::array<CommandAlias, 35> kAliases{{
    {kStatusCommand, "status", "STaTcS"},
    {kStatusCommand, "print status", "PRgNT STaTcS"},
    {kStatusCommand, "printer status", "PRgNTk STaTcS"},
    {kStatusCommand, "printing status", "PRgNTgl STaTcS"},
    {kStatusCommand, "how is the print", "ht gZ jc PRgNT"},
    {kStatusCommand, "how is my print", "ht gZ Mi PRgNT"},
    {kStatusCommand, "how is the printer", "ht gZ jc PRgNTk"},
    {kStatusCommand, "what is printing", "WcT gZ PRgNTgl"},
    {kStatusCommand, "what is the print status", "WcT gZ jc PRgNT STaTcS"},
    {kStatusCommand, "print progress", "PRgNT PRnGRfS"},
    {kStatusCommand, "printing progress", "PRgNTgl PRnGRfS"},
    {kStatusCommand, "how far along is the print", "ht FnR cLel gZ jc PRgNT"},
    {kStatusCommand, "how far is the print", "ht FnR gZ jc PRgNT"},
    {kStatusCommand, "give me the print status", "GgV Mm jc PRgNT STaTcS"},
    {kRemainingTimeCommand, "how much time is left", "ht Mcp TiM gZ LfFT"},
    {kRemainingTimeCommand, "how much time left", "ht Mcp TiM LfFT"},
    {kRemainingTimeCommand, "time left", "TiM LfFT"},
    {kRemainingTimeCommand, "remaining time", "RgMdNgl TiM"},
    {kRemainingTimeCommand, "print time left", "PRgNT TiM LfFT"},
    {kCompletionTimeCommand, "when will it finish", "WfN WgL gT FgNgs"},
    {kCompletionTimeCommand, "when will the print finish", "WfN WgL jc PRgNT FgNgs"},
    {kCompletionTimeCommand, "when is the print done", "WfN gZ jc PRgNT DcN"},
    {kCompletionTimeCommand, "when will it be done", "WfN WgL gT Bm DcN"},
    {kCompletionTimeCommand, "when it will be done", "WfN gT WgL Bm DcN"},
    {kCompletionTimeCommand, "what time will it be done", "WcT TiM WgL gT Bm DcN"},
    {kCompletionTimeCommand, "what time will the print be done", "WcT TiM WgL jc PRgNT Bm DcN"},
    {kCompletionTimeCommand, "what time does it finish", "WcT TiM DcZ gT FgNgs"},
    {kCompletionTimeCommand, "estimated completion time", "fSTcMdTcD KcMPLmscN TiM"},
    {kCompletionTimeCommand, "estimated time of completion", "fSTcMdTcD TiM cV KcMPLmscN"},
    {kCompletionTimeCommand, "what is the E T A", "WcT gZ jc m Tm c"},
    {kCompletionTimeCommand, "give me the E T A", "GgV Mm jc m Tm c"},
    {kCompletionTimeCommand, "print E T A", "PRgNT m Tm c"},
    {kRemainingTimeCommand, "how long until it is done", "ht Lel cNTgL gT gZ DcN"},
    {kRemainingTimeCommand, "how long until the print is done", "ht Lel cNTgL jc PRgNT gZ DcN"},
    {kCompletionTimeCommand, "when is it done", "WfN gZ gT DcN"},
}};

}  // namespace

esp_err_t VoiceService::start(AudioService& audio, WakeCallback wake, void* context, bool custom_wake_word) {
  // Runtime is the single caller. A stopping worker retains ownership until all
  // models and the microphone have been released; a later pass can start again.
  if (running_.load()) return ESP_OK;
  if (!wanted(true, audio.enabled(), audio.volume())) return ESP_ERR_INVALID_STATE;
  custom_wake_word_ = custom_wake_word;
  wake_phrase_ = custom_wake_word_ ? "Hey PrintDeck" : "Hi ESP";
  wakenet_model_name_ = custom_wake_word_ ? "hey-printdeck" : "wn9_hiesp";
  audio_ = &audio;
  wake_callback_ = wake;
  wake_context_ = context;
  stop_requested_.store(false);
  ready_.store(false);
  resources_released_.store(false);
  running_.store(true);
  // Mapping and unmapping the model partition briefly disable the flash/PSRAM
  // cache. This worker therefore needs an internal stack; model data stays in
  // PSRAM and is still expanded one model at a time.
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "voice", 8192, this, 5,
                                      &task_, kServiceCore,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
    running_.store(false);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t VoiceService::initialize_resources() {
  // Only the enabled core-0 worker opens the microphone and expands models.
  if (stop_requested_.load() || !wanted(true, audio_->enabled(), audio_->volume()))
    return ESP_ERR_INVALID_STATE;
  ImageWorkspaceLock workspace(5000);
  if (!workspace) return ESP_ERR_NO_MEM;
  if (stop_requested_.load() || !wanted(true, audio_->enabled(), audio_->volume()))
    return ESP_ERR_INVALID_STATE;
  microphone_ = board_audio_codec_microphone_init();
  if (microphone_ == nullptr) return ESP_FAIL;
  auto microphone = static_cast<esp_codec_dev_handle_t>(microphone_);
  esp_codec_dev_sample_info_t sample{};
  sample.bits_per_sample = 16;
  sample.channel = 1;
  sample.sample_rate = kSampleRate;
  if (esp_codec_dev_open(microphone, &sample) != ESP_CODEC_DEV_OK) return ESP_FAIL;
  if (esp_codec_dev_set_in_gain(microphone, kMicrophoneGainDb) != ESP_CODEC_DEV_OK) {
    ESP_LOGW(kLogTag, "Microphone gain could not be set; using codec default");
  }
  if (stop_requested_.load()) return ESP_ERR_INVALID_STATE;
  const esp_err_t result = open_voice_models();
  if (result != ESP_OK) return result;
  if (!custom_wake_word_ && !activate_voice_model(wakenet_model_name_)) return ESP_FAIL;
  if (stop_requested_.load()) return ESP_ERR_INVALID_STATE;
  multinet_model_name_ = kCommandModel;
  const esp_wn_iface_t* wakenet = custom_wake_word_ ? &printdeck_custom_wake_word : &esp_sr_wakenet9_quantized;
  wakenet_interface_ = wakenet;
  multinet_interface_ = &esp_sr_multinet5_quantized8;
  auto* data = wakenet->create(wakenet_model_name_, kWakeDetectionMode);
  wakenet_data_ = data;
  if (data == nullptr) return ESP_ERR_NO_MEM;
  frame_samples_ = wakenet->get_samp_chunksize(data);
  if (wakenet->get_samp_rate(data) != kSampleRate || frame_samples_ <= 0)
    return ESP_ERR_NOT_SUPPORTED;
  return ESP_OK;
}

void VoiceService::update_status(const AudioService::SpokenPrintStatus& status) {
  std::lock_guard lock(status_mutex_);
  status_ = status;
}

void VoiceService::task_entry(void* context) {
  auto* service = static_cast<VoiceService*>(context);
  const esp_err_t result = service->initialize_resources();
  if (result != ESP_OK || service->stop_requested_.load()) {
    if (!service->stop_requested_.load())
      ESP_LOGW(kLogTag, "Local voice initialization failed: %s", esp_err_to_name(result));
    service->finish_task();
  }
  service->task_loop();
}

bool VoiceService::activate_wakenet() {
  auto wakenet = static_cast<const esp_wn_iface_t*>(wakenet_interface_);
  // A resident WakeNet instance is already in the listening state. Calling
  // clean() while the audio codec is playing is unnecessary and is unsafe
  // when another subsystem is simultaneously reserving internal DMA memory.
  if (wakenet_data_ != nullptr) {
    if (custom_wake_word_) wakenet->clean(static_cast<model_iface_data_t*>(wakenet_data_));
    return true;
  }
  ImageWorkspaceLock workspace(5000);
  if (!workspace || stop_requested_.load()) return false;
  deactivate_multinet();
  if (custom_wake_word_) {
    release_active_voice_model();
  } else if (!activate_voice_model(wakenet_model_name_)) return false;
  auto* data = wakenet->create(wakenet_model_name_, kWakeDetectionMode);
  if (data == nullptr || wakenet->get_samp_rate(data) != kSampleRate ||
      wakenet->get_samp_chunksize(data) != frame_samples_) {
    if (data != nullptr) wakenet->destroy(data);
    ESP_LOGE(kLogTag, "Could not restore the wake-word recognizer");
    return false;
  }
  wakenet_data_ = data;
  return true;
}

void VoiceService::deactivate_multinet() {
  auto multinet = static_cast<const esp_mn_iface_t*>(multinet_interface_);
  if (multinet_data_ == nullptr) return;
  // MultiNet owns and releases the active command grammar when destroyed.
  multinet->destroy(static_cast<model_iface_data_t*>(multinet_data_));
  multinet_data_ = nullptr;
}

bool VoiceService::activate_multinet() {
  if (multinet_data_ != nullptr) return true;
  ImageWorkspaceLock workspace(5000);
  if (!workspace || stop_requested_.load()) return false;
  auto wakenet = static_cast<const esp_wn_iface_t*>(wakenet_interface_);
  auto multinet = static_cast<const esp_mn_iface_t*>(multinet_interface_);
  if (wakenet_data_ != nullptr) {
    wakenet->destroy(static_cast<model_iface_data_t*>(wakenet_data_));
    wakenet_data_ = nullptr;
  }

  if (!activate_voice_model(multinet_model_name_)) {
    ESP_LOGE(kLogTag, "Could not expand the English command model");
    activate_wakenet();
    return false;
  }

  model_iface_data_t* data = multinet->create(multinet_model_name_, kCommandTimeoutMs);
  if (data == nullptr) {
    ESP_LOGE(kLogTag, "Could not load the English command recognizer");
    activate_wakenet();
    return false;
  }
  if (multinet->get_samp_rate(data) != kSampleRate ||
      multinet->get_samp_chunksize(data) != frame_samples_ ||
      esp_mn_commands_alloc(multinet, data) != ESP_OK) {
    ESP_LOGE(kLogTag, "Unsupported MultiNet input format or command allocation failure");
    multinet->destroy(data);
    activate_wakenet();
    return false;
  }

  unsigned aliases_added = 0;
  for (const CommandAlias& alias : kAliases) {
    if (esp_mn_commands_phoneme_add(alias.command_id, alias.phrase, alias.phonemes) == ESP_OK) {
      ++aliases_added;
    } else {
      ESP_LOGW(kLogTag, "Rejected command alias: %s", alias.phrase);
    }
  }
  esp_mn_error_t* command_errors = esp_mn_commands_update();
  if (aliases_added != kAliases.size() ||
      (command_errors != nullptr && command_errors->num > 0)) {
    ESP_LOGE(kLogTag, "English command grammar could not be activated");
    multinet->destroy(data);
    activate_wakenet();
    return false;
  }
  multinet_data_ = data;
  return true;
}

bool VoiceService::return_to_wake_word() {
  if (activate_wakenet()) return true;
  stop_requested_.store(true);
  ready_.store(false);
  return false;
}

void VoiceService::release_resources() {
  VoiceRecording::available(false);
  ready_.store(false);
  deactivate_multinet();
  if (wakenet_data_ != nullptr) {
    auto wakenet = static_cast<const esp_wn_iface_t*>(wakenet_interface_);
    wakenet->destroy(static_cast<model_iface_data_t*>(wakenet_data_));
    wakenet_data_ = nullptr;
  }
  close_voice_models();
  if (microphone_ != nullptr) {
    esp_codec_dev_close(static_cast<esp_codec_dev_handle_t>(microphone_));
    microphone_ = nullptr;
  }
  ESP_LOGI(kLogTag, "Local voice stopped; microphone and models released");
}

void VoiceService::finish_task() {
  release_resources();
  resources_released_.store(true, std::memory_order_release);
  // The owner reaps externally. Self-deletion WithCaps would allocate a
  // temporary internal cleanup task exactly when memory is under pressure.
  for (;;) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
}

void VoiceService::reap_stopped() {
  if (!resources_released_.load(std::memory_order_acquire)) return;
  if (task_ != nullptr) {
    vTaskDeleteWithCaps(task_);
    task_ = nullptr;
  }
  resources_released_.store(false, std::memory_order_release);
  running_.store(false, std::memory_order_release);
}

std::uint32_t VoiceService::speak_command(int command_id) {
  AudioService::SpokenPrintStatus status;
  {
    std::lock_guard lock(status_mutex_);
    status = status_;
  }
  AudioService::SpokenResponse response = AudioService::SpokenResponse::status;
  if (command_id == kRemainingTimeCommand) {
    response = AudioService::SpokenResponse::remaining_time;
  } else if (command_id == kCompletionTimeCommand) {
    response = AudioService::SpokenResponse::completion_time;
  }
  return audio_->speak_print_status(status, response);
}

void VoiceService::task_loop() {
  auto microphone = static_cast<esp_codec_dev_handle_t>(microphone_);
  auto wakenet = static_cast<const esp_wn_iface_t*>(wakenet_interface_);
  auto multinet = static_cast<const esp_mn_iface_t*>(multinet_interface_);
  const int samples = frame_samples_;
  auto* input = static_cast<std::int16_t*>(
      heap_caps_malloc(static_cast<std::size_t>(samples) * sizeof(std::int16_t),
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (input == nullptr) {
    ESP_LOGE(kLogTag, "No internal DMA buffer for microphone");
    finish_task();
  }
  ready_.store(true);
  VoiceRecording::available(true);

  ListenState state = ListenState::wake_word;
  std::uint32_t acknowledgement_ticket = 0;
  std::uint32_t reply_ticket = 0;
  std::int64_t deadline_us = 0;
  bool suppressed_for_playback = false;
  bool suppressed_for_recording = false;
  unsigned consecutive_errors = 0;
  ESP_LOGI(kLogTag, "Local voice ready: %s, %u English aliases", wake_phrase_,
           static_cast<unsigned>(kAliases.size()));
  while (!stop_requested_.load() && wanted(true, audio_->enabled(), audio_->volume())) {
    const int read_result = esp_codec_dev_read(
        microphone, input, samples * static_cast<int>(sizeof(std::int16_t)));
    if (read_result != ESP_CODEC_DEV_OK) {
      if (++consecutive_errors == 1 || consecutive_errors % 100 == 0) {
        ESP_LOGE(kLogTag, "Microphone read failed: %d", read_result);
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    consecutive_errors = 0;
    if (stop_requested_.load() || !wanted(true, audio_->enabled(), audio_->volume())) break;

    if (VoiceRecording::capture(input, static_cast<std::size_t>(samples))) {
      suppressed_for_recording = true;
      continue;
    }
    if (suppressed_for_recording) {
      suppressed_for_recording = false;
      suppressed_for_playback = false;
      state = ListenState::wake_word;
      if (!return_to_wake_word()) break;
    }
    const bool playing = audio_->playback_active();
    if (playing && (state == ListenState::wake_word || state == ListenState::command)) {
      if (!suppressed_for_playback && !return_to_wake_word()) break;
      suppressed_for_playback = true;
      state = ListenState::wake_word;
      continue;
    }
    if (!playing && suppressed_for_playback) {
      suppressed_for_playback = false;
      if (!return_to_wake_word()) break;
    }

    const std::int64_t now_us = esp_timer_get_time();
    if (state == ListenState::waiting_for_acknowledgement) {
      if (audio_->voice_request_complete(acknowledgement_ticket)) {
        multinet->clean(static_cast<model_iface_data_t*>(multinet_data_));
        state = ListenState::command;
        deadline_us = now_us + static_cast<std::int64_t>(kCommandTimeoutMs) * 1000;
        ESP_LOGI(kLogTag, "Listening for local command");
      } else if (now_us >= deadline_us) {
        state = ListenState::wake_word;
        return_to_wake_word();
      }
      continue;
    }
    if (state == ListenState::waiting_for_reply) {
      if (audio_->voice_request_complete(reply_ticket) || now_us >= deadline_us) {
        state = ListenState::wake_word;
        return_to_wake_word();
      }
      continue;
    }
    if (state == ListenState::wake_word) {
      const auto wake_result = wakenet->detect(static_cast<model_iface_data_t*>(wakenet_data_), input);
      // The single-channel adapter reserves this result for a fatal inference
      // failure, so the owner can restart instead of advertising a dead listener.
      if (custom_wake_word_ && wake_result == WAKENET_CHANNEL_VERIFIED) break;
      if (wake_result == WAKENET_DETECTED) {
        if (stop_requested_.load()) break;
        ESP_LOGI(kLogTag, "%s detected", wake_phrase_);
        if (wake_callback_ != nullptr) wake_callback_(wake_context_);
        acknowledgement_ticket = audio_->acknowledge_voice_command();
        if (acknowledgement_ticket == 0 || !activate_multinet()) {
          return_to_wake_word();
        } else {
          state = ListenState::waiting_for_acknowledgement;
          deadline_us = esp_timer_get_time() + 5'000'000;
        }
      }
      continue;
    }

    auto* multinet_data = static_cast<model_iface_data_t*>(multinet_data_);
    const esp_mn_state_t detected = multinet->detect(multinet_data, input);
    if (detected == ESP_MN_STATE_DETECTED) {
      esp_mn_results_t* commands = multinet->get_results(multinet_data);
      if (commands != nullptr && commands->num > 0 &&
          (commands->command_id[0] == kStatusCommand ||
           commands->command_id[0] == kRemainingTimeCommand ||
           commands->command_id[0] == kCompletionTimeCommand)) {
        const char* intent = commands->command_id[0] == kStatusCommand
                                 ? "status"
                             : commands->command_id[0] == kRemainingTimeCommand
                                 ? "remaining-time"
                                 : "completion-time";
        ESP_LOGI(kLogTag, "Recognized %s intent (confidence %.2f)",
                 intent, static_cast<double>(commands->prob[0]));
        reply_ticket = speak_command(commands->command_id[0]);
        if (reply_ticket != 0) {
          state = ListenState::waiting_for_reply;
          deadline_us = now_us + 20'000'000;
          continue;
        }
      }
      state = ListenState::wake_word;
      return_to_wake_word();
    } else if (detected == ESP_MN_STATE_TIMEOUT || now_us >= deadline_us) {
      ESP_LOGI(kLogTag, "Command window timed out");
      state = ListenState::wake_word;
      return_to_wake_word();
    }
  }
  std::memset(input, 0, static_cast<std::size_t>(samples) * sizeof(std::int16_t));
  free(input);
  finish_task();
}

}  // namespace printdeck::platform
