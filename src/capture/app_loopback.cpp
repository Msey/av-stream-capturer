#include "capture/app_loopback.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#else
enum AUDIOCLIENT_ACTIVATION_TYPE {
  AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
  AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1,
};
enum PROCESS_LOOPBACK_MODE {
  PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
  PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1,
};
struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
  DWORD TargetProcessId;
  PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
};
struct AUDIOCLIENT_ACTIVATION_PARAMS {
  AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
  union {
    AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
  };
};
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif
#endif

namespace cfx {
namespace {

const GUID kIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kPcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::string hexHr(HRESULT hr) {
  char text[16]{};
  std::snprintf(text, sizeof(text), "%08lX", static_cast<unsigned long>(hr));
  return text;
}

struct Format {
  int rate = 16000;
  int channels = 1;
  int bits = 16;
  bool isFloat = false;
  int blockAlign = 2;
};

void toMono(const BYTE* src, int frames, const Format& format, bool silent, float* dst) {
  if (silent || !src) {
    std::fill(dst, dst + frames, 0.f);
    return;
  }
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

bool makeFormat(int rate, int channels, bool isFloat, WAVEFORMATEXTENSIBLE& out) {
  std::memset(&out, 0, sizeof(out));
  out.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  out.Format.nChannels = static_cast<WORD>(channels);
  out.Format.nSamplesPerSec = rate;
  out.Format.wBitsPerSample = isFloat ? 32 : 16;
  out.Format.nBlockAlign = static_cast<WORD>(channels * out.Format.wBitsPerSample / 8);
  out.Format.nAvgBytesPerSec = rate * out.Format.nBlockAlign;
  out.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  out.Samples.wValidBitsPerSample = out.Format.wBitsPerSample;
  out.dwChannelMask = channels == 1 ? 0x4 : 0x3;
  out.SubFormat = isFloat ? kIeeeFloat : kPcm;
  return true;
}

class ActivateHandler : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                           IActivateAudioInterfaceCompletionHandler,
                                                           Microsoft::WRL::FtmBase> {
 public:
  ActivateHandler() { event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  ~ActivateHandler() override {
    if (event_) CloseHandle(event_);
    if (client_) client_->Release();
  }

  HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
    IUnknown* unknown = nullptr;
    HRESULT activateHr = E_FAIL;
    hr_ = operation->GetActivateResult(&activateHr, &unknown);
    if (SUCCEEDED(hr_)) hr_ = activateHr;
    if (SUCCEEDED(hr_) && unknown) {
      hr_ = unknown->QueryInterface(IID_PPV_ARGS(&client_));
      unknown->Release();
    }
    if (event_) SetEvent(event_);
    return S_OK;
  }

  bool wait(DWORD ms) const { return event_ && WaitForSingleObject(event_, ms) == WAIT_OBJECT_0; }

  HANDLE event_ = nullptr;
  HRESULT hr_ = E_FAIL;
  IAudioClient* client_ = nullptr;
};

}  // namespace

struct AppLoopback::Impl {
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  HANDLE event = nullptr;
  Format format;
  std::vector<float> pending;
};

AppLoopback::AppLoopback() : impl_(new Impl) {}

AppLoopback::~AppLoopback() {
  close();
  delete impl_;
}

bool AppLoopback::open(std::uint32_t pid, bool exclude, std::string& error) {
  close();
  auto* params = new AUDIOCLIENT_ACTIVATION_PARAMS{};
  params->ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params->ProcessLoopbackParams.TargetProcessId = pid == 0 ? GetCurrentProcessId() : pid;
  params->ProcessLoopbackParams.ProcessLoopbackMode =
      exclude ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

  PROPVARIANT activate{};
  activate.vt = VT_BLOB;
  activate.blob.cbSize = sizeof(*params);
  activate.blob.pBlobData = reinterpret_cast<BYTE*>(params);

  auto handler = Microsoft::WRL::Make<ActivateHandler>();
  IActivateAudioInterfaceAsyncOperation* operation = nullptr;
  HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activate,
                                           handler.Get(), &operation);
  if (FAILED(hr)) {
    delete params;
    error = "Захват звука приложений недоступен (" + hexHr(hr) + ")";
    return false;
  }
  const bool finished = handler->wait(8000);
  if (operation) operation->Release();
  if (!finished || FAILED(handler->hr_) || !handler->client_) {
    error = finished ? "Не удалось открыть звук собеседника (" + hexHr(handler->hr_) + ")"
                     : "Захват звука приложений не ответил";
    if (!finished) handler.Detach();
    else delete params;
    return false;
  }
  delete params;
  impl_->client = handler->client_;
  handler->client_ = nullptr;

  const DWORD flagSets[] = {AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK};
  struct Attempt {
    int rate;
    int channels;
    bool isFloat;
  };
  const Attempt attempts[] = {{16000, 1, false}, {48000, 2, true}, {48000, 2, false}};
  bool opened = false;
  HRESULT last = E_FAIL;
  for (const Attempt& attempt : attempts) {
    for (DWORD flags : flagSets) {
      WAVEFORMATEXTENSIBLE wave{};
      makeFormat(attempt.rate, attempt.channels, attempt.isFloat, wave);
      last = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200 * 10000, 0,
                                       reinterpret_cast<WAVEFORMATEX*>(&wave), nullptr);
      if (SUCCEEDED(last)) {
        impl_->format.rate = attempt.rate;
        impl_->format.channels = attempt.channels;
        impl_->format.isFloat = attempt.isFloat;
        impl_->format.bits = attempt.isFloat ? 32 : 16;
        impl_->format.blockAlign = attempt.channels * impl_->format.bits / 8;
        opened = true;
        break;
      }
    }
    if (opened) break;
  }
  if (!opened) {
    error = "Формат захвата звука собеседника не подошёл (" + hexHr(last) + ")";
    close();
    return false;
  }

  hr = impl_->client->GetService(IID_PPV_ARGS(&impl_->capture));
  impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (FAILED(hr) || !impl_->capture || !impl_->event || FAILED(impl_->client->SetEventHandle(impl_->event)) ||
      FAILED(impl_->client->Start())) {
    error = "Поток звука собеседника не запустился";
    close();
    return false;
  }
  return true;
}

void AppLoopback::close() {
  if (impl_->client) impl_->client->Stop();
  if (impl_->event) CloseHandle(impl_->event);
  if (impl_->capture) impl_->capture->Release();
  if (impl_->client) impl_->client->Release();
  impl_->event = nullptr;
  impl_->capture = nullptr;
  impl_->client = nullptr;
  impl_->pending.clear();
}

int AppLoopback::sampleRate() const { return impl_->format.rate; }

int AppLoopback::read(float* dst, int maxFrames, int timeoutMs) {
  if (!impl_->client || !impl_->capture || !dst || maxFrames <= 0) return -1;
  if (!impl_->pending.empty()) {
    const int got = std::min(maxFrames, static_cast<int>(impl_->pending.size()));
    std::copy(impl_->pending.begin(), impl_->pending.begin() + got, dst);
    impl_->pending.erase(impl_->pending.begin(), impl_->pending.begin() + got);
    return got;
  }
  WaitForSingleObject(impl_->event, static_cast<DWORD>(std::max(0, timeoutMs)));
  int written = 0;
  UINT32 packet = 0;
  while (written < maxFrames && SUCCEEDED(impl_->capture->GetNextPacketSize(&packet)) && packet > 0) {
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    if (FAILED(impl_->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return written > 0 ? written : -1;
    struct Packet {
      IAudioCaptureClient* capture = nullptr;
      UINT32 frames = 0;
      ~Packet() {
        if (capture) capture->ReleaseBuffer(frames);
      }
    } packetRelease{impl_->capture, frames};
    if (frames == 0) continue;
    if (frames > 48000) return written > 0 ? written : -1;
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    const int count = static_cast<int>(frames);
    const int room = maxFrames - written;
    const int take = std::min(count, room);
    std::vector<float> mono(static_cast<size_t>(count));
    toMono(data, count, impl_->format, silent, mono.data());
    std::copy(mono.begin(), mono.begin() + take, dst + written);
    written += take;
    if (take < count) impl_->pending.insert(impl_->pending.end(), mono.begin() + take, mono.end());
  }
  return written;
}

}  // namespace cfx
