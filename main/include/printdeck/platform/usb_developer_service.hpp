#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace printdeck::platform {

class DisplayShell;

const char* usb_developer_status();

// Hidden physical-USB-only development channel. It has no Web Config route or
// persistent setting; authorization is lost on reboot and expires quickly.
class UsbDeveloperService {
 public:
  esp_err_t start(DisplayShell& display);

 private:
  static void task_entry(void* context);
  void task_loop();
  bool send_screenshot();

  DisplayShell* display_ = nullptr;
  TaskHandle_t task_ = nullptr;
};

}  // namespace printdeck::platform
