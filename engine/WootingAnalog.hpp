#pragma once

// Wooting analog keys as a MIDI input source, read directly from the Wooting
// analog SDK instead of through wooting-analog-midi and a teVirtualMIDI port.
//
// The SDK is loaded with LoadLibrary at runtime, so the app starts without it
// and needs no import library or vendored headers. The SDK is MPL-2.0, which
// is GPLv3-compatible.

#include "MidiInput.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>

std::unique_ptr<IMidiInput> CreateWootingAnalogInput();

// True when wooting_analog_sdk.dll is present and reports at least one device.
bool WootingAnalogAvailable();

// Scancode (set 1) to MIDI note, -1 for keys that are not notes. The default
// follows the virtual-piano layout; replace it to follow the user's mapping.
void SetWootingScancodeNoteMap(const std::array<int16_t, 256>& map);
std::array<int16_t, 256> DefaultWootingScancodeNoteMap();

// Builds the map from a key mapping table (note name to key), so a Wooting key
// plays the note the app would type for it. Only unshifted single-character
// bindings map: the analog SDK sees a shifted key as two physical keys.
std::array<int16_t, 256> WootingScancodeNoteMapFrom(
    const std::map<std::string, std::string>& keyMappings);

// How key travel becomes notes, mirroring wooting-analog-midi's controls.
// Defaults match config.hpp, which stores these as WOOTING_ANALOG and
// documents each field and its range.
struct WootingAnalogSettings {
    float trigger = 0.25f;
    float releaseFraction = 0.6f;
    int shiftAmount = 1;
    float velocityScale = 2.0f;
};

// Safe to call while a device is open: the poll loop reads a copy each pass.
void SetWootingAnalogSettings(const WootingAnalogSettings& settings);
WootingAnalogSettings GetWootingAnalogSettings();

// Set 1 scancode of the shift key, the only non-note key the backend reads.
inline constexpr uint16_t kWootingShiftScancode = 0x2A; // Left Shift

// True while the open device's shift key is down. Wooting profiles that stop
// note keys from typing usually leave Left Shift active, so the keystroke path
// checks this and lifts Shift in the same batch as the note.
bool WootingShiftHeld() noexcept;

// MIDI velocity for a key that travelled from previousDepth to depth in the
// given number of seconds. Public so tests can drive it without a keyboard.
uint8_t WootingVelocityFor(float depth, float previousDepth, double seconds, float velocityScale);

// How far back strike speed looks, and how little travel in that time counts as
// a key that has stopped.
inline constexpr double kWootingStrikeWindow = 0.010;
inline constexpr float kWootingRestingTravel = 0.01f;

// State for one poll of the analog buffer, with no SDK or clock dependency, so
// the trigger and release hysteresis, shifted notes and note-off matching can
// be unit tested.
struct WootingPollState {
    std::array<float, 256> lastDepth{};
    // The note each key is sounding, or -1 if up. Stores the note actually
    // sent, because the shift can change while the key is held and the
    // note-off must match its note-on.
    std::array<int16_t, 256> sounding{};
    // Where each key was 10 to 20 ms ago; strike speed is measured against
    // it. Travel since the previous 1 ms poll is too coarse, since keyboard
    // reports arrive irregularly. recent becomes older every
    // kWootingStrikeWindow; a key that has stopped moving keeps its anchor at
    // the present, so a partly pressed key is measured from where it rested.
    struct Anchor { float depth = 0.0f; double age = 0.0; };
    std::array<Anchor, 256> recent{};
    std::array<Anchor, 256> older{};
    std::array<bool, 256> resting{};
    // Whether the shift key was past the trigger in the last poll.
    bool shiftHeld = false;

    WootingPollState() { lastDepth.fill(0.0f); sounding.fill(-1); }
};

struct WootingPollEvent {
    bool on = false;
    uint8_t note = 0;
    uint8_t velocity = 0;   // 0 on a note off
};

// codes and values are the SDK's buffer for this poll, count its length.
// seconds is the time since the previous poll, used only for strike speed.
// Writes at most 2 * count + 256 events, so out must hold that; returns how
// many were written. Allocates nothing: this runs at 1kHz on its own thread.
size_t WootingPollStep(WootingPollState& state,
                       const uint16_t* codes, const float* values, int count,
                       const std::array<int16_t, 256>& noteMap,
                       const WootingAnalogSettings& settings,
                       double seconds,
                       WootingPollEvent* out, size_t outCapacity);
