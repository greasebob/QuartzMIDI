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
    // Hidden while the shownWith control is 0, or when the estimate for this
    // song is negative (noEstimate).
    std::string shownWith;
    bool noEstimate = false;
    std::string estimateName;   // slider: label of the add-on's estimate
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
struct PerformerSection {
    bool loaded = false, on = false;
    std::string name, tag, config;
    std::string presetId, presetField;
    std::vector<std::string> presets;
    std::vector<std::map<std::string, double>> presetValues;
    int preset = 0;
    std::vector<PerformerControl> controls;
    std::vector<PerformerTrigger> triggers;
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
        return !with || with->value != 0;
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
    velocity_telemetry::Snapshot playedVelocities;
    double wootingTriggerThreshold = 0.25;
    int wootingShiftAmount = 1;
    double wootingVelocityScale = 2.0;
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
    std::string ActiveVelocityName() const {
        return comparingCurve ? previousPreset.name + (VelocityEdited(previousCurve) ? " (edited)" : "") : VelocityName(curves, curve);
    }
};

class ShellEngine {
public:
    enum class Action { Scan, Load, Play, Stop, Mute, Solo, SoloPiano, UnmuteAll, Velocity, Sustain,
                        Pause, TogglePlayPause, Restart, Back10, Forward10, Seek, Speed, Transpose, Remap,
                        LiveScan, LiveOpen, LiveActive, LiveChannel, OutputTarget, OutputScan, OutputOpen,
                        CopySheet,
                        // Writes midi-converter's editor page to the temp
                        // folder for the panel to open in a browser.
                        OpenSheetEditor,
                        // Writes the enabled sheet outputs (image, text, page)
                        // under the sheets folder.
                        SaveSheetFiles,
                        CurveSelect, CurveAdjust, CurveEdit, CurveUndo, CurveRedo, CurveCompare, CurveNew,
                        CurveDuplicate, CurveRename, SustainCutoff, VelocityModifier,
                        WootingTriggerThreshold, WootingShiftAmount, WootingVelocityScale, EightyEightKeys,
                        AutoVolumeScan, AutoVolumeCalibrate, AutoVolumeOff, AutoVolumeCancel, ClearLog,
                        PlayCountdown, PlaybackDelay, AcknowledgeTyping,
                        Performer, Shuffle, Previous, Next, SortFiles, MidiConnect, OutRange, SeekStep, SpeedMin, SpeedMax,
                        // value is the setting. Saves the config key and
                        // reloads the open file so the track list matches.
                        DetectDrums, AutoTranspose,
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
                        GameKeyMap };
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
}
