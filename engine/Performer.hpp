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
    int32_t pitch;       // MIDI note, or -1 for a pedal
    // A note's velocity. A pedal's value, 0 to 127, with which pedal it is in
    // bits 12 and 13 (QM_PEDAL_*); estimate()'s score also carries the MIDI
    // channel in bits 8 to 11, and build()'s leaves them 0. A pedal's press is
    // its value at or over the sustain cutoff.
    int32_t velocity;
    int32_t track;
    int32_t mate;        // index of the paired release (on a press) or press (on a release), or -1
    uint8_t press;
    // build(): 1 for what nobody will hear, which the performer leaves out of
    // what it counts: a note or pedal of a muted track, or of one not soloed
    // while another is, and on keystrokes a note with no key after Transpose
    // and fold whose octave doubles have none either. A mute, a solo or a
    // change of output rebuilds the take. estimate(): always 0. Either way the
    // performer decides what the take skips.
    uint8_t skip;
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
// Which pedal a score event is: (velocity >> 12) & 3.
enum { QM_PEDAL_SUSTAIN = 0, QM_PEDAL_SOSTENUTO = 1, QM_PEDAL_SOFT = 2 };

// Exported under these names. `settings` is the add-on's JSON section in the
// form the app saves it, plus the host's own `sustainHeard`: false while the
// app sends no sustain pedal (Sustain off), so a key let go early would cut its
// note. A change to it rebuilds the take like any setting.
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
    //   presets: id, field, name (drawn above them, optional), choices[] of
//   {name, values} (slider targets);
    //   controls: in draw order; triggers.
    // Control: id (key in `settings`), field (key when saved), type (slider
    //   0..1, switch or choice), name, value (initial).
    //   choice: `choices` (names), value is the chosen index; `panel` draws it
    //     on the main panel beside the triggers; `estimate` names an estimate
    //     whose negative answer hides the choice for that song, which is then
    //     sent as its first entry and keeps the value chosen for other songs.
    //   slider: `estimate` starts each song at estimate()'s value for that name
    //     (negative while the estimate is in use; a negative answer hides the
    //     slider); `shows: "note"` displays it as a note between `low` and `high`.
    //   shown_with: drawn only while the named choice is drawn and off its
    //     first entry.
    //   switch: `estimates` names the slider whose estimate it disables,
    //     `remembers` keeps slider values per song, `holds` is step()'s argument.
    // Trigger: the first is the clock. Others either `plays` (while the key is
    //   down) or `steps` (through step(), one tap per key), with `keys` (name,
    //   add, a config field and a default each) and their own `controls`.
    // actions: keys that work whatever the trigger, while the song plays and
    //   the section is on: id (passed to qm_performer_action), name (its row in
    //   Hotkeys), field (key in HOTKEY_SETTINGS) and default (a key name, or
    //   empty for none). They take their places after the triggers' keys.
    //   `schedules`: true for an action that plays through the take, see
    //   qm_performer_action.
    const char* (*describe)(void);
};

// Optional, exported as `qm_performer_action`, for the section's `actions`
// (see describe). Called on the playback thread when an action's key is
// pressed while the song plays, whatever the trigger, with `position` the score
// time the song's clock stands at and the take last built. Writes what the
// action plays to *events and returns how many; 0 plays nothing. Each is a key
// the take adds (score -1) with its pitch, velocity and track, `time` in
// nanoseconds of wall clock after the key, and a press is followed by its
// release. The host holds the song's clock while they play, strikes them in
// time order, lets go of any still down when playback stops or pauses, then
// runs the song on from where it stood. The performer owns *events until the
// next call, build or destroy.
// An action that `schedules` is called the same way but with the clock
// running, and returns keys at score times from `position` on, as the take's
// are, with a press's `hold`: a fill for a gap still to come. The host puts
// them in the take and plays them there as it plays the take's own keys, and
// the song's own events stay where they were. A seek, a loop's wrap, a stop
// or a rebuilt take drops what has not played; until the last of them is due
// the action's key asks for nothing more. Returning 0 plays nothing, and a
// run that begins before `position` is not played. It is not called in Tap,
// where the clock stands still.
typedef size_t qm_performer_action_call(void* performer, const char* id, int64_t position, double speed,
                                        const qm_take_event** events);

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
