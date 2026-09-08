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

// Small geometric scenes share one fixed shape buffer. No image assets or
// per-frame heap allocations; coordinates use the same 240 px design space.
class ScreenSaverScene {
 public:
  using Shape = ScreenSaverSheep::Shape;
  static constexpr unsigned kCount = 22;
  void reset(std::uint32_t seed, int size, std::uint8_t kind) {
    random_ = seed ? seed : 1;
    size_ = std::max(size, 1);
    kind_ = kind;
    ticks_ = 0;
    for (auto& p : particles_) {
      p = {static_cast<int>(next() % 181) - 90,
           static_cast<int>(next() % 181) - 90,
           static_cast<int>(next() % 256)};
    }
  }
  static int wave(int phase) {
    const int p = phase & 255;
    const int x = p < 128 ? p : p - 128;
    const int height = x * (128 - x) / 32;
    return p < 128 ? height : -height;
  }
  std::array<Shape, kCount> tick() {
    ticks_ = (ticks_ + 1) & 4095;
    std::array<Shape, kCount> shapes{};
    unsigned count = 0;
    const auto add = [&](int x, int y, int w, int h, int radius,
                         std::uint32_t color, int opacity) {
      const auto scale = [&](int value) { return value * size_ / 240; };
      shapes[count++] = {scale(x), scale(y), std::max(1, scale(w)),
                         std::max(1, scale(h)), scale(radius), color,
                         static_cast<std::uint8_t>(std::clamp(opacity, 0, 255))};
    };
    if (kind_ == 2) {
      for (const auto& p : particles_) {
        const int age = (ticks_ + p.phase) % 192;
        const int distance = 24 + age * age / 64;
        const int radius = 1 + age / 80;
        const int x = 120 + wave(ticks_ / 2) / 16 + p.x * distance / 256;
        const int y = 120 + wave(ticks_ / 3 + 64) / 16 + p.y * distance / 256;
        const int opacity = std::min({age, 191 - age, 24}) * 190 / 24;
        add(x - radius, y - radius, radius * 2, radius * 2, radius,
            p.phase & 1 ? 0xADBDDB : 0xD9E5EF, opacity);
      }
    } else if (kind_ == 3) {
      for (unsigned i = 0; i < 8; ++i) {
        const auto& p = particles_[i];
        const int x = 120 + p.x * 2 / 3 + wave(ticks_ + p.phase) / 5;
        const int y = 120 + p.y * 2 / 3 + wave(ticks_ / 2 + p.phase + 64) / 5;
        const int brightness = 65 + (wave(ticks_ * 2 + p.phase) + 128) * 150 / 256;
        const int radius = 5 + (wave(ticks_ + p.phase) + 128) / 128;
        const std::uint32_t color = i & 1 ? 0xDDE892 : 0xAEE5B1;
        add(x - radius, y - radius, radius * 2, radius * 2, radius, color, brightness / 9);
        add(x - 2, y - 2, 4, 4, 2, color, brightness);
      }
    } else {
      for (unsigned i = 0; i < 2; ++i) {
        const int x = (ticks_ * (i == 0 ? 2 : 1) + particles_[i].phase) % 352 - 56;
        const int y = (i == 0 ? 91 : 157) + wave(ticks_ * 2 + particles_[i].phase) / 10;
        const int opacity = std::clamp(std::min(x + 40, 280 - x) * 8, 0, 190);
        const std::uint32_t body = i == 0 ? 0xE2AA79 : 0x80BEC8;
        const std::uint32_t fin = i == 0 ? 0xB37D68 : 0x6598B5;
        const auto fish = [&](int dx, int dy, int w, int h, int r, std::uint32_t color) {
          const int left = x + dx;
          add(i == 0 ? left : 240 - left - w, y + dy, w, h, r, color, opacity);
        };
        fish(-23, -8 + wave(ticks_ * 8) / 64, 12, 17, 3, fin);
        fish(-10, -13, 10, 6, 3, fin);
        fish(-8, 6, 9, 6, 3, fin);
        fish(-17, -9, 34, 19, 10, body);
        fish(8, -4, 5, 5, 3, 0xE7E7DD);
        fish(10, -3, 2, 2, 1, 0x16222A);
        fish(2, -2, 2, 8, 1, fin);
      }
      for (unsigned i = 0; i < 4; ++i) {
        const int age = (ticks_ + i * 61) & 255;
        const int x = 44 + i * 48 + wave(ticks_ + i * 43) / 16;
        const int y = 248 - age;
        const int opacity = std::min({age, 255 - age, 32}) * 90 / 32;
        add(x, y, 7, 7, 4, 0x76AFBC, opacity);
        add(x + 1, y + 1, 5, 5, 3, 0, 255);
      }
    }
    return shapes;
  }

 private:
  struct Particle { int x, y, phase; };
  std::array<Particle, 20> particles_{};
  std::uint32_t random_ = 1;
  int size_ = 240, ticks_ = 0;
  std::uint8_t kind_ = 2;
  unsigned next() {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_;
  }
};

}  // namespace printdeck::core
