#pragma once

// SheetPage: a self-contained HTML sheet editor.
//
// The page embeds the notes, key mapping, tempo and meter marks and style
// options, plus a JavaScript port of sheet::Style that redraws on every
// settings change. It also embeds the C++ rendering's text and compares its
// first render against it to detect drift between the two implementations;
// tests/sheet-page-parity.js runs the same check over the ShellTests fixtures.
//
// Header only, no project dependencies.

#include "SheetExport.hpp"
#include "SheetImage.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace sheet {

// A user-marked span in seconds, both ends inclusive. NotesShifted changes the
// notes' pitch silently; Transpose becomes a Section with a "Transpose by" comment.
struct Region {
    enum class Kind { NotesShifted, Transpose };
    double from = 0; double to = 0; int semitones = 0; Kind kind = Kind::NotesShifted;
};

struct PageInput {
    std::string title;                             // MIDI file stem
    std::vector<TimedNote> notes;                  // regions not yet applied
    std::map<std::string, std::string> mapping;
    std::vector<TempoMark> tempos;
    std::vector<MeterMark> meters;
    StyleOptions options;
    std::vector<Region> regions;
    Look look;
};

namespace detail {

inline std::string JsonString(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        // Escape '/' so "</script>" in the data cannot close the script element.
        case '/': out += "\\/"; break;
        default:
            if (c < 0x20) { char buffer[8]; snprintf(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
            else out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

inline std::string JsonNumber(double value) {
    if (!(value == value) || value > 1e300 || value < -1e300) return "0";
    char buffer[40]; snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

inline std::string JsonOptions(const StyleOptions& o) {
    std::string j = "{";
    const auto field = [&](const char* name, const std::string& value) { if (j.size() > 1) j += ","; j += std::string("\"") + name + "\":" + value; };
    const auto flag = [&](bool b) { return std::string(b ? "true" : "false"); };
    field("quantizeMs", JsonNumber(o.quantizeMs));
    field("sequentialQuantize", flag(o.sequentialQuantize));
    field("curlyQuantizes", flag(o.curlyQuantizes));
    field("classicChordOrder", flag(o.classicChordOrder));
    field("shifts", std::to_string(static_cast<int>(o.shifts)));
    field("outOfRangePlace", std::to_string(static_cast<int>(o.outOfRangePlace)));
    field("showOutOfRange", flag(o.showOutOfRange));
    field("outOfRangeMarks", flag(o.outOfRangeMarks));
    field("outOfRangeSeparator", JsonString(o.outOfRangeSeparator));
    field("tempoMarks", flag(o.tempoMarks));
    field("bpmChanges", flag(o.bpmChanges));
    field("bpmStyle", std::to_string(static_cast<int>(o.bpmStyle)));
    field("minSpeedChange", std::to_string(o.minSpeedChange));
    field("breaks", std::to_string(static_cast<int>(o.breaks)));
    field("beats", std::to_string(o.beats));
    field("missingBpm", JsonNumber(o.missingBpm));
    field("transpose", std::to_string(o.transpose));
    field("autoTranspose", flag(o.autoTranspose));
    field("resilience", std::to_string(o.resilience));
    field("autoSections", flag(o.autoSections));
    field("sectionSwitchCost", std::to_string(o.sectionSwitchCost));
    field("sectionMinSeconds", JsonNumber(o.sectionMinSeconds));
    field("sectionRestMs", std::to_string(o.sectionRestMs));
    field("sectionRange", std::to_string(o.sectionRange));
    return j + "}";
}

// Contents of the sheet-data element: the script's inputs plus the C++ text
// ("expected") for the parity check.
inline std::string PageJson(const PageInput& in, const std::string& expectedText) {
    std::string j = "{\"title\":" + JsonString(in.title) + ",\"notes\":[";
    for (size_t i = 0; i < in.notes.size(); ++i)
        j += (i ? "," : "") + std::string("[") + JsonNumber(in.notes[i].seconds) + "," + std::to_string(in.notes[i].midi) + "]";
    j += "],\"mapping\":{";
    bool first = true;
    for (const auto& [name, key] : in.mapping) {
        j += (first ? "" : ",") + JsonString(name) + ":" + JsonString(key);
        first = false;
    }
    j += "},\"tempos\":[";
    for (size_t i = 0; i < in.tempos.size(); ++i)
        j += (i ? "," : "") + std::string("[") + JsonNumber(in.tempos[i].seconds) + "," + JsonNumber(in.tempos[i].bpm) + "]";
    j += "],\"meters\":[";
    for (size_t i = 0; i < in.meters.size(); ++i)
        j += (i ? "," : "") + std::string("[") + JsonNumber(in.meters[i].seconds) + "," + std::to_string(in.meters[i].numerator) + "]";
    j += "],\"regions\":[";
    for (size_t i = 0; i < in.regions.size(); ++i)
        j += (i ? "," : "") + std::string("[") + JsonNumber(in.regions[i].from) + "," + JsonNumber(in.regions[i].to) + "," +
             std::to_string(in.regions[i].semitones) + "," + std::to_string(static_cast<int>(in.regions[i].kind)) + "]";
    j += "],\"options\":" + JsonOptions(in.options) + ",\"page\":{\"fontSize\":" + JsonNumber(in.look.fontSizePt) +
         ",\"lineHeight\":" + JsonNumber(in.look.lineHeightPercent) + "},\"expected\":" + JsonString(expectedText) + "}";
    return j;
}

// The sheet uses midi-converter's background and Verdana; the settings column
// follows the app's spacing.
inline constexpr const char* kPageCss = R"css(
:root{--bg:#2D2A32;--side:#232027;--ink:#f2eff5;--muted:#aaa4b3;--line:rgba(255,255,255,.1);--field:#332f39;--accent:#7cc0ff}
*{box-sizing:border-box}
html,body{margin:0;height:100%}
body{background:var(--bg);color:var(--ink);font-family:Segoe UI,system-ui,sans-serif;font-size:13px;display:flex;flex-direction:column}
header{display:flex;align-items:center;gap:12px;padding:10px 16px;border-bottom:1px solid var(--line);background:var(--side)}
header h1{font-size:15px;font-weight:600;margin:0;flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
header .count{color:var(--muted);white-space:nowrap}
button{font:inherit;color:var(--ink);background:var(--field);border:1px solid var(--line);border-radius:6px;padding:6px 12px;cursor:pointer}
button:hover{border-color:rgba(255,255,255,.3)}
button.primary{background:var(--accent);color:#10131a;border-color:transparent;font-weight:600}
button.small{padding:2px 8px}
#layout{flex:1;display:flex;min-height:0}
aside{width:300px;flex:none;overflow:auto;padding:8px 16px 24px;border-right:1px solid var(--line);background:var(--side)}
main{flex:1;overflow:auto;padding:16px}
#sheet{white-space:pre-wrap;font-family:Verdana,sans-serif;font-size:10pt;line-height:135%;color:#fff}
#sheet .oor{display:inline-flex;justify-content:center;min-width:.6em;border-bottom:2px solid;font-weight:900}
#sheet .comment{color:#c8c4cc}
#sheet .chord.in-section{background:rgba(124,192,255,.16);border-radius:2px}
h2{font-size:11px;font-weight:600;color:var(--muted);text-transform:none;margin:20px 0 6px;letter-spacing:.02em}
label.row{display:flex;align-items:center;justify-content:space-between;gap:12px;min-height:30px}
label.col{display:block;margin:6px 0 10px}
label.col span{display:block;margin-bottom:4px}
p.note{color:var(--muted);margin:0 0 8px;font-size:12px}
input[type=text],input[type=number],select{font:inherit;color:var(--ink);background:var(--field);border:1px solid var(--line);border-radius:6px;padding:5px 8px;width:100%}
input[type=number]{width:90px}
input[type=range]{width:100%;accent-color:var(--accent)}
.range{display:flex;align-items:center;gap:8px}
.range output{min-width:64px;text-align:right;color:var(--muted)}
input[type=checkbox]{width:16px;height:16px;accent-color:#3fb950;margin:0}
#sections li{display:flex;align-items:center;gap:8px;list-style:none;padding:4px 0}
#sections{margin:0;padding:0}
#sections .empty{color:var(--muted)}
#selection{position:fixed;display:none;align-items:center;gap:8px;padding:8px 10px;background:var(--side);border:1px solid var(--line);border-radius:8px;box-shadow:0 8px 24px rgba(0,0,0,.4);z-index:2}
#selection output{min-width:2.5em;text-align:center}
#parity{display:none;margin:0 0 12px;padding:8px 12px;border:1px solid #c66;border-radius:6px;color:#f4b7b7}
#parity.shown{display:block}
@media print{header,aside,#selection,#parity{display:none!important}#layout{display:block}main{overflow:visible;padding:0}body{background:#2D2A32;-webkit-print-color-adjust:exact;print-color-adjust:exact}}
)css";

// JavaScript port of sheet::Style. Functions mirror SheetExport.hpp by name and
// perform the same arithmetic in the same order so results match exactly in
// double precision. Must not touch the DOM: tests/sheet-page-parity.js loads it
// standalone.
inline constexpr const char* kPageCore = R"js(/*SHEET-CORE-START*/
const SheetCore = (() => {
const CAPS = "!@#$%^&*()QWERTYUIOPASDFGHJKLZXCBVNM";
const LOWER = "1234567890qwertyuiopasdfghjklzxcvbnm";
const LOW_OOR = "1234567890qwert", HIGH_OOR = "yuiopasdfghj";
const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const COLOURS = ["#9c0f00", "#ff1900", "#daa6a6", "#da7e5a", "#c0c05a", "#9ada5a", "#74da74", "#a3f0a3", "white"];
const CHORD = 0, BREAK = 1, COMMENT = 2, LONG = 8;
function defaults() {
  return {quantizeMs: 35, sequentialQuantize: true, curlyQuantizes: true, classicChordOrder: false, shifts: 0,
          outOfRangePlace: 2, showOutOfRange: true, outOfRangeMarks: false, outOfRangeSeparator: ":", tempoMarks: false,
          bpmChanges: true, bpmStyle: 0, minSpeedChange: 10, breaks: 0, beats: 4, missingBpm: 120, transpose: 0,
          autoTranspose: false, resilience: 2, autoSections: false, sectionSwitchCost: 12, sectionMinSeconds: 8,
          sectionRestMs: 250, sectionRange: 12};
}
function noteName(midi) { return NAMES[midi % 12] + (Math.floor(midi / 12) - 1); }
function oneOf(character, set) { return character.length === 1 && set.indexOf(character) >= 0; }
function locate(ms, midi, mapping, o) {
  const p = {ms, beatMs: 500, midi, character: "", valid: midi >= 21 && midi <= 108, outOfRange: midi <= 35 || midi >= 97, display: midi};
  if (p.valid) {
    const name = noteName(midi);
    const found = Object.prototype.hasOwnProperty.call(mapping, name) ? mapping[name] : "";
    if (found) p.character = found.startsWith("ctrl+") ? found.slice(5) : found;
    else if (midi <= 35) p.character = LOW_OOR[midi - 21];
    else if (midi >= 97) p.character = HIGH_OOR[midi - 97];
    else p.valid = false;
  }
  if (oneOf(p.character, CAPS)) {
    if (o.shifts === 0) p.display = midi - 108;
    else if (o.shifts === 1) p.display = midi + 108;
  } else if (p.outOfRange) {
    if (o.outOfRangePlace === 0) p.display = midi - 1024;
    else if (o.outOfRangePlace === 1) p.display = midi + 1024;
    else p.display = oneOf(p.character, LOW_OOR) ? midi - 1024 : midi + 1024;
  }
  return p;
}
function classicOrder(notes) {
  notes = notes.slice().sort((a, b) => a.midi - b.midi);
  const start = [], end = [], numeric = [], upper = [], lower = [];
  let last = null;
  for (const n of notes) {
    if (n.outOfRange) {
      if (n.display === n.midi - 1024) start.push(n);
      else if (n.display === n.midi + 1024) end.push(n);
      continue;
    }
    if (n.character.length === 1 && n.character >= "0" && n.character <= "9") numeric.push(n);
    else if (oneOf(n.character, CAPS)) upper.push(n);
    else lower.push(n);
    last = n;
  }
  let out = start.slice();
  if (last) out = out.concat(oneOf(last.character, CAPS) ? numeric.concat(lower, upper) : upper.concat(numeric, lower));
  return out.concat(end);
}
function sortChord(notes, classic) {
  return classic ? classicOrder(notes) : notes.slice().sort((a, b) => a.display - b.display);
}
function item(kind) { return {kind, segments: [], text: "", separator: "", rhythm: LONG, ms: 0, beatMs: 500, msEnd: 0}; }
function renderChord(chord, quantized, o, r) {
  const it = item(CHORD);
  const nonOutOfRange = chord.filter(n => !n.outOfRange).length;
  let isChord = chord.length > 1 && chord.some(n => n.valid);
  if (!o.showOutOfRange && nonOutOfRange <= 1) isChord = false;
  const curly = quantized && o.curlyQuantizes;
  let firstStart = null, lastStart = null, firstEnd = null;
  for (const n of chord) {
    if (!n.outOfRange) continue;
    if (n.display === n.midi - 1024) { if (!firstStart) firstStart = n; lastStart = n; }
    else if (n.display === n.midi + 1024 && !firstEnd) firstEnd = n;
  }
  if (isChord) it.segments.push({text: curly ? "{" : "[", oor: false});
  for (const n of chord) {
    if (!n.valid) { it.segments.push({text: "_", oor: false}); r.unmapped++; continue; }
    const drawOor = n.outOfRange && o.showOutOfRange;
    if (drawOor) {
      const marked = n === firstStart || (!isChord && n === firstEnd) ||
        (isChord && nonOutOfRange === 0 && !firstStart && n === firstEnd) ||
        (isChord && nonOutOfRange > 0 && n === firstEnd);
      let text = n.character;
      if (o.outOfRangeMarks && marked) text = o.outOfRangeSeparator + text;
      it.segments.push({text, oor: true});
      r.notes++;
      if (o.outOfRangeMarks && n === lastStart && nonOutOfRange > 0) it.segments.push({text: "'", oor: false});
    } else if (!n.outOfRange) {
      it.segments.push({text: n.character, oor: false});
      r.notes++;
    } else {
      r.hidden++;
    }
  }
  if (isChord) it.segments.push({text: curly ? "}" : "]", oor: false});
  r.groups++;
  return it;
}
function transpositionScore(notes, by, mapping) {
  let good = 0, lower = 0, upper = 0;
  for (const note of notes) {
    const p = locate(0, note.midi + by, mapping, defaults());
    if (p.outOfRange || !p.valid) continue;
    good++;
    if (oneOf(p.character, LOWER)) lower++; else upper++;
  }
  return good * 2 + Math.abs(upper - lower);
}
function bestTransposition(notes, mapping, stickTo, resilience) {
  let best = transpositionScore(notes, stickTo, mapping);
  let bests = [stickTo];
  const consider = n => {
    const score = transpositionScore(notes, n, mapping);
    if (score > best + resilience) { best = score; bests = [n]; }
    else if (score === best) {
      if (n === 0 && bests.indexOf(0) >= 0) return;
      bests.push(n);
    }
  };
  for (let i = stickTo; i <= stickTo + 11; ++i) { consider(i); consider(-i); }
  return bests[0];
}
function chordIndices(notes, quantizeMs) {
  const indices = new Array(notes.length).fill(0);
  let chord = 0, last = 0;
  for (let i = 0; i < notes.length; ++i) {
    const ms = notes[i].seconds * 1000;
    if (i > 0 && !(Math.abs(ms - last) < quantizeMs)) ++chord;
    indices[i] = chord;
    last = ms;
  }
  return indices;
}
function bestSections(chords, mapping, o) {
  const n = chords.length;
  const candidates = [o.transpose];
  for (let d = 1; d <= o.sectionRange; ++d) { candidates.push(o.transpose - d); candidates.push(o.transpose + d); }
  const T = candidates.length;
  const cost = 2 * o.sectionSwitchCost;
  const minMs = o.sectionMinSeconds * 1000, restMs = o.sectionRestMs;
  const NONE = -Infinity;
  const prefix = candidates.map(() => new Array(n + 1).fill(0));
  for (let t = 0; t < T; ++t)
    for (let k = 0; k < n; ++k) prefix[t][k + 1] = prefix[t][k] + transpositionScore(chords[k].notes, candidates[t], mapping);
  const closed = [], open = [], from = [], before = [];
  for (let i = 0; i < n; ++i) { closed.push(new Array(T).fill(NONE)); open.push(new Array(T).fill(NONE)); from.push(new Array(T).fill(0)); before.push(new Array(T).fill(0)); }
  const running = new Array(T).fill(NONE), runningFrom = new Array(T).fill(0);
  let admit = 0;
  for (let i = 0; i < n; ++i) {
    if (i === 0) {
      for (let t = 0; t < T; ++t) open[0][t] = 0;
    } else if (chords[i].ms - chords[i - 1].msEnd >= restMs) {
      let bestAt = 0, secondAt = 0, best = NONE, second = NONE;
      for (let t = 0; t < T; ++t) {
        const value = closed[i - 1][t];
        if (value > best) { second = best; secondAt = bestAt; best = value; bestAt = t; }
        else if (value > second) { second = value; secondAt = t; }
      }
      for (let t = 0; t < T; ++t) {
        const other = t === bestAt ? second : best;
        if (other === NONE) continue;
        open[i][t] = other - cost;
        before[i][t] = t === bestAt ? secondAt : bestAt;
      }
    }
    while (admit <= i && chords[i].ms - chords[admit].ms >= minMs) {
      for (let t = 0; t < T; ++t) {
        if (open[admit][t] === NONE) continue;
        const value = open[admit][t] - prefix[t][admit];
        if (value > running[t]) { running[t] = value; runningFrom[t] = admit; }
      }
      ++admit;
    }
    for (let t = 0; t < T; ++t) {
      if (running[t] === NONE) continue;
      closed[i][t] = prefix[t][i + 1] + running[t];
      from[i][t] = runningFrom[t];
    }
    if (i + 1 === n && admit === 0)
      for (let t = 0; t < T; ++t) { closed[i][t] = prefix[t][n]; from[i][t] = 0; }
  }
  const shifts = new Array(n).fill(o.transpose);
  if (n === 0) return shifts;
  let t = 0;
  for (let u = 1; u < T; ++u) if (closed[n - 1][u] > closed[n - 1][t]) t = u;
  for (let end = n; end > 0;) {
    const start = from[end - 1][t];
    for (let k = start; k < end; ++k) shifts[k] = candidates[t];
    if (start === 0) break;
    t = before[start][t];
    end = start;
  }
  return shifts;
}
function bpmComment(previous, next, style, minimum) {
  const faster = next > previous;
  const larger = faster ? next : previous, smaller = faster ? previous : next;
  if (smaller <= 0) return null;
  const percent = Math.round((larger - smaller) / smaller * 100);
  if (percent < minimum) return null;
  const word = faster ? "faster" : "slower";
  if (style === 1) {
    const mark = faster ? ">" : "<";
    const increments = Math.max(1, Math.floor(percent / 10));
    if (increments > 20) return mark + " " + percent + "% " + word + " " + mark;
    return mark.repeat(increments);
  }
  return percent + "% " + word + " - BPM changed to " + next;
}
function rhythmFor(beat, d) {
  if (d < beat / 16) return 0;
  if (d < beat / 8) return 1;
  if (d < beat / 4) return 2;
  if (d < beat / 2) return 3;
  if (d < beat) return 4;
  if (d < beat * 2) return 5;
  if (d < beat * 4) return 6;
  if (d < beat * 8) return 7;
  return LONG;
}
function separator(beat, d) {
  if (d < beat / 4) return "-";
  if (d < beat / 2) return " ";
  if (d < beat) return " - ";
  if (d < beat * 2) return ", ";
  if (d < beat * 3) return "... ";
  if (d < beat * 4) return ".... ";
  return "...... ";
}
function tidyLines(text) {
  let out = "", started = false, blank = false;
  for (let line of text.split("\n")) {
    line = line.replace(/ +$/, "");
    if (!line) { blank = started; continue; }
    if (started) out += blank ? "\n\n" : "\n";
    out += line;
    started = true;
    blank = false;
  }
  return out;
}
)js"
R"js(
const NOTES_SHIFTED = 0, TRANSPOSE = 1;
function applyRegions(notes, regions) {
  return notes.map(n => {
    let midi = n.midi;
    for (const region of regions) if (region.kind !== TRANSPOSE && n.seconds >= region.from && n.seconds <= region.to) midi += region.semitones;
    return {seconds: n.seconds, midi};
  });
}
function transposeSections(regions) { return regions.filter(r => r.kind === TRANSPOSE); }
function style(notesIn, mapping, temposIn, metersIn, o, sections) {
  sections = sections || [];
  const r = {items: [], text: "", notes: 0, groups: 0, merged: 0, unmapped: 0, hidden: 0, transposition: 0, sections: [], hasTempo: false};
  const bySeconds = (a, b) => a.seconds - b.seconds;
  const notes = notesIn.slice().sort(bySeconds);
  const tempos = temposIn.slice().sort(bySeconds);
  const meters = metersIn.slice().sort(bySeconds);
  r.hasTempo = tempos.length > 0;
  const chordOf = chordIndices(notes, o.quantizeMs);
  const chords = [];
  for (let i = 0; i < notes.length; ++i) {
    if (chordOf[i] === chords.length) chords.push({notes: [], ms: notes[i].seconds * 1000, msEnd: notes[i].seconds * 1000});
    chords[chords.length - 1].notes.push(notes[i]);
    chords[chords.length - 1].msEnd = notes[i].seconds * 1000;
  }
  const base = o.autoTranspose && notes.length ? bestTransposition(notes, mapping, o.transpose, o.resilience) : o.transpose;
  let shifts = new Array(chords.length).fill(base);
  if (o.autoTranspose && o.autoSections && chords.length) shifts = bestSections(chords, mapping, o);
  for (let k = 0; k < chords.length; ++k)
    for (const section of sections)
      if (chords[k].ms / 1000 >= section.from && chords[k].ms / 1000 <= section.to) shifts[k] = section.semitones;
  r.transposition = chords.length ? shifts[0] : base;
  for (let k = 0; k < chords.length; ++k) {
    if (k === 0 || shifts[k] !== shifts[k - 1]) r.sections.push({from: chords[k].ms / 1000, to: chords[k].msEnd / 1000, semitones: shifts[k]});
    else r.sections[r.sections.length - 1].to = chords[k].msEnd / 1000;
  }
  const comment = text => { const it = item(COMMENT); it.text = text; r.items.push(it); };
  const lineBreak = () => r.items.push(item(BREAK));
  if (r.transposition !== 0) comment("Transpose by: " + (-r.transposition));
  const beatsAt = seconds => {
    let beats = 0, from = 0, bpm = o.missingBpm;
    for (const mark of tempos) {
      if (mark.seconds >= seconds) break;
      beats += (mark.seconds - from) * bpm / 60;
      from = mark.seconds;
      bpm = mark.bpm;
    }
    return beats + (seconds - from) * bpm / 60;
  };
  const events = [];
  tempos.forEach((t, i) => events.push({seconds: t.seconds, order: 0, index: i}));
  meters.forEach((m, i) => events.push({seconds: m.seconds, order: 1, index: i}));
  notes.forEach((n, i) => events.push({seconds: n.seconds, order: 2, index: i}));
  events.sort((a, b) => a.seconds !== b.seconds ? a.seconds - b.seconds : a.order - b.order);
  let bpm = o.missingBpm, previousBpm = 0, haveTempo = false, scheduled = false, numerator = 4;
  const START = 0, UNSET = 1, SET = 2;
  let next = START, nextBar = 0, penalty = 0, current = [], currentChord = 0;
  const flush = () => {
    if (!current.length) return;
    let quantized = false;
    for (let i = 1; i < current.length; ++i) if (current[i].ms !== current[i - 1].ms) { quantized = true; break; }
    let chord = [];
    if (!quantized) {
      for (const n of current) {
        if (chord.some(kept => kept.midi === n.midi)) { r.merged++; continue; }
        chord.push(n);
      }
      chord = sortChord(chord, o.classicChordOrder);
    } else if (o.sequentialQuantize) {
      chord = current.slice().sort((a, b) => a.ms - b.ms);
    } else {
      chord = sortChord(current, o.classicChordOrder);
    }
    const it = renderChord(chord, quantized, o, r);
    it.ms = chord[0].ms;
    it.beatMs = chord[0].beatMs;
    it.msEnd = chord.reduce((m, n) => Math.max(m, n.ms), chord[0].ms);
    r.items.push(it);
    current = [];
  };
  const checkBreak = first => {
    if (o.breaks === 0) {
      const beat = beatsAt(first.ms / 1000);
      if (scheduled) {
        scheduled = false;
        lineBreak();
        if (next !== START) next = UNSET;
      }
      if (next !== SET) { nextBar = (next === UNSET ? beat : 0) + numerator; next = SET; }
      if (beat + 1e-9 >= nextBar) { lineBreak(); nextBar += numerator; }
    } else if (o.breaks === 1) {
      const normalized = first.ms - penalty;
      if (normalized + 0.5 >= first.beatMs * o.beats) { lineBreak(); penalty += normalized; }
    }
  };
  for (const event of events) {
    if (event.order === 0) {
      const newBpm = tempos[event.index].bpm;
      scheduled = true;
      if (o.bpmChanges) {
        if (!haveTempo) comment("Tempo: " + Math.round(newBpm) + " BPM");
        else if (newBpm !== previousBpm) {
          const text = bpmComment(Math.round(previousBpm), Math.round(newBpm), o.bpmStyle, o.minSpeedChange);
          if (text !== null) comment(text);
        }
      }
      haveTempo = true;
      previousBpm = bpm = newBpm;
      continue;
    }
    if (event.order === 1) {
      numerator = Math.max(1, meters[event.index].numerator);
      scheduled = true;
      continue;
    }
    const note = notes[event.index];
    const k = chordOf[event.index];
    if (current.length && k !== currentChord) flush();
    if (!current.length) {
      currentChord = k;
      if (k > 0 && shifts[k] !== shifts[k - 1]) comment("Transpose by: " + (-shifts[k]));
    }
    const placed = locate(note.seconds * 1000, note.midi + shifts[k], mapping, o);
    placed.beatMs = bpm > 0 ? 60000 / bpm : 500;
    current.push(placed);
    checkBreak(current[0]);
  }
  flush();
  const kept = [];
  for (const it of r.items) {
    if (it.kind === BREAK && (!kept.length || kept[kept.length - 1].kind === BREAK)) continue;
    kept.push(it);
  }
  while (kept.length && kept[kept.length - 1].kind === BREAK) kept.pop();
  r.items = kept;
  const written = [];
  r.items.forEach((it, i) => { if (it.kind === CHORD) written.push(i); });
  for (let c = 0; c < written.length; ++c) {
    const it = r.items[written[c]];
    if (c + 1 === written.length) { it.rhythm = LONG; it.separator = ""; continue; }
    const difference = r.items[written[c + 1]].ms - it.ms - 0.5;
    it.rhythm = rhythmFor(it.beatMs, difference - 0.5);
    it.separator = o.tempoMarks ? separator(it.beatMs, difference) : " ";
  }
  let text = "";
  for (let i = 0; i < r.items.length; ++i) {
    const it = r.items[i];
    if (it.kind === CHORD) { for (const s of it.segments) text += s.text; text += it.separator; }
    else if (it.kind === COMMENT) text += "\n" + it.text + "\n";
    else {
      const nearComment = (i > 0 && r.items[i - 1].kind === COMMENT) || (i + 1 < r.items.length && r.items[i + 1].kind === COMMENT);
      if (!nearComment) text += "\n";
    }
  }
  r.text = tidyLines(text);
  return r;
}
function escapeHtml(text) {
  return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}
// The sheet as HTML, as sheet::ToHtml writes it, with each chord numbered so
// a selection can be traced back to its notes.
function toHtml(r) {
  let html = "";
  for (let i = 0; i < r.items.length; ++i) {
    const it = r.items[i];
    if (it.kind === CHORD) {
      html += '<span class="chord" data-i="' + i + '" style="color:' + COLOURS[it.rhythm] + '">';
      for (const s of it.segments) html += s.oor ? '<span class="oor">' + escapeHtml(s.text) + "</span>" : escapeHtml(s.text);
      html += escapeHtml(it.separator) + "</span>";
    } else if (it.kind === COMMENT) {
      html += '<br><span class="comment">' + escapeHtml(it.text) + "</span><br>";
    } else {
      const nearComment = (i > 0 && r.items[i - 1].kind === COMMENT) || (i + 1 < r.items.length && r.items[i + 1].kind === COMMENT);
      if (!nearComment) html += "<br>";
    }
  }
  return html;
}
return {defaults, applyRegions, transposeSections, style, toHtml, escapeHtml, NOTES_SHIFTED, TRANSPOSE};
})();
/*SHEET-CORE-END*/
)js";

// Page UI: settings column, selection toolbar, copy, save and print.
inline constexpr const char* kPageUi = R"js(
(() => {
const data = JSON.parse(document.getElementById("sheet-data").textContent);
// The settings are the page's alone. A page the app just wrote starts from
// what the user last chose in this browser; a page saved from here starts
// from the settings it was saved with.
const STORE = "midipp.sheet.style";
const remembered = () => { try { return JSON.parse(localStorage.getItem(STORE) || "null"); } catch (e) { return null; } };
const stored = data.saved ? null : remembered();
const options = Object.assign(SheetCore.defaults(), data.options, stored && stored.options || {});
const page = Object.assign({fontSize: 10, lineHeight: 135}, data.page || {}, stored && stored.page || {});
function remember() { try { localStorage.setItem(STORE, JSON.stringify({options, page})); } catch (e) {} }
const unpackRegion = r => ({from: r[0], to: r[1], semitones: r[2], kind: r[3] || SheetCore.NOTES_SHIFTED});
let regions = data.regions.map(unpackRegion);
const notes = data.notes.map(n => ({seconds: n[0], midi: n[1]}));
const tempos = data.tempos.map(t => ({seconds: t[0], bpm: t[1]}));
const meters = data.meters.map(m => ({seconds: m[0], numerator: m[1]}));
const $ = id => document.getElementById(id);
let result = null;
const time = seconds => {
  const whole = Math.floor(seconds), minutes = Math.floor(whole / 60), rest = whole % 60;
  return minutes + ":" + (rest < 10 ? "0" : "") + rest + "." + Math.floor((seconds - whole) * 10);
};
const signed = n => (n > 0 ? "+" : "") + n;
const sameRun = (a, b) => a.from === b.from && a.to === b.to;
// The transposition in force at a moment: the section it falls in.
const shiftAt = seconds => {
  for (const s of result.sections) if (seconds >= s.from && seconds <= s.to) return s.semitones;
  return result.transposition;
};
function draw() {
  result = SheetCore.style(SheetCore.applyRegions(notes, regions), data.mapping, tempos, meters, options, SheetCore.transposeSections(regions));
  const sheet = $("sheet");
  sheet.innerHTML = SheetCore.toHtml(result);
  sheet.style.fontSize = page.fontSize + "pt";
  sheet.style.lineHeight = page.lineHeight + "%";
  for (const span of sheet.querySelectorAll(".chord")) {
    const it = result.items[+span.dataset.i];
    const inside = regions.some(r => it.ms / 1000 >= r.from && it.msEnd / 1000 <= r.to);
    span.classList.toggle("in-section", inside);
  }
  let count = result.notes + " notes, " + result.groups + " chords.";
  if (result.merged) count += " " + result.merged + " shared notes merged.";
  if (result.unmapped) count += " " + result.unmapped + " unmapped.";
  if (result.hidden) count += " " + result.hidden + " out of range left out.";
  if (result.sections.length > 1) count += " Transposed in " + result.sections.length + " sections.";
  else if (result.transposition) count += " Transposed " + signed(result.transposition) + ".";
  $("count").textContent = count;
  // Every run of one transposition when there is more than one, then the
  // shifted runs. A run the user made can be removed; a found one cannot,
  // but Keep turns the found runs into the user's.
  const list = $("sections");
  list.innerHTML = "";
  const entry = (text, onRemove) => {
    const li = document.createElement("li");
    const span = document.createElement("span");
    span.textContent = text;
    span.style.flex = "1";
    li.appendChild(span);
    if (onRemove) {
      const remove = document.createElement("button");
      remove.className = "small";
      remove.textContent = "Remove";
      remove.onclick = onRemove;
      li.appendChild(remove);
    }
    list.appendChild(li);
  };
  if (result.sections.length > 1) result.sections.forEach(s => {
    const own = regions.findIndex(r => r.kind === SheetCore.TRANSPOSE && sameRun(r, s));
    entry(time(s.from) + " to " + time(s.to) + ", transposed " + signed(s.semitones), own >= 0 ? () => { regions.splice(own, 1); draw(); } : null);
  });
  regions.forEach((r, i) => {
    if (r.kind === SheetCore.TRANSPOSE && !result.sections.some(s => sameRun(r, s))) {
      entry(time(r.from) + " to " + time(r.to) + ", transposed " + signed(r.semitones) + ", inside another section", () => { regions.splice(i, 1); draw(); });
    } else if (r.kind !== SheetCore.TRANSPOSE) {
      entry(time(r.from) + " to " + time(r.to) + ", notes shifted " + signed(r.semitones), () => { regions.splice(i, 1); draw(); });
    }
  });
  if (!list.children.length) list.innerHTML = '<li class="empty">Select part of the sheet to transpose it on its own, or to shift its notes.</li>';
  const found = options.autoTranspose && options.autoSections && result.sections.length > 1;
  $("keep").style.display = found ? "" : "none";
  $("beats-row").style.display = options.breaks === 1 ? "" : "none";
  $("sections-row").style.display = options.autoTranspose ? "" : "none";
  $("sections-rows").style.display = options.autoTranspose && options.autoSections ? "" : "none";
}
$("keep").onclick = () => {
  regions = regions.filter(r => r.kind !== SheetCore.TRANSPOSE)
    .concat(result.sections.map(s => ({from: s.from, to: s.to, semitones: s.semitones, kind: SheetCore.TRANSPOSE})));
  options.autoSections = false;
  document.querySelectorAll("[data-key]").forEach(el => el._show());
  remember();
  draw();
};
function control(el) {
  const key = el.dataset.key, target = el.dataset.page ? page : options;
  const read = () => {
    if (el.type === "checkbox") return el.checked;
    if (el.type === "text") return el.value;
    return el.type === "range" || el.type === "number" || el.tagName === "SELECT" ? Number(el.value) : el.value;
  };
  const show = () => {
    if (el.type === "checkbox") el.checked = !!target[key];
    else el.value = target[key];
    const out = el.parentElement.querySelector("output");
    if (out) out.textContent = el.dataset.format ? el.dataset.format.replace("%", target[key]) : target[key];
  };
  show();
  el._show = show;
  el.addEventListener("input", () => { target[key] = read(); show(); remember(); draw(); });
}
document.querySelectorAll("[data-key]").forEach(control);
$("reset").onclick = () => {
  try { localStorage.removeItem(STORE); } catch (e) {}
  Object.assign(options, SheetCore.defaults());
  Object.assign(page, {fontSize: 10, lineHeight: 135});
  document.querySelectorAll("[data-key]").forEach(el => el._show());
  draw();
};
// A selection over the sheet names a section: from the first chord's onset to
// the last chord's last note, so every note in the chords selected moves.
const toolbar = $("selection");
let pending = null;
function selectionRegion() {
  const selection = window.getSelection();
  if (!selection || selection.rangeCount === 0 || selection.isCollapsed) return null;
  const range = selection.getRangeAt(0);
  let from = Infinity, to = -Infinity;
  for (const span of $("sheet").querySelectorAll(".chord")) {
    if (!range.intersectsNode(span)) continue;
    const it = result.items[+span.dataset.i];
    from = Math.min(from, it.ms / 1000);
    to = Math.max(to, it.msEnd / 1000);
  }
  return from <= to ? {from, to} : null;
}
// The toolbar's two amounts: the transposition in force over the selection,
// and how far its notes are shifted.
function showAmounts() {
  const shifted = regions.find(r => r.kind !== SheetCore.TRANSPOSE && sameRun(r, pending));
  $("selection-amount").textContent = signed(shiftAt(pending.from));
  $("shift-amount").textContent = signed(shifted ? shifted.semitones : 0);
}
document.addEventListener("selectionchange", () => {
  const region = selectionRegion();
  if (!region) { if (!toolbar.contains(document.activeElement)) toolbar.style.display = "none"; return; }
  pending = region;
  const rect = window.getSelection().getRangeAt(0).getBoundingClientRect();
  toolbar.style.display = "flex";
  toolbar.style.left = Math.max(8, Math.min(window.innerWidth - toolbar.offsetWidth - 8, rect.left)) + "px";
  toolbar.style.top = Math.max(8, rect.top - toolbar.offsetHeight - 8) + "px";
  $("selection-range").textContent = time(region.from) + " to " + time(region.to);
  showAmounts();
});
// Transpose sets the selection's transposition, on top of the sheet's or
// the search's, and the sheet says so where it starts and ends.
function transposeSelection(by) {
  if (!pending) return;
  const existing = regions.find(r => r.kind === SheetCore.TRANSPOSE && sameRun(r, pending));
  if (existing) existing.semitones += by;
  else regions.push({from: pending.from, to: pending.to, semitones: shiftAt(pending.from) + by, kind: SheetCore.TRANSPOSE});
  draw();
  showAmounts();
}
// Shift notes moves the selection's notes and the reader sees other keys.
function shiftSelection(by) {
  if (!pending) return;
  const existing = regions.find(r => r.kind !== SheetCore.TRANSPOSE && sameRun(r, pending));
  if (existing) existing.semitones += by; else regions.push({from: pending.from, to: pending.to, semitones: by, kind: SheetCore.NOTES_SHIFTED});
  regions = regions.filter(r => r.kind === SheetCore.TRANSPOSE || r.semitones !== 0);
  draw();
  showAmounts();
}
$("selection-down").onclick = () => transposeSelection(-1);
$("selection-up").onclick = () => transposeSelection(1);
$("shift-down").onclick = () => shiftSelection(-1);
$("shift-up").onclick = () => shiftSelection(1);
$("selection-close").onclick = () => { toolbar.style.display = "none"; window.getSelection().removeAllRanges(); };
function copyText(text) {
  const fallback = () => {
    const area = document.createElement("textarea");
    area.value = text;
    document.body.appendChild(area);
    area.select();
    let ok = false;
    try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
    area.remove();
    return ok;
  };
  if (navigator.clipboard && navigator.clipboard.writeText) return navigator.clipboard.writeText(text).then(() => true, fallback);
  return Promise.resolve(fallback());
}
$("copy").onclick = () => copyText(result.text).then(ok => {
  const button = $("copy");
  button.textContent = ok ? "Copied" : "Clipboard is busy";
  setTimeout(() => { button.textContent = "Copy sheet"; }, 1500);
});
// A save asks where, in a browser that can, and opens in the folder the
// last sheet went to; elsewhere it is a download. Cancelling saves nothing.
async function saveBlob(blob, name, description, accept) {
  if (window.showSaveFilePicker) {
    try {
      const handle = await window.showSaveFilePicker({id: "midipp-sheets", suggestedName: name, types: [{description, accept}]});
      const writable = await handle.createWritable();
      await writable.write(blob);
      await writable.close();
      return true;
    } catch (e) {
      if (e && e.name === "AbortError") return false;
    }
  }
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = name;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  return true;
}
function report(button, promise, label) {
  promise.then(ok => { button.textContent = ok ? "Saved" : label; }, e => { button.textContent = "Could not save"; console.error(e); })
    .then(() => setTimeout(() => { button.textContent = label; }, 1500));
}
$("save").onclick = () => {
  const saved = Object.assign({}, data, {saved: true, options, page, regions: regions.map(r => [r.from, r.to, r.semitones, r.kind]), expected: result.text});
  const element = $("sheet-data");
  const before = element.textContent;
  element.textContent = JSON.stringify(saved).replace(/<\//g, "<\\/");
  toolbar.style.display = "none";
  const html = "<!doctype html>\n" + document.documentElement.outerHTML;
  element.textContent = before;
  report($("save"), saveBlob(new Blob([html], {type: "text/html"}), (data.title || "Sheet") + ".html", "Sheet page", {"text/html": [".html"]}), "Save page");
};
// The sheet as a picture: the sheet's own markup, drawn by the browser
// inside an SVG image and copied to a canvas at twice the size so the text
// stays sharp. The title goes on top, as Print shows it.
function sheetImage() {
  const sheet = $("sheet");
  const width = Math.ceil(sheet.getBoundingClientRect().width);
  const css = "font-family:Verdana,sans-serif;font-size:" + page.fontSize + "pt;line-height:" + page.lineHeight + "%;color:#fff;" +
    "white-space:pre-wrap;background:#2D2A32;padding:16px;box-sizing:border-box;width:" + (width + 32) + "px;margin:0";
  const rules = ".oor{display:inline-flex;justify-content:center;min-width:.6em;border-bottom:2px solid;font-weight:900}.comment{color:#c8c4cc}";
  const body = '<div style="color:#aaa4b3;margin-bottom:1.35em">' + SheetCore.escapeHtml(data.title || "") + "</div>" + sheet.innerHTML.replace(/<br>/g, "<br/>");
  const probe = document.createElement("div");
  probe.setAttribute("style", css + ";position:absolute;left:-100000px;top:0");
  probe.innerHTML = "<style>" + rules + "</style>" + body;
  document.body.appendChild(probe);
  const height = Math.ceil(probe.getBoundingClientRect().height);
  probe.remove();
  const html = '<div xmlns="http://www.w3.org/1999/xhtml" style="' + css + '"><style>' + rules + "</style>" + body + "</div>";
  const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="' + (width + 32) + '" height="' + height + '">' +
    '<foreignObject width="100%" height="100%">' + html + "</foreignObject></svg>";
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => {
      const canvas = document.createElement("canvas");
      canvas.width = (width + 32) * 2;
      canvas.height = height * 2;
      const context = canvas.getContext("2d");
      context.scale(2, 2);
      context.drawImage(image, 0, 0);
      try { canvas.toBlob(blob => blob ? resolve(blob) : reject(new Error("The browser gave no image.")), "image/png"); }
      catch (e) { reject(e); }
    };
    image.onerror = () => reject(new Error("The browser could not draw the sheet."));
    image.src = "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg);
  });
}
$("image").onclick = () => {
  toolbar.style.display = "none";
  report($("image"), sheetImage().then(blob => saveBlob(blob, (data.title || "Sheet") + ".png", "Sheet image", {"image/png": [".png"]})), "Save image");
};
$("print").onclick = () => window.print();
draw();
// The app wrote its own text of this sheet into the page, drawn with the
// settings it embedded. A difference means the two translations of
// midi-converter have drifted, which is a bug here.
if (typeof data.expected === "string") {
  const written = data.regions.map(unpackRegion);
  const check = SheetCore.style(SheetCore.applyRegions(notes, written), data.mapping, tempos, meters,
                                Object.assign(SheetCore.defaults(), data.options), SheetCore.transposeSections(written)).text;
  if (check !== data.expected) {
    console.error("The page's sheet differs from the app's. Expected:\n" + data.expected + "\nGot:\n" + check);
    $("parity").classList.add("shown");
  }
}
})();
)js";

} // namespace detail

// Apply regions as the page script does: NotesShifted moves notes before
// styling, Transpose regions become sections.
inline std::vector<TimedNote> ShiftedNotes(const PageInput& in) {
    std::vector<TimedNote> notes = in.notes;
    for (auto& note : notes)
        for (const auto& region : in.regions)
            if (region.kind == Region::Kind::NotesShifted && note.seconds >= region.from && note.seconds <= region.to) note.midi += region.semitones;
    return notes;
}
inline std::vector<Section> TransposeSections(const PageInput& in) {
    std::vector<Section> sections;
    for (const auto& region : in.regions)
        if (region.kind == Region::Kind::Transpose) sections.push_back({region.from, region.to, region.semitones});
    return sections;
}

// Builds the editor page. The initial sheet markup is the C++ rendering; the
// script redraws on load and checks against it. If rendered is non-null it
// receives that rendering (for the counts).
inline std::string ToEditorHtml(const PageInput& in, StyledResult* rendered = nullptr) {
    const auto initial = Style(ShiftedNotes(in), in.mapping, in.tempos, in.meters, in.options, TransposeSections(in));
    if (rendered) *rendered = initial;
    std::string html = "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><title>" + detail::EscapeHtml(in.title) +
        "</title><style>" + detail::kPageCss + "</style></head><body>\n"
        "<header><h1>" + detail::EscapeHtml(in.title) + "</h1><span class=\"count\" id=\"count\"></span>"
        "<button id=\"copy\" class=\"primary\">Copy sheet</button><button id=\"image\">Save image</button><button id=\"save\">Save page</button><button id=\"print\">Print</button></header>\n"
        "<div id=\"layout\"><aside>\n"
        "<h2>Chords</h2>"
        "<label class=\"col\"><span>Chord window</span><div class=\"range\"><input type=\"range\" min=\"0\" max=\"200\" step=\"1\" data-key=\"quantizeMs\" data-format=\"% ms\"><output></output></div></label>"
        "<label class=\"row\"><span>Keep spread chords in played order</span><input type=\"checkbox\" data-key=\"sequentialQuantize\"></label>"
        "<label class=\"row\"><span>Write spread chords in braces</span><input type=\"checkbox\" data-key=\"curlyQuantizes\"></label>"
        "<label class=\"row\"><span>Classic chord order</span><input type=\"checkbox\" data-key=\"classicChordOrder\"></label>"
        "<label class=\"col\"><span>Shifted characters</span><select data-key=\"shifts\"><option value=\"0\">First in the chord</option><option value=\"1\">Last in the chord</option><option value=\"2\">In pitch order</option></select></label>\n"
        "<h2>Out of range</h2>"
        "<label class=\"row\"><span>Keep out-of-range notes</span><input type=\"checkbox\" data-key=\"showOutOfRange\"></label>"
        "<label class=\"row\"><span>Mark out-of-range notes</span><input type=\"checkbox\" data-key=\"outOfRangeMarks\"></label>"
        "<label class=\"col\"><span>Separator</span><input type=\"text\" maxlength=\"7\" data-key=\"outOfRangeSeparator\"></label>"
        "<label class=\"col\"><span>Out-of-range notes sit</span><select data-key=\"outOfRangePlace\"><option value=\"0\">First in the chord</option><option value=\"1\">Last in the chord</option><option value=\"2\">Low first, high last</option></select></label>\n"
        "<h2>Rhythm and tempo</h2>"
        "<label class=\"row\"><span>Rhythm separators</span><input type=\"checkbox\" data-key=\"tempoMarks\"></label>"
        "<label class=\"row\"><span>Mention tempo changes</span><input type=\"checkbox\" data-key=\"bpmChanges\"></label>"
        "<label class=\"col\"><span>Tempo change wording</span><select data-key=\"bpmStyle\"><option value=\"0\">Detailed</option><option value=\"1\">Arrows</option></select></label>"
        "<label class=\"col\"><span>Smallest tempo change mentioned</span><div class=\"range\"><input type=\"range\" min=\"0\" max=\"100\" step=\"1\" data-key=\"minSpeedChange\" data-format=\"%%\"><output></output></div></label>"
        "<label class=\"col\"><span>Line breaks</span><select data-key=\"breaks\"><option value=\"0\">Every bar</option><option value=\"1\">Every few beats</option><option value=\"2\">None</option></select></label>"
        "<label class=\"col\" id=\"beats-row\"><span>Beats per line</span><div class=\"range\"><input type=\"range\" min=\"1\" max=\"32\" step=\"1\" data-key=\"beats\" data-format=\"% beats\"><output></output></div></label>"
        "<label class=\"col\"><span>Tempo when the file names none</span><div class=\"range\"><input type=\"range\" min=\"20\" max=\"400\" step=\"1\" data-key=\"missingBpm\" data-format=\"% BPM\"><output></output></div></label>\n"
        "<h2>Transposition</h2>"
        "<label class=\"col\"><span>Transpose</span><div class=\"range\"><input type=\"range\" min=\"-24\" max=\"24\" step=\"1\" data-key=\"transpose\" data-format=\"% semitones\"><output></output></div></label>"
        "<label class=\"row\"><span>Find the best transposition</span><input type=\"checkbox\" data-key=\"autoTranspose\"></label>"
        "<label class=\"col\"><span>Keep Transpose unless better by</span><div class=\"range\"><input type=\"range\" min=\"0\" max=\"20\" step=\"1\" data-key=\"resilience\"><output></output></div></label>"
        "<div id=\"sections-row\"><label class=\"row\"><span>Change transposition part-way</span><input type=\"checkbox\" data-key=\"autoSections\"></label>"
        "</div>"
        "<div id=\"sections-rows\">"
        "<label class=\"col\"><span>Switch cost</span><div class=\"range\"><input type=\"range\" min=\"0\" max=\"60\" step=\"1\" data-key=\"sectionSwitchCost\" data-format=\"% notes\"><output></output></div></label>"
        "<label class=\"col\"><span>Shortest section</span><div class=\"range\"><input type=\"range\" min=\"0\" max=\"60\" step=\"1\" data-key=\"sectionMinSeconds\" data-format=\"% s\"><output></output></div></label>"
        "<label class=\"col\"><span>Rest before a switch</span><div class=\"range\"><input type=\"range\" min=\"0\" max=\"2000\" step=\"50\" data-key=\"sectionRestMs\" data-format=\"% ms\"><output></output></div></label>"
        "<label class=\"col\"><span>Search range</span><div class=\"range\"><input type=\"range\" min=\"1\" max=\"24\" step=\"1\" data-key=\"sectionRange\" data-format=\"% semitones either way\"><output></output></div></label>"
        "</div>\n"
        "<h2>Sections</h2><ul id=\"sections\"></ul><button id=\"keep\">Keep these sections</button>\n"
        "<h2>Page</h2>"
        "<label class=\"col\"><span>Text size</span><div class=\"range\"><input type=\"range\" min=\"6\" max=\"24\" step=\"1\" data-key=\"fontSize\" data-page=\"1\" data-format=\"% pt\"><output></output></div></label>"
        "<label class=\"col\"><span>Line height</span><div class=\"range\"><input type=\"range\" min=\"100\" max=\"250\" step=\"5\" data-key=\"lineHeight\" data-page=\"1\" data-format=\"%%\"><output></output></div></label>"
        "<button id=\"reset\">Reset settings</button>"
        "</aside>\n<main><p id=\"parity\">The page's sheet differs from the app's. Copy styled sheet in the app gives the app's version.</p>"
        "<div id=\"sheet\">";
    // Pre-rendered so the sheet shows before the script runs.
    html += detail::SheetBody(initial);
    html += "</div></main></div>\n"
        "<div id=\"selection\"><span id=\"selection-range\"></span><span>Transpose</span>"
        "<button id=\"selection-down\" class=\"small\">-</button><output id=\"selection-amount\">0</output>"
        "<button id=\"selection-up\" class=\"small\">+</button><span>Shift notes</span>"
        "<button id=\"shift-down\" class=\"small\">-</button><output id=\"shift-amount\">0</output>"
        "<button id=\"shift-up\" class=\"small\">+</button><button id=\"selection-close\" class=\"small\">Done</button></div>\n"
        "<script id=\"sheet-data\" type=\"application/json\">" + detail::PageJson(in, initial.text) + "</script>\n"
        "<script>" + detail::kPageCore + detail::kPageUi + "</script>\n</body></html>\n";
    return html;
}

} // namespace sheet
