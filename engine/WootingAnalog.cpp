// WootingAnalog.cpp: analog key positions turned into MIDI note messages.

#include "WootingAnalog.hpp"

#include <map>
#include <string>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- SDK binding -----------------------------------------------------------
// Prototypes from the wooting-analog-sdk C API. Bound at runtime so a missing
// DLL reports no device instead of failing startup.

enum WootingAnalog_KeycodeType : uint32_t {
    HID = 0,
    ScanCode1 = 1,
    VirtualKey = 2,
    VirtualKeyTranslate = 3,
};

using pfn_initialise = int (*)();
using pfn_is_initialised = bool (*)();
using pfn_uninitialise = int (*)();
using pfn_set_keycode_mode = int (*)(WootingAnalog_KeycodeType);
using pfn_read_full_buffer = int (*)(uint16_t* codeBuffer, float* analogBuffer, unsigned int length);

struct Sdk {
    HMODULE module = nullptr;
    pfn_initialise initialise = nullptr;
    pfn_is_initialised isInitialised = nullptr;
    pfn_uninitialise uninitialise = nullptr;
    pfn_set_keycode_mode setKeycodeMode = nullptr;
    pfn_read_full_buffer readFullBuffer = nullptr;

    bool bound() const {
        return module && initialise && isInitialised && uninitialise && setKeycodeMode && readFullBuffer;
    }
};

Sdk& sdk() {
    static Sdk s = [] {
        Sdk out;
        // The installer puts the DLL in Program Files and does not add it to
        // PATH, so try the known location as well as the loader's own search.
        const wchar_t* candidates[] = {
            L"wooting_analog_sdk.dll",
            L"C:\\Program Files\\wooting-analog-sdk\\wooting_analog_sdk.dll",
            L"C:\\Program Files (x86)\\wooting-analog-sdk\\wooting_analog_sdk.dll",
        };
        for (const wchar_t* path : candidates) {
            out.module = LoadLibraryW(path);
            if (out.module) break;
        }
        if (!out.module) return out;

        out.initialise = reinterpret_cast<pfn_initialise>(
            GetProcAddress(out.module, "wooting_analog_initialise"));
        out.isInitialised = reinterpret_cast<pfn_is_initialised>(
            GetProcAddress(out.module, "wooting_analog_is_initialised"));
        out.uninitialise = reinterpret_cast<pfn_uninitialise>(
            GetProcAddress(out.module, "wooting_analog_uninitialise"));
        out.setKeycodeMode = reinterpret_cast<pfn_set_keycode_mode>(
            GetProcAddress(out.module, "wooting_analog_set_keycode_mode"));
        out.readFullBuffer = reinterpret_cast<pfn_read_full_buffer>(
            GetProcAddress(out.module, "wooting_analog_read_full_buffer"));
        return out;
    }();
    return s;
}

// ---- default note map ------------------------------------------------------
// Set 1 scancodes for the rows the virtual-piano layout uses.
constexpr uint16_t SC_1 = 0x02, SC_2 = 0x03, SC_3 = 0x04, SC_4 = 0x05, SC_5 = 0x06;
constexpr uint16_t SC_6 = 0x07, SC_7 = 0x08, SC_8 = 0x09, SC_9 = 0x0A, SC_0 = 0x0B;
constexpr uint16_t SC_Q = 0x10, SC_W = 0x11, SC_E = 0x12, SC_R = 0x13, SC_T = 0x14;
constexpr uint16_t SC_Y = 0x15, SC_U = 0x16, SC_I = 0x17, SC_O = 0x18, SC_P = 0x19;
constexpr uint16_t SC_A = 0x1E, SC_S = 0x1F, SC_D = 0x20, SC_F = 0x21, SC_G = 0x22;
constexpr uint16_t SC_H = 0x23, SC_J = 0x24, SC_K = 0x25, SC_L = 0x26;
constexpr uint16_t SC_Z = 0x2C, SC_X = 0x2D, SC_C = 0x2E, SC_V = 0x2F, SC_B = 0x30;
constexpr uint16_t SC_N = 0x31, SC_M = 0x32;

std::mutex g_mapMutex;
std::array<int16_t, 256> g_scanToNote = DefaultWootingScancodeNoteMap();
WootingAnalogSettings g_settings{};
std::atomic<bool> g_shiftHeld{ false };

} // namespace

bool WootingShiftHeld() noexcept { return g_shiftHeld.load(std::memory_order_acquire); }

namespace {

// The virtual-piano layout's unshifted keys, low to high, in the order the
// default KEY_MAPPINGS.FULL uses: the number row is the bottom of the range.
constexpr char kVirtualPianoWhites[] = "1234567890qwertyuiopasdfghjklzxcvbnm";

// Set 1 scancode for an unshifted printable key, or 0. Shifted and ctrl
// bindings have none: the analog SDK sees them as two physical keys.
uint16_t ScancodeForKey(char key) {
    switch (key) {
    case '1': return SC_1; case '2': return SC_2; case '3': return SC_3;
    case '4': return SC_4; case '5': return SC_5; case '6': return SC_6;
    case '7': return SC_7; case '8': return SC_8; case '9': return SC_9;
    case '0': return SC_0;
    case 'q': return SC_Q; case 'w': return SC_W; case 'e': return SC_E;
    case 'r': return SC_R; case 't': return SC_T; case 'y': return SC_Y;
    case 'u': return SC_U; case 'i': return SC_I; case 'o': return SC_O;
    case 'p': return SC_P;
    case 'a': return SC_A; case 's': return SC_S; case 'd': return SC_D;
    case 'f': return SC_F; case 'g': return SC_G; case 'h': return SC_H;
    case 'j': return SC_J; case 'k': return SC_K; case 'l': return SC_L;
    case 'z': return SC_Z; case 'x': return SC_X; case 'c': return SC_C;
    case 'v': return SC_V; case 'b': return SC_B; case 'n': return SC_N;
    case 'm': return SC_M;
    default: return 0;
    }
}

int NoteFromName(const std::string& name) {
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#",
                                     "G", "G#", "A", "A#", "B"};
    if (name.size() < 2) return -1;
    const std::string letters = name.substr(0, name.size() - 1);
    const char octaveDigit = name.back();
    if (octaveDigit < '0' || octaveDigit > '9') return -1;
    for (int i = 0; i < 12; ++i) {
        if (letters == kNames[i]) return (octaveDigit - '0' + 1) * 12 + i;
    }
    return -1;
}

} // namespace

std::array<int16_t, 256> DefaultWootingScancodeNoteMap() {
    std::array<int16_t, 256> map{};
    map.fill(-1);

    // White keys from C2 upward in layout order. Black keys stay unmapped:
    // their bindings are shifted keys, which the analog SDK sees as two presses.
    const int steps[7] = { 0, 2, 4, 5, 7, 9, 11 };
    for (size_t i = 0; i + 1 < sizeof(kVirtualPianoWhites); ++i) {
        const int note = 36 + static_cast<int>(i) / 7 * 12 + steps[i % 7];
        if (note > 127) break;
        const uint16_t code = ScancodeForKey(kVirtualPianoWhites[i]);
        if (code) map[code] = static_cast<int16_t>(note);
    }
    return map;
}

std::array<int16_t, 256> WootingScancodeNoteMapFrom(
        const std::map<std::string, std::string>& keyMappings) {
    std::array<int16_t, 256> map{};
    map.fill(-1);
    for (const auto& [name, key] : keyMappings) {
        if (key.size() != 1) continue;          // shifted and ctrl bindings have no single scancode
        const uint16_t code = ScancodeForKey(key[0]);
        const int note = NoteFromName(name);
        if (code && note >= 0 && note <= 127) map[code] = static_cast<int16_t>(note);
    }
    return map;
}

void SetWootingScancodeNoteMap(const std::array<int16_t, 256>& map) {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    g_scanToNote = map;
}

void SetWootingAnalogSettings(const WootingAnalogSettings& settings) {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    g_settings = settings;
}

WootingAnalogSettings GetWootingAnalogSettings() {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    return g_settings;
}

// Velocity comes from key speed as it crosses the trigger; depth is the same
// for every key at that moment. rate * scale / 100 is wooting-analog-midi's
// formula, so its Velocity Scale means the same here: at 5, 20 depth units per
// second (full travel in about 50 ms) is full strength.
size_t WootingPollStep(WootingPollState& state,
                       const uint16_t* codes, const float* values, int count,
                       const std::array<int16_t, 256>& noteMap,
                       const WootingAnalogSettings& settings,
                       double seconds,
                       WootingPollEvent* out, size_t outCapacity) {
    size_t written = 0;
    const auto emit = [&](bool on, int note, uint8_t velocity) {
        if (written < outCapacity) out[written++] = {on, static_cast<uint8_t>(note), velocity};
    };

    const float release = settings.trigger * settings.releaseFraction;
    std::array<bool, 256> seen{};
    seen.fill(false);

    // Read the shift before the note keys so a key struck in the same poll
    // uses it. Keys at rest are absent from the buffer, so no entry means up.
    int shift = 0;
    for (int i = 0; i < count; ++i)
        if (codes[i] == kWootingShiftScancode && values[i] >= settings.trigger)
            shift = settings.shiftAmount;
    state.shiftHeld = false;
    for (int i = 0; i < count; ++i)
        state.shiftHeld |= codes[i] == kWootingShiftScancode && values[i] >= settings.trigger;

    for (int i = 0; i < count; ++i) {
        const uint16_t code = codes[i];
        if (code >= 256) continue;
        const int16_t note = noteMap[code];
        if (note < 0) continue;

        seen[code] = true;
        const float depth = values[i];
        const float previousDepth = state.lastDepth[code];
        state.lastDepth[code] = depth;

        auto& recent = state.recent[code];
        auto& older = state.older[code];
        // Keys at rest are absent from the buffer, so a key with no previous
        // depth left rest during this poll.
        if (previousDepth <= 0.0f) {
            recent = older = {};
            state.resting[code] = false;
        }
        if (state.resting[code] && std::fabs(depth - older.depth) < kWootingRestingTravel) {
            recent.age = older.age = 0.0;
        } else {
            state.resting[code] = false;
            recent.age += seconds;
            older.age += seconds;
        }
        const WootingPollState::Anchor from = older;
        if (recent.age >= kWootingStrikeWindow) {
            state.resting[code] = std::fabs(depth - recent.depth) < kWootingRestingTravel;
            older = state.resting[code] ? WootingPollState::Anchor{depth, 0.0} : recent;
            recent = {depth, 0.0};
        }

        if (state.sounding[code] < 0 && depth >= settings.trigger) {
            // A shift that pushes the note outside 0-127 leaves the key silent
            // and unsounded, so releasing it later sends nothing.
            const int shifted = note + shift;
            if (shifted < 0 || shifted > 127) continue;
            state.sounding[code] = static_cast<int16_t>(shifted);
            emit(true, shifted, WootingVelocityFor(depth, from.depth, from.age, settings.velocityScale));
        }
        else if (state.sounding[code] >= 0 && depth <= release) {
            emit(false, state.sounding[code], 0);
            state.sounding[code] = -1;
        }
    }

    // A key missing from the buffer has been released. Reset its depth even
    // if it never sounded, so the next press is measured from rest.
    for (size_t code = 0; code < state.sounding.size(); ++code) {
        if (seen[code]) continue;
        state.lastDepth[code] = 0.0f;
        if (state.sounding[code] < 0) continue;
        emit(false, state.sounding[code], 0);
        state.sounding[code] = -1;
    }
    return written;
}

uint8_t WootingVelocityFor(float depth, float previousDepth, double seconds, float velocityScale) {
    // No elapsed time means no measurement; return a mid-range velocity.
    if (seconds <= 0.0) return 96;
    const double rate = (static_cast<double>(depth) - static_cast<double>(previousDepth)) / seconds;
    double scaled = rate * static_cast<double>(velocityScale) / 100.0;
    if (scaled < 0.0) scaled = 0.0;
    if (scaled > 1.0) scaled = 1.0;
    return static_cast<uint8_t>(1 + static_cast<int>(scaled * 126.0));
}

bool WootingAnalogAvailable() {
    Sdk& s = sdk();
    if (!s.bound()) return false;
    if (s.isInitialised()) return true;
    const int devices = s.initialise();
    return devices > 0;
}

namespace {

class WootingAnalogInput final : public IMidiInput {
public:
    ~WootingAnalogInput() override { close(); }

    MidiBackend backend() const noexcept override { return MidiBackend::WootingAnalog; }

    std::vector<MidiInputDevice> enumerate() override {
        if (!WootingAnalogAvailable()) return {};
        return { { L"wooting:analog", L"Wooting analog keys (direct)", MidiBackend::WootingAnalog } };
    }

    bool open(const std::wstring& deviceId, MidiInputCallback callback) override {
        close();
        if (deviceId != L"wooting:analog") return false;

        Sdk& s = sdk();
        if (!s.bound()) return false;
        if (!s.isInitialised() && s.initialise() < 0) return false;
        // Set 1 scancodes, as used by the rest of the app.
        s.setKeycodeMode(ScanCode1);

        m_callback = std::move(callback);
        m_openedId = deviceId;
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread(&WootingAnalogInput::pollLoop, this);
        return true;
    }

    void close() override {
        if (m_running.exchange(false, std::memory_order_acq_rel)) {
            if (m_thread.joinable()) m_thread.join();
        }
        m_callback = nullptr;
        m_openedId.clear();
    }

    bool isOpen() const noexcept override { return m_running.load(std::memory_order_acquire); }
    const std::wstring& openedDeviceId() const noexcept override { return m_openedId; }

private:
    static constexpr size_t kMaxKeys = 32;

    void pollLoop() {
        // Dedicated thread: injection must not share a thread with a message loop.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        Sdk& s = sdk();
        std::array<uint16_t, kMaxKeys> codes{};
        std::array<float, kMaxKeys> values{};
        // Room for a note off and a note on per key in the buffer, plus a
        // release for every key that could drop out of it in one poll.
        std::array<WootingPollEvent, 2 * kMaxKeys + 256> events{};
        WootingPollState state;

        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        LARGE_INTEGER previous{};
        QueryPerformanceCounter(&previous);

        while (m_running.load(std::memory_order_acquire)) {
            const int count = s.readFullBuffer(codes.data(), values.data(), static_cast<unsigned>(kMaxKeys));

            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double dt = static_cast<double>(now.QuadPart - previous.QuadPart) / static_cast<double>(freq.QuadPart);
            previous = now;

            size_t produced = 0;
            {
                std::lock_guard<std::mutex> lock(g_mapMutex);
                produced = WootingPollStep(state, codes.data(), values.data(),
                                           (std::min)(count, static_cast<int>(kMaxKeys)),
                                           g_scanToNote, g_settings, dt,
                                           events.data(), events.size());
            }
            // Publish before this poll's notes go out, so a key struck with
            // the shift sees it down.
            g_shiftHeld.store(state.shiftHeld, std::memory_order_release);
            // Emit outside the lock: the callback runs the whole injection
            // path, and a settings change must not wait on it.
            for (size_t i = 0; i < produced; ++i) {
                if (events[i].on) emitNoteOn(events[i].note, events[i].velocity, static_cast<uint64_t>(now.QuadPart));
                else emitNoteOff(events[i].note, static_cast<uint64_t>(now.QuadPart));
            }

            // About 1 kHz: adds well under a millisecond of latency at low CPU cost.
            Sleep(1);
        }

        g_shiftHeld.store(false, std::memory_order_release);
        // An empty poll releases every key still sounding when the device closes.
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            const size_t produced = WootingPollStep(state, nullptr, nullptr, 0, g_scanToNote,
                                                    g_settings, 0, events.data(), events.size());
            for (size_t i = 0; i < produced; ++i)
                if (!events[i].on) emitNoteOff(events[i].note, 0);
        }
    }

    void emitNoteOn(uint8_t note, uint8_t velocity, uint64_t timestampQpc) {
        const uint8_t message[3] = { 0x90, note, velocity };
        if (m_callback) m_callback(timestampQpc, message, sizeof(message));
    }

    void emitNoteOff(uint8_t note, uint64_t timestampQpc) {
        const uint8_t message[3] = { 0x80, note, 0 };
        if (m_callback) m_callback(timestampQpc, message, sizeof(message));
    }

    MidiInputCallback m_callback;
    std::wstring m_openedId;
    std::atomic<bool> m_running{ false };
    std::thread m_thread;
};

} // namespace

std::unique_ptr<IMidiInput> CreateWootingAnalogInput() {
    return std::make_unique<WootingAnalogInput>();
}
