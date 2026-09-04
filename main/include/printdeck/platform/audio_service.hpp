#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace printdeck::platform {

class AudioService {
 public:
  using CompletionCallback = void (*)(void* context);

  enum class Preset : std::uint8_t {
    modern,
    soft,
    oldschool,
    arcade,
    scifi,
    clean,
  };

  enum class Event : std::uint8_t {
    startup,
    navigation,
    orientation,
    print_started,
    progress_25,
    progress_50,
    progress_75,
    print_paused,
    print_finished,
    print_error,
    hms_alert,
    filament_attention,
    shutdown_countdown,
    shutdown,
    test,
    restarting,
  };

  esp_err_t start(bool enabled, int volume_percent, std::string_view preset_id,
                  std::uint16_t muted_events);
  void set_enabled(bool enabled);
  void set_volume(int percent);
  void set_preset(Preset preset);
  void set_muted_events(std::uint16_t muted_events);
  void set_language(std::string_view language);
  bool enabled() const { return enabled_.load(); }
  int volume() const { return volume_.load(); }
  Preset preset() const { return preset_.load(); }
  std::uint16_t muted_events() const { return muted_events_.load(); }
  bool play(Event event);
  bool play(Event event, CompletionCallback completion, void* context);
  bool play(Event event, Preset preset);
  bool preview(Event event, Preset preset, int volume_percent);
  static bool preset_from_id(std::string_view id, Preset& preset);

 private:
  struct Request {
    Event event;
    Preset preset;
    int volume;
    bool force;
    std::uint8_t language;
    CompletionCallback completion;
    void* completion_context;
  };

  static void task_entry(void* context);
  void task_loop();
  void play_now(Event event, Preset preset, int volume, bool force,
                std::uint8_t language);

  std::atomic<bool> enabled_{true};
  std::atomic<int> volume_{60};
  std::atomic<Preset> preset_{Preset::modern};
  std::atomic<std::uint16_t> muted_events_{0};
  std::atomic<std::uint8_t> language_{0};
  std::atomic<bool> preview_busy_{false};
  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  void* codec_ = nullptr;
};

}  // namespace printdeck::platform
