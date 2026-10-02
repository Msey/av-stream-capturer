#include "capture/devices.hpp"

#include <algorithm>
#include <cwctype>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mfidl.h>

namespace cfx {
namespace {

// PKEY_Device_FriendlyName, pid 14. Defined locally so this file does not need INITGUID.
const PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::wstring lowerCopy(std::wstring text) {
  for (wchar_t& ch : text) ch = static_cast<wchar_t>(towlower(ch));
  return text;
}

}  // namespace

bool looksLikeCable(const std::wstring& name) {
  const std::wstring lower = lowerCopy(name);
  return lower.find(L"cable input") != std::wstring::npos || lower.find(L"vb-audio") != std::wstring::npos ||
         lower.find(L"voicemeeter") != std::wstring::npos || lower.find(L"virtual cable") != std::wstring::npos ||
         lower.find(L"cable") != std::wstring::npos;
}

std::wstring defaultAudioId(bool capture) {
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return {};
  IMMDevice* device = nullptr;
  const EDataFlow flow = capture ? eCapture : eRender;
  HRESULT hr = enumerator->GetDefaultAudioEndpoint(flow, eCommunications, &device);
  if (FAILED(hr)) hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
  std::wstring id;
  if (SUCCEEDED(hr) && device) {
    LPWSTR raw = nullptr;
    if (SUCCEEDED(device->GetId(&raw)) && raw) {
      id = raw;
      CoTaskMemFree(raw);
    }
    device->Release();
  }
  enumerator->Release();
  return id;
}

std::vector<AudioEndpoint> enumerateAudio(bool capture) {
  std::vector<AudioEndpoint> result;
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return result;
  const std::wstring fallback = defaultAudioId(capture);
  IMMDeviceCollection* collection = nullptr;
  const EDataFlow flow = capture ? eCapture : eRender;
  if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
    enumerator->Release();
    return result;
  }
  UINT count = 0;
  collection->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* device = nullptr;
    if (FAILED(collection->Item(i, &device))) continue;
    AudioEndpoint endpoint;
    endpoint.capture = capture;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(device->GetId(&raw)) && raw) {
      endpoint.id = raw;
      CoTaskMemFree(raw);
    }
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
      PROPVARIANT value;
      PropVariantInit(&value);
      if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        endpoint.name = value.pwszVal;
      }
      PropVariantClear(&value);
      store->Release();
    }
    if (endpoint.name.empty()) endpoint.name = endpoint.capture ? L"Микрофон" : L"Выход";
    endpoint.cable = !capture && looksLikeCable(endpoint.name);
    endpoint.isDefault = !fallback.empty() && endpoint.id == fallback;
    result.push_back(std::move(endpoint));
    device->Release();
  }
  collection->Release();
  enumerator->Release();
  return result;
}

std::vector<CameraDevice> enumerateCameras() {
  std::vector<CameraDevice> result;
  IMFAttributes* attributes = nullptr;
  if (FAILED(MFCreateAttributes(&attributes, 1))) return result;
  attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  const HRESULT hr = MFEnumDeviceSources(attributes, &devices, &count);
  attributes->Release();
  if (FAILED(hr) || !devices) return result;
  for (UINT32 i = 0; i < count; ++i) {
    CameraDevice camera;
    wchar_t* name = nullptr;
    UINT32 nameLen = 0;
    if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen)) && name) {
      camera.name = name;
      CoTaskMemFree(name);
    }
    wchar_t* link = nullptr;
    UINT32 linkLen = 0;
    if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link,
                                                &linkLen)) &&
        link) {
      camera.link = link;
      CoTaskMemFree(link);
    }
    if (!camera.link.empty()) {
      if (camera.name.empty()) camera.name = L"Камера";
      result.push_back(std::move(camera));
    }
    devices[i]->Release();
  }
  CoTaskMemFree(devices);
  return result;
}

}  // namespace cfx
