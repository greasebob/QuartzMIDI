#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Fonts.hpp"
#include "ShellEngine.hpp"
#include "HelpModel.hpp"
#include "ThemeModel.hpp"
#include "InputLatency.hpp"
#include <windows.h>

namespace shell {
// Hooks for the Open buttons' modal file dialogs, so a render test can click
// the button and check the click arrived without a dialog on screen.
extern std::function<std::filesystem::path(HWND)> PickMidiFile;
// Same for theme files; with `save`, picks where to write a theme named `name`.
extern std::function<std::filesystem::path(HWND, bool save, const std::string& name)> PickThemeFile;

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
    // Media play/previous/next/stop keys act as hotkeys too; off leaves them to
    // the media player.
    bool mediaKeys = true;
    // Not persisted: the window starts closed every run.
    bool keyMappingOpen = false;
    bool alwaysOnTop = false;
    int opacity = 100;
    // Percentage of CPU a conversion may use: 25, 50, 75 or 100.
    int converterCpu = 75;
    // Browse the MIDI Files list by sub-folder; off lists every file flat.
    bool folders = true;
    std::filesystem::path folder;
    // Full window position and width in screen pixels; width 0 means none saved.
    // The height is derived from which panels are open.
    int windowX = 0, windowY = 0, windowWidth = 0;
    // Restore mini mode at startup. The shell switches after placing the full
    // window, because mini records the full window's position to return to.
    bool startMini = false;
    // The tour plays once per build that has one. Defaults to true so a Panels
    // that loaded no settings (as in tests) never plays it.
    bool tourSeen = true;
    // Build Help last showed its what's-new view for; a different build shows it once more.
    std::string helpBuild;
};
class Panels {
public:
    Preferences preferences;
    // Persisted alongside the preferences as themes.json.
    ThemeStore themes;
    skin::Skin ActiveSkin() const { return themes.Active(preferences.theme).Shown(preferences.dark); }
    // Class for a separate OS window. It must carry TopMost when always-on-top is
    // set, or it opens behind the main window and cannot be raised.
    ImGuiWindowClass OwnWindowClass() const {
        ImGuiWindowClass windowClass;
        windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge | (preferences.alwaysOnTop ? ImGuiViewportFlags_TopMost : 0);
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
    bool velocityExpanded = false;
    bool tracksExpanded = true;
    bool miniMode = false;
    bool miniAutoplay = false;
    bool autoVolumeOpen = false;
    bool logOpen = false;
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
    ~Panels();
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
    void Draw(HWND hwnd, const Fonts& fonts, const skin::Skin& design,
              float dpi, ShellEngine& engine);
    // True while something changes with time alone (the timing readout polls every
    // 200 ms), so the on-demand renderer keeps drawing.
    bool Animating() const { return measuring_ || hotkeyCapture >= 0 || (tourStop >= 0 && (tourGlide_ < 1 || tourFade_ < 1)); }
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
    int tourPrevious_ = -1;
    // ImGui's modal dim alpha, saved while the tour draws its own dim with a cutout.
    float tourDim_ = 0;
    bool tourChecked_ = false;
    std::filesystem::path themesPath_;
    mutable std::string savedPreferences_;
    void SaveThemes() const;
    void DrawThemeEditor(const Fonts&, const skin::Skin&, float);
    bool themeEditorWasOpen_ = false;
    // Theme editor Size while its slider is held, or 0.
    float themeSizeDrag_ = 0;
    char themeName_[64]{};
    std::string themeNameFor_;
    char search_[256]{};
    FileSort fileSort_ = FileSort::Name;
    bool descendingFiles_ = false;
    std::shared_ptr<const std::vector<MidiEntry>> filteredFiles_;
    std::string filteredQuery_;
    bool filteredFolders_ = true;
    std::vector<size_t> fileFilter_;
    // Current folder, as a prefix of MidiEntry::name; search covers it and its
    // sub-folders.
    std::string browse_;
    // Set by Open file location; the list scrolls to it once its folder is shown.
    std::filesystem::path revealFile_;
    std::vector<std::string> folderRows_;
    void DrawKeyMapping(const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine);
    int selectedNote_ = -1;
    bool mappingArmed_ = false;
    bool mappingLayout88_ = true;
    float mappingDpi_ = 0;
    float seekPosition_ = 0;
    bool seeking_ = false;
    uint64_t seekGeneration_ = 0;
    uint64_t handledSheetRevision_ = 0;
    uint64_t sheetStatusGeneration_ = 0;
    std::string sheetStatus_;
    std::string sheetNote_;
    bool sheetPending_ = false;
    char convertLink_[1024]{};
    bool convertPlaylist_ = false;
    // Which converter install (CPU or NVIDIA) is running.
    bool installNvidia_ = false;
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
    bool aboutRevealed_ = false;
    bool volumeWasOpen_ = false;
    GameWindow volumeWindow_;
    void SettingsControl(const Fonts&, const skin::Skin&, float, ShellEngine&, ImVec2, float);
    void DrawMini(HWND, const Fonts&, const skin::Skin&, float, ShellEngine&, ImVec2, ImVec2);
    void DrawStatus(const Fonts&, const skin::Skin&, float, const EngineSnapshot&, ImVec2, float, float);
    int nameOperation_ = 0;
    char curveName_[128]{};
    bool focusCurveName_ = false;
    uint64_t nameRevision_ = 0;
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
    std::array<float, 3> wootingPreview_{0.5f, 12.f, 5.f};
    std::array<bool, 3> wootingEditing_{};
    std::array<bool, 3> wootingPending_{};
    bool scannedLive_ = false;
    bool scannedOutput_ = false;
    bool measuring_ = false;
    int timingSource_ = 0;
    input_latency::Collector timing_;
    input_latency::Summary timingSummary_;
    double nextTimingPoll_ = 0;
};
}
