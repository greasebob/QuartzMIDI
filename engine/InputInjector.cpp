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

namespace {

// Runs on every batch on the note thread, so the foreground-window verdict is
// cached until the foreground window changes, and the "Roblox is running"
// check is cached for one second.
bool KeysMayBeTyped() noexcept
{
    constexpr uintptr_t IsRoblox = uintptr_t{1} << 63;
    static std::atomic<uintptr_t> front{0};
    const auto window = reinterpret_cast<uintptr_t>(GetForegroundWindow()) & ~IsRoblox;
    uintptr_t known = front.load(std::memory_order_relaxed);
    if ((known & ~IsRoblox) != window) {
        known = window | (IsRobloxWindow(reinterpret_cast<HWND>(window)) ? IsRoblox : 0);
        front.store(known, std::memory_order_relaxed);
    }
    if (known & IsRoblox) return true;

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
    return !running.load(std::memory_order_relaxed);
}

} // namespace

static UINT __fastcall SendInputCall(ULONG cInputs, LPINPUT pInputs, int cbSize)
{
    if (KeysMayBeTyped() || !pInputs || cbSize != sizeof(INPUT))
        return ::SendInput(static_cast<UINT>(cInputs), pInputs, cbSize);

    // Withheld key-downs are not failures: report the whole batch as sent so
    // callers do not count a fault for every note played with the game behind.
    INPUT ups[64];
    for (ULONG done = 0; done < cInputs; done += static_cast<ULONG>(std::size(ups))) {
        const UINT chunk = static_cast<UINT>((std::min)(cInputs - done, static_cast<ULONG>(std::size(ups))));
        const UINT kept = KeyUpsOnly(pInputs + done, chunk, ups);
        if (kept) ::SendInput(kept, ups, sizeof(INPUT));
    }
    return static_cast<UINT>(cInputs);
}

extern "C" UINT(__fastcall* InjectInput)(ULONG cInputs, LPINPUT pInputs, int cbSize) = SendInputCall;
