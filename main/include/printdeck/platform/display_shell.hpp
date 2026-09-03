#pragma once

#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"
#include "printdeck/core/device_state.hpp"
#include "printdeck/core/configuration_backup.hpp"
#include "printdeck/core/settings.hpp"
#include "printdeck/core/theme.hpp"
#include "printdeck/core/timezone.hpp"
#include "printdeck/platform/inactive_printer_poller.hpp"

namespace printdeck::platform {

struct NetworkStatus;
struct PowerSnapshot;
struct FirmwareUpdateSnapshot;
class ReactionAssetService;

class DisplayShell {
 public:
  using BrightnessChanged = void (*)(void* context, int percent);
  using AudioChanged = void (*)(void* context, bool enabled, int volume_percent);
  using AudioPresetChanged = void (*)(void* context, const char* preset);
  using ThemeChanged = void (*)(void* context, const char* theme);
  using LanguageChanged = void (*)(void* context, const char* language);
  using PrinterAnimationsChanged = void (*)(void* context, bool enabled);
  using PrinterSelected = bool (*)(void* context, std::uint32_t profile_id);
  using NavigationFeedback = void (*)(void* context);
  using PageRefreshRequested = void (*)(void* context);
  using ChamberLightChanged = void (*)(void* context, bool enabled);
  using CameraModeChanged = void (*)(void* context, bool live);
  using UpdateCheckRequested = void (*)(void* context);
  using UpdateInstallRequested = void (*)(void* context);

  esp_err_t start(int initial_rotation_degrees = 0);
  void show_boot_status(const char* text);
  void show_wifi_error(const char* network_name);
  void show_wifi_setup(const char* network_name, const char* local_hostname);
  void show_my_printers(const char* ipv4, const char* local_hostname,
                        const std::vector<core::PrinterProfile>& profiles,
                        std::uint32_t selected_profile,
                        const InactivePrinterSnapshot& inactive,
                        const PowerSnapshot& power,
                        const core::PrinterSnapshot* selected_snapshot = nullptr);
  void show_printer(const core::PrinterProfile& profile,
                    const core::PrinterSnapshot& snapshot, const PowerSnapshot& power,
                    const char* ipv4);
  void show_system_details(const NetworkStatus& network, const PowerSnapshot& power,
                           std::size_t configured_count);
  void show_clock(bool analog, const PowerSnapshot& power);
  void show_web_config(const char* ipv4, const char* local_hostname,
                       const PowerSnapshot& power);
  int page() const { return page_.load(); }
  bool printer_list_visible() const { return horizontal_depth_.load() == 0; }
  void return_to_printer_list();
  void open_printer_when_ready(std::uint32_t profile_id);
  bool camera_page_active() const;
  // Core-0 background rendering waits for this short, touch-driven quiet
  // window instead of contending with the core-1 LVGL event handler. The
  // newest runtime snapshot is rendered immediately after the window closes.
  std::uint32_t background_render_delay_ms() const;
  void release_camera_frame();
  void release_printer_preview();
  bool screen_fully_off() const { return screen_power_mode_.load() == 2; }
  bool set_rotation(int degrees);
  void set_brightness(int percent);
  void set_printer_animations_enabled(bool enabled);
  void set_reaction_progress_visibility(bool bar_enabled, bool percent_enabled);
  void set_reaction_asset_service(ReactionAssetService* service);
  void focus_printer_reactions_if_dashboard_visible();
  void set_power_save_policy(const core::DisplayPowerPolicy& policy);
  void set_theme(std::string_view theme, const core::ThemeColors& custom);
  void set_brightness_changed_callback(BrightnessChanged callback, void* context);
  void set_audio_state(bool enabled, int volume_percent, std::string_view preset);
  void set_language(std::string_view language);
  void set_clock_date_format(core::CalendarDateFormat format);
  void set_audio_changed_callback(AudioChanged callback, void* context);
  void set_audio_preset_changed_callback(AudioPresetChanged callback, void* context);
  void set_theme_changed_callback(ThemeChanged callback, void* context);
  void set_language_changed_callback(LanguageChanged callback, void* context);
  void set_printer_animations_changed_callback(PrinterAnimationsChanged callback,
                                                void* context);
  void set_printer_selected_callback(PrinterSelected callback, void* context);
  void set_navigation_feedback_callback(NavigationFeedback callback, void* context);
  void set_page_refresh_callback(PageRefreshRequested callback, void* context);
  void set_chamber_light_changed_callback(ChamberLightChanged callback, void* context);
  void set_camera_mode_changed_callback(CameraModeChanged callback, void* context);
  void set_camera_preferences(bool live, int snapshot_fps);
  void set_update_check_callback(UpdateCheckRequested callback, void* context);
  void set_update_install_callback(UpdateInstallRequested callback, void* context);
  void set_update_snapshot(const FirmwareUpdateSnapshot& update);
  void set_configuration_backup_activity(core::ConfigurationBackupActivity activity);
  void finish_horizontal_transition(int rendered_page, bool rendered_printer_list,
                                    std::uint32_t rendered_profile_id);
  void update_power_save(bool on_battery, bool keep_awake, bool print_active);
  void request_wake();
  void reset_inactivity_and_wake();
  void show_shutdown_countdown(int seconds);
  void cancel_shutdown_countdown();
  void show_shutdown_screen();
  // Capture the composed LVGL display, including Quick Menu and system
  // overlays, for USB developer capture and Web Config Live View.
  esp_err_t capture_png(std::vector<std::uint8_t>& png, std::string& screen_name) const;
  // Queue one bounded pointer gesture for the normal LVGL input path. Coordinates
  // use the current framebuffer space; only one remote gesture may run at once.
  esp_err_t queue_remote_input(int start_x, int start_y, int end_x, int end_y,
                               std::uint32_t duration_ms);
  // Select a stable documentable view without synthesizing touch input. This
  // is intentionally exposed only to the short-lived physical-USB developer
  // session; normal product navigation continues to use touch gestures.
  esp_err_t navigate_for_capture(std::string_view screen_name);

 private:
  static void screen_event(lv_event_t* event);
  static void wifi_setup_pager_event(lv_event_t* event);
  static void wifi_setup_navigation_event(lv_event_t* event);
  static void wifi_setup_language_event(lv_event_t* event);
  static void nozzle_scroll_event(lv_event_t* event);
  static void printer_list_scroll_event(lv_event_t* event);
  static void printer_retry_wait_finished(lv_timer_t* timer);
  static void camera_mode_event(lv_event_t* event);
  static void horizontal_transition_finished(lv_anim_t* animation);
  static void horizontal_transition_switch(lv_timer_t* timer);
  static void horizontal_transition_timeout(lv_timer_t* timer);
  static void quick_menu_action_async(void* context);
  static void display_draw_failed(void* context);
  static void display_draw_recovery_async(void* context);
  static void theme_selection_timer(lv_timer_t* timer);
  static void printer_animation_tick(lv_timer_t* timer);
  static void printer_animation_source_async(void* context);
  static esp_err_t touch_read(esp_lcd_touch_handle_t touch,
                              esp_lcd_touch_point_data_t* points, uint8_t* count,
                              uint8_t maximum_count, void* context);
  void defer_background_render(std::uint32_t milliseconds);
  void note_activity(bool wake);
  void create_wifi_setup_navigation(lv_obj_t* screen);
  void show_wifi_setup_language_picker();
  void start_horizontal_transition(int target_page, bool show_printer_list,
                                   int direction, std::uint32_t target_profile_id = 0,
                                   int target_printer_subpage = 0);
  void cancel_horizontal_transition_locked(bool stop_reveal_animation = true);
  bool horizontal_destination(bool forward, int* target_page,
                              bool* target_printer_list,
                              int* target_printer_subpage) const;
  void show_quick_menu();
  void show_brightness_overlay();
  void show_audio_overlay();
  void show_theme_overlay();
  void create_quick_overlay_close_button();
  void request_theme_selection(const char* theme);
  void close_quick_overlay();
  void recover_failed_draw_locked();
  void handle_update_version_click();
  void ensure_update_overlay();
  void hide_update_overlay();
  // Every independently documentable screen must provide its stable slug here.
  // The USB screenshot protocol exports this value, so adding a renderer without
  // naming it is intentionally a compile-time error at the call site.
  void prepare_active_screen(const char* screen_name);
  void set_capture_overlay_name(const char* screen_name);
  void clear_capture_overlay_name(const char* expected_screen_name);
  void create_page_header(const char* title);
  void configure_printer_list_scroll(lv_obj_t* list, std::size_t printer_count,
                                     std::size_t visible_count, int item_pitch);
  void update_printer_list_scroll_position();
  void activate_printer_card(lv_obj_t* card);
  const char* tr(const char* english) const;
  void apply_text_style(lv_obj_t* label, lv_color_t color, const lv_font_t* font) const;
  void apply_icon_text_style(lv_obj_t* label, lv_color_t color,
                             const lv_font_t* font) const;
  const lv_font_t* localized_font(const lv_font_t* font,
                                  bool terminal_typography = true) const;
  int themed_radius(int preferred) const;
  void apply_surface_effect(lv_obj_t* object) const;
  lv_obj_t* create_printer_animation_icon(lv_obj_t* parent, int size,
                                          std::uint32_t color) const;
  bool initialize_localized_fonts();
  void create_page_dots(int right_offset = 18);
  void create_printer_view_dots(int right_offset = 39);
  void create_depth_dots(int bottom_offset = 31);
  void create_power_header(const PowerSnapshot* power, int center_y);
  void update_power_header(const PowerSnapshot& power);
  static std::string effective_brand(const core::PrinterProfile& profile);
  static const char* brand_mark(const core::PrinterProfile& profile);
  static std::uint32_t brand_color(const core::PrinterProfile& profile);
  static std::uint32_t brand_logo_color(const core::PrinterProfile& profile,
                                        std::uint32_t background);
  static const lv_image_dsc_t* brand_logo(const core::PrinterProfile& profile);
  static const lv_image_dsc_t* brand_logo_small(const core::PrinterProfile& profile);
  void create_printer_chrome(const core::PrinterProfile& profile,
                             const core::PrinterSnapshot& snapshot,
                             const PowerSnapshot* power = nullptr);
  void update_printer_progress(const core::PrinterSnapshot& snapshot);
  void apply_printer_animations_enabled(bool enabled);
  void apply_reaction_progress_visibility();
  void create_printer_animation(lv_obj_t* parent);
  bool ensure_printer_animation_canvas();
  void release_printer_animation_canvas();
  void update_printer_animation(const core::JobState& job);
  bool update_printer_animation_source();
  void render_printer_animation_frame();
  void show_printer_reactions(const core::PrinterProfile& profile,
                              const core::PrinterSnapshot& snapshot,
                              const PowerSnapshot& power);
  void show_printer_status(const core::PrinterProfile& profile,
                           const core::PrinterSnapshot& snapshot, const PowerSnapshot& power,
                           const char* ipv4);
  void show_printer_nozzles(const core::PrinterProfile& profile,
                            const core::PrinterSnapshot& snapshot,
                            const PowerSnapshot& power);
  void show_printer_compact(const core::PrinterProfile& profile,
                            const core::PrinterSnapshot& snapshot,
                            const PowerSnapshot& power);
  void show_printer_telemetry(const core::PrinterProfile& profile,
                              const core::PrinterSnapshot& snapshot,
                              const PowerSnapshot& power);
  void show_printer_materials(const core::PrinterProfile& profile,
                              const core::PrinterSnapshot& snapshot);
  void show_printer_camera(const core::PrinterProfile& profile,
                           const core::PrinterSnapshot& snapshot,
                           const PowerSnapshot& power);
  void show_printer_light(const core::PrinterProfile& profile,
                          const core::PrinterSnapshot& snapshot);
  // Compact 240x240 renderers. They consume the same navigation state and
  // protocol-neutral snapshots as the established 466x466 renderer, while
  // keeping its geometry completely separate and therefore regression-safe.
  void square_create_initial_screen();
  void square_show_quick_menu();
  void square_show_brightness_overlay();
  void square_show_audio_overlay();
  void square_show_theme_overlay();
  void square_show_wifi_error(const char* network_name);
  void square_show_wifi_setup(const char* network_name, const char* local_hostname);
  void square_show_my_printers(const char* ipv4, const char* local_hostname,
                               const std::vector<core::PrinterProfile>& profiles,
                               std::uint32_t selected_profile,
                               const InactivePrinterSnapshot& inactive,
                               const PowerSnapshot& power,
                               const core::PrinterSnapshot* selected_snapshot);
  void square_show_printer_status(const core::PrinterProfile& profile,
                                  const core::PrinterSnapshot& snapshot,
                                  const PowerSnapshot& power);
  void square_show_printer_nozzles(const core::PrinterProfile& profile,
                                   const core::PrinterSnapshot& snapshot,
                                   const PowerSnapshot& power);
  void square_show_printer_compact(const core::PrinterProfile& profile,
                                   const core::PrinterSnapshot& snapshot,
                                   const PowerSnapshot& power);
  void square_show_printer_telemetry(const core::PrinterProfile& profile,
                                     const core::PrinterSnapshot& snapshot,
                                     const PowerSnapshot& power);
  void square_show_printer_materials(const core::PrinterProfile& profile,
                                     const core::PrinterSnapshot& snapshot);
  void square_show_printer_camera(const core::PrinterProfile& profile,
                                  const core::PrinterSnapshot& snapshot,
                                  const PowerSnapshot& power);
  void square_show_printer_light(const core::PrinterProfile& profile,
                                 const core::PrinterSnapshot& snapshot);
  void square_show_system_details(const NetworkStatus& network,
                                  const PowerSnapshot& power,
                                  std::size_t configured_count);
  void square_show_clock(bool analog, const PowerSnapshot& power);
  void square_show_web_config(const char* ipv4, const char* local_hostname,
                              const PowerSnapshot& power);
  void square_ensure_update_overlay();
  void square_create_header(const char* title, const PowerSnapshot* power = nullptr);
  void square_update_power_header(const PowerSnapshot& power);
  void square_create_printer_chrome(const core::PrinterProfile& profile,
                                     const core::PrinterSnapshot& snapshot,
                                     const PowerSnapshot* power);
  lv_obj_t* status_label_ = nullptr;
  lv_obj_t* wifi_setup_pager_ = nullptr;
  std::array<lv_obj_t*, 2> wifi_setup_dots_{};
  lv_obj_t* title_label_ = nullptr;
  lv_obj_t* detail_label_ = nullptr;
  lv_obj_t* progress_label_ = nullptr;
  lv_obj_t* temperature_label_ = nullptr;
  lv_obj_t* metrics_label_ = nullptr;
  lv_obj_t* header_audio_label_ = nullptr;
  lv_obj_t* header_power_label_ = nullptr;
  lv_obj_t* header_battery_outline_ = nullptr;
  lv_obj_t* header_battery_fill_ = nullptr;
  lv_obj_t* remaining_label_ = nullptr;
  lv_obj_t* total_time_label_ = nullptr;
  lv_obj_t* layer_label_ = nullptr;
  lv_obj_t* nozzle_temperature_label_ = nullptr;
  lv_obj_t* bed_temperature_label_ = nullptr;
  lv_obj_t* chamber_temperature_label_ = nullptr;
  lv_obj_t* nozzle_scroll_ = nullptr;
  lv_obj_t* nozzle_row_ = nullptr;
  bool nozzle_scroll_gesture_horizontal_ = false;
  lv_obj_t* printer_list_scroll_ = nullptr;
  std::size_t printer_list_count_ = 0;
  std::size_t printer_list_visible_count_ = 0;
  std::size_t printer_list_first_visible_ = 0;
  int printer_list_item_pitch_ = 0;
  std::array<lv_obj_t*, 3> telemetry_metric_cards_{};
  std::array<lv_obj_t*, 3> telemetry_metric_caption_labels_{};
  std::array<lv_obj_t*, 3> telemetry_metric_value_labels_{};
  std::array<lv_obj_t*, 2> telemetry_detail_caption_labels_{};
  std::array<lv_obj_t*, core::kMaximumToolheads> nozzle_cards_{};
  std::array<lv_obj_t*, core::kMaximumToolheads> nozzle_tool_labels_{};
  std::array<lv_obj_t*, core::kMaximumToolheads> nozzle_icons_{};
  std::array<lv_obj_t*, core::kMaximumToolheads> nozzle_temperature_labels_{};
  std::array<lv_obj_t*, core::kMaximumToolheads> nozzle_target_labels_{};
  std::array<lv_obj_t*, core::kMaximumToolheads> nozzle_material_labels_{};
  std::array<lv_obj_t*, core::kMaximumToolheads> nozzle_material_dots_{};
  lv_obj_t* version_label_ = nullptr;
  lv_obj_t* clock_status_label_ = nullptr;
  lv_obj_t* progress_arc_ = nullptr;
  lv_obj_t* clock_hour_hand_ = nullptr;
  lv_obj_t* clock_minute_hand_ = nullptr;
  lv_obj_t* clock_second_hand_ = nullptr;
  lv_obj_t* clock_date_label_ = nullptr;
  std::array<std::array<lv_obj_t*, 7>, 6> digital_segments_{};
  std::array<std::array<lv_obj_t*, 2>, 2> digital_colons_{};
  lv_obj_t* quick_overlay_ = nullptr;
  int pending_quick_menu_action_ = -1;
  int pending_theme_selection_ = -1;
  lv_timer_t* theme_selection_timer_ = nullptr;
  lv_timer_t* printer_animation_timer_ = nullptr;
  lv_obj_t* active_accent_label_ = nullptr;
  std::vector<lv_obj_t*> active_accent_text_objects_;
  std::vector<lv_obj_t*> active_accent_bg_objects_;
  lv_obj_t* shutdown_overlay_ = nullptr;
  lv_obj_t* shutdown_title_ = nullptr;
  lv_obj_t* shutdown_detail_ = nullptr;
  lv_obj_t* horizontal_transition_overlay_ = nullptr;
  lv_obj_t* configuration_backup_overlay_ = nullptr;
  lv_obj_t* configuration_backup_spinner_ = nullptr;
  lv_obj_t* configuration_backup_label_ = nullptr;
  lv_obj_t* update_overlay_ = nullptr;
  lv_obj_t* update_overlay_title_ = nullptr;
  lv_obj_t* update_overlay_versions_ = nullptr;
  lv_obj_t* update_overlay_detail_ = nullptr;
  lv_obj_t* update_overlay_progress_ = nullptr;
  lv_obj_t* update_overlay_progress_bar_ = nullptr;
  lv_obj_t* update_install_button_ = nullptr;
  lv_obj_t* update_dismiss_button_ = nullptr;
  lv_obj_t* update_install_button_label_ = nullptr;
  lv_obj_t* media_image_ = nullptr;
  lv_obj_t* printer_animation_root_ = nullptr;
  lv_obj_t* printer_animation_gesture_surface_ = nullptr;
  lv_obj_t* printer_animation_label_ = nullptr;
  lv_obj_t* printer_animation_canvas_ = nullptr;
  lv_obj_t* printer_animation_gif_ = nullptr;
  void* printer_animation_canvas_buffer_ = nullptr;
  lv_obj_t* camera_spinner_ = nullptr;
  lv_obj_t* camera_empty_label_ = nullptr;
  lv_obj_t* camera_activity_dot_ = nullptr;
  lv_obj_t* camera_activity_label_ = nullptr;
  lv_obj_t* camera_mode_row_ = nullptr;
  lv_obj_t* camera_snapshot_button_ = nullptr;
  lv_obj_t* camera_live_button_ = nullptr;
  lv_obj_t* chamber_light_bulb_ = nullptr;
  lv_obj_t* chamber_light_button_ = nullptr;
  lv_obj_t* chamber_light_button_label_ = nullptr;
  lv_obj_t* chamber_light_spinner_ = nullptr;
  lv_image_dsc_t preview_image_dsc_{};
  lv_image_dsc_t camera_image_dsc_{};
  std::shared_ptr<std::vector<std::uint8_t>> preview_encoded_;
  std::shared_ptr<std::vector<std::uint8_t>> preview_pixels_;
  std::shared_ptr<std::vector<std::uint8_t>> camera_pixels_;
  std::int64_t camera_activity_updated_until_us_ = 0;
  bool camera_was_refreshing_ = false;
  std::array<lv_obj_t*, 4> material_cards_{};
  std::array<lv_obj_t*, 4> material_slot_labels_{};
  std::array<lv_obj_t*, 4> material_feed_labels_{};
  std::array<lv_obj_t*, 4> material_name_labels_{};
  std::array<lv_obj_t*, 4> material_percent_labels_{};
  lv_obj_t* external_material_card_ = nullptr;
  lv_obj_t* external_material_dot_ = nullptr;
  lv_obj_t* external_material_label_ = nullptr;
  std::array<lv_point_precise_t, 2> hour_points_{};
  std::array<lv_point_precise_t, 2> minute_points_{};
  std::array<lv_point_precise_t, 2> second_points_{};
  std::uint32_t visible_profile_ = 0;
  int view_ = 0;
  std::atomic<int> page_{0};
  std::atomic<int> printer_subpage_{0};
  std::atomic<int> printer_subpage_count_{4};
  std::atomic<int> horizontal_depth_{0};
  std::atomic<int> horizontal_depth_count_{2};
  std::atomic<int> selected_camera_depth_{0};
  std::atomic<int> selected_light_depth_{0};
  std::atomic<bool> selected_is_bambu_{false};
  std::atomic<bool> selected_online_{false};
  std::uint32_t selected_profile_ = 0;
  std::uint32_t printer_retry_wait_profile_ = 0;
  std::uint64_t printer_retry_wait_until_ms_ = 0;
  core::LinkState visible_selected_link_ = core::LinkState::stopped;
  std::uint32_t visible_inactive_revision_ = 0xffffffffU;
  std::string visible_web_config_host_;
  BrightnessChanged brightness_changed_ = nullptr;
  void* brightness_changed_context_ = nullptr;
  AudioChanged audio_changed_ = nullptr;
  void* audio_changed_context_ = nullptr;
  AudioPresetChanged audio_preset_changed_ = nullptr;
  void* audio_preset_changed_context_ = nullptr;
  ThemeChanged theme_changed_ = nullptr;
  void* theme_changed_context_ = nullptr;
  LanguageChanged language_changed_ = nullptr;
  void* language_changed_context_ = nullptr;
  PrinterAnimationsChanged printer_animations_changed_ = nullptr;
  void* printer_animations_changed_context_ = nullptr;
  PrinterSelected printer_selected_ = nullptr;
  void* printer_selected_context_ = nullptr;
  NavigationFeedback navigation_feedback_ = nullptr;
  void* navigation_feedback_context_ = nullptr;
  PageRefreshRequested page_refresh_requested_ = nullptr;
  void* page_refresh_context_ = nullptr;
  ChamberLightChanged chamber_light_changed_ = nullptr;
  void* chamber_light_changed_context_ = nullptr;
  CameraModeChanged camera_mode_changed_ = nullptr;
  void* camera_mode_changed_context_ = nullptr;
  std::atomic<bool> camera_live_mode_{false};
  std::atomic<int> camera_snapshot_fps_{1};
  UpdateCheckRequested update_check_requested_ = nullptr;
  void* update_check_context_ = nullptr;
  UpdateInstallRequested update_install_requested_ = nullptr;
  void* update_install_context_ = nullptr;
  std::string update_version_text_ = "Version: " PRINTDECK_VERSION;
  std::uint32_t update_version_color_ = 0x94A3B8;
  std::string update_latest_version_;
  std::string update_detail_;
  int update_progress_percent_ = 0;
  int update_state_ = 0;
  bool update_available_ = false;
  bool update_busy_ = false;
  bool update_overlay_manually_opened_ = false;
  std::atomic<int> current_rotation_{0};
  std::atomic<int> touch_rotation_applied_{-1};
  lv_indev_t* touch_input_ = nullptr;
  std::atomic<int> remote_input_state_{0};
  std::int64_t remote_input_started_us_ = 0;
  std::atomic<std::int64_t> remote_activity_suppressed_until_us_{0};
  int remote_input_start_x_ = 0;
  int remote_input_start_y_ = 0;
  int remote_input_end_x_ = 0;
  int remote_input_end_y_ = 0;
  std::uint32_t remote_input_duration_ms_ = 0;
  bool horizontal_transition_active_ = false;
  bool horizontal_transition_target_applied_ = false;
  bool horizontal_transition_reveal_started_ = false;
  int horizontal_transition_direction_ = -1;
  int horizontal_transition_target_page_ = 0;
  bool horizontal_transition_target_printer_list_ = true;
  std::uint32_t horizontal_transition_target_profile_id_ = 0;
  int horizontal_transition_target_depth_ = 0;
  lv_timer_t* horizontal_transition_timeout_timer_ = nullptr;
  bool gesture_active_ = false;
  bool horizontal_gesture_committed_ = false;
  bool vertical_gesture_locked_ = false;
  bool gesture_started_in_printer_list_ = false;
  bool printer_list_vertical_gesture_ = false;
  bool printer_list_scroll_started_ = false;
  lv_obj_t* pressed_printer_card_ = nullptr;
  bool suppress_update_click_ = false;
  lv_coord_t gesture_start_x_ = 0;
  lv_coord_t gesture_start_y_ = 0;
  int square_gesture_peak_dx_ = 0;
  int square_gesture_peak_dy_ = 0;
  std::atomic<std::int64_t> background_render_quiet_until_us_{0};
  std::uint64_t last_activity_ms_ = 0;
  std::atomic<int> screen_power_mode_{0};
  bool power_source_known_ = false;
  bool last_on_battery_ = false;
  int applied_brightness_ = 75;
  bool printer_animations_enabled_ = false;
  bool reaction_progress_bar_enabled_ = true;
  bool reaction_progress_percent_enabled_ = true;
  bool capture_animation_override_active_ = false;
  bool printer_animation_compact_ = false;
  std::string capture_animation_screen_name_;
  core::PrinterActivity capture_animation_override_ = core::PrinterActivity::unknown;
  core::PrinterActivity printer_animation_activity_ = core::PrinterActivity::unknown;
  std::uint32_t printer_animation_primary_color_ = 0;
  std::uint32_t printer_animation_filament_color_ = 0;
  std::uint32_t printer_animation_frame_ = 0;
  ReactionAssetService* reaction_assets_ = nullptr;
  std::string printer_animation_gif_path_;
  std::uint32_t printer_animation_asset_generation_ = 0;
  bool printer_animation_source_pending_ = false;
  int printer_animation_native_width_ = 0;
  int printer_animation_native_height_ = 0;
  std::mutex power_policy_mutex_;
  core::DisplayPowerPolicy power_policy_;
  bool audio_enabled_ = true;
  int audio_volume_ = 60;
  std::string audio_preset_ = "modern";
  std::string active_theme_ = "green";
  std::string language_ = "en";
  std::atomic<core::CalendarDateFormat> clock_date_format_{
      core::CalendarDateFormat::year_month_day};
  std::array<lv_font_t, 5> localized_base_fonts_{};
  std::array<lv_font_t*, 3> terminal_fonts_{};
  std::array<lv_font_t*, 5> localized_latin_fonts_{};
  std::array<lv_font_t*, 5> localized_cjk_fonts_{};
  std::string capture_screen_name_ = "boot-status";
  std::string capture_overlay_name_;
  core::ThemeColors custom_theme_colors_{};
  std::uint32_t accent_color_ = 0x00FF00;
  core::ThemeColors theme_colors_ = core::resolved_theme("green", {});
  core::ThemeStyle theme_style_ = core::resolved_theme_style("green", theme_colors_);
  std::atomic<bool> display_ready_{false};
  std::atomic<bool> draw_recovery_pending_{false};
};

}  // namespace printdeck::platform
