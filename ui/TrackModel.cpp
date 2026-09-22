#include "TrackModel.hpp"
#include "GeneralMidi.hpp"
#include <cctype>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace shell {
namespace {
// DAW exports often put every part on channel 1 with no program change, so by
// program alone every track looks like piano. When a channel never gets a
// program change, classify the track by name instead; an explicit program
// always wins. Returns the display name for the Instrument column, or null if
// the name suggests a piano or nothing recognizable.
const char* NamedNonPiano(std::string name) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Harpsichord and clavinet (GM programs 6 and 7) count as piano; checking
    // them first also stops "harpsichord" matching "harp" below.
    for (const char* keyboard : {"piano", "keys", "harpsi", "clav"})
        if (name.find(keyboard) != std::string::npos) return nullptr;
    // First match wins, so a word that contains another must come first.
    static constexpr std::pair<const char*, const char*> kOthers[] = {
        {"flute", "Flute"}, {"piccolo", "Piccolo"}, {"recorder", "Recorder"}, {"ocarina", "Ocarina"},
        {"whistle", "Whistle"}, {"oboe", "Oboe"}, {"clarinet", "Clarinet"}, {"bassoon", "Bassoon"},
        {"sax", "Saxophone"}, {"violin", "Violin"}, {"viola", "Viola"}, {"cello", "Cello"},
        {"contrabass", "Contrabass"}, {"string", "Strings"}, {"harp", "Harp"}, {"bass", "Bass"},
        {"guitar", "Guitar"}, {"banjo", "Banjo"}, {"sitar", "Sitar"}, {"koto", "Koto"},
        {"trumpet", "Trumpet"}, {"trombone", "Trombone"}, {"tuba", "Tuba"}, {"horn", "Horn"},
        {"brass", "Brass"}, {"organ", "Organ"}, {"accordion", "Accordion"}, {"harmonica", "Harmonica"},
        {"choir", "Choir"}, {"voice", "Voice"}, {"vocal", "Voice"}, {"synth", "Synth"}, {"drum", "Drums"},
        {"perc", "Percussion"}, {"timpani", "Timpani"}, {"bell", "Bells"}, {"marimba", "Marimba"},
        {"xylophone", "Xylophone"}, {"vibraphone", "Vibraphone"}, {"glock", "Glockenspiel"},
        {"celesta", "Celesta"}};
    for (const auto& [word, label] : kOthers)
        if (name.find(word) != std::string::npos) return label;
    return nullptr;
}

bool ValidUtf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : lead >= 0xC2 && lead < 0xE0 ? 2 : lead >= 0xE0 && lead < 0xF0 ? 3 :
                              lead >= 0xF0 && lead < 0xF5 ? 4 : 0;
        if (!length || i + length > text.size()) return false;
        for (size_t k = 1; k < length; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        i += length;
    }
    return true;
}

// SMF track names have no declared encoding. Valid UTF-8 is kept; otherwise
// decode with the system ANSI code page, falling back to Windows-1252, which
// accepts any byte.
std::string NameToUtf8(const std::string& raw) {
    if (raw.empty() || ValidUtf8(raw)) return raw;
    std::wstring wide;
    for (const auto& [page, flags] : {std::pair<UINT, DWORD>{CP_ACP, MB_ERR_INVALID_CHARS}, {1252, 0}}) {
        const int size = MultiByteToWideChar(page, flags, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
        if (size <= 0) continue;
        wide.resize(size);
        MultiByteToWideChar(page, flags, raw.data(), static_cast<int>(raw.size()), wide.data(), size);
        break;
    }
    if (wide.empty()) return raw;
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(size > 0 ? size : 0, '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}
}
std::vector<TrackRow> DescribeTracks(const MidiFile& file) {
    struct Part {
        TrackRow row;
        std::bitset<128> programs;
        std::bitset<16> channels;
        bool nonPiano = false;
        bool programmed = false;      // has a note on a channel with an explicit program
        const char* named = nullptr;  // instrument inferred from the track name
    };
    struct EventRef { const MidiEvent* event; size_t track; };
    std::vector<Part> parts(file.tracks.size());
    std::vector<EventRef> timeline;
    for (size_t i = 0; i < file.tracks.size(); ++i) {
        parts[i].row.index = i;
        parts[i].row.name = file.tracks[i].name;
        for (const auto& event : file.tracks[i].events) {
            if (event.status == 0xFF && event.data1 == 0x03)
                parts[i].row.name.assign(event.metaData.begin(), event.metaData.end());
            if ((event.status & 0xF0) == 0xC0 ||
                ((event.status & 0xF0) == 0x90 && event.data2 > 0))
                timeline.push_back({&event, i});
        }
    }
    // Program changes are channel-wide, and can live in a different MIDI track.
    // Preserve file order for events sharing a tick.
    std::stable_sort(timeline.begin(), timeline.end(), [](const auto& a, const auto& b) {
        return a.event->absoluteTick < b.event->absoluteTick;
    });
    std::array<unsigned, 16> programs{}; // General MIDI defaults to program 0.
    std::bitset<16> programSet;           // which channels ever had a program change
    for (const auto& ref : timeline) {
        const auto& event = *ref.event;
        const unsigned channel = event.status & 0x0F;
        if ((event.status & 0xF0) == 0xC0) {
            programs[channel] = event.data1 & 0x7F;
            programSet.set(channel);
            continue;
        }
        auto& part = parts[ref.track];
        ++part.row.notes;
        part.channels.set(channel);
        if (channel == 9) part.row.drums = true;
        else part.programs.set(programs[channel]);
        if (programSet[channel]) part.programmed = true;
        else if (channel != 9 && !part.named) part.named = NamedNonPiano(part.row.name);
        part.nonPiano |= channel == 9 || programs[channel] > 7 || (!programSet[channel] && part.named);
    }
    std::vector<TrackRow> rows;
    for (auto& part : parts) {
        auto& row = part.row;
        if (!row.notes) continue;
        row.name = NameToUtf8(row.name);
        if (row.name.empty()) row.name = "Track " + std::to_string(row.index + 1);
        for (char& c : row.name) if (static_cast<unsigned char>(c) < 32) c = ' ';
        row.piano = !part.nonPiano;
        if (row.drums && part.programs.none()) row.instrument = "Drums";
        else if (part.named && !part.programmed && !row.drums) row.instrument = part.named;
        else if (part.programs.count() == 1 && !row.drums) {
            for (size_t i = 0; i < 128; ++i)
                if (part.programs[i]) row.instrument = midi::GeneralMidiNames[i];
        } else row.instrument = "Mixed instruments";
        for (size_t i = 0; i < 16; ++i) {
            if (!part.channels[i]) continue;
            if (!row.channels.empty()) row.channels += ", ";
            row.channels += std::to_string(i + 1);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
}
