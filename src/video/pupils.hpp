#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cfx {

struct Pupil {
  float x = 0.f;
  float y = 0.f;
  float r = 0.f;
  bool ok = false;
};

// One Euro filter keeps the painted pupil from jittering on a noisy detector.
class OneEuro {
 public:
  void reset() { ready_ = false; }

  float filter(float value, float dtSeconds) {
    if (!ready_ || dtSeconds <= 0.f || dtSeconds > 0.25f) {
      ready_ = true;
      x_ = value;
      dx_ = 0.f;
      return value;
    }
    const float dx = (value - x_) / dtSeconds;
    const float aD = alpha(1.f, dtSeconds);
    dx_ = aD * dx + (1.f - aD) * dx_;
    const float cutoff = 1.2f + 0.04f * std::fabs(dx_);
    const float a = alpha(cutoff, dtSeconds);
    x_ = a * value + (1.f - a) * x_;
    return x_;
  }

 private:
  static float alpha(float cutoff, float dt) {
    const float tau = 1.f / (2.f * 3.14159265f * cutoff);
    return 1.f / (1.f + tau / dt);
  }

  bool ready_ = false;
  float x_ = 0.f;
  float dx_ = 0.f;
};

inline int lumaOf(const uint8_t* pixel) {
  return (pixel[2] * 54 + pixel[1] * 183 + pixel[0] * 19) >> 8;
}

// Dark blob inside the eye rectangle. ok == false asks the caller to fall back
// to the geometric center of that rectangle.
inline Pupil locatePupil(const uint8_t* bgra, int stride, int imgW, int imgH, float rx, float ry, float rw, float rh) {
  Pupil pupil;
  if (!bgra || rw < 4.f || rh < 4.f) return pupil;
  const int x0 = std::clamp(static_cast<int>(rx), 0, imgW - 1);
  const int y0 = std::clamp(static_cast<int>(ry), 0, imgH - 1);
  const int x1 = std::clamp(static_cast<int>(rx + rw), x0 + 1, imgW);
  const int y1 = std::clamp(static_cast<int>(ry + rh), y0 + 1, imgH);

  double sum = 0.0;
  int count = 0;
  for (int y = y0; y < y1; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (int x = x0; x < x1; ++x) {
      sum += lumaOf(row + x * 4);
      ++count;
    }
  }
  if (count == 0) return pupil;
  const float mean = static_cast<float>(sum / count);
  const int threshold = static_cast<int>(mean * 0.55f);

  double sx = 0.0;
  double sy = 0.0;
  int dark = 0;
  for (int y = y0; y < y1; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (int x = x0; x < x1; ++x) {
      if (lumaOf(row + x * 4) <= threshold) {
        sx += x;
        sy += y;
        ++dark;
      }
    }
  }
  if (dark < 6) return pupil;
  pupil.x = static_cast<float>(sx / dark);
  pupil.y = static_cast<float>(sy / dark);
  float radius = std::sqrt(static_cast<float>(dark) / 3.14159265f);
  radius = std::clamp(radius, rw * 0.08f, rw * 0.28f);
  pupil.r = radius;
  pupil.ok = true;
  return pupil;
}

inline void paintPupil(uint8_t* bgra, int stride, int imgW, int imgH, float cx, float cy, float radius) {
  if (!bgra || radius < 1.f) return;
  const float feather = 2.f;
  const int x0 = std::clamp(static_cast<int>(std::floor(cx - radius - feather)), 0, imgW - 1);
  const int y0 = std::clamp(static_cast<int>(std::floor(cy - radius - feather)), 0, imgH - 1);
  const int x1 = std::clamp(static_cast<int>(std::ceil(cx + radius + feather)), 0, imgW);
  const int y1 = std::clamp(static_cast<int>(std::ceil(cy + radius + feather)), 0, imgH);
  for (int y = y0; y < y1; ++y) {
    uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (int x = x0; x < x1; ++x) {
      const float dx = static_cast<float>(x) - cx;
      const float dy = static_cast<float>(y) - cy;
      const float dist = std::sqrt(dx * dx + dy * dy);
      float alpha = 0.f;
      if (dist <= radius) alpha = 1.f;
      else if (dist < radius + feather) alpha = 1.f - (dist - radius) / feather;
      if (alpha <= 0.f) continue;
      uint8_t* p = row + x * 4;
      p[0] = static_cast<uint8_t>(p[0] + (255.f - p[0]) * alpha);
      p[1] = static_cast<uint8_t>(p[1] + (255.f - p[1]) * alpha);
      p[2] = static_cast<uint8_t>(p[2] + (255.f - p[2]) * alpha);
    }
  }
}

}  // namespace cfx
