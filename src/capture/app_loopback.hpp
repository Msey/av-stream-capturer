#pragma once

#include <cstdint>
#include <string>

namespace cfx {

// Captures audio rendered by other programs. exclude=true keeps every process except pid.
class AppLoopback {
 public:
  AppLoopback();
  ~AppLoopback();

  AppLoopback(const AppLoopback&) = delete;
  AppLoopback& operator=(const AppLoopback&) = delete;

  bool open(std::uint32_t pid, bool exclude, std::string& error);
  void close();
  int sampleRate() const;
  // Writes mono float samples. Returns the frame count, 0 on timeout, and -1 on failure.
  int read(float* dst, int maxFrames, int timeoutMs);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace cfx
