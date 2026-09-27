#pragma once

// MidiOutput: one interface over every MIDI output transport, mirroring
// IMidiInput (see MidiInput.hpp for the opaque-id rules). WinRT, WinMM and a
// named port of the app's own; output has no latency-critical hop for Kernel
// Streaming to remove.

#define NOMINMAX

#include "MidiInput.hpp"   // MidiBackend and MidiInputDevice

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class IMidiOutput {
public:
    virtual ~IMidiOutput() = default;

    virtual MidiBackend backend() const noexcept = 0;

    // Shares the input side's row type so one picker serves both.
    virtual std::vector<MidiInputDevice> enumerate() = 0;

    // Opening replaces any port this instance already had open.
    virtual bool open(const std::wstring& deviceId) = 0;
    virtual void close() = 0;

    virtual bool isOpen() const noexcept = 0;
    virtual const std::wstring& openedDeviceId() const noexcept = 0;

    // Whether the last open failed because another program holds the port,
    // as a synthesizer with one client does.
    virtual bool busy() const noexcept { return false; }

    // Called from the MIDI callback thread and the playback thread; never
    // allocates. Takes a mutex held only for the write, so two threads cannot
    // interleave bytes into one port. It is uncontended unless live input and
    // autoplay run at once.
    virtual void send(const uint8_t* message, size_t length) = 0;
};

std::unique_ptr<IMidiOutput> CreateMidiOutput(MidiBackend backend);

// Device list for the UI. Each id encodes its backend.
std::vector<MidiInputDevice> EnumerateMidiOutputs();

// Test hook: substitutes the list EnumerateMidiOutputs returns, as
// SetMidiInputEnumerator does for inputs. Passing {} restores the real
// transports. Not synchronised: set it before a scan.
using MidiOutputEnumerator = std::function<std::vector<MidiInputDevice>()>;
void SetMidiOutputEnumerator(MidiOutputEnumerator enumerator);

// Which backend produced this id. Defaults to WinRT for empty or unknown ids.
MidiBackend BackendForOutputId(const std::wstring& deviceId);

// A port the app creates under a name the user picks, through teVirtualMIDI,
// the driver loopMIDI installs. Other apps see it as a MIDI input with that
// name, so a player that labels ports by name does not call it virtual.
// Available only where loopMIDI is installed; nothing ships with the app.
bool NamedMidiPortAvailable();
// The output id that opens a port with this name; the name is kept to what a
// WinMM port name holds, without leading or trailing spaces.
std::wstring NamedMidiPortId(const std::wstring& name);
std::wstring NamedMidiPortName(const std::wstring& deviceId);
constexpr size_t kNamedMidiPortLength = 31;
// Whether an input of this name is a port this process has open, which
// EnumerateMidiInputs leaves out so live input cannot play into itself.
bool IsOwnMidiPort(const std::wstring& name);

// Test hook, like SetMidiInputFactory. Passing {} restores the real backends.
// Not synchronised: set it before opening a device.
using MidiOutputFactory = std::function<std::unique_ptr<IMidiOutput>(MidiBackend)>;
void SetMidiOutputFactory(MidiOutputFactory factory);

// The MIDI number for a note name such as "C4" or "A#3", or -1 if invalid.
// Inverse of NOTE_NAME_CACHE in MIDI2Key.cpp; a test round-trips all 128 notes.
int MidiNumberForNoteName(const char* name) noexcept;
