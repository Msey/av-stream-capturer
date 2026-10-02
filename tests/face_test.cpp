#include "video/face_effect.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int main() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring model(path);
  const auto slash = model.find_last_of(L"\\/");
  if (slash != std::wstring::npos) model.resize(slash);
  model += L"\\models\\ultraface.onnx";

  cfx::FaceEffect effect;
  if (!effect.load(model)) {
    std::printf("load failed: %s\n", effect.status().c_str());
    return 1;
  }

  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  constexpr int kStride = kWidth * 4;
  std::vector<uint8_t> frame(static_cast<size_t>(kStride) * kHeight, 0);
  for (size_t i = 3; i < frame.size(); i += 4) frame[i] = 255;

  float lastMs = 0.f;
  bool found = false;
  for (int i = 0; i < 5; ++i) {
    const auto start = std::chrono::steady_clock::now();
    found = effect.apply(frame.data(), kWidth, kHeight, kStride, 0.033f, lastMs);
    const float wall = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("frame %d face=%d process=%.2f ms wall=%.2f ms\n", i, found ? 1 : 0, lastMs, wall);
  }
  if (found) {
    std::printf("black frame was treated as a face\n");
    return 1;
  }
  if (lastMs > 33.f) {
    std::printf("face pass is slower than one 30 fps frame\n");
    return 1;
  }
  std::printf("face test ok\n");
  return 0;
}
