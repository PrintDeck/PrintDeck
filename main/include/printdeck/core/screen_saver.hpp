#pragma once

#include <array>
#include <algorithm>
#include <cstdint>

namespace printdeck::core {

// Six independent ripples, fixed storage and integer arithmetic. No decoded
// textures, frame buffers or allocations are needed during an animation tick.
class ScreenSaverRipples {
 public:
  struct Ripple {
    int x = 0, y = 0, radius = 0;
    std::uint8_t opacity = 0, color = 0;
  };
  static constexpr unsigned kCount = 6;
  void reset(std::uint32_t seed, int size) {
    random_ = seed ? seed : 1;
    size_ = size > 0 ? size : 1;
    for (unsigned i = 0; i < kCount; ++i) {
      restart(i);
      phases_[i].age = i * 19;
    }
  }
  std::array<Ripple, kCount> tick() {
    std::array<Ripple, kCount> result{};
    for (unsigned i = 0; i < kCount; ++i) {
      auto& p = phases_[i];
      if (++p.age >= p.life) restart(i);
      const int progress = p.age * 1024 / p.life;
      const int envelope = progress < 512 ? progress : 1024 - progress;
      result[i] = {p.x + p.dx * progress / 1024,
                   p.y + p.dy * progress / 1024,
                   size_ * (90 + progress / 3) / 1024,
                   static_cast<std::uint8_t>(envelope * envelope * 115 / (512 * 512)),
                   p.color};
    }
    return result;
  }

 private:
  struct Phase { int x, y, dx, dy, age, life; std::uint8_t color; };
  std::array<Phase, kCount> phases_{};
  std::uint32_t random_ = 1;
  int size_ = 240;
  unsigned next() {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_;
  }
  void restart(unsigned index) {
    auto& p = phases_[index];
    p = {size_ * static_cast<int>(20 + next() % 61) / 100,
         size_ * static_cast<int>(20 + next() % 61) / 100,
         size_ * (static_cast<int>(next() % 31) - 15) / 100,
         size_ * (static_cast<int>(next() % 31) - 15) / 100,
         0, 130 + static_cast<int>(next() % 91),
         static_cast<std::uint8_t>(next() % 4)};
  }
};

// A sheep crosses a small fence in a drifting night scene. Rounded primitives
// keep rendering bounded, without a GIF decoder, textures or frame allocations.
class ScreenSaverSheep {
 public:
  struct Shape {
    int x, y, width, height, radius;
    std::uint32_t color;
    std::uint8_t opacity;
  };
  static constexpr unsigned kCount = 21;
  static constexpr int kCycleTicks = 96;
  void reset(std::uint32_t seed, int size) {
    random_ = seed ? seed : 1;
    size_ = std::max(size, 240);
    restart();
  }
  std::array<Shape, kCount> tick() {
    if (++age_ >= kCycleTicks) restart();
    std::array<Shape, kCount> shapes{};
    unsigned index = 0;
    const int opacity = std::min({age_, kCycleTicks - 1 - age_, 10}) * 255 / 10;
    const auto add = [&](int x, int y, int width, int height, int radius,
                         std::uint32_t color, int brightness = 255) {
      if (mirrored_) x = 240 - x - width;
      const auto scale = [&](int value) { return (value * size_ + 120) / 240; };
      shapes[index++] = {scale(x), scale(y), scale(width), scale(height),
                          scale(radius), color,
                          static_cast<std::uint8_t>(opacity * brightness / 255)};
    };
    for (int i = 0; i < 3; ++i) {
      const int pulse = (age_ + 19 * i) % 48;
      add(stars_[i][0], stars_[i][1], 3, 3, 2, 0xADBCE0,
          70 + (pulse < 24 ? pulse : 48 - pulse) * 5);
    }
    // The complete fence fades and changes position on every crossing.
    add(anchor_x_ - 13, ground_y_ - 27, 4, 32, 2, 0x72958E, 190);
    add(anchor_x_ + 9, ground_y_ - 27, 4, 32, 2, 0x72958E, 190);
    add(anchor_x_ - 17, ground_y_ - 23, 34, 3, 1, 0x91B2A4, 180);
    add(anchor_x_ - 17, ground_y_ - 12, 34, 3, 1, 0x91B2A4, 180);

    const int progress = age_ * 1024 / (kCycleTicks - 1);
    const int jump = std::clamp(progress - 64, 0, 896);
    const int lift = 62 * 4 * jump * (896 - jump) / (896 * 896);
    const int x = anchor_x_ - 64 + 128 * progress / 1024;
    const int step = lift > 0 ? 0 : ((age_ / 4) % 2 ? 2 : -2);
    const int y = ground_y_ - lift;
    add(x - 9 + step, y - 8, 3, 12, 1, 0x8492AE);  // back leg
    add(x - 10 + step, y + 2, 6, 3, 1, 0x8492AE);
    add(x + 8 - step, y - 8, 3, 12, 1, 0x8492AE);  // front leg
    add(x + 7 - step, y + 2, 6, 3, 1, 0x8492AE);
    add(x - 23, y - 22, 10, 8, 5, 0xBDC8DA);      // tail
    add(x - 17, y - 27, 35, 23, 12, 0xC8D2E1);   // fleece
    add(x - 19, y - 24, 13, 14, 7, 0xC8D2E1);
    add(x - 13, y - 32, 15, 15, 8, 0xC8D2E1);
    add(x - 2, y - 34, 16, 16, 8, 0xC8D2E1);
    add(x + 7, y - 28, 14, 14, 7, 0xC8D2E1);
    add(x - 7, y - 18, 17, 15, 8, 0xC8D2E1);
    add(x + 14, y - 29, 14, 19, 7, 0x8390AA);     // face
    add(x + 12, y - 32, 11, 6, 3, 0xA8B4CB);      // floppy ear
    add(x + 21, y - 22, 4, 2, 1, 0x182332);       // sleepy eye
    return shapes;
  }

 private:
  std::uint32_t random_ = 1;
  int size_ = 240, age_ = 0, anchor_x_ = 120, ground_y_ = 156;
  bool mirrored_ = false;
  std::array<std::array<int, 2>, 3> stars_{};
  unsigned next() {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_;
  }
  void restart() {
    age_ = 0;
    anchor_x_ = 112 + next() % 17;
    ground_y_ = 142 + next() % 29;
    mirrored_ = (next() & 1U) != 0;
    for (auto& star : stars_)
      star = {60 + static_cast<int>(next() % 121), 48 + static_cast<int>(next() % 31)};
  }
};

}  // namespace printdeck::core
