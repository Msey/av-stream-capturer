#include "capture/camera.hpp"

#include "capture/devices.hpp"
#include "common/color.hpp"
#include "video/face_effect.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace cfx {
namespace {

std::wstring modelBesideExe() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring text(path);
  const auto slash = text.find_last_of(L"\\/");
  if (slash != std::wstring::npos) text.resize(slash);
  return text + L"\\models\\ultraface.onnx";
}

void fitSize(int& width, int& height) {
  if (width <= 1280 && height <= 720) {
    width &= ~1;
    height &= ~1;
    width = std::max(2, width);
    height = std::max(2, height);
    return;
  }
  const float scale = std::min(1280.f / static_cast<float>(width), 720.f / static_cast<float>(height));
  width = std::max(2, static_cast<int>(width * scale) & ~1);
  height = std::max(2, static_cast<int>(height * scale) & ~1);
}

bool setRgb32(IMFSourceReader* reader, UINT32 width, UINT32 height) {
  IMFMediaType* type = nullptr;
  if (FAILED(MFCreateMediaType(&type))) return false;
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
  MFSetAttributeRatio(type, MF_MT_FRAME_RATE, 30, 1);
  const HRESULT hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
  type->Release();
  return SUCCEEDED(hr);
}

void closeCameraSource(ComPtr<IMFSourceReader>& reader, ComPtr<IMFMediaSource>& source) {
  reader.Reset();
  if (source) source->Shutdown();
  source.Reset();
}

}  // namespace

struct CameraCapture::Impl {
  std::thread thread;
  std::atomic<int> run{0};
  std::mutex configMu;
  std::wstring link;
  std::atomic<int> restart{0};
  mutable std::mutex statusMu;
  std::string status = "Камера ещё не запущена";
  mutable std::mutex frameMu;
  PreviewFrame frame;
  FaceEffect effect;
};

CameraCapture::CameraCapture(Params& params, SharedFrames& shared) : params_(params), shared_(shared), impl_(new Impl) {}

CameraCapture::~CameraCapture() {
  stop();
  delete impl_;
}

void CameraCapture::start(std::wstring symbolicLink) {
  stop();
  impl_->effect.load(modelBesideExe());
  {
    std::lock_guard lock(impl_->statusMu);
    impl_->status = impl_->effect.ready() ? "Камера запускается" : "Модель зрачков не загрузилась: " + impl_->effect.status();
  }
  {
    std::lock_guard lock(impl_->configMu);
    impl_->link = std::move(symbolicLink);
  }
  impl_->run.store(1);
  impl_->thread = std::thread([this] { threadMain(); });
}

void CameraCapture::setDevice(std::wstring symbolicLink) {
  {
    std::lock_guard lock(impl_->configMu);
    impl_->link = std::move(symbolicLink);
  }
  impl_->restart.store(1);
}

void CameraCapture::stop() {
  impl_->run.store(0);
  if (impl_->thread.joinable()) impl_->thread.join();
}

bool CameraCapture::copyPreview(PreviewFrame& frame) const {
  std::lock_guard lock(impl_->frameMu);
  if (impl_->frame.bgra.empty()) return false;
  frame = impl_->frame;
  return true;
}

std::string CameraCapture::status() const {
  std::lock_guard lock(impl_->statusMu);
  return impl_->status;
}

void CameraCapture::threadMain() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);
  std::vector<uint8_t> scaled;
  auto lastTick = std::chrono::steady_clock::now();

  while (impl_->run.load()) {
    std::wstring link;
    {
      std::lock_guard lock(impl_->configMu);
      link = impl_->link;
    }
    impl_->restart.store(0);

    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 2))) break;
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (!link.empty()) attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
    ComPtr<IMFMediaSource> source;
    HRESULT hr = MFCreateDeviceSource(attributes.Get(), &source);
    attributes.Reset();
    if (FAILED(hr) && link.empty()) {
      auto cameras = enumerateCameras();
      if (!cameras.empty()) {
        ComPtr<IMFAttributes> retry;
        if (SUCCEEDED(MFCreateAttributes(&retry, 2))) {
          retry->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
          retry->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, cameras.front().link.c_str());
          hr = MFCreateDeviceSource(retry.Get(), &source);
        }
      }
    }
    if (FAILED(hr) || !source) {
      if (source) {
        source->Shutdown();
        source.Reset();
      }
      std::lock_guard lock(impl_->statusMu);
      impl_->status = "Камера не найдена";
      for (int i = 0; i < 10 && impl_->run.load() && !impl_->restart.load(); ++i) Sleep(50);
      continue;
    }

    ComPtr<IMFAttributes> readerAttr;
    MFCreateAttributes(&readerAttr, 2);
    if (readerAttr) {
      readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
      readerAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttr.Get(), &reader);
    readerAttr.Reset();
    if (FAILED(hr) || !reader) {
      closeCameraSource(reader, source);
      std::lock_guard lock(impl_->statusMu);
      impl_->status = "Не удалось читать камеру";
      Sleep(300);
      continue;
    }
    if (!setRgb32(reader.Get(), 1280, 720)) {
      bool chosen = false;
      for (DWORD index = 0; index < 32 && !chosen; ++index) {
        ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &native))) break;
        UINT32 width = 0;
        UINT32 height = 0;
        MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (width >= 320 && height >= 240 && setRgb32(reader.Get(), width, height)) chosen = true;
      }
      if (!chosen) {
        closeCameraSource(reader, source);
        std::lock_guard lock(impl_->statusMu);
        impl_->status = "Камера не отдаёт поддерживаемый формат";
        Sleep(300);
        continue;
      }
    }

    UINT32 width = 1280;
    UINT32 height = 720;
    ComPtr<IMFMediaType> current;
    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current))) {
      MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height);
    }
    {
      std::lock_guard lock(impl_->statusMu);
      impl_->status = impl_->effect.ready() ? "Камера идёт, зрачки считаются UltraFace" : "Камера идёт без модели зрачков";
    }

    while (impl_->run.load() && !impl_->restart.load()) {
      DWORD flags = 0;
      ComPtr<IMFSample> sample;
      hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, &sample);
      if (FAILED(hr)) break;
      if (!sample) continue;
      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
      BYTE* data = nullptr;
      DWORD maxLen = 0;
      DWORD curLen = 0;
      if (FAILED(buffer->Lock(&data, &maxLen, &curLen)) || !data) continue;
      int srcStride = static_cast<int>(width) * 4;
      if (height > 0 && curLen / height >= static_cast<DWORD>(srcStride)) srcStride = static_cast<int>(curLen / height);
      int outW = static_cast<int>(width);
      int outH = static_cast<int>(height);
      fitSize(outW, outH);
      const int outStride = outW * 4;
      if (static_cast<int>(scaled.size()) < outStride * outH) scaled.resize(static_cast<size_t>(outStride) * outH);
      scaleBgra(data, static_cast<int>(width), static_cast<int>(height), srcStride, scaled.data(), outW, outH, outStride);
      buffer->Unlock();

      if (params_.mirror.load()) mirrorBgra(scaled.data(), outW, outH, outStride);

      const auto now = std::chrono::steady_clock::now();
      const float dt = std::chrono::duration<float>(now - lastTick).count();
      const float periodMs = std::chrono::duration<float, std::milli>(now - lastTick).count();
      lastTick = now;
      if (periodMs > 1.f && periodMs < 500.f) {
        params_.framePeriodMs.store(periodMs);
        const float fps = params_.captureFps.load();
        const float instant = 1000.f / periodMs;
        params_.captureFps.store(fps <= 0.f ? instant : fps * 0.9f + instant * 0.1f);
      }

      float processMs = 0.f;
      bool face = false;
      if (params_.pupilsEnabled.load() && impl_->effect.ready()) {
        face = impl_->effect.apply(scaled.data(), outW, outH, outStride, dt, processMs);
      }
      params_.videoProcessMs.store(processMs);
      params_.faceFound.store(face ? 1 : 0);

      shared_.publish(scaled.data(), outW, outH, outStride);
      {
        std::lock_guard lock(impl_->frameMu);
        impl_->frame.width = outW;
        impl_->frame.height = outH;
        impl_->frame.processMs = processMs;
        impl_->frame.face = face;
        impl_->frame.bgra.assign(scaled.begin(), scaled.begin() + static_cast<size_t>(outStride) * outH);
      }
    }
    closeCameraSource(reader, source);
  }

  MFShutdown();
  if (com == S_OK) CoUninitialize();
}

}  // namespace cfx
