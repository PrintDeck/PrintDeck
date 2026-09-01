#include "printdeck/platform/display_shell.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "png.h"
#include "img/printer_brand_logos.h"
#include "img/printdeck_boot_logo.h"

#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <utility>

#include "esp_heap_caps.h"
#include "printdeck/core/localization.hpp"
#include "printdeck/core/printer_driver.hpp"
#include "printdeck/platform/board.hpp"
#include "printdeck/platform/task_affinity.hpp"
#include "printdeck/platform/network_service.hpp"
#include "printdeck/platform/power_service.hpp"
#include "printdeck/platform/printer_animation_renderer.hpp"
#include "printdeck/platform/reaction_asset_service.hpp"
#include "printdeck/platform/firmware_update_service.hpp"
#include "printdeck/platform/reset_diagnostics.hpp"

extern "C" {
extern const lv_font_t mdi_40;
}

extern const std::uint8_t localized_latin_font_start[]
    asm("_binary_DejaVuSans_PrintDeck_ttf_start");
extern const std::uint8_t localized_latin_font_end[]
    asm("_binary_DejaVuSans_PrintDeck_ttf_end");
extern const std::uint8_t localized_cjk_font_start[]
    asm("_binary_NotoSansSC_PrintDeck_ttf_start");
extern const std::uint8_t localized_cjk_font_end[]
    asm("_binary_NotoSansSC_PrintDeck_ttf_end");
extern const std::uint8_t terminal_font_start[]
    asm("_binary_Unscii_PrintDeck_ttf_start");
extern const std::uint8_t terminal_font_end[]
    asm("_binary_Unscii_PrintDeck_ttf_end");

namespace printdeck::platform {
namespace {

constexpr char kLogTag[] = "display";
constexpr std::uint64_t kMinimumVisibleDimStageMs = 10'000;
constexpr int kHorizontalSwipeThresholdPx = 36;
constexpr int kHorizontalFlickMinDisplacementPx = 20;
constexpr int kHorizontalFlickVectorThresholdPx = 6;
constexpr int kVerticalSwipeThresholdPx = kDisplayUsesLargeLayout ? 24 : 14;
constexpr int kGestureAxisLockMarginPx = kDisplayUsesLargeLayout ? 16 : 4;
constexpr int kVerticalGestureLockStartPx = kDisplayUsesLargeLayout ? 12 : 7;
constexpr int kVerticalGestureDominancePx = kDisplayUsesLargeLayout ? 8 : 4;
constexpr int kEdgeDirectionLockPx = 4;
constexpr int kPrinterProgressTopOffsetPx = 8;
constexpr std::uint32_t kHorizontalLoadingDelayMs = 80;
constexpr std::uint32_t kHorizontalRevealDurationMs = 180;
constexpr std::uint32_t kHorizontalLoadingTimeoutMs = 5000;
constexpr std::uint32_t kTouchBackgroundRenderQuietMs = 40;
constexpr std::uint32_t kTransitionBackgroundRenderQuietMs = 20;
constexpr std::size_t kMaximumPreviewDimension = 512;
constexpr std::size_t kMaximumDecodedPreviewBytes =
    kMaximumPreviewDimension * kMaximumPreviewDimension * 4U;
constexpr std::size_t kPreviewDecodeHeapMarginBytes = 64U * 1024U;
constexpr char kMdiClock[] = "\xF3\xB1\x91\x8E";
constexpr char kMdiNozzle[] = "\xF3\xB0\xB9\x9B";
constexpr char kMdiBed[] = "\xF3\xB1\xA1\x9B";

void round_display_invalidation(lv_event_t* event) {
  auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
  if (area == nullptr) return;
  // CO5300 partial updates require an even start and an odd end on both axes.
  // Horizontal card animations frequently invalidate odd-sized regions.
  area->x1 = (area->x1 >> 1) << 1;
  area->y1 = (area->y1 >> 1) << 1;
  area->x2 = ((area->x2 >> 1) << 1) + 1;
  area->y2 = ((area->y2 >> 1) << 1) + 1;
}

std::string wifi_qr_escape(const char* text) {
  std::string escaped;
  if (text == nullptr) return escaped;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor == '\\' || *cursor == ';' || *cursor == ',' || *cursor == ':' ||
        *cursor == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(*cursor);
  }
  return escaped;
}

const char* link_label(core::LinkState state) {
  switch (state) {
    case core::LinkState::stopped: return "Not selected";
    case core::LinkState::waiting_for_network: return "Waiting for network";
    case core::LinkState::connecting: return "Connecting";
    case core::LinkState::online: return "Online";
    case core::LinkState::failed: return "Connection failed";
  }
  return "Unavailable";
}

std::string duration_text(std::uint32_t seconds) {
  char text[24]{};
  const unsigned hours = seconds / 3600U;
  const unsigned minutes = (seconds % 3600U) / 60U;
  std::snprintf(text, sizeof(text), "%uh %02um", hours, minutes);
  return text;
}

std::string duration_hms(std::uint32_t seconds) {
  char text[32]{};
  const unsigned hours = seconds / 3600U;
  const unsigned minutes = (seconds % 3600U) / 60U;
  const unsigned remaining_seconds = seconds % 60U;
  if (hours > 0U) {
    std::snprintf(text, sizeof(text), "%uh %02um %02us", hours, minutes,
                  remaining_seconds);
  } else {
    std::snprintf(text, sizeof(text), "%um %02us", minutes, remaining_seconds);
  }
  return text;
}

std::string uppercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

const char* theme_display_name(std::string_view id) {
  static constexpr std::array<std::pair<std::string_view, const char*>, 12> names{{
      {"green", "SIGNAL"}, {"banana", "BANANA"}, {"sunset", "SOLSTICE"},
      {"ice", "GLACIER"}, {"cyberpunk", "AURORA"}, {"ember", "GROVE"},
      {"mono", "GRAPHITE"}, {"red", "GARNET"}, {"ios_glass", "MIDNIGHT HALO"},
      {"fluent_dark", "DRAGON"}, {"retro_terminal", "TERMINAL"},
      {"custom", "CUSTOM"},
  }};
  for (const auto& [candidate, name] : names) {
    if (candidate == id) return name;
  }
  return "CUSTOM";
}

std::string endpoint_host(std::string value) {
  if (const std::size_t scheme = value.find("://"); scheme != std::string::npos) {
    value.erase(0, scheme + 3);
  }
  if (const std::size_t path = value.find('/'); path != std::string::npos) {
    value.resize(path);
  }
  if (const std::size_t port = value.rfind(':'); port != std::string::npos) {
    value.resize(port);
  }
  return value.empty() ? "No local IP" : value;
}

void make_gesture_passthrough(lv_obj_t* object);

lv_obj_t* transparent_icon_root(lv_obj_t* parent, int width, int height,
                                int x, int y) {
  lv_obj_t* root = lv_obj_create(parent);
  lv_obj_set_size(root, width, height);
  lv_obj_align(root, LV_ALIGN_CENTER, x, y);
  lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
  make_gesture_passthrough(root);
  return root;
}

lv_obj_t* icon_shape(lv_obj_t* parent, int width, int height,
                     int x, int y, std::uint32_t color, int radius = 1) {
  lv_obj_t* shape = lv_obj_create(parent);
  lv_obj_set_size(shape, width, height);
  lv_obj_align(shape, LV_ALIGN_CENTER, x, y);
  lv_obj_set_style_radius(shape, radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(shape, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(shape, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(shape, 0, LV_PART_MAIN);
  make_gesture_passthrough(shape);
  return shape;
}

lv_obj_t* create_mdi_icon(lv_obj_t* parent, const char* glyph, int x, int y,
                          std::uint32_t color, int scale = 256) {
  lv_obj_t* icon = lv_label_create(parent);
  lv_label_set_text(icon, glyph);
  lv_obj_set_style_text_font(icon, &mdi_40, LV_PART_MAIN);
  lv_obj_set_style_text_color(icon, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_transform_scale(icon, scale, LV_PART_MAIN);
  lv_obj_align(icon, LV_ALIGN_CENTER, x, y);
  make_gesture_passthrough(icon);
  return icon;
}

void create_thermometer_icon(lv_obj_t* parent, int x, int y, std::uint32_t color) {
  lv_obj_t* root = transparent_icon_root(parent, 24, 28, x, y);
  lv_obj_t* stem = lv_obj_create(root);
  lv_obj_set_size(stem, 8, 18);
  lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, 1);
  lv_obj_set_style_radius(stem, 7, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(stem, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(stem, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_border_width(stem, 3, LV_PART_MAIN);
  lv_obj_set_style_pad_all(stem, 0, LV_PART_MAIN);
  make_gesture_passthrough(stem);
  lv_obj_t* bulb = icon_shape(root, 12, 12, 0, 7, color, LV_RADIUS_CIRCLE);
  lv_obj_align(bulb, LV_ALIGN_BOTTOM_MID, 0, -1);
}

}  // namespace

std::string DisplayShell::effective_brand(const core::PrinterProfile& profile) {
  if (!profile.brand.empty() && profile.brand != "generic" && profile.brand != "klipper") {
    return profile.brand;
  }
  std::string identity = profile.manufacturer + " " + profile.model + " " + profile.display_name;
  std::transform(identity.begin(), identity.end(), identity.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (identity.find("creality") != std::string::npos ||
      identity.find("ender") != std::string::npos ||
      identity.find("sermoon") != std::string::npos ||
      identity.find("k1") != std::string::npos ||
      identity.find("k2") != std::string::npos ||
      identity.find("cr-") != std::string::npos) return "creality";
  for (const char* brand : {"snapmaker", "prusa", "bambu", "anycubic", "elegoo",
                            "qidi", "sovol", "flashforge", "ankermake", "voron",
                            "ratrig", "lulzbot", "makerbot", "ultimaker"}) {
    if (identity.find(brand) != std::string::npos) return brand;
  }
  if (identity.find("rat rig") != std::string::npos) return "ratrig";
  return profile.protocol == core::PrinterProtocol::bambu_lan ? "bambu" : "klipper";
}

const char* DisplayShell::brand_mark(const core::PrinterProfile& profile) {
  const std::string brand = effective_brand(profile);
  if (brand == "creality") return "CR";
  if (brand == "snapmaker") return "SN";
  if (brand == "prusa") return "PR";
  if (brand == "bambu") return "BL";
  if (brand == "anycubic") return "AC";
  if (brand == "elegoo") return "EL";
  if (brand == "qidi") return "QD";
  if (brand == "sovol") return "SV";
  if (brand == "flashforge") return "FF";
  if (brand == "ankermake") return "AM";
  if (brand == "voron") return "VR";
  if (brand == "ratrig") return "RR";
  if (brand == "lulzbot") return "LZ";
  if (brand == "makerbot") return "MB";
  if (brand == "ultimaker") return "UM";
  return profile.protocol == core::PrinterProtocol::bambu_lan ? "BL" : "KL";
}

std::uint32_t DisplayShell::brand_color(const core::PrinterProfile& profile) {
  const std::string brand = effective_brand(profile);
  if (brand == "creality") return 0x00C651;
  if (brand == "prusa") return 0xF47B20;
  if (brand == "snapmaker") return 0x4F87C7;
  if (brand == "bambu") return 0x00AE42;
  if (brand == "anycubic") return 0x5E72E4;
  if (brand == "elegoo") return 0x00A8E8;
  if (brand == "qidi") return 0x00A6D6;
  if (brand == "sovol") return 0x22B8B2;
  if (brand == "flashforge") return 0xED3325;
  if (brand == "ankermake") return 0x48D597;
  if (brand == "voron") return 0xED1C24;
  return 0x607D9B;
}

std::uint32_t DisplayShell::brand_logo_color(const core::PrinterProfile& profile,
                                             std::uint32_t background) {
  if (effective_brand(profile) != "snapmaker") return brand_color(profile);

  const std::uint32_t red = (background >> 16U) & 0xFFU;
  const std::uint32_t green = (background >> 8U) & 0xFFU;
  const std::uint32_t blue = background & 0xFFU;
  const std::uint32_t luminance = red * 2126U + green * 7152U + blue * 722U;
  return luminance >= 128U * 10000U ? 0x000000 : 0xFFFFFF;
}

const lv_image_dsc_t* DisplayShell::brand_logo(const core::PrinterProfile& profile) {
  const std::string brand = effective_brand(profile);
  if (brand == "ankermake") return &printer_logo_ankermake;
  if (brand == "anycubic") return &printer_logo_anycubic;
  if (brand == "bambu") return &printer_logo_bambu;
  if (brand == "creality") return &printer_logo_creality;
  if (brand == "elegoo") return &printer_logo_elegoo;
  if (brand == "flashforge") return &printer_logo_flashforge;
  if (brand == "lulzbot") return &printer_logo_lulzbot;
  if (brand == "makerbot") return &printer_logo_makerbot;
  if (brand == "prusa") return &printer_logo_prusa;
  if (brand == "qidi") return &printer_logo_qidi;
  if (brand == "ratrig") return &printer_logo_ratrig;
  if (brand == "snapmaker") return &printer_logo_snapmaker;
  if (brand == "sovol") return &printer_logo_sovol;
  if (brand == "ultimaker") return &printer_logo_ultimaker;
  if (brand == "voron") return &printer_logo_voron;
  return nullptr;
}

const lv_image_dsc_t* DisplayShell::brand_logo_small(const core::PrinterProfile& profile) {
  const std::string brand = effective_brand(profile);
  if (brand == "ankermake") return &printer_logo_ankermake_small;
  if (brand == "anycubic") return &printer_logo_anycubic_small;
  if (brand == "bambu") return &printer_logo_bambu_small;
  if (brand == "creality") return &printer_logo_creality_small;
  if (brand == "elegoo") return &printer_logo_elegoo_small;
  if (brand == "flashforge") return &printer_logo_flashforge_small;
  if (brand == "lulzbot") return &printer_logo_lulzbot_small;
  if (brand == "makerbot") return &printer_logo_makerbot_small;
  if (brand == "prusa") return &printer_logo_prusa_small;
  if (brand == "qidi") return &printer_logo_qidi_small;
  if (brand == "ratrig") return &printer_logo_ratrig_small;
  if (brand == "snapmaker") return &printer_logo_snapmaker_small;
  if (brand == "sovol") return &printer_logo_sovol_small;
  if (brand == "ultimaker") return &printer_logo_ultimaker_small;
  if (brand == "voron") return &printer_logo_voron_small;
  return nullptr;
}

namespace {

void prepare_screen(lv_obj_t* screen, std::uint32_t background) {
  lv_obj_clean(screen);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  // Screen-level gestures are routed by DisplayShell.  Leaving LVGL's
  // default AUTO scrollbar mode enabled can still expose the native white
  // scroll indicators after a rotation or an interrupted drag, even though
  // the screen itself is no longer scrollable.
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_color(screen, lv_color_hex(background), LV_PART_MAIN);
  // The panel renders in RGB565. Dark gradients quantize into visible bands,
  // so theme backgrounds deliberately use one exact, saturated color.
  lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
}

void make_gesture_passthrough(lv_obj_t* object) {
  if (object == nullptr) return;
  // Decorative dashboard layers must not become the pointer target. Otherwise
  // a swipe that begins over a logo or print preview never reaches the
  // screen-wide gesture recognizer.
  lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

bool decode_preview_png(const std::shared_ptr<std::vector<std::uint8_t>>& encoded,
                        std::shared_ptr<std::vector<std::uint8_t>>& pixels,
                        lv_image_dsc_t& descriptor) {
  if (!encoded || encoded->empty()) return false;
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&image, encoded->data(), encoded->size())) return false;
  image.format = PNG_FORMAT_BGRA;
  if (image.width == 0 || image.height == 0 || image.width > kMaximumPreviewDimension ||
      image.height > kMaximumPreviewDimension) {
    ESP_LOGW(kLogTag, "Preview PNG dimensions are unsupported: %ux%u",
             static_cast<unsigned>(image.width), static_cast<unsigned>(image.height));
    png_image_free(&image);
    return false;
  }
  const std::size_t decoded_size = PNG_IMAGE_SIZE(image);
  if (decoded_size == 0 || decoded_size > kMaximumDecodedPreviewBytes) {
    ESP_LOGW(kLogTag, "Preview PNG decoded size is unsupported: %u bytes",
             static_cast<unsigned>(decoded_size));
    png_image_free(&image);
    return false;
  }
  const std::size_t largest_psram_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (largest_psram_block < decoded_size + kPreviewDecodeHeapMarginBytes) {
    ESP_LOGW(kLogTag,
             "Preview PNG skipped: need %u bytes, largest PSRAM block is %u bytes",
             static_cast<unsigned>(decoded_size),
             static_cast<unsigned>(largest_psram_block));
    png_image_free(&image);
    return false;
  }
  mark_reset_checkpoint(ResetCheckpoint::kPreviewDecode);
  auto decoded = std::make_shared<std::vector<std::uint8_t>>(decoded_size);
  const std::size_t stride = static_cast<std::size_t>(image.width) * 4U;
  if (!png_image_finish_read(&image, nullptr, decoded->data(),
                             static_cast<png_int_32>(stride), nullptr)) {
    ESP_LOGW(kLogTag, "Preview PNG decode failed: %s", image.message);
    png_image_free(&image);
    mark_reset_checkpoint(ResetCheckpoint::kRunning);
    return false;
  }
  png_image_free(&image);
  descriptor = {};
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
  descriptor.header.w = static_cast<std::uint16_t>(image.width);
  descriptor.header.h = static_cast<std::uint16_t>(image.height);
  descriptor.header.stride = static_cast<std::uint16_t>(stride);
  descriptor.data_size = static_cast<std::uint32_t>(decoded->size());
  descriptor.data = decoded->data();
  pixels = std::move(decoded);
  mark_reset_checkpoint(ResetCheckpoint::kRunning);
  return true;
}

lv_draw_buf_t* take_transparent_snapshot(lv_obj_t* object) {
  if (object == nullptr) return nullptr;
  lv_draw_buf_t* buffer =
      lv_snapshot_create_draw_buf(object, LV_COLOR_FORMAT_ARGB8888);
  if (buffer == nullptr) return nullptr;
  lv_draw_buf_clear(buffer, nullptr);
  if (lv_snapshot_take_to_draw_buf(object, LV_COLOR_FORMAT_ARGB8888, buffer) !=
      LV_RESULT_OK) {
    lv_draw_buf_destroy(buffer);
    return nullptr;
  }
  return buffer;
}

void composite_snapshot_bgra(lv_draw_buf_t* destination,
                             const lv_draw_buf_t* source, int width, int height) {
  if (destination == nullptr || destination->data == nullptr ||
      destination->header.w != width || destination->header.h != height ||
      destination->header.stride < width * 4 || source == nullptr ||
      source->data == nullptr || source->header.w != width ||
      source->header.h != height || source->header.stride < width * 4) {
    return;
  }
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* source_row =
        source->data + static_cast<std::size_t>(y) * source->header.stride;
    std::uint8_t* destination_row =
        destination->data + static_cast<std::size_t>(y) * destination->header.stride;
    for (int x = 0; x < width; ++x) {
      const std::uint8_t* source_pixel = source_row + x * 4;
      std::uint8_t* destination_pixel = destination_row + x * 4;
      const unsigned alpha = source_pixel[3];
      if (alpha == 0U) continue;
      if (alpha == 255U) {
        std::memcpy(destination_pixel, source_pixel, 4U);
        continue;
      }
      const unsigned inverse = 255U - alpha;
      for (int channel = 0; channel < 3; ++channel) {
        destination_pixel[channel] = static_cast<std::uint8_t>(
            (source_pixel[channel] * alpha + destination_pixel[channel] * inverse + 127U) /
            255U);
      }
      destination_pixel[3] = static_cast<std::uint8_t>(
          alpha + (destination_pixel[3] * inverse + 127U) / 255U);
    }
  }
}

}  // namespace

esp_err_t DisplayShell::start(int initial_rotation_degrees) {
  current_rotation_ = initial_rotation_degrees == 90 ? 90
                    : initial_rotation_degrees == 180 ? 180
                    : initial_rotation_degrees == 270 ? 270 : 0;
  esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
  adapter_config.task_core_id = kLvglCore;
  esp_err_t display_result = esp_lv_adapter_init(&adapter_config);
  if (display_result != ESP_OK) {
    ESP_LOGE(kLogTag, "LVGL adapter initialization failed: %s",
             esp_err_to_name(display_result));
    return display_result;
  }
  esp_lcd_panel_handle_t panel_handle = nullptr;
  esp_lcd_panel_io_handle_t panel_io_handle = nullptr;
  display_result = board_display_new(kDisplayWidth * 24 * 2,
                                     &panel_handle, &panel_io_handle);
  if (display_result != ESP_OK || panel_handle == nullptr || panel_io_handle == nullptr) {
    ESP_LOGE(kLogTag, "Display panel initialization failed: %s",
             esp_err_to_name(display_result));
    return display_result == ESP_OK ? ESP_FAIL : display_result;
  }
  esp_lcd_panel_handle_t adapter_panel =
      board_adapt_display_panel(panel_handle, panel_io_handle);
  board_display_set_draw_failure_callback(display_draw_failed, this);
  display_result = board_display_set_rotation(current_rotation_.load());
  if (display_result != ESP_OK) {
    ESP_LOGE(kLogTag, "Initial display rotation failed: %s",
             esp_err_to_name(display_result));
    return display_result;
  }
  const esp_lv_adapter_display_config_t adapter_display_config = {
      .panel = adapter_panel,
      .panel_io = panel_io_handle,
      .profile = {
          .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
          .rotation = ESP_LV_ADAPTER_ROTATE_0,
          .hor_res = kDisplayWidth,
          .ver_res = kDisplayHeight,
          .buffer_height = 24,
          .use_psram = true,
          .enable_ppa_accel = false,
          .require_double_buffer = true,
      },
      .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
  };
  lv_display_t* display = esp_lv_adapter_register_display(&adapter_display_config);
  if (display == nullptr) {
    ESP_LOGE(kLogTag, "Display initialization failed");
    return ESP_FAIL;
  }
  if constexpr (kDisplayRequiresEvenInvalidation) {
    lv_display_add_event_cb(display, round_display_invalidation,
                            LV_EVENT_INVALIDATE_AREA, nullptr);
  }
  esp_lcd_touch_handle_t touch_handle = nullptr;
  display_result = board_touch_new(&touch_handle);
  if (display_result != ESP_OK || touch_handle == nullptr) {
    ESP_LOGE(kLogTag, "Touch controller initialization failed: %s",
             esp_err_to_name(display_result));
    return display_result == ESP_OK ? ESP_FAIL : display_result;
  }
  const esp_lv_adapter_touch_config_t touch_config =
      ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch_handle);
  lv_indev_t* touch_input = esp_lv_adapter_register_touch(&touch_config);
  if (touch_input == nullptr) {
    ESP_LOGE(kLogTag, "Touch input registration failed");
    return ESP_FAIL;
  }
  display_result = board_display_brightness_init();
  if (display_result != ESP_OK) return display_result;
  display_result = esp_lv_adapter_start();
  if (display_result != ESP_OK) {
    ESP_LOGE(kLogTag, "LVGL task start failed: %s", esp_err_to_name(display_result));
    return display_result;
  }
  const esp_lv_adapter_touch_callbacks_t touch_callbacks = {
      .on_interrupt = nullptr,
      .custom_touch_read = touch_read,
      .user_ctx = this,
  };
  if (esp_lv_adapter_set_touch_callbacks(touch_input, &touch_callbacks) != ESP_OK) {
    ESP_LOGE(kLogTag, "Touch orientation callback could not be installed");
    return ESP_FAIL;
  }
  if (board_display_lock(2000) != ESP_OK) {
    ESP_LOGE(kLogTag, "Display lock timed out during initialization");
    return ESP_ERR_TIMEOUT;
  }

  if (!initialize_localized_fonts()) {
    board_display_unlock();
    ESP_LOGE(kLogTag, "Localized display fonts could not be initialized");
    return ESP_ERR_NO_MEM;
  }

  if constexpr (!kDisplayUsesLargeLayout) {
    square_create_initial_screen();
    board_display_unlock();
    display_ready_.store(true, std::memory_order_release);
    ESP_LOGI(kLogTag, "Compact display ready; LVGL task pinned to core %d", kLvglCore);
    return ESP_OK;
  }

  lv_obj_t* screen = lv_screen_active();
  lv_obj_add_event_cb(screen, screen_event, LV_EVENT_PRESSED, this);
  lv_obj_add_event_cb(screen, screen_event, LV_EVENT_PRESSING, this);
  lv_obj_add_event_cb(screen, screen_event, LV_EVENT_RELEASED, this);
  lv_obj_add_event_cb(screen, screen_event, LV_EVENT_PRESS_LOST, this);
  lv_obj_add_event_cb(screen, screen_event, LV_EVENT_LONG_PRESSED, this);
  last_activity_ms_ = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_color(screen, lv_color_hex(theme_style_.background), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

  // A software-blurred shadow around a 342 px circle monopolizes the LVGL
  // core for several seconds on this target. Two translucent vector rings
  // retain the glow treatment while remaining cheap to redraw.
  for (const auto [size, width, opacity] :
       {std::array<int, 3>{378, 2, LV_OPA_10},
        std::array<int, 3>{360, 3, LV_OPA_20}}) {
    lv_obj_t* ring = lv_obj_create(screen);
    lv_obj_set_size(ring, size, size);
    lv_obj_center(ring);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(accent_color_), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, width, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, opacity, LV_PART_MAIN);
  }

  lv_obj_t* halo = lv_obj_create(screen);
  lv_obj_set_size(halo, 342, 342);
  lv_obj_center(halo);
  lv_obj_remove_flag(halo, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(halo, lv_color_hex(theme_style_.surface), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(halo, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(halo, lv_color_hex(accent_color_), LV_PART_MAIN);
  lv_obj_set_style_border_width(halo, 2, LV_PART_MAIN);

  constexpr int kBootLogoWidth = 260;
  constexpr int kBootLogoHeight = 62;

  lv_obj_t* logo_stage = lv_obj_create(screen);
  lv_obj_set_size(logo_stage, kBootLogoWidth, kBootLogoHeight);
  lv_obj_align(logo_stage, LV_ALIGN_CENTER, 0, -42);
  lv_obj_remove_flag(logo_stage, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(logo_stage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(logo_stage, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(logo_stage, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(logo_stage, 0, LV_PART_MAIN);

  lv_obj_t* logo_clip = lv_obj_create(logo_stage);
  lv_obj_set_pos(logo_clip, 0, kBootLogoHeight - 1);
  lv_obj_set_size(logo_clip, kBootLogoWidth, 1);
  lv_obj_remove_flag(logo_clip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(logo_clip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(logo_clip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_set_style_bg_opa(logo_clip, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(logo_clip, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(logo_clip, 0, LV_PART_MAIN);

  lv_obj_t* logo_image = lv_image_create(logo_clip);
  lv_image_set_src(logo_image, &printdeck_boot_logo_large);
  lv_obj_set_pos(logo_image, 0, -(kBootLogoHeight - 1));
  lv_obj_remove_flag(logo_image, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* print_line = lv_obj_create(logo_stage);
  lv_obj_set_pos(print_line, 0, kBootLogoHeight - 2);
  lv_obj_set_size(print_line, kBootLogoWidth, 2);
  lv_obj_remove_flag(print_line, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(print_line, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(print_line, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(print_line, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(print_line, lv_color_hex(accent_color_), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(print_line, LV_OPA_TRANSP, LV_PART_MAIN);

  lv_anim_t logo_reveal;
  lv_anim_init(&logo_reveal);
  lv_anim_set_var(&logo_reveal, logo_stage);
  lv_anim_set_values(&logo_reveal, 1, kBootLogoHeight);
  lv_anim_set_duration(&logo_reveal, 440);
  lv_anim_set_delay(&logo_reveal, 650);
  lv_anim_set_path_cb(&logo_reveal, lv_anim_path_linear);
  lv_anim_set_exec_cb(&logo_reveal, [](void* object, std::int32_t value) {
    auto* stage = static_cast<lv_obj_t*>(object);
    if (stage == nullptr || !lv_obj_is_valid(stage)) return;
    lv_obj_t* clip = lv_obj_get_child(stage, 0);
    lv_obj_t* line = lv_obj_get_child(stage, 1);
    lv_obj_t* image = clip == nullptr ? nullptr : lv_obj_get_child(clip, 0);
    if (clip == nullptr || line == nullptr || image == nullptr) return;

    const int revealed = std::min(
        62, std::max(1, ((static_cast<int>(value) + 3) / 4) * 4));
    const int top = 62 - revealed;
    lv_obj_set_pos(clip, 0, top);
    lv_obj_set_size(clip, 260, revealed);
    lv_obj_set_pos(image, 0, -top);
    lv_obj_set_y(line, top);

    constexpr int kFadeStart = 48;
    const int opacity = revealed <= kFadeStart
                            ? LV_OPA_COVER
                            : ((62 - revealed) * LV_OPA_COVER) /
                                  (62 - kFadeStart);
    lv_obj_set_style_bg_opa(line, static_cast<lv_opa_t>(opacity), LV_PART_MAIN);
  });
  lv_anim_start(&logo_reveal);

  status_label_ = lv_label_create(screen);
  lv_label_set_text(status_label_, tr("Starting device services"));
  apply_text_style(status_label_, lv_color_hex(accent_color_), &lv_font_montserrat_16);
  lv_obj_set_width(status_label_, 300);
  lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 58);

  lv_obj_t* version = lv_label_create(screen);
  lv_label_set_text_fmt(version, "%s %s", tr("Version"), PRINTDECK_VERSION);
  apply_text_style(version, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
  lv_obj_align(version, LV_ALIGN_BOTTOM_MID, 0, -92);

  lv_obj_invalidate(screen);
  board_display_unlock();
  display_ready_.store(true, std::memory_order_release);
  ESP_LOGI(kLogTag, "Display ready; LVGL task pinned to core %d", kLvglCore);
  return ESP_OK;
}

void DisplayShell::screen_event(lv_event_t* event) {
  auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
  if (shell == nullptr) return;
  const lv_event_code_t code = lv_event_get_code(event);
  shell->note_activity(true);
  lv_indev_t* input = lv_indev_active();
  if (input == nullptr) return;
  lv_point_t point{};
  lv_indev_get_point(input, &point);

  if (code == LV_EVENT_PRESSED) {
    // A USB-only animation preview must never leak into normal device use.
    // The first physical interaction returns the page to live printer state.
    shell->capture_animation_override_active_ = false;
    shell->capture_animation_screen_name_.clear();
    if (shell->horizontal_transition_active_) {
      shell->gesture_active_ = false;
      shell->gesture_started_in_printer_list_ = false;
      shell->printer_list_vertical_gesture_ = false;
      shell->printer_list_scroll_started_ = false;
      shell->pressed_printer_card_ = nullptr;
      return;
    }
    shell->gesture_active_ = true;
    shell->horizontal_gesture_committed_ = false;
    shell->vertical_gesture_locked_ = false;
    shell->gesture_started_in_printer_list_ = false;
    shell->printer_list_vertical_gesture_ = false;
    shell->printer_list_scroll_started_ = false;
    shell->pressed_printer_card_ = nullptr;
    shell->suppress_update_click_ = false;
    shell->gesture_start_x_ = point.x;
    shell->gesture_start_y_ = point.y;
    shell->square_gesture_peak_dx_ = 0;
    shell->square_gesture_peak_dy_ = 0;
    if (shell->printer_list_scroll_ != nullptr &&
        lv_obj_is_valid(shell->printer_list_scroll_)) {
      lv_area_t area{};
      lv_obj_get_coords(shell->printer_list_scroll_, &area);
      const bool point_in_list =
          point.x >= area.x1 && point.x <= area.x2 &&
          point.y >= area.y1 && point.y <= area.y2;
      shell->gesture_started_in_printer_list_ =
          point_in_list &&
          shell->printer_list_count_ > shell->printer_list_visible_count_;
      if (point_in_list) {
        const std::uint32_t child_count =
            lv_obj_get_child_count(shell->printer_list_scroll_);
        for (std::uint32_t index = 0; index < child_count; ++index) {
          lv_obj_t* child = lv_obj_get_child(shell->printer_list_scroll_, index);
          if (child == nullptr || !lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            continue;
          }
          lv_area_t child_area{};
          lv_obj_get_coords(child, &child_area);
          if (point.x >= child_area.x1 && point.x <= child_area.x2 &&
              point.y >= child_area.y1 && point.y <= child_area.y2) {
            shell->pressed_printer_card_ = child;
            break;
          }
        }
      }
    }
    return;
  }

  if (!shell->gesture_active_) return;
  const int dx = static_cast<int>(point.x - shell->gesture_start_x_);
  const int dy = static_cast<int>(shell->gesture_start_y_ - point.y);
  const int abs_dx = std::abs(dx);
  const int abs_dy = std::abs(dy);

  if constexpr (!kDisplayUsesLargeLayout) {
    // CST816S can omit or slightly regress the final coordinate just before
    // release. Keep the furthest sampled displacement so a deliberate square
    // display swipe is not lost with that final sample.
    if (abs_dx > std::abs(shell->square_gesture_peak_dx_)) {
      shell->square_gesture_peak_dx_ = dx;
    }
    if (abs_dy > std::abs(shell->square_gesture_peak_dy_)) {
      shell->square_gesture_peak_dy_ = dy;
    }
  }

  if (code == LV_EVENT_PRESSING) {
    if (shell->horizontal_gesture_committed_ || shell->vertical_gesture_locked_ ||
        shell->printer_list_vertical_gesture_) {
      return;
    }
    // The printer list and the screen carousel both use the vertical axis.
    // Resolve ownership once, near the start of the drag, and never hand the
    // same gesture from the list to the carousel on release. LVGL's selected
    // scroll object is authoritative; the displacement check covers the few
    // samples before LVGL crosses its own scroll threshold.
    const bool lvgl_list_scroll =
        shell->gesture_started_in_printer_list_ &&
        lv_indev_get_scroll_obj(input) == shell->printer_list_scroll_;
    if (shell->gesture_started_in_printer_list_ &&
        (lvgl_list_scroll ||
         (abs_dy >= kVerticalGestureLockStartPx &&
          abs_dy >= abs_dx + kVerticalGestureDominancePx))) {
      shell->printer_list_vertical_gesture_ = true;
      shell->suppress_update_click_ = true;
      return;
    }
    // Lock the dominant axis early.  On the printer-list depth a mostly
    // vertical gesture must never open the active-printer depth just because
    // the finger also drifted far enough in X.  The old UI relied on LVGL's
    // native vertical scroll lock; this explicit screen router needs the same
    // arbitration before either threshold is reached.
    if (abs_dy >= kVerticalGestureLockStartPx &&
        abs_dy >= abs_dx + kVerticalGestureDominancePx) {
      shell->vertical_gesture_locked_ = true;
      shell->suppress_update_click_ = true;
      return;
    }
    // Do not cancel a possible vertical swipe because of the first few pixels
    // of normal finger drift. The old 4 px edge lock made pages 1-4 feel
    // stuck whenever the gesture started even slightly diagonally.
    const bool clearly_horizontal =
        abs_dx >= kHorizontalFlickMinDisplacementPx && abs_dy < 10 &&
        abs_dx >= abs_dy + 8;
    int target_page = 0;
    bool target_list = true;
    int target_subpage = 0;
    if (clearly_horizontal &&
        !shell->horizontal_destination(dx < 0, &target_page, &target_list,
                                       &target_subpage)) {
      shell->gesture_active_ = false;
      shell->horizontal_gesture_committed_ = true;
      lv_indev_wait_release(input);
      return;
    }
    const bool horizontal =
        abs_dx >= kHorizontalSwipeThresholdPx && abs_dx >= abs_dy + 8;
    const bool vertical = abs_dy >= kVerticalSwipeThresholdPx &&
        abs_dy >= abs_dx + kGestureAxisLockMarginPx;
    if (horizontal &&
        shell->horizontal_destination(dx < 0, &target_page, &target_list,
                                      &target_subpage)) {
      shell->gesture_active_ = false;
      shell->horizontal_gesture_committed_ = true;
      shell->suppress_update_click_ = true;
      lv_indev_wait_release(input);
      shell->start_horizontal_transition(target_page, target_list, dx < 0 ? -1 : 1,
                                         0, target_subpage);
      return;
    }
    if (vertical) {
      shell->vertical_gesture_locked_ = true;
      shell->suppress_update_click_ = true;
    }
    return;
  }

  if (code == LV_EVENT_LONG_PRESSED) {
    if (!shell->horizontal_gesture_committed_ && !shell->vertical_gesture_locked_ &&
        !shell->printer_list_vertical_gesture_ &&
        abs_dx < kHorizontalFlickMinDisplacementPx &&
        abs_dy < kHorizontalFlickMinDisplacementPx) {
      shell->gesture_active_ = false;
      shell->show_quick_menu();
    }
    return;
  }

  if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
  const bool vertical_locked = shell->vertical_gesture_locked_;
  lv_obj_t* pressed_printer_card = shell->pressed_printer_card_;
  const int release_abs_dx = kDisplayUsesLargeLayout
      ? abs_dx
      : std::max(abs_dx, std::abs(shell->square_gesture_peak_dx_));
  const int release_abs_dy = kDisplayUsesLargeLayout
      ? abs_dy
      : std::max(abs_dy, std::abs(shell->square_gesture_peak_dy_));
  const int tap_movement_limit = kDisplayUsesLargeLayout ? 16 : 12;
  const bool actual_list_scroll =
      shell->printer_list_scroll_started_ ||
      (shell->gesture_started_in_printer_list_ &&
       lv_indev_get_scroll_obj(input) == shell->printer_list_scroll_);
  const bool list_owned_vertical =
      actual_list_scroll ||
      (shell->printer_list_vertical_gesture_ &&
       release_abs_dy > tap_movement_limit);
  const bool released_as_printer_tap =
      code == LV_EVENT_RELEASED && pressed_printer_card != nullptr &&
      !actual_list_scroll &&
      release_abs_dx <= tap_movement_limit &&
      release_abs_dy <= tap_movement_limit;
  shell->gesture_active_ = false;
  shell->horizontal_gesture_committed_ = false;
  shell->vertical_gesture_locked_ = false;
  shell->gesture_started_in_printer_list_ = false;
  shell->printer_list_vertical_gesture_ = false;
  shell->printer_list_scroll_started_ = false;
  shell->pressed_printer_card_ = nullptr;
  if (released_as_printer_tap && lv_obj_is_valid(pressed_printer_card)) {
    shell->activate_printer_card(pressed_printer_card);
    return;
  }
  if (list_owned_vertical) return;
  const int vertical_dy =
      kDisplayUsesLargeLayout ? dy : shell->square_gesture_peak_dy_;
  const int vertical_abs_dy = std::abs(vertical_dy);
  const bool released_as_vertical =
      vertical_abs_dy >= kVerticalSwipeThresholdPx &&
      vertical_abs_dy >= std::abs(kDisplayUsesLargeLayout ? dx : shell->square_gesture_peak_dx_) +
                             kVerticalGestureDominancePx;
  if (vertical_locked || released_as_vertical) {
    if (vertical_abs_dy >= kVerticalSwipeThresholdPx) {
      const int depth = shell->horizontal_depth_.load();
      int current = 0;
      int next = 0;
      if (depth == 0) {
        current = shell->page_.load();
        next = vertical_dy > 0 ? (current + 1) % 5 : (current + 4) % 5;
      } else if (depth == 1) {
        const int count = std::max(1, shell->printer_subpage_count_.load());
        current = shell->printer_subpage_.load();
        next = vertical_dy > 0 ? (current + 1) % count
                               : (current + count - 1) % count;
      } else {
        return;
      }
      if (next != current) {
        if (depth == 0) shell->page_.store(next);
        else shell->printer_subpage_.store(next);
        shell->view_ = -1;
        if (shell->navigation_feedback_ != nullptr) {
          shell->navigation_feedback_(shell->navigation_feedback_context_);
        }
        if (shell->page_refresh_requested_ != nullptr) {
          shell->page_refresh_requested_(shell->page_refresh_context_);
        }
      }
    }
    return;
  }

  lv_point_t vector{};
  lv_indev_get_vect(input, &vector);
  const int abs_vector_x = std::abs(static_cast<int>(vector.x));
  const int abs_vector_y = std::abs(static_cast<int>(vector.y));
  const bool normal_swipe =
      abs_dx >= kHorizontalSwipeThresholdPx && abs_dx >= abs_dy + 8;
  const bool fast_flick =
      abs_dx >= kHorizontalFlickMinDisplacementPx &&
      abs_vector_x >= kHorizontalFlickVectorThresholdPx &&
      abs_vector_x >= abs_vector_y && abs_dx >= abs_dy + 8 &&
      ((dx < 0 && vector.x <= 0) || (dx > 0 && vector.x >= 0));
  int target_page = 0;
  bool target_list = true;
  int target_subpage = 0;
  if ((normal_swipe || fast_flick) &&
      shell->horizontal_destination(dx < 0, &target_page, &target_list,
                                    &target_subpage)) {
    shell->start_horizontal_transition(target_page, target_list, dx < 0 ? -1 : 1,
                                       0, target_subpage);
  }
}

void DisplayShell::wifi_setup_pager_event(lv_event_t* event) {
  auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
  if (shell == nullptr) return;
  const lv_event_code_t code = lv_event_get_code(event);
  lv_obj_t* pager = lv_event_get_current_target_obj(event);
  auto switch_step = [&](bool forward) {
    lv_obj_t* active = pager != nullptr ? lv_tileview_get_tile_active(pager) : nullptr;
    const std::uintptr_t step = active != nullptr
        ? reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(active)) : 1U;
    const std::uint32_t target_index = forward ? 1U : 0U;
    if ((forward && step != 1U) || (!forward && step != 2U) || pager == nullptr ||
        target_index >= lv_obj_get_child_count(pager)) {
      return;
    }
    // Moving two large QR codes continuously exceeds the round AMOLED's clean
    // partial-refresh cadence. Keep the card fixed under the finger and switch
    // once the horizontal displacement crosses the normal navigation threshold.
    lv_tileview_set_tile(pager, lv_obj_get_child(pager, target_index), LV_ANIM_OFF);
    lv_obj_send_event(pager, LV_EVENT_VALUE_CHANGED, nullptr);
    if (shell->navigation_feedback_ != nullptr) {
      shell->navigation_feedback_(shell->navigation_feedback_context_);
    }
  };

  if (code == LV_EVENT_VALUE_CHANGED) {
    lv_obj_t* active = pager != nullptr ? lv_tileview_get_tile_active(pager) : nullptr;
    const std::uintptr_t step = active != nullptr
        ? reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(active)) : 1U;
    for (std::size_t index = 0; index < shell->wifi_setup_dots_.size(); ++index) {
      lv_obj_t* dot = shell->wifi_setup_dots_[index];
      if (dot == nullptr || !lv_obj_is_valid(dot)) continue;
      const bool selected = index + 1U == step;
      lv_obj_set_style_border_color(
          dot, lv_color_hex(selected ? shell->accent_color_ : shell->theme_style_.track),
          LV_PART_MAIN);
      lv_obj_set_style_bg_color(
          dot, lv_color_hex(selected ? shell->theme_style_.surface_soft
                                     : shell->theme_style_.surface), LV_PART_MAIN);
    }
    return;
  }

  lv_indev_t* input = lv_indev_active();
  lv_point_t point{};
  if (input != nullptr) lv_indev_get_point(input, &point);
  shell->note_activity(true);
  if (code == LV_EVENT_PRESSED && input != nullptr) {
    shell->gesture_active_ = true;
    shell->horizontal_gesture_committed_ = false;
    shell->gesture_start_x_ = point.x;
    shell->gesture_start_y_ = point.y;
  } else if (code == LV_EVENT_PRESSING && input != nullptr && shell->gesture_active_ &&
             !shell->horizontal_gesture_committed_) {
    const int dx = static_cast<int>(point.x - shell->gesture_start_x_);
    const int dy = static_cast<int>(point.y - shell->gesture_start_y_);
    if (std::abs(dx) >= kHorizontalSwipeThresholdPx &&
        std::abs(dx) >= std::abs(dy) + 8) {
      switch_step(dx < 0);
      shell->gesture_active_ = false;
      shell->horizontal_gesture_committed_ = true;
      lv_indev_wait_release(input);
    }
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    shell->gesture_active_ = false;
    shell->horizontal_gesture_committed_ = false;
  } else if (code == LV_EVENT_LONG_PRESSED && shell->gesture_active_) {
    shell->gesture_active_ = false;
    shell->show_quick_menu();
  }
  if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING ||
      code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
      code == LV_EVENT_LONG_PRESSED || code == LV_EVENT_GESTURE) {
    lv_event_stop_bubbling(event);
  }
}

void DisplayShell::wifi_setup_navigation_event(lv_event_t* event) {
  auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
  if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
      shell->wifi_setup_pager_ == nullptr ||
      !lv_obj_is_valid(shell->wifi_setup_pager_)) {
    return;
  }
  shell->note_activity(true);
  lv_obj_t* active = lv_tileview_get_tile_active(shell->wifi_setup_pager_);
  const std::uintptr_t step = active != nullptr
      ? reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(active)) : 1U;
  const std::uint32_t target_index = step == 1U ? 1U : 0U;
  if (target_index < lv_obj_get_child_count(shell->wifi_setup_pager_)) {
    lv_tileview_set_tile(shell->wifi_setup_pager_,
                         lv_obj_get_child(shell->wifi_setup_pager_, target_index),
                         LV_ANIM_OFF);
    lv_obj_send_event(shell->wifi_setup_pager_, LV_EVENT_VALUE_CHANGED, nullptr);
    if (shell->navigation_feedback_ != nullptr) {
      shell->navigation_feedback_(shell->navigation_feedback_context_);
    }
  }
  lv_event_stop_bubbling(event);
}

void DisplayShell::wifi_setup_language_event(lv_event_t* event) {
  auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
  if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  shell->show_wifi_setup_language_picker();
  lv_event_stop_bubbling(event);
}

void DisplayShell::create_wifi_setup_navigation(lv_obj_t* screen) {
  if (screen == nullptr) return;
  constexpr int navigation_width = kDisplayUsesLargeLayout ? 92 : 60;
  constexpr int navigation_height = kDisplayUsesLargeLayout
                                        ? 36
                                        : kDisplayUsesCompactRoundLayout ? 16 : 28;
  constexpr int circle_size = kDisplayUsesLargeLayout
                                  ? 12
                                  : kDisplayUsesCompactRoundLayout ? 8 : 9;
  constexpr int circle_offset = kDisplayUsesLargeLayout ? 14 : 10;
  constexpr int bottom_offset = kDisplayUsesLargeLayout
                                    ? -10
                                    : kDisplayUsesCompactRoundLayout ? 0 : -1;

  lv_obj_t* navigation = lv_obj_create(screen);
  lv_obj_set_size(navigation, navigation_width, navigation_height);
  lv_obj_align(navigation, LV_ALIGN_BOTTOM_MID, 0, bottom_offset);
  lv_obj_add_flag(navigation, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(navigation, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(navigation, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(navigation, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(navigation, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(navigation, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(navigation, wifi_setup_navigation_event, LV_EVENT_CLICKED, this);

  for (std::size_t index = 0; index < wifi_setup_dots_.size(); ++index) {
    const bool selected = index == 0;
    lv_obj_t* circle = lv_obj_create(navigation);
    wifi_setup_dots_[index] = circle;
    lv_obj_set_size(circle, circle_size, circle_size);
    lv_obj_align(circle, LV_ALIGN_CENTER, index == 0 ? -circle_offset : circle_offset, 0);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circle, lv_color_hex(selected ? theme_style_.surface_soft : theme_style_.surface),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(circle, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        circle, lv_color_hex(selected ? accent_color_ : theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_pad_all(circle, 0, LV_PART_MAIN);
  }
  lv_obj_move_foreground(navigation);
}

void DisplayShell::show_wifi_setup_language_picker() {
  set_capture_overlay_name("language-picker");
  if (quick_overlay_ == nullptr || !lv_obj_is_valid(quick_overlay_)) {
    quick_overlay_ = lv_obj_create(lv_layer_top());
  } else {
    lv_obj_clean(quick_overlay_);
  }
  lv_obj_remove_flag(quick_overlay_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(quick_overlay_, LV_PCT(100), LV_PCT(100));
  lv_obj_center(quick_overlay_);
  lv_obj_remove_flag(quick_overlay_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(quick_overlay_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_radius(quick_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(quick_overlay_, lv_color_hex(theme_style_.background), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(quick_overlay_, LV_GRAD_DIR_NONE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(quick_overlay_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(quick_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(quick_overlay_, 0, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(quick_overlay_);
  lv_label_set_text(title, tr("LANGUAGE"));
  apply_text_style(title, lv_color_hex(accent_color_),
                   kDisplayUsesLargeLayout
                       ? &lv_font_montserrat_24
                       : kDisplayUsesCompactRoundLayout
                             ? &lv_font_montserrat_14
                             : &lv_font_montserrat_16);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0,
               kDisplayUsesLargeLayout
                   ? 34
                   : kDisplayUsesCompactRoundLayout ? 15 : 10);

  constexpr int button_width = kDisplayUsesLargeLayout
                                   ? 290
                                   : kDisplayUsesCompactRoundLayout ? 92 : 100;
  constexpr int button_height = kDisplayUsesLargeLayout
                                    ? 45
                                    : kDisplayUsesCompactRoundLayout ? 36 : 38;
  constexpr int first_y = kDisplayUsesLargeLayout
                              ? 82
                              : kDisplayUsesCompactRoundLayout ? 45 : 47;
  constexpr int spacing = kDisplayUsesLargeLayout
                              ? 50
                              : kDisplayUsesCompactRoundLayout ? 46 : 49;
  for (std::size_t index = 0; index < core::kLanguages.size(); ++index) {
    const core::Language& language = core::kLanguages[index];
    const bool selected = language.code == language_;
    lv_obj_t* button = lv_button_create(quick_overlay_);
    lv_obj_set_size(button, button_width, button_height);
    if constexpr (kDisplayUsesLargeLayout) {
      lv_obj_align(button, LV_ALIGN_TOP_MID, 0,
                   first_y + static_cast<int>(index) * spacing);
    } else {
      const int column = static_cast<int>(index % 2U);
      const int row = static_cast<int>(index / 2U);
      lv_obj_align(button, LV_ALIGN_TOP_LEFT,
                   (kDisplayUsesCompactRoundLayout ? 22 : 14) +
                       column * (kDisplayUsesCompactRoundLayout ? 104 : 112),
                   first_y + row * spacing);
    }
    lv_obj_set_style_radius(button, themed_radius(kDisplayUsesLargeLayout ? 20 : 13), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(selected ? theme_style_.surface_soft : theme_style_.surface_raised),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(button, selected ? 2 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        button, lv_color_hex(selected ? accent_color_ : theme_style_.border), LV_PART_MAIN);
    apply_surface_effect(button);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(index));
    lv_obj_add_event_cb(button, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      const std::size_t index = reinterpret_cast<std::uintptr_t>(
          lv_obj_get_user_data(lv_event_get_current_target_obj(event)));
      if (index >= core::kLanguages.size()) return;
      const core::Language& language = core::kLanguages[index];
      shell->set_language(language.code);
      shell->view_ = -1;
      if (shell->language_changed_ != nullptr) {
        shell->language_changed_(shell->language_changed_context_, language.code.data());
      }
      shell->close_quick_overlay();
      if (shell->page_refresh_requested_ != nullptr) {
        shell->page_refresh_requested_(shell->page_refresh_context_);
      }
      lv_event_stop_bubbling(event);
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, language.native_name.data());
    apply_text_style(label, lv_color_hex(selected ? accent_color_ : theme_style_.text_primary),
                     kDisplayUsesLargeLayout ? &lv_font_montserrat_16 : &lv_font_montserrat_12);
    lv_obj_center(label);
  }

  create_quick_overlay_close_button();
  lv_obj_move_foreground(quick_overlay_);
}

void DisplayShell::nozzle_scroll_event(lv_event_t* event) {
  auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
  if (shell == nullptr) return;
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING ||
      code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
      code == LV_EVENT_LONG_PRESSED || code == LV_EVENT_GESTURE) {
    shell->note_activity(true);
    lv_obj_t* scroll = lv_event_get_current_target_obj(event);
    if (scroll == nullptr || !lv_obj_has_flag(scroll, LV_OBJ_FLAG_SCROLLABLE)) {
      // Four or fewer tools fit without scrolling. Let the gesture continue
      // bubbling to the screen-wide horizontal depth navigator.
      return;
    }
    if constexpr (!kDisplayUsesLargeLayout) {
      // The square strip scrolls horizontally when a fourth tool is present,
      // but vertical page navigation must still work when the swipe starts
      // over it. Let the initial press and vertical motion bubble to the
      // screen router; claim only a clearly horizontal drag for the strip.
      if (code == LV_EVENT_PRESSED) {
        shell->nozzle_scroll_gesture_horizontal_ = false;
        return;
      }
      if (code == LV_EVENT_PRESSING) {
        lv_indev_t* input = lv_indev_active();
        if (input != nullptr) {
          lv_point_t point{};
          lv_indev_get_point(input, &point);
          const int dx = static_cast<int>(point.x - shell->gesture_start_x_);
          const int dy = static_cast<int>(point.y - shell->gesture_start_y_);
          if (std::abs(dx) >= 8 && std::abs(dx) >= std::abs(dy) + 4) {
            shell->nozzle_scroll_gesture_horizontal_ = true;
            shell->gesture_active_ = false;
          }
        }
      }
      const bool horizontal = shell->nozzle_scroll_gesture_horizontal_;
      if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        shell->nozzle_scroll_gesture_horizontal_ = false;
      }
      if (horizontal) lv_event_stop_bubbling(event);
      return;
    }
    // A horizontal drag in the nozzle strip scrolls tools. It must not bubble
    // into the screen-wide depth navigation gesture recognizer.
    lv_event_stop_bubbling(event);
  }
}

void DisplayShell::printer_list_scroll_event(lv_event_t* event) {
  auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
  if (shell == nullptr) return;
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_SCROLL_BEGIN) {
    shell->printer_list_vertical_gesture_ = true;
    shell->printer_list_scroll_started_ = true;
    shell->suppress_update_click_ = true;
  } else if (code == LV_EVENT_SCROLL) {
    shell->update_printer_list_scroll_position();
  } else if (code == LV_EVENT_SCROLL_END) {
    lv_obj_t* list = lv_event_get_current_target_obj(event);
    if (list == nullptr || !lv_obj_is_valid(list)) return;
    const int current = static_cast<int>(lv_obj_get_scroll_y(list));
    const int maximum = std::max(
        0, current + static_cast<int>(lv_obj_get_scroll_bottom(list)));
    const int bounded = std::clamp(current, 0, maximum);
    if (bounded != current) {
      lv_obj_scroll_to_y(list, static_cast<lv_coord_t>(bounded), LV_ANIM_ON);
    }
    shell->update_printer_list_scroll_position();
  }
}

void DisplayShell::configure_printer_list_scroll(lv_obj_t* list,
                                                 std::size_t printer_count,
                                                 std::size_t visible_count,
                                                 int item_pitch) {
  printer_list_scroll_ = list;
  printer_list_count_ = printer_count;
  printer_list_visible_count_ = visible_count;
  printer_list_item_pitch_ = item_pitch;
  if (list == nullptr || printer_count <= visible_count || visible_count == 0 ||
      item_pitch <= 0) {
    printer_list_first_visible_ = 0;
    return;
  }

  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_START);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_width(list, kDisplayUsesLargeLayout ? 4 : 3,
                         LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(list, lv_color_hex(accent_color_),
                            LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_70, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(list, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
  lv_obj_set_style_border_width(list, 0, LV_PART_SCROLLBAR);
  if constexpr (kDisplayUsesCompactRoundLayout) {
    lv_obj_set_style_translate_x(list, 13, LV_PART_SCROLLBAR);
  }
  lv_obj_add_event_cb(list, printer_list_scroll_event, LV_EVENT_SCROLL_BEGIN, this);
  lv_obj_add_event_cb(list, printer_list_scroll_event, LV_EVENT_SCROLL, this);
  lv_obj_add_event_cb(list, printer_list_scroll_event, LV_EVENT_SCROLL_END, this);

  lv_obj_update_layout(list);
  const std::size_t maximum_start = printer_count - visible_count;
  printer_list_first_visible_ = std::min(printer_list_first_visible_, maximum_start);
  lv_obj_scroll_to_y(list,
                     static_cast<lv_coord_t>(printer_list_first_visible_ * item_pitch),
                     LV_ANIM_OFF);
  update_printer_list_scroll_position();
}

void DisplayShell::update_printer_list_scroll_position() {
  if (printer_list_scroll_ == nullptr ||
      !lv_obj_is_valid(printer_list_scroll_) ||
      printer_list_item_pitch_ <= 0 || printer_list_visible_count_ == 0) {
    return;
  }
  const std::size_t maximum_start =
      printer_list_count_ > printer_list_visible_count_
          ? printer_list_count_ - printer_list_visible_count_
          : 0;
  const int scroll_y = std::max(0, static_cast<int>(
      lv_obj_get_scroll_y(printer_list_scroll_)));
  const std::size_t first = std::min(
      maximum_start,
      static_cast<std::size_t>((scroll_y + printer_list_item_pitch_ / 2) /
                               printer_list_item_pitch_));
  printer_list_first_visible_ = first;
}

bool DisplayShell::horizontal_destination(bool forward, int* target_page,
                                          bool* target_printer_list,
                                          int* target_printer_subpage) const {
  if (target_page == nullptr || target_printer_list == nullptr ||
      target_printer_subpage == nullptr) return false;
  // Wi-Fi onboarding is not part of the page carousel. Treating a small
  // long-press drift as navigation here creates a curtain with no destination
  // renderer to acknowledge it.
  if (view_ == 1) return false;
  const int depth = horizontal_depth_.load();
  const bool dashboard_available = selected_profile_ != 0 &&
                                   selected_online_.load();
  const int last_depth = std::max(1, horizontal_depth_count_.load() - 1);
  *target_page = 0;
  if (forward) {
    if (depth == 0 && dashboard_available) {
      *target_printer_list = false;
      *target_printer_subpage = 1;
      return true;
    }
    if (depth > 0 && depth < last_depth) {
      *target_printer_list = false;
      *target_printer_subpage = depth + 1;
      return true;
    }
    return false;
  }
  if (depth == 0) return false;
  if (depth > 1) {
    *target_printer_list = false;
    *target_printer_subpage = depth - 1;
    return true;
  }
  if (depth == 1) {
    *target_printer_list = true;
    *target_printer_subpage = 0;
    return true;
  }
  return false;
}

void DisplayShell::start_horizontal_transition(int target_page, bool show_printer_list,
                                               int direction,
                                               std::uint32_t target_profile_id,
                                               int target_printer_subpage) {
  if (horizontal_transition_active_) return;
  set_capture_overlay_name("loading-printer");
  horizontal_transition_overlay_ = lv_obj_create(lv_layer_top());
  if (horizontal_transition_overlay_ == nullptr) {
    clear_capture_overlay_name("loading-printer");
    return;
  }
  horizontal_transition_active_ = true;
  horizontal_transition_target_applied_ = false;
  horizontal_transition_reveal_started_ = false;
  horizontal_transition_direction_ = direction < 0 ? -1 : 1;
  horizontal_transition_target_page_ = std::clamp(target_page, 0, 4);
  horizontal_transition_target_printer_list_ = show_printer_list;
  horizontal_transition_target_profile_id_ = target_profile_id;
  horizontal_transition_target_depth_ = show_printer_list ? 0 : std::max(1, target_printer_subpage);
  lv_obj_set_size(horizontal_transition_overlay_, LV_PCT(100), LV_PCT(100));
  lv_obj_center(horizontal_transition_overlay_);
  lv_obj_remove_flag(horizontal_transition_overlay_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(horizontal_transition_overlay_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_radius(horizontal_transition_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(horizontal_transition_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(horizontal_transition_overlay_,
                            lv_color_hex(theme_style_.background), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(horizontal_transition_overlay_, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t* spinner = lv_spinner_create(horizontal_transition_overlay_);
  lv_obj_set_size(spinner, 58, 58);
  lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(accent_color_), LV_PART_INDICATOR);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -18);
  lv_obj_t* label = lv_label_create(horizontal_transition_overlay_);
  lv_label_set_text(label, tr("LOADING"));
  apply_text_style(label, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_14);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 42);
  lv_obj_move_foreground(horizontal_transition_overlay_);

  if (navigation_feedback_ != nullptr) {
    navigation_feedback_(navigation_feedback_context_);
  }
  // Give the opaque curtain one LVGL refresh cycle before invalidating and
  // rebuilding the destination underneath it. Only the small spinner moves.
  lv_timer_t* timer = lv_timer_create(horizontal_transition_switch,
                                      kHorizontalLoadingDelayMs, this);
  if (timer != nullptr) lv_timer_set_repeat_count(timer, 1);
  horizontal_transition_timeout_timer_ =
      lv_timer_create(horizontal_transition_timeout, kHorizontalLoadingTimeoutMs, this);
  if (horizontal_transition_timeout_timer_ != nullptr) {
    lv_timer_set_repeat_count(horizontal_transition_timeout_timer_, 1);
  }
}

void DisplayShell::horizontal_transition_switch(lv_timer_t* timer) {
  auto* shell = static_cast<DisplayShell*>(lv_timer_get_user_data(timer));
  if (shell == nullptr || !shell->horizontal_transition_active_) return;
  shell->page_.store(shell->horizontal_transition_target_page_);
  shell->horizontal_depth_.store(shell->horizontal_transition_target_depth_);
  shell->view_ = -1;
  shell->horizontal_transition_target_applied_ = true;
  // The page-refresh notification wakes the core-0 monitor while this LVGL
  // timer callback still owns the adapter mutex. Let the callback return
  // before the destination renderer tries to acquire the same mutex.
  shell->defer_background_render(kTransitionBackgroundRenderQuietMs);
  if (shell->page_refresh_requested_ != nullptr) {
    shell->page_refresh_requested_(shell->page_refresh_context_);
  }
}

void DisplayShell::cancel_horizontal_transition_locked(bool stop_reveal_animation) {
  if (horizontal_transition_timeout_timer_ != nullptr) {
    lv_timer_delete(horizontal_transition_timeout_timer_);
    horizontal_transition_timeout_timer_ = nullptr;
  }
  if (horizontal_transition_overlay_ != nullptr &&
      lv_obj_is_valid(horizontal_transition_overlay_)) {
    if (stop_reveal_animation) {
      lv_anim_delete(horizontal_transition_overlay_, nullptr);
    }
    lv_obj_delete(horizontal_transition_overlay_);
  }
  horizontal_transition_overlay_ = nullptr;
  horizontal_transition_active_ = false;
  horizontal_transition_target_applied_ = false;
  horizontal_transition_reveal_started_ = false;
  horizontal_transition_target_profile_id_ = 0;
  clear_capture_overlay_name("loading-printer");
}

void DisplayShell::horizontal_transition_timeout(lv_timer_t* timer) {
  auto* shell = static_cast<DisplayShell*>(lv_timer_get_user_data(timer));
  if (shell == nullptr || !shell->horizontal_transition_active_) return;
  // This one-shot timer is deleted by LVGL after the callback. Clearing the
  // member first prevents cancel_horizontal_transition_locked() from deleting
  // the timer while its callback is running.
  shell->horizontal_transition_timeout_timer_ = nullptr;
  shell->cancel_horizontal_transition_locked();
  shell->page_.store(0);
  shell->horizontal_depth_.store(0);
  shell->printer_subpage_.store(0);
  shell->view_ = -1;
  if (shell->page_refresh_requested_ != nullptr) {
    shell->page_refresh_requested_(shell->page_refresh_context_);
  }
}

void DisplayShell::finish_horizontal_transition(int rendered_page,
                                                bool rendered_printer_list,
                                                std::uint32_t rendered_profile_id) {
  if (board_display_lock(250) != ESP_OK) return;
  if (!horizontal_transition_active_ || horizontal_transition_overlay_ == nullptr ||
      !horizontal_transition_target_applied_ || horizontal_transition_reveal_started_ ||
      rendered_page != horizontal_transition_target_page_ ||
      (rendered_page == 0 && rendered_printer_list !=
       (horizontal_transition_target_depth_ == 0)) ||
      (horizontal_transition_target_profile_id_ != 0 &&
       rendered_profile_id != horizontal_transition_target_profile_id_)) {
    board_display_unlock();
    return;
  }
  if (!lv_obj_is_valid(horizontal_transition_overlay_)) {
    cancel_horizontal_transition_locked();
    board_display_unlock();
    return;
  }
  horizontal_transition_reveal_started_ = true;
  lv_anim_delete(horizontal_transition_overlay_, nullptr);
  lv_anim_t reveal;
  lv_anim_init(&reveal);
  lv_anim_set_var(&reveal, horizontal_transition_overlay_);
  lv_anim_set_user_data(&reveal, this);
  lv_anim_set_exec_cb(&reveal, [](void* object, std::int32_t value) {
    lv_obj_set_x(static_cast<lv_obj_t*>(object), value);
  });
  lv_anim_set_values(&reveal, 0, horizontal_transition_direction_ * kDisplayWidth);
  lv_anim_set_duration(&reveal, kHorizontalRevealDurationMs);
  lv_anim_set_path_cb(&reveal, lv_anim_path_ease_out);
  lv_anim_set_completed_cb(&reveal, horizontal_transition_finished);
  lv_anim_start(&reveal);
  board_display_unlock();
}

void DisplayShell::horizontal_transition_finished(lv_anim_t* animation) {
  auto* shell = static_cast<DisplayShell*>(lv_anim_get_user_data(animation));
  if (shell == nullptr) return;
  shell->cancel_horizontal_transition_locked(false);
}

void DisplayShell::create_quick_overlay_close_button() {
  if (quick_overlay_ == nullptr || !lv_obj_is_valid(quick_overlay_)) return;
  lv_obj_t* close = lv_button_create(quick_overlay_);
  lv_obj_set_size(close, kDisplayUsesLargeLayout ? 62 : 34,
                  kDisplayUsesLargeLayout ? 62 : 34);
  lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, kDisplayUsesLargeLayout ? -16 : -4);
  lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(close, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
  if constexpr (kDisplayUsesLargeLayout) {
    lv_obj_set_style_border_width(close, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(close, lv_color_hex(theme_style_.text_muted), LV_PART_MAIN);
  } else {
    lv_obj_set_ext_click_area(close, 4);
  }
  lv_obj_t* close_label = lv_label_create(close);
  lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
  lv_obj_align(close_label, LV_ALIGN_CENTER, 0, kDisplayUsesLargeLayout ? 0 : 2);
  lv_obj_add_flag(close_label, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(close, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell != nullptr && lv_event_get_code(event) == LV_EVENT_CLICKED) {
      lv_event_stop_bubbling(event);
      shell->close_quick_overlay();
    }
  }, LV_EVENT_CLICKED, this);
  lv_obj_move_foreground(close);
}

void DisplayShell::show_quick_menu() {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_quick_menu();
    return;
  }
  set_capture_overlay_name("quick-menu");
  if (quick_overlay_ == nullptr || !lv_obj_is_valid(quick_overlay_)) {
    quick_overlay_ = lv_obj_create(lv_layer_top());
  } else {
    lv_obj_clean(quick_overlay_);
  }
  lv_obj_remove_flag(quick_overlay_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(quick_overlay_, 466, 466);
  lv_obj_center(quick_overlay_);
  lv_obj_remove_flag(quick_overlay_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(quick_overlay_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_radius(quick_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(quick_overlay_, lv_color_hex(theme_style_.background), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(quick_overlay_, LV_GRAD_DIR_NONE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(quick_overlay_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(quick_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(quick_overlay_, 0, LV_PART_MAIN);
  lv_obj_t* title = lv_label_create(quick_overlay_);
  lv_label_set_text(title, tr("QUICK MENU"));
  apply_text_style(title, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_32);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

  static constexpr const char* icons[]{LV_SYMBOL_EYE_OPEN, LV_SYMBOL_VOLUME_MAX, "Aa"};
  static constexpr const char* labels[]{"DISPLAY & BRIGHTNESS", "SOUNDS", "LANGUAGE"};
  const std::uint32_t colors[]{theme_style_.accent_secondary,
                               theme_colors_.preparing, theme_colors_.done};
  for (int index = 0; index < 3; ++index) {
    lv_obj_t* button = lv_button_create(quick_overlay_);
    if (index == 0) {
      lv_obj_set_size(button, 302, 112);
      lv_obj_align(button, LV_ALIGN_CENTER, 0, -63);
    } else {
      lv_obj_set_size(button, 142, 108);
      lv_obj_align(button, LV_ALIGN_CENTER, index == 1 ? -80 : 80, 65);
    }
    lv_obj_set_style_radius(button, themed_radius(30), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(colors[index]), LV_PART_MAIN);
    apply_surface_effect(button);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<std::intptr_t>(index)));
    lv_obj_t* icon = lv_label_create(button);
    lv_label_set_text(icon, icons[index]);
    apply_icon_text_style(icon, lv_color_hex(theme_style_.text_primary),
                          &lv_font_montserrat_32);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, index == 0 ? -29 : -19);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, tr(labels[index]));
    apply_text_style(label, lv_color_hex(colors[index]), &lv_font_montserrat_16);
    if (index == 0) {
      lv_obj_set_size(label, 270, 20);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_align(label, LV_ALIGN_CENTER, 0, 2);
      lv_obj_t* detail = lv_label_create(button);
      lv_label_set_text_fmt(detail, "%d%%  /  %s",
                            std::clamp(board_display_brightness_get(), 10, 100),
                            tr(theme_display_name(active_theme_)));
      apply_text_style(detail, lv_color_hex(theme_style_.text_secondary),
                       &lv_font_montserrat_14);
      lv_obj_set_size(detail, 270, 18);
      lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_align(detail, LV_ALIGN_CENTER, 0, 29);
    } else {
      lv_obj_set_width(label, 126);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(label, LV_ALIGN_CENTER, 0, 26);
    }
    lv_obj_add_event_cb(button, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (lv_event_get_code(event) != LV_EVENT_CLICKED || shell == nullptr) return;
      lv_event_stop_bubbling(event);
      const int action = static_cast<int>(reinterpret_cast<std::intptr_t>(
          lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
      // Each destination replaces the children of quick_overlay_. Defer that
      // replacement until LVGL has finished dispatching this button's event;
      // deleting the current event target from inside its own callback corrupts
      // the event stack after several quick-menu visits.
      shell->pending_quick_menu_action_ = action;
      if (lv_async_call(quick_menu_action_async, shell) != LV_RESULT_OK) {
        shell->pending_quick_menu_action_ = -1;
      }
    }, LV_EVENT_CLICKED, this);
  }

  create_quick_overlay_close_button();
  lv_obj_move_foreground(quick_overlay_);
}

void DisplayShell::quick_menu_action_async(void* context) {
  auto* shell = static_cast<DisplayShell*>(context);
  if (shell == nullptr) return;
  const int action = shell->pending_quick_menu_action_;
  shell->pending_quick_menu_action_ = -1;
  if (shell->quick_overlay_ == nullptr || !lv_obj_is_valid(shell->quick_overlay_)) return;
  if (action == 0) shell->show_brightness_overlay();
  else if (action == 1) shell->show_audio_overlay();
  else if (action == 2) shell->show_wifi_setup_language_picker();
  else if (action == 3) shell->show_theme_overlay();
  else if (action == 4) shell->show_quick_menu();
}

void DisplayShell::request_theme_selection(const char* theme) {
  static constexpr const char* ids[]{"green", "banana", "sunset", "ice",
                                      "cyberpunk", "ember", "mono", "red",
                                      "ios_glass", "fluent_dark", "retro_terminal", "custom"};
  if (theme == nullptr) return;
  int index = -1;
  for (int candidate = 0; candidate < 12; ++candidate) {
    if (std::strcmp(theme, ids[candidate]) == 0) {
      index = candidate;
      break;
    }
  }
  if (index < 0) return;
  pending_theme_selection_ = index;
  if (theme_selection_timer_ != nullptr) return;
  // A zero-delay async callback may still run in the same lv_timer_handler()
  // pass as the pointer event that created it. Give the input device one full
  // released-state cycle before hiding its target and waking the settings task.
  theme_selection_timer_ = lv_timer_create(theme_selection_timer, 75, this);
  if (theme_selection_timer_ == nullptr) {
    pending_theme_selection_ = -1;
    return;
  }
  lv_timer_set_repeat_count(theme_selection_timer_, 1);
}

void DisplayShell::theme_selection_timer(lv_timer_t* timer) {
  auto* shell = static_cast<DisplayShell*>(lv_timer_get_user_data(timer));
  if (shell == nullptr) return;
  static constexpr const char* ids[]{"green", "banana", "sunset", "ice",
                                      "cyberpunk", "ember", "mono", "red",
                                      "ios_glass", "fluent_dark", "retro_terminal", "custom"};
  const int index = shell->pending_theme_selection_;
  shell->pending_theme_selection_ = -1;
  shell->theme_selection_timer_ = nullptr;
  if (index < 0 || index >= 12) return;

  // The button event has fully unwound before this callback runs. Hide the
  // overlay first, then notify the worker which persists the theme and rebuilds
  // the active screen under the normal LVGL lock.
  shell->close_quick_overlay();
  if (shell->theme_changed_ != nullptr) {
    shell->theme_changed_(shell->theme_changed_context_, ids[index]);
  }
  ESP_LOGI(kLogTag, "Deferred theme selection: %s", ids[index]);
}

void DisplayShell::show_brightness_overlay() {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_brightness_overlay();
    return;
  }
  if (quick_overlay_ == nullptr || !lv_obj_is_valid(quick_overlay_)) return;
  set_capture_overlay_name("brightness");
  lv_obj_clean(quick_overlay_);
  lv_obj_t* card = lv_obj_create(quick_overlay_);
  lv_obj_set_size(card, 376, 382);
  lv_obj_center(card);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(card, themed_radius(34), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(card);
  lv_label_set_text(title, tr("DISPLAY & BRIGHTNESS"));
  apply_text_style(title, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_16);
  lv_obj_set_size(title, 220, 20);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 33);

  lv_obj_t* slider = lv_slider_create(card);
  lv_obj_set_size(slider, 312, 18);
  lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 91);
  lv_slider_set_range(slider, 10, 100);
  lv_slider_set_value(slider, std::clamp(board_display_brightness_get(), 10, 100), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.accent_secondary), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.text_primary), LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
  // The knob extends past the track at both endpoints. Include the complete
  // visible knob plus a finger-sized margin so 10% and 100% remain draggable.
  lv_obj_set_ext_click_area(slider, 32);
  lv_obj_add_event_cb(slider, [](lv_event_t* event) {
    lv_event_stop_bubbling(event);
    auto* slider = lv_event_get_target_obj(event);
    const int next = std::clamp<int>(lv_slider_get_value(slider), 10, 100);
    board_display_brightness_set(next);
  }, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(slider, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    auto* slider = lv_event_get_target_obj(event);
    if (shell == nullptr || slider == nullptr || lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    lv_event_stop_bubbling(event);
    const int next = std::clamp<int>(lv_slider_get_value(slider), 10, 100);
    shell->applied_brightness_ = next;
    if (shell->brightness_changed_ != nullptr) {
      shell->brightness_changed_(shell->brightness_changed_context_, next);
    }
    shell->close_quick_overlay();
  }, LV_EVENT_RELEASED, this);

  lv_obj_t* theme = lv_button_create(card);
  lv_obj_set_size(theme, 312, 66);
  lv_obj_align(theme, LV_ALIGN_TOP_MID, 0, 145);
  lv_obj_set_style_radius(theme, themed_radius(20), LV_PART_MAIN);
  lv_obj_set_style_bg_color(theme, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
  lv_obj_set_style_border_width(theme, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(theme, lv_color_hex(theme_style_.accent), LV_PART_MAIN);
  apply_surface_effect(theme);
  lv_obj_t* theme_icon = lv_label_create(theme);
  lv_label_set_text(theme_icon, LV_SYMBOL_IMAGE);
  apply_icon_text_style(theme_icon, lv_color_hex(theme_style_.accent),
                        &lv_font_montserrat_24);
  lv_obj_set_size(theme_icon, 30, 30);
  lv_obj_align(theme_icon, LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_t* theme_label = lv_label_create(theme);
  lv_label_set_text_fmt(theme_label, "%s: %s", tr("THEME"),
                        tr(theme_display_name(active_theme_)));
  apply_text_style(theme_label, lv_color_hex(theme_style_.text_primary),
                   &lv_font_montserrat_14);
  lv_obj_set_size(theme_label, 205, 20);
  lv_label_set_long_mode(theme_label, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(theme_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_align(theme_label, LV_ALIGN_LEFT_MID, 57, 0);
  lv_obj_t* theme_next = lv_label_create(theme);
  lv_label_set_text(theme_next, LV_SYMBOL_RIGHT);
  apply_icon_text_style(theme_next, lv_color_hex(theme_style_.text_muted),
                        &lv_font_montserrat_16);
  lv_obj_align(theme_next, LV_ALIGN_RIGHT_MID, -14, 0);
  lv_obj_add_event_cb(theme, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_event_stop_bubbling(event);
    shell->pending_quick_menu_action_ = 3;
    if (lv_async_call(quick_menu_action_async, shell) != LV_RESULT_OK) {
      shell->pending_quick_menu_action_ = -1;
    }
  }, LV_EVENT_CLICKED, this);

  lv_obj_t* animation_row = lv_obj_create(card);
  lv_obj_set_size(animation_row, 312, 66);
  lv_obj_align(animation_row, LV_ALIGN_TOP_MID, 0, 235);
  lv_obj_add_flag(animation_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(animation_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(animation_row, themed_radius(20), LV_PART_MAIN);
  lv_obj_set_style_bg_color(animation_row, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
  lv_obj_set_style_border_width(animation_row, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(animation_row, lv_color_hex(theme_colors_.printing), LV_PART_MAIN);
  lv_obj_set_style_pad_all(animation_row, 0, LV_PART_MAIN);
  apply_surface_effect(animation_row);
  lv_obj_t* animation_icon = create_printer_animation_icon(
      animation_row, 26, theme_colors_.printing);
  lv_obj_align(animation_icon, LV_ALIGN_LEFT_MID, 14, 0);
  lv_obj_t* animation_label = lv_label_create(animation_row);
  lv_label_set_text(animation_label, tr("PRINTER ANIMATIONS"));
  apply_text_style(animation_label, lv_color_hex(theme_style_.text_primary),
                   &lv_font_montserrat_14);
  lv_obj_set_size(animation_label, 172, 18);
  lv_label_set_long_mode(animation_label, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_align(animation_label, LV_ALIGN_LEFT_MID, 57, 0);
  lv_obj_t* animation_switch = lv_switch_create(animation_row);
  lv_obj_set_size(animation_switch, 58, 32);
  lv_obj_align(animation_switch, LV_ALIGN_RIGHT_MID, -14, 0);
  lv_obj_set_style_bg_color(animation_switch, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_bg_color(animation_switch, lv_color_hex(theme_colors_.printing),
                            static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                                static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
  lv_obj_set_style_bg_color(animation_switch, lv_color_hex(theme_style_.text_primary),
                            LV_PART_KNOB);
  lv_obj_set_ext_click_area(animation_switch, 10);
  if (printer_animations_enabled_) {
    lv_obj_add_state(animation_switch, LV_STATE_CHECKED);
  }
  lv_obj_set_user_data(animation_row, animation_switch);
  lv_obj_add_event_cb(animation_row, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_event_stop_bubbling(event);
    auto* animation_switch = static_cast<lv_obj_t*>(
        lv_obj_get_user_data(lv_event_get_current_target_obj(event)));
    if (animation_switch == nullptr || !lv_obj_is_valid(animation_switch)) return;
    const bool enabled = !lv_obj_has_state(animation_switch, LV_STATE_CHECKED);
    if (enabled) lv_obj_add_state(animation_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(animation_switch, LV_STATE_CHECKED);
    if (shell->printer_animations_enabled_ == enabled) return;
    shell->apply_printer_animations_enabled(enabled);
    if (shell->printer_animations_changed_ != nullptr) {
      shell->printer_animations_changed_(shell->printer_animations_changed_context_, enabled);
    }
    shell->close_quick_overlay();
  }, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(animation_switch, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    lv_event_stop_bubbling(event);
    const bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    if (shell->printer_animations_enabled_ == enabled) return;
    shell->apply_printer_animations_enabled(enabled);
    if (shell->printer_animations_changed_ != nullptr) {
      shell->printer_animations_changed_(shell->printer_animations_changed_context_, enabled);
    }
    shell->close_quick_overlay();
  }, LV_EVENT_VALUE_CHANGED, this);

  create_quick_overlay_close_button();
}

void DisplayShell::show_audio_overlay() {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_audio_overlay();
    return;
  }
  if (quick_overlay_ == nullptr || !lv_obj_is_valid(quick_overlay_)) return;
  set_capture_overlay_name("audio");
  lv_obj_clean(quick_overlay_);
  lv_obj_t* card = lv_obj_create(quick_overlay_);
  lv_obj_set_size(card, 350, 370);
  lv_obj_center(card);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(card, themed_radius(34), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
  lv_obj_t* title = lv_label_create(card);
  lv_label_set_text(title, tr("AUDIO"));
  apply_text_style(title, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_24);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
  const int current = std::clamp(audio_volume_ > 0 ? audio_volume_ : 80, 1, 100);
  lv_obj_t* preset_title = lv_label_create(card);
  lv_label_set_text(preset_title, tr("SOUND SET"));
  apply_text_style(preset_title, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
  lv_obj_align(preset_title, LV_ALIGN_TOP_LEFT, 25, 61);
  static constexpr const char* preset_ids[]{"modern", "soft", "oldschool",
                                             "arcade", "scifi", "clean"};
  static constexpr const char* preset_names[]{"MODERN", "SOFT", "RETRO",
                                               "ARCADE", "SCI-FI", "VOICE"};
  for (int index = 0; index < 6; ++index) {
    const bool active = audio_preset_ == preset_ids[index];
    lv_obj_t* button = lv_button_create(card);
    lv_obj_set_size(button, 92, 32);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 25 + (index % 3) * 104,
                 83 + (index / 3) * 38);
    lv_obj_set_style_radius(button, themed_radius(12), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, active ? 3 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        button, lv_color_hex(active ? theme_style_.accent_secondary : theme_style_.border), LV_PART_MAIN);
    apply_surface_effect(button);
    lv_obj_set_user_data(button,
                         reinterpret_cast<void*>(static_cast<std::intptr_t>(index)));
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, tr(preset_names[index]));
    apply_text_style(label, lv_color_hex(active ? theme_style_.accent_secondary : theme_style_.text_secondary),
                     &lv_font_montserrat_12);
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(button, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      static constexpr const char* ids[]{"modern", "soft", "oldschool",
                                          "arcade", "scifi", "clean"};
      const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(
          lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
      if (index < 0 || index >= 6) return;
      shell->audio_preset_ = ids[index];
      if (shell->audio_preset_changed_ != nullptr) {
        shell->audio_preset_changed_(shell->audio_preset_changed_context_, ids[index]);
      }
      shell->close_quick_overlay();
    }, LV_EVENT_CLICKED, this);
  }
  lv_obj_t* mute_button = lv_button_create(card);
  lv_obj_set_size(mute_button, 48, 48);
  lv_obj_align(mute_button, LV_ALIGN_TOP_LEFT, 25, 194);
  lv_obj_set_style_radius(mute_button, themed_radius(15), LV_PART_MAIN);
  lv_obj_set_style_bg_color(mute_button, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
  lv_obj_set_style_border_width(mute_button, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(mute_button, lv_color_hex(theme_style_.accent_secondary), LV_PART_MAIN);
  lv_obj_set_ext_click_area(mute_button, 10);
  apply_surface_effect(mute_button);
  lv_obj_t* mute_icon = lv_label_create(mute_button);
  lv_label_set_text(mute_icon, audio_enabled_ ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
  apply_icon_text_style(mute_icon, lv_color_hex(theme_style_.accent_secondary),
                        &lv_font_montserrat_16);
  lv_obj_center(mute_icon);
  lv_obj_add_flag(mute_icon, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(mute_button, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_event_stop_bubbling(event);
    shell->audio_enabled_ = !shell->audio_enabled_;
    if (shell->audio_enabled_ && shell->audio_volume_ <= 0) shell->audio_volume_ = 80;
    if (shell->audio_changed_ != nullptr) {
      shell->audio_changed_(shell->audio_changed_context_, shell->audio_enabled_,
                            shell->audio_volume_);
    }
    shell->close_quick_overlay();
  }, LV_EVENT_CLICKED, this);

  lv_obj_t* slider = lv_slider_create(card);
  lv_obj_set_size(slider, 232, 22);
  lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 91, 207);
  // Keep both endpoint knobs easy to acquire. At 0/100 the knob center lies
  // on the slider boundary, so the hit box must extend beyond the track.
  lv_obj_set_ext_click_area(slider, 36);
  lv_slider_set_range(slider, 1, 100);
  lv_slider_set_value(slider, current, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.accent_secondary), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(theme_style_.text_primary), LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 13, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    auto* slider = lv_event_get_target_obj(event);
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    lv_event_stop_bubbling(event);
    shell->audio_volume_ = std::clamp<int>(lv_slider_get_value(slider), 1, 100);
    shell->audio_enabled_ = true;
    if (shell->audio_changed_ != nullptr) {
      shell->audio_changed_(shell->audio_changed_context_, shell->audio_enabled_,
                            shell->audio_volume_);
    }
    shell->close_quick_overlay();
  }, LV_EVENT_RELEASED, this);
  create_quick_overlay_close_button();
}

void DisplayShell::show_theme_overlay() {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_theme_overlay();
    return;
  }
  if (quick_overlay_ == nullptr || !lv_obj_is_valid(quick_overlay_)) return;
  set_capture_overlay_name("theme");
  lv_obj_clean(quick_overlay_);
  lv_obj_t* title = lv_label_create(quick_overlay_);
  lv_label_set_text(title, tr("CHOOSE THEME"));
  apply_text_style(title, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_32);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);
  static constexpr const char* ids[]{"green", "banana", "sunset", "ice",
                                      "cyberpunk", "ember", "mono", "red",
                                      "ios_glass", "fluent_dark", "retro_terminal", "custom"};
  static constexpr const char* names[]{"SIGNAL", "BANANA", "SOLSTICE", "GLACIER",
                                        "AURORA", "GROVE", "GRAPHITE", "GARNET",
                                        "MIDNIGHT HALO", "DRAGON", "TERMINAL", "CUSTOM"};
  for (int index = 0; index < 12; ++index) {
    const core::ThemeColors colors = core::resolved_theme(ids[index], custom_theme_colors_);
    const core::ThemeStyle style = core::resolved_theme_style(ids[index], colors);
    lv_obj_t* button = lv_button_create(quick_overlay_);
    lv_obj_set_size(button, 80, 66);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 55 + (index % 4) * 92,
                 112 + (index / 4) * 82);
    const int card_radius = style.corner_radius == 0
                                ? 0
                                : (style.corner_radius < 12 ? style.corner_radius : 20);
    lv_obj_set_style_radius(button, card_radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(style.surface_raised), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, active_theme_ == ids[index] ? 4 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(colors.printing), LV_PART_MAIN);
    lv_obj_set_user_data(button, const_cast<char*>(ids[index]));
    lv_obj_t* name = lv_label_create(button);
    lv_label_set_text(name, tr(names[index]));
    apply_text_style(name, lv_color_hex(style.text_primary), &lv_font_montserrat_14);
    lv_obj_set_width(name, 82);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(name);
    lv_obj_add_event_cb(button, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      const char* id = static_cast<const char*>(
          lv_obj_get_user_data(lv_event_get_current_target_obj(event)));
      if (shell == nullptr || id == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      // Persisting the setting wakes a worker which rebuilds the active view.
      // Defer both that notification and hiding this button's ancestor until
      // LVGL has completely unwound the current input event.
      shell->request_theme_selection(id);
    }, LV_EVENT_CLICKED, this);
  }
  create_quick_overlay_close_button();
}

void DisplayShell::close_quick_overlay() {
  lv_async_call_cancel(quick_menu_action_async, this);
  pending_quick_menu_action_ = -1;
  if (theme_selection_timer_ != nullptr) {
    lv_timer_delete(theme_selection_timer_);
    theme_selection_timer_ = nullptr;
    pending_theme_selection_ = -1;
  }
  if (quick_overlay_ != nullptr && lv_obj_is_valid(quick_overlay_)) {
    // Reuse one overlay for the lifetime of the UI. Hiding is safe from a
    // child callback and avoids repeated asynchronous destruction/allocation.
    lv_obj_add_flag(quick_overlay_, LV_OBJ_FLAG_HIDDEN);
  }
  capture_overlay_name_.clear();
  note_activity(false);
}

void DisplayShell::display_draw_failed(void* context) {
  auto* shell = static_cast<DisplayShell*>(context);
  if (shell == nullptr || shell->draw_recovery_pending_.exchange(true)) return;
  // Keep the flag set if LVGL cannot allocate the asynchronous call. The
  // existing monitor path below will then perform the same recovery without a
  // new LVGL allocation.
  lv_async_call(display_draw_recovery_async, shell);
}

void DisplayShell::display_draw_recovery_async(void* context) {
  auto* shell = static_cast<DisplayShell*>(context);
  if (shell == nullptr ||
      !shell->draw_recovery_pending_.exchange(false,
                                              std::memory_order_acq_rel)) {
    return;
  }
  shell->recover_failed_draw_locked();
}

void DisplayShell::recover_failed_draw_locked() {
  bool overlay_rolled_back = false;
  if (quick_overlay_ != nullptr && lv_obj_is_valid(quick_overlay_) &&
      !lv_obj_has_flag(quick_overlay_, LV_OBJ_FLAG_HIDDEN)) {
    close_quick_overlay();
    overlay_rolled_back = true;
  }
  const bool update_installing =
      update_state_ == static_cast<int>(FirmwareUpdateState::downloading) ||
      update_state_ == static_cast<int>(FirmwareUpdateState::rebooting);
  if (!update_installing && update_overlay_ != nullptr &&
      lv_obj_is_valid(update_overlay_) &&
      !lv_obj_has_flag(update_overlay_, LV_OBJ_FLAG_HIDDEN)) {
    hide_update_overlay();
    overlay_rolled_back = true;
  }
  if (horizontal_transition_active_) {
    cancel_horizontal_transition_locked();
    overlay_rolled_back = true;
  }
  if (overlay_rolled_back) {
    ESP_LOGW(kLogTag,
             "Display draw failed; rolled back the pending full-screen overlay");
    lv_obj_t* screen = lv_screen_active();
    if (screen != nullptr) lv_obj_invalidate(screen);
    lv_obj_invalidate(lv_layer_top());
  }
}

void DisplayShell::set_capture_overlay_name(const char* screen_name) {
  capture_overlay_name_ = screen_name == nullptr ? "" : screen_name;
}

void DisplayShell::clear_capture_overlay_name(const char* expected_screen_name) {
  if (expected_screen_name == nullptr || capture_overlay_name_ == expected_screen_name) {
    capture_overlay_name_.clear();
  }
}

void DisplayShell::prepare_active_screen(const char* screen_name) {
  lv_async_call_cancel(printer_animation_source_async, this);
  printer_animation_source_pending_ = false;
  printer_animation_native_width_ = 0;
  printer_animation_native_height_ = 0;
  if (printer_animation_timer_ != nullptr) {
    lv_timer_delete(printer_animation_timer_);
    printer_animation_timer_ = nullptr;
  }
  capture_screen_name_ = screen_name == nullptr ? "" : screen_name;
  active_accent_label_ = nullptr;
  active_accent_text_objects_.clear();
  active_accent_bg_objects_.clear();
  version_label_ = nullptr;
  clock_status_label_ = nullptr;
  clock_hour_hand_ = nullptr;
  clock_minute_hand_ = nullptr;
  clock_second_hand_ = nullptr;
  clock_date_label_ = nullptr;
  digital_segments_ = {};
  digital_colons_ = {};
  material_cards_ = {};
  material_slot_labels_ = {};
  material_feed_labels_ = {};
  material_name_labels_ = {};
  material_percent_labels_ = {};
  nozzle_cards_ = {};
  nozzle_tool_labels_ = {};
  nozzle_icons_ = {};
  nozzle_temperature_labels_ = {};
  nozzle_target_labels_ = {};
  nozzle_material_labels_ = {};
  nozzle_material_dots_ = {};
  external_material_card_ = nullptr;
  external_material_dot_ = nullptr;
  external_material_label_ = nullptr;
  progress_arc_ = nullptr;
  progress_label_ = nullptr;
  header_audio_label_ = nullptr;
  header_power_label_ = nullptr;
  header_battery_outline_ = nullptr;
  header_battery_fill_ = nullptr;
  nozzle_scroll_ = nullptr;
  nozzle_row_ = nullptr;
  printer_list_scroll_ = nullptr;
  gesture_started_in_printer_list_ = false;
  printer_list_vertical_gesture_ = false;
  printer_list_scroll_started_ = false;
  pressed_printer_card_ = nullptr;
  printer_list_count_ = 0;
  printer_list_visible_count_ = 0;
  printer_list_item_pitch_ = 0;
  telemetry_metric_cards_ = {};
  telemetry_metric_caption_labels_ = {};
  telemetry_metric_value_labels_ = {};
  telemetry_detail_caption_labels_ = {};
  media_image_ = nullptr;
  printer_animation_root_ = nullptr;
  printer_animation_gesture_surface_ = nullptr;
  printer_animation_label_ = nullptr;
  printer_animation_canvas_ = nullptr;
  printer_animation_gif_ = nullptr;
  camera_spinner_ = nullptr;
  camera_activity_dot_ = nullptr;
  camera_activity_label_ = nullptr;
  camera_mode_row_ = nullptr;
  camera_snapshot_button_ = nullptr;
  camera_live_button_ = nullptr;
  chamber_light_bulb_ = nullptr;
  chamber_light_button_ = nullptr;
  chamber_light_button_label_ = nullptr;
  chamber_light_spinner_ = nullptr;
  prepare_screen(lv_screen_active(), theme_style_.background);
  printer_animation_gif_path_.clear();
  // The LVGL canvas does not own its external pixel buffer. Keep that buffer
  // alive until lv_obj_clean() above has synchronously destroyed the canvas.
  if (printer_animation_canvas_buffer_ != nullptr) {
    heap_caps_free(printer_animation_canvas_buffer_);
    printer_animation_canvas_buffer_ = nullptr;
  }
}

void DisplayShell::create_page_header(const char* title) {
  lv_obj_t* heading = lv_label_create(lv_screen_active());
  lv_label_set_text(heading, tr(title));
  apply_text_style(heading, lv_color_hex(accent_color_), &lv_font_montserrat_32);
  lv_obj_set_width(heading, 340);
  lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
  lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 42);
  active_accent_label_ = heading;
  active_accent_text_objects_.push_back(heading);
}

void DisplayShell::create_page_dots(int right_offset) {
  if constexpr (kDisplayUsesCompactRoundLayout) {
    right_offset = std::max(right_offset, 9);
  }
  lv_obj_t* column = lv_obj_create(lv_screen_active());
  lv_obj_set_size(column, 12, 76);
  lv_obj_align(column, LV_ALIGN_RIGHT_MID, -right_offset, 0);
  lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(column, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(column, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(column, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(column, 0, LV_PART_MAIN);
  const int active = std::clamp(page_.load(), 0, 4);
  for (int index = 0; index < 5; ++index) {
    lv_obj_t* dot = lv_obj_create(column);
    lv_obj_set_size(dot, index == active ? 9 : 7, index == active ? 9 : 7);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot,
                              lv_color_hex(index == active ? accent_color_ : theme_style_.track),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    if (index == active) active_accent_bg_objects_.push_back(dot);
  }
}

void DisplayShell::create_printer_view_dots(int right_offset) {
  if constexpr (kDisplayUsesCompactRoundLayout) {
    right_offset = std::max(right_offset, 9);
  }
  const int count = std::max(1, printer_subpage_count_.load());
  const int active = std::clamp(printer_subpage_.load(), 0, count - 1);
  if (count <= 1 || horizontal_depth_.load() != 1) return;
  lv_obj_t* row = lv_obj_create(lv_screen_active());
  lv_obj_set_size(row, 12, count * 18);
  lv_obj_align(row, LV_ALIGN_RIGHT_MID, -right_offset, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  for (int index = 0; index < count; ++index) {
    lv_obj_t* dot = lv_obj_create(row);
    lv_obj_set_size(dot, index == active ? 9 : 7, index == active ? 9 : 7);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot,
                              lv_color_hex(index == active ? accent_color_ : theme_style_.track),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    if (index == active) active_accent_bg_objects_.push_back(dot);
  }
}

void DisplayShell::create_depth_dots(int bottom_offset) {
  const int depth = horizontal_depth_.load();
  const int count = std::max(1, horizontal_depth_count_.load());
  if (depth <= 0 || count <= 1) return;
  lv_obj_t* row = lv_obj_create(lv_screen_active());
  lv_obj_set_size(row, count * 16, 12);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -bottom_offset);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  for (int index = 0; index < count; ++index) {
    const bool selected = index == std::clamp(depth, 0, count - 1);
    lv_obj_t* dot = lv_obj_create(row);
    lv_obj_set_size(dot, selected ? 8 : 6, selected ? 8 : 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(selected ? accent_color_ : theme_style_.track),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    if (selected) active_accent_bg_objects_.push_back(dot);
  }
}

bool DisplayShell::camera_page_active() const {
  const int camera_depth = selected_camera_depth_.load();
  return page_.load() == 0 && camera_depth > 0 &&
         horizontal_depth_.load() == camera_depth;
}

std::uint32_t DisplayShell::background_render_delay_ms() const {
  const std::int64_t remaining =
      background_render_quiet_until_us_.load(std::memory_order_acquire) -
      esp_timer_get_time();
  if (remaining <= 0) return 0;
  return static_cast<std::uint32_t>((remaining + 999) / 1000);
}

void DisplayShell::release_camera_frame() {
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ == 22 && media_image_ != nullptr && lv_obj_is_valid(media_image_)) {
    lv_image_set_src(media_image_, nullptr);
  }
  camera_pixels_.reset();
  camera_image_dsc_ = {};
  camera_was_refreshing_ = false;
  board_display_unlock();
}

void DisplayShell::release_printer_preview() {
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ == 3 && media_image_ != nullptr && lv_obj_is_valid(media_image_)) {
    lv_image_set_src(media_image_, nullptr);
    view_ = -1;
  }
  preview_encoded_.reset();
  preview_pixels_.reset();
  preview_image_dsc_ = {};
  board_display_unlock();
}

void DisplayShell::show_boot_status(const char* text) {
  if (status_label_ == nullptr || text == nullptr) return;
  if (board_display_lock(250) != ESP_OK) return;
  capture_screen_name_ = std::strncmp(text, "Connecting to Wi-Fi", 19) == 0
      ? "wifi-connecting" : "boot-status";
  capture_overlay_name_.clear();
  if (std::strncmp(text, "Connecting to Wi-Fi", 19) == 0) {
    const std::string localized = std::string(tr("Connecting to Wi-Fi")) + (text + 19);
    lv_label_set_text(status_label_, localized.c_str());
  } else {
    lv_label_set_text(status_label_, tr(text));
  }
  lv_obj_set_style_text_line_space(status_label_, std::strchr(text, '\n') != nullptr ? 5 : 0,
                                   LV_PART_MAIN);
  board_display_unlock();
}

void DisplayShell::show_wifi_error(const char* network_name) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_wifi_error(network_name);
    return;
  }
  if (view_ == 8 || board_display_lock(1000) != ESP_OK) return;
  lv_obj_t* screen = lv_screen_active();
  prepare_active_screen("wifi-error");

  lv_obj_t* heading = lv_label_create(screen);
  lv_label_set_text(heading, tr("WI-FI ERROR"));
  apply_text_style(heading, lv_color_hex(theme_colors_.error), &lv_font_montserrat_32);
  lv_obj_align(heading, LV_ALIGN_CENTER, 0, -88);

  lv_obj_t* detail = lv_label_create(screen);
  const char* target = network_name != nullptr && network_name[0] != '\0'
                           ? network_name
                           : tr("saved network");
  lv_label_set_text_fmt(detail, "%s:\n%s\n\n%s", tr("Could not connect to Wi-Fi"),
                        target, tr("Opening Wi-Fi Setup..."));
  apply_text_style(detail, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_16);
  lv_obj_set_width(detail, 350);
  lv_obj_align(detail, LV_ALIGN_CENTER, 0, 28);
  status_label_ = nullptr;
  view_ = 8;
  board_display_unlock();
}

void DisplayShell::show_shutdown_countdown(int seconds) {
  if (seconds < 1 || seconds > 3 || board_display_lock(500) != ESP_OK) return;
  set_capture_overlay_name("shutdown-countdown");
  if (shutdown_overlay_ == nullptr || !lv_obj_is_valid(shutdown_overlay_)) {
    shutdown_overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(shutdown_overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_center(shutdown_overlay_);
    lv_obj_remove_flag(shutdown_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(shutdown_overlay_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(shutdown_overlay_, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(shutdown_overlay_, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(shutdown_overlay_, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(shutdown_overlay_, LV_OPA_COVER, LV_PART_MAIN);
    shutdown_title_ = lv_label_create(shutdown_overlay_);
    apply_text_style(shutdown_title_, lv_color_hex(theme_style_.text_primary),
                     kDisplayUsesLargeLayout ? &lv_font_montserrat_32 : &lv_font_montserrat_24);
    if constexpr (!kDisplayUsesLargeLayout) {
      lv_obj_set_size(shutdown_title_, 220, 32);
      lv_obj_set_style_text_align(shutdown_title_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_label_set_long_mode(shutdown_title_, LV_LABEL_LONG_DOT);
    }
    lv_obj_align(shutdown_title_, LV_ALIGN_CENTER, 0, -35);
    shutdown_detail_ = lv_label_create(shutdown_overlay_);
    apply_text_style(shutdown_detail_, lv_color_hex(theme_style_.text_muted),
                     kDisplayUsesLargeLayout ? &lv_font_montserrat_16 : &lv_font_montserrat_14);
    if constexpr (!kDisplayUsesLargeLayout) {
      lv_obj_set_size(shutdown_detail_, 210, 42);
      lv_obj_set_style_text_align(shutdown_detail_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    lv_obj_align(shutdown_detail_, LV_ALIGN_CENTER, 0, 50);
  }
  lv_label_set_text_fmt(shutdown_title_, "%d", seconds);
  lv_label_set_text(shutdown_detail_, tr("Keep holding POWER"));
  lv_obj_remove_flag(shutdown_overlay_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(shutdown_overlay_);
  board_display_unlock();
}

void DisplayShell::cancel_shutdown_countdown() {
  if (board_display_lock(500) != ESP_OK) return;
  if (shutdown_overlay_ != nullptr && lv_obj_is_valid(shutdown_overlay_)) {
    lv_obj_add_flag(shutdown_overlay_, LV_OBJ_FLAG_HIDDEN);
  }
  clear_capture_overlay_name("shutdown-countdown");
  board_display_unlock();
}

void DisplayShell::show_shutdown_screen() {
  show_shutdown_countdown(1);
  if (board_display_lock(500) != ESP_OK) return;
  set_capture_overlay_name("power-off");
  if (shutdown_title_ != nullptr && lv_obj_is_valid(shutdown_title_)) {
    lv_label_set_text(shutdown_title_, tr("POWERING OFF"));
    lv_obj_set_style_text_font(
        shutdown_title_,
        localized_font(kDisplayUsesLargeLayout ? &lv_font_montserrat_32 : &lv_font_montserrat_24),
        LV_PART_MAIN);
  }
  if (shutdown_detail_ != nullptr && lv_obj_is_valid(shutdown_detail_)) {
    lv_label_set_text(shutdown_detail_, tr("Saving settings\nPlease wait"));
  }
  board_display_unlock();
}

void DisplayShell::show_wifi_setup(const char* network_name, const char* local_hostname) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_wifi_setup(network_name, local_hostname);
    return;
  }
  const std::string primary_host = local_hostname != nullptr && local_hostname[0] != '\0'
      ? local_hostname : "192.168.4.1";
  if (network_name == nullptr ||
      (view_ == 1 && visible_web_config_host_ == primary_host) ||
      board_display_lock(1000) != ESP_OK) return;
  lv_obj_t* screen = lv_screen_active();
  prepare_active_screen("wifi-setup");

  lv_obj_t* heading = lv_label_create(screen);
  lv_label_set_text(heading, tr("WI-FI SETUP"));
  apply_text_style(heading, lv_color_hex(accent_color_), &lv_font_montserrat_32);
  lv_obj_set_width(heading, 390);
  lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 25);
  active_accent_label_ = heading;
  active_accent_text_objects_.push_back(heading);

  const std::string wifi_payload = "WIFI:T:nopass;S:" + wifi_qr_escape(network_name) + ";;";
  const char* current_language_name = "English";
  for (const core::Language& language : core::kLanguages) {
    if (language.code == language_) {
      current_language_name = language.native_name.data();
      break;
    }
  }
  wifi_setup_pager_ = lv_tileview_create(screen);
  lv_obj_set_size(wifi_setup_pager_, 466, 342);
  lv_obj_align(wifi_setup_pager_, LV_ALIGN_TOP_MID, 0, 72);
  lv_obj_set_scrollbar_mode(wifi_setup_pager_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_radius(wifi_setup_pager_, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(wifi_setup_pager_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(wifi_setup_pager_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_pad_all(wifi_setup_pager_, 0, LV_PART_MAIN);
  lv_obj_remove_flag(wifi_setup_pager_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(wifi_setup_pager_, wifi_setup_pager_event, LV_EVENT_ALL, this);

  auto create_tile = [&](std::uint8_t column, lv_dir_t direction, std::uintptr_t step) {
    lv_obj_t* tile = lv_tileview_add_tile(wifi_setup_pager_, column, 0, direction);
    lv_obj_set_user_data(tile, reinterpret_cast<void*>(step));
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_radius(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    return tile;
  };
  auto add_label = [&](lv_obj_t* tile, const char* text, const lv_font_t* font,
                       lv_color_t color, int y, int width = 400) {
    lv_obj_t* label = lv_label_create(tile);
    lv_label_set_text(label, text);
    apply_text_style(label, color, font);
    lv_obj_set_width(label, width);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    make_gesture_passthrough(label);
    return label;
  };
  auto add_qr = [&](lv_obj_t* tile, const char* payload) {
    lv_obj_t* qr = lv_qrcode_create(tile);
    lv_qrcode_set_size(qr, 172);
    lv_qrcode_set_dark_color(qr, lv_color_hex(theme_style_.surface));
    lv_qrcode_set_light_color(qr, lv_color_hex(theme_style_.text_primary));
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_set_data(qr, payload);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 48);
    make_gesture_passthrough(qr);
  };
  auto add_language_button = [&](lv_obj_t* tile) {
    lv_obj_t* button = lv_button_create(tile);
    lv_obj_set_size(button, 112, 32);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 282);
    lv_obj_set_style_radius(button, themed_radius(16), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(accent_color_), LV_PART_MAIN);
    lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(button, wifi_setup_language_event, LV_EVENT_CLICKED, this);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text_fmt(label, "%s  >", current_language_name);
    apply_text_style(label, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_12);
    lv_obj_center(label);
  };

  lv_obj_t* join_tile = create_tile(0, LV_DIR_RIGHT, 1U);
  lv_obj_t* join_title = add_label(
      join_tile, tr("Connect to PrintDeck Wi-Fi"), &lv_font_montserrat_16,
      lv_color_hex(theme_style_.text_primary), 2);
  lv_obj_set_height(join_title, 40);
  lv_label_set_long_mode(join_title, LV_LABEL_LONG_WRAP);
  add_qr(join_tile, wifi_payload.c_str());
  const std::string network_label = std::string("Wi-Fi: ") + network_name;
  lv_obj_t* network = add_label(join_tile, network_label.c_str(), &lv_font_montserrat_16,
                                lv_color_hex(accent_color_), 226);
  active_accent_text_objects_.push_back(network);
  add_label(join_tile, tr("Setup opens automatically"), &lv_font_montserrat_12,
            lv_color_hex(theme_style_.text_muted), 254);
  add_language_button(join_tile);

  lv_obj_t* web_tile = create_tile(1, LV_DIR_LEFT, 2U);
  lv_obj_t* web_title = add_label(
      web_tile, tr("If setup does not open"), &lv_font_montserrat_16,
      lv_color_hex(theme_style_.text_primary), 2);
  lv_obj_set_height(web_title, 40);
  lv_label_set_long_mode(web_title, LV_LABEL_LONG_WRAP);
  const std::string web_url = std::string("http://") + primary_host + "/";
  add_qr(web_tile, web_url.c_str());
  lv_obj_t* address = add_label(web_tile, primary_host.c_str(), &lv_font_montserrat_16,
                                lv_color_hex(accent_color_), 226);
  active_accent_text_objects_.push_back(address);
  add_language_button(web_tile);

  create_wifi_setup_navigation(screen);
  lv_tileview_set_tile(wifi_setup_pager_, join_tile, LV_ANIM_OFF);
  status_label_ = nullptr;
  visible_web_config_host_ = primary_host;
  view_ = 1;
  board_display_unlock();
}

void DisplayShell::activate_printer_card(lv_obj_t* card) {
  if (card == nullptr || !lv_obj_is_valid(card)) return;
  note_activity(true);
  const auto id = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(card)));
  if (id == 0) return;
  if (id == selected_profile_) {
    if (selected_online_.load()) {
      start_horizontal_transition(0, false, -1, id, 1);
    }
  } else if (printer_selected_ != nullptr) {
    const bool accepted =
        printer_selected_(printer_selected_context_, id);
    if (!accepted) {
      const std::uint64_t now_ms =
          static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
      printer_retry_wait_profile_ = id;
      printer_retry_wait_until_ms_ = now_ms + 5000;
      const std::int32_t state_index = kDisplayUsesLargeLayout ? 4 : 2;
      lv_obj_t* state = lv_obj_get_child(card, state_index);
      if (state != nullptr) {
        lv_label_set_text(state, tr("WAIT"));
        lv_obj_set_style_text_color(state,
                                    lv_color_hex(theme_colors_.preparing),
                                    LV_PART_MAIN);
      }
      lv_timer_t* timer = lv_timer_create(printer_retry_wait_finished, 5000, this);
      if (timer != nullptr) lv_timer_set_repeat_count(timer, 1);
    }
  }
}

void DisplayShell::printer_retry_wait_finished(lv_timer_t* timer) {
  auto* shell = static_cast<DisplayShell*>(lv_timer_get_user_data(timer));
  if (shell == nullptr) return;
  const std::uint64_t now_ms =
      static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  if (now_ms < shell->printer_retry_wait_until_ms_) {
    const std::uint64_t remaining_ms = shell->printer_retry_wait_until_ms_ - now_ms;
    lv_timer_t* retry = lv_timer_create(
        printer_retry_wait_finished,
        static_cast<std::uint32_t>(std::max<std::uint64_t>(1, remaining_ms)), shell);
    if (retry != nullptr) lv_timer_set_repeat_count(retry, 1);
    return;
  }
  shell->printer_retry_wait_profile_ = 0;
  shell->printer_retry_wait_until_ms_ = 0;
  shell->view_ = -1;
  if (shell->page_refresh_requested_ != nullptr) {
    shell->page_refresh_requested_(shell->page_refresh_context_);
  }
}

void DisplayShell::show_my_printers(const char* ipv4, const char* local_hostname,
                                    const std::vector<core::PrinterProfile>& profiles,
                                    std::uint32_t selected_profile,
                                    const InactivePrinterSnapshot& inactive,
                                    const PowerSnapshot& power,
                                    const core::PrinterSnapshot* selected_snapshot) {
  if (capture_animation_override_active_) {
    core::PrinterProfile preview_profile;
    if (!profiles.empty()) preview_profile = profiles.front();
    if (preview_profile.id == 0) preview_profile.id = 1;
    if (preview_profile.display_name.empty()) preview_profile.display_name = "PrintDeck";
    core::PrinterSnapshot preview_snapshot;
    preview_snapshot.profile_id = preview_profile.id;
    preview_snapshot.link = core::LinkState::online;
    preview_snapshot.job.reachable = true;
    preview_snapshot.job.phase = core::JobPhase::printing;
    preview_snapshot.job.completion = 42.0F;
    show_printer_reactions(preview_profile, preview_snapshot, power);
    return;
  }
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_my_printers(ipv4, local_hostname, profiles, selected_profile, inactive, power,
                            selected_snapshot);
    return;
  }
  if (ipv4 == nullptr || board_display_lock(1000) != ESP_OK) return;
  const std::string primary_host = local_hostname != nullptr && local_hostname[0] != '\0'
      ? local_hostname : ipv4;
  const core::LinkState selected_link = selected_snapshot != nullptr
      ? selected_snapshot->link : core::LinkState::stopped;
  if (view_ != 2 || visible_inactive_revision_ != inactive.revision ||
      selected_profile_ != selected_profile || visible_selected_link_ != selected_link ||
      (profiles.empty() && visible_web_config_host_ != primary_host)) {
    lv_obj_t* screen = lv_screen_active();
    prepare_active_screen("my-printers");
    create_power_header(&power, -208);
    lv_obj_t* heading = lv_label_create(screen);
    lv_label_set_text(heading, tr("MY PRINTERS"));
    apply_text_style(heading, lv_color_hex(accent_color_), &lv_font_montserrat_32);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 44);
    active_accent_label_ = heading;

    lv_obj_t* list = lv_obj_create(screen);
    lv_obj_set_size(list, 360, 292);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 9, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    constexpr std::size_t kVisiblePrinterCards = 3;
    const bool list_overflow = profiles.size() > kVisiblePrinterCards;
    if (list_overflow) {
      lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_scroll_dir(list, LV_DIR_VER);
      lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_START);
      lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    } else {
      lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_scroll_dir(list, LV_DIR_NONE);
      lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    }
    if (profiles.empty()) {
      lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
      lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 44);
      lv_obj_set_style_text_color(heading, lv_color_hex(theme_style_.text_primary), LV_PART_MAIN);
      active_accent_label_ = nullptr;
      lv_obj_t* empty = lv_label_create(screen);
      lv_label_set_text(empty, tr("No printers added yet"));
      apply_text_style(empty, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_24);
      lv_obj_set_width(empty, 360);
      lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 89);
      lv_obj_remove_flag(empty, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t* instruction = lv_label_create(screen);
      lv_label_set_text(instruction, tr("Open Web Config to add your first printer"));
      apply_text_style(instruction, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_14);
      lv_obj_set_width(instruction, 390);
      lv_obj_align(instruction, LV_ALIGN_TOP_MID, 0, 126);
      lv_obj_remove_flag(instruction, LV_OBJ_FLAG_CLICKABLE);

      const std::string web_config_url = std::string("http://") + primary_host;
      lv_obj_t* qr = lv_qrcode_create(screen);
      lv_qrcode_set_size(qr, 154);
      lv_qrcode_set_dark_color(qr, lv_color_hex(theme_style_.background));
      lv_qrcode_set_light_color(qr, lv_color_hex(theme_style_.text_primary));
      lv_qrcode_set_quiet_zone(qr, true);
      lv_qrcode_set_data(qr, web_config_url.c_str());
      lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 155);
      lv_obj_remove_flag(qr, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t* caption = lv_label_create(screen);
      lv_label_set_text(caption, tr("WEB CONFIG URL"));
      apply_text_style(caption, lv_color_hex(accent_color_), &lv_font_montserrat_12);
      lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 327);
      active_accent_text_objects_.push_back(caption);
      lv_obj_remove_flag(caption, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t* url = lv_label_create(screen);
      lv_label_set_text(url, web_config_url.c_str());
      apply_text_style(url, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_14);
      lv_obj_set_width(url, 390);
      lv_label_set_long_mode(url, LV_LABEL_LONG_DOT);
      lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 353);
      lv_obj_remove_flag(url, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t* footer = lv_label_create(screen);
      lv_label_set_text(footer, tr("Use a device on the same Wi-Fi network"));
      apply_text_style(footer, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
      lv_obj_set_width(footer, 400);
      lv_obj_align(footer, LV_ALIGN_TOP_MID, 0, 392);
      lv_obj_remove_flag(footer, LV_OBJ_FLAG_CLICKABLE);
    }
    for (const auto& profile : profiles) {
      const auto inactive_status = std::find_if(
          inactive.printers.begin(), inactive.printers.end(),
          [&profile](const InactivePrinterStatus& status) {
            return status.profile_id == profile.id;
          });
      const bool has_status = inactive_status != inactive.printers.end() &&
                              inactive_status->available;
      const bool checking = has_status && inactive_status->checking;
      const bool is_selected = profile.id == selected_profile;
      const bool selected_online = is_selected && selected_snapshot != nullptr &&
                                   selected_snapshot->link == core::LinkState::online;
      const bool connected = is_selected ? selected_online
                                         : has_status && inactive_status->connected;
      const core::PrinterReachability reachability =
          connected ? core::PrinterReachability::online
                    : (has_status ? core::PrinterReachability::offline
                                  : core::PrinterReachability::unknown);
      const bool selectable = !checking &&
                              core::printer_selectable(is_selected, reachability);
      lv_obj_t* card = lv_obj_create(list);
      lv_obj_set_size(card, 330, 82);
      lv_obj_set_flex_grow(card, 0);
      lv_obj_set_style_pad_all(card, 10, 0);
      lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
      if (list_overflow) lv_obj_add_flag(card, LV_OBJ_FLAG_SNAPPABLE);
      lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
      lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
      lv_obj_set_user_data(card, reinterpret_cast<void*>(static_cast<std::uintptr_t>(profile.id)));
      if (selectable) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(theme_style_.surface_soft), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(card, lv_color_hex(accent_color_),
                                      LV_STATE_PRESSED);
        lv_obj_set_style_border_width(card, 2, LV_STATE_PRESSED);
      } else {
        lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
      }
      lv_obj_set_style_radius(card, themed_radius(16), 0);
      lv_obj_set_style_bg_color(card, lv_color_hex(theme_style_.surface_raised), 0);
      lv_obj_set_style_border_color(card, lv_color_hex(profile.id == selected_profile
                                                           ? accent_color_ : theme_style_.border), 0);
      lv_obj_set_style_border_width(card, profile.id == selected_profile ? 2 : 1, 0);
      apply_surface_effect(card);

      lv_obj_t* logo_slot = lv_obj_create(card);
      lv_obj_set_size(logo_slot, 50, 50);
      lv_obj_align(logo_slot, LV_ALIGN_LEFT_MID, 0, 0);
      lv_obj_set_style_bg_opa(logo_slot, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(logo_slot, 0, 0);
      lv_obj_set_style_pad_all(logo_slot, 0, 0);
      make_gesture_passthrough(logo_slot);
      if (const lv_image_dsc_t* logo = brand_logo_small(profile); logo != nullptr) {
        lv_obj_t* mark = lv_image_create(logo_slot);
        lv_image_set_src(mark, logo);
        lv_obj_set_style_image_recolor(
            mark, lv_color_hex(brand_logo_color(profile, theme_style_.surface_raised)), 0);
        lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
        lv_obj_center(mark);
        make_gesture_passthrough(mark);
      } else {
        lv_obj_t* mark = lv_label_create(logo_slot);
        lv_label_set_text(mark, brand_mark(profile));
        apply_text_style(mark, lv_color_hex(brand_color(profile)), &lv_font_montserrat_16);
        lv_obj_center(mark);
        make_gesture_passthrough(mark);
      }

      lv_obj_t* name = lv_label_create(card);
      lv_label_set_text(name, profile.display_name.c_str());
      lv_obj_set_size(name, theme_style_.terminal_typography ? 116 : 145, 20);
      lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
      apply_text_style(name, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_16);
      lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_align(name, LV_ALIGN_TOP_LEFT, 58, -4);
      make_gesture_passthrough(name);
      lv_obj_t* detail = lv_label_create(card);
      std::string detail_text;
      if (!profile.manufacturer.empty()) detail_text = profile.manufacturer;
      else detail_text = profile.protocol == core::PrinterProtocol::bambu_lan
                             ? "Bambu Lab" : "Klipper";
      if (!profile.model.empty()) {
        if (!detail_text.empty()) detail_text.push_back(' ');
        detail_text += profile.model;
      }
      lv_label_set_text(detail, detail_text.c_str());
      lv_obj_set_width(detail, 145);
      lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
      apply_text_style(detail, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_12);
      lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 58, 19);
      make_gesture_passthrough(detail);
      lv_obj_t* host = lv_label_create(card);
      const std::string host_text = endpoint_host(profile.endpoint);
      lv_label_set_text(host, host_text.c_str());
      lv_obj_set_width(host, 145);
      lv_label_set_long_mode(host, LV_LABEL_LONG_DOT);
      apply_text_style(host, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
      lv_obj_set_style_text_align(host, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_align(host, LV_ALIGN_BOTTOM_LEFT, 58, 4);
      make_gesture_passthrough(host);
      lv_obj_t* state = lv_label_create(card);
      const std::uint64_t now_ms =
          static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
      const bool retry_waiting = profile.id == printer_retry_wait_profile_ &&
                                 now_ms < printer_retry_wait_until_ms_;
      const char* state_text = "UNKNOWN";
      std::uint32_t state_color = theme_style_.text_muted;
      std::string selected_state;
      if (retry_waiting) {
        state_text = "WAIT";
        state_color = theme_colors_.preparing;
      } else if (is_selected) {
        if (selected_snapshot != nullptr &&
            selected_snapshot->link == core::LinkState::failed) {
          state_text = "OFFLINE";
          state_color = theme_colors_.offline;
        } else if (selected_snapshot == nullptr ||
                   selected_snapshot->link != core::LinkState::online) {
          state_text = "CONNECTING";
          state_color = theme_colors_.preparing;
        } else {
          selected_state = uppercase_ascii(core::job_status_label(selected_snapshot->job));
          state_text = selected_state.c_str();
          state_color = core::phase_color(theme_colors_, selected_snapshot->job.phase, true);
        }
      } else if (checking) {
        state_text = "CONNECTING";
        state_color = theme_colors_.preparing;
      } else if (has_status && !inactive_status->connected) {
        state_text = "OFFLINE";
        state_color = theme_colors_.offline;
      } else if (has_status) {
        if (inactive_status->kind == core::JobKind::calibration &&
            (inactive_status->phase == core::JobPhase::preparing ||
             inactive_status->phase == core::JobPhase::printing)) {
          state_text = "CALIBRATION";
        } else {
          switch (inactive_status->phase) {
            case core::JobPhase::printing: state_text = "PRINTING"; break;
            case core::JobPhase::paused: state_text = "PAUSED"; break;
            case core::JobPhase::completed: state_text = "DONE"; break;
            case core::JobPhase::failed: state_text = "ATTENTION"; break;
            default: state_text = "READY"; break;
          }
        }
        state_color = core::phase_color(theme_colors_, inactive_status->phase, true);
      }
      lv_label_set_text(state, tr(state_text));
      lv_obj_set_size(state, theme_style_.terminal_typography ? 124 : 98, 20);
      lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
      apply_text_style(state, lv_color_hex(state_color),
                       &lv_font_montserrat_12);
      lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_align(state, LV_ALIGN_TOP_RIGHT, -15, -1);
      make_gesture_passthrough(state);
      lv_obj_t* status_dot = lv_obj_create(card);
      lv_obj_set_size(status_dot, 9, 9);
      lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(status_dot,
                                lv_color_hex(connected ? theme_colors_.done
                                                       : theme_colors_.unknown), 0);
      lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(status_dot, 0, 0);
      lv_obj_set_style_pad_all(status_dot, 0, 0);
      lv_obj_align(status_dot, LV_ALIGN_TOP_RIGHT, 0, 2);
      make_gesture_passthrough(status_dot);
    }
    configure_printer_list_scroll(list, profiles.size(), kVisiblePrinterCards,
                                  82 + 9);
    create_page_dots();
    view_ = 2;
    visible_profile_ = 0;
    visible_inactive_revision_ = inactive.revision;
    visible_web_config_host_ = primary_host;
    visible_selected_link_ = selected_link;
  }
  selected_profile_ = selected_profile;
  selected_online_.store(core::dashboard_available(selected_profile, selected_snapshot));
  update_power_header(power);
  board_display_unlock();
}

void DisplayShell::return_to_printer_list() {
  const bool locked = board_display_lock(1000) == ESP_OK;
  if (locked && horizontal_transition_active_) {
    cancel_horizontal_transition_locked();
  }
  page_.store(0);
  horizontal_depth_.store(0);
  printer_subpage_.store(0);
  view_ = -1;
  if (locked) board_display_unlock();
  if (page_refresh_requested_ != nullptr) {
    page_refresh_requested_(page_refresh_context_);
  }
}

esp_err_t DisplayShell::navigate_for_capture(std::string_view screen_name) {
  if (screen_name.empty()) return ESP_ERR_INVALID_ARG;

  const bool overlay = screen_name == "quick-menu" || screen_name == "brightness" ||
                       screen_name == "theme" || screen_name == "audio" ||
                       screen_name == "language-picker";
  if (overlay) {
    if (board_display_lock(1000) != ESP_OK) return ESP_ERR_TIMEOUT;
    capture_animation_override_active_ = false;
    capture_animation_screen_name_.clear();
    show_quick_menu();
    if (screen_name == "brightness") show_brightness_overlay();
    else if (screen_name == "theme") show_theme_overlay();
    else if (screen_name == "audio") show_audio_overlay();
    else if (screen_name == "language-picker") show_wifi_setup_language_picker();
    board_display_unlock();
    reset_inactivity_and_wake();
    return ESP_OK;
  }

  int target_page = 0;
  int target_depth = 0;
  int target_subpage = 0;
  bool animation_preview = false;
  core::PrinterActivity preview_activity = core::PrinterActivity::unknown;
  if (screen_name == "my-printers") target_page = 0;
  else if (screen_name == "system-details") target_page = 1;
  else if (screen_name == "digital-clock") target_page = 2;
  else if (screen_name == "analog-clock") target_page = 3;
  else if (screen_name == "web-config") target_page = 4;
  else if (screen_name == "printer-reactions") {
    if (!selected_online_.load()) return ESP_ERR_INVALID_STATE;
    target_depth = 1;
    target_subpage = 0;
  }
  else if (screen_name == "printer-status" || screen_name == "nozzles" ||
           screen_name == "compact-details" || screen_name == "speeds") {
    if (!selected_online_.load()) return ESP_ERR_INVALID_STATE;
    target_depth = 1;
    const int reaction_offset = printer_animations_enabled_ ? 1 : 0;
    target_subpage = reaction_offset + (screen_name == "printer-status" ? 0
                     : screen_name == "nozzles" ? 1
                     : screen_name == "compact-details" ? 2 : 3);
  } else if (screen_name == "animation-standby" ||
             screen_name == "animation-preparing" ||
             screen_name == "animation-nozzle-heating" ||
             screen_name == "animation-bed-heating" ||
             screen_name == "animation-homing" ||
             screen_name == "animation-bed-leveling" ||
             screen_name == "animation-nozzle-cleaning" ||
             screen_name == "animation-calibrating" ||
             screen_name == "animation-filament-changing" ||
             screen_name == "animation-filament-unloading" ||
             screen_name == "animation-filament-loading" ||
             screen_name == "animation-filament-purging" ||
             screen_name == "animation-printing" ||
             screen_name == "animation-paused" ||
             screen_name == "animation-completed" ||
             screen_name == "animation-failed" ||
             screen_name == "animation-cancelled" ||
             screen_name == "animation-unavailable") {
    // Documentation previews must remain capturable while every configured
    // printer is powered down. This hidden USB path supplies a synthetic
    // dashboard shell and never makes an offline dashboard user-visible.
    animation_preview = true;
    target_depth = 1;
    target_subpage = 0;
    if (screen_name == "animation-standby") preview_activity = core::PrinterActivity::standby;
    else if (screen_name == "animation-preparing") preview_activity = core::PrinterActivity::preparing;
    else if (screen_name == "animation-nozzle-heating") preview_activity = core::PrinterActivity::nozzle_heating;
    else if (screen_name == "animation-bed-heating") preview_activity = core::PrinterActivity::bed_heating;
    else if (screen_name == "animation-homing") preview_activity = core::PrinterActivity::homing;
    else if (screen_name == "animation-bed-leveling") preview_activity = core::PrinterActivity::bed_leveling;
    else if (screen_name == "animation-nozzle-cleaning") preview_activity = core::PrinterActivity::nozzle_cleaning;
    else if (screen_name == "animation-calibrating") preview_activity = core::PrinterActivity::calibrating;
    else if (screen_name == "animation-filament-changing") preview_activity = core::PrinterActivity::filament_changing;
    else if (screen_name == "animation-filament-unloading") preview_activity = core::PrinterActivity::filament_unloading;
    else if (screen_name == "animation-filament-loading") preview_activity = core::PrinterActivity::filament_loading;
    else if (screen_name == "animation-filament-purging") preview_activity = core::PrinterActivity::filament_purging;
    else if (screen_name == "animation-printing") preview_activity = core::PrinterActivity::printing;
    else if (screen_name == "animation-paused") preview_activity = core::PrinterActivity::paused;
    else if (screen_name == "animation-completed") preview_activity = core::PrinterActivity::completed;
    else if (screen_name == "animation-failed") preview_activity = core::PrinterActivity::failed;
    else if (screen_name == "animation-cancelled") preview_activity = core::PrinterActivity::cancelled;
  } else if (screen_name == "ams-lite") {
    if (!selected_online_.load() || !selected_is_bambu_.load()) {
      return ESP_ERR_NOT_SUPPORTED;
    }
    target_depth = 2;
  } else if (screen_name == "local-camera") {
    target_depth = selected_camera_depth_.load();
    if (!selected_online_.load() || target_depth <= 0) return ESP_ERR_NOT_SUPPORTED;
  } else if (screen_name == "printer-light") {
    target_depth = selected_light_depth_.load();
    if (!selected_online_.load() || target_depth <= 0) return ESP_ERR_NOT_SUPPORTED;
  } else {
    return ESP_ERR_NOT_FOUND;
  }

  if (board_display_lock(1000) != ESP_OK) return ESP_ERR_TIMEOUT;
  if (horizontal_transition_active_) {
    if (screen_name != "my-printers") {
      board_display_unlock();
      return ESP_ERR_INVALID_STATE;
    }
    cancel_horizontal_transition_locked();
  }
  if (quick_overlay_ != nullptr && lv_obj_is_valid(quick_overlay_) &&
      !lv_obj_has_flag(quick_overlay_, LV_OBJ_FLAG_HIDDEN)) {
    close_quick_overlay();
  }
  capture_animation_override_active_ = animation_preview;
  capture_animation_override_ = preview_activity;
  capture_animation_screen_name_ = animation_preview ? std::string(screen_name) : std::string{};
  page_.store(target_page);
  horizontal_depth_.store(target_depth);
  printer_subpage_.store(target_subpage);
  view_ = -1;
  board_display_unlock();
  reset_inactivity_and_wake();
  if (page_refresh_requested_ != nullptr) {
    page_refresh_requested_(page_refresh_context_);
  }
  return ESP_OK;
}

void DisplayShell::open_printer_when_ready(std::uint32_t profile_id) {
  if (profile_id == 0 || horizontal_depth_.load() != 0 ||
      !selected_online_.load() || board_display_lock(250) != ESP_OK) return;
  selected_profile_ = profile_id;
  start_horizontal_transition(0, false, -1, profile_id, 1);
  board_display_unlock();
}

void DisplayShell::show_printer(const core::PrinterProfile& profile,
                                const core::PrinterSnapshot& snapshot,
                                const PowerSnapshot& power, const char* ipv4) {
  selected_profile_ = profile.id;
  selected_online_.store(snapshot.profile_id == profile.id &&
                         snapshot.link == core::LinkState::online);
  const bool is_bambu = core::printer_supports(
      profile.protocol, core::PrinterCapability::material_system);
  // Camera discovery is deliberately lazy.  Keep the page reachable for both
  // supported printer protocols so opening it can start the bounded probe.
  const bool has_camera = profile.protocol == core::PrinterProtocol::moonraker ||
                          profile.protocol == core::PrinterProtocol::bambu_lan;
  const bool has_light = is_bambu || snapshot.job.chamber_light_supported;
  const int camera_depth = has_camera ? (is_bambu ? 3 : 2) : 0;
  const int light_depth = has_light ? 2 + (is_bambu ? 1 : 0) + (has_camera ? 1 : 0) : 0;
  selected_is_bambu_.store(is_bambu);
  selected_camera_depth_.store(camera_depth);
  selected_light_depth_.store(light_depth);
  horizontal_depth_count_.store(2 + (is_bambu ? 1 : 0) + (has_camera ? 1 : 0) +
                                (has_light ? 1 : 0));
  const bool reactions_visible =
      printer_animations_enabled_ || capture_animation_override_active_;
  const int reaction_offset = reactions_visible ? 1 : 0;
  const int subpage_count = 4 + reaction_offset;
  printer_subpage_count_.store(subpage_count);
  if (horizontal_depth_.load() >= horizontal_depth_count_.load()) {
    horizontal_depth_.store(horizontal_depth_count_.load() - 1);
  }
  const int depth = horizontal_depth_.load();
  if (is_bambu && depth == 2) {
    show_printer_materials(profile, snapshot);
    return;
  }
  if (camera_depth > 0 && depth == camera_depth) {
    show_printer_camera(profile, snapshot, power);
    return;
  }
  if (light_depth > 0 && depth == light_depth) {
    show_printer_light(profile, snapshot);
    return;
  }
  const int subpage = std::clamp(printer_subpage_.load(), 0, subpage_count - 1);
  printer_subpage_.store(subpage);
  if (reactions_visible && subpage == 0) {
    show_printer_reactions(profile, snapshot, power);
    return;
  }
  const int content_subpage = subpage - reaction_offset;
  if (content_subpage == 1) {
    show_printer_nozzles(profile, snapshot, power);
    return;
  }
  if (content_subpage == 2) {
    show_printer_compact(profile, snapshot, power);
    return;
  }
  if (content_subpage == 3) {
    show_printer_telemetry(profile, snapshot, power);
    return;
  }
  show_printer_status(profile, snapshot, power, ipv4);
}

void DisplayShell::create_printer_chrome(const core::PrinterProfile& profile,
                                         const core::PrinterSnapshot& snapshot,
                                         const PowerSnapshot* power) {
  lv_obj_t* screen = lv_screen_active();
  progress_arc_ = lv_arc_create(screen);
  lv_obj_set_size(progress_arc_, 466, 466);
  lv_obj_center(progress_arc_);
  lv_arc_set_range(progress_arc_, 0, 100);
  lv_arc_set_rotation(progress_arc_, 270);
  lv_arc_set_bg_angles(progress_arc_, 13, 347);
  lv_arc_set_value(progress_arc_, 0);
  lv_obj_remove_flag(progress_arc_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(progress_arc_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(progress_arc_, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(progress_arc_, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_style_arc_width(progress_arc_, 22, LV_PART_MAIN);
  lv_obj_set_style_arc_color(progress_arc_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
  lv_obj_set_style_arc_width(progress_arc_, 22, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(progress_arc_, lv_color_hex(accent_color_), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(progress_arc_, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(progress_arc_, true, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(progress_arc_, LV_OPA_TRANSP, LV_PART_KNOB);

  progress_label_ = lv_label_create(screen);
  lv_label_set_text(progress_label_, "0%");
  apply_text_style(progress_label_, lv_color_hex(accent_color_), &lv_font_montserrat_24);
  lv_obj_set_width(progress_label_, 100);
  lv_obj_align(progress_label_, LV_ALIGN_TOP_MID, 0, kPrinterProgressTopOffsetPx);
  update_printer_progress(snapshot);

  create_power_header(power, -177);

  title_label_ = lv_label_create(screen);
  lv_label_set_text(title_label_, profile.display_name.c_str());
  apply_text_style(title_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_16);
  // Keep a deliberate margin inside the round progress ring. Truncate roughly
  // one glyph earlier so the first/last visible characters stay off the arc.
  lv_obj_set_size(title_label_, 288, 22);
  lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(title_label_, LV_LABEL_LONG_DOT);
  lv_obj_align(title_label_, LV_ALIGN_CENTER, 0, -145);
  create_printer_view_dots();
  // Keep the horizontal depth indicator low in the round display so it stays
  // clear of the status text while remaining inside the progress ring.
  create_depth_dots();
}

void DisplayShell::update_printer_progress(const core::PrinterSnapshot& snapshot) {
  if (progress_arc_ == nullptr || progress_label_ == nullptr) return;
  const int progress = std::clamp(static_cast<int>(snapshot.job.completion), 0, 100);
  const bool round_reaction_progress =
      kDisplayUsesCompactRoundLayout && view_ == 24;
  if (kDisplayUsesLargeLayout || round_reaction_progress) {
    lv_obj_set_style_arc_color(progress_arc_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_arc_color(progress_arc_, lv_color_hex(theme_colors_.printing),
                               LV_PART_INDICATOR);
    lv_obj_set_style_text_color(progress_label_, lv_color_hex(theme_colors_.printing),
                                LV_PART_MAIN);
    lv_arc_set_value(progress_arc_, progress);
  } else {
    lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_colors_.printing),
                              LV_PART_INDICATOR);
    lv_obj_set_style_text_color(progress_label_, lv_color_hex(theme_colors_.printing),
                                LV_PART_MAIN);
    lv_bar_set_value(progress_arc_, progress, LV_ANIM_OFF);
  }
  lv_label_set_text_fmt(progress_label_, "%d%%", progress);
}

void DisplayShell::create_printer_animation(lv_obj_t* parent) {
  if ((!printer_animations_enabled_ && !capture_animation_override_active_) ||
      parent == nullptr) return;
  // The square LCD keeps compact typography, but its reaction assets are
  // native 240 x 240 full frames. Give every hardware family the complete
  // display canvas and keep the percentage chrome as a foreground overlay.
  printer_animation_compact_ =
      !kDisplayUsesLargeLayout && !kDisplayUsesCompactRoundLayout;
  printer_animation_frame_ = 0;
  printer_animation_activity_ = core::PrinterActivity::unknown;
  printer_animation_primary_color_ = theme_colors_.idle;
  printer_animation_filament_color_ = theme_colors_.filament;

  printer_animation_root_ = lv_obj_create(parent);
  // The percentage is an overlay, not reserved layout space. Let a native
  // screen-sized GIF use the whole display while keeping smaller uploads at
  // their original pixel size instead of stretching them.
  lv_obj_set_size(printer_animation_root_, kDisplayWidth, kDisplayHeight);
  lv_obj_align(printer_animation_root_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(printer_animation_root_,
                            lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(printer_animation_root_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(printer_animation_root_, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(printer_animation_root_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(printer_animation_root_, 0, LV_PART_MAIN);
  // The animation occupies most of the display, so make its root an explicit
  // gesture surface. Relying only on event bubbling from the GIF decoder can
  // lose PRESSING samples while a frame is being replaced, which makes the
  // swipe back to My Printers appear unresponsive.
  lv_obj_add_flag(printer_animation_root_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(printer_animation_root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(printer_animation_root_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(printer_animation_root_, screen_event, LV_EVENT_PRESSED, this);
  lv_obj_add_event_cb(printer_animation_root_, screen_event, LV_EVENT_PRESSING, this);
  lv_obj_add_event_cb(printer_animation_root_, screen_event, LV_EVENT_RELEASED, this);
  lv_obj_add_event_cb(printer_animation_root_, screen_event, LV_EVENT_PRESS_LOST, this);
  lv_obj_add_event_cb(printer_animation_root_, screen_event, LV_EVENT_LONG_PRESSED, this);

  printer_animation_label_ = lv_label_create(printer_animation_root_);
  apply_text_style(printer_animation_label_, lv_color_hex(theme_colors_.idle),
                   printer_animation_compact_ ? &lv_font_montserrat_14
                                              : &lv_font_montserrat_16);
  lv_obj_set_width(printer_animation_label_, printer_animation_compact_ ? 210 : 330);
  lv_obj_set_style_text_align(printer_animation_label_, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_align(printer_animation_label_, LV_ALIGN_TOP_MID, 0,
               printer_animation_compact_ ? 5 : 12);
  make_gesture_passthrough(printer_animation_label_);

  printer_animation_gif_ = lv_gif_create(printer_animation_root_);
  // Web Config flattens custom uploads into opaque full frames. RGB565 keeps
  // the decoder buffer and display bandwidth bounded without relying on
  // transparent delta-frame state inside the embedded GIF decoder.
  lv_gif_set_color_format(printer_animation_gif_, LV_COLOR_FORMAT_RGB565);
  lv_obj_add_flag(printer_animation_gif_, LV_OBJ_FLAG_HIDDEN);
  make_gesture_passthrough(printer_animation_gif_);

  // A 60 ms cadence is smooth enough for directional motion while leaving
  // the LVGL task ample time for touch and the rest of the dashboard.
  printer_animation_timer_ = lv_timer_create(printer_animation_tick, 60, this);
  render_printer_animation_frame();
}

bool DisplayShell::ensure_printer_animation_canvas() {
  if (printer_animation_canvas_ != nullptr &&
      lv_obj_is_valid(printer_animation_canvas_)) return true;
  if (printer_animation_root_ == nullptr ||
      !lv_obj_is_valid(printer_animation_root_)) return false;
  const int canvas_width = printer_animation_compact_
                               ? kDisplayWidth
                               : kDisplayUsesCompactRoundLayout ? 200 : 350;
  const int canvas_height = printer_animation_compact_
                                ? kDisplayHeight
                                : kDisplayUsesCompactRoundLayout ? 200 : 350;
  const std::size_t buffer_size = static_cast<std::size_t>(canvas_width) *
                                  static_cast<std::size_t>(canvas_height) * 2U;
  printer_animation_canvas_buffer_ = heap_caps_malloc(
      buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (printer_animation_canvas_buffer_ == nullptr) {
    ESP_LOGW(kLogTag, "Printer animation canvas allocation failed (%u bytes)",
             static_cast<unsigned>(buffer_size));
    return false;
  }
  printer_animation_canvas_ = lv_canvas_create(printer_animation_root_);
  if (printer_animation_canvas_ == nullptr) {
    heap_caps_free(printer_animation_canvas_buffer_);
    printer_animation_canvas_buffer_ = nullptr;
    return false;
  }
  lv_canvas_set_buffer(printer_animation_canvas_, printer_animation_canvas_buffer_,
                       canvas_width, canvas_height, LV_COLOR_FORMAT_RGB565);
  lv_obj_align(printer_animation_canvas_, LV_ALIGN_CENTER, 0,
               printer_animation_compact_ ? 8 : 0);
  make_gesture_passthrough(printer_animation_canvas_);
  return true;
}

void DisplayShell::release_printer_animation_canvas() {
  if (printer_animation_canvas_ != nullptr &&
      lv_obj_is_valid(printer_animation_canvas_)) {
    lv_obj_delete(printer_animation_canvas_);
  }
  printer_animation_canvas_ = nullptr;
  if (printer_animation_canvas_buffer_ != nullptr) {
    heap_caps_free(printer_animation_canvas_buffer_);
    printer_animation_canvas_buffer_ = nullptr;
  }
}

void DisplayShell::show_printer_reactions(const core::PrinterProfile& profile,
                                          const core::PrinterSnapshot& snapshot,
                                          const PowerSnapshot& power) {
  (void)power;
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 24 || visible_profile_ != profile.id) {
    prepare_active_screen(capture_animation_override_active_
                              ? capture_animation_screen_name_.c_str()
                              : "printer-reactions");
    // Reaction GIFs are flattened onto black so transparent uploads and the
    // built-in sets share one predictable AMOLED canvas without a visible
    // square around the artwork.
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), LV_PART_MAIN);
    if constexpr (kDisplayUsesLargeLayout || kDisplayUsesCompactRoundLayout) {
      progress_arc_ = lv_arc_create(lv_screen_active());
      const int arc_size = kDisplayUsesLargeLayout ? kDisplayWidth : 232;
      const int arc_width = kDisplayUsesLargeLayout ? 22 : 8;
      lv_obj_set_size(progress_arc_, arc_size, arc_size);
      lv_obj_center(progress_arc_);
      lv_arc_set_range(progress_arc_, 0, 100);
      lv_arc_set_rotation(progress_arc_, 270);
      lv_arc_set_bg_angles(progress_arc_, 13, 347);
      lv_arc_set_value(progress_arc_, 0);
      lv_obj_set_style_arc_width(progress_arc_, arc_width, LV_PART_MAIN);
      lv_obj_set_style_arc_color(progress_arc_, lv_color_hex(theme_style_.track),
                                 LV_PART_MAIN);
      lv_obj_set_style_arc_width(progress_arc_, arc_width, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(progress_arc_, lv_color_hex(accent_color_),
                                 LV_PART_INDICATOR);
      lv_obj_set_style_arc_rounded(progress_arc_, true, LV_PART_MAIN);
      lv_obj_set_style_arc_rounded(progress_arc_, true, LV_PART_INDICATOR);
      lv_obj_set_style_bg_opa(progress_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
    } else {
      progress_arc_ = lv_bar_create(lv_screen_active());
      lv_obj_set_size(progress_arc_, 190, 6);
      lv_obj_align(progress_arc_, LV_ALIGN_TOP_RIGHT, -6, 11);
      lv_bar_set_range(progress_arc_, 0, 100);
      lv_bar_set_value(progress_arc_, 0, LV_ANIM_OFF);
      lv_obj_set_style_radius(progress_arc_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(theme_style_.track),
                                 LV_PART_MAIN);
      lv_obj_set_style_bg_color(progress_arc_, lv_color_hex(accent_color_),
                                 LV_PART_INDICATOR);
    }
    lv_obj_remove_flag(progress_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(progress_arc_, LV_OBJ_FLAG_SCROLLABLE);
    progress_label_ = lv_label_create(lv_screen_active());
    lv_label_set_text(progress_label_, "0%");
    apply_text_style(progress_label_, lv_color_hex(accent_color_),
                     kDisplayUsesLargeLayout ? &lv_font_montserrat_24
                                     : &lv_font_montserrat_12);
    lv_obj_set_width(progress_label_, kDisplayUsesLargeLayout ? 100
                                                             : kDisplayUsesCompactRoundLayout
                                                                   ? 44 : 38);
    lv_obj_set_style_text_align(progress_label_, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    if constexpr (!kDisplayUsesLargeLayout && !kDisplayUsesCompactRoundLayout) {
      lv_obj_align(progress_label_, LV_ALIGN_TOP_LEFT, 2, 7);
    } else {
      lv_obj_align(progress_label_, LV_ALIGN_TOP_MID, 0,
                   kDisplayUsesLargeLayout ? kPrinterProgressTopOffsetPx : 7);
    }
    create_printer_animation(lv_screen_active());
    // The dedicated reactions page is intentionally visual-only: the progress
    // percentage stays above the animation, while the activity is conveyed by
    // the animation itself without a second textual status label.
    if (printer_animation_label_ != nullptr) {
      lv_obj_add_flag(printer_animation_label_, LV_OBJ_FLAG_HIDDEN);
    }
    // Keep the progress chrome above opaque custom GIFs.
    if (progress_arc_ != nullptr) lv_obj_move_foreground(progress_arc_);
    if (progress_label_ != nullptr) lv_obj_move_foreground(progress_label_);

    // GIF decoder objects can replace their internal image while a finger is
    // moving. Route reactions-page gestures through one stable, transparent
    // full-screen target above both the animation and progress chrome.
    printer_animation_gesture_surface_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(printer_animation_gesture_surface_, LV_PCT(100), LV_PCT(100));
    lv_obj_center(printer_animation_gesture_surface_);
    lv_obj_set_style_bg_opa(printer_animation_gesture_surface_, LV_OPA_TRANSP,
                            LV_PART_MAIN);
    lv_obj_set_style_border_width(printer_animation_gesture_surface_, 0,
                                  LV_PART_MAIN);
    lv_obj_set_style_pad_all(printer_animation_gesture_surface_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(printer_animation_gesture_surface_, 0, LV_PART_MAIN);
    lv_obj_add_flag(printer_animation_gesture_surface_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(printer_animation_gesture_surface_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(printer_animation_gesture_surface_,
                              LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(printer_animation_gesture_surface_, screen_event,
                        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(printer_animation_gesture_surface_, screen_event,
                        LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(printer_animation_gesture_surface_, screen_event,
                        LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(printer_animation_gesture_surface_, screen_event,
                        LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(printer_animation_gesture_surface_, screen_event,
                        LV_EVENT_LONG_PRESSED, this);
    view_ = 24;
    visible_profile_ = profile.id;
    apply_reaction_progress_visibility();
  }
  update_printer_progress(snapshot);
  update_printer_animation(snapshot.job);
  board_display_unlock();
}

void DisplayShell::printer_animation_tick(lv_timer_t* timer) {
  auto* shell = static_cast<DisplayShell*>(lv_timer_get_user_data(timer));
  if (shell == nullptr || shell->printer_animation_root_ == nullptr ||
      !lv_obj_is_valid(shell->printer_animation_root_)) {
    return;
  }
  shell->printer_animation_frame_ = (shell->printer_animation_frame_ + 1U) % 240U;
  shell->render_printer_animation_frame();
}

void DisplayShell::update_printer_animation(const core::JobState& job) {
  if (printer_animation_root_ == nullptr ||
      !lv_obj_is_valid(printer_animation_root_)) return;
  const core::PrinterActivity next = capture_animation_override_active_
                                         ? capture_animation_override_
                                         : core::effective_printer_activity(job);
  if (next != printer_animation_activity_) {
    printer_animation_activity_ = next;
    printer_animation_frame_ = 0;
    printer_animation_asset_generation_ = 0xffffffffU;
  }

  printer_animation_primary_color_ = theme_colors_.idle;
  switch (next) {
    case core::PrinterActivity::preparing:
    case core::PrinterActivity::nozzle_heating:
    case core::PrinterActivity::bed_heating:
    case core::PrinterActivity::homing:
    case core::PrinterActivity::bed_leveling:
    case core::PrinterActivity::nozzle_cleaning:
    case core::PrinterActivity::calibrating:
      printer_animation_primary_color_ = theme_colors_.preparing;
      break;
    case core::PrinterActivity::filament_changing:
    case core::PrinterActivity::filament_unloading:
    case core::PrinterActivity::filament_loading:
    case core::PrinterActivity::filament_purging:
      printer_animation_primary_color_ = theme_colors_.filament;
      break;
    case core::PrinterActivity::printing:
      printer_animation_primary_color_ = theme_colors_.printing;
      break;
    case core::PrinterActivity::paused:
    case core::PrinterActivity::cancelled:
      printer_animation_primary_color_ = theme_colors_.paused;
      break;
    case core::PrinterActivity::completed:
      printer_animation_primary_color_ = theme_colors_.done;
      break;
    case core::PrinterActivity::failed:
      printer_animation_primary_color_ = theme_colors_.error;
      break;
    case core::PrinterActivity::standby:
    case core::PrinterActivity::unknown:
      break;
  }

  printer_animation_filament_color_ = theme_colors_.filament;
  const auto use_slot_color = [this](const core::MaterialSlot& slot) {
    if (slot.feeding && slot.rgba != 0) {
      printer_animation_filament_color_ = (slot.rgba >> 8U) & 0x00FFFFFFU;
      return true;
    }
    return false;
  };
  bool found_color = false;
  for (const auto& slot : job.materials.slots) {
    if (use_slot_color(slot)) {
      found_color = true;
      break;
    }
  }
  if (!found_color) use_slot_color(job.materials.external_spool);

  lv_label_set_text(printer_animation_label_,
                    tr(core::printer_activity_label(printer_animation_activity_)));
  render_printer_animation_frame();
}

bool DisplayShell::update_printer_animation_source() {
  if (printer_animation_gif_ == nullptr || !lv_obj_is_valid(printer_animation_gif_) ||
      reaction_assets_ == nullptr) return false;
  const auto& event = core::reaction_event(printer_animation_activity_);
  if (!reaction_assets_->event_enabled(event.id)) {
    printer_animation_asset_generation_ = reaction_assets_->generation();
    if (!printer_animation_gif_path_.empty()) {
      printer_animation_gif_path_.clear();
      if (!printer_animation_source_pending_) {
        printer_animation_source_pending_ = true;
        if (lv_async_call(printer_animation_source_async, this) != LV_RESULT_OK) {
          printer_animation_source_pending_ = false;
        }
      }
    }
    lv_obj_add_flag(printer_animation_gif_, LV_OBJ_FLAG_HIDDEN);
    release_printer_animation_canvas();
    if (printer_animation_timer_ != nullptr) lv_timer_pause(printer_animation_timer_);
    return true;
  }
  const std::uint32_t generation = reaction_assets_->generation();
  const bool unchanged = generation == printer_animation_asset_generation_ &&
                         !printer_animation_gif_path_.empty();
  if (!unchanged) {
    printer_animation_asset_generation_ = generation;
    const std::string path = reaction_assets_->effective_lvgl_path(
        printer_animation_activity_);
    printer_animation_gif_path_ = path;
    printer_animation_native_width_ = 0;
    printer_animation_native_height_ = 0;
    // A replacement may keep the same filesystem path, so a generation
    // change must reopen the source even when the path string is unchanged.
    // Opening a LittleFS-backed GIF may disable the flash cache. The monitor
    // worker intentionally uses a PSRAM stack, so defer the open to LVGL's
    // core-1 task, whose stack is internal RAM. The decoder's later reads
    // then remain on that same safe task as well.
    if (!printer_animation_gif_path_.empty() &&
        !printer_animation_source_pending_) {
      printer_animation_source_pending_ = true;
      if (lv_async_call(printer_animation_source_async, this) != LV_RESULT_OK) {
        printer_animation_source_pending_ = false;
      }
    }
  }
  if (printer_animation_gif_path_.empty() ||
      !lv_gif_is_loaded(printer_animation_gif_)) {
    lv_obj_add_flag(printer_animation_gif_, LV_OBJ_FLAG_HIDDEN);
    // A valid set asset is opened asynchronously on LVGL's internal-RAM
    // stack. Keep the viewport blank for that very short interval instead of
    // drawing the differently-sized procedural fallback and visibly shrinking
    // when the GIF becomes ready.
    if (!printer_animation_gif_path_.empty() &&
        printer_animation_source_pending_) {
      if (printer_animation_canvas_ != nullptr) {
        lv_obj_add_flag(printer_animation_canvas_, LV_OBJ_FLAG_HIDDEN);
      }
      if (printer_animation_timer_ != nullptr) {
        lv_timer_pause(printer_animation_timer_);
      }
      return true;
    }
    if (printer_animation_canvas_ != nullptr) {
      lv_obj_remove_flag(printer_animation_canvas_, LV_OBJ_FLAG_HIDDEN);
    }
    if (printer_animation_timer_ != nullptr) lv_timer_resume(printer_animation_timer_);
    return false;
  }
  const int width = printer_animation_native_width_ > 0
                        ? printer_animation_native_width_
                        : std::max<int>(1, lv_obj_get_width(printer_animation_gif_));
  const int height = printer_animation_native_height_ > 0
                         ? printer_animation_native_height_
                         : std::max<int>(1, lv_obj_get_height(printer_animation_gif_));
  const int available_width = kDisplayWidth;
  const int available_height = kDisplayHeight;
  const std::uint32_t scale = static_cast<std::uint32_t>(std::min(
      static_cast<int>(LV_SCALE_NONE),
      std::min(available_width * static_cast<int>(LV_SCALE_NONE) / width,
               available_height * static_cast<int>(LV_SCALE_NONE) / height)));
  lv_image_set_scale(printer_animation_gif_, scale);
  lv_obj_align(printer_animation_gif_, LV_ALIGN_CENTER, 0,
               printer_animation_compact_ ? 8 : 0);
  lv_obj_remove_flag(printer_animation_gif_, LV_OBJ_FLAG_HIDDEN);
  release_printer_animation_canvas();
  if (printer_animation_timer_ != nullptr) lv_timer_pause(printer_animation_timer_);
  return true;
}

void DisplayShell::printer_animation_source_async(void* context) {
  auto* shell = static_cast<DisplayShell*>(context);
  if (shell == nullptr) return;
  shell->printer_animation_source_pending_ = false;
  if (shell->printer_animation_gif_ == nullptr ||
      !lv_obj_is_valid(shell->printer_animation_gif_)) return;
  lv_gif_set_src(shell->printer_animation_gif_,
                 shell->printer_animation_gif_path_.empty()
                     ? nullptr : shell->printer_animation_gif_path_.c_str());
  // lv_gif_set_src() has already opened the decoder and assigned its native
  // frame buffer to the image object. Cache that layout size once, before any
  // scale is applied, instead of opening the LittleFS file a second time.
  if (lv_gif_is_loaded(shell->printer_animation_gif_)) {
    shell->printer_animation_native_width_ =
        std::max<int>(1, lv_obj_get_width(shell->printer_animation_gif_));
    shell->printer_animation_native_height_ =
        std::max<int>(1, lv_obj_get_height(shell->printer_animation_gif_));
  } else {
    shell->printer_animation_native_width_ = 0;
    shell->printer_animation_native_height_ = 0;
  }
  shell->render_printer_animation_frame();
}

void DisplayShell::render_printer_animation_frame() {
  if (update_printer_animation_source()) return;
  if (!ensure_printer_animation_canvas()) return;

  lv_obj_set_style_text_color(printer_animation_label_,
                              lv_color_hex(printer_animation_primary_color_),
                              LV_PART_MAIN);
  const PrinterAnimationPalette palette{
      .background = theme_style_.background,
      .primary = printer_animation_primary_color_,
      .secondary = theme_style_.accent_secondary,
      .muted = theme_style_.text_muted,
      .filament = printer_animation_filament_color_,
  };
  render_printer_animation(printer_animation_canvas_,
                           printer_animation_activity_,
                           printer_animation_frame_, palette);
}

void DisplayShell::create_power_header(const PowerSnapshot* power, int center_y) {
  lv_obj_t* screen = lv_screen_active();
  lv_obj_t* audio = lv_label_create(screen);
  lv_label_set_text(audio, audio_enabled_ ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
  apply_icon_text_style(audio, lv_color_hex(theme_style_.text_primary),
                        &lv_font_montserrat_16);
  lv_obj_align(audio, LV_ALIGN_CENTER, -39, center_y);
  header_power_label_ = lv_label_create(screen);
  apply_text_style(header_power_label_, lv_color_hex(theme_style_.text_primary),
                   &lv_font_montserrat_16);
  // Anchor the right edge while allowing the visible power text to determine
  // its own width. This removes the empty lightning-sized gap on battery power.
  lv_obj_set_width(header_power_label_, LV_SIZE_CONTENT);
  lv_obj_align(header_power_label_, LV_ALIGN_RIGHT_MID, -164, center_y);
  header_battery_outline_ = nullptr;
  header_battery_fill_ = nullptr;
  if (power != nullptr && power->available && power->battery_present) {
    lv_obj_t* battery = transparent_icon_root(screen, 16, 22, -5, center_y);
    header_battery_outline_ = lv_obj_create(battery);
    lv_obj_set_size(header_battery_outline_, 10, 16);
    lv_obj_align(header_battery_outline_, LV_ALIGN_CENTER, 0, 1);
    lv_obj_set_style_radius(header_battery_outline_, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header_battery_outline_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(header_battery_outline_,
                                  lv_color_hex(theme_style_.text_primary),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(header_battery_outline_, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header_battery_outline_, 2, LV_PART_MAIN);
    make_gesture_passthrough(header_battery_outline_);
    icon_shape(battery, 5, 3, 0, -8, theme_style_.text_primary, 1);
    const int fill_height = std::max(2, 10 * static_cast<int>(power->battery_percent) / 100);
    header_battery_fill_ = icon_shape(header_battery_outline_, 4, fill_height, 0,
                                      4 - fill_height / 2, theme_style_.text_primary, 1);
    update_power_header(*power);
  } else {
    lv_label_set_text(header_power_label_, LV_SYMBOL_USB);
    lv_obj_align(header_power_label_, LV_ALIGN_CENTER, 4, center_y);
  }
}

void DisplayShell::update_power_header(const PowerSnapshot& power) {
  if (header_power_label_ == nullptr || !power.available || !power.battery_present) return;
  const bool externally_powered = power.charging || power.usb_present;
  const std::uint32_t color = power.charging ? theme_style_.accent_secondary
                                             : theme_style_.text_primary;
  if (externally_powered) {
    lv_label_set_text_fmt(header_power_label_, LV_SYMBOL_CHARGE " %u%%",
                          static_cast<unsigned>(power.battery_percent));
  } else {
    lv_label_set_text_fmt(header_power_label_, "%u%%",
                          static_cast<unsigned>(power.battery_percent));
  }
  lv_obj_set_style_text_color(header_power_label_, lv_color_hex(color), LV_PART_MAIN);
  if (header_battery_outline_ != nullptr && lv_obj_is_valid(header_battery_outline_)) {
    lv_obj_t* battery = lv_obj_get_parent(header_battery_outline_);
    if (battery != nullptr && lv_obj_is_valid(battery)) {
      lv_obj_update_layout(header_power_label_);
      lv_obj_align_to(battery, header_power_label_, LV_ALIGN_OUT_LEFT_MID, -4, -1);
    }
  }
  if (header_battery_outline_ != nullptr) {
    lv_obj_set_style_border_color(header_battery_outline_, lv_color_hex(color), LV_PART_MAIN);
  }
  if (header_battery_fill_ != nullptr) {
    const int fill_height = std::max(2, 10 * static_cast<int>(power.battery_percent) / 100);
    lv_obj_set_size(header_battery_fill_, 4, fill_height);
    lv_obj_align(header_battery_fill_, LV_ALIGN_CENTER, 0, 4 - fill_height / 2);
    lv_obj_set_style_bg_color(header_battery_fill_, lv_color_hex(color), LV_PART_MAIN);
  }
}

void DisplayShell::show_printer_status(const core::PrinterProfile& profile,
                                       const core::PrinterSnapshot& snapshot,
                                       const PowerSnapshot& power,
                                       const char* ipv4) {
  (void)ipv4;
  if (snapshot.job.preview && snapshot.job.preview->empty()) {
    preview_encoded_.reset();
    preview_pixels_.reset();
  } else if (snapshot.job.preview && preview_encoded_.get() != snapshot.job.preview.get()) {
    std::shared_ptr<std::vector<std::uint8_t>> decoded;
    lv_image_dsc_t descriptor{};
    if (decode_preview_png(snapshot.job.preview, decoded, descriptor)) {
      preview_encoded_ = snapshot.job.preview;
      preview_pixels_ = std::move(decoded);
      preview_image_dsc_ = descriptor;
      view_ = -1;
    }
  } else if (!snapshot.job.preview && preview_encoded_) {
    preview_encoded_.reset();
    preview_pixels_.reset();
    preview_image_dsc_ = {};
    view_ = -1;
  }
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_printer_status(profile, snapshot, power);
    return;
  }
  if (ipv4 == nullptr || board_display_lock(1000) != ESP_OK) return;
  if (view_ != 3 || visible_profile_ != profile.id) {
    lv_obj_t* screen = lv_screen_active();
    prepare_active_screen("printer-status");
    create_printer_chrome(profile, snapshot, &power);

    media_image_ = nullptr;
    if (preview_pixels_ && !preview_pixels_->empty()) {
      lv_obj_t* preview_frame = lv_obj_create(screen);
      lv_obj_set_size(preview_frame, 158, 158);
      lv_obj_align(preview_frame, LV_ALIGN_CENTER, -90, -3);
      lv_obj_set_style_radius(preview_frame, themed_radius(9), LV_PART_MAIN);
      lv_obj_set_style_bg_color(preview_frame,
                                lv_color_hex(theme_colors_.preview_background), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(preview_frame, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(preview_frame,
                                    lv_color_hex(theme_colors_.done), LV_PART_MAIN);
      lv_obj_set_style_border_opa(preview_frame, LV_OPA_70, LV_PART_MAIN);
      lv_obj_set_style_border_width(preview_frame, 1, LV_PART_MAIN);
      lv_obj_set_style_pad_all(preview_frame, 0, LV_PART_MAIN);
      make_gesture_passthrough(preview_frame);
      media_image_ = lv_image_create(screen);
      lv_image_set_src(media_image_, &preview_image_dsc_);
      lv_obj_set_size(media_image_, 148, 148);
      lv_image_set_inner_align(media_image_, LV_IMAGE_ALIGN_CONTAIN);
      lv_obj_align(media_image_, LV_ALIGN_CENTER, -90, -3);
      make_gesture_passthrough(media_image_);
    } else {
      lv_obj_t* badge_slot = lv_obj_create(screen);
      lv_obj_set_size(badge_slot, 158, 158);
      lv_obj_align(badge_slot, LV_ALIGN_CENTER, -90, -3);
      lv_obj_set_style_radius(badge_slot, themed_radius(9), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(badge_slot, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_color(badge_slot,
                                    lv_color_hex(theme_colors_.done), LV_PART_MAIN);
      lv_obj_set_style_border_opa(badge_slot, LV_OPA_70, LV_PART_MAIN);
      lv_obj_set_style_border_width(badge_slot, 1, LV_PART_MAIN);
      lv_obj_set_style_pad_all(badge_slot, 0, LV_PART_MAIN);
      make_gesture_passthrough(badge_slot);

      lv_obj_t* badge = lv_obj_create(badge_slot);
      lv_obj_set_size(badge, 148, 148);
      lv_obj_set_style_radius(badge, themed_radius(8), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_opa(badge, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(badge, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);
      lv_obj_center(badge);
      make_gesture_passthrough(badge);
      if (const lv_image_dsc_t* logo = brand_logo(profile); logo != nullptr) {
        lv_obj_t* mark = lv_image_create(badge);
        lv_image_set_src(mark, logo);
        lv_image_set_scale(mark, 256);
        lv_image_set_antialias(mark, true);
        lv_obj_set_style_image_recolor(
            mark, lv_color_hex(brand_logo_color(profile, theme_style_.background)),
            LV_PART_MAIN);
        lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_center(mark);
        make_gesture_passthrough(mark);
      } else {
        lv_obj_t* mark = lv_label_create(badge);
        lv_label_set_text(mark, brand_mark(profile));
        apply_text_style(mark, lv_color_hex(brand_color(profile)), &lv_font_montserrat_32);
        lv_obj_center(mark);
        make_gesture_passthrough(mark);
      }
    }
    detail_label_ = lv_label_create(screen);
    apply_text_style(detail_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_16);
    lv_obj_set_size(detail_label_, 300, 20);
    lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label_, LV_ALIGN_CENTER, 0, -116);

    create_mdi_icon(screen, kMdiClock, 16, -17, theme_style_.accent_secondary);
    remaining_label_ = lv_label_create(screen);
    apply_text_style(remaining_label_, lv_color_hex(theme_style_.accent_secondary),
                     &lv_font_montserrat_24);
    lv_obj_set_style_text_align(remaining_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(remaining_label_, 150);
    lv_obj_align(remaining_label_, LV_ALIGN_CENTER, 120, -26);
    total_time_label_ = lv_label_create(screen);
    apply_text_style(total_time_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_16);
    lv_obj_set_style_text_align(total_time_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(total_time_label_, 150);
    lv_obj_align(total_time_label_, LV_ALIGN_CENTER, 120, -2);
    layer_label_ = lv_label_create(screen);
    // UNSCII is monospaced and substantially wider than Montserrat at the
    // same nominal size. Keep three-digit current/total layer values on one
    // line in Retro Terminal without changing the other themes.
    const lv_font_t* layer_font = theme_style_.terminal_typography
                                      ? &lv_font_montserrat_14
                                      : &lv_font_montserrat_24;
    apply_text_style(layer_label_, lv_color_hex(theme_style_.text_primary),
                     layer_font);
    lv_obj_set_width(layer_label_, 190);
    lv_obj_set_style_text_align(layer_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(layer_label_, LV_ALIGN_CENTER, 92, 30);

    constexpr std::uint32_t kNozzleColor = 0xFF7043;
    constexpr std::uint32_t kBedColor = 0xFFB020;
    constexpr std::uint32_t kChamberColor = 0xA78BFA;
    create_mdi_icon(screen, kMdiNozzle, -140, 120, kNozzleColor, 158);
    nozzle_temperature_label_ = lv_label_create(screen);
    apply_text_style(nozzle_temperature_label_, lv_color_hex(kNozzleColor),
                     &lv_font_montserrat_24);
    lv_obj_set_style_text_align(nozzle_temperature_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(nozzle_temperature_label_, 78);
    lv_obj_align(nozzle_temperature_label_, LV_ALIGN_CENTER, -88, 113);
    create_mdi_icon(screen, kMdiBed, -15, 118, kBedColor, 158);
    bed_temperature_label_ = lv_label_create(screen);
    apply_text_style(bed_temperature_label_, lv_color_hex(kBedColor),
                     &lv_font_montserrat_24);
    lv_obj_set_style_text_align(bed_temperature_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(bed_temperature_label_, 78);
    lv_obj_align(bed_temperature_label_, LV_ALIGN_CENTER, 37, 113);
    create_thermometer_icon(screen, 85, 112, kChamberColor);
    chamber_temperature_label_ = lv_label_create(screen);
    apply_text_style(chamber_temperature_label_, lv_color_hex(kChamberColor),
                     &lv_font_montserrat_24);
    lv_obj_set_style_text_align(chamber_temperature_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(chamber_temperature_label_, 78);
    lv_obj_align(chamber_temperature_label_, LV_ALIGN_CENTER, 137, 113);

    metrics_label_ = lv_label_create(screen);
    apply_text_style(metrics_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_16);
    lv_obj_set_width(metrics_label_, 330);
    lv_label_set_long_mode(metrics_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(metrics_label_, LV_ALIGN_CENTER, 0, 142);
    status_label_ = lv_label_create(screen);
    apply_text_style(status_label_, lv_color_hex(accent_color_), &lv_font_montserrat_24);
    lv_obj_set_width(status_label_, 250);
    lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, -50);
    view_ = 3;
    visible_profile_ = profile.id;
  }

  lv_label_set_text(title_label_, profile.display_name.c_str());
  const std::uint32_t state_color =
      core::phase_color(theme_colors_, snapshot.job.phase, snapshot.job.reachable);
  const bool active_job = snapshot.job.phase == core::JobPhase::printing ||
                          snapshot.job.phase == core::JobPhase::preparing ||
                          snapshot.job.phase == core::JobPhase::paused;
  update_printer_progress(snapshot);
  const std::string display_job_name = core::job_name_for_display(snapshot.job.name);
  lv_label_set_text(detail_label_, snapshot.job.kind == core::JobKind::calibration
                                       ? tr("Printer calibration")
                                       : display_job_name.empty()
                                             ? tr("No active print")
                                             : display_job_name.c_str());
  const std::string remaining = active_job ? duration_hms(snapshot.job.remaining_seconds) : "--m";
  const std::uint32_t total_seconds = snapshot.job.elapsed_seconds + snapshot.job.remaining_seconds;
  const std::string total = active_job && total_seconds > 0 ? duration_hms(total_seconds) : "--";
  lv_label_set_text(remaining_label_, remaining.c_str());
  lv_label_set_text(total_time_label_, total.c_str());
  if (snapshot.job.current_layer > 0 || snapshot.job.total_layers > 0) {
    lv_label_set_text_fmt(layer_label_, "%s: %u / %u", tr("Layer"),
                          snapshot.job.current_layer, snapshot.job.total_layers);
  } else {
    lv_label_set_text_fmt(layer_label_, "%s: -- / --", tr("Layer"));
  }
  lv_label_set_text_fmt(nozzle_temperature_label_, "%.0f°C",
                        snapshot.job.temperatures.nozzle_c);
  lv_label_set_text_fmt(bed_temperature_label_, "%.0f°C",
                        snapshot.job.temperatures.bed_c);
  if (snapshot.job.temperatures.chamber_known) {
    lv_label_set_text_fmt(chamber_temperature_label_, "%.0f°C",
                          snapshot.job.temperatures.chamber_c);
  } else {
    lv_label_set_text(chamber_temperature_label_, "--°C");
  }
  const char* ready_text = snapshot.link == core::LinkState::online
                               ? (profile.protocol == core::PrinterProtocol::moonraker
                                      ? "Klipper ready" : "Printer ready")
                               : link_label(snapshot.link);
  lv_label_set_text(metrics_label_, active_job ? "" : tr(ready_text));
  lv_obj_set_style_text_color(status_label_, lv_color_hex(state_color), LV_PART_MAIN);
  lv_label_set_text(status_label_, tr(core::job_status_label(snapshot.job)));
  update_power_header(power);
  board_display_unlock();
}

void DisplayShell::show_printer_nozzles(const core::PrinterProfile& profile,
                                        const core::PrinterSnapshot& snapshot,
                                        const PowerSnapshot& power) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_printer_nozzles(profile, snapshot, power);
    return;
  }
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 19 || visible_profile_ != profile.id) {
    prepare_active_screen("nozzles");
    create_printer_chrome(profile, snapshot, &power);
    // The established nozzle page keeps the header intentionally sparse: the
    // tool columns are the identity of this view.
    lv_obj_add_flag(title_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* screen = lv_screen_active();
    nozzle_scroll_ = lv_obj_create(screen);
    lv_obj_set_size(nozzle_scroll_, 360, 194);
    lv_obj_align(nozzle_scroll_, LV_ALIGN_CENTER, 0, 12);
    lv_obj_set_scroll_dir(nozzle_scroll_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(nozzle_scroll_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(nozzle_scroll_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(nozzle_scroll_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nozzle_scroll_, 0, LV_PART_MAIN);
    lv_obj_set_style_width(nozzle_scroll_, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(nozzle_scroll_, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(nozzle_scroll_, lv_color_hex(accent_color_),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(nozzle_scroll_, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(nozzle_scroll_, nozzle_scroll_event, LV_EVENT_ALL, this);

    nozzle_row_ = lv_obj_create(nozzle_scroll_);
    lv_obj_set_size(nozzle_row_, 360, 178);
    lv_obj_set_pos(nozzle_row_, 0, 0);
    lv_obj_remove_flag(nozzle_row_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(nozzle_row_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(nozzle_row_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(nozzle_row_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nozzle_row_, 0, LV_PART_MAIN);
    for (std::size_t index = 0; index < core::kMaximumToolheads; ++index) {
      lv_obj_t* card = lv_obj_create(nozzle_row_);
      nozzle_cards_[index] = card;
      lv_obj_set_size(card, 82, 178);
      lv_obj_set_pos(card, static_cast<int>(index) * 90, 0);
      lv_obj_set_style_radius(card, themed_radius(14), LV_PART_MAIN);
      lv_obj_set_style_bg_color(card, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_70, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, lv_color_hex(theme_style_.track), LV_PART_MAIN);
      lv_obj_set_style_border_opa(card, LV_OPA_70, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
      lv_obj_set_style_shadow_width(card, 4, LV_PART_MAIN);
      lv_obj_set_style_shadow_opa(card, LV_OPA_20, LV_PART_MAIN);
      lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), LV_PART_MAIN);
      lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
      apply_surface_effect(card);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
      make_gesture_passthrough(card);

      nozzle_tool_labels_[index] = lv_label_create(card);
      apply_text_style(nozzle_tool_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_16);
      lv_obj_set_width(nozzle_tool_labels_[index], 80);
      lv_obj_set_style_text_align(nozzle_tool_labels_[index], LV_TEXT_ALIGN_CENTER,
                                  LV_PART_MAIN);
      lv_obj_align(nozzle_tool_labels_[index], LV_ALIGN_TOP_MID, 0, 4);
      make_gesture_passthrough(nozzle_tool_labels_[index]);

      nozzle_icons_[index] = lv_label_create(card);
      lv_label_set_text(nozzle_icons_[index], kMdiNozzle);
      apply_text_style(nozzle_icons_[index], lv_color_hex(theme_style_.text_muted), &mdi_40);
      lv_obj_align(nozzle_icons_[index], LV_ALIGN_TOP_MID, 0, 42);
      make_gesture_passthrough(nozzle_icons_[index]);

      nozzle_target_labels_[index] = lv_label_create(card);
      apply_text_style(nozzle_target_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_14);
      lv_obj_set_width(nozzle_target_labels_[index], 80);
      lv_obj_set_style_text_align(nozzle_target_labels_[index], LV_TEXT_ALIGN_CENTER,
                                  LV_PART_MAIN);
      lv_obj_align(nozzle_target_labels_[index], LV_ALIGN_TOP_MID, 0, 23);
      make_gesture_passthrough(nozzle_target_labels_[index]);

      nozzle_temperature_labels_[index] = lv_label_create(card);
      apply_text_style(nozzle_temperature_labels_[index], lv_color_hex(theme_style_.text_primary),
                       &lv_font_montserrat_16);
      lv_obj_set_width(nozzle_temperature_labels_[index], 78);
      lv_obj_set_height(nozzle_temperature_labels_[index], 20);
      lv_label_set_long_mode(nozzle_temperature_labels_[index], LV_LABEL_LONG_CLIP);
      lv_obj_set_style_text_align(nozzle_temperature_labels_[index], LV_TEXT_ALIGN_CENTER,
                                  LV_PART_MAIN);
      lv_obj_align(nozzle_temperature_labels_[index], LV_ALIGN_TOP_MID, 0, 84);
      make_gesture_passthrough(nozzle_temperature_labels_[index]);

      nozzle_material_labels_[index] = lv_label_create(card);
      apply_text_style(nozzle_material_labels_[index], lv_color_hex(theme_style_.text_secondary),
                       &lv_font_montserrat_14);
      lv_obj_set_width(nozzle_material_labels_[index], 80);
      lv_obj_set_height(nozzle_material_labels_[index], 18);
      lv_label_set_long_mode(nozzle_material_labels_[index], LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_align(nozzle_material_labels_[index], LV_TEXT_ALIGN_CENTER,
                                  LV_PART_MAIN);
      lv_obj_align(nozzle_material_labels_[index], LV_ALIGN_TOP_MID, 0, 109);
      make_gesture_passthrough(nozzle_material_labels_[index]);

      nozzle_material_dots_[index] = lv_obj_create(card);
      lv_obj_set_size(nozzle_material_dots_[index], 18, 18);
      lv_obj_set_style_radius(nozzle_material_dots_[index], LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(nozzle_material_dots_[index], lv_color_hex(theme_style_.text_muted),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(nozzle_material_dots_[index], 0, LV_PART_MAIN);
      lv_obj_align(nozzle_material_dots_[index], LV_ALIGN_TOP_MID, 0, 138);
      make_gesture_passthrough(nozzle_material_dots_[index]);
    }
    view_ = 19;
    visible_profile_ = profile.id;
  }

  int count = snapshot.job.toolhead_count;
  if (count <= 0) count = 1;
  count = std::clamp(count, 1, static_cast<int>(core::kMaximumToolheads));
  constexpr int kViewportWidth = 360;
  constexpr int kCardGap = 8;
  const bool overflow = count > 4;
  const int overflow_card_width = 82;
  const int overflow_stride = overflow_card_width + kCardGap;
  const int content_width = overflow ? count * overflow_stride - kCardGap : kViewportWidth;
  lv_obj_set_width(nozzle_row_, content_width);
  if (overflow) {
    lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(nozzle_scroll_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(nozzle_scroll_, LV_OBJ_FLAG_GESTURE_BUBBLE);
  } else {
    lv_obj_remove_flag(nozzle_scroll_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_GESTURE_BUBBLE);
  }
  lv_obj_set_scrollbar_mode(nozzle_scroll_, overflow ? LV_SCROLLBAR_MODE_ON
                                                     : LV_SCROLLBAR_MODE_OFF);
  if (!overflow) lv_obj_scroll_to_x(nozzle_scroll_, 0, LV_ANIM_OFF);
  for (int index = 0; index < static_cast<int>(core::kMaximumToolheads); ++index) {
    const bool present = index < count;
    if (present) lv_obj_remove_flag(nozzle_cards_[index], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(nozzle_cards_[index], LV_OBJ_FLAG_HIDDEN);
    if (!present) continue;

    core::ToolheadState fallback;
    const core::ToolheadState* tool = nullptr;
    if (index < snapshot.job.toolhead_count && snapshot.job.toolheads[index].present) {
      tool = &snapshot.job.toolheads[index];
    } else {
      fallback.present = true;
      fallback.active = index == 0;
      fallback.temperature_known = index == 0;
      fallback.temperature_c = index == 0 ? snapshot.job.temperatures.nozzle_c : 0.0F;
      fallback.target_c = index == 0 ? snapshot.job.temperatures.nozzle_target_c : 0.0F;
      tool = &fallback;
    }

    const int card_width = overflow ? overflow_card_width
        : count == 1 ? 150 : count == 2 ? 118 : count == 3 ? 96 : 82;
    const int gap = count == 2 ? 18 : count == 3 ? 14 : kCardGap;
    const int stride = card_width + gap;
    const int occupied_width = count * card_width + (count - 1) * gap;
    const int x = overflow ? index * overflow_stride
                           : (kViewportWidth - occupied_width) / 2 + index * stride;
    lv_obj_set_width(nozzle_cards_[index], card_width);
    for (lv_obj_t* label : {nozzle_tool_labels_[index], nozzle_target_labels_[index],
                            nozzle_temperature_labels_[index], nozzle_material_labels_[index]}) {
      lv_obj_set_width(label, card_width - 8);
    }
    lv_obj_set_pos(nozzle_cards_[index], x, 0);
    lv_label_set_text_fmt(nozzle_tool_labels_[index], "T%d", index);
    if (tool->temperature_known) {
      lv_label_set_text_fmt(nozzle_temperature_labels_[index], "%.0f°C", tool->temperature_c);
    } else {
      lv_label_set_text(nozzle_temperature_labels_[index], "--°C");
    }
    lv_label_set_text_fmt(nozzle_target_labels_[index], "%s %.0f°C", tr("SET"), tool->target_c);
    const bool empty = tool->filament_state_known && !tool->filament_detected;
    lv_label_set_text(nozzle_material_labels_[index],
                      empty ? "---" : (tool->material.empty() ? "--" : tool->material.c_str()));

    const std::uint32_t material_color = tool->material_rgba != 0
                                             ? (tool->material_rgba >> 8U) & 0x00FFFFFFU
                                             : (tool->active ? theme_colors_.done : theme_style_.text_muted);
    const std::uint32_t icon_color = material_color == 0 ? theme_style_.track : material_color;
    const std::uint32_t foreground = empty ? theme_style_.text_muted : theme_style_.text_primary;
    const bool loaded = !empty && (tool->filament_detected || !tool->material.empty());
    const std::uint32_t card_border = loaded ? icon_color
                                             : (tool->active ? theme_colors_.done : theme_style_.track);
    lv_obj_set_style_bg_color(nozzle_cards_[index],
                              lv_color_hex(empty ? theme_style_.surface_soft : theme_style_.surface_raised), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nozzle_cards_[index], empty ? LV_OPA_50 : LV_OPA_80,
                            LV_PART_MAIN);
    lv_obj_set_style_border_color(nozzle_cards_[index], lv_color_hex(card_border),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(nozzle_cards_[index],
                                tool->active ? LV_OPA_COVER : (loaded ? LV_OPA_60 : LV_OPA_40),
                                LV_PART_MAIN);
    lv_obj_set_style_border_width(nozzle_cards_[index], tool->active ? 2 : 1,
                                  LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_tool_labels_[index],
                                lv_color_hex(empty ? theme_style_.text_muted : theme_style_.text_secondary), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_icons_[index],
                                lv_color_hex(empty ? theme_style_.text_muted : icon_color), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_temperature_labels_[index], lv_color_hex(foreground),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_target_labels_[index],
                                lv_color_hex(empty ? theme_style_.text_muted : theme_style_.text_muted), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_material_labels_[index],
                                lv_color_hex(empty ? theme_style_.text_muted : theme_style_.text_secondary), LV_PART_MAIN);
    if (empty) {
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(nozzle_material_dots_[index], lv_color_hex(theme_style_.text_muted),
                                    LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(nozzle_material_dots_[index], lv_color_hex(material_color),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 1, LV_PART_MAIN);
      lv_obj_set_style_border_color(nozzle_material_dots_[index], lv_color_hex(theme_style_.text_muted),
                                    LV_PART_MAIN);
    }
  }
  update_printer_progress(snapshot);
  update_power_header(power);
  board_display_unlock();
}

void DisplayShell::show_printer_compact(const core::PrinterProfile& profile,
                                        const core::PrinterSnapshot& snapshot,
                                        const PowerSnapshot& power) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_printer_compact(profile, snapshot, power);
    return;
  }
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 18 || visible_profile_ != profile.id) {
    lv_obj_t* screen = lv_screen_active();
    prepare_active_screen("compact-details");
    create_printer_chrome(profile, snapshot, &power);

    detail_label_ = lv_label_create(screen);
    apply_text_style(detail_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_16);
    lv_obj_set_size(detail_label_, 300, 20);
    lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0, 103);

    nozzle_scroll_ = lv_obj_create(screen);
    lv_obj_set_size(nozzle_scroll_, 328, 108);
    lv_obj_align(nozzle_scroll_, LV_ALIGN_TOP_MID, 0, 126);
    lv_obj_set_scroll_dir(nozzle_scroll_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(nozzle_scroll_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(nozzle_scroll_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(nozzle_scroll_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nozzle_scroll_, 0, LV_PART_MAIN);
    lv_obj_set_style_width(nozzle_scroll_, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(nozzle_scroll_, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(nozzle_scroll_, lv_color_hex(accent_color_),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(nozzle_scroll_, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(nozzle_scroll_, nozzle_scroll_event, LV_EVENT_ALL, this);

    nozzle_row_ = lv_obj_create(nozzle_scroll_);
    lv_obj_set_size(nozzle_row_, 328, 92);
    lv_obj_set_pos(nozzle_row_, 0, 0);
    lv_obj_remove_flag(nozzle_row_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(nozzle_row_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(nozzle_row_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(nozzle_row_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nozzle_row_, 0, LV_PART_MAIN);

    for (std::size_t index = 0; index < core::kMaximumToolheads; ++index) {
      nozzle_cards_[index] = lv_obj_create(nozzle_row_);
      lv_obj_set_size(nozzle_cards_[index], 76, 92);
      lv_obj_set_pos(nozzle_cards_[index], static_cast<int>(index) * 82, 0);
      lv_obj_set_style_radius(nozzle_cards_[index], themed_radius(12), LV_PART_MAIN);
      lv_obj_set_style_bg_color(nozzle_cards_[index], lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(nozzle_cards_[index], LV_OPA_80, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_cards_[index], 1, LV_PART_MAIN);
      lv_obj_set_style_border_color(nozzle_cards_[index], lv_color_hex(theme_style_.border),
                                    LV_PART_MAIN);
      lv_obj_set_style_pad_all(nozzle_cards_[index], 0, LV_PART_MAIN);
      apply_surface_effect(nozzle_cards_[index]);
      lv_obj_remove_flag(nozzle_cards_[index], LV_OBJ_FLAG_SCROLLABLE);
      make_gesture_passthrough(nozzle_cards_[index]);

      nozzle_tool_labels_[index] = lv_label_create(nozzle_cards_[index]);
      apply_text_style(nozzle_tool_labels_[index], lv_color_hex(theme_style_.text_secondary),
                       &lv_font_montserrat_14);
      lv_obj_set_width(nozzle_tool_labels_[index], 70);
      lv_obj_align(nozzle_tool_labels_[index], LV_ALIGN_TOP_MID, 0, 3);
      make_gesture_passthrough(nozzle_tool_labels_[index]);

      nozzle_material_dots_[index] = lv_obj_create(nozzle_cards_[index]);
      lv_obj_set_size(nozzle_material_dots_[index], 9, 9);
      lv_obj_set_style_radius(nozzle_material_dots_[index], LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(nozzle_material_dots_[index], lv_color_hex(theme_style_.text_muted),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(nozzle_material_dots_[index], 0, LV_PART_MAIN);
      lv_obj_align(nozzle_material_dots_[index], LV_ALIGN_TOP_RIGHT, -7, 8);
      make_gesture_passthrough(nozzle_material_dots_[index]);

      nozzle_icons_[index] = lv_label_create(nozzle_cards_[index]);
      lv_label_set_text(nozzle_icons_[index], kMdiNozzle);
      apply_text_style(nozzle_icons_[index], lv_color_hex(theme_style_.text_muted), &mdi_40);
      lv_obj_set_style_transform_scale(nozzle_icons_[index], 145, LV_PART_MAIN);
      lv_obj_align(nozzle_icons_[index], LV_ALIGN_TOP_MID, 7, 25);
      make_gesture_passthrough(nozzle_icons_[index]);

      nozzle_temperature_labels_[index] = lv_label_create(nozzle_cards_[index]);
      apply_text_style(nozzle_temperature_labels_[index], lv_color_hex(theme_style_.text_primary),
                       &lv_font_montserrat_16);
      lv_obj_set_width(nozzle_temperature_labels_[index], 70);
      lv_label_set_long_mode(nozzle_temperature_labels_[index], LV_LABEL_LONG_CLIP);
      lv_obj_align(nozzle_temperature_labels_[index], LV_ALIGN_TOP_MID, 0, 48);
      make_gesture_passthrough(nozzle_temperature_labels_[index]);

      nozzle_material_labels_[index] = lv_label_create(nozzle_cards_[index]);
      apply_text_style(nozzle_material_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_14);
      lv_obj_set_width(nozzle_material_labels_[index], 70);
      lv_label_set_long_mode(nozzle_material_labels_[index], LV_LABEL_LONG_DOT);
      lv_obj_align(nozzle_material_labels_[index], LV_ALIGN_BOTTOM_MID, 0, -2);
      make_gesture_passthrough(nozzle_material_labels_[index]);
    }

    layer_label_ = lv_label_create(screen);
    apply_text_style(layer_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_16);
    lv_obj_set_width(layer_label_, 300);
    lv_obj_align(layer_label_, LV_ALIGN_TOP_MID, 0, 241);

    constexpr std::uint32_t kNozzleColor = 0xFF7043;
    constexpr std::uint32_t kBedColor = 0xFFB020;
    constexpr std::uint32_t kChamberColor = 0xA78BFA;
    create_mdi_icon(screen, kMdiNozzle, -131, 70, kNozzleColor, 145);
    nozzle_temperature_label_ = lv_label_create(screen);
    apply_text_style(nozzle_temperature_label_, lv_color_hex(kNozzleColor),
                     &lv_font_montserrat_16);
    lv_obj_set_width(nozzle_temperature_label_, 76);
    lv_obj_set_style_text_align(nozzle_temperature_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(nozzle_temperature_label_, LV_ALIGN_CENTER, -85, 62);

    create_mdi_icon(screen, kMdiBed, -11, 70, kBedColor, 145);
    bed_temperature_label_ = lv_label_create(screen);
    apply_text_style(bed_temperature_label_, lv_color_hex(kBedColor),
                     &lv_font_montserrat_16);
    lv_obj_set_width(bed_temperature_label_, 76);
    lv_obj_set_style_text_align(bed_temperature_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(bed_temperature_label_, LV_ALIGN_CENTER, 34, 62);

    create_thermometer_icon(screen, 103, 62, kChamberColor);
    chamber_temperature_label_ = lv_label_create(screen);
    apply_text_style(chamber_temperature_label_, lv_color_hex(kChamberColor),
                     &lv_font_montserrat_16);
    lv_obj_set_width(chamber_temperature_label_, 76);
    lv_obj_set_style_text_align(chamber_temperature_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(chamber_temperature_label_, LV_ALIGN_CENTER, 156, 62);

    create_mdi_icon(screen, kMdiClock, -110, 105, theme_style_.accent_secondary, 190);
    remaining_label_ = lv_label_create(screen);
    apply_text_style(remaining_label_, lv_color_hex(theme_style_.accent_secondary), &lv_font_montserrat_24);
    lv_obj_set_width(remaining_label_, 190);
    lv_obj_align(remaining_label_, LV_ALIGN_CENTER, 28, 101);

    total_time_label_ = lv_label_create(screen);
    apply_text_style(total_time_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_14);
    lv_obj_set_width(total_time_label_, 300);
    lv_obj_set_style_text_line_space(total_time_label_, 3, LV_PART_MAIN);
    lv_obj_align(total_time_label_, LV_ALIGN_TOP_MID, 0, 359);

    status_label_ = lv_label_create(screen);
    apply_text_style(status_label_, lv_color_hex(accent_color_), &lv_font_montserrat_24);
    lv_obj_set_width(status_label_, 220);
    lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, -50);
    view_ = 18;
    visible_profile_ = profile.id;
  }

  lv_label_set_text(title_label_, profile.display_name.c_str());
  const std::string display_job_name = core::job_name_for_display(snapshot.job.name);
  const char* job = display_job_name.empty() ? tr("No active file") : display_job_name.c_str();
  lv_label_set_text(detail_label_, job);

  int count = snapshot.job.toolhead_count;
  if (count <= 0) count = 1;
  count = std::clamp(count, 1, static_cast<int>(core::kMaximumToolheads));
  constexpr int kViewportWidth = 328;
  constexpr int kCardWidth = 76;
  constexpr int kCardGap = 6;
  constexpr int kStride = kCardWidth + kCardGap;
  const bool overflow = count > 4;
  const int content_width = overflow ? count * kStride - kCardGap : kViewportWidth;
  lv_obj_set_width(nozzle_row_, content_width);
  if (overflow) {
    lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(nozzle_scroll_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(nozzle_scroll_, LV_OBJ_FLAG_GESTURE_BUBBLE);
  } else {
    lv_obj_remove_flag(nozzle_scroll_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(nozzle_scroll_, LV_OBJ_FLAG_GESTURE_BUBBLE);
  }
  lv_obj_set_scrollbar_mode(nozzle_scroll_, overflow ? LV_SCROLLBAR_MODE_ON
                                                     : LV_SCROLLBAR_MODE_OFF);
  if (!overflow) lv_obj_scroll_to_x(nozzle_scroll_, 0, LV_ANIM_OFF);
  const std::uint32_t state_color =
      core::phase_color(theme_colors_, snapshot.job.phase, snapshot.job.reachable);
  for (int index = 0; index < static_cast<int>(core::kMaximumToolheads); ++index) {
    const bool present = index < count;
    if (present) lv_obj_remove_flag(nozzle_cards_[index], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(nozzle_cards_[index], LV_OBJ_FLAG_HIDDEN);
    if (!present) continue;

    core::ToolheadState fallback;
    const core::ToolheadState* tool = nullptr;
    if (index < snapshot.job.toolhead_count && snapshot.job.toolheads[index].present) {
      tool = &snapshot.job.toolheads[index];
    } else {
      fallback.present = true;
      fallback.active = index == 0;
      fallback.temperature_known = index == 0;
      fallback.temperature_c = index == 0 ? snapshot.job.temperatures.nozzle_c : 0.0F;
      fallback.target_c = index == 0 ? snapshot.job.temperatures.nozzle_target_c : 0.0F;
      tool = &fallback;
    }

    const int occupied_width = count * kCardWidth + (count - 1) * kCardGap;
    const int x = overflow ? index * kStride
                           : (kViewportWidth - occupied_width) / 2 + index * kStride;
    lv_obj_set_pos(nozzle_cards_[index], x, 0);
    lv_label_set_text_fmt(nozzle_tool_labels_[index], "T%d", index);
    if (tool->temperature_known) {
      lv_label_set_text_fmt(nozzle_temperature_labels_[index], "%.0f°C", tool->temperature_c);
    } else {
      lv_label_set_text(nozzle_temperature_labels_[index], "--°C");
    }
    const bool empty = tool->filament_state_known && !tool->filament_detected;
    lv_label_set_text(nozzle_material_labels_[index],
                      empty ? "---" : (tool->material.empty() ? "--" : tool->material.c_str()));
    const std::uint32_t material_color = tool->material_rgba != 0
                                             ? (tool->material_rgba >> 8U) & 0x00FFFFFFU
                                             : (tool->active ? theme_colors_.done : theme_style_.text_muted);
    const std::uint32_t muted = empty ? theme_style_.text_muted : theme_style_.text_secondary;
    lv_obj_set_style_border_width(nozzle_cards_[index], tool->active ? 2 : 1,
                                  LV_PART_MAIN);
    lv_obj_set_style_border_color(nozzle_cards_[index],
                                  lv_color_hex(tool->active ? state_color : theme_style_.border),
                                  LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_tool_labels_[index], lv_color_hex(muted), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_icons_[index],
                                lv_color_hex(empty ? muted : material_color), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_temperature_labels_[index],
                                lv_color_hex(empty ? muted : theme_style_.text_primary), LV_PART_MAIN);
    lv_obj_set_style_text_color(nozzle_material_labels_[index],
                                lv_color_hex(empty ? muted : theme_style_.text_muted), LV_PART_MAIN);
    if (empty) {
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(nozzle_material_dots_[index], lv_color_hex(muted),
                                    LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(nozzle_material_dots_[index], lv_color_hex(material_color),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_opa(nozzle_material_dots_[index], LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(nozzle_material_dots_[index], 0, LV_PART_MAIN);
    }
  }

  lv_label_set_text_fmt(layer_label_, "%s: %u / %u", tr("Layer"),
                        snapshot.job.current_layer, snapshot.job.total_layers);
  lv_label_set_text_fmt(nozzle_temperature_label_, "%.0f°C",
                        snapshot.job.temperatures.nozzle_c);
  lv_label_set_text_fmt(bed_temperature_label_, "%.0f°C", snapshot.job.temperatures.bed_c);
  if (snapshot.job.temperatures.chamber_known) {
    lv_label_set_text_fmt(chamber_temperature_label_, "%.0f°C",
                          snapshot.job.temperatures.chamber_c);
  } else {
    lv_label_set_text(chamber_temperature_label_, "--°C");
  }
  const bool active_job = snapshot.job.phase == core::JobPhase::printing ||
                          snapshot.job.phase == core::JobPhase::preparing ||
                          snapshot.job.phase == core::JobPhase::paused;
  const std::string remaining = active_job ? duration_text(snapshot.job.remaining_seconds) : "--m";
  const std::string elapsed = snapshot.job.elapsed_seconds > 0
                                  ? duration_text(snapshot.job.elapsed_seconds) : "--";
  const std::uint32_t total_seconds = snapshot.job.elapsed_seconds + snapshot.job.remaining_seconds;
  const std::string total = total_seconds > 0 ? duration_text(total_seconds) : "--";
  lv_label_set_text(remaining_label_, remaining.c_str());
  lv_label_set_text_fmt(total_time_label_, "%s                         %s\n%s                         %s",
                        tr("PRINT"), tr("TOTAL"), elapsed.c_str(), total.c_str());
  lv_label_set_text(status_label_, tr(core::job_status_label(snapshot.job)));
  lv_obj_set_style_text_color(status_label_, lv_color_hex(state_color), LV_PART_MAIN);
  update_printer_progress(snapshot);
  update_power_header(power);
  board_display_unlock();
}

void DisplayShell::show_printer_telemetry(const core::PrinterProfile& profile,
                                          const core::PrinterSnapshot& snapshot,
                                          const PowerSnapshot& power) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_printer_telemetry(profile, snapshot, power);
    return;
  }
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 20 || visible_profile_ != profile.id) {
    lv_obj_t* screen = lv_screen_active();
    prepare_active_screen("speeds");
    create_printer_chrome(profile, snapshot, &power);

    detail_label_ = lv_label_create(screen);
    apply_text_style(detail_label_, lv_color_hex(theme_style_.accent_secondary), &lv_font_montserrat_24);
    lv_obj_set_width(detail_label_, 300);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0, 108);

    constexpr std::array<const char*, 3> kCaptions = {"SPEED", "FLOW", "FAN"};
    const std::array<std::uint32_t, 3> kColors = {
        theme_style_.accent_secondary, theme_colors_.done, theme_colors_.preparing};
    for (std::size_t index = 0; index < telemetry_metric_cards_.size(); ++index) {
      telemetry_metric_cards_[index] = lv_obj_create(screen);
      lv_obj_set_size(telemetry_metric_cards_[index], 118, 68);
      lv_obj_align(telemetry_metric_cards_[index], LV_ALIGN_TOP_MID,
                   -122 + static_cast<int>(index) * 122, 174);
      lv_obj_set_style_radius(telemetry_metric_cards_[index], themed_radius(12), LV_PART_MAIN);
      lv_obj_set_style_bg_color(telemetry_metric_cards_[index], lv_color_hex(theme_style_.surface_raised),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_opa(telemetry_metric_cards_[index], LV_OPA_80, LV_PART_MAIN);
      lv_obj_set_style_border_width(telemetry_metric_cards_[index], 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(telemetry_metric_cards_[index],
                                    lv_color_hex(kColors[index]), LV_PART_MAIN);
      lv_obj_set_style_pad_all(telemetry_metric_cards_[index], 0, LV_PART_MAIN);
      apply_surface_effect(telemetry_metric_cards_[index]);
      lv_obj_remove_flag(telemetry_metric_cards_[index], LV_OBJ_FLAG_SCROLLABLE);
      make_gesture_passthrough(telemetry_metric_cards_[index]);

      telemetry_metric_caption_labels_[index] =
          lv_label_create(telemetry_metric_cards_[index]);
      lv_label_set_text(telemetry_metric_caption_labels_[index], tr(kCaptions[index]));
      apply_text_style(telemetry_metric_caption_labels_[index], lv_color_hex(theme_style_.text_muted),
                       &lv_font_montserrat_14);
      lv_obj_set_size(telemetry_metric_caption_labels_[index], 112, 16);
      lv_label_set_long_mode(telemetry_metric_caption_labels_[index], LV_LABEL_LONG_DOT);
      lv_obj_align(telemetry_metric_caption_labels_[index], LV_ALIGN_TOP_MID, 0, 7);
      make_gesture_passthrough(telemetry_metric_caption_labels_[index]);

      telemetry_metric_value_labels_[index] =
          lv_label_create(telemetry_metric_cards_[index]);
      apply_text_style(telemetry_metric_value_labels_[index], lv_color_hex(kColors[index]),
                       &lv_font_montserrat_24);
      lv_obj_set_width(telemetry_metric_value_labels_[index], 112);
      lv_obj_align(telemetry_metric_value_labels_[index], LV_ALIGN_BOTTOM_MID, 0, -5);
      make_gesture_passthrough(telemetry_metric_value_labels_[index]);
    }

    temperature_label_ = lv_label_create(screen);
    apply_text_style(temperature_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_16);
    lv_obj_set_width(temperature_label_, 360);
    lv_obj_align(temperature_label_, LV_ALIGN_TOP_MID, 0, 256);

    for (std::size_t index = 0; index < telemetry_detail_caption_labels_.size(); ++index) {
      telemetry_detail_caption_labels_[index] = lv_label_create(screen);
      apply_text_style(telemetry_detail_caption_labels_[index],
                       lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_14);
      lv_obj_set_width(telemetry_detail_caption_labels_[index], 150);
      lv_obj_align(telemetry_detail_caption_labels_[index], LV_ALIGN_TOP_MID,
                   index == 0 ? -78 : 78, 294);
    }

    nozzle_temperature_label_ = lv_label_create(screen);
    apply_text_style(nozzle_temperature_label_, lv_color_hex(0xFF7043),
                     &lv_font_montserrat_24);
    lv_obj_set_width(nozzle_temperature_label_, 150);
    lv_obj_align(nozzle_temperature_label_, LV_ALIGN_TOP_MID, -78, 315);

    bed_temperature_label_ = lv_label_create(screen);
    apply_text_style(bed_temperature_label_, lv_color_hex(0xFFB020),
                     &lv_font_montserrat_24);
    lv_obj_set_width(bed_temperature_label_, 150);
    lv_obj_align(bed_temperature_label_, LV_ALIGN_TOP_MID, 78, 315);

    layer_label_ = lv_label_create(screen);
    apply_text_style(layer_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_14);
    lv_obj_set_width(layer_label_, 250);
    lv_obj_align(layer_label_, LV_ALIGN_TOP_MID, 0, 355);

    status_label_ = lv_label_create(screen);
    apply_text_style(status_label_, lv_color_hex(accent_color_), &lv_font_montserrat_24);
    lv_obj_set_width(status_label_, 220);
    lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, -50);
    view_ = 20;
    visible_profile_ = profile.id;
  }

  lv_label_set_text(title_label_, profile.display_name.c_str());
  const auto& motion = snapshot.job.motion;
  const int active_tool = snapshot.job.active_toolhead >= 0
                              ? snapshot.job.active_toolhead : 0;
  const bool nozzle_power_known = active_tool < snapshot.job.toolhead_count &&
      snapshot.job.toolheads[active_tool].present &&
      snapshot.job.toolheads[active_tool].heater_power_known;
  const bool detailed_motion = motion.velocity_known || motion.position_known ||
                               nozzle_power_known || snapshot.job.bed_heater_power_known;
  const bool bambu_summary = profile.protocol == core::PrinterProtocol::bambu_lan &&
                             !detailed_motion;
  if (bambu_summary) {
    const std::string remaining = snapshot.job.remaining_seconds > 0
                                      ? duration_text(snapshot.job.remaining_seconds) + " " + tr("remaining")
                                      : tr("Local printer telemetry");
    lv_label_set_text(detail_label_, remaining.c_str());
    lv_label_set_text(telemetry_metric_caption_labels_[0], tr("SPEED"));
    lv_label_set_text(telemetry_metric_caption_labels_[1], tr("FAN"));
    lv_label_set_text(telemetry_metric_caption_labels_[2], tr("PROGRESS"));
    if (motion.speed_multiplier_known) {
      lv_label_set_text_fmt(telemetry_metric_value_labels_[0], "%.0f%%",
                            motion.speed_multiplier);
    } else {
      lv_label_set_text(telemetry_metric_value_labels_[0], "--%");
    }
    if (motion.fan_percent_known) {
      lv_label_set_text_fmt(telemetry_metric_value_labels_[1], "%.0f%%", motion.fan_percent);
    } else {
      lv_label_set_text(telemetry_metric_value_labels_[1], "--%");
    }
    lv_label_set_text_fmt(telemetry_metric_value_labels_[2], "%.0f%%", snapshot.job.completion);
    if (snapshot.job.total_layers > 0) {
      lv_label_set_text_fmt(temperature_label_, "%s %u / %u", tr("LAYER"), snapshot.job.current_layer,
                            snapshot.job.total_layers);
    } else {
      lv_label_set_text_fmt(temperature_label_, "%s -- / --", tr("LAYER"));
    }
    lv_label_set_text(telemetry_detail_caption_labels_[0], tr("NOZZLE TEMP"));
    lv_label_set_text(telemetry_detail_caption_labels_[1], tr("BED TEMP"));
    lv_label_set_text_fmt(nozzle_temperature_label_, "%.0f° / %.0f°",
                          snapshot.job.temperatures.nozzle_c,
                          snapshot.job.temperatures.nozzle_target_c);
    lv_label_set_text_fmt(bed_temperature_label_, "%.0f° / %.0f°",
                          snapshot.job.temperatures.bed_c,
                          snapshot.job.temperatures.bed_target_c);
    if (snapshot.job.temperatures.chamber_known) {
      lv_label_set_text_fmt(layer_label_, "%s: %.0f°C", tr("CHAMBER"),
                            snapshot.job.temperatures.chamber_c);
    } else {
      lv_label_set_text(layer_label_, tr("ACTUAL / TARGET"));
    }
  } else {
    lv_label_set_text(telemetry_metric_caption_labels_[0], tr("SPEED"));
    lv_label_set_text(telemetry_metric_caption_labels_[1], tr("FLOW"));
    lv_label_set_text(telemetry_metric_caption_labels_[2], tr("FAN"));
    if (motion.velocity_known) {
      lv_label_set_text_fmt(detail_label_, "%.1f mm/s", motion.velocity_mm_s);
    } else {
      lv_label_set_text(detail_label_, "-- mm/s");
    }
    if (motion.speed_multiplier_known) {
      lv_label_set_text_fmt(telemetry_metric_value_labels_[0], "%.0f%%",
                            motion.speed_multiplier);
    } else {
      lv_label_set_text(telemetry_metric_value_labels_[0], "--%");
    }
    if (motion.extrusion_multiplier_known) {
      lv_label_set_text_fmt(telemetry_metric_value_labels_[1], "%.0f%%",
                            motion.extrusion_multiplier);
    } else {
      lv_label_set_text(telemetry_metric_value_labels_[1], "--%");
    }
    if (motion.fan_percent_known) {
      lv_label_set_text_fmt(telemetry_metric_value_labels_[2], "%.0f%%", motion.fan_percent);
    } else {
      lv_label_set_text(telemetry_metric_value_labels_[2], "--%");
    }
    if (motion.position_known) {
      lv_label_set_text_fmt(temperature_label_, "X %.1f   Y %.1f   Z %.2f",
                            motion.x_mm, motion.y_mm, motion.z_mm);
    } else {
      lv_label_set_text(temperature_label_, "X --   Y --   Z --");
    }
    lv_label_set_text(telemetry_detail_caption_labels_[0], tr("NOZZLE POWER"));
    lv_label_set_text(telemetry_detail_caption_labels_[1], tr("BED POWER"));
    if (nozzle_power_known) {
      lv_label_set_text_fmt(nozzle_temperature_label_, "%.0f%%",
                            snapshot.job.toolheads[active_tool].heater_power * 100.0F);
    } else {
      lv_label_set_text(nozzle_temperature_label_, "--%");
    }
    if (snapshot.job.bed_heater_power_known) {
      lv_label_set_text_fmt(bed_temperature_label_, "%.0f%%",
                            snapshot.job.bed_heater_power * 100.0F);
    } else {
      lv_label_set_text(bed_temperature_label_, "--%");
    }
    std::string homed = motion.homed_axes;
    for (char& character : homed) {
      if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
    }
    lv_label_set_text_fmt(layer_label_, "%s: %s", tr("HOMED"), homed.empty() ? "--" : homed.c_str());
  }
  lv_label_set_text(status_label_, tr(core::job_status_label(snapshot.job)));
  const std::uint32_t state_color =
      core::phase_color(theme_colors_, snapshot.job.phase, snapshot.job.reachable);
  lv_obj_set_style_text_color(status_label_, lv_color_hex(state_color), LV_PART_MAIN);
  update_printer_progress(snapshot);
  update_power_header(power);
  board_display_unlock();
}

void DisplayShell::show_printer_materials(const core::PrinterProfile& profile,
                                          const core::PrinterSnapshot& snapshot) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_printer_materials(profile, snapshot);
    return;
  }
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 21 || visible_profile_ != profile.id) {
    prepare_active_screen("ams-lite");
    create_page_header("AMS LITE");
    lv_obj_t* screen = lv_screen_active();
    for (int index = 0; index < 4; ++index) {
      lv_obj_t* card = lv_obj_create(screen);
      material_cards_[index] = card;
      lv_obj_set_size(card, 70, 156);
      lv_obj_align(card, LV_ALIGN_TOP_MID, (index * 78) - 117, 104);
      lv_obj_set_style_radius(card, themed_radius(24), LV_PART_MAIN);
      lv_obj_set_style_bg_color(card, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, lv_color_hex(theme_style_.track), LV_PART_MAIN);
      lv_obj_set_style_border_opa(card, LV_OPA_70, LV_PART_MAIN);
      lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
      apply_surface_effect(card);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);

      material_slot_labels_[index] = lv_label_create(card);
      lv_label_set_text_fmt(material_slot_labels_[index], "%d", index + 1);
      apply_text_style(material_slot_labels_[index], lv_color_hex(theme_style_.text_secondary),
                       &lv_font_montserrat_12);
      lv_obj_set_width(material_slot_labels_[index], 62);
      lv_obj_align(material_slot_labels_[index], LV_ALIGN_TOP_MID, 0, 10);
      lv_obj_add_flag(material_slot_labels_[index], LV_OBJ_FLAG_EVENT_BUBBLE);

      material_feed_labels_[index] = lv_label_create(screen);
      lv_label_set_text(material_feed_labels_[index], "");
      apply_text_style(material_feed_labels_[index], lv_color_hex(accent_color_),
                       &lv_font_montserrat_24);
      lv_obj_set_width(material_feed_labels_[index], 70);
      lv_obj_set_style_text_align(material_feed_labels_[index], LV_TEXT_ALIGN_CENTER,
                                  LV_PART_MAIN);
      lv_obj_align(material_feed_labels_[index], LV_ALIGN_TOP_MID,
                   (index * 78) - 117, 258);
      make_gesture_passthrough(material_feed_labels_[index]);

      material_name_labels_[index] = lv_label_create(card);
      apply_text_style(material_name_labels_[index], lv_color_hex(theme_style_.text_primary),
                       &lv_font_montserrat_14);
      lv_obj_set_width(material_name_labels_[index], 62);
      lv_label_set_long_mode(material_name_labels_[index], LV_LABEL_LONG_DOT);
      lv_obj_align(material_name_labels_[index], LV_ALIGN_CENTER, 0, -2);
      lv_obj_add_flag(material_name_labels_[index], LV_OBJ_FLAG_EVENT_BUBBLE);

      material_percent_labels_[index] = lv_label_create(card);
      apply_text_style(material_percent_labels_[index], lv_color_hex(theme_style_.text_primary),
                       &lv_font_montserrat_14);
      lv_obj_set_width(material_percent_labels_[index], 62);
      lv_obj_align(material_percent_labels_[index], LV_ALIGN_BOTTOM_MID, 0, -12);
      lv_obj_add_flag(material_percent_labels_[index], LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    external_material_card_ = lv_obj_create(screen);
    lv_obj_set_size(external_material_card_, 248, 58);
    lv_obj_align(external_material_card_, LV_ALIGN_TOP_MID, 0, 285);
    lv_obj_set_style_radius(external_material_card_, themed_radius(29), LV_PART_MAIN);
    lv_obj_set_style_bg_color(external_material_card_, lv_color_hex(theme_style_.surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(external_material_card_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(external_material_card_, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(external_material_card_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_pad_all(external_material_card_, 0, LV_PART_MAIN);
    apply_surface_effect(external_material_card_);
    lv_obj_remove_flag(external_material_card_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(external_material_card_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(external_material_card_, LV_OBJ_FLAG_EVENT_BUBBLE);

    external_material_dot_ = lv_obj_create(external_material_card_);
    lv_obj_set_size(external_material_dot_, 18, 18);
    lv_obj_align(external_material_dot_, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_radius(external_material_dot_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(external_material_dot_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_border_width(external_material_dot_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(external_material_dot_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(external_material_dot_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(external_material_dot_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(external_material_dot_, LV_OBJ_FLAG_EVENT_BUBBLE);

    external_material_label_ = lv_label_create(external_material_card_);
    apply_text_style(external_material_label_, lv_color_hex(theme_style_.text_secondary),
                     &lv_font_montserrat_14);
    lv_obj_set_style_text_align(external_material_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(external_material_label_, 190);
    lv_label_set_long_mode(external_material_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(external_material_label_, LV_ALIGN_LEFT_MID, 48, 0);
    lv_obj_add_flag(external_material_label_, LV_OBJ_FLAG_EVENT_BUBBLE);
    create_depth_dots();
    view_ = 21;
    visible_profile_ = profile.id;
  }
  for (std::size_t index = 0; index < material_cards_.size(); ++index) {
    const core::MaterialSlot* slot = index < snapshot.job.materials.slots.size()
                                         ? &snapshot.job.materials.slots[index]
                                         : nullptr;
    const bool installed = slot != nullptr && slot->installed;
    const std::uint32_t color = installed && slot->rgba != 0
                                    ? (slot->rgba >> 8U) & 0xFFFFFFU
                                    : theme_style_.surface_soft;
    const std::uint32_t red = (color >> 16U) & 0xFFU;
    const std::uint32_t green = (color >> 8U) & 0xFFU;
    const std::uint32_t blue = color & 0xFFU;
    const bool light = red * 299U + green * 587U + blue * 114U > 150000U;
    const std::uint32_t text_color = installed ? (light ? theme_style_.surface : 0xFFFFFF) : theme_style_.text_muted;
    lv_obj_set_style_bg_color(material_cards_[index], lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(material_cards_[index], installed ? LV_OPA_COVER : LV_OPA_60,
                            LV_PART_MAIN);
    lv_obj_set_style_border_color(material_cards_[index],
                                  lv_color_hex(slot != nullptr && slot->feeding
                                                   ? accent_color_ : installed ? theme_style_.text_secondary : theme_style_.track),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(material_cards_[index],
                                  slot != nullptr && slot->feeding ? 4 : 2, LV_PART_MAIN);
    lv_obj_set_style_text_color(material_slot_labels_[index], lv_color_hex(text_color),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(material_feed_labels_[index], lv_color_hex(accent_color_),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(material_name_labels_[index], lv_color_hex(text_color),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(material_percent_labels_[index], lv_color_hex(text_color),
                                LV_PART_MAIN);
    lv_label_set_text(material_name_labels_[index],
                      installed ? (slot->material.empty() ? "FIL" : slot->material.c_str()) : tr("EMPTY"));
    lv_label_set_text(material_feed_labels_[index],
                      slot != nullptr && slot->feeding ? LV_SYMBOL_UP : "");
    if (installed && slot->remaining_percent >= 0) {
      lv_label_set_text_fmt(material_percent_labels_[index], "%d%%", slot->remaining_percent);
    } else {
      lv_label_set_text(material_percent_labels_[index], "--");
    }
  }
  const auto& external = snapshot.job.materials.external_spool;
  const std::uint32_t external_color = external.installed && external.rgba != 0
                                           ? (external.rgba >> 8U) & 0xFFFFFFU
                                           : theme_style_.track;
  lv_obj_set_style_bg_color(external_material_dot_, lv_color_hex(external_color), LV_PART_MAIN);
  lv_obj_set_style_border_color(external_material_card_,
                                lv_color_hex(external.feeding ? accent_color_ : theme_style_.track),
                                LV_PART_MAIN);
  if (external.installed) {
    const char* material = external.material.empty() ? tr("Material") : external.material.c_str();
    if (external.remaining_percent >= 0) {
      lv_label_set_text_fmt(external_material_label_, "%s  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET "  %d%%",
                            tr("External"), material, external.remaining_percent);
    } else {
      lv_label_set_text_fmt(external_material_label_, "%s  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET "  --",
                            tr("External"), material);
    }
  } else {
    lv_label_set_text_fmt(external_material_label_, "%s  " LV_SYMBOL_BULLET "  %s",
                          tr("External spool"), tr("Not detected"));
  }
  board_display_unlock();
}

void DisplayShell::show_printer_camera(const core::PrinterProfile& profile,
                                       const core::PrinterSnapshot& snapshot,
                                       const PowerSnapshot& power) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_printer_camera(profile, snapshot, power);
    return;
  }
  if (board_display_lock(1000) != ESP_OK) return;
  const bool frame_changed = snapshot.job.camera_frame &&
      camera_pixels_.get() != snapshot.job.camera_frame.get();
  const bool refresh_completed = camera_was_refreshing_ &&
      !snapshot.job.camera_refreshing && snapshot.job.camera_frame &&
      !snapshot.job.camera_frame->empty();
  camera_was_refreshing_ = snapshot.job.camera_refreshing;
  if (frame_changed || refresh_completed) {
    camera_activity_updated_until_us_ = esp_timer_get_time() + 800000;
    view_ = -1;
  }
  if (view_ != 22 || visible_profile_ != profile.id) {
    prepare_active_screen("local-camera");
    create_printer_chrome(profile, snapshot, &power);
    lv_label_set_text(title_label_, tr("CAMERA"));
    detail_label_ = lv_label_create(lv_screen_active());
    apply_text_style(detail_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_12);
    lv_obj_set_width(detail_label_, 390);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0, 106);
    media_image_ = lv_image_create(lv_screen_active());
    lv_obj_set_size(media_image_, 360, 203);
    lv_image_set_inner_align(media_image_, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(media_image_, LV_ALIGN_CENTER, 0, 7);
    camera_spinner_ = lv_spinner_create(lv_screen_active());
    lv_obj_set_size(camera_spinner_, 58, 58);
    lv_obj_set_style_arc_width(camera_spinner_, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(camera_spinner_, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(camera_spinner_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_arc_color(camera_spinner_, lv_color_hex(accent_color_),
                               LV_PART_INDICATOR);
    lv_obj_align(camera_spinner_, LV_ALIGN_CENTER, 0, 7);

    camera_empty_label_ = lv_label_create(lv_screen_active());
    apply_text_style(camera_empty_label_, lv_color_hex(theme_style_.text_secondary),
                     &lv_font_montserrat_16);
    lv_obj_set_size(camera_empty_label_, 330, 92);
    lv_label_set_long_mode(camera_empty_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(camera_empty_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(camera_empty_label_, 8, LV_PART_MAIN);
    lv_obj_align(camera_empty_label_, LV_ALIGN_CENTER, 0, -5);
    lv_obj_add_flag(camera_empty_label_, LV_OBJ_FLAG_HIDDEN);
    make_gesture_passthrough(camera_empty_label_);

    camera_activity_dot_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(camera_activity_dot_, 8, 8);
    lv_obj_align(camera_activity_dot_, LV_ALIGN_CENTER, -164, 117);
    lv_obj_set_style_radius(camera_activity_dot_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(camera_activity_dot_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(camera_activity_dot_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(camera_activity_dot_, LV_OBJ_FLAG_SCROLLABLE);
    make_gesture_passthrough(camera_activity_dot_);

    camera_activity_label_ = lv_label_create(lv_screen_active());
    apply_text_style(camera_activity_label_, lv_color_hex(theme_style_.text_muted),
                     &lv_font_montserrat_12);
    // Keep the left edge beside the activity dot while reserving enough room
    // for the wider terminal glyphs in "Refreshing…".
    lv_obj_set_width(camera_activity_label_, 210);
    lv_label_set_long_mode(camera_activity_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(camera_activity_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(camera_activity_label_, LV_ALIGN_CENTER, -47, 117);
    make_gesture_passthrough(camera_activity_label_);

    camera_mode_row_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(camera_mode_row_, 230, 38);
    lv_obj_align(camera_mode_row_, LV_ALIGN_CENTER, 0, 145);
    lv_obj_set_style_radius(camera_mode_row_, themed_radius(19), LV_PART_MAIN);
    lv_obj_set_style_bg_color(camera_mode_row_, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
    lv_obj_set_style_border_color(camera_mode_row_, lv_color_hex(theme_style_.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(camera_mode_row_, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(camera_mode_row_, 2, LV_PART_MAIN);
    apply_surface_effect(camera_mode_row_);
    lv_obj_set_flex_flow(camera_mode_row_, LV_FLEX_FLOW_ROW);
    // Backend discovery decides whether live video is truly available.  Keep
    // the selector hidden until that result is known to avoid a one-frame flash.
    lv_obj_add_flag(camera_mode_row_, LV_OBJ_FLAG_HIDDEN);
    camera_snapshot_button_ = lv_button_create(camera_mode_row_);
    camera_live_button_ = lv_button_create(camera_mode_row_);
    for (lv_obj_t* button : {camera_snapshot_button_, camera_live_button_}) {
      lv_obj_set_height(button, LV_PCT(100));
      lv_obj_set_flex_grow(button, 1);
      lv_obj_set_style_radius(button, themed_radius(16), LV_PART_MAIN);
      lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
      lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
      lv_obj_add_event_cb(button, camera_mode_event, LV_EVENT_CLICKED, this);
    }
    lv_obj_t* snapshots_label = lv_label_create(camera_snapshot_button_);
    lv_label_set_text(snapshots_label, tr("Snapshots"));
    apply_text_style(snapshots_label, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_14);
    lv_obj_center(snapshots_label);
    lv_obj_t* live_label = lv_label_create(camera_live_button_);
    lv_label_set_text(live_label, tr("Live"));
    apply_text_style(live_label, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_14);
    lv_obj_center(live_label);
    view_ = 22;
    visible_profile_ = profile.id;
  }
  const bool live = snapshot.job.camera_live_supported && camera_live_mode_.load();
  update_printer_progress(snapshot);
  update_power_header(power);
  const bool recently_updated = esp_timer_get_time() < camera_activity_updated_until_us_;
  const bool refreshing = snapshot.job.camera_refreshing;
  if (live || !snapshot.job.camera_frame || snapshot.job.camera_frame->empty()) {
    lv_obj_add_flag(camera_activity_dot_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(camera_activity_label_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(camera_activity_dot_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(camera_activity_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(camera_activity_dot_,
                              lv_color_hex((refreshing || recently_updated)
                                               ? accent_color_ : theme_style_.track),
                              LV_PART_MAIN);
    lv_obj_set_style_text_color(camera_activity_label_,
                                lv_color_hex((refreshing || recently_updated)
                                                 ? accent_color_ : theme_style_.text_muted),
                                LV_PART_MAIN);
    lv_label_set_text(camera_activity_label_, refreshing
                                                  ? tr("Refreshing…")
                                                  : recently_updated ? tr("Updated")
                                                                     : tr("Waiting…"));
  }
  if (snapshot.job.camera_live_supported) {
    lv_obj_remove_flag(camera_mode_row_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(camera_snapshot_button_,
                              lv_color_hex(live ? theme_style_.surface_soft : accent_color_), LV_PART_MAIN);
    lv_obj_set_style_bg_color(camera_live_button_,
                              lv_color_hex(live ? accent_color_ : theme_style_.surface_soft), LV_PART_MAIN);
  } else {
    lv_obj_add_flag(camera_mode_row_, LV_OBJ_FLAG_HIDDEN);
  }
  if (snapshot.job.camera_frame && !snapshot.job.camera_frame->empty() &&
      snapshot.job.camera_width > 0 && snapshot.job.camera_height > 0) {
    camera_pixels_ = snapshot.job.camera_frame;
    camera_image_dsc_ = {};
    camera_image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    camera_image_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    camera_image_dsc_.header.w = snapshot.job.camera_width;
    camera_image_dsc_.header.h = snapshot.job.camera_height;
    camera_image_dsc_.header.stride = snapshot.job.camera_width * sizeof(std::uint16_t);
    camera_image_dsc_.data_size = static_cast<std::uint32_t>(camera_pixels_->size());
    camera_image_dsc_.data = camera_pixels_->data();
    lv_image_set_src(media_image_, &camera_image_dsc_);
    lv_obj_add_flag(camera_spinner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(camera_empty_label_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(detail_label_, live ? tr("Live local stream")
                                          : tr("Live local snapshot"));
  } else {
    lv_image_set_src(media_image_, nullptr);
    const bool detection_failed = snapshot.job.camera_detail == "No camera detected";
    if (detection_failed) {
      lv_obj_add_flag(camera_spinner_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(camera_empty_label_, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(camera_empty_label_,
                        tr("No camera detected\nHave a camera? Contact support."));
      lv_label_set_text(detail_label_, tr("Camera unavailable"));
    } else {
      lv_obj_remove_flag(camera_spinner_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(camera_empty_label_, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(detail_label_, tr("Detecting camera…"));
    }
  }
  board_display_unlock();
}

void DisplayShell::show_printer_light(const core::PrinterProfile& profile,
                                      const core::PrinterSnapshot& snapshot) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_printer_light(profile, snapshot);
    return;
  }
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 23 || visible_profile_ != profile.id) {
    prepare_active_screen("printer-light");
    create_page_header("PRINTER LIGHT");

    chamber_light_bulb_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(chamber_light_bulb_, 132, 132);
    lv_obj_align(chamber_light_bulb_, LV_ALIGN_CENTER, 0, -38);
    lv_obj_set_style_radius(chamber_light_bulb_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(chamber_light_bulb_, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chamber_light_bulb_, 0, LV_PART_MAIN);
    make_gesture_passthrough(chamber_light_bulb_);

    lv_obj_t* bulb_base = lv_obj_create(lv_screen_active());
    lv_obj_set_size(bulb_base, 58, 32);
    lv_obj_align_to(bulb_base, chamber_light_bulb_, LV_ALIGN_OUT_BOTTOM_MID, 0, -9);
    lv_obj_set_style_radius(bulb_base, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bulb_base, lv_color_hex(theme_style_.text_muted), LV_PART_MAIN);
    lv_obj_set_style_border_width(bulb_base, 0, LV_PART_MAIN);
    make_gesture_passthrough(bulb_base);

    detail_label_ = lv_label_create(lv_screen_active());
    apply_text_style(detail_label_, lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_16);
    lv_obj_set_width(detail_label_, 320);
    lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(detail_label_, LV_ALIGN_CENTER, 0, 77);
    make_gesture_passthrough(detail_label_);

    chamber_light_button_ = lv_button_create(lv_screen_active());
    lv_obj_set_size(chamber_light_button_, 190, 58);
    lv_obj_align(chamber_light_button_, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_ext_click_area(chamber_light_button_, 12);
    lv_obj_set_style_radius(chamber_light_button_, themed_radius(22), LV_PART_MAIN);
    lv_obj_set_style_border_width(chamber_light_button_, 2, LV_PART_MAIN);
    lv_obj_add_flag(chamber_light_button_, LV_OBJ_FLAG_CHECKABLE);
    chamber_light_button_label_ = lv_label_create(chamber_light_button_);
    apply_text_style(chamber_light_button_label_, lv_color_hex(theme_style_.surface),
                     &lv_font_montserrat_16);
    lv_obj_center(chamber_light_button_label_);
    lv_obj_add_flag(chamber_light_button_label_, LV_OBJ_FLAG_EVENT_BUBBLE);
    chamber_light_spinner_ = lv_spinner_create(chamber_light_button_);
    lv_obj_set_size(chamber_light_spinner_, 28, 28);
    lv_obj_set_style_arc_width(chamber_light_spinner_, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(chamber_light_spinner_, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(chamber_light_spinner_, lv_color_hex(theme_style_.track), LV_PART_MAIN);
    lv_obj_set_style_arc_color(chamber_light_spinner_, lv_color_hex(theme_style_.surface),
                               LV_PART_INDICATOR);
    lv_obj_align(chamber_light_spinner_, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_add_flag(chamber_light_spinner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(chamber_light_button_, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      const bool enabled = lv_obj_has_state(lv_event_get_current_target_obj(event),
                                            LV_STATE_CHECKED);
      if (shell->chamber_light_changed_ != nullptr) {
        shell->chamber_light_changed_(shell->chamber_light_changed_context_, enabled);
      }
    }, LV_EVENT_CLICKED, this);
    create_depth_dots();
    view_ = 23;
    visible_profile_ = profile.id;
  }

  const bool supported = snapshot.job.chamber_light_supported;
  const bool enabled = supported && snapshot.job.chamber_light_on;
  const bool pending = supported && snapshot.job.chamber_light_pending;
  const std::uint32_t light_color = enabled ? theme_style_.accent_secondary : theme_style_.track;
  lv_obj_set_style_bg_color(chamber_light_bulb_,
                            lv_color_hex(enabled ? 0xFDE68A : theme_style_.surface_soft), LV_PART_MAIN);
  lv_obj_set_style_border_color(chamber_light_bulb_, lv_color_hex(light_color), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(chamber_light_bulb_, enabled ? 34 : 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(chamber_light_bulb_, lv_color_hex(theme_style_.accent_secondary), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(chamber_light_bulb_, enabled ? LV_OPA_50 : LV_OPA_TRANSP,
                              LV_PART_MAIN);
  lv_obj_set_style_bg_color(chamber_light_button_,
                            lv_color_hex(supported ? accent_color_ : theme_style_.border), LV_PART_MAIN);
  lv_obj_set_style_border_color(chamber_light_button_,
                                lv_color_hex(supported ? accent_color_ : theme_style_.track), LV_PART_MAIN);
  if (enabled) lv_obj_add_state(chamber_light_button_, LV_STATE_CHECKED);
  else lv_obj_remove_state(chamber_light_button_, LV_STATE_CHECKED);
  if (supported && !pending) lv_obj_remove_state(chamber_light_button_, LV_STATE_DISABLED);
  else lv_obj_add_state(chamber_light_button_, LV_STATE_DISABLED);
  if (pending) {
    lv_obj_remove_flag(chamber_light_spinner_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(detail_label_, tr(snapshot.job.chamber_light_target_on
                                        ? "Turning light on…"
                                        : "Turning light off…"));
    lv_label_set_text(chamber_light_button_label_, tr(snapshot.job.chamber_light_target_on
                                                 ? "TURNING ON"
                                                 : "TURNING OFF"));
    lv_obj_align(chamber_light_button_label_, LV_ALIGN_CENTER, 18, 0);
  } else {
    lv_obj_add_flag(chamber_light_spinner_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(detail_label_, tr(supported ? (enabled ? "Light is on" : "Light is off")
                                               : "Light status unavailable"));
    lv_label_set_text(chamber_light_button_label_, tr(supported ? (enabled ? "TURN OFF" : "TURN ON")
                                                            : "UNAVAILABLE"));
    lv_obj_center(chamber_light_button_label_);
  }
  board_display_unlock();
}

void DisplayShell::show_system_details(const NetworkStatus& network, const PowerSnapshot& power,
                                       std::size_t configured_count) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_system_details(network, power, configured_count);
    return;
  }
  if (board_display_lock(1000) != ESP_OK) return;
  if (view_ != 4) {
    prepare_active_screen("system-details");
    lv_obj_t* screen = lv_screen_active();
    auto label = [&](const char* text, const lv_font_t* font, std::uint32_t color,
                     int width, lv_align_t align, int x, int y) {
      lv_obj_t* object = lv_label_create(screen);
      lv_label_set_text(object, text);
      apply_text_style(object, lv_color_hex(color), font);
      lv_obj_set_width(object, width);
      lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
      lv_obj_align(object, align, x, y);
      lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
      return object;
    };
    label(tr("SYSTEM DETAILS"), &lv_font_montserrat_32, theme_style_.text_primary, 350,
          LV_ALIGN_TOP_MID, 0, 44);
    create_power_header(&power, -208);
    status_label_ = label("", &lv_font_montserrat_16, theme_style_.text_muted, 330,
                          LV_ALIGN_TOP_MID, 0, 127);
    title_label_ = label("", &lv_font_montserrat_24, theme_style_.accent_secondary, 340,
                         LV_ALIGN_TOP_MID, 0, 158);
    lv_obj_set_height(title_label_, 30);
    detail_label_ = label("", &lv_font_montserrat_16, theme_style_.text_muted, 330,
                          LV_ALIGN_TOP_MID, 0, 190);

    const char* captions[4] = {"DEVICE TEMP", "INTERNAL", "PSRAM", "SOUND"};
    const std::uint32_t colors[4] = {theme_colors_.paused, theme_style_.accent_secondary,
                                     theme_style_.accent, theme_colors_.preparing};
    std::array<lv_obj_t*, 4> values{};
    for (int index = 0; index < 4; ++index) {
      const int column = index % 2;
      const int row = index / 2;
      lv_obj_t* card = lv_obj_create(screen);
      lv_obj_set_size(card, 166, 66);
      lv_obj_align(card, LV_ALIGN_TOP_MID, column == 0 ? -86 : 86, 226 + row * 76);
      lv_obj_set_style_radius(card, themed_radius(16), LV_PART_MAIN);
      lv_obj_set_style_bg_color(card, lv_color_hex(theme_style_.surface_raised), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, lv_color_hex(colors[index]), LV_PART_MAIN);
      lv_obj_set_style_border_opa(card, LV_OPA_60, LV_PART_MAIN);
      lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
      apply_surface_effect(card);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);

      lv_obj_t* caption = lv_label_create(card);
      lv_label_set_text(caption, tr(captions[index]));
      apply_text_style(caption, lv_color_hex(colors[index]), &lv_font_montserrat_14);
      lv_obj_set_size(caption, 160, 16);
      lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
      lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 8);
      lv_obj_add_flag(caption, LV_OBJ_FLAG_EVENT_BUBBLE);

      values[index] = lv_label_create(card);
      apply_text_style(values[index], lv_color_hex(theme_style_.text_secondary), &lv_font_montserrat_24);
      lv_obj_set_width(values[index], 160);
      lv_obj_align(values[index], LV_ALIGN_BOTTOM_MID, 0, -6);
      lv_obj_add_flag(values[index], LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    temperature_label_ = values[0];
    metrics_label_ = values[1];
    progress_label_ = values[2];
    active_accent_label_ = values[3];

    version_label_ = label(update_version_text_.c_str(), &lv_font_montserrat_14,
                           update_version_color_, 260, LV_ALIGN_BOTTOM_MID, 0, -47);
    // Match the forgiving Web Config retry target while keeping UPTIME below.
    lv_obj_set_size(version_label_, 320, 40);
    lv_label_set_long_mode(version_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(version_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(version_label_, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_flag(version_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(version_label_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(version_label_, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      if (shell->suppress_update_click_) return;
      shell->handle_update_version_click();
    }, LV_EVENT_CLICKED, this);
    std::string uptime_placeholder = std::string(tr("UPTIME")) + " --";
    lv_obj_t* uptime = label(uptime_placeholder.c_str(), &lv_font_montserrat_14, theme_style_.text_muted, 330,
                             LV_ALIGN_BOTTOM_MID, 0, -23);
    lv_obj_add_flag(uptime, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(uptime, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(uptime, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      if (shell->suppress_update_click_) return;
      shell->handle_update_version_click();
    }, LV_EVENT_CLICKED, this);
    // Reuse a pointer that is otherwise not used by this view.
    clock_date_label_ = uptime;
    create_page_dots();
    view_ = 4;
  }
  const std::size_t internal_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U;
  const float psram_mb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024.0F * 1024.0F);
  update_power_header(power);
  lv_label_set_text(status_label_, tr(power.charging ? "CHARGING" :
                    power.usb_present ? "USB POWER" : power.battery_present ? "BATTERY POWER" :
                    "POWER STATUS UNAVAILABLE"));
  lv_label_set_text_fmt(title_label_, "WI-FI: %s",
                        network.station_name.empty() ? tr("OFFLINE") : network.station_name.c_str());
  lv_label_set_text_fmt(detail_label_, "IP: %s",
                        network.ipv4.empty() ? "--" : network.ipv4.c_str());
  char power_temperature_text[24]{};
  if (power.available) {
    std::snprintf(power_temperature_text, sizeof(power_temperature_text), "%.0f°C",
                  static_cast<double>(power.temperature_c));
  } else {
    std::snprintf(power_temperature_text, sizeof(power_temperature_text), "--");
  }
  lv_label_set_text(temperature_label_, power_temperature_text);
  lv_label_set_text_fmt(metrics_label_, "%u KB", static_cast<unsigned>(internal_kb));
  char psram_text[24]{};
  std::snprintf(psram_text, sizeof(psram_text), "%.1f MB", static_cast<double>(psram_mb));
  lv_label_set_text(progress_label_, psram_text);
  lv_label_set_text_fmt(active_accent_label_, "%d%%", audio_enabled_ ? audio_volume_ : 0);
  const std::uint64_t uptime_seconds = static_cast<std::uint64_t>(esp_timer_get_time()) / 1000000ULL;
  lv_label_set_text_fmt(clock_date_label_, "%s %lluh %02llum", tr("UPTIME"),
                        static_cast<unsigned long long>(uptime_seconds / 3600ULL),
                        static_cast<unsigned long long>((uptime_seconds % 3600ULL) / 60ULL));
  (void)configured_count;
  board_display_unlock();
}

void DisplayShell::show_clock(bool analog, const PowerSnapshot& power) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_clock(analog, power);
    return;
  }
  if (board_display_lock(1000) != ESP_OK) return;
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  const bool time_known = now > 1'700'000'000;
  const int wanted_view = analog ? 6 : 5;
  if (view_ != wanted_view) {
    prepare_active_screen(analog ? "analog-clock" : "digital-clock");
    lv_obj_t* screen = lv_screen_active();
    create_power_header(&power, -208);
    if (analog) {
      lv_obj_t* face = lv_obj_create(screen);
      lv_obj_set_size(face, 316, 316);
      lv_obj_align(face, LV_ALIGN_CENTER, 0, -13);
      lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(face, LV_OBJ_FLAG_EVENT_BUBBLE);
      lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(face, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
      lv_obj_set_style_border_color(face, lv_color_hex(accent_color_), LV_PART_MAIN);
      lv_obj_set_style_border_width(face, 2, LV_PART_MAIN);
      lv_obj_set_style_pad_all(face, 0, LV_PART_MAIN);
      apply_surface_effect(face);
      // A software-blurred shadow around this large circle dominates the
      // first render and makes the 24-row partial flush visibly sweep down the
      // AMOLED. Keep the glass fill and border, but omit that expensive blur.
      lv_obj_set_style_shadow_width(face, 0, LV_PART_MAIN);
      lv_obj_set_style_shadow_opa(face, LV_OPA_TRANSP, LV_PART_MAIN);
      constexpr int center = 156;
      constexpr float pi = 3.14159265358979323846F;
      for (int index = 0; index < 12; ++index) {
        const float angle = (index * 30.0F - 90.0F) * pi / 180.0F;
        const bool cardinal = index % 3 == 0;
        lv_obj_t* tick = lv_obj_create(face);
        lv_obj_set_size(tick, cardinal ? 8 : 5, cardinal ? 8 : 5);
        lv_obj_set_pos(tick, center + static_cast<int>(std::cos(angle) * 135.0F) -
                                 (cardinal ? 4 : 2),
                         center + static_cast<int>(std::sin(angle) * 135.0F) -
                                 (cardinal ? 4 : 2));
        lv_obj_set_style_radius(tick, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(tick,
                                  lv_color_hex(cardinal ? accent_color_ : theme_style_.text_muted),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_width(tick, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tick, 0, LV_PART_MAIN);
        lv_obj_remove_flag(tick, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
      }
      clock_hour_hand_ = lv_line_create(face);
      clock_minute_hand_ = lv_line_create(face);
      clock_second_hand_ = lv_line_create(face);
      for (lv_obj_t* hand : {clock_hour_hand_, clock_minute_hand_, clock_second_hand_}) {
        lv_obj_set_style_line_rounded(hand, true, LV_PART_MAIN);
      }
      lv_obj_set_style_line_width(clock_hour_hand_, 8, LV_PART_MAIN);
      lv_obj_set_style_line_color(clock_hour_hand_, lv_color_hex(theme_style_.text_primary), LV_PART_MAIN);
      lv_obj_set_style_line_width(clock_minute_hand_, 5, LV_PART_MAIN);
      lv_obj_set_style_line_color(clock_minute_hand_, lv_color_hex(accent_color_), LV_PART_MAIN);
      lv_obj_set_style_line_width(clock_second_hand_, 2, LV_PART_MAIN);
      lv_obj_set_style_line_color(clock_second_hand_, lv_color_hex(theme_colors_.error),
                                  LV_PART_MAIN);
      lv_obj_t* center_dot = lv_obj_create(face);
      lv_obj_set_size(center_dot, 13, 13);
      lv_obj_set_pos(center_dot, center - 6, center - 6);
      lv_obj_set_style_radius(center_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(center_dot, lv_color_hex(theme_style_.text_primary), LV_PART_MAIN);
      lv_obj_set_style_border_width(center_dot, 3, LV_PART_MAIN);
      lv_obj_set_style_border_color(center_dot, lv_color_hex(accent_color_), LV_PART_MAIN);
      lv_obj_remove_flag(center_dot, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(center_dot, LV_OBJ_FLAG_CLICKABLE);
      clock_date_label_ = lv_label_create(screen);
      apply_text_style(clock_date_label_, lv_color_hex(theme_style_.text_muted), &lv_font_montserrat_14);
      lv_obj_set_size(clock_date_label_, 370, 24);
      lv_obj_set_style_text_align(clock_date_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(clock_date_label_, LV_ALIGN_BOTTOM_MID, 0, -23);
      lv_obj_add_flag(clock_date_label_, LV_OBJ_FLAG_EVENT_BUBBLE);
    } else {
      constexpr int digit_x[6] = {62, 112, 182, 232, 302, 352};
      constexpr int colon_x[2] = {164, 284};
      constexpr int clock_y = 166;
      constexpr int segment_x[7] = {6, 37, 37, 6, 0, 0, 6};
      constexpr int segment_y[7] = {0, 7, 48, 81, 48, 7, 40};
      constexpr int segment_w[7] = {34, 9, 9, 34, 9, 9, 34};
      constexpr int segment_h[7] = {9, 35, 35, 9, 35, 35, 9};
      for (int digit = 0; digit < 6; ++digit) {
        for (int segment = 0; segment < 7; ++segment) {
          lv_obj_t* bar = lv_obj_create(screen);
          digital_segments_[digit][segment] = bar;
          lv_obj_set_size(bar, segment_w[segment], segment_h[segment]);
          lv_obj_set_pos(bar, digit_x[digit] + segment_x[segment],
                         clock_y + segment_y[segment]);
          lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
          lv_obj_set_style_bg_color(bar, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
          lv_obj_set_style_bg_opa(bar, LV_OPA_70, LV_PART_MAIN);
          lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
          lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
          lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
          lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        }
      }
      for (int colon = 0; colon < 2; ++colon) {
        for (int dot = 0; dot < 2; ++dot) {
          lv_obj_t* marker = lv_obj_create(screen);
          digital_colons_[colon][dot] = marker;
          lv_obj_set_size(marker, 9, 9);
          lv_obj_set_pos(marker, colon_x[colon], clock_y + (dot == 0 ? 26 : 57));
          lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
          lv_obj_set_style_bg_color(marker, lv_color_hex(accent_color_), LV_PART_MAIN);
          lv_obj_set_style_border_width(marker, 0, LV_PART_MAIN);
          lv_obj_set_style_pad_all(marker, 0, LV_PART_MAIN);
          lv_obj_remove_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
          lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE);
        }
      }
      clock_date_label_ = lv_label_create(screen);
      apply_text_style(clock_date_label_, lv_color_hex(theme_style_.accent_secondary), &lv_font_montserrat_24);
      lv_obj_set_width(clock_date_label_, 370);
      lv_obj_set_style_text_line_space(clock_date_label_, 8, LV_PART_MAIN);
      lv_obj_align(clock_date_label_, LV_ALIGN_TOP_MID, 0, 292);
      lv_obj_add_flag(clock_date_label_, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    create_page_dots();
    view_ = wanted_view;
  }
  update_power_header(power);
  static constexpr const char* weekdays[]{"Sunday", "Monday", "Tuesday", "Wednesday",
                                           "Thursday", "Friday", "Saturday"};
  const auto format_date = [&](char* output, std::size_t output_size) {
    const int day = local.tm_mday;
    const int month = local.tm_mon + 1;
    const int year = local.tm_year + 1900;
    const char* separator = analog ? "  " : "\n";
    switch (clock_date_format_.load()) {
      case core::CalendarDateFormat::month_day_year:
        std::snprintf(output, output_size, "%s%s%02d/%02d/%04d",
                      tr(weekdays[local.tm_wday]), separator, month, day, year);
        break;
      case core::CalendarDateFormat::year_month_day:
        std::snprintf(output, output_size, "%s%s%04d-%02d-%02d",
                      tr(weekdays[local.tm_wday]), separator, year, month, day);
        break;
      case core::CalendarDateFormat::day_month_year:
      default:
        std::snprintf(output, output_size, "%s%s%02d.%02d.%04d",
                      tr(weekdays[local.tm_wday]), separator, day, month, year);
        break;
    }
  };
  if (!analog) {
    constexpr std::uint8_t masks[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                        0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    const int digits[6] = {local.tm_hour / 10, local.tm_hour % 10,
                           local.tm_min / 10, local.tm_min % 10,
                           local.tm_sec / 10, local.tm_sec % 10};
    for (int digit = 0; digit < 6; ++digit) {
      for (int segment = 0; segment < 7; ++segment) {
        const bool active = time_known && (masks[digits[digit]] & (1U << segment));
        lv_obj_set_style_bg_color(digital_segments_[digit][segment],
                                  lv_color_hex(active ? accent_color_ : theme_style_.surface_soft),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(digital_segments_[digit][segment],
                                active ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN);
      }
    }
    char date[96]{};
    if (time_known) {
      format_date(date, sizeof(date));
    } else {
      std::snprintf(date, sizeof(date), "%s", tr("Waiting for network time"));
    }
    lv_label_set_text(clock_date_label_, date);
  } else {
    constexpr float kPi = 3.14159265358979323846F;
    const float minute_angle = (static_cast<float>(local.tm_min) + local.tm_sec / 60.0F) * 6.0F;
    const float hour_angle = (static_cast<float>(local.tm_hour % 12) + local.tm_min / 60.0F) * 30.0F;
    const float second_angle = static_cast<float>(local.tm_sec) * 6.0F;
    auto set_hand = [kPi](auto& points, float angle, float length) {
      const float radians = (angle - 90.0F) * kPi / 180.0F;
      points[0] = {156, 156};
      points[1] = {static_cast<lv_value_precise_t>(156 + std::cos(radians) * length),
                   static_cast<lv_value_precise_t>(156 + std::sin(radians) * length)};
    };
    set_hand(hour_points_, time_known ? hour_angle : 0.0F, 75.0F);
    set_hand(minute_points_, time_known ? minute_angle : 0.0F, 108.0F);
    set_hand(second_points_, time_known ? second_angle : 0.0F, 120.0F);
    lv_line_set_points(clock_hour_hand_, hour_points_.data(), hour_points_.size());
    lv_line_set_points(clock_minute_hand_, minute_points_.data(), minute_points_.size());
    lv_line_set_points(clock_second_hand_, second_points_.data(), second_points_.size());
    char date[96]{};
    if (time_known) {
      format_date(date, sizeof(date));
    } else {
      std::snprintf(date, sizeof(date), "%s", tr("TIME NOT SYNCED"));
    }
    lv_label_set_text(clock_date_label_, date);
  }
  board_display_unlock();
}

void DisplayShell::show_web_config(const char* ipv4, const char* local_hostname,
                                   const PowerSnapshot& power) {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_show_web_config(ipv4, local_hostname, power);
    return;
  }
  if (ipv4 == nullptr || board_display_lock(1000) != ESP_OK) return;
  const std::string primary_host = local_hostname != nullptr && local_hostname[0] != '\0'
      ? local_hostname : ipv4;
  if (view_ != 7 || visible_web_config_host_ != primary_host) {
    prepare_active_screen("web-config");
    lv_obj_t* title = lv_label_create(lv_screen_active());
    lv_label_set_text(title, "Web Config");
    apply_text_style(title, lv_color_hex(theme_style_.text_primary), &lv_font_montserrat_32);
    lv_obj_set_width(title, 330);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);
    create_power_header(&power, -208);
    detail_label_ = lv_label_create(lv_screen_active());
    apply_text_style(detail_label_, lv_color_hex(theme_style_.accent_secondary), &lv_font_montserrat_16);
    lv_obj_set_width(detail_label_, 350);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_add_flag(detail_label_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t* qr = lv_qrcode_create(lv_screen_active());
    lv_qrcode_set_size(qr, 154);
    lv_qrcode_set_dark_color(qr, lv_color_hex(theme_style_.background));
    lv_qrcode_set_light_color(qr, lv_color_hex(theme_style_.text_primary));
    lv_qrcode_set_quiet_zone(qr, true);
    const std::string address = std::string("http://") + primary_host;
    lv_qrcode_set_data(qr, address.c_str());
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 142);
    lv_obj_remove_flag(qr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    version_label_ = lv_label_create(lv_screen_active());
    lv_label_set_text(version_label_, update_version_text_.c_str());
    apply_text_style(version_label_, lv_color_hex(update_version_color_),
                     &lv_font_montserrat_14);
    // Keep the visual text compact, but make the entire lower status band an
    // easy touch target.  Retrying must not require tapping individual glyphs.
    lv_obj_set_size(version_label_, 350, 58);
    lv_label_set_long_mode(version_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(version_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(version_label_, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_add_flag(version_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(version_label_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(version_label_, [](lv_event_t* event) {
      auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
      if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      lv_event_stop_bubbling(event);
      if (shell->suppress_update_click_) return;
      shell->handle_update_version_click();
    }, LV_EVENT_CLICKED, this);
    create_page_dots();
    visible_web_config_host_ = primary_host;
    view_ = 7;
  }
  if (detail_label_ != nullptr &&
      std::strcmp(lv_label_get_text(detail_label_), primary_host.c_str()) != 0) {
    lv_label_set_text(detail_label_, primary_host.c_str());
  }
  update_power_header(power);
  board_display_unlock();
}

bool DisplayShell::set_rotation(int degrees) {
  degrees = degrees == 90 ? 90 : degrees == 180 ? 180 : degrees == 270 ? 270 : 0;
  if (degrees == current_rotation_.load()) return true;

  // The AMOLED panel has an asynchronous QSPI queue.  Changing MADCTL while
  // the final color transfer is still in flight can leave the controller and
  // LVGL describing different orientations.  Pause and drain the renderer in
  // the same order as the proven implementation before touching the panel.
  const esp_err_t pause_result = esp_lv_adapter_pause(1000);
  if (pause_result != ESP_OK) {
    ESP_LOGW(kLogTag, "Rotation deferred because LVGL could not pause: %s",
             esp_err_to_name(pause_result));
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(40));

  esp_err_t rotation_result = ESP_ERR_TIMEOUT;
  if (board_display_lock(500) == ESP_OK) {
    rotation_result = board_display_set_rotation(degrees);
    if (rotation_result == ESP_OK) {
      view_ = -1;
      lv_obj_invalidate(lv_screen_active());
    }
    board_display_unlock();
  }
  const esp_err_t resume_result = esp_lv_adapter_resume();
  if (rotation_result != ESP_OK) {
    ESP_LOGW(kLogTag, "Physical display rotation failed: %s",
             esp_err_to_name(rotation_result));
    return false;
  } else if (resume_result != ESP_OK) {
    ESP_LOGE(kLogTag, "LVGL resume after rotation failed: %s",
             esp_err_to_name(resume_result));
    return false;
  } else {
    current_rotation_ = degrees;
    ESP_LOGI(kLogTag, "Physical display rotation applied: %d degrees", degrees);
    return true;
  }
}

esp_err_t DisplayShell::touch_read(esp_lcd_touch_handle_t touch,
                                   esp_lcd_touch_point_data_t* points, uint8_t* count,
                                   uint8_t maximum_count, void* context) {
  auto* shell = static_cast<DisplayShell*>(context);
  if (touch == nullptr || points == nullptr || count == nullptr || shell == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  const int rotation = shell->current_rotation_.load();
  if (shell->touch_rotation_applied_.load() != rotation) {
    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    board_touch_transform(rotation, &swap_xy, &mirror_x, &mirror_y);
    esp_err_t result = esp_lcd_touch_set_swap_xy(touch, swap_xy);
    if (result == ESP_OK) result = esp_lcd_touch_set_mirror_x(touch, mirror_x);
    if (result == ESP_OK) result = esp_lcd_touch_set_mirror_y(touch, mirror_y);
    if (result != ESP_OK) return result;
    shell->touch_rotation_applied_ = rotation;
  }
  const esp_err_t read_result = esp_lcd_touch_read_data(touch);
  if (read_result != ESP_OK) return read_result;
  const esp_err_t data_result =
      esp_lcd_touch_get_data(touch, points, count, maximum_count);
  if (data_result == ESP_OK && *count > 0) {
    shell->defer_background_render(kTouchBackgroundRenderQuietMs);
    shell->note_activity(true);
  }
  return data_result;
}

void DisplayShell::set_brightness(int percent) {
  const int clamped = std::clamp(percent, 5, 100);
  const bool ready = display_ready_.load(std::memory_order_acquire);
  if (ready && board_display_lock(1000) != ESP_OK) {
    ESP_LOGW(kLogTag, "Brightness update deferred because the LVGL lock is busy");
    return;
  }
  applied_brightness_ = clamped;
  if (screen_power_mode_ == 0) board_display_brightness_set(applied_brightness_);
  if (ready) board_display_unlock();
}

void DisplayShell::set_printer_animations_enabled(bool enabled) {
  const bool ready = display_ready_.load(std::memory_order_acquire);
  if (ready && board_display_lock(1000) != ESP_OK) {
    ESP_LOGW(kLogTag, "Printer animation update deferred because the LVGL lock is busy");
    return;
  }
  apply_printer_animations_enabled(enabled);
  if (ready) board_display_unlock();
}

void DisplayShell::set_reaction_progress_visibility(bool bar_enabled,
                                                    bool percent_enabled) {
  const bool ready = display_ready_.load(std::memory_order_acquire);
  if (ready && board_display_lock(1000) != ESP_OK) {
    ESP_LOGW(kLogTag, "Reaction progress update deferred because the LVGL lock is busy");
    return;
  }
  reaction_progress_bar_enabled_ = bar_enabled;
  reaction_progress_percent_enabled_ = percent_enabled;
  apply_reaction_progress_visibility();
  if (ready) board_display_unlock();
}

void DisplayShell::set_reaction_asset_service(ReactionAssetService* service) {
  reaction_assets_ = service;
  printer_animation_asset_generation_ = 0xffffffffU;
}

void DisplayShell::apply_printer_animations_enabled(bool enabled) {
  if (printer_animations_enabled_ == enabled) return;
  const int depth = horizontal_depth_.load();
  const int current_subpage = std::max(0, printer_subpage_.load());
  if (depth == 0) {
    // Opening a printer from My Printers always begins with the optional
    // reactions page.
    printer_subpage_.store(0);
  } else if (enabled) {
    // Existing dashboard pages move one slot to the right when reactions are
    // inserted in front of them. Preserve the page currently being viewed.
    printer_subpage_.store(current_subpage + 1);
  } else {
    // Removing reactions shifts every existing dashboard page back one slot;
    // the reactions page itself falls through to the normal status page.
    printer_subpage_.store(std::max(0, current_subpage - 1));
  }
  printer_animations_enabled_ = enabled;
  printer_subpage_count_.store(enabled ? 5 : 4);
  view_ = -1;
}

void DisplayShell::apply_reaction_progress_visibility() {
  if (view_ != 24) return;
  const auto apply = [](lv_obj_t* object, bool enabled) {
    if (object == nullptr || !lv_obj_is_valid(object)) return;
    if (enabled) {
      lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
  };
  apply(progress_arc_, reaction_progress_bar_enabled_);
  apply(progress_label_, reaction_progress_percent_enabled_);
}

void DisplayShell::focus_printer_reactions_if_dashboard_visible() {
  const bool ready = display_ready_.load(std::memory_order_acquire);
  if (ready && board_display_lock(1000) != ESP_OK) {
    ESP_LOGW(kLogTag, "Printer reaction focus deferred because the LVGL lock is busy");
    return;
  }
  if (printer_animations_enabled_ && horizontal_depth_.load() == 1) {
    printer_subpage_.store(0);
    view_ = -1;
  }
  if (ready) board_display_unlock();
}

void DisplayShell::set_theme(std::string_view theme, const core::ThemeColors& custom) {
  const core::ThemeColors resolved = core::resolved_theme(theme, custom);
  const std::uint32_t next = resolved.printing;
  const bool ready = display_ready_.load(std::memory_order_acquire);
  if (ready && board_display_lock(1000) != ESP_OK) {
    ESP_LOGW(kLogTag, "Theme update deferred because the LVGL lock is busy");
    return;
  }
  active_theme_ = theme;
  custom_theme_colors_ = custom;
  theme_colors_ = resolved;
  theme_style_ = core::resolved_theme_style(theme, resolved);
  accent_color_ = next;
  // Theme fields are read by LVGL callbacks on the other core. Update the
  // complete palette under the display mutex so a new Quick Menu can never
  // observe a half-written std::string/style while the monitor rebuilds.
  view_ = -1;
  if (ready) board_display_unlock();
}

void DisplayShell::set_brightness_changed_callback(BrightnessChanged callback, void* context) {
  brightness_changed_ = callback;
  brightness_changed_context_ = context;
}

void DisplayShell::set_printer_animations_changed_callback(
    PrinterAnimationsChanged callback, void* context) {
  printer_animations_changed_ = callback;
  printer_animations_changed_context_ = context;
}

void DisplayShell::set_power_save_policy(const core::DisplayPowerPolicy& policy) {
  const std::lock_guard<std::mutex> lock(power_policy_mutex_);
  power_policy_ = policy;
}

void DisplayShell::set_audio_state(bool enabled, int volume_percent, std::string_view preset) {
  audio_enabled_ = enabled;
  audio_volume_ = std::clamp(volume_percent, 0, 100);
  if (core::supported_audio_preset(preset)) audio_preset_.assign(preset);
}

void DisplayShell::set_language(std::string_view language) {
  language_.assign(language);
  if (update_state_ == static_cast<int>(FirmwareUpdateState::idle)) {
    update_version_text_ = std::string(tr("Version")) + ": " PRINTDECK_VERSION;
  }
}

void DisplayShell::set_clock_date_format(core::CalendarDateFormat format) {
  clock_date_format_.store(format);
}

const char* DisplayShell::tr(const char* english) const {
  return core::localized_text(language_, english);
}

bool DisplayShell::initialize_localized_fonts() {
  const lv_font_t* base_fonts[]{&lv_font_montserrat_12, &lv_font_montserrat_14,
                                &lv_font_montserrat_16, &lv_font_montserrat_24,
                                &lv_font_montserrat_32};
  const int sizes[]{12, 14, 16, 24, 32};
  const std::size_t latin_size = static_cast<std::size_t>(
      localized_latin_font_end - localized_latin_font_start);
  const std::size_t cjk_size = static_cast<std::size_t>(
      localized_cjk_font_end - localized_cjk_font_start);
  // These fonts are fallbacks for characters Montserrat does not contain. A
  // large cache per fallback and per size quickly consumes the internal heap
  // needed by MQTT/TLS (most cached glyph bitmaps are smaller than the PSRAM
  // allocation threshold). Keep the fallback caches deliberately small; the
  // UI uses only a bounded set of localized strings and can rasterize an
  // evicted glyph again when a screen changes.
  constexpr std::size_t kFallbackGlyphCacheEntries = 16;
  for (std::size_t index = 0; index < localized_latin_fonts_.size(); ++index) {
    localized_latin_fonts_[index] =
        lv_tiny_ttf_create_data_ex(localized_latin_font_start, latin_size, sizes[index],
                                   LV_FONT_KERNING_NONE, kFallbackGlyphCacheEntries);
    localized_cjk_fonts_[index] =
        lv_tiny_ttf_create_data_ex(localized_cjk_font_start, cjk_size, sizes[index],
                                   LV_FONT_KERNING_NONE, kFallbackGlyphCacheEntries);
    if (localized_latin_fonts_[index] == nullptr || localized_cjk_fonts_[index] == nullptr) {
      return false;
    }
    localized_latin_fonts_[index]->fallback = localized_cjk_fonts_[index];
    localized_base_fonts_[index] = *base_fonts[index];
    localized_base_fonts_[index].fallback = localized_latin_fonts_[index];
  }
  const std::size_t terminal_size = static_cast<std::size_t>(
      terminal_font_end - terminal_font_start);
  const int terminal_sizes[]{kDisplayUsesLargeLayout ? 10 : 8, 14, 16};
  const std::size_t terminal_fallback_indices[]{0, 1, 2};
  constexpr std::size_t kTerminalGlyphCacheEntries = 12;
  for (std::size_t index = 0; index < terminal_fonts_.size(); ++index) {
    terminal_fonts_[index] = lv_tiny_ttf_create_data_ex(
        terminal_font_start, terminal_size, terminal_sizes[index],
        LV_FONT_KERNING_NONE, kTerminalGlyphCacheEntries);
    if (terminal_fonts_[index] == nullptr) return false;
    // The terminal subset intentionally contains printable ASCII only. Route
    // LVGL private-use symbols and localized glyphs through the normal font
    // chain instead of displaying missing-glyph boxes.
    terminal_fonts_[index]->fallback =
        &localized_base_fonts_[terminal_fallback_indices[index]];
  }
  return true;
}

const lv_font_t* DisplayShell::localized_font(const lv_font_t* font,
                                              bool terminal_typography) const {
  const lv_font_t* base_fonts[]{&lv_font_montserrat_12, &lv_font_montserrat_14,
                                &lv_font_montserrat_16, &lv_font_montserrat_24,
                                &lv_font_montserrat_32};
  // Retro Terminal previously replaced every requested size with UNSCII 16.
  // Preserve a compact hierarchy without expanding beyond the geometry that
  // each screen was designed for.
  static constexpr std::size_t terminal_font_indices[]{0, 0, 1, 2, 2};
  for (std::size_t index = 0; index < localized_base_fonts_.size(); ++index) {
    if (font == base_fonts[index]) {
      return terminal_typography && theme_style_.terminal_typography
                 ? terminal_fonts_[terminal_font_indices[index]]
                 : &localized_base_fonts_[index];
    }
  }
  return font;
}

int DisplayShell::themed_radius(int preferred) const {
  if (theme_style_.corner_radius == 0) return 0;
  if (theme_style_.corner_radius < 12) {
    return std::min(preferred, static_cast<int>(theme_style_.corner_radius));
  }
  if (theme_style_.corner_radius > 12) {
    return std::min(preferred + 6, static_cast<int>(theme_style_.corner_radius));
  }
  return preferred;
}

void DisplayShell::apply_surface_effect(lv_obj_t* object) const {
  if (object == nullptr || !theme_style_.glass_effect) return;
  lv_obj_set_style_bg_opa(object, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(object, lv_color_hex(theme_style_.surface_soft), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(object, LV_GRAD_DIR_VER, LV_PART_MAIN);
  lv_obj_set_style_border_opa(object, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(object, 14, LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(object, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(object, lv_color_hex(theme_style_.accent), LV_PART_MAIN);
  lv_obj_set_style_shadow_offset_y(object, 5, LV_PART_MAIN);
}

lv_obj_t* DisplayShell::create_printer_animation_icon(
    lv_obj_t* parent, int size, std::uint32_t color) const {
  lv_obj_t* icon = lv_obj_create(parent);
  lv_obj_set_size(icon, size, size);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(icon, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);

  lv_obj_t* stem = lv_obj_create(icon);
  lv_obj_set_size(stem, std::max(4, size / 3), std::max(3, size / 6));
  lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_remove_flag(stem, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(stem, std::max(1, size / 12), LV_PART_MAIN);
  lv_obj_set_style_bg_color(stem, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_border_width(stem, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(stem, 0, LV_PART_MAIN);

  lv_obj_t* head = lv_obj_create(icon);
  lv_obj_set_size(head, size * 4 / 5, size / 2);
  lv_obj_align(head, LV_ALIGN_TOP_MID, 0, size / 7);
  lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(head, std::max(3, size / 6), LV_PART_MAIN);
  lv_obj_set_style_bg_color(head, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_border_width(head, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(head, 0, LV_PART_MAIN);

  const int eye_size = std::max(2, size / 9);
  for (int direction : {-1, 1}) {
    lv_obj_t* eye = lv_obj_create(head);
    lv_obj_set_size(eye, eye_size, eye_size);
    lv_obj_remove_flag(eye, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(eye, lv_color_hex(theme_style_.background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(eye, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(eye, 0, LV_PART_MAIN);
    lv_obj_align(eye, LV_ALIGN_CENTER, direction * size / 5, -size / 24);
  }

  lv_obj_t* nozzle = lv_obj_create(icon);
  lv_obj_set_size(nozzle, std::max(5, size / 3), std::max(3, size / 6));
  lv_obj_align(nozzle, LV_ALIGN_TOP_MID, 0, size * 4 / 7);
  lv_obj_remove_flag(nozzle, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(nozzle, std::max(1, size / 16), LV_PART_MAIN);
  lv_obj_set_style_bg_color(nozzle, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_border_width(nozzle, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(nozzle, 0, LV_PART_MAIN);

  for (int direction : {-1, 0, 1}) {
    lv_obj_t* heat = lv_obj_create(icon);
    lv_obj_set_size(heat, std::max(1, size / 14), std::max(3, size / 7));
    lv_obj_align(heat, LV_ALIGN_BOTTOM_MID, direction * size / 4, 0);
    lv_obj_remove_flag(heat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(heat, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(heat, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_width(heat, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(heat, 0, LV_PART_MAIN);
  }
  return icon;
}

void DisplayShell::apply_text_style(lv_obj_t* label, lv_color_t color,
                                    const lv_font_t* font) const {
  lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
  lv_obj_set_style_text_font(label, localized_font(font), LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

void DisplayShell::apply_icon_text_style(lv_obj_t* label, lv_color_t color,
                                         const lv_font_t* font) const {
  lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
  lv_obj_set_style_text_font(label, localized_font(font, false), LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

void DisplayShell::set_audio_changed_callback(AudioChanged callback, void* context) {
  audio_changed_ = callback;
  audio_changed_context_ = context;
}

void DisplayShell::set_audio_preset_changed_callback(AudioPresetChanged callback, void* context) {
  audio_preset_changed_ = callback;
  audio_preset_changed_context_ = context;
}

void DisplayShell::set_theme_changed_callback(ThemeChanged callback, void* context) {
  theme_changed_ = callback;
  theme_changed_context_ = context;
}

void DisplayShell::set_language_changed_callback(LanguageChanged callback, void* context) {
  language_changed_ = callback;
  language_changed_context_ = context;
}

void DisplayShell::set_printer_selected_callback(PrinterSelected callback, void* context) {
  printer_selected_ = callback;
  printer_selected_context_ = context;
}

void DisplayShell::set_navigation_feedback_callback(NavigationFeedback callback, void* context) {
  navigation_feedback_ = callback;
  navigation_feedback_context_ = context;
}

void DisplayShell::set_page_refresh_callback(PageRefreshRequested callback, void* context) {
  page_refresh_requested_ = callback;
  page_refresh_context_ = context;
}

void DisplayShell::set_chamber_light_changed_callback(ChamberLightChanged callback,
                                                      void* context) {
  chamber_light_changed_ = callback;
  chamber_light_changed_context_ = context;
}

void DisplayShell::set_camera_mode_changed_callback(CameraModeChanged callback,
                                                    void* context) {
  camera_mode_changed_ = callback;
  camera_mode_changed_context_ = context;
}

void DisplayShell::set_camera_preferences(bool live, int snapshot_fps) {
  camera_live_mode_.store(live);
  camera_snapshot_fps_.store(std::clamp(snapshot_fps, 1, 5));
}

void DisplayShell::camera_mode_event(lv_event_t* event) {
  auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
  if (shell == nullptr) return;
  const bool live = lv_event_get_target_obj(event) == shell->camera_live_button_;
  if (live == shell->camera_live_mode_.load()) return;
  shell->note_activity(true);
  shell->camera_live_mode_.store(live);
  shell->view_ = -1;
  if (shell->camera_mode_changed_ != nullptr) {
    shell->camera_mode_changed_(shell->camera_mode_changed_context_, live);
  }
}

void DisplayShell::set_update_check_callback(UpdateCheckRequested callback, void* context) {
  update_check_requested_ = callback;
  update_check_context_ = context;
}

void DisplayShell::set_update_install_callback(UpdateInstallRequested callback, void* context) {
  update_install_requested_ = callback;
  update_install_context_ = context;
}

void DisplayShell::handle_update_version_click() {
  note_activity(true);
  if (update_busy_) return;
  update_overlay_manually_opened_ = true;
  if (update_available_) {
    set_capture_overlay_name("update-available");
    ensure_update_overlay();
    // An automatic background check may have populated the cached snapshot
    // before the overlay existed.  Fill it here as well so a later tap never
    // opens an empty decision screen merely because the snapshot is unchanged.
    lv_label_set_text(update_overlay_title_, tr("UPDATE AVAILABLE"));
    lv_obj_set_style_text_color(update_overlay_title_, lv_color_hex(theme_style_.accent),
                                LV_PART_MAIN);
    lv_label_set_text_fmt(update_overlay_versions_, "%s: %s\n%s: %s", tr("New"),
                              update_latest_version_.empty()
                                  ? "--" : update_latest_version_.c_str(),
                              tr("Installed"), PRINTDECK_VERSION);
    lv_label_set_text(update_overlay_detail_, tr(
        "Install the new firmware securely over Wi-Fi.\n"
        "Keep PrintDeck powered until it restarts."));
    lv_obj_add_flag(update_overlay_progress_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(update_overlay_progress_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(update_install_button_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(update_dismiss_button_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(update_install_button_label_, tr("UPDATE NOW"));
    lv_obj_remove_flag(update_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(update_overlay_);
    return;
  }
  if (update_check_requested_ != nullptr) {
    update_check_requested_(update_check_context_);
  }
}

void DisplayShell::hide_update_overlay() {
  update_overlay_manually_opened_ = false;
  if (update_overlay_ != nullptr && lv_obj_is_valid(update_overlay_)) {
    lv_obj_add_flag(update_overlay_, LV_OBJ_FLAG_HIDDEN);
  }
  if (capture_overlay_name_.rfind("update-", 0) == 0) capture_overlay_name_.clear();
}

void DisplayShell::ensure_update_overlay() {
  if constexpr (!kDisplayUsesLargeLayout) {
    square_ensure_update_overlay();
    return;
  }
  if (update_overlay_ != nullptr && lv_obj_is_valid(update_overlay_)) return;
  update_overlay_ = lv_obj_create(lv_layer_top());
  lv_obj_set_size(update_overlay_, 466, 466);
  lv_obj_center(update_overlay_);
  lv_obj_remove_flag(update_overlay_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(update_overlay_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(update_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(update_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(update_overlay_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(update_overlay_, lv_color_hex(theme_style_.background), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(update_overlay_, LV_GRAD_DIR_NONE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(update_overlay_, LV_OPA_COVER, LV_PART_MAIN);

  update_overlay_title_ = lv_label_create(update_overlay_);
  apply_text_style(update_overlay_title_, lv_color_hex(theme_style_.accent),
                   &lv_font_montserrat_24);
  lv_obj_set_width(update_overlay_title_, 330);
  lv_obj_set_style_text_align(update_overlay_title_, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_align(update_overlay_title_, LV_ALIGN_TOP_MID, 0, 72);

  update_overlay_versions_ = lv_label_create(update_overlay_);
  apply_text_style(update_overlay_versions_, lv_color_hex(theme_style_.text_primary),
                   &lv_font_montserrat_24);
  lv_obj_set_width(update_overlay_versions_, 370);
  lv_obj_set_style_text_line_space(update_overlay_versions_, 9, LV_PART_MAIN);
  lv_obj_align(update_overlay_versions_, LV_ALIGN_TOP_MID, 0, 116);

  update_overlay_detail_ = lv_label_create(update_overlay_);
  apply_text_style(update_overlay_detail_, lv_color_hex(theme_style_.text_muted),
                   &lv_font_montserrat_16);
  lv_obj_set_width(update_overlay_detail_, 360);
  lv_label_set_long_mode(update_overlay_detail_, LV_LABEL_LONG_WRAP);
  lv_obj_align(update_overlay_detail_, LV_ALIGN_TOP_MID, 0, 202);

  update_overlay_progress_ = lv_label_create(update_overlay_);
  apply_text_style(update_overlay_progress_, lv_color_hex(theme_style_.accent_secondary),
                   &lv_font_montserrat_16);
  lv_obj_set_width(update_overlay_progress_, 350);
  lv_obj_align(update_overlay_progress_, LV_ALIGN_TOP_MID, 0, 267);

  update_overlay_progress_bar_ = lv_bar_create(update_overlay_);
  lv_obj_set_size(update_overlay_progress_bar_, 330, 12);
  lv_obj_align(update_overlay_progress_bar_, LV_ALIGN_TOP_MID, 0, 302);
  lv_bar_set_range(update_overlay_progress_bar_, 0, 100);
  lv_obj_set_style_radius(update_overlay_progress_bar_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(update_overlay_progress_bar_, lv_color_hex(theme_style_.surface_soft),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(update_overlay_progress_bar_, lv_color_hex(theme_style_.accent_secondary),
                            LV_PART_INDICATOR);

  auto make_button = [&](int x, std::uint32_t color, const char* text,
                         lv_obj_t** label_out) {
    lv_obj_t* button = lv_button_create(update_overlay_);
    lv_obj_set_size(button, 136, 54);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, x, -79);
    lv_obj_set_style_radius(button, themed_radius(18), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    apply_text_style(label, lv_color_hex(x < 0 ? theme_style_.on_accent : theme_style_.text_primary),
                     &lv_font_montserrat_16);
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (label_out != nullptr) *label_out = label;
    return button;
  };
  update_install_button_ = make_button(-74, theme_style_.accent, tr("UPDATE NOW"),
                                       &update_install_button_label_);
  update_dismiss_button_ = make_button(74, theme_style_.surface_soft, tr("NOT NOW"), nullptr);
  lv_obj_add_event_cb(update_install_button_, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_event_stop_bubbling(event);
    shell->note_activity(true);
    shell->update_overlay_manually_opened_ = true;
    if (shell->update_install_requested_ != nullptr) {
      shell->update_install_requested_(shell->update_install_context_);
    }
  }, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(update_dismiss_button_, [](lv_event_t* event) {
    auto* shell = static_cast<DisplayShell*>(lv_event_get_user_data(event));
    if (shell == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_event_stop_bubbling(event);
    shell->note_activity(true);
    shell->hide_update_overlay();
  }, LV_EVENT_CLICKED, this);
  lv_obj_add_flag(update_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayShell::set_update_snapshot(const FirmwareUpdateSnapshot& update) {
  // The immediate LVGL callback normally handles a failed full-screen draw.
  // If LVGL could not allocate that callback under pressure, this existing
  // once-per-second monitor path performs the same rollback without allocating
  // any additional recovery object.
  if (draw_recovery_pending_.load(std::memory_order_acquire) &&
      board_display_lock(250) == ESP_OK) {
    if (draw_recovery_pending_.exchange(false, std::memory_order_acq_rel)) {
      recover_failed_draw_locked();
    }
    board_display_unlock();
  }
  std::string text = std::string(tr("Version")) + ": " PRINTDECK_VERSION;
  std::uint32_t color = theme_style_.text_muted;
  switch (update.state) {
    case FirmwareUpdateState::checking:
      text = tr("Checking for updates...");
      color = theme_style_.accent_secondary;
      break;
    case FirmwareUpdateState::available:
      text = update.latest_version.empty()
                 ? tr("Update available")
                 : std::string(tr("Version")) + ": " PRINTDECK_VERSION "  |  " +
                       update.latest_version + " " + tr("available");
      color = theme_style_.accent;
      break;
    case FirmwareUpdateState::failed:
      text = tr("Update check failed\nTap to retry");
      color = theme_colors_.error;
      break;
    case FirmwareUpdateState::current:
      text = std::string(tr("Version")) + ": " PRINTDECK_VERSION "\n" + tr("Up to date");
      color = theme_style_.text_muted;
      break;
    case FirmwareUpdateState::unavailable:
      text = std::string(tr("Version")) + ": " PRINTDECK_VERSION "\n" +
             tr("No update for this hardware");
      color = theme_style_.text_muted;
      break;
    case FirmwareUpdateState::downloading:
      text = tr("Installing update...");
      color = theme_style_.accent_secondary;
      break;
    case FirmwareUpdateState::rebooting:
      text = tr("Restarting...");
      color = theme_style_.accent;
      break;
    case FirmwareUpdateState::idle:
      break;
  }
  const bool changed = text != update_version_text_ || color != update_version_color_ ||
      update_latest_version_ != update.latest_version || update_detail_ != update.detail ||
      update_progress_percent_ != update.progress_percent ||
      update_state_ != static_cast<int>(update.state) ||
      update_available_ != update.update_available || update_busy_ != update.busy;
  if (!changed) return;
  update_version_text_ = std::move(text);
  update_version_color_ = color;
  update_latest_version_ = update.latest_version;
  update_detail_ = update.detail;
  update_progress_percent_ = update.progress_percent;
  update_state_ = static_cast<int>(update.state);
  update_available_ = update.update_available;
  update_busy_ = update.busy;
  if (board_display_lock(250) != ESP_OK) return;
  if (version_label_ != nullptr && lv_obj_is_valid(version_label_)) {
    if constexpr (kDisplayUsesLargeLayout) {
      lv_label_set_text(version_label_, update_version_text_.c_str());
    } else {
      std::string compact_text = update_version_text_;
      std::size_t newline = 0;
      while ((newline = compact_text.find('\n', newline)) != std::string::npos) {
        compact_text.replace(newline, 1, " - ");
        newline += 3;
      }
      lv_label_set_text(version_label_, compact_text.c_str());
    }
    lv_obj_set_style_text_color(version_label_, lv_color_hex(update_version_color_), LV_PART_MAIN);
  }
  if constexpr (kDisplayUsesCompactRoundLayout) {
    if (view_ == 4 && clock_date_label_ != nullptr &&
        lv_obj_is_valid(clock_date_label_)) {
      const bool hide_uptime = update.state == FirmwareUpdateState::failed ||
                               update.state == FirmwareUpdateState::current ||
                               update.state == FirmwareUpdateState::unavailable;
      if (hide_uptime) {
        lv_obj_add_flag(clock_date_label_, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_remove_flag(clock_date_label_, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
  const bool installing = update.state == FirmwareUpdateState::downloading ||
                          update.state == FirmwareUpdateState::rebooting;
  const bool failed_install = update.state == FirmwareUpdateState::failed &&
                              update.update_available && update_overlay_manually_opened_;
  const bool show_overlay = installing || failed_install ||
      (update_overlay_manually_opened_ && update.state == FirmwareUpdateState::available);
  if (show_overlay) {
    if (update.state == FirmwareUpdateState::failed) {
      set_capture_overlay_name("update-failed");
    } else if (update.state == FirmwareUpdateState::rebooting) {
      set_capture_overlay_name("update-restarting");
    } else if (update.state == FirmwareUpdateState::downloading) {
      set_capture_overlay_name("update-downloading");
    } else {
      set_capture_overlay_name("update-available");
    }
    ensure_update_overlay();
    lv_obj_remove_flag(update_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(update_overlay_);
    const bool failed = update.state == FirmwareUpdateState::failed;
    lv_label_set_text(update_overlay_title_, failed ? tr("UPDATE FAILED") :
                      installing ? tr("INSTALLING UPDATE") : tr("UPDATE AVAILABLE"));
    lv_obj_set_style_text_color(update_overlay_title_,
        lv_color_hex(failed ? theme_colors_.error : theme_style_.accent), LV_PART_MAIN);
    lv_label_set_text_fmt(update_overlay_versions_, "%s: %s\n%s: %s", tr("New"),
                          update.latest_version.empty() ? "--" : update.latest_version.c_str(),
                          tr("Installed"), update.current_version.c_str());
    lv_label_set_text(update_overlay_detail_, tr(
        installing || failed ? update.detail.c_str() :
        "Install the new firmware securely over Wi-Fi.\n"
        "Keep PrintDeck powered until it restarts."));
    if (update.state == FirmwareUpdateState::rebooting) {
      lv_label_set_text(update_overlay_progress_, tr("Restarting..."));
    } else {
      lv_label_set_text_fmt(update_overlay_progress_, "%s: %d%%", tr("Downloading"),
                            update.progress_percent);
    }
    lv_bar_set_value(update_overlay_progress_bar_, update.progress_percent, LV_ANIM_OFF);
    if (installing) {
      lv_obj_remove_flag(update_overlay_progress_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(update_overlay_progress_bar_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(update_install_button_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(update_dismiss_button_, LV_OBJ_FLAG_HIDDEN);
      note_activity(true);
    } else {
      lv_obj_add_flag(update_overlay_progress_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(update_overlay_progress_bar_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(update_install_button_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(update_dismiss_button_, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(update_install_button_label_, failed ? tr("RETRY") : tr("UPDATE NOW"));
    }
  } else if (update_overlay_ != nullptr && lv_obj_is_valid(update_overlay_)) {
    lv_obj_add_flag(update_overlay_, LV_OBJ_FLAG_HIDDEN);
    if (capture_overlay_name_.rfind("update-", 0) == 0) capture_overlay_name_.clear();
  }
  board_display_unlock();
}

void DisplayShell::note_activity(bool wake) {
  last_activity_ms_ = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  if (wake) request_wake();
}

void DisplayShell::defer_background_render(std::uint32_t milliseconds) {
  const std::int64_t target =
      esp_timer_get_time() + static_cast<std::int64_t>(milliseconds) * 1000;
  std::int64_t current =
      background_render_quiet_until_us_.load(std::memory_order_relaxed);
  while (current < target &&
         !background_render_quiet_until_us_.compare_exchange_weak(
             current, target, std::memory_order_release,
             std::memory_order_relaxed)) {
  }
}

void DisplayShell::request_wake() {
  if (screen_power_mode_ == 0) return;
  if (board_display_lock(500) != ESP_OK) {
    ESP_LOGW(kLogTag, "Display wake deferred because the LVGL lock is busy");
    return;
  }
  screen_power_mode_ = 0;
  board_display_brightness_set(applied_brightness_);
  board_display_unlock();
}

void DisplayShell::reset_inactivity_and_wake() {
  last_activity_ms_ = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  request_wake();
}

void DisplayShell::update_power_save(bool on_battery, bool keep_awake, bool print_active) {
  const std::uint64_t now = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
  core::DisplayPowerPolicy policy;
  {
    const std::lock_guard<std::mutex> lock(power_policy_mutex_);
    policy = power_policy_;
  }
  if (!power_source_known_) {
    power_source_known_ = true;
    last_on_battery_ = on_battery;
  } else if (on_battery && !last_on_battery_) {
    last_activity_ms_ = now;
    request_wake();
  }
  last_on_battery_ = on_battery;
  if (screen_power_mode_ == 2 && board_touch_interrupt_active()) {
    note_activity(true);
    return;
  }
  if (keep_awake || (!on_battery && !policy.usb_power_save_enabled)) {
    request_wake();
    return;
  }
  const std::uint64_t idle = now - last_activity_ms_;
  const std::uint64_t dim_at = 1000ULL * (print_active
      ? policy.dim_timeout_active_s : policy.dim_timeout_idle_s);
  std::uint64_t off_at = 1000ULL * (print_active
      ? policy.off_timeout_active_s : policy.off_timeout_idle_s);
  if (policy.dim_enabled && policy.screen_off_enabled && off_at <= dim_at) {
    off_at = dim_at + kMinimumVisibleDimStageMs;
  }
  int target = 0;
  if (policy.screen_off_enabled && idle >= off_at) target = 2;
  else if (policy.dim_enabled && idle >= dim_at) target = 1;
  if (target == screen_power_mode_) return;
  const int previous = screen_power_mode_;
  if (previous != 2 && esp_lv_adapter_pause(1000) != ESP_OK) {
    last_activity_ms_ = now;
    return;
  }
  screen_power_mode_ = target;
  const int dim_brightness = policy.dim_brightness_percent == 0
      ? std::max(8, std::min(18, applied_brightness_ / 3))
      : policy.dim_brightness_percent;
  board_display_brightness_set(target == 2 ? 0 : target == 1
      ? std::min(applied_brightness_, dim_brightness) : applied_brightness_);
  // Keep LVGL and its touch reader running even at zero brightness. Otherwise
  // a tap or swipe cannot be observed while the OLED is fully dark.
  esp_lv_adapter_resume();
  ESP_LOGI(kLogTag, "Display power mode %d -> %d", previous, target);
}

esp_err_t DisplayShell::capture_png(std::vector<std::uint8_t>& png,
                                    std::string& screen_name) const {
  png.clear();
  screen_name.clear();
  if (board_display_lock(2000) != ESP_OK) return ESP_ERR_TIMEOUT;

  screen_name = capture_overlay_name_.empty() ? capture_screen_name_
                                              : capture_overlay_name_;

  lv_display_t* display = lv_display_get_default();
  const int width = display == nullptr ? 0 : lv_display_get_horizontal_resolution(display);
  const int height = display == nullptr ? 0 : lv_display_get_vertical_resolution(display);
  bool captured = width > 0 && height > 0;

  lv_draw_buf_t* screen = captured
      ? take_transparent_snapshot(lv_display_get_screen_active(display)) : nullptr;
  captured = captured && screen != nullptr && screen->data != nullptr &&
             screen->header.w == width && screen->header.h == height &&
             screen->header.stride >= width * 4;

  if (captured) {
    lv_draw_buf_t* top = take_transparent_snapshot(lv_display_get_layer_top(display));
    composite_snapshot_bgra(screen, top, width, height);
    if (top != nullptr) lv_draw_buf_destroy(top);

    lv_draw_buf_t* system = take_transparent_snapshot(lv_display_get_layer_sys(display));
    composite_snapshot_bgra(screen, system, width, height);
    if (system != nullptr) lv_draw_buf_destroy(system);
  }
  board_display_unlock();
  if (!captured || screen_name.empty()) {
    if (screen != nullptr) lv_draw_buf_destroy(screen);
    return ESP_FAIL;
  }

  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = static_cast<png_uint_32>(width);
  image.height = static_cast<png_uint_32>(height);
  image.format = PNG_FORMAT_BGRA;
  png_alloc_size_t encoded_size = 0;
  const png_int_32 stride = static_cast<png_int_32>(screen->header.stride);
  if (!png_image_write_to_memory(&image, nullptr, &encoded_size, 0,
                                 screen->data, stride, nullptr) || encoded_size == 0) {
    png_image_free(&image);
    lv_draw_buf_destroy(screen);
    return ESP_FAIL;
  }
  png.resize(encoded_size);
  image = {};
  image.version = PNG_IMAGE_VERSION;
  image.width = static_cast<png_uint_32>(width);
  image.height = static_cast<png_uint_32>(height);
  image.format = PNG_FORMAT_BGRA;
  if (!png_image_write_to_memory(&image, png.data(), &encoded_size, 0,
                                 screen->data, stride, nullptr)) {
    png.clear();
    png_image_free(&image);
    lv_draw_buf_destroy(screen);
    return ESP_FAIL;
  }
  png.resize(encoded_size);
  png_image_free(&image);
  lv_draw_buf_destroy(screen);
  return ESP_OK;
}

}  // namespace printdeck::platform
