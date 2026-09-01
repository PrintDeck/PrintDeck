#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_peer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/device_state.hpp"

namespace printdeck::platform {

struct MoonrakerCameraSnapshot {
  bool supported = false;
  bool live_supported = false;
  bool connected = false;
  bool refreshing = false;
  std::string detail = "Camera off";
  std::shared_ptr<std::vector<std::uint8_t>> frame;
  std::uint16_t width = 0;
  std::uint16_t height = 0;
};

// Camera transport is deliberately separate from the Moonraker status
// adapter: individual vendors may expose entirely different camera APIs.
// The first backends cover Snapmaker U1 stock firmware and PAXX firmware.
class MoonrakerCameraClient {
 public:
  void configure(const core::PrinterProfile* profile);
  void set_network_ready(bool ready);
  void set_enabled(bool enabled);
  void set_mode(bool live, int snapshot_fps);
  esp_err_t start();
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }
  MoonrakerCameraSnapshot snapshot() const;

 private:
  enum class Backend : std::uint8_t {
    unknown,
    generic_snapshot,
    paxx_snapshot,
    snapmaker_stock,
    creality_webrtc,
  };
  static void task_entry(void* context);
  static void decoder_task_entry(void* context);
  void finish_task(bool decoder);
  void task_loop();
  void decoder_loop();
  core::PrinterProfile profile() const;
  bool fetch_frame(const core::PrinterProfile& profile, const char* path);
  bool detect_backend(const core::PrinterProfile& profile);
  bool send_stock_command(const core::PrinterProfile& profile, bool start);
  bool start_creality_peer(const core::PrinterProfile& profile);
  void stop_creality_peer();
  bool exchange_creality_offer(const core::PrinterProfile& profile);
  bool decode_creality_frame(const std::uint8_t* data, std::size_t size,
                             std::uint32_t pts);
  static int peer_state_callback(esp_peer_state_t state, void* context);
  static int peer_message_callback(esp_peer_msg_t* message, void* context);
  static int peer_video_info_callback(esp_peer_video_stream_info_t* info, void* context);
  static int peer_video_callback(esp_peer_video_frame_t* frame, void* context);
  void publish_status(bool connected, const char* detail, bool clear_frame = false);
  void set_refreshing(bool refreshing);
  void publish_frame(std::shared_ptr<std::vector<std::uint8_t>> frame,
                     std::uint16_t width, std::uint16_t height);

  mutable std::mutex profile_mutex_{};
  core::PrinterProfile profile_{};
  mutable std::mutex snapshot_mutex_{};
  MoonrakerCameraSnapshot snapshot_{};
  TaskHandle_t task_ = nullptr;
  TaskHandle_t decoder_task_ = nullptr;
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> enabled_{false};
  std::atomic<bool> reconfigure_requested_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint8_t> active_tasks_{0};
  mutable std::mutex task_mutex_{};
  std::atomic<Backend> backend_{Backend::unknown};
  std::atomic<bool> live_mode_{false};
  std::atomic<int> snapshot_fps_{1};
  void* peer_ = nullptr;
  void* h264_decoder_ = nullptr;
  std::mutex peer_mutex_{};
  std::string pending_offer_{};
  std::atomic<bool> offer_ready_{false};
  std::atomic<bool> peer_connected_{false};
  std::atomic<bool> frame_received_{false};
  std::atomic<bool> h264_parameter_sets_sent_{false};
  std::atomic<bool> idr_snapshot_decoder_{false};
  std::atomic<bool> creality_decoder_busy_{false};
  std::atomic<std::uint32_t> creality_idr_count_{0};
  std::mutex pending_idr_mutex_{};
  std::shared_ptr<std::vector<std::uint8_t>> pending_idr_{};
  std::uint32_t pending_idr_generation_ = 0;
  std::atomic<std::uint32_t> creality_session_generation_{0};
  std::atomic<std::uint32_t> video_callback_count_{0};
  std::atomic<std::uint64_t> last_published_frame_us_{0};
  std::atomic<std::uint64_t> last_creality_idr_queued_us_{0};
  std::atomic<std::uint64_t> last_creality_video_us_{0};
  std::uint8_t creality_codec_attempt_ = 0;
  std::string snapshot_path_{};
  std::string creality_signal_url_{};
};

}  // namespace printdeck::platform
