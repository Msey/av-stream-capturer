#include "captions/model_files.hpp"

#include <filesystem>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace cfx {
namespace {

namespace fs = std::filesystem;

template <typename Visit>
bool walkFiles(const fs::path& root, Visit visit) {
  std::error_code error;
  if (root.empty() || !fs::exists(root, error) || error) return false;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
  if (error) return false;
  const fs::recursive_directory_iterator end;
  while (it != end) {
    if (visit(*it)) return true;
    it.increment(error);
    if (error) return false;
  }
  return false;
}

fs::path findTaggedFile(const fs::path& root, const wchar_t* directoryTag, const std::wstring& name) {
  fs::path found;
  walkFiles(root, [&](const fs::directory_entry& entry) {
    std::error_code error;
    if (!entry.is_regular_file(error) || error || entry.path().filename() != name) return false;
    if (entry.path().wstring().find(directoryTag) == std::wstring::npos) return false;
    found = entry.path();
    return true;
  });
  return found;
}

fs::path findDir(const fs::path& root, const std::wstring& name) {
  fs::path found;
  walkFiles(root, [&](const fs::directory_entry& entry) {
    std::error_code error;
    if (!entry.is_directory(error) || error || entry.path().filename() != name) return false;
    found = entry.path();
    return true;
  });
  return found;
}

fs::path findOnnx(const fs::path& root, bool wantRuslan) {
  fs::path fallback;
  walkFiles(root, [&](const fs::directory_entry& entry) {
    std::error_code error;
    if (!entry.is_regular_file(error) || error) return false;
    const std::wstring name = entry.path().filename().wstring();
    if (name.size() < 5 || name.substr(name.size() - 5) != L".onnx") return false;
    const bool ruslan = name.find(L"ruslan") != std::wstring::npos;
    if (wantRuslan && ruslan) {
      fallback = entry.path();
      return true;
    }
    if (!wantRuslan && fallback.empty()) fallback = entry.path();
    return false;
  });
  return fallback;
}

std::string utf8Path(const fs::path& path) {
  if (path.empty()) return {};
  return wideToUtf8(path.wstring());
}

// espeak-ng opens this directory with a narrow path. A short path stays ASCII
// when the program lives in a folder whose name is not Latin.
std::wstring shortPath(const std::wstring& path) {
  if (path.empty()) return {};
  DWORD size = GetShortPathNameW(path.c_str(), nullptr, 0);
  if (size == 0) return path;
  std::wstring out(size, L'\0');
  size = GetShortPathNameW(path.c_str(), out.data(), size);
  if (size == 0) return path;
  out.resize(size);
  return out;
}

}  // namespace

std::wstring exeDirectory() {
  std::wstring text(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, text.data(), static_cast<DWORD>(text.size()));
  if (length == 0 || length >= text.size()) return {};
  text.resize(length);
  const auto slash = text.find_last_of(L"\\/");
  if (slash != std::wstring::npos) text.resize(slash);
  return text;
}

std::string wideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
  out.resize(static_cast<size_t>(size - 1));
  return out;
}

AsrPaths findAsrModels(const std::wstring& exeDir) {
  const fs::path root = fs::path(exeDir) / L"models" / L"asr";
  AsrPaths paths;
  const fs::path tone = findTaggedFile(root, L"t-one", L"model.onnx");
  const fs::path giga = findTaggedFile(root, L"giga", L"model.int8.onnx");
  paths.toneModel = utf8Path(tone);
  paths.gigaModel = utf8Path(giga);
  if (!tone.empty()) {
    paths.toneTokens = utf8Path(tone.parent_path() / L"tokens.txt");
    paths.toneWav = utf8Path(tone.parent_path() / L"0.wav");
  }
  if (!giga.empty()) paths.gigaTokens = utf8Path(giga.parent_path() / L"tokens.txt");
  return paths;
}

VoicePaths findVoiceModel(const std::wstring& exeDir) {
  const fs::path root = fs::path(exeDir) / L"models" / L"tts";
  const fs::path data = findDir(root, L"espeak-ng-data");
  VoicePaths voice;
  voice.dataDir = utf8Path(shortPath(data.wstring()));
  const fs::path ownDir = root / L"my_voice";
  const fs::path ownModel = findOnnx(ownDir, false);
  const fs::path ownTokens = ownModel.empty() ? fs::path() : ownModel.parent_path() / L"tokens.txt";
  std::error_code error;
  if (!ownModel.empty() && fs::exists(ownTokens, error) && !voice.dataDir.empty()) {
    voice.model = utf8Path(ownModel);
    voice.tokens = utf8Path(ownTokens);
    voice.own = true;
    return voice;
  }
  const fs::path ruslan = findOnnx(root, true);
  if (!ruslan.empty() && !voice.dataDir.empty()) {
    const fs::path tokens = ruslan.parent_path() / L"tokens.txt";
    if (fs::exists(tokens, error)) {
      voice.model = utf8Path(ruslan);
      voice.tokens = utf8Path(tokens);
    }
  }
  return voice;
}

}  // namespace cfx
