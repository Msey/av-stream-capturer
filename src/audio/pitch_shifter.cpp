#include "audio/pitch_shifter.hpp"

#include <algorithm>
#include <cmath>

namespace cfx {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float semitoneRatio(float semitones) {
  return std::pow(2.f, std::clamp(semitones, -12.f, 12.f) / 12.f);
}

}  // namespace

void PitchShifter::Biquad::setPeak(float sampleRate, float freq, float q, float gainDb) {
  const float nyquist = sampleRate * 0.45f;
  freq = std::clamp(freq, 40.f, nyquist);
  const float A = std::pow(10.f, gainDb / 40.f);
  const float w0 = 2.f * kPi * freq / sampleRate;
  const float alpha = std::sin(w0) / (2.f * q);
  const float cosw = std::cos(w0);
  const float a0 = 1.f + alpha / A;
  b0 = (1.f + alpha * A) / a0;
  b1 = (-2.f * cosw) / a0;
  b2 = (1.f - alpha * A) / a0;
  a1 = (-2.f * cosw) / a0;
  a2 = (1.f - alpha / A) / a0;
}

float PitchShifter::Biquad::process(float x) {
  const float y = b0 * x + z1;
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  return y;
}

void PitchShifter::prepare(int sampleRate) {
  sampleRate_ = std::max(8000, sampleRate);
  grain_ = std::max(240, sampleRate_ / 50);
  if (grain_ > 4000) grain_ = 4000;
  buffer_ = storage_;
  bufferCapacity_ = static_cast<int>(sizeof(storage_) / sizeof(storage_[0]));
  mask_ = bufferCapacity_ - 1;
  std::fill(std::begin(storage_), std::end(storage_), 0.f);
  write_ = 0;
  readPos_ = -static_cast<double>(grain_);
  fade_ = 0.f;
  ratio_ = 1.f;
  ratioTarget_ = 1.f;
  mix_ = 0.f;
  mixTarget_ = 1.f;
  formantApplied_ = -1000.f;
  for (auto& peak : peaks_) peak.reset();
  updateFilters();
}

void PitchShifter::setPitchSemitones(float semitones) { ratioTarget_ = semitoneRatio(semitones); }

void PitchShifter::setFormantSemitones(float semitones) { formantTarget_ = semitones; }

void PitchShifter::setMix(float mix) { mixTarget_ = std::clamp(mix, 0.f, 1.f); }

void PitchShifter::setEnabled(bool enabled) { enabled_ = enabled; }

void PitchShifter::updateFilters() {
  const float semis = std::clamp(formantTarget_, 0.f, 12.f);
  formantApplied_ = formantTarget_;
  filtersActive_ = semis > 0.05f;
  if (!filtersActive_) {
    for (auto& peak : peaks_) peak.reset();
    return;
  }
  const float scale = std::pow(2.f, semis / 12.f);
  const float gain = semis * 0.8f;
  const float centers[3] = {650.f, 1400.f, 2800.f};
  for (int i = 0; i < 3; ++i) peaks_[i].setPeak(static_cast<float>(sampleRate_), centers[i] * scale, 1.2f, gain);
}

float PitchShifter::readAt(double position) const {
  const double size = static_cast<double>(mask_ + 1);
  double wrapped = std::fmod(position, size);
  if (wrapped < 0.0) wrapped += size;
  const int i0 = static_cast<int>(wrapped);
  const int i1 = (i0 + 1) & mask_;
  const float frac = static_cast<float>(wrapped - i0);
  return buffer_[i0] + (buffer_[i1] - buffer_[i0]) * frac;
}

void PitchShifter::process(float* samples, int count) {
  if (!buffer_ || count <= 0) return;
  if (std::fabs(formantApplied_ - formantTarget_) > 0.01f) updateFilters();
  const float mixGoal = enabled_ ? mixTarget_ : 0.f;
  const double grain = static_cast<double>(grain_);
  for (int i = 0; i < count; ++i) {
    buffer_[static_cast<size_t>(write_ & mask_)] = samples[i];
    ratio_ += std::clamp(ratioTarget_ - ratio_, -0.00005f, 0.00005f);
    mix_ += std::clamp(mixGoal - mix_, -0.0008f, 0.0008f);

    // The read head moves at the pitch ratio, then jumps back by the extra
    // distance so the output keeps the original duration. The crossfade hides the jump.
    const double jump = (static_cast<double>(ratio_) - 1.0) * grain;
    const float s1 = readAt(readPos_);
    const float s2 = readAt(readPos_ - jump);
    const float wetFade = std::clamp(fade_, 0.f, 1.f);
    float wet = s1 * (1.f - wetFade) + s2 * wetFade;
    if (filtersActive_) {
      wet = peaks_[0].process(wet);
      wet = peaks_[1].process(wet);
      wet = peaks_[2].process(wet);
    }
    const float dry = readAt(static_cast<double>(write_) - grain);
    samples[i] = dry * (1.f - mix_) + wet * mix_;

    readPos_ += ratio_;
    fade_ += 1.f / static_cast<float>(grain_);
    if (fade_ >= 1.f) {
      fade_ -= 1.f;
      readPos_ -= jump;
    }
    ++write_;
  }
}

}  // namespace cfx
