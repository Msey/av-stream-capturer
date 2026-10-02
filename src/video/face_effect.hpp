#pragma once

#include <string>

namespace cfx {

struct FaceBox {
  float x1 = 0.f;
  float y1 = 0.f;
  float x2 = 0.f;
  float y2 = 0.f;
  float score = 0.f;
  bool found = false;
};

class FaceEffect {
 public:
  FaceEffect();
  ~FaceEffect();
  FaceEffect(const FaceEffect&) = delete;
  FaceEffect& operator=(const FaceEffect&) = delete;

  bool load(const std::wstring& modelPath);
  bool ready() const { return ready_; }
  const std::string& status() const { return status_; }

  // Paints white pupils into a top-down BGRA frame. Returns false when no face is found.
  bool apply(uint8_t* bgra, int width, int height, int stride, float dtSeconds, float& processMs);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool ready_ = false;
  std::string status_;
};

}  // namespace cfx
