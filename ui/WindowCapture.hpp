#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace shell {
// Windows leaves a window out of screenshots, recordings and screen sharing from
// Windows 10 version 2004 (build 19041); before that the affinity would only
// black it out, so Hide from screen capture is not offered there.
inline bool CaptureExclusionOffered() {
    using Version = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto version = reinterpret_cast<Version>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW info{sizeof info};
    return version && version(&info) == 0 &&
        (info.dwMajorVersion > 10 || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 19041));
}

// Sets a window's display affinity and reads it back: true when it took.
inline bool ApplyAffinity(HWND window, DWORD affinity) {
    DWORD now = WDA_NONE;
    if (GetWindowDisplayAffinity(window, &now) && now == affinity) return true;
    SetWindowDisplayAffinity(window, affinity);
    return GetWindowDisplayAffinity(window, &now) && now == affinity;
}
}
