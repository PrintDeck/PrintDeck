#pragma once

#include <cstdint>
#include <string_view>

namespace printdeck::platform {

enum class BambuPrinterModel : std::uint8_t {
  unknown,
  a1_mini,
  a1,
  a2_l,
  p1p,
  p1s,
  p2s,
  x1,
  x1c,
  x1e,
  h2s,
  h2d,
  h2d_pro,
  h2c,
  x2d,
};

enum class BambuCameraProtocol : std::uint8_t { unknown, jpeg_tls, rtsps };

struct BambuModelCapabilities {
  BambuCameraProtocol camera = BambuCameraProtocol::unknown;
  std::uint8_t maximum_live_nozzles = 1;
  bool v2_device_report = false;
  bool local_preview_ftps = true;
  std::uint32_t full_report_refresh_ms = 300000;
};

BambuPrinterModel bambu_model_from_identity(std::string_view product_name,
                                             std::string_view configured_model = {});
const char* bambu_model_name(BambuPrinterModel model);
BambuModelCapabilities bambu_capabilities_for(BambuPrinterModel model);

}  // namespace printdeck::platform
