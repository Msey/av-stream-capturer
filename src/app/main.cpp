#include "capture/audio_engine.hpp"
#include "capture/camera.hpp"
#include "capture/devices.hpp"
#include "captions/caption_engine.hpp"
#include "captions/speech_player.hpp"
#include "common/params.hpp"
#include "common/shared_frame.hpp"
#include "vcam/camera_name.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
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
cfx::CaptionEngine* g_captions = nullptr;
cfx::SpeechPlayer* g_speech = nullptr;
ImFont* g_captionFont = nullptr;
std::vector<cfx::AudioApp> g_audioApps;
std::uint32_t g_captionPid = 0;

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
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
  out.resize(static_cast<size_t>(size - 1));
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

bool blankText(const std::string& text) {
  for (unsigned char ch : text) {
    if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') return false;
  }
  return true;
}

int snapUtf8(const std::string& text, int index, bool toEnd) {
  if (index <= 0) return 0;
  const int length = static_cast<int>(std::min(text.size(), static_cast<size_t>(0x7fffffff)));
  if (index >= length) return length;
  auto continuation = [](unsigned char value) { return (value & 0xC0) == 0x80; };
  if (!continuation(static_cast<unsigned char>(text[static_cast<size_t>(index)]))) return index;
  if (toEnd) {
    while (index < length && continuation(static_cast<unsigned char>(text[static_cast<size_t>(index)]))) ++index;
  } else {
    while (index > 0 && continuation(static_cast<unsigned char>(text[static_cast<size_t>(index)]))) --index;
  }
  return index;
}

void drawTranscript() {
  ImGui::BeginChild("transcript", ImVec2(0, 0), false);
  bool captionsOn = g_params.captions.load() != 0;
  if (ImGui::Checkbox("Субтитры собеседника", &captionsOn)) {
    g_params.captions.store(captionsOn ? 1 : 0);
    if (captionsOn) {
      g_captions->setRefine(g_params.captionsRefine.load() != 0);
      g_captions->setSource(g_captionPid, g_captionPid == 0);
      g_captions->start();
    } else {
      g_captions->stop();
    }
  }
  bool refine = g_params.captionsRefine.load() != 0;
  ImGui::SameLine();
  if (ImGui::Checkbox("Пунктуация", &refine)) {
    g_params.captionsRefine.store(refine ? 1 : 0);
    if (g_captions) g_captions->setRefine(refine);
  }

  std::vector<std::string> appNames;
  appNames.push_back("Всё, кроме Conference FX");
  int appIndex = 0;
  for (int i = 0; i < static_cast<int>(g_audioApps.size()); ++i) {
    appNames.push_back(utf8(g_audioApps[static_cast<size_t>(i)].name));
    if (g_captionPid != 0 && g_audioApps[static_cast<size_t>(i)].pid == g_captionPid) appIndex = i + 1;
  }
  if (g_captionPid != 0 && appIndex == 0) {
    g_captionPid = 0;
    if (captionsOn) g_captions->setSource(0, true);
  }
  if (ImGui::Combo("Чей звук", &appIndex, [](void* data, int idx, const char** out) {
        auto* list = static_cast<std::vector<std::string>*>(data);
        if (idx < 0 || idx >= static_cast<int>(list->size())) return false;
        *out = (*list)[static_cast<size_t>(idx)].c_str();
        return true;
      }, &appNames, static_cast<int>(appNames.size())) && g_captions) {
    if (appIndex <= 0) {
      g_captionPid = 0;
      g_captions->setSource(0, true);
    } else {
      g_captionPid = g_audioApps[static_cast<size_t>(appIndex - 1)].pid;
      g_captions->setSource(g_captionPid, false);
    }
  }

  const cfx::CaptionView view = g_captions->copyView();
  ImGui::TextWrapped("%s", view.status.c_str());
  ImGui::TextWrapped("%s", g_speech->status().c_str());
  bool readToCall = g_params.readToCall.load() != 0;
  const char* routeLabel = g_speech->ownVoice() ? "Читать свой голос в микрофон созвона" : "Читать ruslan в микрофон созвона";
  if (ImGui::Checkbox(routeLabel, &readToCall)) {
    g_params.readToCall.store(readToCall ? 1 : 0);
    g_speech->setRouteToCall(readToCall);
  }

  std::string built;
  for (const cfx::CaptionLine& line : view.lines) {
    if (!built.empty()) built.push_back('\n');
    built += line.text;
  }
  if (!view.draft.empty()) {
    if (!built.empty()) built.push_back('\n');
    built += view.draft;
  }

  static std::string shown;
  static std::vector<char> buffer(1, '\0');
  static std::string selected;
  static bool follow = true;
  const bool selecting = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  const bool textChanged = built != shown && !selecting;
  if (textChanged) {
    shown = built;
    buffer.assign(shown.begin(), shown.end());
    buffer.push_back('\0');
  }
  if (buffer.empty()) buffer.push_back('\0');

  if (ImGui::Button("Копировать всё")) ImGui::SetClipboardText(shown.c_str());
  ImGui::SameLine();
  if (ImGui::Button("Очистить")) {
    g_captions->clear();
    selected.clear();
  }
  ImGui::SameLine();
  if (ImGui::Button("К новым строкам")) follow = true;
  ImGui::SameLine();
  if (ImGui::Button("Стоп")) g_speech->stop();

  const float boxH = std::max(160.f, ImGui::GetContentRegionAvail().y);
  ImGui::BeginChild("caption-scroll", ImVec2(0, boxH), true);
  if (g_captionFont) ImGui::PushFont(g_captionFont);
  ImGui::PushID("caption-box");
  const ImGuiID textId = ImGui::GetID("##text");
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 boxMin = ImGui::GetCursorScreenPos();
    const ImVec2 boxMax(boxMin.x + ImGui::GetContentRegionAvail().x, boxMin.y + ImGui::GetContentRegionAvail().y);
    if (mouse.x >= boxMin.x && mouse.x <= boxMax.x && mouse.y >= boxMin.y && mouse.y <= boxMax.y) {
      selected.clear();
      if (ImGuiInputTextState* state = ImGui::GetInputTextState(textId)) {
        if (state->HasSelection()) {
          int from = state->GetSelectionStart();
          int to = state->GetSelectionEnd();
          if (from > to) std::swap(from, to);
          const int length = static_cast<int>(std::min(shown.size(), static_cast<size_t>(0x7fffffff)));
          from = snapUtf8(shown, std::clamp(from, 0, length), false);
          to = snapUtf8(shown, std::clamp(to, 0, length), true);
          if (to > from) selected = shown.substr(static_cast<size_t>(from), static_cast<size_t>(to - from));
        }
      }
    }
  }
  if (textChanged) {
    if (ImGuiInputTextState* state = ImGui::GetInputTextState(textId)) state->ReloadUserBufAndKeepSelection();
  }
  int lineCount = 1;
  for (char ch : shown) {
    if (ch == '\n') ++lineCount;
  }
  const float inputH = std::max(boxH, lineCount * ImGui::GetTextLineHeightWithSpacing() + 8.f);
  ImGui::InputTextMultiline("##text", buffer.data(), static_cast<int>(buffer.size()), ImVec2(-1, inputH),
                            ImGuiInputTextFlags_ReadOnly);
  if (ImGui::BeginPopupContextItem("caption-menu")) {
    const bool canRead = !blankText(selected);
    if (ImGui::MenuItem(g_speech->menuLabel().c_str(), nullptr, false, canRead)) g_speech->speak(selected);
    if (ImGui::MenuItem("Стоп", nullptr, false, g_speech->speaking())) g_speech->stop();
    if (!canRead) ImGui::TextDisabled("Сначала выделите текст");
    ImGui::EndPopup();
  }
  ImGui::PopID();
  if (g_captionFont) ImGui::PopFont();
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
      (ImGui::GetIO().MouseWheel != 0.f || ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
    follow = false;
  }
  if (follow) ImGui::SetScrollHereY(1.f);
  ImGui::EndChild();
  if (g_speech->speaking() && ImGui::IsKeyPressed(ImGuiKey_Escape)) g_speech->stop();
  ImGui::EndChild();
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
                              CW_USEDEFAULT, 1360, 900, nullptr, nullptr, instance, nullptr);
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
  g_captionFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 26.f * dpi, &fontConfig,
                                               io.Fonts->GetGlyphRangesCyrillic());
  ImGui::StyleColorsDark();
  ImGui::GetStyle().ScaleAllSizes(dpi);
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_device, g_context);

  auto microphones = cfx::enumerateAudio(true);
  auto outputs = cfx::enumerateAudio(false);
  auto cameras = cfx::enumerateCameras();
  g_audioApps = cfx::enumerateAudioApps();
  g_micId = cfx::defaultAudioId(true);
  const std::wstring cablePlayback = cfx::virtualCablePlaybackId(outputs);
  g_outId = !cablePlayback.empty() ? cablePlayback : cfx::defaultAudioId(false);
  if (!cameras.empty()) g_camLink = cameras.front().link;

  g_audio = new cfx::AudioEngine(g_params);
  g_camera = new cfx::CameraCapture(g_params, g_shared);
  g_captions = new cfx::CaptionEngine(g_params);
  g_speech = new cfx::SpeechPlayer();
  g_audio->start(g_micId, g_outId);
  g_camera->start(g_camLink);

  bool registered = cameraRegistered();
  int selectedPreset = static_cast<int>(cfx::Preset::Cartoon);
  auto lastEnum = GetTickCount64();

  MSG message{};
  while (message.message != WM_QUIT) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) break;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (message.message == WM_QUIT) break;

    if (GetTickCount64() - lastEnum > 2000) {
      microphones = cfx::enumerateAudio(true);
      outputs = cfx::enumerateAudio(false);
      cameras = cfx::enumerateCameras();
      g_audioApps = cfx::enumerateAudioApps();
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

    const float columns = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("stage", ImVec2(columns * 0.46f, 0), false);
    cfx::PreviewFrame preview;
    if (g_camera->copyPreview(preview)) uploadPreview(preview);
    if (g_previewSrv) {
      const float avail = ImGui::GetContentRegionAvail().x;
      float height = avail * (preview.height > 0 ? static_cast<float>(preview.height) / preview.width : 0.5625f);
      const float maxH = ImGui::GetContentRegionAvail().y * 0.42f;
      if (height > maxH) height = maxH;
      ImGui::Image(reinterpret_cast<ImTextureID>(g_previewSrv), ImVec2(avail, height), ImVec2(0, 0), ImVec2(1, 1),
                   ImVec4(1, 1, 1, 1), ImVec4(0.35f, 0.35f, 0.35f, 1));
    } else {
      ImGui::TextUnformatted("Нет кадра с камеры");
    }
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
    ImGui::Text("Субтитры: %.0f мс на кусок, пунктуация %.0f мс", g_params.captionChunkMs.load(),
                g_params.captionRefineMs.load());
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
    ImGui::EndChild();
    ImGui::SameLine();
    drawTranscript();
    ImGui::End();

    ImGui::Render();
    const float clear[4] = {0.08f, 0.09f, 0.11f, 1.f};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(1, 0);
  }

  if (g_speech) g_speech->stop();
  if (g_captions) g_captions->stop();
  delete g_speech;
  delete g_captions;
  g_speech = nullptr;
  g_captions = nullptr;
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
