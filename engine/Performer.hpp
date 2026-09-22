#pragma once
// Performer add-on ABI. A performer turns the score into a take: the same notes
// with changed timing and velocity, some skipped, some struck on a neighbouring
// key, some added. While the clock is stopped it also maps user taps to notes.
// With no performer the take is the score. The interface is plain C with JSON
// only for settings, because a take can be rebuilt between two notes of a
// playing song.
#include <cstddef>
#include <cstdint>

extern "C" {

struct qm_score_event {
    int64_t time;        // nanoseconds of score time
    int32_t pitch;       // MIDI note, or -1 for the pedal
    int32_t velocity;
    int32_t track;
    int32_t mate;        // index of the paired release (on a press) or press (on a release), or -1
    uint8_t press;
    uint8_t skip;        // always 0; the performer decides what the take skips
};

// One event of a take, in take order.
struct qm_take_event {
    int64_t time;        // displaced time, ns
    int64_t hold;        // on a press, ns until its release, or -1
    int32_t score;       // index of the score event, or -1 for an added key
    int32_t pitch;       // key struck: the score's pitch or a neighbour
    int32_t velocity;
    int32_t track;
    int32_t step;        // index of the score chord this press belongs to; one tap plays one step
    uint8_t press;
    uint8_t skip;
};

struct qm_tap { int32_t key; uint8_t down; };

// Callbacks the performer uses during step(). `player` is passed back unchanged.
struct qm_player {
    void* player;
    void (*fire)(void* player, size_t take_index, int32_t pitch);   // strike a take event on this key
    void (*release)(void* player, int32_t pitch, int32_t track);
    uint64_t (*now_ns)(void* player);                               // monotonic wall clock
    int (*stopping)(void* player);
};

enum { QM_STEP_PLAYING = 0, QM_STEP_HOLDING = 1, QM_STEP_ENDED = 2 };

// Exported under these names. `settings` is the add-on's JSON section in the
// form the app saves it.
struct qm_performer_api {
    void* (*create)(void);
    void (*destroy)(void* performer);
    // The performer owns *events until the next build or destroy. With on == 0
    // the take is the score in the form step() reads. Returning fewer events
    // than the score has is a failure; the player then plays the score.
    size_t (*build)(void* performer, int on, const char* settings, const qm_score_event* score, size_t count,
                    uint64_t seed, double speed, const qm_take_event** events);
    // Starts a new playthrough: no keys held, no taps seen.
    void (*reset)(void* performer, uint64_t seed);
    // Called while the clock is stopped. Plays what the taps request from
    // *cursor on, advances *cursor and *position, and returns a QM_STEP_* value.
    int (*step)(void* performer, const qm_player* player, const qm_tap* taps, size_t count,
                size_t* cursor, int64_t* position, double speed, int holds);
    // Releases every key a tap still holds.
    void (*lift)(void* performer, const qm_player* player);
    // Value for the estimate named `what`, or negative when there is none.
    double (*estimate)(const char* what, const qm_score_event* score, size_t count, double speed);
    // JSON describing the settings section the app draws and saves:
    //   name, tag; config: key in config.json holding the values;
    //   presets: id, field, choices[] of {name, values} (slider targets);
    //   controls: in draw order; triggers.
    // Control: id (key in `settings`), field (key when saved), type (slider
    //   0..1, switch or choice), name, value (initial).
    //   choice: `choices` (names), value is the chosen index; `panel` draws it
    //     on the main panel beside the triggers.
    //   slider: `estimate` starts each song at estimate()'s value for that name
    //     (negative while the estimate is in use; a negative answer hides the
    //     slider); `shows: "note"` displays it as a note between `low` and `high`.
    //   shown_with: drawn only while the named choice is off its first entry.
    //   switch: `estimates` names the slider whose estimate it disables,
    //     `remembers` keeps slider values per song, `holds` is step()'s argument.
    // Trigger: the first is the clock. Others either `plays` (while the key is
    //   down) or `steps` (through step(), one tap per key), with `keys` (name,
    //   add, a config field and a default each) and their own `controls`.
    const char* (*describe)(void);
};

}

#include <unordered_map>
#include <vector>
// Pairs a score in place: a release closes the latest open press of its pitch and track.
inline void PairScore(std::vector<qm_score_event>& score) {
    std::unordered_map<int64_t, std::vector<int32_t>> open;
    for (size_t i = 0; i < score.size(); ++i) {
        auto& e = score[i];
        e.mate = -1;
        if (e.pitch < 0) continue;
        auto& stack = open[(static_cast<int64_t>(e.track) << 8) | e.pitch];
        if (e.press) stack.push_back(static_cast<int32_t>(i));
        else if (!stack.empty()) {
            e.mate = stack.back();
            score[stack.back()].mate = static_cast<int32_t>(i);
            stack.pop_back();
        }
    }
}
