#include "printdeck/platform/printer_animation_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace printdeck::platform {
namespace {

constexpr float kPi = 3.14159265358979323846F;

std::uint32_t mix_color(std::uint32_t from, std::uint32_t to, int amount) {
  amount = std::clamp(amount, 0, 255);
  const int inverse = 255 - amount;
  const auto channel = [inverse, amount](int a, int b) {
    return static_cast<std::uint32_t>((a * inverse + b * amount) / 255);
  };
  return (channel((from >> 16U) & 0xFFU, (to >> 16U) & 0xFFU) << 16U) |
         (channel((from >> 8U) & 0xFFU, (to >> 8U) & 0xFFU) << 8U) |
         channel(from & 0xFFU, to & 0xFFU);
}

float cycle(std::uint32_t frame, std::uint32_t period) {
  return static_cast<float>(frame % period) / static_cast<float>(period);
}

float pulse(std::uint32_t frame, std::uint32_t period) {
  return 0.5F + 0.5F * std::sin(cycle(frame, period) * 2.0F * kPi);
}

float ping_pong(std::uint32_t frame, std::uint32_t period) {
  const float value = cycle(frame, period);
  return value < 0.5F ? value * 2.0F : (1.0F - value) * 2.0F;
}

class Painter {
 public:
  Painter(lv_layer_t* layer, int width, int height)
      : layer_(layer), scale_(std::min(width / 300.0F, height / 180.0F)),
        offset_x_((width - 300.0F * scale_) * 0.5F),
        offset_y_((height - 180.0F * scale_) * 0.5F) {}

  int px(float value) const {
    return std::max(1, static_cast<int>(std::lround(value * scale_)));
  }

  int x(float value) const {
    return static_cast<int>(std::lround(offset_x_ + value * scale_));
  }

  int y(float value) const {
    return static_cast<int>(std::lround(offset_y_ + value * scale_));
  }

  void rect(float cx, float cy, float width, float height, std::uint32_t color,
            float radius = 0, lv_opa_t opacity = LV_OPA_COVER,
            float border_width = 0, std::uint32_t border_color = 0) const {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = opacity;
    dsc.radius = px(radius);
    if (border_width > 0) {
      dsc.border_width = px(border_width);
      dsc.border_color = lv_color_hex(border_color);
      dsc.border_opa = opacity;
    }
    lv_area_t area{x(cx - width * 0.5F), y(cy - height * 0.5F),
                   x(cx + width * 0.5F), y(cy + height * 0.5F)};
    lv_draw_rect(layer_, &dsc, &area);
  }

  void circle(float cx, float cy, float diameter, std::uint32_t color,
              lv_opa_t opacity = LV_OPA_COVER, float border_width = 0,
              std::uint32_t border_color = 0) const {
    rect(cx, cy, diameter, diameter, color, diameter * 0.5F, opacity,
         border_width, border_color);
  }

  void line(float x1, float y1, float x2, float y2, std::uint32_t color,
            float width, lv_opa_t opacity = LV_OPA_COVER,
            bool rounded = true) const {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_hex(color);
    dsc.width = px(width);
    dsc.opa = opacity;
    dsc.round_start = rounded;
    dsc.round_end = rounded;
    dsc.p1 = {x(x1), y(y1)};
    dsc.p2 = {x(x2), y(y2)};
    lv_draw_line(layer_, &dsc);
  }

  void arc(float cx, float cy, float radius, float start, float end,
           std::uint32_t color, float width,
           lv_opa_t opacity = LV_OPA_COVER) const {
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = lv_color_hex(color);
    dsc.width = px(width);
    dsc.opa = opacity;
    dsc.rounded = true;
    dsc.center = {x(cx), y(cy)};
    dsc.radius = static_cast<std::uint16_t>(px(radius));
    dsc.start_angle = start;
    dsc.end_angle = end;
    lv_draw_arc(layer_, &dsc);
  }

  void triangle(float x1, float y1, float x2, float y2, float x3, float y3,
                std::uint32_t color,
                lv_opa_t opacity = LV_OPA_COVER) const {
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = lv_color_hex(color);
    dsc.opa = opacity;
    dsc.p[0] = {x(x1), y(y1)};
    dsc.p[1] = {x(x2), y(y2)};
    dsc.p[2] = {x(x3), y(y3)};
    lv_draw_triangle(layer_, &dsc);
  }

 private:
  lv_layer_t* layer_;
  float scale_;
  float offset_x_;
  float offset_y_;
};

void draw_eye(const Painter& painter, float cx, float cy, float width, float height,
              std::uint32_t color, std::uint32_t background, float pupil_x,
              float pupil_y, bool blink) {
  if (blink) {
    painter.line(cx - width * 0.42F, cy, cx + width * 0.42F, cy, color, 5);
    return;
  }
  painter.rect(cx, cy, width, height, color, height * 0.5F);
  painter.circle(cx + pupil_x, cy + pupil_y, height * 0.48F, background);
  painter.circle(cx + pupil_x - height * 0.08F, cy + pupil_y - height * 0.09F,
                 height * 0.10F, color, LV_OPA_70);
}

void draw_face(const Painter& painter, std::uint32_t frame, std::uint32_t color,
               std::uint32_t background, float pupil_x = 0, float pupil_y = 0,
               bool sleepy = false, bool happy = false) {
  const bool blink = (frame % 86U) < 3U;
  painter.arc(150, 91, 72, 205, 335, color, 3, LV_OPA_30);
  painter.arc(150, 91, 72, 25, 155, color, 3, LV_OPA_20);
  if (happy) {
    painter.arc(111, 76, 16, 205, 335, color, 5);
    painter.arc(189, 76, 16, 205, 335, color, 5);
    painter.arc(150, 104, 27, 28, 152, color, 6);
    return;
  }
  const float eye_height = sleepy ? 12.0F : 30.0F;
  draw_eye(painter, 111, 75, 48, eye_height, color, background, pupil_x, pupil_y,
           blink);
  draw_eye(painter, 189, 75, 48, eye_height, color, background, -pupil_x, pupil_y,
           blink);
  if (sleepy) painter.arc(150, 109, 15, 25, 155, color, 4);
  else painter.rect(150, 118, 32, 5, color, 3);
}

void draw_toolhead(const Painter& painter, float cx, float cy, std::uint32_t color,
                   std::uint32_t background, float pupil_x = 0,
                   bool probe = false) {
  painter.rect(cx, cy - 30, 30, 14, color, 5, LV_OPA_70);
  painter.rect(cx, cy, 70, 48, color, 12, LV_OPA_COVER, 3,
               mix_color(color, 0xFFFFFF, 65));
  draw_eye(painter, cx - 17, cy - 2, 20, 14, color, background, pupil_x, 0, false);
  draw_eye(painter, cx + 17, cy - 2, 20, 14, color, background, -pupil_x, 0, false);
  painter.rect(cx, cy + 28, 52, 10, color, 4);
  painter.triangle(cx - 12, cy + 33, cx + 12, cy + 33, cx, cy + 50, color);
  if (probe) {
    painter.line(cx + 26, cy + 22, cx + 26, cy + 59, color, 4);
    painter.circle(cx + 26, cy + 61, 7, color);
  }
}

void draw_heat_wave(const Painter& painter, float x, float base_y, float phase,
                    std::uint32_t color, float height) {
  std::array<std::pair<float, float>, 5> points{};
  for (int index = 0; index < 5; ++index) {
    const float t = index / 4.0F;
    points[index] = {x + std::sin((t * 1.7F + phase) * 2.0F * kPi) * 5.0F,
                     base_y - t * height};
    if (index > 0) {
      painter.line(points[index - 1].first, points[index - 1].second,
                   points[index].first, points[index].second, color, 4,
                   static_cast<lv_opa_t>(210 - index * 22));
    }
  }
}

void draw_grip_roller(const Painter& painter, float cx, float cy, float angle,
                      std::uint32_t color, std::uint32_t background) {
  painter.circle(cx, cy, 54, background, LV_OPA_COVER, 5, color);
  painter.circle(cx, cy, 22, color);
  painter.circle(cx, cy, 8, background);
  for (int index = 0; index < 8; ++index) {
    const float a = angle + index * kPi / 4.0F;
    const float inner_x = cx + std::cos(a) * 23.0F;
    const float inner_y = cy + std::sin(a) * 23.0F;
    const float outer_x = cx + std::cos(a) * 31.0F;
    const float outer_y = cy + std::sin(a) * 31.0F;
    painter.line(inner_x, inner_y, outer_x, outer_y, color, 6);
  }
}

void draw_direction_arrow(const Painter& painter, float x, float y, bool down,
                          std::uint32_t color) {
  const float sign = down ? 1.0F : -1.0F;
  painter.line(x, y - sign * 13, x, y + sign * 13, color, 4);
  painter.line(x, y + sign * 13, x - 8, y + sign * 5, color, 4);
  painter.line(x, y + sign * 13, x + 8, y + sign * 5, color, 4);
}

void draw_filament_drive(const Painter& painter, core::PrinterActivity activity,
                         std::uint32_t frame,
                         const PrinterAnimationPalette& palette) {
  bool down = activity != core::PrinterActivity::filament_unloading;
  if (activity == core::PrinterActivity::filament_changing) {
    down = ((frame / 32U) % 2U) != 0U;
  }
  const float direction = down ? 1.0F : -1.0F;
  const float rotation = cycle(frame, 24) * 2.0F * kPi * direction;
  const std::uint32_t drive = palette.primary;
  const std::uint32_t guide = mix_color(palette.secondary, palette.background, 75);

  painter.rect(150, 35, 34, 14, guide, 7, LV_OPA_COVER, 2, drive);
  painter.line(150, 43, 150, 157, palette.filament, 7);
  const float dash_offset = static_cast<float>((frame * 5U) % 24U) * direction;
  for (int index = -2; index < 7; ++index) {
    float y = 50.0F + index * 24.0F + dash_offset;
    while (y < 46) y += 168;
    while (y > 162) y -= 168;
    painter.line(150, y, 150, std::min(162.0F, y + 9),
                 mix_color(palette.filament, 0xFFFFFF, 105), 3);
  }
  draw_grip_roller(painter, 117, 99, rotation, drive, palette.background);
  draw_grip_roller(painter, 183, 99, -rotation, drive, palette.background);
  draw_direction_arrow(painter, 246, 99, down, drive);

  draw_eye(painter, 123, 38, 26, 18, drive, palette.background,
           down ? 2 : -2, 1, false);
  draw_eye(painter, 177, 38, 26, 18, drive, palette.background,
           down ? 2 : -2, 1, false);

  if (activity == core::PrinterActivity::filament_changing) {
    draw_direction_arrow(painter, 54, 84, false, palette.secondary);
    draw_direction_arrow(painter, 54, 114, true, palette.secondary);
  } else if (activity == core::PrinterActivity::filament_purging) {
    const float blob = 11.0F + pulse(frame, 18) * 7.0F;
    painter.triangle(142, 158, 158, 158, 150, 171, palette.filament);
    painter.circle(150, 170, blob, palette.filament, LV_OPA_90);
    painter.arc(150, 170, 26 + pulse(frame, 18) * 5, 205, 335,
                palette.filament, 3, LV_OPA_40);
  }
}

void draw_printing(const Painter& painter, std::uint32_t frame,
                   const PrinterAnimationPalette& palette) {
  const int phase = static_cast<int>(frame % 72U);
  const int layer = phase / 12;
  const float travel = (phase % 12) / 11.0F;
  const bool reverse = (layer % 2) != 0;
  const float nozzle_x = 78.0F + (reverse ? 1.0F - travel : travel) * 144.0F;
  const float current_y = 145.0F - layer * 13.0F;
  const std::uint32_t soft = mix_color(palette.primary, palette.background, 90);

  painter.line(42, 28, 258, 28, soft, 5);
  painter.line(nozzle_x, 28, nozzle_x, current_y - 36, soft, 4);
  draw_toolhead(painter, nozzle_x, current_y - 58, palette.primary,
                palette.background, reverse ? -2 : 2);
  painter.line(47, 158, 253, 158, palette.secondary, 6);
  for (int index = 0; index < 6; ++index) {
    const float y = 145.0F - index * 13.0F;
    if (index < layer) {
      const float inset = index * 7.0F;
      painter.line(74 + inset, y, 226 - inset, y, palette.primary, 8);
    } else if (index == layer) {
      const float inset = index * 7.0F;
      const float left = 74 + inset;
      const float right = 226 - inset;
      const float endpoint = reverse ? right - travel * (right - left)
                                     : left + travel * (right - left);
      painter.line(reverse ? right : left, y, endpoint, y, palette.primary, 8);
      painter.circle(endpoint, y, 9, mix_color(palette.primary, 0xFFFFFF, 75));
    }
  }
  for (int index = 0; index < 3; ++index) {
    const float spark_phase = cycle(frame + index * 7U, 22);
    painter.circle(nozzle_x - 13 + index * 13, current_y - 8 - spark_phase * 18,
                   4, palette.secondary,
                   static_cast<lv_opa_t>(230 - spark_phase * 180));
  }
}

void draw_confetti(const Painter& painter, std::uint32_t frame,
                   std::uint32_t primary, std::uint32_t secondary) {
  for (int index = 0; index < 12; ++index) {
    const float phase = cycle(frame + index * 11U, 52);
    const float x = 32.0F + index * 21.5F + std::sin(index * 2.2F + phase * kPi) * 9.0F;
    const float y = 12.0F + phase * 168.0F;
    const float dx = (index % 2 == 0 ? 5.0F : -5.0F);
    painter.line(x, y, x + dx, y + 8, index % 3 == 0 ? secondary : primary,
                 4, static_cast<lv_opa_t>(255 - phase * 150));
  }
}

}  // namespace

void render_printer_animation(lv_obj_t* canvas, core::PrinterActivity activity,
                              std::uint32_t frame,
                              const PrinterAnimationPalette& palette) {
  if (canvas == nullptr || !lv_obj_is_valid(canvas)) return;
  const lv_image_dsc_t* image = lv_canvas_get_image(canvas);
  if (image == nullptr || image->header.w == 0 || image->header.h == 0) return;

  lv_canvas_fill_bg(canvas, lv_color_hex(palette.background), LV_OPA_COVER);
  lv_layer_t layer;
  lv_canvas_init_layer(canvas, &layer);
  Painter painter(&layer, image->header.w, image->header.h);
  const std::uint32_t soft = mix_color(palette.primary, palette.background, 105);

  switch (activity) {
    case core::PrinterActivity::standby: {
      const float look = std::sin(cycle(frame, 96) * 2.0F * kPi) * 4.0F;
      draw_face(painter, frame, palette.primary, palette.background, look, 1,
                (frame % 120U) > 78U);
      const float drift = pulse(frame, 46);
      painter.circle(235, 53 - drift * 8, 7, palette.secondary, LV_OPA_70);
      painter.circle(251, 35 - drift * 12, 5, palette.secondary, LV_OPA_50);
      painter.circle(263, 20 - drift * 15, 3, palette.secondary, LV_OPA_30);
      break;
    }
    case core::PrinterActivity::preparing: {
      const float angle = cycle(frame, 42) * 2.0F * kPi;
      draw_face(painter, frame, palette.primary, palette.background,
                std::cos(angle) * 5, std::sin(angle) * 3);
      painter.arc(150, 91, 82, frame * 11 % 360, frame * 11 % 360 + 92,
                  palette.secondary, 5, LV_OPA_80);
      for (int index = 0; index < 3; ++index) {
        const float a = angle + index * 2.0F * kPi / 3.0F;
        painter.circle(150 + std::cos(a) * 82, 91 + std::sin(a) * 82,
                       index == 0 ? 11 : 7, palette.secondary,
                       index == 0 ? LV_OPA_COVER : LV_OPA_60);
      }
      break;
    }
    case core::PrinterActivity::nozzle_heating: {
      const float glow = pulse(frame, 24);
      painter.circle(150, 101, 142 + glow * 12, palette.primary, LV_OPA_10);
      draw_toolhead(painter, 150, 78, palette.primary, palette.background,
                    std::sin(cycle(frame, 38) * 2 * kPi) * 2);
      for (int index = 0; index < 3; ++index) {
        draw_heat_wave(painter, 120 + index * 30, 173,
                       cycle(frame + index * 6U, 32), palette.secondary, 34);
      }
      break;
    }
    case core::PrinterActivity::bed_heating: {
      draw_eye(painter, 113, 48, 43, 26, palette.primary, palette.background,
               0, 3, false);
      draw_eye(painter, 187, 48, 43, 26, palette.primary, palette.background,
               0, 3, false);
      painter.rect(150, 139, 210, 20, soft, 8, LV_OPA_COVER, 4,
                   palette.primary);
      painter.line(68, 152, 56, 165, palette.primary, 5);
      painter.line(232, 152, 244, 165, palette.primary, 5);
      for (int index = 0; index < 5; ++index) {
        draw_heat_wave(painter, 90 + index * 30, 126,
                       cycle(frame + index * 5U, 34), palette.secondary,
                       48 + index % 2 * 7);
      }
      break;
    }
    case core::PrinterActivity::homing: {
      const float travel = ping_pong(frame, 46);
      const float head_x = 62 + travel * 176;
      painter.line(38, 25, 262, 25, soft, 6);
      painter.line(43, 25, 43, 151, soft, 4);
      painter.line(257, 25, 257, 151, soft, 4);
      draw_toolhead(painter, head_x, 83, palette.primary, palette.background,
                    travel > 0.5F ? 3 : -3);
      painter.line(47, 153, 253, 153, palette.secondary, 5);
      const float target = 11 + pulse(frame, 23) * 7;
      painter.arc(47, 153, target, 0, 355, palette.secondary, 3, LV_OPA_80);
      painter.line(32, 153, 62, 153, palette.secondary, 2, LV_OPA_60);
      painter.line(47, 138, 47, 168, palette.secondary, 2, LV_OPA_60);
      break;
    }
    case core::PrinterActivity::bed_leveling: {
      static constexpr std::array<float, 5> points{55, 102, 150, 198, 245};
      const int point = static_cast<int>((frame / 18U) % points.size());
      const float settle = std::min(1.0F, (frame % 18U) / 7.0F);
      const float head_x = points[point];
      painter.line(35, 24, 265, 24, soft, 5);
      draw_toolhead(painter, head_x, 73 + settle * 7, palette.primary,
                    palette.background, 0, true);
      painter.line(43, 151, 257, 151, palette.secondary, 6);
      for (std::size_t index = 0; index < points.size(); ++index) {
        painter.circle(points[index], 151, index == static_cast<std::size_t>(point)
                                            ? 11 + pulse(frame, 18) * 5
                                            : 7,
                       index < static_cast<std::size_t>(point)
                           ? palette.secondary : palette.primary,
                       index == static_cast<std::size_t>(point) ? LV_OPA_90 : LV_OPA_50);
      }
      break;
    }
    case core::PrinterActivity::nozzle_cleaning: {
      const float scrub = ping_pong(frame, 22);
      const float head_x = 82 + scrub * 136;
      painter.line(38, 25, 262, 25, soft, 5);
      draw_toolhead(painter, head_x, 75, palette.primary, palette.background,
                    scrub > 0.5F ? 3 : -3);
      painter.rect(150, 151, 204, 12, soft, 6, LV_OPA_COVER, 3,
                   palette.secondary);
      for (int index = 0; index < 13; ++index) {
        const float x = 67 + index * 14.0F;
        painter.line(x, 145, x + (index % 2 ? 5 : -5), 128,
                     palette.secondary, 4);
      }
      if (scrub < 0.08F || scrub > 0.92F) {
        for (int index = 0; index < 4; ++index) {
          const float a = index * kPi * 0.5F + pulse(frame, 10);
          painter.line(head_x + std::cos(a) * 12, 126 + std::sin(a) * 8,
                       head_x + std::cos(a) * 22, 126 + std::sin(a) * 17,
                       palette.secondary, 3);
        }
      }
      break;
    }
    case core::PrinterActivity::calibrating: {
      const float angle = cycle(frame, 48) * 360.0F;
      draw_face(painter, frame, palette.primary, palette.background,
                std::sin(cycle(frame, 26) * 2 * kPi) * 4, 0);
      painter.arc(150, 91, 82, angle, angle + 86, palette.secondary, 5);
      painter.arc(150, 91, 82, angle + 180, angle + 246, palette.primary, 3,
                  LV_OPA_60);
      for (int index = 0; index < 3; ++index) {
        const float a = (angle + index * 120.0F) * kPi / 180.0F;
        painter.circle(150 + std::cos(a) * 82, 91 + std::sin(a) * 82,
                       9, index == 0 ? palette.secondary : palette.primary,
                       index == 0 ? LV_OPA_COVER : LV_OPA_60);
      }
      for (int index = 0; index < 5; ++index) {
        const float x1 = 104 + index * 23.0F;
        const float amplitude = 5 + pulse(frame + index * 4U, 24) * 12;
        painter.line(x1, 145, x1 + 11, 145 - amplitude, palette.secondary, 3);
        painter.line(x1 + 11, 145 - amplitude, x1 + 22, 145, palette.secondary, 3);
      }
      break;
    }
    case core::PrinterActivity::filament_changing:
    case core::PrinterActivity::filament_unloading:
    case core::PrinterActivity::filament_loading:
    case core::PrinterActivity::filament_purging:
      draw_filament_drive(painter, activity, frame, palette);
      break;
    case core::PrinterActivity::printing:
      draw_printing(painter, frame, palette);
      break;
    case core::PrinterActivity::paused: {
      draw_face(painter, frame, palette.primary, palette.background, 5, 5);
      const float ring = 62 + pulse(frame, 30) * 8;
      painter.arc(150, 91, ring, 205, 335, palette.secondary, 3, LV_OPA_40);
      painter.rect(139, 139, 12, 35, palette.secondary, 6);
      painter.rect(161, 139, 12, 35, palette.secondary, 6);
      break;
    }
    case core::PrinterActivity::completed: {
      draw_confetti(painter, frame, palette.primary, palette.secondary);
      const float grow = std::min(1.0F, (frame % 60U) / 16.0F);
      painter.arc(150, 92, 62 * grow, 0, 355, palette.primary, 7);
      if (grow > 0.48F) {
        const float check = std::min(1.0F, (grow - 0.48F) / 0.52F);
        painter.line(118, 94, 140, 116, palette.primary, 9);
        painter.line(140, 116, 187, 65 + (1.0F - check) * 51,
                     palette.primary, 9);
      }
      painter.arc(125, 52, 11, 205, 335, palette.secondary, 4);
      painter.arc(175, 52, 11, 205, 335, palette.secondary, 4);
      break;
    }
    case core::PrinterActivity::failed: {
      painter.arc(150, 91, 76 + pulse(frame, 22) * 4, 0, 355,
                  palette.primary, 5, LV_OPA_50);
      for (float cx : {112.0F, 188.0F}) {
        painter.line(cx - 14, 61, cx + 14, 89, palette.primary, 7);
        painter.line(cx + 14, 61, cx - 14, 89, palette.primary, 7);
      }
      painter.line(122, 132, 136, 122, palette.primary, 6);
      painter.line(136, 122, 151, 133, palette.primary, 6);
      painter.line(151, 133, 166, 121, palette.primary, 6);
      painter.line(166, 121, 180, 132, palette.primary, 6);
      painter.triangle(230, 41, 266, 108, 194, 108, palette.secondary,
                       LV_OPA_70);
      painter.line(230, 61, 230, 87, palette.background, 6);
      painter.circle(230, 98, 7, palette.background);
      break;
    }
    case core::PrinterActivity::cancelled: {
      const float breathe = 1.0F + pulse(frame, 34) * 0.08F;
      painter.circle(150, 92, 126 * breathe, palette.background,
                     LV_OPA_COVER, 6, palette.primary);
      draw_eye(painter, 114, 75, 42, 13, palette.primary, palette.background,
               0, 0, false);
      draw_eye(painter, 186, 75, 42, 13, palette.primary, palette.background,
               0, 0, false);
      painter.rect(150, 118, 47, 6, palette.primary, 3);
      painter.rect(150, 151, 60, 9, palette.secondary, 5, LV_OPA_70);
      break;
    }
    case core::PrinterActivity::unknown: {
      const float angle = cycle(frame, 54) * 360.0F;
      painter.arc(150, 91, 76, 0, 355, soft, 3, LV_OPA_60);
      painter.arc(150, 91, 52, 0, 355, soft, 2, LV_OPA_40);
      const float radians = angle * kPi / 180.0F;
      painter.line(150, 91, 150 + std::cos(radians) * 73,
                   91 + std::sin(radians) * 73, palette.secondary, 4,
                   LV_OPA_70);
      const float look = std::cos(radians) * 6;
      draw_eye(painter, 116, 77, 42, 28, palette.primary, palette.background,
               look, 0, false);
      draw_eye(painter, 184, 77, 42, 28, palette.primary, palette.background,
               look, 0, false);
      for (int index = 0; index < 3; ++index) {
        painter.circle(132 + index * 18, 128, 6, palette.primary,
                       static_cast<lv_opa_t>(90 + ((frame / 6U + index) % 3U) * 70));
      }
      break;
    }
  }

  lv_canvas_finish_layer(canvas, &layer);
}

}  // namespace printdeck::platform
