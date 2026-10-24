#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cfx {

struct AudioEndpoint {
  std::wstring id;
  std::wstring name;
  bool capture = false;
  bool cable = false;
  bool isDefault = false;
  bool active = true;
};

struct CameraDevice {
  std::wstring link;
  std::wstring name;
};

std::vector<AudioEndpoint> enumerateAudio(bool capture);
std::wstring defaultAudioId(bool capture);
bool looksLikeCable(const std::wstring& name);
bool isVirtualCablePlayback(const std::wstring& name);
bool isVirtualCableMicrophone(const std::wstring& name);
std::wstring virtualCablePlaybackId(const std::vector<AudioEndpoint>& outputs);
std::wstring virtualCableMicrophoneName(const std::vector<AudioEndpoint>& inputs);

std::vector<CameraDevice> enumerateCameras();

struct AudioApp {
  std::uint32_t pid = 0;
  std::wstring name;
};

std::vector<AudioApp> enumerateAudioApps();
std::wstring speakerId();

}  // namespace cfx
