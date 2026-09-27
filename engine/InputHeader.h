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
struct Foreground { HWND window; bool roblox; bool own; bool shell; bool robloxRunning; };
Foreground ForegroundNow() noexcept;
// Playback reads the foreground through this pointer, which calls
// ForegroundNow. Tests substitute a window of their own.
extern Foreground (*ForegroundProbe)() noexcept;
#endif
