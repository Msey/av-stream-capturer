#pragma once

#include <string>

namespace cfx {

struct AsrPaths {
  std::string toneModel;
  std::string toneTokens;
  std::string toneWav;
  std::string gigaModel;
  std::string gigaTokens;
};

struct VoicePaths {
  std::string model;
  std::string tokens;
  std::string dataDir;
  bool own = false;
};

std::wstring exeDirectory();
std::string wideToUtf8(const std::wstring& text);
AsrPaths findAsrModels(const std::wstring& exeDir);
VoicePaths findVoiceModel(const std::wstring& exeDir);

}  // namespace cfx
