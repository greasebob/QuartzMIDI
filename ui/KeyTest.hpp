#pragma once
#include "InputHeader.h"
#include <algorithm>
#include <string>

// Settings' key test: one harmless key sent to the app's own window, and the
// game's integrity level beside the app's, since Windows drops keys sent to a
// process of a higher level (UIPI) without saying so.
namespace shell {
enum class KeyTestOutcome { Arrived, Lost, NoFocus };

// A process's integrity level as its SECURITY_MANDATORY_*_RID, or 0 when it
// cannot be read.
inline DWORD ProcessIntegrity(DWORD process) {
    DWORD level = 0;
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) return 0;
    HANDLE token = nullptr;
    if (OpenProcessToken(handle, TOKEN_QUERY, &token)) {
        alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[TOKEN_INTEGRITY_LEVEL_MAX_SIZE]{};
        DWORD length = 0;
        if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &length)) {
            const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
            level = *GetSidSubAuthority(sid, static_cast<DWORD>(*GetSidSubAuthorityCount(sid) - 1));
        }
        CloseHandle(token);
    }
    CloseHandle(handle);
    return level;
}

// The highest integrity level among the running Roblox windows' processes, or
// 0 when none runs or none can be read.
inline DWORD RobloxIntegrity() {
    DWORD highest = 0;
    for (HWND window = FindWindowExW(nullptr, nullptr, nullptr, L"Roblox"); window;
         window = FindWindowExW(nullptr, window, nullptr, L"Roblox")) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process && IsRobloxWindow(window)) highest = std::max(highest, ProcessIntegrity(process));
    }
    return highest;
}

// Whether keys will reach the game: `ours` and `game` are integrity levels,
// `game` 0 when no game was found or its level cannot be read.
inline bool KeyTestPassed(KeyTestOutcome outcome, DWORD ours, DWORD game) {
    return outcome == KeyTestOutcome::Arrived && !(ours && game > ours);
}

// The line Settings shows for a test.
inline std::string KeyTestResult(KeyTestOutcome outcome, DWORD ours, DWORD game) {
    if (outcome == KeyTestOutcome::NoFocus) return "Another window has the keyboard.";
    if (outcome == KeyTestOutcome::Lost) return "Windows blocked the keys.";
    if (!KeyTestPassed(outcome, ours, game))
        return game >= SECURITY_MANDATORY_HIGH_RID ? "Windows blocks the keys: Roblox runs as administrator." : "Windows blocks the keys to Roblox.";
    return "Keys arrive.";
}
}
