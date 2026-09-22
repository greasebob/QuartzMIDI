#ifndef PLAYBACK_SYSTEM_HPP
#define PLAYBACK_SYSTEM_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#pragma once

// Windows headers
#include <windows.h>
#include <windowsx.h>
#include <avrt.h>

// Standard headers
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <array>
#include <map>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <condition_variable>
#include <functional>

#include "Performer.hpp"

// Project-specific headers
#include "MidiOutput.hpp"  // IMidiOutput, for the output target below
#include "config.hpp"      // Contains midi::Config and configuration definitions
#include "Transpose.h"
#include "json.hpp"
#include "midi_parser.h"
#include "InputHeader.h"   // For InjectInput
#include "thread_pool.h"   // dp::thread_pool
#include "timer.h"

class VirtualPianoPlayer;
extern VirtualPianoPlayer* g_player;

// Global variables (definitions provided in CPP)
extern double g_totalSongSeconds;
extern int    g_sustainCutoff;

// =====================================================
// Sustain Mode Enumeration
// =====================================================
enum class SustainMode {
    IG,
    SPACE_DOWN,
    SPACE_UP
};

// =====================================================
// EventType: For note actions (Press/Release)
// =====================================================
enum class EventType : uint8_t {
    Press,
    Release
};

// =====================================================
// NoteEvent: Represents an internal note or control event.
// =====================================================
struct alignas(64) NoteEvent {
    std::chrono::nanoseconds time;
    std::string_view note;    // e.g. "C4" or "sustain"
    EventType action;         // Press or Release
    int velocity;
    bool isSustain;           // true if sustain pedal event
    int sustainValue;
    int trackIndex;
    // Set by the take: skip drops the note from this playthrough; hold is how
    // long a press is held, or -1 when it has no release.
    bool skip = false;
    std::chrono::nanoseconds hold{ -1 };
    NoteEvent() noexcept;
    NoteEvent(std::chrono::nanoseconds t, std::string_view n, EventType a, int v, bool s, int sv, int trackIdx) noexcept;
    bool operator>(const NoteEvent& other) const noexcept;
};

// =====================================================
// NoteEventPool: Fast allocator for NoteEvent objects.
// =====================================================
class alignas(64) NoteEventPool {
public:
    NoteEventPool();
    ~NoteEventPool();

    template<typename... Args>
    NoteEvent* allocate(Args&&... args) {
        if (reinterpret_cast<size_t>(current) + sizeof(NoteEvent) > reinterpret_cast<size_t>(end))
            allocateNewBlock();
        NoteEvent* event = new (current) NoteEvent(std::forward<Args>(args)...);
        current += sizeof(NoteEvent);
        allocated_count.fetch_add(1, std::memory_order_relaxed);
        return event;
    }
    void reset();
    size_t getAllocatedCount() const;
private:
    static constexpr size_t CACHE_LINE_SIZE = 64;
    static constexpr size_t BLOCK_SIZE = 1024 * 1024; // 1 MB block

    struct alignas(CACHE_LINE_SIZE) Block {
        char data[BLOCK_SIZE];
        Block* next;
    };

    Block* head;
    char* current;
    char* end;
    std::atomic<size_t> allocated_count;

    void allocateNewBlock();
};

// =====================================================
// PlaybackControl: Controls skip/rewind/restart commands.
// =====================================================
class PlaybackControl {
public:
    enum class Command { NONE, SKIP, REWIND, RESTART };
    struct State {
        std::chrono::nanoseconds position{ 0 };
        size_t event_index{ 0 };
        bool needs_reset{ false };
    };

    void requestSkip(std::chrono::seconds amount);
    void requestRewind(std::chrono::seconds amount);
    bool hasCommand() const;
    State processCommand(const State& current_state, double speed, size_t buffer_size);
private:
    mutable std::mutex mutex;
    Command pending_command{ Command::NONE };
    std::chrono::seconds command_amount{ 0 };
    std::atomic<bool> command_processed{ true };
};

// ----------------------------------------------------
// RawNoteEvent: Holds raw MIDI event data.
// ----------------------------------------------------
struct RawNoteEvent {
    std::chrono::nanoseconds time;
    std::string_view note_or_control;
    EventType action;         // Converted to enum
    int velocity;
    int trackIndex;
};

// Folds an out-of-range MIDI note onto the game's 61 keys, C2 to C7 (36..96).
// Shifts by whole octaves so the pitch class is kept: notes below land in the
// lowest octave, notes above in the highest. Any distance folds.
constexpr int FoldOntoSixtyOneKeys(int midi) noexcept {
    const int pitch = midi % 12;
    return midi < 36 ? 36 + pitch : midi > 96 ? (pitch == 0 ? 96 : 84 + pitch) : midi;
}

// =====================================================
// VirtualPianoPlayer: Main class for virtual piano playback.
// =====================================================
class VirtualPianoPlayer {
public:
    VirtualPianoPlayer(bool listenForHotkeys = true,
                       const std::filesystem::path& configPath = "config.json") noexcept(false);
    ~VirtualPianoPlayer();

    // Track controls
    void set_track_mute(size_t trackIndex, bool mute);
    void set_track_solo(size_t trackIndex, bool solo);

    // Playback controls
    void toggle_play_pause();
    void skip(std::chrono::seconds duration);
    void rewind(std::chrono::seconds duration);
    void restart_song();
    void speed_up();
    void slow_down();
    void toggle_out_of_range_transpose();
    void toggle_88_key_mode();
    void toggle_velocity_keypress();
    void toggle_volume_adjustment();
    void toggleSustainMode();
    void toggle_performer();
    int  toggle_transpose_adjustment();
    // release_all_keys() releases the keys pressed_keys records as down; every
    // transport action (load, pause, seek, skip, rewind, restart) uses it.
    // release_every_mapped_key() releases all 88 mapped keys regardless of
    // pressed_keys, for the panic key, where that bookkeeping may be wrong.
    void release_all_keys();
    void release_every_mapped_key();
    void calibrate_volume();
    void process_tracks(const MidiFile& midi_file);

    // ---- Velocity tap modifier ---------------------------------------------
    //
    // Scan code of the velocity modifier, resolved when the setting is applied
    // so the injection path never parses a string. Static because
    // build_velocity_tap is and the setting lives on the config singleton.
    static std::atomic<WORD> velocity_modifier_scan;   // ALT by default

    // Scan code for "alt", "ctrl" or "shift"; anything else maps to ALT, since
    // a note-playing key here would be worse than the wrong modifier.
    static WORD VelocityModifierScan(const std::string& name) noexcept;

    // Called at construction and whenever the setting changes.
    void apply_velocity_modifier();

    // Velocity characters that, under the configured modifier, are also key
    // mappings in the current layout, spelled as the mapping does ("ctrl+w").
    // The 88-key layout binds its lowest notes to ctrl+ combinations, so with
    // ctrl some velocity taps also play a note.
    std::vector<std::string> velocity_modifier_conflicts() const;

    // ---- MIDI output -----------------------------------------------------
    //
    // One output target for the whole app. Live input and autoplay both either
    // inject keystrokes or send MIDI to the open port, never both.
    enum class OutputTarget { Keystrokes, MidiDevice };
    std::atomic<OutputTarget> output_target{ OutputTarget::Keystrokes };

    // A key must be released on the target it was pressed on, or it sticks (a
    // keystroke held in the game, a note sounding on the synth). This releases
    // everything held on the outgoing target, then stores the new one; nothing
    // may be sent in between. Closing the port, losing the device and stopping
    // playback also go through here.
    void set_output_target(OutputTarget target);

    bool open_midi_output(const std::wstring& deviceId);
    void close_midi_output();
    std::wstring opened_midi_output() const;

    // No-op unless the MIDI target is selected and a port is open. Safe from
    // the MIDI callback and playback threads.
    void send_midi_output(const uint8_t* message, size_t length) noexcept;

    // All Notes Off and sustain off on every channel used. Called on panic,
    // where the held-note bookkeeping may be wrong.
    void silence_midi_output() noexcept;

    // MIDI2Key tracks held keystrokes in pressed[] and scancodeOwner[]. Its
    // owner registers a reset here so a target switch clears that state in the
    // same call that releases the keys.
    void set_live_release_hook(std::function<void()> hook);

    // Static handle for command event
    static HANDLE command_event;

    // Data members
    std::vector<RawNoteEvent> note_events;
    std::vector<std::string> string_storage;
    std::vector<std::pair<double, double>> tempo_changes;
    std::vector<TimeSignature> timeSignatures;
    std::unique_ptr<std::jthread> playback_thread;
    std::atomic<bool> eightyEightKeyModeActive{ true };

    std::vector<std::shared_ptr<std::atomic<bool>>> trackMuted;
    std::vector<std::shared_ptr<std::atomic<bool>>> trackSoloed;
    std::atomic<bool> midiFileSelected{ false };
    std::atomic<bool> should_stop{ false };
    std::atomic<bool> paused{ true };
    std::atomic<bool> playback_started{ false };
    std::atomic<size_t> buffer_index{ 0 };

    double current_speed{ 1.0 };
    double paused_time = 0.0;
    int currentTransposition = 0;
    std::chrono::nanoseconds total_adjusted_time{ 0 };

    // Returns current adjusted playback time.
    std::chrono::nanoseconds get_adjusted_time() noexcept;

    // Precomputed scan table for key mapping.
    static const std::array<WORD, 256> SCAN_TABLE_AUTO;

    MidiFile midi_file;
    std::vector<NoteEvent*> note_buffer;

    // Velocity functions
    void setVelocityCurveIndex(size_t index);
    std::string getVelocityCurveName(midi::VelocityCurveType curveType);
    std::string getVelocityKey(int targetVelocity);
    // Writes the four-input modifier tap for a velocity key into out and
    // returns the count, or 0 for a key with no scan code. The caller sends it
    // in the same SendInput batch as the note (see press_key).
    static size_t build_velocity_tap(char velocityKey, INPUT* out) noexcept;
    static constexpr size_t VELOCITY_TAP_INPUTS = 4;
    // Velocity keys are also note keys, and tapping one that is held as a note
    // releases that note in the game. Returns the nearest velocity key that
    // held() does not claim, quieter first, or 0 when all 32 are held.
    template <class Held>
    static char nearest_free_velocity_key(char wanted, Held&& held) noexcept {
        static constexpr char keys[] = "1234567890qwertyuiopasdfghjklzxc";
        constexpr int count = static_cast<int>(sizeof(keys)) - 1;
        int index = -1;
        for (int i = 0; i < count; ++i) if (keys[i] == wanted) { index = i; break; }
        if (index < 0) return wanted;
        for (int distance = 0; distance < count; ++distance) {
            const int quieter = index - distance, louder = index + distance;
            if (quieter >= 0 && !held(keys[quieter])) return keys[quieter];
            if (louder < count && !held(keys[louder])) return keys[louder];
        }
        return 0;
    }

    // Sustain settings
    SustainMode currentSustainMode{ SustainMode::IG };
    std::atomic<bool> enable_volume_adjustment{ false };
    std::atomic<bool> enable_velocity_keypress{ false };

    // The take: the score minus the hand the user plays, passed through the
    // performer add-on (Performer.hpp) when one is set and enabled.
    // performer_on is written by the UI thread; settings are guarded by
    // take_mutex. Changing either marks the take stale, and the playback thread
    // rebuilds it between events so dispatch never waits on it.
    std::atomic<bool> performer_on{ false };
    // Call before playback starts; the api must outlive the player. `settings`
    // is the add-on's JSON, passed through unchanged.
    void set_performer(const qm_performer_api* api);
    void set_performer_settings(std::string settings);
    // Non-zero replaces the per-song seed so a run is reproducible. Test only.
    std::atomic<uint64_t> take_seed_override{ 0 };
    // The score in performer form with mates paired, for estimates before playback.
    std::vector<qm_score_event> score() const;
    // Clock rate, writable from any thread. The playback thread applies it
    // within one slice and keeps the song position, so no note is cut.
    std::atomic<double> requested_speed{ 1.0 };

    // In Tap the clock is stopped and the performer plays what each tap asks
    // for. Each tap carries its key, so alternating keys release only their
    // own notes. Tap requires a performer.
    enum class Trigger : uint8_t { Auto, Tap };
    std::atomic<Trigger> trigger{ Trigger::Auto };
    std::atomic<bool> tap_holds_notes{ true };   // false keeps the recording's lengths
    void tap(int key, bool down);
    // Key mapping
    std::map<std::string, std::string> limited_key_mappings;
    std::map<std::string, std::string> full_key_mappings;
    std::unordered_map<std::string, std::atomic<bool>> pressed_keys;
    std::string lastPressedKey;
    bool isSustainPressed{ false };
    WORD sustain_key_code{ 0 };

    bool ENABLE_OUT_OF_RANGE_TRANSPOSE{ false };

    std::array<int, 128> volume_lookup;
    WORD volume_up_key_code{ 0 };
    WORD volume_down_key_code{ 0 };
    // Whether each is sent with KEYEVENTF_EXTENDEDKEY; true for the default arrow keys.
    bool volume_up_extended{ true };
    bool volume_down_extended{ true };
    bool volume_key_extended(WORD sc) const { return sc == volume_up_key_code ? volume_up_extended : volume_down_extended; }
    static bool isExtendedVK(WORD vk) {
        switch (vk) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_INSERT: case VK_DELETE:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU:
            return true;
        default: return false;
        }
    }
    WORD pause_key_code{ 0 };
    WORD rewind_key_code{ 0 };
    WORD skip_key_code{ 0 };
    WORD emergency_exit_key_code{ 0 };
    std::atomic<int> current_volume{ 0 };
    std::atomic<int> max_volume{ 0 };
    std::vector<bool> drum_flags;
    std::unique_ptr<std::jthread> hotkey_thread;
    std::atomic<bool> hotkey_stop{ false }; 
    void hotkey_listener();
    void emergency_exit();
    bool isTrackEnabled(int trackIndex) const;
    WORD vkToScanCode(int vk);
    std::condition_variable playback_cv;
    std::mutex playback_cv_mutex;
    unsigned long long last_resume_tsc;
    unsigned long long playback_start_time;

private:
    // output_mutex guards midi_output, midi_output_held and live_release_hook,
    // so a target switch cannot race an open or close. It is also taken on the
    // note path, which is why IMidiOutput::send must not block. It is not
    // recursive: code already holding it uses midi_output directly instead of
    // send_midi_output.
    std::unique_ptr<IMidiOutput> midi_output;
    std::atomic<uint16_t> midi_output_channels{ 0 };   // bit per channel touched
    // Unmatched note-ons per channel and note. silence_midi_output sends a
    // note-off for each, since many receivers ignore All Notes Off.
    std::array<std::array<uint8_t, 128>, 16> midi_output_held{};
    std::function<void()> live_release_hook;
    mutable std::mutex output_mutex;

    std::mutex buffer_mutex;
    PlaybackControl playback_control;
    UINT m_timerResolutionSet{ 0 };
    // Dispatch ownership prevents an inaudible track's note-off from releasing
    // another track's note, while allowing releases after a live mute change.
    std::mutex dispatch_mutex;
    std::unordered_map<std::string, std::unordered_map<int, size_t>> track_note_owners;
    // Last strike time per note, so two tracks striking it at the same instant
    // produce one keypress.
    std::unordered_map<std::string, std::chrono::nanoseconds> last_strike_time;
    std::set<int> sustain_owners;
    double inv_cpu_freq;  // Optional for optimization
    double time_factor;
    // Per instance, not static: each player creates and closes its own timer.
    HANDLE waitable_timer = nullptr;
    // True when CREATE_WAITABLE_TIMER_HIGH_RESOLUTION succeeded. A normal timer
    // fires on the scheduler tick, too coarse to end a short spin on.
    bool waitable_timer_precise = false;

    // Helper to signal playback thread (notify condition variable and legacy event)
    inline void signalPlayback() noexcept {
        SetEvent(command_event);
        playback_cv.notify_all();
    }

    // Core playback functions.
    void play_notes();
    void prepare_event_queue();
    void execute_note_event(const NoteEvent& event) noexcept;
    void handle_sustain_event(const NoteEvent& event);
    size_t find_next_event_index(const std::chrono::nanoseconds& target_time);
    void reset_volume();
    void initializeKeyCache();
    void KeyPress(std::string_view key, bool press);
    void release_keys(bool everyMapping);
    int stringToVK(std::string_view keyName);
    void sendVirtualKey(WORD vk, bool is_press);
    void pressKey(WORD vk);
    void releaseKey(WORD vk);
    // velocityKey is the velocity tap to send ahead of the note, or 0 for none.
    // Both go out in one SendInput batch.
    void press_key(std::string_view note, char velocityKey = 0) noexcept;
    // True when a note that is down is typed on this velocity key's own key.
    bool velocity_key_is_held(char velocityKey) noexcept;
    void release_key(std::string_view note) noexcept;
    std::string sounding_note(std::string_view note);
    std::string transpose_note(std::string_view note);
    int note_name_to_midi(std::string_view note_name);
    std::string get_note_name(int midi_note);
    void handle_note_off(std::chrono::nanoseconds ctime, int ch, int note, int vel, int trackIndex,
        std::unordered_map<int, std::unordered_map<int, std::vector<std::chrono::nanoseconds>>>& active_notes);
    void handle_note_on(std::chrono::nanoseconds ctime, int ch, int note, int vel, int trackIndex,
        std::unordered_map<int, std::unordered_map<int, std::vector<std::chrono::nanoseconds>>>& active_notes);
    void add_sustain_event(std::chrono::nanoseconds time, int channel, int sustainValue, int trackIndex);
    void add_note_event(std::chrono::nanoseconds time, std::string_view note, EventType action, int velocity, int trackIndex);
    void adjust_playback_speed(double factor);
    void arrowsend(WORD scanCode, bool extended);
    void precompute_volume_adjustments();
    void AdjustVolumeBasedOnVelocity(int velocity) noexcept;
    std::pair<std::map<std::string, std::string>, std::map<std::string, std::string>> define_key_mappings();
    // Pool
    NoteEventPool event_pool;

    // Take state; playback thread only unless noted.
    mutable std::mutex take_mutex;           // guards the three below
    std::string performer_settings = "{}";
    qm_performer_api performer_api{};
    void* performer = nullptr;
    std::atomic<bool> take_stale{ false };
    uint64_t take_seed{ 0 };
    // Taps: the UI thread queues, the playback thread drains.
    std::mutex tap_mutex;
    std::vector<qm_tap> tap_queue;
    std::atomic<bool> clock_frozen{ false };           // true in Tap mode
    qm_player performer_host();
    void tap_step(size_t& current_index, size_t buffer_size);
    // Note name with static storage, for keys the take substitutes for the score's.
    static std::string_view stable_note_name(int midi_note);
    TransposeEngine transposeEngine;
    size_t currentVelocityCurveIndex = 0;
};

#endif // PLAYBACK_SYSTEM_HPP
