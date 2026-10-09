#pragma once
#include <windows.h>

#ifdef __cplusplus
#include "InputLatency.hpp"
extern "C" {
#endif

	// Every injected keystroke goes through this pointer, which calls
	// SendInput. Tests substitute an in-process recorder.
	extern UINT(__fastcall* InjectInput)(ULONG cInputs, LPINPUT pInputs, int cbSize);

#ifdef __cplusplus
}

#include <atomic>
#include <cstdint>
#include <utility>

// Sends that lost an input on the way: key-downs held back while Roblox is
// behind (below), or a SendInput that sent fewer than it was given
// (input_latency::send). After one, what the game was last told is unknown.
extern std::atomic<uint64_t> InputsLost;

// While Roblox is running, key-downs are sent only when it is the foreground
// window, so notes do not type into other applications. Key-ups always go out
// so nothing is left held. With no Roblox process, nothing is withheld.
bool IsRobloxImage(const wchar_t* imagePath) noexcept;
bool IsRobloxWindow(HWND window) noexcept;
// Copies the key ups from in to out, which must hold count, and returns how many.
UINT KeyUpsOnly(const INPUT* in, UINT count, INPUT* out) noexcept;

// The window in front; whether it is Roblox's, this process's own, or the
// taskbar, desktop or task switcher, passed through on the way to a window;
// and whether Roblox is running at all. Cheap enough for the note thread: the
// verdict is kept until the foreground window changes, and Roblox is looked
// for once a second. The key-down gate above uses the same check.
// `elevated`: Windows drops the keys sent to it (below).
struct Foreground { HWND window; bool roblox; bool own; bool shell; bool robloxRunning; bool elevated; };
Foreground ForegroundNow() noexcept;
// Playback reads the foreground through this pointer, which calls
// ForegroundNow. Tests substitute a window of their own.
extern Foreground (*ForegroundProbe)() noexcept;

// Windows drops keys sent to a process running as administrator from one that
// is not (UIPI), and SendInput reports no error. Each process in front is
// checked once, when it comes in front; the first keys typed into one whose
// keys are dropped leave its id here, once per process, for the window to report.
extern std::atomic<DWORD> KeysDroppedBy;
enum class Elevation : unsigned char { Unknown, Normal, Administrator };
// What a process's token says. A token this process may not read is unknown,
// not above it: anti-cheat guards a game's process the same way, and a false
// report on every song costs more than a missed one.
constexpr Elevation ReadElevation(bool opened, bool tokenRead, bool tokenElevated, bool tokenDenied) noexcept {
    (void)tokenDenied;
    if (!opened || !tokenRead) return Elevation::Unknown;
    return tokenElevated ? Elevation::Administrator : Elevation::Normal;
}
constexpr bool KeysDropped(Elevation target, Elevation self) noexcept {
    return target == Elevation::Administrator && self != Elevation::Administrator;
}
// Each process's elevation, read once, and whether it was reported; the
// oldest of the last few processes seen gives way to a new one.
class ElevationCache {
public:
    template <class Read> Elevation Of(DWORD process, Read&& read) {
        if (Entry* seen = Find(process)) return seen->elevation;
        entries_[next_] = {process, read(process), false};
        return entries_[std::exchange(next_, (next_ + 1) % kEntries)].elevation;
    }
    // True the first time for a process seen.
    bool FirstReport(DWORD process) noexcept {
        Entry* seen = Find(process);
        return seen && !std::exchange(seen->reported, true);
    }
private:
    struct Entry { DWORD process = 0; Elevation elevation = Elevation::Unknown; bool reported = false; };
    static constexpr size_t kEntries = 16;
    Entry* Find(DWORD process) noexcept {
        for (auto& entry : entries_) if (process && entry.process == process) return &entry;
        return nullptr;
    }
    Entry entries_[kEntries]{};
    size_t next_ = 0;
};
#endif
