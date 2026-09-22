#pragma once

#include <string>
#include <map>
#include <stdexcept>
#include <filesystem>
#include <optional>
#include "json.hpp"

namespace midi {

    class ConfigException : public std::runtime_error {
    public:
        explicit ConfigException(const std::string& message) : std::runtime_error(message) {}
    };

    enum class VelocityCurveType {
        LinearCoarse = 0,
        LinearFine = 1,
        ImprovedLowVolume = 2,
        Logarithmic = 3,
        Exponential = 4,
        SCurve = 5,
        Custom = 6
    };

    // Custom curve indices start after the built-ins wherever a curve index is
    // used (the player, the UI preset list, SHELL_VELOCITY).
    inline constexpr size_t kBuiltinVelocityCurves = 6;

    enum class NoteHandlingMode {
        FIFO,
        LIFO,
        NoHandling
    };

    struct VolumeSettings {
        int MIN_VOLUME = 10;
        int MAX_VOLUME = 200;
        int INITIAL_VOLUME = 100;
        int VOLUME_STEP = 10;
        int ADJUSTMENT_INTERVAL_MS = 50;

        void validate() const;
    };
    struct AutoTranspose {
        bool ENABLED = false;
        std::string TRANSPOSE_UP_KEY = "VK_UP";
        std::string TRANSPOSE_DOWN_KEY = "VK_DOWN";

        void validate() const;
    };

    // TSC frequency calibration at startup: MAX_PASSES busy-wait passes, each
    // timed against QPC; the median rejects scheduler noise. 100 ms per pass is
    // ample resolution and keeps startup fast.
    struct AutoplayerTimingAccuracy {
        int MAX_PASSES = 5;
        double MEASURE_SEC = 0.1;

        void validate() const;
    };

    struct UISettings {
        bool alwaysOnTop = false;
    };

    struct MIDISettings {
        bool DETECT_DRUMS = true;

        void validate() const;
    };

    // Wooting analog keyboard settings. Names follow wooting-analog-midi's
    // settings; the defaults are tuned for the game (upstream uses 0.5, 12 and 5).
    struct WootingAnalogSettings {
        // "Note Trigger Threshold": key travel at which a note is struck, 0 to 1.
        double TRIGGER_THRESHOLD = 0.25;
        // Hysteresis: the key must rise below this fraction of the trigger before
        // it can strike again, so a key resting at the threshold doesn't stutter.
        // Upstream has none. 0 to 1.
        double RELEASE_FRACTION = 0.6;
        // "Shift Amount": semitones added while shift is held. At 1, shift gives
        // the sharp, matching the game's layout (upstream defaults to an octave).
        int SHIFT_AMOUNT = 1;
        // "Velocity Scale": velocity = key travel rate at the trigger * SCALE / 100,
        // the same formula as upstream.
        double VELOCITY_SCALE = 2.0;

        void validate() const;
    };

    struct HotkeySettings {
        std::string SUSTAIN_KEY = "VK_SPACE";
        std::string VOLUME_UP_KEY = "VK_RIGHT";
        std::string VOLUME_DOWN_KEY = "VK_LEFT";
        std::string PLAY_PAUSE_KEY = "VK_F1";
        std::string REWIND_KEY = "VK_F2";
        std::string SKIP_KEY = "VK_F3";
        std::string EMERGENCY_EXIT_KEY = "VK_F4";
        // Empty means unbound; so may the four above be.
        std::string PREVIOUS_SONG_KEY;
        std::string NEXT_SONG_KEY;
        void validate() const;
    };

    struct CustomVelocityCurve {
        std::string name;
        std::array<int, 32> velocityValues;
    };

    struct PlaybackSettings {
        VelocityCurveType velocityCurve = VelocityCurveType::LinearCoarse;
        NoteHandlingMode noteHandlingMode = NoteHandlingMode::LIFO;
        std::vector<CustomVelocityCurve> customVelocityCurves;

        // Modifier held while tapping a velocity character. A modifier is
        // required because the velocity characters are also note keys. Alt+1
        // collides with Roblox's capture shortcut, hence the choice.
        std::string velocityModifier = "alt";   // alt, ctrl or shift

        void validate() const;
    };

    class Config {
    public:
        MIDISettings midi;
        PlaybackSettings playback;
        VolumeSettings volume;
        AutoTranspose auto_transpose;
        HotkeySettings hotkeys;
        UISettings ui;
        WootingAnalogSettings wooting;
        AutoplayerTimingAccuracy autoplayer_timing;
        std::map<std::string, std::map<std::string, std::string>> key_mappings;
        std::map<std::string, std::string> controls;
        std::vector<std::string> playlistFiles;

        static Config& getInstance();

        void loadFromFile(const std::filesystem::path& path);
        void saveToFile(const std::filesystem::path& path) const;
        void validate() const;
        void setDefaults();

        static NoteHandlingMode stringToNoteHandlingMode(const std::string& mode);
        static std::string noteHandlingModeToString(NoteHandlingMode mode);

        Config(const Config&) = delete;
        Config& operator=(const Config&) = delete;

    private:
        Config() = default;

        void validateKeyMappings() const;
    };

    void to_json(nlohmann::json& j, const VolumeSettings& v);
    void from_json(const nlohmann::json& j, VolumeSettings& v);
    void to_json(nlohmann::json& j, const AutoTranspose& l);
    void from_json(const nlohmann::json& j, AutoTranspose& l);
    void to_json(nlohmann::json& j, const AutoplayerTimingAccuracy& a);
    void from_json(const nlohmann::json& j, AutoplayerTimingAccuracy& a);
    void to_json(nlohmann::json& j, const MIDISettings& m);
    void from_json(const nlohmann::json& j, MIDISettings& m);
    void to_json(nlohmann::json& j, const HotkeySettings& h);
    void from_json(const nlohmann::json& j, HotkeySettings& h);
    void to_json(nlohmann::json& j, const PlaybackSettings& p);
    void from_json(const nlohmann::json& j, PlaybackSettings& p);
    void to_json(nlohmann::json& j, const Config& c);
    void from_json(const nlohmann::json& j, Config& c);
    void to_json(nlohmann::json& j, const UISettings& ui);
    void from_json(const nlohmann::json& j, UISettings& ui);
    void to_json(nlohmann::json& j, const WootingAnalogSettings& w);
    void from_json(const nlohmann::json& j, WootingAnalogSettings& w);

} // namespace midi