#pragma once

#include <string>
#include <vector>

namespace cfx {

struct AudioEndpoint {
  std::wstring id;
  std::wstring name;
  bool capture = false;
  bool cable = false;
  bool isDefault = false;
};

struct CameraDevice {
  std::wstring link;
  std::wstring name;
};

std::vector<AudioEndpoint> enumerateAudio(bool capture);
std::wstring defaultAudioId(bool capture);
bool looksLikeCable(const std::wstring& name);

std::vector<CameraDevice> enumerateCameras();

}  // namespace cfx
