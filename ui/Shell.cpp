// QuartzMIDI's window: Win32, Direct3D 11 and Dear ImGui.
//
// ShellEngine owns PlaybackCore on its own command thread. Input injection
// must never run on the message-loop thread, or dragging the window stalls
// the injection syscall.

#include "SkinDraw.hpp"
#include "Fonts.hpp"
#include "Panels.hpp"
#include "NativeConnectInput.hpp"
#include "HotkeyNames.hpp"
#include "Bundle.hpp"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <d3d11.h>
#include <tchar.h>
#include <shellapi.h>
#include <dwmapi.h>
#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>

static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_context = nullptr;
static IDXGISwapChain*         g_swapChain = nullptr;
static ID3D11RenderTargetView* g_target = nullptr;
static float g_dpi = 1.f;
static shell::ShellEngine* g_engine = nullptr;
static shell::Panels* g_panels = nullptr;
// Monitor DPI times the theme's Size. Applies to the client area and window
// size; the non-client frame uses the monitor DPI alone.
static float UiScale() { return g_dpi * (g_panels ? g_panels->ActiveSkin().scale : 1.f); }

// Global hotkeys.
//
// WM_HOTKEY arrives on the message-loop thread, so handlers only enqueue an
// engine command and never touch the player or inject input.
namespace {
// Hotkey id = index in the snapshot's hotkeys + 1. Names come from the engine,
// which owns config.json; Registered::names records what was requested so a
// key that failed isn't retried every frame.
//
// Windows matches hotkeys against the modifiers this app injects (Shift for
// black keys, Alt for velocity, Ctrl for 88 keys), so while typing each key is
// also registered under every Shift/Ctrl/Alt combination. Alt+F4 is never taken.
constexpr int kModifierMixes = 8;   // bit 0 Shift, bit 1 Ctrl, bit 2 Alt
constexpr int kHotkeyIdStride = 32; // id = place + 1 + stride * mix
static_assert(shell::kHotkeys < kHotkeyIdStride);

// Media transport keys, registered in addition to the user's binds when the
// Settings switch is on. A media key the user bound explicitly registers
// first, so its registration here fails.
constexpr int kMediaIdBase = 0x1000;
constexpr std::array<UINT, 4> kMediaKeys{VK_MEDIA_PLAY_PAUSE, VK_MEDIA_PREV_TRACK, VK_MEDIA_NEXT_TRACK, VK_MEDIA_STOP};

struct Registered {
    std::array<bool, shell::kHotkeys> held{};
    std::array<uint8_t, shell::kHotkeys> mixes{};
    std::array<uint8_t, kMediaKeys.size()> mediaMixes{};
    std::array<std::string, shell::kHotkeys> names;
    bool typing = false;
    bool media = false;
    bool any = false;
};

void UnregisterHotkeys(HWND hwnd, Registered& done) {
    for (size_t i = 0; i < shell::kHotkeys; ++i)
        for (int mix = 0; mix < kModifierMixes; ++mix)
            if (done.mixes[i] & (1u << mix)) UnregisterHotKey(hwnd, static_cast<int>(i) + 1 + kHotkeyIdStride * mix);
    for (size_t i = 0; i < kMediaKeys.size(); ++i)
        for (int mix = 0; mix < kModifierMixes; ++mix)
            if (done.mediaMixes[i] & (1u << mix)) UnregisterHotKey(hwnd, kMediaIdBase + static_cast<int>(i) + kHotkeyIdStride * mix);
    done.held.fill(false);
    done.mixes.fill(0);
    done.mediaMixes.fill(0);
    done.any = false;
}

// A key owned by another application fails to register; that is reported
// through Registered::held, not treated as fatal.
void RegisterHotkeys(HWND hwnd, Registered& done, const std::array<std::string, shell::kHotkeys>& names, bool typing, bool media) {
    UnregisterHotkeys(hwnd, done);
    const auto modifiersOf = [](int mix) {
        return static_cast<UINT>(MOD_NOREPEAT | (mix & 1 ? MOD_SHIFT : 0) | (mix & 2 ? MOD_CONTROL : 0) | (mix & 4 ? MOD_ALT : 0));
    };
    for (size_t i = 0; i < shell::kHotkeys; ++i) {
        const int vk = shell::NameToVK(names[i]);
        for (int mix = 0; vk != 0 && mix < (typing ? kModifierMixes : 1); ++mix) {
            if (vk == VK_F4 && (mix & 4)) continue;
            if (RegisterHotKey(hwnd, static_cast<int>(i) + 1 + kHotkeyIdStride * mix, modifiersOf(mix), static_cast<UINT>(vk)))
                done.mixes[i] |= static_cast<uint8_t>(1u << mix);
        }
        done.held[i] = (done.mixes[i] & 1) != 0;
    }
    for (size_t i = 0; media && i < kMediaKeys.size(); ++i)
        for (int mix = 0; mix < (typing ? kModifierMixes : 1); ++mix)
            if (RegisterHotKey(hwnd, kMediaIdBase + static_cast<int>(i) + kHotkeyIdStride * mix, modifiersOf(mix), kMediaKeys[i]))
                done.mediaMixes[i] |= static_cast<uint8_t>(1u << mix);
    done.names = names;
    done.typing = typing;
    done.media = media;
    done.any = true;
}
}


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// Set on DEVICE_REMOVED/RESET; the frame loop rebuilds the device.
static bool g_deviceLost = false;

static void CreateTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_target);
        back->Release();
    }
}

static void ReleaseTarget() {
    if (g_target) { g_target->Release(); g_target = nullptr; }
}

static bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL wanted[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted, 2,
        D3D11_SDK_VERSION, &desc, &g_swapChain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, wanted, 2,
            D3D11_SDK_VERSION, &desc, &g_swapChain, &g_device, &level, &g_context);
    }
    if (FAILED(hr)) return false;
    CreateTarget();
    return true;
}

static void CleanupDevice() {
    ReleaseTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_HOTKEY: {
        if (!g_engine) return 0;
        // Only enqueue. The engine worker owns the player.
        const uint64_t generation = g_engine->Snapshot()->generation;
        using Action = shell::ShellEngine::Action;
        // Performer keys are registered only to keep them from the game; the
        // engine polls their state, so they have no action here.
        static constexpr std::array<Action, 6> actions{
            Action::TogglePlayPause, Action::Back10, Action::Forward10, Action::Stop, Action::Previous, Action::Next};
        // id % kHotkeyIdStride is the hotkey index; the rest encodes the modifier mix.
        static constexpr std::array<Action, kMediaKeys.size()> media{
            Action::TogglePlayPause, Action::Previous, Action::Next, Action::Stop};
        const size_t place = wp % kHotkeyIdStride;
        if (wp >= kMediaIdBase) { if (place < media.size()) g_engine->Send({media[place], {}, generation}); }
        else if (place >= 1 && place <= actions.size()) g_engine->Send({actions[place - 1], {}, generation});
        return 0;
    }
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wp);
        wchar_t path[32768]{};
        if (g_engine && g_panels && DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path))))
            g_engine->Send({shell::ShellEngine::Action::Load, path, 0, 0, g_panels->preferences.autoSolo});
        DragFinish(drop);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = static_cast<float>(HIWORD(wp)) / 96.f;
        const RECT& rect = *reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left,
                     rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        const bool mini = g_panels && g_panels->miniMode;
        if (!g_panels) break;
        ImVec2 minimum = mini ? g_panels->DesiredSize() : g_panels->MinimumSize();
        // With Tracks closed the full height can be below MinimumSize.
        if (!mini) minimum.y = std::min(minimum.y, g_panels->FullHeight());
        RECT rect{0, 0, static_cast<LONG>(minimum.x * UiScale()), static_cast<LONG>(minimum.y * UiScale())};
        AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0,
                                static_cast<UINT>(96.f * g_dpi));
        info->ptMinTrackSize = {rect.right - rect.left, rect.bottom - rect.top};
        // Mini has a fixed size.
        if (mini) info->ptMaxTrackSize = info->ptMinTrackSize;
        return 0;
    }
    case WM_SIZE:
        if (g_device && wp != SIZE_MINIMIZED) {
            ReleaseTarget();
            const HRESULT resized = g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lp), (UINT)HIWORD(lp),
                                                               DXGI_FORMAT_UNKNOWN, 0);
            if (resized == DXGI_ERROR_DEVICE_REMOVED || resized == DXGI_ERROR_DEVICE_RESET) g_deviceLost = true;
            else CreateTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // no ALT menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Match the DWM caption to the skin. Windows 11 honours the colours; Windows 10
// 20H1+ honours only dark mode; older builds ignore both.
void ApplyCaption(HWND hwnd, const skin::Skin& s) {
    const BOOL dark = s.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const auto colour = [](skin::Argb argb) -> COLORREF { return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF); };
    const COLORREF caption = colour(s.surface.structure), text = colour(s.ink.primary);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &caption, sizeof(caption));
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto& directory = shell::bundle::DataDirectory();
    // The single-exe build writes its add-ons, config and licences into its
    // data folder; --unpack does only that and exits.
    if (const auto bundled = shell::bundle::FromResource(inst); !bundled.empty()) shell::bundle::Unpack(bundled, directory);
    {
        int count = 0;
        LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        const bool unpackOnly = args && count > 1 && std::wstring_view(args[1]) == L"--unpack";
        if (args) LocalFree(args);
        if (unpackOnly) return 0;
    }
    const auto preferencesPath = directory / L"shell-settings.json";
    shell::Panels panels;
    panels.LoadPreferences(preferencesPath);
    shell::CaptureShellLog captureLog;
    shell::ShellEngine engine(directory / L"config.json", {}, false,
        [] { return std::make_unique<shell::NativeConnectInput>(); });
    g_engine = &engine;
    g_panels = &panels;
    g_dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{100, 100}, MONITOR_DEFAULTTOPRIMARY));
    // Icon resource 1 in Shell.rc.
    HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst,
                       icon, nullptr, nullptr, nullptr, L"QuartzMIDI", icon };
    ::RegisterClassExW(&wc);
    const ImVec2 desired = panels.DesiredSize();
    RECT initial{0, 0, static_cast<LONG>(desired.x * UiScale()), static_cast<LONG>(desired.y * UiScale())};
    AdjustWindowRectExForDpi(&initial, WS_OVERLAPPEDWINDOW, FALSE, 0, static_cast<UINT>(96.f * g_dpi));
    // Restore the saved position if it is still on a monitor.
    int startX = 100, startY = 100, startWidth = initial.right - initial.left;
    if (const auto& saved = panels.preferences; saved.windowWidth > 0) {
        const RECT last{saved.windowX, saved.windowY, saved.windowX + saved.windowWidth, saved.windowY + 100};
        if (MonitorFromRect(&last, MONITOR_DEFAULTTONULL)) {
            startX = saved.windowX; startY = saved.windowY;
            startWidth = std::max(startWidth, saved.windowWidth);
        }
    }
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"QuartzMIDI",
                                WS_OVERLAPPEDWINDOW, startX, startY, startWidth, initial.bottom - initial.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    ApplyCaption(hwnd, panels.ActiveSkin());  // before the first paint to avoid a white flash
    if (!CreateDevice(hwnd)) {
        CleanupDevice();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);
    DragAcceptFiles(hwnd, TRUE);
    // Registered from the loop because RegisterHotKey binds to the thread that
    // owns hwnd; re-registered whenever the snapshot's hotkeys change.
    Registered hotkeys;
    shell::HotkeyCapture capture;
    bool capturing = false;
    // Key that ended a capture, swallowed until released so its auto-repeat
    // doesn't reach the focused control.
    WPARAM capturedKey = 0;
    const auto keyDown = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    shell::Fonts fonts;
    fonts.Load();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // SkinSignature of the applied style. A theme being edited keeps its name,
    // so compare by colours, not name or index.
    uint64_t appliedSkin = 0;
    float appliedDpi = 0.f;
    bool appliedMini = false, appliedMiniAutoplay = false;
    // Last applied layout, in theme pixels: {full height, minimum width, mini
    // width, Size}. Any change triggers a window resize.
    const auto layoutNow = [&] {
        const float scale = panels.ActiveSkin().scale;
        return std::array<float, 4>{panels.DesiredSize().y * scale, panels.MinimumSize().x * scale,
                                    panels.miniMode ? panels.DesiredSize().x * scale : 0.f, scale};
    };
    std::array<float, 4> appliedLayout = layoutNow();
    RECT fullRect{}; GetWindowRect(hwnd, &fullRect);
    bool fullMaximized = false;
    panels.miniMode = panels.preferences.startMini;
    ULONGLONG preferencesSaved = GetTickCount64();
    if (panels.preferences.folder.empty()) {
        auto folder = directory / L"midi";
        if (!std::filesystem::is_directory(folder)) folder = directory.parent_path().parent_path() / L"x64" / L"Release" / L"midi";
        if (std::filesystem::is_directory(folder)) panels.preferences.folder = std::filesystem::weakly_canonical(folder);
    }
    if (!panels.preferences.folder.empty()) engine.Send({shell::ShellEngine::Action::Scan, panels.preferences.folder});
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1) engine.Send({shell::ShellEngine::Action::Load, argv[1], 0, 0, panels.preferences.autoSolo});
        LocalFree(argv);
    }

    bool running = true;
    bool appliedTopmost = false;
    int appliedOpacity = 100;
    // Render on demand: on input, a new engine snapshot (the engine posts
    // WM_NULL), or a size/skin/scale change, then for a 750 ms tail so tooltip
    // delays and popups can finish. An idle window presents nothing.
    engine.SetWakeWindow(hwnd);
    struct WakeGuard { shell::ShellEngine& engine; ~WakeGuard() { engine.SetWakeWindow(nullptr); } } wakeGuard{engine};
    std::shared_ptr<const shell::EngineSnapshot> drawnSnapshot;
    auto drawUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool occluded = false;
    bool rendererUp = true;
    while (running) {
        MSG msg;
        bool input = false;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            // Swallow key-downs during a rebind capture so they don't reach
            // ImGui. Filtered here because Settings can be its own OS window.
            const bool keyDownMessage = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
            if ((msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) && msg.wParam == capturedKey) capturedKey = 0;
            if (keyDownMessage && (capturing || (capturedKey != 0 && msg.wParam == capturedKey))) { input = true; continue; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
            input = true;
        }
        if (!running) break;
        {
            DWORD foreground = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
            // GetAsyncKeyState is global, so cancel a capture when another
            // process takes the foreground.
            if (panels.hotkeyCapture >= 0 && foreground != GetCurrentProcessId()) panels.hotkeyCapture = -1;
            if (capturedKey != 0 && !keyDown(static_cast<int>(capturedKey))) capturedKey = 0;
            if (panels.hotkeyCapture >= 0) {
                if (!capturing) { UnregisterHotkeys(hwnd, hotkeys); capture.Begin(keyDown); capturing = true; }
                const int pressed = capture.Poll(keyDown);
                if (pressed > 0) {
                    shell::ShellEngine::Command command{shell::ShellEngine::Action::Hotkey};
                    command.track = static_cast<size_t>(panels.hotkeyCapture);
                    command.key = shell::VKToName(pressed);
                    engine.Send(std::move(command));
                }
                if (pressed != shell::HotkeyCapture::None) {
                    capturedKey = pressed > 0 ? static_cast<WPARAM>(pressed) : VK_ESCAPE;
                    panels.hotkeyCapture = -1;
                }
            }
            if (panels.hotkeyCapture < 0) capturing = false;
            // Don't re-register until the captured key is released, or its
            // auto-repeat fires the action just bound to it.
            if (capturedKey != 0) drawUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
            else if (!capturing) {
                const auto snapshot = engine.Snapshot();
                // Performer keys are registered only while their trigger is
                // active. An inactive key is not reported as unavailable.
                auto wanted = snapshot->hotkeys;
                std::array<bool, shell::kHotkeys> resting{};
                for (size_t i = shell::kAppHotkeys; i < shell::kHotkeys; ++i)
                    resting[i] = snapshot->trigger == 0 || snapshot->performer.TriggerOfKey(i) != snapshot->trigger;
                for (size_t i = 0; i < shell::kHotkeys; ++i) if (resting[i]) wanted[i].clear();
                const bool typing = snapshot->playing || snapshot->playbackCountdown > 0 ||
                                    snapshot->liveActive || snapshot->midiConnect;
                const bool media = panels.preferences.mediaKeys;
                if (!hotkeys.any || wanted != hotkeys.names || typing != hotkeys.typing || media != hotkeys.media) {
                    RegisterHotkeys(hwnd, hotkeys, wanted, typing, media);
                    panels.stopHotkeyAvailable = hotkeys.held[3];
                    for (size_t i = 0; i < shell::kHotkeys; ++i) {
                        panels.transportKeysAvailable[i] = hotkeys.held[i] || resting[i];
                        panels.transportKeys[i] = shell::HotkeyLabel(snapshot->hotkeys[i]);
                    }
                    drawUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
                }
            }
        }
        if (appliedOpacity != panels.preferences.opacity) {
            // WS_EX_LAYERED only while translucent; it costs a composition
            // pass on every present.
            const auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            const bool opaque = panels.preferences.opacity >= 100;
            bool applied = true;
            if (!opaque) {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED);
                applied = SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(panels.preferences.opacity * 255 / 100), LWA_ALPHA) != FALSE;
            } else if (style & WS_EX_LAYERED) {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
                RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
            }
            if (applied) appliedOpacity = panels.preferences.opacity;
            else {
                panels.preferences.opacity = appliedOpacity;
                shell::ShellLog::Instance().Append("[error] Could not change window opacity.\n");
            }
        }
        if (appliedTopmost != panels.preferences.alwaysOnTop) {
            if (SetWindowPos(hwnd, panels.preferences.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
                appliedTopmost = panels.preferences.alwaysOnTop;
            else panels.preferences.alwaysOnTop = appliedTopmost;
        }
        if (IsIconic(hwnd)) { WaitMessage(); continue; }
        const skin::Skin current = panels.ActiveSkin();
        const uint64_t active = shell::SkinSignature(current);
        {
            const auto now = std::chrono::steady_clock::now();
            auto snapshot = engine.Snapshot();
            const bool pending = appliedMini != panels.miniMode || appliedLayout != layoutNow() ||
                (panels.miniMode && appliedMiniAutoplay != panels.miniAutoplay) || appliedSkin != active || appliedDpi != UiScale();
            // Keep the drawn snapshot alive: a freed snapshot's address can be
            // reused by the next one, so a raw-pointer compare would miss it.
            if (input || pending || snapshot != drawnSnapshot || panels.Animating() || snapshot->converting || snapshot->busy ||
                (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) || g_deviceLost)
                drawUntil = now + std::chrono::milliseconds(750);
            if (now >= drawUntil) {
                MsgWaitForMultipleObjectsEx(0, nullptr, 500, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                continue;
            }
            drawnSnapshot = std::move(snapshot);
        }
        // Rebuild the device after a driver reset or adapter removal.
        if (g_deviceLost) {
            // Restart both backends: shutting down the renderer destroys the
            // platform windows, clearing the main viewport's Win32 handle,
            // which the next NewFrame asserts on.
            if (rendererUp) { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); rendererUp = false; }
            CleanupDevice();
            if (!CreateDevice(hwnd)) {
                // The adapter may not be back yet; retry in a second.
                MsgWaitForMultipleObjectsEx(0, nullptr, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                continue;
            }
            ImGui_ImplWin32_Init(hwnd);
            ImGui_ImplDX11_Init(g_device, g_context);
            rendererUp = true;
            g_deviceLost = false;
            shell::ShellLog::Instance().Append("The graphics device was reset and has been rebuilt.\n");
        }
        // While occluded (covered or locked), poll with DXGI_PRESENT_TEST instead
        // of drawing, unless another viewport window may still be visible.
        if (occluded && ImGui::GetPlatformIO().Viewports.Size <= 1 &&
            g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        occluded = false;
        const bool modeChanged = appliedMini != panels.miniMode;
        const bool sizeChanged = modeChanged || appliedLayout != layoutNow() ||
            (panels.miniMode && appliedMiniAutoplay != panels.miniAutoplay);
        // Leave a maximized full window alone: SetWindowPos on it leaves the
        // window flagged maximized at a non-maximized size.
        if (sizeChanged && !modeChanged && !panels.miniMode && IsZoomed(hwnd)) appliedLayout = layoutNow();
        else if (sizeChanged) {
            if (modeChanged) {
                // Mini has no maximize box. A maximized full window is
                // restored on entering mini and re-maximized on leaving it.
                if (panels.miniMode) { fullMaximized = IsZoomed(hwnd) != FALSE; if (fullMaximized) ShowWindow(hwnd, SW_RESTORE); }
                const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
                SetWindowLongPtrW(hwnd, GWL_STYLE, panels.miniMode ? style & ~WS_MAXIMIZEBOX : style | WS_MAXIMIZEBOX);
            }
            RECT window{}, client{}; GetWindowRect(hwnd, &window); GetClientRect(hwnd, &client);
            if (modeChanged && panels.miniMode) fullRect = window;
            RECT target{};
            const auto desired = panels.DesiredSize();
            if (modeChanged && !panels.miniMode) target = fullRect;
            else {
                // The full window keeps its user-set width, rescaled by the
                // change in Size; WM_GETMINMAXINFO enforces the floor.
                const float ratio = layoutNow()[3] / appliedLayout[3];
                const float width = panels.miniMode ? desired.x * UiScale() : client.right * ratio;
                // Likewise any height dragged beyond the desired height.
                const float extra = panels.miniMode || modeChanged ? 0.f : std::max(0.f, client.bottom - appliedLayout[0] * g_dpi);
                RECT dimensions{0, 0, static_cast<LONG>(width), static_cast<LONG>(desired.y * UiScale() + extra * ratio)};
                AdjustWindowRectExForDpi(&dimensions, WS_OVERLAPPEDWINDOW, FALSE, 0, static_cast<UINT>(96.f * g_dpi));
                target = {window.left, window.top, window.left + dimensions.right - dimensions.left,
                    window.top + dimensions.bottom - dimensions.top};
            }
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
            const LONG width = std::min(target.right - target.left, monitor.rcWork.right - monitor.rcWork.left);
            const LONG height = std::min(target.bottom - target.top, monitor.rcWork.bottom - monitor.rcWork.top);
            target.left = std::clamp(target.left, monitor.rcWork.left, monitor.rcWork.right - width);
            target.top = std::clamp(target.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
            SetWindowPos(hwnd, nullptr, target.left, target.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            if (modeChanged && !panels.miniMode && fullMaximized) ShowWindow(hwnd, SW_MAXIMIZE);
            appliedMini = panels.miniMode;
            appliedLayout = layoutNow();
            appliedMiniAutoplay = panels.miniAutoplay;
        }
        if (appliedSkin != active || appliedDpi != UiScale()) {
            skin::ApplyStyle(current, UiScale());
            ApplyCaption(hwnd, current);
            ImGui::GetIO().FontDefault = fonts.Get(current);
            appliedSkin = active;
            appliedDpi = UiScale();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(fonts.Get(current), current.type.body);

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::Begin("##shell", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        panels.Draw(hwnd, fonts, current, UiScale(), engine);
        ImGui::End();
        ImGui::PopFont();

        ImGui::Render();
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        const float clear[4] = { bg.x, bg.y, bg.z, 1.f };
        if (g_target) {
            g_context->OMSetRenderTargets(1, &g_target, nullptr);
            g_context->ClearRenderTargetView(g_target, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        const HRESULT presented = g_swapChain->Present(1, 0);
        if (presented == DXGI_STATUS_OCCLUDED) occluded = true;
        else if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET || !g_target) g_deviceLost = true;
        // Save preferences every second so a crash or shutdown doesn't lose them.
        if (const auto now = GetTickCount64(); now - preferencesSaved >= 1000) {
            preferencesSaved = now;
            RECT window{};
            if (!panels.miniMode && !IsZoomed(hwnd) && !IsIconic(hwnd) && GetWindowRect(hwnd, &window)) {
                panels.preferences.windowX = window.left;
                panels.preferences.windowY = window.top;
                panels.preferences.windowWidth = window.right - window.left;
            }
            panels.SavePreferences(preferencesPath, false);
        }
        // Throttle redraws to ~12 fps when unfocused; playback doesn't depend on
        // frame rate. "Focused" means any window of this process, since viewports
        // such as Key Mapping are separate top-level windows.
        DWORD foregroundProcess = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        const bool ours = foregroundProcess == GetCurrentProcessId();
        MsgWaitForMultipleObjectsEx(0, nullptr, ours ? 16 : 80,
                                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if (rendererUp) { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); }
    ImGui::DestroyContext();
    UnregisterHotkeys(hwnd, hotkeys);
    panels.SavePreferences(preferencesPath);
    g_engine = nullptr;
    g_panels = nullptr;
    CleanupDevice();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
