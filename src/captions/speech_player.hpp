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
  void setRouteToCall(bool enabled);
  bool speaking() const;
  bool ownVoice() const;
  std::string status() const;
  std::string menuLabel() const;

 private:
  void threadMain();

  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace cfx
