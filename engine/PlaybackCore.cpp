#include "PlaybackSystem.hpp"
#include "InputHeader.h"
#include "CrashGuard.hpp"
#include "timer.h"
#include <cmath>
#include <algorithm>
#include <future>
#include <thread>
#include <condition_variable>
#include <iostream>

#pragma comment(lib, "avrt.lib")

// Local mutex for input operations.
static std::mutex s_inputMutex;

// The keystrokes play_batch is gathering on this thread into one SendInput
// call, or null. Per thread, so a key another thread sends meanwhile goes out
// on its own and never into a batch it does not belong to.
static thread_local std::vector<INPUT>* t_gathering = nullptr;

// Windows version query typedef.
typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

//----------------------------------------------------------------
// Global definitions.
HANDLE VirtualPianoPlayer::command_event = nullptr;
double g_totalSongSeconds = 0.0;


// 1e9 / rdtsc_timer_get_frequency(), set once the TSC is calibrated.
static double cyclesToNs = 0.0;  // TSC cycles -> nanoseconds

// The calibration busy-waits for MAX_PASSES x MEASURE_SEC, so it runs once per
// process and off the constructing thread. Everything that reads cyclesToNs
// calls AwaitClock first; after the first success that is one atomic load.
static std::shared_future<double> g_clock;
static std::once_flag g_clockOnce;
static std::atomic<bool> g_clockReady{ false };

static void StartClock(int passes, double seconds) {
    std::call_once(g_clockOnce, [=] {
        g_clock = std::async(std::launch::async, [=] {
            rdtsc_timer_init(passes, seconds);
            const double frequency = rdtsc_timer_get_frequency(); // in Hz
            if (frequency > 1e5) cyclesToNs = 1.0e9 / frequency;
            return frequency;
        }).share();
    });
}

// Waits for the calibration if it is still running; throws if it failed.
static void AwaitClock() {
    if (g_clockReady.load(std::memory_order_acquire)) return;
    if (g_clock.get() <= 1e5)
        throw std::runtime_error("High-res TSC timer not available or calibration failed.");
    g_clockReady.store(true, std::memory_order_release);
}

struct KeySequence {
    std::vector<INPUT> events_press;
    std::vector<INPUT> events_release;
};
static std::unordered_map<std::string, KeySequence> g_keyCache;

const std::array<WORD, 256> VirtualPianoPlayer::SCAN_TABLE_AUTO = []() {
    std::array<WORD, 256> table{};
    table.fill(0);
    // Numbers
    table[static_cast<unsigned char>('1')] = 0x02;
    table[static_cast<unsigned char>('2')] = 0x03;
    table[static_cast<unsigned char>('3')] = 0x04;
    table[static_cast<unsigned char>('4')] = 0x05;
    table[static_cast<unsigned char>('5')] = 0x06;
    table[static_cast<unsigned char>('6')] = 0x07;
    table[static_cast<unsigned char>('7')] = 0x08;
    table[static_cast<unsigned char>('8')] = 0x09;
    table[static_cast<unsigned char>('9')] = 0x0A;
    table[static_cast<unsigned char>('0')] = 0x0B;

    // Lowercase letters
    table[static_cast<unsigned char>('q')] = 0x10;
    table[static_cast<unsigned char>('w')] = 0x11;
    table[static_cast<unsigned char>('e')] = 0x12;
    table[static_cast<unsigned char>('r')] = 0x13;
    table[static_cast<unsigned char>('t')] = 0x14;
    table[static_cast<unsigned char>('y')] = 0x15;
    table[static_cast<unsigned char>('u')] = 0x16;
    table[static_cast<unsigned char>('i')] = 0x17;
    table[static_cast<unsigned char>('o')] = 0x18;
    table[static_cast<unsigned char>('p')] = 0x19;
    table[static_cast<unsigned char>('a')] = 0x1E;
    table[static_cast<unsigned char>('s')] = 0x1F;
    table[static_cast<unsigned char>('d')] = 0x20;
    table[static_cast<unsigned char>('f')] = 0x21;
    table[static_cast<unsigned char>('g')] = 0x22;
    table[static_cast<unsigned char>('h')] = 0x23;
    table[static_cast<unsigned char>('j')] = 0x24;
    table[static_cast<unsigned char>('k')] = 0x25;
    table[static_cast<unsigned char>('l')] = 0x26;
    table[static_cast<unsigned char>('z')] = 0x2C;
    table[static_cast<unsigned char>('x')] = 0x2D;
    table[static_cast<unsigned char>('c')] = 0x2E;
    table[static_cast<unsigned char>('v')] = 0x2F;
    table[static_cast<unsigned char>('b')] = 0x30;
    table[static_cast<unsigned char>('n')] = 0x31;
    table[static_cast<unsigned char>('m')] = 0x32;

    // Punctuation (shifted numbers)
    table[static_cast<unsigned char>('!')] = 0x02;
    table[static_cast<unsigned char>('@')] = 0x03;
    table[static_cast<unsigned char>('#')] = 0x04;
    table[static_cast<unsigned char>('$')] = 0x05;
    table[static_cast<unsigned char>('%')] = 0x06;
    table[static_cast<unsigned char>('^')] = 0x07;
    table[static_cast<unsigned char>('&')] = 0x08;
    table[static_cast<unsigned char>('*')] = 0x09;
    table[static_cast<unsigned char>('(')] = 0x0A;
    table[static_cast<unsigned char>(')')] = 0x0B;

    // Uppercase letters (map same as lowercase)
    table[static_cast<unsigned char>('Q')] = 0x10;
    table[static_cast<unsigned char>('W')] = 0x11;
    table[static_cast<unsigned char>('E')] = 0x12;
    table[static_cast<unsigned char>('R')] = 0x13;
    table[static_cast<unsigned char>('T')] = 0x14;
    table[static_cast<unsigned char>('Y')] = 0x15;
    table[static_cast<unsigned char>('U')] = 0x16;
    table[static_cast<unsigned char>('I')] = 0x17;
    table[static_cast<unsigned char>('O')] = 0x18;
    table[static_cast<unsigned char>('P')] = 0x19;
    table[static_cast<unsigned char>('A')] = 0x1E;
    table[static_cast<unsigned char>('S')] = 0x1F;
    table[static_cast<unsigned char>('D')] = 0x20;
    table[static_cast<unsigned char>('F')] = 0x21;
    table[static_cast<unsigned char>('G')] = 0x22;
    table[static_cast<unsigned char>('H')] = 0x23;
    table[static_cast<unsigned char>('J')] = 0x24;
    table[static_cast<unsigned char>('K')] = 0x25;
    table[static_cast<unsigned char>('L')] = 0x26;
    table[static_cast<unsigned char>('Z')] = 0x2C;
    table[static_cast<unsigned char>('X')] = 0x2D;
    table[static_cast<unsigned char>('C')] = 0x2E;
    table[static_cast<unsigned char>('V')] = 0x2F;
    table[static_cast<unsigned char>('B')] = 0x30;
    table[static_cast<unsigned char>('N')] = 0x31;
    table[static_cast<unsigned char>('M')] = 0x32;
    return table;
}();


std::pair<std::map<std::string, std::string>, std::map<std::string, std::string>>
VirtualPianoPlayer::define_key_mappings() {
    auto& cfg = midi::Config::getInstance();
    return { cfg.key_mappings["LIMITED"], cfg.key_mappings["FULL"] };
}

// ------------------------------
// KeySequence precomputation helper
// ------------------------------
static KeySequence computeKeySequence(const std::string& key) {
    KeySequence seq;
    // Detect modifiers
    bool hasAlt  = (key.find("alt+")  != std::string::npos);
    bool hasCtrl = (key.find("ctrl+") != std::string::npos);

    char lastChar   = key.empty() ? '\0' : key.back();
    bool shifted    = std::isupper(static_cast<unsigned char>(lastChar)) ||
                      std::ispunct(static_cast<unsigned char>(lastChar));
    char lookupChar = shifted ? static_cast<char>(std::tolower(lastChar)) : lastChar;

    WORD mainScan   = VirtualPianoPlayer::SCAN_TABLE_AUTO[static_cast<unsigned char>(lookupChar)];
    constexpr WORD SHIFT_SCAN = 0x2A;
    constexpr WORD CTRL_SCAN  = 0x1D;
    constexpr WORD ALT_SCAN   = 0x38;

    // Build press sequence
    std::vector<INPUT> pressSeq;
    if (shifted && mainScan != 0) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = mainScan;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        pressSeq.push_back(input);
    }
    if (hasAlt) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = ALT_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        pressSeq.push_back(input);
    }
    if (hasCtrl) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = CTRL_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        pressSeq.push_back(input);
    }
    if (shifted) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = SHIFT_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        pressSeq.push_back(input);
    }
    if (mainScan != 0) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = mainScan;
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        pressSeq.push_back(input);
    }
    if (shifted) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = SHIFT_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        pressSeq.push_back(input);
    }
    if (hasCtrl) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = CTRL_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        pressSeq.push_back(input);
    }
    if (hasAlt) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = ALT_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        pressSeq.push_back(input);
    }

    // Build release sequence (simulate full tap)
    std::vector<INPUT> releaseSeq;
    if (hasAlt) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = ALT_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        releaseSeq.push_back(input);
    }
    if (hasCtrl) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = CTRL_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        releaseSeq.push_back(input);
    }
    if (shifted) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = SHIFT_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        releaseSeq.push_back(input);
    }
    if (mainScan != 0) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = mainScan;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        releaseSeq.push_back(input);
    }
    if (shifted) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = SHIFT_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        releaseSeq.push_back(input);
    }
    if (hasCtrl) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = CTRL_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        releaseSeq.push_back(input);
    }
    if (hasAlt) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = ALT_SCAN;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        releaseSeq.push_back(input);
    }
    seq.events_press   = std::move(pressSeq);
    seq.events_release = std::move(releaseSeq);
    return seq;
}

NoteEvent::NoteEvent() noexcept
    : time(std::chrono::nanoseconds::zero()),
      note(""),
      action(EventType::Press),
      velocity(0),
      isSustain(false),
      sustainValue(0),
      trackIndex(-1)
{}

NoteEvent::NoteEvent(std::chrono::nanoseconds t,
                     std::string_view n,
                     EventType a,
                     int v,
                     bool s,
                     int sv,
                     int trackIdx) noexcept
    : time(t),
      note(n),
      action(a),
      velocity(v),
      isSustain(s),
      sustainValue(sv),
      trackIndex(trackIdx)
{}

bool NoteEvent::operator>(const NoteEvent& other) const noexcept {
    return time > other.time;
}

//----------------------------------------------------------------
// NoteEventPool Implementation.
NoteEventPool::NoteEventPool()
    : head(nullptr),
      current(nullptr),
      end(nullptr),
      allocated_count(0)
{
    allocateNewBlock();
}

NoteEventPool::~NoteEventPool() {
    while (head) {
        Block* next = head->next;
        _aligned_free(head);
        head = next;
    }
}

void NoteEventPool::allocateNewBlock() {
    Block* newBlock = static_cast<Block*>(_aligned_malloc(sizeof(Block), CACHE_LINE_SIZE));
    if (!newBlock)
        throw std::bad_alloc();
    newBlock->next = head;
    head           = newBlock;
    current        = newBlock->data;
    end            = current + BLOCK_SIZE;
}

void NoteEventPool::reset() {
    // Keep only the newest block; otherwise every Play and seek would leave
    // another song's worth of blocks allocated.
    while (head->next) {
        Block* older = head->next;
        head->next = older->next;
        _aligned_free(older);
    }
    current = head->data;
    end     = current + BLOCK_SIZE;
    allocated_count.store(0, std::memory_order_relaxed);
}

size_t NoteEventPool::getAllocatedCount() const {
    return allocated_count.load(std::memory_order_relaxed);
}

//----------------------------------------------------------------
// PlaybackControl Implementation.
void PlaybackControl::requestSkip(std::chrono::seconds amount) {
    std::lock_guard<std::mutex> lock(mutex);
    pending_command = Command::SKIP;
    command_amount  = amount;
    command_processed.store(false, std::memory_order_release);
    SetEvent(VirtualPianoPlayer::command_event); // Signal command event
}

void PlaybackControl::requestRewind(std::chrono::seconds amount) {
    std::lock_guard<std::mutex> lock(mutex);
    pending_command = Command::REWIND;
    command_amount  = amount;
    command_processed.store(false, std::memory_order_release);
    SetEvent(VirtualPianoPlayer::command_event); // Signal command event
}

bool PlaybackControl::hasCommand() const {
    return (pending_command != Command::NONE) &&
           !command_processed.load(std::memory_order_acquire);
}

PlaybackControl::State PlaybackControl::processCommand(const State& current_state,
                                                       double speed,
                                                       size_t /*buffer_size*/)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (command_processed.load(std::memory_order_acquire))
        return current_state;

    State new_state = current_state;
    auto scaled = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(command_amount.count() * speed)
    );

    switch (pending_command) {
    case Command::SKIP:
        new_state.position += scaled;
        new_state.needs_reset = true;
        break;
    case Command::REWIND:
        new_state.position = (scaled > new_state.position)
                             ? std::chrono::nanoseconds(0)
                             : new_state.position - scaled;
        new_state.needs_reset = true;
        break;
    default:
        break;
    }

    command_processed.store(true, std::memory_order_release);
    pending_command = Command::NONE;
    return new_state;
}

//----------------------------------------------------------------
// IsWin7OrWin8_Real: Check if OS is exactly Windows 7 or 8.
bool IsWin7OrWin8_Real() {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll)
        return false;
    auto pRtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
        GetProcAddress(hNtdll, "RtlGetVersion"));
    if (!pRtlGetVersion)
        return false;

    RTL_OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (pRtlGetVersion(&info) != 0)
        return false;

    return (info.dwMajorVersion == 6 &&
           (info.dwMinorVersion == 1 || info.dwMinorVersion == 2));
}
//----------------------------------------------------------------
// VirtualPianoPlayer Implementation.
VirtualPianoPlayer::VirtualPianoPlayer(bool listenForHotkeys,
                                     const std::filesystem::path& configPath) noexcept(false)
{
    try {
        midi::Config::getInstance().loadFromFile(configPath);
    }
    catch (const midi::ConfigException& e) {
        std::cerr << "Configuration error: " << e.what()
            << "\nLoading default settings...\n";
        midi::Config::getInstance().setDefaults();
        // Write defaults only when there is no file: a config that fails to
        // parse still holds the user's mappings and must not be overwritten.
        std::error_code missing;
        if (!std::filesystem::exists(configPath, missing)) {
            try {
                midi::Config::getInstance().saveToFile(configPath);
            }
            catch (const midi::ConfigException& e2) {
                std::cerr << "Failed to save default config: " << e2.what() << "\n";
            }
        }
    }

    // Needs the loaded config; must run before any velocity tap is built.
    apply_velocity_modifier();

    if (IsWin7OrWin8_Real()) {
        MessageBoxA(nullptr,
            "Incompatible OS detected.\nThis software requires Windows 8.1 or later.",
            "Incompatibility Warning",
            MB_ICONERROR | MB_OK);
        throw std::runtime_error("Incompatible OS (Windows 7/8) detected");
    }
    // The timer tick is asked for by play_notes and live input, not here: held
    // for the app's life it cost every idle minute a faster tick.
    // Windows 11 ignores timeBeginPeriod for a process whose windows are all
    // minimized or occluded, which drops the tick back to 15.6 ms. Opt out of
    // that throttling; older builds reject IGNORE_TIMER_RESOLUTION, so retry without it.
    {
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
        PROCESS_POWER_THROTTLING_STATE throttling{};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        throttling.StateMask = 0;
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling))) {
            throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
        }
    }
    auto mappings = define_key_mappings();
    limited_key_mappings = std::move(mappings.first);
    full_key_mappings = std::move(mappings.second);
    note_buffer.reserve(1 << 20);
    waitable_timer = CreateWaitableTimerEx(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS
    );
    waitable_timer_precise = waitable_timer != nullptr;
    if (!waitable_timer) {
        waitable_timer = CreateWaitableTimer(nullptr, FALSE, nullptr);
        if (!waitable_timer) {
            throw std::runtime_error("Failed to create waitable timer");
        }
    }
    command_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!command_event) {
        CloseHandle(waitable_timer);
        throw std::runtime_error("Failed to create command event");
    }
    try {
        sustain_key_code = stringToVK(midi::Config::getInstance().hotkeys.SUSTAIN_KEY);
        volume_up_key_code = vkToScanCode(stringToVK(midi::Config::getInstance().hotkeys.VOLUME_UP_KEY));
        volume_down_key_code = vkToScanCode(stringToVK(midi::Config::getInstance().hotkeys.VOLUME_DOWN_KEY));
        // Only send KEYEVENTF_EXTENDEDKEY for keys that are extended; the game
        // does not recognise [ or ] sent as extended.
        volume_up_extended = isExtendedVK(stringToVK(midi::Config::getInstance().hotkeys.VOLUME_UP_KEY));
        volume_down_extended = isExtendedVK(stringToVK(midi::Config::getInstance().hotkeys.VOLUME_DOWN_KEY));
    }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string("Error mapping hotkeys: ") + e.what());
    }

    if (sustain_key_code == 0 || volume_up_key_code == 0 || volume_down_key_code == 0) {
        throw std::runtime_error("Invalid hotkey configuration. Check your config file.");
    }

    if (listenForHotkeys)
        hotkey_thread = std::make_unique<std::jthread>(&VirtualPianoPlayer::hotkey_listener, this);

    // The timing settings are only valid once config.json is loaded. Returns
    // at once; starting playback waits for the calibration and reports a failure.
    {
        const auto& timing = midi::Config::getInstance().autoplayer_timing;
        StartClock(timing.MAX_PASSES, timing.MEASURE_SEC);
    }

    // Never throws: falls back to SendInput when the syscall cannot be built.
    initializeKeyCache();
    set_crash_key_ups();
}

void VirtualPianoPlayer::set_crash_key_ups() {
    std::vector<INPUT> ups;
    std::set<WORD> scans;
    for (const auto* mappings : {&limited_key_mappings, &full_key_mappings})
        for (const auto& [note, key] : *mappings) {
            if (key.empty()) continue;
            // A shifted or Ctrl mapping still holds the letter's own key.
            const WORD scan = SCAN_TABLE_AUTO[static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(key.back())))];
            if (scan && scans.insert(scan).second) {
                INPUT up{};
                up.type = INPUT_KEYBOARD;
                up.ki.wScan = scan;
                up.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                ups.push_back(up);
            }
        }
    for (const WORD vk : {sustain_key_code, static_cast<WORD>(VK_SHIFT), static_cast<WORD>(VK_CONTROL), static_cast<WORD>(VK_MENU)}) {
        if (!vk) continue;
        INPUT up{};
        up.type = INPUT_KEYBOARD;
        up.ki.wVk = vk;
        up.ki.wScan = static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
        up.ki.dwFlags = KEYEVENTF_KEYUP;
        ups.push_back(up);
    }
    crash::SetKeyUps(ups.data(), ups.size());
}

VirtualPianoPlayer::~VirtualPianoPlayer() {
    if (isSustainPressed) {
        releaseKey(sustain_key_code);
        isSustainPressed = false;
    }

    should_stop.store(true, std::memory_order_release);
    signalPlayback();
    if (playback_thread && playback_thread->joinable()) {
        playback_thread->join();
    }
    if (performer) { performer_api.destroy(performer); performer = nullptr; }

    if (command_event) {
        CloseHandle(command_event);
    }

    if (waitable_timer) {
        CloseHandle(waitable_timer);
    }

    if (live_timer_held) {
        hold_timer_resolution(false);
        live_timer_held = false;
    }

    hotkey_stop.store(true, std::memory_order_release);
    if (hotkey_thread && hotkey_thread->joinable()) {
        hotkey_thread->join();
    }
}

std::chrono::nanoseconds VirtualPianoPlayer::get_adjusted_time() noexcept {
    // Paused, in Tap mode or held while the game is behind, the clock is
    // stopped; in Tap the position is the last note tapped.
    if (paused.load(std::memory_order_relaxed) || clock_frozen.load(std::memory_order_relaxed) ||
        held_behind.load(std::memory_order_acquire))
        return total_adjusted_time;
    uint64_t current_tsc = __rdtsc();
    uint64_t tick_diff   = current_tsc - last_resume_tsc;

    double rawNs = double(tick_diff) * cyclesToNs * current_speed;
    auto adjusted_ns = static_cast<std::chrono::nanoseconds::rep>(rawNs + 0.5);

    return total_adjusted_time + std::chrono::nanoseconds(adjusted_ns);
}

// RawNoteEvent and NoteEvent hold string_views, so every note name, the
// score's and the keys the take substitutes, comes from this static table.
std::string_view VirtualPianoPlayer::stable_note_name(int midi_note) {
    static const std::array<std::string, 128> names = [] {
        static constexpr const char* kPitches[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        std::array<std::string, 128> built;
        for (int n = 0; n < 128; ++n) built[n] = std::string(kPitches[n % 12]) + std::to_string(n / 12 - 1);
        return built;
    }();
    return names[std::clamp(midi_note, 0, 127)];
}

void VirtualPianoPlayer::prepare_event_queue() {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    note_buffer.clear();
    event_pool.reset();
    scheduled.clear();   // a new take holds nothing an action scheduled
    scheduled_until.clear();

    for (const auto& e : note_events) {
        EventType act = e.action;
        auto time_ns  = e.time;
        int vel       = e.velocity;
        int tIdx      = e.trackIndex;

        bool isSust   = PedalForName(e.note_or_control) >= 0;
        int sVal      = isSust ? (vel & 0xFF) : 0;
        int realVel   = isSust ? 0 : vel;

        note_buffer.push_back(event_pool.allocate(time_ns,
                                                  e.note_or_control,
                                                  act,
                                                  realVel,
                                                  isSust,
                                                  sVal,
                                                  tIdx));
    }

    const auto byTime = [](const NoteEvent* a, const NoteEvent* b) { return a->time < b->time; };
    std::stable_sort(note_buffer.begin(), note_buffer.end(), byTime);

    const std::vector<NoteEvent*> scoreOrder = note_buffer;   // take indices refer to this order

    // Build the take. note_events stays the score (seek, position and duration
    // read it); only note_buffer is displaced. The performer, if any, decides
    // what is skipped; without one nothing is, and a mute is kept at dispatch.
    std::string settings;
    { std::lock_guard takeLock(take_mutex); settings = performer_settings; }
    const size_t count = scoreOrder.size();
    std::vector<qm_score_event> score(count);
    // What nobody will hear is marked skip: a muted or unsoloed track's events,
    // and on keystrokes a note with no key after Transpose and fold whose
    // octave doubles have none either.
    const bool keystrokes = output_target.load(std::memory_order_acquire) != OutputTarget::MidiDevice;
    const auto& mappings = eightyEightKeyModeActive ? full_key_mappings : limited_key_mappings;
    const auto keyed = [&](std::string_view note) {
        const auto found = mappings.find(sounding_note(note));
        return found != mappings.end() && !found->second.empty();
    };
    const auto sounds = [&](std::string_view note) {
        if (!keystrokes || keyed(note)) return true;
        for (const int8_t doubled : octave_doubles[std::clamp(note_name_to_midi(note), 0, 127)])
            if (doubled >= 0 && keyed(stable_note_name(doubled))) return true;
        return false;
    };
    for (size_t i = 0; i < count; ++i) {
        const auto* e = scoreOrder[i];
        const bool heard = isTrackEnabled(e->trackIndex) && (e->isSustain || sounds(e->note));
        // A pedal's velocity is its value and which pedal it is (Performer.hpp).
        score[i] = { e->time.count(), e->isSustain ? -1 : note_name_to_midi(e->note),
                     e->isSustain ? (e->sustainValue & 0xFF) | std::max(0, PedalForName(e->note)) << 12 : e->velocity, e->trackIndex, -1,
                     static_cast<uint8_t>(e->action == EventType::Press), static_cast<uint8_t>(!heard) };
    }
    PairScore(score);
    const qm_take_event* events = nullptr;
    size_t taken = 0;
    if (performer)
        taken = performer_api.build(performer, performer_on.load(std::memory_order_relaxed), settings.c_str(),
                                    score.data(), count, take_seed, current_speed, &events);
    if (!events || taken < count) {
        // No usable take: play the score, with each press's hold taken from its release.
        for (size_t i = 0; i < count; ++i) {
            auto& e = *scoreOrder[i];
            e.skip = false;
            if (!e.isSustain && e.action == EventType::Press && score[i].mate >= 0)
                e.hold = scoreOrder[score[i].mate]->time - e.time;
        }
        return;
    }
    // Rebuild note_buffer in take order; step indices refer to it.
    note_buffer.clear();
    for (size_t i = 0; i < taken; ++i) {
        const auto& t = events[i];
        if (t.score < 0 || static_cast<size_t>(t.score) >= count) {
            // A key the take adds, such as a neighbouring key caught with the note.
            auto* added = event_pool.allocate(std::chrono::nanoseconds(t.time), stable_note_name(t.pitch),
                                              t.press ? EventType::Press : EventType::Release, t.velocity, false, 0, t.track);
            added->hold = std::chrono::nanoseconds(t.hold);
            added->skip = t.skip != 0;
            note_buffer.push_back(added);
            continue;
        }
        auto& e = *scoreOrder[t.score];
        e.time = std::chrono::nanoseconds(t.time);
        e.skip = t.skip != 0;
        if (!e.isSustain) {
            e.velocity = t.velocity;
            // Wrong-key slip: the press and its release both use the key actually struck.
            if (t.pitch >= 0 && t.pitch != score[t.score].pitch) e.note = stable_note_name(t.pitch);
            if (e.action == EventType::Press) e.hold = std::chrono::nanoseconds(t.hold);
        }
        note_buffer.push_back(&e);
    }
}

std::vector<qm_score_event> VirtualPianoPlayer::score() const {
    std::vector<qm_score_event> score;
    score.reserve(note_events.size());
    for (const auto& e : note_events) {
        const int pedal = PedalForName(e.note_or_control);
        score.push_back({ e.time.count(), pedal >= 0 ? -1 : const_cast<VirtualPianoPlayer*>(this)->note_name_to_midi(e.note_or_control),
                          pedal >= 0 ? (e.velocity & 0xFFF) | pedal << 12 : e.velocity, e.trackIndex, -1,
                          static_cast<uint8_t>(e.action == EventType::Press), 0 });
    }
    std::stable_sort(score.begin(), score.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    PairScore(score);
    return score;
}

void VirtualPianoPlayer::set_performer(const qm_performer_api* api) {
    if (performer) { performer_api.destroy(performer); performer = nullptr; }
    performer_api = api ? *api : qm_performer_api{};
    if (performer_api.create && performer_api.destroy && performer_api.build && performer_api.reset &&
        performer_api.step && performer_api.lift) performer = performer_api.create();
    take_stale.store(true, std::memory_order_release);
}

void VirtualPianoPlayer::set_performer_settings(std::string settings) {
    { std::lock_guard lock(take_mutex); performer_settings = std::move(settings); }
    take_stale.store(true, std::memory_order_release);
}

static std::mutex s_timerMutex;
static int s_timerHolds = 0;
static UINT s_timerPeriod = 0;

void VirtualPianoPlayer::hold_timer_resolution(bool hold) noexcept {
    std::lock_guard lock(s_timerMutex);
    if (hold) {
        if (s_timerHolds++ > 0) return;
        TIMECAPS tc;
        if (timeGetDevCaps(&tc, sizeof(tc)) != TIMERR_NOERROR) return;
        const UINT period = std::max(1U, tc.wPeriodMin);
        if (timeBeginPeriod(period) == TIMERR_NOERROR) s_timerPeriod = period;
        return;
    }
    if (s_timerHolds == 0 || --s_timerHolds > 0 || !s_timerPeriod) return;
    timeEndPeriod(s_timerPeriod);
    s_timerPeriod = 0;
}

int VirtualPianoPlayer::timer_resolution_holds() noexcept {
    std::lock_guard lock(s_timerMutex);
    return s_timerHolds;
}

// An error on this thread, an add-on's build throwing among others, used to
// end the whole process with the song's keys held in the game. It now ends
// the song: the keys come up, the song reads as done, and the log says why.
void VirtualPianoPlayer::play_notes() {
    crash::GuardThread();
    song_failed.store(false, std::memory_order_release);
    // The finest tick for the waits below, while this thread plays.
    struct TimerHold {
        TimerHold() noexcept { hold_timer_resolution(true); }
        ~TimerHold() { hold_timer_resolution(false); }
    } timerHold;
    try { play_song(); return; }
    catch (const std::exception& error) { std::cerr << "Playback stopped: " << error.what() << "\n"; }
    catch (...) { std::cerr << "Playback stopped on an error it could not name.\n"; }
    try {
        end_hold();
        release_all_keys();
    }
    catch (...) {
        // Even the record of what is held failed: every key comes up.
        crash::ReleaseKeys();
    }
    song_failed.store(true, std::memory_order_release);
    song_done.store(true, std::memory_order_release);
}

void VirtualPianoPlayer::play_song() {
    // Every starter awaits the clock before creating this thread; this covers
    // one that did not.
    try { AwaitClock(); }
    catch (const std::exception&) { should_stop.store(true, std::memory_order_release); return; }
    // One seed per playthrough: reseed only when starting from the top, so
    // pauses, seeks and take rebuilds reproduce the same take.
    if (const uint64_t forced = take_seed_override.load(std::memory_order_relaxed)) take_seed = forced;
    else if (!take_seed || total_adjusted_time <= std::chrono::nanoseconds::zero())
        take_seed = static_cast<uint64_t>(__rdtsc()) | 1ull;
    current_speed = std::clamp(requested_speed.load(std::memory_order_acquire), .05, 8.0);
    take_stale.store(false, std::memory_order_release);
    tap_moved = false;
    prepare_event_queue();
    song_done.store(false, std::memory_order_release);
    // The game may have been given another level since the last note: the
    // first note of every start taps its own.
    sent_velocity = 0;
    // Resume by time, since buffer indices change when the take is rebuilt.
    buffer_index.store(find_next_event_index(total_adjusted_time), std::memory_order_release);
    { std::lock_guard lock(tap_mutex); tap_queue.clear(); }
    if (performer) performer_api.reset(performer, take_seed);
    set_crash_key_ups();
    // Enable MMCSS for low-latency pro audio, reverted however the song ends.
    struct Mmcss {
        DWORD taskIndex = 0;
        HANDLE handle = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
        Mmcss() { if (handle) AvSetMmThreadPriority(handle, AVRT_PRIORITY_CRITICAL); }
        ~Mmcss() { if (handle) AvRevertMmThreadCharacteristics(handle); }
    } mmcss;

    if (!playback_started.load(std::memory_order_acquire)) {
        playback_started.store(true, std::memory_order_release);
        uint64_t now_tsc = __rdtsc();
        playback_start_time = now_tsc;
        last_resume_tsc     = now_tsc;
    }

    size_t buffer_size   = note_buffer.size();
    size_t current_index = buffer_index.load(std::memory_order_acquire);

    if (midi::Config::getInstance().auto_transpose.ENABLED &&
        [this]() {
            int suggestedTransposition = toggle_transpose_adjustment();
            int diff = suggestedTransposition - currentTransposition;
            if (diff) {
                std::cout << "Suggested Transposition: " << suggestedTransposition
                          << "\n[Transpose] Adjusting by " << diff << " steps.\n";
                [&]() -> bool {
                    auto& cfg = midi::Config::getInstance().auto_transpose;
                    const WORD transposeVK = stringToVK(diff > 0 ? cfg.TRANSPOSE_UP_KEY
                                                                 : cfg.TRANSPOSE_DOWN_KEY);
                    WORD scanCode = MapVirtualKey(transposeVK, MAPVK_VK_TO_VSC);
                    int steps = std::abs(diff);
                    // Sleep before each press; the game drops the first one otherwise.
                    for (int i = 0; i < steps; ++i) {
                        Sleep(50);
                        arrowsend(scanCode, isExtendedVK(transposeVK));
                    }
                    currentTransposition = suggestedTransposition;
                    return true;
                }();
                return true;
            }
            return false;
        }());

    // A start past the pedal going down holds it from the first note.
    restore_pedal(current_index);

    // The song clock starts here, after the take is built and the thread has
    // its MMCSS priority. Counted from before that setup, every note due during
    // it would go out together in the first batch.
    if (!paused.load(std::memory_order_acquire) && !clock_frozen.load(std::memory_order_relaxed))
        last_resume_tsc = __rdtsc();

    while (!should_stop.load(std::memory_order_acquire)) {
        // Apply a speed change from the current position.
        const double wanted = std::clamp(requested_speed.load(std::memory_order_acquire), .05, 8.0);
        if (wanted != current_speed) {
            total_adjusted_time = get_adjusted_time();
            last_resume_tsc = __rdtsc();
            current_speed = wanted;
            // Take offsets are in wall-clock time, so a speed change rebuilds the take.
            if (performer_on.load(std::memory_order_relaxed)) take_stale.store(true, std::memory_order_release);
        }
        if (take_stale.exchange(false, std::memory_order_acquire)) {
            // The clock is held while the take is built, as for an action, so
            // the notes that come due meanwhile play once it is built rather
            // than being passed over by a clock that ran on.
            const bool held = clock_frozen.load(std::memory_order_relaxed);
            if (!held) { total_adjusted_time = get_adjusted_time(); clock_frozen.store(true, std::memory_order_release); }
            prepare_event_queue();
            buffer_size = note_buffer.size();
            song_done.store(false, std::memory_order_release);
            // In Tap the clock stands on the last note tapped, which is behind the cursor.
            current_index = find_next_event_index(get_adjusted_time() + std::chrono::nanoseconds(tap_moved ? 1 : 0));
            buffer_index.store(current_index, std::memory_order_release);
            // Release keys the old take holds that the new take has already
            // released (or never pressed) before the current index; their
            // release events are behind us and would never run.
            std::unordered_map<std::string_view, int> due;
            const bool keystrokes = output_target.load(std::memory_order_acquire) != OutputTarget::MidiDevice;
            const auto count = [&](std::string_view note, bool press) {
                if (press) ++due[note];
                else if (auto found = due.find(note); found != due.end() && found->second > 0) --found->second;
            };
            for (size_t i = 0; i < current_index; ++i) {
                const auto* e = note_buffer[i];
                if (e->isSustain || (e->action == EventType::Press && e->skip)) continue;
                count(e->note, e->action == EventType::Press);
                // A held note's octave doubles are held with it.
                if (keystrokes)
                    for (const int8_t doubled : octave_doubles[std::clamp(note_name_to_midi(e->note), 0, 127)]) {
                        if (doubled < 0) break;
                        count(stable_note_name(doubled), e->action == EventType::Press);
                    }
            }
            std::lock_guard lock(dispatch_mutex);
            for (auto it = track_note_owners.begin(); it != track_note_owners.end();) {
                const auto found = due.find(it->first);
                if (found != due.end() && found->second > 0) { ++it; continue; }
                if (output_target.load(std::memory_order_acquire) == OutputTarget::MidiDevice) {
                    const int number = MidiNumberForNoteName(it->first.c_str());
                    const uint8_t message[3] = { 0x80, static_cast<uint8_t>(number & 0x7F), 0 };
                    if (number >= 0) send_midi_output(message, 3);
                } else release_key(it->first);
                it = track_note_owners.erase(it);
            }
            if (!held) { last_resume_tsc = __rdtsc(); clock_frozen.store(false, std::memory_order_release); }
        }
        if (action_pending.exchange(false, std::memory_order_acquire)) {
            play_actions(current_index);
            buffer_size = note_buffer.size();
        }
        // Entering Tap freezes the clock at the current position; leaving resumes from it.
        const bool tapping = performer && trigger.load(std::memory_order_acquire) == Trigger::Tap;
        if (tapping != clock_frozen.load(std::memory_order_relaxed)) {
            tap_moved = false;
            if (tapping) total_adjusted_time = get_adjusted_time();
            else last_resume_tsc = __rdtsc();
            clock_frozen.store(tapping, std::memory_order_release);
            if (!tapping && performer) { const auto host = performer_host(); performer_api.lift(performer, &host); }
        }
        if (tapping && !paused.load(std::memory_order_acquire)) {
            tap_step(current_index, buffer_size);
            continue;
        }

        auto current_time = get_adjusted_time();

        if (WaitForSingleObject(command_event, 0) == WAIT_OBJECT_0) {
            auto new_state = playback_control.processCommand(
                { current_time, current_index, false },
                current_speed,
                buffer_size
            );
            if (new_state.needs_reset) {
                drop_scheduled();
                current_index = find_next_event_index(new_state.position);
                song_done.store(false, std::memory_order_release);
                buffer_index.store(current_index, std::memory_order_release);
                total_adjusted_time = new_state.position;
                last_resume_tsc     = __rdtsc();
                restore_pedal(current_index);
            }
            ResetEvent(command_event);
        }

        // If paused, wait until resumed or commanded
        if (paused.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(playback_cv_mutex);
            playback_cv.wait_for(lock,
                                 std::chrono::milliseconds(5),
                                 [this]() {
                return !paused.load() ||
                        should_stop.load() ||
                       (WaitForSingleObject(command_event, 0) == WAIT_OBJECT_0);
            });
            continue;
        }
        // Held while the game is behind: look again every 5 ms.
        bool quiet = false;
        if (hold_while_behind(quiet)) {
            std::unique_lock<std::mutex> lock(playback_cv_mutex);
            playback_cv.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                return paused.load() || should_stop.load() || (WaitForSingleObject(command_event, 0) == WAIT_OBJECT_0);
            });
            continue;
        }
        // A loop wraps at its section's end, or just after the take's last event.
        std::optional<std::chrono::nanoseconds> wrapAt;
        if (const auto end = loop_end(); end && buffer_size)
            wrapAt = *end == std::chrono::nanoseconds::max() ? note_buffer.back()->time + std::chrono::nanoseconds(1) : *end;
        // At the end, wait on command_event: the wait above returns at once
        // while not paused, and would spin this critical-priority thread until
        // the worker stops it. Stop, seek and skip all set the event.
        if (current_index >= buffer_size && !wrapAt) {
            song_done.store(true, std::memory_order_release);
            WaitForSingleObject(command_event, 5);
            continue;
        }

        // Next due: a note, or the loop's end when nothing before it is left.
        auto next_event_time = current_index < buffer_size ? note_buffer[current_index]->time : *wrapAt;
        const bool wrapNext = wrapAt && next_event_time >= *wrapAt;
        if (wrapNext) next_event_time = *wrapAt;
        current_time = get_adjusted_time();

        if (next_event_time > current_time) {
            // Score time to wall-clock time.
            auto wait_duration = std::chrono::nanoseconds(
                static_cast<int64_t>((next_event_time - current_time).count() / current_speed));
            // condition_variable::wait_for rounds up to the scheduler tick
            // (up to 15.6 ms). With a high-resolution timer, sleep to within
            // `spin` of the note and busy-wait the rest. Sleeps are capped at
            // 5 ms slices because stop and pause notify the condition variable,
            // not this timer; the loop rechecks them each slice.
            constexpr auto spin = std::chrono::microseconds(300);
            constexpr auto slice = std::chrono::milliseconds(5);
            if (waitable_timer_precise) {
                if (wait_duration > spin) {
                    const auto sleep = std::min<std::chrono::nanoseconds>(wait_duration - spin, slice);
                    LARGE_INTEGER due;
                    due.QuadPart = -std::max<LONGLONG>(1, sleep.count() / 100);
                    const HANDLE handles[2] = { command_event, waitable_timer };
                    if (SetWaitableTimer(waitable_timer, &due, 0, nullptr, nullptr, FALSE) &&
                        WaitForMultipleObjects(2, handles, FALSE, 20) != WAIT_FAILED)
                        continue;
                } else {
                    while (get_adjusted_time() < next_event_time &&
                           !should_stop.load(std::memory_order_relaxed) && !paused.load(std::memory_order_relaxed))
                        _mm_pause();
                    continue;
                }
            }
            std::unique_lock<std::mutex> lock(playback_cv_mutex);
            playback_cv.wait_for(lock,
                                 wait_duration,
                                 [this]() {
                return paused.load() ||
                       (WaitForSingleObject(command_event, 0) == WAIT_OBJECT_0) ||
                        should_stop.load();
            });
            continue;
        }

        if (wrapNext) {
            wrap_loop(current_index, get_adjusted_time() - *wrapAt);
            continue;
        }

        // Process all events that are due
        current_time = get_adjusted_time();
        // More than a quarter second behind, after the thread or the machine
        // stalled, the clock moves up to the next event instead: what was
        // passed plays from now at its own spacing, not all in one cluster.
        if (current_index < buffer_size) {
            constexpr auto backlog = std::chrono::milliseconds(250);   // in wall-clock time
            const auto next = note_buffer[current_index]->time;
            if (static_cast<double>((current_time - next).count()) > static_cast<double>(std::chrono::nanoseconds(backlog).count()) * current_speed) {
                total_adjusted_time = next;
                last_resume_tsc = __rdtsc();
                current_time = next;
            }
        }
        std::vector<NoteEvent*> batch;
        while (current_index < buffer_size &&
               note_buffer[current_index]->time <= current_time &&
               !(wrapAt && note_buffer[current_index]->time >= *wrapAt))
        {
            batch.push_back(note_buffer[current_index]);
            ++current_index;
        }
        buffer_index.store(current_index, std::memory_order_release);

        // With this app's own window in front, the song plays on unheard.
        if (!batch.empty()) play_batch(batch, quiet);
    }

    end_hold();
}

// Injects on the MMCSS-boosted playback thread; handing batches to a pool
// thread would add context switches and run SendInput at normal priority.
void VirtualPianoPlayer::play_batch(const std::vector<NoteEvent*>& batch, bool quiet) {
    // Releases run before presses so a key released and restruck in one
    // batch sounds twice. A release whose press is in the same batch (a
    // zero-length or very short note) runs after the presses, or the key
    // would stay held.
    const auto closesOwnPress = [&batch](const NoteEvent* release) {
        for (const auto* e : batch) {
            if (e == release) return false;
            if (e->action == EventType::Press && e->isSustain == release->isSustain &&
                e->trackIndex == release->trackIndex && e->note == release->note) return true;
        }
        return false;
    };
    // For a skipped note (dropped by the take, or in the hand the user plays)
    // only the press is withheld. Its release still runs and is a no-op;
    // dropping releases would leave keys held.
    const auto sounds = [quiet](const NoteEvent* e) { return !quiet && !(e->skip && !e->isSustain); };
    uint32_t notes = 0;
    bool noteOffs = false;
    for (const auto* e : batch) {
        if (e->isSustain) continue;
        if (e->action == EventType::Press) notes += sounds(e);
        else noteOffs = true;
    }
    // One measurement for the one call.
    input_latency::Trace trace(input_latency::Source::Autoplay,
        notes ? input_latency::Kind::NoteOn : noteOffs ? input_latency::Kind::NoteOff : input_latency::Kind::Sustain);
    trace.notes(notes);

    // The chord goes to the game in one SendInput call, which Windows puts in
    // the input stream whole: nothing typed meanwhile lands inside it, and it
    // costs one call, not one per note. Each key's own Ctrl or Shift stays
    // around it, as in a call of its own. Held throughout, so a transport
    // action's release cannot fall between the gathering and the send.
    std::lock_guard lock(dispatch_mutex);
    std::vector<INPUT> gathered;
    gathered.reserve(64);
    t_gathering = &gathered;
    for (auto* e : batch)
        if (e->action == EventType::Release && !closesOwnPress(e)) execute_note_locked(*e);
    chord_level = !quiet && send_chord_level(batch);
    for (auto* e : batch)
        if (e->action == EventType::Press && sounds(e)) execute_note_locked(*e);
    chord_level = false;
    for (auto* e : batch)
        if (e->action == EventType::Release && closesOwnPress(e)) execute_note_locked(*e);
    t_gathering = nullptr;
    send_keys(gathered.data(), gathered.size());
}

// The game has one velocity level, which a tap sets for every note after it.
// Tapped per note, a chord whose hands differ in loudness sent two levels, and
// its notes sounded at whichever came last. The level is the loudest press
// that will be heard: not skipped, on a track heard, on a key of the layout.
// A level whose key a sounding note holds gives way to the nearest free one,
// as a single note's does; the releases due with the chord have already run.
bool VirtualPianoPlayer::send_chord_level(const std::vector<NoteEvent*>& batch) noexcept {
    if (!enable_velocity_keypress.load(std::memory_order_relaxed) ||
        output_target.load(std::memory_order_acquire) == OutputTarget::MidiDevice) return false;
    try {
        const auto& mappings = eightyEightKeyModeActive ? full_key_mappings : limited_key_mappings;
        int loudest = 0;
        for (const auto* e : batch) {
            if (e->action != EventType::Press || e->isSustain || e->skip || e->velocity <= loudest ||
                !isTrackEnabled(e->trackIndex)) continue;
            const auto key = mappings.find(sounding_note(e->note));
            if (key != mappings.end() && !key->second.empty()) loudest = e->velocity;
        }
        if (!loudest) return false;
        const char wanted = getVelocityKey(loudest).back();
        if (wanted == sent_velocity) return true;
        const char free = nearest_free_velocity_key(wanted, [this](char key) { return velocity_key_is_held(key); });
        INPUT tap[VELOCITY_TAP_INPUTS];
        if (free && free != sent_velocity && build_velocity_tap(free, tap)) {
            sent_velocity = free;
            send_keys(tap, VELOCITY_TAP_INPUTS);
        }
        return true;
    }
    catch (...) {
        // Allocation failed: each press taps its own, as before.
        return false;
    }
}

void VirtualPianoPlayer::end_hold() noexcept {
    if (!held_behind.load(std::memory_order_relaxed)) return;
    last_resume_tsc = __rdtsc();
    // The user may have played the game while it was in front of the song.
    sent_velocity = 0;
    held_behind.store(false, std::memory_order_release);
}

bool VirtualPianoPlayer::hold_while_behind(bool& quiet) {
    quiet = false;
    const bool holding = held_behind.load(std::memory_order_relaxed);
    if (!hold_behind.load(std::memory_order_acquire) || output_target.load(std::memory_order_acquire) != OutputTarget::Keystrokes) {
        end_hold();
        return false;
    }
    const Foreground front = ForegroundProbe();
    // Mid-switch nothing is in front; stay as we are.
    bool behind = holding;
    // This app's own window holds the song once there is a target; before
    // one is known the song plays on there without key-downs.
    if (front.own) {
        behind = front.robloxRunning || hold_target.load(std::memory_order_acquire);
        quiet = !behind;
    }
    else if (front.window && front.robloxRunning) behind = !front.roblox;
    else if (front.window) {
        // The taskbar or the desktop, passed on the way to a window, is never the target.
        HWND target = nullptr;
        if (!front.shell) hold_target.compare_exchange_strong(target, front.window, std::memory_order_acq_rel);
        else target = hold_target.load(std::memory_order_acquire);
        behind = target && front.window != target;
    }
    if (behind) {
        hold_resume_at = {};
        if (holding) return true;
        total_adjusted_time = get_adjusted_time();
        held_behind.store(true, std::memory_order_release);
        release_all_keys();
        return true;
    }
    if (!holding) return false;
    // The game takes a moment to be ready for keys once it is in front again.
    const auto now = std::chrono::steady_clock::now();
    if (hold_resume_at == std::chrono::steady_clock::time_point{}) hold_resume_at = now + std::chrono::milliseconds(250);
    if (now < hold_resume_at) return true;
    hold_resume_at = {};
    end_hold();
    // The hold let the pedal go with the keys.
    restore_pedal(buffer_index.load(std::memory_order_acquire));
    return false;
}

void VirtualPianoPlayer::restore_pedal(size_t index) {
    std::lock_guard lock(dispatch_mutex);
    index = std::min(index, note_buffer.size());
    if (output_target.load(std::memory_order_acquire) == OutputTarget::MidiDevice) {
        // Each track's latest value, a muted track's only ever lowered, and
        // the highest across tracks, as send_pedal_to_midi keeps them.
        for (int pedal = 0; pedal < 3; ++pedal) {
            if (pedal == 0 && currentSustainMode == SustainMode::IG) continue;
            auto& tracks = pedal_tracks[pedal];
            tracks.clear();
            for (size_t i = 0; i < index; ++i) {
                const auto* e = note_buffer[i];
                if (!e->isSustain || PedalForName(e->note) != pedal) continue;
                const auto found = tracks.find(e->trackIndex);
                const int held = found != tracks.end() ? found->second.value : 0;
                tracks[e->trackIndex] = { e->time, isTrackEnabled(e->trackIndex) ? e->sustainValue : (std::min)(e->sustainValue, held) };
            }
            int highest = 0;
            for (const auto& [track, state] : tracks) highest = (std::max)(highest, state.value);
            if (highest == 0 || highest == pedal_sent[pedal]) continue;
            pedal_sent[pedal] = highest;
            const uint8_t message[3] = { 0xB0, kPedalControllers[pedal], static_cast<uint8_t>(highest) };
            send_midi_output(message, 3);
        }
        return;
    }
    // The game has the sustain key only. Its owners are the tracks holding
    // the pedal, as dispatch_note_locked keeps them: a muted track's press is
    // not heard, its release is.
    if (currentSustainMode == SustainMode::IG || isSustainPressed) return;
    const bool inverted = currentSustainMode == SustainMode::SPACE_UP;
    std::set<int> owners;
    for (size_t i = 0; i < index; ++i) {
        const auto* e = note_buffer[i];
        if (!e->isSustain || PedalForName(e->note) != 0) continue;
        const bool press = inverted ? e->action == EventType::Release : e->action == EventType::Press;
        if (!press) owners.erase(e->trackIndex);
        else if (isTrackEnabled(e->trackIndex)) owners.insert(e->trackIndex);
    }
    if (owners.empty()) return;
    sustain_owners = std::move(owners);
    pressKey(sustain_key_code);
    isSustainPressed = true;
}

size_t VirtualPianoPlayer::find_next_event_index(const std::chrono::nanoseconds& target_time) {
    auto it = std::lower_bound(note_buffer.begin(),
                               note_buffer.end(),
                               target_time,
                               [](const NoteEvent* e, const std::chrono::nanoseconds& t) {
                                   return e->time < t;
                               });
    return std::distance(note_buffer.begin(), it);
}

std::optional<std::chrono::nanoseconds> VirtualPianoPlayer::loop_end() const noexcept {
    switch (loop.load(std::memory_order_acquire)) {
    case Loop::Song: return std::chrono::nanoseconds::max();
    case Loop::Section: {
        const int64_t start = loop_start_ns.load(std::memory_order_acquire), end = loop_end_ns.load(std::memory_order_acquire);
        if (end > start) return std::chrono::nanoseconds(end);
        return std::nullopt;
    }
    default: return std::nullopt;
    }
}

void VirtualPianoPlayer::wrap_loop(size_t& current_index, std::chrono::nanoseconds late) {
    release_all_keys();
    const auto start = loop.load(std::memory_order_acquire) == Loop::Section
        ? std::chrono::nanoseconds(loop_start_ns.load(std::memory_order_acquire)) : std::chrono::nanoseconds::zero();
    drop_scheduled();
    // Notes from the start on are still played; the time carried over only
    // brings them due that much sooner, and a long stall carries none.
    current_index = find_next_event_index(start);
    total_adjusted_time = start + (late < std::chrono::milliseconds(20) ? std::max(late, std::chrono::nanoseconds::zero()) : std::chrono::nanoseconds::zero());
    last_resume_tsc = __rdtsc();
    buffer_index.store(current_index, std::memory_order_release);
    // A loop chosen after the song reached its end takes it back from there.
    song_done.store(false, std::memory_order_release);
    // A section that starts with the pedal down starts with it down again.
    restore_pedal(current_index);
    loop_wraps.fetch_add(1, std::memory_order_release);
}

void VirtualPianoPlayer::toggleSustainMode() {
    switch (currentSustainMode) {
    case SustainMode::IG:
        currentSustainMode = SustainMode::SPACE_DOWN;
        std::cout << "[SUSTAIN] DOWN cutoff=" << g_sustainCutoff << "\n";
        break;
    case SustainMode::SPACE_DOWN:
        currentSustainMode = SustainMode::SPACE_UP;
        std::cout << "[SUSTAIN] UP (inverted) cutoff=" << g_sustainCutoff << "\n";
        break;
    case SustainMode::SPACE_UP:
        currentSustainMode = SustainMode::IG;
        std::cout << "[SUSTAIN] IGNORE\n";
        if (isSustainPressed) {
            releaseKey(sustain_key_code);
            isSustainPressed = false;
        }
        break;
    }
}

// Releases only the keys pressed_keys records as down. Every transport action
// calls this, and 88 key-ups take about 9 ms. pressed_keys is authoritative: a
// press that was never sent leaves its flag false.
void VirtualPianoPlayer::release_all_keys() { release_keys(false); }

// Panic path for emergency_exit(): releases every mapping without trusting
// pressed_keys. It runs once before exit, so the cost does not matter.
void VirtualPianoPlayer::release_every_mapped_key() { release_keys(true); }

void VirtualPianoPlayer::panic() {
    // Called with playback stopped, so a performer action's run has ended and let
    // go of what it held; the actions queued behind it are dropped, and a song
    // held while the game was behind is held no longer.
    { std::lock_guard lock(tap_mutex); action_queue.clear(); }
    action_pending.store(false, std::memory_order_release);
    end_hold();
    set_crash_key_ups();
    std::lock_guard lock(dispatch_mutex);
    // The port first, while the channels written are still known.
    if (output_target.load(std::memory_order_acquire) == OutputTarget::MidiDevice) silence_midi_output(true);
    // The record is cleared here; the sweep below lets every key go, so Stop,
    // which sweeps too, sends each release once rather than twice.
    release_keys_locked(false);
    // A key pressed on the keystroke target before a switch to MIDI, or under
    // the other layout, is released here too. A mapping's release carries its
    // own Ctrl or Shift, so an 88-key note comes up as the note it was.
    std::set<std::string> keys;
    for (const auto* mappings : {&limited_key_mappings, &full_key_mappings})
        for (const auto& [note, key] : *mappings) if (!key.empty()) keys.insert(key);
    for (const auto& key : keys) KeyPress(key, false);
    if (sustain_key_code) releaseKey(sustain_key_code);
    isSustainPressed = false;
    releaseKey(VK_SHIFT);
    releaseKey(VK_CONTROL);
    releaseKey(VK_MENU);
}

void VirtualPianoPlayer::release_keys(bool everyMapping) {
    std::lock_guard lock(dispatch_mutex);
    release_keys_locked(everyMapping);
}

// The caller holds dispatch_mutex, so no note is dispatched while this runs.
void VirtualPianoPlayer::release_keys_locked(bool everyMapping) {
    // Stop, pause, seek, a loop's wrap and Panic all pass here; what the game
    // is set to after them is not ours to know.
    sent_velocity = 0;
    track_note_owners.clear();
    last_strike_time.clear();
    sustain_owners.clear();
    key_holders.clear();
    for (auto& tracks : pedal_tracks) tracks.clear();
    pedal_sent.fill(-1);
    // On the MIDI target, held notes are on the port, so stop, pause and seek
    // must silence it here too, not only a target switch.
    if (output_target.load(std::memory_order_acquire) == OutputTarget::MidiDevice) {
        silence_midi_output();
        isSustainPressed = false;
        for (auto& [note, state] : pressed_keys) state.store(false, std::memory_order_relaxed);
        return;
    }
    if (isSustainPressed) {
        releaseKey(sustain_key_code);
        isSustainPressed = false;
    }
    const auto& mappings = (eightyEightKeyModeActive ? full_key_mappings
                                                     : limited_key_mappings);
    if (everyMapping) {
        for (const auto& [note, key] : mappings) KeyPress(key, false);
        for (auto& [note, state] : pressed_keys) state.store(false, std::memory_order_relaxed);
    } else {
        // A key two notes hold comes up once.
        std::set<std::string_view> released;
        for (auto& [note, state] : pressed_keys) {
            if (!state.exchange(false, std::memory_order_relaxed)) continue;
            const auto mapping = mappings.find(note);
            if (mapping != mappings.end() && !mapping->second.empty() && released.insert(mapping->second).second)
                KeyPress(mapping->second, false);
        }
    }
    // Always release Alt and Ctrl: a stuck modifier changes every later
    // keystroke, including the user's own.
    releaseKey(VK_MENU);
    releaseKey(VK_CONTROL);
    // Shift only when it is the velocity modifier, the only case where it is
    // held across events. An interrupted tap would otherwise leave it down.
    if (velocity_modifier_scan.load(std::memory_order_acquire) == 0x2A) releaseKey(VK_SHIFT);
}

void VirtualPianoPlayer::reset_volume() {
    int diff  = midi::Config::getInstance().volume.INITIAL_VOLUME -
                current_volume.load(std::memory_order_relaxed);
    int steps = std::abs(diff) /
                midi::Config::getInstance().volume.VOLUME_STEP;
    WORD sc   = (diff > 0) ? volume_up_key_code : volume_down_key_code;
    for (int i = 0; i < steps; ++i) {
        arrowsend(sc, volume_key_extended(sc));
    }
    current_volume.store(midi::Config::getInstance().volume.INITIAL_VOLUME,
                         std::memory_order_relaxed);
}

void VirtualPianoPlayer::restart_song() {
    try {
        AwaitClock();
        if (isSustainPressed) {
            releaseKey(sustain_key_code);
            isSustainPressed = false;
        }
        should_stop.store(true, std::memory_order_release);
        signalPlayback();
        if (playback_thread && playback_thread->joinable()) {
            playback_thread->join();
        }
        playback_started.store(false, std::memory_order_release);
        paused.store(false, std::memory_order_release);

        constexpr auto initialBuffer = std::chrono::milliseconds(50);
        total_adjusted_time = -initialBuffer;
        current_speed       = 1.0;
        requested_speed.store(1.0, std::memory_order_release);
        buffer_index.store(0, std::memory_order_release);
        song_done.store(false, std::memory_order_release);
        short_send_logged.store(false, std::memory_order_release);
        release_all_keys();

        uint64_t now_tsc = __rdtsc();
        playback_start_time = now_tsc;
        last_resume_tsc     = now_tsc;

        should_stop.store(false, std::memory_order_release);
        playback_thread = std::make_unique<std::jthread>(
            &VirtualPianoPlayer::play_notes, this
        );
        std::cout << "[RESTART] Done.\n";
    }
    catch (const std::exception& e) {
        std::cerr << "[RESTART] Error: " << e.what() << "\n";
    }
}

// A velocity tap is four inputs in one SendInput call, the same as MIDI2Key
// sends: modifier down, key down, key up, modifier up. The game reads the
// modified keypress as the velocity; the key-ups keep the following note from
// being typed with the modifier held. The protocol is the game's and fixed.
// getVelocityKey() only returns characters from
// "1234567890qwertyuiopasdfghjklzxc", so no shift handling is needed.
std::atomic<WORD> VirtualPianoPlayer::velocity_modifier_scan{ 0x38 };

WORD VirtualPianoPlayer::VelocityModifierScan(const std::string& name) noexcept {
    if (name == "ctrl") return 0x1D;
    if (name == "shift") return 0x2A;
    return 0x38;   // alt, and the fallback for anything validate() would refuse
}

void VirtualPianoPlayer::apply_velocity_modifier() {
    const WORD scan = VelocityModifierScan(midi::Config::getInstance().playback.velocityModifier);
    // On a change, forget the level so the next note sends a tap with the
    // new modifier.
    if (velocity_modifier_scan.exchange(scan, std::memory_order_release) != scan) {
        sent_velocity = 0;
    }
}

std::vector<std::string> VirtualPianoPlayer::velocity_modifier_conflicts() const {
    const std::string modifier = midi::Config::getInstance().playback.velocityModifier;
    const std::string keys = "1234567890qwertyuiopasdfghjklzxc";
    const auto& mappings = eightyEightKeyModeActive.load(std::memory_order_relaxed)
        ? full_key_mappings : limited_key_mappings;

    std::vector<std::string> conflicts;
    for (char key : keys) {
        const std::string combination = modifier + "+" + key;
        for (const auto& [note, mapped] : mappings) {
            if (mapped != combination) continue;
            conflicts.push_back(combination);
            break;
        }
    }
    return conflicts;
}

size_t VirtualPianoPlayer::build_velocity_tap(char velocityKey, INPUT* out) noexcept {
    const WORD modifier = velocity_modifier_scan.load(std::memory_order_acquire);
    const WORD scan = SCAN_TABLE_AUTO[static_cast<unsigned char>(velocityKey)];
    if (scan == 0) return 0;
    for (size_t i = 0; i < VELOCITY_TAP_INPUTS; ++i) {
        out[i] = {};
        out[i].type = INPUT_KEYBOARD;
    }
    out[0].ki.wScan = modifier; out[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    out[1].ki.wScan = scan;     out[1].ki.dwFlags = KEYEVENTF_SCANCODE;
    out[2].ki.wScan = scan;     out[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    out[3].ki.wScan = modifier; out[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    return VELOCITY_TAP_INPUTS;
}

static const KeySequence& cachedSequence(const std::string& keyStr) {
    auto it = g_keyCache.find(keyStr);
    if (it == g_keyCache.end()) it = g_keyCache.emplace(keyStr, computeKeySequence(keyStr)).first;
    return it->second;
}

void VirtualPianoPlayer::KeyPress(std::string_view key, bool press) {
    const auto& seq = cachedSequence(std::string(key));
    const auto& events = press ? seq.events_press : seq.events_release;
    send_keys(events.data(), events.size());
}

UINT VirtualPianoPlayer::send_keys(const INPUT* inputs, size_t count) noexcept {
    if (!count) return 0;
    if (auto* gathering = t_gathering) {
        try {
            gathering->insert(gathering->end(), inputs, inputs + count);
            return static_cast<UINT>(count);
        }
        catch (...) {
            // Out of memory: send it on its own rather than lose it.
        }
    }
    const UINT sent = input_latency::send(static_cast<UINT>(count), inputs, sizeof(INPUT));
    // Keys held back while Roblox is behind count as sent; a short count is
    // Windows refusing input, such as a game run as administrator. Once a song,
    // so a song that loses every note does not fill the log.
    if (sent < count && !short_send_logged.exchange(true, std::memory_order_acq_rel)) {
        const DWORD error = GetLastError();
        std::cerr << "Windows sent " << sent << " of " << count << " key events to the game";
        if (error) std::cerr << " (error " << error << ")";
        std::cerr << "; notes of this song may not have played.\n";
    }
    return sent;
}

int VirtualPianoPlayer::stringToVK(std::string_view keyName) {
    static const std::unordered_map<std::string, int> keyMap = {
        {"ENTER", VK_RETURN}, {"ESC", VK_ESCAPE}, {"SPACE", VK_SPACE},
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"CAPS", VK_CAPITAL}, {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL},
        {"ALT", VK_MENU}, {"TAB", VK_TAB}, {"F1", VK_F1}, {"F2", VK_F2},
        {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6},
        {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10},
        {"F11", VK_F11}, {"F12", VK_F12}, {"PAUSE", VK_PAUSE},
        {"BACK", VK_BACK}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME},
        {"END", VK_END}, {"INSERT", VK_INSERT}, {"PGUP", VK_PRIOR},
        {"PGDN", VK_NEXT}, {"NUMLOCK", VK_NUMLOCK}, {"SCROLL", VK_SCROLL},
        {"PRTSC", VK_SNAPSHOT}, {"APPS", VK_APPS},
        {"VOLUME_MUTE", VK_VOLUME_MUTE}, {"VOLUME_DOWN", VK_VOLUME_DOWN},
        {"VOLUME_UP", VK_VOLUME_UP}, {"MEDIA_NEXT", VK_MEDIA_NEXT_TRACK},
        {"MEDIA_PREV", VK_MEDIA_PREV_TRACK}, {"MEDIA_PLAY", VK_MEDIA_PLAY_PAUSE}
    };

    std::string upper;
    upper.reserve(keyName.size());
    for (char c : keyName) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    // strip "VK_" prefix if present
    if (upper.rfind("VK_", 0) == 0) {
        upper.erase(0, 3);
    }
    else if (upper.rfind("VK", 0) == 0) {
        upper.erase(0, 2);
    }

    auto it = keyMap.find(upper);
    if (it != keyMap.end()) {
        return it->second;
    }

    // If single alphanumeric char, try VkKeyScanA
    if (upper.size() == 1) {
        char c = upper[0];
        if (std::isalnum(static_cast<unsigned char>(c))) {
            SHORT vk = VkKeyScanA(c);
            if (vk != -1)
                return LOBYTE(vk);
        }
    }

    // If that fails, try numeric parse
    try {
        int vkCode = std::stoi(upper);
        if (vkCode >= 0 && vkCode <= 255) {
            return vkCode;
        }
    }
    catch (...) {
        // ignore
    }
    throw std::runtime_error(
        "Cannot map key '" + std::string(keyName) + "' to a VK code"
    );
}

WORD VirtualPianoPlayer::vkToScanCode(int vk) {
    return static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
}

void VirtualPianoPlayer::sendVirtualKey(WORD vk, bool press) {
    INPUT in{};
    in.type    = INPUT_KEYBOARD;
    in.ki.wVk  = vk;
    in.ki.wScan= static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
    if (!press) {
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    send_keys(&in, 1);
}

void VirtualPianoPlayer::pressKey(WORD vk) {
    sendVirtualKey(vk, true);
}

void VirtualPianoPlayer::releaseKey(WORD vk) {
    sendVirtualKey(vk, false);
}

// The note actually played, after out-of-range folding. press_key and
// release_key must both go through this, or a folded note's release looks up
// the unfolded name in pressed_keys and the key stays down.
std::string VirtualPianoPlayer::sounding_note(std::string_view note) {
    return ENABLE_OUT_OF_RANGE_TRANSPOSE ? transpose_note(note) : std::string(note);
}

bool VirtualPianoPlayer::velocity_key_is_held(char velocityKey) noexcept {
    INPUT tap[VELOCITY_TAP_INPUTS];
    if (!build_velocity_tap(velocityKey, tap)) return false;
    const WORD scan = tap[1].ki.wScan;
    const auto& mappings = eightyEightKeyModeActive ? full_key_mappings : limited_key_mappings;
    for (const auto& [note, down] : pressed_keys) {
        if (!down.load(std::memory_order_relaxed)) continue;
        const auto mapped = mappings.find(note);
        if (mapped == mappings.end() || mapped->second.empty()) continue;
        // Shifted and ctrl mappings still press the letter's scan code.
        for (const auto& event : cachedSequence(mapped->second).events_press)
            if (event.ki.wScan == scan) return true;
    }
    return false;
}

bool VirtualPianoPlayer::press_key(std::string_view note, char velocityKey, bool doubled) noexcept {
    std::string actual = sounding_note(note);
    const std::string& key = (eightyEightKeyModeActive
                              ? full_key_mappings[actual]
                              : limited_key_mappings[actual]);
    if (key.empty()) return false;

    // Release a key that is already down before striking it, so the repeat
    // sounds, whichever note holds it. A doubled note only holds it too.
    const bool again = pressed_keys[actual].exchange(true, std::memory_order_relaxed);
    int& holders = key_holders[key];
    if (!again) ++holders;
    const bool down = again || holders > 1;
    if (doubled && down) return false;

    // Send the velocity tap and the note in one SendInput call. SendInput
    // inserts a batch without interleaving other input, so nothing can change
    // the velocity between the tap and the note.
    INPUT batch[32];
    size_t count = velocityKey ? build_velocity_tap(velocityKey, batch) : 0;

    const KeySequence& seq = cachedSequence(key);
    const size_t needed = count + (down ? seq.events_release.size() : 0) + seq.events_press.size();
    if (needed > std::size(batch)) {
        // Unreachable with any valid mapping. Truncating could leave a key
        // down, so fall back to separate calls.
        send_keys(batch, count);
        if (down) KeyPress(key, false);
        KeyPress(key, true);
        return true;
    }
    if (down)
        for (const auto& event : seq.events_release) batch[count++] = event;
    for (const auto& event : seq.events_press) batch[count++] = event;
    send_keys(batch, count);
    return true;
}

void VirtualPianoPlayer::release_key(std::string_view note) noexcept {
    const std::string actual = sounding_note(note);
    const std::string& key = (eightyEightKeyModeActive
                              ? full_key_mappings[actual]
                              : limited_key_mappings[actual]);
    if (!key.empty() &&
        pressed_keys[actual].exchange(false, std::memory_order_relaxed))
    {
        // A key two notes hold comes up when the last of them lets go.
        if (const auto held = key_holders.find(key); held != key_holders.end()) {
            if (--held->second > 0) return;
            key_holders.erase(held);
        }
        KeyPress(key, false);
    }
}

std::string VirtualPianoPlayer::transpose_note(std::string_view note) {
    return get_note_name(FoldOntoSixtyOneKeys(note_name_to_midi(note)));
}

int VirtualPianoPlayer::note_name_to_midi(std::string_view note_name) {
    static constexpr const char* NAMES[12] = {
        "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
    };
    if (note_name.size() < 2)
        return 60;
    int octave = note_name.back() - '0';
    std::string_view pitch = note_name.substr(0, note_name.size() - 1);
    // Octave -1 is spelled with a trailing '-' before the digit ("D-1").
    if (!pitch.empty() && pitch.back() == '-') {
        octave = -octave;
        pitch.remove_suffix(1);
    }
    int idx = 0;
    for (int i = 0; i < 12; ++i) {
        if (pitch == NAMES[i]) {
            idx = i;
            break;
        }
    }
    return (octave + 1) * 12 + idx;
}

std::string VirtualPianoPlayer::get_note_name(int midi_note) {
    static constexpr const char* NAMES[12] = {
        "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
    };
    int octave = (midi_note / 12) - 1;
    int pitch  = midi_note % 12;
    if (pitch < 0 || pitch > 11)
        return "Unknown";
    return std::string(NAMES[pitch]) + std::to_string(octave);
}

std::string VirtualPianoPlayer::getVelocityCurveName(midi::VelocityCurveType curveType) {
    using VT = midi::VelocityCurveType;
    switch (curveType) {
    case VT::LinearCoarse:       return "Linear Coarse";
    case VT::LinearFine:         return "Linear Fine";
    case VT::ImprovedLowVolume:  return "Improved Low Volume";
    case VT::Logarithmic:        return "Logarithmic";
    case VT::Exponential:        return "Exponential";
    case VT::SCurve:             return "S-Curve";
    default:                     return "Unknown";
    }
}

void VirtualPianoPlayer::setVelocityCurveIndex(size_t index) {
    auto& cfg = midi::Config::getInstance();
    size_t mx = midi::kBuiltinVelocityCurves + cfg.playback.customVelocityCurves.size();
    currentVelocityCurveIndex = std::min(index, mx);
}

std::string VirtualPianoPlayer::getVelocityKey(int targetVelocity) {
    // Each table holds input thresholds: entry i is the loudest input velocity
    // that maps to velocity key i. Every curve spans all 32 steps up to 127.
    //
    // S-Curve is the "radiant grand" curve, defined as output velocity per
    // step (an S from 25 to 127), inverted into thresholds: the response
    // passes through step i / 31 -> radiant[i] / 127. Steps 0 to 5 are
    // unreachable, so the softest input plays step 6. Derived in
    // tests/ShellTests.cpp, BuiltinCurveTests.
    static constexpr std::array<int, 32> builtinCurves[midi::kBuiltinVelocityCurves] = {
        {4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92,96,100,104,108,112,116,120,124,127},
        {2,6,10,14,18,22,26,30,34,38,42,46,50,54,58,62,66,70,74,78,82,86,90,94,98,102,106,110,114,118,122,127},
        {1,2,4,5,7,9,11,14,16,19,22,25,29,32,36,41,45,50,55,61,67,73,79,86,92,99,107,114,119,122,124,127},
        {1,2,3,4,5,6,7,8,9,10,12,14,17,20,23,27,30,35,39,44,49,55,61,67,74,81,89,96,105,113,120,127},
        {1,2,3,4,7,11,17,22,27,33,38,44,49,54,60,65,71,76,82,87,92,98,103,107,111,115,118,121,124,125,126,127},
        {0,0,0,0,0,0,6,17,24,28,32,36,39,42,46,49,52,56,59,62,66,69,73,78,82,86,90,94,99,104,112,127}
    };
    static constexpr char velocityKeys[] = "1234567890qwertyuiopasdfghjklzxc";
    const auto& config = midi::Config::getInstance();
    const auto& velocityTable = (currentVelocityCurveIndex < midi::kBuiltinVelocityCurves)
        ? builtinCurves[currentVelocityCurveIndex]
        : config.playback.customVelocityCurves[currentVelocityCurveIndex - midi::kBuiltinVelocityCurves].velocityValues;

    int idx = 0;
    while (idx < 32 && velocityTable[idx] < targetVelocity) {
        ++idx;
    }
    return std::string(1, velocityKeys[(idx < 32) ? idx : 31]);
}

void VirtualPianoPlayer::toggle_88_key_mode() {
    eightyEightKeyModeActive = !eightyEightKeyModeActive;
    std::cout << "[88-KEY MODE] "
              << (eightyEightKeyModeActive ? "Enabled" : "Disabled")
              << "\n";
}

void VirtualPianoPlayer::toggle_volume_adjustment() {
    bool newVal = !enable_volume_adjustment.load(std::memory_order_relaxed);
    enable_volume_adjustment.store(newVal, std::memory_order_relaxed);
    if (newVal) {
        max_volume = midi::Config::getInstance().volume.MAX_VOLUME;
        precompute_volume_adjustments();
        calibrate_volume();
        std::cout << "[AUTOVOL] On. Initial="
                  << midi::Config::getInstance().volume.INITIAL_VOLUME
                  << "% Step="
                  << midi::Config::getInstance().volume.VOLUME_STEP
                  << "% Max="
                  << midi::Config::getInstance().volume.MAX_VOLUME << "%\n";
    }
    else {
        std::cout << "[AUTOVOL] Off.\n";
    }
}

// ---------------------------------------------------------------------------
// The performer
//
// prepare_event_queue() writes the take into note_buffer ahead of time, so
// the dispatch loop plays it like any other song. Never apply take offsets by
// sleeping at dispatch: everything due during the sleep would be delayed.
// ---------------------------------------------------------------------------

void VirtualPianoPlayer::toggle_performer() {
    performer_on.store(!performer_on.load(std::memory_order_relaxed), std::memory_order_relaxed);
    take_stale.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Tap
//
// The clock is stopped and each tap advances the song. The performer decides
// what a tap plays; the player passes it the taps and sends what it fires.
// ---------------------------------------------------------------------------

void VirtualPianoPlayer::tap(int key, bool down) {
    { std::lock_guard lock(tap_mutex); tap_queue.push_back({ key, static_cast<uint8_t>(down) }); }
    signalPlayback();
}

qm_player VirtualPianoPlayer::performer_host() {
    AwaitClock();   // now_ns reads cyclesToNs
    qm_player host{};
    host.player = this;
    host.fire = [](void* player, size_t index, int32_t pitch) {
        auto& self = *static_cast<VirtualPianoPlayer*>(player);
        if (index >= self.note_buffer.size()) return;
        NoteEvent struck = *self.note_buffer[index];
        if (!struck.isSustain && pitch >= 0 && pitch != self.note_name_to_midi(struck.note)) struck.note = stable_note_name(pitch);
        self.execute_note_event(struck);
    };
    host.release = [](void* player, int32_t pitch, int32_t track) {
        const NoteEvent release(std::chrono::nanoseconds::zero(), stable_note_name(pitch), EventType::Release, 0, false, 0, track);
        static_cast<VirtualPianoPlayer*>(player)->execute_note_event(release);
    };
    host.now_ns = [](void*) { return static_cast<uint64_t>(static_cast<double>(__rdtsc()) * cyclesToNs); };
    host.stopping = [](void* player) { return static_cast<int>(static_cast<VirtualPianoPlayer*>(player)->should_stop.load(std::memory_order_relaxed)); };
    return host;
}

void VirtualPianoPlayer::tap_step(size_t& current_index, size_t buffer_size) {
    std::vector<qm_tap> taps;
    { std::lock_guard lock(tap_mutex); taps.swap(tap_queue); }
    const auto host = performer_host();
    int64_t position = total_adjusted_time.count();
    const size_t cursor = current_index;
    int standing = performer_api.step(performer, &host, taps.data(), taps.size(), &current_index, &position, current_speed,
                                      tap_holds_notes.load(std::memory_order_acquire));
    if (current_index != cursor) tap_moved = true;
    // Looping, the tap after the loop's last chord plays its first. Keys a tap
    // holds are left to the tap, which lets them go when its key comes up.
    if (const auto end = loop_end(); end && buffer_size &&
        (current_index >= buffer_size || note_buffer[current_index]->time >= *end)) {
        const auto start = loop.load(std::memory_order_acquire) == Loop::Section
            ? std::chrono::nanoseconds(loop_start_ns.load(std::memory_order_acquire)) : std::chrono::nanoseconds::zero();
        current_index = find_next_event_index(start);
        position = start.count();
        tap_moved = false;
        standing = QM_STEP_PLAYING;
        loop_wraps.fetch_add(1, std::memory_order_release);
    }
    total_adjusted_time = std::chrono::nanoseconds(position);
    // The song ends once its last note has been tapped and released.
    const size_t reached = standing == QM_STEP_PLAYING ? current_index : standing == QM_STEP_HOLDING ? (buffer_size ? buffer_size - 1 : 0) : buffer_size;
    buffer_index.store(reached, std::memory_order_release);
    song_done.store(reached >= buffer_size, std::memory_order_release);
    WaitForSingleObject(command_event, 1);
    ResetEvent(command_event);
}

// ---------------------------------------------------------------------------
// Performer actions
//
// A key the performer declared plays what the performer returns, between two
// events of the song, with the song's clock held: the song stands where it is,
// its held keys stay held, and it goes on from there when the action is done.
// One that schedules instead puts its keys in the take ahead of the clock,
// which runs on, and they play as the take's own do.
// ---------------------------------------------------------------------------

void VirtualPianoPlayer::perform_action(std::string id, bool schedules) {
    { std::lock_guard lock(tap_mutex); action_queue.emplace_back(std::move(id), schedules); }
    action_pending.store(true, std::memory_order_release);
}

void VirtualPianoPlayer::play_actions(size_t current_index) {
    std::vector<std::pair<std::string, bool>> queued;
    { std::lock_guard lock(tap_mutex); queued.swap(action_queue); }
    if (!performer || !performer_action || paused.load(std::memory_order_acquire)) return;
    AwaitClock();   // cyclesToNs
    const bool frozen = clock_frozen.load(std::memory_order_relaxed);
    // In Tap there is no time ahead to schedule into.
    std::vector<std::string> ids;
    for (auto& [id, schedules] : queued) {
        if (!schedules) ids.push_back(std::move(id));
        else if (!frozen) schedule_action(id, current_index);
    }
    if (ids.empty()) return;
    // The clock is held as Tap holds it, so a pause, stop or seek during the
    // run reads the position the run began at.
    if (!frozen) { total_adjusted_time = get_adjusted_time(); clock_frozen.store(true, std::memory_order_release); }
    for (const auto& id : ids) {
        const auto position = total_adjusted_time;
        const qm_take_event* events = nullptr;
        const size_t count = performer_action(performer, id.c_str(), position.count(), current_speed, &events);
        std::vector<qm_take_event> run(events, events + (events ? count : 0));
        std::stable_sort(run.begin(), run.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        const double start = static_cast<double>(__rdtsc()) * cyclesToNs;
        const auto elapsed = [&] { return static_cast<double>(__rdtsc()) * cyclesToNs - start; };
        const auto interrupted = [this] { return should_stop.load(std::memory_order_relaxed) || paused.load(std::memory_order_relaxed); };
        std::vector<std::pair<int32_t, int32_t>> down;   // pitch and track of what the action holds
        bool stopped = false;
        for (const auto& e : run) {
            if (e.pitch < 0 || e.pitch > 127) continue;
            while (!(stopped = interrupted()) && elapsed() < static_cast<double>(e.time)) {
                if (static_cast<double>(e.time) - elapsed() > 2e6) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                else _mm_pause();
            }
            if (stopped) break;
            // A time past the song's own strikes, so none is taken for one of them.
            const NoteEvent struck(position + std::chrono::nanoseconds(std::max<int64_t>(0, e.time) + 1), stable_note_name(e.pitch),
                                   e.press ? EventType::Press : EventType::Release, e.velocity, false, 0, e.track);
            execute_note_event(struck);
            if (e.press) down.emplace_back(e.pitch, e.track);
            else if (const auto held = std::find(down.begin(), down.end(), std::pair{e.pitch, e.track}); held != down.end()) down.erase(held);
        }
        for (const auto& [pitch, track] : down)
            execute_note_event(NoteEvent(position, stable_note_name(pitch), EventType::Release, 0, false, 0, track));
        if (stopped) break;
    }
    if (!frozen) { last_resume_tsc = __rdtsc(); clock_frozen.store(false, std::memory_order_release); }
}

// The action is told where the clock stands and returns keys at score times
// from there on. They go into note_buffer after the next event due, in time
// order, so they reach the keys as the take's own do, through Transpose, fold,
// Octaves and the track's mute. A run that begins behind the clock is not
// played. While what it scheduled is still ahead, its key asks for nothing.
void VirtualPianoPlayer::schedule_action(const std::string& id, size_t current_index) {
    const auto position = get_adjusted_time();
    if (const auto armed = scheduled_until.find(id); armed != scheduled_until.end() && armed->second >= position) return;
    const qm_take_event* events = nullptr;
    const size_t count = performer_action(performer, id.c_str(), position.count(), current_speed, &events);
    if (!events || !count) return;
    std::vector<qm_take_event> run(events, events + count);
    std::stable_sort(run.begin(), run.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    if (run.front().time < position.count()) return;
    std::lock_guard lock(buffer_mutex);
    auto until = position;
    for (const auto& e : run) {
        if (e.pitch < 0 || e.pitch > 127) continue;
        auto* added = event_pool.allocate(std::chrono::nanoseconds(e.time), stable_note_name(e.pitch),
                                          e.press ? EventType::Press : EventType::Release, e.velocity, false, 0, e.track);
        added->hold = std::chrono::nanoseconds(e.press ? e.hold : -1);
        const auto at = std::upper_bound(note_buffer.begin() + std::min(current_index, note_buffer.size()), note_buffer.end(), added->time,
                                         [](std::chrono::nanoseconds time, const NoteEvent* other) { return time < other->time; });
        note_buffer.insert(at, added);
        scheduled.push_back(added);
        until = std::max(until, added->time);
    }
    scheduled_until[id] = until;
}

// A seek or a loop's wrap: its keys already struck were let go with the song's.
void VirtualPianoPlayer::drop_scheduled() {
    scheduled_until.clear();
    if (scheduled.empty()) return;
    std::lock_guard lock(buffer_mutex);
    std::erase_if(note_buffer, [this](const NoteEvent* e) { return std::find(scheduled.begin(), scheduled.end(), e) != scheduled.end(); });
    scheduled.clear();
}
void VirtualPianoPlayer::toggle_velocity_keypress() {
    bool newVal = !enable_velocity_keypress.load(std::memory_order_relaxed);
    enable_velocity_keypress.store(newVal, std::memory_order_relaxed);
    std::cout << "[VELOCITY KEY] "
              << (newVal ? "Enabled" : "Disabled") << "\n";
    if (newVal) {
        std::cout << "[WARNING] ALT+key combos in use; ensure no conflicting overlays.\n";
    }
}

void VirtualPianoPlayer::precompute_volume_adjustments() {
    int currMaxVol = max_volume.load(std::memory_order_relaxed);
    for (int v = 0; v < 128; ++v) {
        double ratio = static_cast<double>(v) / 127.0;
        int tv       = static_cast<int>(ratio * currMaxVol);
        int mn       = std::min(midi::Config::getInstance().volume.MIN_VOLUME,
                                currMaxVol);
        int mx       = std::max(midi::Config::getInstance().volume.MIN_VOLUME,
                                currMaxVol);
        tv = std::clamp(tv, mn, mx);
        // Round to nearest 10
        tv = ((tv + 5) / 10) * 10;
        volume_lookup[v] = tv;
    }
}

void VirtualPianoPlayer::calibrate_volume() {
    auto& vc = midi::Config::getInstance().volume;

    // Drive the volume to its minimum first.
    for (int i = 0; i < 50; ++i) {
        arrowsend(volume_down_key_code, volume_key_extended(volume_down_key_code));
        for (volatile int j = 0; j < 8000; ++j) {} // small busy loop
    }
    double cf = (vc.INITIAL_VOLUME > 150) ? 1.1 : 1.0;
    int steps = static_cast<int>(
                    std::ceil(((vc.INITIAL_VOLUME - vc.MIN_VOLUME) /
                                vc.VOLUME_STEP) * cf));

    for (int i = 0; i < steps; ++i) {
        arrowsend(volume_up_key_code, volume_key_extended(volume_up_key_code));
        for (volatile int j = 0; j < (8000 + (i > 10 ? (i - 10) * 300 : 0)); ++j) {}
    }

    current_volume.store(vc.INITIAL_VOLUME, std::memory_order_relaxed);
}

void VirtualPianoPlayer::AdjustVolumeBasedOnVelocity(int velocity) noexcept {
    if (velocity < 0 || velocity >= static_cast<int>(volume_lookup.size()))
        return;

    int target_vol = volume_lookup[velocity];
    int current_vol= current_volume.load(std::memory_order_relaxed);
    int diff       = target_vol - current_vol;
    int step_size  = midi::Config::getInstance().volume.VOLUME_STEP;
    if (std::abs(diff) >= step_size) {
        WORD sc = (diff > 0) ? volume_up_key_code : volume_down_key_code;
        int steps = std::abs(diff) / step_size;
        for (int i = 0; i < steps; ++i) {
            arrowsend(sc, volume_key_extended(sc));
        }
        current_volume.store(target_vol, std::memory_order_relaxed);
    }
}

void VirtualPianoPlayer::toggle_out_of_range_transpose() {
    ENABLE_OUT_OF_RANGE_TRANSPOSE = !ENABLE_OUT_OF_RANGE_TRANSPOSE;
    std::cout << "[TRANSPOSE] "
              << (ENABLE_OUT_OF_RANGE_TRANSPOSE ? "Enabled" : "Disabled")
              << "\n";
}

// ---------------------------------------------------------------------------
// MIDI output
//
// One target for the whole app. A key must be released on the target it was
// pressed on, so switching targets releases everything first.
// ---------------------------------------------------------------------------

void VirtualPianoPlayer::set_live_release_hook(std::function<void()> hook) {
    const bool live = static_cast<bool>(hook);
    {
        std::lock_guard lock(output_mutex);
        live_release_hook = std::move(hook);
    }
    // MIDI2Key registers its hook when it is made and clears it when it goes,
    // so live input is open while a hook is set, and holds the timer tick.
    if (live != live_timer_held) {
        hold_timer_resolution(live);
        live_timer_held = live;
    }
}

bool VirtualPianoPlayer::open_midi_output(const std::wstring& deviceId) {
    auto port = CreateMidiOutput(BackendForOutputId(deviceId));
    // Open outside output_mutex: opening talks to the driver and the note path
    // must not wait on it.
    if (!port || !port->open(deviceId)) {
        midi_output_refused_busy.store(port && port->busy(), std::memory_order_relaxed);
        return false;
    }
    std::lock_guard lock(output_mutex);
    midi_output = std::move(port);
    midi_output_channels.store(0, std::memory_order_relaxed);
    midi_output_held = {};
    return true;
}

void VirtualPianoPlayer::close_midi_output() {
    // Closing counts as switching away, which silences held notes first.
    set_output_target(OutputTarget::Keystrokes);
    std::unique_ptr<IMidiOutput> port;
    {
        std::lock_guard lock(output_mutex);
        port = std::move(midi_output);
    }
    // Destroy outside the lock, since closing also talks to the driver.
    port.reset();
}

void VirtualPianoPlayer::drop_midi_output() {
    // What the port still sounds is let go first, in case it is still there.
    silence_midi_output();
    std::unique_ptr<IMidiOutput> port;
    {
        std::lock_guard lock(output_mutex);
        port = std::move(midi_output);
    }
    port.reset();
}

std::wstring VirtualPianoPlayer::opened_midi_output() const {
    std::lock_guard lock(output_mutex);
    return midi_output ? midi_output->openedDeviceId() : std::wstring();
}

void VirtualPianoPlayer::send_midi_output(const uint8_t* message, size_t length) noexcept {
    if (output_target.load(std::memory_order_acquire) != OutputTarget::MidiDevice) return;
    if (!message || length == 0) return;
    try {
        std::lock_guard lock(output_mutex);
        if (!midi_output) return;
        if (length >= 1 && (message[0] & 0xF0) != 0xF0)
            midi_output_channels.fetch_or(static_cast<uint16_t>(1u << (message[0] & 0x0F)),
                                          std::memory_order_relaxed);
        if (length >= 3 && message[1] < 128) {
            auto& held = midi_output_held[message[0] & 0x0F][message[1]];
            const uint8_t kind = message[0] & 0xF0;
            if (kind == 0x90 && message[2] > 0) { if (held < 255) ++held; }
            else if ((kind == 0x90 || kind == 0x80) && held > 0) --held;
        }
        midi_output->send(message, length);
    }
    catch (...) {
        // A port error must not escape onto the playback thread.
    }
}

void VirtualPianoPlayer::silence_midi_output(bool everyNote) noexcept {
    try {
        std::lock_guard lock(output_mutex);
        if (!midi_output) return;
        // Every channel written since the port opened, since the held-note
        // bookkeeping may be wrong when this runs.
        uint16_t channels = midi_output_channels.load(std::memory_order_relaxed);
        // Always include channel 0, to clear a sustain left by another sender.
        if (channels == 0) channels = 1;
        for (uint8_t channel = 0; channel < 16; ++channel) {
            if (!(channels & (1u << channel))) continue;
            const uint8_t allNotesOff[3] = { static_cast<uint8_t>(0xB0 | channel), 123, 0 };
            const uint8_t allSoundOff[3] = { static_cast<uint8_t>(0xB0 | channel), 120, 0 };
            // Pedals off first: many synths keep notes ringing after All Notes
            // Off while the sustain or sostenuto pedal is down.
            for (const uint8_t controller : kPedalControllers) {
                const uint8_t pedalOff[3] = { static_cast<uint8_t>(0xB0 | channel), controller, 0 };
                midi_output->send(pedalOff, 3);
            }
            // Then one note-off per unmatched note-on, for receivers that
            // ignore All Notes Off.
            for (uint8_t note = 0; note < 128; ++note) {
                const uint8_t noteOff[3] = { static_cast<uint8_t>(0x80 | channel), note, 0 };
                uint8_t& held = midi_output_held[channel][note];
                if (everyNote) { midi_output->send(noteOff, 3); held = 0; }
                for (; held > 0; --held) midi_output->send(noteOff, 3);
            }
            midi_output->send(allNotesOff, 3);
            midi_output->send(allSoundOff, 3);
        }
    }
    catch (...) {
    }
}

void VirtualPianoPlayer::set_output_target(OutputTarget target) {
    // Held from the release through the store: a note that comes due during
    // the release would otherwise be pressed on the outgoing target after it
    // was cleared, and released on the new one, which leaves it held.
    std::lock_guard dispatchLock(dispatch_mutex);
    const OutputTarget current = output_target.load(std::memory_order_acquire);
    if (current == target) return;

    // Release everything held on the outgoing target before storing the new
    // one; nothing may be sent on the new target until this finishes. On the
    // MIDI target this silences the port and lifts the pedal; on keystrokes it
    // releases the keys, the sustain key and the modifiers.
    release_keys_locked(false);
    if (current == OutputTarget::Keystrokes) {
        // Clear MIDI2Key's keystroke state (pressed[], scancodeOwner[]).
        std::function<void()> hook;
        {
            std::lock_guard lock(output_mutex);
            hook = live_release_hook;
        }
        if (hook) hook();
    }

    output_target.store(target, std::memory_order_release);
    // A note with no key is heard on the port: a performer's take marks it.
    if (performer) take_stale.store(true, std::memory_order_release);
}

int VirtualPianoPlayer::toggle_transpose_adjustment() {
    auto [notes, durations] = transposeEngine.extractNotesAndDurations(midi_file);
    if (notes.empty()) {
        std::cout << "[TRANSPOSE] No notes.\n";
        return 0;
    }
    std::string key   = transposeEngine.estimateKey(notes, durations);
    std::string genre = transposeEngine.detectGenre(midi_file);
    std::cout << "Detected key: " << key << "\nDetected genre: " << genre << "\n";
    return transposeEngine.findBestTranspose(notes, durations, key, genre);
}

// Drum track detection helpers.
std::string getTrackName(const MidiTrack& track) {
    if (!track.name.empty())
        return track.name;
    for (const auto& evt : track.events) {
        if (evt.status == 0xFF && evt.data1 == 0x03 && !evt.metaData.empty())
            return std::string(evt.metaData.begin(), evt.metaData.end());
    }
    return "(no name)";
}

double computeDrumConfidence(const MidiTrack& track) {
    double confidence = 0.0;
    std::string trackName = track.name;
    if (trackName.empty()) {
        for (const auto& evt : track.events) {
            if (evt.status == 0xFF && evt.data1 == 0x03 && !evt.metaData.empty()) {
                trackName = std::string(evt.metaData.begin(), evt.metaData.end());
                break;
            }
        }
    }
    if (!trackName.empty()) {
        std::string lowerName = trackName;
        std::transform(lowerName.begin(), lowerName.end(),
                       lowerName.begin(), ::tolower);
        const std::vector<std::string> drumKeywords = {
            "drum","drums","ezdrummer","addictive drums","superior drummer",
            "bfd","drum kit","drumkit","percussion"
        };
        for (const auto& keyword : drumKeywords) {
            if (lowerName.find(keyword) != std::string::npos) {
                confidence += 1.0;
                break;
            }
        }
    }

    int totalNotes=0, minPitch=INT_MAX, maxPitch=0, sumPitch=0;
    std::unordered_map<int,int> pitchHistogram;
    std::set<int> channels;
    int noteOnCount=0, noteOffCount=0;
    uint32_t firstTick=UINT_MAX, lastTick=0;

    for (const auto& evt : track.events) {
        uint8_t status = evt.status;
        if ((status & 0xF0) == 0x90 || (status & 0xF0) == 0x80) {
            ++totalNotes;
            if ((status & 0xF0) == 0x90)
                ++noteOnCount;
            else
                ++noteOffCount;
            int pitch = evt.data1;
            sumPitch  += pitch;
            pitchHistogram[pitch]++;
            firstTick  = std::min(firstTick, evt.absoluteTick);
            lastTick   = std::max(lastTick, evt.absoluteTick);
            channels.insert(status & 0x0F);
            minPitch   = std::min(minPitch, pitch);
            maxPitch   = std::max(maxPitch, pitch);
        }
    }
    if (totalNotes == 0)
        return confidence;

    double noteOffRatio = static_cast<double>(noteOffCount) / totalNotes;
    confidence += (noteOffRatio < 0.1 ? 0.1
                  : (noteOffRatio > 0.3 ? -0.1 : 0.0));
    int range = maxPitch - minPitch;
    confidence += (range < 20 ? 0.3 : (range < 30 ? 0.2 : 0.0));

    int maxCount = 0;
    for (const auto& p : pitchHistogram) {
        maxCount = std::max(maxCount, p.second);
    }
    double dominantRatio = static_cast<double>(maxCount) / totalNotes;
    confidence += (dominantRatio > 0.7 ? 0.2
                  : (dominantRatio > 0.5 ? 0.1 : 0.0));
    double avgPitch = static_cast<double>(sumPitch) / totalNotes;
    if (avgPitch >= 35 && avgPitch <= 81) {
        confidence += 0.1;
    }
    if (channels.size() == 1 && *channels.begin() == 9) {
        confidence += 0.5;
    }
    if (lastTick > firstTick) {
        double density = static_cast<double>(totalNotes) /
                         (lastTick - firstTick + 1);
        if (density > 0.03) confidence += 0.05;
        if (density > 0.05) confidence += 0.05;
        if (density > 0.1)  confidence += 0.05;
    }
    std::vector<int> velocities;
    for (const auto& evt : track.events) {
        if (((evt.status & 0xF0) == 0x90) && evt.data2 > 0) {
            velocities.push_back(evt.data2);
        }
    }
    if (!velocities.empty()) {
        double sumVel = 0.0;
        for (int v : velocities) {
            sumVel += v;
        }
        double meanVel = sumVel / velocities.size();
        double variance= 0.0;
        for (int v : velocities) {
            variance += (v - meanVel)*(v - meanVel);
        }
        variance /= velocities.size();
        if (std::sqrt(variance) < 10) {
            confidence += 0.05;
        }
    }
    return confidence;
}

void VirtualPianoPlayer::process_tracks(const MidiFile& mid) {
    song_done.store(false, std::memory_order_release);
    short_send_logged.store(false, std::memory_order_release);
    note_events.clear();
    tempo_changes.clear();
    timeSignatures.clear();

    bool smpte = (mid.division & 0x8000) != 0;
    struct TempoPoint { uint64_t tick; uint64_t tempo; };
    std::vector<TempoPoint> tempo_points;
    if (!smpte) {
        // default 120 bpm if no tempo is set
        tempo_points.push_back({0, 500000ULL});
    }

    bool filterDrums = midi::Config::getInstance().midi.DETECT_DRUMS;
    if (filterDrums) {
        drum_flags.clear();
        drum_flags.resize(mid.tracks.size(), false);
        for (size_t i = 0; i < mid.tracks.size(); ++i) {
            double conf = computeDrumConfidence(mid.tracks[i]);
            double clampedConf = (conf > 1.0 ? 1.0 : conf);
            double threshold   = (!getTrackName(mid.tracks[i]).empty())
                                ? 0.8
                                : 0.9;
            if (clampedConf >= threshold) {
                drum_flags[i] = true;
                std::string trackName = getTrackName(mid.tracks[i]);
                std::cout << "[DRUMS] Track #" << i
                          << " \"" << trackName
                          << "\" flagged as drums (confidence: "
                          << std::fixed << std::setprecision(1)
                          << clampedConf * 100 << "%, raw: "
                          << conf << ")\n";
            }
        }
    }

    std::chrono::nanoseconds current_time_ns(0);
    std::vector<std::tuple<uint64_t,int,MidiEvent>> all_events;
    all_events.reserve(100000);

    for (int trackIndex = 0;
         trackIndex < static_cast<int>(mid.tracks.size());
         ++trackIndex)
    {
        for (auto& evt : mid.tracks[trackIndex].events) {
            all_events.push_back({evt.absoluteTick, trackIndex, evt});
        }
    }
    // Stable sort: events on the same tick must keep file order. A zero-length
    // note is an on and an off on one tick; reversed, the off closes nothing
    // and the on is never closed.
    std::stable_sort(all_events.begin(),
                     all_events.end(),
                     [](auto& a, auto& b) {
                         return std::get<0>(a) < std::get<0>(b);
                     });

    std::unordered_map<int,
        std::unordered_map<int, std::vector<std::chrono::nanoseconds>>
    > active_notes;
    // Track that struck each open note, keyed (channel << 8) | note; parallel to active_notes.
    std::unordered_map<int, std::vector<int>> open_tracks;

    size_t tempo_cursor = 0;
    auto tick2ns = [&](uint64_t st, uint64_t en) -> std::chrono::nanoseconds {
        if (smpte) {
            int8_t fps_val = static_cast<int8_t>(mid.division >> 8);
            int fps        = -fps_val;
            uint8_t tpf    = static_cast<uint8_t>(mid.division & 0xFF);
            // Guard against a zero frame rate or ticks per frame.
            if (fps * tpf <= 0) return std::chrono::nanoseconds(0);
            uint64_t nspt  = 1000000000ULL / (fps * tpf);
            return std::chrono::nanoseconds((en - st) * nspt);
        }
        else {
            std::chrono::nanoseconds total_ns(0);
            // Events arrive in tick order, so resume the tempo search from the last position.
            size_t tempo_idx = tempo_cursor;
            while (tempo_idx < tempo_points.size() - 1 &&
                   tempo_points[tempo_idx + 1].tick <= st)
            {
                ++tempo_idx;
            }
            tempo_cursor = tempo_idx;
            uint64_t current_tick = st;
            while (current_tick < en) {
                uint64_t nxt = (tempo_idx < tempo_points.size() - 1
                                ? tempo_points[tempo_idx + 1].tick
                                : en);
                uint64_t seg = std::min(en - current_tick, nxt - current_tick);
                // tempo in microseconds per quarter note
                uint64_t nspt = (tempo_points[tempo_idx].tempo * 1000ULL) /
                                 mid.division;
                total_ns += std::chrono::nanoseconds(seg * nspt);
                current_tick += seg;
                if (current_tick >= nxt && tempo_idx < tempo_points.size() - 1) {
                    ++tempo_idx;
                }
            }
            return total_ns;
        }
    };

    auto close_active_notes = [&](std::chrono::nanoseconds ctime) {
        using NH = midi::NoteHandlingMode;
        if (midi::Config::getInstance().playback.noteHandlingMode == NH::NoHandling) {
            active_notes.clear();
            return;
        }
        // Close each unterminated note with a release of the same name and
        // track; any other name would leave the key held.
        for (auto& [key, tracks] : open_tracks) {
            const std::string_view name = stable_note_name(key & 0xFF);
            for (int track : tracks)
                add_note_event(ctime, name, EventType::Release, 0, track);
        }
        open_tracks.clear();
        active_notes.clear();
    };

    uint64_t current_tick = 0;
    for (auto& [tick, trackIdx, evt] : all_events) {
        auto delta_ns = tick2ns(current_tick, tick);
        current_time_ns += delta_ns;
        current_tick     = tick;

        if (evt.status == 0xFF && evt.data1 == 0x51 && evt.metaData.size() == 3) {
            uint32_t tempo_val = (static_cast<uint8_t>(evt.metaData[0]) << 16)
                               | (static_cast<uint8_t>(evt.metaData[1]) << 8)
                               | (static_cast<uint8_t>(evt.metaData[2]));
            if (!smpte) {
                tempo_points.push_back({tick, tempo_val});
            }
            tempo_changes.push_back({ static_cast<double>(tick),
                                      static_cast<double>(tempo_val) });
        }
        else if (evt.status == 0xFF && evt.data1 == 0x58 && evt.metaData.size() == 4) {
            TimeSignature ts;
            ts.tick                     = tick;
            ts.numerator                = evt.metaData[0];
            ts.denominator              = static_cast<uint8_t>(1 << evt.metaData[1]);
            ts.clocksPerClick           = evt.metaData[2];
            ts.thirtySecondNotesPerQuarter= evt.metaData[3];
            timeSignatures.push_back(ts);
        }
        else if ((evt.status & 0xF0) == 0xB0 && PedalForController(evt.data1) >= 0) {
            add_pedal_event(current_time_ns,
                            evt.status & 0x0F,
                            PedalForController(evt.data1),
                            evt.data2,
                            trackIdx);
        }
        else if ((evt.status & 0xF0) == 0x90 ||
                 (evt.status & 0xF0) == 0x80)
        {
            int note     = evt.data1;
            int channel  = evt.status & 0x0F;
            int velocity = evt.data2;

            if ((evt.status & 0xF0) == 0x90 && velocity > 0) {
                handle_note_on(current_time_ns,
                               channel,
                               note,
                               velocity,
                               trackIdx,
                               active_notes);
                open_tracks[(channel << 8) | note].push_back(trackIdx);
            }
            else {
                // The release belongs to a track that struck the note, or its
                // key stays down: execute_note_event releases only for an
                // owner. The note-off's own track when it has the note open,
                // else the track the mode picks, so a note-off on another
                // track of the same channel still closes it.
                auto& tracks = open_tracks[(channel << 8) | note];
                int owner = trackIdx;
                if (!tracks.empty()) {
                    const bool lifo = midi::Config::getInstance().playback.noteHandlingMode == midi::NoteHandlingMode::LIFO;
                    auto open = tracks.end();
                    if (lifo) {
                        const auto last = std::find(tracks.rbegin(), tracks.rend(), trackIdx);
                        if (last != tracks.rend()) open = std::prev(last.base());
                    }
                    else open = std::find(tracks.begin(), tracks.end(), trackIdx);
                    if (open == tracks.end()) open = lifo ? std::prev(tracks.end()) : tracks.begin();
                    owner = *open;
                    tracks.erase(open);
                }
                handle_note_off(current_time_ns,
                                channel,
                                note,
                                velocity,
                                owner,
                                active_notes);
            }
        }
    }
    close_active_notes(current_time_ns);

    std::stable_sort(note_events.begin(),
                     note_events.end(),
                     [](const RawNoteEvent& a, const RawNoteEvent& b) {
                         return a.time < b.time;
                     });

    // Drop a zero-length note when another track strikes the same key at the
    // same instant; otherwise its release falls between the two strikes and the
    // game hears the note twice. A zero-length note on its own is kept.
    for (size_t first = 0; first < note_events.size();) {
        size_t last = first;
        while (last < note_events.size() && note_events[last].time == note_events[first].time) ++last;
        for (size_t press = first; press < last; ++press) {
            const auto& on = note_events[press];
            if (on.action != EventType::Press || PedalForName(on.note_or_control) >= 0) continue;
            size_t release = press + 1;
            while (release < last && !(note_events[release].action == EventType::Release &&
                                       note_events[release].trackIndex == on.trackIndex &&
                                       note_events[release].note_or_control == on.note_or_control)) ++release;
            if (release == last) continue;
            bool shared = false;
            for (size_t other = first; other < last && !shared; ++other)
                shared = note_events[other].action == EventType::Press &&
                         note_events[other].trackIndex != on.trackIndex &&
                         note_events[other].note_or_control == on.note_or_control;
            if (!shared) continue;
            note_events.erase(note_events.begin() + release);
            note_events.erase(note_events.begin() + press);
            last -= 2;
            --press;
        }
        first = last;
    }
}

void VirtualPianoPlayer::handle_note_off(std::chrono::nanoseconds ctime,
                                         int ch,
                                         int note,
                                         int vel,
                                         int trackIndex,
     std::unordered_map<int,std::unordered_map<int,std::vector<std::chrono::nanoseconds>>>& active_notes)
{
    using NH = midi::NoteHandlingMode;
    auto mode = midi::Config::getInstance().playback.noteHandlingMode;
    if (mode == NH::NoHandling) {
        note_events.push_back({ ctime,
                                stable_note_name(note),
                                EventType::Release,
                                vel,
                                trackIndex });
        return;
    }
    auto itCh = active_notes.find(ch);
    if (itCh != active_notes.end()) {
        auto& noteMap = itCh->second;
        auto itN = noteMap.find(note);
        if (itN != noteMap.end() && !itN->second.empty()) {
            note_events.push_back({ ctime,
                                    stable_note_name(note),
                                    EventType::Release,
                                    vel,
                                    trackIndex });
            if (mode == NH::LIFO) {
                itN->second.pop_back();
            }
            else {
                itN->second.erase(itN->second.begin());
            }
        }
    }
}

void VirtualPianoPlayer::handle_note_on(std::chrono::nanoseconds ctime,
                                        int ch,
                                        int note,
                                        int vel,
                                        int trackIndex,
    std::unordered_map<int,std::unordered_map<int,std::vector<std::chrono::nanoseconds>>>& active_notes)
{
    active_notes[ch][note].push_back(ctime);

    note_events.push_back({ ctime,
                            stable_note_name(note),
                            EventType::Press,
                            vel,
                            trackIndex });
}

void VirtualPianoPlayer::add_pedal_event(std::chrono::nanoseconds time,
                                         int channel,
                                         int pedal,
                                         int value,
                                         int trackIndex)
{
    EventType et = (value >= g_sustainCutoff)
                   ? EventType::Press
                   : EventType::Release;

    note_events.push_back({ time,
                            kPedalNames[pedal],
                            et,
                            (value | (channel << 8)),
                            trackIndex });
}

void VirtualPianoPlayer::add_note_event(std::chrono::nanoseconds time,
                                        std::string_view note,
                                        EventType action,
                                        int velocity,
                                        int trackIndex)
{
    // The event keeps the view, so note must have static storage.
    note_events.push_back({ time,
                            note,
                            action,
                            velocity,
                            trackIndex });
}

void VirtualPianoPlayer::speed_up() {
    adjust_playback_speed(1.1);
}

void VirtualPianoPlayer::slow_down() {
    adjust_playback_speed(1.0 / 1.1);
}

void VirtualPianoPlayer::adjust_playback_speed(double factor) {
    AwaitClock();
    uint64_t now_tsc = __rdtsc();
    if (!playback_started.load(std::memory_order_relaxed)) {
        playback_start_time = now_tsc;
        last_resume_tsc     = now_tsc;
    }
    else {
        // Bank the time elapsed at the old speed.
        uint64_t tick_diff = now_tsc - last_resume_tsc;
        double elapsed_ns = double(tick_diff) * cyclesToNs * current_speed;
        total_adjusted_time += std::chrono::nanoseconds(
            static_cast<std::chrono::nanoseconds::rep>(elapsed_ns + 0.5)
        );
        last_resume_tsc = now_tsc;
    }

    current_speed *= factor;
    current_speed = std::clamp(current_speed, 0.25, 2.0);
    if (std::fabs(current_speed - 1.0) < 0.05) {
        current_speed = 1.0; // Snap to normal speed if close
    }

    requested_speed.store(current_speed, std::memory_order_release);

    time_factor = cyclesToNs * current_speed;
    std::cout << "[SPEED] x" << current_speed << "\n";
}

void VirtualPianoPlayer::arrowsend(WORD sc, bool extended) {
    INPUT in[2] = {};
    in[0].type      = INPUT_KEYBOARD;
    in[0].ki.wScan  = sc;
    in[0].ki.dwFlags= KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0);

    in[1].type      = INPUT_KEYBOARD;
    in[1].ki.wScan  = sc;
    in[1].ki.dwFlags= KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP
                      | (extended ? KEYEVENTF_EXTENDEDKEY : 0);

    send_keys(in, 2);
}

void VirtualPianoPlayer::hotkey_listener() {
    int playPauseVK     = stringToVK(midi::Config::getInstance().hotkeys.PLAY_PAUSE_KEY);
    int rewindVK        = stringToVK(midi::Config::getInstance().hotkeys.REWIND_KEY);
    int skipVK          = stringToVK(midi::Config::getInstance().hotkeys.SKIP_KEY);
    int emergencyExitVK = stringToVK(midi::Config::getInstance().hotkeys.EMERGENCY_EXIT_KEY);

    bool wasPlayPause   = false;
    bool wasRewind      = false;
    bool wasSkip        = false;
    bool wasEmergency   = false;

    while (!hotkey_stop.load(std::memory_order_acquire)) {
        bool playPauseDown  = (GetAsyncKeyState(playPauseVK) & 0x8000) != 0;
        bool rewindDown     = (GetAsyncKeyState(rewindVK) & 0x8000) != 0;
        bool skipDown       = (GetAsyncKeyState(skipVK) & 0x8000) != 0;
        bool emergencyDown  = (GetAsyncKeyState(emergencyExitVK) & 0x8000) != 0;

        if (playPauseDown && !wasPlayPause) {
            toggle_play_pause();
        }
        if (rewindDown && !wasRewind) {
            rewind(std::chrono::seconds(10));
        }
        if (skipDown && !wasSkip) {
            skip(std::chrono::seconds(10));
        }
        if (emergencyDown && !wasEmergency) {
            emergency_exit();
        }

        wasPlayPause = playPauseDown;
        wasRewind    = rewindDown;
        wasSkip      = skipDown;
        wasEmergency = emergencyDown;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void VirtualPianoPlayer::emergency_exit() {
    std::cout << "[EMERGENCY] Emergency exit triggered. Stopping playback and exiting.\n";
    should_stop.store(true, std::memory_order_release);
    release_every_mapped_key();
    signalPlayback();
    std::exit(1);
}

void VirtualPianoPlayer::initializeKeyCache() {
    for (const auto& [note, key] : limited_key_mappings) {
        if (!key.empty() && g_keyCache.find(key) == g_keyCache.end()) {
            g_keyCache.emplace(key, computeKeySequence(key));
        }
    }
    for (const auto& [note, key] : full_key_mappings) {
        if (!key.empty() && g_keyCache.find(key) == g_keyCache.end()) {
            g_keyCache.emplace(key, computeKeySequence(key));
        }
    }
}

void VirtualPianoPlayer::execute_note_event(const NoteEvent& event) noexcept {
    std::lock_guard lock(dispatch_mutex);
    execute_note_locked(event);
}

void VirtualPianoPlayer::execute_note_locked(const NoteEvent& event) noexcept {
    dispatch_note_locked(event, false);
    // Octave doubling: the note's doubles are pressed and released with it.
    if (event.isSustain || output_target.load(std::memory_order_acquire) == OutputTarget::MidiDevice) return;
    for (const int8_t doubled : octave_doubles[std::clamp(note_name_to_midi(event.note), 0, 127)]) {
        if (doubled < 0) break;
        NoteEvent copy = event;
        copy.note = stable_note_name(doubled);
        dispatch_note_locked(copy, true);
    }
}

void VirtualPianoPlayer::dispatch_note_locked(const NoteEvent& event, bool doubled) noexcept {
    // Pedals on the MIDI target carry their value, not the cutoff's on or off.
    if (event.isSustain && output_target.load(std::memory_order_acquire) == OutputTarget::MidiDevice) {
        input_latency::Trace trace(input_latency::Source::Autoplay, input_latency::Kind::Sustain);
        send_pedal_to_midi(event, PedalForName(event.note), isTrackEnabled(event.trackIndex));
        return;
    }
    if (!isTrackEnabled(event.trackIndex)) {
        // A track may be muted/unsoloed after pressing a key. Its release must
        // still run, including the reversed pedal release in SPACE_UP mode.
        const bool releases = event.isSustain
            ? (currentSustainMode == SustainMode::SPACE_UP
                ? event.action == EventType::Press : event.action == EventType::Release)
            : event.action == EventType::Release;
        if (!releases) return;
    }

    // A gathered batch is measured as the one call play_batch sends.
    std::optional<input_latency::Trace> trace;
    if (!t_gathering)
        trace.emplace(input_latency::Source::Autoplay,
            event.isSustain ? input_latency::Kind::Sustain :
            event.action == EventType::Press ? input_latency::Kind::NoteOn : input_latency::Kind::NoteOff);

    // Track ownership applies on both targets: without it, one track's release
    // would cut another track's note on the same pitch. Only the emit step differs.
    const bool toMidi = output_target.load(std::memory_order_acquire) == OutputTarget::MidiDevice;

    if (!event.isSustain) {
        if (event.action == EventType::Press) {
            auto& owners = track_note_owners[std::string(event.note)];
            // Two tracks striking one key at the same instant produce one
            // strike. Both still own the key, so it stays down until both release.
            auto& struck = last_strike_time[std::string(event.note)];
            const bool struckThisInstant = !owners.empty() && struck == event.time;
            const bool down = !owners.empty();
            ++owners[event.trackIndex];
            if (struckThisInstant) return;
            // A doubled note on a key the song holds adds nothing to hear.
            if (doubled && down) return;
            struck = event.time;
            if (toMidi) {
                // No out-of-range fold (sounding_note): a MIDI device has all
                // 128 notes. No volume keys or velocity tap: velocity is in
                // the note-on, and a tap would send an extra note.
                const int number = MidiNumberForNoteName(std::string(event.note).c_str());
                if (number >= 0) {
                    // A key that is down comes up before it is struck again, as
                    // on a keyboard and as press_key does: a receiver that pairs
                    // note-ons with note-offs would otherwise hold the first.
                    if (down) {
                        const uint8_t off[3] = { 0x80, static_cast<uint8_t>(number), 0 };
                        send_midi_output(off, 3);
                    }
                    const uint8_t message[3] = { 0x90, static_cast<uint8_t>(number),
                                                 static_cast<uint8_t>(event.velocity & 0x7F) };
                    send_midi_output(message, 3);
                }
                return;
            }
            if (!doubled && enable_volume_adjustment.load(std::memory_order_relaxed)) {
                AdjustVolumeBasedOnVelocity(event.velocity);
            }
            const char tapBefore = sent_velocity;
            char velocityTap = 0;
            // A chord's level was tapped once before its presses (send_chord_level).
            if (enable_velocity_keypress.load(std::memory_order_relaxed) &&
                event.velocity != 0 && !chord_level)
            {
                const char wanted = getVelocityKey(event.velocity).back();
                if (wanted != tapBefore) {
                    // Only look for a free key when the velocity actually changes.
                    const char free = nearest_free_velocity_key(wanted,
                        [this](char key) { return velocity_key_is_held(key); });
                    if (free && free != tapBefore) {
                        velocityTap = free;
                        sent_velocity = free;
                    }
                }
            }
            // A tap that went nowhere was not sent: the next note sends it.
            if (!press_key(event.note, velocityTap, doubled) && velocityTap) sent_velocity = tapBefore;
        }
        else {
            const auto note = track_note_owners.find(std::string(event.note));
            if (note == track_note_owners.end()) return;
            const auto owner = note->second.find(event.trackIndex);
            if (owner == note->second.end()) return;
            if (--owner->second == 0) note->second.erase(owner);
            if (note->second.empty()) {
                track_note_owners.erase(note);
                if (toMidi) {
                    const int number = MidiNumberForNoteName(std::string(event.note).c_str());
                    if (number >= 0) {
                        const uint8_t message[3] = { 0x80, static_cast<uint8_t>(number), 0 };
                        send_midi_output(message, 3);
                    }
                }
                else release_key(event.note);
            }
        }
    }
    else {
        // The game has a sustain key only; the other pedals go to the MIDI target.
        if (currentSustainMode == SustainMode::IG || PedalForName(event.note) != 0) return;
        const bool inverted = currentSustainMode == SustainMode::SPACE_UP;
        const bool press = inverted ? event.action == EventType::Release : event.action == EventType::Press;
        if (press) sustain_owners.insert(event.trackIndex);
        else {
            if (!sustain_owners.erase(event.trackIndex) || !sustain_owners.empty()) return;
        }
        handle_sustain_event(event);
    }
}

// The pedal's value as played, merged across tracks by taking the highest.
// Ignore still silences sustain. The cutoff and the SPACE_UP inversion are
// keystroke concerns: a synth reads the value itself. A muted track can only
// lower its pedal, as a muted track's note can only be released.
void VirtualPianoPlayer::send_pedal_to_midi(const NoteEvent& event, int pedal, bool trackEnabled) noexcept {
    if (pedal < 0 || (pedal == 0 && currentSustainMode == SustainMode::IG)) return;
    try {
        auto& tracks = pedal_tracks[pedal];
        const auto found = tracks.find(event.trackIndex);
        // A late batch runs its releases first, so an older value can arrive
        // after a newer one; the newer one stands.
        if (found != tracks.end() && event.time < found->second.time) return;
        const int held = found != tracks.end() ? found->second.value : 0;
        const int value = trackEnabled ? event.sustainValue : (std::min)(event.sustainValue, held);
        tracks[event.trackIndex] = { event.time, value };

        int highest = 0;
        for (const auto& [track, state] : tracks) highest = (std::max)(highest, state.value);
        if (highest == pedal_sent[pedal]) return;
        pedal_sent[pedal] = highest;
        const uint8_t message[3] = { 0xB0, kPedalControllers[pedal], static_cast<uint8_t>(highest) };
        send_midi_output(message, 3);
    }
    catch (...) {
        // An allocation failure must not escape onto the playback thread.
    }
}

void VirtualPianoPlayer::handle_sustain_event(const NoteEvent& event) {
    switch (currentSustainMode) {
    case SustainMode::IG:
        return;

    case SustainMode::SPACE_DOWN:
        if (event.action == EventType::Press && !isSustainPressed) {
            if (event.sustainValue >= g_sustainCutoff) {
                pressKey(sustain_key_code);
                isSustainPressed = true;
            }
        }
        else if (event.action == EventType::Release && isSustainPressed) {
            if (event.sustainValue < g_sustainCutoff) {
                releaseKey(sustain_key_code);
                isSustainPressed = false;
            }
        }
        break;

    case SustainMode::SPACE_UP:
        if (event.action == EventType::Release && !isSustainPressed) {
            if (event.sustainValue < g_sustainCutoff) {
                pressKey(sustain_key_code);
                isSustainPressed = true;
            }
        }
        else if (event.action == EventType::Press && isSustainPressed) {
            if (event.sustainValue >= g_sustainCutoff) {
                releaseKey(sustain_key_code);
                isSustainPressed = false;
            }
        }
        break;
    }
}

void VirtualPianoPlayer::toggle_play_pause() {
    AwaitClock();
    bool wasPaused = paused.load(std::memory_order_acquire);
    paused.store(!wasPaused, std::memory_order_release);
    if (!wasPaused) {
        signalPlayback();
        // Let the playback thread stop before releasing, or it could re-press sustain.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        release_all_keys();

        // A held clock (Tap, a performer's action, a hold while the game is
        // behind) has not moved since it was held.
        if (!clock_frozen.load(std::memory_order_acquire) && !held_behind.load(std::memory_order_acquire)) {
            uint64_t current_tsc = __rdtsc();
            uint64_t tick_diff   = current_tsc - last_resume_tsc;
            double elapsed_ns    = double(tick_diff) * cyclesToNs * current_speed;
            total_adjusted_time += std::chrono::nanoseconds(
                static_cast<std::chrono::nanoseconds::rep>(elapsed_ns + 0.5)
            );
        }

        std::cout << "[PLAYBACK] Paused\n";
    }
    else {
        last_resume_tsc = __rdtsc();
        signalPlayback();
        std::cout << "[PLAYBACK] Resumed\n";
    }

    if (!playback_thread) {
        playback_thread = std::make_unique<std::jthread>(
            &VirtualPianoPlayer::play_notes, this
        );
    }
}

void VirtualPianoPlayer::skip(std::chrono::seconds duration) {
    release_all_keys();
    auto skip_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);

    if (!playback_started.load(std::memory_order_acquire)) {
        total_adjusted_time += skip_ns;
        if (total_adjusted_time < std::chrono::nanoseconds(0)) {
            total_adjusted_time = std::chrono::nanoseconds(0);
        }
        buffer_index.store(find_next_event_index(total_adjusted_time),
                           std::memory_order_release);
        return;
    }

    bool song_ended = (buffer_index.load(std::memory_order_acquire) >= note_buffer.size()) &&
                      playback_started.load(std::memory_order_acquire);

    if (song_ended) {
        restart_song();
        total_adjusted_time += skip_ns;
        if (total_adjusted_time < std::chrono::nanoseconds(0)) {
            total_adjusted_time = std::chrono::nanoseconds(0);
        }
        buffer_index.store(find_next_event_index(total_adjusted_time),
                           std::memory_order_release);
        last_resume_tsc = __rdtsc();
    }
    else {
        playback_control.requestSkip(duration);
        signalPlayback();
    }
}

void VirtualPianoPlayer::rewind(std::chrono::seconds duration) {
    release_all_keys();
    auto rewind_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);

    bool ended = (buffer_index.load(std::memory_order_acquire) >= note_buffer.size()) &&
                 playback_started.load(std::memory_order_acquire);

    if (ended) {
        should_stop.store(true, std::memory_order_release);
        signalPlayback();
        if (playback_thread && playback_thread->joinable()) {
            playback_thread->join();
        }
        auto total_len = note_buffer.empty()
                         ? std::chrono::nanoseconds(0)
                         : note_buffer.back()->time;
        total_adjusted_time = (rewind_ns > total_len)
                             ? std::chrono::nanoseconds(0)
                             : total_len - rewind_ns;
        buffer_index.store(find_next_event_index(total_adjusted_time),
                           std::memory_order_release);
        song_done.store(false, std::memory_order_release);
        paused.store(false, std::memory_order_release);
        should_stop.store(false, std::memory_order_release);

        last_resume_tsc = __rdtsc();
        playback_thread = std::make_unique<std::jthread>(
            &VirtualPianoPlayer::play_notes, this
        );
    }
    else {
        playback_control.requestRewind(duration);
        signalPlayback();
    }
}

bool VirtualPianoPlayer::isTrackEnabled(int trackIndex) const {
    if (trackIndex < 0 ||
        trackIndex >= static_cast<int>(trackMuted.size()))
    {
        return true;
    }
    bool anySolo = false;
    for (const auto& solo : trackSoloed) {
        if (solo->load(std::memory_order_relaxed)) {
            anySolo = true;
            break;
        }
    }
    if (anySolo) {
        return trackSoloed[trackIndex]->load(std::memory_order_relaxed);
    }
    return !trackMuted[trackIndex]->load(std::memory_order_relaxed);
}

// A change rebuilds a performer's take, whose score marks what is not heard.
void VirtualPianoPlayer::set_track_mute(size_t trackIndex, bool mute) {
    if (trackIndex < trackMuted.size()) {
        if (trackMuted[trackIndex]->exchange(mute, std::memory_order_relaxed) != mute && performer)
            take_stale.store(true, std::memory_order_release);
    }
}

void VirtualPianoPlayer::set_track_solo(size_t trackIndex, bool solo) {
    if (trackIndex < trackSoloed.size()) {
        if (trackSoloed[trackIndex]->exchange(solo, std::memory_order_relaxed) != solo && performer)
            take_stale.store(true, std::memory_order_release);
    }
}
