#pragma once

#include <string>

namespace cfx {

class SpeechPlayer {
 public:
  SpeechPlayer();
  ~SpeechPlayer();

  SpeechPlayer(const SpeechPlayer&) = delete;
  SpeechPlayer& operator=(const SpeechPlayer&) = delete;

  void speak(std::string text);
  void stop();
  bool speaking() const;
  std::string status() const;
  std::string menuLabel() const;

 private:
  void threadMain();

  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace cfx
