#pragma once

#include "captions/model_files.hpp"

#include <string>
#include <vector>

namespace cfx {

class StreamingRecognizer {
 public:
  StreamingRecognizer(std::string model, std::string tokens);
  ~StreamingRecognizer();

  StreamingRecognizer(const StreamingRecognizer&) = delete;
  StreamingRecognizer& operator=(const StreamingRecognizer&) = delete;

  bool ok() const;
  void accept(int sampleRate, const float* samples, int count);
  void decode();
  std::string text() const;
  bool isEndpoint() const;
  void reset();
  void restart();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

class PhraseRefiner {
 public:
  PhraseRefiner(std::string model, std::string tokens);
  ~PhraseRefiner();

  PhraseRefiner(const PhraseRefiner&) = delete;
  PhraseRefiner& operator=(const PhraseRefiner&) = delete;

  bool ok() const;
  std::string refine(int sampleRate, const float* samples, int count);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

class VoiceSpeaker {
 public:
  explicit VoiceSpeaker(VoicePaths voice);
  ~VoiceSpeaker();

  VoiceSpeaker(const VoiceSpeaker&) = delete;
  VoiceSpeaker& operator=(const VoiceSpeaker&) = delete;

  bool ok() const;
  bool ownVoice() const;
  std::vector<float> synthesize(const std::string& text, int& sampleRate) const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace cfx
