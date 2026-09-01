#include "printdeck/platform/usb_developer_service.hpp"

#include "printdeck/platform/display_shell.hpp"

namespace printdeck::platform {

const char* usb_developer_status() { return "uart-console-only"; }

esp_err_t UsbDeveloperService::start(DisplayShell&) {
  return ESP_ERR_NOT_SUPPORTED;
}

void UsbDeveloperService::task_entry(void*) {}

void UsbDeveloperService::task_loop() {}

bool UsbDeveloperService::send_screenshot() { return false; }

}  // namespace printdeck::platform
