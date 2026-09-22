// MidiInput.cpp: WinRT and WinMM implementations of IMidiInput.

#include "MidiInput.hpp"

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Midi.h>
#include <winrt/Windows.Storage.Streams.h>

// C++/WinRT calls RoGetActivationFactory and RoOriginateLanguageException.
// SDK 10.0.22621 resolves them with GetProcAddress, but newer SDKs link them
// directly (LNK2019 without this). runtimeobject.lib provides only those;
// windowsapp.lib would also reroute shared Win32 imports.
#pragma comment(lib, "runtimeobject.lib")

#include "RtMidi.h"
#include "WootingAnalog.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace {

// WinMM ids are prefixed so BackendForDeviceId can route without a lookup.
constexpr wchar_t kWinMMPrefix[] = L"winmm:";
constexpr wchar_t kWootingPrefix[] = L"wooting:";

// A WinMM id is "winmm:<index>|<name>". The '|' is found from the front, so a
// name may contain '|'.
bool parseWinMMId(const std::wstring& id, unsigned& index, std::wstring& name) {
    const size_t prefix = wcslen(kWinMMPrefix);
    if (id.size() < prefix || id.compare(0, prefix, kWinMMPrefix) != 0) return false;
    const size_t bar = id.find(L'|', prefix);
    const std::wstring digits = id.substr(prefix, bar == std::wstring::npos ? std::wstring::npos : bar - prefix);
    for (wchar_t c : digits) if (c < L'0' || c > L'9') return false;
    index = digits.empty() ? 0u : static_cast<unsigned>(_wtoi(digits.c_str()));
    name = (bar == std::wstring::npos) ? std::wstring() : id.substr(bar + 1);
    return true;
}

inline uint64_t nowQpc() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

// The UI thread may already be an STA (shell dialogs, drag-drop and OLE all do
// this), in which case asking for an MTA throws RPC_E_CHANGED_MODE. Either
// apartment works for us, so treat that as success.
void ensureApartment() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (winrt::hresult_error const& ex) {
        if (ex.code() != RPC_E_CHANGED_MODE) throw;
    }
}

class WinRTMidiInput final : public IMidiInput {
public:
    ~WinRTMidiInput() override { close(); }

    MidiBackend backend() const noexcept override { return MidiBackend::WinRT; }

    std::vector<MidiInputDevice> enumerate() override {
        std::vector<MidiInputDevice> out;
        try {
            ensureApartment();
            auto selector = winrt::Windows::Devices::Midi::MidiInPort::GetDeviceSelector();
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

    bool open(const std::wstring& deviceId, MidiInputCallback callback) override {
        close();
        if (deviceId.empty()) return false;
        try {
            ensureApartment();
            m_port = winrt::Windows::Devices::Midi::MidiInPort::FromIdAsync(winrt::hstring(deviceId)).get();
        }
        catch (winrt::hresult_error const&) {
            m_port = nullptr;
        }
        if (!m_port) return false;

        m_callback = std::move(callback);
        m_token = m_port.MessageReceived(
            [this](winrt::Windows::Devices::Midi::MidiInPort const&,
                   winrt::Windows::Devices::Midi::MidiMessageReceivedEventArgs const& args) {
                const uint64_t t0 = nowQpc();
                auto raw = args.Message().RawData();
                if (!raw || raw.Length() == 0 || !m_callback) return;
                m_callback(t0, raw.data(), static_cast<size_t>(raw.Length()));
            });
        m_openedId = deviceId;
        return true;
    }

    void close() override {
        if (m_port) {
            m_port.MessageReceived(m_token);
            m_port.Close();
            m_port = nullptr;
        }
        m_callback = nullptr;
        m_openedId.clear();
    }

    bool isOpen() const noexcept override { return static_cast<bool>(m_port); }
    const std::wstring& openedDeviceId() const noexcept override { return m_openedId; }

private:
    winrt::Windows::Devices::Midi::MidiInPort m_port{ nullptr };
    winrt::event_token m_token{};
    MidiInputCallback m_callback;
    std::wstring m_openedId;
};

class WinMMMidiInput final : public IMidiInput {
public:
    ~WinMMMidiInput() override { close(); }

    MidiBackend backend() const noexcept override { return MidiBackend::WinMM; }

    std::vector<MidiInputDevice> enumerate() override {
        std::vector<MidiInputDevice> out;
        try {
            RtMidiIn probe(RtMidi::Api::WINDOWS_MM, "QuartzMIDI enumerate", 100);
            const unsigned count = probe.getPortCount();
            out.reserve(count);
            std::vector<std::wstring> stripped;
            stripped.reserve(count);
            for (unsigned i = 0; i < count; ++i)
                stripped.push_back(StripRtMidiPortIndex(widen(probe.getPortName(i))));
            for (unsigned i = 0; i < count; ++i) {
                // The port name is the identity; the index only breaks ties
                // between ports with the same name. The id stores the name
                // without RtMidi's index suffix.
                const std::wstring& name = stripped[i];
                // Show the index only when it is needed to tell two ports
                // apart, e.g. two keyboards of the same model.
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

    bool open(const std::wstring& deviceId, MidiInputCallback callback) override {
        close();
        if (deviceId.empty()) return false;

        try {
            m_in = new RtMidiIn(RtMidi::Api::WINDOWS_MM, "QuartzMIDI", 100);
            const unsigned count = m_in->getPortCount();
            if (count == 0) { destroy(); return false; }

            std::vector<std::wstring> names;
            names.reserve(count);
            for (unsigned i = 0; i < count; ++i) names.push_back(widen(m_in->getPortName(i)));

            // Fail when the device is missing; never fall back to port 0.
            const int target = ResolveWinMMPort(deviceId, names);
            if (target < 0) { destroy(); return false; }

            m_callback = std::move(callback);
            m_in->setCallback(&WinMMMidiInput::trampoline, this);
            m_in->ignoreTypes(true, true, true);
            m_in->setBufferSize(256, 1);
            m_in->openPort(static_cast<unsigned>(target));
            m_openedId = deviceId;
            return true;
        }
        catch (RtMidiError const&) {
            destroy();
            return false;
        }
    }

    void close() override {
        if (m_in) {
            try {
                m_in->cancelCallback();
                m_in->closePort();
            }
            catch (RtMidiError const&) {
            }
            destroy();
        }
        m_callback = nullptr;
        m_openedId.clear();
    }

    bool isOpen() const noexcept override { return m_in != nullptr; }
    const std::wstring& openedDeviceId() const noexcept override { return m_openedId; }

private:
    static void __stdcall trampoline(double, std::vector<unsigned char>* message, void* user) {
        const uint64_t t0 = nowQpc();
        auto* self = static_cast<WinMMMidiInput*>(user);
        if (!self || !message || message->empty() || !self->m_callback) return;
        self->m_callback(t0, message->data(), message->size());
    }

    void destroy() {
        delete m_in;
        m_in = nullptr;
    }

    RtMidiIn* m_in = nullptr;
    MidiInputCallback m_callback;
    std::wstring m_openedId;
};

} // namespace

namespace {
MidiInputFactory g_factory;
}

void SetMidiInputFactory(MidiInputFactory factory) {
    g_factory = std::move(factory);
}

std::unique_ptr<IMidiInput> CreateMidiInput(MidiBackend backend) {
    if (g_factory) {
        if (auto substituted = g_factory(backend)) return substituted;
    }
    if (backend == MidiBackend::WootingAnalog) return CreateWootingAnalogInput();
    if (backend == MidiBackend::WinMM) return std::make_unique<WinMMMidiInput>();
    if (backend == MidiBackend::KernelStreaming) return CreateKernelStreamingInput();
    return std::make_unique<WinRTMidiInput>();
}

std::vector<MidiInputDevice> EnumerateMidiInputs() {
    WinRTMidiInput winrtInput;
    auto devices = winrtInput.enumerate();

    // List every transport. They reach the same socket, and drivers differ
    // in which one behaves better, so each must be selectable.
    const size_t winrtCount = devices.size();
    WinMMMidiInput winmmInput;
    for (auto& device : winmmInput.enumerate()) {
        // Most ports appear on both transports, so suffix the WinMM row.
        // `group` keeps the unsuffixed name so callers can merge the rows.
        const bool alsoOnWinRT = std::any_of(devices.begin(), devices.begin() + winrtCount,
            [&](const MidiInputDevice& other) { return other.group == device.group; });
        if (alsoOnWinRT) device.name += L" (WinMM)";
        devices.push_back(std::move(device));
    }

    // Kernel Streaming is a third transport for the same sockets. A driver
    // that exposes no MIDI capture pin contributes no rows.
    {
        auto kernel = CreateKernelStreamingInput();
        for (auto& device : kernel->enumerate()) {
            device.group = device.name;   // before the suffix
            device.name += L" (KS)";
            devices.push_back(std::move(device));
        }
    }

    if (WootingAnalogAvailable()) {
        auto wooting = CreateWootingAnalogInput();
        // A Wooting keyboard reads analog key depth through the Wooting SDK,
        // not a MIDI port. Give it its own group; an empty key would merge it
        // with other rows that lack one.
        for (auto& device : wooting->enumerate()) {
            if (device.group.empty()) device.group = device.name;
            devices.push_back(std::move(device));
        }
    }
    return devices;
}

std::wstring StripRtMidiPortIndex(const std::wstring& name) {
    const size_t space = name.find_last_of(L' ');
    if (space == std::wstring::npos || space + 1 >= name.size()) return name;
    for (size_t i = space + 1; i < name.size(); ++i)
        if (name[i] < L'0' || name[i] > L'9') return name;
    return name.substr(0, space);
}

int ResolveWinMMPort(const std::wstring& deviceId, const std::vector<std::wstring>& portNames) {
    unsigned index = 0;
    std::wstring wanted;
    if (!parseWinMMId(deviceId, index, wanted)) return -1;
    const std::wstring target = StripRtMidiPortIndex(wanted);

    if (!target.empty()) {
        // The stored index breaks ties: try it first, but accept it only if
        // the name matches.
        if (index < portNames.size() && StripRtMidiPortIndex(portNames[index]) == target)
            return static_cast<int>(index);
        for (size_t i = 0; i < portNames.size(); ++i)
            if (StripRtMidiPortIndex(portNames[i]) == target) return static_cast<int>(i);
        return -1;
    }

    // An id with no name ("winmm:<index>") resolves by index alone.
    return index < portNames.size() ? static_cast<int>(index) : -1;
}

MidiBackend BackendForDeviceId(const std::wstring& deviceId) {
    if (KernelStreamingIdentifies(deviceId)) return MidiBackend::KernelStreaming;
    const size_t winmmPrefix = wcslen(kWinMMPrefix);
    if (deviceId.size() >= winmmPrefix && deviceId.compare(0, winmmPrefix, kWinMMPrefix) == 0) {
        return MidiBackend::WinMM;
    }
    const size_t wootingPrefix = wcslen(kWootingPrefix);
    if (deviceId.size() >= wootingPrefix && deviceId.compare(0, wootingPrefix, kWootingPrefix) == 0) {
        return MidiBackend::WootingAnalog;
    }
    return MidiBackend::WinRT;
}
