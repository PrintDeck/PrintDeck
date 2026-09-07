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

#if defined(PRINTDECK_LOCAL_VOICE)
  enum class SpokenResponse : std::uint8_t {
    status,
    remaining_time,
    completion_time,
  };

  struct SpokenPrintStatus {
    bool printer_available = false;
    bool print_active = false;
    bool timing_available = false;
    bool eta_available = false;
    std::uint8_t completion_percent = 0;
    std::uint8_t eta_hour = 0;
    std::uint8_t eta_minute = 0;
    std::uint32_t elapsed_seconds = 0;
    std::uint32_t remaining_seconds = 0;
  };

#endif

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
  void set_display_volume_scale(int percent) { display_volume_scale_.store(percent); }
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
#if defined(PRINTDECK_LOCAL_VOICE)
  std::uint32_t acknowledge_voice_command();
  std::uint32_t speak_print_status(const SpokenPrintStatus& status,
                                   SpokenResponse response);
  bool voice_request_complete(std::uint32_t ticket) const;
  bool playback_active() const { return playback_active_.load(); }
#endif
  static bool preset_from_id(std::string_view id, Preset& preset);

 private:
#if defined(PRINTDECK_LOCAL_VOICE)
  enum class RequestKind : std::uint8_t {
    event,
    voice_acknowledgement,
    voice_status,
    voice_remaining_time,
    voice_completion_time,
  };

#endif
  struct Request {
    Event event;
    Preset preset;
    int volume;
    bool force;
    std::uint8_t language;
    CompletionCallback completion;
    void* completion_context;
    std::uint32_t generation;
#if defined(PRINTDECK_LOCAL_VOICE)
    RequestKind kind = RequestKind::event;
    std::uint32_t ticket = 0;
    SpokenPrintStatus status{};
#endif
  };

  static void task_entry(void* context);
  void task_loop();
  void play_now(Event event, Preset preset, int volume, bool force,
                std::uint8_t language, std::uint32_t generation);

  std::atomic<bool> enabled_{true};
  std::atomic<int> volume_{60};
  std::atomic<int> display_volume_scale_{100};
  std::atomic<Preset> preset_{Preset::modern};
  std::atomic<std::uint16_t> muted_events_{0};
  std::atomic<std::uint8_t> language_{0};
  std::atomic<bool> preview_busy_{false};
  std::atomic<std::uint32_t> playback_generation_{0};
#if defined(PRINTDECK_LOCAL_VOICE)
  void play_voice_now(RequestKind kind, const SpokenPrintStatus& status, int volume,
                      std::uint32_t generation);
  std::uint32_t queue_voice(RequestKind kind, const SpokenPrintStatus& status);
  std::atomic<std::uint32_t> next_voice_ticket_{1};
  std::atomic<std::uint32_t> completed_voice_ticket_{0};
  std::atomic<bool> playback_active_{false};
#endif
  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  void* codec_ = nullptr;
};

}  // namespace printdeck::platform
