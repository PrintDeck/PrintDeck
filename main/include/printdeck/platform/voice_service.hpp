#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/platform/audio_service.hpp"

namespace printdeck::platform {

// Fully local wake-word and fixed-grammar voice control. Audio
// samples never leave the device and no network service is used by this class.
class VoiceService {
 public:
  esp_err_t start(AudioService& audio);
  void update_status(const AudioService::SpokenPrintStatus& status);
  void set_power_suspended(bool suspended) { power_suspended_.store(suspended); }
  bool ready() const { return ready_.load(); }

 private:
  enum class ListenState : std::uint8_t {
    wake_word,
    waiting_for_acknowledgement,
    command,
    waiting_for_reply,
  };

  static void task_entry(void* context);
  void task_loop();
  bool activate_wakenet();
  bool activate_multinet();
  void deactivate_multinet();
  bool return_to_wake_word();
  void release_resources();
  std::uint32_t speak_command(int command_id);

  AudioService* audio_ = nullptr;
  void* microphone_ = nullptr;
  const char* wakenet_model_name_ = nullptr;
  const char* multinet_model_name_ = nullptr;
  const void* wakenet_interface_ = nullptr;
  void* wakenet_data_ = nullptr;
  const void* multinet_interface_ = nullptr;
  void* multinet_data_ = nullptr;
  int frame_samples_ = 0;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> power_suspended_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> ready_{false};
  std::mutex status_mutex_;
  AudioService::SpokenPrintStatus status_;
};

}  // namespace printdeck::platform
