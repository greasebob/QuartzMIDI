#pragma once

// MidiOutput: one interface over every MIDI output transport, mirroring
// IMidiInput (see MidiInput.hpp for the opaque-id rules). WinRT and WinMM
// only; output has no latency-critical hop for Kernel Streaming to remove.

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

    // Called from the MIDI callback thread and the playback thread; never
    // allocates. Takes a mutex held only for the write, so two threads cannot
    // interleave bytes into one port. It is uncontended unless live input and
    // autoplay run at once.
    virtual void send(const uint8_t* message, size_t length) = 0;
};

std::unique_ptr<IMidiOutput> CreateMidiOutput(MidiBackend backend);

// Device list for the UI. Each id encodes its backend.
std::vector<MidiInputDevice> EnumerateMidiOutputs();

// Which backend produced this id. Defaults to WinRT for empty or unknown ids.
MidiBackend BackendForOutputId(const std::wstring& deviceId);

// Test hook, like SetMidiInputFactory. Passing {} restores the real backends.
// Not synchronised: set it before opening a device.
using MidiOutputFactory = std::function<std::unique_ptr<IMidiOutput>(MidiBackend)>;
void SetMidiOutputFactory(MidiOutputFactory factory);

// The MIDI number for a note name such as "C4" or "A#3", or -1 if invalid.
// Inverse of NOTE_NAME_CACHE in MIDI2Key.cpp; a test round-trips all 128 notes.
int MidiNumberForNoteName(const char* name) noexcept;
