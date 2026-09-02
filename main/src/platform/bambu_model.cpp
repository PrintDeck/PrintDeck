#include "printdeck/platform/bambu_model.hpp"

#include <cctype>
#include <string>

namespace printdeck::platform {
namespace {

std::string normalized(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    if (std::isalnum(character) != 0) {
      result.push_back(static_cast<char>(std::toupper(character)));
    }
  }
  return result;
}

BambuPrinterModel model_from_one(std::string_view value) {
  const std::string name = normalized(value);
  if (name.empty()) return BambuPrinterModel::unknown;
  if (name.find("A1MINI") != std::string::npos || name == "N1") {
    return BambuPrinterModel::a1_mini;
  }
  if (name.find("A2L") != std::string::npos || name == "N9") {
    return BambuPrinterModel::a2_l;
  }
  if (name.find("P1P") != std::string::npos || name == "C11") {
    return BambuPrinterModel::p1p;
  }
  if (name.find("P1S") != std::string::npos || name == "C12") {
    return BambuPrinterModel::p1s;
  }
  if (name.find("P2S") != std::string::npos || name == "N7") {
    return BambuPrinterModel::p2s;
  }
  if (name.find("X1CARBON") != std::string::npos ||
      name.find("X1C") != std::string::npos || name == "BLP001") {
    return BambuPrinterModel::x1c;
  }
  if (name.find("X1E") != std::string::npos || name == "C13") {
    return BambuPrinterModel::x1e;
  }
  if (name.find("H2DPRO") != std::string::npos || name == "O1E") {
    return BambuPrinterModel::h2d_pro;
  }
  if (name.find("H2D") != std::string::npos || name == "O1D") {
    return BambuPrinterModel::h2d;
  }
  if (name.find("H2C") != std::string::npos || name == "O1C2" || name == "O1C") {
    return BambuPrinterModel::h2c;
  }
  if (name.find("H2S") != std::string::npos || name == "O1S") {
    return BambuPrinterModel::h2s;
  }
  if (name.find("X2D") != std::string::npos || name == "N6") {
    return BambuPrinterModel::x2d;
  }
  if (name == "A1" || name == "N2S" ||
      name.find("3DPRINTERA1") != std::string::npos) {
    return BambuPrinterModel::a1;
  }
  if (name == "X1" || name == "BLP002" ||
      name.find("3DPRINTERX1") != std::string::npos) {
    return BambuPrinterModel::x1;
  }
  return BambuPrinterModel::unknown;
}

}  // namespace

BambuPrinterModel bambu_model_from_identity(std::string_view product_name,
                                             std::string_view configured_model) {
  const BambuPrinterModel reported = model_from_one(product_name);
  return reported != BambuPrinterModel::unknown ? reported : model_from_one(configured_model);
}

const char* bambu_model_name(BambuPrinterModel model) {
  switch (model) {
    case BambuPrinterModel::a1_mini: return "A1 mini";
    case BambuPrinterModel::a1: return "A1";
    case BambuPrinterModel::a2_l: return "A2L";
    case BambuPrinterModel::p1p: return "P1P";
    case BambuPrinterModel::p1s: return "P1S";
    case BambuPrinterModel::p2s: return "P2S";
    case BambuPrinterModel::x1: return "X1";
    case BambuPrinterModel::x1c: return "X1C";
    case BambuPrinterModel::x1e: return "X1E";
    case BambuPrinterModel::h2s: return "H2S";
    case BambuPrinterModel::h2d: return "H2D";
    case BambuPrinterModel::h2d_pro: return "H2D Pro";
    case BambuPrinterModel::h2c: return "H2C";
    case BambuPrinterModel::x2d: return "X2D";
    case BambuPrinterModel::unknown: return "Unknown Bambu Lab model";
  }
  return "Unknown Bambu Lab model";
}

BambuModelCapabilities bambu_capabilities_for(BambuPrinterModel model) {
  BambuModelCapabilities capabilities;
  switch (model) {
    case BambuPrinterModel::a1_mini:
    case BambuPrinterModel::a1:
    case BambuPrinterModel::p1p:
    case BambuPrinterModel::p1s:
      capabilities.camera = BambuCameraProtocol::jpeg_tls;
      return capabilities;
    case BambuPrinterModel::x1:
    case BambuPrinterModel::x1c:
    case BambuPrinterModel::x1e:
      capabilities.camera = BambuCameraProtocol::rtsps;
      return capabilities;
    case BambuPrinterModel::a2_l:
      capabilities.camera = BambuCameraProtocol::jpeg_tls;
      capabilities.v2_device_report = true;
      capabilities.full_report_refresh_ms = 60000;
      return capabilities;
    case BambuPrinterModel::p2s:
    case BambuPrinterModel::h2s:
      capabilities.camera = BambuCameraProtocol::rtsps;
      capabilities.v2_device_report = true;
      capabilities.full_report_refresh_ms = 60000;
      return capabilities;
    case BambuPrinterModel::h2d:
    case BambuPrinterModel::h2d_pro:
    case BambuPrinterModel::h2c:
    case BambuPrinterModel::x2d:
      capabilities.camera = BambuCameraProtocol::rtsps;
      capabilities.maximum_live_nozzles = 2;
      capabilities.v2_device_report = true;
      capabilities.full_report_refresh_ms = 60000;
      return capabilities;
    case BambuPrinterModel::unknown:
      return capabilities;
  }
  return capabilities;
}

}  // namespace printdeck::platform
