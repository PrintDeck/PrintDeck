#pragma once
#include <mutex>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printdeck/core/gcode_source.hpp"

namespace printdeck::platform {
class GcodePreviewService {
 public:
  struct Result {
    unsigned status=503;
    std::string reason="waiting",key;
    std::uint32_t generation=0;
    std::uint64_t size=0,offset=0;
    std::shared_ptr<std::string> bytes;
  };
  void update(const core::PrinterProfile*,const core::PrinterSnapshot&,bool suspended);
  Result request(std::uint32_t profile,bool metadata,std::uint32_t generation,
      std::uint64_t offset,std::uint32_t length);
 private:
  static void entry(void*);
  void run();
  std::mutex mutex_;
  TaskHandle_t task_=nullptr;
  core::PrinterProfile profile_;
  std::string identity_,path_,validator_,key_,unavailable_;
  std::uint32_t generation_=0,last_elapsed_=0,last_layer_=0;
  std::uint64_t size_=0,status_at_=0,lease_=0,next_=0,metadata_at_=0;
  bool online_=false,suspended_=false;
  struct Work{bool pending=false,metadata=true;std::uint64_t offset=0;std::uint32_t length=0;unsigned attempts=0;};
  Work work_;
  Result result_;
};
} // namespace printdeck::platform
