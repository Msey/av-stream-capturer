#include "video/pupils.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
  constexpr int kWidth = 200;
  constexpr int kHeight = 200;
  constexpr int kStride = kWidth * 4;
  std::vector<uint8_t> image(static_cast<size_t>(kStride) * kHeight, 180);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      uint8_t* pixel = image.data() + static_cast<size_t>(y) * kStride + x * 4;
      pixel[3] = 255;
      const float dx = static_cast<float>(x) - 80.f;
      const float dy = static_cast<float>(y) - 90.f;
      if (dx * dx + dy * dy <= 64.f) {
        pixel[0] = pixel[1] = pixel[2] = 12;
      }
    }
  }

  const cfx::Pupil pupil = cfx::locatePupil(image.data(), kStride, kWidth, kHeight, 50.f, 60.f, 60.f, 60.f);
  if (!pupil.ok) {
    std::printf("pupil not found\n");
    return 1;
  }
  std::printf("pupil %.1f %.1f r %.1f\n", pupil.x, pupil.y, pupil.r);
  if (std::fabs(pupil.x - 80.f) > 4.f || std::fabs(pupil.y - 90.f) > 4.f) return 1;

  cfx::paintPupil(image.data(), kStride, kWidth, kHeight, pupil.x, pupil.y, pupil.r);
  const uint8_t* center = image.data() + 90 * kStride + 80 * 4;
  std::printf("center %u %u %u\n", center[0], center[1], center[2]);
  if (center[0] < 240 || center[1] < 240 || center[2] < 240) return 1;

  std::vector<uint8_t> blank(static_cast<size_t>(kStride) * kHeight, 210);
  const cfx::Pupil none = cfx::locatePupil(blank.data(), kStride, kWidth, kHeight, 20.f, 20.f, 50.f, 40.f);
  if (none.ok) {
    std::printf("false pupil on a flat frame\n");
    return 1;
  }
  std::printf("pupil test ok\n");
  return 0;
}
