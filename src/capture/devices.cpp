#include "capture/devices.hpp"

#include <algorithm>
#include <cwctype>
#include <audiopolicy.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
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

bool containsLower(const std::wstring& name, const wchar_t* needle) {
  return lowerCopy(name).find(needle) != std::wstring::npos;
}

bool looksLikeCable(const std::wstring& name) {
  return isVirtualCablePlayback(name) || isVirtualCableMicrophone(name);
}

bool isVirtualCablePlayback(const std::wstring& name) {
  return containsLower(name, L"cable input") || containsLower(name, L"voicemeeter input") ||
         containsLower(name, L"voicemeeter aux input");
}

bool isVirtualCableMicrophone(const std::wstring& name) {
  return containsLower(name, L"cable output") || containsLower(name, L"voicemeeter output") ||
         containsLower(name, L"voicemeeter aux output");
}

std::wstring virtualCablePlaybackId(const std::vector<AudioEndpoint>& outputs) {
  std::wstring fallback;
  for (const AudioEndpoint& endpoint : outputs) {
    if (!endpoint.active || !isVirtualCablePlayback(endpoint.name)) continue;
    if (containsLower(endpoint.name, L"cable input")) return endpoint.id;
    if (fallback.empty()) fallback = endpoint.id;
  }
  return fallback;
}

std::wstring virtualCableMicrophoneName(const std::vector<AudioEndpoint>& inputs) {
  std::wstring fallback;
  for (const AudioEndpoint& endpoint : inputs) {
    if (!endpoint.active || !isVirtualCableMicrophone(endpoint.name)) continue;
    if (containsLower(endpoint.name, L"cable output")) return endpoint.name;
    if (fallback.empty()) fallback = endpoint.name;
  }
  return fallback;
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
  const DWORD states = DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED;
  if (FAILED(enumerator->EnumAudioEndpoints(flow, states, &collection))) {
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
    DWORD state = DEVICE_STATE_ACTIVE;
    device->GetState(&state);
    endpoint.active = state == DEVICE_STATE_ACTIVE;
    if (endpoint.name.empty()) endpoint.name = endpoint.capture ? L"Микрофон" : L"Выход";
    if (!endpoint.active) endpoint.name += L" (не подключено)";
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

std::wstring speakerId() {
  const std::vector<AudioEndpoint> outputs = enumerateAudio(false);
  std::wstring fallback;
  for (const AudioEndpoint& endpoint : outputs) {
    if (!endpoint.active || isVirtualCablePlayback(endpoint.name)) continue;
    if (endpoint.isDefault) return endpoint.id;
    if (fallback.empty()) fallback = endpoint.id;
  }
  return fallback;
}

std::vector<AudioApp> enumerateAudioApps() {
  std::vector<AudioApp> result;
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
    return result;
  }
  IMMDeviceCollection* collection = nullptr;
  if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
    enumerator->Release();
    return result;
  }
  const DWORD self = GetCurrentProcessId();
  UINT count = 0;
  collection->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* device = nullptr;
    if (FAILED(collection->Item(i, &device))) continue;
    IAudioSessionManager2* manager = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager)))) {
      device->Release();
      continue;
    }
    IAudioSessionEnumerator* sessions = nullptr;
    if (FAILED(manager->GetSessionEnumerator(&sessions))) {
      manager->Release();
      device->Release();
      continue;
    }
    int sessionCount = 0;
    sessions->GetCount(&sessionCount);
    for (int s = 0; s < sessionCount; ++s) {
      IAudioSessionControl* control = nullptr;
      if (FAILED(sessions->GetSession(s, &control))) continue;
      IAudioSessionControl2* control2 = nullptr;
      if (FAILED(control->QueryInterface(IID_PPV_ARGS(&control2)))) {
        control->Release();
        continue;
      }
      AudioSessionState state = AudioSessionStateInactive;
      control->GetState(&state);
      DWORD pid = 0;
      control2->GetProcessId(&pid);
      control2->Release();
      control->Release();
      if (state != AudioSessionStateActive || pid == 0 || pid == self) continue;
      bool known = false;
      for (const AudioApp& app : result) {
        if (app.pid == pid) known = true;
      }
      if (known) continue;
      AudioApp app;
      app.pid = pid;
      wchar_t path[MAX_PATH]{};
      DWORD pathSize = MAX_PATH;
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (process && QueryFullProcessImageNameW(process, 0, path, &pathSize)) {
        std::wstring text(path);
        const auto slash = text.find_last_of(L"\\/");
        app.name = slash == std::wstring::npos ? text : text.substr(slash + 1);
      }
      if (process) CloseHandle(process);
      if (app.name.empty()) app.name = L"Процесс " + std::to_wstring(pid);
      if (containsLower(app.name, L"audiodg")) continue;
      result.push_back(std::move(app));
    }
    sessions->Release();
    manager->Release();
    device->Release();
  }
  collection->Release();
  enumerator->Release();
  std::sort(result.begin(), result.end(), [](const AudioApp& a, const AudioApp& b) { return a.name < b.name; });
  return result;
}

}  // namespace cfx
