#pragma once

#include <atomic>
#include <cstdint>

namespace cfx {

// Cartoon is the default product preset. Off is the baseline path with
// neither the pitch shifter nor the pupil paint.
enum class Preset : int {
  Off = 0,
  Cartoon = 1,
  Pupils = 2,
  Custom = 3,
};

struct Params {
  std::atomic<float> pitchSemitones{5.f};
  std::atomic<float> formantSemitones{4.f};
  std::atomic<float> voiceMix{1.f};
  std::atomic<int> voiceEnabled{1};
  std::atomic<int> pupilsEnabled{1};
  std::atomic<int> mirror{1};
  std::atomic<int> monitor{1};
  std::atomic<int> lipExtraMs{0};
  std::atomic<int> preset{static_cast<int>(Preset::Cartoon)};

  std::atomic<float> videoProcessMs{0.f};
  std::atomic<float> framePeriodMs{33.f};
  std::atomic<float> audioTotalMs{0.f};
  std::atomic<float> audioAlgoMs{0.f};
  std::atomic<float> wasapiMs{0.f};
  std::atomic<int> faceFound{0};
  std::atomic<float> captureFps{0.f};

  void applyPreset(Preset presetValue) {
    preset.store(static_cast<int>(presetValue));
    if (presetValue == Preset::Off) {
      voiceEnabled.store(0);
      pupilsEnabled.store(0);
      voiceMix.store(0.f);
    } else if (presetValue == Preset::Cartoon) {
      voiceEnabled.store(1);
      pupilsEnabled.store(1);
      pitchSemitones.store(5.f);
      formantSemitones.store(4.f);
      voiceMix.store(1.f);
    } else if (presetValue == Preset::Pupils) {
      voiceEnabled.store(0);
      pupilsEnabled.store(1);
      voiceMix.store(0.f);
    }
  }
};

}  // namespace cfx
