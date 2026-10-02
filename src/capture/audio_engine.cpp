#include "capture/audio_engine.hpp"

#include "audio/delay_line.hpp"
#include "audio/pitch_shifter.hpp"
#include "capture/devices.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

namespace cfx {
namespace {

const GUID kIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

struct Format {
  int rate = 48000;
  int channels = 2;
  int bits = 32;
  bool isFloat = true;
  int blockAlign = 8;
  std::vector<BYTE> bytes;

  const WAVEFORMATEX* wave() const { return reinterpret_cast<const WAVEFORMATEX*>(bytes.data()); }
};

struct FloatRing {
  std::vector<float> data;
  int read = 0;
  int count = 0;

  void reset(int capacity) {
    data.assign(static_cast<size_t>(std::max(capacity, 8)), 0.f);
    read = 0;
    count = 0;
  }

  void write(const float* samples, int n) {
    if (data.empty()) return;
    const int size = static_cast<int>(data.size());
    for (int i = 0; i < n; ++i) {
      const int write = (read + count) % size;
      if (count == size) read = (read + 1) % size;
      else ++count;
      data[static_cast<size_t>(write)] = samples[i];
    }
  }

  int readSamples(float* dst, int n) {
    const int size = data.empty() ? 1 : static_cast<int>(data.size());
    const int got = std::min(n, count);
    for (int i = 0; i < got; ++i) {
      dst[i] = data[static_cast<size_t>(read)];
      read = (read + 1) % size;
    }
    count -= got;
    if (got < n) std::fill(dst + got, dst + n, 0.f);
    return got;
  }
};

struct Resampler {
  FloatRing ring;
  bool primed = false;
  float a = 0.f;
  float b = 0.f;
  double frac = 0.0;

  void prepare(int capacity) {
    ring.reset(capacity);
    primed = false;
    frac = 0.0;
    a = b = 0.f;
  }

  void push(const float* samples, int n) { ring.write(samples, n); }

  void pull(float* dst, int n, double step) {
    if (std::fabs(step - 1.0) < 1e-6) {
      ring.readSamples(dst, n);
      return;
    }
    if (!primed) {
      if (ring.count < 2) {
        std::fill(dst, dst + n, 0.f);
        return;
      }
      ring.readSamples(&a, 1);
      ring.readSamples(&b, 1);
      primed = true;
    }
    for (int i = 0; i < n; ++i) {
      while (frac >= 1.0) {
        a = b;
        if (ring.count > 0) ring.readSamples(&b, 1);
        frac -= 1.0;
      }
      dst[i] = a + (b - a) * static_cast<float>(frac);
      frac += step;
    }
  }
};

void toMono(const BYTE* src, int frames, const Format& format, float* dst) {
  if (format.isFloat) {
    const float* in = reinterpret_cast<const float*>(src);
    for (int i = 0; i < frames; ++i) {
      float sum = 0.f;
      for (int c = 0; c < format.channels; ++c) sum += in[i * format.channels + c];
      dst[i] = sum / static_cast<float>(std::max(1, format.channels));
    }
    return;
  }
  if (format.bits == 16) {
    const int16_t* in = reinterpret_cast<const int16_t*>(src);
    for (int i = 0; i < frames; ++i) {
      float sum = 0.f;
      for (int c = 0; c < format.channels; ++c) sum += in[i * format.channels + c] / 32768.f;
      dst[i] = sum / static_cast<float>(std::max(1, format.channels));
    }
    return;
  }
  std::fill(dst, dst + frames, 0.f);
}

void fromMono(const float* src, int frames, const Format& format, BYTE* dst) {
  if (format.isFloat) {
    float* out = reinterpret_cast<float*>(dst);
    for (int i = 0; i < frames; ++i) {
      const float sample = std::clamp(src[i], -1.f, 1.f);
      for (int c = 0; c < format.channels; ++c) out[i * format.channels + c] = sample;
    }
    return;
  }
  if (format.bits == 16) {
    int16_t* out = reinterpret_cast<int16_t*>(dst);
    for (int i = 0; i < frames; ++i) {
      const float sample = std::clamp(src[i], -1.f, 1.f);
      const auto pcm = static_cast<int16_t>(std::lrintf(sample * 32767.f));
      for (int c = 0; c < format.channels; ++c) out[i * format.channels + c] = pcm;
    }
  }
}

Format makeDesired() {
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
  Format format;
  format.bytes.resize(sizeof(desired));
  std::memcpy(format.bytes.data(), &desired, sizeof(desired));
  return format;
}

Format adoptWave(const WAVEFORMATEX* wave) {
  Format format;
  const size_t bytes = sizeof(WAVEFORMATEX) + wave->cbSize;
  format.bytes.resize(bytes);
  std::memcpy(format.bytes.data(), wave, bytes);
  format.rate = static_cast<int>(wave->nSamplesPerSec);
  format.channels = wave->nChannels;
  format.bits = wave->wBitsPerSample;
  format.blockAlign = wave->nBlockAlign;
  if (wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) format.isFloat = true;
  else if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wave->cbSize >= 22) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
    format.isFloat = IsEqualGUID(ext->SubFormat, kIeeeFloat) != FALSE;
  } else {
    format.isFloat = false;
  }
  return format;
}

bool openClient(const std::wstring& id, EDataFlow flow, IAudioClient** client, Format& format, UINT32& bufferFrames,
                float& periodMs, std::string& error) {
  *client = nullptr;
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) {
    error = "Аудиосистема недоступна";
    return false;
  }
  IMMDevice* device = nullptr;
  if (id.empty()) hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
  else hr = enumerator->GetDevice(id.c_str(), &device);
  enumerator->Release();
  if (FAILED(hr) || !device) {
    error = flow == eCapture ? "Микрофон не найден" : "Устройство вывода не найдено";
    return false;
  }
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client));
  device->Release();
  if (FAILED(hr) || !*client) {
    *client = nullptr;
    error = "Не удалось открыть аудиоустройство";
    return false;
  }

  Format desired = makeDesired();
  WAVEFORMATEX* closest = nullptr;
  hr = (*client)->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, desired.wave(), &closest);
  if (hr == S_OK) format = desired;
  else if (hr == S_FALSE && closest) {
    format = adoptWave(closest);
    CoTaskMemFree(closest);
  } else {
    if (closest) CoTaskMemFree(closest);
    WAVEFORMATEX* mix = nullptr;
    if (FAILED((*client)->GetMixFormat(&mix)) || !mix) {
      (*client)->Release();
      *client = nullptr;
      error = "Устройство не сообщило формат";
      return false;
    }
    format = adoptWave(mix);
    CoTaskMemFree(mix);
  }

  const DWORD flags = flow == eCapture ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;
  const REFERENCE_TIME period = 10 * 10000;
  hr = (*client)->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, period, 0, format.wave(), nullptr);
  if (FAILED(hr)) hr = (*client)->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, format.wave(), nullptr);
  if (FAILED(hr)) {
    error = flow == eCapture ? "Микрофон занят или не открывается" : "Вывод звука не открывается";
    (*client)->Release();
    *client = nullptr;
    return false;
  }
  (*client)->GetBufferSize(&bufferFrames);
  REFERENCE_TIME defPeriod = 0;
  REFERENCE_TIME minPeriod = 0;
  (*client)->GetDevicePeriod(&defPeriod, &minPeriod);
  periodMs = static_cast<float>(defPeriod) / 10000.f;
  return true;
}

}  // namespace

struct AudioEngine::Impl {
  std::thread thread;
  std::atomic<int> run{0};
  std::mutex configMu;
  std::wstring micId;
  std::wstring outId;
  std::atomic<int> restart{0};
  mutable std::mutex statusMu;
  std::string status = "Звук ещё не запущен";
};

AudioEngine::AudioEngine(Params& params) : params_(params), impl_(new Impl) {}

AudioEngine::~AudioEngine() {
  stop();
  delete impl_;
}

void AudioEngine::start(std::wstring microphoneId, std::wstring outputId) {
  stop();
  {
    std::lock_guard lock(impl_->configMu);
    impl_->micId = std::move(microphoneId);
    impl_->outId = std::move(outputId);
  }
  impl_->run.store(1);
  impl_->thread = std::thread([this] { threadMain(); });
}

void AudioEngine::setDevices(std::wstring microphoneId, std::wstring outputId) {
  {
    std::lock_guard lock(impl_->configMu);
    impl_->micId = std::move(microphoneId);
    impl_->outId = std::move(outputId);
  }
  impl_->restart.store(1);
}

void AudioEngine::stop() {
  impl_->run.store(0);
  if (impl_->thread.joinable()) impl_->thread.join();
}

std::string AudioEngine::status() const {
  std::lock_guard lock(impl_->statusMu);
  return impl_->status;
}

void AudioEngine::threadMain() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD taskIndex = 0;
  HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

  PitchShifter shifter;
  DelayLine delay;
  bool prepared = false;
  int captureRate = 48000;
  Resampler primary;
  Resampler monitor;
  primary.prepare(48000);
  monitor.prepare(48000);
  std::vector<float> mono(4096);
  std::vector<float> rendered(4096);

  while (impl_->run.load()) {
    std::wstring micId;
    std::wstring outId;
    {
      std::lock_guard lock(impl_->configMu);
      micId = impl_->micId;
      outId = impl_->outId;
    }
    impl_->restart.store(0);

    IAudioClient* capture = nullptr;
    IAudioClient* render = nullptr;
    IAudioClient* monitorClient = nullptr;
    Format captureFormat;
    Format renderFormat;
    Format monitorFormat;
    UINT32 captureFrames = 0;
    UINT32 renderFrames = 0;
    UINT32 monitorFrames = 0;
    float capturePeriod = 10.f;
    float renderPeriod = 10.f;
    float monitorPeriod = 0.f;
    std::string error;
    if (!openClient(micId, eCapture, &capture, captureFormat, captureFrames, capturePeriod, error)) {
      std::lock_guard lock(impl_->statusMu);
      impl_->status = error;
      for (int i = 0; i < 10 && impl_->run.load() && !impl_->restart.load(); ++i) Sleep(50);
      continue;
    }
    if (!openClient(outId, eRender, &render, renderFormat, renderFrames, renderPeriod, error)) {
      capture->Release();
      std::lock_guard lock(impl_->statusMu);
      impl_->status = error;
      for (int i = 0; i < 10 && impl_->run.load() && !impl_->restart.load(); ++i) Sleep(50);
      continue;
    }

    const std::wstring defaultRender = defaultAudioId(false);
    const bool monitorWanted = params_.monitor.load() != 0 && !defaultRender.empty() && defaultRender != outId;
    if (monitorWanted) {
      std::string monitorError;
      if (!openClient(defaultRender, eRender, &monitorClient, monitorFormat, monitorFrames, monitorPeriod, monitorError)) {
        monitorClient = nullptr;
      }
    }

    IAudioCaptureClient* capturer = nullptr;
    IAudioRenderClient* renderer = nullptr;
    IAudioRenderClient* monitorRenderer = nullptr;
    capture->GetService(IID_PPV_ARGS(&capturer));
    render->GetService(IID_PPV_ARGS(&renderer));
    if (monitorClient) monitorClient->GetService(IID_PPV_ARGS(&monitorRenderer));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!capturer || !renderer || !event || FAILED(capture->SetEventHandle(event))) {
      if (event) CloseHandle(event);
      if (capturer) capturer->Release();
      if (renderer) renderer->Release();
      if (monitorRenderer) monitorRenderer->Release();
      capture->Release();
      render->Release();
      if (monitorClient) monitorClient->Release();
      std::lock_guard lock(impl_->statusMu);
      impl_->status = "Не удалось запустить поток микрофона";
      Sleep(300);
      continue;
    }

    captureRate = captureFormat.rate;
    shifter.prepare(captureRate);
    delay.prepare(captureRate);
    prepared = true;
    primary.prepare(std::max(captureRate, renderFormat.rate));
    if (monitorRenderer) monitor.prepare(std::max(captureRate, monitorFormat.rate));
    capture->Start();
    render->Start();
    if (monitorClient) monitorClient->Start();
    {
      std::lock_guard lock(impl_->statusMu);
      impl_->status = monitorRenderer ? "Микрофон обрабатывается, копия идёт в наушники"
                                      : "Микрофон обрабатывается";
    }
    params_.wasapiMs.store(capturePeriod + renderPeriod);

    while (impl_->run.load() && !impl_->restart.load()) {
      WaitForSingleObject(event, 10);
      UINT32 packet = 0;
      while (SUCCEEDED(capturer->GetNextPacketSize(&packet)) && packet > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capturer->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        if (mono.size() < frames) mono.resize(frames);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(mono.begin(), mono.begin() + frames, 0.f);
        else toMono(data, static_cast<int>(frames), captureFormat, mono.data());
        capturer->ReleaseBuffer(frames);

        shifter.setPitchSemitones(params_.pitchSemitones.load());
        shifter.setFormantSemitones(params_.formantSemitones.load());
        shifter.setMix(params_.voiceMix.load());
        shifter.setEnabled(params_.voiceEnabled.load() != 0);
        shifter.process(mono.data(), static_cast<int>(frames));

        const float videoMs = params_.videoProcessMs.load() + params_.framePeriodMs.load();
        const float algoMs = 1000.f * shifter.latencySamples() / static_cast<float>(captureRate);
        const float wasapiMs = params_.wasapiMs.load();
        float lipMs = videoMs + static_cast<float>(params_.lipExtraMs.load()) - algoMs - wasapiMs;
        if (lipMs < 0.f) lipMs = 0.f;
        delay.setTargetSamples(static_cast<int>(lipMs * captureRate / 1000.f));
        delay.process(mono.data(), static_cast<int>(frames));
        const float delayMs = 1000.f * delay.delaySamples() / static_cast<float>(captureRate);
        params_.audioAlgoMs.store(algoMs);
        params_.audioTotalMs.store(algoMs + wasapiMs + delayMs);

        primary.push(mono.data(), static_cast<int>(frames));
        if (monitorRenderer && params_.monitor.load()) monitor.push(mono.data(), static_cast<int>(frames));
      }

      auto pump = [&](IAudioClient* client, IAudioRenderClient* writer, UINT32 bufferFrames, const Format& format,
                      Resampler& source) {
        if (!client || !writer) return;
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) return;
        UINT32 need = bufferFrames - padding;
        if (need == 0) return;
        BYTE* out = nullptr;
        if (FAILED(writer->GetBuffer(need, &out)) || !out) return;
        if (rendered.size() < need) rendered.resize(need);
        const double step = static_cast<double>(captureRate) / static_cast<double>(std::max(1, format.rate));
        source.pull(rendered.data(), static_cast<int>(need), step);
        fromMono(rendered.data(), static_cast<int>(need), format, out);
        writer->ReleaseBuffer(need, 0);
      };
      pump(render, renderer, renderFrames, renderFormat, primary);
      if (monitorRenderer && params_.monitor.load()) {
        pump(monitorClient, monitorRenderer, monitorFrames, monitorFormat, monitor);
      }
    }

    capture->Stop();
    render->Stop();
    if (monitorClient) monitorClient->Stop();
    CloseHandle(event);
    capturer->Release();
    renderer->Release();
    if (monitorRenderer) monitorRenderer->Release();
    capture->Release();
    render->Release();
    if (monitorClient) monitorClient->Release();
    (void)prepared;
  }

  if (task) AvRevertMmThreadCharacteristics(task);
  if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace cfx
