#include "vcam/camera_name.hpp"

#include "common/color.hpp"
#include "common/shared_frame.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// The SDK declares these as dllimport. Hide those declarations so this DLL can define them.
#define DllGetClassObject DllGetClassObject_Skip
#define DllCanUnloadNow DllCanUnloadNow_Skip
#define DllRegisterServer DllRegisterServer_Skip
#define DllUnregisterServer DllUnregisterServer_Skip
#include <dshow.h>
#include <dvdmedia.h>
#undef DllGetClassObject
#undef DllCanUnloadNow
#undef DllRegisterServer
#undef DllUnregisterServer

namespace {

const GUID CLSID_ConferenceFxCam = {0xa7c3e1b2, 0x4f58, 0x4d1a, {0x9e, 0x63, 0x0b, 0x8f, 0x2d, 0x6a, 0x71, 0xc4}};
const GUID kNv12 = {0x3231564E, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::atomic<long> g_locks{0};
HMODULE g_module = nullptr;

constexpr int kModeCount = 9;

struct Mode {
  int width;
  int height;
  int format;  // 0 NV12, 1 YUY2, 2 RGB32
};

constexpr Mode kModes[kModeCount] = {
    {1280, 720, 0}, {1280, 720, 1}, {1280, 720, 2}, {1920, 1080, 0}, {1920, 1080, 1},
    {1920, 1080, 2}, {640, 480, 0},  {640, 480, 1},  {640, 480, 2},
};

void freeMt(AM_MEDIA_TYPE& mt) {
  if (mt.pbFormat) {
    CoTaskMemFree(mt.pbFormat);
    mt.pbFormat = nullptr;
  }
  if (mt.pUnk) {
    mt.pUnk->Release();
    mt.pUnk = nullptr;
  }
  mt.cbFormat = 0;
}

long imageBytes(int width, int height, int format) {
  if (format == 2) return width * height * 4;
  if (format == 1) return width * height * 2;
  return width * height * 3 / 2;
}

GUID subtypeOf(int format) {
  if (format == 2) return MEDIASUBTYPE_RGB32;
  if (format == 1) return MEDIASUBTYPE_YUY2;
  return kNv12;
}

int formatOf(const GUID& subtype) {
  if (subtype == MEDIASUBTYPE_RGB32) return 2;
  if (subtype == MEDIASUBTYPE_YUY2) return 1;
  if (subtype == kNv12) return 0;
  return -1;
}

HRESULT makeType(int index, AM_MEDIA_TYPE& mt) {
  if (index < 0 || index >= kModeCount) return E_INVALIDARG;
  ZeroMemory(&mt, sizeof(mt));
  const Mode mode = kModes[index];
  const long bytes = imageBytes(mode.width, mode.height, mode.format);
  auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
  if (!vih) return E_OUTOFMEMORY;
  ZeroMemory(vih, sizeof(*vih));
  vih->AvgTimePerFrame = 333333;
  vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  vih->bmiHeader.biWidth = mode.width;
  vih->bmiHeader.biHeight = mode.format == 2 ? -mode.height : mode.height;
  vih->bmiHeader.biPlanes = 1;
  vih->bmiHeader.biBitCount = mode.format == 2 ? 32 : (mode.format == 1 ? 16 : 12);
  vih->bmiHeader.biCompression = mode.format == 2 ? BI_RGB : (mode.format == 1 ? 0x32595559 : 0x3231564E);
  vih->bmiHeader.biSizeImage = bytes;
  mt.majortype = MEDIATYPE_Video;
  mt.subtype = subtypeOf(mode.format);
  mt.bFixedSizeSamples = TRUE;
  mt.bTemporalCompression = FALSE;
  mt.lSampleSize = bytes;
  mt.formattype = FORMAT_VideoInfo;
  mt.cbFormat = sizeof(VIDEOINFOHEADER);
  mt.pbFormat = reinterpret_cast<BYTE*>(vih);
  return S_OK;
}

struct FrameDesc {
  int width = 1280;
  int height = 720;
  int format = 0;
  REFERENCE_TIME duration = 333333;
  long bytes = 1280 * 720 * 3 / 2;
};

FrameDesc describe(const AM_MEDIA_TYPE& mt) {
  FrameDesc desc;
  desc.format = formatOf(mt.subtype);
  if (desc.format < 0) desc.format = 0;
  if (mt.formattype == FORMAT_VideoInfo && mt.pbFormat && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
    desc.width = std::abs(vih->bmiHeader.biWidth);
    desc.height = std::abs(vih->bmiHeader.biHeight);
    if (vih->AvgTimePerFrame > 0) desc.duration = vih->AvgTimePerFrame;
  }
  if (desc.width < 2 || desc.height < 2) {
    desc.width = 1280;
    desc.height = 720;
  }
  desc.bytes = imageBytes(desc.width, desc.height, desc.format);
  return desc;
}

bool acceptable(const AM_MEDIA_TYPE& mt) {
  if (mt.majortype != MEDIATYPE_Video && mt.majortype != GUID_NULL) return false;
  if (mt.subtype != GUID_NULL && formatOf(mt.subtype) < 0) return false;
  if (mt.formattype != GUID_NULL && mt.formattype != FORMAT_VideoInfo && mt.formattype != FORMAT_VideoInfo2) return false;
  if (mt.pbFormat && mt.cbFormat >= sizeof(VIDEOINFOHEADER) &&
      (mt.formattype == FORMAT_VideoInfo || mt.formattype == GUID_NULL)) {
    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
    const int width = std::abs(vih->bmiHeader.biWidth);
    const int height = std::abs(vih->bmiHeader.biHeight);
    if (width < 2 || height < 2 || width > 1920 || height > 1080) return false;
    if ((width & 1) || (height & 1)) return false;
  }
  return true;
}

void paintBlack(uint8_t* dst, const FrameDesc& desc) {
  if (desc.format == 2) {
    std::memset(dst, 0, static_cast<size_t>(desc.bytes));
    return;
  }
  if (desc.format == 1) {
    for (long i = 0; i < desc.bytes; i += 4) {
      dst[i] = 16;
      dst[i + 1] = 128;
      if (i + 2 < desc.bytes) dst[i + 2] = 16;
      if (i + 3 < desc.bytes) dst[i + 3] = 128;
    }
    return;
  }
  std::memset(dst, 16, static_cast<size_t>(desc.width) * desc.height);
  std::memset(dst + static_cast<size_t>(desc.width) * desc.height, 128,
              static_cast<size_t>(desc.width) * desc.height / 2);
}

void paintFrame(uint8_t* dst, long capacity, const FrameDesc& desc, cfx::SharedFrames& shared, std::vector<uint8_t>& raw,
                std::vector<uint8_t>& scaled) {
  if (capacity < desc.bytes) return;
  paintBlack(dst, desc);
  int width = 0;
  int height = 0;
  const int rawStride = cfx::kSharedMaxWidth * 4;
  if (raw.size() < static_cast<size_t>(rawStride) * cfx::kSharedMaxHeight) {
    raw.resize(static_cast<size_t>(rawStride) * cfx::kSharedMaxHeight);
  }
  if (!shared.copyLatest(raw.data(), rawStride, cfx::kSharedMaxWidth, cfx::kSharedMaxHeight, width, height)) return;
  const int scaledStride = desc.width * 4;
  if (scaled.size() < static_cast<size_t>(scaledStride) * desc.height) {
    scaled.resize(static_cast<size_t>(scaledStride) * desc.height);
  }
  cfx::scaleBgra(raw.data(), width, height, rawStride, scaled.data(), desc.width, desc.height, scaledStride);
  if (desc.format == 2) {
    for (int y = 0; y < desc.height; ++y) {
      std::memcpy(dst + static_cast<size_t>(y) * scaledStride, scaled.data() + static_cast<size_t>(y) * scaledStride,
                  scaledStride);
    }
  } else if (desc.format == 1) {
    cfx::bgraToYuy2(scaled.data(), desc.width, desc.height, scaledStride, dst);
  } else {
    cfx::bgraToNv12(scaled.data(), desc.width, desc.height, scaledStride, dst);
  }
}

class Filter;

class Pin : public IPin, public IAMStreamConfig, public IKsPropertySet {
 public:
  Filter* filter = nullptr;
  IPin* connected = nullptr;
  IMemInputPin* memInput = nullptr;
  IMemAllocator* allocator = nullptr;
  AM_MEDIA_TYPE connection{};
  int preferred = 0;
  std::atomic<bool> streaming{false};
  std::thread thread;

  Pin() { ZeroMemory(&connection, sizeof(connection)); }
  ~Pin() { stopStreaming(); }

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
  STDMETHODIMP_(ULONG) AddRef();
  STDMETHODIMP_(ULONG) Release();
  STDMETHODIMP Connect(IPin* receivePin, const AM_MEDIA_TYPE* pmt);
  STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) { return E_UNEXPECTED; }
  STDMETHODIMP Disconnect();
  STDMETHODIMP ConnectedTo(IPin** pin);
  STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt);
  STDMETHODIMP QueryPinInfo(PIN_INFO* info);
  STDMETHODIMP QueryDirection(PIN_DIRECTION* dir);
  STDMETHODIMP QueryId(LPWSTR* id);
  STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt);
  STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum);
  STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) { return E_NOTIMPL; }
  STDMETHODIMP EndOfStream() { return E_UNEXPECTED; }
  STDMETHODIMP BeginFlush() { return E_UNEXPECTED; }
  STDMETHODIMP EndFlush() { return E_UNEXPECTED; }
  STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }
  STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt);
  STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt);
  STDMETHODIMP GetNumberOfCapabilities(int* count, int* size);
  STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* scc);
  STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) { return E_NOTIMPL; }
  STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD cb, DWORD* returned);
  STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support);

  void stopStreaming();
  void startStreaming();
  void dropConnection();
  void streamLoop();
};

class TypeEnum : public IEnumMediaTypes {
 public:
  long refs = 1;
  int index = 0;
  explicit TypeEnum(int start) : index(start) {}
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
      *ppv = static_cast<IEnumMediaTypes*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
  STDMETHODIMP_(ULONG) Release() {
    const ULONG left = InterlockedDecrement(&refs);
    if (!left) delete this;
    return left;
  }
  STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) {
    ULONG got = 0;
    while (got < count && index < kModeCount) {
      auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
      if (!mt) return E_OUTOFMEMORY;
      ZeroMemory(mt, sizeof(*mt));
      AM_MEDIA_TYPE built{};
      if (FAILED(makeType(index, built))) {
        CoTaskMemFree(mt);
        return E_OUTOFMEMORY;
      }
      *mt = built;
      types[got++] = mt;
      ++index;
    }
    if (fetched) *fetched = got;
    return got == count ? S_OK : S_FALSE;
  }
  STDMETHODIMP Skip(ULONG count) {
    index = std::min(kModeCount, index + static_cast<int>(count));
    return index >= kModeCount ? S_FALSE : S_OK;
  }
  STDMETHODIMP Reset() {
    index = 0;
    return S_OK;
  }
  STDMETHODIMP Clone(IEnumMediaTypes** clone) {
    if (!clone) return E_POINTER;
    auto* copy = new TypeEnum(index);
    *clone = copy;
    return S_OK;
  }
};

class Filter : public IBaseFilter, public IAMFilterMiscFlags {
 public:
  long refs = 1;
  CRITICAL_SECTION lock;
  FILTER_STATE state = State_Stopped;
  IFilterGraph* graph = nullptr;
  IReferenceClock* clock = nullptr;
  wchar_t name[128] = L"Conference FX Camera";
  Pin pin;

  Filter() {
    g_locks.fetch_add(1);
    InitializeCriticalSection(&lock);
    pin.filter = this;
    wcscpy_s(name, cfx::kCameraName);
  }
  ~Filter() {
    pin.stopStreaming();
    pin.dropConnection();
    if (clock) clock->Release();
    DeleteCriticalSection(&lock);
    g_locks.fetch_sub(1);
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
  STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
  STDMETHODIMP_(ULONG) Release() {
    const ULONG left = InterlockedDecrement(&refs);
    if (!left) delete this;
    return left;
  }
  STDMETHODIMP GetClassID(CLSID* clsid) {
    if (!clsid) return E_POINTER;
    *clsid = CLSID_ConferenceFxCam;
    return S_OK;
  }
  STDMETHODIMP Stop();
  STDMETHODIMP Pause();
  STDMETHODIMP Run(REFERENCE_TIME);
  STDMETHODIMP GetState(DWORD, FILTER_STATE* out);
  STDMETHODIMP SetSyncSource(IReferenceClock* source);
  STDMETHODIMP GetSyncSource(IReferenceClock** source);
  STDMETHODIMP EnumPins(IEnumPins** ppEnum);
  STDMETHODIMP FindPin(LPCWSTR id, IPin** out);
  STDMETHODIMP QueryFilterInfo(FILTER_INFO* info);
  STDMETHODIMP JoinFilterGraph(IFilterGraph* next, LPCWSTR nextName);
  STDMETHODIMP QueryVendorInfo(LPWSTR* vendor) {
    if (!vendor) return E_POINTER;
    *vendor = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP_(ULONG) GetMiscFlags() { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }
};

class PinEnum : public IEnumPins {
 public:
  Filter* filter = nullptr;
  long refs = 1;
  int index = 0;
  explicit PinEnum(Filter* owner, int start) : filter(owner), index(start) { filter->AddRef(); }
  ~PinEnum() { filter->Release(); }
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IEnumPins) {
      *ppv = static_cast<IEnumPins*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
  STDMETHODIMP_(ULONG) Release() {
    const ULONG left = InterlockedDecrement(&refs);
    if (!left) delete this;
    return left;
  }
  STDMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) {
    ULONG got = 0;
    if (index == 0 && count > 0) {
      pins[0] = &filter->pin;
      pins[0]->AddRef();
      ++index;
      got = 1;
    }
    if (fetched) *fetched = got;
    return got == count ? S_OK : S_FALSE;
  }
  STDMETHODIMP Skip(ULONG count) {
    index = std::min(1, index + static_cast<int>(count));
    return S_OK;
  }
  STDMETHODIMP Reset() {
    index = 0;
    return S_OK;
  }
  STDMETHODIMP Clone(IEnumPins** clone) {
    if (!clone) return E_POINTER;
    *clone = new PinEnum(filter, index);
    return S_OK;
  }
};

STDMETHODIMP Pin::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IPin) *ppv = static_cast<IPin*>(this);
  else if (riid == IID_IAMStreamConfig) *ppv = static_cast<IAMStreamConfig*>(this);
  else if (riid == IID_IKsPropertySet) *ppv = static_cast<IKsPropertySet*>(this);
  else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) Pin::AddRef() { return filter->AddRef(); }
STDMETHODIMP_(ULONG) Pin::Release() { return filter->Release(); }

STDMETHODIMP Pin::Connect(IPin* receivePin, const AM_MEDIA_TYPE* pmt) {
  if (!receivePin) return E_POINTER;
  if (connected) return VFW_E_ALREADY_CONNECTED;
  AM_MEDIA_TYPE chosen{};
  HRESULT hr = S_OK;
  if (pmt) {
    if (!acceptable(*pmt) || formatOf(pmt->subtype) < 0 || !pmt->pbFormat) return VFW_E_TYPE_NOT_ACCEPTED;
    hr = [&] {
      freeMt(chosen);
      chosen = *pmt;
      chosen.pbFormat = nullptr;
      chosen.pUnk = nullptr;
      chosen.cbFormat = 0;
      if (pmt->cbFormat && pmt->pbFormat) {
        chosen.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(pmt->cbFormat));
        if (!chosen.pbFormat) return E_OUTOFMEMORY;
        std::memcpy(chosen.pbFormat, pmt->pbFormat, pmt->cbFormat);
        chosen.cbFormat = pmt->cbFormat;
      }
      return S_OK;
    }();
    if (FAILED(hr)) return hr;
  } else {
    bool found = false;
    for (int i = 0; i < kModeCount; ++i) {
      AM_MEDIA_TYPE candidate{};
      if (FAILED(makeType(i, candidate))) continue;
      const HRESULT accept = receivePin->QueryAccept(&candidate);
      if (accept == S_OK) {
        chosen = candidate;
        found = true;
        break;
      }
      freeMt(candidate);
    }
    if (!found && FAILED(makeType(preferred, chosen))) return VFW_E_NO_ACCEPTABLE_TYPES;
  }

  hr = receivePin->ReceiveConnection(static_cast<IPin*>(this), &chosen);
  if (FAILED(hr)) {
    freeMt(chosen);
    return hr;
  }

  IMemInputPin* input = nullptr;
  hr = receivePin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&input));
  if (FAILED(hr)) {
    receivePin->Disconnect();
    freeMt(chosen);
    return hr;
  }
  IMemAllocator* alloc = nullptr;
  if (FAILED(input->GetAllocator(&alloc))) {
    hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                          reinterpret_cast<void**>(&alloc));
  }
  if (FAILED(hr) || !alloc) {
    input->Release();
    receivePin->Disconnect();
    freeMt(chosen);
    return FAILED(hr) ? hr : E_FAIL;
  }
  const FrameDesc desc = describe(chosen);
  ALLOCATOR_PROPERTIES props{};
  props.cBuffers = 4;
  props.cbBuffer = desc.bytes;
  props.cbAlign = 1;
  ALLOCATOR_PROPERTIES actual{};
  hr = alloc->SetProperties(&props, &actual);
  if (FAILED(hr) || actual.cbBuffer < desc.bytes) {
    alloc->Release();
    alloc = nullptr;
    hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                          reinterpret_cast<void**>(&alloc));
    if (SUCCEEDED(hr)) hr = alloc->SetProperties(&props, &actual);
  }
  if (FAILED(hr) || !alloc) {
    if (alloc) alloc->Release();
    input->Release();
    receivePin->Disconnect();
    freeMt(chosen);
    return hr;
  }
  hr = input->NotifyAllocator(alloc, FALSE);
  if (FAILED(hr)) {
    alloc->Release();
    input->Release();
    receivePin->Disconnect();
    freeMt(chosen);
    return hr;
  }
  dropConnection();
  connected = receivePin;
  connected->AddRef();
  memInput = input;
  allocator = alloc;
  freeMt(connection);
  connection = chosen;
  return S_OK;
}

void Pin::dropConnection() {
  stopStreaming();
  if (connected) connected->Release();
  if (memInput) memInput->Release();
  if (allocator) allocator->Release();
  connected = nullptr;
  memInput = nullptr;
  allocator = nullptr;
  freeMt(connection);
}

STDMETHODIMP Pin::Disconnect() {
  if (!connected) return S_FALSE;
  if (filter->state != State_Stopped) return VFW_E_NOT_STOPPED;
  dropConnection();
  return S_OK;
}

STDMETHODIMP Pin::ConnectedTo(IPin** out) {
  if (!out) return E_POINTER;
  if (!connected) {
    *out = nullptr;
    return VFW_E_NOT_CONNECTED;
  }
  *out = connected;
  connected->AddRef();
  return S_OK;
}

STDMETHODIMP Pin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
  if (!pmt) return E_POINTER;
  if (!connected) return VFW_E_NOT_CONNECTED;
  ZeroMemory(pmt, sizeof(*pmt));
  *pmt = connection;
  pmt->pbFormat = nullptr;
  pmt->pUnk = nullptr;
  pmt->cbFormat = 0;
  if (connection.cbFormat && connection.pbFormat) {
    pmt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(connection.cbFormat));
    if (!pmt->pbFormat) return E_OUTOFMEMORY;
    std::memcpy(pmt->pbFormat, connection.pbFormat, connection.cbFormat);
    pmt->cbFormat = connection.cbFormat;
  }
  return S_OK;
}

STDMETHODIMP Pin::QueryPinInfo(PIN_INFO* info) {
  if (!info) return E_POINTER;
  info->pFilter = filter;
  filter->AddRef();
  info->dir = PINDIR_OUTPUT;
  wcscpy_s(info->achName, L"Capture");
  return S_OK;
}

STDMETHODIMP Pin::QueryDirection(PIN_DIRECTION* dir) {
  if (!dir) return E_POINTER;
  *dir = PINDIR_OUTPUT;
  return S_OK;
}

STDMETHODIMP Pin::QueryId(LPWSTR* id) {
  if (!id) return E_POINTER;
  const size_t bytes = sizeof(L"Capture");
  auto* text = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
  if (!text) return E_OUTOFMEMORY;
  std::memcpy(text, L"Capture", bytes);
  *id = text;
  return S_OK;
}

STDMETHODIMP Pin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
  if (!pmt) return E_POINTER;
  return acceptable(*pmt) ? S_OK : S_FALSE;
}

STDMETHODIMP Pin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
  if (!ppEnum) return E_POINTER;
  *ppEnum = new TypeEnum(0);
  return S_OK;
}

STDMETHODIMP Pin::SetFormat(AM_MEDIA_TYPE* pmt) {
  if (!pmt) return E_POINTER;
  if (connected) return VFW_E_WRONG_STATE;
  if (!acceptable(*pmt)) return VFW_E_INVALIDMEDIATYPE;
  for (int i = 0; i < kModeCount; ++i) {
    if (kModes[i].format == formatOf(pmt->subtype)) {
      preferred = i;
      break;
    }
  }
  return S_OK;
}

STDMETHODIMP Pin::GetFormat(AM_MEDIA_TYPE** ppmt) {
  if (!ppmt) return E_POINTER;
  auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
  if (!mt) return E_OUTOFMEMORY;
  ZeroMemory(mt, sizeof(*mt));
  if (connection.pbFormat) {
    *mt = connection;
    mt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(connection.cbFormat));
    if (!mt->pbFormat) {
      CoTaskMemFree(mt);
      return E_OUTOFMEMORY;
    }
    std::memcpy(mt->pbFormat, connection.pbFormat, connection.cbFormat);
    mt->pUnk = nullptr;
    *ppmt = mt;
    return S_OK;
  }
  AM_MEDIA_TYPE built{};
  if (FAILED(makeType(preferred, built))) {
    CoTaskMemFree(mt);
    return E_OUTOFMEMORY;
  }
  *mt = built;
  *ppmt = mt;
  return S_OK;
}

STDMETHODIMP Pin::GetNumberOfCapabilities(int* count, int* size) {
  if (!count || !size) return E_POINTER;
  *count = kModeCount;
  *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
  return S_OK;
}

STDMETHODIMP Pin::GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* scc) {
  if (!ppmt || !scc) return E_POINTER;
  if (index < 0 || index >= kModeCount) return S_FALSE;
  auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
  if (!mt) return E_OUTOFMEMORY;
  ZeroMemory(mt, sizeof(*mt));
  AM_MEDIA_TYPE built{};
  if (FAILED(makeType(index, built))) {
    CoTaskMemFree(mt);
    return E_OUTOFMEMORY;
  }
  *mt = built;
  *ppmt = mt;
  auto* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(scc);
  ZeroMemory(caps, sizeof(*caps));
  caps->guid = FORMAT_VideoInfo;
  caps->InputSize.cx = caps->MinOutputSize.cx = caps->MaxOutputSize.cx = kModes[index].width;
  caps->InputSize.cy = caps->MinOutputSize.cy = caps->MaxOutputSize.cy = kModes[index].height;
  caps->OutputGranularityX = 1;
  caps->OutputGranularityY = 1;
  caps->MinFrameInterval = 333333;
  caps->MaxFrameInterval = 333333;
  const long bytes = imageBytes(kModes[index].width, kModes[index].height, kModes[index].format);
  caps->MinBitsPerSecond = caps->MaxBitsPerSecond = bytes * 30 * 8;
  return S_OK;
}

STDMETHODIMP Pin::Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD cb, DWORD* returned) {
  if (set != AMPROPSETID_Pin || id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
  if (!data || cb < sizeof(GUID)) return E_POINTER;
  *static_cast<GUID*>(data) = PIN_CATEGORY_CAPTURE;
  if (returned) *returned = sizeof(GUID);
  return S_OK;
}

STDMETHODIMP Pin::QuerySupported(REFGUID set, DWORD id, DWORD* support) {
  if (set != AMPROPSETID_Pin || id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
  if (support) *support = 1;
  return S_OK;
}

void Pin::stopStreaming() {
  streaming.store(false);
  if (allocator) allocator->Decommit();
  if (thread.joinable()) thread.join();
}

void Pin::startStreaming() {
  if (thread.joinable() || !allocator || !memInput) return;
  allocator->Commit();
  streaming.store(true);
  thread = std::thread([this] { streamLoop(); });
}

void Pin::streamLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  cfx::SharedFrames shared;
  shared.open();
  std::vector<uint8_t> raw;
  std::vector<uint8_t> scaled;
  long long frame = 0;
  auto next = std::chrono::steady_clock::now();
  while (streaming.load()) {
    IMediaSample* sample = nullptr;
    const HRESULT hr = allocator->GetBuffer(&sample, nullptr, nullptr, AM_GBF_NOWAIT);
    if (FAILED(hr) || !sample) {
      Sleep(2);
      continue;
    }
    BYTE* data = nullptr;
    const FrameDesc desc = describe(connection);
    if (SUCCEEDED(sample->GetPointer(&data)) && data && sample->GetSize() >= desc.bytes) {
      paintFrame(data, sample->GetSize(), desc, shared, raw, scaled);
      sample->SetActualDataLength(desc.bytes);
      REFERENCE_TIME t0 = frame * desc.duration;
      REFERENCE_TIME t1 = t0 + desc.duration;
      sample->SetTime(&t0, &t1);
      sample->SetSyncPoint(TRUE);
      sample->SetDiscontinuity(frame == 0);
      if (streaming.load() && memInput) memInput->Receive(sample);
      ++frame;
      next += std::chrono::nanoseconds(desc.duration * 100);
      const auto now = std::chrono::steady_clock::now();
      if (now < next) std::this_thread::sleep_until(next);
      else next = now;
    }
    sample->Release();
  }
  shared.close();
  CoUninitialize();
}

STDMETHODIMP Filter::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IBaseFilter) *ppv = static_cast<IBaseFilter*>(this);
  else if (riid == IID_IMediaFilter) *ppv = static_cast<IMediaFilter*>(static_cast<IBaseFilter*>(this));
  else if (riid == IID_IPersist) *ppv = static_cast<IPersist*>(static_cast<IBaseFilter*>(this));
  else if (riid == IID_IAMFilterMiscFlags) *ppv = static_cast<IAMFilterMiscFlags*>(this);
  else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP Filter::Stop() {
  EnterCriticalSection(&lock);
  pin.streaming.store(false);
  if (pin.allocator) pin.allocator->Decommit();
  state = State_Stopped;
  LeaveCriticalSection(&lock);
  if (pin.thread.joinable()) pin.thread.join();
  return S_OK;
}

STDMETHODIMP Filter::Pause() {
  EnterCriticalSection(&lock);
  state = State_Paused;
  LeaveCriticalSection(&lock);
  return S_OK;
}

STDMETHODIMP Filter::Run(REFERENCE_TIME) {
  EnterCriticalSection(&lock);
  state = State_Running;
  pin.startStreaming();
  LeaveCriticalSection(&lock);
  return S_OK;
}

STDMETHODIMP Filter::GetState(DWORD, FILTER_STATE* out) {
  if (!out) return E_POINTER;
  *out = state;
  return state == State_Paused ? VFW_S_CANT_CUE : S_OK;
}

STDMETHODIMP Filter::SetSyncSource(IReferenceClock* source) {
  EnterCriticalSection(&lock);
  if (clock) clock->Release();
  clock = source;
  if (clock) clock->AddRef();
  LeaveCriticalSection(&lock);
  return S_OK;
}

STDMETHODIMP Filter::GetSyncSource(IReferenceClock** source) {
  if (!source) return E_POINTER;
  EnterCriticalSection(&lock);
  *source = clock;
  if (clock) clock->AddRef();
  LeaveCriticalSection(&lock);
  return S_OK;
}

STDMETHODIMP Filter::EnumPins(IEnumPins** ppEnum) {
  if (!ppEnum) return E_POINTER;
  *ppEnum = new PinEnum(this, 0);
  return S_OK;
}

STDMETHODIMP Filter::FindPin(LPCWSTR id, IPin** out) {
  if (!out) return E_POINTER;
  if (id && wcscmp(id, L"Capture") == 0) {
    *out = &pin;
    pin.AddRef();
    return S_OK;
  }
  *out = nullptr;
  return VFW_E_NOT_FOUND;
}

STDMETHODIMP Filter::QueryFilterInfo(FILTER_INFO* info) {
  if (!info) return E_POINTER;
  wcscpy_s(info->achName, name);
  info->pGraph = graph;
  if (graph) graph->AddRef();
  return S_OK;
}

STDMETHODIMP Filter::JoinFilterGraph(IFilterGraph* next, LPCWSTR nextName) {
  EnterCriticalSection(&lock);
  graph = next;
  if (nextName) wcscpy_s(name, nextName);
  else wcscpy_s(name, cfx::kCameraName);
  LeaveCriticalSection(&lock);
  return S_OK;
}

class Factory : public IClassFactory {
 public:
  long refs = 1;
  Factory() { g_locks.fetch_add(1); }
  ~Factory() { g_locks.fetch_sub(1); }
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *ppv = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
  STDMETHODIMP_(ULONG) Release() {
    const ULONG left = InterlockedDecrement(&refs);
    if (!left) delete this;
    return left;
  }
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!ppv) return E_POINTER;
    auto* filter = new Filter();
    const HRESULT hr = filter->QueryInterface(riid, ppv);
    filter->Release();
    return hr;
  }
  STDMETHODIMP LockServer(BOOL lock) {
    if (lock) g_locks.fetch_add(1);
    else g_locks.fetch_sub(1);
    return S_OK;
  }
};

bool setSz(HKEY key, const wchar_t* name, const wchar_t* value) {
  const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
  return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes) == ERROR_SUCCESS;
}

HRESULT writeHive(HKEY root) {
  wchar_t path[MAX_PATH]{};
  if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return E_FAIL;
  const std::wstring clsidPath = std::wstring(L"Software\\Classes\\CLSID\\") + cfx::kCameraClsid;
  HKEY clsid = nullptr;
  if (RegCreateKeyExW(root, clsidPath.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &clsid, nullptr) != ERROR_SUCCESS) {
    return E_FAIL;
  }
  setSz(clsid, nullptr, cfx::kCameraName);
  HKEY inproc = nullptr;
  const LONG opened = RegCreateKeyExW(clsid, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc, nullptr);
  RegCloseKey(clsid);
  if (opened != ERROR_SUCCESS) return E_FAIL;
  setSz(inproc, nullptr, path);
  setSz(inproc, L"ThreadingModel", L"Both");
  RegCloseKey(inproc);

  const std::wstring instance = std::wstring(L"Software\\Classes\\CLSID\\") + cfx::kVideoInputCategory + L"\\Instance\\" +
                                cfx::kCameraClsid;
  HKEY inst = nullptr;
  if (RegCreateKeyExW(root, instance.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &inst, nullptr) != ERROR_SUCCESS) {
    return E_FAIL;
  }
  setSz(inst, L"FriendlyName", cfx::kCameraName);
  setSz(inst, L"CLSID", cfx::kCameraClsid);
  RegCloseKey(inst);
  return S_OK;
}

void deleteHive(HKEY root) {
  const std::wstring clsidPath = std::wstring(L"Software\\Classes\\CLSID\\") + cfx::kCameraClsid;
  const std::wstring instance = std::wstring(L"Software\\Classes\\CLSID\\") + cfx::kVideoInputCategory + L"\\Instance\\" +
                                cfx::kCameraClsid;
  RegDeleteTreeW(root, clsidPath.c_str());
  RegDeleteTreeW(root, instance.c_str());
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    DisableThreadLibraryCalls(module);
  }
  return TRUE;
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (clsid != CLSID_ConferenceFxCam) return CLASS_E_CLASSNOTAVAILABLE;
  auto* factory = new Factory();
  const HRESULT hr = factory->QueryInterface(riid, ppv);
  factory->Release();
  return hr;
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllCanUnloadNow() {
  return g_locks.load() == 0 ? S_OK : S_FALSE;
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllRegisterServer() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const HRESULT hkcu = writeHive(HKEY_CURRENT_USER);
  writeHive(HKEY_LOCAL_MACHINE);
  if (SUCCEEDED(com)) CoUninitialize();
  return hkcu;
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllUnregisterServer() {
  deleteHive(HKEY_CURRENT_USER);
  deleteHive(HKEY_LOCAL_MACHINE);
  return S_OK;
}
