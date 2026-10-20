#include "capture/audio_engine.hpp"
#include "capture/camera.hpp"
#include "capture/devices.hpp"
#include "common/params.hpp"
#include "common/shared_frame.hpp"
#include "vcam/camera_name.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace {

cfx::Params g_params;
cfx::SharedFrames g_shared;
cfx::AudioEngine* g_audio = nullptr;
cfx::CameraCapture* g_camera = nullptr;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11Texture2D* g_preview = nullptr;
ID3D11ShaderResourceView* g_previewSrv = nullptr;
int g_previewW = 0;
int g_previewH = 0;

std::wstring g_micId;
std::wstring g_outId;
std::wstring g_camLink;
bool g_outputLocked = false;

std::string utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring dllPath() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring text(path);
  const auto slash = text.find_last_of(L"\\/");
  if (slash != std::wstring::npos) text.resize(slash);
  return text + L"\\ConferenceFxCam.dll";
}

bool cameraRegistered() {
  const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + cfx::kVideoInputCategory + L"\\Instance\\" +
                           cfx::kCameraClsid;
  HKEY handle = nullptr;
  const bool ok = RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &handle) == ERROR_SUCCESS;
  if (ok) RegCloseKey(handle);
  return ok;
}

bool registerCamera() {
  const std::wstring path = dllPath();
  HMODULE library = LoadLibraryW(path.c_str());
  if (!library) return false;
  using RegFn = HRESULT(STDAPICALLTYPE*)();
  auto reg = reinterpret_cast<RegFn>(GetProcAddress(library, "DllRegisterServer"));
  const HRESULT hr = reg ? reg() : E_FAIL;
  FreeLibrary(library);
  return SUCCEEDED(hr);
}

bool createDevice(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC desc{};
  desc.BufferCount = 2;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.OutputWindow = hwnd;
  desc.SampleDesc.Count = 1;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL chosen{};
  if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                          &desc, &g_swap, &g_device, &chosen, &g_context))) {
    return false;
  }
  return true;
}

void createRtv() {
  if (g_rtv) {
    g_rtv->Release();
    g_rtv = nullptr;
  }
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
  g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
  back->Release();
}

void ensurePreview(int width, int height) {
  if (g_preview && g_previewW == width && g_previewH == height) return;
  if (g_previewSrv) g_previewSrv->Release();
  if (g_preview) g_preview->Release();
  g_preview = nullptr;
  g_previewSrv = nullptr;
  g_previewW = width;
  g_previewH = height;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_preview))) return;
  D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = desc.Format;
  srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  srv.Texture2D.MipLevels = 1;
  g_device->CreateShaderResourceView(g_preview, &srv, &g_previewSrv);
}

void uploadPreview(const cfx::PreviewFrame& frame) {
  if (!g_device || frame.bgra.empty() || frame.width <= 0 || frame.height <= 0) return;
  ensurePreview(frame.width, frame.height);
  if (!g_preview) return;
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(g_context->Map(g_preview, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
  const int row = frame.width * 4;
  for (int y = 0; y < frame.height; ++y) {
    std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                frame.bgra.data() + static_cast<size_t>(y) * row, row);
  }
  g_context->Unmap(g_preview, 0);
}

void applyPreset(int preset) {
  g_params.applyPreset(static_cast<cfx::Preset>(preset));
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
  if (msg == WM_SIZE && g_swap && wparam != SIZE_MINIMIZED) {
    if (g_rtv) {
      g_rtv->Release();
      g_rtv = nullptr;
    }
    g_swap->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
    createRtv();
    return 0;
  }
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  MFStartup(MF_VERSION);
  g_shared.open();

  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = wndProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = L"ConferenceFxWindow";
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&windowClass);
  HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"Conference FX", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, 1180, 820, nullptr, nullptr, instance, nullptr);
  if (!hwnd || !createDevice(hwnd)) {
    MessageBoxW(hwnd, L"Не удалось создать окно Direct3D.", L"Conference FX", MB_ICONERROR);
    if (hwnd) DestroyWindow(hwnd);
    g_shared.close();
    MFShutdown();
    if (com == S_OK) CoUninitialize();
    return 1;
  }
  createRtv();
  ShowWindow(hwnd, show);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  const float dpi = GetDpiForWindow(hwnd) / 96.f;
  ImFontConfig fontConfig;
  fontConfig.OversampleH = 2;
  fontConfig.OversampleV = 1;
  if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.f * dpi, &fontConfig,
                                    io.Fonts->GetGlyphRangesCyrillic())) {
    io.Fonts->AddFontDefault();
  }
  ImGui::StyleColorsDark();
  ImGui::GetStyle().ScaleAllSizes(dpi);
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_device, g_context);

  auto microphones = cfx::enumerateAudio(true);
  auto outputs = cfx::enumerateAudio(false);
  auto cameras = cfx::enumerateCameras();
  g_micId = cfx::defaultAudioId(true);
  const std::wstring cablePlayback = cfx::virtualCablePlaybackId(outputs);
  g_outId = !cablePlayback.empty() ? cablePlayback : cfx::defaultAudioId(false);
  if (!cameras.empty()) g_camLink = cameras.front().link;

  g_audio = new cfx::AudioEngine(g_params);
  g_camera = new cfx::CameraCapture(g_params, g_shared);
  g_audio->start(g_micId, g_outId);
  g_camera->start(g_camLink);

  bool registered = cameraRegistered();
  int selectedPreset = static_cast<int>(cfx::Preset::Cartoon);
  auto lastEnum = GetTickCount64();

  MSG message{};
  while (message.message != WM_QUIT) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (message.message == WM_QUIT) break;

    if (GetTickCount64() - lastEnum > 2000) {
      microphones = cfx::enumerateAudio(true);
      outputs = cfx::enumerateAudio(false);
      cameras = cfx::enumerateCameras();
      registered = cameraRegistered();
      if (!g_outputLocked) {
        const std::wstring cablePlayback = cfx::virtualCablePlaybackId(outputs);
        if (!cablePlayback.empty() && cablePlayback != g_outId) {
          g_outId = cablePlayback;
          g_audio->setDevices(g_micId, g_outId);
        }
      }
      lastEnum = GetTickCount64();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Conference FX", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar);

    cfx::PreviewFrame preview;
    if (g_camera->copyPreview(preview)) uploadPreview(preview);
    if (g_previewSrv) {
      const float avail = ImGui::GetContentRegionAvail().x * 0.62f;
      const float height = avail * (preview.height > 0 ? static_cast<float>(preview.height) / preview.width : 0.5625f);
      ImGui::Image(reinterpret_cast<ImTextureID>(g_previewSrv), ImVec2(avail, height), ImVec2(0, 0), ImVec2(1, 1),
                   ImVec4(1, 1, 1, 1), ImVec4(0.35f, 0.35f, 0.35f, 1));
    } else {
      ImGui::TextUnformatted("Нет кадра с камеры");
    }
    ImGui::SameLine();
    ImGui::BeginChild("controls", ImVec2(0, 0), false);

    const char* presets[] = {"Выключено", "Мультфильм", "Только зрачки", "Свой"};
    if (ImGui::Combo("Пресет", &selectedPreset, presets, 4) && selectedPreset != static_cast<int>(cfx::Preset::Custom)) {
      applyPreset(selectedPreset);
    }

    bool voice = g_params.voiceEnabled.load() != 0;
    bool pupils = g_params.pupilsEnabled.load() != 0;
    bool mirror = g_params.mirror.load() != 0;
    bool monitor = g_params.monitor.load() != 0;
    if (ImGui::Checkbox("Мультяшный голос", &voice)) {
      g_params.voiceEnabled.store(voice ? 1 : 0);
      selectedPreset = static_cast<int>(cfx::Preset::Custom);
      g_params.preset.store(selectedPreset);
    }
    if (ImGui::Checkbox("Белые зрачки", &pupils)) {
      g_params.pupilsEnabled.store(pupils ? 1 : 0);
      selectedPreset = static_cast<int>(cfx::Preset::Custom);
      g_params.preset.store(selectedPreset);
    }
    if (ImGui::Checkbox("Зеркало", &mirror)) g_params.mirror.store(mirror ? 1 : 0);
    if (ImGui::Checkbox("Слушать в наушниках", &monitor)) g_params.monitor.store(monitor ? 1 : 0);

    float pitch = g_params.pitchSemitones.load();
    float formant = g_params.formantSemitones.load();
    float mix = g_params.voiceMix.load();
    int lip = g_params.lipExtraMs.load();
    if (ImGui::SliderFloat("Тон, полутона", &pitch, -12.f, 12.f, "%.1f")) {
      g_params.pitchSemitones.store(pitch);
      selectedPreset = static_cast<int>(cfx::Preset::Custom);
    }
    if (ImGui::SliderFloat("Форманты вверх", &formant, 0.f, 12.f, "%.1f")) {
      g_params.formantSemitones.store(formant);
      selectedPreset = static_cast<int>(cfx::Preset::Custom);
    }
    if (ImGui::SliderFloat("Смесь голоса", &mix, 0.f, 1.f, "%.2f")) {
      g_params.voiceMix.store(mix);
      selectedPreset = static_cast<int>(cfx::Preset::Custom);
    }
    if (ImGui::SliderInt("Доп. сдвиг звука, мс", &lip, -80, 200)) g_params.lipExtraMs.store(lip);

    ImGui::Separator();
    ImGui::Text("Кадр: %dx%d, %.1f мс, %.0f fps, лицо: %s", preview.width, preview.height,
                g_params.videoProcessMs.load(), g_params.captureFps.load(), g_params.faceFound.load() ? "да" : "нет");
    ImGui::Text("Звук: %.0f мс (алгоритм %.0f + устройство %.0f)", g_params.audioTotalMs.load(),
                g_params.audioAlgoMs.load(), g_params.wasapiMs.load());
    ImGui::TextWrapped("%s", g_camera->status().c_str());
    ImGui::TextWrapped("%s", g_audio->status().c_str());

    ImGui::Separator();
    auto comboDevices = [](const char* label, const std::vector<std::string>& names, int& current) {
      if (names.empty()) {
        ImGui::TextUnformatted(label);
        return false;
      }
      current = std::clamp(current, 0, static_cast<int>(names.size()) - 1);
      return ImGui::Combo(label, &current, [](void* data, int idx, const char** out) {
        auto* list = static_cast<std::vector<std::string>*>(data);
        if (idx < 0 || idx >= static_cast<int>(list->size())) return false;
        *out = (*list)[static_cast<size_t>(idx)].c_str();
        return true;
      }, const_cast<std::vector<std::string>*>(&names), static_cast<int>(names.size()));
    };

    std::vector<std::string> camNames;
    int camIndex = 0;
    for (int i = 0; i < static_cast<int>(cameras.size()); ++i) {
      camNames.push_back(utf8(cameras[static_cast<size_t>(i)].name));
      if (cameras[static_cast<size_t>(i)].link == g_camLink) camIndex = i;
    }
    if (comboDevices("Камера", camNames, camIndex) && camIndex >= 0 && camIndex < static_cast<int>(cameras.size())) {
      g_camLink = cameras[static_cast<size_t>(camIndex)].link;
      g_camera->setDevice(g_camLink);
    }

    std::vector<std::string> micNames;
    int micIndex = 0;
    for (int i = 0; i < static_cast<int>(microphones.size()); ++i) {
      micNames.push_back(utf8(microphones[static_cast<size_t>(i)].name));
      if (microphones[static_cast<size_t>(i)].id == g_micId) micIndex = i;
    }
    if (comboDevices("Микрофон", micNames, micIndex) && micIndex >= 0 &&
        micIndex < static_cast<int>(microphones.size())) {
      g_micId = microphones[static_cast<size_t>(micIndex)].id;
      g_audio->setDevices(g_micId, g_outId);
    }

    std::vector<std::string> outNames;
    int outIndex = 0;
    for (int i = 0; i < static_cast<int>(outputs.size()); ++i) {
      std::string name = utf8(outputs[static_cast<size_t>(i)].name);
      if (outputs[static_cast<size_t>(i)].cable) name += " (виртуальный кабель)";
      outNames.push_back(name);
      if (outputs[static_cast<size_t>(i)].id == g_outId) outIndex = i;
    }
    if (comboDevices("Куда писать звук", outNames, outIndex) && outIndex >= 0 &&
        outIndex < static_cast<int>(outputs.size())) {
      g_outputLocked = true;
      g_outId = outputs[static_cast<size_t>(outIndex)].id;
      g_audio->setDevices(g_micId, g_outId);
    }

    ImGui::Separator();
    const std::wstring appMicName = cfx::virtualCableMicrophoneName(microphones);
    if (!appMicName.empty()) {
      const std::string appMicLabel = utf8(appMicName);
      ImGui::TextWrapped(
          "Голосовые и звонки: в Телеграме, Скайпе и Teams выберите микрофон «%s». Туда уже пишется обработанный голос.",
          appMicLabel.c_str());
    } else {
      ImGui::TextWrapped(
          "Чтобы мультяшный голос попал в голосовое и на созвон, поставьте VB-CABLE с vb-audio.com. Это отдельный donationware-драйвер, его нельзя встроить в установку программы. После установки перезагрузите компьютер: здесь само выберется CABLE Input, а в Телеграме микрофоном будет CABLE Output.");
      if (ImGui::Button("Открыть страницу VB-CABLE")) {
        ShellExecuteW(nullptr, L"open", L"https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL);
      }
    }
    ImGui::Text("Виртуальная камера: %s", registered ? "зарегистрирована" : "не зарегистрирована");
    if (ImGui::Button("Зарегистрировать Conference FX Camera")) registered = registerCamera() && cameraRegistered();
    ImGui::TextWrapped("В созвоне камеру выберите «Conference FX Camera». Галка «Слушать в наушниках» оставляет копию голоса у вас.");
    ImGui::EndChild();
    ImGui::End();

    ImGui::Render();
    const float clear[4] = {0.08f, 0.09f, 0.11f, 1.f};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(1, 0);
  }

  g_camera->stop();
  g_audio->stop();
  delete g_camera;
  delete g_audio;
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  if (g_previewSrv) g_previewSrv->Release();
  if (g_preview) g_preview->Release();
  if (g_rtv) g_rtv->Release();
  if (g_swap) g_swap->Release();
  if (g_context) g_context->Release();
  if (g_device) g_device->Release();
  g_shared.close();
  MFShutdown();
  if (com == S_OK) CoUninitialize();
  return 0;
}
