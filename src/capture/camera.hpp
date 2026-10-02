#pragma once

#include "common/params.hpp"
#include "common/shared_frame.hpp"

#include <string>
#include <vector>

namespace cfx {

struct PreviewFrame {
  std::vector<uint8_t> bgra;
  int width = 0;
  int height = 0;
  float processMs = 0.f;
  bool face = false;
};

class CameraCapture {
 public:
  CameraCapture(Params& params, SharedFrames& shared);
  ~CameraCapture();

  CameraCapture(const CameraCapture&) = delete;
  CameraCapture& operator=(const CameraCapture&) = delete;

  void start(std::wstring symbolicLink);
  void setDevice(std::wstring symbolicLink);
  void stop();
  bool copyPreview(PreviewFrame& frame) const;
  std::string status() const;

 private:
  void threadMain();

  Params& params_;
  SharedFrames& shared_;
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace cfx
