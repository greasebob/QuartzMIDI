// MIDI2Key.cpp

#include "MIDI2Key.hpp"
#include "InputHeader.h"
#include "InputLatency.hpp"
#include "VelocityTelemetry.hpp"
#include "WootingAnalog.hpp"

#pragma comment(lib, "avrt.lib")

// Registers the calling thread with MMCSS "Pro Audio". Called once by each
// thread that delivers MIDI, on its first message. Failure is not fatal (the
// thread keeps normal priority) and is logged only once per process, because
// a MIDI2Key is constructed on every device change. The process priority
// class is left alone so the game keeps precedence over the UI.
static void setThreadToRealTime() {
    static std::atomic<bool> reported{false};
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!hTask) {
        if (!reported) std::wcout << L"MMCSS Pro Audio is unavailable; live input runs at normal thread priority." << std::endl;
        reported = true;
    }
    else {
        BOOL ok = AvSetMmThreadPriority(hTask, AVRT_PRIORITY_CRITICAL);
        if (!ok && !reported) std::wcout << L"MMCSS accepted the thread but refused the critical priority." << std::endl;
        if (!ok) reported = true;
    }
}
alignas(64) char  MIDI2Key::m_lastVelocityKey = '\0';
static __forceinline INPUT makeKeybdInput(WORD wScan, DWORD dwFlags) {
    INPUT inp{};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = 0;     
    inp.ki.wScan = wScan;
    inp.ki.dwFlags = dwFlags;
    return inp;
}

// Note names ("C#4", "E3") indexed by MIDI note number.
static std::array<const char*, 128> NOTE_NAME_CACHE = []() {
    std::array<const char*, 128> cache{};
    constexpr std::array<const char[3], 12> names = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };
    static char bufs[128][8]{};
    for (int n = 0; n < 128; ++n) {
        int pitch = n % 12;
        int octave = (n / 12) - 1;
        int len = (names[pitch][1] == '#') ? 2 : 1;
        std::memcpy(bufs[n], names[pitch], len);
        sprintf_s(bufs[n] + len, sizeof(bufs[n]) - len, "%d", octave);
        cache[n] = bufs[n];
    }
    return cache;
    }();

// Set 1 scancodes for the QWERTY digits, letters and shifted digits.
alignas(64) static const std::array<WORD, 256> SCAN_TABLE = []() {
    std::array<WORD, 256> table{};
    table.fill(0);

    table[(unsigned char)'1'] = 0x02;   table[(unsigned char)'2'] = 0x03;
    table[(unsigned char)'3'] = 0x04;   table[(unsigned char)'4'] = 0x05;
    table[(unsigned char)'5'] = 0x06;   table[(unsigned char)'6'] = 0x07;
    table[(unsigned char)'7'] = 0x08;   table[(unsigned char)'8'] = 0x09;
    table[(unsigned char)'9'] = 0x0A;   table[(unsigned char)'0'] = 0x0B;

    table[(unsigned char)'q'] = 0x10;   table[(unsigned char)'w'] = 0x11;
    table[(unsigned char)'e'] = 0x12;   table[(unsigned char)'r'] = 0x13;
    table[(unsigned char)'t'] = 0x14;   table[(unsigned char)'y'] = 0x15;
    table[(unsigned char)'u'] = 0x16;   table[(unsigned char)'i'] = 0x17;
    table[(unsigned char)'o'] = 0x18;   table[(unsigned char)'p'] = 0x19;
    table[(unsigned char)'a'] = 0x1E;   table[(unsigned char)'s'] = 0x1F;
    table[(unsigned char)'d'] = 0x20;   table[(unsigned char)'f'] = 0x21;
    table[(unsigned char)'g'] = 0x22;   table[(unsigned char)'h'] = 0x23;
    table[(unsigned char)'j'] = 0x24;   table[(unsigned char)'k'] = 0x25;
    table[(unsigned char)'l'] = 0x26;   table[(unsigned char)'z'] = 0x2C;
    table[(unsigned char)'x'] = 0x2D;   table[(unsigned char)'c'] = 0x2E;
    table[(unsigned char)'v'] = 0x2F;   table[(unsigned char)'b'] = 0x30;
    table[(unsigned char)'n'] = 0x31;   table[(unsigned char)'m'] = 0x32;

    // Shifted digits share the digit's scancode.
    table[(unsigned char)'!'] = 0x02;   table[(unsigned char)'@'] = 0x03;
    table[(unsigned char)'#'] = 0x04;   table[(unsigned char)'$'] = 0x05;
    table[(unsigned char)'%'] = 0x06;   table[(unsigned char)'^'] = 0x07;
    table[(unsigned char)'&'] = 0x08;   table[(unsigned char)'*'] = 0x09;
    table[(unsigned char)'('] = 0x0A;   table[(unsigned char)')'] = 0x0B;

    return table;
    }();

// Maps a char to its unshifted key and whether it needs Shift.
struct CharInfoRec {
    char canonical;
    bool shifted;
};

inline std::array<CharInfoRec, 256> initCharInfoTable() {
    std::array<CharInfoRec, 256> table{};
    for (int i = 0; i < 256; ++i) {
        char c = static_cast<char>(i);
        table[i] = { c, false };
        if (c >= 'A' && c <= 'Z') {
            table[i] = { static_cast<char>(c + 32), true };
        }
    }
    // Shifted digits.
    table[(unsigned char)'!'] = { '1', true };
    table[(unsigned char)'@'] = { '2', true };
    table[(unsigned char)'#'] = { '3', true };
    table[(unsigned char)'$'] = { '4', true };
    table[(unsigned char)'%'] = { '5', true };
    table[(unsigned char)'^'] = { '6', true };
    table[(unsigned char)'&'] = { '7', true };
    table[(unsigned char)'*'] = { '8', true };
    table[(unsigned char)'('] = { '9', true };
    table[(unsigned char)')'] = { '0', true };
    return table;
}
static const auto CHAR_INFO_TABLE = initCharInfoTable();

// Scancode of SUSTAIN_KEY, the key autoplay uses for the pedal; Space if unset.
static WORD SustainScan(const VirtualPianoPlayer& player) {
    const UINT scan = player.sustain_key_code ? MapVirtualKeyW(player.sustain_key_code, MAPVK_VK_TO_VSC) : 0;
    return scan ? static_cast<WORD>(scan) : WORD{0x39};
}

// Modifier press/release sequences, indexed by alt | ctrl << 1 | shift << 2.
struct FixedKeyEvent {
    WORD scan;
    DWORD flags;
};

struct ModSequence {
    size_t downCount;
    std::array<FixedKeyEvent, 3> down;
    size_t upCount;
    std::array<FixedKeyEvent, 3> up;
};

constexpr WORD ALT_SCAN = 0x38;
constexpr WORD CTRL_SCAN = 0x1D;
constexpr WORD SHIFT_SCAN = 0x2A;
constexpr DWORD SC_FLAG = KEYEVENTF_SCANCODE;
constexpr DWORD KU_FLAG = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

inline std::array<ModSequence, 8> initModSequences() {
    std::array<ModSequence, 8> seq{};

    // 0 = none
    seq[0] = {
        0, {{{0,0},{0,0},{0,0}}},
        0, {{{0,0},{0,0},{0,0}}}
    };
    // 1 = alt
    seq[1] = {
        1, {{{ALT_SCAN, SC_FLAG},{0,0},{0,0}}},
        1, {{{ALT_SCAN, KU_FLAG},{0,0},{0,0}}}
    };
    // 2 = ctrl
    seq[2] = {
        1, {{{CTRL_SCAN, SC_FLAG},{0,0},{0,0}}},
        1, {{{CTRL_SCAN, KU_FLAG},{0,0},{0,0}}}
    };
    // 3 = alt + ctrl
    seq[3] = {
        2, {{{ALT_SCAN, SC_FLAG},{CTRL_SCAN, SC_FLAG},{0,0}}},
        2, {{{CTRL_SCAN, KU_FLAG},{ALT_SCAN, KU_FLAG},{0,0}}}
    };
    // 4 = shift
    seq[4] = {
        1, {{{SHIFT_SCAN, SC_FLAG},{0,0},{0,0}}},
        1, {{{SHIFT_SCAN, KU_FLAG},{0,0},{0,0}}}
    };
    // 5 = alt + shift
    seq[5] = {
        2, {{{ALT_SCAN, SC_FLAG},{SHIFT_SCAN, SC_FLAG},{0,0}}},
        2, {{{SHIFT_SCAN, KU_FLAG},{ALT_SCAN, KU_FLAG},{0,0}}}
    };
    // 6 = ctrl + shift
    seq[6] = {
        2, {{{CTRL_SCAN, SC_FLAG},{SHIFT_SCAN, SC_FLAG},{0,0}}},
        2, {{{SHIFT_SCAN, KU_FLAG},{CTRL_SCAN, KU_FLAG},{0,0}}}
    };
    // 7 = alt + ctrl + shift
    seq[7] = {
        3, {{{ALT_SCAN, SC_FLAG},{CTRL_SCAN, SC_FLAG},{SHIFT_SCAN, SC_FLAG}}},
        3, {{{SHIFT_SCAN, KU_FLAG},{CTRL_SCAN, KU_FLAG},{ALT_SCAN, KU_FLAG}}}
    };
    return seq;
}
static const auto modSequences = initModSequences();

// Press and release INPUT sequences for one key combo.
struct alignas(64) PrecomputedKeyEvents {
    size_t pressCount;
    std::array<INPUT, 16> press;
    size_t releaseCount;
    std::array<INPUT, 16> release;

    uint16_t mainScan;
    char keySource;
    uint8_t padding[45];
};

// Builds the sequences for a mapping such as "ctrl+a" or "Z". Each one wraps
// the main key in its own modifier down/up, so modifiers are never left held.
static PrecomputedKeyEvents computePrecomputedKeyEvents(std::string_view key_str) {
    PrecomputedKeyEvents evts{};
    bool hasAlt = (key_str.find("alt+") != std::string_view::npos);
    bool hasCtrl = (key_str.find("ctrl+") != std::string_view::npos);

    char lastChar = key_str.empty() ? '\0' : key_str.back();

    const CharInfoRec& info = CHAR_INFO_TABLE[(unsigned char)lastChar];
    WORD main_scan = SCAN_TABLE[(unsigned char)info.canonical];
    bool isShifted = info.shifted;

    int mods = (int)hasAlt | ((int)hasCtrl << 1) | ((int)isShifted << 2);
    const ModSequence& seq = modSequences[mods];

    {
        size_t idx = 0;
        for (size_t i = 0; i < seq.downCount; ++i) {
            evts.press[idx++] = makeKeybdInput(seq.down[i].scan, seq.down[i].flags);
        }
        evts.press[idx++] = makeKeybdInput(main_scan, SC_FLAG);
        for (size_t i = 0; i < seq.upCount; ++i) {
            evts.press[idx++] = makeKeybdInput(seq.up[i].scan, seq.up[i].flags);
        }
        evts.pressCount = idx;
    }
    {
        size_t idx = 0;
        for (size_t i = 0; i < seq.downCount; ++i) {
            evts.release[idx++] = makeKeybdInput(seq.down[i].scan, seq.down[i].flags);
        }
        evts.release[idx++] = makeKeybdInput(main_scan, SC_FLAG | KEYEVENTF_KEYUP);
        for (size_t i = 0; i < seq.upCount; ++i) {
            evts.release[idx++] = makeKeybdInput(seq.up[i].scan, seq.up[i].flags);
        }
        evts.releaseCount = idx;
    }
    evts.mainScan = main_scan;
    evts.keySource = lastChar;
    return evts;
}

// Lookup tables built by precomputeAllMappings.
static std::unordered_map<std::string, PrecomputedKeyEvents> g_precomputeMap;

// Per MIDI note, the 88-key (full) and 61-key (limited) events.
static PrecomputedKeyEvents* g_fullKeyEvents[128];
static PrecomputedKeyEvents* g_limitedKeyEvents[128];

// Velocity key for each MIDI velocity.
static char g_velocityMapping[128]{};
// Note after folding out-of-range notes into the 61-key window.
static int  g_adjustedNote[128]{};

// Which mapping holds each scancode down, and how many notes share it.
static PrecomputedKeyEvents* scancodeOwner[256]{};
static std::atomic<short>    scancodeCount[256];

// Rebuilds the tables above from the player's current mappings.
static void precomputeAllMappings(VirtualPianoPlayer& player) {
    g_precomputeMap.clear();

    std::fill(std::begin(g_fullKeyEvents), std::end(g_fullKeyEvents), nullptr);
    std::fill(std::begin(g_limitedKeyEvents), std::end(g_limitedKeyEvents), nullptr);

    for (int i = 0; i < 256; ++i) {
        scancodeOwner[i] = nullptr;
        scancodeCount[i].store(0, std::memory_order_relaxed);
    }

    for (int vel = 0; vel < 128; ++vel) {
        std::string ks = player.getVelocityKey(vel);
        g_velocityMapping[vel] = ks.empty() ? 0 : ks[0];
    }

    if (player.eightyEightKeyModeActive || !player.ENABLE_OUT_OF_RANGE_TRANSPOSE) {
        for (int n = 0; n < 128; ++n) {
            g_adjustedNote[n] = n;
        }
    }
    else {
        // Same fold as autoplay, so a note maps to the same key on both paths.
        for (int n = 0; n < 128; ++n) g_adjustedNote[n] = FoldOntoSixtyOneKeys(n);
    }

    for (int midi_n = 0; midi_n < 128; ++midi_n) {
        const char* noteStr = NOTE_NAME_CACHE[midi_n];
        if (!noteStr || !noteStr[0]) continue;

        const std::string& fullKey = player.full_key_mappings[noteStr];
        if (!fullKey.empty()) {
            auto it = g_precomputeMap.find(fullKey);
            if (it == g_precomputeMap.end()) {
                auto [newIt, _] = g_precomputeMap.emplace(fullKey, computePrecomputedKeyEvents(fullKey));
                it = newIt;
            }
            g_fullKeyEvents[midi_n] = &it->second;
        }
        const std::string& limitedKey = player.limited_key_mappings[noteStr];
        if (!limitedKey.empty()) {
            auto it2 = g_precomputeMap.find(limitedKey);
            if (it2 == g_precomputeMap.end()) {
                auto [newIt2, _] = g_precomputeMap.emplace(limitedKey, computePrecomputedKeyEvents(limitedKey));
                it2 = newIt2;
            }
            g_limitedKeyEvents[midi_n] = &it2->second;
        }
    }
    std::atomic_thread_fence(std::memory_order_release);
}

MIDI2Key::MIDI2Key(VirtualPianoPlayer* player)
    : m_selectedChannel(-1)
    , m_isActive(false)
    , m_player(player)
{
    for (auto& b : pressed) {
        b.store(false, std::memory_order_relaxed);
    }
    // Lets the player release this path's keys when it changes output target.
    if (m_player) m_player->set_live_release_hook([this] { ReleaseHeldKeys(); });
}

MIDI2Key::~MIDI2Key() {
    CloseDevice();
    // Unregister so the player never calls into a destroyed object.
    if (m_player) m_player->set_live_release_hook({});
}

void MIDI2Key::ReleaseHeldKeys() {
    for (int note = 0; note < 128; ++note) {
        if (!pressed[note].exchange(false, std::memory_order_relaxed)) continue;
        PrecomputedKeyEvents* evPtr = m_player && m_player->eightyEightKeyModeActive
            ? g_fullKeyEvents[note] : g_limitedKeyEvents[note];
        if (!evPtr) continue;
        const WORD sc = evPtr->mainScan & 0xFF;
        // Same rule as note-off: a shared scancode is released by the last
        // note holding it.
        if (scancodeOwner[sc] != evPtr) continue;
        const short remaining = scancodeCount[sc].fetch_sub(1, std::memory_order_relaxed);
        if (remaining != 1) continue;
        input_latency::send(static_cast<UINT>(evPtr->releaseCount), evPtr->release.data(), sizeof(INPUT));
        scancodeOwner[sc] = nullptr;
    }
    // The sustain key is not a note mapping, so release it separately.
    if (m_player && m_player->isSustainPressed) {
        const WORD spaceScan = SustainScan(*m_player);
        INPUT sustain = makeKeybdInput(spaceScan, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP);
        input_latency::send(1, &sustain, sizeof(INPUT));
        m_player->isSustainPressed = false;
    }
    m_lastVelocityKey = 0;
}

// An analog key plays whatever note the active layout maps to it, so the
// Wooting map must be built from the same layout (88- or 61-key) the note path
// reads. Called on open and on activation, so a layout change applies without
// reopening the device.
void MIDI2Key::ApplyWootingLayout(const std::wstring& deviceId) {
    if (!m_player || deviceId.empty()) return;
    if (BackendForDeviceId(deviceId) != MidiBackend::WootingAnalog) return;

    const bool full = m_player->eightyEightKeyModeActive.load(std::memory_order_relaxed);
    SetWootingScancodeNoteMap(WootingScancodeNoteMapFrom(
        full ? m_player->full_key_mappings : m_player->limited_key_mappings));

    // Refresh the analog settings along with the layout. The layout covers only
    // the white keys; SHIFT_AMOUNT is what makes black keys playable.
    const auto& configured = midi::Config::getInstance().wooting;
    SetWootingAnalogSettings({static_cast<float>(configured.TRIGGER_THRESHOLD),
                              static_cast<float>(configured.RELEASE_FRACTION),
                              configured.SHIFT_AMOUNT,
                              static_cast<float>(configured.VELOCITY_SCALE)});
}

void MIDI2Key::OpenDevice(const std::wstring& deviceId) {
    CloseDevice();
    if (deviceId.empty()) return;

    ApplyWootingLayout(deviceId);

    m_input = CreateMidiInput(BackendForDeviceId(deviceId));
    if (!m_input) return;

    const bool opened = m_input->open(deviceId,
        [this](uint64_t timestampQpc, const uint8_t* data, size_t length) {
            this->ProcessMidiMessage(timestampQpc, data, length);
        });
    if (!opened) {
        std::wcerr << L"Failed to open MIDI device: " << deviceId << std::endl;
        m_input.reset();
        return;
    }
    m_selectedDevice = deviceId;
}

void MIDI2Key::CloseDevice() {
    if (m_input) {
        m_input->close();
        m_input.reset();
    }
    m_selectedDevice.clear();
}

void MIDI2Key::Quiesce() {
    m_isActive.store(false, std::memory_order_seq_cst);
    while (m_inFlight.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
}

void MIDI2Key::SetMidiChannel(int channel) {
    if (channel == m_selectedChannel.load(std::memory_order_relaxed)) return;
    // The channel filter runs before note-off handling, so release notes held
    // on the old channel first.
    const bool active = m_isActive.load(std::memory_order_acquire);
    if (active) { Quiesce(); ReleaseHeldKeys(); }
    m_selectedChannel.store(channel, std::memory_order_release);
    if (active) m_isActive.store(true, std::memory_order_release);
}

bool MIDI2Key::IsActive() const {
    return m_isActive.load(std::memory_order_relaxed);
}

void MIDI2Key::SetActive(bool active) {
    // Re-arming rebuilds the tables callbacks read and resets scancode
    // ownership, so drain the callbacks and release held keys with the old
    // tables first.
    const bool rearm = active && m_isActive.load(std::memory_order_acquire);
    if (rearm) { Quiesce(); ReleaseHeldKeys(); }
    if (active && m_player) {
        precomputeAllMappings(*m_player);
    }
    m_isActive.store(active, std::memory_order_release);
    // The layout may have changed since the device was opened.
    if (active) ApplyWootingLayout(m_selectedDevice);
}

const std::wstring& MIDI2Key::GetSelectedDevice() const {
    return m_selectedDevice;
}

int MIDI2Key::GetSelectedChannel() const {
    return m_selectedChannel;
}

void MIDI2Key::ProcessMidiMessage(uint64_t timestampQpc, const uint8_t* bytes, size_t length) {
    struct InFlight {
        std::atomic<int>& count;
        explicit InFlight(std::atomic<int>& c) : count(c) { count.fetch_add(1, std::memory_order_seq_cst); }
        ~InFlight() { count.fetch_sub(1, std::memory_order_release); }
    } inFlight(m_inFlight);
    if (!m_isActive.load(std::memory_order_seq_cst)) return;
    // Once per delivering thread; see setThreadToRealTime.
    thread_local bool boosted = false;
    if (!boosted) { boosted = true; setThreadToRealTime(); }
    if (!bytes || length < 3) return;
    if (bytes[1] > 127 || bytes[2] > 127) return;
    uint8_t status = bytes[0];
    uint8_t cmd = status & 0xF0;
    uint8_t channel = status & 0x0F;
    const int selectedChannel = m_selectedChannel.load(std::memory_order_relaxed);
    if (selectedChannel >= 0 && channel != (uint8_t)selectedChannel) return;
    if (cmd != 0x90 && cmd != 0x80 && !(cmd == 0xB0 && bytes[1] == 64)) return;

    // After the channel filter so the histogram covers only the selected
    // channel, and before the enable checks so it works with velocity output off.
    velocity_telemetry::observe(bytes, length);

    input_latency::Trace trace(input_latency::Source::LiveKeys,
        cmd == 0xB0 ? input_latency::Kind::Sustain :
        cmd == 0x90 && bytes[2] > 0 ? input_latency::Kind::NoteOn : input_latency::Kind::NoteOff,
        timestampQpc);

    VirtualPianoPlayer& p = *m_player;

    // On the MIDI target, forward the message unchanged. The 61-key fold and
    // the 88-key flag are keystroke concepts and must not reach the wire.
    // Live input has no transpose; if one is added, apply it to bytes[1] here.
    // pressed[] and scancodeOwner[] track keystrokes only and stay untouched.
    if (p.output_target.load(std::memory_order_acquire) == VirtualPianoPlayer::OutputTarget::MidiDevice) {
        p.send_midi_output(bytes, length);
        return;
    }

    // Handle Note On (0x90 with velocity > 0)
    if (cmd == 0x90 && bytes[2] > 0) {
        uint8_t note = bytes[1];
        uint8_t velocity = bytes[2];

        // The velocity tap goes out in the same SendInput batch as the note
        // press. The buffer must be local: several input threads can be in
        // here at once.
        INPUT batch[24];
        size_t batchCount = 0;
        bool batchSent = false;
        // While Left Shift is held for the Wooting shift amount the game sees
        // it down, so the note would read as its sharp and the velocity tap as
        // a note. Lift it in the same batch so the keyboard's Shift repeat
        // cannot land in between. Mappings that need Shift press it themselves.
        const bool lifted = WootingShiftHeld();
        if (lifted) batch[batchCount++] = makeKeybdInput(SHIFT_SCAN, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP);
        if (p.enable_velocity_keypress.load(std::memory_order_relaxed)) {
            char newVelKey = g_velocityMapping[velocity];
            // Substitute a free key only when a tap is actually needed.
            if (newVelKey != m_lastVelocityKey)
                newVelKey = VirtualPianoPlayer::nearest_free_velocity_key(newVelKey, [](char key) {
                    return scancodeCount[SCAN_TABLE[(unsigned char)key] & 0xFF].load(std::memory_order_relaxed) > 0;
                });
            if (newVelKey && newVelKey != m_lastVelocityKey) {
                m_lastVelocityKey = newVelKey;
                WORD sc = SCAN_TABLE[(unsigned char)newVelKey];
                // Configurable modifier, Alt by default. Autoplay reads the
                // same field in build_velocity_tap, so both paths send the
                // same tap.
                const WORD mod = p.velocity_modifier_scan.load(std::memory_order_acquire);
                batch[batchCount++] = makeKeybdInput(mod, KEYEVENTF_SCANCODE);                 // modifier down
                batch[batchCount++] = makeKeybdInput(sc, KEYEVENTF_SCANCODE);                  // Key down
                batch[batchCount++] = makeKeybdInput(sc, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP); // Key up
                batch[batchCount++] = makeKeybdInput(mod, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP); // modifier up
            }
        }
        if (p.enable_volume_adjustment.load(std::memory_order_relaxed)) {
            int target_vol = p.volume_lookup[velocity];
            int curr_vol = p.current_volume.load(std::memory_order_relaxed);
            int diff = target_vol - curr_vol;
            int step_size = midi::Config::getInstance().volume.VOLUME_STEP;
            if (std::abs(diff) >= step_size) {
                // Volume keys from config.json, the same ones autoplay sends.
                const WORD sc = (diff > 0) ? p.volume_up_key_code : p.volume_down_key_code;
                const DWORD extended = p.volume_key_extended(sc) ? KEYEVENTF_EXTENDEDKEY : 0;
                const int steps = (std::min)(std::abs(diff) / step_size, 20);
                INPUT inputs[40];
                for (int i = 0; i < steps; ++i) {
                    inputs[i * 2] = makeKeybdInput(sc, KEYEVENTF_SCANCODE | extended);
                    inputs[i * 2 + 1] = makeKeybdInput(sc, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | extended);
                }
                input_latency::send(steps * 2, inputs, sizeof(INPUT));
                p.current_volume.store(target_vol, std::memory_order_relaxed);
            }
        }

        // Press the note
        int midi_n = g_adjustedNote[note];
        PrecomputedKeyEvents* evPtr = p.eightyEightKeyModeActive ? g_fullKeyEvents[midi_n] : g_limitedKeyEvents[midi_n];
        if (evPtr) {
            bool wasNotPressed = !pressed[midi_n].exchange(true, std::memory_order_relaxed);
            if (wasNotPressed) {
                WORD sc = evPtr->mainScan & 0xFF;
                _mm_prefetch((const char*)&scancodeOwner[sc], _MM_HINT_T0);
                _mm_prefetch((const char*)&scancodeCount[sc], _MM_HINT_T0);
                PrecomputedKeyEvents* ownerPtr = scancodeOwner[sc];
                if (ownerPtr && ownerPtr != evPtr) {
                    short oldCount = scancodeCount[sc].exchange(0, std::memory_order_relaxed);
                    if (oldCount > 0) {
                        input_latency::send((UINT)ownerPtr->releaseCount, ownerPtr->release.data(), sizeof(INPUT));
                    }
                    scancodeOwner[sc] = nullptr;
                }
                short cnt = scancodeCount[sc].load(std::memory_order_relaxed);
                if (cnt == 0) {
                    if (batchCount + evPtr->pressCount <= std::size(batch)) {
                        for (size_t i = 0; i < evPtr->pressCount; ++i)
                            batch[batchCount + i] = evPtr->press[i];
                        input_latency::send((UINT)(batchCount + evPtr->pressCount), batch, sizeof(INPUT));
                        batchSent = true;
                    }
                    else {
                        // Unreachable with any accepted mapping. Send in two
                        // calls so no key is left down.
                        if (batchCount) input_latency::send((UINT)batchCount, batch, sizeof(INPUT));
                        input_latency::send((UINT)evPtr->pressCount, evPtr->press.data(), sizeof(INPUT));
                        batchSent = true;
                    }
                    scancodeOwner[sc] = evPtr;
                    scancodeCount[sc].store(1, std::memory_order_relaxed);
                }
                else {
                    scancodeCount[sc].fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        // Send the velocity tap even when the note sent no press, or
        // m_lastVelocityKey would claim a velocity the game never received.
        // A Shift lift on its own is not sent.
        if (batchCount > (lifted ? 1u : 0u) && !batchSent) input_latency::send((UINT)batchCount, batch, sizeof(INPUT));
    }
    // Handle Note Off (0x80 or 0x90 with velocity == 0)
    else if ((cmd == 0x90 && bytes[2] == 0) || cmd == 0x80) {
        uint8_t note = bytes[1];
        int midi_n = g_adjustedNote[note];
        PrecomputedKeyEvents* evPtr = p.eightyEightKeyModeActive ? g_fullKeyEvents[midi_n] : g_limitedKeyEvents[midi_n];
        if (evPtr) {
            bool wasPressed = pressed[midi_n].exchange(false, std::memory_order_relaxed);
            if (wasPressed) {
                WORD sc = evPtr->mainScan & 0xFF;
                PrecomputedKeyEvents* ownerPtr = scancodeOwner[sc];
                if (ownerPtr == evPtr) {
                    short c = scancodeCount[sc].fetch_sub(1, std::memory_order_relaxed);
                    if (c == 1) {
                        input_latency::send((UINT)evPtr->releaseCount, evPtr->release.data(), sizeof(INPUT));
                        scancodeOwner[sc] = nullptr;
                    }
                }
            }
        }
    }
    // Handle Control Change (sustain pedal, controller 64)
    else if (cmd == 0xB0 && bytes[1] == 64 && p.currentSustainMode != SustainMode::IG) {
        bool pedal_on = (bytes[2] >= g_sustainCutoff);
        bool shouldPress = (p.currentSustainMode == SustainMode::SPACE_DOWN) ? pedal_on : !pedal_on;
        if (shouldPress != p.isSustainPressed) {
            const WORD spaceScan = SustainScan(*m_player);
            // makeKeybdInput leaves wVk at 0, so KEYEVENTF_SCANCODE is required;
            // without it Windows injects virtual key 0, though a low-level hook
            // still sees the scancode.
            INPUT sustain = makeKeybdInput(spaceScan,
                shouldPress ? KEYEVENTF_SCANCODE : (KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP));
            input_latency::send(1, &sustain, sizeof(INPUT));
            p.isSustainPressed = shouldPress;
        }
    }
}
