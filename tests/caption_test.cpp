#include "captions/model_files.hpp"
#include "captions/recognizers.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool readWav(const std::string& path, std::vector<float>& samples, int& rate) {
  const int wideSize = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  std::wstring wide(wideSize > 1 ? static_cast<size_t>(wideSize) : 1, L'\0');
  if (wideSize > 1) {
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wideSize);
    wide.resize(static_cast<size_t>(wideSize - 1));
  }
  std::ifstream file(std::filesystem::path(wide), std::ios::binary);
  if (!file) return false;
  char header[12]{};
  file.read(header, 12);
  if (std::string(header, 4) != "RIFF" || std::string(header + 8, 4) != "WAVE") return false;
  int channels = 0;
  int bits = 0;
  rate = 0;
  std::vector<char> data;
  while (file) {
    char chunk[8]{};
    file.read(chunk, 8);
    if (!file) break;
    const uint32_t size = static_cast<unsigned char>(chunk[4]) | (static_cast<unsigned char>(chunk[5]) << 8) |
                          (static_cast<unsigned char>(chunk[6]) << 16) | (static_cast<unsigned char>(chunk[7]) << 24);
    std::vector<char> body(size);
    file.read(body.data(), size);
    if (std::string(chunk, 4) == "fmt " && body.size() >= 16) {
      channels = static_cast<unsigned char>(body[2]) | (static_cast<unsigned char>(body[3]) << 8);
      rate = static_cast<unsigned char>(body[4]) | (static_cast<unsigned char>(body[5]) << 8) |
             (static_cast<unsigned char>(body[6]) << 16) | (static_cast<unsigned char>(body[7]) << 24);
      bits = static_cast<unsigned char>(body[14]) | (static_cast<unsigned char>(body[15]) << 8);
    } else if (std::string(chunk, 4) == "data") {
      data = std::move(body);
    }
    if (size % 2) file.ignore(1);
  }
  if (data.empty() || channels <= 0 || rate <= 0 || bits != 16) return false;
  const int frames = static_cast<int>(data.size() / static_cast<size_t>(channels * 2));
  samples.resize(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    float sum = 0.f;
    for (int c = 0; c < channels; ++c) {
      const size_t at = static_cast<size_t>(i * channels + c) * 2;
      const int16_t pcm = static_cast<int16_t>(static_cast<unsigned char>(data[at]) |
                                               (static_cast<unsigned char>(data[at + 1]) << 8));
      sum += pcm / 32768.f;
    }
    samples[static_cast<size_t>(i)] = sum / static_cast<float>(channels);
  }
  return true;
}

bool hasCyrillic(const std::string& text) {
  for (unsigned char ch : text) {
    if (ch == 0xD0 || ch == 0xD1) return true;
  }
  return false;
}

bool hasPunctuationOrCapital(const std::string& text) {
  if (text.find_first_of(".,!?;:") != std::string::npos) return true;
  if (text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xD0) {
    const unsigned char second = static_cast<unsigned char>(text[1]);
    if (second >= 0x90 && second <= 0xAF) return true;
  }
  return false;
}

}  // namespace

int main() {
  const cfx::AsrPaths models = cfx::findAsrModels(cfx::exeDirectory());
  if (models.toneModel.empty() || models.toneWav.empty() || models.gigaModel.empty()) {
    std::cerr << "ASR models were not copied next to caption_test\n";
    return 1;
  }
  std::vector<float> samples;
  int rate = 0;
  if (!readWav(models.toneWav, samples, rate)) {
    std::cerr << "Cannot read " << models.toneWav << "\n";
    return 1;
  }

  cfx::StreamingRecognizer tone(models.toneModel, models.toneTokens);
  if (!tone.ok()) {
    std::cerr << "T-one model did not open\n";
    return 1;
  }
  const auto started = std::chrono::steady_clock::now();
  const int chunk = std::max(1, rate / 100);
  std::vector<float> pad(static_cast<size_t>(rate * 3 / 10), 0.f);
  tone.accept(rate, pad.data(), static_cast<int>(pad.size()));
  tone.decode();
  for (int at = 0; at < static_cast<int>(samples.size()); at += chunk) {
    const int count = std::min(chunk, static_cast<int>(samples.size()) - at);
    tone.accept(rate, samples.data() + at, count);
    tone.decode();
  }
  std::vector<float> tail(static_cast<size_t>(rate * 6 / 10), 0.f);
  tone.accept(rate, tail.data(), static_cast<int>(tail.size()));
  tone.decode();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  const std::string text = tone.text();
  const double audio = static_cast<double>(samples.size()) / static_cast<double>(rate);
  std::cout << "tone: " << text << "\nrtf " << (audio > 0 ? seconds / audio : 0) << "\n";
  if (!hasCyrillic(text)) {
    std::cerr << "T-one returned no Cyrillic text\n";
    return 1;
  }
#ifdef NDEBUG
  const double limit = 0.30;
#else
  const double limit = 1.5;
#endif
  if (audio > 0 && seconds > audio * limit) {
    std::cerr << "T-one was slower than " << limit << " of the audio\n";
    return 1;
  }

  cfx::PhraseRefiner refiner(models.gigaModel, models.gigaTokens);
  if (!refiner.ok()) {
    std::cerr << "GigaAM model did not open\n";
    return 1;
  }
  const std::string refined = refiner.refine(rate, samples.data(), static_cast<int>(samples.size()));
  std::cout << "gigaam: " << refined << "\n";
  if (!hasCyrillic(refined) || !hasPunctuationOrCapital(refined)) {
    std::cerr << "GigaAM did not return punctuated Cyrillic text\n";
    return 1;
  }
  return 0;
}
