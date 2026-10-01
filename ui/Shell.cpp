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
#include "WootingAnalog.hpp"
#include "Bundle.hpp"
#include "WindowCapture.hpp"
#include "MiniFocus.hpp"
#include "KeyTest.hpp"
#include "CrashGuard.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <d3d11.h>
#include <tchar.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <dbt.h>
#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
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
// also registered under every Shift/Ctrl/Alt combination. Alt+F4 is taken only
// with the Block Alt+F4 switch on.
constexpr int kModifierMixes = 8;   // bit 0 Shift, bit 1 Ctrl, bit 2 Alt
constexpr int kHotkeyIdStride = 32; // id = place + 1 + stride * mix
static_assert(shell::kHotkeys < kHotkeyIdStride);

// Media transport keys, registered in addition to the user's binds when the
// Settings switch is on. A media key the user bound explicitly registers
// first, so its registration here fails.
constexpr int kMediaIdBase = 0x1000;
constexpr std::array<UINT, 4> kMediaKeys{VK_MEDIA_PLAY_PAUSE, VK_MEDIA_PREV_TRACK, VK_MEDIA_NEXT_TRACK, VK_MEDIA_STOP};

// A song's own key: id = base + song * kModifierMixes + mix, below 0xC000.
constexpr int kSongIdBase = 0x2000;
constexpr size_t kMostSongHotkeys = (0xC000 - kSongIdBase) / kModifierMixes;

struct Registered {
    std::array<bool, shell::kHotkeys> held{};
    std::array<uint8_t, shell::kHotkeys> mixes{};
    std::array<uint8_t, kMediaKeys.size()> mediaMixes{};
    std::array<std::string, shell::kHotkeys> names;
    std::vector<shell::SongHotkey> songs;
    std::vector<uint8_t> songMixes;
    std::vector<bool> songHeld;
    // Hotkey bound to each mouse side button, kHotkeys and on for a song's, or
    // -1. RegisterHotKey cannot take a mouse button, so while one is bound the
    // window reads the mouse as raw
    // input, in the background too, and fires on the button's press. Whatever
    // the modifiers are, as a key registered while typing does. Unlike a key,
    // the press still reaches the game.
    std::array<int, 2> mouse{-1, -1};
    bool rawMouse = false;
    // Keys another program registered first, read as raw keyboard input: each
    // one's virtual key and hotkey, kHotkeys and on for a song's. Like a mouse
    // button, such a key still reaches the game.
    std::vector<std::pair<int, size_t>> rawKeys;
    bool rawKeyboard = false;
    bool typing = false;
    bool media = false;
    bool blockAltF4 = false;
    // The layout and key map the keys were checked against: a key the app types
    // under them is left out, and a change checks every key again.
    HKL layout = nullptr;
    uint64_t mappingRevision = 0;
    bool any = false;
};
// What WM_INPUT fires, owned by the message loop's thread like WM_HOTKEY.
const Registered* g_registered = nullptr;
shell::RawKeyEdges g_rawKeys;

bool ReadRawMouse(HWND hwnd, bool on) {
    const RAWINPUTDEVICE mouse{0x01, 0x02, static_cast<DWORD>(on ? RIDEV_INPUTSINK : RIDEV_REMOVE), on ? hwnd : nullptr};
    return RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
}
bool ReadRawKeyboard(HWND hwnd, bool on) {
    const RAWINPUTDEVICE keyboard{0x01, 0x06, static_cast<DWORD>(on ? RIDEV_INPUTSINK : RIDEV_REMOVE), on ? hwnd : nullptr};
    return RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard)) != FALSE;
}

void UnregisterHotkeys(HWND hwnd, Registered& done) {
    for (size_t i = 0; i < shell::kHotkeys; ++i)
        for (int mix = 0; mix < kModifierMixes; ++mix)
            if (done.mixes[i] & (1u << mix)) UnregisterHotKey(hwnd, static_cast<int>(i) + 1 + kHotkeyIdStride * mix);
    for (size_t i = 0; i < kMediaKeys.size(); ++i)
        for (int mix = 0; mix < kModifierMixes; ++mix)
            if (done.mediaMixes[i] & (1u << mix)) UnregisterHotKey(hwnd, kMediaIdBase + static_cast<int>(i) + kHotkeyIdStride * mix);
    for (size_t i = 0; i < done.songMixes.size(); ++i)
        for (int mix = 0; mix < kModifierMixes; ++mix)
            if (done.songMixes[i] & (1u << mix)) UnregisterHotKey(hwnd, kSongIdBase + static_cast<int>(i) * kModifierMixes + mix);
    done.songMixes.clear();
    done.songHeld.clear();
    done.songs.clear();
    if (done.rawMouse) ReadRawMouse(hwnd, false);
    done.rawMouse = false;
    if (done.rawKeyboard) ReadRawKeyboard(hwnd, false);
    done.rawKeyboard = false;
    done.rawKeys.clear();
    done.mouse.fill(-1);
    done.held.fill(false);
    done.mixes.fill(0);
    done.mediaMixes.fill(0);
    done.any = false;
}

// A key owned by another application fails to register and is read as raw
// input instead; one that cannot be read either is reported through
// Registered::held, not treated as fatal.
void RegisterHotkeys(HWND hwnd, Registered& done, const std::array<std::string, shell::kHotkeys>& names,
                     const std::vector<shell::SongHotkey>& songs, bool typing, bool media, bool blockAltF4,
                     const std::map<std::string, std::string>& keyMappings, uint64_t mappingRevision) {
    UnregisterHotkeys(hwnd, done);
    const HKL layout = shell::UiLayout();
    const auto typed = [&](int vk) { return shell::IsNoteKeyOn(vk, keyMappings, layout); };
    const auto modifiersOf = [](int mix) {
        return static_cast<UINT>(MOD_NOREPEAT | (mix & 1 ? MOD_SHIFT : 0) | (mix & 2 ? MOD_CONTROL : 0) | (mix & 4 ? MOD_ALT : 0));
    };
    // Registers `vk` under id base + stride * mix; true when it went in unmodified.
    const auto take = [&](int vk, int base, int stride, uint8_t& mixes) {
        for (int mix = 0; vk != 0 && mix < (typing ? kModifierMixes : 1); ++mix) {
            if (!shell::RegistersMix(vk, mix, blockAltF4)) continue;
            if (RegisterHotKey(hwnd, base + stride * mix, modifiersOf(mix), static_cast<UINT>(vk))) mixes |= static_cast<uint8_t>(1u << mix);
        }
        return (mixes & 1) != 0;
    };
    for (size_t i = 0; i < shell::kHotkeys; ++i) {
        const int vk = shell::RegisteredVK(names[i], typed);
        if (shell::IsMouseHotkey(vk)) { done.mouse[vk == VK_XBUTTON1 ? 0 : 1] = static_cast<int>(i); continue; }
        done.held[i] = take(vk, static_cast<int>(i) + 1, kHotkeyIdStride, done.mixes[i]);
        if (vk != 0 && !done.held[i]) done.rawKeys.push_back({vk, i});
    }
    done.songs = songs;
    done.songMixes.assign(songs.size(), 0);
    done.songHeld.assign(songs.size(), false);
    for (size_t i = 0; i < songs.size() && i < kMostSongHotkeys; ++i) {
        const int vk = shell::RegisteredVK(songs[i].key, typed);
        if (shell::IsMouseHotkey(vk)) { done.mouse[vk == VK_XBUTTON1 ? 0 : 1] = static_cast<int>(shell::kHotkeys + i); continue; }
        done.songHeld[i] = take(vk, kSongIdBase + static_cast<int>(i) * kModifierMixes, 1, done.songMixes[i]);
        if (vk != 0 && !done.songHeld[i]) done.rawKeys.push_back({vk, shell::kHotkeys + i});
    }
    for (size_t i = 0; media && i < kMediaKeys.size(); ++i)
        for (int mix = 0; mix < (typing ? kModifierMixes : 1); ++mix)
            if (RegisterHotKey(hwnd, kMediaIdBase + static_cast<int>(i) + kHotkeyIdStride * mix, modifiersOf(mix), kMediaKeys[i]))
                done.mediaMixes[i] |= static_cast<uint8_t>(1u << mix);
    if (done.mouse[0] >= 0 || done.mouse[1] >= 0) {
        done.rawMouse = ReadRawMouse(hwnd, true);
        for (const int bound : done.mouse) {
            if (bound < 0) continue;
            if (static_cast<size_t>(bound) < shell::kHotkeys) done.held[static_cast<size_t>(bound)] = done.rawMouse;
            else done.songHeld[static_cast<size_t>(bound) - shell::kHotkeys] = done.rawMouse;
        }
    }
    if (!done.rawKeys.empty()) {
        g_rawKeys.Start([](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; });
        done.rawKeyboard = ReadRawKeyboard(hwnd, true);
        for (const auto& [vk, bound] : done.rawKeys) {
            if (bound < shell::kHotkeys) done.held[bound] = done.rawKeyboard;
            else done.songHeld[bound - shell::kHotkeys] = done.rawKeyboard;
        }
    }
    done.names = names;
    done.typing = typing;
    done.media = media;
    done.blockAltF4 = blockAltF4;
    done.layout = layout;
    done.mappingRevision = mappingRevision;
    done.any = true;
}

// The main window, for the show/hide key.
HWND g_window = nullptr;
// ImGui's own windows hidden with the main window, shown again with it.
std::vector<HWND> g_hiddenViewports;

bool OfThisProcess(HWND window) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}

// Brings the full window in front with the keyboard. Windows lets a program
// take the foreground after its own input, as a hotkey is (WM_HOTKEY), but not
// after raw input, which a mouse side button and a key another program took
// are read as; joined to the input of the thread in front, the app may. No key
// is sent for it: the game reads Alt as velocity. Refused all the same, the
// window still comes above the one in front, without the keyboard.
void TakeForeground(HWND hwnd) {
    const HWND front = GetForegroundWindow();
    if (front == hwnd) return;
    const DWORD self = GetCurrentThreadId(), frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0;
    const bool joined = frontThread != 0 && frontThread != self && AttachThreadInput(self, frontThread, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (joined) AttachThreadInput(self, frontThread, FALSE);
    if (GetForegroundWindow() != hwnd && !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// The show/hide key (shell::ShowHideMove): a window in front hides, with
// ImGui's windows of its own; one the game covers comes in front; a hidden or
// minimized one comes back in front. In front is the app's own window having
// the keyboard, or nothing another program shows covering it.
void ShowOrHide(HWND hwnd) {
    const bool mini = g_panels && g_panels->miniMode;
    const bool onScreen = IsWindowVisible(hwnd) && !IsIconic(hwnd);
    const HWND front = GetForegroundWindow();
    const bool inFront = onScreen && ((front && OfThisProcess(front)) || !shell::Covered(hwnd, OfThisProcess));
    switch (shell::ShowHideMove(onScreen, inFront)) {
    case shell::ShowHide::Raise:
        // Mini comes in front without taking the keyboard from the game, as a click on it does.
        if (mini) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        else TakeForeground(hwnd);
        return;
    case shell::ShowHide::Hide: {
        g_hiddenViewports.clear();
        if (ImGui::GetCurrentContext())
            for (const auto* viewport : ImGui::GetPlatformIO().Viewports)
                if (const HWND own = static_cast<HWND>(viewport->PlatformHandle); own && own != hwnd && IsWindowVisible(own)) {
                    ShowWindow(own, SW_HIDE);
                    g_hiddenViewports.push_back(own);
                }
        ShowWindow(hwnd, SW_HIDE);
        return;
    }
    case shell::ShowHide::Show:
        // Mini comes back without taking the keyboard from the game, as a click on it does.
        ShowWindow(hwnd, IsIconic(hwnd) ? (mini ? SW_SHOWNOACTIVATE : SW_RESTORE) : (mini ? SW_SHOWNA : SW_SHOW));
        for (const HWND own : std::exchange(g_hiddenViewports, {})) if (IsWindow(own)) ShowWindow(own, SW_SHOWNA);
        if (!mini) TakeForeground(hwnd);
        return;
    }
}

// No taskbar button and out of Alt+Tab: the window is owned by a hidden tool
// window, which both pass over, so its caption and size stay as they are, and
// ImGui's windows of its own carry no button either. The owner is never shown,
// and takes the capture affinity with the app's other windows.
HWND g_taskbarOwner = nullptr;
void HideFromTaskbar(HWND hwnd, bool hide) {
    HWND& owner = g_taskbarOwner;
    if (hide && !owner) {
        WNDCLASSEXW ownerClass{sizeof(ownerClass)};
        ownerClass.lpfnWndProc = DefWindowProcW;
        ownerClass.hInstance = GetModuleHandleW(nullptr);
        ownerClass.lpszClassName = L"QuartzMIDI.Owner";
        RegisterClassExW(&ownerClass);
        owner = CreateWindowExW(WS_EX_TOOLWINDOW, ownerClass.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, ownerClass.hInstance, nullptr);
    }
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, hide ? reinterpret_cast<LONG_PTR>(owner) : 0);
    ImGui::GetIO().ConfigViewportsNoTaskBarIcon = hide;
    // The taskbar reads the owner when a window is shown; one on screen now is told.
    if (!IsWindowVisible(hwnd)) return;
    ITaskbarList* taskbar = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar)))) {
        if (SUCCEEDED(taskbar->HrInit())) hide ? taskbar->DeleteTab(hwnd) : taskbar->AddTab(hwnd);
        taskbar->Release();
    }
}

// Sends the action of the hotkey at `index`. Only enqueues: the engine worker
// owns the player. Performer keys are registered only to keep them from the
// game; the engine polls their state, so they have no action here.
void FireHotkey(size_t index) {
    if (!g_engine) return;
    using Action = shell::ShellEngine::Action;
    static constexpr std::array<Action, shell::kAppHotkeys> actions{
        Action::TogglePlayPause, Action::Back10, Action::Forward10, Action::Stop, Action::Previous, Action::Next};
    const uint64_t generation = g_engine->Snapshot()->generation;
    if (index < actions.size()) g_engine->Send({actions[index], {}, generation});
    else if (index == shell::kPanicHotkey) g_engine->Send({Action::Panic});
    // A performer's action key, which works whatever the trigger.
    else if (const int action = g_engine->Snapshot()->performer.ActionOfKey(index); action >= 0)
        g_engine->Send({Action::PerformerAction, {}, generation, static_cast<size_t>(action)});
    else if (index == shell::kSpeedUpHotkey || index == shell::kSpeedDownHotkey)
        g_engine->Send({Action::SpeedStep, {}, 0, 0, false, index == shell::kSpeedUpHotkey ? 1.0 : -1.0});
    else if (index == shell::kShowHideHotkey) { if (g_window) ShowOrHide(g_window); }
    // A song's key, kHotkeys and on as the mouse buttons number them.
    else if (index >= shell::kHotkeys && g_registered && index - shell::kHotkeys < g_registered->songs.size())
        g_engine->Send({Action::PlaySong, g_registered->songs[index - shell::kHotkeys].song});
}
}


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// Set on DEVICE_REMOVED/RESET; the frame loop rebuilds the device.
static bool g_deviceLost = false;
// While mini is shown, a click on any window of the app leaves the keyboard with
// the game, and a window opens without taking it. The frame loop gives a window
// the keyboard only while a control in it reads keys.
static bool g_noActivate = false;

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

// Paths dropped or handed over, opened by the frame loop between frames: a
// modal dialog open inside a frame dispatches those messages too.
static std::vector<std::filesystem::path> g_opened;

// A path dropped on the window, given on the command line or handed over by a
// second start: a folder becomes the MIDI Files list's folder, a theme file is
// imported, and anything else opens as a row click opens a song. True when a
// song was sent to load.
static bool OpenPath(std::filesystem::path path) {
    if (!g_engine || !g_panels || path.empty()) return false;
    std::error_code error;
    if (auto full = std::filesystem::absolute(path, error); !error) path = std::move(full);
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (std::filesystem::is_directory(path, error)) { g_panels->OpenFolder(path, *g_engine); return false; }
    if (extension == L".qmtheme") { g_panels->ImportTheme(path); return false; }
    g_engine->Send({shell::ShellEngine::Action::Load, path, 0, 0, g_panels->preferences.autoSolo});
    return true;
}

// Settings' key test sends F24, which nothing uses, tagged so the loop knows it
// arrived and keeps it from every control.
static constexpr ULONG_PTR kKeyTestTag = 0x514D4B54;
static constexpr ULONGLONG kKeyTestWaitMs = 500;

// A device arriving or leaving sends a burst of WM_DEVICECHANGE; MIDI devices
// are rescanned once it has been quiet this long.
static constexpr UINT_PTR kDeviceScanTimer = 1;
static constexpr UINT kDeviceScanSettleMs = 500;

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_HOTKEY: {
        if (!g_engine) return 0;
        using Action = shell::ShellEngine::Action;
        // id % kHotkeyIdStride is the hotkey index; the rest encodes the modifier mix.
        static constexpr std::array<Action, kMediaKeys.size()> media{
            Action::TogglePlayPause, Action::Previous, Action::Next, Action::Stop};
        const size_t place = wp % kHotkeyIdStride;
        if (wp >= kSongIdBase) FireHotkey(shell::kHotkeys + (wp - kSongIdBase) / kModifierMixes);
        else if (wp >= kMediaIdBase) { if (place < media.size()) g_engine->Send({media[place], {}, g_engine->Snapshot()->generation}); }
        else if (place >= 1) FireHotkey(place - 1);
        return 0;
    }
    case WM_INPUT: {
        RAWINPUT input{};
        UINT size = sizeof(input);
        if (g_registered && GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
            input.header.dwType == RIM_TYPEMOUSE) {
            const USHORT buttons = input.data.mouse.usButtonFlags;
            if ((buttons & RI_MOUSE_BUTTON_4_DOWN) && g_registered->mouse[0] >= 0) FireHotkey(static_cast<size_t>(g_registered->mouse[0]));
            if ((buttons & RI_MOUSE_BUTTON_5_DOWN) && g_registered->mouse[1] >= 0) FireHotkey(static_cast<size_t>(g_registered->mouse[1]));
        } else if (g_registered && input.header.dwType == RIM_TYPEKEYBOARD && input.data.keyboard.VKey < 256) {
            const USHORT vk = input.data.keyboard.VKey;
            if (g_rawKeys.Fires(vk, !(input.data.keyboard.Flags & RI_KEY_BREAK))) {
                const auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
                const int mix = (held(VK_SHIFT) ? 1 : 0) | (held(VK_CONTROL) ? 2 : 0) | (held(VK_MENU) ? 4 : 0);
                for (const auto& [key, bound] : g_registered->rawKeys) {
                    if (key != vk) continue;
                    const unsigned mixes = bound < shell::kHotkeys ? g_registered->mixes[bound]
                        : bound - shell::kHotkeys < g_registered->songMixes.size() ? g_registered->songMixes[bound - shell::kHotkeys] : 0u;
                    if (shell::RawHotkeyFires(vk, mixes, mix, g_registered->typing)) FireHotkey(bound);
                }
            }
        }
        break;  // DefWindowProc frees the input
    }
    case WM_INPUTLANGCHANGE:
        // Wakes the loop, hidden too, to check the bound keys under the new layout.
        PostMessageW(hwnd, WM_NULL, 0, 0);
        break;
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wp);
        wchar_t path[32768]{};
        if (DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path)))) g_opened.emplace_back(path);
        DragFinish(drop);
        return 0;
    }
    case WM_DEVICECHANGE:
        if (wp != DBT_DEVNODES_CHANGED) break;
        SetTimer(hwnd, kDeviceScanTimer, kDeviceScanSettleMs, nullptr);
        return TRUE;
    case WM_TIMER:
        if (wp != kDeviceScanTimer) break;
        KillTimer(hwnd, kDeviceScanTimer);
        if (g_engine) {
            g_engine->Send({shell::ShellEngine::Action::LiveScan});
            g_engine->Send({shell::ShellEngine::Action::OutputScan});
        }
        return 0;
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
        // With no taskbar button to come back from, minimizing hides instead.
        if ((wp & 0xfff0) == SC_MINIMIZE && GetWindow(hwnd, GW_OWNER)) { ShowOrHide(hwnd); return 0; }
        break;
    case WM_MOUSEACTIVATE:
        if (g_noActivate) return MA_NOACTIVATE;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// One copy runs per data folder, so the hotkeys and the saved settings have one
// owner; builds with other data folders still run side by side. A second start
// hands its path to the running copy's message-only receiver, named like the
// mutex, and exits.
constexpr wchar_t kReceiverClass[] = L"QuartzMIDI.Open";
constexpr ULONG_PTR kOpenPathData = 0x514D4F50;

static std::wstring InstanceName(const std::filesystem::path& directory) {
    std::wstring folder = directory.lexically_normal().wstring();
    CharLowerBuffW(folder.data(), static_cast<DWORD>(folder.size()));
    const std::string hash = shell::bundle::Fnv1a({reinterpret_cast<const char*>(folder.data()), folder.size() * sizeof(wchar_t)});
    return L"QuartzMIDI-" + std::wstring(hash.begin(), hash.end());
}

// GWLP_USERDATA holds the main window, raised after each hand-over.
static LRESULT WINAPI ReceiverProc(HWND receiver, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_COPYDATA) return DefWindowProcW(receiver, msg, wp, lp);
    const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lp);
    if (!data || data->dwData != kOpenPathData || data->cbData % sizeof(wchar_t) != 0 || data->cbData > 32768 * sizeof(wchar_t) ||
        (data->cbData > 0 && !data->lpData))
        return FALSE;
    if (data->cbData > 0) g_opened.emplace_back(std::wstring(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t)));
    if (const HWND main = reinterpret_cast<HWND>(GetWindowLongPtrW(receiver, GWLP_USERDATA))) {
        // A window the show/hide key hid comes back too.
        if (!IsWindowVisible(main)) ShowOrHide(main);
        if (IsIconic(main)) ShowWindow(main, SW_RESTORE);
        SetForegroundWindow(main);
    }
    return TRUE;
}

// Hands `path` (possibly empty) to the copy holding `instance`. False when that
// copy exits first, leaving `instance` owned by this one, or cannot be reached;
// either way this copy then starts as usual.
static bool HandToRunningCopy(HANDLE instance, const std::wstring& name, const std::filesystem::path& path) {
    // The receiver appears once the running copy's window is up.
    const ULONGLONG deadline = GetTickCount64() + 10000;
    HWND receiver = nullptr;
    while (!(receiver = FindWindowExW(HWND_MESSAGE, nullptr, kReceiverClass, name.c_str()))) {
        const DWORD waited = WaitForSingleObject(instance, 50);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED || GetTickCount64() >= deadline) return false;
    }
    // Only the foreground process can let another take the foreground.
    DWORD process = 0;
    GetWindowThreadProcessId(receiver, &process);
    AllowSetForegroundWindow(process);
    const std::wstring text = path.wstring();
    COPYDATASTRUCT data{kOpenPathData, static_cast<DWORD>(text.size() * sizeof(wchar_t)), const_cast<wchar_t*>(text.data())};
    DWORD_PTR accepted = 0;
    return SendMessageTimeoutW(receiver, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000, &accepted) && accepted;
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

using shell::ApplyAffinity;

// The affinity every window of the app carries. A viewport window (a popup that
// leaves the main window, Key Mapping, the mini window's menus) gets it as it is
// made, before it is first shown.
static DWORD g_affinity = WDA_NONE;
static void (*g_createViewport)(ImGuiViewport*) = nullptr;

// Mini's popups and windows too (g_noActivate).
static WNDPROC g_viewportProc = nullptr;
static void (*g_showViewport)(ImGuiViewport*) = nullptr;

static LRESULT WINAPI ViewportProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE && g_noActivate) return MA_NOACTIVATE;
    return CallWindowProcW(g_viewportProc, window, msg, wp, lp);
}

// Called after each ImGui_ImplWin32_Init, which sets the backend's own.
static void HookViewports() {
    auto& platform = ImGui::GetPlatformIO();
    g_createViewport = platform.Platform_CreateWindow;
    g_showViewport = platform.Platform_ShowWindow;
    platform.Platform_CreateWindow = [](ImGuiViewport* viewport) {
        g_createViewport(viewport);
        const auto window = static_cast<HWND>(viewport->PlatformHandle);
        if (!window) return;
        // Every viewport window shares the backend's window procedure.
        if (const auto proc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)); proc != ViewportProc) {
            g_viewportProc = proc;
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ViewportProc));
        }
        if (g_affinity != WDA_NONE && !ApplyAffinity(window, g_affinity) && g_panels)
            g_panels->ReportError("Could not hide a window from screen capture.");
    };
    platform.Platform_ShowWindow = [](ImGuiViewport* viewport) {
        const ImGuiViewportFlags flags = viewport->Flags;
        if (g_noActivate) viewport->Flags |= ImGuiViewportFlags_NoFocusOnAppearing;
        g_showViewport(viewport);
        viewport->Flags = flags;
    };
}

// The window holding the control that reads keys: where ImGui's active item,
// else its focused window, is drawn.
static HWND KeyWindow(HWND main) {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiWindow* window = g.ActiveIdWindow ? g.ActiveIdWindow : g.NavWindow;
    const auto own = window && window->Viewport ? static_cast<HWND>(window->Viewport->PlatformHandle) : nullptr;
    return own ? own : main;
}

// Fits a window rect into a monitor's work area: no larger than it, then moved
// inside it, so neither the title bar nor the status bar ends up off screen or
// under the taskbar.
static RECT ClampToWork(RECT target, HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const RECT& work = info.rcWork;
    const LONG width = std::min(target.right - target.left, work.right - work.left);
    const LONG height = std::min(target.bottom - target.top, work.bottom - work.top);
    const LONG left = std::clamp(target.left, work.left, work.right - width);
    const LONG top = std::clamp(target.top, work.top, work.bottom - height);
    return {left, top, left + width, top + height};
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
    // Owned for this copy's lifetime; checked before anything loads settings.
    const std::wstring instanceName = InstanceName(directory);
    HANDLE instance = CreateMutexW(nullptr, TRUE, instanceName.c_str());
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        std::filesystem::path handed;
        int count = 0;
        if (LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count)) {
            // Resolved here: the running copy has its own working folder.
            std::error_code error;
            if (count > 1) handed = std::filesystem::absolute(args[1], error);
            if (count > 1 && (error || handed.empty())) handed = args[1];
            LocalFree(args);
        }
        if (HandToRunningCopy(instance, instanceName, handed)) {
            CloseHandle(instance);
            if (SUCCEEDED(com)) CoUninitialize();
            return 0;
        }
    }
    // Released when wWinMain returns, after the engine below has written
    // config.json and the settings are saved, so a start waiting on it reads both.
    struct InstanceGuard { HANDLE handle; ~InstanceGuard() { if (handle) { ReleaseMutex(handle); CloseHandle(handle); } } } instanceGuard{instance};
    // The log also goes to a file beside the settings, and a crash lets every
    // key go and leaves its report beside that.
    shell::ShellLog::Instance().SetFile(directory / L"quartzmidi.log");
    crash::Install(directory, directory / L"quartzmidi.log");
    const auto preferencesPath = directory / L"shell-settings.json";
    shell::Panels panels;
    panels.LoadPreferences(preferencesPath);
    shell::CaptureShellLog captureLog;
    // The engine checks a key it binds against this thread's layout, the one the user switches.
    shell::g_layoutThread = GetCurrentThreadId();
    shell::ShellEngine engine(directory / L"config.json", {}, false,
        [] { return std::make_unique<shell::NativeConnectInput>(); });
    g_engine = &engine;
    g_panels = &panels;
    // Icon resource 1 in Shell.rc.
    HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst,
                       icon, nullptr, nullptr, nullptr, L"QuartzMIDI", icon };
    ::RegisterClassExW(&wc);
    // Restore the saved position if its title bar is still on a monitor, and
    // size the window for that monitor's DPI; otherwise open on the primary.
    const auto& saved = panels.preferences;
    const RECT last{saved.windowX, saved.windowY, saved.windowX + saved.windowWidth, saved.windowY + 100};
    const HMONITOR restored = saved.windowWidth > 0 ? MonitorFromRect(&last, MONITOR_DEFAULTTONULL) : nullptr;
    const HMONITOR home = restored ? restored : MonitorFromPoint(POINT{100, 100}, MONITOR_DEFAULTTOPRIMARY);
    g_dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(home);
    const ImVec2 desired = panels.DesiredSize();
    // Any height the full window was dragged beyond the desired one comes back too.
    RECT initial{0, 0, static_cast<LONG>(desired.x * UiScale()), static_cast<LONG>((desired.y + saved.windowExtra) * UiScale())};
    AdjustWindowRectExForDpi(&initial, WS_OVERLAPPEDWINDOW, FALSE, 0, static_cast<UINT>(96.f * g_dpi));
    const LONG startWidth = restored ? std::max(initial.right - initial.left, static_cast<LONG>(saved.windowWidth)) : initial.right - initial.left;
    const LONG startX = restored ? saved.windowX : 100, startY = restored ? saved.windowY : 100;
    const RECT start = ClampToWork({startX, startY, startX + startWidth, startY + initial.bottom - initial.top}, home);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"QuartzMIDI",
                                WS_OVERLAPPEDWINDOW, start.left, start.top, start.right - start.left, start.bottom - start.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    // Windows sends no WM_DPICHANGED to a window created on a monitor, so one
    // that still lands at another DPI is rescaled for it here.
    if (const float actual = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd); actual != g_dpi) {
        const float ratio = actual / g_dpi;
        g_dpi = actual;
        SetWindowPos(hwnd, nullptr, 0, 0, static_cast<LONG>((start.right - start.left) * ratio),
                     static_cast<LONG>((start.bottom - start.top) * ratio), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ApplyCaption(hwnd, panels.ActiveSkin());  // before the first paint to avoid a white flash
    if (!CreateDevice(hwnd)) {
        shell::ShellLog::Instance().Append("[error] The graphics device could not be created.\n");
        MessageBoxW(hwnd, L"The graphics device could not be created.", L"QuartzMIDI", MB_ICONERROR | MB_OK);
        CleanupDevice();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    DragAcceptFiles(hwnd, TRUE);
    WNDCLASSEXW receiverClass{sizeof(receiverClass)};
    receiverClass.lpfnWndProc = ReceiverProc;
    receiverClass.hInstance = inst;
    receiverClass.lpszClassName = kReceiverClass;
    ::RegisterClassExW(&receiverClass);
    HWND receiver = ::CreateWindowExW(0, kReceiverClass, instanceName.c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (receiver) SetWindowLongPtrW(receiver, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(hwnd));
    // Registered from the loop because RegisterHotKey binds to the thread that
    // owns hwnd; re-registered whenever the snapshot's hotkeys change.
    Registered hotkeys;
    g_registered = &hotkeys;
    g_window = hwnd;
    // Whether the window is out of the taskbar, and the hotkeys last seen.
    bool appliedTaskbarHidden = false;
    uint64_t seenHotkeys = 0;
    shell::HotkeyCapture capture;
    bool capturing = false;
    // Key that ended a capture, swallowed until released so its auto-repeat
    // doesn't reach the focused control.
    WPARAM capturedKey = 0;
    // A Wooting pedal key is being learnt from the Wooting itself.
    bool pedalCapturing = false;
    // A control reads keys, and the window mini took the keyboard from for it.
    bool readingKeys = false;
    HWND keyboardFrom = nullptr;
    // When the running key test gives up, 0 when none runs, and whether its key came.
    ULONGLONG keyTestUntil = 0;
    bool keyTestArrived = false;
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
    HookViewports();
    ImGui_ImplDX11_Init(g_device, g_context);
    panels.captureExclusionOffered = shell::CaptureExclusionOffered();

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
    // DPI fullRect was measured at; mini may be moved to a monitor at another.
    float fullDpi = g_dpi;
    bool fullMaximized = panels.preferences.maximized;
    panels.miniMode = panels.preferences.startMini;
    ULONGLONG preferencesSaved = GetTickCount64();
    if (panels.preferences.folder.empty()) {
        auto folder = directory / L"midi";
        if (!std::filesystem::is_directory(folder)) folder = directory.parent_path().parent_path() / L"x64" / L"Release" / L"midi";
        if (std::filesystem::is_directory(folder)) panels.preferences.folder = std::filesystem::weakly_canonical(folder);
    }
    engine.Send({shell::ShellEngine::Action::AutoSolo, {}, 0, 0, panels.preferences.autoSolo});
    if (!panels.preferences.folder.empty()) engine.Send({shell::ShellEngine::Action::Scan, panels.preferences.folder});
    int argc = 0;
    bool songOpened = false;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1) songOpened = OpenPath(argv[1]);
        LocalFree(argv);
    }
    // Without a song to open, the song open at the last close opens again,
    // paused at its beginning. One that is gone is left alone.
    if (std::error_code gone; !songOpened && std::filesystem::is_regular_file(panels.preferences.lastSong, gone))
        engine.Send({shell::ShellEngine::Action::Load, panels.preferences.lastSong, 0, 0, panels.preferences.autoSolo});
    // Saved with the preferences. Nothing is open until the reopened song
    // loads, so an empty snapshot keeps the song already saved.
    const auto keepSong = [&] {
        if (const auto open = engine.Snapshot()->loaded; !open.empty()) panels.preferences.lastSong = open;
    };
    // Check for updates: once a session, on a thread of its own, when the switch
    // is on at start or first turned on. Its answer waits in the slot for the
    // loop, and a check still running at exit is left to end with the process.
    struct UpdateSlot { std::mutex mutex; std::optional<shell::AvailableUpdate> found; };
    const auto updates = std::make_shared<UpdateSlot>();
    bool updateAsked = false;

    bool running = true;
    bool appliedTopmost = false;
    // Always on top as last applied, which mini leaves alone.
    bool appliedSwitch = false;
    int appliedOpacity = 100;
    DWORD appliedAffinity = WDA_NONE;
    // Render on demand: on input, a new engine snapshot (the engine posts
    // WM_NULL), or a size/skin/scale change, then for a 750 ms tail so tooltip
    // delays and popups can finish. An idle window presents nothing.
    engine.SetWakeWindow(hwnd);
    struct WakeGuard { shell::ShellEngine& engine; ~WakeGuard() { engine.SetWakeWindow(nullptr); } } wakeGuard{engine};
    std::shared_ptr<const shell::EngineSnapshot> drawnSnapshot;
    // Live velocities are drawn only by the open Velocity Response graph, so a
    // note redraws only while it is shown.
    uint64_t drawnPlayed = 0;
    auto drawUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // Set before the first frame and after an idle wait, so the frame that follows
    // doesn't count the wait as frame time and skip the transitions input starts.
    bool waited = true;
    std::chrono::steady_clock::time_point drawnAt;
    bool occluded = false;
    bool rendererUp = true;
    // The window is first shown by the first pass, at its final size and mode,
    // so a start in mini never shows an empty full-size window.
    bool shown = false;
    while (running) {
        MSG msg;
        bool input = false;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            // Swallow key-downs during a rebind capture so they don't reach
            // ImGui. Filtered here because Settings can be its own OS window.
            const bool keyDownMessage = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
            if (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST && static_cast<ULONG_PTR>(GetMessageExtraInfo()) == kKeyTestTag) {
                if (keyDownMessage) keyTestArrived = true;
                input = true;
                continue;
            }
            if ((msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) && msg.wParam == capturedKey) capturedKey = 0;
            if (keyDownMessage && (capturing || pedalCapturing || (capturedKey != 0 && msg.wParam == capturedKey))) { input = true; continue; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
            // Raw mouse input, read while a side button is bound, arrives on
            // every move anywhere and draws nothing.
            if (msg.message != WM_INPUT) input = true;
        }
        if (!running) break;
        for (auto& path : std::exchange(g_opened, {})) { OpenPath(std::move(path)); input = true; }
        g_noActivate = panels.miniMode;
        // A key capture is for a hotkey's place or, from a song's menu or row, for a song.
        const auto armed = [&] { return panels.hotkeyCapture >= 0 || !panels.songHotkeyCapture.empty(); };
        {
            // A text field, a key being bound (an action's or a song's, which
            // the game in front would cancel) or a Wooting pedal key being learnt
            // reads keys, as does the key test. Taken once: a field left active
            // behind the game does not pull the keyboard back.
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            const bool reads = g.IO.WantTextInput || g.WantTextInputNextFrame == 1 ||
                               armed() || panels.wootingPedalCapture >= 0 ||
                               panels.keyTestRequested || keyTestUntil != 0;
            const HWND front = GetForegroundWindow();
            DWORD owner = 0;
            GetWindowThreadProcessId(front, &owner);
            const bool ours = owner == GetCurrentProcessId();
            switch (shell::MiniKeyboard(panels.miniMode, reads, readingKeys, ours, keyboardFrom != nullptr)) {
            case shell::KeyboardMove::Take:
                keyboardFrom = front;
                SetForegroundWindow(KeyWindow(hwnd));
                break;
            case shell::KeyboardMove::GiveBack:
                // Unless the user has since gone to another window, or to the full window.
                if (panels.miniMode && ours && IsWindow(keyboardFrom)) SetForegroundWindow(keyboardFrom);
                keyboardFrom = nullptr;
                break;
            case shell::KeyboardMove::Stay: break;
            }
            readingKeys = reads;
        }
        // Settings' key test: F24 down and up to the app's own window, which has
        // the keyboard by now, then the game's integrity level beside the app's.
        if (std::exchange(panels.keyTestRequested, false) && !keyTestUntil) {
            DWORD owner = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &owner);
            if (owner != GetCurrentProcessId()) {
                panels.keyTestResult = shell::KeyTestResult(shell::KeyTestOutcome::NoFocus, 0, 0);
                panels.keyTestPassed = false;
            }
            else {
                // Off while it runs, so a hotkey on F24 cannot take the key.
                UnregisterHotkeys(hwnd, hotkeys);
                INPUT keys[2]{};
                for (int i = 0; i < 2; ++i) {
                    keys[i].type = INPUT_KEYBOARD;
                    keys[i].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_F24, MAPVK_VK_TO_VSC));
                    keys[i].ki.dwFlags = KEYEVENTF_SCANCODE | (i ? KEYEVENTF_KEYUP : 0);
                    keys[i].ki.dwExtraInfo = kKeyTestTag;
                }
                keyTestArrived = false;
                keyTestUntil = GetTickCount64() + kKeyTestWaitMs;
                // A send Windows refuses ends the test at once.
                if (SendInput(2, keys, sizeof(INPUT)) != 2) keyTestUntil = 1;
            }
        }
        if (keyTestUntil && (keyTestArrived || GetTickCount64() >= keyTestUntil)) {
            const DWORD ours = shell::ProcessIntegrity(GetCurrentProcessId()), game = shell::RobloxIntegrity();
            panels.keyTestResult = shell::KeyTestResult(keyTestArrived ? shell::KeyTestOutcome::Arrived : shell::KeyTestOutcome::Lost, ours, game);
            panels.keyTestPassed = shell::KeyTestPassed(keyTestArrived ? shell::KeyTestOutcome::Arrived : shell::KeyTestOutcome::Lost, ours, game);
            keyTestUntil = 0;
        }
        panels.keyTesting = keyTestUntil != 0;
        {
            DWORD foreground = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
            // GetAsyncKeyState is global, so cancel a capture when another
            // process takes the foreground.
            if (armed() && foreground != GetCurrentProcessId()) { panels.hotkeyCapture = -1; panels.songHotkeyCapture.clear(); }
            if (capturedKey != 0 && !keyDown(static_cast<int>(capturedKey))) capturedKey = 0;
            if (armed()) {
                if (!capturing) { UnregisterHotkeys(hwnd, hotkeys); capture.Begin(keyDown); capturing = true; }
                const auto mapped = engine.Snapshot();
                const int pressed = capture.Poll(keyDown, [&](int vk) { return shell::IsNoteKey(vk, mapped->keyMappings); });
                if (capture.Refused()) panels.captureRefusedAt = ImGui::GetTime();
                if (pressed > 0) {
                    const bool song = panels.hotkeyCapture < 0;
                    shell::ShellEngine::Command command{song ? shell::ShellEngine::Action::SongHotkey : shell::ShellEngine::Action::Hotkey};
                    if (song) command.path = panels.songHotkeyCapture;
                    else command.track = static_cast<size_t>(panels.hotkeyCapture);
                    command.key = shell::VKToName(pressed);
                    engine.Send(std::move(command));
                    if (panels.hideOnShowHideKey && panels.hotkeyCapture == static_cast<int>(shell::kShowHideHotkey))
                        panels.hideOnceShowHideWorks = true;
                }
                if (pressed != shell::HotkeyCapture::None) {
                    capturedKey = pressed > 0 ? static_cast<WPARAM>(pressed) : VK_ESCAPE;
                    panels.hotkeyCapture = -1;
                    panels.songHotkeyCapture.clear();
                }
            }
            if (!armed()) capturing = false;
            if (panels.hotkeyCapture != static_cast<int>(shell::kShowHideHotkey)) panels.hideOnShowHideKey = false;
            if (panels.wootingPedalCapture >= 0 && foreground != GetCurrentProcessId()) panels.wootingPedalCapture = -1;
            if (panels.wootingPedalCapture >= 0) {
                if (!pedalCapturing) { WootingBeginKeyCapture(); pedalCapturing = true; }
                if (const uint16_t key = WootingCapturedKey()) {
                    // Escape cancels, as it does for the hotkeys.
                    if (key != 0x01) {
                        shell::ShellEngine::Command command{shell::ShellEngine::Action::WootingPedalKey};
                        command.track = static_cast<size_t>(panels.wootingPedalCapture);
                        command.amount = key;
                        engine.Send(std::move(command));
                    }
                    const UINT vk = MapVirtualKeyW(key, MAPVK_VSC_TO_VK_EX);
                    capturedKey = vk ? static_cast<WPARAM>(vk) : VK_ESCAPE;
                    panels.wootingPedalCapture = -1;
                }
            }
            if (panels.wootingPedalCapture < 0 && pedalCapturing) { WootingEndKeyCapture(); pedalCapturing = false; }
            // Don't re-register until the captured key is released, or its
            // auto-repeat fires the action just bound to it, nor while the key
            // test runs.
            if (capturedKey != 0 || keyTestUntil) drawUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
            else if (!capturing) {
                const auto snapshot = engine.Snapshot();
                // Performer keys are registered only while their trigger is
                // active, and an action's while the song plays with the
                // performer on. An inactive key is not reported as unavailable.
                auto wanted = snapshot->hotkeys;
                const bool typing = snapshot->playing || snapshot->playbackCountdown > 0 ||
                                    snapshot->liveActive || snapshot->midiConnect;
                std::array<bool, shell::kHotkeys> resting{};
                for (size_t i = shell::kAppHotkeys; i < shell::kFirstLaterHotkey; ++i)
                    resting[i] = snapshot->performer.ActionOfKey(i) >= 0 ? !(snapshot->performer.on && snapshot->playing)
                        : snapshot->trigger == 0 || snapshot->performer.TriggerOfKey(i) != snapshot->trigger;
                for (size_t i = 0; i < shell::kHotkeys; ++i) if (resting[i]) wanted[i].clear();
                const bool media = panels.preferences.mediaKeys;
                const bool blockAltF4 = panels.preferences.blockAltF4;
                // A layout switch (WM_INPUTLANGCHANGE wakes the loop) or a key map
                // edit can make a bound key one the app types.
                if (!hotkeys.any || wanted != hotkeys.names || snapshot->songHotkeys != hotkeys.songs ||
                    typing != hotkeys.typing || media != hotkeys.media || blockAltF4 != hotkeys.blockAltF4 ||
                    shell::UiLayout() != hotkeys.layout || snapshot->mappingRevision != hotkeys.mappingRevision) {
                    RegisterHotkeys(hwnd, hotkeys, wanted, snapshot->songHotkeys, typing, media, blockAltF4,
                                    snapshot->keyMappings, snapshot->mappingRevision);
                    panels.songKeysAvailable = hotkeys.songHeld;
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
                panels.ReportError("Could not change window opacity.");
            }
        }
        if (!updateAsked && panels.preferences.checkForUpdates) {
            updateAsked = true;
            std::thread([updates, hwnd] {
                auto found = shell::CheckForUpdate(shell::FetchLatestRelease);
                { std::lock_guard lock(updates->mutex); updates->found = std::move(found); }
                PostMessageW(hwnd, WM_NULL, 0, 0);
            }).detach();
        }
        if (std::lock_guard lock(updates->mutex); updates->found) {
            panels.update = std::move(*updates->found);
            updates->found.reset();
            drawUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
        }
        // Mini is topmost whatever the switch says; the switch is put back only
        // when it was the switch's change that failed to apply.
        if (const bool topmost = panels.Topmost(); appliedTopmost != topmost) {
            if (SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
                appliedTopmost = topmost;
            else if (panels.preferences.alwaysOnTop != appliedSwitch) panels.preferences.alwaysOnTop = appliedSwitch;
            else appliedTopmost = topmost;
        }
        appliedSwitch = panels.preferences.alwaysOnTop;
        if (const DWORD wanted = panels.captureExclusionOffered && panels.preferences.hideFromCapture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
            appliedAffinity != wanted) {
            // Every window the app has now; later ones get it as they are made.
            std::vector<HWND> windows{hwnd};
            if (g_taskbarOwner) windows.push_back(g_taskbarOwner);
            for (const ImGuiViewport* viewport : ImGui::GetPlatformIO().Viewports)
                if (const auto window = static_cast<HWND>(viewport->PlatformHandle); window && window != hwnd) windows.push_back(window);
            bool took = true;
            for (const HWND window : windows) took = ApplyAffinity(window, wanted) && took;
            if (took) appliedAffinity = g_affinity = wanted;
            else {
                for (const HWND window : windows) ApplyAffinity(window, appliedAffinity);
                panels.preferences.hideFromCapture = appliedAffinity == WDA_EXCLUDEFROMCAPTURE;
                panels.ReportError(wanted == WDA_NONE ? "Could not show the app to screen capture." : "Could not hide the app from screen capture.");
            }
        }
        {
            // Unbinding the show/hide key turns the switch off. Without a key
            // that registered, the window keeps its taskbar button either way.
            const auto current = engine.Snapshot();
            const bool showHide = !current->hotkeys[shell::kShowHideHotkey].empty();
            if (current->hotkeyRevision != seenHotkeys) {
                seenHotkeys = current->hotkeyRevision;
                if (!showHide) panels.preferences.hideFromTaskbar = false;
            }
            const bool hide = shown && panels.preferences.hideFromTaskbar && showHide && panels.transportKeysAvailable[shell::kShowHideHotkey];
            if (hide != appliedTaskbarHidden) {
                HideFromTaskbar(hwnd, hide);
                appliedTaskbarHidden = hide;
                if (g_taskbarOwner) ApplyAffinity(g_taskbarOwner, g_affinity);
            }
        }
        // Hidden by the show/hide key, it draws nothing until shown again. A song
        // a key opens meanwhile is saved as the last song all the same, which
        // the save after each frame drawn would miss at a shutdown or crash.
        if (IsIconic(hwnd) || (shown && !IsWindowVisible(hwnd))) {
            if (const auto open = engine.Snapshot()->loaded; !open.empty() && open != panels.preferences.lastSong) {
                keepSong();
                panels.SavePreferences(preferencesPath, false);
                preferencesSaved = GetTickCount64();
            }
            WaitMessage();
            continue;
        }
        const skin::Skin current = panels.ActiveSkin();
        const uint64_t active = shell::SkinSignature(current);
        // "Focused" means any window of this process, since viewports such as
        // Key Mapping are separate top-level windows.
        DWORD foregroundProcess = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        const bool ours = foregroundProcess == GetCurrentProcessId();
        {
            const auto now = std::chrono::steady_clock::now();
            auto snapshot = engine.Snapshot();
            const uint64_t played = velocity_telemetry::snapshot().revision;
            const bool pending = appliedMini != panels.miniMode || appliedLayout != layoutNow() ||
                (panels.miniMode && appliedMiniAutoplay != panels.miniAutoplay) || appliedSkin != active || appliedDpi != UiScale();
            // Keep the drawn snapshot alive: a freed snapshot's address can be
            // reused by the next one, so a raw-pointer compare would miss it.
            const bool graphShown = panels.velocityExpanded && !panels.miniMode;
            if (input || pending || snapshot != drawnSnapshot || panels.Animating() || snapshot->busy ||
                (graphShown && played != drawnPlayed) || g_deviceLost)
                drawUntil = now + std::chrono::milliseconds(750);
            // An active text field only needs its caret blinking: a frame every
            // 100 ms, as the blink follows elapsed time. ImGui keeps the field
            // active after the game takes the foreground, so only while focused.
            const bool caret = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput && ours;
            const auto caretDue = drawnAt + std::chrono::milliseconds(100);
            if (now >= drawUntil && !(caret && now >= caretDue)) {
                // Live notes reach the velocity graph without a publish to wake
                // the loop, so poll for them while the graph is shown and live
                // input can deliver any.
                auto wait = std::chrono::milliseconds(graphShown && (snapshot->liveActive || snapshot->midiConnect) ? 16 : 500);
                if (caret) wait = std::min(wait, std::chrono::ceil<std::chrono::milliseconds>(caretDue - now));
                MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(wait.count()), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                waited = true;
                continue;
            }
            drawnSnapshot = std::move(snapshot);
            drawnPlayed = played;
            drawnAt = now;
            // A frame drawn only for the caret keeps its real frame time: capped
            // like the frame after an idle wait, the blink would run slow.
            if (now >= drawUntil) waited = false;
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
            HookViewports();
            ImGui_ImplDX11_Init(g_device, g_context);
            rendererUp = true;
            g_deviceLost = false;
            shell::ShellLog::Instance().Append("The graphics device was reset and has been rebuilt.\n");
        }
        // While occluded (covered or locked), poll with DXGI_PRESENT_TEST instead
        // of drawing, unless another viewport window may still be visible.
        if (occluded && ImGui::GetPlatformIO().Viewports.Size <= 1 &&
            g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            // Nothing was shown, so the current snapshot is drawn once visible;
            // a song that ended while locked would otherwise stay on screen.
            drawnSnapshot.reset();
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
                // Before the first show, the saved state stands in for IsZoomed.
                if (panels.miniMode && shown) { fullMaximized = IsZoomed(hwnd) != FALSE; if (fullMaximized) ShowWindow(hwnd, SW_RESTORE); }
                const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
                SetWindowLongPtrW(hwnd, GWL_STYLE, panels.miniMode ? style & ~WS_MAXIMIZEBOX : style | WS_MAXIMIZEBOX);
            }
            RECT window{}, client{}; GetWindowRect(hwnd, &window); GetClientRect(hwnd, &client);
            if (modeChanged && panels.miniMode) { fullRect = window; fullDpi = g_dpi; }
            // Mini reopens where it was left, in this session and the next.
            if (modeChanged && !panels.miniMode) {
                panels.preferences.miniX = window.left;
                panels.preferences.miniY = window.top;
                panels.preferences.miniSaved = true;
            }
            RECT target{};
            HMONITOR onto = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            const auto desired = panels.DesiredSize();
            if (modeChanged && !panels.miniMode) {
                // The full window returns where it was, rescaled for any DPI
                // change since; if that monitor is gone, it opens where mini is.
                const float ratio = g_dpi / fullDpi;
                target = {fullRect.left, fullRect.top, fullRect.left + static_cast<LONG>((fullRect.right - fullRect.left) * ratio),
                          fullRect.top + static_cast<LONG>((fullRect.bottom - fullRect.top) * ratio)};
                if (const HMONITOR fullMonitor = MonitorFromRect(&fullRect, MONITOR_DEFAULTTONULL)) onto = fullMonitor;
                else OffsetRect(&target, window.left - target.left, window.top - target.top);
            } else {
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
                if (modeChanged && panels.preferences.miniSaved) {
                    RECT parked = target;
                    OffsetRect(&parked, panels.preferences.miniX - target.left, panels.preferences.miniY - target.top);
                    if (const HMONITOR there = MonitorFromRect(&parked, MONITOR_DEFAULTTONULL)) { target = parked; onto = there; }
                }
            }
            // A move onto a monitor at another DPI rescales the window through
            // WM_DPICHANGED, so it is clamped at the size it will have there.
            const float landing = ImGui_ImplWin32_GetDpiScaleForMonitor(onto) / g_dpi;
            const RECT placed = ClampToWork({target.left, target.top, target.left + static_cast<LONG>((target.right - target.left) * landing),
                                             target.top + static_cast<LONG>((target.bottom - target.top) * landing)}, onto);
            SetWindowPos(hwnd, nullptr, placed.left, placed.top, static_cast<LONG>((placed.right - placed.left) / landing),
                         static_cast<LONG>((placed.bottom - placed.top) / landing), SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            if (modeChanged && !panels.miniMode && fullMaximized) ShowWindow(hwnd, SW_MAXIMIZE);
            // The full window, asked for from mini, takes the keyboard as any window does.
            if (modeChanged && !panels.miniMode && shown) SetForegroundWindow(hwnd);
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
        if (!shown) {
            // A shortcut set to start minimized still restores to the maximized window left last time.
            STARTUPINFOW start{sizeof start};
            GetStartupInfoW(&start);
            const bool startMinimized = (start.dwFlags & STARTF_USESHOWWINDOW) &&
                (start.wShowWindow == SW_SHOWMINIMIZED || start.wShowWindow == SW_MINIMIZE ||
                 start.wShowWindow == SW_SHOWMINNOACTIVE || start.wShowWindow == SW_FORCEMINIMIZE);
            if (startMinimized && !panels.miniMode && fullMaximized) {
                WINDOWPLACEMENT place{sizeof place};
                GetWindowPlacement(hwnd, &place);
                place.flags |= WPF_RESTORETOMAXIMIZED;
                place.showCmd = SW_SHOWMINNOACTIVE;
                SetWindowPlacement(hwnd, &place);
            } else {
                ::ShowWindow(hwnd, !panels.miniMode && fullMaximized ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT);
            }
            ::UpdateWindow(hwnd);
            shown = true;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (waited) ImGui::GetIO().DeltaTime = std::min(ImGui::GetIO().DeltaTime, 1.f / 60);
        waited = false;
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
        const HWND frontBefore = GetForegroundWindow();
        panels.Draw(hwnd, fonts, current, UiScale(), engine);
        // A file dialog opened from mini takes the keyboard while it is open, and
        // on closing activates the app; the keyboard goes back where it was.
        if (panels.miniMode && !keyboardFrom && frontBefore && GetForegroundWindow() != frontBefore && IsWindow(frontBefore)) {
            DWORD before = 0, after = 0;
            GetWindowThreadProcessId(frontBefore, &before);
            GetWindowThreadProcessId(GetForegroundWindow(), &after);
            if (before != GetCurrentProcessId() && after == GetCurrentProcessId()) SetForegroundWindow(frontBefore);
        }
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
            RECT window{}, client{};
            // While mini is shown, the full window's state is the one it returns to.
            if (!IsIconic(hwnd)) panels.preferences.maximized = panels.miniMode ? fullMaximized : IsZoomed(hwnd) != FALSE;
            if (!panels.miniMode && !IsZoomed(hwnd) && !IsIconic(hwnd) && GetWindowRect(hwnd, &window) && GetClientRect(hwnd, &client)) {
                panels.preferences.windowX = window.left;
                panels.preferences.windowY = window.top;
                panels.preferences.windowWidth = window.right - window.left;
                // Height dragged beyond the applied layout's, in dp.
                panels.preferences.windowExtra = std::max(0.f, client.bottom - appliedLayout[0] * g_dpi) / (g_dpi * appliedLayout[3]);
            }
            if (panels.miniMode && !IsIconic(hwnd) && GetWindowRect(hwnd, &window)) {
                panels.preferences.miniX = window.left;
                panels.preferences.miniY = window.top;
                panels.preferences.miniSaved = true;
            }
            keepSong();
            panels.SavePreferences(preferencesPath, false);
        }
        // Throttle redraws to ~12 fps when unfocused; playback doesn't depend on
        // frame rate. The engine's WM_NULL posts wait for the next pass then, or
        // each publish would draw a frame; input and hotkeys still end the wait.
        MsgWaitForMultipleObjectsEx(0, nullptr, ours ? 16 : 80,
                                    ours ? QS_ALLINPUT : QS_ALLINPUT & ~QS_POSTMESSAGE, MWMO_INPUTAVAILABLE);
    }
    // Nothing is pumped from here on, so a start now waits for the mutex instead.
    if (receiver) ::DestroyWindow(receiver);

    if (rendererUp) { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); }
    ImGui::DestroyContext();
    UnregisterHotkeys(hwnd, hotkeys);
    g_registered = nullptr;
    g_window = nullptr;
    keepSong();
    panels.SavePreferences(preferencesPath);
    g_engine = nullptr;
    g_panels = nullptr;
    CleanupDevice();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ::UnregisterClassW(kReceiverClass, inst);

    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
