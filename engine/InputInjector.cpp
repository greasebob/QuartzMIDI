#include "InputHeader.h"
#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <iterator>

#pragma comment(lib, "shlwapi.lib")

// Keystroke injection through SendInput. InputLatency calls through the
// InjectInput pointer, and tests replace it with a recorder so no test
// keystroke reaches Windows.
bool IsRobloxImage(const wchar_t* imagePath) noexcept
{
    if (!imagePath || !*imagePath) return false;
    const wchar_t* name = imagePath;
    for (const wchar_t* p = imagePath; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    if (_wcsnicmp(name, L"RobloxPlayer", 12) == 0) return true;
    // The Microsoft Store build runs under a generic name from its package folder.
    return _wcsicmp(name, L"Windows10Universal.exe") == 0 && StrStrIW(imagePath, L"ROBLOXCORPORATION");
}

UINT KeyUpsOnly(const INPUT* in, UINT count, INPUT* out) noexcept
{
    UINT kept = 0;
    for (UINT i = 0; i < count; ++i)
        if (in[i].type == INPUT_KEYBOARD && (in[i].ki.dwFlags & KEYEVENTF_KEYUP)) out[kept++] = in[i];
    return kept;
}

bool IsRobloxWindow(HWND window) noexcept
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (!process) return false;
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) return false;
    wchar_t path[MAX_PATH * 2];
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool roblox = QueryFullProcessImageNameW(handle, 0, path, &length) && IsRobloxImage(path);
    CloseHandle(handle);
    return roblox;
}

// Runs on every batch on the note thread, so the foreground-window verdict is
// cached until the foreground window changes, and the "Roblox is running"
// check is cached for one second.
bool IsShellWindow(HWND window) noexcept
{
    wchar_t name[64];
    if (!GetClassNameW(window, name, static_cast<int>(std::size(name)))) return false;
    for (const wchar_t* shell : { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW", L"MultitaskingViewFrame",
                                  L"XamlExplorerHostIslandWindow", L"ForegroundStaging", L"TaskSwitcherWnd", L"Windows.UI.Core.CoreWindow" })
        if (_wcsicmp(name, shell) == 0) return true;
    return false;
}

namespace {

Elevation ProcessElevation(HANDLE process) noexcept
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool opened = OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE;
    const bool denied = !opened && GetLastError() == ERROR_ACCESS_DENIED;
    const bool read = opened && GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    if (token) CloseHandle(token);
    return ReadElevation(true, read, read && elevation.TokenIsElevated, denied);
}

Elevation ElevationOf(DWORD process) noexcept
{
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) return ReadElevation(false, false, false, false);
    const Elevation elevation = ProcessElevation(handle);
    CloseHandle(handle);
    return elevation;
}

// The note thread and the window's thread both read the foreground.
SRWLOCK elevationLock = SRWLOCK_INIT;
ElevationCache elevations;

bool DroppedBy(DWORD process) noexcept
{
    static const Elevation self = ProcessElevation(GetCurrentProcess());
    if (!process || process == GetCurrentProcessId() || self == Elevation::Administrator) return false;
    AcquireSRWLockExclusive(&elevationLock);
    const Elevation target = elevations.Of(process, ElevationOf);
    ReleaseSRWLockExclusive(&elevationLock);
    return KeysDropped(target, self);
}

} // namespace

std::atomic<DWORD> KeysDroppedBy{0};

Foreground ForegroundNow() noexcept
{
    constexpr uintptr_t IsRoblox = uintptr_t{1} << 63, IsOwn = uintptr_t{1} << 62, IsShell = uintptr_t{1} << 61,
                        IsElevated = uintptr_t{1} << 60, Flags = IsRoblox | IsOwn | IsShell | IsElevated;
    static std::atomic<uintptr_t> front{0};
    const auto window = reinterpret_cast<uintptr_t>(GetForegroundWindow()) & ~Flags;
    uintptr_t known = front.load(std::memory_order_relaxed);
    if ((known & ~Flags) != window) {
        DWORD process = 0;
        GetWindowThreadProcessId(reinterpret_cast<HWND>(window), &process);
        known = window | (IsRobloxWindow(reinterpret_cast<HWND>(window)) ? IsRoblox : 0) |
                (window && process == GetCurrentProcessId() ? IsOwn : 0) |
                (window && IsShellWindow(reinterpret_cast<HWND>(window)) ? IsShell : 0) |
                (window && DroppedBy(process) ? IsElevated : 0);
        front.store(known, std::memory_order_relaxed);
    }
    Foreground result{ reinterpret_cast<HWND>(known & ~Flags), (known & IsRoblox) != 0, (known & IsOwn) != 0, (known & IsShell) != 0, true,
                       (known & IsElevated) != 0 };
    if (result.roblox) return result;

    static std::atomic<ULONGLONG> checked{0};
    static std::atomic<bool> running{false};
    const ULONGLONG now = GetTickCount64();
    if (now - checked.load(std::memory_order_relaxed) >= 1000) {
        bool found = false;
        for (HWND w = FindWindowExW(nullptr, nullptr, nullptr, L"Roblox"); w && !found;
             w = FindWindowExW(nullptr, w, nullptr, L"Roblox"))
            found = IsRobloxWindow(w);
        running.store(found, std::memory_order_relaxed);
        checked.store(now, std::memory_order_relaxed);
    }
    result.robloxRunning = running.load(std::memory_order_relaxed);
    return result;
}

Foreground (*ForegroundProbe)() noexcept = ForegroundNow;

namespace {

// Keys typed into a process that drops them are reported, once for it, and
// still sent: playback goes on either way.
bool KeysMayBeTyped() noexcept
{
    const Foreground front = ForegroundNow();
    const bool typed = front.roblox || !front.robloxRunning;
    if (typed && front.elevated) {
        DWORD process = 0;
        GetWindowThreadProcessId(front.window, &process);
        AcquireSRWLockExclusive(&elevationLock);
        const bool first = elevations.FirstReport(process);
        ReleaseSRWLockExclusive(&elevationLock);
        if (first) KeysDroppedBy.store(process, std::memory_order_release);
    }
    return typed;
}

} // namespace

static UINT __fastcall SendInputCall(ULONG cInputs, LPINPUT pInputs, int cbSize)
{
    if (KeysMayBeTyped() || !pInputs || cbSize != sizeof(INPUT))
        return ::SendInput(static_cast<UINT>(cInputs), pInputs, cbSize);

    // Withheld key-downs are not failures: report the whole batch as sent so
    // callers do not count a fault for every note played with the game behind.
    // They are counted as lost, so a velocity the game never got is sent again.
    INPUT ups[64];
    bool withheld = false;
    for (ULONG done = 0; done < cInputs; done += static_cast<ULONG>(std::size(ups))) {
        const UINT chunk = static_cast<UINT>((std::min)(cInputs - done, static_cast<ULONG>(std::size(ups))));
        const UINT kept = KeyUpsOnly(pInputs + done, chunk, ups);
        withheld |= kept < chunk;
        if (kept) ::SendInput(kept, ups, sizeof(INPUT));
    }
    if (withheld) InputsLost.fetch_add(1, std::memory_order_acq_rel);
    return static_cast<UINT>(cInputs);
}

std::atomic<uint64_t> InputsLost{0};

extern "C" UINT(__fastcall* InjectInput)(ULONG cInputs, LPINPUT pInputs, int cbSize) = SendInputCall;
