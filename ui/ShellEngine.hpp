#pragma once
#include "TrackModel.hpp"
#include "VelocityModel.hpp"
#include "AutoVolume.hpp"
#include "LibraryModel.hpp"
#include "ShellLog.hpp"
#include "ConnectInput.hpp"
#include "DeviceModel.hpp"
#include "HotkeyNames.hpp"
#include "../engine/VelocityTelemetry.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace shell {

// The bar holding `at` and the point `bars` bars after its start, in seconds.
std::pair<double, double> BarsFrom(const MidiFile& file, double at, int bars);

// A performer add-on control as declared in engine/Performer.hpp, plus the
// value the engine stores for it by id. The app attaches no meaning to it.
struct PerformerControl {
    std::string id, field, name;
    bool isSwitch = false;
    // A choice's value is the index into choices. panel = drawn beside the triggers.
    bool isChoice = false, panel = false;
    std::vector<std::string> choices;
    // Switch: 0 or 1. Slider: 0..1, or negative while it follows its estimate.
    double value = 0;
    // Slider displayed as a MIDI note in [low, high] instead of a fraction.
    bool showsNote = false;
    int low = 0, high = 127;
    // Hidden while the shownWith control is 0 or hidden, or when the estimate
    // for this song is negative (noEstimate).
    std::string shownWith;
    bool noEstimate = false;
    std::string estimateName;   // slider or choice: label of the add-on's estimate
    double estimate = 0;
    std::string estimates;      // switch: id of the slider whose estimate it disables
    bool remembers = false;     // switch: song controls per song; a song can be pinned
    bool holds = false;         // switch: a tap key holds what it played
    bool perSong = false;       // section choice or slider: kept per song while remembers is on
    // What a song without a value of its own starts at: the declared value, or
    // -1 for a slider with an estimate.
    double rest = 0;
    int trigger = -1;           // owning trigger, or -1 for the section
};
// A row's own amounts on a preset's habits, kept while a preset is in use.
struct PerformerCustom {
    int preset = 0;
    std::map<std::string, double> values;
};
struct PerformerTrigger {
    std::string name;
    bool plays = false, steps = false;
    // Hotkey indices start at firstKey; keyFields are their config fields.
    std::string keysName, keysAdd;
    size_t firstKey = 0;
    std::vector<std::string> keyFields, keyDefaults;
};
// A performer key that works whatever the trigger, while the song plays.
struct PerformerAction {
    std::string id, name, field, keyDefault;
    bool schedules = false;   // plays through the take, with the clock running
    size_t key = 0;   // its hotkey index
};
struct PerformerSection {
    bool loaded = false, on = false;
    std::string name, tag, config;
    std::string presetId, presetField, presetName;
    std::vector<std::string> presets;
    std::vector<std::map<std::string, double>> presetValues;
    // What each preset is saved and sent as: the value its choice declares, or its index.
    std::vector<int> presetNumbers;
    // Each preset's values from before; a set that sits on one reads as the preset.
    std::vector<std::vector<std::map<std::string, double>>> presetEarlier;
    // The row's sliders: those a preset sets, in draw order.
    std::vector<std::string> rowIds;
    // The Player row: a preset, or Custom (the row's own amounts on the
    // preset's habits). customSaved is the Custom kept while a preset is lit.
    int preset = 0;
    bool custom = false;
    std::optional<PerformerCustom> customSaved;
    // The open song plays its own row.
    bool pinned = false;
    std::vector<PerformerControl> controls;
    std::vector<PerformerTrigger> triggers;
    std::vector<PerformerAction> actions;
    int PresetNumber() const { return presetNumbers.empty() ? preset : presetNumbers[static_cast<size_t>(preset)]; }
    // The preset saved as `number`, or -1.
    int PresetOf(int number) const {
        for (size_t i = 0; i < presetNumbers.size(); ++i) if (presetNumbers[i] == number) return static_cast<int>(i);
        return -1;
    }
    // Index of the action owning a hotkey index, or -1.
    int ActionOfKey(size_t hotkey) const {
        for (size_t i = 0; i < actions.size(); ++i) if (actions[i].key == hotkey) return static_cast<int>(i);
        return -1;
    }
    const PerformerControl* Find(const std::string& id) const {
        for (const auto& control : controls) if (control.id == id) return &control;
        return nullptr;
    }
    PerformerControl* Find(const std::string& id) { return const_cast<PerformerControl*>(std::as_const(*this).Find(id)); }
    // True when the slider has an estimate and no switch disables it.
    bool Estimated(const PerformerControl& slider) const {
        if (slider.estimateName.empty()) return false;
        for (const auto& control : controls) if (control.isSwitch && control.estimates == slider.id && control.value == 0) return false;
        return true;
    }
    double Shown(const PerformerControl& slider) const { return slider.value < 0 ? slider.estimate : slider.value; }
    bool InRow(const std::string& id) const { return std::find(rowIds.begin(), rowIds.end(), id) != rowIds.end(); }
    // Kept per song while remembers is on: a section choice or slider declared
    // per song or with an estimate.
    bool PerSong(const PerformerControl& control) const {
        return !control.isSwitch && control.trigger < 0 && (control.perSong || !control.estimateName.empty());
    }
    // The value preset `index` sets a row slider to: its own, or the slider's declared value.
    double Target(int index, const std::string& id) const {
        const auto& values = presetValues[static_cast<size_t>(index)];
        if (const auto found = values.find(id); found != values.end()) return std::clamp(found->second, 0.0, 1.0);
        const auto* control = Find(id);
        return control ? control->rest : 0;
    }
    // True when every row slider equals, to the whole percent, the values of
    // preset `index` or one of its earlier sets; an id an earlier set leaves
    // out takes the preset's value.
    bool OnPreset(int index, const std::map<std::string, double>& values) const {
        const auto percent = [](double value) { return std::lround(value * 100); };
        const auto sits = [&](const std::map<std::string, double>& set) {
            for (const auto& id : rowIds) {
                const double target = Target(index, id);
                const auto have = values.find(id), earlier = set.find(id);
                if (percent(have == values.end() ? target : have->second) != percent(earlier == set.end() ? target : earlier->second)) return false;
            }
            return true;
        };
        if (sits({})) return true;
        for (const auto& set : presetEarlier[static_cast<size_t>(index)]) if (sits(set)) return true;
        return false;
    }
    // The remembers switch is on.
    bool Remembers() const {
        for (const auto& control : controls) if (control.remembers) return control.value != 0;
        return false;
    }
    bool Drawn(const PerformerControl& control) const {
        if (control.noEstimate) return false;
        const auto* with = control.shownWith.empty() ? nullptr : Find(control.shownWith);
        return !with || (with->value != 0 && !with->noEstimate);
    }
    // Index of the trigger owning a hotkey index, or -1.
    int TriggerOfKey(size_t hotkey) const {
        for (size_t i = 0; i < triggers.size(); ++i)
            if (hotkey >= triggers[i].firstKey && hotkey < triggers[i].firstKey + triggers[i].keyFields.size()) return static_cast<int>(i);
        return -1;
    }
};

struct EngineSnapshot {
    std::shared_ptr<const std::vector<MidiEntry>> files = std::make_shared<const std::vector<MidiEntry>>();
    // The songs playlists and the queue list from outside the library, each
    // named by its full path, with its size and length once the worker has read them.
    std::shared_ptr<const std::vector<MidiEntry>> outside = std::make_shared<const std::vector<MidiEntry>>();
    std::vector<TrackRow> rows;
    std::filesystem::path folder;
    std::filesystem::path loaded;
    std::string error;
    std::shared_ptr<const LogView> log = std::make_shared<const LogView>();
    uint64_t generation = 0;
    bool busy = false;
    bool playing = false;
    int playbackCountdown = 0;
    int playbackDelay = 3;
    // Seek step in seconds for the transport buttons and F2/F3.
    int seekStep = 10;
    // Global hotkey names in kHotkeyFields order; empty means unbound. The
    // shell re-registers them when hotkeyRevision changes.
    std::array<std::string, kHotkeys> hotkeys = [] {
        std::array<std::string, kHotkeys> names;
        for (size_t i = 0; i < kAppHotkeys; ++i) names[i] = kHotkeyDefaults[i];
        return names;
    }();
    uint64_t hotkeyRevision = 0;
    bool typingAcknowledged = true;
    // Drawn only when performer.loaded is set.
    PerformerSection performer;
    // Keep the performer's song controls per song and let a song be pinned to
    // its own Player row: the section's remembers switch, off without one.
    bool rememberPerSong = false;
    // Active performer trigger; 0 is the clock. A "plays" trigger runs the song
    // while its key is held; a "steps" trigger advances once per press.
    int trigger = 0;
    bool TriggerPlays() const { return trigger > 0 && static_cast<size_t>(trigger) < performer.triggers.size() && performer.triggers[trigger].plays; }
    bool TriggerSteps() const { return trigger > 0 && static_cast<size_t>(trigger) < performer.triggers.size() && performer.triggers[trigger].steps; }
    bool shuffle = false;
    // Applied when a file loads. detectDrums flags kits off channel 10 so Solo
    // Piano excludes them; autoTranspose sets Transpose to the file's best fit.
    bool detectDrums = true;
    bool autoTranspose = false;
    FileSort fileSort = FileSort::Name;
    bool descendingFiles = false;
    // Each velocity bucket change types the modifier plus one of
    // "1234567890qwertyuiopasdfghjklzxc", all of which are also notes in the
    // FULL mapping. A game that doesn't consume the modified keypress plays
    // that character as an extra note.
    bool velocity = true;
    bool sustain = true;
    bool eightyEightKeys = true;
    bool outRange = false;
    bool autoVolume = false;
    bool autoVolumeNeedsCalibration = false;
    int autoVolumeCountdown = 0;
    bool autoVolumeFocusing = false;
    uint64_t autoVolumeRevision = 0;
    std::vector<GameWindow> volumeWindows;
    GameWindow volumeTarget;
    std::string volumeDownKey;
    std::string volumeUpKey;
    // The keys the app holds for the sustain and sostenuto pedals, VK_ names.
    std::string sustainKey = "VK_SPACE";
    std::string sostenutoKey = "VK_OEM_6";
    // Whether `vk` cannot be the sostenuto key: a mouse button, which the app
    // cannot type, a note key, or a key it types for sustain or volume.
    bool SostenutoKeyRefused(int vk) const {
        return vk == 0 || IsMouseHotkey(vk) || IsNoteKey(vk, keyMappings) ||
               vk == NameToVK(sustainKey) || vk == NameToVK(volumeDownKey) || vk == NameToVK(volumeUpKey);
    }
    int volumeInitial = 100;
    double position = 0;
    // While a song plays, the worker moves its position here between
    // publishes rather than copying the whole state; NaN until it does. Every
    // copy shares it, so read Position().
    std::shared_ptr<std::atomic<double>> clock = std::make_shared<std::atomic<double>>(std::numeric_limits<double>::quiet_NaN());
    double Position() const { const double live = clock->load(std::memory_order_relaxed); return live == live ? live : position; }
    double duration = 0;
    // Live MIDI input. Devices are opaque backend-specific ids, never indices
    // (see engine/MidiInput.hpp).
    std::vector<LiveDevice> devices;
    std::wstring liveDevice;
    bool liveActive = false;
    bool midiConnect = false;
    int liveChannel = -1;  // -1 listens on every channel
    bool outputMidi = false;
    std::wstring outputDevice;
    std::vector<LiveDevice> outputDevices;
    // The name of the port the app creates (see NamedMidiPortId), which other
    // apps list as a MIDI input.
    std::string outputPortName = "QuartzMIDI";
    double speed = 1.0;
    // Speed slider range; speedMin <= 1 <= speedMax.
    double speedMin = .25, speedMax = 2.0;
    int transpose = 0;
    std::map<std::string, std::string> keyMappings;
    uint64_t mappingRevision = 0;
    std::shared_ptr<const std::string> sheetText = std::make_shared<const std::string>();
    size_t sheetNotes = 0;
    size_t sheetGroups = 0;
    size_t sheetMerged = 0;
    size_t sheetUnmapped = 0;
    uint64_t sheetRevision = 0;
    bool sheetReady = false;
    // Sheets add-on loaded; the panel hides sheet controls without it.
    bool sheetsAddon = false;
    // An addons folder exists; gates the add-on entries in Help.
    bool addonsFolder = false;
    // Set when the sheet was written to the editor page.
    std::filesystem::path sheetSaved;
    // Sheet output mirrors the MIDI folder's sub-folders under sheetsFolder;
    // empty means "<MIDI folder> sheets" beside the library. sheetStylePage is
    // a page saved from the editor; empty uses midi-converter's defaults.
    std::filesystem::path sheetsFolder;
    std::filesystem::path sheetStylePage;
    bool sheetImage = true;
    bool sheetTextFile = true;
    bool sheetPageFile = true;
    // Output folder of the last SaveSheetFiles; empty for clipboard and editor.
    std::filesystem::path sheetFilesSaved;
    // Whole-library export, run on its own thread.
    bool sheetBatchRunning = false;
    size_t sheetBatchDone = 0;
    size_t sheetBatchTotal = 0;
    size_t sheetBatchFailed = 0;
    std::string sheetBatchStatus;
    std::vector<VelocityPreset> curves;
    VelocityEdit curve;
    VelocityEdit previousCurve;
    VelocityPreset previousPreset;
    bool comparingCurve = false;
    bool hasPreviousCurve = false;
    bool canUndoCurve = false;
    bool canRedoCurve = false;
    uint64_t curveRevision = 0;
    int sustainCutoff = 64;
    std::string velocityModifier = "alt";
    std::vector<std::string> velocityModifierConflicts;
    double wootingTriggerThreshold = 0.25;
    int wootingShiftAmount = 1;
    double wootingVelocitySensitivity = 1.0;
    int wootingMinVelocity = 1;
    // Set 1 scancodes of the sustain, sostenuto and soft pedal keys, 0 for none.
    std::array<int, 3> wootingPedalKeys{0x39, 0, 0};
    // Audio-to-MIDI via tools/mp3-to-midi. conversionStatus is the converter's
    // latest output line; the finished .mid is written to the MIDI folder.
    bool converting = false;
    bool conversionFailed = false;
    std::string conversionStatus;
    // Sign-in and setup.ps1 run as the converter's job, so converting is also
    // true during them; signingIn and settingUp tell them apart.
    bool signingIn = false;
    bool youtubeSignedIn = false;
    bool converterInstalled = false;
    bool converterCanSetUp = false;
    bool settingUp = false;
    // The installed converter's engine and the ones this PC can run, best
    // first; see audio_to_midi::Install.
    std::string converterEngine;
    std::vector<std::string> converterEngines;
    // The YouTube window (browse.py) runs beside the converter. A video picked
    // there while a conversion runs waits its turn; conversionsWaiting counts them.
    bool browsingYouTube = false;
    // The open YouTube window reads the app's messages (protocol 2), so Sign in goes to it.
    bool youtubeWindowSignsIn = false;
    size_t conversionsWaiting = 0;
    // The window asked for the MIDI folder picker; the panel opens it and
    // answers FolderAsked.
    bool folderWanted = false;
    // The queue waits for a sign-in or a folder: pausedReason is "signin" or
    // "folder".
    bool conversionsPaused = false;
    std::string pausedReason;
    // Loop: 0 off, 1 the song, 2 the section from loopStart to loopEnd, in
    // seconds of the song. The mode is kept; the section is the song's own.
    int loop = 0;
    double loopStart = 0, loopEnd = 0;
    // Keystrokes stop the song, not only the keys, while the game is behind
    // another window, and pick it up where it was (see VirtualPianoPlayer::hold_behind).
    bool holdBehind = true;
    // The song Next loads, named in the mini window: the one after the open song
    // in the list followed (SongsFollowed), or the one Shuffle Play has drawn. Empty when it would be
    // the open song again.
    std::filesystem::path upNext;
    // The lists the Files panel shows besides the folder, kept in library.json
    // beside config.json. Shared, as files is, so a publish copies no list.
    std::shared_ptr<const LibraryLists> library = std::make_shared<const LibraryLists>();
    // The list the Files panel shows, which Previous, Next and shuffle follow
    // when it is the favourites or a playlist: a ListId or a playlist.
    int openList = kFolderList;
    // How many octaves each note sounds in, 1 to 5: itself, then an octave
    // above, below, two above, two below. A double off the keys is dropped.
    int octaves = 1;
    // Set at start when a saved file could not be read, was kept aside as
    // .damaged and was started again; the status bar shows it once.
    std::string resetNotice;
    // An output that went away, or is not back since the last session, waiting
    // for a scan to list it again: its id and the name it was listed under.
    // Empty while none waits. On the MIDI target nothing is typed meanwhile.
    std::wstring outputWaiting;
    std::string outputWaitingName;
    // An input waiting the same way, which reopens as it was left.
    std::wstring liveWaiting;
    std::string liveWaitingName;
    std::string ActiveVelocityName() const {
        return comparingCurve ? previousPreset.name + (VelocityEdited(previousCurve) ? " (edited)" : "") : VelocityName(curves, curve);
    }
};

class ShellEngine {
public:
    enum class Action { Scan, Load, Play, Stop, Mute, Solo, SoloPiano, UnmuteAll, Velocity, Sustain,
                        Pause, TogglePlayPause, Restart, Back10, Forward10, Seek, Speed, Transpose, Remap,
                        LiveScan, LiveOpen, LiveActive, LiveChannel, OutputTarget, OutputScan, OutputOpen,
                        // key is the new name; an open port is made again under it.
                        OutputPortName,
                        CopySheet,
                        // Writes midi-converter's editor page to the temp
                        // folder for the panel to open in a browser.
                        OpenSheetEditor,
                        // Writes the enabled sheet outputs (image, text, page)
                        // under the sheets folder.
                        SaveSheetFiles,
                        CurveSelect, CurveAdjust, CurveEdit, CurveUndo, CurveRedo, CurveCompare, CurveNew,
                        // track is the index of one of the user's own curves;
                        // the built-ins are never deleted.
                        CurveDelete,
                        CurveDuplicate, CurveRename, SustainCutoff, VelocityModifier,
                        WootingTriggerThreshold, WootingShiftAmount, WootingVelocitySensitivity, WootingMinVelocity,
                        // track is the pedal (sustain, sostenuto, soft), amount
                        // its key's set 1 scancode or 0 to unbind. A key another
                        // pedal has is moved.
                        WootingPedalKey,
                        // key is the name of the key held for the sostenuto
                        // pedal. A hotkey on that key is unbound.
                        SostenutoKey,
                        EightyEightKeys,
                        AutoVolumeScan, AutoVolumeCalibrate, AutoVolumeOff, AutoVolumeCancel, ClearLog,
                        PlayCountdown, PlaybackDelay, AcknowledgeTyping,
                        Performer, Shuffle, Previous, Next, SortFiles, MidiConnect, OutRange, SeekStep, SpeedMin, SpeedMax,
                        // value is the setting. Saves the config key and
                        // reloads the open file so the track list matches.
                        DetectDrums, AutoTranspose,
                        // value is the panel's Solo Piano on load, for the loads
                        // the engine starts itself (Previous, Next, shuffle, reloads).
                        AutoSolo,
                        // SheetsFolder/SheetStylePage: path, empty to clear.
                        // SheetFiles: key names the output (image, text, page),
                        // value enables it. SaveLibrarySheets exports the whole
                        // list on a worker thread; SheetBatchProgress is its
                        // report (track done, amount failed, key status, value finished).
                        // KeepSheetStyle: key is the style the open editor page sent,
                        // kept for the sheets drawn with no style page.
                        SheetsFolder, SheetStylePage, SheetFiles, SaveLibrarySheets, SheetBatchCancel, SheetBatchProgress, KeepSheetStyle,
                        // ConvertAudio: path is an audio file or key a URL.
                        // ConvertProgress comes from the converter thread: key is
                        // the text, track an audio_to_midi::Status::Kind.
                        ConvertAudio, ConvertCancel, ConvertProgress,
                        // Opens tools/mp3-to-midi/signin.py; ConvertCancel closes it.
                        YouTubeSignIn,
                        // Opens tools/mp3-to-midi/browse.py, or brings it to the
                        // front; key is words to search for, if any.
                        // BrowseProgress is its report, as ConvertProgress is.
                        YouTubeBrowse, BrowseProgress,
                        // Runs the add-on's setup.ps1; ConvertCancel stops it.
                        ConverterSetUp,
                        // track is the hotkey index (kHotkeyFields, then the
                        // performer's), key the new name or empty to unbind.
                        // A key bound elsewhere is moved and the other unbound.
                        Hotkey,
                        // Performer: value is the section switch. PerformerPreset:
                        // track is the preset index, or the number of presets for
                        // Custom. PerformerValue: key is the control id, amount its
                        // value (negative returns a slider to its estimate).
                        // Trigger: track is the trigger index. PerformerPin: value
                        // pins the open song to its own Player row.
                        PerformerPreset, PerformerValue, Trigger, PerformerPin,
                        // track indexes kGameKeyMaps; replaces every bind in both layouts.
                        GameKeyMap,
                        // Loop: track is the mode (EngineSnapshot::loop); a
                        // section starts from the position to four bars on.
                        // LoopStart, LoopEnd: amount is the section's edge in seconds.
                        Loop, LoopStart, LoopEnd,
                        // value is the switch.
                        HoldBehind,
                        // track indexes the performer's actions; played while the song plays.
                        PerformerAction,
                        // Favourite: path is the song, value its star. OpenList:
                        // amount is the ListId or playlist the panel shows.
                        Favourite, OpenList,
                        // NewPlaylist: key is the name, path a first song if any,
                        // and value opens it. RenamePlaylist and DeletePlaylist:
                        // amount is the playlist, key the new name.
                        NewPlaylist, RenamePlaylist, DeletePlaylist,
                        // amount is a playlist or kQueueList, path the song.
                        // MoveInList: track is the row it goes before.
                        AddToList, RemoveFromList, MoveInList,
                        // path is a song, sent to the Recycle Bin and taken out of every list.
                        // SongRecycled is the report from the thread that sends it.
                        DeleteSong, SongRecycled,
                        // amount is the octaves each note sounds in, 1 to 5; saved.
                        Octaves,
                        // From the thread reading song lengths: files are the songs
                        // read, each with its size, time and seconds; value marks
                        // the last batch of the run numbered generation.
                        Lengths,
                        // key is the skin as the YouTube window's theme message.
                        BrowseTheme,
                        // amount is the converter's CPU limit, 25 to 100.
                        ConverterCpu,
                        // The panel opened the folder picker folderWanted asked for.
                        FolderAsked,
                        // key is the converter engine to switch to: "cuda13",
                        // "cuda12" or "directml". setup.ps1 installs it.
                        ConverterEngine };
    struct Command {
        Action action;
        std::filesystem::path path;
        uint64_t generation = 0;
        size_t track = 0;
        bool value = false;
        double amount = 0;
        std::string key;
        std::wstring device;
        std::vector<VelocityPoint> anchors;
        GameWindow window;
        // Scan: what the folder watcher's own walk found, so the worker does not
        // walk again. Lengths: the songs read.
        std::shared_ptr<std::vector<MidiEntry>> files;
        // Scan: the folders that walk found, for the YouTube window's Save to.
        std::shared_ptr<std::vector<std::string>> folders;
        // A reopen a device scan sends for a device that came back. It runs
        // whenever a device comes or goes, so it cancels no calibration, and
        // when it fails the song plays on and the device stays saved.
        bool automatic = false;
    };
    explicit ShellEngine(std::filesystem::path config, std::shared_ptr<AutoVolumeHost> volumeHost = {},
                         bool requireTypingAcknowledgement = false, ConnectFactory connectFactory = {});
    ~ShellEngine();
    void Send(Command command);
    std::shared_ptr<const EngineSnapshot> Snapshot() const;
    // HWND posted WM_NULL on each publish, and on a moved position while it
    // is shown; the shell renders on demand.
    void SetWakeWindow(void* window);
    // Key-state probe for the Hold and Tap keys (GetAsyncKeyState by default).
    // Set before the first command.
    void SetKeyProbe(std::function<bool(int)> probe);
    // Add-on folder and signing key (an addon::PublicKey); defaults to the
    // addons folder beside the exe and kOwnerKey. Call before constructing an engine.
    static void SetAddons(std::filesystem::path folder, const std::array<std::uint8_t, 72>& key);
    // What sends files to the Recycle Bin, true when every one went; the bin
    // itself by default. The tests replace it so a run leaves the real bin
    // alone. Call before constructing an engine.
    using Recycler = std::function<bool(const std::vector<std::filesystem::path>&)>;
    static void SetRecycler(Recycler recycle);
private:
    std::function<bool(int)> keyProbe_ = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    std::mutex keyProbeMutex_;
    void Run(std::stop_token stop);
    void Publish(const EngineSnapshot& state);
    void PublishPosition(const EngineSnapshot& state);
    std::filesystem::path config_;
    std::shared_ptr<AutoVolumeHost> volumeHost_;
    bool requireTypingAcknowledgement_;
    ConnectFactory connectFactory_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Command> commands_;
    mutable std::shared_ptr<const EngineSnapshot> snapshot_ = std::make_shared<const EngineSnapshot>();
    std::atomic<void*> wakeWindow_{nullptr};
    std::jthread worker_; // Last member: every dependency is initialized before Run.
};

std::string Utf8(const std::filesystem::path& path);
// The song's length in seconds, to its last note, through its tempo changes
// as playback times them; below 0 when the file cannot be read.
double SongSeconds(const std::filesystem::path& song);
// Where sheet files go when no sheets folder has been chosen.
std::filesystem::path DefaultSheetsFolder(const std::filesystem::path& midiFolder);
std::string NoteName(int note);
}
