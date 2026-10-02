#include "audio/pitch_shifter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

float goertzel(const float* samples, int count, float freq, float sampleRate) {
  const float pi = 3.14159265358979323846f;
  const float coeff = 2.f * std::cos(2.f * pi * freq / sampleRate);
  float s0 = 0.f;
  float s1 = 0.f;
  float s2 = 0.f;
  for (int i = 0; i < count; ++i) {
    s0 = samples[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

int peakHz(const float* samples, int count, int sampleRate, int from, int to) {
  float best = -1.f;
  int hzAt = from;
  for (int hz = from; hz <= to; ++hz) {
    const float power = goertzel(samples, count, static_cast<float>(hz), static_cast<float>(sampleRate));
    if (power > best) {
      best = power;
      hzAt = hz;
    }
  }
  return hzAt;
}

void render(cfx::PitchShifter& shifter, float freq, std::vector<float>& out) {
  const int sampleRate = 48000;
  const int hop = 480;
  int written = 0;
  while (written < static_cast<int>(out.size())) {
    const int count = std::min(hop, static_cast<int>(out.size()) - written);
    for (int i = 0; i < count; ++i) {
      const int t = written + i;
      out[static_cast<size_t>(written + i)] = 0.5f * std::sin(2.f * 3.14159265f * freq * t / sampleRate);
    }
    shifter.process(out.data() + written, count);
    written += count;
  }
}

}  // namespace

int main() {
  constexpr int kRate = 48000;
  constexpr float kFreq = 440.f;
  constexpr float kSemis = 5.f;
  const float expected = kFreq * std::pow(2.f, kSemis / 12.f);

  cfx::PitchShifter shifter;
  shifter.prepare(kRate);
  if (shifter.latencySamples() > kRate * 30 / 1000) {
    std::printf("latency %d samples exceeds 30 ms\n", shifter.latencySamples());
    return 1;
  }
  shifter.setEnabled(true);
  shifter.setMix(1.f);
  shifter.setPitchSemitones(kSemis);
  shifter.setFormantSemitones(0.f);

  std::vector<float> shifted(kRate);
  render(shifter, kFreq, shifted);
  const int skip = kRate / 4;
  const int count = kRate - skip;
  const int peak = peakHz(shifted.data() + skip, count, kRate, 300, 900);
  int crossings = 0;
  float previousSample = shifted[static_cast<size_t>(skip)];
  for (int i = 1; i < count; ++i) {
    const float sample = shifted[static_cast<size_t>(skip + i)];
    if ((previousSample <= 0.f && sample > 0.f) || (previousSample >= 0.f && sample < 0.f)) ++crossings;
    previousSample = sample;
  }
  const float crossingHz = crossings * static_cast<float>(kRate) / (2.f * static_cast<float>(count));
  const float atExpected = goertzel(shifted.data() + skip, count, expected, static_cast<float>(kRate));
  const float atInput = goertzel(shifted.data() + skip, count, kFreq, static_cast<float>(kRate));
  const float atPeak = goertzel(shifted.data() + skip, count, static_cast<float>(peak), static_cast<float>(kRate));
  double energy = 0.0;
  for (int i = 0; i < count; ++i) energy += shifted[static_cast<size_t>(skip + i)] * shifted[static_cast<size_t>(skip + i)];
  std::printf("shifted peak %d Hz, crossings %.2f Hz, expected %.2f\n", peak, crossingHz, expected);
  std::printf("power expected %.3e input %.3e peak %.3e rms %.3f\n", atExpected, atInput, atPeak,
              std::sqrt(energy / count));
  if (std::fabs(crossingHz - expected) > 8.f) return 1;
  if (atPeak < atInput * 20.f) return 1;

  cfx::PitchShifter unity;
  unity.prepare(kRate);
  unity.setEnabled(true);
  unity.setMix(1.f);
  unity.setPitchSemitones(0.f);
  unity.setFormantSemitones(0.f);
  std::vector<float> plain(kRate);
  render(unity, kFreq, plain);
  const int unityPeak = peakHz(plain.data() + skip, count, kRate, 200, 800);
  std::printf("unity peak %d Hz\n", unityPeak);
  if (std::abs(unityPeak - 440) > 3) return 1;

  cfx::PitchShifter ramp;
  ramp.prepare(kRate);
  ramp.setPitchSemitones(5.f);
  ramp.setFormantSemitones(4.f);
  ramp.setMix(1.f);
  ramp.setEnabled(false);
  float previous = 0.f;
  float maxJump = 0.f;
  for (int i = 0; i < kRate / 2; ++i) {
    float sample = 0.2f;
    if (i == kRate / 5) ramp.setEnabled(true);
    ramp.process(&sample, 1);
    if (i > kRate / 5) maxJump = std::max(maxJump, std::fabs(sample - previous));
    previous = sample;
  }
  std::printf("max step after enable %.4f\n", maxJump);
  if (maxJump > 0.08f) return 1;
  std::printf("pitch test ok, algorithmic latency %d samples\n", shifter.latencySamples());
  return 0;
}
