#pragma once

namespace cfx {

// Dual-saw delay pitch shifter. Formants ride up with the pitch, which is the
// cartoon color; three peaking filters push them further. Latency is one grain
// (20 ms at the prepared sample rate).
class PitchShifter {
 public:
  void prepare(int sampleRate);
  void setPitchSemitones(float semitones);
  void setFormantSemitones(float semitones);
  void setMix(float mix);
  void setEnabled(bool enabled);
  void process(float* samples, int count);

  int latencySamples() const { return grain_; }
  int sampleRate() const { return sampleRate_; }

 private:
  void updateFilters();
  float readAt(double position) const;

  int sampleRate_ = 48000;
  int grain_ = 960;
  int mask_ = 16383;
  float* buffer_ = nullptr;
  int bufferCapacity_ = 0;
  long long write_ = 0;
  double readPos_ = 0.0;
  float fade_ = 0.f;
  float ratio_ = 1.f;
  float ratioTarget_ = 1.f;
  float mix_ = 1.f;
  float mixTarget_ = 1.f;
  float formantTarget_ = 0.f;
  float formantApplied_ = -1000.f;
  bool enabled_ = true;
  bool filtersActive_ = false;

  struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    void setPeak(float sampleRate, float freq, float q, float gainDb);
    float process(float x);
    void reset() { z1 = z2 = 0; }
  };
  Biquad peaks_[3];

  float storage_[16384]{};
};

}  // namespace cfx
