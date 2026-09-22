#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>

namespace shell {
// Global hotkeys, in snapshot/legend/Settings order. The first four match
// upstream's fields and defaults; the song keys default to unbound. No media
// key is a default, since RegisterHotKey takes it from every other application.
//
// Slots after kAppHotkeys belong to a performer add-on's declared triggers, in
// declaration order. Note keys are excluded because the app's own typing would
// press them. These are registered like the others (so the game never sees
// them) but the engine polls them for key-up, which WM_HOTKEY doesn't report.
// Each is registered only while its trigger is selected.
inline constexpr size_t kAppHotkeys = 6, kAddonHotkeys = 16;
inline constexpr size_t kHotkeys = kAppHotkeys + kAddonHotkeys;
inline constexpr std::array<const char*, kAppHotkeys> kHotkeyFields{
    "PLAY_PAUSE_KEY", "REWIND_KEY", "SKIP_KEY", "EMERGENCY_EXIT_KEY", "PREVIOUS_SONG_KEY", "NEXT_SONG_KEY"};
inline constexpr std::array<const char*, kAppHotkeys> kHotkeyDefaults{"VK_F1", "VK_F2", "VK_F3", "VK_F4", "", ""};

// Named keys without the VK_ prefix; letters, digits and F-keys are computed.
// Escape (cancels capture), bare modifiers and mouse buttons are excluded.
inline constexpr std::pair<const char*, int> kNamedKeys[]{
    {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"PAUSE", VK_PAUSE}, {"BACK", VK_BACK}, {"RETURN", VK_RETURN},
    {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
    {"HOME", VK_HOME}, {"END", VK_END}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},
    {"PRIOR", VK_PRIOR}, {"NEXT", VK_NEXT},
    {"CAPITAL", VK_CAPITAL}, {"SCROLL", VK_SCROLL}, {"NUMLOCK", VK_NUMLOCK}, {"SNAPSHOT", VK_SNAPSHOT}, {"APPS", VK_APPS},
    {"NUMPAD0", VK_NUMPAD0}, {"NUMPAD1", VK_NUMPAD1}, {"NUMPAD2", VK_NUMPAD2}, {"NUMPAD3", VK_NUMPAD3},
    {"NUMPAD4", VK_NUMPAD4}, {"NUMPAD5", VK_NUMPAD5}, {"NUMPAD6", VK_NUMPAD6}, {"NUMPAD7", VK_NUMPAD7},
    {"NUMPAD8", VK_NUMPAD8}, {"NUMPAD9", VK_NUMPAD9},
    {"MULTIPLY", VK_MULTIPLY}, {"ADD", VK_ADD}, {"SUBTRACT", VK_SUBTRACT}, {"DECIMAL", VK_DECIMAL}, {"DIVIDE", VK_DIVIDE},
    {"OEM_1", VK_OEM_1}, {"OEM_2", VK_OEM_2}, {"OEM_3", VK_OEM_3}, {"OEM_4", VK_OEM_4}, {"OEM_5", VK_OEM_5},
    {"OEM_6", VK_OEM_6}, {"OEM_7", VK_OEM_7}, {"OEM_8", VK_OEM_8}, {"OEM_102", VK_OEM_102},
    {"OEM_PLUS", VK_OEM_PLUS}, {"OEM_COMMA", VK_OEM_COMMA}, {"OEM_MINUS", VK_OEM_MINUS}, {"OEM_PERIOD", VK_OEM_PERIOD},
    {"MEDIA_PLAY_PAUSE", VK_MEDIA_PLAY_PAUSE}, {"MEDIA_STOP", VK_MEDIA_STOP},
    {"MEDIA_NEXT_TRACK", VK_MEDIA_NEXT_TRACK}, {"MEDIA_PREV_TRACK", VK_MEDIA_PREV_TRACK},
    {"VOLUME_MUTE", VK_VOLUME_MUTE}, {"VOLUME_DOWN", VK_VOLUME_DOWN}, {"VOLUME_UP", VK_VOLUME_UP},
    {"BROWSER_BACK", VK_BROWSER_BACK}, {"BROWSER_FORWARD", VK_BROWSER_FORWARD},
};

// Parses a config name such as "VK_F1". Returns 0 for unknown names.
inline int NameToVK(std::string name) {
    if (name.rfind("VK_", 0) == 0) name.erase(0, 3);
    if (name.empty()) return 0;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (name.size() == 1 && (std::isalnum(static_cast<unsigned char>(name[0])) != 0))
        return static_cast<unsigned char>(name[0]);
    if (name[0] == 'F' && name.size() <= 3) {
        const int number = std::atoi(name.c_str() + 1);
        if (number >= 1 && number <= 24) return VK_F1 + number - 1;
    }
    for (const auto& [known, vk] : kNamedKeys) if (name == known) return vk;
    return 0;
}

// Config name for a virtual key, or empty if it can't be a hotkey.
inline std::string VKToName(int vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string("VK_") + static_cast<char>(vk);
    if (vk >= VK_F1 && vk <= VK_F24) return "VK_F" + std::to_string(vk - VK_F1 + 1);
    for (const auto& [known, value] : kNamedKeys) if (value == vk) return std::string("VK_") + known;
    return {};
}

// Captures the next key for a rebind by polling (with hotkeys unregistered),
// because registered hotkeys never produce WM_KEYDOWN and Settings may be a
// separate OS window. `down` is GetAsyncKeyState in the app, a table in tests.
class HotkeyCapture {
public:
    static constexpr int None = 0, Cancelled = -1;
    // Keys already down (e.g. the click or Enter that started capture) are
    // ignored until released.
    template <class Down> void Begin(Down down) {
        for (int vk = 1; vk < 256; ++vk) held_[vk] = down(vk);
    }
    // Returns the newly pressed key, Cancelled for Escape, or None.
    template <class Down> int Poll(Down down) {
        int pressed = None;
        for (int vk = 1; vk < 256; ++vk) {
            const bool now = down(vk);
            if (!now) { held_[vk] = false; continue; }
            if (held_[vk] || pressed != None) continue;
            if (vk == VK_ESCAPE) pressed = Cancelled;
            else if (!VKToName(vk).empty()) pressed = vk;
        }
        return pressed;
    }
private:
    std::array<bool, 256> held_{};
};

// Display label for a key. OEM keys are positional, so they show the character
// the current keyboard layout maps there.
inline std::string HotkeyLabel(std::string name) {
    const int vk = NameToVK(name);
    if (name.rfind("VK_", 0) == 0) name.erase(0, 3);
    switch (vk) {
    case VK_MEDIA_PLAY_PAUSE: return "Media Play";
    case VK_MEDIA_STOP:       return "Media Stop";
    case VK_MEDIA_NEXT_TRACK: return "Media Next";
    case VK_MEDIA_PREV_TRACK: return "Media Prev";
    case VK_VOLUME_MUTE:      return "Mute";
    case VK_VOLUME_DOWN:      return "Vol -";
    case VK_VOLUME_UP:        return "Vol +";
    case VK_PRIOR:            return "PgUp";
    case VK_NEXT:             return "PgDn";
    case VK_CAPITAL:          return "Caps";
    case VK_SNAPSHOT:         return "PrtSc";
    case VK_RETURN:           return "Enter";
    case VK_BACK:             return "Backspace";
    case VK_MULTIPLY:         return "Num *";
    case VK_ADD:              return "Num +";
    case VK_SUBTRACT:         return "Num -";
    case VK_DECIMAL:          return "Num .";
    case VK_DIVIDE:           return "Num /";
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num " + std::to_string(vk - VK_NUMPAD0);
    if (name.rfind("OEM_", 0) == 0) {
        const UINT character = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0x7fff;
        if (character > 32 && character < 127) return std::string(1, static_cast<char>(character));
    }
    return name;
}
}
