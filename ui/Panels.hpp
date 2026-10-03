#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Fonts.hpp"
#include "ShellEngine.hpp"
#include "HelpModel.hpp"
#include "ThemeModel.hpp"
#include "UpdateCheck.hpp"
#include <windows.h>

namespace shell {
// Hooks for the Open buttons' modal file dialogs, so a render test can click
// the button and check the click arrived without a dialog on screen.
extern std::function<std::filesystem::path(HWND)> PickMidiFile;
// Same for theme files; with `save`, picks where to write a theme named `name`.
extern std::function<std::filesystem::path(HWND, bool save, const std::string& name)> PickThemeFile;
// True while a control's 160 ms transition is still moving; part of Animating.
bool MotionPending();
// Every control's transition jumps to its end on the current frame. Called
// after NewFrame by a render test, so each capture starts at rest.
void SettleMotion();

// Extra layout size a theme's shape adds over the built-ins, before DPI scaling.
struct LayoutGrowth { float control, window, panel, text, meta; };
LayoutGrowth GrowthOf(const skin::Skin& design);
float LeftColumnFloor(const skin::Skin& design);
float RightColumnFloor(const skin::Skin& design);

struct Preferences {
    // Theme id (built-in or user) and variant. A legacy "skin" index in the
    // settings file maps to the same pair.
    std::string theme = "blue";
    bool dark = false;
    bool autoSolo = false;
    // While playing, a hotkey on F4 is also taken with Alt held, so an F4 that
    // lands on a velocity tap's Alt reaches no window as Alt+F4.
    bool blockAltF4 = true;
    // Not persisted: the window starts closed every run.
    bool keyMappingOpen = false;
    bool alwaysOnTop = false;
    int opacity = 100;
    // Percentage of CPU a conversion may use: 25, 50, 75 or 100.
    int converterCpu = 75;
    // Browse the MIDI Files list by sub-folder; off lists every file flat.
    bool folders = true;
    std::filesystem::path folder;
    // The song open when the app last closed; the next start opens it again,
    // paused at its beginning.
    std::filesystem::path lastSong;
    // Full window position and width in screen pixels; width 0 means none saved.
    // The height is derived from which panels are open.
    int windowX = 0, windowY = 0, windowWidth = 0;
    // Any height the full window was dragged beyond that, in dp, and whether it
    // was maximized.
    float windowExtra = 0;
    bool maximized = false;
    // Mini window position in screen pixels; miniSaved is false until mini has
    // been shown.
    int miniX = 0, miniY = 0;
    bool miniSaved = false;
    // Restore mini mode at startup. The shell switches after placing the full
    // window, because mini records the full window's position to return to.
    bool startMini = false;
    // No taskbar button and not in Alt+Tab. Applied only while the show/hide
    // key works, and turned off when that key is unbound.
    bool hideFromTaskbar = false;
    // The tour plays once per build that has one. Defaults to true so a Panels
    // that loaded no settings (as in tests) never plays it.
    bool tourSeen = true;
    // Build Help last showed its what's-new view for; a different build shows it once more.
    std::string helpBuild;
    // Every window of the app is left out of screenshots, recordings and screen
    // sharing; the shell applies it where Windows can (captureExclusionOffered).
    bool hideFromCapture = false;
    // Ask GitHub once at start for a newer release, offered in the status bar.
    bool checkForUpdates = true;
};
class Panels {
public:
    Preferences preferences;
    // Makes `path` the MIDI Files list's folder, as Choose MIDI folder does.
    void OpenFolder(const std::filesystem::path& path, ShellEngine& engine);
    // Persisted alongside the preferences as themes.json.
    ThemeStore themes;
    // Adds a theme file and selects it, as Import does.
    void ImportTheme(const std::filesystem::path& path);
    skin::Skin ActiveSkin() const { return themes.Active(preferences.theme).Shown(preferences.dark); }
    // Whether the main window stays above others: with Always on top, and always
    // in mini, which a borderless game would otherwise cover. Leaving mini goes
    // back to the switch, which mini never changes.
    bool Topmost() const { return preferences.alwaysOnTop || miniMode; }
    // Class for a separate OS window. It must carry TopMost while the main window
    // is topmost, or it opens behind the main window and cannot be raised.
    ImGuiWindowClass OwnWindowClass() const {
        ImGuiWindowClass windowClass;
        windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge | (Topmost() ? ImGuiViewportFlags_TopMost : 0);
        return windowClass;
    }
    bool themeEditorOpen = false;
    bool stopHotkeyAvailable = false;
    // Keycap label per hotkey (empty when unbound) and whether it registered;
    // the shell sets both on each registration.
    std::array<std::string, kHotkeys> transportKeys{"F1", "F2", "F3", "F4"};
    std::array<bool, kHotkeys> transportKeysAvailable{};
    // Hotkey being rebound in Settings, or -1. The shell unregisters the hotkeys,
    // polls for the key and sends the rebind.
    int hotkeyCapture = -1;
    // Set when the greyed Hide from taskbar switch arms the Show/hide window
    // capture, so the key it takes turns the switch on. The shell clears it
    // once that capture ends.
    bool hideOnShowHideKey = false;
    // A key taken that way turns the switch on only once it registers; a key
    // another program holds leaves it off. Cleared when Settings closes.
    bool hideOnceShowHideWorks = false;
    // ImGui time the armed capture last passed over a key it refuses, such as a
    // note key, or -1; its button flashes in the warning colour for a moment.
    double captureRefusedAt = -1;
    static constexpr double kRefusedFlash = .4;
    // Wooting pedal whose key is being learnt, or -1. The shell listens to the
    // Wooting for the key and sends it.
    int wootingPedalCapture = -1;
    // Set by the shell where Windows can leave a window out of capture; without
    // it, Settings has no Hide from screen capture.
    bool captureExclusionOffered = false;
    // A newer release the shell's check found, offered in a corner card while
    // Check for updates is on.
    AvailableUpdate update;
    bool velocityExpanded = false;
    bool tracksExpanded = true;
    bool miniMode = false;
    bool miniAutoplay = false;
    bool autoVolumeOpen = false;
    bool logOpen = false;
    // The log's rows wrapped to the panel, rebuilt when the log text, the width
    // or the font size changes, so a full log isn't re-measured every frame.
    std::shared_ptr<const std::string> logWrappedText;
    float logWrappedWidth = 0, logWrappedFont = 0;
    std::vector<std::pair<size_t, size_t>> logRows;
    // One-frame request to open the Convert audio popover, set by the Files
    // header button or a render test; Draw clears it.
    bool openConvert = false;
    // One-frame request to open the library save confirmation.
    bool openLibrarySave = false;
    // One-frame request to open Help; helpAdded selects its what's-new filter.
    bool openHelp = false;
    bool helpAdded = false;
    // Help folder to expand, as an index into kHelpFolders, or -1 (render scenarios).
    int revealHelpFolder = -1;
    // Current tour stop, or -1. StartTour jumps to `stop` without animating.
    int tourStop = -1;
    // Space between the tour text and the buttons, for the render tests.
    float tourTextRoom = 0;
    void StartTour(int stop = 0);
    // Ends the tour and marks it seen.
    void EndTour();
    void SearchHelp(const char* text) { strncpy_s(helpSearch_, text, _TRUNCATE); }
    // Render-scenario hooks that scroll the open Settings popover below the fold:
    // the drum and auto-transpose switches,
    bool revealSettingsSwitches = false;
    // the Hotkeys section,
    bool revealSettingsHotkeys = false;
    // the end of the tap key row,
    bool revealSettingsTapKeys = false;
    // and the About section, expanded.
    bool revealSettingsAbout = false;
    ImVec2 DesiredSize() const;
    // Full window height for the panels currently open; changes when Tracks or
    // Velocity Response is toggled.
    float FullHeight() const;
    // Updates whether a performer row is shown, which affects both sizes. Draw
    // calls it; a test that sizes a window before drawing must call it first.
    void SyncLayout(const EngineSnapshot& state);
    // Smallest full window: Files at 240 and the right column at its floor (600
    // for built-in themes). Mini has a single size, DesiredSize.
    ImVec2 MinimumSize() const;
    void LoadPreferences(const std::filesystem::path& path);
    // Writes only when the serialized preferences changed, so the shell can call
    // it every second and an unclean exit still keeps settings. Themes are saved
    // only when `exiting`; the theme editor saves its own.
    void SavePreferences(const std::filesystem::path& path, bool exiting = true) const;
    // A failure on the panel side: `result` shows in the status bar until the next
    // click, and the log gets `detail`, or `result` when there is none.
    void ReportError(const std::string& result, const std::string& detail = {}) const;
    void Draw(HWND hwnd, const Fonts& fonts, const skin::Skin& design,
              float dpi, ShellEngine& engine);
    // True while something changes with time alone (a control's transition
    // moves), so the on-demand renderer keeps drawing.
    bool Animating() const { return hotkeyCapture >= 0 || wootingPedalCapture >= 0 || convertBarDrawn_ || (tourStop >= 0 && (tourGlide_ < 1 || tourFade_ < 1 || tourText_ < 1 || tourTextStop_ != tourStop || tourClosing_)) || MotionPending(); }
private:
    float HeightGrowth(bool tracksOpen, bool velocityOpen) const;
    void DrawHelp(const Fonts&, const skin::Skin&, float, const EngineSnapshot&);
    void DrawTour(const Fonts&, const skin::Skin&, float, ImVec2, ImVec2);
    bool performerRow_ = false;
    char helpSearch_[128]{};
    int helpFolderRevealed_ = -1;
    // Screen rect of each tour stop's control, recorded as it draws, so the tour
    // tracks the layout at any size and DPI.
    std::array<TourRect, static_cast<size_t>(TourStop::Count)> tourRects_{};
    TourRect tourFrom_, tourShown_;
    float tourGlide_ = 1;
    int tourDrawn_ = -1;
    TourRect tourCardFrom_, tourCardShown_;
    float tourHeightFrom_ = 0, tourHeightShown_ = 0, tourFade_ = 1;
    // Skip or Done was pressed; the tour fades out, then ends.
    bool tourClosing_ = false;
    // The stop whose text the card shows, and its linear alpha (TourTextStep).
    int tourTextStop_ = -1;
    float tourText_ = 1;
    // ImGui's modal dim alpha, saved while the tour draws its own dim with a cutout.
    float tourDim_ = 0;
    bool tourChecked_ = false;
    std::filesystem::path themesPath_;
    mutable std::string savedPreferences_;
    // Preferences whose write last failed, so a retry reports only a new change.
    mutable std::string unsavedPreferences_;
    // Status bar text from ReportError, and the engine error last seen, so a
    // newer engine error replaces it.
    mutable std::string panelError_;
    std::string errorSeen_;
    // The engine's resetNotice has been shown.
    bool resetShown_ = false;
    void SaveThemes() const;
    void DrawThemeEditor(const Fonts&, const skin::Skin&, float);
    bool themeEditorWasOpen_ = false;
    // The copy of a built-in Customise made, as made, and that built-in's id;
    // closing the editor with the copy unchanged removes it.
    Theme customisedCopy_;
    std::string customisedFrom_;
    // Theme editor Size while its slider is held, or 0.
    float themeSizeDrag_ = 0;
    char themeName_[64]{};
    // The own MIDI port's name as typed; sent when the field is left.
    char portName_[128]{};
    std::string themeNameFor_;
    char search_[256]{};
    FileSort fileSort_ = FileSort::Name;
    bool descendingFiles_ = false;
    std::shared_ptr<const std::vector<MidiEntry>> filteredFiles_;
    std::string filteredQuery_;
    bool filteredFolders_ = true;
    std::shared_ptr<const LibraryLists> filteredLibrary_;
    int filteredList_ = kFolderList;
    // The playlist or queue shown, as its rows draw: a library file's entry, or
    // one named by its full path for a file outside the library.
    std::vector<MidiEntry> listEntries_;
    // Requests from the list menus, opened at the panel's level: name a new
    // playlist (with a first song when one is set), rename one, delete one.
    bool namePlaylist_ = false;
    int renamePlaylist_ = -1, deletePlaylist_ = -1;
    // The playlist being renamed (-1 for a new one) and the one asked about deleting.
    int renamingPlaylist_ = -1, deletingPlaylist_ = -1;
    std::filesystem::path playlistFirstSong_;
    char playlistName_[128]{};
    // The list menu, opened under a dragged song, closes when the drag ends.
    bool listsByDrag_ = false;
    std::vector<size_t> fileFilter_;
    // Current folder, as a prefix of MidiEntry::name; search covers it and its
    // sub-folders.
    std::string browse_;
    // Set by Open file location; the list scrolls to it once its folder is shown.
    std::filesystem::path revealFile_;
    // Loaded file the list last saw, and the file a row click loaded, so a song
    // that Next, a hotkey or shuffle loads is revealed once and a clicked one isn't.
    std::filesystem::path followedFile_, clickedFile_;
    std::vector<std::string> folderRows_;
    void DrawKeyMapping(const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine);
    int selectedNote_ = -1;
    bool mappingArmed_ = false;
    bool mappingLayout88_ = true;
    float mappingDpi_ = 0;
    float seekPosition_ = 0;
    bool seeking_ = false;
    uint64_t seekGeneration_ = 0;
    // The loop section handle held or awaiting the engine (see SectionHandles),
    // its time, and the load it belongs to.
    int loopHandle_ = -1;
    float loopHandleAt_ = 0;
    uint64_t loopHandleGeneration_ = 0;
    // True when the seek groove carries the loop's section; forgets a handle of an earlier load.
    bool LoopSection(const EngineSnapshot& state);
    // Sends the time of a handle SectionHandles let go (0 the start, 1 the end; -1 none).
    void SendLoopHandle(int released, ShellEngine& engine, const EngineSnapshot& state) const;
    // Load the Tracks table last scrolled to the top for.
    uint64_t tracksGeneration_ = 0;
    uint64_t handledSheetRevision_ = 0;
    uint64_t sheetStatusGeneration_ = 0;
    std::string sheetStatus_;
    std::string sheetNote_;
    bool sheetPending_ = false;
    char convertLink_[1024]{};
    bool convertPlaylist_ = false;
    // Whether the last frame drew the Convert popover's moving progress bar; a
    // conversion with the popover closed has nothing that moves on its own.
    bool convertBarDrawn_ = false;
    void DrawVelocity(const Fonts&, const skin::Skin&, float, ShellEngine&, ImVec2, ImVec2);
    void DrawSettings(const Fonts&, const skin::Skin&, float, ShellEngine&);
    void DrawMidiDevices(const Fonts&, const skin::Skin&, float, ShellEngine&);
    void DrawConvert(HWND, const Fonts&, const skin::Skin&, float, ShellEngine&);
    void DrawAutoVolume(const Fonts&, const skin::Skin&, float, ShellEngine&);
    void DrawLog(HWND, const Fonts&, const skin::Skin&, float, ShellEngine&);
    // Hotkey legend: a keycap per key followed by its action. Measures only when
    // draw is null. Callers push the meta font first. tapCaps is how many tap keys
    // get caps; the rest collapse to "+N".
    float DrawTransportHints(ImDrawList* draw, const skin::Skin& s, float dpi, ImVec2 origin, const EngineSnapshot& state,
                             size_t tapCaps = kAddonHotkeys, bool performerOnly = false) const;
    // One frame: scroll Settings to the Show/hide window row just armed.
    bool revealShowHideKey_ = false;
    bool volumeWasOpen_ = false;
    GameWindow volumeWindow_;
    void SettingsControl(const Fonts&, const skin::Skin&, float, ShellEngine&, ImVec2, float);
    void DrawMini(HWND, const Fonts&, const skin::Skin&, float, ShellEngine&, ImVec2, ImVec2);
    void DrawStatus(const Fonts&, const skin::Skin&, float, const EngineSnapshot&, ImVec2, float, float);
    // A newer release as a card in the window's bottom-right corner, above the
    // status bar whose top is `bottom`, until Later or Update closes it.
    void DrawUpdateToast(const Fonts&, const skin::Skin&, float, ImVec2, ImVec2, float bottom);
    bool updateDismissed_ = false;
    float updateFade_ = 0;
    // The position and length right-aligned at `right`, with a looped section's
    // bounds before them while there is room after `left`.
    void TimeReadout(ImDrawList*, const EngineSnapshot&, bool section, float left, float right, float y, const skin::Skin&) const;
    // A looped section's start and end, following a handle while it is dragged.
    std::string SectionBounds(const EngineSnapshot&) const;
    // The armed capture's ring over the last item: the accent, or just after a
    // refused key the warning colour with a tint of it inside.
    void CaptureRing(ImDrawList* draw, const skin::Skin& s, float dpi) const;
    int nameOperation_ = 0;
    char curveName_[128]{};
    bool focusCurveName_ = false;
    // Curve the name field was opened for; choosing another closes it.
    size_t namePreset_ = 0;
    uint64_t editorRevision_ = UINT64_MAX;
    uint64_t listRevision_ = UINT64_MAX;
    uint64_t histogramRevision_ = UINT64_MAX;
    std::array<float, velocity_telemetry::kBuckets> histogramHeights_{};
    bool histogramVisible_ = false;
    VelocityEdit editor_;
    std::string editorError_;
    int curveTool_ = 0;
    int activeAnchor_ = -1;
    // Anchors as they were when the drag started, and the dragged anchor's index.
    std::vector<VelocityPoint> dragAnchors_;
    int dragIndex_ = -1;
    bool curveGesture_ = false;
    VelocityEdit curveGestureBase_;
    std::vector<VelocityPoint> freeDraw_;
    bool cutoffEditing_ = false;
    float cutoffPreview_ = 64;
    // Sent and not yet published: the preview stays shown until the engine
    // reports the value or an error.
    bool cutoffPending_ = false;
    std::string cutoffPendingError_;
    std::array<float, 4> wootingPreview_{0.25f, 1.f, 1.f, 1.f};
    std::array<bool, 4> wootingEditing_{};
    std::array<bool, 4> wootingPending_{};
    bool scannedLive_ = false;
    bool scannedOutput_ = false;
};
}
