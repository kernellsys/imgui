#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dcomp.h>
#include <wincodec.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <thread>

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "thirdparty/fontawesome/fontawesome.h"
#include "thirdparty/fontawesome/fonts.h"

#include "thirdparty/icon/icon.h"

static ID3D11Device*            g_Device = nullptr;
static ID3D11DeviceContext*     g_Context = nullptr;
static IDXGISwapChain1*         g_SwapChain = nullptr;
static ID3D11RenderTargetView*  g_RenderTargetView = nullptr;
static IDCompositionDevice*     g_DcompDevice = nullptr;
static IDCompositionTarget*     g_DcompTarget = nullptr;
static IDCompositionVisual*     g_DcompVisual = nullptr;
static ID3D11ShaderResourceView* g_HeaderIconSRV = nullptr;
static float g_HeaderIconAspect = 1.0f;
static HWND                     g_Hwnd = nullptr;
static bool                     g_WindowHidden = false;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0;
static UINT                     g_ResizeHeight = 0;

static const wchar_t* g_FailedStep = L"nenhuma";
static HRESULT g_FailedResult = S_OK;
static constexpr ImU32 kDarkPurpleLight = IM_COL32(126, 87, 194, 255);
static constexpr ImU32 kDarkPurple = IM_COL32(74, 20, 140, 255);
static constexpr ImU32 kDarkPurpleDeep = IM_COL32(38, 8, 74, 255);
static constexpr ImU32 kBlack = IM_COL32(0, 0, 0, 255);

static void FailGraphics(const wchar_t* etapa, HRESULT resultado) { g_FailedStep = etapa; g_FailedResult = resultado; }

static void ShowError(const wchar_t* titulo, const wchar_t* texto) { ::MessageBoxW(nullptr, texto, titulo, MB_OK | MB_ICONERROR); }

static void ShowGraphicsError() {
    wchar_t buffer[512] = {};
    ::wsprintfW(buffer,
        L"Falha ao iniciar o DirectX 11 / DirectComposition.\n\n"
        L"Etapa: %s\n"
        L"HRESULT: 0x%08X\n\n"
        L"Envie exatamente esta tela para diagnostico.",
        g_FailedStep, (unsigned int)g_FailedResult);
    ShowError(L"Hijacking External - erro grafico", buffer);
}

static bool    ApplyStreamProofAffinity(HWND hwnd);
static bool    CreateDeviceD3D(HWND hwnd);
static void    CleanupDeviceD3D();
static void    CreateRenderTarget();
static void    CleanupRenderTarget();
static void    SetupStyle();
static bool    LoadFonts();
static bool    LoadHeaderIconTexture();
static void    CleanupHeaderIconTexture();
static void    DrawHeaderIcon(ImDrawList* drawList, float windowWidth, float offsetX, float alpha);
static bool    DrawCloseButton(float windowWidth);
static float   EaseOutCubic(float value);
static float   EaseInOutCubic(float value);
static void    StartScreenTransition(float target);
static float AnimTowards(float current, float target, float dt, float duration);

static void    ShowNotify(const char* title, const char* content);
static void    StopLoadThread();
static bool    ShowMainUI();
static void    PollMenuKeyToggle();
static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC | CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ImGuiDX11Window";
    if (::RegisterClassExW(&wc) == 0) {
        ShowError(L"Hijacking External", L"Falha ao registrar a classe da janela.");
        return 1;
    }

    constexpr int kWindowWidth = 650;
    constexpr int kWindowHeight = 400;
    const int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

    g_Hwnd = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"ImGui DX11", WS_POPUP | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        (screenWidth - kWindowWidth) / 2, (screenHeight - kWindowHeight) / 2,
        kWindowWidth, kWindowHeight, nullptr, nullptr, hInstance, nullptr);

    if (g_Hwnd == nullptr) {
        ShowError(L"Hijacking External", L"Falha ao criar a janela (CreateWindowW).");
        ::UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    ApplyStreamProofAffinity(g_Hwnd);
    if (!CreateDeviceD3D(g_Hwnd)) {
        ShowGraphicsError();
        CleanupDeviceD3D();
        ::DestroyWindow(g_Hwnd);
        ::UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    if (!LoadFonts()) {
        ShowError(L"Hijacking External", L"Falha ao carregar as fontes do ImGui.");
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        ::DestroyWindow(g_Hwnd);
        ::UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    SetupStyle();

    ImGui_ImplWin32_Init(g_Hwnd);
    ImGui_ImplDX11_Init(g_Device, g_Context);
    LoadHeaderIconTexture();

    bool running = true;
    while (running) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (!running)
            break;

        if (g_SwapChainOccluded && g_SwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_SwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        PollMenuKeyToggle();

        if (g_WindowHidden)
            continue;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (!ShowMainUI())
            running = false;
        ImGui::Render();

        const float clearTransparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_Context->OMSetRenderTargets(1, &g_RenderTargetView, nullptr);
        g_Context->ClearRenderTargetView(g_RenderTargetView, clearTransparent);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT hr = g_SwapChain->Present(0, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    StopLoadThread();
    CleanupHeaderIconTexture();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(g_Hwnd);
    ::UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}

static void TruncateTo(ImFont* font, float fontSize, char* text, float maxWidth) {
    if (text == nullptr || maxWidth <= 0.0f) { if (text) text[0] = 0; return; }
    const int len0 = (int)strlen(text);
    if (len0 <= 0) return;
    if (font->CalcTextSizeA(fontSize, 1e9f, 0.0f, text).x <= maxWidth) return;
    const float dots = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, "...").x;
    if (dots >= maxWidth) { text[0] = 0; return; }
    int len = len0;
    while (len > 0) {
        text[len] = 0;
        if (font->CalcTextSizeA(fontSize, 1e9f, 0.0f, text).x + dots <= maxWidth)
            break;
        len--;
    }
    if (len + 4 <= 256) {
        memcpy(text + len, "...", 3);
        text[len + 3] = 0;
    } else if (len < 64) {
        text[len] = 0;
    }
}
static bool g_IsLoaded = false;
static float g_ScreenAnim = 1.0f;
static float g_ScreenFrom = 0.0f;
static float g_ScreenTarget = 0.0f;
static int g_ActiveTab = 0;
static float g_TabWeights[3] = { 1.0f, 0.0f, 0.0f };
static int g_MenuKey = VK_INSERT;
static bool g_MenuKeyBinding = false;
static bool g_MenuKeyArmed = false;
static float g_MenuKeyHover = 0.0f;
static float g_MenuKeyExpand = 0.0f;
static bool g_OptionChecked = false;
static bool g_ShowEspOutOfGame = false;
static float g_ShowEspOutAnim = 0.0f;
static float g_ShowEspOutHover = 0.0f;
static bool g_MenuKeyWasDown = false;
static float g_OptionCheckAnim = 0.0f;
static float g_OptionCheckHover = 0.0f;
static bool g_EspDropdownOpen = false;
static int g_EspDropTarget = 0;
static float g_EspDropAnim = 0.0f;
static bool g_EspMaster = false;
static float g_EspMasterAnim = 0.0f;
static float g_EspMasterHover = 0.0f;
static bool g_EspBox = false;
static float g_EspBoxAnim = 0.0f;
static float g_EspBoxHover = 0.0f;
static int g_EspBoxStyle = 2;
static bool g_EspBoxFilled = false;
static float g_EspBoxFilledAnim = 0.0f;
static float g_EspBoxFilledHover = 0.0f;
static bool g_EspLines = false;
static float g_EspLinesAnim = 0.0f;
static float g_EspLinesHover = 0.0f;
static int g_EspLinesPos = 0;
static bool g_EspHealth = false;
static float g_EspHealthAnim = 0.0f;
static float g_EspHealthHover = 0.0f;
static bool g_EspArmor = false;
static float g_EspArmorAnim = 0.0f;
static float g_EspArmorHover = 0.0f;
static bool g_EspNames = false;
static float g_EspNamesAnim = 0.0f;
static float g_EspNamesHover = 0.0f;
static bool g_EspDistance = false;
static float g_EspDistanceAnim = 0.0f;
static float g_EspDistanceHover = 0.0f;
static bool g_EspSkeleton = false;
static float g_EspSkeletonAnim = 0.0f;
static float g_EspSkeletonHover = 0.0f;
static float g_EspReveal = 0.0f;
static float g_EspBoxReveal = 0.0f;
static float g_EspLinesReveal = 0.0f;
static bool g_EspSelfSynced = false;
static bool g_EspPlayersMaster = false;
static float g_EspPlayersMasterAnim = 0.0f;
static float g_EspPlayersMasterHover = 0.0f;
static float g_EspPlayersReveal = 0.0f;
static float g_EspBoxRevealP = 0.0f;
static float g_EspLinesRevealP = 0.0f;
static bool g_EspBoxP = false;
static float g_EspBoxPAnim = 0.0f;
static float g_EspBoxPHover = 0.0f;
static bool g_EspHealthP = false;
static float g_EspHealthPAnim = 0.0f;
static float g_EspHealthPHover = 0.0f;
static bool g_EspArmorP = false;
static float g_EspArmorPAnim = 0.0f;
static float g_EspArmorPHover = 0.0f;
static bool g_EspNamesP = false;
static float g_EspNamesPAnim = 0.0f;
static float g_EspNamesPHover = 0.0f;
static bool g_EspDistanceP = false;
static float g_EspDistancePAnim = 0.0f;
static float g_EspDistancePHover = 0.0f;
static bool g_EspSkeletonP = false;
static float g_EspSkeletonPAnim = 0.0f;
static float g_EspSkeletonPHover = 0.0f;
static bool g_EspLinesP = false;
static float g_EspLinesPAnim = 0.0f;
static float g_EspLinesPHover = 0.0f;
static int g_EspLinesPosP = 0;
static bool g_EspBoxFilledP = false;
static float g_EspBoxFilledPAnim = 0.0f;
static float g_EspBoxFilledPHover = 0.0f;
static int g_EspBoxStyleP = 2;
static int g_EspDropTargetP = -1;
struct EspColorState { float color[4]; float boxAnim; float openAnim; float hoverAnim; };
static EspColorState g_EspColorsLocal[8] = { {{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{0.29f, 0.92f, 0.42f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{0.35f, 0.55f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{0.85f, 0.85f, 0.85f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f} };
static EspColorState g_EspColorsPlayers[8] = { {{1.0f, 0.25f, 0.25f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{1.0f, 0.25f, 0.25f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{1.0f, 0.25f, 0.25f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{0.29f, 0.92f, 0.42f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{0.35f, 0.55f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{0.85f, 0.85f, 0.85f, 1.0f}, 0.0f, 0.0f, 0.0f}, {{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f} };
static int g_ColorPickerOpen = -1;
static int g_ColorPickerCard = 0;
static float g_ColorPickerAnim = 0.0f;
static float g_EspScroll[2] = { 0.0f, 0.0f };
static float g_EspScrollTarget[2] = { 0.0f, 0.0f };
enum EspColorIndex { EspColor_Box = 0, EspColor_BoxFilled, EspColor_Lines, EspColor_Health, EspColor_Armor, EspColor_Names, EspColor_Distance, EspColor_Skeleton };

static void StartScreenTransition(float target) {
    float current = g_ScreenTarget;
    if (g_ScreenAnim < 1.0f)
        current = g_ScreenFrom + (g_ScreenTarget - g_ScreenFrom) * EaseInOutCubic(g_ScreenAnim);
    g_ScreenFrom = current;
    g_ScreenTarget = target;
    g_ScreenAnim = 0.0f;
}

static std::atomic_bool g_LoadBusy(false);
static std::atomic_bool g_LoadThreadRunning(false);
static std::thread g_LoadThread;

static DWORD FindProcessIdByName(const wchar_t* processName) {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    DWORD found = 0;
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName) == 0) {
                found = entry.th32ProcessID;
                break;
            }
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return found;
}

static bool StartMtaProcess() {
    const wchar_t* mtaPath = L"C:\\Program Files (x86)\\MTA San Andreas 1.6\\Multi Theft Auto.exe";
    if (::GetFileAttributesW(mtaPath) == INVALID_FILE_ATTRIBUTES)
        return false;

    SHELLEXECUTEINFOW execute = {};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.lpFile = mtaPath;
    execute.lpDirectory = L"C:\\Program Files (x86)\\MTA San Andreas 1.6";
    execute.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&execute))
        return false;

    if (execute.hProcess != nullptr)
        ::CloseHandle(execute.hProcess);
    return true;
}

static void LoadWorker() {
    if (FindProcessIdByName(L"gta_sa.exe") == 0) {
        if (!StartMtaProcess()) {
            g_LoadBusy = false;
            g_LoadThreadRunning = false;
            return;
        }
    }

    while (FindProcessIdByName(L"gta_sa.exe") == 0) {
        if (!g_LoadBusy.load()) {
            g_LoadThreadRunning = false;
            return;
        }
        ::Sleep(250);
    }

    g_IsLoaded = true;
    g_LoadBusy = false;
    g_LoadThreadRunning = false;
    StartScreenTransition(1.0f);
    ShowNotify("Loaded", "Menu Successfully Loaded!");
}

static void StartLoad() {
    if (g_LoadBusy.load() || g_IsLoaded)
        return;

    if (g_LoadThread.joinable())
        g_LoadThread.join();

    g_LoadBusy = true;
    g_LoadThreadRunning = true;
    try {
        g_LoadThread = std::thread(LoadWorker);
    } catch (...) {
        g_LoadBusy = false;
        g_LoadThreadRunning = false;
    }
}

static void StopLoadThread() {
    g_MenuKeyBinding = false;
    g_MenuKeyArmed = false;
    g_MenuKeyExpand = 0.0f;
    g_LoadBusy = false;
    if (g_LoadThread.joinable())
        g_LoadThread.join();
    g_LoadThreadRunning = false;
}

struct NotifyState {
    const char* title = nullptr;
    const char* content = nullptr;
    double startedAt = 0.0;
    bool active = false;
};

static NotifyState g_Notify;

static void ShowNotify(const char* title, const char* content) { g_Notify.title = title; g_Notify.content = content; g_Notify.startedAt = ImGui::GetTime(); g_Notify.active = true; }

static float g_ButtonHover = 0.0f;

static float SmoothStep01(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    return value * value * (3.0f - 2.0f * value);
}

static float AnimStep(float dt, float duration) {
    if (duration <= 0.0f)
        return 1.0f;
    float step = dt / duration;
    if (step < 0.0f) step = 0.0f;
    if (step > 1.0f) step = 1.0f;
    return step;
}

static float AnimTowards(float current, float target, float dt, float duration) {
    const float step = AnimStep(dt, duration);
    if (current < target) {
        const float next = current + step;
        return next < target ? next : target;
    }
    if (current > target) {
        const float next = current - step;
        return next > target ? next : target;
    }
    return target;
}

static float LerpF(float a, float b, float t) { return a + (b - a) * t; }

static ImU32 LerpCol(ImU32 from, ImU32 to, float t, int alpha) {
    const float r = LerpF((float)((from >> IM_COL32_R_SHIFT) & 0xFF), (float)((to >> IM_COL32_R_SHIFT) & 0xFF), t);
    const float g = LerpF((float)((from >> IM_COL32_G_SHIFT) & 0xFF), (float)((to >> IM_COL32_G_SHIFT) & 0xFF), t);
    const float b = LerpF((float)((from >> IM_COL32_B_SHIFT) & 0xFF), (float)((to >> IM_COL32_B_SHIFT) & 0xFF), t);
    return IM_COL32((int)r, (int)g, (int)b, alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha));
}

static bool DrawToggleButton(float width, float height, bool enabled, float offsetX, float alpha) {
    constexpr float buttonWidth = 210.0f;
    constexpr float buttonHeight = 42.0f;
    alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    const float x = (width - buttonWidth) * 0.5f + offsetX;
    const float y = (height - buttonHeight) * 0.5f + 58.0f;
    ImGui::SetCursorPos(ImVec2(x, y));
    if (!enabled)
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool pressed = ImGui::Button("##LoadButton", ImVec2(buttonWidth, buttonHeight));
    const bool clicked = enabled && pressed;
    ImGui::PopStyleColor(3);
    if (!enabled)
        ImGui::PopItemFlag();
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool active = enabled && ImGui::IsItemActive();
    const float target = active ? 0.65f : (hovered ? 1.0f : 0.0f);
    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.05f) dt = 0.05f;
    g_ButtonHover = AnimTowards(g_ButtonHover, target, dt, 0.07f);
    const float h = SmoothStep01(g_ButtonHover);
    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const ImVec2 rectMax = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    drawList->AddRectFilled(rectMin, rectMax, IM_COL32(3, 1, 8, static_cast<int>(255.0f * alpha)), rounding);
    drawList->AddRectFilled(rectMin, rectMax, IM_COL32(40, 10, 76, static_cast<int>(92.0f * h * alpha)), rounding);
    drawList->AddRect(rectMin, rectMax, IM_COL32(92, 38, 158, static_cast<int>((155.0f + 100.0f * h) * alpha)), rounding, 0, 1.0f + h * 0.6f);
    if (h > 0.01f) {
        drawList->AddRect(ImVec2(rectMin.x - 2.0f, rectMin.y - 2.0f), ImVec2(rectMax.x + 2.0f, rectMax.y + 2.0f), IM_COL32(74, 20, 140, static_cast<int>(38.0f * h * alpha)), rounding + 2.0f, 0, 3.0f);
        drawList->AddRectFilled(ImVec2(rectMin.x + 8.0f, rectMin.y + 1.0f), ImVec2(rectMax.x - 8.0f, rectMin.y + 3.0f), IM_COL32(150, 105, 220, static_cast<int>(36.0f * h * alpha)), rounding);
    }
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const bool busy = g_LoadBusy.load();
    const char* label = busy ? "Waiting MTA Startup..." : "Load";
    const ImVec2 labelSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, label);
    const float spinnerDiameter = busy ? 16.0f : 0.0f;
    const float spinnerGap = busy ? 8.0f : 0.0f;
    const float contentWidth = spinnerDiameter + spinnerGap + labelSize.x;
    const float contentX = rectMin.x + (buttonWidth - contentWidth) * 0.5f;
    if (busy) {
        const ImVec2 center(contentX + spinnerDiameter * 0.5f, rectMin.y + buttonHeight * 0.5f);
        const float angle = static_cast<float>(fmod(ImGui::GetTime() * 4.2, 6.28318530718));
        drawList->AddCircle(center, spinnerDiameter * 0.5f, IM_COL32(74, 20, 140, static_cast<int>(82.0f * alpha)), 24, 2.0f);
        drawList->PathClear();
        drawList->PathArcTo(center, spinnerDiameter * 0.5f, angle, angle + 4.35f, 24);
        drawList->PathStroke(IM_COL32(158, 112, 226, static_cast<int>(255.0f * alpha)), 0, 2.0f);
    }
    drawList->AddText(font, fontSize, ImVec2(contentX + spinnerDiameter + spinnerGap, rectMin.y + (buttonHeight - labelSize.y) * 0.5f), enabled ? IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha)) : IM_COL32(230, 222, 244, static_cast<int>(235.0f * alpha)), label);
    return clicked;
}

static float g_CloseHover = 0.0f;

static bool DrawCloseButton(float windowWidth) {
    constexpr float buttonSize = 34.0f;
    constexpr float rightMargin = 16.0f;
    constexpr float topMargin = 14.0f;
    const ImVec2 buttonMin(windowWidth - rightMargin - buttonSize, topMargin);
    const ImVec2 buttonMax(buttonMin.x + buttonSize, buttonMin.y + buttonSize);
    ImGui::SetCursorScreenPos(buttonMin);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool clicked = ImGui::InvisibleButton("##CloseButton", ImVec2(buttonSize, buttonSize));
    ImGui::PopStyleColor(3);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const float target = active ? 0.70f : (hovered ? 1.0f : 0.0f);
    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.05f) dt = 0.05f;
    g_CloseHover = AnimTowards(g_CloseHover, target, dt, 0.06f);
    const float h = SmoothStep01(g_CloseHover);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 iconSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, ICON_FA_TIMES);
    const ImVec2 iconPos((buttonMin.x + buttonMax.x - iconSize.x) * 0.5f, (buttonMin.y + buttonMax.y - iconSize.y) * 0.5f);
    if (h > 0.01f) {
        const int glowAlpha = static_cast<int>(58.0f * h);
        const ImU32 glowColor = IM_COL32(126, 87, 194, glowAlpha);
        drawList->AddText(font, fontSize, ImVec2(iconPos.x - 1.0f, iconPos.y), glowColor, ICON_FA_TIMES);
        drawList->AddText(font, fontSize, ImVec2(iconPos.x + 1.0f, iconPos.y), glowColor, ICON_FA_TIMES);
        drawList->AddText(font, fontSize, ImVec2(iconPos.x, iconPos.y - 1.0f), glowColor, ICON_FA_TIMES);
        drawList->AddText(font, fontSize, ImVec2(iconPos.x, iconPos.y + 1.0f), glowColor, ICON_FA_TIMES);
        drawList->AddText(font, fontSize, ImVec2(iconPos.x - 1.0f, iconPos.y - 1.0f), glowColor, ICON_FA_TIMES);
        drawList->AddText(font, fontSize, ImVec2(iconPos.x + 1.0f, iconPos.y - 1.0f), glowColor, ICON_FA_TIMES);
        drawList->AddText(font, fontSize, ImVec2(iconPos.x - 1.0f, iconPos.y + 1.0f), glowColor, ICON_FA_TIMES);
        drawList->AddText(font, fontSize, ImVec2(iconPos.x + 1.0f, iconPos.y + 1.0f), glowColor, ICON_FA_TIMES);
    }
    const int iconAlpha = static_cast<int>(190.0f + 65.0f * h);
    const ImU32 iconColor = IM_COL32(126 + static_cast<int>(54.0f * h), 87 + static_cast<int>(51.0f * h), 194 + static_cast<int>(34.0f * h), iconAlpha);
    drawList->AddText(font, fontSize, iconPos, iconColor, ICON_FA_TIMES);
    return clicked;
}

static const char* VirtualKeyName(int vk) {
    switch (vk) {
        case VK_INSERT: return "Insert"; case VK_DELETE: return "Delete"; case VK_HOME: return "Home"; case VK_END: return "End";
        case VK_PRIOR: return "Page Up"; case VK_NEXT: return "Page Down"; case VK_UP: return "Up"; case VK_DOWN: return "Down";
        case VK_LEFT: return "Left"; case VK_RIGHT: return "Right"; case VK_SPACE: return "Space"; case VK_TAB: return "Tab";
        case VK_CAPITAL: return "Caps Lock"; case VK_NUMLOCK: return "Num Lock"; case VK_SCROLL: return "Scroll Lock";
        case VK_SHIFT: return "Shift"; case VK_LSHIFT: return "LShift"; case VK_RSHIFT: return "RShift";
        case VK_CONTROL: return "Ctrl"; case VK_LCONTROL: return "LCtrl"; case VK_RCONTROL: return "RCtrl";
        case VK_MENU: return "Alt"; case VK_LMENU: return "LAlt"; case VK_RMENU: return "RAlt";
        case VK_LWIN: return "LWin"; case VK_RWIN: return "RWin"; case VK_APPS: return "Menu";
        case VK_BACK: return "Backspace"; case VK_RETURN: return "Enter"; case VK_PAUSE: return "Pause";
        case VK_SNAPSHOT: return "Print Screen"; case VK_ESCAPE: return "Esc";
        case VK_OEM_1: return ";"; case VK_OEM_PLUS: return "="; case VK_OEM_COMMA: return ",";
        case VK_OEM_MINUS: return "-"; case VK_OEM_PERIOD: return "."; case VK_OEM_2: return "/";
        case VK_OEM_3: return "`"; case VK_OEM_4: return "["; case VK_OEM_5: return "\\";
        case VK_OEM_6: return "]"; case VK_OEM_7: return "'"; case VK_OEM_102: return "<";
        case VK_ADD: return "Num +"; case VK_SUBTRACT: return "Num -"; case VK_MULTIPLY: return "Num *";
        case VK_DIVIDE: return "Num /"; case VK_DECIMAL: return "Num ."; case VK_SEPARATOR: return "Num Enter";
        default: break;
    }
    static char nameBuffer[16] = {};
    if (vk >= VK_F1 && vk <= VK_F24) { nameBuffer[0] = 'F'; if (vk - VK_F1 + 1 >= 10) { nameBuffer[1] = '1'; nameBuffer[2] = static_cast<char>('0' + (vk - VK_F1 + 1) % 10); nameBuffer[3] = 0; } else { nameBuffer[1] = static_cast<char>('0' + vk - VK_F1 + 1); nameBuffer[2] = 0; } return nameBuffer; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { nameBuffer[0] = static_cast<char>(vk); nameBuffer[1] = 0; return nameBuffer; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { nameBuffer[0] = 'N'; nameBuffer[1] = 'u'; nameBuffer[2] = 'm'; nameBuffer[3] = ' '; nameBuffer[4] = static_cast<char>('0' + vk - VK_NUMPAD0); nameBuffer[5] = 0; return nameBuffer; }
    const UINT scanCode = ::MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (scanCode != 0) { wchar_t wideName[32] = {}; if (::GetKeyNameTextW(static_cast<LONG>(scanCode << 16), wideName, 32) > 0) { ::WideCharToMultiByte(CP_UTF8, 0, wideName, -1, nameBuffer, static_cast<int>(sizeof(nameBuffer)), nullptr, nullptr); return nameBuffer; } }
    nameBuffer[0] = 'K'; nameBuffer[1] = 'e'; nameBuffer[2] = 'y'; nameBuffer[3] = ' '; nameBuffer[4] = '0'; nameBuffer[5] = 'x'; const char* hex = "0123456789ABCDEF"; nameBuffer[6] = hex[(vk >> 4) & 0xF]; nameBuffer[7] = hex[vk & 0xF]; nameBuffer[8] = 0; return nameBuffer;
}

static void PollMenuKeyToggle() {
    if (g_MenuKeyBinding) {
        g_MenuKeyWasDown = (::GetAsyncKeyState(g_MenuKey) & 0x8000) != 0;
        return;
    }
    const bool down = (::GetAsyncKeyState(g_MenuKey) & 0x8000) != 0;
    if (down && !g_MenuKeyWasDown && g_Hwnd != nullptr) {
        g_WindowHidden = !g_WindowHidden;
        if (g_WindowHidden) {
            ::ShowWindowAsync(g_Hwnd, SW_HIDE);
        } else {
            ::SetWindowPos(g_Hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ::ShowWindowAsync(g_Hwnd, SW_SHOWNOACTIVATE);
        }
    }
    g_MenuKeyWasDown = down;
}

static void PollMenuKeyBinding() {
    if (!g_MenuKeyBinding)
        return;
    if (!g_MenuKeyArmed) {
        bool anyDown = false;
        for (int vk = 1; vk < 256; vk++) { if (::GetAsyncKeyState(vk) & 0x8000) { anyDown = true; break; } }
        if (!anyDown)
            g_MenuKeyArmed = true;
        return;
    }
    if (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) { g_MenuKeyBinding = false; g_MenuKeyArmed = false; return; }
    for (int vk = 1; vk < 256; vk++) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
            continue;
        if (::GetAsyncKeyState(vk) & 0x8000) {
            if (vk == VK_SHIFT) vk = (::GetAsyncKeyState(VK_RSHIFT) & 0x8000) ? VK_RSHIFT : VK_LSHIFT;
            else if (vk == VK_CONTROL) vk = (::GetAsyncKeyState(VK_RCONTROL) & 0x8000) ? VK_RCONTROL : VK_LCONTROL;
            else if (vk == VK_MENU) vk = (::GetAsyncKeyState(VK_RMENU) & 0x8000) ? VK_RMENU : VK_LMENU;
            g_MenuKey = vk;
            g_MenuKeyBinding = false;
            g_MenuKeyArmed = false;
            ShowNotify("Menu Key", "Key updated successfully!");
            return;
        }
    }
}

static void DrawLoadedPage(float width, float height, float offsetX, float alpha, bool interactive) {
    alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    float tabDt = ImGui::GetIO().DeltaTime;
    if (tabDt > 0.05f) tabDt = 0.05f;
    for (int i = 0; i < 3; i++)
        g_TabWeights[i] = AnimTowards(g_TabWeights[i], g_ActiveTab == i ? 1.0f : 0.0f, tabDt, 0.16f);
    PollMenuKeyBinding();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    constexpr float barHeight = 62.0f;
    constexpr float dividerY = 0.0f;
    const float barTop = height - barHeight;
    drawList->AddLine(ImVec2(offsetX, barTop + dividerY), ImVec2(offsetX + width, barTop + dividerY), IM_COL32(74, 20, 140, static_cast<int>(210.0f * alpha)), 1.0f);
    drawList->AddRectFilled(ImVec2(offsetX, barTop + 1.0f), ImVec2(offsetX + width, height), IM_COL32(7, 2, 14, static_cast<int>(235.0f * alpha)), 12.0f, ImDrawFlags_RoundCornersBottom);
    const ImU32 edgeFade = IM_COL32(0, 0, 0, static_cast<int>(255.0f * alpha));
    const ImU32 edgeClear = IM_COL32(0, 0, 0, 0);
    drawList->AddRectFilledMultiColor(ImVec2(offsetX, barTop + 1.0f), ImVec2(offsetX + 64.0f, height), edgeFade, edgeClear, edgeClear, edgeFade);
    drawList->AddRectFilledMultiColor(ImVec2(offsetX + width - 64.0f, barTop + 1.0f), ImVec2(offsetX + width, height), edgeClear, edgeFade, edgeFade, edgeClear);

    constexpr float buttonSize = 38.0f;
    const float centers[3] = { width * 0.5f - 82.0f, width * 0.5f, width * 0.5f + 82.0f };
    const char* icons[3] = { ICON_FA_CROSSHAIRS, ICON_FA_EYE, ICON_FA_COG };
    const char* ids[3] = { "##TabCrosshair", "##TabEye", "##TabSettings" };
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();

    if (g_TabWeights[2] > 0.01f) {
        const float cardAlpha = alpha * SmoothStep01(g_TabWeights[2]);
        const float cardShift = (1.0f - g_TabWeights[2]) * 12.0f;
        const ImVec2 cardMin(offsetX + 24.0f + cardShift, 24.0f);
        const ImVec2 cardMax(cardMin.x + 302.0f, cardMin.y + 212.0f);
        drawList->AddRectFilled(cardMin, cardMax, IM_COL32(8, 2, 14, static_cast<int>(242.0f * cardAlpha)), 8.0f);
        drawList->AddRectFilled(cardMin, ImVec2(cardMax.x, cardMin.y + 32.0f), IM_COL32(74, 20, 140, static_cast<int>(255.0f * cardAlpha)), 8.0f, ImDrawFlags_RoundCornersTop);
        drawList->AddRect(cardMin, cardMax, IM_COL32(126, 87, 194, static_cast<int>(190.0f * cardAlpha)), 8.0f, 0, 1.0f);
        const ImVec2 gearSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, ICON_FA_COG);
        const ImVec2 gearPos(cardMin.x + 14.0f, cardMin.y + (32.0f - gearSize.y) * 0.5f);
        drawList->AddText(font, fontSize, gearPos, IM_COL32(255, 255, 255, static_cast<int>(255.0f * cardAlpha)), ICON_FA_COG);
        const char* kOptionsLabel = "Options";
        const ImVec2 labelSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, kOptionsLabel);
        drawList->AddText(font, fontSize, ImVec2(gearPos.x + gearSize.x + 9.0f, cardMin.y + (32.0f - labelSize.y) * 0.5f), IM_COL32(255, 255, 255, static_cast<int>(255.0f * cardAlpha)), kOptionsLabel);
        const float optionsLabelX = cardMin.x + 14.0f;
        constexpr float optionsSwitchW = 20.0f;
        constexpr float optionsSwitchH = 20.0f;
        auto drawOptionsSwitch = [&](const char* id, const char* label, bool* value, float* anim, float* hover, float rowY, float rowH, void (*onToggle)()) {
            const bool disabled = !interactive || cardAlpha < 0.90f;
            const ImVec2 labelTextSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, label);
            if (disabled)
                drawList->AddText(font, fontSize, ImVec2(optionsLabelX, rowY + (rowH - labelTextSize.y) * 0.5f), IM_COL32(255, 255, 255, static_cast<int>(255.0f * cardAlpha)), label);
            drawList->AddLine(ImVec2(cardMin.x + 14.0f, rowY - 4.0f), ImVec2(cardMax.x - 14.0f, rowY - 4.0f), IM_COL32(126, 87, 194, static_cast<int>(52.0f * cardAlpha)), 1.0f);
            const ImVec2 swMin(cardMax.x - 14.0f - optionsSwitchW, rowY + (rowH - optionsSwitchH) * 0.5f);
            const ImVec2 swMax(swMin.x + optionsSwitchW, swMin.y + optionsSwitchH);
            ImGui::SetCursorScreenPos(swMin);
            if (disabled)
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(optionsSwitchW, optionsSwitchH));
            if (!disabled && pressed) {
                *value = !*value;
                if (onToggle) onToggle();
            }
            ImGui::PopStyleColor(3);
            if (disabled)
                ImGui::PopItemFlag();
            const bool hovered = !disabled && ImGui::IsItemHovered();
            const bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            float dt = ImGui::GetIO().DeltaTime;
            if (dt > 0.05f) dt = 0.05f;
            *anim = AnimTowards(*anim, *value ? 1.0f : 0.0f, dt, 0.10f);
            *hover = AnimTowards(*hover, hovered ? 1.0f : 0.0f, dt, 0.07f);
            const float on = SmoothStep01(*anim);
            const float hovK = SmoothStep01(*hover);
            if (!disabled) {
                const ImU32 textCol = IM_COL32(255, 255, 255, static_cast<int>((*value ? 255.0f : (125.0f + 45.0f * hovK)) * cardAlpha));
                drawList->AddText(font, fontSize, ImVec2(optionsLabelX, rowY + (rowH - labelTextSize.y) * 0.5f), textCol, label);
            }
            const ImU32 baseBorder = IM_COL32(118, 112, 118, 255);
            const ImU32 ungreyBorder = LerpCol(baseBorder, kDarkPurpleLight, hovK, static_cast<int>(245.0f * cardAlpha));
            const ImU32 borderCol = *value ? kDarkPurpleLight : ungreyBorder;
            const int borderA = static_cast<int>((*value ? 255.0f : (120.0f + 90.0f * hovK)) * cardAlpha);
            const float radius = 4.0f;
            drawList->AddRectFilled(swMin, swMax, IM_COL32(10, 10, 10, static_cast<int>(245.0f * cardAlpha)), radius);
            if (on > 0.01f) {
                const float shrink = (1.0f - on) * 3.2f;
                const ImVec2 fillMin(swMin.x + shrink, swMin.y + shrink);
                const ImVec2 fillMax(swMax.x - shrink, swMax.y - shrink);
                drawList->AddRectFilled(fillMin, fillMax, LerpCol(IM_COL32(29, 10, 58, 255), held ? IM_COL32(86, 34, 150, 255) : IM_COL32(116, 62, 190, 255), on, static_cast<int>((210.0f + 45.0f * on) * cardAlpha)), radius * on);
            }
            const float fade = on > 0.55f ? (on - 0.55f) / 0.45f : 0.0f;
            if (fade > 0.01f) {
                const float fs = optionsSwitchW;
                const float tn = 2.1f;
                const float cx = swMin.x + fs * 0.50f;
                const float cy = swMin.y + fs * 0.52f;
                const ImVec2 pa(cx - fs * 0.28f, cy + fs * 0.02f);
                const ImVec2 pb(cx - fs * 0.09f, cy + fs * 0.21f);
                const ImVec2 pc(cx + fs * 0.30f, cy - fs * 0.22f);
                const ImU32 checkCol = IM_COL32(16, 7, 26, static_cast<int>(255.0f * fade * cardAlpha));
                drawList->PathClear();
                drawList->PathLineTo(pa);
                drawList->PathLineTo(pb);
                drawList->PathLineTo(pc);
                drawList->PathStroke(checkCol, 0, tn);
            }
            const ImU32 finalBorder = held ? IM_COL32(168, 132, 235, static_cast<int>(255.0f * cardAlpha)) : borderCol;
            const int finalA = *value ? static_cast<int>(255.0f * cardAlpha) : borderA;
            drawList->AddRect(swMin, swMax, IM_COL32((finalBorder >> IM_COL32_R_SHIFT) & 0xFF, (finalBorder >> IM_COL32_G_SHIFT) & 0xFF, (finalBorder >> IM_COL32_B_SHIFT) & 0xFF, finalA), 4.0f, 0, 1.15f);
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
        };
        constexpr float optionRowH = 34.0f;
        constexpr float optionRowGap = 8.0f;
        float optionY = cardMin.y + 44.0f;
        const char* menuKeyLabel = "Menu Key";
        const ImVec2 menuKeySize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, menuKeyLabel);
        drawList->AddText(font, fontSize, ImVec2(optionsLabelX, optionY + (optionRowH - menuKeySize.y) * 0.5f), IM_COL32(255, 255, 255, static_cast<int>(255.0f * cardAlpha)), menuKeyLabel);
        float keyDt = ImGui::GetIO().DeltaTime;
        if (keyDt > 0.05f) keyDt = 0.05f;
        g_MenuKeyExpand = AnimTowards(g_MenuKeyExpand, g_MenuKeyBinding ? 1.0f : 0.0f, keyDt, 0.10f);
        const float expand = SmoothStep01(g_MenuKeyExpand);
        constexpr float collapsedKeyWidth = 98.0f;
        constexpr float expandedKeyWidth = 168.0f;
        const float keyButtonWidth = collapsedKeyWidth + (expandedKeyWidth - collapsedKeyWidth) * expand;
        constexpr float keyButtonHeight = 30.0f;
        const ImVec2 keyButtonMax(cardMax.x - 14.0f, optionY + optionRowH);
        const ImVec2 keyButtonMin(keyButtonMax.x - keyButtonWidth, optionY + (optionRowH - keyButtonHeight) * 0.5f);
        ImGui::SetCursorScreenPos(keyButtonMin);
        if (g_MenuKeyBinding || cardAlpha < 0.90f || !interactive)
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        const bool keyButtonPressed = ImGui::InvisibleButton("##MenuKeyButton", ImVec2(keyButtonWidth, keyButtonHeight));
        const bool keyButtonClicked = interactive && cardAlpha >= 0.90f && !g_MenuKeyBinding && keyButtonPressed;
        ImGui::PopStyleColor(3);
        if (g_MenuKeyBinding || cardAlpha < 0.90f || !interactive)
            ImGui::PopItemFlag();
        const bool keyButtonHovered = interactive && cardAlpha >= 0.90f && !g_MenuKeyBinding && ImGui::IsItemHovered();
        const float keyTarget = g_MenuKeyBinding ? 1.0f : (keyButtonHovered ? 1.0f : 0.0f);
        g_MenuKeyHover = AnimTowards(g_MenuKeyHover, keyTarget, keyDt, 0.07f);
        const float kh = SmoothStep01(g_MenuKeyHover);
        drawList->AddRectFilled(keyButtonMin, keyButtonMax, IM_COL32(18, 5, 32, static_cast<int>(242.0f * cardAlpha)), 7.0f);
        drawList->AddRectFilled(keyButtonMin, keyButtonMax, IM_COL32(74, 20, 140, static_cast<int>((14.0f + 26.0f * kh) * cardAlpha)), 7.0f);
        drawList->AddRect(keyButtonMin, keyButtonMax, IM_COL32(126 + static_cast<int>(54.0f * kh), 87 + static_cast<int>(40.0f * kh), 194 + static_cast<int>(34.0f * kh), static_cast<int>((185.0f + 70.0f * kh) * cardAlpha)), 7.0f, 0, 1.0f + kh * 0.5f);
        const ImVec2 keyboardSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, ICON_FA_KEYBOARD);
        const ImVec2 keyboardPos(keyButtonMin.x + 9.0f, keyButtonMin.y + (keyButtonHeight - keyboardSize.y) * 0.5f);
        drawList->AddText(font, fontSize, keyboardPos, IM_COL32(214, 196, 240, static_cast<int>(240.0f * cardAlpha)), ICON_FA_KEYBOARD);
        const char* keyText = g_MenuKeyBinding ? "Press a key..." : VirtualKeyName(g_MenuKey);
        const ImVec2 keyTextSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, keyText);
        drawList->AddText(font, fontSize, ImVec2(keyboardPos.x + keyboardSize.x + 7.0f, keyButtonMin.y + (keyButtonHeight - keyTextSize.y) * 0.5f), IM_COL32(255, 255, 255, static_cast<int>(255.0f * cardAlpha)), keyText);
        if (keyButtonClicked) { g_MenuKeyBinding = true; g_MenuKeyArmed = false; }
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        optionY += optionRowH + optionRowGap;
        drawOptionsSwitch("##StreamProofSwitch", "Stream Proof", &g_OptionChecked, &g_OptionCheckAnim, &g_OptionCheckHover, optionY, optionRowH, [](){ ::SetWindowDisplayAffinity(g_Hwnd, g_OptionChecked ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE); });
        optionY += optionRowH + optionRowGap;
        drawOptionsSwitch("##ShowEspOutSwitch", "Show Esp in Out of Game", &g_ShowEspOutOfGame, &g_ShowEspOutAnim, &g_ShowEspOutHover, optionY, optionRowH, nullptr);
        optionY += optionRowH + optionRowGap;
    }

    if (g_TabWeights[1] > 0.01f) {
        const float visAlpha = alpha * SmoothStep01(g_TabWeights[1]);
        const float visShift = (1.0f - g_TabWeights[1]) * 12.0f;
        const float contentTop = 18.0f;
        constexpr float espCardW = 292.0f;
        constexpr float espCardMargin = 20.0f;
        const float espCardH = barTop - 10.0f - contentTop;
        const ImVec2 localMin(offsetX + espCardMargin + visShift, contentTop);
        const ImVec2 localMax(localMin.x + espCardW, localMin.y + espCardH);
        const ImVec2 playersMin(offsetX + width - espCardMargin - espCardW + visShift, contentTop);
        const ImVec2 playersMax(playersMin.x + espCardW, playersMin.y + espCardH);
        float espDt = ImGui::GetIO().DeltaTime;
        if (espDt > 0.05f) espDt = 0.05f;

        g_EspDropAnim = AnimTowards(g_EspDropAnim, g_EspDropdownOpen ? 1.0f : 0.0f, espDt, 0.12f);
        g_EspReveal = AnimTowards(g_EspReveal, g_EspMaster ? 1.0f : 0.0f, espDt, 0.18f);
        g_EspPlayersReveal = AnimTowards(g_EspPlayersReveal, g_EspPlayersMaster ? 1.0f : 0.0f, espDt, 0.18f);
        g_EspBoxReveal = AnimTowards(g_EspBoxReveal, g_EspBox ? 1.0f : 0.0f, espDt, 0.14f);
        g_EspLinesReveal = AnimTowards(g_EspLinesReveal, g_EspLines ? 1.0f : 0.0f, espDt, 0.14f);
        g_EspBoxRevealP = AnimTowards(g_EspBoxRevealP, g_EspBoxP ? 1.0f : 0.0f, espDt, 0.14f);
        g_EspLinesRevealP = AnimTowards(g_EspLinesRevealP, g_EspLinesP ? 1.0f : 0.0f, espDt, 0.14f);
        g_ColorPickerAnim = AnimTowards(g_ColorPickerAnim, g_ColorPickerOpen >= 0 ? 1.0f : 0.0f, espDt, 0.12f);

        const float localAlpha = visAlpha * SmoothStep01(g_EspReveal);
        const float playersAlpha = visAlpha * SmoothStep01(g_EspPlayersReveal);

        static ImVec2 s_LastColorPopupMin(0.0f, 0.0f);
        static ImVec2 s_LastColorPopupMax(0.0f, 0.0f);
        static ImVec2 s_LastDropPopupMin(0.0f, 0.0f);
        static ImVec2 s_LastDropPopupMax(0.0f, 0.0f);
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        const bool mouseInColorPopup = (g_ColorPickerOpen >= 0) &&
            mousePos.x >= s_LastColorPopupMin.x && mousePos.x <= s_LastColorPopupMax.x &&
            mousePos.y >= s_LastColorPopupMin.y && mousePos.y <= s_LastColorPopupMax.y;
        const bool mouseInDropPopup = g_EspDropdownOpen &&
            mousePos.x >= s_LastDropPopupMin.x && mousePos.x <= s_LastDropPopupMax.x &&
            mousePos.y >= s_LastDropPopupMin.y && mousePos.y <= s_LastDropPopupMax.y;
        const bool popupBlockingRows = mouseInColorPopup || mouseInDropPopup;

        EspColorState* activeColorState = nullptr;
        ImVec2 activeColorAnchorMin(0.0f, 0.0f);
        ImVec2 activeColorAnchorMax(0.0f, 0.0f);
        float activeColorAlpha = 0.0f;
        bool activeColorEnabled = false;

        struct DeferredDropdown {
            bool active = false;
            int* value = nullptr;
            const char* const* items = nullptr;
            int count = 0;
            int target = 0;
            ImVec2 ddMin = ImVec2(0.0f, 0.0f);
            ImVec2 ddMax = ImVec2(0.0f, 0.0f);
            float alphaRow = 0.0f;
            bool disabled = false;
        } deferredDrop;

        auto drawCardFrame = [&](const ImVec2& cMin, const ImVec2& cMax, const char* icon, const char* title) {
            drawList->AddRectFilled(cMin, cMax, IM_COL32(8, 2, 14, static_cast<int>(242.0f * visAlpha)), 8.0f);
            drawList->AddRectFilled(cMin, ImVec2(cMax.x, cMin.y + 32.0f), IM_COL32(74, 20, 140, static_cast<int>(255.0f * visAlpha)), 8.0f, ImDrawFlags_RoundCornersTop);
            drawList->AddRect(cMin, cMax, IM_COL32(126, 87, 194, static_cast<int>(190.0f * visAlpha)), 8.0f, 0, 1.0f);
            const ImVec2 iconSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, icon);
            drawList->AddText(font, fontSize, ImVec2(cMin.x + 14.0f, cMin.y + (32.0f - iconSize.y) * 0.5f), IM_COL32(255, 255, 255, static_cast<int>(255.0f * visAlpha)), icon);
            drawList->AddText(font, fontSize, ImVec2(cMin.x + 14.0f + iconSize.x + 9.0f, cMin.y + (32.0f - font->CalcTextSizeA(fontSize, 1e9f, 0.0f, title).y) * 0.5f), IM_COL32(255, 255, 255, static_cast<int>(255.0f * visAlpha)), title);
        };
        auto drawHeaderButton = [&](const char* id, const ImVec2& cMin, const ImVec2& cMax, bool* master) {
            ImGui::SetCursorScreenPos(ImVec2(cMin.x + 1.0f, cMin.y));
            const bool dis = !interactive || visAlpha < 0.90f || popupBlockingRows;
            if (dis)
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            const bool pressed = ImGui::InvisibleButton(id, ImVec2((cMax.x - cMin.x) - 2.0f, 32.0f));
            if (!dis && pressed) { *master = !*master; g_EspDropdownOpen = false; g_ColorPickerOpen = -1; }
            ImGui::PopStyleColor(3);
            if (dis)
                ImGui::PopItemFlag();
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
        };
        auto clampColor = [](float* col, float alphaRow) {
            for (int i = 0; i < 3; i++)
                col[i] = col[i] < 0.0f ? 0.0f : (col[i] > 1.0f ? 1.0f : col[i]);
            col[3] = 1.0f;
            const ImVec4 rgba(col[0], col[1], col[2], alphaRow);
            return ImGui::ColorConvertFloat4ToU32(rgba);
        };
        auto drawColorBox = [&](const char* id, EspColorState& state, const ImVec2& boxMin, float alphaRow, bool enabledRow, bool rowVisible, int pickerIndex, int cardIndex) {
            constexpr float boxSize = 18.0f;
            const ImVec2 boxMax(boxMin.x + boxSize, boxMin.y + boxSize);
            const bool disabled = !interactive || visAlpha < 0.90f || !enabledRow || !rowVisible || popupBlockingRows;
            ImGui::SetCursorScreenPos(boxMin);
            if (disabled)
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(boxSize, boxSize));
            if (!disabled && pressed) {
                if (g_ColorPickerOpen == pickerIndex && g_ColorPickerCard == cardIndex)
                    g_ColorPickerOpen = -1;
                else { g_ColorPickerOpen = pickerIndex; g_ColorPickerCard = cardIndex; g_EspDropdownOpen = false; }
            }
            ImGui::PopStyleColor(3);
            if (disabled)
                ImGui::PopItemFlag();
            const bool hovered = !disabled && ImGui::IsItemHovered();
            state.hoverAnim = AnimTowards(state.hoverAnim, hovered ? 1.0f : 0.0f, espDt, 0.07f);
            const bool selected = g_ColorPickerOpen == pickerIndex && g_ColorPickerCard == cardIndex;
            state.openAnim = AnimTowards(state.openAnim, selected ? 1.0f : 0.0f, espDt, 0.10f);
            drawList->AddRectFilled(boxMin, boxMax, clampColor(state.color, alphaRow), 4.0f);
            const int borderA = static_cast<int>((175.0f + 60.0f * SmoothStep01(state.hoverAnim) + 40.0f * SmoothStep01(state.openAnim)) * alphaRow);
            drawList->AddRect(boxMin, boxMax, IM_COL32(126 + static_cast<int>(54.0f * SmoothStep01(state.hoverAnim)), 87 + static_cast<int>(40.0f * SmoothStep01(state.hoverAnim)), 194 + static_cast<int>(34.0f * SmoothStep01(state.hoverAnim)), borderA > 255 ? 255 : borderA), 4.0f, 0, 1.0f + SmoothStep01(state.openAnim) * 0.35f);
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
            if (selected) {
                activeColorState = &state;
                activeColorAnchorMin = boxMin;
                activeColorAnchorMax = boxMax;
                activeColorAlpha = alphaRow;
                activeColorEnabled = enabledRow && rowVisible;
            }
        };
        auto drawColorSquarePopup = [&](EspColorState& state, const ImVec2& anchorMin, const ImVec2& anchorMax, float alphaRow, bool enabledRow) {
            const float open = SmoothStep01(g_ColorPickerAnim);
            if (open <= 0.01f) {
                s_LastColorPopupMin = ImVec2(0.0f, 0.0f);
                s_LastColorPopupMax = ImVec2(0.0f, 0.0f);
                return;
            }
            constexpr float sqSize = 112.0f;
            float sqX = anchorMax.x - sqSize;
            float sqY = anchorMax.y + 5.0f;
            if (sqY + sqSize > barTop - 6.0f)
                sqY = anchorMin.y - sqSize - 5.0f;
            const float animSize = sqSize * (0.88f + 0.12f * open);
            const ImVec2 sqMin(sqX + (sqSize - animSize), sqY);
            const ImVec2 sqMax(sqMin.x + animSize, sqMin.y + animSize);
            s_LastColorPopupMin = sqMin;
            s_LastColorPopupMax = sqMax;

            const float popAlpha = alphaRow * open;
            const bool disabled = !interactive || visAlpha < 0.90f || !enabledRow || popAlpha < 0.05f;
            ImDrawList* fgList = ImGui::GetForegroundDrawList();

            auto hueToRgb = [](float h, float& hr, float& hg, float& hb) {
                float h6 = (h - floorf(h)) * 6.0f;
                int sector = static_cast<int>(h6);
                float f = h6 - static_cast<float>(sector);
                switch (sector % 6) {
                    case 0: hr = 1.0f; hg = f; hb = 0.0f; break;
                    case 1: hr = 1.0f - f; hg = 1.0f; hb = 0.0f; break;
                    case 2: hr = 0.0f; hg = 1.0f; hb = f; break;
                    case 3: hr = 0.0f; hg = 1.0f - f; hb = 1.0f; break;
                    case 4: hr = f; hg = 0.0f; hb = 1.0f; break;
                    default: hr = 1.0f; hg = 0.0f; hb = 1.0f - f; break;
                }
            };

            float r = state.color[0] < 0.0f ? 0.0f : (state.color[0] > 1.0f ? 1.0f : state.color[0]);
            float g = state.color[1] < 0.0f ? 0.0f : (state.color[1] > 1.0f ? 1.0f : state.color[1]);
            float b = state.color[2] < 0.0f ? 0.0f : (state.color[2] > 1.0f ? 1.0f : state.color[2]);
            float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float curX = state.boxAnim;
            float curY = 0.5f;
            if (mx != mn) {
                float h = 0.0f;
                if (mx == r) h = (g - b) / (mx - mn);
                else if (mx == g) h = 2.0f + (b - r) / (mx - mn);
                else h = 4.0f + (r - g) / (mx - mn);
                h /= 6.0f;
                if (h < 0.0f) h += 1.0f;
                curX = h;
                state.boxAnim = curX;
            }
            if (mn > 1.0f - mx)
                curY = 0.5f * (1.0f - mn);
            else
                curY = 0.5f + 0.5f * (1.0f - mx);

            fgList->AddRectFilled(ImVec2(sqMin.x - 2.0f, sqMin.y - 2.0f), ImVec2(sqMax.x + 2.0f, sqMax.y + 2.0f), IM_COL32(8, 2, 14, static_cast<int>(250.0f * popAlpha)), 5.0f);
            const int steps = 28;
            const float midY = (sqMin.y + sqMax.y) * 0.5f;
            const ImU32 whiteCol = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, popAlpha));
            const ImU32 blackCol = ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, popAlpha));
            for (int sx = 0; sx < steps; sx++) {
                const float t0 = static_cast<float>(sx) / static_cast<float>(steps);
                const float t1 = static_cast<float>(sx + 1) / static_cast<float>(steps);
                const float x0 = sqMin.x + animSize * t0;
                const float x1 = sqMin.x + animSize * t1;
                float r0 = 1.0f, g0 = 0.0f, b0 = 0.0f;
                float r1 = 1.0f, g1 = 0.0f, b1 = 0.0f;
                hueToRgb(t0, r0, g0, b0);
                hueToRgb(t1, r1, g1, b1);
                const ImU32 c0 = ImGui::ColorConvertFloat4ToU32(ImVec4(r0, g0, b0, popAlpha));
                const ImU32 c1 = ImGui::ColorConvertFloat4ToU32(ImVec4(r1, g1, b1, popAlpha));
                fgList->AddRectFilledMultiColor(ImVec2(x0, sqMin.y), ImVec2(x1, midY), whiteCol, whiteCol, c1, c0);
                fgList->AddRectFilledMultiColor(ImVec2(x0, midY), ImVec2(x1, sqMax.y), c0, c1, blackCol, blackCol);
            }
            fgList->AddRect(ImVec2(sqMin.x - 1.0f, sqMin.y - 1.0f), ImVec2(sqMax.x + 1.0f, sqMax.y + 1.0f), IM_COL32(126, 87, 194, static_cast<int>(210.0f * popAlpha)), 4.0f, 0, 1.0f);

            char fieldId[40] = {};
            ::snprintf(fieldId, sizeof(fieldId), "##ColorSquare%p", (void*)&state);
            ImGui::SetCursorScreenPos(sqMin);
            if (disabled)
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            (void)ImGui::InvisibleButton(fieldId, ImVec2(animSize, animSize));
            ImGui::PopStyleColor(3);
            const bool sqActive = !disabled && ImGui::IsItemActive();
            if (sqActive) {
                float nx = (ImGui::GetIO().MousePos.x - sqMin.x) / animSize;
                float ny = (ImGui::GetIO().MousePos.y - sqMin.y) / animSize;
                if (nx < 0.0f) nx = 0.0f;
                if (nx > 0.999f) nx = 0.999f;
                if (ny < 0.0f) ny = 0.0f;
                if (ny > 1.0f) ny = 1.0f;
                curX = nx;
                curY = ny;
                state.boxAnim = curX;
                float hr = 1.0f, hg = 0.0f, hb = 0.0f;
                hueToRgb(curX, hr, hg, hb);
                if (curY <= 0.5f) {
                    const float t = curY * 2.0f;
                    state.color[0] = 1.0f + (hr - 1.0f) * t;
                    state.color[1] = 1.0f + (hg - 1.0f) * t;
                    state.color[2] = 1.0f + (hb - 1.0f) * t;
                } else {
                    const float t = (curY - 0.5f) * 2.0f;
                    state.color[0] = hr * (1.0f - t);
                    state.color[1] = hg * (1.0f - t);
                    state.color[2] = hb * (1.0f - t);
                }
                state.color[3] = 1.0f;
            }
            if (disabled)
                ImGui::PopItemFlag();
            ImGui::Dummy(ImVec2(1.0f, 1.0f));

            const ImVec2 marker(sqMin.x + animSize * curX, sqMin.y + animSize * curY);
            fgList->AddCircle(marker, 4.5f, IM_COL32(255, 255, 255, static_cast<int>(245.0f * popAlpha)), 18, 1.5f);
            fgList->AddCircle(marker, 3.5f, IM_COL32(12, 4, 22, static_cast<int>(200.0f * popAlpha)), 18, 1.0f);
        };

        auto drawSwitchRow = [&](const char* id, const char* label, bool* value, float* anim, float* hover, EspColorState* colorState, int colorIndex, const ImVec2& cMin, const ImVec2& cMax, const ImVec2& bodyMin, const ImVec2& bodyMax, float& cursorY, float rowSpacing, bool enabledRow, float alphaRow, int cardIndex) {
            const float rowCenterY = cursorY + rowSpacing * 0.5f;
            const bool rowVisible = (rowCenterY >= bodyMin.y + 2.0f) && (rowCenterY <= bodyMax.y - 2.0f);
            constexpr float rowPadLeft = 14.0f;
            constexpr float rowPadRight = 18.0f;
            constexpr float colorSize = 18.0f;
            constexpr float gap = 10.0f;
            constexpr float swW = 20.0f;
            constexpr float swH = 20.0f;
            const float controlsW = colorState ? (colorSize + gap + swW) : swW;
            const float textMax = (cMax.x - rowPadRight) - (cMin.x + rowPadLeft) - controlsW - 14.0f;
            char buf[64] = {};
            ::strncpy(buf, label, sizeof(buf) - 1);
            TruncateTo(font, fontSize, buf, textMax);
            const ImVec2 labelSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, buf);
            drawList->AddLine(ImVec2(cMin.x + rowPadLeft, cursorY + rowSpacing - 0.5f), ImVec2(cMax.x - rowPadRight, cursorY + rowSpacing - 0.5f), IM_COL32(126, 87, 194, static_cast<int>(44.0f * alphaRow)), 1.0f);
            const ImVec2 swMin(cMax.x - rowPadRight - swW, rowCenterY - swH * 0.5f);
            const ImVec2 swMax(swMin.x + swW, swMin.y + swH);
            const bool disabled = !interactive || visAlpha < 0.90f || !enabledRow || !rowVisible || popupBlockingRows;
            ImGui::SetCursorScreenPos(swMin);
            if (disabled)
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(swW, swH));
            if (!disabled && pressed)
                *value = !*value;
            ImGui::PopStyleColor(3);
            if (disabled)
                ImGui::PopItemFlag();
            const bool hovered = !disabled && ImGui::IsItemHovered();
            const bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            *anim = AnimTowards(*anim, *value ? 1.0f : 0.0f, espDt, 0.10f);
            *hover = AnimTowards(*hover, hovered ? 1.0f : 0.0f, espDt, 0.07f);
            const float on = SmoothStep01(*anim);
            const float hovK = SmoothStep01(*hover);
            {
                const ImU32 textCol = IM_COL32(255, 255, 255, static_cast<int>((*value ? 255.0f : (125.0f + 45.0f * hovK)) * alphaRow));
                drawList->AddText(font, fontSize, ImVec2(cMin.x + rowPadLeft, rowCenterY - labelSize.y * 0.5f), textCol, buf);
            }
            const ImU32 baseBorder = IM_COL32(118, 112, 118, 255);
            const ImU32 ungreyBorder = LerpCol(baseBorder, kDarkPurpleLight, hovK, static_cast<int>(245.0f * alphaRow));
            const ImU32 borderCol = *value ? kDarkPurpleLight : ungreyBorder;
            const int borderA = static_cast<int>((*value ? 255.0f : (120.0f + 90.0f * hovK)) * alphaRow);
            const float radius = 4.0f;
            drawList->AddRectFilled(swMin, swMax, IM_COL32(10, 10, 10, static_cast<int>(245.0f * alphaRow)), radius);
            if (on > 0.01f) {
                const float shrink = (1.0f - on) * 3.2f;
                const ImVec2 fillMin(swMin.x + shrink, swMin.y + shrink);
                const ImVec2 fillMax(swMax.x - shrink, swMax.y - shrink);
                drawList->AddRectFilled(fillMin, fillMax, LerpCol(IM_COL32(29, 10, 58, 255), held ? IM_COL32(86, 34, 150, 255) : IM_COL32(116, 62, 190, 255), on, static_cast<int>((210.0f + 45.0f * on) * alphaRow)), radius * on);
            }
            const float fade = on > 0.55f ? (on - 0.55f) / 0.45f : 0.0f;
            if (fade > 0.01f) {
                const float fs = swW;
                const float tn = 2.1f;
                const float cx = swMin.x + fs * 0.50f;
                const float cy = swMin.y + fs * 0.52f;
                const ImVec2 pa(cx - fs * 0.28f, cy + fs * 0.02f);
                const ImVec2 pb(cx - fs * 0.09f, cy + fs * 0.21f);
                const ImVec2 pc(cx + fs * 0.30f, cy - fs * 0.22f);
                const ImU32 checkCol = IM_COL32(16, 7, 26, static_cast<int>(255.0f * fade * alphaRow));
                drawList->PathClear();
                drawList->PathLineTo(pa);
                drawList->PathLineTo(pb);
                drawList->PathLineTo(pc);
                drawList->PathStroke(checkCol, 0, tn);
            }
            const ImU32 finalBorder = held ? IM_COL32(168, 132, 235, static_cast<int>(255.0f * alphaRow)) : borderCol;
            const int finalA = *value ? static_cast<int>(255.0f * alphaRow) : borderA;
            drawList->AddRect(swMin, swMax, IM_COL32((finalBorder >> IM_COL32_R_SHIFT) & 0xFF, (finalBorder >> IM_COL32_G_SHIFT) & 0xFF, (finalBorder >> IM_COL32_B_SHIFT) & 0xFF, finalA), 4.0f, 0, 1.15f);
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
            if (colorState) {
                char colorId[48] = {};
                ::snprintf(colorId, sizeof(colorId), "##EspColor%d_%d", cardIndex, colorIndex);
                const ImVec2 colorMin(swMin.x - gap - colorSize, rowCenterY - colorSize * 0.5f);
                drawColorBox(colorId, *colorState, colorMin, alphaRow, enabledRow, rowVisible, colorIndex, cardIndex);
            }
            cursorY += rowSpacing;
        };
        auto drawDropdownRow = [&](const char* id, const char* label, int* value, const char* const* items, int count, int target, const ImVec2& cMin, const ImVec2& cMax, const ImVec2& bodyMin, const ImVec2& bodyMax, float& cursorY, float rowSpacing, bool enabledRow, float alphaRow) {
            const float rowCenterY = cursorY + rowSpacing * 0.5f;
            const bool rowVisible = (rowCenterY >= bodyMin.y + 2.0f) && (rowCenterY <= bodyMax.y - 2.0f);
            constexpr float rowPadLeft = 14.0f;
            constexpr float rowPadRight = 18.0f;
            char buf[64] = {};
            ::strncpy(buf, label, sizeof(buf) - 1);
            constexpr float ddW = 128.0f;
            constexpr float ddH = 26.0f;
            const float textMax = (cMax.x - rowPadRight - ddW) - (cMin.x + 24.0f);
            TruncateTo(font, fontSize, buf, textMax);
            const ImVec2 labelSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, buf);
            drawList->AddLine(ImVec2(cMin.x + rowPadLeft, cursorY + rowSpacing - 0.5f), ImVec2(cMax.x - rowPadRight, cursorY + rowSpacing - 0.5f), IM_COL32(126, 87, 194, static_cast<int>(44.0f * alphaRow)), 1.0f);
            drawList->AddText(font, fontSize, ImVec2(cMin.x + 24.0f, rowCenterY - labelSize.y * 0.5f), IM_COL32(216, 205, 235, static_cast<int>(235.0f * alphaRow)), buf);
            const ImVec2 ddMin(cMax.x - rowPadRight - ddW, rowCenterY - ddH * 0.5f);
            const ImVec2 ddMax(ddMin.x + ddW, ddMin.y + ddH);
            const bool disabled = !interactive || visAlpha < 0.90f || !enabledRow || !rowVisible || popupBlockingRows;
            ImGui::SetCursorScreenPos(ddMin);
            if (disabled)
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(ddW, ddH));
            if (!disabled && pressed) {
                if (g_EspDropdownOpen && g_EspDropTarget == target)
                    g_EspDropdownOpen = false;
                else { g_EspDropdownOpen = true; g_EspDropTarget = target; g_ColorPickerOpen = -1; }
            }
            ImGui::PopStyleColor(3);
            if (disabled)
                ImGui::PopItemFlag();
            drawList->AddRectFilled(ddMin, ddMax, IM_COL32(9, 3, 17, static_cast<int>(245.0f * alphaRow)), 6.0f);
            drawList->AddRect(ddMin, ddMax, IM_COL32(126, 87, 194, static_cast<int>(190.0f * alphaRow)), 6.0f, 0, 1.0f);
            const ImVec2 valSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, items[*value]);
            drawList->AddText(font, fontSize, ImVec2(ddMin.x + 9.0f, ddMin.y + (ddH - valSize.y) * 0.5f), IM_COL32(255, 255, 255, static_cast<int>(255.0f * alphaRow)), items[*value]);
            const char* arrow = (g_EspDropdownOpen && g_EspDropTarget == target) ? ICON_FA_CHEVRON_UP : ICON_FA_CHEVRON_DOWN;
            const ImVec2 arrowSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, arrow);
            drawList->AddText(font, fontSize, ImVec2(ddMax.x - 9.0f - arrowSize.x, ddMin.y + (ddH - arrowSize.y) * 0.5f), IM_COL32(190, 168, 225, static_cast<int>(235.0f * alphaRow)), arrow);
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
            cursorY += rowSpacing;
            if (g_EspDropdownOpen && g_EspDropTarget == target) {
                deferredDrop.active = true;
                deferredDrop.value = value;
                deferredDrop.items = items;
                deferredDrop.count = count;
                deferredDrop.target = target;
                deferredDrop.ddMin = ddMin;
                deferredDrop.ddMax = ddMax;
                deferredDrop.alphaRow = alphaRow;
                deferredDrop.disabled = !interactive || visAlpha < 0.90f || !enabledRow || !rowVisible;
            }
        };

        auto updateCardScroll = [&](int cardIndex, const ImVec2& cMin, const ImVec2& cMax, bool master, float reveal, float boxSlot, float linesSlot, float rowStep, float& outMaxScroll, float& outContentH, float& outViewH) {
            outViewH = (cMax.y - 3.0f) - (cMin.y + 32.0f);
            float rows = 1.0f;
            if (master || reveal > 0.01f) {
                const float rev = SmoothStep01(reveal);
                rows += rev * (7.0f + 2.0f * boxSlot + linesSlot);
            }
            outContentH = 10.0f + rows * rowStep;
            outMaxScroll = outContentH > outViewH ? (outContentH - outViewH) : 0.0f;
            const ImVec2 bodyMin(cMin.x, cMin.y + 32.0f);
            if (interactive && visAlpha >= 0.90f && outMaxScroll > 0.0f && !popupBlockingRows && ImGui::IsMouseHoveringRect(bodyMin, cMax, false)) {
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    g_EspScrollTarget[cardIndex] -= wheel * 38.0f;
                    g_EspDropdownOpen = false;
                    g_ColorPickerOpen = -1;
                }
            }
            if (g_EspScrollTarget[cardIndex] < 0.0f) g_EspScrollTarget[cardIndex] = 0.0f;
            if (g_EspScrollTarget[cardIndex] > outMaxScroll) g_EspScrollTarget[cardIndex] = outMaxScroll;
            const float diff = g_EspScrollTarget[cardIndex] - g_EspScroll[cardIndex];
            if (fabsf(diff) > 0.15f)
                g_EspScroll[cardIndex] += diff * (1.0f - expf(-espDt * 22.0f));
            else
                g_EspScroll[cardIndex] = g_EspScrollTarget[cardIndex];
            if (g_EspScroll[cardIndex] < 0.0f) g_EspScroll[cardIndex] = 0.0f;
            if (g_EspScroll[cardIndex] > outMaxScroll) g_EspScroll[cardIndex] = outMaxScroll;
        };

        auto drawCardScrollbar = [&](const char* scrollId, int cardIndex, const ImVec2& cMin, const ImVec2& cMax, float maxScroll, float contentH, float viewH, float cardVisAlpha) {
            if (maxScroll <= 0.5f || contentH <= 0.0f)
                return;
            const float trackX0 = cMax.x - 7.0f;
            const float trackX1 = cMax.x - 3.0f;
            const float trackY0 = cMin.y + 37.0f;
            const float trackY1 = cMax.y - 6.0f;
            const float trackH = trackY1 - trackY0;
            if (trackH <= 12.0f)
                return;
            float thumbH = trackH * (viewH / contentH);
            if (thumbH < 24.0f) thumbH = 24.0f;
            if (thumbH > trackH) thumbH = trackH;
            const float usableH = trackH - thumbH;
            ImGui::SetCursorScreenPos(ImVec2(trackX0 - 4.0f, trackY0));
            const bool dis = !interactive || visAlpha < 0.90f || popupBlockingRows;
            if (dis)
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            (void)ImGui::InvisibleButton(scrollId, ImVec2(10.0f, trackH));
            ImGui::PopStyleColor(3);
            const bool hovered = !dis && ImGui::IsItemHovered();
            const bool held = !dis && ImGui::IsItemActive();
            if (held && usableH > 0.0f) {
                float ratio = (ImGui::GetIO().MousePos.y - trackY0 - thumbH * 0.5f) / usableH;
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                g_EspScrollTarget[cardIndex] = ratio * maxScroll;
                g_EspScroll[cardIndex] = g_EspScrollTarget[cardIndex];
            }
            if (dis)
                ImGui::PopItemFlag();
            ImGui::Dummy(ImVec2(1.0f, 1.0f));

            const float ratio = maxScroll > 0.0f ? (g_EspScroll[cardIndex] / maxScroll) : 0.0f;
            const float thumbY0 = trackY0 + usableH * ratio;
            const float thumbY1 = thumbY0 + thumbH;
            drawList->AddRectFilled(ImVec2(trackX0, trackY0), ImVec2(trackX1, trackY1), IM_COL32(22, 7, 38, static_cast<int>(185.0f * cardVisAlpha)), 2.0f);
            const ImU32 thumbCol = held ? IM_COL32(168, 132, 235, static_cast<int>(255.0f * cardVisAlpha))
                : (hovered ? IM_COL32(146, 108, 214, static_cast<int>(245.0f * cardVisAlpha))
                           : IM_COL32(126, 87, 194, static_cast<int>(220.0f * cardVisAlpha)));
            drawList->AddRectFilled(ImVec2(trackX0, thumbY0), ImVec2(trackX1, thumbY1), thumbCol, 2.0f);
        };

        drawCardFrame(playersMin, playersMax, ICON_FA_USERS, "Players");
        drawHeaderButton("##PlayersHeader", playersMin, playersMax, &g_EspPlayersMaster);
        drawCardFrame(localMin, localMax, ICON_FA_USER, "Local");
        drawHeaderButton("##LocalHeader", localMin, localMax, &g_EspMaster);

        constexpr float kEspRowHeight = 38.0f;
        {
            const float boxSlot = SmoothStep01(g_EspBoxReveal);
            const float linesSlot = SmoothStep01(g_EspLinesReveal);
            float maxScroll = 0.0f, contentH = 0.0f, viewH = 0.0f;
            updateCardScroll(0, localMin, localMax, g_EspMaster, g_EspReveal, boxSlot, linesSlot, kEspRowHeight, maxScroll, contentH, viewH);
            const ImVec2 bodyMin(localMin.x + 1.0f, localMin.y + 32.0f);
            const ImVec2 bodyMax(localMax.x - 1.0f, localMax.y - 3.0f);
            drawList->PushClipRect(bodyMin, bodyMax, true);
            float y = localMin.y + 32.0f + 4.0f - g_EspScroll[0];
            drawSwitchRow("##EspEnableLocal", "Enable Local Esp", &g_EspMaster, &g_EspMasterAnim, &g_EspMasterHover, nullptr, -1, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight, true, visAlpha, 0);
            if (g_EspMaster) g_EspSelfSynced = true;
            if (g_EspMaster || g_EspReveal > 0.01f) {
                drawSwitchRow("##EspBox", "Esp Box", &g_EspBox, &g_EspBoxAnim, &g_EspBoxHover, &g_EspColorsLocal[EspColor_Box], EspColor_Box, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspMaster, localAlpha, 0);
                if (g_EspBox || g_EspBoxReveal > 0.01f) {
                    static const char* const boxStyles[3] = { "Corner", "Rounded", "Default" };
                    drawDropdownRow("##EspBoxStyle", "Box Style", &g_EspBoxStyle, boxStyles, 3, 0, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight * boxSlot, g_EspMaster, localAlpha * boxSlot);
                    drawSwitchRow("##EspBoxFilled", "Box Filled", &g_EspBoxFilled, &g_EspBoxFilledAnim, &g_EspBoxFilledHover, &g_EspColorsLocal[EspColor_BoxFilled], EspColor_BoxFilled, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight * boxSlot, g_EspMaster, localAlpha * boxSlot, 0);
                }
                drawSwitchRow("##EspLines", "Esp Lines", &g_EspLines, &g_EspLinesAnim, &g_EspLinesHover, &g_EspColorsLocal[EspColor_Lines], EspColor_Lines, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspMaster, localAlpha, 0);
                if (g_EspLines || g_EspLinesReveal > 0.01f) {
                    static const char* const linePositions[3] = { "Top", "Mid", "Bottom" };
                    drawDropdownRow("##EspLinesPos", "Lines Positions", &g_EspLinesPos, linePositions, 3, 1, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight * linesSlot, g_EspMaster, localAlpha * linesSlot);
                }
                drawSwitchRow("##EspHealth", "Esp Health", &g_EspHealth, &g_EspHealthAnim, &g_EspHealthHover, &g_EspColorsLocal[EspColor_Health], EspColor_Health, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspMaster, localAlpha, 0);
                drawSwitchRow("##EspArmor", "Esp Armor", &g_EspArmor, &g_EspArmorAnim, &g_EspArmorHover, &g_EspColorsLocal[EspColor_Armor], EspColor_Armor, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspMaster, localAlpha, 0);
                drawSwitchRow("##EspNames", "Esp Names", &g_EspNames, &g_EspNamesAnim, &g_EspNamesHover, &g_EspColorsLocal[EspColor_Names], EspColor_Names, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspMaster, localAlpha, 0);
                drawSwitchRow("##EspDistance", "Esp Distance", &g_EspDistance, &g_EspDistanceAnim, &g_EspDistanceHover, &g_EspColorsLocal[EspColor_Distance], EspColor_Distance, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspMaster, localAlpha, 0);
                drawSwitchRow("##EspSkeleton", "Esp Skeleton", &g_EspSkeleton, &g_EspSkeletonAnim, &g_EspSkeletonHover, &g_EspColorsLocal[EspColor_Skeleton], EspColor_Skeleton, localMin, localMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspMaster, localAlpha, 0);
            }
            drawList->PopClipRect();
            drawCardScrollbar("##LocalCardScroll", 0, localMin, localMax, maxScroll, contentH, viewH, localAlpha);
            drawList->AddRect(localMin, localMax, IM_COL32(126, 87, 194, static_cast<int>(190.0f * visAlpha)), 8.0f, 0, 1.0f);
        }
        {
            const float boxSlotP = SmoothStep01(g_EspBoxRevealP);
            const float linesSlotP = SmoothStep01(g_EspLinesRevealP);
            float maxScrollP = 0.0f, contentHP = 0.0f, viewHP = 0.0f;
            updateCardScroll(1, playersMin, playersMax, g_EspPlayersMaster, g_EspPlayersReveal, boxSlotP, linesSlotP, kEspRowHeight, maxScrollP, contentHP, viewHP);
            const ImVec2 bodyMin(playersMin.x + 1.0f, playersMin.y + 32.0f);
            const ImVec2 bodyMax(playersMax.x - 1.0f, playersMax.y - 3.0f);
            drawList->PushClipRect(bodyMin, bodyMax, true);
            float y = playersMin.y + 32.0f + 4.0f - g_EspScroll[1];
            drawSwitchRow("##EspEnablePlayers", "Enable Player Esp", &g_EspPlayersMaster, &g_EspPlayersMasterAnim, &g_EspPlayersMasterHover, nullptr, -1, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight, true, visAlpha, 1);
            if (g_EspPlayersMaster || g_EspPlayersReveal > 0.01f) {
                drawSwitchRow("##EspPBox", "Esp Box", &g_EspBoxP, &g_EspBoxPAnim, &g_EspBoxPHover, &g_EspColorsPlayers[EspColor_Box], EspColor_Box, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspPlayersMaster, playersAlpha, 1);
                if (g_EspBoxP || g_EspBoxRevealP > 0.01f) {
                    static const char* const boxStylesP[3] = { "Corner", "Rounded", "Default" };
                    drawDropdownRow("##EspPBoxStyle", "Box Style", &g_EspBoxStyleP, boxStylesP, 3, 2, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight * boxSlotP, g_EspPlayersMaster, playersAlpha * boxSlotP);
                    drawSwitchRow("##EspPBoxFilled", "Box Filled", &g_EspBoxFilledP, &g_EspBoxFilledPAnim, &g_EspBoxFilledPHover, &g_EspColorsPlayers[EspColor_BoxFilled], EspColor_BoxFilled, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight * boxSlotP, g_EspPlayersMaster, playersAlpha * boxSlotP, 1);
                }
                drawSwitchRow("##EspPLines", "Esp Lines", &g_EspLinesP, &g_EspLinesPAnim, &g_EspLinesPHover, &g_EspColorsPlayers[EspColor_Lines], EspColor_Lines, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspPlayersMaster, playersAlpha, 1);
                if (g_EspLinesP || g_EspLinesRevealP > 0.01f) {
                    static const char* const linePositionsP[3] = { "Top", "Mid", "Bottom" };
                    drawDropdownRow("##EspPLinesPos", "Lines Positions", &g_EspLinesPosP, linePositionsP, 3, 3, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight * linesSlotP, g_EspPlayersMaster, playersAlpha * linesSlotP);
                }
                drawSwitchRow("##EspPHealth", "Esp Health", &g_EspHealthP, &g_EspHealthPAnim, &g_EspHealthPHover, &g_EspColorsPlayers[EspColor_Health], EspColor_Health, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspPlayersMaster, playersAlpha, 1);
                drawSwitchRow("##EspPArmor", "Esp Armor", &g_EspArmorP, &g_EspArmorPAnim, &g_EspArmorPHover, &g_EspColorsPlayers[EspColor_Armor], EspColor_Armor, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspPlayersMaster, playersAlpha, 1);
                drawSwitchRow("##EspPNames", "Esp Names", &g_EspNamesP, &g_EspNamesPAnim, &g_EspNamesPHover, &g_EspColorsPlayers[EspColor_Names], EspColor_Names, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspPlayersMaster, playersAlpha, 1);
                drawSwitchRow("##EspPDistance", "Esp Distance", &g_EspDistanceP, &g_EspDistancePAnim, &g_EspDistancePHover, &g_EspColorsPlayers[EspColor_Distance], EspColor_Distance, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspPlayersMaster, playersAlpha, 1);
                drawSwitchRow("##EspPSkeleton", "Esp Skeleton", &g_EspSkeletonP, &g_EspSkeletonPAnim, &g_EspSkeletonPHover, &g_EspColorsPlayers[EspColor_Skeleton], EspColor_Skeleton, playersMin, playersMax, bodyMin, bodyMax, y, kEspRowHeight, g_EspPlayersMaster, playersAlpha, 1);
            }
            drawList->PopClipRect();
            drawCardScrollbar("##PlayersCardScroll", 1, playersMin, playersMax, maxScrollP, contentHP, viewHP, playersAlpha);
            drawList->AddRect(playersMin, playersMax, IM_COL32(126, 87, 194, static_cast<int>(190.0f * visAlpha)), 8.0f, 0, 1.0f);
        }

        if (deferredDrop.active) {
            constexpr float ddW = 128.0f;
            constexpr float ddH = 26.0f;
            const float fullH = ddH * static_cast<float>(deferredDrop.count);
            const float listH = fullH * g_EspDropAnim;
            const ImVec2 listMin(deferredDrop.ddMin.x, deferredDrop.ddMax.y + 4.0f);
            const ImVec2 listMax(deferredDrop.ddMax.x, listMin.y + listH);
            s_LastDropPopupMin = listMin;
            s_LastDropPopupMax = listMax;
            ImDrawList* fgList = ImGui::GetForegroundDrawList();
            fgList->AddRectFilled(listMin, listMax, IM_COL32(9, 3, 17, static_cast<int>(248.0f * deferredDrop.alphaRow)), 6.0f);
            fgList->AddRect(listMin, listMax, IM_COL32(126, 87, 194, static_cast<int>(190.0f * deferredDrop.alphaRow)), 6.0f, 0, 1.0f);
            for (int item = 0; item < deferredDrop.count; item++) {
                const ImVec2 itemMin(deferredDrop.ddMin.x, listMin.y + ddH * static_cast<float>(item));
                const ImVec2 itemMax(deferredDrop.ddMax.x, itemMin.y + ddH);
                const bool clipped = itemMax.y > listMax.y + 1.0f;
                ImGui::SetCursorScreenPos(itemMin);
                if (deferredDrop.disabled || clipped)
                    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                char itemId[40] = {};
                ::snprintf(itemId, sizeof(itemId), "##EspDrop%d_%d", deferredDrop.target, item);
                const bool itemPressed = ImGui::InvisibleButton(itemId, ImVec2(ddW, ddH));
                if (!deferredDrop.disabled && !clipped && itemPressed) {
                    *deferredDrop.value = item;
                    g_EspDropdownOpen = false;
                }
                ImGui::PopStyleColor(3);
                if (deferredDrop.disabled || clipped)
                    ImGui::PopItemFlag();
                if (!clipped) {
                    if (!deferredDrop.disabled && ImGui::IsItemHovered())
                        fgList->AddRectFilled(itemMin, itemMax, IM_COL32(56, 13, 104, static_cast<int>(110.0f * deferredDrop.alphaRow)), 4.0f);
                    const ImVec2 itemSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, deferredDrop.items[item]);
                    fgList->AddText(font, fontSize, ImVec2(itemMin.x + 9.0f, itemMin.y + (ddH - itemSize.y) * 0.5f), item == *deferredDrop.value ? IM_COL32(188, 158, 232, static_cast<int>(255.0f * deferredDrop.alphaRow)) : IM_COL32(255, 255, 255, static_cast<int>(235.0f * deferredDrop.alphaRow)), deferredDrop.items[item]);
                }
                ImGui::Dummy(ImVec2(1.0f, 1.0f));
            }
        } else {
            s_LastDropPopupMin = ImVec2(0.0f, 0.0f);
            s_LastDropPopupMax = ImVec2(0.0f, 0.0f);
        }

        if (activeColorState != nullptr && g_ColorPickerOpen >= 0) {
            drawColorSquarePopup(*activeColorState, activeColorAnchorMin, activeColorAnchorMax, activeColorAlpha, activeColorEnabled);
        } else {
            s_LastColorPopupMin = ImVec2(0.0f, 0.0f);
            s_LastColorPopupMax = ImVec2(0.0f, 0.0f);
        }
    }

    for (int i = 0; i < 3; i++) {
        const ImVec2 buttonMin(offsetX + centers[i] - buttonSize * 0.5f, barTop + (barHeight - buttonSize) * 0.5f);
        ImGui::SetCursorScreenPos(buttonMin);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (ImGui::InvisibleButton(ids[i], ImVec2(buttonSize, buttonSize)) && interactive)
            g_ActiveTab = i;
        ImGui::PopStyleColor(3);

        const float tabWeight = SmoothStep01(g_TabWeights[i]);
        const ImVec2 iconSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, icons[i]);
        const ImVec2 iconPos(buttonMin.x + (buttonSize - iconSize.x) * 0.5f, buttonMin.y + (buttonSize - iconSize.y) * 0.5f);
        if (tabWeight > 0.01f) {
            const ImU32 glowColor = IM_COL32(74, 20, 140, static_cast<int>(64.0f * tabWeight * alpha));
            drawList->AddText(font, fontSize, ImVec2(iconPos.x - 1.0f, iconPos.y), glowColor, icons[i]);
            drawList->AddText(font, fontSize, ImVec2(iconPos.x + 1.0f, iconPos.y), glowColor, icons[i]);
            drawList->AddText(font, fontSize, ImVec2(iconPos.x, iconPos.y - 1.0f), glowColor, icons[i]);
            drawList->AddText(font, fontSize, ImVec2(iconPos.x, iconPos.y + 1.0f), glowColor, icons[i]);
        }
        const ImU32 iconColor = IM_COL32(
            static_cast<int>(255.0f - 129.0f * tabWeight),
            static_cast<int>(255.0f - 168.0f * tabWeight),
            static_cast<int>(255.0f - 61.0f * tabWeight),
            static_cast<int>((235.0f + 20.0f * tabWeight) * alpha));
        drawList->AddText(font, fontSize, iconPos, iconColor, icons[i]);
    }
}

static float EaseOutCubic(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    const float inverse = 1.0f - value;
    return 1.0f - inverse * inverse * inverse;
}

static float EaseInOutCubic(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    if (value < 0.5f)
        return 4.0f * value * value * value;
    const float f = -2.0f * value + 2.0f;
    return 1.0f - f * f * f * 0.5f;
}

static void DrawNotify(float width, float height) {
    if (!g_Notify.active || g_Notify.title == nullptr || g_Notify.content == nullptr)
        return;

    constexpr double duration = 3.4;
    constexpr double slideTime = 0.45;
    constexpr double fadeTime = 0.30;
    const double elapsed = ImGui::GetTime() - g_Notify.startedAt;
    if (elapsed >= duration) {
        g_Notify.active = false;
        return;
    }

    float alpha = 1.0f;
    if (elapsed > duration - fadeTime)
        alpha = static_cast<float>((duration - elapsed) / fadeTime);

    const float slide = elapsed < slideTime
        ? EaseOutCubic(static_cast<float>(elapsed / slideTime))
        : (elapsed > duration - fadeTime
            ? EaseOutCubic(static_cast<float>((duration - elapsed) / fadeTime))
            : 1.0f);

    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 titleSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, g_Notify.title);
    const ImVec2 contentSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, g_Notify.content);

    constexpr float notifyWidth = 286.0f;
    constexpr float paddingX = 14.0f;
    constexpr float paddingY = 11.0f;
    constexpr float iconBox = 28.0f;
    constexpr float gap = 10.0f;
    constexpr float lineGap = 3.0f;
    constexpr float margin = 16.0f;
    const float textHeight = titleSize.y + lineGap + contentSize.y;
    const float notifyHeight = (textHeight > iconBox ? textHeight : iconBox) + paddingY * 2.0f;

    const float finalX = width - notifyWidth + 18.0f;
    const float startX = width + 12.0f;
    const float notifyX = startX + (finalX - startX) * slide;
    const ImVec2 notifyMin(notifyX, height - notifyHeight - margin);
    const ImVec2 notifyMax(notifyMin.x + notifyWidth, notifyMin.y + notifyHeight);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const int bgAlpha = static_cast<int>(248.0f * alpha);
    const int borderAlpha = static_cast<int>(220.0f * alpha);
    const int iconAlpha = static_cast<int>(235.0f * alpha);
    drawList->AddRectFilled(notifyMin, notifyMax, IM_COL32(18, 5, 32, bgAlpha), 8.0f);
    const ImVec2 stripeMin(notifyMin.x + 1.0f, notifyMin.y + 9.0f);
    const ImVec2 stripeMax(notifyMin.x + 4.0f, notifyMax.y - 9.0f);
    drawList->AddRectFilled(stripeMin, stripeMax, IM_COL32(74, 20, 140, borderAlpha), 1.5f);
    drawList->AddRect(notifyMin, notifyMax, IM_COL32(74, 20, 140, borderAlpha), 8.0f, 0, 1.0f);

    const ImVec2 iconMin(notifyMin.x + paddingX, notifyMin.y + (notifyHeight - iconBox) * 0.5f);
    const ImVec2 iconMax(iconMin.x + iconBox, iconMin.y + iconBox);
    drawList->AddRectFilled(iconMin, iconMax, IM_COL32(74, 20, 140, iconAlpha), 7.0f);
    drawList->AddRect(iconMin, iconMax, IM_COL32(126, 87, 194, borderAlpha), 7.0f, 0, 1.0f);
    const ImVec2 bellSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, ICON_FA_BELL);
    drawList->AddText(font, fontSize,
        ImVec2(iconMin.x + (iconBox - bellSize.x) * 0.5f, iconMin.y + (iconBox - bellSize.y) * 0.5f),
        IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha)), ICON_FA_BELL);

    const float textX = iconMax.x + gap;
    float textY = notifyMin.y + paddingY;
    drawList->AddText(font, fontSize, ImVec2(textX, textY),
        IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha)), g_Notify.title);
    textY += titleSize.y + lineGap;
    drawList->AddText(font, fontSize, ImVec2(textX, textY),
        IM_COL32(216, 205, 235, static_cast<int>(225.0f * alpha)), g_Notify.content);
}

static bool ShowMainUI() {
    const ImGuiIO& io = ImGui::GetIO();
    const float width = io.DisplaySize.x;
    const float height = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, height));

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;

    bool running = true;
    ImGui::Begin("##MainWindow", nullptr, flags);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 windowMin = ImGui::GetWindowPos();
    const ImVec2 windowMax(windowMin.x + width, windowMin.y + height);

    for (float y = windowMin.y; y < windowMax.y; y += 1.0f) {
        const float centerY = y + 0.5f;
        float inset = 0.0f;
        if (centerY < windowMin.y + 12.0f) {
            const float dy = (windowMin.y + 12.0f) - centerY;
            inset = 12.0f - sqrtf(144.0f - dy * dy);
        } else if (centerY > windowMax.y - 12.0f) {
            const float dy = centerY - (windowMax.y - 12.0f);
            inset = 12.0f - sqrtf(144.0f - dy * dy);
        }
        const float t = (y - windowMin.y) / height;
        const float local = t > 0.90f ? (t - 0.90f) / 0.10f : 0.0f;
        const float eased = local * local * (3.0f - 2.0f * local);
        const ImU32 rowColor = IM_COL32((int)(24.0f * eased), (int)(6.0f * eased), (int)(42.0f * eased), 255);
        drawList->AddRectFilled(ImVec2(windowMin.x + inset, y), ImVec2(windowMax.x - inset, y + 1.0f), rowColor, 0.0f);
    }
    if (g_ScreenAnim < 1.0f) {
        float slideDt = ImGui::GetIO().DeltaTime;
        if (slideDt > 0.05f) slideDt = 0.05f;
        g_ScreenAnim += AnimStep(slideDt, 0.48f);
        if (g_ScreenAnim > 1.0f) g_ScreenAnim = 1.0f;
    }
    const float pageAmount = g_ScreenFrom + (g_ScreenTarget - g_ScreenFrom) * EaseInOutCubic(g_ScreenAnim);
    const bool transitioning = g_ScreenAnim < 1.0f;
    const float homeAlpha = 1.0f - pageAmount;
    const float loadedAlpha = pageAmount;
    const float homeOffset = -pageAmount * width;
    const float loadedOffset = (1.0f - pageAmount) * width;

    if (pageAmount < 0.999f) {
        if (transitioning)
            drawList->PushClipRect(windowMin, windowMax, true);
        DrawHeaderIcon(drawList, width, homeOffset, homeAlpha);
        if (transitioning)
            drawList->PopClipRect();
    } else {
        DrawHeaderIcon(drawList, width, homeOffset, 0.0f);
    }
    if (DrawCloseButton(width))
        running = false;

    static bool dragging = false;
    static POINT dragStartCursor = {};
    static RECT dragStartWindow = {};

    if (ImGui::IsMouseHoveringRect(ImVec2(0.0f, 0.0f), ImVec2(width, 128.0f)) &&
        !ImGui::IsAnyItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        dragging = true;
        ::GetCursorPos(&dragStartCursor);
        ::GetWindowRect(g_Hwnd, &dragStartWindow);
    }
    if (dragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            POINT cursor = {};
            ::GetCursorPos(&cursor);
            ::SetWindowPos(g_Hwnd, nullptr,
                dragStartWindow.left + (cursor.x - dragStartCursor.x),
                dragStartWindow.top + (cursor.y - dragStartCursor.y),
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        else {
            dragging = false;
        }
    }

    const bool loadBusy = g_LoadBusy.load();
    if (pageAmount < 0.999f) {
        if (DrawToggleButton(width, height, !loadBusy && !transitioning, homeOffset, homeAlpha) && !g_IsLoaded)
            StartLoad();
    } else {
        (void)DrawToggleButton(width, height, false, homeOffset, 0.0f);
    }

    if (pageAmount > 0.001f)
        DrawLoadedPage(width, height, loadedOffset, loadedAlpha, !transitioning);

    DrawNotify(width, height);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        running = false;

    ImGui::End();
    return running;
}

static void SetupStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 12.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.055f, 0.018f, 0.105f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.025f, 0.006f, 0.055f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(0.290f, 0.078f, 0.549f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.145f, 0.031f, 0.290f, 0.35f);
    colors[ImGuiCol_TextDisabled] = ImVec4(1.0f, 1.0f, 1.0f, 0.38f);
}

static bool LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();

    ImFontConfig fontConfig = {};
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 2;
    ImFont* arial = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\arial.ttf", 17.0f, &fontConfig);
    if (arial == nullptr)
        arial = io.Fonts->AddFontDefault();
    io.FontDefault = arial;

    static const ImWchar kIconRange[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    ImFontConfig iconConfig = {};
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = 17.0f;
    iconConfig.OversampleH = 3;
    iconConfig.OversampleV = 1;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        FA_solid_900_data, FA_solid_900_size, 15.0f, &iconConfig, kIconRange);

    return true;
}

static bool LoadHeaderIconTexture() {
    if (g_HeaderIconSRV != nullptr)
        return true;
    if (g_Device == nullptr || sizeof(icon) <= 8)
        return false;

    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool mustUninitialize = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return false;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    BYTE* pixels = nullptr;
    bool ok = false;

    hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
        hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<unsigned char*>(icon)),
            static_cast<DWORD>(sizeof(icon)));
    if (SUCCEEDED(hr))
        hr = factory->CreateDecoderFromStream(stream, nullptr,
            WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr))
        hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);

    UINT imageW = 0;
    UINT imageH = 0;
    if (SUCCEEDED(hr))
        hr = converter->GetSize(&imageW, &imageH);

    if (SUCCEEDED(hr) && imageW > 0 && imageH > 0 && imageW <= 4096 && imageH <= 4096) {
        g_HeaderIconAspect = static_cast<float>(imageW) / static_cast<float>(imageH);
        const UINT stride = imageW * 4;
        pixels = new BYTE[static_cast<size_t>(stride) * imageH];
        hr = converter->CopyPixels(nullptr, stride, stride * imageH, pixels);
        if (SUCCEEDED(hr)) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = imageW;
            desc.Height = imageH;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem = pixels;
            initData.SysMemPitch = stride;

            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(g_Device->CreateTexture2D(&desc, &initData, &texture)) && texture != nullptr) {
                ok = SUCCEEDED(g_Device->CreateShaderResourceView(texture, nullptr, &g_HeaderIconSRV));
                texture->Release();
            }
        }
    }

    delete[] pixels;
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (mustUninitialize)
        ::CoUninitialize();
    return ok;
}

static void CleanupHeaderIconTexture() { if (g_HeaderIconSRV) { g_HeaderIconSRV->Release(); g_HeaderIconSRV = nullptr; } }

static void DrawHeaderIcon(ImDrawList* drawList, float windowWidth, float offsetX, float alpha) {
    constexpr float iconBox = 96.0f;
    constexpr float iconTop = 28.0f;
    alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    float iconWidth = iconBox;
    float iconHeight = iconBox;
    if (g_HeaderIconAspect > 1.0f)
        iconHeight = iconBox / g_HeaderIconAspect;
    else if (g_HeaderIconAspect > 0.0f)
        iconWidth = iconBox * g_HeaderIconAspect;
    const ImVec2 iconMin((windowWidth - iconWidth) * 0.5f + offsetX, iconTop + (iconBox - iconHeight) * 0.5f);
    const ImVec2 iconMax(iconMin.x + iconWidth, iconMin.y + iconHeight);
    if (g_HeaderIconSRV != nullptr)
        drawList->AddImage(reinterpret_cast<ImTextureID>(g_HeaderIconSRV), iconMin, iconMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha)));
}

static bool ApplyStreamProofAffinity(HWND hwnd) {
    return hwnd != nullptr && ::SetWindowDisplayAffinity(hwnd, WDA_NONE) != FALSE;
}

static bool CreateDeviceD3D(HWND hwnd) {
    const D3D_FEATURE_LEVEL featureLevels[2] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, 2, D3D11_SDK_VERSION,
        &g_Device, &createdLevel, &g_Context);
    if (FAILED(hr)) {
        const HRESULT hardwareResult = hr;
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, 2, D3D11_SDK_VERSION,
            &g_Device, &createdLevel, &g_Context);
        if (FAILED(hr)) {
            FailGraphics(L"D3D11CreateDevice (hardware e WARP)", hr);
            (void)hardwareResult;
            return false;
        }
    }

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* dxgiAdapter = nullptr;
    IDXGIFactory2* dxgiFactory = nullptr;

    hr = g_Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr))
        FailGraphics(L"QueryInterface(IDXGIDevice)", hr);
    else {
        hr = dxgiDevice->GetAdapter(&dxgiAdapter);
        if (FAILED(hr))
            FailGraphics(L"IDXGIDevice::GetAdapter", hr);
    }
    if (SUCCEEDED(hr)) {
        if (dxgiAdapter == nullptr) {
            hr = E_POINTER;
            FailGraphics(L"IDXGIAdapter::GetParent(IDXGIFactory2)", hr);
        }
        else {
            hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
            if (FAILED(hr))
                FailGraphics(L"IDXGIAdapter::GetParent(IDXGIFactory2)", hr);
        }
    }

    if (SUCCEEDED(hr)) {
        RECT clientRectSwapchain = {};
        ::GetClientRect(hwnd, &clientRectSwapchain);
        DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
        swapDesc.Width = (UINT)(clientRectSwapchain.right - clientRectSwapchain.left);
        swapDesc.Height = (UINT)(clientRectSwapchain.bottom - clientRectSwapchain.top);
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;

        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        if (swapDesc.Width == 0 || swapDesc.Height == 0) {
            hr = E_INVALIDARG;
            FailGraphics(L"SwapChain tamanho do cliente (0x0)", hr);
        }
        else {
            hr = dxgiFactory->CreateSwapChainForComposition(dxgiDevice, &swapDesc, nullptr, &g_SwapChain);
            if (FAILED(hr))
                FailGraphics(L"CreateSwapChainForComposition", hr);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = ::DCompositionCreateDevice(dxgiDevice, IID_PPV_ARGS(&g_DcompDevice));
        if (FAILED(hr))
            FailGraphics(L"DCompositionCreateDevice", hr);
    }
    if (SUCCEEDED(hr)) {
        hr = g_DcompDevice->CreateTargetForHwnd(hwnd, TRUE, &g_DcompTarget);
        if (FAILED(hr))
            FailGraphics(L"IDCompositionDevice::CreateTargetForHwnd", hr);
    }
    if (SUCCEEDED(hr)) {
        hr = g_DcompDevice->CreateVisual(&g_DcompVisual);
        if (FAILED(hr))
            FailGraphics(L"IDCompositionDevice::CreateVisual", hr);
    }
    if (SUCCEEDED(hr)) {
        hr = g_DcompVisual->SetContent(g_SwapChain);
        if (FAILED(hr))
            FailGraphics(L"IDCompositionVisual::SetContent", hr);
    }
    if (SUCCEEDED(hr)) {
        hr = g_DcompTarget->SetRoot(g_DcompVisual);
        if (FAILED(hr))
            FailGraphics(L"IDCompositionTarget::SetRoot", hr);
    }
    if (SUCCEEDED(hr)) {
        hr = g_DcompDevice->Commit();
        if (FAILED(hr))
            FailGraphics(L"IDCompositionDevice::Commit", hr);
    }

    if (dxgiDevice) dxgiDevice->Release();
    if (dxgiAdapter) dxgiAdapter->Release();
    if (dxgiFactory) dxgiFactory->Release();
    if (FAILED(hr))
        return false;

    ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ::UpdateWindow(hwnd);
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_DcompVisual) { g_DcompVisual->Release(); g_DcompVisual = nullptr; }
    if (g_DcompTarget) { g_DcompTarget->Release(); g_DcompTarget = nullptr; }
    if (g_DcompDevice) { g_DcompDevice->Release(); g_DcompDevice = nullptr; }
    if (g_SwapChain) { g_SwapChain->Release(); g_SwapChain = nullptr; }
    if (g_Context) { g_Context->Release(); g_Context = nullptr; }
    if (g_Device) { g_Device->Release(); g_Device = nullptr; }
}

static void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_Device->CreateRenderTargetView(backBuffer, nullptr, &g_RenderTargetView);
        backBuffer->Release();
    }
}

static void CleanupRenderTarget() { if (g_RenderTargetView) { g_RenderTargetView->Release(); g_RenderTargetView = nullptr; } }

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SETFOCUS:
        return 0;
    case WM_KILLFOCUS:
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ACTIVATE:
        return 0;
    case WM_ACTIVATEAPP:
        return 0;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}
