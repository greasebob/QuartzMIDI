#pragma once

// SheetExport: converts a score to virtual piano sheet text using the app's
// key mapping in reverse.
//
// Notation: each note is the character the app would type for it, simultaneous
// notes are bracketed, and time is spaces. Velocity and note length are not
// represented.
//
// Header only, no project dependencies.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sheet {

struct Note {
    double seconds = 0;   // onset from the start of the score
    std::string name;     // "C4", "F#3", as spelled in KEY_MAPPINGS
};

struct Options {
    // Onsets closer than this (seconds) form one chord. Human chord asynchrony
    // is roughly 30 to 50 ms (Goebl; Repp).
    double chordWindow = 0.045;
    // Seconds per beat, 0 when unknown. When set, a gap emits one space per
    // whole beat (minimum one).
    double beatSeconds = 0;
    size_t groupsPerLine = 8;
    // Cap on spaces emitted for a single gap.
    size_t maxGapSpaces = 4;
};

struct Result {
    std::string text;
    size_t notes = 0;      // characters written
    size_t groups = 0;     // chords and single notes written
    size_t unmapped = 0;   // notes with no key in the mapping (dropped)
    size_t merged = 0;     // notes whose character duplicated another in the same chord
};

// Invariant: notes + merged + unmapped == number of input notes.

namespace detail {

// Sorts and dedupes keys in place so a chord is always written the same way.
// Returns the number of duplicates removed; keys.size() afterwards is the
// written count.
inline size_t appendGroup(std::string& out, std::vector<std::string>& keys) {
    std::sort(keys.begin(), keys.end());
    const size_t before = keys.size();
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const size_t merged = before - keys.size();
    if (keys.size() == 1) {
        out += keys.front();
        return merged;
    }
    out += '[';
    for (const auto& key : keys) out += key;
    out += ']';
    return merged;
}

} // namespace detail

// notes need not be sorted.
inline Result ToVirtualPiano(std::vector<Note> notes,
                             const std::map<std::string, std::string>& mapping,
                             const Options& options = {}) {
    Result result;
    std::stable_sort(notes.begin(), notes.end(),
                     [](const Note& a, const Note& b) { return a.seconds < b.seconds; });

    std::vector<std::string> group;
    double groupStart = 0;
    double previousEnd = 0;
    size_t onLine = 0;
    bool first = true;

    const auto flush = [&](bool last) {
        if (group.empty()) return;
        if (!first) {
            const double gap = groupStart - previousEnd;
            size_t spaces = 1;
            if (options.beatSeconds > 0 && gap > 0) {
                const auto beats = static_cast<size_t>(gap / options.beatSeconds);
                spaces = (std::max)(size_t{1}, (std::min)(beats, options.maxGapSpaces));
            }
            result.text.append(spaces, ' ');
        }
        result.merged += detail::appendGroup(result.text, group);
        result.notes += group.size();
        ++result.groups;
        previousEnd = groupStart;
        first = false;
        group.clear();
        if (++onLine >= options.groupsPerLine && !last) {
            result.text += '\n';
            onLine = 0;
            first = true; // no leading space after a newline
        }
    };

    for (const auto& note : notes) {
        const auto found = mapping.find(note.name);
        if (found == mapping.end() || found->second.empty()) {
            ++result.unmapped;
            continue;
        }
        if (!group.empty() && note.seconds - groupStart > options.chordWindow) flush(false);
        if (group.empty()) groupStart = note.seconds;
        group.push_back(found->second);
    }
    flush(true);
    return result;
}

// ---------------------------------------------------------------------------
// Styled sheets
//
// Ported from ArijanJ's midi-converter, github.com/ArijanJ/midi-converter,
// mainly src/utils/VP.js and src/utils/Rendering.js. MIT licence, Copyright
// (c) 2024 ArijanJ; the notice is in third_party/midi-converter/LICENSE.
// Behaviour follows the original; intentional deviations are commented.
//
// Adds to ToVirtualPiano: chained quantize with spread chords in braces, two
// chord orders, marked out-of-range notes, rhythm colours, tempo separators,
// BPM comments, per-bar line breaks and a transposition search.

enum class Rhythm { SixtyFourth, ThirtySecond, Sixteenth, Eighth, Quarter, Half, Whole, Quadruple, Long };
// Position of shifted characters and out-of-range notes within a chord.
// InOrder keeps shifted characters in pitch order and puts low out-of-range
// notes first, high ones last.
enum class Place { Start, End, InOrder };
// Bars: break at each bar and at every tempo or meter change (the original's
// "realistic"). Beats: break every StyleOptions::beats beats ("manual").
// Phrases (not in the original): as Bars, and also at each rest that ends a
// phrase; see PhraseGapMs.
enum class Breaks { Bars, Beats, None, Phrases };
enum class BpmStyle { Detailed, Simple };
// Which sustain pedal lines a sheet draws: where it is down, where it is up
// (for a player whose sustain key is held to lift it), or both in two colours.
enum class PedalMarks { Off, Normal, Inverted, Both };

// end: the release in seconds, below seconds when the score does not say.
struct TimedNote { double seconds = 0; int midi = 0; double end = -1; };
struct TempoMark { double seconds = 0; double bpm = 120; };
struct MeterMark { double seconds = 0; int numerator = 4; };

struct StyleOptions {
    double quantizeMs = 35;          // a note within this of the previous one joins its chord
    bool sequentialQuantize = true;  // spread chords keep played order
    bool curlyQuantizes = true;      // spread chords use braces
    bool classicChordOrder = false;  // ordering used by early VP converters
    Place shifts = Place::Start;
    Place outOfRangePlace = Place::InOrder;
    bool showOutOfRange = true;
    bool outOfRangeMarks = false;
    std::string outOfRangeSeparator = ":";
    bool tempoMarks = false;         // rhythm separators instead of single spaces
    bool bpmChanges = true;
    BpmStyle bpmStyle = BpmStyle::Detailed;
    int minSpeedChange = 10;         // percent; smaller tempo changes get no comment
    Breaks breaks = Breaks::Phrases;
    int beats = 4;
    double missingBpm = 120;         // used before the first tempo mark or when there is none
    int transpose = 0;               // semitones, applied before mapping
    bool autoTranspose = true;       // search around transpose for the best fit
    int resilience = 2;              // notes a found transposition must gain on transpose to replace it
    // Section search (not in the original): transposition may change mid-sheet.
    bool autoSections = false;
    int sectionSwitchCost = 12;      // notes a change must bring onto keys to pay off
    double sectionMinSeconds = 8;    // minimum section length
    int sectionRestMs = 250;         // minimum rest before a change
    int sectionRange = 12;           // semitones either side of transpose to try
    PedalMarks pedalMarks = PedalMarks::Both;
};

// A span under one transposition, in seconds, both ends inclusive. Each start
// is announced with a "Transpose by" comment.
struct Section { double from = 0; double to = 0; int semitones = 0; };

struct Segment { std::string text; bool outOfRange = false; };

struct StyledItem {
    enum class Kind { Chord, Break, Comment };
    Kind kind = Kind::Chord;
    std::vector<Segment> segments;   // chord as written, including brackets and marks
    std::string text;                // comment text
    std::string separator;           // text after the chord
    Rhythm rhythm = Rhythm::Long;
    double ms = 0;                   // onset of the chord's first note
    double msEnd = 0;                // onset of its last note
    double beatMs = 500;             // length of a beat at that onset
    // The sustain pedal while the chord sounds: 0 up, 1 part way, 2 down.
    // pedalHeld: it stays down into the next chord, so the gap to it is down
    // too; otherwise it comes up after this chord, if only to be taken again.
    int pedal = 0;
    bool pedalHeld = false;
    // The keys written for the chord, how many are shifted characters, and
    // its lowest and highest note after transposition.
    int keys = 0;
    int shifted = 0;
    int low = 0;
    int high = 0;
};

struct StyledResult {
    std::vector<StyledItem> items;
    std::string text;
    size_t notes = 0;      // characters written for real notes
    size_t groups = 0;     // chords and single notes written
    size_t merged = 0;     // repeated pitches within one struck chord, written once
    size_t unmapped = 0;   // outside A0 to C8 or missing from the mapping; written as _
    size_t hidden = 0;     // out-of-range notes omitted because showOutOfRange is off
    int transposition = 0; // of the first section
    std::vector<Section> sections;   // runs of one transposition, in order
    bool hasTempo = false;
    PedalMarks pedalMarks = PedalMarks::Off;   // the lines to draw, Off when the song has no pedal
    int difficulty = 0;    // 1 to 10, 0 for a sheet with no keys; see Difficulty
};
// Invariant: notes + merged + unmapped + hidden == number of input notes.

// Convert tick-timed tempo and meter events to seconds for a ticks-per-quarter
// division. SMPTE division is not supported.
struct TickTempo { uint64_t tick = 0; uint32_t microsecondsPerQuarter = 500000; };
struct TickMeter { uint64_t tick = 0; int numerator = 4; };

// tempos must be sorted by tick.
inline double SecondsAtTick(uint64_t tick, const std::vector<TickTempo>& tempos, uint16_t division) {
    if (division == 0) return 0;
    double seconds = 0, microseconds = 500000;
    uint64_t from = 0;
    for (const auto& tempo : tempos) {
        if (tempo.tick >= tick) break;
        seconds += static_cast<double>(tempo.tick - from) * microseconds / division / 1e6;
        from = tempo.tick;
        microseconds = tempo.microsecondsPerQuarter;
    }
    return seconds + static_cast<double>(tick - from) * microseconds / division / 1e6;
}

inline std::vector<TempoMark> TempoMarksFromTicks(std::vector<TickTempo> tempos, uint16_t division) {
    std::stable_sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    std::vector<TempoMark> marks;
    for (const auto& tempo : tempos) {
        if (tempo.microsecondsPerQuarter == 0) continue;
        marks.push_back({SecondsAtTick(tempo.tick, tempos, division), 60e6 / tempo.microsecondsPerQuarter});
    }
    return marks;
}

inline std::vector<MeterMark> MeterMarksFromTicks(const std::vector<TickMeter>& meters, std::vector<TickTempo> tempos, uint16_t division) {
    std::stable_sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    std::vector<MeterMark> marks;
    for (const auto& meter : meters) marks.push_back({SecondsAtTick(meter.tick, tempos, division), meter.numerator});
    return marks;
}

namespace detail {

inline constexpr std::string_view kCapitals = "!@#$%^&*()QWERTYUIOPASDFGHJKLZXCBVNM";
inline constexpr std::string_view kLowercase = "1234567890qwertyuiopasdfghjklzxcvbnm";
// The original's characters for notes outside the 61-key layout, used when the
// mapping has none.
inline constexpr std::string_view kLowOutOfRange = "1234567890qwert";   // A0 to B1
inline constexpr std::string_view kHighOutOfRange = "yuiopasdfghj";     // C#7 to C8

inline std::string NoteName(int midi) {
    static constexpr const char* names[]{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[midi % 12]) + std::to_string(midi / 12 - 1);
}
inline bool OneOf(const std::string& character, std::string_view set) {
    return character.size() == 1 && set.find(character[0]) != std::string_view::npos;
}

struct Placed {
    double ms = 0;
    double beatMs = 500;
    int midi = 0;
    std::string character;
    bool valid = false;
    bool outOfRange = false;
    int display = 0;   // sort key (displayValue in the original)
};

inline Placed Locate(double ms, int midi, const std::map<std::string, std::string>& mapping, const StyleOptions& o) {
    Placed p;
    p.ms = ms;
    p.midi = midi;
    p.valid = midi >= 21 && midi <= 108;
    p.outOfRange = midi <= 35 || midi >= 97;
    if (p.valid) {
        const auto found = mapping.find(NoteName(midi));
        // 88-key bindings outside C2 to C7 are "ctrl+<key>"; the sheet writes the key.
        if (found != mapping.end() && !found->second.empty())
            p.character = found->second.starts_with("ctrl+") ? found->second.substr(5) : found->second;
        else if (midi <= 35) p.character = std::string(1, kLowOutOfRange[midi - 21]);
        else if (midi >= 97) p.character = std::string(1, kHighOutOfRange[midi - 97]);
        else p.valid = false;
    }
    p.display = midi;
    if (OneOf(p.character, kCapitals)) {
        if (o.shifts == Place::Start) p.display = midi - 108;
        else if (o.shifts == Place::End) p.display = midi + 108;
    } else if (p.outOfRange) {
        if (o.outOfRangePlace == Place::Start) p.display = midi - 1024;
        else if (o.outOfRangePlace == Place::End) p.display = midi + 1024;
        else p.display = OneOf(p.character, kLowOutOfRange) ? midi - 1024 : midi + 1024;
    }
    return p;
}

inline std::vector<Placed> ClassicOrder(std::vector<Placed> notes) {
    std::stable_sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) { return a.midi < b.midi; });
    std::vector<Placed> start, end, numeric, upper, lower;
    const Placed* last = nullptr;
    for (const auto& n : notes) {
        if (n.outOfRange) {
            if (n.display == n.midi - 1024) start.push_back(n);
            else if (n.display == n.midi + 1024) end.push_back(n);
            continue;
        }
        if (n.character.size() == 1 && n.character[0] >= '0' && n.character[0] <= '9') numeric.push_back(n);
        else if (OneOf(n.character, kCapitals)) upper.push_back(n);
        else lower.push_back(n);
        last = &n;
    }
    std::vector<Placed> out(start);
    const auto append = [&](const std::vector<Placed>& part) { out.insert(out.end(), part.begin(), part.end()); };
    if (last) {
        if (OneOf(last->character, kCapitals)) { append(numeric); append(lower); append(upper); }
        else { append(upper); append(numeric); append(lower); }
    }
    append(end);
    return out;
}

inline std::vector<Placed> SortChord(std::vector<Placed> notes, bool classic) {
    if (classic) return ClassicOrder(std::move(notes));
    std::stable_sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) { return a.display < b.display; });
    return notes;
}

inline StyledItem RenderChord(const std::vector<Placed>& chord, bool quantized, const StyleOptions& o, StyledResult& r) {
    StyledItem item;
    const auto nonOutOfRange = static_cast<size_t>(std::count_if(chord.begin(), chord.end(), [](const auto& n) { return !n.outOfRange; }));
    bool isChord = chord.size() > 1 && std::any_of(chord.begin(), chord.end(), [](const auto& n) { return n.valid; });
    if (!o.showOutOfRange && nonOutOfRange <= 1) isChord = false;
    const bool curly = quantized && o.curlyQuantizes;
    const Placed* firstStart = nullptr;
    const Placed* lastStart = nullptr;
    const Placed* firstEnd = nullptr;
    for (const auto& n : chord) {
        if (!n.outOfRange) continue;
        if (n.display == n.midi - 1024) { if (!firstStart) firstStart = &n; lastStart = &n; }
        else if (n.display == n.midi + 1024 && !firstEnd) firstEnd = &n;
    }
    if (isChord) item.segments.push_back({curly ? "{" : "["});
    const auto press = [&](const Placed& n) {
        item.low = item.keys ? (std::min)(item.low, n.midi) : n.midi;
        item.high = item.keys ? (std::max)(item.high, n.midi) : n.midi;
        ++item.keys;
        if (OneOf(n.character, kCapitals)) ++item.shifted;
        ++r.notes;
    };
    for (const auto& n : chord) {
        if (!n.valid) { item.segments.push_back({"_"}); ++r.unmapped; continue; }
        const bool drawOor = n.outOfRange && o.showOutOfRange;
        if (drawOor) {
            const bool marked = &n == firstStart ||
                (!isChord && &n == firstEnd) ||
                (isChord && nonOutOfRange == 0 && !firstStart && &n == firstEnd) ||
                (isChord && nonOutOfRange > 0 && &n == firstEnd);
            std::string text = n.character;
            if (o.outOfRangeMarks && marked) text = o.outOfRangeSeparator + text;
            item.segments.push_back({text, true});
            press(n);
            if (o.outOfRangeMarks && &n == lastStart && nonOutOfRange > 0) item.segments.push_back({"'"});
        } else if (!n.outOfRange) {
            item.segments.push_back({n.character});
            press(n);
        } else {
            ++r.hidden;
        }
    }
    if (isChord) item.segments.push_back({curly ? "}" : "]"});
    ++r.groups;
    return item;
}

// How notes sit on the 61 keys under a transposition: how many land on them,
// and how many of those are shifted characters.
struct Fit { long long onKeys = 0; long long shifted = 0; };

inline Fit TranspositionFit(const std::vector<TimedNote>& notes, int by, const std::map<std::string, std::string>& mapping) {
    Fit fit;
    for (const auto& note : notes) {
        const auto p = Locate(0, note.midi + by, mapping, {});
        if (p.outOfRange || !p.valid) continue;
        ++fit.onKeys;
        if (!OneOf(p.character, kLowercase)) ++fit.shifted;
    }
    return fit;
}

// A note off the keys is lost to a 61-key player; a shifted character is only
// harder to press. Across the owner's library (3349 files, 2026-09-26) the
// fewest shifted among the transpositions with the most notes on keys left
// 26% of notes shifted and 1.6% off the keys; weighing a note off the keys as
// four shifted characters leaves 11% shifted and 2.0% off.
inline constexpr long long kShiftedPerNote = 4;
inline long long FitScore(const Fit& fit) { return fit.onKeys * kShiftedPerNote - fit.shifted; }

// Deviation: the original scores notes on keys twice plus the difference
// between plain and shifted characters, so a key of mostly shifted characters
// scores as well as one of mostly plain ones, and it searches stickTo up to
// eleven above and the negatives of those. Here the best of -11 to 11 by
// FitScore wins, ties going to the one nearest stickTo, then the lower, and
// it replaces stickTo only when better by more than resilience notes.
inline int BestTransposition(const std::vector<TimedNote>& notes, const std::map<std::string, std::string>& mapping, int stickTo, int resilience) {
    const long long kept = FitScore(TranspositionFit(notes, stickTo, mapping));
    int best = stickTo;
    long long bestScore = kept;
    for (int d = 1; d <= 11 + std::abs(stickTo); ++d) {
        for (const int n : {stickTo - d, stickTo + d}) {
            if (n < -11 || n > 11) continue;
            const long long score = FitScore(TranspositionFit(notes, n, mapping));
            if (score > bestScore) { best = n; bestScore = score; }
        }
    }
    return bestScore - kept > resilience * kShiftedPerNote ? best : stickTo;
}

// ms and msEnd are the first and last onsets in milliseconds.
struct SearchChord { std::vector<TimedNote> notes; double ms = 0; double msEnd = 0; };

// Chord index of each note; notes must be sorted by onset. The window is
// chained: each note is compared with the previous note, not the chord's first.
inline std::vector<size_t> ChordIndices(const std::vector<TimedNote>& notes, double quantizeMs) {
    std::vector<size_t> indices(notes.size(), 0);
    size_t chord = 0;
    double last = 0;
    for (size_t i = 0; i < notes.size(); ++i) {
        const double ms = notes[i].seconds * 1000;
        if (i > 0 && !(std::abs(ms - last) < quantizeMs)) ++chord;
        indices[i] = chord;
        last = ms;
    }
    return indices;
}

// The rest before each chord in ms, the first's 0: from when every note
// struck before it has been released to its first onset, none while one
// still sounds, so a chord held into the next is no rest. A note whose
// release is unknown ends at its onset, which makes this the gap from the
// previous chord's last onset.
inline std::vector<double> RestsBefore(const std::vector<SearchChord>& chords) {
    std::vector<double> rests(chords.size(), 0);
    double sounding = 0;
    for (size_t k = 0; k < chords.size(); ++k) {
        if (k > 0) rests[k] = (std::max)(0.0, chords[k].ms - sounding);
        for (const auto& note : chords[k].notes) sounding = (std::max)(sounding, (std::max)(note.seconds, note.end) * 1000);
    }
    return rests;
}

// The rest that ends a phrase, in ms: at least half a second and 3.2 times
// the song's median rest between chords (RestsBefore), Velo's rule.
// Infinite with under two chords. By themselves such rests left a flowing
// passage as one line of hundreds of chords (Fur Elise 414), so they add to
// the bar breaks. Across the owner's library (3347 files, 2026-09-27), with
// notes held by their releases and the sustain pedal (HeldByPedal), phrases
// add 0.6% more lines than bars and leave Flower Man (Chewie Melodies) at its
// 83; from onsets alone they added 10.1% and took it to 133, and from
// releases without the pedal 3.0% and 109.
inline double PhraseGapMs(const std::vector<double>& rests) {
    std::vector<double> gaps(rests.size() > 1 ? rests.begin() + 1 : rests.end(), rests.end());
    if (gaps.empty()) return std::numeric_limits<double>::infinity();
    std::sort(gaps.begin(), gaps.end());
    const size_t n = gaps.size();
    const double median = n % 2 ? gaps[n / 2] : (gaps[n / 2 - 1] + gaps[n / 2]) / 2;
    return (std::max)(500.0, 3.2 * median);
}

// Per-chord transposition maximizing the summed FitScore less
// sectionSwitchCost notes per change (dynamic programming over chords x
// candidates). Sections shorter than sectionMinSeconds are disallowed unless they span the
// whole sheet, and a change requires a preceding rest of sectionRestMs.
// Candidates are ordered nearest-first, lower before higher, and ties keep the
// earlier one; the editor page's script must use the same order to agree.
inline std::vector<int> BestSections(const std::vector<SearchChord>& chords, const std::map<std::string, std::string>& mapping, const StyleOptions& o) {
    const size_t n = chords.size();
    std::vector<int> candidates{o.transpose};
    for (int d = 1; d <= o.sectionRange; ++d) { candidates.push_back(o.transpose - d); candidates.push_back(o.transpose + d); }
    const size_t T = candidates.size();
    const long long cost = o.sectionSwitchCost * kShiftedPerNote;
    const double minMs = o.sectionMinSeconds * 1000, restMs = o.sectionRestMs;
    constexpr long long NONE = -(1LL << 60);
    // prefix[t][k]: score of chords 0..k-1 under candidate t.
    std::vector<std::vector<long long>> prefix(T, std::vector<long long>(n + 1, 0));
    for (size_t t = 0; t < T; ++t)
        for (size_t k = 0; k < n; ++k) prefix[t][k + 1] = prefix[t][k] + FitScore(TranspositionFit(chords[k].notes, candidates[t], mapping));
    // closed[i][t]: best score of chords 0..i whose last section is under t and
    // long enough to end at i; from[i][t] is that section's first chord.
    // open[j][t]: best score before a section under t starting at chord j, with
    // the switch cost paid; before[j][t] is the preceding section's candidate.
    std::vector<std::vector<long long>> closed(n, std::vector<long long>(T, NONE)), open(n, std::vector<long long>(T, NONE));
    std::vector<std::vector<size_t>> from(n, std::vector<size_t>(T, 0)), before(n, std::vector<size_t>(T, 0));
    std::vector<long long> running(T, NONE);
    std::vector<size_t> runningFrom(T, 0);
    size_t admit = 0;   // next section start not yet folded into running
    for (size_t i = 0; i < n; ++i) {
        if (i == 0) {
            for (size_t t = 0; t < T; ++t) open[0][t] = 0;
        } else if (chords[i].ms - chords[i - 1].msEnd >= restMs) {
            size_t bestAt = 0, secondAt = 0; long long best = NONE, second = NONE;
            for (size_t t = 0; t < T; ++t) {
                const long long value = closed[i - 1][t];
                if (value > best) { second = best; secondAt = bestAt; best = value; bestAt = t; }
                else if (value > second) { second = value; secondAt = t; }
            }
            for (size_t t = 0; t < T; ++t) {
                const long long other = t == bestAt ? second : best;
                if (other == NONE) continue;
                open[i][t] = other - cost;
                before[i][t] = t == bestAt ? secondAt : bestAt;
            }
        }
        while (admit <= i && chords[i].ms - chords[admit].ms >= minMs) {
            for (size_t t = 0; t < T; ++t) {
                if (open[admit][t] == NONE) continue;
                const long long value = open[admit][t] - prefix[t][admit];
                if (value > running[t]) { running[t] = value; runningFrom[t] = admit; }
            }
            ++admit;
        }
        for (size_t t = 0; t < T; ++t) {
            if (running[t] == NONE) continue;
            closed[i][t] = prefix[t][i + 1] + running[t];
            from[i][t] = runningFrom[t];
        }
        // A single section spanning the whole sheet is allowed at any length.
        if (i + 1 == n && admit == 0)
            for (size_t t = 0; t < T; ++t) { closed[i][t] = prefix[t][n]; from[i][t] = 0; }
    }
    std::vector<int> shifts(n, o.transpose);
    if (n == 0) return shifts;
    size_t t = 0;
    for (size_t u = 1; u < T; ++u) if (closed[n - 1][u] > closed[n - 1][t]) t = u;
    for (size_t end = n; end > 0;) {
        const size_t start = from[end - 1][t];
        for (size_t k = start; k < end; ++k) shifts[k] = candidates[t];
        if (start == 0) break;
        t = before[start][t];
        end = start;
    }
    return shifts;
}

inline std::optional<std::string> BpmComment(long previous, long next, BpmStyle style, int minimum) {
    const bool faster = next > previous;
    const double larger = static_cast<double>(faster ? next : previous);
    const double smaller = static_cast<double>(faster ? previous : next);
    if (smaller <= 0) return std::nullopt;
    const long percent = std::lround((larger - smaller) / smaller * 100);
    if (percent < minimum) return std::nullopt;
    const std::string word = faster ? "faster" : "slower";
    if (style == BpmStyle::Simple) {
        const char mark = faster ? '>' : '<';
        const long increments = (std::max)(1L, percent / 10);
        if (increments > 20) return std::string(1, mark) + " " + std::to_string(percent) + "% " + word + " " + mark;
        return std::string(static_cast<size_t>(increments), mark);
    }
    return std::to_string(percent) + "% " + word + " - BPM changed to " + std::to_string(next);
}

inline Rhythm RhythmFor(double beat, double difference) {
    if (difference < beat / 16) return Rhythm::SixtyFourth;
    if (difference < beat / 8) return Rhythm::ThirtySecond;
    if (difference < beat / 4) return Rhythm::Sixteenth;
    if (difference < beat / 2) return Rhythm::Eighth;
    if (difference < beat) return Rhythm::Quarter;
    if (difference < beat * 2) return Rhythm::Half;
    if (difference < beat * 4) return Rhythm::Whole;
    if (difference < beat * 8) return Rhythm::Quadruple;
    return Rhythm::Long;
}

inline std::string Separator(double beat, double difference) {
    if (difference < beat / 4) return "-";
    if (difference < beat / 2) return " ";
    if (difference < beat) return " - ";
    if (difference < beat * 2) return ", ";
    if (difference < beat * 3) return "... ";
    if (difference < beat * 4) return ".... ";
    return "...... ";
}

// Strips trailing spaces and collapses runs of blank lines (the original copies
// innerText unchanged).
inline std::string TidyLines(const std::string& text) {
    std::vector<std::string> lines(1);
    for (char c : text) {
        if (c == '\n') lines.emplace_back();
        else lines.back() += c;
    }
    std::string out;
    bool started = false, blank = false;
    for (auto& line : lines) {
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty()) { blank = started; continue; }
        if (started) out += blank ? "\n\n" : "\n";
        out += line;
        started = true;
        blank = false;
    }
    return out;
}

inline std::string EscapeHtml(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

} // namespace detail

// How hard the sheet is to play by hand, 1 to 10, from the keys it writes:
// its speed (keys a second over the time it is played, rests capped at two
// seconds, averaged with the busiest ten seconds), its big chords (the size
// only one chord in a hundred exceeds), its share of shifted characters and
// its hand jumps (an octave or more between chords under 300 ms apart).
// Tuned on the owner's library (3349 files, 2026-09-26), each in its best
// transposition: Sweden 2, Trisha's Lullaby 3, Chopin's Nocturne Op. 9 No. 2
// 4, Fur Elise 5, Rush E 7, Winter Wind 8, La Campanella 9, with 4 and 5 the
// commonest and 10 one file in a thousand. 0 when nothing is written. The
// page's difficulty does the same arithmetic.
inline int Difficulty(const std::vector<StyledItem>& items) {
    std::vector<const StyledItem*> chords;
    for (const auto& item : items)
        if (item.kind == StyledItem::Kind::Chord && item.keys > 0) chords.push_back(&item);
    if (chords.empty()) return 0;
    double keys = 0, shifted = 0, played = 0, jumps = 0, busiest = 0, recent = 0;
    std::vector<int> sizes;
    size_t first = 0;
    for (size_t c = 0; c < chords.size(); ++c) {
        const auto& chord = *chords[c];
        keys += chord.keys;
        shifted += chord.shifted;
        sizes.push_back(chord.keys);
        if (c > 0) {
            const auto& previous = *chords[c - 1];
            const double gap = chord.ms - previous.msEnd;
            played += (std::min)(gap, 2000.0);
            if (gap < 300 && (std::abs(chord.low - previous.low) >= 12 || std::abs(chord.high - previous.high) >= 12)) ++jumps;
        }
        recent += chord.keys;
        while (chord.ms - chords[first]->ms > 10000) recent -= chords[first++]->keys;
        busiest = (std::max)(busiest, recent / 10);
    }
    played = (std::max)(played / 1000, 1.0);
    std::sort(sizes.begin(), sizes.end());
    const double big = sizes[(sizes.size() - 1) * 99 / 100];
    const double speed = (keys / played + busiest) / 2;
    const auto part = [](double value) { return (std::min)((std::max)(value, 0.0), 1.0); };
    const double points = part((speed - 2) / 20) * 5.5 + part((big - 1) / 6) * 1.5 + part(shifted / keys / 0.5) + part(jumps / played / 5);
    return static_cast<int>((std::min)((std::max)(std::lround(1 + points), 1L), 10L));
}

// sections override any other transposition for chords starting inside them.
inline StyledResult Style(std::vector<TimedNote> notes, const std::map<std::string, std::string>& mapping,
                          std::vector<TempoMark> tempos = {}, std::vector<MeterMark> meters = {},
                          const StyleOptions& o = {}, const std::vector<Section>& sections = {}) {
    using Kind = StyledItem::Kind;
    StyledResult r;
    const auto bySeconds = [](const auto& a, const auto& b) { return a.seconds < b.seconds; };
    std::stable_sort(notes.begin(), notes.end(), bySeconds);
    std::stable_sort(tempos.begin(), tempos.end(), bySeconds);
    std::stable_sort(meters.begin(), meters.end(), bySeconds);
    r.hasTempo = !tempos.empty();

    // Group chords, then assign each a transposition: fixed, searched, or from sections.
    const auto chordOf = detail::ChordIndices(notes, o.quantizeMs);
    std::vector<detail::SearchChord> chords;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (chordOf[i] == chords.size()) chords.push_back({{}, notes[i].seconds * 1000, notes[i].seconds * 1000});
        chords.back().notes.push_back(notes[i]);
        chords.back().msEnd = notes[i].seconds * 1000;
    }
    const int base = o.autoTranspose && !notes.empty() ? detail::BestTransposition(notes, mapping, o.transpose, o.resilience) : o.transpose;
    std::vector<int> shifts(chords.size(), base);
    if (o.autoTranspose && o.autoSections && !chords.empty()) shifts = detail::BestSections(chords, mapping, o);
    for (size_t k = 0; k < chords.size(); ++k)
        for (const auto& section : sections)
            if (chords[k].ms / 1000 >= section.from && chords[k].ms / 1000 <= section.to) shifts[k] = section.semitones;
    r.transposition = chords.empty() ? base : shifts[0];
    for (size_t k = 0; k < chords.size(); ++k) {
        if (k == 0 || shifts[k] != shifts[k - 1]) r.sections.push_back({chords[k].ms / 1000, chords[k].msEnd / 1000, shifts[k]});
        else r.sections.back().to = chords[k].msEnd / 1000;
    }

    const auto comment = [&](std::string text) {
        StyledItem item; item.kind = Kind::Comment; item.text = std::move(text);
        r.items.push_back(std::move(item));
    };
    const auto lineBreak = [&] { StyledItem item; item.kind = Kind::Break; r.items.push_back(std::move(item)); };
    if (r.transposition != 0) comment("Transpose by: " + std::to_string(-r.transposition));

    // Beats elapsed at a time in seconds (the original counts ticks).
    const auto beatsAt = [&](double seconds) {
        double beats = 0, from = 0, bpm = o.missingBpm;
        for (const auto& mark : tempos) {
            if (mark.seconds >= seconds) break;
            beats += (mark.seconds - from) * bpm / 60;
            from = mark.seconds;
            bpm = mark.bpm;
        }
        return beats + (seconds - from) * bpm / 60;
    };

    struct Event { double seconds; int order; size_t index; };   // order: tempo, meter, note
    std::vector<Event> events;
    for (size_t i = 0; i < tempos.size(); ++i) events.push_back({tempos[i].seconds, 0, i});
    for (size_t i = 0; i < meters.size(); ++i) events.push_back({meters[i].seconds, 1, i});
    for (size_t i = 0; i < notes.size(); ++i) events.push_back({notes[i].seconds, 2, i});
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.seconds != b.seconds ? a.seconds < b.seconds : a.order < b.order;
    });

    // Deviation: the original initializes nextTempo to bpm * 4166.66, correct
    // only at 120 BPM. Here a beat is 60000 / BPM ms.
    double bpm = o.missingBpm, previousBpm = 0;
    bool haveTempo = false, scheduled = false;
    int numerator = 4;
    enum class Next { Start, Unset, Set } next = Next::Start;
    double nextBar = 0, penalty = 0;
    std::vector<detail::Placed> current;
    size_t currentChord = 0;
    const auto rests = detail::RestsBefore(chords);
    const double phraseGapMs = o.breaks == Breaks::Phrases ? detail::PhraseGapMs(rests) : std::numeric_limits<double>::infinity();

    const auto flush = [&] {
        if (current.empty()) return;
        bool quantized = false;
        for (size_t i = 1; i < current.size(); ++i)
            if (current[i].ms != current[i - 1].ms) { quantized = true; break; }
        std::vector<detail::Placed> chord;
        if (!quantized) {
            for (const auto& n : current) {
                if (std::any_of(chord.begin(), chord.end(), [&](const auto& kept) { return kept.midi == n.midi; })) { ++r.merged; continue; }
                chord.push_back(n);
            }
            chord = detail::SortChord(std::move(chord), o.classicChordOrder);
        } else if (o.sequentialQuantize) {
            chord = current;
            std::stable_sort(chord.begin(), chord.end(), [](const auto& a, const auto& b) { return a.ms < b.ms; });
        } else {
            chord = detail::SortChord(current, o.classicChordOrder);
        }
        auto item = detail::RenderChord(chord, quantized, o, r);
        item.ms = chord.front().ms;
        item.beatMs = chord.front().beatMs;
        item.msEnd = chord.front().ms;
        for (const auto& n : chord) item.msEnd = (std::max)(item.msEnd, n.ms);
        r.items.push_back(std::move(item));
        current.clear();
    };
    const auto checkBreak = [&](const detail::Placed& first) {
        if (o.breaks == Breaks::Bars || o.breaks == Breaks::Phrases) {
            const double beat = beatsAt(first.ms / 1000);
            if (scheduled) {
                scheduled = false;
                lineBreak();
                if (next != Next::Start) next = Next::Unset;
            }
            if (next != Next::Set) { nextBar = (next == Next::Unset ? beat : 0) + numerator; next = Next::Set; }
            if (beat + 1e-9 >= nextBar) { lineBreak(); nextBar += numerator; }
        } else if (o.breaks == Breaks::Beats) {
            const double normalized = first.ms - penalty;
            if (normalized + 0.5 >= first.beatMs * o.beats) { lineBreak(); penalty += normalized; }
        }
    };

    for (const auto& event : events) {
        if (event.order == 0) {
            const double newBpm = tempos[event.index].bpm;
            // Tempo changes break the line, as meter changes do.
            scheduled = true;
            if (o.bpmChanges) {
                if (!haveTempo) comment("Tempo: " + std::to_string(std::lround(newBpm)) + " BPM");
                else if (newBpm != previousBpm)
                    if (auto text = detail::BpmComment(std::lround(previousBpm), std::lround(newBpm), o.bpmStyle, o.minSpeedChange))
                        comment(*text);
            }
            haveTempo = true;
            previousBpm = bpm = newBpm;
            continue;
        }
        if (event.order == 1) {
            numerator = (std::max)(1, meters[event.index].numerator);
            scheduled = true;
            continue;
        }
        const auto& note = notes[event.index];
        // Chord membership comes from ChordIndices above. Emit a "Transpose by"
        // comment before a chord whose transposition differs from the previous
        // one, and a line break before one that follows a phrase's rest.
        const size_t k = chordOf[event.index];
        if (!current.empty() && k != currentChord) flush();
        if (current.empty()) {
            currentChord = k;
            if (k > 0 && rests[k] >= phraseGapMs) lineBreak();
            if (k > 0 && shifts[k] != shifts[k - 1]) comment("Transpose by: " + std::to_string(-shifts[k]));
        }
        auto placed = detail::Locate(note.seconds * 1000, note.midi + shifts[k], mapping, o);
        placed.beatMs = bpm > 0 ? 60000 / bpm : 500;
        current.push_back(placed);
        checkBreak(current.front());
    }
    flush();

    // Drop leading, trailing and repeated breaks, matching the original's combined export.
    std::vector<StyledItem> kept;
    for (auto& item : r.items) {
        if (item.kind == Kind::Break && (kept.empty() || kept.back().kind == Kind::Break)) continue;
        kept.push_back(std::move(item));
    }
    while (!kept.empty() && kept.back().kind == Kind::Break) kept.pop_back();
    r.items = std::move(kept);
    r.difficulty = Difficulty(r.items);

    std::vector<size_t> written;
    for (size_t i = 0; i < r.items.size(); ++i)
        if (r.items[i].kind == Kind::Chord) written.push_back(i);
    for (size_t c = 0; c < written.size(); ++c) {
        auto& item = r.items[written[c]];
        // Deviation: no separator after the last chord (the original writes its longest).
        if (c + 1 == written.size()) { item.rhythm = Rhythm::Long; item.separator.clear(); continue; }
        const double difference = r.items[written[c + 1]].ms - item.ms - 0.5;
        // Matches the original: 0.5 ms is subtracted twice for the colour, once for the separator.
        item.rhythm = detail::RhythmFor(item.beatMs, difference - 0.5);
        item.separator = o.tempoMarks ? detail::Separator(item.beatMs, difference) : " ";
    }

    std::string text;
    for (size_t i = 0; i < r.items.size(); ++i) {
        const auto& item = r.items[i];
        if (item.kind == Kind::Chord) {
            for (const auto& segment : item.segments) text += segment.text;
            text += item.separator;
        } else if (item.kind == Kind::Comment) {
            text += '\n' + item.text + '\n';
        } else {
            const bool nearComment = (i > 0 && r.items[i - 1].kind == Kind::Comment) ||
                (i + 1 < r.items.size() && r.items[i + 1].kind == Kind::Comment);
            if (!nearComment) text += '\n';
        }
    }
    r.text = detail::TidyLines(text);
    return r;
}

// The sheet's text as it is copied and saved: its difficulty on a line of its
// own above it, as its Transpose by line is. The page and the image carry the
// difficulty in their heading instead.
inline std::string SheetText(const StyledResult& r) {
    if (r.difficulty == 0) return r.text;
    return "Difficulty: " + std::to_string(r.difficulty) + " of 10" + (r.text.empty() ? "" : "\n" + r.text);
}

// A sustain pedal value (CC64) from one track. Tracks are merged by the
// deepest, as playback merges them.
struct PedalChange { double seconds = 0; int value = 0; int track = 0; };

// Where a pedal counts as down, and as all the way down: the MIDI on/off line,
// with half pedal from a quarter to it. Sampled from the owner's library on
// 2026-09-25 (563 files, five a folder): a press is held at 127, or 85 from a
// few converters, and time between 32 and 63 is at most 16% of pedalled time
// in any file, 0 to 5% in most.
inline constexpr int kPedalDown = 32, kPedalFull = 64;
// How long after a chord's onset a pedal pressed counts as pressed with it:
// pedalling after the chord, so its notes are caught and the last ones let
// go, is the usual way.
inline constexpr double kPedalCatchMs = 500;
// A half value held for less than this is the pedal passing through on its
// way down or up, not half pedal. In the owner's library (three files a
// folder, 2026-09-26) nearly every half value lasts under 50 ms, as a press or
// lift is recorded in steps; files played with half pedal hold it 100 ms to
// over a second. A chord struck mid-lift was drawn half pedalled.
inline constexpr double kPedalPassMs = 100;

namespace detail {
// The pedal over every track, each change of the deepest value in ms. A half
// value passed through keeps the pedal where it was until it settles.
inline std::vector<std::pair<double, int>> MergedPedal(std::vector<PedalChange> changes) {
    std::stable_sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
    std::map<int, int> tracks;
    std::vector<std::pair<double, int>> merged;   // ms, deepest of the tracks
    for (const auto& change : changes) {
        tracks[change.track] = change.value;
        int deepest = 0;
        for (const auto& [track, value] : tracks) deepest = (std::max)(deepest, value);
        if (deepest != (merged.empty() ? 0 : merged.back().second)) merged.push_back({change.seconds * 1000, deepest});
    }
    for (size_t k = 0; k + 1 < merged.size(); ++k)
        if (merged[k].second >= kPedalDown && merged[k].second < kPedalFull && merged[k + 1].first - merged[k].first < kPedalPassMs)
            merged[k].second = k ? merged[k - 1].second : 0;
    return merged;
}
} // namespace detail

// The notes as they sound, for RestsBefore: a key let go while the sustain
// pedal is down sounds on until the pedal lifts, so a rest under the pedal is
// none, and a pedal that never lifts holds it to the end. A note whose release
// is not known is left as it is. The page's heldByPedal must do the same.
inline std::vector<TimedNote> HeldByPedal(std::vector<TimedNote> notes, const std::vector<PedalChange>& changes) {
    const auto merged = detail::MergedPedal(changes);
    if (merged.empty()) return notes;
    for (auto& note : notes) {
        if (note.end < note.seconds) continue;
        const double ms = note.end * 1000;
        // Past the last change at or before the release.
        size_t at = 0, to = merged.size();
        while (at < to) { const size_t mid = (at + to) / 2; if (ms < merged[mid].first) to = mid; else at = mid + 1; }
        if (at == 0 || merged[at - 1].second < kPedalDown) continue;
        while (at < merged.size() && merged[at].second >= kPedalDown) ++at;
        note.end = at < merged.size() ? merged[at].first / 1000 : std::numeric_limits<double>::infinity();
    }
    return notes;
}

// Sets each chord's pedal marks. A chord is pedalled by the deepest the pedal
// goes from its onset until the next chord or kPedalCatchMs, whichever is
// sooner. The pedal is held from one pedalled chord to the next unless it came
// up and went down again between them.
// Does not change the text. The page's markPedals must do the same arithmetic.
inline void MarkPedals(StyledResult& r, std::vector<PedalChange> changes, const StyleOptions& o) {
    r.pedalMarks = PedalMarks::Off;
    if (o.pedalMarks == PedalMarks::Off || changes.empty()) return;
    r.pedalMarks = o.pedalMarks;
    const auto merged = detail::MergedPedal(std::move(changes));
    std::vector<size_t> chords;
    for (size_t i = 0; i < r.items.size(); ++i)
        if (r.items[i].kind == StyledItem::Kind::Chord) chords.push_back(i);
    std::vector<int> press(chords.size(), 0);
    size_t at = 0;
    int value = 0, presses = 0;
    const auto apply = [&] {
        if (value < kPedalDown && merged[at].second >= kPedalDown) ++presses;
        value = merged[at++].second;
    };
    for (size_t c = 0; c < chords.size(); ++c) {
        auto& item = r.items[chords[c]];
        const double onset = item.ms;
        const double end = c + 1 < chords.size() ? (std::min)(r.items[chords[c + 1]].ms, onset + kPedalCatchMs) : onset + kPedalCatchMs;
        while (at < merged.size() && merged[at].first <= onset) apply();
        int deepest = value;
        while (at < merged.size() && merged[at].first < end) { apply(); deepest = (std::max)(deepest, value); }
        item.pedal = deepest >= kPedalFull ? 2 : deepest >= kPedalDown ? 1 : 0;
        press[c] = presses;
    }
    for (size_t c = 0; c + 1 < chords.size(); ++c) {
        auto& item = r.items[chords[c]];
        item.pedalHeld = item.pedal != 0 && r.items[chords[c + 1]].pedal != 0 && press[c + 1] == press[c];
    }
}

// How a sheet is drawn: its size, colours, background and font, stored in the
// page data as "page" (the size) and "look" (the rest). The defaults are
// midi-converter's dark page. The page's THEMES[0] holds the same.
struct Look {
    enum Ground { Colour, Image };         // 2 was grain, read as Colour
    enum Fit { Cover, Tile };
    double fontSizePt = 10;
    double lineHeightPercent = 135;
    int theme = 0;                         // the page's theme: 0 dark, 2 white, 3 the user's own (1 was paper, read as 3)
    std::string background = "#2D2A32";
    Ground ground = Colour;
    std::string image;                     // a data URL, for Ground::Image
    Fit fit = Cover;
    int dim = 0;                           // percent of the background colour over the image
    bool oneColour = false;                // every chord in the text colour
    std::string text = "#ffffff";
    std::string comment = "#c8c4cc";
    std::string heading = "#aaa4b3";
    // Pedal highlight colours, in hues no note is drawn in: a blue for down and
    // a magenta for up, each a tint of the background so the notes read on it.
    std::string pedalDown = "#31406b";
    std::string pedalUp = "#6b3262";
    // By Rhythm: the original's palette, long notes green through short notes red.
    std::array<std::string, 9> rhythm{"#9c0f00", "#ff1900", "#daa6a6", "#da7e5a", "#c0c05a", "#9ada5a", "#74da74", "#a3f0a3", "white"};
    int font = 0;                          // an index of kFonts
};

// The fonts a sheet can use, all installed with Windows. The page's FONTS
// holds the same, in the same order; a new font goes at the end.
struct FontFace { const char* name; const char* css; };
inline constexpr FontFace kFonts[] = {
    {"Verdana", "Verdana,sans-serif"}, {"Segoe UI", "'Segoe UI',sans-serif"}, {"Consolas", "Consolas,monospace"},
    {"Arial", "Arial,sans-serif"}, {"Bahnschrift", "Bahnschrift,sans-serif"}, {"Calibri", "Calibri,sans-serif"},
    {"Cambria", "Cambria,serif"}, {"Candara", "Candara,sans-serif"}, {"Cascadia Mono", "'Cascadia Mono',monospace"},
    {"Comic Sans MS", "'Comic Sans MS',cursive"}, {"Constantia", "Constantia,serif"}, {"Corbel", "Corbel,sans-serif"},
    {"Courier New", "'Courier New',monospace"}, {"Franklin Gothic Medium", "'Franklin Gothic Medium',sans-serif"},
    {"Georgia", "Georgia,serif"}, {"Ink Free", "'Ink Free',cursive"}, {"Lucida Console", "'Lucida Console',monospace"},
    {"Palatino Linotype", "'Palatino Linotype',serif"}, {"Segoe Print", "'Segoe Print',cursive"},
    {"Sitka Text", "'Sitka Text',serif"}, {"Tahoma", "Tahoma,sans-serif"}, {"Times New Roman", "'Times New Roman',serif"},
    {"Trebuchet MS", "'Trebuchet MS',sans-serif"},
};
inline constexpr int kFontCount = static_cast<int>(std::size(kFonts));

// A colour the look accepts: #rrggbb, or "white" as the original writes it.
inline bool IsLookColour(const std::string& colour) {
    if (colour == "white") return true;
    if (colour.size() != 7 || colour[0] != '#') return false;
    for (size_t i = 1; i < 7; ++i) if (!std::isxdigit(static_cast<unsigned char>(colour[i]))) return false;
    return true;
}

namespace detail {
inline const Look& DefaultLook() { static const Look look; return look; }

inline const char* FontCss(int font) {
    return kFonts[font >= 0 && font < kFontCount ? font : 0].css;
}

inline const std::string& ChordColour(Rhythm rhythm, const Look& look) {
    return look.oneColour ? look.text : look.rhythm[static_cast<size_t>(rhythm)];
}

// The highlight behind a stretch of the sheet with the pedal at level (0 up,
// 1 part way, 2 down), as a style, or empty for none. Part way highlights the
// lower half of the line. The page's pedalHighlight writes the same.
inline std::string PedalHighlight(int level, PedalMarks marks, const Look& look = DefaultLook()) {
    const bool down = level > 0;
    if (marks == PedalMarks::Off || (down && marks == PedalMarks::Inverted) || (!down && marks == PedalMarks::Normal)) return {};
    const std::string& colour = down ? look.pedalDown : look.pedalUp;
    return level == 1 ? "background:linear-gradient(transparent 50%," + colour + " 50%)" : "background:" + colour;
}

// The line above a sheet's picture: its title and its difficulty. The page's
// heading writes the same.
inline std::string Heading(const std::string& title, int difficulty) {
    if (difficulty == 0) return title;
    return (title.empty() ? "" : title + " \xC2\xB7 ") + "Difficulty " + std::to_string(difficulty) + " of 10";
}

inline std::string WithPedalHighlight(const std::string& html, int level, PedalMarks marks, const Look& look) {
    const auto highlight = PedalHighlight(level, marks, look);
    return highlight.empty() || html.empty() ? html : "<span style=\"" + highlight + "\">" + html + "</span>";
}

// Sheet markup shared by ToHtml and the editor page. Under the separator the
// pedal is the chord's while it is held into the next chord, and up otherwise.
inline std::string SheetBody(const StyledResult& r, const Look& look = DefaultLook()) {
    using Kind = StyledItem::Kind;
    std::string html;
    for (size_t i = 0; i < r.items.size(); ++i) {
        const auto& item = r.items[i];
        if (item.kind == Kind::Chord) {
            html += "<span style=\"color:" + ChordColour(item.rhythm, look) + "\">";
            std::string chord;
            for (const auto& segment : item.segments) {
                if (segment.outOfRange)
                    chord += "<span style=\"display:inline-flex;justify-content:center;min-width:0.6em;"
                             "border-bottom:2px solid;font-weight:900\">" + detail::EscapeHtml(segment.text) + "</span>";
                else chord += detail::EscapeHtml(segment.text);
            }
            html += WithPedalHighlight(chord, item.pedal, r.pedalMarks, look);
            html += WithPedalHighlight(detail::EscapeHtml(item.separator), item.pedalHeld ? item.pedal : 0, r.pedalMarks, look);
            html += "</span>";
        } else if (item.kind == Kind::Comment) {
            html += "<br><span style=\"color:" + look.comment + "\">" + detail::EscapeHtml(item.text) + "</span><br>";
        } else {
            const bool nearComment = (i > 0 && r.items[i - 1].kind == Kind::Comment) ||
                (i + 1 < r.items.size() && r.items[i + 1].kind == Kind::Comment);
            if (!nearComment) html += "<br>";
        }
    }
    return html;
}
} // namespace detail

// The header above a sheet: which lines show, how they sit, and the arranger,
// who is the same on every sheet. Title and difficulty by default, as before.
struct Header {
    bool title = true, subtitle = false, artist = false, arranger = false, tempo = false, key = false, difficulty = true, date = false;
    bool centre = false;
    std::string arrangerName;
};
// A song's own header text. Sheet files written for a library leave all but
// the title (the file's name) and the date (today) empty.
struct SongText { std::string title, subtitle, artist, date; };
// What the header says of the music: its first tempo and the key found.
struct HeaderFacts { double bpm = 0; std::string key; };

// Today as the header writes a date: 30 September 2026.
inline std::string Today() {
    static const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return std::to_string(local.tm_mday) + " " + months[local.tm_mon] + " " + std::to_string(local.tm_year + 1900);
}

// The key by Krumhansl and Schmuckler: the major or minor profile that best
// matches how often each pitch class is played. The page's findKey does the same.
inline std::string FindKey(const std::vector<TimedNote>& notes) {
    if (notes.empty()) return {};
    static const char* names[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    static const double major[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const double minor[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    double counts[12] = {};
    for (const auto& note : notes) counts[((note.midi % 12) + 12) % 12] += 1;
    const auto correlation = [&](const double* profile, int tonic) {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += counts[(i + tonic) % 12]; my += profile[i]; }
        mx /= 12; my /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i) {
            const double x = counts[(i + tonic) % 12] - mx, y = profile[i] - my;
            sxy += x * y; sxx += x * x; syy += y * y;
        }
        return sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : 0.0;
    };
    double best = -2;
    std::string key;
    for (int tonic = 0; tonic < 12; ++tonic)
        for (int minorKey = 0; minorKey < 2; ++minorKey) {
            const double c = correlation(minorKey ? minor : major, tonic);
            if (c > best) { best = c; key = std::string(names[tonic]) + (minorKey ? " minor" : " major"); }
        }
    return key;
}

namespace detail {
inline std::string Joined(const std::vector<std::string>& parts, const char* separator) {
    std::string out;
    for (const auto& part : parts) out += (out.empty() ? "" : separator) + part;
    return out;
}
// The header's lines after the first, which the picture and the text share:
// subtitle, artist and arranger, tempo and key, date. The page's
// headerLines writes the same.
inline std::vector<std::string> HeaderLines(const StyledResult& r, const Header& h, const SongText& song, const HeaderFacts& facts) {
    std::vector<std::string> lines;
    if (h.subtitle && !song.subtitle.empty()) lines.push_back(song.subtitle);
    std::vector<std::string> people;
    if (h.artist && !song.artist.empty()) people.push_back(song.artist);
    if (h.arranger && !h.arrangerName.empty()) people.push_back("Arranged by " + h.arrangerName);
    if (!people.empty()) lines.push_back(Joined(people, " \xC2\xB7 "));
    std::vector<std::string> music;
    if (h.tempo && facts.bpm > 0) music.push_back(std::to_string(static_cast<long long>(std::floor(facts.bpm + 0.5))) + " BPM");
    if (h.key) {
        if (!facts.key.empty()) music.push_back(facts.key);
        if (r.sections.size() > 1) music.push_back("Transposed in " + std::to_string(r.sections.size()) + " sections");
        // As the sheet's own Transpose by line says it: what the game is set to.
        else if (r.transposition) music.push_back("Transpose by " + std::string(r.transposition < 0 ? "+" : "") + std::to_string(-r.transposition));
    }
    if (!music.empty()) lines.push_back(Joined(music, " \xC2\xB7 "));
    if (h.date && !song.date.empty()) lines.push_back(song.date);
    return lines;
}
} // namespace detail

// The header over the picture and the page: the title and difficulty on the
// first line, as before, then the rest. The page's headingLines writes the same.
inline std::vector<std::string> HeadingLines(const StyledResult& r, const Header& h, const SongText& song, const HeaderFacts& facts) {
    std::vector<std::string> lines;
    const auto first = detail::Heading(h.title ? song.title : "", h.difficulty ? r.difficulty : 0);
    if (!first.empty()) lines.push_back(first);
    for (auto& line : detail::HeaderLines(r, h, song, facts)) lines.push_back(std::move(line));
    return lines;
}

// The sheet's text with its header: a title of the song's own (not the file's
// name), the other lines, then the difficulty, as SheetText(r) writes it. The
// page's headerText writes the same.
inline std::string SheetText(const StyledResult& r, const Header& h, const SongText& song, const HeaderFacts& facts, const std::string& fileName) {
    std::vector<std::string> lines;
    if (h.title && !song.title.empty() && song.title != fileName) lines.push_back(song.title);
    for (auto& line : detail::HeaderLines(r, h, song, facts)) lines.push_back(std::move(line));
    if (h.difficulty && r.difficulty) lines.push_back("Difficulty: " + std::to_string(r.difficulty) + " of 10");
    const auto head = detail::Joined(lines, "\n");
    if (head.empty()) return r.text;
    return head + (r.text.empty() ? "" : "\n" + r.text);
}

// Self-contained HTML page in the look, the original's dark page by default,
// out-of-range notes bold and underlined. title sets the <title> (the MIDI
// file's stem from the app).
inline std::string ToHtml(const StyledResult& r, const std::string& title = "Sheet", const Look& look = detail::DefaultLook()) {
    std::string background = "background:" + look.background;
    if (look.ground == Look::Image && !look.image.empty()) {
        const auto shade = "color-mix(in srgb," + look.background + " " + std::to_string(std::clamp(look.dim, 0, 100)) + "%,transparent)";
        background += ";background-image:linear-gradient(" + shade + "," + shade + "),url(" + look.image + ")" +
                      (look.fit == Look::Tile ? ";background-repeat:repeat" : ";background-size:cover;background-position:center");
    }
    char size[64];
    snprintf(size, sizeof(size), "font-size:%gpt;line-height:%g%%", look.fontSizePt, look.lineHeightPercent);
    return "<!doctype html><meta charset=\"utf-8\"><title>" + detail::EscapeHtml(title) + "</title>"
        "<body style=\"margin:16px;" + background + ";color:" + look.text + ";font-family:" + detail::FontCss(look.font) + ";" +
        size + "\"><div style=\"white-space:pre-wrap\">" + detail::SheetBody(r, look) + "</div></body>";
}

} // namespace sheet
