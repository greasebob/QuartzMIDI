#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// Help content (FAQ entries grouped by folder) and the guided tour.
namespace shell {
// Current build. The "What's new" chip shows entries whose `build` matches.
inline constexpr const char* kHelpBuild = "1.0";

struct HelpEntry {
    const char* folder;
    const char* question;
    const char* answer;
    // Build that added the entry; empty for entries predating build tags.
    const char* build = "";
    // Shown only when an addons folder exists beside the exe.
    bool addonsFolder = false;
};

// Folders are organized by task.
inline constexpr const char* kHelpFolders[]{
    "Getting Started", "Playback", "Live Input", "Velocity and Output",
    "Conversion and Sheets", "Appearance and Controls", "Troubleshooting", "Add-ons"};

inline const std::vector<HelpEntry>& HelpEntries() {
    static const std::vector<HelpEntry> entries{
        {"Getting Started", "How do I play a song in my game?",
         "Load a song, switch to your game, then press the Play hotkey. QuartzMIDI types the notes into whichever window is active."},
        {"Getting Started", "How do I control a song from inside the game?",
         "Use the hotkeys shown beside the song's title. To change them, open Settings and go to Hotkeys."},
        {"Getting Started", "Should I use 61 or 88 keys?",
         "Choose the one that matches your game's piano. On 61 keys, turn on Fold out-of-range notes onto the keys in Settings."},

        {"Playback", "Why does Solo Piano leave a track playing, or mute a piano?",
         "Solo Piano goes by the instrument each track names. Open Tracks and use Mute or Solo to correct it."},
        {"Playback", "Where do Previous, Next and Shuffle look for songs?",
         "They stay in the folder of the song that is open.", "1.0"},

        {"Live Input", "Which MIDI input should I pick?",
         "Pick your keyboard by name. Use Kernel Streaming for the lowest delay, and switch to WinMM if the keyboard won't open."},
        {"Live Input", "What is Midi2Key?",
         "Turn on Midi2Key to play the game's piano from your MIDI keyboard."},
        {"Live Input", "What is MidiConnect?",
         "Turn on MidiConnect for a game piano that reads notes and velocity from the number pad."},
        {"Live Input", "Can I play from a Wooting keyboard?",
         "Yes: choose Wooting Analog as the MIDI input. The faster you press a key, the louder the note."},

        {"Velocity and Output", "Why do I hear extra notes with Velocity on?",
         "Velocity is sent as Alt with a key, and some pianos play that key as a note. Turn Velocity off for those pianos."},
        {"Velocity and Output", "What does AutoVol do?",
         "AutoVol follows the song's loudness with the game's volume keys. Calibrate it with the game open so it starts from a known volume."},
        {"Velocity and Output", "What does Sustain cutoff change?",
         "It sets how far the pedal must be pressed to count as down."},
        {"Velocity and Output", "What is MIDI output for?",
         "Choose MIDI output to send the song, or your playing, to a MIDI port instead of typing keys."},

        {"Conversion and Sheets", "Should I install the CPU or the GPU converter?",
         "Install GPU only if your PC has an NVIDIA graphics card. Every other PC needs CPU.", "1.0"},
        {"Conversion and Sheets", "Why does the converter ask me to sign in?",
         "YouTube blocks some downloads unless you are signed in. Files from your PC never need it."},
        {"Conversion and Sheets", "Where do converted files go?",
         "They are saved to your MIDI folder and appear in the list when the conversion finishes."},
        {"Conversion and Sheets", "Where are my sheets saved?",
         "Open the Sheets menu and look under Save to."},

        {"Appearance and Controls", "Why is a hotkey greyed out?",
         "Another program is using that key. Close that program, or choose a different key in Settings under Hotkeys."},
        {"Appearance and Controls", "Can my keyboard's media keys control a song?",
         "Yes: turn on Media keys in Settings to use Play, Stop, Previous and Next.", "1.0"},
        {"Appearance and Controls", "Why is the light and dark button greyed out?",
         "The selected theme has only one palette. Open Customise in Settings and turn on Light and dark.", "1.0"},
        {"Appearance and Controls", "Can a theme change more than the colours?",
         "Yes: open Customise in Settings to adjust Corners, Spacing, Size and Shadow. Fine detail sets each measurement separately.", "1.0"},
        {"Appearance and Controls", "How do I share a theme?",
         "Select the theme in Settings and click Export to save it as a file. Click Import to add a theme someone sent you.", "1.0"},

        {"Troubleshooting", "Why did it type into my browser?",
         "QuartzMIDI types into whichever window is active, so switch to the game before you press Play. The lowest notes of 88 keys use Ctrl, which a browser treats as shortcuts."},
        {"Troubleshooting", "Why does nothing play in the game?",
         "The game must be the active window. Switch to it, then press the Play hotkey."},
        {"Troubleshooting", "How do I release a stuck key?",
         "Press Stop to release every key."},

        {"Add-ons", "What is the addons folder for?",
         "Each folder inside it is an add-on that loads when the app starts. Legit mode is a separate add-on: ask BobGrease on Discord.", "1.0", true},
        {"Add-ons", "Why didn't an add-on load?",
         "QuartzMIDI only loads add-ons signed for your copy of the app.", "1.0", true},
    };
    return entries;
}

inline std::string HelpLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Indices of entries to list, in order. The query matches question or answer
// case-insensitively; addedOnly keeps entries from `build`; add-on entries
// require addonsFolder.
inline std::vector<size_t> FilterHelp(const std::vector<HelpEntry>& entries, const std::string& query,
                                      bool addedOnly, const std::string& build, bool addonsFolder) {
    const std::string wanted = HelpLower(query);
    std::vector<size_t> found;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.addonsFolder && !addonsFolder) continue;
        if (addedOnly && build != entry.build) continue;
        if (!wanted.empty() && HelpLower(entry.question).find(wanted) == std::string::npos &&
            HelpLower(entry.answer).find(wanted) == std::string::npos) continue;
        found.push_back(i);
    }
    return found;
}

// True when the query should surface the tour button.
inline bool HelpFindsTour(const std::string& query) {
    const std::string wanted = HelpLower(query);
    return wanted.size() >= 2 && (wanted.find("tour") != std::string::npos || std::string_view("tour").find(wanted) != std::string_view::npos);
}

// True when `build` added at least one visible entry (the chip is shown only then).
inline bool HelpAdded(const std::vector<HelpEntry>& entries, const std::string& build, bool addonsFolder) {
    return !build.empty() && !FilterHelp(entries, {}, true, build, addonsFolder).empty();
}

// Tour stops, one sentence each; keep it to eight or fewer. Each stop's control
// records its rectangle when drawn.
enum class TourStop { Files, Play, Speed, Tracks, Pills, Device, Settings, Count };
struct TourText { const char* title; const char* text; };
inline constexpr TourText kTour[static_cast<int>(TourStop::Count)]{
    {"MIDI Files", "Choose your MIDI folder, then click a song to load it."},
    {"Play", "Switch to your game, then use these hotkeys to play, skip and stop."},
    {"Speed and Transpose", "Drag a slider to change the speed or the key, or click its value: left lowers it, right raises it, middle resets it."},
    {"Tracks", "Open Tracks to mute or solo a part, or use Solo Piano to keep only the piano."},
    {"Switches", "Turn Velocity, Sustain and 88 Keys on or off to match the piano in your game."},
    {"MIDI devices", "Choose a MIDI keyboard to play live, or send playback to a MIDI output."},
    {"Settings and Help", "Find every other option in Settings, and replay this tour from Help."},
};

struct TourRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float Width() const { return x1 - x0; }
    float Height() const { return y1 - y0; }
};

// Places the card below, above, right or left of the target (first that fits),
// clamped inside the window.
inline TourRect PlaceTourCard(const TourRect& target, float width, float height, const TourRect& window, float gap) {
    const auto inside = [&](float x, float y) {
        x = std::clamp(x, window.x0 + gap, std::max(window.x0 + gap, window.x1 - gap - width));
        y = std::clamp(y, window.y0 + gap, std::max(window.y0 + gap, window.y1 - gap - height));
        return TourRect{x, y, x + width, y + height};
    };
    if (target.y1 + gap + height <= window.y1 - gap) return inside(target.x0, target.y1 + gap);
    if (target.y0 - gap - height >= window.y0 + gap) return inside(target.x0, target.y0 - gap - height);
    if (target.x1 + gap + width <= window.x1 - gap) return inside(target.x1 + gap, target.y0);
    if (target.x0 - gap - width >= window.x0 + gap) return inside(target.x0 - gap - width, target.y0);
    // No side fits: bottom-right corner of the window.
    return inside(window.x1, window.y1);
}

// Smoothstep easing for the highlight transition.
inline float TourEase(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
inline TourRect TourBetween(const TourRect& from, const TourRect& to, float t) {
    const float e = TourEase(t);
    return {from.x0 + (to.x0 - from.x0) * e, from.y0 + (to.y0 - from.y0) * e,
            from.x1 + (to.x1 - from.x1) * e, from.y1 + (to.y1 - from.y1) * e};
}
}
