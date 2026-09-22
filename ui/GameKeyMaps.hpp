#pragma once

#include <map>
#include <string>
#include <string_view>

namespace shell {
std::string NoteName(int note);

// The game's Notebinds key maps. A map keeps the key labels and changes the
// pitch a key sounds (in Dorian, 3 is E flat and 7 is B flat, with or without
// Ctrl). move[] is in semitones for C D E F G A B; values match the game's
// plain and Ctrl layers.
struct GameKeyMap { const char* name; int move[7]; };
inline constexpr GameKeyMap kGameKeyMaps[]{
    {"Major", {0, 0, 0, 0, 0, 0, 0}},
    {"Dorian", {0, 0, -1, 0, 0, 0, -1}},
    {"Lydian", {0, 0, 0, 1, 0, 0, 0}},
    {"Locrian", {0, -1, -1, 0, -1, -1, -1}},
    {"Mixolydian", {0, 0, 0, 0, 0, 0, -1}},
    {"Minor", {0, 0, -1, 0, 0, -1, -1}},
    {"Phrygian", {0, -1, -1, 0, 0, -1, -1}},
    {"Phrygian Dom", {0, -1, 0, 0, 0, -1, -1}},
};

// Note-to-key table under a map: the 61 keys, plus the Ctrl notes when `full`.
// A note that a map moves onto a white key is typed by that key without Shift.
// Ctrl keys have no Shift layer, so a Ctrl note moved off its key is unmapped.
inline std::map<std::string, std::string> GameKeyTable(const GameKeyMap& map, bool full) {
    static constexpr int degree[12]{0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    static constexpr std::string_view plain = "1234567890qwertyuiopasdfghjklzxcvbnm",
        shifted = "!@#$%^&*()QWERTYUIOPASDFGHJKLZXCVBNM", low = "1234567890qwert", high = "yuiopasdfghj";
    const auto sounds = [&](int note) { return degree[note % 12] < 0 ? note : note + map.move[degree[note % 12]]; };
    std::map<std::string, std::string> table;
    // Shift sounds one semitone above the key's mapped pitch, so a natural
    // moved off its key is reached with Shift (Dorian's E is #). Plain keys
    // are assigned first and win over Shift.
    const auto whites = [&](auto&& each) {
        int note = 36;
        for (size_t i = 0; i < plain.size(); ++i) {
            each(i, sounds(note));
            note += degree[(note + 1) % 12] < 0 ? 2 : 1;
        }
    };
    whites([&](size_t i, int target) { table[NoteName(target)] = std::string(1, plain[i]); });
    whites([&](size_t i, int target) { if (target < 96) table.try_emplace(NoteName(target + 1), std::string(1, shifted[i])); });
    if (!full) return table;
    const auto ctrl = [&](std::string_view keys, int from) {
        for (size_t i = 0; i < keys.size(); ++i) {
            const int target = sounds(from + static_cast<int>(i));
            // Keep within A0..C8 (Locrian's Ctrl+1 would be A flat 0).
            if (target >= 21 && target <= 108) table.try_emplace(NoteName(target), std::string("ctrl+") + keys[i]);
        }
    };
    ctrl(low, 21);
    ctrl(high, 97);
    return table;
}
}
