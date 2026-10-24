#include "captions/model_files.hpp"
#include "captions/recognizers.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

int main() {
  const cfx::VoicePaths voice = cfx::findVoiceModel(cfx::exeDirectory());
  if (voice.model.empty()) {
    std::cerr << "Piper voice was not copied next to speech_test\n";
    return 1;
  }
  cfx::VoiceSpeaker speaker(voice);
  if (!speaker.ok()) {
    std::cerr << "Voice model did not open\n";
    return 1;
  }
  int rate = 0;
  const std::vector<float> samples = speaker.synthesize("Проверка голоса.", rate);
  std::cout << "samples " << samples.size() << " rate " << rate << " own " << speaker.ownVoice() << "\n";
  if (samples.size() < static_cast<size_t>(rate / 5) || rate < 8000) {
    std::cerr << "Synthesis returned too little audio\n";
    return 1;
  }
  double energy = 0.0;
  int clipped = 0;
  for (float sample : samples) {
    if (!std::isfinite(sample)) {
      std::cerr << "Synthesis contained a non-finite sample\n";
      return 1;
    }
    if (sample > 1.05f || sample < -1.05f) ++clipped;
    energy += static_cast<double>(sample) * sample;
  }
  if (energy < 0.01) {
    std::cerr << "Synthesis was silent\n";
    return 1;
  }
  if (clipped * 100 > static_cast<int>(samples.size())) {
    std::cerr << "Synthesis clipped more than one percent of samples\n";
    return 1;
  }
  return 0;
}
