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
    std::vector<PedalChange> pedals;               // sustain, in seconds
    bool styled = false;                           // options and look come from the style page, over the browser's own
    Header header;
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
    field("pedalMarks", std::to_string(static_cast<int>(o.pedalMarks)));
    return j + "}";
}

inline std::string JsonLook(const Look& look) {
    std::string j = "{\"theme\":" + std::to_string(look.theme) + ",\"background\":" + JsonString(look.background) +
        ",\"ground\":" + std::to_string(static_cast<int>(look.ground)) + ",\"fit\":" + std::to_string(static_cast<int>(look.fit)) +
        ",\"dim\":" + std::to_string(look.dim) + ",\"grain\":" + std::to_string(look.grain) +
        ",\"oneColour\":" + (look.oneColour ? "true" : "false") + ",\"text\":" + JsonString(look.text) +
        ",\"comment\":" + JsonString(look.comment) + ",\"heading\":" + JsonString(look.heading) +
        ",\"pedalDown\":" + JsonString(look.pedalDown) + ",\"pedalUp\":" + JsonString(look.pedalUp) + ",\"rhythm\":[";
    for (size_t i = 0; i < look.rhythm.size(); ++i) j += (i ? "," : "") + JsonString(look.rhythm[i]);
    j += "],\"font\":" + std::to_string(static_cast<int>(look.font));
    if (!look.image.empty()) j += ",\"image\":" + JsonString(look.image);
    return j + "}";
}

inline std::string JsonHeader(const Header& h) {
    const auto flag = [](bool b) { return std::string(b ? "true" : "false"); };
    return "{\"title\":" + flag(h.title) + ",\"subtitle\":" + flag(h.subtitle) + ",\"artist\":" + flag(h.artist) +
        ",\"arranger\":" + flag(h.arranger) + ",\"tempo\":" + flag(h.tempo) + ",\"key\":" + flag(h.key) +
        ",\"difficulty\":" + flag(h.difficulty) + ",\"date\":" + flag(h.date) + ",\"centre\":" + flag(h.centre) +
        ",\"arrangerName\":" + JsonString(h.arrangerName) + "}";
}

// Contents of the sheet-data element: the script's inputs plus the C++ text
// ("expected") for the parity check.
inline std::string PageJson(const PageInput& in, const std::string& expectedText) {
    std::string j = "{\"title\":" + JsonString(in.title) + ",\"notes\":[";
    for (size_t i = 0; i < in.notes.size(); ++i)
        j += (i ? "," : "") + std::string("[") + JsonNumber(in.notes[i].seconds) + "," + std::to_string(in.notes[i].midi) +
             (in.notes[i].end < in.notes[i].seconds ? std::string() : "," + JsonNumber(in.notes[i].end)) + "]";
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
    j += "],\"pedals\":[";
    for (size_t i = 0; i < in.pedals.size(); ++i)
        j += (i ? "," : "") + std::string("[") + JsonNumber(in.pedals[i].seconds) + "," + std::to_string(in.pedals[i].value) + "," +
             std::to_string(in.pedals[i].track) + "]";
    j += "],\"options\":" + JsonOptions(in.options) + ",\"page\":{\"fontSize\":" + JsonNumber(in.look.fontSizePt) +
         ",\"lineHeight\":" + JsonNumber(in.look.lineHeightPercent) + "},\"look\":" + JsonLook(in.look) + ",\"header\":" + JsonHeader(in.header) + (in.styled ? ",\"styled\":true" : "") +
         ",\"expected\":" + JsonString(expectedText) + "}";
    return j;
}

// The sheet uses midi-converter's background and Verdana; the settings column
// follows the app's spacing.
inline constexpr const char* kPageCss = R"css(
:root{--bg:#2D2A32;--side:#232027;--ink:#f2eff5;--muted:#aaa4b3;--line:rgba(255,255,255,.1);--field:#332f39;--accent:#7cc0ff;--on:#3fb950}
*{box-sizing:border-box}
[hidden]{display:none!important}
html,body{margin:0;height:100%}
body{background:var(--bg);color:var(--ink);font-family:Segoe UI,system-ui,sans-serif;font-size:13px;display:flex;flex-direction:column}
header{display:flex;flex-wrap:wrap;align-items:center;gap:8px 10px;padding:10px 16px;border-bottom:1px solid var(--line);background:var(--side)}
header h1{font-size:15px;font-weight:600;margin:0;flex:1 1 160px;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
header .count{color:var(--muted);flex:0 1 auto}
button{font:inherit;color:var(--ink);background:var(--field);border:1px solid var(--line);border-radius:6px;padding:6px 12px;cursor:pointer}
button:hover:not(:disabled){border-color:rgba(255,255,255,.3)}
button:disabled{opacity:.4;cursor:default}
button.primary{background:var(--accent);color:#10131a;border-color:transparent;font-weight:600}
button.small{padding:2px 8px}
#layout{flex:1;display:flex;min-height:0}
aside{width:330px;flex:none;display:flex;flex-direction:column;min-height:0;border-right:1px solid var(--line);background:var(--side)}
#presets{display:flex;flex-wrap:wrap;gap:6px;padding:12px 16px;border-bottom:1px solid var(--line)}
.chip{border-radius:999px;padding:3px 11px;display:inline-flex;align-items:center;gap:6px}
.chip[aria-pressed=true]{background:var(--accent);color:#10131a;border-color:transparent;font-weight:600}
.chip .remove{border:0;background:none;padding:0 0 0 2px;color:inherit;opacity:.7;line-height:1}
#presets input{width:130px;padding:3px 10px;border-radius:999px}
#panel{flex:1;overflow:auto;padding:0 16px 12px}
#panel-foot{display:flex;gap:8px;justify-content:flex-end;padding:10px 16px;border-top:1px solid var(--line)}
.fold{border-bottom:1px solid var(--line)}
.fold-head{display:flex;width:100%;align-items:center;justify-content:space-between;background:none;border:0;border-radius:0;padding:14px 0 10px;font-size:15px;font-weight:600}
.fold-head::after{content:"";width:7px;height:7px;margin-right:4px;border-right:2px solid var(--muted);border-bottom:2px solid var(--muted);transform:rotate(45deg)}
.fold-head[aria-expanded=false]::after{transform:rotate(-45deg)}
.fold-head[aria-expanded=false]+.fold-body{display:none}
.fold-body{padding:0 0 14px}
h3{font-size:13px;font-weight:600;margin:14px 0 2px}
.row{display:flex;align-items:center;justify-content:space-between;gap:10px;min-height:36px}
.row>span:first-child{flex:1;min-width:0}
.row.stack{display:block;padding:6px 0}
.row.stack>span:first-child{display:block;margin-bottom:6px}
.row.sub{margin-left:2px;padding-left:12px;border-left:2px solid var(--line)}
.inline{display:flex;align-items:center;gap:8px}
.seg{display:flex;background:var(--field);border:1px solid var(--line);border-radius:7px;padding:2px;gap:2px}
.seg button{flex:1 1 auto;border:0;background:none;padding:5px 6px;border-radius:5px;white-space:nowrap;display:inline-flex;align-items:center;justify-content:center;gap:5px}
.seg button[aria-pressed=true]{background:var(--accent);color:#10131a;font-weight:600}
.dot{width:9px;height:9px;border-radius:50%;display:inline-block;border:1px solid rgba(255,255,255,.35)}
.stepper{display:inline-flex;align-items:center;flex:none;border:1px solid var(--line);border-radius:7px;background:var(--field)}
.stepper button{border:0;background:none;border-radius:6px;padding:4px 10px;font-size:15px;line-height:1.1}
.stepper input{width:48px;border:0;background:none;text-align:center;padding:5px 0;-moz-appearance:textfield}
.stepper input::-webkit-inner-spin-button,.stepper input::-webkit-outer-spin-button{-webkit-appearance:none;margin:0}
.unit{color:var(--muted);white-space:nowrap}
.num{display:inline-flex;align-items:center;gap:6px;flex:none}
.num input{width:76px}
.found{background:rgba(124,192,255,.16);color:var(--accent);padding:3px 9px;border-radius:999px;font-size:12px;white-space:nowrap}
input.switch{appearance:none;-webkit-appearance:none;width:34px;height:20px;border-radius:999px;background:#4a4552;position:relative;cursor:pointer;flex:none;margin:0;transition:background .15s}
input.switch::after{content:"";position:absolute;top:2px;left:2px;width:16px;height:16px;border-radius:50%;background:#fff;transition:left .15s}
input.switch:checked{background:var(--on)}
input.switch:checked::after{left:16px}
input[type=text],input[type=number]{font:inherit;color:var(--ink);background:var(--field);border:1px solid var(--line);border-radius:6px;padding:5px 8px;min-width:0}
.row input[type=text]{width:150px}
input[type=color]{width:36px;height:26px;padding:1px;border:1px solid var(--line);border-radius:6px;background:var(--field);cursor:pointer;flex:none}
main{flex:1;overflow:auto;min-width:0}
#page{padding:16px;min-height:100%}
#sheet{white-space:pre-wrap;font-family:Verdana,sans-serif;font-size:10pt;line-height:135%;color:#fff}
#sheet .oor{display:inline-flex;justify-content:center;min-width:.6em;border-bottom:2px solid;font-weight:900}
#sheet .comment{color:var(--comment,#c8c4cc)}
#sheet .chord.in-section{background:rgba(124,192,255,.16);border-radius:2px}
#heading{font-family:Verdana,sans-serif;color:#aaa4b3;margin-bottom:1.35em;white-space:pre-wrap}
#heading:empty{display:none}
.swatches{display:grid;grid-template-columns:repeat(3,1fr);gap:6px 10px}
.swatches label{display:flex;align-items:center;gap:6px;min-width:0}
#sections{margin:0;padding:0}
#sections li{display:flex;align-items:center;gap:8px;list-style:none;padding:4px 0}
#selection{position:fixed;display:none;align-items:center;gap:8px;padding:8px 10px;background:var(--side);border:1px solid var(--line);border-radius:8px;box-shadow:0 8px 24px rgba(0,0,0,.4);z-index:2}
#selection output{min-width:2.5em;text-align:center}
#parity{display:none;margin:16px 16px 0;padding:8px 12px;border:1px solid #c66;border-radius:6px;color:#f4b7b7}
#parity.shown{display:block}
@media (max-width:720px){html,body{height:auto}body{display:block}#layout{display:block}aside{width:auto;border-right:0;border-bottom:1px solid var(--line)}#panel{overflow:visible}main{overflow:visible}}
@media print{header,aside,#selection,#parity{display:none!important}html,body{height:auto}body{display:block;background:var(--sheet-bg,#2D2A32);-webkit-print-color-adjust:exact;print-color-adjust:exact}#layout{display:block}main{overflow:visible}#page{padding:0;min-height:0}}
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
const CHORD = 0, BREAK = 1, COMMENT = 2, LONG = 8, PHRASES = 3;
function defaults() {
  return {quantizeMs: 35, sequentialQuantize: true, curlyQuantizes: true, classicChordOrder: false, shifts: 0,
          outOfRangePlace: 2, showOutOfRange: true, outOfRangeMarks: false, outOfRangeSeparator: ":", tempoMarks: false,
          bpmChanges: true, bpmStyle: 0, minSpeedChange: 10, breaks: 3, beats: 4, missingBpm: 120, transpose: 0,
          autoTranspose: true, resilience: 2, autoSections: false, sectionSwitchCost: 12, sectionMinSeconds: 8,
          sectionRestMs: 250, sectionRange: 12, pedalMarks: 3};
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
function item(kind) { return {kind, segments: [], text: "", separator: "", rhythm: LONG, ms: 0, beatMs: 500, msEnd: 0, keys: 0, shifted: 0, low: 0, high: 0}; }
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
  const press = n => {
    it.low = it.keys ? Math.min(it.low, n.midi) : n.midi;
    it.high = it.keys ? Math.max(it.high, n.midi) : n.midi;
    it.keys++;
    if (oneOf(n.character, CAPS)) it.shifted++;
    r.notes++;
  };
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
      press(n);
      if (o.outOfRangeMarks && n === lastStart && nonOutOfRange > 0) it.segments.push({text: "'", oor: false});
    } else if (!n.outOfRange) {
      it.segments.push({text: n.character, oor: false});
      press(n);
    } else {
      r.hidden++;
    }
  }
  if (isChord) it.segments.push({text: curly ? "}" : "]", oor: false});
  r.groups++;
  return it;
}
function transpositionFit(notes, by, mapping) {
  const fit = {onKeys: 0, shifted: 0};
  for (const note of notes) {
    const p = locate(0, note.midi + by, mapping, defaults());
    if (p.outOfRange || !p.valid) continue;
    fit.onKeys++;
    if (!oneOf(p.character, LOWER)) fit.shifted++;
  }
  return fit;
}
const SHIFTED_PER_NOTE = 4;
function fitScore(fit) { return fit.onKeys * SHIFTED_PER_NOTE - fit.shifted; }
function bestTransposition(notes, mapping, stickTo, resilience) {
  const kept = fitScore(transpositionFit(notes, stickTo, mapping));
  let best = stickTo, bestScore = kept;
  for (let d = 1; d <= 11 + Math.abs(stickTo); ++d) {
    for (const n of [stickTo - d, stickTo + d]) {
      if (n < -11 || n > 11) continue;
      const score = fitScore(transpositionFit(notes, n, mapping));
      if (score > bestScore) { best = n; bestScore = score; }
    }
  }
  return bestScore - kept > resilience * SHIFTED_PER_NOTE ? best : stickTo;
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
// sheet::detail::RestsBefore.
function restsBefore(chords) {
  const rests = new Array(chords.length).fill(0);
  let sounding = 0;
  for (let k = 0; k < chords.length; ++k) {
    if (k > 0) rests[k] = Math.max(0, chords[k].ms - sounding);
    for (const n of chords[k].notes) sounding = Math.max(sounding, Math.max(n.seconds, n.end) * 1000);
  }
  return rests;
}
// sheet::detail::PhraseGapMs.
function phraseGapMs(rests) {
  const gaps = rests.slice(1);
  if (!gaps.length) return Infinity;
  gaps.sort((a, b) => a - b);
  const n = gaps.length;
  const median = n % 2 ? gaps[(n - 1) / 2] : (gaps[n / 2 - 1] + gaps[n / 2]) / 2;
  return Math.max(500, 3.2 * median);
}
function bestSections(chords, mapping, o) {
  const n = chords.length;
  const candidates = [o.transpose];
  for (let d = 1; d <= o.sectionRange; ++d) { candidates.push(o.transpose - d); candidates.push(o.transpose + d); }
  const T = candidates.length;
  const cost = o.sectionSwitchCost * SHIFTED_PER_NOTE;
  const minMs = o.sectionMinSeconds * 1000, restMs = o.sectionRestMs;
  const NONE = -Infinity;
  const prefix = candidates.map(() => new Array(n + 1).fill(0));
  for (let t = 0; t < T; ++t)
    for (let k = 0; k < n; ++k) prefix[t][k + 1] = prefix[t][k] + fitScore(transpositionFit(chords[k].notes, candidates[t], mapping));
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
// sheet::Difficulty.
function difficulty(items) {
  const chords = items.filter(it => it.kind === CHORD && it.keys > 0);
  if (!chords.length) return 0;
  let keys = 0, shifted = 0, played = 0, jumps = 0, busiest = 0, recent = 0, first = 0;
  const sizes = [];
  for (let c = 0; c < chords.length; ++c) {
    const chord = chords[c];
    keys += chord.keys;
    shifted += chord.shifted;
    sizes.push(chord.keys);
    if (c > 0) {
      const previous = chords[c - 1];
      const gap = chord.ms - previous.msEnd;
      played += Math.min(gap, 2000);
      if (gap < 300 && (Math.abs(chord.low - previous.low) >= 12 || Math.abs(chord.high - previous.high) >= 12)) ++jumps;
    }
    recent += chord.keys;
    while (chord.ms - chords[first].ms > 10000) recent -= chords[first++].keys;
    busiest = Math.max(busiest, recent / 10);
  }
  played = Math.max(played / 1000, 1);
  sizes.sort((a, b) => a - b);
  const big = sizes[Math.floor((sizes.length - 1) * 99 / 100)];
  const speed = (keys / played + busiest) / 2;
  const part = value => Math.min(Math.max(value, 0), 1);
  const points = part((speed - 2) / 20) * 5.5 + part((big - 1) / 6) * 1.5 + part(shifted / keys / 0.5) + part(jumps / played / 5);
  return Math.min(Math.max(Math.round(1 + points), 1), 10);
}
const NOTES_SHIFTED = 0, TRANSPOSE = 1;
function applyRegions(notes, regions) {
  return notes.map(n => {
    let midi = n.midi;
    for (const region of regions) if (region.kind !== TRANSPOSE && n.seconds >= region.from && n.seconds <= region.to) midi += region.semitones;
    return {seconds: n.seconds, midi, end: n.end === undefined ? -1 : n.end};
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
  const rests = restsBefore(chords);
  const phraseGap = o.breaks === PHRASES ? phraseGapMs(rests) : Infinity;
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
    if (o.breaks === 0 || o.breaks === PHRASES) {
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
      if (k > 0 && rests[k] >= phraseGap) lineBreak();
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
  r.difficulty = difficulty(r.items);
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
const PEDAL_DOWN = 32, PEDAL_FULL = 64, PEDAL_CATCH_MS = 500, PEDAL_PASS_MS = 100;
// sheet::detail::MergedPedal.
function mergedPedal(changesIn) {
  const changes = changesIn.slice().sort((a, b) => a.seconds - b.seconds);
  const tracks = new Map(), merged = [];
  for (const change of changes) {
    tracks.set(change.track, change.value);
    let deepest = 0;
    for (const value of tracks.values()) deepest = Math.max(deepest, value);
    if (deepest !== (merged.length ? merged[merged.length - 1].value : 0)) merged.push({ms: change.seconds * 1000, value: deepest});
  }
  for (let k = 0; k + 1 < merged.length; ++k)
    if (merged[k].value >= PEDAL_DOWN && merged[k].value < PEDAL_FULL && merged[k + 1].ms - merged[k].ms < PEDAL_PASS_MS)
      merged[k].value = k ? merged[k - 1].value : 0;
  return merged;
}
// sheet::HeldByPedal.
function heldByPedal(notes, changes) {
  const merged = mergedPedal(changes);
  if (!merged.length) return notes;
  return notes.map(n => {
    if (n.end < n.seconds) return n;
    const ms = n.end * 1000;
    let at = 0, to = merged.length;
    while (at < to) { const mid = Math.floor((at + to) / 2); if (ms < merged[mid].ms) to = mid; else at = mid + 1; }
    if (at === 0 || merged[at - 1].value < PEDAL_DOWN) return n;
    while (at < merged.length && merged[at].value >= PEDAL_DOWN) ++at;
    return {seconds: n.seconds, midi: n.midi, end: at < merged.length ? merged[at].ms / 1000 : Infinity};
  });
}
// sheet::MarkPedals.
const PEDAL_OFF = 0, PEDAL_NORMAL = 1, PEDAL_INVERTED = 2;
function markPedals(r, changesIn, o) {
  for (const it of r.items) { it.pedal = 0; it.pedalHeld = false; }
  r.pedalMarks = PEDAL_OFF;
  if (o.pedalMarks === PEDAL_OFF || !changesIn.length) return;
  r.pedalMarks = o.pedalMarks;
  const merged = mergedPedal(changesIn);
  const chords = [];
  r.items.forEach((it, i) => { if (it.kind === CHORD) chords.push(i); });
  const press = new Array(chords.length).fill(0);
  let at = 0, value = 0, presses = 0;
  const apply = () => {
    if (value < PEDAL_DOWN && merged[at].value >= PEDAL_DOWN) ++presses;
    value = merged[at++].value;
  };
  for (let c = 0; c < chords.length; ++c) {
    const it = r.items[chords[c]];
    const onset = it.ms;
    const end = c + 1 < chords.length ? Math.min(r.items[chords[c + 1]].ms, onset + PEDAL_CATCH_MS) : onset + PEDAL_CATCH_MS;
    while (at < merged.length && merged[at].ms <= onset) apply();
    let deepest = value;
    while (at < merged.length && merged[at].ms < end) { apply(); deepest = Math.max(deepest, value); }
    it.pedal = deepest >= PEDAL_FULL ? 2 : deepest >= PEDAL_DOWN ? 1 : 0;
    press[c] = presses;
  }
  for (let c = 0; c + 1 < chords.length; ++c) {
    const it = r.items[chords[c]];
    it.pedalHeld = it.pedal !== 0 && r.items[chords[c + 1]].pedal !== 0 && press[c + 1] === press[c];
  }
}
)js"
R"js(
// sheet::Look's colours by theme: dark is the original's and the default.
const INK = ["#8a0b00", "#c41400", "#a8574f", "#a8501f", "#6e6a00", "#3f7d12", "#1f7a2f", "#145c3a", "#2b2620"];
const THEMES = [
  {background: "#2D2A32", ground: 0, grain: 0, text: "#ffffff", comment: "#c8c4cc", heading: "#aaa4b3", pedalDown: "#31406b", pedalUp: "#6b3262", rhythm: COLOURS},
  {background: "#f3ead7", ground: 2, grain: 40, text: "#2b2620", comment: "#6b6258", heading: "#7a6f63", pedalDown: "#c8d2ec", pedalUp: "#ecc8df", rhythm: INK},
  {background: "#ffffff", ground: 0, grain: 0, text: "#1a1a1a", comment: "#5f5f66", heading: "#77777f", pedalDown: "#d3dcf5", pedalUp: "#f3d3e8", rhythm: INK.slice(0, 8).concat(["#1a1a1a"])},
];
const LOOK = Object.assign({theme: 0, fit: 0, dim: 0, oneColour: false, font: 0}, THEMES[0]);
const FONTS = ["Verdana,sans-serif", "'Segoe UI',sans-serif", "Consolas,monospace"];
// sheet::detail::ChordColour.
function chordColour(rhythm, look) { return look.oneColour ? look.text : look.rhythm[rhythm]; }
// sheet::GrainTile and GrainChannel.
const GRAIN_TILE = 192, GRAIN_CELL = 16;
function grainTile() {
  let a = 0x5eed;
  const next = () => {
    a = (a + 0x6D2B79F5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), a | 1);
    t = (t + Math.imul(t ^ (t >>> 7), t | 61)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  const cells = GRAIN_TILE / GRAIN_CELL, coarse = [];
  for (let i = 0; i < cells * cells; ++i) coarse.push(next());
  const tile = new Float64Array(GRAIN_TILE * GRAIN_TILE);
  for (let y = 0; y < GRAIN_TILE; ++y)
    for (let x = 0; x < GRAIN_TILE; ++x) {
      const x0 = Math.floor(x / GRAIN_CELL), y0 = Math.floor(y / GRAIN_CELL), x1 = (x0 + 1) % cells, y1 = (y0 + 1) % cells;
      const fx = (x % GRAIN_CELL) / GRAIN_CELL, fy = (y % GRAIN_CELL) / GRAIN_CELL;
      const top = coarse[y0 * cells + x0] + (coarse[y0 * cells + x1] - coarse[y0 * cells + x0]) * fx;
      const bottom = coarse[y1 * cells + x0] + (coarse[y1 * cells + x1] - coarse[y1 * cells + x0]) * fx;
      tile[y * GRAIN_TILE + x] = 0.6 * (top + (bottom - top) * fy) + 0.4 * next();
    }
  return tile;
}
function grainChannel(channel, value, grain) { return Math.min(255, Math.max(0, Math.floor(channel + (value - 0.5) * grain * 0.6 + 0.5))); }
// sheet::detail::PedalHighlight and WithPedalHighlight.
function pedalHighlight(level, marks, look) {
  const down = level > 0;
  if (marks === PEDAL_OFF || (down && marks === PEDAL_INVERTED) || (!down && marks === PEDAL_NORMAL)) return "";
  const colour = down ? (look || LOOK).pedalDown : (look || LOOK).pedalUp;
  return level === 1 ? "background:linear-gradient(transparent 50%," + colour + " 50%)" : "background:" + colour;
}
function withPedalHighlight(html, level, marks, look) {
  const highlight = pedalHighlight(level, marks, look);
  return !highlight || !html ? html : '<span style="' + highlight + '">' + html + "</span>";
}
// sheet::detail::Heading.
function heading(title, level) {
  if (!level) return title;
  return (title ? title + " · " : "") + "Difficulty " + level + " of 10";
}
// sheet::SheetText.
function sheetText(r) {
  if (!r.difficulty) return r.text;
  return "Difficulty: " + r.difficulty + " of 10" + (r.text ? "\n" + r.text : "");
}
// sheet::Header's defaults.
const HEADER = {title: true, subtitle: false, artist: false, arranger: false, tempo: false, key: false, difficulty: true, date: false, centre: false, arrangerName: ""};
const MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"];
// sheet::Today.
function today() { const d = new Date(); return d.getDate() + " " + MONTHS[d.getMonth()] + " " + d.getFullYear(); }
// sheet::FindKey.
function findKey(notes) {
  if (!notes.length) return "";
  const names = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"];
  const major = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88];
  const minor = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17];
  const counts = new Array(12).fill(0);
  for (const n of notes) counts[((n.midi % 12) + 12) % 12] += 1;
  const correlation = (profile, tonic) => {
    let mx = 0, my = 0;
    for (let i = 0; i < 12; ++i) { mx += counts[(i + tonic) % 12]; my += profile[i]; }
    mx /= 12; my /= 12;
    let sxy = 0, sxx = 0, syy = 0;
    for (let i = 0; i < 12; ++i) {
      const x = counts[(i + tonic) % 12] - mx, y = profile[i] - my;
      sxy += x * y; sxx += x * x; syy += y * y;
    }
    return sxx > 0 && syy > 0 ? sxy / Math.sqrt(sxx * syy) : 0;
  };
  let best = -2, key = "";
  for (let tonic = 0; tonic < 12; ++tonic)
    for (let minorKey = 0; minorKey < 2; ++minorKey) {
      const c = correlation(minorKey ? minor : major, tonic);
      if (c > best) { best = c; key = names[tonic] + (minorKey ? " minor" : " major"); }
    }
  return key;
}
// sheet::detail::HeaderLines.
function headerLines(r, h, song, facts) {
  const lines = [], people = [], music = [];
  if (h.subtitle && song.subtitle) lines.push(song.subtitle);
  if (h.artist && song.artist) people.push(song.artist);
  if (h.arranger && h.arrangerName) people.push("Arranged by " + h.arrangerName);
  if (people.length) lines.push(people.join(" · "));
  if (h.tempo && facts.bpm > 0) music.push(Math.floor(facts.bpm + 0.5) + " BPM");
  if (h.key) {
    if (facts.key) music.push(facts.key);
    if (r.sections.length > 1) music.push("Transposed in " + r.sections.length + " sections");
    else if (r.transposition) music.push("Transpose by " + (r.transposition < 0 ? "+" : "") + -r.transposition);
  }
  if (music.length) lines.push(music.join(" · "));
  if (h.date && song.date) lines.push(song.date);
  return lines;
}
// sheet::HeadingLines.
function headingLines(r, h, song, facts) {
  const first = heading(h.title ? song.title : "", h.difficulty ? r.difficulty : 0);
  return (first ? [first] : []).concat(headerLines(r, h, song, facts));
}
// sheet::SheetText with a header.
function headerText(r, h, song, facts, fileName) {
  const lines = [];
  if (h.title && song.title && song.title !== fileName) lines.push(song.title);
  lines.push(...headerLines(r, h, song, facts));
  if (h.difficulty && r.difficulty) lines.push("Difficulty: " + r.difficulty + " of 10");
  const head = lines.join("\n");
  if (!head) return r.text;
  return head + (r.text ? "\n" + r.text : "");
}
function escapeHtml(text) {
  return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}
// The sheet as HTML, as sheet::ToHtml writes it, with each chord numbered so
// a selection can be traced back to its notes.
function toHtml(r, look) {
  look = look || LOOK;
  let html = "";
  for (let i = 0; i < r.items.length; ++i) {
    const it = r.items[i];
    if (it.kind === CHORD) {
      html += '<span class="chord" data-i="' + i + '" style="color:' + chordColour(it.rhythm, look) + '">';
      let chord = "";
      for (const s of it.segments) chord += s.oor ? '<span class="oor">' + escapeHtml(s.text) + "</span>" : escapeHtml(s.text);
      html += withPedalHighlight(chord, it.pedal || 0, r.pedalMarks || PEDAL_OFF, look);
      html += withPedalHighlight(escapeHtml(it.separator), it.pedalHeld ? it.pedal : 0, r.pedalMarks || PEDAL_OFF, look);
      html += "</span>";
    } else if (it.kind === COMMENT) {
      html += '<br><span class="comment">' + escapeHtml(it.text) + "</span><br>";
    } else {
      const nearComment = (i > 0 && r.items[i - 1].kind === COMMENT) || (i + 1 < r.items.length && r.items[i + 1].kind === COMMENT);
      if (!nearComment) html += "<br>";
    }
  }
  return html;
}
return {defaults, applyRegions, heldByPedal, transposeSections, style, markPedals, toHtml, escapeHtml, heading, sheetText,
        THEMES, LOOK, FONTS, grainTile, grainChannel, GRAIN_TILE, NOTES_SHIFTED, TRANSPOSE,
        HEADER, today, findKey, headerLines, headingLines, headerText};
})();
/*SHEET-CORE-END*/
)js";

// Page UI: settings panel, selection toolbar, copy, save and print.
// In pieces: one string literal holds at most 16 KB.
inline constexpr const char* kPageUi[] = {R"js(
(() => {
const data = JSON.parse(document.getElementById("sheet-data").textContent);
const $ = id => document.getElementById(id);
const STORE = "midipp.sheet.style", PANEL = "midipp.sheet.panel", PRESETS = "midipp.sheet.presets", IMAGE = "midipp.sheet.image";
const readStore = key => { try { return JSON.parse(localStorage.getItem(key) || "null"); } catch (e) { return null; } };
const writeStore = (key, value) => { try { localStorage.setItem(key, JSON.stringify(value)); } catch (e) {} };
const copy = value => JSON.parse(JSON.stringify(value));
// Pedal marks were a switch for a day; on was the one line under the pedal.
const fixMarks = o => { if (typeof o.pedalMarks === "boolean") o.pedalMarks = o.pedalMarks ? 1 : 0; return o; };
const PAGE = {fontSize: 10, lineHeight: 135};
// A style, filled out from the defaults so every key is there.
// The background image is kept apart, under its own key: it is large.
function fullLook(l) {
  const look = Object.assign({}, SheetCore.LOOK, l);
  delete look.image;
  look.rhythm = SheetCore.LOOK.rhythm.map((c, i) => l && Array.isArray(l.rhythm) && typeof l.rhythm[i] === "string" ? l.rhythm[i] : c);
  return look;
}
const full = s => ({options: fixMarks(Object.assign(SheetCore.defaults(), s && s.options)), page: Object.assign({}, PAGE, s && s.page), look: fullLook(s && s.look),
                    header: Object.assign({}, SheetCore.HEADER, s && s.header)});
// The style the page came with: a saved page's own, the app's style page, or
// the defaults. A page the app just wrote without a style page starts from
// what the user last chose in this browser.
const own = () => full({options: data.options, page: data.page, look: data.look, header: data.header});
const ownImage = () => data.look && typeof data.look.image === "string" ? data.look.image : "";
const stored = data.saved || data.styled ? null : readStore(STORE);
const S = stored ? full({options: Object.assign({}, data.options, fixMarks(stored.options || {})), page: Object.assign({}, data.page, stored.page),
                         look: Object.assign({}, data.look, stored.look), header: Object.assign({}, data.header, stored.header)}) : own();
const storedImage = stored ? readStore(IMAGE) : null;
let image = typeof storedImage === "string" ? storedImage : ownImage(), keptImage = storedImage;
const styleOf = () => ({options: S.options, page: S.page, look: S.look, header: S.header});
function remember() {
  writeStore(STORE, styleOf());
  if (image !== keptImage) { writeStore(IMAGE, image); keptImage = image; }
}
const unpackRegion = r => ({from: r[0], to: r[1], semitones: r[2], kind: r[3] || SheetCore.NOTES_SHIFTED});
const notes = data.notes.map(n => ({seconds: n[0], midi: n[1], end: n.length > 2 ? n[2] : -1}));
const tempos = data.tempos.map(t => ({seconds: t[0], bpm: t[1]}));
const meters = data.meters.map(m => ({seconds: m[0], numerator: m[1]}));
const pedals = (data.pedals || []).map(p => ({seconds: p[0], value: p[1], track: p[2]}));
// Sections are the song's: kept in this browser under the file's name and
// its notes, so they come back when the app writes the page again.
const SONG = "midipp.sheet.song." + (data.title || "") + "." + notes.length + "." + (notes.length ? notes[notes.length - 1].seconds : 0);
const song = readStore(SONG) || {};
let regions = (!data.saved && Array.isArray(song.regions) ? song.regions : data.regions).map(unpackRegion);
// The header's text of the song's own; what is left unwritten is the file's
// name for the title and today for the date.
const textOf = t => { const o = {}; for (const k of ["title", "subtitle", "artist", "date"]) if (t && typeof t[k] === "string") o[k] = t[k]; return o; };
let words = textOf(data.saved ? data.song : song.text);
const songText = () => Object.assign({title: data.title || "", subtitle: "", artist: "", date: SheetCore.today()}, words);
const facts = () => ({bpm: tempos.length ? tempos[0].bpm : S.options.missingBpm, key: SheetCore.findKey(SheetCore.applyRegions(notes, regions))});
const headingLines = () => SheetCore.headingLines(result, S.header, songText(), facts());
const sheetText = () => SheetCore.headerText(result, S.header, songText(), facts(), data.title || "");
function rememberSong() { song.regions = regions.map(r => [r.from, r.to, r.semitones, r.kind]); song.text = words; writeStore(SONG, song); }
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

// Undo: every change keeps the style and sections from before it. A stepper
// click is one change; typing or dragging a colour is one until it settles.
const history = [];
let gesture = false;
function record() { history.push({state: JSON.stringify({S, regions, words}), image}); if (history.length > 200) history.shift(); }
function settle(now) { remember(); rememberSong(); if (now) draw(); else redraw(); }
function change(apply, now) { gesture = false; record(); apply(); settle(now); }
function preview(apply) { if (!gesture) { record(); gesture = true; } apply(); settle(); }
// Changes are drawn once a frame.
let frame = 0;
function redraw() { if (!frame) frame = requestAnimationFrame(() => { frame = 0; draw(); }); }

// Controls. Each keeps itself up to date through an updater run after
// every draw.
const updaters = [];
function make(tag, props, children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props || {})) {
    if (key === "class") node.className = value;
    else if (key === "text") node.textContent = value;
    else node.setAttribute(key, value);
  }
  for (const child of children || []) if (child !== null && child !== undefined) node.appendChild(typeof child === "string" ? document.createTextNode(child) : child);
  return node;
}
// choices: [value, label, dot colours or none].
function segment(get, set, choices) {
  const box = make("div", {class: "seg", role: "group"});
  const buttons = choices.map(([value, label, dots]) => {
    const marks = dots ? dots().map(() => make("span", {class: "dot"})) : [];
    const button = make("button", {type: "button"}, marks.concat([label]));
    button.onclick = () => { if (get() !== value) change(() => set(value)); };
    box.appendChild(button);
    return {value, button, marks, dots};
  });
  updaters.push(() => buttons.forEach(b => {
    b.button.setAttribute("aria-pressed", get() === b.value);
    if (b.dots) b.dots().forEach((c, i) => { b.marks[i].style.background = c; });
  }));
  return box;
}
function clampTo(o, v) { return Math.min(o.max, Math.max(o.min, v)); }
// A number typed or stepped; bounds o.min and o.max, o.step per click.
function stepper(get, set, o) {
  const step = o.step || 1;
  const input = make("input", {type: "number", min: o.min, max: o.max, step: "any", inputmode: "decimal"});
  const less = make("button", {type: "button", "aria-label": "Less"}, ["−"]);
  const more = make("button", {type: "button", "aria-label": "More"}, ["+"]);
  const bump = d => { const v = clampTo(o, Math.round((get() + d) / step) * step); if (v !== get()) change(() => set(v)); };
  less.onclick = () => bump(-step);
  more.onclick = () => bump(step);
  typed(input, get, set, o);
  updaters.push(() => { if (document.activeElement !== input) input.value = get(); less.disabled = get() <= o.min; more.disabled = get() >= o.max; });
  return make("div", {class: "stepper"}, [less, input, o.unit ? make("span", {class: "unit", text: o.unit}) : null, more]);
}
function typed(input, get, set, o) {
  const commit = () => {
    const v = Number(input.value);
    if (input.value.trim() === "" || !isFinite(v)) { input.value = get(); return; }
    const c = clampTo(o, o.whole === false ? v : Math.round(v));
    input.value = c;
    if (c !== get()) change(() => set(c));
  };
  input.addEventListener("change", commit);
  input.addEventListener("keydown", e => { if (e.key === "Enter") input.blur(); });
}
function number(get, set, o) {
  const input = make("input", {type: "number", min: o.min, max: o.max, step: o.step || 1});
  typed(input, get, set, o);
  updaters.push(() => { if (document.activeElement !== input) input.value = get(); });
  return make("div", {class: "num"}, [input, o.unit ? make("span", {class: "unit", text: o.unit}) : null]);
}
function toggle(get, set) {
  const input = make("input", {type: "checkbox", role: "switch", class: "switch"});
  input.onchange = () => change(() => set(input.checked));
  updaters.push(() => { input.checked = !!get(); });
  return input;
}
function text(get, set, props) {
  const input = make("input", Object.assign({type: "text"}, props || {}));
  input.addEventListener("input", () => preview(() => set(input.value)));
  input.addEventListener("change", () => { gesture = false; });
  updaters.push(() => { if (document.activeElement !== input) input.value = get(); });
  return input;
}
const hex = c => c === "white" ? "#ffffff" : c;
const rgbOf = c => [1, 3, 5].map(i => parseInt(hex(c).substr(i, 2), 16));
function colour(get, set) {
  const input = make("input", {type: "color"});
  input.addEventListener("input", () => preview(() => set(input.value)));
  input.addEventListener("change", () => { gesture = false; });
  updaters.push(() => { if (input.value !== hex(get())) input.value = hex(get()); });
  return input;
}
// A row: its label and control, shown while o.when holds; o.sub indents a
// setting that depends on the one above it.
function row(label, control, o) {
  o = o || {};
  const node = make("div", {class: "row" + (o.stack ? " stack" : "") + (o.sub ? " sub" : "")}, [make("span", {text: label}), control]);
  if (o.when) updaters.push(() => { node.hidden = !o.when(); });
  return node;
}
function heading(label, when) {
  const node = make("h3", {text: label});
  if (when) updaters.push(() => { node.hidden = !when(); });
  return node;
}
const panelOpen = Object.assign({sheet: true, look: true, notation: false, advanced: false}, readStore(PANEL) || {});
function fold(id, label, children) {
  const head = make("button", {type: "button", class: "fold-head", "aria-expanded": !!panelOpen[id]}, [label]);
  head.onclick = () => { panelOpen[id] = !panelOpen[id]; head.setAttribute("aria-expanded", panelOpen[id]); writeStore(PANEL, panelOpen); };
  return make("section", {class: "fold"}, [head, make("div", {class: "fold-body"}, children)]);
}
)js",
R"js(
// The panel.
const o = () => S.options;
const flag = key => toggle(() => o()[key], v => { o()[key] = v; });
const choose = (key, choices) => segment(() => o()[key], v => { o()[key] = v; }, choices);
const step = (key, opts) => stepper(() => o()[key], v => { o()[key] = v; }, opts);
const field = (key, opts) => number(() => o()[key], v => { o()[key] = v; }, opts);
const found = make("span", {class: "found"});
updaters.push(() => {
  const many = result.sections.length > 1;
  found.hidden = !o().autoTranspose;
  found.textContent = many ? result.sections.length + " sections" : signed(result.transposition) || "0";
});
const L = () => S.look;
const pedalDown = () => L().pedalDown, pedalUp = () => L().pedalUp;
// A colour of the look's own makes it a custom theme.
const tint = key => colour(() => L()[key], v => { L()[key] = v; L().theme = 3; });
const sectionsBlock = make("div", {}, [make("h3", {text: "Sections"}), make("ul", {id: "sections"}), make("button", {type: "button", id: "keep"}, ["Keep these sections"])]);
const sheetRows = [
  row("Transposition", segment(() => !o().autoTranspose ? 0 : o().autoSections ? 2 : 1,
                               v => { o().autoTranspose = v > 0; o().autoSections = v === 2; },
                               [[0, "Fixed"], [1, "Best key"], [2, "Best per section"]]), {stack: true}),
  row("Semitones", make("div", {class: "inline"}, [found, step("transpose", {min: -24, max: 24})])),
  row("Line breaks", choose("breaks", [[0, "Bars"], [3, "Phrases"], [1, "Beats"], [2, "None"]]), {stack: true}),
  row("Beats per line", step("beats", {min: 1, max: 32}), {sub: true, when: () => o().breaks === 1}),
  row("Pedal marks", choose("pedalMarks", [[0, "Off"], [1, "Down", () => [pedalDown()]], [2, "Up", () => [pedalUp()]],
                                           [3, "Both", () => [pedalDown(), pedalUp()]]]), {stack: true, when: () => pedals.length > 0}),
  sectionsBlock,
];
// The header: each line on or off, with the song's own words beside it.
const H = () => S.header;
const shown = key => toggle(() => H()[key], v => { H()[key] = v; });
const worded = (key, get, set) => {
  const input = text(get, set, {"aria-label": key});
  updaters.push(() => { input.hidden = !H()[key]; });
  return make("div", {class: "inline"}, [input, shown(key)]);
};
const said = key => worded(key, () => songText()[key], v => { words[key] = v; });
const headerRows = [
  make("h3", {text: "Header"}),
  row("Title", said("title")),
  row("Subtitle", said("subtitle")),
  row("Artist", said("artist")),
  row("Arranger", worded("arranger", () => H().arrangerName, v => { H().arrangerName = v; })),
  row("Tempo", shown("tempo")),
  row("Key", shown("key")),
  row("Difficulty", shown("difficulty")),
  row("Date", said("date")),
  row("Alignment", segment(() => H().centre, v => { H().centre = v; }, [[false, "Left"], [true, "Centre"]])),
];
sheetRows.splice(sheetRows.length - 1, 0, ...headerRows);
// A picture for the background, at most 1600 pixels along its long edge.
const picker = make("input", {type: "file", accept: "image/*", hidden: ""});
picker.onchange = () => {
  const file = picker.files && picker.files[0];
  picker.value = "";
  if (!file) return;
  const url = URL.createObjectURL(file), picture = new Image();
  picture.onload = () => {
    URL.revokeObjectURL(url);
    const shrink = Math.min(1, 1600 / Math.max(picture.naturalWidth, picture.naturalHeight));
    const canvas = document.createElement("canvas");
    canvas.width = Math.max(1, Math.round(picture.naturalWidth * shrink));
    canvas.height = Math.max(1, Math.round(picture.naturalHeight * shrink));
    const context = canvas.getContext("2d");
    context.fillStyle = hex(L().background);
    context.fillRect(0, 0, canvas.width, canvas.height);
    context.drawImage(picture, 0, 0, canvas.width, canvas.height);
    const shrunk = canvas.toDataURL("image/jpeg", 0.88);
    change(() => { image = shrunk; L().ground = 1; });
  };
  picture.onerror = () => URL.revokeObjectURL(url);
  picture.src = url;
};
const chooseImage = make("button", {type: "button"}, ["Choose image"]);
chooseImage.onclick = () => picker.click();
const lookChoose = (key, choices) => segment(() => L()[key], v => { L()[key] = v; }, choices);
const lookRows = [
  row("Theme", segment(() => L().theme, v => {
    if (v < 3) Object.assign(L(), copy(SheetCore.THEMES[v]), {theme: v}); else L().theme = 3;
  }, [[0, "Dark"], [1, "Paper"], [2, "White"], [3, "Custom"]]), {stack: true}),
  row("Background", lookChoose("ground", [[0, "Colour"], [1, "Image"], [2, "Grain"]]), {stack: true}),
  row("Background colour", tint("background")),
  row("Image", make("div", {class: "inline"}, [chooseImage, picker]), {sub: true, when: () => L().ground === 1}),
  row("Fit", lookChoose("fit", [[0, "Cover"], [1, "Tile"]]), {sub: true, when: () => L().ground === 1 && !!image}),
  row("Dim", stepper(() => L().dim, v => { L().dim = v; }, {min: 0, max: 90, step: 5, unit: "%"}), {sub: true, when: () => L().ground === 1 && !!image}),
  row("Grain", stepper(() => L().grain, v => { L().grain = v; }, {min: 0, max: 100, step: 5, unit: "%"}), {sub: true, when: () => L().ground === 2}),
  row("Colours", lookChoose("oneColour", [[false, "Rhythm"], [true, "One colour"]]), {stack: true}),
  row("Text", tint("text")),
  row("Comments", tint("comment")),
  row("Header", tint("heading")),
  row("Pedal down", tint("pedalDown"), {when: () => pedals.length > 0}),
  row("Pedal up", tint("pedalUp"), {when: () => pedals.length > 0}),
  row("Text size", stepper(() => S.page.fontSize, v => { S.page.fontSize = v; }, {min: 6, max: 24, unit: "pt"})),
  row("Line height", stepper(() => S.page.lineHeight, v => { S.page.lineHeight = v; }, {min: 100, max: 250, step: 5, unit: "%"})),
];
const RHYTHMS = ["64th", "32nd", "16th", "8th", "Quarter", "Half", "Whole", "Double whole", "Longer"];
const rhythmBox = make("div", {class: "swatches"}, RHYTHMS.map((label, i) =>
  make("label", {}, [colour(() => L().rhythm[i], v => { L().rhythm[i] = v; L().theme = 3; }), make("span", {text: label})])));
const notationRows = [
  row("Chord window", step("quantizeMs", {min: 0, max: 200, step: 5, unit: "ms"})),
  row("Keep spread chords in played order", flag("sequentialQuantize")),
  row("Write spread chords in braces", flag("curlyQuantizes")),
  row("Classic chord order", flag("classicChordOrder")),
  row("Shifted characters", choose("shifts", [[0, "First"], [1, "Last"], [2, "In pitch order"]]), {stack: true}),
  row("Rhythm separators", flag("tempoMarks")),
  row("Mention tempo changes", flag("bpmChanges")),
  row("Wording", choose("bpmStyle", [[0, "Detailed"], [1, "Arrows"]]), {sub: true, when: () => o().bpmChanges}),
  row("Smallest change", step("minSpeedChange", {min: 0, max: 100, step: 5, unit: "%"}), {sub: true, when: () => o().bpmChanges}),
  row("Keep out-of-range notes", flag("showOutOfRange")),
  row("Placement", choose("outOfRangePlace", [[0, "First"], [1, "Last"], [2, "Low first, high last"]]), {stack: true, sub: true, when: () => o().showOutOfRange}),
  row("Mark them", flag("outOfRangeMarks"), {sub: true, when: () => o().showOutOfRange}),
  row("Separator", text(() => o().outOfRangeSeparator, v => { o().outOfRangeSeparator = v; }, {maxlength: 7}), {sub: true, when: () => o().showOutOfRange && o().outOfRangeMarks}),
];
const perSection = () => o().autoTranspose && o().autoSections;
const advancedRows = [
  row("Keep unless better by", field("resilience", {min: 0, max: 20, unit: "notes"}), {when: () => o().autoTranspose}),
  row("Section switch cost", field("sectionSwitchCost", {min: 0, max: 60, unit: "notes"}), {when: perSection}),
  row("Shortest section", field("sectionMinSeconds", {min: 0, max: 60, unit: "s", whole: false, step: 0.5}), {when: perSection}),
  row("Rest before a switch", field("sectionRestMs", {min: 0, max: 2000, step: 50, unit: "ms"}), {when: perSection}),
  row("Section search range", field("sectionRange", {min: 1, max: 24, unit: "semitones"}), {when: perSection}),
  row("Tempo when the file names none", field("missingBpm", {min: 20, max: 400, unit: "BPM", whole: false})),
  row("Rhythm colours", rhythmBox, {stack: true, when: () => !L().oneColour}),
  row("Font", lookChoose("font", [[0, "Verdana"], [1, "Segoe UI"], [2, "Consolas"]]), {stack: true}),
];
const panel = $("panel");
panel.appendChild(fold("sheet", "Sheet", sheetRows));
panel.appendChild(fold("look", "Look", lookRows));
panel.appendChild(fold("notation", "Notation", notationRows));
panel.appendChild(fold("advanced", "Advanced", advancedRows));

// Presets: the two the page knows and the user's own, by name.
const canon = value => Array.isArray(value) ? "[" + value.map(canon).join(",") + "]" :
  value && typeof value === "object" ? "{" + Object.keys(value).sort().map(k => JSON.stringify(k) + ":" + canon(value[k])).join(",") + "}" : JSON.stringify(value);
const BUILT_IN = [["Default", () => full({})], ["Compact", () => full({options: {breaks: 0}, page: {fontSize: 9, lineHeight: 115}})]];
let presets = (readStore(PRESETS) || []).filter(p => p && typeof p.name === "string" && p.style);
const presetBox = $("presets");
let naming = false;
// The arranger is the user's name, not a style: presets and Reset keep it.
const takeStyle = s => { s.header.arrangerName = S.header.arrangerName; S.options = s.options; S.page = s.page; S.look = s.look; S.header = s.header; };
const presetKey = style => { const s = full(style); delete s.header.arrangerName; return canon(s); };
function usePreset(style) { change(() => takeStyle(full(copy(style)))); }
function renderPresets() {
  presetBox.innerHTML = "";
  const chip = (name, style, onRemove) => {
    const button = make("button", {type: "button", class: "chip"}, [name]);
    button._style = presetKey(style);
    button.onclick = e => { if (!e.target.classList.contains("remove")) usePreset(style); };
    if (onRemove) {
      const remove = make("span", {class: "remove", role: "button", "aria-label": "Remove " + name}, ["×"]);
      remove.onclick = onRemove;
      button.appendChild(remove);
    }
    presetBox.appendChild(button);
  };
  for (const [name, style] of BUILT_IN) chip(name, style());
  presets.forEach((p, i) => chip(p.name, p.style, () => { presets.splice(i, 1); writeStore(PRESETS, presets); renderPresets(); }));
  if (naming) {
    const input = make("input", {type: "text", maxlength: 40, value: "Preset " + (presets.length + 1)});
    const done = keep => {
      if (!naming) return;
      naming = false;
      const name = input.value.trim();
      if (keep && name) { presets = presets.filter(p => p.name !== name).concat([{name, style: copy(styleOf())}]); writeStore(PRESETS, presets); }
      renderPresets();
    };
    input.addEventListener("keydown", e => { if (e.key === "Enter") done(true); else if (e.key === "Escape") done(false); });
    input.addEventListener("blur", () => done(true));
    presetBox.appendChild(input);
    input.focus();
    input.select();
  } else {
    const add = make("button", {type: "button", class: "chip"}, ["Save preset"]);
    add.onclick = () => { naming = true; renderPresets(); };
    presetBox.appendChild(add);
  }
  markPresets();
}
function markPresets() {
  const current = presetKey(styleOf());
  for (const chip of presetBox.querySelectorAll(".chip")) if (chip._style) chip.setAttribute("aria-pressed", chip._style === current);
}
renderPresets();
)js",
R"js(
// Paper grain, drawn here as sheet::detail::PaintBackground draws it.
let grainValues = null;
const grainUrls = {};
function grainUrl(background, grain) {
  const key = background + "/" + grain;
  if (grainUrls[key]) return grainUrls[key];
  grainValues = grainValues || SheetCore.grainTile();
  const size = SheetCore.GRAIN_TILE, canvas = document.createElement("canvas");
  canvas.width = canvas.height = size;
  const context = canvas.getContext("2d"), pixels = context.createImageData(size, size), rgb = rgbOf(background);
  for (let i = 0; i < size * size; ++i) {
    for (let c = 0; c < 3; ++c) pixels.data[i * 4 + c] = SheetCore.grainChannel(rgb[c], grainValues[i], grain);
    pixels.data[i * 4 + 3] = 255;
  }
  context.putImageData(pixels, 0, 0);
  return grainUrls[key] = canvas.toDataURL("image/png");
}
// The look's background as CSS, for the page and its picture.
function backgroundCss() {
  const look = L(), css = {"background-color": hex(look.background)};
  if (look.ground === 2 && look.grain > 0) Object.assign(css, {"background-image": "url(" + grainUrl(look.background, look.grain) + ")", "background-repeat": "repeat"});
  else if (look.ground === 1 && image) {
    const shade = "rgba(" + rgbOf(look.background).join(",") + "," + look.dim / 100 + ")";
    css["background-image"] = "linear-gradient(" + shade + "," + shade + "),url(" + image + ")";
    Object.assign(css, look.fit === 1 ? {"background-repeat": "repeat"} : {"background-size": "cover", "background-position": "center", "background-repeat": "no-repeat"});
  }
  return css;
}
const cssText = css => Object.entries(css).map(([k, v]) => k + ":" + v).join(";");
let drawnBackground = "";
function applyLook() {
  const look = L(), page = $("page"), background = cssText(backgroundCss());
  if (background !== drawnBackground) { page.setAttribute("style", background); drawnBackground = background; }
  page.style.setProperty("--comment", look.comment);
  document.body.style.setProperty("--sheet-bg", hex(look.background));
  for (const id of ["sheet", "heading"]) $(id).style.fontFamily = SheetCore.FONTS[look.font];
  $("sheet").style.color = look.text;
  $("heading").style.color = look.heading;
}
function draw() {
  result = SheetCore.style(SheetCore.heldByPedal(SheetCore.applyRegions(notes, regions), pedals), data.mapping, tempos, meters, S.options, SheetCore.transposeSections(regions));
  SheetCore.markPedals(result, pedals, S.options);
  const sheet = $("sheet");
  sheet.innerHTML = SheetCore.toHtml(result, L());
  sheet.style.fontSize = S.page.fontSize + "pt";
  sheet.style.lineHeight = S.page.lineHeight + "%";
  applyLook();
  for (const span of sheet.querySelectorAll(".chord")) {
    const it = result.items[+span.dataset.i];
    const inside = regions.some(r => it.ms / 1000 >= r.from && it.msEnd / 1000 <= r.to);
    span.classList.toggle("in-section", inside);
  }
  let count = result.notes + " notes, " + result.groups + " chords.";
  if (result.merged) count += " " + result.merged + " shared notes merged.";
  if (result.unmapped) count += " " + result.unmapped + " unmapped.";
  if (result.hidden) count += " " + result.hidden + " out of range left out.";
  if (result.difficulty) count += " Difficulty " + result.difficulty + " of 10.";
  $("count").textContent = count;
  const heading = $("heading");
  heading.textContent = headingLines().join("\n");
  heading.style.fontSize = S.page.fontSize + "pt";
  heading.style.textAlign = S.header.centre ? "center" : "";
  // Every run of one transposition when there is more than one, then the
  // shifted runs. A run the user made can be removed; a found one cannot,
  // but Keep turns the found runs into the user's.
  const list = $("sections");
  list.innerHTML = "";
  const entry = (label, onRemove) => {
    const li = make("li", {}, [make("span", {text: label})]);
    li.firstChild.style.flex = "1";
    if (onRemove) {
      const remove = make("button", {type: "button", class: "small"}, ["Remove"]);
      remove.onclick = () => change(onRemove, true);
      li.appendChild(remove);
    }
    list.appendChild(li);
  };
  if (result.sections.length > 1) result.sections.forEach(s => {
    const own = regions.findIndex(r => r.kind === SheetCore.TRANSPOSE && sameRun(r, s));
    entry(time(s.from) + " to " + time(s.to) + ", transposed " + signed(s.semitones), own >= 0 ? () => regions.splice(own, 1) : null);
  });
  regions.forEach((r, i) => {
    if (r.kind === SheetCore.TRANSPOSE && !result.sections.some(s => sameRun(r, s)))
      entry(time(r.from) + " to " + time(r.to) + ", transposed " + signed(r.semitones) + ", inside another section", () => regions.splice(i, 1));
    else if (r.kind !== SheetCore.TRANSPOSE)
      entry(time(r.from) + " to " + time(r.to) + ", notes shifted " + signed(r.semitones), () => regions.splice(i, 1));
  });
  sectionsBlock.hidden = !list.children.length;
  $("keep").hidden = !(S.options.autoTranspose && S.options.autoSections && result.sections.length > 1);
  updaters.forEach(update => update());
  $("undo").disabled = !history.length;
  markPresets();
}
$("keep").onclick = () => change(() => {
  regions = regions.filter(r => r.kind !== SheetCore.TRANSPOSE)
    .concat(result.sections.map(s => ({from: s.from, to: s.to, semitones: s.semitones, kind: SheetCore.TRANSPOSE})));
  S.options.autoSections = false;
}, true);
$("undo").onclick = () => {
  if (!history.length) return;
  const entry = history.pop(), before = JSON.parse(entry.state);
  S.options = before.S.options; S.page = before.S.page; S.look = before.S.look; S.header = before.S.header;
  regions = before.regions;
  words = before.words;
  image = entry.image;
  gesture = false;
  settle(true);
};
// Reset returns to the style and sections the page came with: a saved page's
// own, the app's style page, or the defaults.
$("reset").onclick = () => change(() => {
  takeStyle(own());
  image = ownImage();
  regions = data.regions.map(unpackRegion);
  words = textOf(data.saved ? data.song : null);
}, true);
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
  $("selection-amount").textContent = signed(shiftAt(pending.from)) || "0";
  $("shift-amount").textContent = signed(shifted ? shifted.semitones : 0) || "0";
}
// The toolbar sits above the selection and follows it as the sheet scrolls.
let selectedRange = null;
function placeToolbar() {
  if (!selectedRange || toolbar.style.display === "none") return;
  const rect = selectedRange.getBoundingClientRect();
  toolbar.style.left = Math.max(8, Math.min(window.innerWidth - toolbar.offsetWidth - 8, rect.left)) + "px";
  toolbar.style.top = Math.max(8, rect.top - toolbar.offsetHeight - 8) + "px";
}
document.addEventListener("selectionchange", () => {
  const region = selectionRegion();
  if (!region) { if (!toolbar.contains(document.activeElement)) toolbar.style.display = "none"; return; }
  pending = region;
  selectedRange = window.getSelection().getRangeAt(0).cloneRange();
  toolbar.style.display = "flex";
  placeToolbar();
  $("selection-range").textContent = time(region.from) + " to " + time(region.to);
  showAmounts();
});
document.querySelector("main").addEventListener("scroll", placeToolbar);
window.addEventListener("scroll", placeToolbar);
window.addEventListener("resize", placeToolbar);
// Transpose a part selects the first line in view; the toolbar takes it from there.
$("pick").onclick = () => {
  const top = Math.max(0, document.querySelector("main").getBoundingClientRect().top);
  const spans = $("sheet").querySelectorAll(".chord");
  let first = -1;
  for (let i = 0; i < spans.length; ++i) if (spans[i].getBoundingClientRect().bottom > top) { first = i; break; }
  if (first < 0) return;
  const lineTop = spans[first].getBoundingClientRect().top;
  let last = first;
  while (last + 1 < spans.length && spans[last + 1].getBoundingClientRect().top < lineTop + 2) ++last;
  const range = document.createRange();
  range.setStartBefore(spans[first]);
  range.setEndAfter(spans[last]);
  const selection = window.getSelection();
  selection.removeAllRanges();
  selection.addRange(range);
};
// Transpose sets the selection's transposition, on top of the sheet's or
// the search's, and the sheet says so where it starts and ends.
function transposeSelection(by) {
  if (!pending) return;
  change(() => {
    const existing = regions.find(r => r.kind === SheetCore.TRANSPOSE && sameRun(r, pending));
    if (existing) existing.semitones += by;
    else regions.push({from: pending.from, to: pending.to, semitones: shiftAt(pending.from) + by, kind: SheetCore.TRANSPOSE});
  }, true);
  showAmounts();
}
// Shift notes moves the selection's notes and the reader sees other keys.
function shiftSelection(by) {
  if (!pending) return;
  change(() => {
    const existing = regions.find(r => r.kind !== SheetCore.TRANSPOSE && sameRun(r, pending));
    if (existing) existing.semitones += by; else regions.push({from: pending.from, to: pending.to, semitones: by, kind: SheetCore.NOTES_SHIFTED});
    regions = regions.filter(r => r.kind === SheetCore.TRANSPOSE || r.semitones !== 0);
  }, true);
  showAmounts();
}
$("selection-down").onclick = () => transposeSelection(-1);
$("selection-up").onclick = () => transposeSelection(1);
$("shift-down").onclick = () => shiftSelection(-1);
$("shift-up").onclick = () => shiftSelection(1);
$("selection-close").onclick = () => { toolbar.style.display = "none"; window.getSelection().removeAllRanges(); };
)js",
R"js(
function copyText(value) {
  const fallback = () => {
    const area = document.createElement("textarea");
    area.value = value;
    document.body.appendChild(area);
    area.select();
    let ok = false;
    try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
    area.remove();
    return ok;
  };
  if (navigator.clipboard && navigator.clipboard.writeText) return navigator.clipboard.writeText(value).then(() => true, fallback);
  return Promise.resolve(fallback());
}
$("copy").onclick = () => copyText(sheetText()).then(ok => {
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
  const saved = Object.assign({}, data, {saved: true, options: S.options, page: S.page, look: Object.assign({}, S.look, image ? {image} : {}),
                                         header: S.header, song: songText(), regions: regions.map(r => [r.from, r.to, r.semitones, r.kind]), expected: result.text});
  delete saved.styled;
  const element = $("sheet-data");
  const before = element.textContent;
  element.textContent = JSON.stringify(saved).replace(/<\//g, "<\\/");
  toolbar.style.display = "none";
  // The background is drawn again from the data; the image goes in once.
  const pageStyle = $("page").getAttribute("style");
  $("page").removeAttribute("style");
  const html = "<!doctype html>\n" + document.documentElement.outerHTML;
  $("page").setAttribute("style", pageStyle || "");
  element.textContent = before;
  report($("save"), saveBlob(new Blob([html], {type: "text/html"}), (data.title || "Sheet") + ".html", "Sheet page", {"text/html": [".html"]}), "Save page");
};
// The sheet as a picture: the sheet's own markup, drawn by the browser
// inside an SVG image and copied to a canvas at twice the size so the text
// stays sharp. The heading goes on top, as Print shows it.
function sheetImage() {
  const sheet = $("sheet");
  const width = Math.ceil(sheet.getBoundingClientRect().width);
  const look = L();
  const css = "font-family:" + SheetCore.FONTS[look.font] + ";font-size:" + S.page.fontSize + "pt;line-height:" + S.page.lineHeight + "%;color:" + look.text + ";" +
    "white-space:pre-wrap;" + cssText(backgroundCss()) + ";padding:16px;box-sizing:border-box;width:" + (width + 32) + "px;margin:0";
  const rules = ".oor{display:inline-flex;justify-content:center;min-width:.6em;border-bottom:2px solid;font-weight:900}.comment{color:" + look.comment + "}";
  const lines = headingLines();
  const body = (lines.length ? '<div style="color:' + look.heading + ";margin-bottom:1.35em;text-align:" + (S.header.centre ? "center" : "left") + '">' +
                lines.map(SheetCore.escapeHtml).join("<br/>") + "</div>" : "") + sheet.innerHTML.replace(/<br>/g, "<br/>");
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
      // Twice the size, or as large as the browser's canvas allows for a
      // long sheet: 32767 pixels a side and 268 million in all.
      const scale = Math.min(2, 32767 / height, 32767 / (width + 32), Math.sqrt(268000000 / ((width + 32) * height)));
      const canvas = document.createElement("canvas");
      canvas.width = Math.floor((width + 32) * scale);
      canvas.height = Math.floor(height * scale);
      const context = canvas.getContext("2d");
      context.scale(scale, scale);
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
  const check = SheetCore.style(SheetCore.heldByPedal(SheetCore.applyRegions(notes, written), pedals), data.mapping, tempos, meters,
                                Object.assign(SheetCore.defaults(), data.options), SheetCore.transposeSections(written)).text;
  if (check !== data.expected) {
    console.error("The page's sheet differs from the app's. Expected:\n" + data.expected + "\nGot:\n" + check);
    $("parity").classList.add("shown");
  }
}
})();
)js"};

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

// The header's facts of the music: the first tempo, or the tempo used when the
// file names none, and the key of the notes as they are played.
inline HeaderFacts FactsOf(const PageInput& in) {
    return {in.tempos.empty() ? in.options.missingBpm : in.tempos.front().bpm, FindKey(ShiftedNotes(in))};
}
// A sheet written by the app has no text of the song's own: its file's name is
// the title and today the date.
inline SongText FileSongText(const PageInput& in) { return {in.title, "", "", Today()}; }

// Builds the editor page. The initial sheet markup is the C++ rendering; the
// script redraws on load and checks against it. If rendered is non-null it
// receives that rendering (for the counts).
inline std::string ToEditorHtml(const PageInput& in, StyledResult* rendered = nullptr) {
    auto initial = Style(HeldByPedal(ShiftedNotes(in), in.pedals), in.mapping, in.tempos, in.meters, in.options, TransposeSections(in));
    MarkPedals(initial, in.pedals, in.options);
    if (rendered) *rendered = initial;
    std::string html = "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>" + detail::EscapeHtml(in.title) +
        "</title><style>" + detail::kPageCss + "</style></head><body>\n"
        "<header><h1 id=\"title\">" + detail::EscapeHtml(in.title) + "</h1><span class=\"count\" id=\"count\"></span>"
        "<button id=\"pick\">Transpose a part</button><button id=\"copy\" class=\"primary\">Copy sheet</button><button id=\"image\">Save image</button>"
        "<button id=\"save\">Save page</button><button id=\"print\">Print</button></header>\n"
        "<div id=\"layout\"><aside><div id=\"presets\"></div><div id=\"panel\"></div>"
        "<footer id=\"panel-foot\"><button id=\"undo\">Undo</button><button id=\"reset\">Reset</button></footer></aside>\n"
        "<main><p id=\"parity\">The page's sheet differs from the app's. Copy styled sheet in the app gives the app's version.</p>"
        "<div id=\"page\"><div id=\"heading\"></div><div id=\"sheet\">";
    // Pre-rendered so the sheet shows before the script runs.
    html += detail::SheetBody(initial, in.look);
    html += "</div></div></main></div>\n"
        "<div id=\"selection\"><span id=\"selection-range\"></span><span>Transpose</span>"
        "<button id=\"selection-down\" class=\"small\">-</button><output id=\"selection-amount\">0</output>"
        "<button id=\"selection-up\" class=\"small\">+</button><span>Shift notes</span>"
        "<button id=\"shift-down\" class=\"small\">-</button><output id=\"shift-amount\">0</output>"
        "<button id=\"shift-up\" class=\"small\">+</button><button id=\"selection-close\" class=\"small\">Done</button></div>\n"
        "<script id=\"sheet-data\" type=\"application/json\">" + detail::PageJson(in, initial.text) + "</script>\n"
        "<script>" + detail::kPageCore;
    for (const char* part : detail::kPageUi) html += part;
    html += "</script>\n</body></html>\n";
    return html;
}

} // namespace sheet
