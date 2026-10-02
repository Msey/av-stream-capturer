#pragma once

#include "common/params.hpp"

#include <string>

namespace cfx {

class AudioEngine {
 public:
  explicit AudioEngine(Params& params);
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  void start(std::wstring microphoneId, std::wstring outputId);
  void setDevices(std::wstring microphoneId, std::wstring outputId);
  void stop();
  std::string status() const;

 private:
  void threadMain();

  Params& params_;
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace cfx
