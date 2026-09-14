#include "printdeck/platform/board.hpp"

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace printdeck::platform {
namespace {
sdmmc_card_t* card = nullptr;
}

esp_err_t board_sd_mount(std::uint64_t* identity) {
  if (!kBoardHasSdCard) return ESP_ERR_NOT_SUPPORTED;
  if (!identity || card) return ESP_ERR_INVALID_STATE;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.max_freq_khz = 10000;
  host.command_timeout_ms = 500;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.cd = SDMMC_SLOT_NO_CD;
  slot.wp = SDMMC_SLOT_NO_WP;
  slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#if defined(PRINTDECK_BOARD_LCD_1_54)
  // Waveshare LCD 1.54 reference: CLK16, CMD15, D0=17 (one-bit SDMMC).
  slot.clk = GPIO_NUM_16;
  slot.cmd = GPIO_NUM_15;
  slot.d0 = GPIO_NUM_17;
#elif !defined(PRINTDECK_BOARD_KNOMI2)
  // AMOLED 1.75 BSP 3.0.1 mapping, independent of its formatting option.
  slot.clk = GPIO_NUM_2;
  slot.cmd = GPIO_NUM_1;
  slot.d0 = GPIO_NUM_3;
#endif
  slot.d1 = slot.d2 = slot.d3 = slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
  esp_vfs_fat_sdmmc_mount_config_t config{};
  config.format_if_mount_failed = false;
  config.max_files = 3;
  config.allocation_unit_size = 16 * 1024;
  const esp_err_t result = esp_vfs_fat_sdmmc_mount(kSdMountPath, &host, &slot, &config, &card);
  if (result != ESP_OK) { card = nullptr; return result; }
  // Identity stays private. Include manufacturer and date to distinguish cards
  // that reuse a serial number; it is never returned to a browser or logged.
  *identity = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(card->cid.serial)) << 32) |
              (static_cast<std::uint64_t>(card->cid.mfg_id & 0xff) << 24) |
              (static_cast<std::uint64_t>(card->cid.oem_id & 0xff) << 16) |
              static_cast<std::uint64_t>(card->cid.date & 0xffff);
  if (!*identity) *identity = 1;
  return ESP_OK;
}

esp_err_t board_sd_status() {
  return card ? sdmmc_get_status(card) : ESP_ERR_INVALID_STATE;
}

esp_err_t board_sd_unmount() {
  if (!card) return ESP_OK;
  const auto result = esp_vfs_fat_sdcard_unmount(kSdMountPath, card);
  // ESP-IDF frees the card before unregistering VFS, even if that step fails.
  card = nullptr;
  return result;
}

esp_err_t board_sd_space(std::uint64_t* total, std::uint64_t* free) {
  return card ? esp_vfs_fat_info(kSdMountPath, total, free) : ESP_ERR_INVALID_STATE;
}
}  // namespace printdeck::platform
