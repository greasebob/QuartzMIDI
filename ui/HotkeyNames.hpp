#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
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
//
// The app's later keys follow the performer's places, so those keep the
// indices and ids earlier builds gave them. Each defaults to unbound.
inline constexpr size_t kAppHotkeys = 6, kAddonHotkeys = 16;
inline constexpr size_t kFirstLaterHotkey = kAppHotkeys + kAddonHotkeys;
inline constexpr std::array<const char*, 4> kLaterHotkeyFields{"PANIC_KEY", "SPEED_UP_KEY", "SPEED_DOWN_KEY", "SHOW_HIDE_KEY"};
inline constexpr size_t kPanicHotkey = kFirstLaterHotkey;
inline constexpr size_t kSpeedUpHotkey = kFirstLaterHotkey + 1, kSpeedDownHotkey = kFirstLaterHotkey + 2;
inline constexpr size_t kShowHideHotkey = kFirstLaterHotkey + 3;
inline constexpr size_t kHotkeys = kFirstLaterHotkey + kLaterHotkeyFields.size();
inline constexpr std::array<const char*, kAppHotkeys> kHotkeyFields{
    "PLAY_PAUSE_KEY", "REWIND_KEY", "SKIP_KEY", "EMERGENCY_EXIT_KEY", "PREVIOUS_SONG_KEY", "NEXT_SONG_KEY"};
inline constexpr std::array<const char*, kAppHotkeys> kHotkeyDefaults{"VK_F1", "VK_F2", "VK_F3", "VK_F4", "", ""};
// True for a place a performer's trigger keys may take.
inline constexpr bool IsPerformerHotkey(size_t i) { return i >= kAppHotkeys && i < kFirstLaterHotkey; }

// Named keys without the VK_ prefix; letters, digits and F-keys are computed.
// Escape (cancels capture), bare modifiers and the left, right and middle
// buttons (a click on a control) are excluded; the side buttons are keys.
inline constexpr std::pair<const char*, int> kNamedKeys[]{
    {"XBUTTON1", VK_XBUTTON1}, {"XBUTTON2", VK_XBUTTON2},
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

// A mouse side button, which RegisterHotKey cannot take; the shell reads it as raw input.
inline constexpr bool IsMouseHotkey(int vk) { return vk == VK_XBUTTON1 || vk == VK_XBUTTON2; }

// Whether the shell registers `vk` under modifier mix `mix` (1 Shift, 2 Ctrl,
// 4 Alt). Alt+F4 is left to Windows unless `blockAltF4`: while playing, velocity
// holds Alt, so an F4 hotkey pressed then would close the game.
inline constexpr bool RegistersMix(int vk, int mix, bool blockAltF4) {
    return blockAltF4 || !(vk == VK_F4 && (mix & 4));
}

// A key another program has registered cannot be registered again, so the shell
// reads it as raw input instead, and the press still reaches the game. Whether
// a raw press of `vk` held with modifier mix `mix` (1 Shift, 2 Ctrl, 4 Alt)
// fires: not where RegisterHotKey took that mix (`registered`, a bit per mix),
// with modifiers only while typing, as the registered keys are then, and never
// Alt+F4, which closes the window in front.
inline constexpr bool RawHotkeyFires(int vk, unsigned registered, int mix, bool typing) {
    if (registered & (1u << mix)) return false;
    if (mix != 0 && !typing) return false;
    return !(vk == VK_F4 && (mix & 4));
}

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

// Set 1 scan codes of the keys the app types notes and velocity taps on: the
// digit row and the three letter rows of a US keyboard, by position.
inline constexpr unsigned char kTypedScans[]{
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
    0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32};

// The key map stores a key by its US name, which is a position: "q" is the key
// right of Tab and "Q" that key with Shift, and the app types that position
// whatever the layout, since the game reads positions. Every printable key of
// a US keyboard, by scan code, plain and with Shift.
struct KeyPosition { unsigned char scan = 0; bool shift = false; };
inline constexpr struct { unsigned char scan; char plain, shifted; } kUsKeys[]{
    {0x02, '1', '!'}, {0x03, '2', '@'}, {0x04, '3', '#'}, {0x05, '4', '$'}, {0x06, '5', '%'}, {0x07, '6', '^'},
    {0x08, '7', '&'}, {0x09, '8', '*'}, {0x0A, '9', '('}, {0x0B, '0', ')'}, {0x0C, '-', '_'}, {0x0D, '=', '+'},
    {0x10, 'q', 'Q'}, {0x11, 'w', 'W'}, {0x12, 'e', 'E'}, {0x13, 'r', 'R'}, {0x14, 't', 'T'}, {0x15, 'y', 'Y'},
    {0x16, 'u', 'U'}, {0x17, 'i', 'I'}, {0x18, 'o', 'O'}, {0x19, 'p', 'P'}, {0x1A, '[', '{'}, {0x1B, ']', '}'},
    {0x1E, 'a', 'A'}, {0x1F, 's', 'S'}, {0x20, 'd', 'D'}, {0x21, 'f', 'F'}, {0x22, 'g', 'G'}, {0x23, 'h', 'H'},
    {0x24, 'j', 'J'}, {0x25, 'k', 'K'}, {0x26, 'l', 'L'}, {0x27, ';', ':'}, {0x28, '\'', '"'}, {0x29, '`', '~'},
    {0x2B, '\\', '|'}, {0x2C, 'z', 'Z'}, {0x2D, 'x', 'X'}, {0x2E, 'c', 'C'}, {0x2F, 'v', 'V'}, {0x30, 'b', 'B'},
    {0x31, 'n', 'N'}, {0x32, 'm', 'M'}, {0x33, ',', '<'}, {0x34, '.', '>'}, {0x35, '/', '?'}};
inline bool UsPosition(char name, KeyPosition& position) {
    for (const auto& key : kUsKeys)
        if (key.plain == name || key.shifted == name) { position = {key.scan, key.shifted == name}; return true; }
    return false;
}
inline char UsName(KeyPosition position) {
    for (const auto& key : kUsKeys) if (key.scan == position.scan) return position.shift ? key.shifted : key.plain;
    return 0;
}
inline std::string Utf8Of(const std::wstring& text) {
    if (text.empty()) return {};
    std::string out(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    return out;
}
// A stored key as the user's layout names it: on AZERTY "q" shows as "a" and
// "Q" as "A". A modifier before the key ("ctrl+2") is kept. typed(scan, shift)
// gives what the layout types at that position, empty for nothing.
template <class Typed> std::string ShownKey(const std::string& stored, Typed typed) {
    if (stored.empty()) return stored;
    KeyPosition position;
    if (!UsPosition(stored.back(), position)) return stored;
    const std::wstring name = typed(position.scan, position.shift);
    if (name.empty()) return stored;
    return stored.substr(0, stored.size() - 1) + Utf8Of(name);
}
// The stored name of a character typed under the user's layout, by the key it
// is on: on AZERTY "a" is stored as "q". position(character) finds the key and
// Shift the layout types it with; empty when no plain or shifted key does.
template <class Position> std::string StoredKey(wchar_t typed, Position position) {
    KeyPosition key;
    if (!position(typed, key)) return {};
    const char name = UsName(key);
    return name ? std::string(1, name) : std::string();
}
// The same under the keyboard layout in use.
inline std::string ShownKey(const std::string& stored) {
    const HKL layout = GetKeyboardLayout(0);
    return ShownKey(stored, [layout](unsigned char scan, bool shift) {
        BYTE keys[256]{};
        if (shift) keys[VK_SHIFT] = 0x80;
        const UINT vk = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, layout);
        wchar_t text[8]{};
        // 4: leave the keyboard's state, a dead key's included, as it was.
        const int count = vk ? ToUnicodeEx(vk, scan, keys, text, 8, 4, layout) : 0;
        return std::wstring(text, count < 0 ? 1 : static_cast<size_t>(std::max(count, 0)));
    });
}
inline std::string StoredKey(wchar_t typed) {
    const HKL layout = GetKeyboardLayout(0);
    return StoredKey(typed, [layout](wchar_t character, KeyPosition& key) {
        const SHORT found = VkKeyScanExW(character, layout);
        // Only Shift: a character AltGr or Ctrl types is not a position the game reads.
        if (found == -1 || (found >> 8) & ~1) return false;
        key = {static_cast<unsigned char>(MapVirtualKeyExW(found & 0xff, MAPVK_VK_TO_VSC, layout)), ((found >> 8) & 1) != 0};
        return key.scan != 0;
    });
}

// True for a key the game plays or the app types: every letter and digit (the
// notes and the velocity taps), Space and the arrows (sustain, volume and
// transpose), and the key of each binding in keyMappings (values such as "q",
// "E" or "ctrl+2"). Bound as a hotkey, the app's own typing would press it.
// The app types by scan code, so the game reads a key's position whatever the
// layout, but the layout names the key a hotkey is: on AZERTY the M position
// is the comma key, and a comma hotkey took C7 from the game. layoutVK gives
// the virtual key the layout sends for a scan code.
template <class KeyMappings, class LayoutVK> bool IsNoteKey(int vk, const KeyMappings& keyMappings, LayoutVK layoutVK) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return true;
    if (vk == VK_SPACE || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) return true;
    for (const unsigned char scan : kTypedScans) if (static_cast<int>(layoutVK(scan)) == vk) return true;
    // A binding is a US position, which the layout names as it names the rows above.
    for (const auto& binding : keyMappings) {
        const auto& key = binding.second;
        KeyPosition position;
        if (key.empty()) continue;
        if (UsPosition(key.back(), position) ? static_cast<int>(layoutVK(position.scan)) == vk
                                             : (VkKeyScanW(static_cast<unsigned char>(key.back())) & 0xff) == vk) return true;
    }
    return false;
}
// Under the keyboard layout in use.
template <class KeyMappings> bool IsNoteKey(int vk, const KeyMappings& keyMappings) {
    return IsNoteKey(vk, keyMappings, [](unsigned char scan) { return static_cast<int>(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX)); });
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
    // Returns the newly pressed key, Cancelled for Escape, or None. A key
    // `refused` names is passed over, and capture keeps listening.
    template <class Down, class Refused> int Poll(Down down, Refused refused) {
        int pressed = None;
        refused_ = false;
        for (int vk = 1; vk < 256; ++vk) {
            const bool now = down(vk);
            if (!now) { held_[vk] = false; continue; }
            if (held_[vk] || pressed != None) continue;
            if (vk == VK_ESCAPE) pressed = Cancelled;
            else if (VKToName(vk).empty()) continue;
            else if (refused(vk)) held_[vk] = refused_ = true;   // passed over until released
            else pressed = vk;
        }
        return pressed;
    }
    template <class Down> int Poll(Down down) { return Poll(down, [](int) { return false; }); }
    // True when the last Poll passed over a key just pressed, once per press,
    // so the capture can show the key was refused.
    bool Refused() const { return refused_; }
private:
    std::array<bool, 256> held_{};
    bool refused_ = false;
};

// Display label for a key. OEM keys are positional, so they show the character
// the current keyboard layout maps there.
inline std::string HotkeyLabel(std::string name) {
    const int vk = NameToVK(name);
    if (name.rfind("VK_", 0) == 0) name.erase(0, 3);
    switch (vk) {
    case VK_XBUTTON1:         return "Mouse 4";
    case VK_XBUTTON2:         return "Mouse 5";
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
    case VK_SPACE:            return "Space";
    case VK_TAB:              return "Tab";
    case VK_PAUSE:            return "Pause";
    case VK_LEFT:             return "Left";
    case VK_RIGHT:            return "Right";
    case VK_UP:               return "Up";
    case VK_DOWN:             return "Down";
    case VK_HOME:             return "Home";
    case VK_END:              return "End";
    case VK_INSERT:           return "Insert";
    case VK_DELETE:           return "Delete";
    case VK_SCROLL:           return "Scroll Lock";
    case VK_NUMLOCK:          return "Num Lock";
    case VK_APPS:             return "Menu";
    case VK_BROWSER_BACK:     return "Browser Back";
    // "Browser Forward" is wider than the Settings keycap at the largest text size.
    case VK_BROWSER_FORWARD:  return "Browser Fwd";
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

// Display label for a set 1 scancode, 0xE0 in the high byte for an extended
// key, as the keyboard layout names it. Empty for 0.
inline std::string ScancodeLabel(int scancode) {
    if (scancode <= 0) return {};
    LONG lParam = (scancode & 0xFF) << 16;
    if ((scancode & 0xFF00) == 0xE000) lParam |= 1 << 24;
    wchar_t name[64]{};
    const int length = GetKeyNameTextW(lParam, name, 64);
    char utf8[256]{};
    const int written = length > 0 ? WideCharToMultiByte(CP_UTF8, 0, name, length, utf8, sizeof(utf8), nullptr, nullptr) : 0;
    if (written <= 0) {
        char code[16];
        snprintf(code, sizeof(code), "Key %X", scancode);
        return code;
    }
    std::string label(utf8, static_cast<size_t>(written));
    // Some layouts name keys in capitals ("SPACE"); show them as the hotkeys are.
    if (label.size() > 1 && std::all_of(label.begin(), label.end(), [](unsigned char c) { return !std::islower(c); }))
        std::transform(label.begin() + 1, label.end(), label.begin() + 1,
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return label;
}
}
