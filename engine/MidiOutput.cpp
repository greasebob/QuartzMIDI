// MidiOutput.cpp: WinRT and WinMM implementations of IMidiOutput.
//
// WinMM reuses ResolveWinMMPort and StripRtMidiPortIndex from the input side so
// both directions share one rule: an id naming a missing device opens nothing,
// never a different port.

#include "MidiOutput.hpp"

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Midi.h>
#include <winrt/Windows.Storage.Streams.h>

// See MidiInput.cpp: newer SDKs link C++/WinRT's runtime functions directly.
#pragma comment(lib, "runtimeobject.lib")

#include "RtMidi.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWinMMPrefix[] = L"winmm:";

// Current callers send at most three bytes. send() drops anything longer
// instead of truncating it.
constexpr size_t kMaxMessage = 16;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

// Either apartment works; a thread that is already an STA is not an error.
void ensureApartment() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (winrt::hresult_error const& ex) {
        if (ex.code() != RPC_E_CHANGED_MODE) throw;
    }
}

class WinRTMidiOutput final : public IMidiOutput {
public:
    ~WinRTMidiOutput() override { close(); }

    MidiBackend backend() const noexcept override { return MidiBackend::WinRT; }

    std::vector<MidiInputDevice> enumerate() override {
        std::vector<MidiInputDevice> out;
        try {
            ensureApartment();
            auto selector = winrt::Windows::Devices::Midi::MidiOutPort::GetDeviceSelector();
            auto devices = winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(selector).get();
            out.reserve(devices.Size());
            for (auto const& info : devices) {
                out.push_back({ std::wstring(info.Id()), std::wstring(info.Name()), MidiBackend::WinRT,
                                std::wstring(info.Name()) });
            }
        }
        catch (winrt::hresult_error const&) {
            out.clear();
        }
        return out;
    }

    bool open(const std::wstring& deviceId) override {
        close();
        if (deviceId.empty()) return false;
        try {
            ensureApartment();
            m_port = winrt::Windows::Devices::Midi::MidiOutPort::FromIdAsync(winrt::hstring(deviceId)).get();
        }
        catch (winrt::hresult_error const&) {
            m_port = nullptr;
        }
        if (!m_port) return false;
        // Allocated once so send() never allocates on the note path.
        m_buffer = winrt::Windows::Storage::Streams::Buffer(static_cast<uint32_t>(kMaxMessage));
        m_openedId = deviceId;
        return true;
    }

    void close() override {
        std::lock_guard lock(m_mutex);
        if (m_port) {
            m_port.Close();
            m_port = nullptr;
        }
        m_buffer = nullptr;
        m_openedId.clear();
    }

    bool isOpen() const noexcept override { return static_cast<bool>(m_port); }
    const std::wstring& openedDeviceId() const noexcept override { return m_openedId; }

    void send(const uint8_t* message, size_t length) override {
        if (!message || length == 0 || length > kMaxMessage) return;
        std::lock_guard lock(m_mutex);
        if (!m_port || !m_buffer) return;
        try {
            std::memcpy(m_buffer.data(), message, length);
            m_buffer.Length(static_cast<uint32_t>(length));
            m_port.SendBuffer(m_buffer);
        }
        catch (winrt::hresult_error const&) {
            // An unplugged port must not throw into the playback thread.
            // Switching back to keystrokes is the caller's decision.
        }
    }

private:
    winrt::Windows::Devices::Midi::IMidiOutPort m_port{ nullptr };
    winrt::Windows::Storage::Streams::Buffer m_buffer{ nullptr };
    std::wstring m_openedId;
    std::mutex m_mutex;
};

class WinMMMidiOutput final : public IMidiOutput {
public:
    ~WinMMMidiOutput() override { close(); }

    MidiBackend backend() const noexcept override { return MidiBackend::WinMM; }

    std::vector<MidiInputDevice> enumerate() override {
        std::vector<MidiInputDevice> out;
        try {
            RtMidiOut probe(RtMidi::Api::WINDOWS_MM, "QuartzMIDI enumerate");
            const unsigned count = probe.getPortCount();
            out.reserve(count);
            std::vector<std::wstring> stripped;
            stripped.reserve(count);
            for (unsigned i = 0; i < count; ++i)
                stripped.push_back(StripRtMidiPortIndex(widen(probe.getPortName(i))));
            for (unsigned i = 0; i < count; ++i) {
                const std::wstring& name = stripped[i];
                const bool ambiguous =
                    std::count(stripped.begin(), stripped.end(), name) > 1;
                std::wstring shown = ambiguous ? name + L" " + std::to_wstring(i) : name;
                out.push_back({ kWinMMPrefix + std::to_wstring(i) + L"|" + name,
                                std::move(shown), MidiBackend::WinMM, name });
            }
        }
        catch (RtMidiError const&) {
            out.clear();
        }
        return out;
    }

    bool open(const std::wstring& deviceId) override {
        close();
        if (deviceId.empty()) return false;
        try {
            auto port = std::make_unique<RtMidiOut>(RtMidi::Api::WINDOWS_MM, "QuartzMIDI");
            const unsigned count = port->getPortCount();
            if (count == 0) return false;

            std::vector<std::wstring> names;
            names.reserve(count);
            for (unsigned i = 0; i < count; ++i) names.push_back(widen(port->getPortName(i)));

            // Fail when the device is missing; never fall back to port 0.
            const int target = ResolveWinMMPort(deviceId, names);
            if (target < 0) return false;

            port->openPort(static_cast<unsigned>(target));
            std::lock_guard lock(m_mutex);
            m_out = std::move(port);
            m_openedId = deviceId;
            return true;
        }
        catch (RtMidiError const&) {
            return false;
        }
    }

    void close() override {
        std::lock_guard lock(m_mutex);
        if (m_out) {
            try { m_out->closePort(); }
            catch (RtMidiError const&) {}
            m_out.reset();
        }
        m_openedId.clear();
    }

    bool isOpen() const noexcept override { return m_out != nullptr; }
    const std::wstring& openedDeviceId() const noexcept override { return m_openedId; }

    void send(const uint8_t* message, size_t length) override {
        if (!message || length == 0 || length > kMaxMessage) return;
        std::lock_guard lock(m_mutex);
        if (!m_out) return;
        try {
            // Pointer overload, so the note path allocates nothing.
            m_out->sendMessage(message, length);
        }
        catch (RtMidiError const&) {
        }
    }

private:
    std::unique_ptr<RtMidiOut> m_out;
    std::wstring m_openedId;
    std::mutex m_mutex;
};

MidiOutputFactory g_factory;

} // namespace

void SetMidiOutputFactory(MidiOutputFactory factory) {
    g_factory = std::move(factory);
}

std::unique_ptr<IMidiOutput> CreateMidiOutput(MidiBackend backend) {
    if (g_factory) {
        if (auto substituted = g_factory(backend)) return substituted;
    }
    if (backend == MidiBackend::WinMM) return std::make_unique<WinMMMidiOutput>();
    return std::make_unique<WinRTMidiOutput>();
}

std::vector<MidiInputDevice> EnumerateMidiOutputs() {
    WinRTMidiOutput winrtOutput;
    auto devices = winrtOutput.enumerate();

    // List both transports so either can be selected, as on the input side.
    const size_t winrtCount = devices.size();
    WinMMMidiOutput winmmOutput;
    for (auto& device : winmmOutput.enumerate()) {
        const bool alsoOnWinRT = std::any_of(devices.begin(), devices.begin() + winrtCount,
            [&](const MidiInputDevice& other) { return other.group == device.group; });
        if (alsoOnWinRT) device.name += L" (WinMM)";
        devices.push_back(std::move(device));
    }
    return devices;
}

MidiBackend BackendForOutputId(const std::wstring& deviceId) {
    const size_t prefix = wcslen(kWinMMPrefix);
    if (deviceId.size() >= prefix && deviceId.compare(0, prefix, kWinMMPrefix) == 0)
        return MidiBackend::WinMM;
    return MidiBackend::WinRT;
}

// Inverse of NOTE_NAME_CACHE in MIDI2Key.cpp: pitch letter, optional '#', then
// the octave as a signed decimal, octave = (number / 12) - 1. "C-1" is 0 and
// "G9" is 127.
int MidiNumberForNoteName(const char* name) noexcept {
    if (!name || !name[0]) return -1;

    int pitch;
    switch (name[0]) {
    case 'C': pitch = 0; break;
    case 'D': pitch = 2; break;
    case 'E': pitch = 4; break;
    case 'F': pitch = 5; break;
    case 'G': pitch = 7; break;
    case 'A': pitch = 9; break;
    case 'B': pitch = 11; break;
    default: return -1;
    }

    const char* cursor = name + 1;
    if (*cursor == '#') { ++pitch; ++cursor; }

    bool negative = false;
    if (*cursor == '-') { negative = true; ++cursor; }
    if (*cursor < '0' || *cursor > '9') return -1;

    int octave = 0;
    for (; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return -1;
        octave = octave * 10 + (*cursor - '0');
        if (octave > 12) return -1;   // above octave 9 cannot be valid
    }
    if (negative) octave = -octave;

    const int number = (octave + 1) * 12 + pitch;
    return (number >= 0 && number <= 127) ? number : -1;
}
