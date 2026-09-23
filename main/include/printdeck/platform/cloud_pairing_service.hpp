#pragma once
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"

namespace printdeck::platform {
// Cloud credentials are deliberately separate from exportable device settings.
class CloudPairingService {
 public:
  ~CloudPairingService();
  using CommandSink = bool (*)(void*, const std::string&);
  void set_command_sink(CommandSink sink);
  using ReactionUpload = bool (*)(void*, const std::string&, const std::uint8_t*, std::size_t, bool);
  void set_reaction_upload(ReactionUpload callback) { reaction_upload_ = callback; }
  using FeedSource = std::string (*)(void*);
  void set_feed_source(FeedSource source, void* context);
  struct Thumbnail {
    std::uint32_t printer_id = 0;
    std::string key;
    std::shared_ptr<std::vector<std::uint8_t>> image;
  };
  using ThumbnailSource = Thumbnail (*)(void*);
  void set_thumbnail_source(ThumbnailSource source);
  using ScreenSource = bool (*)(void*, std::vector<std::uint8_t>&);
  using ScreenInput = bool (*)(void*, const std::string&, int, int, int, int);
  using ScreenBusy = bool (*)(void*);
  void set_screen_source(ScreenSource source, ScreenInput input, ScreenBusy busy);
  bool preview_enabled() const;
  void initialize();
  bool set_developer_mode(bool enabled, bool& changed, const std::string& host = {}, unsigned port = 0);
  void tick(bool online);
  bool request(const std::string& action, const std::string& id,
               const std::string& name, const std::string& locale);
  std::string status_json() const;
  void pause(bool paused);
 private:
  static void entry(void* context);
  void run();
  void step();
  void close_http();
  void load_credential();
  std::string local_host_, local_api_, local_panel_;
  unsigned local_port_ = 8000;
  bool developer_mode_ = false;
  bool reset_transport_ = false;
  esp_http_client_handle_t http_client_ = nullptr; // Owned exclusively by the cloud worker.
  std::int64_t feed_due_ = 0;
  unsigned poll_interval_ms_ = 0;

  void stop_screen();
  void screen_directive(const std::string& payload, std::int64_t started, std::uint32_t input_frame = UINT32_MAX);
  void upload_screen();
  ScreenSource screen_source_ = nullptr;
  ScreenInput screen_input_ = nullptr;
  ScreenBusy screen_busy_ = nullptr;
  std::string screen_session_, screen_closed_, screen_input_id_, screen_result_;
  std::int64_t screen_expires_ = 0, screen_due_ = 0, screen_settle_ = 0;
  std::uint32_t screen_seq_ = 0;
  bool screen_waiting_ = false;
  void upload_thumbnail(std::uint32_t printer, const std::string& key, std::uint32_t generation);
  ThumbnailSource thumbnail_source_ = nullptr;
  std::string thumbnail_key_;
  unsigned thumbnail_attempts_ = 0;
  std::int64_t thumbnail_due_ = 0;
  bool save(const std::string& token, const std::string& account, bool disconnect);
  mutable std::mutex mutex_;
  TaskHandle_t task_ = nullptr;
  FeedSource feed_source_ = nullptr;
  void* feed_context_ = nullptr;
  CommandSink command_sink_ = nullptr;
  ReactionUpload reaction_upload_ = nullptr;
  bool download_reaction(const std::string& base, const std::string& token, bool local, const std::string& asset,
                         std::size_t size, const std::string& hash, const std::string& payload, std::uint32_t generation);
  std::string last_command_id_, last_command_status_;
  bool result_pending_ = false;
  bool confirmed_ = false;
  unsigned feed_failures_ = 0;
  bool online_ = false, paused_ = false, disconnect_ = false, initialized_ = false;
  std::uint32_t generation_ = 0;
  std::int64_t due_ = 0, expires_ = 0;
  std::string state_ = "disconnected", command_, token_, account_, secret_, code_, link_;
  std::string id_, name_, locale_;
};
} // namespace printdeck::platform
