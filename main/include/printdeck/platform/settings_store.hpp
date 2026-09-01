#pragma once

#include "esp_err.h"
#include "printdeck/core/settings.hpp"

namespace printdeck::platform {

class SettingsStore {
 public:
  esp_err_t load(core::DeviceSettings& destination) const;
  esp_err_t save(const core::DeviceSettings& settings) const;
  esp_err_t save_selected_profile(std::uint32_t profile_id) const;
};

}  // namespace printdeck::platform
