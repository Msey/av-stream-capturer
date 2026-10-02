#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dshow.h>

#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>

namespace {

const GUID CLSID_ConferenceFxCam = {0xa7c3e1b2, 0x4f58, 0x4d1a, {0x9e, 0x63, 0x0b, 0x8f, 0x2d, 0x6a, 0x71, 0xc4}};
std::atomic<int> g_samples{0};

class SinkPin : public IPin, public IMemInputPin {
 public:
  long refs = 1;
  IMemAllocator* allocator = nullptr;
  AM_MEDIA_TYPE type{};

  SinkPin() { ZeroMemory(&type, sizeof(type)); }
  ~SinkPin() {
    if (allocator) allocator->Release();
    if (type.pbFormat) CoTaskMemFree(type.pbFormat);
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) *ppv = static_cast<IPin*>(this);
    else if (riid == IID_IMemInputPin) *ppv = static_cast<IMemInputPin*>(this);
    else {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
  STDMETHODIMP_(ULONG) Release() {
    const ULONG left = InterlockedDecrement(&refs);
    if (!left) delete this;
    return left;
  }
  STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) { return E_UNEXPECTED; }
  STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    if (mt->majortype != MEDIATYPE_Video) return VFW_E_TYPE_NOT_ACCEPTED;
    if (type.pbFormat) CoTaskMemFree(type.pbFormat);
    type = *mt;
    type.pUnk = nullptr;
    type.pbFormat = nullptr;
    type.cbFormat = 0;
    if (mt->cbFormat && mt->pbFormat) {
      type.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(mt->cbFormat));
      if (!type.pbFormat) return E_OUTOFMEMORY;
      std::memcpy(type.pbFormat, mt->pbFormat, mt->cbFormat);
      type.cbFormat = mt->cbFormat;
    }
    return S_OK;
  }
  STDMETHODIMP Disconnect() {
    if (type.pbFormat) CoTaskMemFree(type.pbFormat);
    ZeroMemory(&type, sizeof(type));
    return S_OK;
  }
  STDMETHODIMP ConnectedTo(IPin** pin) {
    if (pin) *pin = nullptr;
    return VFW_E_NOT_CONNECTED;
  }
  STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    *mt = type;
    mt->pbFormat = nullptr;
    mt->pUnk = nullptr;
    if (type.cbFormat && type.pbFormat) {
      mt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(type.cbFormat));
      if (!mt->pbFormat) return E_OUTOFMEMORY;
      std::memcpy(mt->pbFormat, type.pbFormat, type.cbFormat);
    }
    return S_OK;
  }
  STDMETHODIMP QueryPinInfo(PIN_INFO* info) {
    if (!info) return E_POINTER;
    ZeroMemory(info, sizeof(*info));
    info->dir = PINDIR_INPUT;
    wcscpy_s(info->achName, L"In");
    return S_OK;
  }
  STDMETHODIMP QueryDirection(PIN_DIRECTION* dir) {
    if (!dir) return E_POINTER;
    *dir = PINDIR_INPUT;
    return S_OK;
  }
  STDMETHODIMP QueryId(LPWSTR* id) {
    if (!id) return E_POINTER;
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(L"In")));
    if (!*id) return E_OUTOFMEMORY;
    std::memcpy(*id, L"In", sizeof(L"In"));
    return S_OK;
  }
  STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    return mt->majortype == MEDIATYPE_Video ? S_OK : S_FALSE;
  }
  STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** en) {
    if (en) *en = nullptr;
    return VFW_E_NOT_FOUND;
  }
  STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) { return E_NOTIMPL; }
  STDMETHODIMP EndOfStream() { return S_OK; }
  STDMETHODIMP BeginFlush() { return S_OK; }
  STDMETHODIMP EndFlush() { return S_OK; }
  STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

  STDMETHODIMP GetAllocator(IMemAllocator** out) {
    if (!out) return E_POINTER;
    if (!allocator) {
      const HRESULT hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                                          reinterpret_cast<void**>(&allocator));
      if (FAILED(hr)) return hr;
    }
    *out = allocator;
    allocator->AddRef();
    return S_OK;
  }
  STDMETHODIMP NotifyAllocator(IMemAllocator* next, BOOL) {
    if (allocator) allocator->Release();
    allocator = next;
    if (allocator) allocator->AddRef();
    return S_OK;
  }
  STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES* props) {
    if (!props) return E_POINTER;
    ZeroMemory(props, sizeof(*props));
    return S_OK;
  }
  STDMETHODIMP Receive(IMediaSample* sample) {
    if (!sample) return E_POINTER;
    if (sample->GetActualDataLength() > 0) g_samples.fetch_add(1);
    return S_OK;
  }
  STDMETHODIMP ReceiveMultiple(IMediaSample** samples, long count, long* processed) {
    long got = 0;
    for (long i = 0; i < count; ++i) {
      if (FAILED(Receive(samples[i]))) break;
      ++got;
    }
    if (processed) *processed = got;
    return got == count ? S_OK : S_FALSE;
  }
  STDMETHODIMP ReceiveCanBlock() { return S_FALSE; }
};

std::wstring dllBesideExe() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring text(path);
  const auto slash = text.find_last_of(L"\\/");
  if (slash != std::wstring::npos) text.resize(slash);
  return text + L"\\ConferenceFxCam.dll";
}

}  // namespace

int main() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const std::wstring path = dllBesideExe();
  HMODULE library = LoadLibraryW(path.c_str());
  if (!library) {
    std::printf("ConferenceFxCam.dll was not found beside the test\n");
    return 1;
  }
  using RegFn = HRESULT(STDAPICALLTYPE*)();
  auto reg = reinterpret_cast<RegFn>(GetProcAddress(library, "DllRegisterServer"));
  if (!reg || FAILED(reg())) {
    std::printf("DllRegisterServer failed\n");
    FreeLibrary(library);
    return 1;
  }

  IBaseFilter* filter = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_ConferenceFxCam, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                                reinterpret_cast<void**>(&filter));
  if (FAILED(hr) || !filter) {
    std::printf("CoCreateInstance failed 0x%08lx\n", static_cast<unsigned long>(hr));
    FreeLibrary(library);
    return 1;
  }

  IEnumPins* pins = nullptr;
  filter->EnumPins(&pins);
  IPin* pin = nullptr;
  ULONG fetched = 0;
  if (!pins || pins->Next(1, &pin, &fetched) != S_OK || !pin) {
    std::printf("filter has no pin\n");
    if (pins) pins->Release();
    filter->Release();
    FreeLibrary(library);
    return 1;
  }
  pins->Release();

  IKsPropertySet* props = nullptr;
  pin->QueryInterface(IID_IKsPropertySet, reinterpret_cast<void**>(&props));
  if (!props) {
    std::printf("pin has no category\n");
    pin->Release();
    filter->Release();
    FreeLibrary(library);
    return 1;
  }
  GUID category{};
  DWORD returned = 0;
  hr = props->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &category, sizeof(category), &returned);
  props->Release();
  if (FAILED(hr) || category != PIN_CATEGORY_CAPTURE) {
    std::printf("pin category is not CAPTURE\n");
    pin->Release();
    filter->Release();
    FreeLibrary(library);
    return 1;
  }

  auto* sink = new SinkPin();
  hr = pin->Connect(sink, nullptr);
  if (FAILED(hr)) {
    std::printf("Connect failed 0x%08lx\n", static_cast<unsigned long>(hr));
    sink->Release();
    pin->Release();
    filter->Release();
    FreeLibrary(library);
    return 1;
  }

  IMediaFilter* media = nullptr;
  filter->QueryInterface(IID_IMediaFilter, reinterpret_cast<void**>(&media));
  media->Run(0);
  Sleep(400);
  media->Stop();
  media->Release();
  const int samples = g_samples.load();
  std::printf("received %d frames\n", samples);

  pin->Disconnect();
  sink->Release();
  pin->Release();
  filter->Release();
  FreeLibrary(library);
  if (SUCCEEDED(com)) CoUninitialize();
  if (samples < 3) {
    std::printf("virtual camera did not deliver frames\n");
    return 1;
  }
  std::printf("vcam smoke ok\n");
  return 0;
}
