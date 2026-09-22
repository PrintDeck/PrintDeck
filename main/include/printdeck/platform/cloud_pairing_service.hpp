#pragma once
#include <mutex>
#include <string>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace printdeck::platform {
// Cloud credentials are deliberately separate from exportable device settings.
class CloudPairingService {
 public:
  using CommandSink = bool (*)(void*, const std::string&);
  void set_command_sink(CommandSink sink);
  using FeedSource = std::string (*)(void*);
  void set_feed_source(FeedSource source, void* context);
  void initialize();
  void tick(bool online);
  bool request(const std::string& action, const std::string& id,
               const std::string& name, const std::string& locale);
  std::string status_json() const;
  void pause(bool paused);
 private:
  static void entry(void* context);
  void run();
  void step();
  bool save(const std::string& token, const std::string& account, bool disconnect);
  mutable std::mutex mutex_;
  TaskHandle_t task_ = nullptr;
  FeedSource feed_source_ = nullptr;
  void* feed_context_ = nullptr;
  CommandSink command_sink_ = nullptr;
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
