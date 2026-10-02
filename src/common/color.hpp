#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace cfx {

inline uint8_t clampByte(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

inline void scaleBgra(const uint8_t* src, int srcW, int srcH, int srcStride, uint8_t* dst, int dstW, int dstH,
                      int dstStride) {
  if (srcW == dstW && srcH == dstH) {
    for (int y = 0; y < dstH; ++y) {
      std::memcpy(dst + static_cast<size_t>(y) * dstStride, src + static_cast<size_t>(y) * srcStride,
                  static_cast<size_t>(dstW) * 4);
    }
    return;
  }
  for (int y = 0; y < dstH; ++y) {
    const float fy = (static_cast<float>(y) + 0.5f) * srcH / dstH - 0.5f;
    const int y0 = std::clamp(static_cast<int>(fy), 0, srcH - 1);
    const int y1 = std::min(y0 + 1, srcH - 1);
    const float ty = std::clamp(fy - y0, 0.f, 1.f);
    uint8_t* out = dst + static_cast<size_t>(y) * dstStride;
    for (int x = 0; x < dstW; ++x) {
      const float fx = (static_cast<float>(x) + 0.5f) * srcW / dstW - 0.5f;
      const int x0 = std::clamp(static_cast<int>(fx), 0, srcW - 1);
      const int x1 = std::min(x0 + 1, srcW - 1);
      const float tx = std::clamp(fx - x0, 0.f, 1.f);
      const uint8_t* p00 = src + static_cast<size_t>(y0) * srcStride + x0 * 4;
      const uint8_t* p10 = src + static_cast<size_t>(y0) * srcStride + x1 * 4;
      const uint8_t* p01 = src + static_cast<size_t>(y1) * srcStride + x0 * 4;
      const uint8_t* p11 = src + static_cast<size_t>(y1) * srcStride + x1 * 4;
      for (int c = 0; c < 4; ++c) {
        const float top = p00[c] + (p10[c] - p00[c]) * tx;
        const float bot = p01[c] + (p11[c] - p01[c]) * tx;
        out[x * 4 + c] = static_cast<uint8_t>(top + (bot - top) * ty + 0.5f);
      }
    }
  }
}

inline void mirrorBgra(uint8_t* bgra, int width, int height, int stride) {
  for (int y = 0; y < height; ++y) {
    uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width / 2; ++x) {
      uint8_t* a = row + x * 4;
      uint8_t* b = row + (width - 1 - x) * 4;
      for (int c = 0; c < 4; ++c) std::swap(a[c], b[c]);
    }
  }
}

// Studio-range BT.601. Y plane first, then interleaved UV.
inline void bgraToNv12(const uint8_t* bgra, int width, int height, int stride, uint8_t* dst) {
  uint8_t* yPlane = dst;
  uint8_t* uvPlane = dst + static_cast<size_t>(width) * height;
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      const int b = row[x * 4 + 0];
      const int g = row[x * 4 + 1];
      const int r = row[x * 4 + 2];
      yPlane[y * width + x] = clampByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    }
  }
  for (int y = 0; y < height; y += 2) {
    for (int x = 0; x < width; x += 2) {
      int u = 0;
      int v = 0;
      const int y1 = std::min(y + 1, height - 1);
      const int x1 = std::min(x + 1, width - 1);
      const int xs[2] = {x, x1};
      const int ys[2] = {y, y1};
      for (int yy : ys) {
        for (int xx : xs) {
          const uint8_t* p = bgra + static_cast<size_t>(yy) * stride + xx * 4;
          const int b = p[0];
          const int g = p[1];
          const int r = p[2];
          u += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
          v += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
        }
      }
      uint8_t* uv = uvPlane + static_cast<size_t>(y / 2) * width + x;
      uv[0] = clampByte(u / 4);
      uv[1] = clampByte(v / 4);
    }
  }
}

inline void bgraToYuy2(const uint8_t* bgra, int width, int height, int stride, uint8_t* dst) {
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    uint8_t* out = dst + static_cast<size_t>(y) * width * 2;
    for (int x = 0; x < width; x += 2) {
      const int x1 = std::min(x + 1, width - 1);
      auto yuv = [&](int px, int& Y, int& U, int& V) {
        const int b = row[px * 4 + 0];
        const int g = row[px * 4 + 1];
        const int r = row[px * 4 + 2];
        Y = clampByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        U = clampByte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
        V = clampByte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
      };
      int y0, u0, v0, y1, u1, v1;
      yuv(x, y0, u0, v0);
      yuv(x1, y1, u1, v1);
      out[0] = static_cast<uint8_t>(y0);
      out[1] = static_cast<uint8_t>((u0 + u1) / 2);
      out[2] = static_cast<uint8_t>(y1);
      out[3] = static_cast<uint8_t>((v0 + v1) / 2);
      out += 4;
    }
  }
}

inline void nv12ToBgra(const uint8_t* src, int width, int height, int yStride, uint8_t* dst, int dstStride) {
  const uint8_t* yPlane = src;
  const uint8_t* uvPlane = src + static_cast<size_t>(yStride) * height;
  const int uvStride = yStride;
  for (int y = 0; y < height; ++y) {
    uint8_t* out = dst + static_cast<size_t>(y) * dstStride;
    const uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * uvStride;
    for (int x = 0; x < width; ++x) {
      const int Y = static_cast<int>(yPlane[y * yStride + x]) - 16;
      const int U = static_cast<int>(uvRow[(x & ~1) + 0]) - 128;
      const int V = static_cast<int>(uvRow[(x & ~1) + 1]) - 128;
      const int c = std::max(0, Y);
      out[x * 4 + 2] = clampByte((298 * c + 409 * V + 128) >> 8);
      out[x * 4 + 1] = clampByte((298 * c - 100 * U - 208 * V + 128) >> 8);
      out[x * 4 + 0] = clampByte((298 * c + 516 * U + 128) >> 8);
      out[x * 4 + 3] = 255;
    }
  }
}

inline void yuy2ToBgra(const uint8_t* src, int width, int height, int srcStride, uint8_t* dst, int dstStride) {
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = src + static_cast<size_t>(y) * srcStride;
    uint8_t* out = dst + static_cast<size_t>(y) * dstStride;
    for (int x = 0; x < width; x += 2) {
      const int y0 = static_cast<int>(row[0]) - 16;
      const int u = static_cast<int>(row[1]) - 128;
      const int y1 = static_cast<int>(row[2]) - 16;
      const int v = static_cast<int>(row[3]) - 128;
      auto store = [&](int px, int Y) {
        const int c = std::max(0, Y);
        out[px * 4 + 2] = clampByte((298 * c + 409 * v + 128) >> 8);
        out[px * 4 + 1] = clampByte((298 * c - 100 * u - 208 * v + 128) >> 8);
        out[px * 4 + 0] = clampByte((298 * c + 516 * u + 128) >> 8);
        out[px * 4 + 3] = 255;
      };
      store(x, y0);
      if (x + 1 < width) store(x + 1, y1);
      row += 4;
    }
  }
}

}  // namespace cfx
