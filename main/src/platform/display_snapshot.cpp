#include "printdeck/platform/display_snapshot.hpp"

#include "src/core/lv_obj_draw_private.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"
#include "src/draw/lv_draw_private.h"

namespace printdeck::platform {

lv_draw_buf_t* capture_composed_display(lv_display_t* display) {
  if (display == nullptr) return nullptr;
  const int width = lv_display_get_horizontal_resolution(display);
  const int height = lv_display_get_vertical_resolution(display);
  if (width <= 0 || height <= 0) return nullptr;
  lv_obj_t* roots[] = {lv_display_get_screen_active(display),
                      lv_display_get_layer_top(display),
                      lv_display_get_layer_sys(display)};
  if (roots[0] == nullptr) return nullptr;
  for (auto* root : roots) if (root != nullptr) lv_obj_update_layout(root);

  auto* buffer = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
  if (buffer == nullptr) return nullptr;
  lv_draw_buf_clear(buffer, nullptr);

  // Follow LVGL's snapshot draw lifecycle, but submit all three roots to the
  // same canvas. Independent snapshots would clear or duplicate that canvas.
  lv_layer_t layer;
  lv_layer_init(&layer);
  layer.draw_buf = buffer;
  layer.buf_area = {0, 0, width - 1, height - 1};
  layer.color_format = LV_COLOR_FORMAT_ARGB8888;
  layer._clip_area = layer.buf_area;
  layer.phy_clip_area = layer.buf_area;
  lv_draw_unit_send_event(nullptr, LV_EVENT_CHILD_CREATED, &layer);

  auto* previous_display = lv_refr_get_disp_refreshing();
  auto* previous_layer = display->layer_head;
  display->layer_head = &layer;
  lv_refr_set_disp_refreshing(display);
  for (auto* root : roots) if (root != nullptr) lv_obj_redraw(&layer, root);
  layer.all_tasks_added = true;
  while (layer.draw_task_head != nullptr) {
    lv_draw_dispatch_wait_for_request();
    lv_draw_dispatch();
  }
  display->layer_head = previous_layer;
  lv_refr_set_disp_refreshing(previous_display);
  lv_draw_unit_send_event(nullptr, LV_EVENT_SCREEN_LOAD_START, &layer);
  lv_draw_unit_send_event(nullptr, LV_EVENT_CHILD_DELETED, &layer);
  return buffer;
}

}  // namespace printdeck::platform
