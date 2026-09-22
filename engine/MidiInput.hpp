#pragma once

// MidiInput: one interface over every MIDI input transport. Devices are
// identified by an opaque string id that only the producing backend parses;
// transports number their ports differently, so indices are not portable.

#define NOMINMAX

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class MidiBackend {
    WinRT,   // Windows.Devices.Midi, Windows 10+
    WinMM,   // legacy midiIn*, through the vendored RtMidi
    KernelStreaming, // KS MIDI capture pin, which WinRT and WinMM sit on
    WootingAnalog // analog key depth from the Wooting SDK
};

struct MidiInputDevice {
    std::wstring id;     // opaque, backend-specific, stable enough to reopen with
    std::wstring name;   // for display
    MidiBackend  backend = MidiBackend::WinRT;

    // Physical device key, so the same socket reached through several
    // transports can be grouped. It is the display name before any suffix,
    // so two devices with identical names share a group; use the id to
    // tell them apart.
    std::wstring group;
};

// timestampQpc is QueryPerformanceCounter ticks taken on entry to the backend
// callback (t0 of the latency chain). data points at one complete MIDI message
// and is valid only for the duration of the call.
using MidiInputCallback = std::function<void(uint64_t timestampQpc,
                                             const uint8_t* data,
                                             size_t length)>;

class IMidiInput {
public:
    virtual ~IMidiInput() = default;

    virtual MidiBackend backend() const noexcept = 0;
    virtual std::vector<MidiInputDevice> enumerate() = 0;

    // Opening replaces any port this instance already had open.
    virtual bool open(const std::wstring& deviceId, MidiInputCallback callback) = 0;
    virtual void close() = 0;

    virtual bool isOpen() const noexcept = 0;
    virtual const std::wstring& openedDeviceId() const noexcept = 0;
};

// Implemented in KernelStreamingInput.cpp, the only translation unit that
// includes the KS headers.
std::unique_ptr<IMidiInput> CreateKernelStreamingInput();
bool KernelStreamingIdentifies(const std::wstring& deviceId);

std::unique_ptr<IMidiInput> CreateMidiInput(MidiBackend backend);

// Test hook: substitutes the transport CreateMidiInput returns, so tests can
// feed MIDI2Key without hardware (same pattern as InjectInput in InputHeader.h).
// Passing {} restores the real backends. Not synchronised: set it before
// opening a device.
using MidiInputFactory = std::function<std::unique_ptr<IMidiInput>(MidiBackend)>;
void SetMidiInputFactory(MidiInputFactory factory);

// Device list for the UI: WinRT, WinMM, Kernel Streaming and, when available,
// Wooting. Each id encodes its backend, so any entry can be opened directly.
std::vector<MidiInputDevice> EnumerateMidiInputs();

// Which backend produced this id. Defaults to WinRT for empty or unknown ids.
MidiBackend BackendForDeviceId(const std::wstring& deviceId);

// RtMidi appends " <index>" to every WinMM port name. Ports renumber when a
// device is unplugged, so names are compared and stored in ids without that
// suffix. Only the last token is removed, so a device named "Piano 2" keeps
// its number.
std::wstring StripRtMidiPortIndex(const std::wstring& name);

// The port index a WinMM device id names, or -1 when that device is not
// present. Never guesses: opening the wrong keyboard is worse than failing.
// Exposed for tests.
int ResolveWinMMPort(const std::wstring& deviceId, const std::vector<std::wstring>& portNames);
