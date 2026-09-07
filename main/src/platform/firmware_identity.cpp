#include "printdeck/platform/firmware_identity.hpp"

#include <array>
#include "esp_flash.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"
#include "printdeck_layout.hpp"
#include "printdeck/core/firmware_image.hpp"
#include "printdeck/core/firmware_channel.hpp"

extern "C" {
extern const printdeck::core::FirmwareIdentity printdeck_firmware_identity
    __attribute__((section(".rodata_custom_desc"), used, aligned(4))) = {
        "PrintDeck OTA 1", PRINTDECK_IDENTITY_TARGET, PRINTDECK_LAYOUT_SHA256, {0, 0, 0}};
}

namespace printdeck::platform {
std::string_view firmware_layout() { return printdeck_firmware_identity.layout; }

std::string firmware_asset_family() {
  return std::string(printdeck_firmware_identity.target) + "_layout_" +
         std::string(firmware_layout().substr(0, 12));
}

std::string firmware_image_version(std::span<const std::uint8_t> header) {
  if (header.size() < core::kFirmwareIdentityHeaderBytes) return {};
  core::FirmwareIdentity identity{};
  std::memcpy(&identity, header.data() + core::kFirmwareIdentityOffset, sizeof(identity));
  const std::string_view layout(identity.layout, strnlen(identity.layout, sizeof(identity.layout)));
  if (layout.size() != 64 || !core::compatible_firmware_header(
          header, printdeck_firmware_identity.target, layout)) return {};
  for (const char c : layout) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
  const char* version = reinterpret_cast<const char*>(header.data() + 48);
  const std::string value(version, strnlen(version, 32));
  return core::parse_firmware_version(value) ? value : std::string{};
}

bool compatible_firmware_image(std::span<const std::uint8_t> header) {
  if (!core::compatible_firmware_header(header, printdeck_firmware_identity.target,
                                       firmware_layout())) return false;
  // Also check physical flash: an application-only USB flash cannot establish
  // that its intended partition table is the table actually installed.
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts(&context, 0) == 0;
  std::array<std::uint8_t, 256> bytes{};
  for (std::size_t offset = 0; ok && offset < 3072; offset += bytes.size()) {
    ok = esp_flash_read(nullptr, bytes.data(), CONFIG_PARTITION_TABLE_OFFSET + offset,
                        bytes.size()) == ESP_OK &&
         mbedtls_sha256_update(&context, bytes.data(), bytes.size()) == 0;
  }
  std::array<std::uint8_t, 32> digest{};
  ok = ok && mbedtls_sha256_finish(&context, digest.data()) == 0;
  mbedtls_sha256_free(&context);
  constexpr char hex[] = "0123456789abcdef";
  for (std::size_t i = 0; ok && i < digest.size(); ++i) {
    ok = printdeck_firmware_identity.layout[i * 2] == hex[digest[i] >> 4] &&
         printdeck_firmware_identity.layout[i * 2 + 1] == hex[digest[i] & 15];
  }
  return ok;
}
}  // namespace printdeck::platform
