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

// While Roblox is running, key-downs are sent only when it is the foreground
// window, so notes do not type into other applications. Key-ups always go out
// so nothing is left held. With no Roblox process, nothing is withheld.
bool IsRobloxImage(const wchar_t* imagePath) noexcept;
bool IsRobloxWindow(HWND window) noexcept;
// Copies the key ups from in to out, which must hold count, and returns how many.
UINT KeyUpsOnly(const INPUT* in, UINT count, INPUT* out) noexcept;
#endif
