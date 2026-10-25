#include "captions/speech_player.hpp"

#include "captions/model_files.hpp"
#include "captions/recognizers.hpp"
#include "capture/devices.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <stdio.h>

namespace cfx {
namespace {

const GUID kIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

struct Format {
  int rate = 48000;
  int channels = 2;
  int bits = 32;
  bool isFloat = true;
};

char32_t codepointAt(const std::string& text, size_t index) {
  if (index >= text.size()) return 0;
  const unsigned char lead = static_cast<unsigned char>(text[index]);
  int length = 1;
  char32_t value = lead;
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    value = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    value = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    value = lead & 0x07;
  }
  if (index + static_cast<size_t>(length) > text.size()) return lead;
  for (int i = 1; i < length; ++i) {
    value = (value << 6) | (static_cast<unsigned char>(text[index + static_cast<size_t>(i)]) & 0x3F);
  }
  return value;
}

bool isLetter(char32_t value) {
  if ((value >= U'0' && value <= U'9') || (value >= U'A' && value <= U'Z') || (value >= U'a' && value <= U'z')) return true;
  return value >= 0x0400 && value <= 0x04FF;
}

bool letterBefore(const std::string& text, size_t index) {
  if (index == 0 || index > text.size()) return false;
  size_t at = index - 1;
  while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
  return isLetter(codepointAt(text, at));
}

void trimUtf8(std::string& text) {
  while (!text.empty()) {
    size_t at = text.size() - 1;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
    const unsigned char lead = static_cast<unsigned char>(text[at]);
    int need = 1;
    if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    if ((lead & 0xC0) != 0x80 && text.size() - at >= static_cast<size_t>(need)) return;
    text.resize(at);
  }
}

std::string applyStress(const std::string& text, const std::wstring& dictionaryPath) {
  FILE* file = nullptr;
  if (_wfopen_s(&file, dictionaryPath.c_str(), L"rb") != 0 || !file) return text;
  std::string raw;
  char chunk[1024];
  size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) raw.append(chunk, got);
  std::fclose(file);
  std::vector<std::pair<std::string, std::string>> rules;
  size_t line = 0;
  while (line < raw.size()) {
    size_t end = raw.find('\n', line);
    if (end == std::string::npos) end = raw.size();
    std::string row = raw.substr(line, end - line);
    if (!row.empty() && row.back() == '\r') row.pop_back();
    line = end + 1;
    if (row.empty() || row[0] == '#') continue;
    const size_t eq = row.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    rules.emplace_back(row.substr(0, eq), row.substr(eq + 1));
  }
  std::sort(rules.begin(), rules.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
  std::string out = text;
  for (const auto& rule : rules) {
    size_t at = 0;
    while ((at = out.find(rule.first, at)) != std::string::npos) {
      const bool left = !letterBefore(out, at);
      const size_t after = at + rule.first.size();
      const bool right = after >= out.size() || !isLetter(codepointAt(out, after));
      if (left && right) {
        out.replace(at, rule.first.size(), rule.second);
        at += rule.second.size();
      } else {
        at += rule.first.size();
      }
    }
  }
  return out;
}

std::vector<std::string> sentences(const std::string& text) {
  std::vector<std::string> parts;
  std::string current;
  for (size_t i = 0; i < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    int length = 1;
    if ((lead & 0xE0) == 0xC0) length = 2;
    else if ((lead & 0xF0) == 0xE0) length = 3;
    else if ((lead & 0xF8) == 0xF0) length = 4;
    if (i + static_cast<size_t>(length) > text.size()) length = 1;
    current.append(text, i, static_cast<size_t>(length));
    i += static_cast<size_t>(length);
    const bool mark = length == 1 && (lead == '\n' || lead == '.' || lead == '!' || lead == '?' || lead == ';');
    if (mark || current.size() >= 280) {
      if (!current.empty()) parts.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

std::string trimCopy(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
  return text;
}

void writeMono(const float* src, int frames, const Format& format, BYTE* dst) {
  if (format.isFloat) {
    float* out = reinterpret_cast<float*>(dst);
    for (int i = 0; i < frames; ++i) {
      const float sample = std::clamp(src[i], -1.f, 1.f);
      for (int c = 0; c < format.channels; ++c) out[i * format.channels + c] = sample;
    }
    return;
  }
  int16_t* out = reinterpret_cast<int16_t*>(dst);
  for (int i = 0; i < frames; ++i) {
    const float sample = std::clamp(src[i], -1.f, 1.f);
    const auto pcm = static_cast<int16_t>(std::lrintf(sample * 32767.f));
    for (int c = 0; c < format.channels; ++c) out[i * format.channels + c] = pcm;
  }
}

struct Playback {
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  Format format;
  UINT32 bufferFrames = 0;

  void close() {
    if (client) client->Stop();
    if (render) render->Release();
    if (client) client->Release();
    render = nullptr;
    client = nullptr;
  }

  bool open(bool toCall, std::string& error) {
    close();
    const std::wstring id = toCall ? virtualCablePlaybackId(enumerateAudio(false)) : speakerId();
    if (id.empty()) {
      error = toCall ? "Нет виртуального кабеля, голос не попадёт в созвон"
                     : "Нет динамиков кроме виртуального кабеля";
      return false;
    }
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
      error = "Аудиосистема недоступна";
      return false;
    }
    IMMDevice* device = nullptr;
    hr = enumerator->GetDevice(id.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
      error = toCall ? "Кабель созвона не найден" : "Динамики не найдены";
      return false;
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(hr) || !client) {
      client = nullptr;
      error = toCall ? "Не удалось открыть кабель созвона" : "Не удалось открыть динамики";
      return false;
    }
    WAVEFORMATEXTENSIBLE desired{};
    desired.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    desired.Format.nChannels = 2;
    desired.Format.nSamplesPerSec = 48000;
    desired.Format.wBitsPerSample = 32;
    desired.Format.nBlockAlign = 8;
    desired.Format.nAvgBytesPerSec = 48000 * 8;
    desired.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    desired.Samples.wValidBitsPerSample = 32;
    desired.dwChannelMask = 0x3;
    desired.SubFormat = kIeeeFloat;
    WAVEFORMATEX* closest = nullptr;
    hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, reinterpret_cast<WAVEFORMATEX*>(&desired), &closest);
    WAVEFORMATEX* mix = nullptr;
    const WAVEFORMATEX* chosen = nullptr;
    if (hr == S_OK) chosen = reinterpret_cast<WAVEFORMATEX*>(&desired);
    else if (hr == S_FALSE && closest) chosen = closest;
    else if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) chosen = mix;
    if (!chosen) {
      error = "Устройство вывода не сообщило формат";
      if (closest) CoTaskMemFree(closest);
      if (mix) CoTaskMemFree(mix);
      close();
      return false;
    }
    format.rate = static_cast<int>(chosen->nSamplesPerSec);
    format.channels = chosen->nChannels;
    format.bits = chosen->wBitsPerSample;
    format.isFloat = chosen->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (chosen->wFormatTag == WAVE_FORMAT_EXTENSIBLE && chosen->cbSize >= 22) {
      const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(chosen);
      format.isFloat = IsEqualGUID(ext->SubFormat, kIeeeFloat) != FALSE;
    }
    const bool float32 = format.isFloat && format.bits == 32;
    const bool pcm16 = !format.isFloat && format.bits == 16;
    if (format.rate < 8000 || format.rate > 192000 || (!float32 && !pcm16) || format.channels < 1 ||
        format.channels > 8) {
      error = "Формат вывода не поддерживается";
      if (closest) CoTaskMemFree(closest);
      if (mix) CoTaskMemFree(mix);
      close();
      return false;
    }
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 100 * 10000, 0, chosen, nullptr);
    if (closest) CoTaskMemFree(closest);
    if (mix) CoTaskMemFree(mix);
    if (FAILED(hr)) {
      error = "Вывод речи не открывается";
      close();
      return false;
    }
    client->GetBufferSize(&bufferFrames);
    if (bufferFrames == 0) {
      error = "Устройство вывода не выделило буфер";
      close();
      return false;
    }
    hr = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr) || !render || FAILED(client->Start())) {
      error = "Поток речи не запустился";
      close();
      return false;
    }
    return true;
  }

  template <typename Stopped>
  bool write(const std::vector<float>& samples, int sampleRate, Stopped stopped) {
    if (!client || !render || samples.empty() || sampleRate <= 0 || format.rate <= 0) return false;
    double cursor = 0.0;
    const double step = static_cast<double>(sampleRate) / static_cast<double>(format.rate);
    std::vector<float> mono(std::max<UINT32>(bufferFrames, 1));
    while (cursor < static_cast<double>(samples.size())) {
      if (stopped()) return false;
      UINT32 padding = 0;
      if (FAILED(client->GetCurrentPadding(&padding))) return false;
      UINT32 need = bufferFrames > padding ? bufferFrames - padding : 0;
      if (need == 0) {
        Sleep(5);
        continue;
      }
      const double left = (static_cast<double>(samples.size()) - cursor) / step;
      if (left < static_cast<double>(need)) need = std::max(1u, static_cast<UINT32>(left));
      BYTE* out = nullptr;
      if (FAILED(render->GetBuffer(need, &out)) || !out) return false;
      for (UINT32 i = 0; i < need; ++i) {
        if (cursor >= static_cast<double>(samples.size())) {
          mono[i] = 0.f;
          continue;
        }
        const int index = std::min(static_cast<int>(cursor), static_cast<int>(samples.size()) - 1);
        const int next = std::min(index + 1, static_cast<int>(samples.size()) - 1);
        const float frac = static_cast<float>(cursor - index);
        mono[i] = samples[static_cast<size_t>(index)] +
                  (samples[static_cast<size_t>(next)] - samples[static_cast<size_t>(index)]) * frac;
        cursor += step;
      }
      writeMono(mono.data(), static_cast<int>(need), format, out);
      render->ReleaseBuffer(need, 0);
    }
    return true;
  }
};

std::string readyText(bool own, bool toCall) {
  const char* where = toCall ? "Читаю в микрофон созвона." : "Читаю в динамики, не в микрофон созвона.";
  if (own) return std::string("Свой голос готов. ") + where;
  return std::string("Голос ruslan готов. Свою модель положите в models\\tts\\my_voice. ") + where;
}

std::string idleStatus(bool own, bool toCall) {
  if (toCall && virtualCablePlaybackId(enumerateAudio(false)).empty()) {
    return "Нет виртуального кабеля, голос ruslan не попадёт в созвон.";
  }
  return readyText(own, toCall);
}

}  // namespace

struct SpeechPlayer::Impl {
  std::thread thread;
  std::atomic<int> run{0};
  std::atomic<int> active{0};
  std::atomic<int> generation{0};
  std::atomic<int> own{0};
  std::atomic<int> toCall{0};
  std::atomic<int> voiceReady{0};
  std::mutex mu;
  std::condition_variable cv;
  std::string pending;
  mutable std::mutex statusMu;
  std::string status = "Голос загрузится при первом чтении";
};

SpeechPlayer::SpeechPlayer() : impl_(new Impl) {}

SpeechPlayer::~SpeechPlayer() {
  stop();
  impl_->run.store(0);
  impl_->cv.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
  delete impl_;
}

void SpeechPlayer::speak(std::string text) {
  text = trimCopy(std::move(text));
  if (text.size() > 8000) {
    text.resize(8000);
    trimUtf8(text);
  }
  if (text.empty()) return;
  if (!impl_->run.load()) {
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->run.store(1);
    impl_->thread = std::thread([this] { threadMain(); });
  }
  {
    std::lock_guard lock(impl_->mu);
    impl_->pending = std::move(text);
    impl_->generation.fetch_add(1);
  }
  impl_->cv.notify_all();
}

void SpeechPlayer::stop() {
  impl_->generation.fetch_add(1);
  std::lock_guard lock(impl_->mu);
  impl_->pending.clear();
  impl_->cv.notify_all();
}

void SpeechPlayer::setRouteToCall(bool enabled) {
  const int next = enabled ? 1 : 0;
  const bool changed = impl_->toCall.exchange(next) != next;
  if (changed && impl_->active.load()) stop();
  if (!impl_->voiceReady.load() || impl_->active.load()) return;
  const std::string nextStatus = idleStatus(impl_->own.load() != 0, enabled);
  if (impl_->active.load()) return;
  std::lock_guard lock(impl_->statusMu);
  if (impl_->active.load()) return;
  impl_->status = nextStatus;
}

bool SpeechPlayer::speaking() const { return impl_->active.load() != 0; }

bool SpeechPlayer::ownVoice() const { return impl_->own.load() != 0; }

std::string SpeechPlayer::status() const {
  std::lock_guard lock(impl_->statusMu);
  return impl_->status;
}

std::string SpeechPlayer::menuLabel() const {
  return impl_->own.load() ? "Прочитать своим голосом" : "Прочитать вслух";
}

struct ComApartment {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~ComApartment() {
    if (SUCCEEDED(hr)) CoUninitialize();
  }
};

void SpeechPlayer::threadMain() {
  [[maybe_unused]] const ComApartment com;
  auto setStatus = [this](const std::string& text) {
    std::lock_guard lock(impl_->statusMu);
    impl_->status = text;
  };
  setStatus("Загружаю голос…");
  const std::wstring exe = exeDirectory();
  VoiceSpeaker speaker(findVoiceModel(exe));
  if (!speaker.ok()) {
    setStatus("Голос не открылся. Нужны модель Piper и espeak-ng-data рядом с программой.");
    impl_->run.store(0);
    return;
  }
  impl_->own.store(speaker.ownVoice() ? 1 : 0);
  impl_->voiceReady.store(1);
  const std::wstring stressPath = exe + L"\\models\\tts\\stress.txt";
  setStatus(idleStatus(speaker.ownVoice(), impl_->toCall.load() != 0));

  while (impl_->run.load()) {
    std::string text;
    int generation = 0;
    {
      std::unique_lock lock(impl_->mu);
      impl_->cv.wait(lock, [this] { return !impl_->run.load() || !impl_->pending.empty(); });
      if (!impl_->run.load()) break;
      text = std::move(impl_->pending);
      impl_->pending.clear();
      generation = impl_->generation.load();
    }
    text = applyStress(text, stressPath);
    for (char& ch : text) {
      if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    const std::vector<std::string> parts = sentences(text);
    const bool toCall = impl_->toCall.load() != 0;
    impl_->active.store(1);
    setStatus(toCall ? "Читаю в микрофон созвона…" : "Читаю в динамики…");
    Playback playback;
    std::string error;
    if (!playback.open(toCall, error)) {
      setStatus(error);
      impl_->active.store(0);
      continue;
    }
    bool stopped = false;
    for (const std::string& part : parts) {
      if (generation != impl_->generation.load() || !impl_->run.load()) {
        stopped = true;
        break;
      }
      const std::string piece = trimCopy(part);
      if (piece.empty()) continue;
      int rate = 0;
      std::vector<float> samples = speaker.synthesize(piece, rate);
      if (samples.empty() || rate <= 0) continue;
      samples.insert(samples.end(), static_cast<size_t>(rate / 25), 0.f);
      if (!playback.write(samples, rate, [&] { return generation != impl_->generation.load() || !impl_->run.load(); })) {
        stopped = true;
        break;
      }
    }
    playback.close();
    impl_->active.store(0);
    bool queued = false;
    {
      std::lock_guard lock(impl_->mu);
      queued = !impl_->pending.empty();
    }
    if (!queued) setStatus(idleStatus(impl_->own.load() != 0, impl_->toCall.load() != 0));
  }
}

}  // namespace cfx
