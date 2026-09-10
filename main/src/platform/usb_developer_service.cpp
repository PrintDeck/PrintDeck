#include "printdeck/platform/image_workspace.hpp"
#include "printdeck/platform/usb_developer_service.hpp"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "printdeck/platform/display_shell.hpp"
#include "printdeck/platform/task_affinity.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "usb_dev";
constexpr std::string_view kEnableCommand = "PRINTDECK.DEV ENABLE SCREEN-CAPTURE/1";
constexpr std::string_view kCaptureCommand = "PRINTDECK.DEV SCREENSHOT";
constexpr std::string_view kProfileCommand = "PRINTDECK.DEV PROFILE";
constexpr std::string_view kNavigatePrefix = "PRINTDECK.DEV NAVIGATE ";
constexpr std::uint64_t kSessionDurationUs = 5ULL * 60ULL * 1'000'000ULL;
constexpr std::array<std::uint8_t, 8> kFrameMagic = {'P', 'D', 'S', 'C', 'R', 'N', '2', '\0'};
constexpr std::array<std::uint8_t, 4> kFrameEnd = {'P', 'D', 'O', 'K'};
constexpr std::size_t kMaximumCommandLength = 96;
std::atomic<const char*> s_status{"not-started"};

void append_u32_le(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void append_u16_le(std::vector<std::uint8_t>& output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

bool valid_screen_name(std::string_view name) {
  if (name.empty() || name.size() > 63 || name.front() == '-' || name.back() == '-') {
    return false;
  }
  for (const char character : name) {
    if ((character < 'a' || character > 'z') &&
        (character < '0' || character > '9') && character != '-') {
      return false;
    }
  }
  return true;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

bool write_all(const std::uint8_t* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const std::size_t chunk = std::min<std::size_t>(size - sent, 1536U);
    const int result = usb_serial_jtag_write_bytes(
        data + sent, chunk, pdMS_TO_TICKS(500));
    if (result > 0) {
      sent += static_cast<std::size_t>(result);
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    return false;
  }
  return usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000)) == ESP_OK;
}

bool write_text(std::string_view text) {
  return write_all(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

bool send_runtime_profile() {
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
  constexpr UBaseType_t maximum_tasks = 96;
  struct Snapshot {
    TaskStatus_t tasks[maximum_tasks];
    char names[maximum_tasks][configMAX_TASK_NAME_LEN];
    UBaseType_t count;
    configRUN_TIME_COUNTER_TYPE total;
  };
  const auto internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const auto internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const auto psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const auto psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  using OwnedSnapshot = std::unique_ptr<Snapshot, decltype(&heap_caps_free)>;
  OwnedSnapshot before(static_cast<Snapshot*>(heap_caps_calloc(
      1, sizeof(Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
  OwnedSnapshot after(static_cast<Snapshot*>(heap_caps_calloc(
      1, sizeof(Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
  if (!before || !after) return write_text("PRINTDECK.DEV ERROR PROFILE MEMORY\n");
  const auto sample = [](Snapshot& snapshot) {
    // All service-task creation/deletion belongs to core 0. Copy task names
    // before resuming that scheduler; task-name pointers must not be retained
    // during the measurement or USB transmission.
    vTaskSuspendAll();
    snapshot.count = uxTaskGetSystemState(snapshot.tasks, maximum_tasks, &snapshot.total);
    for (UBaseType_t i = 0; i < snapshot.count; ++i) {
      std::snprintf(snapshot.names[i], sizeof(snapshot.names[i]), "%s",
                    snapshot.tasks[i].pcTaskName);
    }
    xTaskResumeAll();
  };
  if (!write_text("PRINTDECK.DEV PROFILE BEGIN\n")) return false;
  sample(*before);
  if (before->count == 0) return write_text("PRINTDECK.DEV ERROR PROFILE TASK-LIMIT\n");
  vTaskDelay(pdMS_TO_TICKS(5000));
  sample(*after);
  const configRUN_TIME_COUNTER_TYPE elapsed = after->total - before->total;
  if (after->count == 0 || elapsed == 0)
    return write_text("PRINTDECK.DEV ERROR PROFILE SAMPLE\n");
  char line[240];
  std::snprintf(line, sizeof(line),
                "PRINTDECK.DEV PROFILE HEAP internal=%u internal_largest=%u psram=%u psram_largest=%u window_us=%llu\n",
                static_cast<unsigned>(internal_free), static_cast<unsigned>(internal_largest),
                static_cast<unsigned>(psram_free), static_cast<unsigned>(psram_largest),
                static_cast<unsigned long long>(elapsed));
  if (!write_text(line)) return false;
  for (UBaseType_t i = 0; i < after->count; ++i) {
    const TaskStatus_t& current = after->tasks[i];
    configRUN_TIME_COUNTER_TYPE previous = 0;
    for (UBaseType_t j = 0; j < before->count; ++j) {
      if (before->tasks[j].xTaskNumber == current.xTaskNumber &&
          before->tasks[j].xHandle == current.xHandle) {
        previous = before->tasks[j].ulRunTimeCounter;
        break;
      }
    }
    const configRUN_TIME_COUNTER_TYPE consumed = current.ulRunTimeCounter - previous;
    const auto basis_points = static_cast<unsigned>(
        static_cast<std::uint64_t>(consumed) * 10000ULL / elapsed);
    int core = -1;
#if configTASKLIST_INCLUDE_COREID
    core = static_cast<int>(current.xCoreID);
#endif
    std::snprintf(line, sizeof(line),
                  "PRINTDECK.DEV PROFILE TASK name=%s core=%d priority=%u cpu_bp=%u stack_free=%u\n",
                  after->names[i], core, static_cast<unsigned>(current.uxCurrentPriority),
                  basis_points, static_cast<unsigned>(current.usStackHighWaterMark * sizeof(StackType_t)));
    if (!write_text(line)) return false;
  }
  return write_text("PRINTDECK.DEV PROFILE END\n");
#else
  return write_text("PRINTDECK.DEV ERROR PROFILE STATS-DISABLED\n");
#endif
}

}  // namespace

const char* usb_developer_status() { return s_status.load(); }

esp_err_t UsbDeveloperService::start(DisplayShell& display) {
  if (task_ != nullptr) return ESP_OK;
  display_ = &display;

  usb_serial_jtag_driver_config_t config = {
      // The developer channel is idle almost all of the time. Keep its
      // permanent DMA queues deliberately small so normal printer workers can
      // still reserve their stacks. Screenshot writes may block briefly while
      // this small TX queue drains; that is acceptable for an explicit USB-only
      // developer action.
      .tx_buffer_size = 2048U,
      .rx_buffer_size = 256U,
  };
  const esp_err_t result = usb_serial_jtag_driver_install(&config);
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
    s_status.store("driver-failed");
    return result;
  }
  if (!usb_serial_jtag_is_driver_installed()) {
    s_status.store("driver-missing");
    return ESP_ERR_INVALID_STATE;
  }
  s_status.store("driver-ready");
  usb_serial_jtag_vfs_use_driver();
  // The screenshot frame is binary. Console-style LF -> CRLF conversion would
  // silently insert bytes into a valid PNG and make its CRC/trailer fail.
  usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
  usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);

  // Screenshot encoding is not used while the flash cache is disabled, so keep
  // this permanent diagnostic stack in PSRAM. Reserving it in internal RAM left
  // only a few KiB for TLS and display-state work on a fully configured device.
  // Retain an internal-RAM fallback so USB diagnostics still start if external
  // stack allocation is unexpectedly unavailable during early boot.
  s_status.store("task-creating");
  if (xTaskCreatePinnedToCoreWithCaps(task_entry, "usb_developer", 6144, this, 2,
                                     &task_, kServiceCore,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGW(kLogTag,
             "USB developer task PSRAM stack unavailable; using internal RAM fallback");
    if (xTaskCreatePinnedToCore(task_entry, "usb_developer", 6144, this, 2,
                               &task_, kServiceCore) != pdPASS) {
      task_ = nullptr;
      s_status.store("task-failed");
      return ESP_ERR_NO_MEM;
    }
  }
  return ESP_OK;
}

void UsbDeveloperService::task_entry(void* context) {
  static_cast<UsbDeveloperService*>(context)->task_loop();
}

void UsbDeveloperService::task_loop() {
  s_status.store("running");
  std::array<char, kMaximumCommandLength + 1U> command{};
  std::size_t length = 0;
  std::uint64_t enabled_until = 0;
  while (true) {
    char byte = '\0';
    const int received = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(20));
    if (received != 1) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    s_status.store("rx-active");
    if (byte != '\n' && byte != '\r') {
      if (length < kMaximumCommandLength) command[length++] = byte;
      else length = 0;
      continue;
    }
    if (length == 0) continue;
    command[length] = '\0';
    const std::string_view line(command.data(), length);
    length = 0;

    const std::uint64_t now = static_cast<std::uint64_t>(esp_timer_get_time());
    if (line == kEnableCommand) {
      enabled_until = now + kSessionDurationUs;
      s_status.store("authorized");
      static constexpr char response[] = "PRINTDECK.DEV OK SCREEN-CAPTURE/1\n";
      write_all(reinterpret_cast<const std::uint8_t*>(response),
                sizeof(response) - 1U);
    } else if (line == kCaptureCommand && enabled_until > now) {
      s_status.store(send_screenshot() ? "capture-sent" : "capture-failed");
    } else if (line == kProfileCommand && enabled_until > now) {
      s_status.store(send_runtime_profile() ? "profile-sent" : "profile-failed");
    } else if (line.starts_with(kNavigatePrefix) && enabled_until > now) {
      const std::string_view screen_name = line.substr(kNavigatePrefix.size());
      const esp_err_t result = valid_screen_name(screen_name) && display_ != nullptr
                                   ? display_->navigate_for_capture(screen_name)
                                   : ESP_ERR_INVALID_ARG;
      std::string response = result == ESP_OK ? "PRINTDECK.DEV OK NAVIGATE "
                                               : "PRINTDECK.DEV ERROR NAVIGATE ";
      response.append(screen_name);
      response.push_back(' ');
      response.append(esp_err_to_name(result));
      response.push_back('\n');
      write_text(response);
      s_status.store(result == ESP_OK ? "navigation-sent" : "navigation-failed");
    }
  }
}

bool UsbDeveloperService::send_screenshot() {
  ImageWorkspaceLock workspace(5000);
  std::vector<std::uint8_t> png;
  std::string screen_name;
  if (!workspace || display_ == nullptr || display_->capture_png(png, screen_name) != ESP_OK ||
      png.empty() || !valid_screen_name(screen_name)) {
    static constexpr char response[] = "PRINTDECK.DEV ERROR CAPTURE\n";
    write_all(reinterpret_cast<const std::uint8_t*>(response),
              sizeof(response) - 1U);
    return false;
  }

  // Keep the only large allocation in this path as the encoded PNG itself.
  // Copying it into a second frame buffer can temporarily starve the Wi-Fi
  // stack while capturing a reaction that already owns a decoded GIF canvas.
  std::vector<std::uint8_t> header;
  header.reserve(kFrameMagic.size() + 10U + screen_name.size());
  header.insert(header.end(), kFrameMagic.begin(), kFrameMagic.end());
  append_u32_le(header, static_cast<std::uint32_t>(png.size()));
  append_u32_le(header, crc32(png.data(), png.size()));
  append_u16_le(header, static_cast<std::uint16_t>(screen_name.size()));
  header.insert(header.end(), screen_name.begin(), screen_name.end());
  return write_all(header.data(), header.size()) &&
         write_all(png.data(), png.size()) &&
         write_all(kFrameEnd.data(), kFrameEnd.size());
}

}  // namespace printdeck::platform
