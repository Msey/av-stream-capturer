#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace cfx {

// Sample-by-sample delay ramp so a lip-sync change does not click.
class DelayLine {
 public:
  void prepare(int sampleRate) {
    sampleRate_ = sampleRate;
    buffer_.assign(static_cast<size_t>(sampleRate) * 2, 0.f);
    write_ = 0;
    delay_ = 0;
    target_ = 0;
  }

  void setTargetSamples(int samples) {
    const int limit = buffer_.empty() ? 0 : static_cast<int>(buffer_.size()) - 1;
    target_ = std::clamp(samples, 0, limit);
  }

  int delaySamples() const { return delay_; }

  void process(float* samples, int count) {
    if (buffer_.empty()) return;
    const int size = static_cast<int>(buffer_.size());
    for (int i = 0; i < count; ++i) {
      if (delay_ < target_) ++delay_;
      else if (delay_ > target_) --delay_;
      buffer_[static_cast<size_t>(write_ % size)] = samples[i];
      const long long read = write_ - delay_;
      samples[i] = read < 0 ? 0.f : buffer_[static_cast<size_t>(read % size)];
      ++write_;
    }
  }

 private:
  int sampleRate_ = 48000;
  std::vector<float> buffer_;
  long long write_ = 0;
  int delay_ = 0;
  int target_ = 0;
};

}  // namespace cfx
