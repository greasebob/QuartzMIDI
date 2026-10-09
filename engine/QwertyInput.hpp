#pragma once

// The computer keyboard as a MIDI input source. While Roblox is in front, a
// low-level keyboard hook takes each mapped key the player types and plays its
// note through the live path instead, so Octaves and the 88-key layout apply
// to typing as they do to a MIDI keyboard. Every other key, and every key
// while anything else is in front, reaches its window untouched.

#include "MidiInput.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>

inline constexpr wchar_t kQwertyDeviceId[] = L"qwerty:keyboard";
inline constexpr wchar_t kQwertyPrefix[] = L"qwerty:";

std::unique_ptr<IMidiInput> CreateQwertyInput();

// Set 1 scancode to note for each modifier the player holds, -1 for a key that
// plays nothing. A key with no Shift or Ctrl binding plays its plain note with
// that modifier held. armed is false while live play is off, and then every
// key passes through.
struct QwertyNoteMap {
    std::array<int16_t, 128> plain{};
    std::array<int16_t, 128> shift{};
    std::array<int16_t, 128> ctrl{};
    bool armed = false;

    QwertyNoteMap() { plain.fill(-1); shift.fill(-1); ctrl.fill(-1); }
};

// Builds the map from a key mapping table (note name to key, such as "C4" to
// "t", "C#4" to "T" or "A0" to "ctrl+1"). When several notes share a key, the
// one nearest the middle of the keyboard plays.
QwertyNoteMap QwertyNoteMapFrom(const std::map<std::string, std::string>& keyMappings);

// Safe to call while the hook runs: it takes the new map on its next key.
void SetQwertyNoteMap(const QwertyNoteMap& map);

// Velocity of a typed note. Typed notes send no velocity taps, so this reaches
// only a MIDI output target.
inline constexpr uint8_t kQwertyVelocity = 100;

// State for the hook, with no Windows dependency, so the swallowing rules can
// be unit tested.
struct QwertyKeyState {
    // Whether Shift and Ctrl are held, and whether the hook kept them from
    // the game; a modifier's key-up follows its key-down.
    bool shift = false, ctrl = false, alt = false;
    bool shiftKept = false, ctrlKept = false;
    // After '/', the game's chat box has the keys until Enter or Esc.
    bool chat = false;
    // Per scancode: 0 up, 1 down and passed through, 2 down and kept.
    std::array<uint8_t, 128> held{};
    // The note each kept key is sounding, or -1. Stores the note actually
    // sent, because Shift can change while the key is held.
    std::array<int16_t, 128> sounding{};

    QwertyKeyState() { sounding.fill(-1); }
};

struct QwertyKeyResult {
    bool swallow = false; // keep the key from every window
    int note = -1;        // a note to start or end, or -1
    bool on = false;
};

// One physical key event. vk is the virtual key, scan the set 1 scancode,
// extended whether it carries the E0 prefix. robloxFront is whether Roblox is
// the window in front. Injected keys never come here.
QwertyKeyResult QwertyKeyStep(QwertyKeyState& state, const QwertyNoteMap& map,
                              uint32_t vk, uint32_t scan, bool extended, bool up,
                              bool robloxFront);
