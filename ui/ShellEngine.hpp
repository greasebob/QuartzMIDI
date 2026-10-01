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
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace shell {

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
    bool remembers = false;     // switch: sliders are stored per song
    bool holds = false;         // switch: a tap key holds what it played
    int trigger = -1;           // owning trigger, or -1 for the section
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
    int preset = 0;
    std::vector<PerformerControl> controls;
    std::vector<PerformerTrigger> triggers;
    std::vector<PerformerAction> actions;
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

// A key bound to one song: pressing it anywhere loads and plays the song.
struct SongHotkey {
    std::filesystem::path song;
    std::string key;
    bool operator==(const SongHotkey&) const = default;
};

struct EngineSnapshot {
    std::shared_ptr<const std::vector<MidiEntry>> files = std::make_shared<const std::vector<MidiEntry>>();
    std::vector<TrackRow> rows;
    std::filesystem::path folder;
    std::filesystem::path loaded;
    std::string error;
    std::shared_ptr<const std::string> log = std::make_shared<const std::string>();
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
    // Store performer choices and sliders per song; a performer switch can clear it.
    bool rememberPerSong = true;
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
    int volumeInitial = 100;
    double position = 0;
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
    // Notes struck in each of kDensitySlices slices of the open song, drawn in
    // the mini window's seek bar; empty with no song open.
    std::shared_ptr<const std::vector<uint16_t>> density = std::make_shared<const std::vector<uint16_t>>();
    // The lists the Files panel shows besides the folder, kept in library.json
    // beside config.json. Shared, as files is, so a publish copies no list.
    std::shared_ptr<const LibraryLists> library = std::make_shared<const LibraryLists>();
    // The list the Files panel shows, which Previous, Next and shuffle follow
    // when it is the favourites or a playlist: a ListId or a playlist.
    int openList = kFolderList;
    // A scan for MIDI files: the folder it reads and how many new files it has
    // found so far, then, once it ends, those files, each named by its folder.
    bool scanning = false;
    std::string scanFolder;
    size_t scanFound = 0;
    std::shared_ptr<const std::vector<MidiEntry>> scanResults = std::make_shared<const std::vector<MidiEntry>>();
    uint64_t scanRevision = 0;
    // Songs with a key of their own, in the order they were bound. A song
    // whose file is gone keeps its key. Changes bump hotkeyRevision.
    std::vector<SongHotkey> songHotkeys;
    // Applied when a file loads: Transpose moves by the fewest semitones that
    // put every note on the layout's keys, or the most notes when none can.
    bool fitToKeys = false;
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
                        SheetsFolder, SheetStylePage, SheetFiles, SaveLibrarySheets, SheetBatchCancel, SheetBatchProgress,
                        // ConvertAudio: path is an audio file or key a URL.
                        // ConvertProgress comes from the converter thread: key is
                        // the text, track an audio_to_midi::Status::Kind.
                        ConvertAudio, ConvertCancel, ConvertProgress,
                        // Opens tools/mp3-to-midi/signin.py; ConvertCancel closes it.
                        YouTubeSignIn,
                        // Runs the add-on's setup.ps1; ConvertCancel stops it.
                        ConverterSetUp,
                        // track is the hotkey index (kHotkeyFields, then the
                        // performer's), key the new name or empty to unbind.
                        // A key bound elsewhere is moved and the other unbound.
                        Hotkey,
                        // Performer: value is the section switch. PerformerPreset:
                        // track is the preset index. PerformerValue: key is the
                        // control id, amount its value (negative returns a slider
                        // to its estimate). Trigger: track is the trigger index.
                        PerformerPreset, PerformerValue, Trigger,
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
                        // Trash: path is a song, moved into the MIDI folder's Trash
                        // (kTrashFolder). Restore and DeleteForever: path is the file
                        // in the Trash; deleting sends it to the Recycle Bin.
                        Trash, Restore, DeleteForever,
                        // ScanDrives looks below paths for MIDI files not in the
                        // library, on threads of its own; ScanCancel stops it and
                        // ScanProgress is its report (generation the scan, key the
                        // folder read, track the count found, value finished, files
                        // what it found). AddScanned moves the paths chosen into the
                        // library folder, and PutBack returns paths moved so, or all
                        // of them when there are none, to where they came from.
                        ScanDrives, ScanCancel, ScanProgress, AddScanned, PutBack,
                        // Stop, then every note key, modifier and the sustain key
                        // released, and on the MIDI port every note and pedal.
                        Panic,
                        // amount is how many 0.1 steps to move Speed; applied as Speed.
                        SpeedStep,
                        // SongHotkey: path is the song, key the new name or empty
                        // to unbind; a key bound elsewhere is moved. PlaySong loads
                        // the song at path and plays it; one that is gone does nothing.
                        SongHotkey, PlaySong,
                        // value is the setting; saved, and reloads the open file as AutoTranspose does.
                        FitToKeys,
                        // amount is the octaves each note sounds in, 1 to 5; saved.
                        Octaves,
                        // Sends every song in the Trash to the Recycle Bin.
                        EmptyTrash };
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
        // Scan: what the folder watcher's own walk found, so the worker does not walk again.
        std::shared_ptr<std::vector<MidiEntry>> files;
        // A reopen a device scan sends for a device that came back. It runs
        // whenever a device comes or goes, so it cancels no calibration, and
        // when it fails the song plays on and the device stays saved.
        bool automatic = false;
        // ScanDrives: where to look. AddScanned and PutBack: the files.
        std::vector<std::filesystem::path> paths;
        // ScanDrives: folders it passes over, with all below them.
        std::vector<std::filesystem::path> ignored;
    };
    explicit ShellEngine(std::filesystem::path config, std::shared_ptr<AutoVolumeHost> volumeHost = {},
                         bool requireTypingAcknowledgement = false, ConnectFactory connectFactory = {});
    ~ShellEngine();
    void Send(Command command);
    std::shared_ptr<const EngineSnapshot> Snapshot() const;
    // HWND posted WM_NULL on each publish; the shell renders on demand.
    void SetWakeWindow(void* window);
    // Key-state probe for the Hold and Tap keys (GetAsyncKeyState by default).
    // Set before the first command.
    void SetKeyProbe(std::function<bool(int)> probe);
    // Add-on folder and signing key (an addon::PublicKey); defaults to the
    // addons folder beside the exe and kOwnerKey. Call before constructing an engine.
    static void SetAddons(std::filesystem::path folder, const std::array<std::uint8_t, 72>& key);
private:
    std::function<bool(int)> keyProbe_ = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    std::mutex keyProbeMutex_;
    void Run(std::stop_token stop);
    void Publish(const EngineSnapshot& state);
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
// Where sheet files go when no sheets folder has been chosen.
std::filesystem::path DefaultSheetsFolder(const std::filesystem::path& midiFolder);
std::string NoteName(int note);
// The user's Downloads, Desktop, Documents, Music and OneDrive folders that
// exist, once each: where Scan for MIDI files looks unless told otherwise.
std::vector<std::filesystem::path> UserScanFolders();
}
