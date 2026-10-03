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
        ",\"dim\":" + std::to_string(look.dim) +
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
:root{--canvas:#191C21;--bar:#20242A;--card:#272B31;--raised:#2E333A;--hot:#363C44;--well:#14171B;--ink:#E7E9EC;--ink2:#A3AAB5;--ink3:#79818D;
--accent:#4EA3EA;--accent-hot:#66B0EE;--accent-soft:rgba(78,163,234,.14);--accent-ink:#101218;
--ok:#8FDCAA;--ok-soft:rgba(79,178,119,.16);--ok-line:rgba(79,178,119,.40);--ok-fill:#283B36;--ok-hot:#2A433B;
--bad:#E0685A;--line:rgba(255,255,255,.08);--line2:rgba(255,255,255,.14);
--hi:inset 0 1px 0 rgba(255,255,255,.06);--raise:0 1px 1px rgba(0,0,0,.30),0 2px 6px rgba(0,0,0,.22);--sunk:inset 0 1px 2px rgba(0,0,0,.45);
--h:32px;--w:154px;--panel:360px;--ui:"Segoe UI Variable Text","Segoe UI",system-ui,sans-serif;color-scheme:dark}
*{box-sizing:border-box} [hidden]{display:none!important} html,body{margin:0;height:100%}
body{background:var(--canvas);color:var(--ink);font:14px/20px var(--ui);display:flex;flex-direction:column}
button,input{font:inherit;color:inherit;margin:0}
.i{width:16px;height:16px;flex:none;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
button{display:inline-flex;align-items:center;justify-content:center;gap:6px;flex:none;height:var(--h);padding:0 12px;border:1px solid var(--line);border-radius:10px;background:var(--raised);box-shadow:var(--raise),var(--hi);color:var(--ink);font-weight:400;white-space:nowrap;cursor:pointer}
button:hover{background:var(--hot)} button:active{background:var(--raised);box-shadow:var(--sunk)}
button:disabled{background:transparent;border:1px dashed var(--line2);box-shadow:none;color:var(--ink3);cursor:default}
button.primary{background:var(--accent);border-color:transparent;color:var(--accent-ink);font-weight:600} button.primary:hover{background:var(--accent-hot)}
button.icon{width:var(--h);padding:0}
.say{display:inline-grid}.say>span{grid-area:1/1;text-align:center}.say>span:not(.on){visibility:hidden}
header{flex:none;display:flex;align-items:center;gap:16px;height:48px;padding:0 16px;background:var(--bar);border-bottom:1px solid var(--line)}
.titles{flex:1 1 0;min-width:0;display:flex;align-items:baseline;gap:12px}
header h1{flex:0 1 auto;min-width:0;max-width:60%;margin:0;font-size:14px;line-height:20px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#count{flex:1 1 0;min-width:0;font-size:12px;line-height:16px;color:var(--ink2);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-variant-numeric:tabular-nums}
.tools{flex:none;display:flex;align-items:center;gap:8px} #copy{margin-left:8px} #view{display:none}
#layout{flex:1;display:flex;min-height:0}
aside{flex:none;width:var(--panel);display:flex;flex-direction:column;min-height:0;background:var(--bar);border-right:1px solid var(--line)}
#presets{flex:none;display:flex;flex-wrap:wrap;align-items:center;gap:8px;padding:8px 16px;border-bottom:1px solid var(--line)}
#panel{flex:1;overflow:auto;padding:0 16px;scrollbar-width:thin;scrollbar-gutter:stable;scrollbar-color:var(--line2) transparent}
.fold+.fold{border-top:1px solid var(--line)}
.fold-head{width:100%;height:32px;justify-content:flex-start;gap:4px;padding:0;border:0;border-radius:0;background:none;box-shadow:none;font-weight:600}
.fold-head:hover,.fold-head:active{background:none;box-shadow:none} .fold-head .i{color:var(--ink2)} .fold-head .chev{transition:transform .12s}
.fold-head .chev+.i{margin-right:4px} .fold-head:hover .i{color:var(--ink)} .fold-head[aria-expanded=true] .chev{transform:rotate(90deg)} .fold-head:focus-visible{outline-offset:-2px}
.fold-head[aria-expanded=false]+.fold-body{display:none}
.fold-body{display:flex;flex-direction:column;gap:8px;padding-bottom:16px} .group{margin-top:8px}
)css"
R"css(
.row{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));align-items:center;column-gap:8px;min-height:var(--h)}
.row>:first-child{min-width:0;overflow-wrap:break-word} .row:not(.stack)>:nth-child(2){width:100%}
.row.sub>:first-child{padding-left:16px} .row.stack{grid-template-columns:minmax(0,1fr);row-gap:4px}
.pair{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));column-gap:8px}
.label-line{display:flex;align-items:baseline;justify-content:space-between;gap:8px;line-height:18px}
.result{font-size:12px;line-height:16px;color:var(--ink2);font-variant-numeric:tabular-nums;white-space:nowrap}
.heading{display:flex;align-items:baseline;gap:8px;font-size:12px;line-height:16px;font-weight:600;color:var(--ink2)}
.sections{display:flex;flex-direction:column;gap:8px}
.pills,.actions{display:flex;flex-wrap:wrap;align-items:center;gap:8px} .pills.facts{display:grid;grid-template-columns:repeat(2,minmax(0,1fr))}
.pill{padding:0 9px} .words{display:inline-grid;place-items:center} .words>span{grid-area:1/1} .words>.ghost{font-weight:600;visibility:hidden}
.pill[aria-pressed=true]{background:var(--ok-fill);border-color:var(--ok-line);color:var(--ok);font-weight:600}
.pill[aria-pressed=true]:hover{background:var(--ok-hot)}
.split{display:inline-flex;flex:none;min-width:0} .split>:first-child{border-top-right-radius:0;border-bottom-right-radius:0}
.split>.x{border-left:0;border-top-left-radius:0;border-bottom-left-radius:0;color:var(--ink2)}
.split:has([aria-pressed=true])>.x{background:var(--ok-fill);border-color:var(--ok-line);color:var(--ok)} .split:has([aria-pressed=true])>.x:hover{background:var(--ok-hot)}
.cells>div{display:flex;min-width:0}.cells>div>.cell{flex:1}
.x{width:24px;padding:0} .x.flat{height:24px;border:0;border-radius:8px;background:none;box-shadow:none;color:var(--ink2)} .x.flat:hover{background:var(--hot);color:var(--ink)}
.seg{display:grid;grid-auto-flow:column;grid-auto-columns:minmax(0,1fr);gap:4px;height:var(--h);padding:3px;border:1px solid var(--line);border-radius:10px;background:var(--well);box-shadow:var(--sunk)}
.seg.inline{width:var(--w)}
.seg>button{height:auto;min-width:0;padding:0 8px;gap:4px;border:1px solid transparent;border-radius:8px;background:none;box-shadow:none}
.seg>button:hover{background:var(--hot)} .seg>button:focus-visible{outline-offset:1px}
.seg>button[aria-checked=true]{background:var(--raised);border-color:var(--accent);box-shadow:var(--raise),var(--hi)} .seg>button[aria-checked=true]:hover{background:var(--hot)}
.dot{width:8px;height:8px;border-radius:50%;border:1px solid rgba(255,255,255,.28);flex:none}
.stepper{display:inline-grid;grid-template-columns:calc(var(--h) - 2px) minmax(0,1fr) calc(var(--h) - 2px);align-items:center;width:var(--w);height:var(--h);border:1px solid var(--line);border-radius:10px;background:var(--well);box-shadow:var(--sunk)}
.stepper:focus-within{border-color:var(--accent);box-shadow:var(--sunk),0 0 0 1px var(--accent)}
.stepper>button{width:auto;height:calc(var(--h) - 2px);padding:0;border:0;border-radius:9px;background:none;box-shadow:none;color:var(--ink2)}
.stepper>button:hover{background:var(--hot);color:var(--ink)} .stepper>button:active{background:var(--raised);box-shadow:var(--sunk)}
.stepper>button:disabled{border:0;background:none;color:var(--ink3)}
.value{display:flex;align-items:center;justify-content:center;gap:3px;min-width:0;height:100%;overflow:hidden;cursor:text}
.value input{field-sizing:content;min-width:1ch;max-width:100%;height:100%;padding:0;border:0;outline:0;background:none;text-align:center;font-variant-numeric:tabular-nums}
.unit{font-size:12px;color:var(--ink2);white-space:nowrap}
.switch{width:calc(100% + 16px);margin:0 -8px;justify-content:space-between;gap:12px;padding:0 8px;border:0;background:none;box-shadow:none;text-align:left}
.switch.sub{padding-left:24px} .switch.group{margin-top:8px} .switch:hover,.switch:active{background:var(--hot);box-shadow:none} .switch:focus-visible{outline-offset:-2px}
.rail{position:relative;width:32px;height:16px;flex:none;border:1px solid var(--line2);border-radius:8px;background:var(--well)}
.rail::after{content:"";position:absolute;top:2px;left:2px;width:10px;height:10px;border-radius:5px;background:var(--ink2);transition:transform .12s}
.switch[aria-checked=true] .rail{background:var(--ok-soft);border-color:var(--ok-line)}
.switch[aria-checked=true] .rail::after{transform:translateX(16px);background:var(--ok)}
.cells{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}
.cell{position:relative;display:flex;align-items:center;justify-content:flex-start;gap:8px;min-width:0;height:var(--h);padding:0 12px 0 8px;border:1px solid var(--line);border-radius:10px;background:var(--raised);box-shadow:var(--raise),var(--hi);cursor:pointer}
.cell:hover{background:var(--hot)}
.swatch{width:16px;height:16px;flex:none;border:1px solid var(--line2);border-radius:4px}
.name{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.thumb{width:24px;height:16px;flex:none;border-radius:4px;object-fit:cover}
.field{width:100%;min-width:0;height:var(--h);padding:0 12px;border:1px solid var(--line);border-radius:10px;background:var(--well);box-shadow:var(--sunk);color:var(--ink)}
.field:focus{border-color:var(--accent);outline:1px solid var(--accent);outline-offset:0} .field.center{text-align:center} #preset-name{width:var(--w)}
#sections{display:flex;flex-direction:column;gap:4px;margin:0;padding:0;list-style:none}
#sections li{display:grid;grid-template-columns:minmax(0,1fr) var(--h);align-items:center;gap:4px} #sections li>.x.flat{width:var(--h);height:var(--h);border-radius:10px}
.run{width:100%;justify-content:space-between;gap:8px;padding:0 8px;border:0;background:none;box-shadow:none;font-variant-numeric:tabular-nums} .run:hover{background:var(--hot)}
main{flex:1;min-width:0;overflow:auto;padding:24px;background:var(--canvas)}
#page{width:fit-content;max-width:100%;margin:0 auto;padding:40px 48px;border-radius:8px;background:#2D2A32;box-shadow:0 1px 2px rgba(0,0,0,.30),0 8px 24px rgba(0,0,0,.35)}
#sheet{white-space:pre-wrap;font-family:Verdana,sans-serif;font-size:10pt;line-height:135%;color:#fff}
#sheet .oor{display:inline-flex;justify-content:center;min-width:.6em;border-bottom:2px solid;font-weight:900}
#sheet .comment{color:var(--comment,#c8c4cc)}
#sheet .chord.in-section{background:var(--accent-soft);border-radius:2px} #sheet .chord.picked{background:rgba(78,163,234,.32);border-radius:2px}
#heading{font-family:Verdana,sans-serif;color:#aaa4b3;margin-bottom:1.35em;white-space:pre-wrap;border-radius:2px;cursor:pointer}
#heading:hover{background:var(--accent-soft);box-shadow:0 0 0 6px var(--accent-soft)} #heading:focus-visible{outline-offset:6px} #heading:empty{display:none}
#parity{display:none;align-items:center;gap:8px;margin:0 0 16px;padding:4px 4px 4px 12px;border:1px solid var(--bad);border-radius:10px;color:var(--bad)} #parity.shown{display:flex} #parity>.x{margin-left:auto}
#selection,#colour{position:fixed;z-index:4;width:288px;display:flex;flex-direction:column;gap:8px;padding:12px;background:var(--card);border:1px solid var(--line2);border-radius:12px;box-shadow:var(--raise),var(--hi),0 8px 24px rgba(0,0,0,.35)}
.card-top{display:flex;align-items:center;justify-content:space-between;gap:8px}
#selection-range{font-size:12px;line-height:16px;color:var(--ink2);font-variant-numeric:tabular-nums}
#selection-rows{display:flex;flex-direction:column;gap:8px} #selection-rows .row{grid-template-columns:minmax(0,1fr) var(--w)}
@media (max-width:1000px){main{padding:16px}#page{padding:24px}}
@media (max-width:720px){
:root{--h:40px}
html,body{height:auto} body{display:block}
header{flex-wrap:wrap;height:auto;gap:8px;padding:8px 12px}
#outputs{flex:1 1 100%;display:grid;grid-template-columns:1fr 1fr;gap:8px}
#outputs>button{width:100%} #copy{margin-left:0}
#view{display:block;position:sticky;top:0;z-index:3;padding:8px 12px;background:var(--bar);border-bottom:1px solid var(--line)}
#layout{display:block} aside{width:auto;border-right:0}
#presets{padding:8px 12px} #panel{overflow:visible;padding:0 12px}
main{overflow:visible;padding:12px} #page{padding:24px 16px}
body[data-view=sheet] aside,body[data-view=settings] main{display:none}
#selection{left:8px;right:8px;top:auto;bottom:8px;width:auto}
body.part-open main{padding-bottom:200px}
.fold-head{height:44px} .fold{scroll-margin-top:57px}
}
@media print{header,aside,#selection,#parity{display:none!important}html,body{height:auto}body{display:block;background:var(--sheet-bg,#2D2A32);-webkit-print-color-adjust:exact;print-color-adjust:exact}#layout{display:block}main{overflow:visible}#page{padding:0;min-height:0}#view{display:none!important}main{display:block!important;padding:0;background:none}#page{width:auto;max-width:none;margin:0;border-radius:0;box-shadow:none}#sheet .chord.in-section,#sheet .chord.picked{background:none}#heading{box-shadow:none;background:none}}
)css"
R"css(
.menu-box{position:relative} .menu-button{width:100%;justify-content:space-between;padding:0 8px 0 12px} .menu-button>.i{color:var(--ink2)}
.menu-button>.face{min-width:0;overflow:hidden;text-overflow:ellipsis}
.menu{position:absolute;z-index:4;left:0;right:0;top:calc(100% + 4px);display:flex;flex-direction:column;max-height:320px;overflow:auto;padding:4px;border:1px solid var(--line2);border-radius:10px;background:var(--card);box-shadow:0 8px 24px rgba(0,0,0,.45);scrollbar-width:thin;scrollbar-color:var(--line2) transparent}
.menu-item{justify-content:flex-start;gap:8px;padding:0 8px;border:0;border-radius:8px;background:none;box-shadow:none}
.menu-item:hover,.menu-item:focus-visible{background:var(--hot);outline:0} .menu-item:active{box-shadow:none}
.menu-item>.i{visibility:hidden;color:var(--accent)} .menu-item[aria-selected=true]>.i{visibility:visible}
.cells>.wide{grid-column:1/-1}
.vh{position:absolute!important;width:1px;height:1px;margin:-1px;padding:0;border:0;overflow:hidden;clip:rect(0 0 0 0);white-space:nowrap}
.legend{display:flex;flex-direction:column;gap:8px;padding:8px;border:1px solid var(--line);border-radius:12px;box-shadow:var(--sunk);color:var(--paper-ink,var(--ink))}
.inks{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:4px}
.ink{position:relative;justify-content:flex-start;gap:8px;min-width:0;padding:0 12px 0 8px;border:1px solid var(--paper-line,var(--line));background:var(--paper-fill,var(--raised));box-shadow:none;color:inherit;font-weight:400;text-align:left}
.ink.wide{grid-column:1/-1}
.ink:hover,.ink:active{background:var(--paper-fill,var(--raised));border-color:var(--paper-line2,var(--line2));box-shadow:none}
.ink[aria-expanded=true]{border-color:var(--accent);box-shadow:0 0 0 1px var(--accent)}
.ink .swatch{position:relative;border-color:var(--paper-line2,var(--line2))}
.changed{position:absolute;top:-3px;right:-3px;width:6px;height:6px;border-radius:50%;background:var(--ok);box-shadow:0 0 0 1.5px var(--paper-solid,var(--card))}
#colour{z-index:5}
#colour-name{flex:1;min-width:0;font-size:14px;line-height:20px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#colour .seg>button>span{min-width:0;overflow:hidden;text-overflow:ellipsis}
.card-rows{display:flex;flex-direction:column;gap:8px} .card-rows .row{grid-template-columns:minmax(0,1fr) var(--w)}
.sv{position:relative;flex:none;height:128px;border:1px solid var(--line);border-radius:8px;cursor:crosshair;touch-action:none}
.hue{position:relative;flex:none;height:16px;border:1px solid var(--line);border-radius:8px;background:linear-gradient(90deg,#f00,#ff0 17%,#0f0 33%,#0ff 50%,#00f 67%,#f0f 83%,#f00);cursor:pointer;touch-action:none}
.knob{position:absolute;width:14px;height:14px;margin:-7px 0 0 -7px;border:2px solid #fff;border-radius:50%;box-shadow:0 0 0 1px rgba(0,0,0,.6);pointer-events:none}
.hue>.knob{top:50%;width:16px;height:16px;margin:-8px 0 0 -8px}
.tools-row{display:flex;gap:8px} #colour-hex{flex:1;width:auto;font-variant-numeric:tabular-nums}
.chips{display:grid;grid-template-columns:repeat(9,minmax(0,1fr));gap:6px}
.chip{aspect-ratio:1;width:100%;height:auto;padding:0;border:1px solid rgba(255,255,255,.28);border-radius:6px;box-shadow:none}
.chip:hover{box-shadow:0 0 0 2px var(--hot)} .chip:active{box-shadow:none} .chip[aria-pressed=true]{box-shadow:0 0 0 2px var(--accent)}
@media screen{
#page.spot #heading:not(.lit),#page.spot #sheet .chord:not(.lit),#page.spot #sheet .comment:not(.lit){opacity:.15}
#page.spot-band #sheet .chord{color:transparent!important}
#page{cursor:pointer} #sheet{cursor:text} #sheet .chord,#sheet .comment{cursor:pointer}
#sheet .chord:hover,#sheet .comment:hover{outline:1px solid color-mix(in srgb,currentColor 45%,transparent);outline-offset:1px;border-radius:2px}
#page:hover:not(:has(#sheet:hover,#heading:hover)){outline:1px solid color-mix(in srgb,currentColor 45%,transparent);outline-offset:-4px}
body.colour-open #sheet .lit{outline:2px solid var(--accent);outline-offset:1px;border-radius:2px}
body.colour-open #heading.lit{outline:2px solid var(--accent);outline-offset:6px} body.colour-open #page.lit{outline:2px solid var(--accent);outline-offset:4px}
}
@media (max-width:720px){
#colour{left:8px;right:8px;top:auto;bottom:8px;width:auto;max-height:calc(100vh - 16px);overflow:auto}
body.colour-open main,body.colour-open #panel{padding-bottom:480px}
.sv{height:140px} .hue{height:24px} .hue>.knob{width:24px;height:24px;margin:-12px 0 0 -12px}
}
@media print{#colour{display:none!important}}
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
// sheet::Look's colours by theme: 0 dark, the original's and the default, and 2 white.
// 1 was paper; a look saved with it reads as 3, the user's own.
const THEMES = {
  0: {background: "#2D2A32", text: "#ffffff", comment: "#c8c4cc", heading: "#aaa4b3", pedalDown: "#31406b", pedalUp: "#6b3262", rhythm: COLOURS},
  2: {background: "#ffffff", text: "#1a1a1a", comment: "#5f5f66", heading: "#77777f", pedalDown: "#d3dcf5", pedalUp: "#f3d3e8",
      rhythm: ["#8a0b00", "#c41400", "#a8574f", "#a8501f", "#6e6a00", "#3f7d12", "#1f7a2f", "#145c3a", "#1a1a1a"]},
};
const LOOK = Object.assign({theme: 0, ground: 0, fit: 0, dim: 0, oneColour: false, font: 0}, THEMES[0]);
// SheetExport.hpp's kFonts: the names, and each as CSS.
const FONT_NAMES = ["Verdana", "Segoe UI", "Consolas", "Arial", "Bahnschrift", "Calibri", "Cambria", "Candara", "Cascadia Mono",
  "Comic Sans MS", "Constantia", "Corbel", "Courier New", "Franklin Gothic Medium", "Georgia", "Ink Free", "Lucida Console",
  "Palatino Linotype", "Segoe Print", "Sitka Text", "Tahoma", "Times New Roman", "Trebuchet MS"];
const FONTS = FONT_NAMES.map((name, i) => (/ /.test(name) ? "'" + name + "'" : name) + "," +
  (["Cascadia Mono", "Consolas", "Courier New", "Lucida Console"].includes(name) ? "monospace" :
   ["Comic Sans MS", "Ink Free", "Segoe Print"].includes(name) ? "cursive" :
   ["Cambria", "Constantia", "Georgia", "Palatino Linotype", "Sitka Text", "Times New Roman"].includes(name) ? "serif" : "sans-serif"));
// sheet::detail::ChordColour.
function chordColour(rhythm, look) { return look.oneColour ? look.text : look.rhythm[rhythm]; }
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
return {defaults, applyRegions, heldByPedal, transposeSections, bestTransposition, style, markPedals, toHtml, escapeHtml, heading, sheetText,
        THEMES, LOOK, FONTS, FONT_NAMES, NOTES_SHIFTED, TRANSPOSE,
        HEADER, today, findKey, headerLines, headingLines, headerText};
})();
/*SHEET-CORE-END*/
)js";

// Page UI: settings panel, presets, part card, copy, save and print.
// In pieces: one string literal holds at most 16 KB.
inline constexpr const char* kPageUi[] = {R"js(
(() => {
const data = JSON.parse(document.getElementById("sheet-data").textContent);
const $ = id => document.getElementById(id);
const STORE = "midipp.sheet.style", PANEL = "midipp.sheet.panel", PRESETS = "midipp.sheet.presets", IMAGE = "midipp.sheet.image";
const CUSTOM = "midipp.sheet.custom", VIEW = "midipp.sheet.view", RECENT = "midipp.sheet.recent";
const readStore = key => { try { return JSON.parse(localStorage.getItem(key) || "null"); } catch (e) { return null; } };
const writeStore = (key, value) => { try { localStorage.setItem(key, JSON.stringify(value)); } catch (e) {} };
const copy = value => JSON.parse(JSON.stringify(value));
// A page saved from here carries what the script built; it is built again.
for (const id of ["presets", "panel", "view", "selection-rows", "colour"]) $(id).replaceChildren();
$("parity").classList.remove("shown");
for (const b of $("parity").querySelectorAll("button")) b.remove();
for (const id of ["selection", "colour"]) { $(id).hidden = true; $(id).removeAttribute("style"); }
document.body.classList.remove("part-open", "colour-open");
$("page").classList.remove("spot", "spot-band", "lit");
$("heading").classList.remove("lit");
document.body.dataset.view = readStore(VIEW) === "settings" ? "settings" : "sheet";
// Pedal marks were a switch for a day; on was the one line under the pedal.
const fixMarks = o => { if (typeof o.pedalMarks === "boolean") o.pedalMarks = o.pedalMarks ? 1 : 0; return o; };
const PAGE = {fontSize: 10, lineHeight: 135};
// A style, filled out from the defaults so every key is there.
// The background image is kept apart, under its own key: it is large.
// The theme is what the colours are, so Paper's (1) read as the user's own.
// Grain (ground 2) is gone: its paper is the plain colour.
function fullLook(l) {
  const look = Object.assign({}, SheetCore.LOOK, l);
  delete look.image;
  delete look.grain;
  if (look.ground !== 1) look.ground = 0;
  if (!(look.font >= 0 && look.font < SheetCore.FONTS.length)) look.font = 0;
  look.rhythm = SheetCore.LOOK.rhythm.map((c, i) => l && Array.isArray(l.rhythm) && typeof l.rhythm[i] === "string" ? l.rhythm[i] : c);
  look.theme = themeOf(look);
  return look;
}
// A credit prints when it has words, so its switch stays on.
const CREDITS = {title: true, subtitle: true, artist: true, arranger: true};
const full = s => ({options: fixMarks(Object.assign(SheetCore.defaults(), s && s.options)), page: Object.assign({}, PAGE, s && s.page), look: fullLook(s && s.look),
                    header: Object.assign({}, SheetCore.HEADER, s && s.header, CREDITS)});
// The style the page came with: a saved page's own, the app's style page, or
// the defaults. A page the app just wrote without a style page starts from
// what the user last chose in this browser.
const own = () => full({options: data.options, page: data.page, look: data.look, header: data.header});
const ownImage = () => data.look && typeof data.look.image === "string" ? data.look.image : "";
const stored = data.saved || data.styled ? null : readStore(STORE);
// A header from before the words decided: a credit switched off printed nothing.
const incoming = Object.assign({}, data.header, stored ? stored.header : null);
const S = stored ? full({options: Object.assign({}, data.options, fixMarks(stored.options || {})), page: Object.assign({}, data.page, stored.page),
                         look: Object.assign({}, data.look, stored.look), header: incoming}) : own();
if (incoming.arranger === false) S.header.arrangerName = "";
const storedImage = stored ? readStore(IMAGE) : null;
let image = typeof storedImage === "string" ? storedImage : ownImage(), keptImage = storedImage;
const styleOf = () => ({options: S.options, page: S.page, look: S.look, header: S.header});
const readPresets = () => { const list = readStore(PRESETS); return Array.isArray(list) ? list.filter(p => p && typeof p.name === "string" && p.style) : []; };
let keptPresets = JSON.stringify(readPresets());
function remember() {
  writeStore(STORE, styleOf());
  if (image !== keptImage) { writeStore(IMAGE, image); keptImage = image; }
  const list = JSON.stringify(presets);
  if (list !== keptPresets) { writeStore(PRESETS, presets); keptPresets = list; }
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
// A title switched off is an empty title, unless the song has its own.
const keepNoTitle = (h, w) => { if (h && h.title === false && typeof w.title !== "string") w.title = ""; return w; };
let words = keepNoTitle(incoming, textOf(data.saved ? data.song : song.text));
const ownWords = () => keepNoTitle(data.header, textOf(data.saved ? data.song : null));
const songText = () => Object.assign({title: data.title || "", subtitle: "", artist: "", date: SheetCore.today()}, words);
const facts = () => ({bpm: tempos.length ? tempos[0].bpm : S.options.missingBpm, key: SheetCore.findKey(SheetCore.applyRegions(notes, regions))});
const headingLines = () => SheetCore.headingLines(result, S.header, songText(), facts());
const sheetText = () => SheetCore.headerText(result, S.header, songText(), facts(), data.title || "");
function rememberSong() { song.regions = regions.map(r => [r.from, r.to, r.semitones, r.kind]); song.text = words; writeStore(SONG, song); }
let result = null;
// The notes as the sheet plays them: draw works them out and styles them.
let played = [];
const time = seconds => {
  const whole = Math.floor(seconds), minutes = Math.floor(whole / 60), rest = whole % 60;
  return minutes + ":" + (rest < 10 ? "0" : "") + rest + "." + Math.floor((seconds - whole) * 10);
};
const sameRun = (a, b) => a.from === b.from && a.to === b.to;
// The transposition in force at a moment: the section it falls in.
const shiftAt = seconds => {
  for (const s of result.sections) if (seconds >= s.from && seconds <= s.to) return s.semitones;
  return result.transposition;
};
const o = () => S.options, L = () => S.look, H = () => S.header;
// The transposition: 0 fixed, 1 the best key, 2 the best key per section.
const mode = () => !o().autoTranspose ? 0 : o().autoSections ? 2 : 1;
// The whole song's transposition, as the sheet uses it outside a part the
// user transposed.
const songShift = () => mode() === 2 ? result.transposition : mode() === 1 && notes.length ?
  SheetCore.bestTransposition(played, data.mapping, o().transpose, o().resilience) : o().transpose;
// Which folds are open; Advanced's colours moved to Colours.
const panelOpen = {sheet: true, header: true, look: false, colours: false, notation: false};
{
  const open = readStore(PANEL);
  if (open && typeof open === "object") {
    if (open.colours === undefined) open.colours = open.advanced;
    for (const k in panelOpen) if (typeof open[k] === "boolean") panelOpen[k] = open[k];
  }
}
// The Custom slot: the last colours of the user's own in use, so the Custom
// pill brings them back on any song. A set equal to Dark or White is none.
let slot = (() => {
  const kept = readStore(CUSTOM), c = kept && typeof kept === "object" ? coloursOf(fullLook(kept)) : null;
  return c && themeOf(c) === 3 && [c.background, c.text, c.comment, c.heading, c.pedalDown, c.pedalUp].concat(c.rhythm).every(isHex) ? c : null;
})();
// Kept whenever a change is committed with colours of the user's own, and as
// the page opens with them; a preview never keeps them.
function keepSlot() {
  if (gesture || themeOf(L()) !== 3 || sameColours(L(), slot)) return;
  slot = coloursOf(L());
  writeStore(CUSTOM, slot);
}
// A number as the panel writes it: a true minus, a plus when asked, at most
// two decimals.
const show = (v, plus) => { const r = Math.round(v * 100) / 100; return (r < 0 ? "−" : plus && r > 0 ? "+" : "") + Math.abs(r); };
const plural = (n, one, many) => n + " " + (n === 1 ? one : many);

// Undo and redo: every change keeps the style, sections, words and presets
// from before it. A stepper click is one change; typing, holding a stepper
// or dragging a colour is one until it settles.
const history = [], future = [];
let gesture = false;
const snapshot = () => ({state: JSON.stringify({S, regions, words, presets}), image});
function record() { history.push(snapshot()); if (history.length > 200) history.shift(); future.length = 0; }
function settle(now) { L().theme = themeOf(L()); if (now) keepSlot(); remember(); rememberSong(); if (now) draw(); else redraw(); }
function change(apply, now) { gesture = false; record(); apply(); settle(now); }
function preview(apply) { if (!gesture) { record(); gesture = true; } apply(); settle(); }
function restore(entry) {
  const before = JSON.parse(entry.state);
  keepSlot();
  S.options = before.S.options; S.page = before.S.page; S.look = before.S.look; S.header = before.S.header;
  regions = before.regions;
  words = before.words;
  if (before.presets) presets = before.presets;
  image = entry.image;
  gesture = false;
  settle(true);
}
function undo() { if (!history.length) return; future.push(snapshot()); restore(history.pop()); }
function redo() { if (!future.length) return; history.push(snapshot()); restore(future.pop()); }
// Changes are drawn once a frame.
let frame = 0;
function redraw() { if (!frame) frame = requestAnimationFrame(() => { frame = 0; draw(); }); }
// What the page came with, as Reset brings it back. The arranger is the
// user's name and stays.
const START = {S: own(), regions: data.regions.map(unpackRegion), words: ownWords()};
const atStart = () => {
  START.S.header.arrangerName = S.header.arrangerName;
  return image === ownImage() && canon({S: styleOf(), regions, words}) === canon(START);
};
keepSlot();
)js",
R"js(
// Controls. Each keeps itself up to date through an updater run after
// every draw. renderHooks(look, result) run after every render of the
// sheet, previews included; drawHooks() after every draw, last.
const updaters = [], renderHooks = [], drawHooks = [];
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
let ids = 0;
const nextId = () => "ui" + ++ids;
// Lucide's icons (ISC licence), drawn in the text's colour.
const ICONS = {
  chevron: '<path d="m9 18 6-6-6-6"/>',
  chevronDown: '<path d="m6 9 6 6 6-6"/>',
  minus: '<path d="M5 12h14"/>',
  plus: '<path d="M5 12h14"/><path d="M12 5v14"/>',
  x: '<path d="M18 6 6 18"/><path d="m6 6 12 12"/>',
  undo: '<path d="M9 14 4 9l5-5"/><path d="M4 9h10.5a5.5 5.5 0 0 1 5.5 5.5a5.5 5.5 0 0 1-5.5 5.5H11"/>',
  redo: '<path d="m15 14 5-5-5-5"/><path d="M20 9H9.5A5.5 5.5 0 0 0 4 14.5A5.5 5.5 0 0 0 9.5 20H13"/>',
  image: '<rect width="18" height="18" x="3" y="3" rx="2" ry="2"/><circle cx="9" cy="9" r="2"/><path d="m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21"/>',
  pipette: '<path d="m2 22 1-1h3l9-9"/><path d="M3 21v-3l9-9"/><path d="m15 6 3.4-3.4a2.1 2.1 0 1 1 3 3L18 9l.4.4a2.1 2.1 0 1 1-3 3l-3.8-3.8a2.1 2.1 0 1 1 3-3l.4.4Z"/>',
  reset: '<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/>',
  imageDown: '<path d="M10.3 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2v10l-3.1-3.1a2 2 0 0 0-2.814.014L6 21"/><path d="m14 19 3 3v-5.5"/><path d="m17 22 3-3"/><circle cx="9" cy="9" r="2"/>',
  fileDown: '<path d="M6 22a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h8a2.4 2.4 0 0 1 1.704.706l3.588 3.588A2.4 2.4 0 0 1 20 8v12a2 2 0 0 1-2 2z"/><path d="M14 2v5a1 1 0 0 0 1 1h5"/><path d="M12 18v-6"/><path d="m9 15 3 3 3-3"/>',
  printer: '<path d="M6 18H4a2 2 0 0 1-2-2v-5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2h-2"/><path d="M6 9V3a1 1 0 0 1 1-1h10a1 1 0 0 1 1 1v6"/><rect x="6" y="14" width="12" height="8" rx="1"/>',
  copy: '<rect width="14" height="14" x="8" y="8" rx="2" ry="2"/><path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2"/>',
  check: '<path d="M20 6 9 17l-5-5"/>',
  moon: '<path d="M20.985 12.486a9 9 0 1 1-9.473-9.472c.405-.022.617.46.402.803a6 6 0 0 0 8.268 8.268c.344-.215.825-.004.803.401"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2"/><path d="M12 20v2"/><path d="m4.93 4.93 1.41 1.41"/><path d="m17.66 17.66 1.41 1.41"/><path d="M2 12h2"/><path d="M20 12h2"/><path d="m6.34 17.66-1.41 1.41"/><path d="m19.07 4.93-1.41 1.41"/>',
  pencil: '<path d="M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z"/><path d="m15 5 4 4"/>',
  sheet: '<path d="M11.65 22H18a2 2 0 0 0 2-2V8a2.4 2.4 0 0 0-.706-1.706l-3.588-3.588A2.4 2.4 0 0 0 14 2H6a2 2 0 0 0-2 2v10.35"/><path d="M14 2v5a1 1 0 0 0 1 1h5"/><path d="M8 20v-7l3 1.474"/><circle cx="6" cy="20" r="2"/>',
  settings: '<path d="M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915"/><circle cx="12" cy="12" r="3"/>',
  heading: '<path d="M6 12h12"/><path d="M6 20V4"/><path d="M18 20V4"/>',
  type: '<path d="M12 4v16"/><path d="M4 7V5a1 1 0 0 1 1-1h14a1 1 0 0 1 1 1v2"/><path d="M9 20h6"/>',
  palette: '<path d="M12 22a1 1 0 0 1 0-20 10 9 0 0 1 10 9 5 5 0 0 1-5 5h-2.25a1.75 1.75 0 0 0-1.4 2.8l.3.4a1.75 1.75 0 0 1-1.4 2.8z"/><circle cx="13.5" cy="6.5" r=".5" fill="currentColor"/><circle cx="17.5" cy="10.5" r=".5" fill="currentColor"/><circle cx="6.5" cy="12.5" r=".5" fill="currentColor"/><circle cx="8.5" cy="7.5" r=".5" fill="currentColor"/>',
  music: '<path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/>',
  metronome: '<path d="M12 11.4V9.1"/><path d="m12 17 6.59-6.59"/><path d="m15.05 5.7-.218-.691a3 3 0 0 0-5.663 0L4.418 19.695A1 1 0 0 0 5.37 21h13.253a1 1 0 0 0 .951-1.31L18.45 16.2"/><circle cx="20" cy="9" r="2"/>',
  hash: '<path d="M4 9h16"/><path d="M4 15h16"/><path d="M10 3 8 21"/><path d="M16 3l-2 18"/>',
  gauge: '<path d="m12 14 4-4"/><path d="M3.34 19a10 10 0 1 1 17.32 0"/>',
  calendar: '<path d="M8 2v3"/><path d="M16 2v3"/><rect x="3" y="3" width="18" height="18" rx="2"/><path d="M3 9h18"/>',
  alignStart: '<path d="M21 5H3"/><path d="M15 12H3"/><path d="M17 19H3"/>',
  alignCentre: '<path d="M21 5H3"/><path d="M17 12H7"/><path d="M19 19H5"/>',
  bucket: '<path d="M11 7 6 2"/><path d="M18.992 12H2.041"/><path d="M21.145 18.38A3.34 3.34 0 0 1 20 16.5a3.3 3.3 0 0 1-1.145 1.88c-.575.46-.855 1.02-.855 1.595A2 2 0 0 0 20 22a2 2 0 0 0 2-2.025c0-.58-.285-1.13-.855-1.595"/><path d="m8.5 4.5 2.148-2.148a1.205 1.205 0 0 1 1.704 0l7.296 7.296a1.205 1.205 0 0 1 0 1.704l-7.592 7.592a3.615 3.615 0 0 1-5.112 0l-3.888-3.888a3.615 3.615 0 0 1 0-5.112L5.67 7.33"/>',
  select: '<path d="M12.034 12.681a.498.498 0 0 1 .647-.647l9 3.5a.5.5 0 0 1-.033.943l-3.444 1.068a1 1 0 0 0-.66.66l-1.067 3.443a.5.5 0 0 1-.943.033z"/><path d="M5 3a2 2 0 0 0-2 2"/><path d="M19 3a2 2 0 0 1 2 2"/><path d="M5 21a2 2 0 0 1-2-2"/><path d="M9 3h1"/><path d="M9 21h2"/><path d="M14 3h1"/><path d="M3 9v1"/><path d="M21 9v2"/><path d="M3 14v1"/>',
  pin: '<path d="M12 17v5"/><path d="M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z"/>',
};
function icon(name) {
  const t = document.createElement("template");
  t.innerHTML = '<svg class="i" viewBox="0 0 24 24" aria-hidden="true">' + ICONS[name] + "</svg>";
  return t.content.firstChild;
}
// A row on the panel's two columns: the label, then its control. o.stack
// puts the control under its label, across both; o.sub indents the label
// of a setting that depends on the one above; o.group starts a group;
// o.when shows the row while it holds; o.result() is a few words of what
// the setting found, at the end of the label's line. A label can be a
// function, read again on every draw.
function row(label, control, o) {
  o = o || {};
  const input = control._input || (control.tagName === "INPUT" ? control : null);
  const name = input ? make("label", {for: input.id || (input.id = nextId())}) : make("span", {id: nextId()});
  if (!input) control.setAttribute("aria-labelledby", name.id);
  const first = o.stack || o.result ? make("div", {class: "label-line"}, [name]) : name;
  const node = make("div", {class: "row" + (o.stack ? " stack" : "") + (o.sub ? " sub" : "") + (o.group ? " group" : "")}, [first, control]);
  if (typeof label === "function") updaters.push(() => { name.textContent = label(); });
  else name.textContent = label;
  if (o.result) {
    const found = first.appendChild(make("span", {class: "result"}));
    updaters.push(() => { found.textContent = o.result() || ""; found.hidden = !found.textContent; });
  }
  if (o.when) updaters.push(() => { node.hidden = !o.when(); });
  return node;
}
// Two stacked rows side by side, on the same two columns.
function pair(a, b, o) {
  o = o || {};
  const node = make("div", {class: "pair" + (o.group ? " group" : "")}, [a, b]);
  if (o.when) updaters.push(() => { node.hidden = !o.when(); });
  return node;
}
// A small heading, hidden while count() is 0.
function heading(label, count) {
  const node = make("div", {class: "heading"}, [make("span", {text: label})]);
  if (count) updaters.push(() => { node.hidden = !count(); });
  return node;
}
// A fold of the panel, with its icon. It stays open or closed as it was left.
const folds = {};
function fold(id, label, name, children) {
  const chevron = icon("chevron");
  chevron.classList.add("chev");
  const head = make("button", {type: "button", class: "fold-head", "aria-expanded": !!panelOpen[id], "aria-controls": "fold-" + id},
                    [chevron, icon(name), make("span", {text: label})]);
  head.onclick = () => setFold(id, !panelOpen[id]);
  const node = make("section", {class: "fold"}, [head, make("div", {class: "fold-body", id: "fold-" + id}, children.flat())]);
  folds[id] = {node, head};
  return node;
}
function setFold(id, open) {
  panelOpen[id] = open;
  if (folds[id]) folds[id].head.setAttribute("aria-expanded", open);
  writeStore(PANEL, panelOpen);
  // The colour card and the legend's light go with the Colours fold.
  if (id === "colours" && !open) { closeColour(false); spot(null); }
}
// Opens a fold at the top of the panel and moves the cursor to focus.
function openFold(id, focus) {
  if (matchMedia("(max-width:720px)").matches) showView("settings");
  setFold(id, true);
  if (folds[id]) folds[id].node.scrollIntoView({block: "start"});
  if (focus) focus.focus({preventScroll: true});
}
)js",
R"js(
// One of a few choices: [value, label, x], where x may hold dots (a function
// of their colours), icon, and bare (the icon without the label).
// o.inline fills a row's second column; o.label names it where no row does;
// o.after(value) runs after a pick; o.direct sets the value with no undo
// or draw and runs the updaters.
function segment(get, set, choices, o) {
  o = o || {};
  const box = make("div", {class: "seg" + (o.inline ? " inline" : ""), role: "radiogroup"});
  if (o.label) box.setAttribute("aria-label", o.label);
  const pick = value => {
    if (get() !== value) {
      if (o.direct) { set(value); updaters.forEach(update => update()); }
      else change(() => set(value), true);
    }
    if (o.after) o.after(value);
  };
  const parts = choices.map(([value, label, x]) => {
    x = x || {};
    const marks = x.dots ? x.dots().map(() => make("span", {class: "dot"})) : [];
    const button = make("button", {type: "button", role: "radio"}, marks.concat(x.icon ? [icon(x.icon)] : [], x.bare ? [] : [label]));
    if (x.bare) button.setAttribute("aria-label", label);
    button.onclick = () => pick(value);
    box.appendChild(button);
    return {value, button, marks, dots: x.dots};
  });
  box.addEventListener("keydown", e => {
    const i = parts.findIndex(p => p.button === document.activeElement), n = parts.length;
    const to = {ArrowRight: i + 1, ArrowDown: i + 1, ArrowLeft: i - 1, ArrowUp: i - 1, Home: 0, End: n - 1}[e.key];
    if (i < 0 || typeof to !== "number") return;
    e.preventDefault();
    const p = parts[(to + n) % n];
    pick(p.value);
    p.button.focus();
  });
  updaters.push(() => {
    const on = parts.findIndex(p => p.value === get());
    parts.forEach((p, i) => {
      p.button.setAttribute("aria-checked", i === on);
      p.button.tabIndex = i === Math.max(on, 0) ? 0 : -1;
      if (p.dots) p.dots().forEach((c, j) => { if (p.marks[j]) p.marks[j].style.background = c; });
    });
  });
  return box;
}
// The one number control: o.min, o.max, o.step (1), o.unit (a word, or
// [one, many]), o.signed (a plus on positive values), o.whole (false for
// fractions). Click or hold an end (Shift for ten steps), type, or use
// the arrows, Page Up and Down, Home and End. Returns the box; box._input
// is its field.
const fits = !!(window.CSS && CSS.supports && CSS.supports("field-sizing", "content"));
function stepper(get, set, o) {
  const step = o.step || 1, big = 10 * step;
  const input = make("input", {type: "text", role: "spinbutton", id: nextId(), autocomplete: "off", spellcheck: "false", "aria-valuemin": o.min, "aria-valuemax": o.max});
  if (o.min >= 0) input.setAttribute("inputmode", "decimal");
  const unit = o.unit ? make("span", {class: "unit", "aria-hidden": "true"}) : null;
  const less = make("button", {type: "button", tabindex: "-1", "aria-label": "Less"}, [icon("minus")]);
  const more = make("button", {type: "button", tabindex: "-1", "aria-label": "More"}, [icon("plus")]);
  const value = make("div", {class: "value"}, [input, unit]);
  const box = make("div", {class: "stepper"}, [less, value, more]);
  const clamp = v => Math.min(o.max, Math.max(o.min, v));
  const snap = (v, d) => clamp(Math.round(Math.round((v + d) / step) * step * 100) / 100);
  const unitOf = v => !o.unit ? "" : typeof o.unit === "string" ? o.unit : o.unit[Math.abs(v) === 1 ? 0 : 1];
  const typed = () => {
    const t = input.value.replace(/−/g, "-").replace(/,/g, ".").replace(/[^0-9.+-]/g, "");
    const v = t === "" ? NaN : Number(t);
    return isFinite(v) ? v : NaN;
  };
  const size = () => { if (!fits) input.style.width = (input.value.length + 1) + "ch"; };
  let edited = false, held = false, timer = 0;
  const refresh = () => {
    const v = get(), text = show(v, o.signed), words = unitOf(v);
    if (!edited && input.value !== text) { input.value = text; size(); }
    if (unit) unit.textContent = words;
    input.setAttribute("aria-valuenow", v);
    input.setAttribute("aria-valuetext", words ? text + " " + words : text);
    less.disabled = v <= o.min;
    more.disabled = v >= o.max;
  };
  // Typing counts on Enter or leaving the field; what is not a number puts the value back.
  const commit = () => {
    if (!edited) return;
    edited = false;
    const v = typed();
    if (isNaN(v)) { refresh(); return; }
    const c = clamp(o.whole === false ? Math.round(v * 100) / 100 : Math.round(v));
    if (c !== get()) change(() => set(c), true); else refresh();
  };
  // A held key or button is one change.
  const end = () => { clearTimeout(timer); if (held) { held = false; gesture = false; settle(true); } };
  const go = v => { held = true; if (v !== get()) preview(() => set(v)); else refresh(); };
  input.addEventListener("input", () => { edited = true; size(); });
  input.addEventListener("change", commit);
  input.addEventListener("blur", () => { commit(); end(); });
  input.addEventListener("keyup", end);
  input.addEventListener("keydown", e => {
    const from = () => { const v = edited ? typed() : NaN; return isNaN(v) ? get() : v; };
    let v;
    if (e.key === "ArrowUp" || e.key === "ArrowDown") v = snap(from(), (e.key === "ArrowUp" ? 1 : -1) * (e.shiftKey ? big : step));
    else if (e.key === "PageUp" || e.key === "PageDown") v = snap(from(), e.key === "PageUp" ? big : -big);
    else if (e.key === "Home" || e.key === "End") v = e.key === "Home" ? o.min : o.max;
    else if (e.key === "Enter") { commit(); input.blur(); return; }
    else if (e.key === "Escape") { if (edited) e.stopPropagation(); edited = false; refresh(); input.blur(); return; }
    else return;
    e.preventDefault();
    edited = false;
    go(v);
  });
  value.addEventListener("mousedown", e => {
    if (e.target === input && document.activeElement === input) return;
    e.preventDefault();
    input.focus();
    input.select();
  });
  for (const [button, sign] of [[less, -1], [more, 1]]) {
    button.addEventListener("pointerdown", e => {
      if (e.button !== 0) return;
      e.preventDefault();
      commit();
      gesture = false;
      clearTimeout(timer);
      try { button.setPointerCapture(e.pointerId); } catch (error) {}
      const d = sign * (e.shiftKey ? big : step);
      const tick = () => { const v = snap(get(), d); if (v === get() || !box.getClientRects().length) { end(); return false; } go(v); return true; };
      if (tick()) timer = setTimeout(function again() { if (tick()) timer = setTimeout(again, 80); }, 400);
    });
    for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) button.addEventListener(type, end);
    // A click with no pointer, from a screen reader, is one step.
    button.addEventListener("click", e => { if (!e.detail) { const v = snap(get(), sign * step); if (v !== get()) change(() => set(v), true); } });
  }
  updaters.push(refresh);
  box._input = input;
  return box;
}
// The app's pill: on is green. Its width is the bold label's, on or off.
// With on(), the pill shows it on every draw; click runs as given; name is
// its icon.
function pill(label, on, click, name) {
  const words = make("span", {class: "words"}, [make("span", {text: label}), make("span", {class: "ghost", "aria-hidden": "true", text: label})]);
  const button = make("button", {type: "button", class: "pill", "aria-pressed": "false"}, name ? [icon(name), words] : [words]);
  if (click) button.onclick = click;
  if (on) updaters.push(() => { button.setAttribute("aria-pressed", !!on()); });
  return button;
}
// A setting that is on or off; the whole row is the switch.
function switchRow(label, get, set, o) {
  o = o || {};
  const button = make("button", {type: "button", role: "switch", class: "switch" + (o.sub ? " sub" : "") + (o.group ? " group" : "")},
                      [make("span", {text: label}), make("span", {class: "rail"})]);
  button.onclick = () => change(() => set(!get()), true);
  updaters.push(() => {
    button.setAttribute("aria-checked", !!get());
    if (o.when) button.hidden = !o.when();
  });
  return button;
}
const hex = c => c === "white" ? "#ffffff" : c;
const rgbOf = c => [1, 3, 5].map(i => parseInt(hex(c).substr(i, 2), 16));
// A grid of cells on the two columns; o.wide spans each across both.
function cells(children, o) {
  o = o || {};
  const node = make("div", {class: "cells" + (o.group ? " group" : "")}, children);
  if (o.wide) for (const child of node.children) child.classList.add("wide");
  if (o.when) updaters.push(() => { node.hidden = !o.when(); });
  return node;
}
// Words typed in a field. o.stack puts it under its label, across both
// columns; o.center centres it; o.maxlength limits it; o.spaces keeps words
// of only spaces, which are otherwise none. Returns the row, whose _input
// is the field.
function textRow(label, get, set, o) {
  o = o || {};
  const input = make("input", {type: "text", class: "field" + (o.center ? " center" : ""), id: nextId(), autocomplete: "off", spellcheck: "false"});
  if (o.maxlength) input.maxLength = o.maxlength;
  input.addEventListener("input", () => preview(() => set(input.value)));
  input.addEventListener("change", () => {
    if (!o.spaces && input.value !== "" && !input.value.trim()) { input.value = ""; preview(() => set("")); }
    gesture = false;
  });
  updaters.push(() => { if (document.activeElement !== input) { const v = get(); input.value = v == null ? "" : v; } });
  const node = row(label, input, o);
  node._input = input;
  return node;
}
function iconButton(label, name, onClick, cls) {
  const button = make("button", {type: "button", class: cls || "icon", "aria-label": label}, [icon(name)]);
  if (onClick) button.onclick = onClick;
  return button;
}
// A button's words: list[0] normally, say(i) shows list[i] for a moment.
// Every word sits in the same place, so the button keeps the widest's width.
// name is its icon.
function sayButton(button, list, name) {
  const spans = list.map((word, i) => make("span", {class: i ? "" : "on", text: word}));
  button.replaceChildren(icon(name), make("span", {class: "say", "aria-live": "polite"}, spans));
  let timer = 0;
  const pick = i => spans.forEach((s, j) => s.classList.toggle("on", i === j));
  return i => {
    clearTimeout(timer);
    pick(i);
    if (i) timer = setTimeout(() => pick(0), 1500);
  };
}
)js",
R"js(
/*COLOUR-START*/
// Colour maths. Every name is a function declaration with its constants
// inside: fullLook asks themeOf while the style is first built, and
// tests/sheet-colours.js loads this block with SheetCore alone.
// A colour as the page compares and keeps it: lowercase #rrggbb.
function norm(c) {
  c = typeof c === "string" ? c.trim().toLowerCase() : "";
  if (c === "white") return "#ffffff";
  if (/^#?[0-9a-f]{3}$/.test(c)) return "#" + c.replace("#", "").replace(/./g, "$&$&");
  return /^[0-9a-f]{6}$/.test(c) ? "#" + c : c;
}
function isHex(c) { return /^#[0-9a-f]{6}$/.test(norm(c)); }
// A look's fifteen colours, normalised, always in this order.
function coloursOf(l) {
  l = l || {};
  const c = {};
  for (const k of ["background", "text", "comment", "heading", "pedalDown", "pedalUp"]) c[k] = norm(l[k]);
  c.rhythm = [0, 1, 2, 3, 4, 5, 6, 7, 8].map(i => norm(Array.isArray(l.rhythm) ? l.rhythm[i] : ""));
  return c;
}
function coloursKey(l) { return JSON.stringify(coloursOf(l)); }
function sameColours(a, b) { return !!a && !!b && coloursKey(a) === coloursKey(b); }
// The theme a look's colours are: 0 Dark, 2 White, else 3, the user's own.
function themeOf(l) {
  const key = coloursKey(l);
  return key === coloursKey(SheetCore.THEMES[0]) ? 0 : key === coloursKey(SheetCore.THEMES[2]) ? 2 : 3;
}
function hexRgb(c) {
  c = norm(c);
  return /^#[0-9a-f]{6}$/.test(c) ? [1, 3, 5].map(i => parseInt(c.substr(i, 2), 16)) : [0, 0, 0];
}
function rgbHex(rgb) { return "#" + rgb.map(x => Math.min(255, Math.max(0, Math.round(x))).toString(16).padStart(2, "0")).join(""); }
// HSV as the card holds it: h 0 to 360, s and v 0 to 100.
function rgbHsv(rgb) {
  const [r, g, b] = rgb.map(x => x / 255), max = Math.max(r, g, b), d = max - Math.min(r, g, b);
  const h = !d ? 0 : max === r ? ((g - b) / d + 6) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return {h: h * 60, s: max ? d / max * 100 : 0, v: max * 100};
}
function hsvRgb(hsv) {
  const h = (((hsv.h % 360) + 360) % 360) / 60, s = hsv.s / 100, v = hsv.v / 100;
  const f = n => { const k = (n + h) % 6; return (v - v * s * Math.max(0, Math.min(k, 4 - k, 1))) * 255; };
  return [f(5), f(3), f(1)];
}
function hexOf(hsv) { return rgbHex(hsvRgb(hsv)); }
// An sRGB channel from 0 to 255 as linear light.
function srgbLinear(x) { x /= 255; return x <= 0.04045 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4); }
// Björn Ottosson's OKLab: [L, a, b] of a colour, and back to sRGB from 0 to 1, unclamped.
function oklab(c) {
  const [r, g, b] = hexRgb(c).map(srgbLinear);
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return [0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
          0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s];
}
function labRgb(L, a, b) {
  const l = Math.pow(L + 0.3963377774 * a + 0.2158037573 * b, 3);
  const m = Math.pow(L - 0.1055613458 * a - 0.0638541728 * b, 3);
  const s = Math.pow(L - 0.0894841775 * a - 1.2914855480 * b, 3);
  return [4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
          -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s].map(x => x <= 0.0031308 ? 12.92 * x : 1.055 * Math.pow(x, 1 / 2.4) - 0.055);
}
// [L, C, h in degrees].
function oklch(c) {
  const [L, a, b] = oklab(c), h = Math.atan2(b, a) * 180 / Math.PI;
  return [L, Math.sqrt(a * a + b * b), h < 0 ? h + 360 : h];
}
// An OKLCH colour as #rrggbb. Outside sRGB its chroma is bisected 16 times
// down to the most that fits.
function fromOklch(L, C, h) {
  L = Math.min(1, Math.max(0, L));
  const at = c => labRgb(L, c * Math.cos(h * Math.PI / 180), c * Math.sin(h * Math.PI / 180));
  const fits = rgb => rgb.every(x => x >= -0.0005 && x <= 1.0005);
  let rgb = at(C);
  if (!fits(rgb)) {
    let lo = 0, hi = C;
    for (let i = 0; i < 16; ++i) { const mid = (lo + hi) / 2; if (fits(at(mid))) lo = mid; else hi = mid; }
    rgb = at(lo);
  }
  return rgbHex(rgb.map(x => x * 255));
}
// WCAG's relative luminance and contrast ratio.
function lum(c) { const [r, g, b] = hexRgb(c).map(srgbLinear); return 0.2126 * r + 0.7152 * g + 0.0722 * b; }
function contrast(a, b) { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); }
// Light paper: past 0.18 dark ink reads better than light.
function isLight(c) { return lum(c) > 0.18; }
// Nine rhythm colours from one. Quarter is the seed; the others take the
// lightness of the paper's own theme, the seed's chroma (at least 0.09 unless
// it is a grey) and its hue turned 25 degrees a rhythm, then step away from
// the paper until they read at 3:1. Longer is the theme's.
function seedRhythms(s, bg, themes) {
  themes = themes || SheetCore.THEMES;
  const light = isLight(bg), t = themes[light ? 2 : 0], [, cs, hs] = oklch(s), c = cs < 0.04 ? cs : Math.max(cs, 0.09);
  return t.rhythm.map((own, i) => {
    if (i === 4) return norm(s);
    if (i === 8) return norm(own);
    const h = hs + (i - 4) * 25;
    let L = oklch(own)[0], out = fromOklch(L, c, h);
    while (contrast(out, bg) < 3 && L > 0 && L < 1) { L = Math.min(1, Math.max(0, L + (light ? -0.01 : 0.01))); out = fromOklch(L, c, h); }
    return out;
  });
}
/*COLOUR-END*/
)js",
R"js(
// A colour is named by its key: background, heading, comment, chords (the
// seed of all nine rhythms, which is Quarter), text, r0 to r8, pedalDown and
// pedalUp.
function colourIn(look, key) {
  return norm(key === "chords" ? look.rhythm[4] : /^r[0-8]$/.test(key) ? look.rhythm[+key[1]] : look[key]);
}
function colourOf(key) { return colourIn(L(), key); }
// One colour, or all nine from the seed for chords. A background picked over
// an image with no dim starts it at 30%. False when v is not a colour.
function setColour(key, v) {
  v = norm(v);
  if (!isHex(v)) return false;
  const l = L();
  if (key === "chords") l.rhythm = seedRhythms(v, l.background, SheetCore.THEMES);
  else if (/^r[0-8]$/.test(key)) l.rhythm[+key[1]] = v;
  else l[key] = v;
  if (key === "background" && l.ground === 1 && image && !l.dim) l.dim = 30;
  return true;
}
// The default that suits the paper: White's on light paper, Dark's on dark.
function base() { return SheetCore.THEMES[isLight(L().background) ? 2 : 0]; }
function baseOf(key) { return colourIn(base(), key); }
// A colour that differs from its default; the seed when any rhythm does.
function isChanged(key) {
  const b = base();
  return key === "chords" ? L().rhythm.some((c, i) => norm(c) !== norm(b.rhythm[i])) : colourOf(key) !== baseOf(key);
}
function resetColour(key) {
  const b = base();
  if (key === "chords") L().rhythm = b.rhythm.slice();
  else if (/^r[0-8]$/.test(key)) L().rhythm[+key[1]] = b.rhythm[+key[1]];
  else L()[key] = b[key];
}
// A theme's colours onto the look; the background's mode, image, fit, dim,
// one colour and font stay.
function useColours(c) {
  for (const k of ["background", "text", "comment", "heading", "pedalDown", "pedalUp"]) L()[k] = c[k];
  L().rhythm = c.rhythm.slice();
}
// Theme: Dark, White and Custom, each green while the colours are its own.
// Custom holds the slot, or the
// colours of the user's own while they are in use. Hovering a pill shows it
// on the sheet.
function themeRow() {
  const box = make("div", {class: "pills looks", role: "group"});
  const pills = [[0, "Dark", "moon"], [2, "White", "sun"], [3, "Custom", "pencil"]].map(([n, name, mark]) => {
    const colours = () => n < 3 ? SheetCore.THEMES[n] : themeOf(L()) === 3 ? L() : slot;
    const button = pill(name, null, null, mark);
    button.onclick = () => { const c = colours(); if (c && !sameColours(c, L())) change(() => useColours(c), true); };
    button.addEventListener("pointerenter", e => { const c = colours(); if (e.pointerType === "mouse" && c) showLook(coloursOf(c)); });
    button.addEventListener("pointerleave", e => { if (e.pointerType === "mouse") showLook(); });
    box.appendChild(button);
    return {n, button, colours};
  });
  updaters.push(() => {
    const on = themeOf(L());
    for (const p of pills) {
      const c = p.colours();
      p.button.setAttribute("aria-pressed", p.n === on);
      p.button.disabled = !c;
    }
  });
  return box;
}
)js",
R"js(
// The legend: every colour by name, on a sample of the paper. Hovering or
// focusing one dims on the sheet what it does not paint; clicking one opens
// its card.
const RHYTHMS = ["64th", "32nd", "16th", "8th", "Quarter", "Half", "Whole", "Double whole", "Longer"];
const NAMES = {background: "Background", heading: "Header", comment: "Transpose & tempo", chords: "Chords", text: "Chords", pedalDown: "Pedal down", pedalUp: "Pedal up"};
const nameOf = key => /^r[0-8]$/.test(key) ? RHYTHMS[+key[1]] : NAMES[key];
// The pedal bands by their marks: 1 down, 2 up.
const BANDS = {pedalDown: 1, pedalUp: 2};
const narrow = () => matchMedia("(max-width:720px)").matches;
const focusVisible = node => { try { return node.matches(":focus-visible"); } catch (e) { return true; } };
const inks = [];
let rove = "background", pointer = "", drawnLegend = "";
function inkOf(key) { const item = inks.find(x => x.key === key && !x.node.hidden); return item ? item.node : null; }
function inkItem(key, o) {
  o = o || {};
  const dot = make("span", {class: "changed", hidden: ""}), swatch = make("span", {class: "swatch", "aria-hidden": "true"}, [dot]), state = make("span", {class: "vh"});
  const node = make("button", {type: "button", class: "ink" + (o.wide ? " wide" : ""), "data-key": key, "aria-haspopup": "dialog", "aria-controls": "colour",
                               "aria-expanded": "false", tabindex: "-1"}, [swatch, make("span", {class: "name", text: nameOf(key)}), state]);
  // Its card opens, or, when it is open already, takes the cursor.
  node.onclick = () => {
    if (editing === key && anchor && anchor.legend === key) sv.focus({preventScroll: true});
    else openColour(key, {legend: key, touch: pointer === "touch"}, true);
  };
  node.addEventListener("pointerenter", e => { if (e.pointerType !== "touch" && !editing) spot(key); });
  node.addEventListener("pointerleave", e => { if (e.pointerType !== "touch" && spotKey === key) spot(null); });
  node.addEventListener("focus", () => {
    rove = key;
    for (const x of inks) x.node.tabIndex = x.node === node ? 0 : -1;
    if (!editing && focusVisible(node)) spot(key);
  });
  node.addEventListener("blur", () => { if (spotKey === key) spot(null); });
  inks.push({key, node, swatch, dot, state, when: o.when});
  return node;
}
// Page, chords (by rhythm, or the one colour) and pedals. One tab stop:
// the arrows move along the rows and between them.
function legend() {
  const one = () => !!L().oneColour, byRhythm = () => !L().oneColour;
  const box = make("div", {class: "legend group", role: "group", "aria-label": "Colours"}, [
    make("div", {class: "inks"}, [inkItem("background"), inkItem("heading"), inkItem("comment", {wide: true})]),
    make("div", {class: "inks"}, [inkItem("chords", {when: byRhythm})].concat(RHYTHMS.map((name, i) => inkItem("r" + i, {when: byRhythm})), [inkItem("text", {when: one})])),
    make("div", {class: "inks"}, [inkItem("pedalDown"), inkItem("pedalUp")]),
  ]);
  box.addEventListener("pointerdown", e => { pointer = e.pointerType; });
  box.addEventListener("keydown", e => {
    pointer = "";
    const list = inks.filter(x => !x.node.hidden).map(x => x.node), i = list.indexOf(document.activeElement);
    const to = {ArrowLeft: i - 1, ArrowRight: i + 1, ArrowUp: i - 2, ArrowDown: i + 2, Home: 0, End: list.length - 1}[e.key];
    if (i < 0 || typeof to !== "number") return;
    e.preventDefault();
    const next = list[Math.max(0, Math.min(list.length - 1, to))];
    rove = next.dataset.key;
    for (const node of list) node.tabIndex = node === next ? 0 : -1;
    next.focus();
  });
  updaters.push(() => {
    for (const x of inks) { x.node.hidden = !!x.when && !x.when(); x.node.setAttribute("aria-expanded", editing === x.key); }
    const shown = inks.filter(x => !x.node.hidden), at = shown.find(x => x.key === rove) || shown[0];
    for (const x of inks) x.node.tabIndex = x === at ? 0 : -1;
  });
  // The paper, the swatches and the dots follow every render, so a theme
  // shown on the sheet shows here too. The ink is whichever reads better.
  renderHooks.push(look => {
    const bg = norm(look.background), on = contrast("#1a1a1a", bg) >= contrast("#f5f5f5", bg) ? "#1a1a1a" : "#f5f5f5", rgb = hexRgb(on).join(",");
    const style = cssText(backgroundCss(look)) + ";--paper-ink:" + on + ";--paper-line:rgba(" + rgb + ",.25);--paper-line2:rgba(" + rgb + ",.5);--paper-fill:rgba(" +
      hexRgb(bg).join(",") + "," + (look.ground === 1 && image ? 0.85 : 0.72) + ");--paper-solid:" + bg;
    if (style !== drawnLegend) { box.setAttribute("style", style); drawnLegend = style; }
    const b = SheetCore.THEMES[isLight(bg) ? 2 : 0], stop = i => (i * 100 / 9).toFixed(2) + "%";
    for (const x of inks) {
      const seed = x.key === "chords", c = colourIn(look, x.key);
      const changed = seed ? look.rhythm.some((r, i) => norm(r) !== norm(b.rhythm[i])) : c !== colourIn(b, x.key);
      x.swatch.style.background = seed ? "linear-gradient(90deg," + look.rhythm.map((r, i) => norm(r) + " " + stop(i) + " " + stop(i + 1)).join(",") + ")" : c;
      x.dot.hidden = !changed;
      x.state.textContent = changed ? ", changed" : "";
    }
  });
  return box;
}
// The sheet lit for a colour: what it paints keeps its ink and the rest
// dims; a pedal's colour shows that pedal's bands alone. While a card is
// open on a touch screen or a phone, what its colour paints is outlined.
let spotKey = null, peeked = false;
function paints(key) {
  const sheet = $("sheet");
  if (key === "background") return [$("page")];
  if (key === "heading") return [$("heading")];
  if (key === "comment") return sheet.querySelectorAll(".comment");
  if (BANDS[key]) return sheet.querySelectorAll('.chord>span[data-p="' + (key === "pedalDown" ? "d" : "u") + '"]');
  return sheet.querySelectorAll(/^r[0-8]$/.test(key) ? '.chord[data-r="' + key[1] + '"]' : ".chord");
}
function paintLit() {
  const page = $("page"), band = !!spotKey && !!BANDS[spotKey];
  for (const node of page.querySelectorAll(".lit")) node.classList.remove("lit");
  page.classList.remove("lit");
  page.classList.toggle("spot", !!spotKey);
  page.classList.toggle("spot-band", band);
  const key = spotKey || (editing && (anchor.touch || narrow()) ? editing : null);
  if (!key || (spotKey && key === "background")) return;
  for (const node of band ? $("sheet").querySelectorAll(".chord") : paints(key)) node.classList.add("lit");
}
function spot(key) {
  if (key && BANDS[key] && !pedals.length) key = null;
  if (key === spotKey) return;
  spotKey = key;
  if (key && BANDS[key]) { peeked = true; render(L(), peekPedals(BANDS[key])); }
  else if (peeked) { peeked = false; render(); }
  else paintLit();
}
renderHooks.push(paintLit);
drawHooks.push(() => { if (spotKey && BANDS[spotKey]) render(L(), peekPedals(BANDS[spotKey])); });
)js",
R"js(
// The colour card: a shade square, a hue bar, the hex code, the eyedropper,
// Reset, the sheet's colours and the recent ones. A drag, a held key, a
// code or a pick is one change.
const card = $("colour");
let editing = null, anchor = null, pairKeys = null, chordKey = "chords", startColour = "", hsv = {h: 0, s: 0, v: 0}, dragging = false, hexEdited = false, clickTimer = 0;
let recent = (() => { const r = readStore(RECENT); return Array.isArray(r) ? r.map(norm).filter((c, i, a) => isHex(c) && a.indexOf(c) === i).slice(0, 9) : []; })();
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const colourName = make("span", {id: "colour-name"}), pairNames = [make("span"), make("span")];
// A chord on a pedal band: its colour or the pedal's.
const pairSeg = segment(() => pairKeys && editing === pairKeys[1] ? 1 : 0, v => { if (pairKeys) retarget(pairKeys[v]); },
                        [[0, pairNames[0]], [1, pairNames[1]]], {label: "Colour", direct: true});
const doneButton = make("button", {type: "button"}, [icon("check"), "Done"]);
doneButton.onclick = () => closeColour(true);
const cardRows = make("div", {class: "card-rows"});
const svKnob = make("span", {class: "knob"}), hueKnob = make("span", {class: "knob"});
const sv = make("div", {class: "sv", tabindex: "0", role: "slider", "aria-label": "Shade", "aria-valuemin": "0", "aria-valuemax": "100"}, [svKnob]);
const hue = make("div", {class: "hue", tabindex: "0", role: "slider", "aria-label": "Hue", "aria-valuemin": "0", "aria-valuemax": "359"}, [hueKnob]);
const hexField = make("input", {type: "text", class: "field", id: "colour-hex", maxlength: "7", autocomplete: "off", spellcheck: "false", "aria-label": "Hex"});
const dropper = iconButton("Pick from screen", "pipette", pickFromScreen);
dropper.disabled = !window.EyeDropper;
const resetButton = make("button", {type: "button"}, [icon("reset"), "Reset"]);
const sheetHead = heading("This sheet"), recentHead = heading("Recent", () => recent.length);
sheetHead.id = nextId();
recentHead.id = nextId();
const sheetChips = make("div", {class: "chips", role: "group", "aria-labelledby": sheetHead.id});
const recentChips = make("div", {class: "chips", role: "group", "aria-labelledby": recentHead.id});
card.append(make("div", {class: "card-top"}, [colourName, doneButton]), pairSeg, cardRows, sv, hue,
            make("div", {class: "tools-row"}, [hexField, dropper, resetButton]), sheetHead, sheetChips, recentHead, recentChips);
// The colours on the sheet, as the legend shows them, each once.
function sheetColours() {
  const keys = ["background", "heading", "comment"].concat(L().oneColour ? ["text"] : RHYTHMS.map((name, i) => "r" + i), ["pedalDown", "pedalUp"]), out = [];
  for (const k of keys) {
    const c = colourOf(k), at = out.find(x => x.colour === c);
    if (at) at.names.push(nameOf(k)); else if (out.length < 14) out.push({colour: c, names: [nameOf(k)]});
  }
  return out.map(x => ({colour: x.colour, label: x.names.join(", ")}));
}
// A row of colour chips, made again only when its colours change; the one
// of the colour being edited is pressed. One tab stop.
function chips(box, list, now) {
  const key = list.map(x => x.colour + x.label).join("|");
  if (box._key !== key) {
    const was = box.contains(document.activeElement) ? document.activeElement : null, at = was ? [...box.children].indexOf(was) : -1;
    box._key = key;
    box.replaceChildren(...list.map(x => {
      const chip = make("button", {type: "button", class: "chip", "aria-label": x.label});
      chip.style.background = x.colour;
      chip._colour = x.colour;
      chip.onclick = () => useChip(x.colour, box === recentChips);
      return chip;
    }));
    const all = [...box.children], to = was && (all.find(c => c._colour === was._colour) || all[Math.min(at, all.length - 1)]);
    if (to) to.focus();
  }
  const all = [...box.children];
  let stop = all.indexOf(document.activeElement);
  if (stop < 0) stop = Math.max(0, all.findIndex(c => c._colour === now));
  all.forEach((c, i) => { c.setAttribute("aria-pressed", c._colour === now); c.tabIndex = i === stop ? 0 : -1; });
}
for (const box of [sheetChips, recentChips]) box.addEventListener("keydown", e => {
  const all = [...box.children], i = all.indexOf(document.activeElement);
  const to = {ArrowLeft: i - 1, ArrowRight: i + 1, ArrowUp: i - 9, ArrowDown: i + 9, Home: 0, End: all.length - 1}[e.key];
  if (i < 0 || typeof to !== "number") return;
  e.preventDefault();
  const chip = all[clamp(to, 0, all.length - 1)];
  all.forEach(c => { c.tabIndex = c === chip ? 0 : -1; });
  chip.focus();
});
// A recent colour goes to the front.
function useChip(c, fromRecent) {
  const key = editing;
  if (!key) return;
  if (fromRecent) { recent = [c].concat(recent.filter(x => x !== c)); writeStore(RECENT, recent); }
  if (c !== colourOf(key)) change(() => setColour(key, c), true); else paintCard();
}
// Chords by rhythm or in one colour, as undo, redo or the panel left them:
// the card moves to the colour they show, as its own switch does.
function followChords() {
  const one = !!L().oneColour, to = k => /^(r[0-8]|chords|text)$/.test(k) && (k === "text") !== one ? (one ? "text" : chordKey) : k;
  if (pairKeys) pairKeys = [to(pairKeys[0]), pairKeys[1]];
  const key = to(editing);
  if (key === editing) return;
  editing = key;
  if (anchor.legend) anchor.legend = key;
  startColour = colourOf(key);
  hsv = rgbHsv(hexRgb(startColour));
  paintLit();
}
// The card shows the colour being edited. A drag makes it, so it is kept;
// otherwise a grey keeps the hue it had, and black its saturation too.
function paintCard() {
  if (!editing) return;
  followChords();
  const c = colourOf(editing), pure = "hsl(" + Math.round(hsv.h) + ",100%,50%)";
  if (!dragging && c !== hexOf(hsv)) { const n = rgbHsv(hexRgb(c)); hsv = {h: n.s && n.v ? n.h : hsv.h, s: n.v ? n.s : hsv.s, v: n.v}; }
  colourName.textContent = nameOf(editing);
  pairSeg.hidden = !pairKeys;
  if (pairKeys) pairKeys.forEach((k, i) => { pairNames[i].textContent = nameOf(k); });
  sv.style.background = "linear-gradient(to top,#000,transparent),linear-gradient(to right,#fff," + pure + ")";
  Object.assign(svKnob.style, {left: hsv.s + "%", top: 100 - hsv.v + "%", background: c});
  Object.assign(hueKnob.style, {left: hsv.h / 3.6 + "%", background: pure});
  sv.setAttribute("aria-valuenow", Math.round(hsv.v));
  sv.setAttribute("aria-valuetext", c.toUpperCase());
  hue.setAttribute("aria-valuenow", Math.round(hsv.h));
  if (!hexEdited || document.activeElement !== hexField) { hexEdited = false; hexField.value = c.toUpperCase(); }
  resetButton.disabled = !isChanged(editing);
  chips(sheetChips, sheetColours(), c);
  chips(recentChips, recent.map(x => ({colour: x, label: x.toUpperCase()})), c);
  recentHead.hidden = recentChips.hidden = !recent.length;
}
updaters.push(paintCard);
// The colour before and after a change go to the front of Recent.
function addRecent() {
  if (!editing) return;
  const c = colourOf(editing);
  if (c === startColour) return;
  recent = [c, startColour].concat(recent).filter((x, i, a) => isHex(x) && a.indexOf(x) === i).slice(0, 9);
  startColour = c;
  writeStore(RECENT, recent);
  paintCard();
}
// A drag, a held key or a typed code ends as one change.
function finish() {
  dragging = false;
  if (!gesture) return;
  gesture = false;
  settle(true);
  addRecent();
}
function put() {
  const c = hexOf(hsv);
  if (c !== colourOf(editing)) preview(() => setColour(editing, c));
  paintCard();
}
function slider(node, at, keys) {
  node.addEventListener("pointerdown", e => {
    if (e.button !== 0 || !editing) return;
    e.preventDefault();
    node.focus({preventScroll: true});
    try { node.setPointerCapture(e.pointerId); } catch (error) {}
    gesture = false;
    dragging = true;
    at(e);
  });
  node.addEventListener("pointermove", e => { if (dragging && node.hasPointerCapture(e.pointerId)) at(e); });
  for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) node.addEventListener(type, () => { if (dragging) finish(); });
  node.addEventListener("keydown", e => {
    const next = editing && keys(e.key, e.shiftKey ? 10 : 1);
    if (!next) return;
    e.preventDefault();
    hsv = next;
    put();
  });
  node.addEventListener("keyup", () => { if (!dragging) finish(); });
}
slider(sv, e => {
  const r = sv.getBoundingClientRect();
  hsv = {h: hsv.h, s: clamp((e.clientX - r.left) / r.width, 0, 1) * 100, v: (1 - clamp((e.clientY - r.top) / r.height, 0, 1)) * 100};
  put();
}, (key, big) => {
  let s = hsv.s, v = hsv.v;
  if (key === "ArrowLeft") s -= big; else if (key === "ArrowRight") s += big; else if (key === "ArrowUp") v += big; else if (key === "ArrowDown") v -= big;
  else if (key === "PageUp") v += 10; else if (key === "PageDown") v -= 10; else if (key === "Home") s = 0; else if (key === "End") s = 100; else return null;
  return {h: hsv.h, s: clamp(Math.round(s), 0, 100), v: clamp(Math.round(v), 0, 100)};
});
slider(hue, e => {
  const r = hue.getBoundingClientRect();
  hsv = {h: clamp((e.clientX - r.left) / r.width * 360, 0, 359), s: hsv.s, v: hsv.v};
  put();
}, (key, big) => {
  const step = {ArrowLeft: -big, ArrowDown: -big, ArrowRight: big, ArrowUp: big, PageUp: 30, PageDown: -30}[key];
  const h = key === "Home" ? 0 : key === "End" ? 359 : step === undefined ? null : clamp(Math.round(hsv.h) + step, 0, 359);
  return h === null ? null : {h, s: hsv.s, v: hsv.v};
});
// Six hex digits show as they are typed; Enter or leaving keeps a code of
// three or six, and anything else goes back. Escape puts back what was there.
hexField.addEventListener("input", () => {
  hexEdited = true;
  const t = hexField.value.trim();
  if (editing && /^#?[0-9a-f]{6}$/i.test(t) && norm(t) !== colourOf(editing)) preview(() => setColour(editing, t));
});
function commitHex() {
  if (!hexEdited || !editing) return;
  hexEdited = false;
  const v = norm(hexField.value);
  if (!isHex(v)) { if (gesture) restore(history.pop()); paintCard(); return; }
  if (v !== colourOf(editing)) preview(() => setColour(editing, v));
  finish();
  paintCard();
}
hexField.addEventListener("change", commitHex);
hexField.addEventListener("blur", commitHex);
hexField.addEventListener("keydown", e => {
  if (e.key === "Enter") { e.preventDefault(); commitHex(); hexField.select(); }
  else if (e.key === "Escape" && hexEdited) { e.stopPropagation(); hexEdited = false; if (gesture) restore(history.pop()); paintCard(); }
});
)js",
R"js(
async function pickFromScreen() {
  const key = editing;
  if (!key || !window.EyeDropper) return;
  let got = null;
  try { got = await new EyeDropper().open(); } catch (e) { return; }
  const v = norm(got && got.sRGBHex);
  if (editing === key && isHex(v) && v !== colourOf(key)) { change(() => setColour(key, v), true); addRecent(); }
}
resetButton.onclick = () => {
  if (!editing || !isChanged(editing)) return;
  change(() => resetColour(editing), true);
  if (resetButton.disabled) sv.focus({preventScroll: true});
};
// Tab goes round the card.
card.addEventListener("keydown", e => {
  if (e.key !== "Tab") return;
  const list = [...card.querySelectorAll("button,input,[tabindex]")].filter(x => x.tabIndex >= 0 && !x.disabled && x.getClientRects().length);
  const first = list[0], last = list[list.length - 1];
  if (!first) return;
  if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
  else if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
});
// What is being edited is kept before the card moves on or closes.
function settleCard() { if (hexEdited) commitHex(); finish(); }
// The card on another colour, where it is; beside the legend, beside that
// colour's item.
function retarget(key) {
  if (!editing || key === editing) return;
  settleCard();
  if (pairKeys && !pairKeys.includes(key)) pairKeys = /^(r[0-8]|text)$/.test(key) ? [key, pairKeys[1]] : null;
  editing = key;
  if (anchor.legend) anchor.legend = key;
  startColour = colourOf(key);
  hsv = rgbHsv(hexRgb(startColour));
  updaters.forEach(update => update());
  paintLit();
  placeColour();
}
// Opening the card:from a legend item beside the panel, the cursor in the
// square; from the sheet under what was clicked, with its legend item shown.
// Anchors are kept as data, so a redraw does not lose them.
function openColour(key, at, focus, keys) {
  clearTimeout(clickTimer);
  if (!toolbar.hidden) closePart(false);
  spot(null);
  settleCard();
  editing = key;
  anchor = at;
  pairKeys = keys || null;
  chordKey = at.chord !== undefined ? "r" + result.items[at.chord].rhythm : /^r[0-8]$/.test(key) ? key : "chords";
  startColour = colourOf(key);
  hsv = rgbHsv(hexRgb(startColour));
  card.hidden = false;
  document.body.classList.add("colour-open");
  updaters.forEach(update => update());
  paintLit();
  placeColour();
  if (focus) sv.focus({preventScroll: true});
  if (narrow()) keepAbove();
  else if (!at.legend) { setFold("colours", true); const item = inkOf(key); if (item) item.scrollIntoView({block: "nearest"}); }
}
function closeColour(returnFocus) {
  clearTimeout(clickTimer);
  if (!editing) return;
  settleCard();
  const from = anchor && anchor.legend;
  editing = anchor = pairKeys = null;
  card.hidden = true;
  card.style.left = card.style.top = "";
  document.body.classList.remove("colour-open");
  for (const x of inks) x.node.setAttribute("aria-expanded", "false");
  paintLit();
  const item = returnFocus && from && inkOf(from);
  if (item && item.getClientRects().length) item.focus();
}
function anchorRect() {
  const a = anchor;
  let node = null;
  if (!a) return null;
  if (a.margin) { const r = $("page").getBoundingClientRect(), x = r.left + a.margin[0], y = r.top + a.margin[1]; return {left: x, right: x, top: y, bottom: y}; }
  if (a.legend) node = inkOf(a.legend);
  else if (a.chord !== undefined) node = $("sheet").querySelector('.chord[data-i="' + a.chord + '"]');
  else if (a.comment !== undefined) node = $("sheet").querySelectorAll(".comment")[a.comment];
  return node && node.getClientRects()[0] || null;
}
// Under the anchor, or over it when there is no room; beside the panel for
// a legend item; kept in the window. A phone keeps it at the bottom.
function placeColour() {
  if (!editing) return;
  if (narrow()) { card.style.left = card.style.top = ""; return; }
  const r = anchorRect();
  if (!r) return;
  const height = card.offsetHeight, bar = document.querySelector("header").getBoundingClientRect().bottom;
  let left = r.left, top = r.bottom + 8;
  if (anchor.legend) { left = document.querySelector("aside").getBoundingClientRect().right + 8; top = r.top - 8; }
  else if (top + height > innerHeight - 8) top = r.top - height - 8;
  // No room under or over it: beside it, so it stays in sight.
  if (!anchor.legend && top < bar + 8 && r.bottom + 8 + height > innerHeight - 8) {
    left = r.right + 304 <= innerWidth ? r.right + 8 : r.left - 296;
    top = (r.top + r.bottom - height) / 2;
  }
  card.style.left = Math.max(8, Math.min(innerWidth - 296, left)) + "px";
  card.style.top = Math.max(bar + 8, Math.min(innerHeight - height - 8, top)) + "px";
}
// On a phone the window scrolls so what is being edited stays above the card.
function keepAbove() {
  const r = anchorRect(), top = card.getBoundingClientRect().top - 8;
  if (r && r.bottom > top) scrollBy(0, r.bottom - top);
}
drawHooks.push(placeColour);
for (const node of [window, document.querySelector("main"), $("panel")]) node.addEventListener("scroll", placeColour, {passive: true});
window.addEventListener("resize", placeColour);
window.addEventListener("beforeprint", () => spot(null));
// A press anywhere else closes the card, but not one on a scroll bar; off
// the paper it also stops a click there from opening it.
document.addEventListener("pointerdown", e => {
  const t = e.target;
  if (!t.closest || t.closest("#page") || (t.clientWidth && (e.offsetX >= t.clientWidth || e.offsetY >= t.clientHeight))) return;
  clearTimeout(clickTimer);
  if (editing && !t.closest("#colour,.legend,#undo,#redo,#view")) closeColour(false);
}, true);
document.addEventListener("keydown", e => { if (e.key === "Escape") closeColour(true); });
// Clicking the paper: a chord opens its rhythm's colour (or the one colour),
// and on a pedal band that pedal's too; a Transpose or Tempo line its
// colour; the margin the background. A click that brings the window back,
// ends a selection or closes the part card opens nothing; a double click
// selects a word, so the card waits 200 ms for it.
const pageNode = $("page");
let partAtDown = false, touchAtDown = false, focusedAt = -1000;
window.addEventListener("focus", () => { focusedAt = performance.now(); });
pageNode.addEventListener("pointerdown", e => { partAtDown = !toolbar.hidden; touchAtDown = e.pointerType === "touch"; }, true);
function targetOf(e) {
  const t = e.target, sheet = $("sheet");
  if (t === pageNode) { const r = pageNode.getBoundingClientRect(); return {key: "background", at: {margin: [e.clientX - r.left, e.clientY - r.top]}}; }
  const chord = t.closest(".chord");
  if (chord && sheet.contains(chord)) {
    const i = +chord.dataset.i, own = L().oneColour ? "text" : "r" + result.items[i].rhythm, band = t.closest(".chord>span[data-p]");
    if (!band) return {key: own, at: {chord: i}};
    const pedal = band.dataset.p === "d" ? "pedalDown" : "pedalUp";
    return {key: band.previousSibling ? pedal : own, at: {chord: i}, keys: [own, pedal]};
  }
  const comment = t.closest(".comment");
  return comment && sheet.contains(comment) ? {key: "comment", at: {comment: [...sheet.querySelectorAll(".comment")].indexOf(comment)}} : null;
}
pageNode.addEventListener("click", e => {
  if (e.button !== 0 || e.shiftKey || e.ctrlKey || e.altKey || e.metaKey || partAtDown || performance.now() - focusedAt < 300) return;
  const selection = getSelection();
  if (selection && !selection.isCollapsed) return;
  clearTimeout(clickTimer);
  const hit = e.detail > 1 ? null : targetOf(e);
  if (!hit) { closeColour(false); return; }
  hit.at.touch = touchAtDown;
  clickTimer = setTimeout(() => {
    const now = getSelection();
    if (toolbar.hidden && (!now || now.isCollapsed)) openColour(hit.key, hit.at, false, hit.keys);
  }, 200);
});
)js",
R"js(
// The panel: five folds of rows.
const oSeg = (key, choices, x) => segment(() => o()[key], v => { o()[key] = v; }, choices, x);
const oStep = (key, x) => stepper(() => o()[key], v => { o()[key] = v; }, x);
const oSwitch = (label, key, x) => switchRow(label, () => o()[key], v => { o()[key] = v; }, x);
const lSeg = (key, choices, x) => segment(() => L()[key], v => { L()[key] = v; }, choices, x);
const lStep = (key, x) => stepper(() => L()[key], v => { L()[key] = v; }, x);
const NOTES = ["note", "notes"], SEMITONES = ["semitone", "semitones"];
const perSection = () => mode() === 2;
// Leaving the search for Fixed keeps the shift the sheet had outside the
// user's parts, so it does not jump.
function setMode(v) {
  if (v === 0 && mode() !== 0) o().transpose = perSection() ? SheetCore.style(played, data.mapping, tempos, meters, o(), []).transposition : songShift();
  o().autoTranspose = v > 0;
  o().autoSections = v === 2;
}
// The number is the one the sheet prints. Setting it fixes the transposition
// there, so the number moves with the button from the key that was found.
const shiftStepper = stepper(() => -songShift(), v => { o().transpose = v ? -v : 0; o().autoTranspose = false; o().autoSections = false; },
                             {min: -24, max: 24, signed: true, unit: SEMITONES});
// The key Best key keeps unless another is better, in the sheet's sign.
const preferStepper = stepper(() => -o().transpose, v => { o().transpose = v ? -v : 0; }, {min: -24, max: 24, signed: true, unit: SEMITONES});
const searchFound = () => !perSection() ? "" : result.sections.length > 1 ? result.sections.length + " sections" :
  "Transpose by " + show(-result.transposition, true);
// While the user's parts hold every chord, the song's number moves nothing.
const allParts = () => result.sections.length > 0 &&
  result.sections.every(s => regions.some(r => r.kind === SheetCore.TRANSPOSE && r.from <= s.from && s.to <= r.to));
// The runs of one transposition: draw fills #sections and sets runCount.
let runCount = 0;
const sectionsHead = heading("Sections", () => runCount);
sectionsHead.id = "sections-head";
const sectionList = make("ul", {id: "sections", "aria-labelledby": "sections-head"});
const pickButton = make("button", {type: "button", id: "pick"}, [icon("select"), "Transpose part"]);
const keepButton = make("button", {type: "button", id: "keep"}, [icon("pin"), "Keep sections"]);
updaters.push(() => {
  sectionList.hidden = !runCount;
  pickButton.disabled = !$("sheet").querySelector(".chord");
  keepButton.hidden = !(perSection() && result.sections.length > 1);
});
const pedalDown = () => L().pedalDown, pedalUp = () => L().pedalUp;
const sheetRows = [
  row("Transposition", segment(mode, setMode, [[0, "Fixed"], [1, "Best key"], [2, "Per section"]]), {stack: true, result: searchFound}),
  row("Transpose by", shiftStepper, {when: () => !perSection() && !allParts()}),
  row("Preferred", preferStepper, {sub: true, when: () => mode() === 1 && !allParts()}),
  row("Threshold", oStep("resilience", {min: 0, max: 20, unit: NOTES}), {sub: true, when: () => mode() === 1 && !allParts()}),
  row("Range", oStep("sectionRange", {min: 1, max: 24, unit: SEMITONES}), {sub: true, when: perSection}),
  row("Switch cost", oStep("sectionSwitchCost", {min: 0, max: 60, unit: NOTES}), {sub: true, when: perSection}),
  row("Minimum length", oStep("sectionMinSeconds", {min: 0, max: 60, step: 0.5, whole: false, unit: "s"}), {sub: true, when: perSection}),
  row("Minimum rest", oStep("sectionRestMs", {min: 0, max: 2000, step: 50, unit: "ms"}), {sub: true, when: perSection}),
  make("div", {class: "group sections"}, [sectionsHead, sectionList, make("div", {class: "actions"}, [pickButton, keepButton])]),
  row("Pedal marks", oSeg("pedalMarks", [[0, "Off"], [1, "Down", {dots: () => [pedalDown()]}], [2, "Up", {dots: () => [pedalUp()]}],
                                         [3, "Both", {dots: () => [pedalDown(), pedalUp()]}]]), {stack: true, group: true, when: () => pedals.length > 0}),
];
// The header. A credit prints when it has words; the facts the app works
// out are on or off.
const credit = (label, key, x) => textRow(label, () => songText()[key], v => { words[key] = v; }, x);
const titleRow = credit("Title", "title", {stack: true});
const dateRow = credit("Date", "date", {sub: true, when: () => H().date});
const fact = (label, key, name) => pill(label, () => H()[key], () => {
  change(() => { H()[key] = !H()[key]; }, true);
  if (key === "date" && H().date) dateRow._input.focus();
}, name);
const headerRows = [
  titleRow,
  credit("Subtitle", "subtitle", {stack: true}),
  pair(credit("Artist", "artist", {stack: true}), textRow("Arranger", () => H().arrangerName, v => { H().arrangerName = v; }, {stack: true})),
  make("div", {class: "pills facts group", role: "group", "aria-label": "Facts"},
       [fact("Tempo", "tempo", "metronome"), fact("Key", "key", "hash"), fact("Difficulty", "difficulty", "gauge"), fact("Date", "date", "calendar")]),
  dateRow,
  row("Alignment", segment(() => !!H().centre, v => { H().centre = v; },
                           [[false, "Left", {icon: "alignStart", bare: true}], [true, "Centre", {icon: "alignCentre", bare: true}]], {inline: true})),
];
)js",
R"js(
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
    // The first picture starts dimmed by 30%, so the chords read on it.
    change(() => { if (!image && !L().dim) L().dim = 30; image = shrunk; L().ground = 1; });
  };
  picture.onerror = () => URL.revokeObjectURL(url);
  picture.src = url;
};
// The background picture's cell: Choose image, or its thumbnail and Replace
// beside a remove button. The panel and the colour card each have one.
function imageCellOf() {
  const button = make("button", {type: "button", class: "cell"}), remove = iconButton("Remove image", "x", () => change(() => { image = ""; }, true), "x");
  button.onclick = () => picker.click();
  const node = make("div", {}, [button, remove]);
  let shown = null;
  updaters.push(() => {
    if (shown === image) return;
    shown = image;
    node.classList.toggle("split", !!image);
    remove.hidden = !image;
    button.replaceChildren(image ? make("img", {class: "thumb", src: image, alt: ""}) : icon("image"),
                           make("span", {class: "name", text: image ? "Replace" : "Choose image"}));
  });
  return node;
}
// The font in use, in its own face; its list shows every font in its own
// face, A to Z.
function fontMenu() {
  const names = SheetCore.FONT_NAMES, order = names.map((_, i) => i).sort((a, b) => names[a].localeCompare(names[b]));
  const face = make("span", {class: "face"});
  const button = make("button", {type: "button", class: "menu-button", "aria-haspopup": "listbox", "aria-expanded": "false"}, [face, icon("chevronDown")]);
  const list = make("div", {class: "menu", role: "listbox", "aria-label": "Font", hidden: ""});
  const node = make("div", {class: "menu-box"}, [button, list]);
  const close = refocus => {
    if (list.hidden) return;
    list.hidden = true;
    button.setAttribute("aria-expanded", "false");
    if (refocus) button.focus();
  };
  const items = order.map(i => {
    const item = make("button", {type: "button", role: "option", class: "menu-item", tabindex: "-1"}, [icon("check"), make("span", {text: names[i]})]);
    item.style.fontFamily = SheetCore.FONTS[i];
    item.onclick = () => { close(true); if (L().font !== i) change(() => { L().font = i; }, true); };
    item._font = i;
    list.appendChild(item);
    return item;
  });
  button.onclick = () => {
    if (!list.hidden) { close(false); return; }
    list.hidden = false;
    button.setAttribute("aria-expanded", "true");
    const on = items.find(item => item._font === L().font) || items[0];
    on.scrollIntoView({block: "nearest"});
    on.focus({preventScroll: true});
  };
  list.addEventListener("keydown", e => {
    const i = items.indexOf(document.activeElement), n = items.length;
    const to = {ArrowDown: i + 1, ArrowUp: i - 1, Home: 0, End: n - 1, PageDown: i + 8, PageUp: i - 8}[e.key];
    if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); close(true); }
    else if (e.key === "Tab") close(false);
    else if (typeof to === "number") { e.preventDefault(); items[Math.max(0, Math.min(n - 1, to))].focus(); }
  });
  document.addEventListener("pointerdown", e => { if (!node.contains(e.target)) close(false); });
  updaters.push(() => {
    face.textContent = names[L().font];
    face.style.fontFamily = SheetCore.FONTS[L().font];
    for (const item of items) item.setAttribute("aria-selected", item._font === L().font);
  });
  return node;
}
const pageStep = (key, x) => stepper(() => S.page[key], v => { S.page[key] = v; }, x);
const isImage = () => L().ground === 1, imageShown = () => isImage() && !!image;
const lookRows = [
  row("Size", pageStep("fontSize", {min: 6, max: 24, unit: "pt"})),
  row("Line height", pageStep("lineHeight", {min: 100, max: 250, step: 5, unit: "%"})),
  row("Line breaks", oSeg("breaks", [[0, "Bars"], [3, "Phrases"], [1, "Beats"], [2, "None"]]), {stack: true, group: true}),
  row("Beats", oStep("beats", {min: 1, max: 32}), {sub: true, when: () => o().breaks === 1}),
  row("Font", fontMenu(), {stack: true, group: true}),
];
// The colours. The text's colour shows only in chords of one colour; by
// rhythm, every chord takes its rhythm's.
// The colour card has the background's and the chords' rows too.
const groundSeg = x => lSeg("ground", [[0, "Colour", {icon: "bucket"}], [1, "Image", {icon: "image"}]], x);
const chordSeg = x => lSeg("oneColour", [[false, "By rhythm"], [true, "One colour"]], x);
const fitSeg = () => lSeg("fit", [[0, "Cover"], [1, "Tile"]], {inline: true});
const dimStep = () => lStep("dim", {min: 0, max: 90, step: 5, unit: "%"});
const colourRows = [
  row("Theme", themeRow(), {stack: true}),
  row("Background", groundSeg(), {stack: true, group: true}),
  cells([imageCellOf()], {wide: true, when: isImage}),
  row("Fit", fitSeg(), {sub: true, when: imageShown}),
  row("Dim", dimStep(), {sub: true, when: imageShown}),
  row("Chord colours", chordSeg(), {stack: true, group: true}),
  legend(),
];
const isChordKey = () => /^(r[0-8]|text|chords)$/.test(editing || ""), isBackground = () => editing === "background";
const shownWhen = (node, when) => { updaters.push(() => { node.hidden = !when(); }); return node; };
cardRows.append(
  shownWhen(chordSeg({label: "Chord colours", after: v => retarget(v ? "text" : chordKey)}), isChordKey),
  shownWhen(groundSeg({label: "Background"}), isBackground),
  cells([imageCellOf()], {wide: true, when: () => isBackground() && isImage()}),
  row("Fit", fitSeg(), {sub: true, when: () => isBackground() && imageShown()}),
  row("Dim", dimStep(), {sub: true, when: () => isBackground() && imageShown()}),
);
updaters.push(() => { cardRows.hidden = ![...cardRows.children].some(node => !node.hidden); });
const notationRows = [
  row("Quantize", oStep("quantizeMs", {min: 0, max: 200, step: 5, unit: "ms"})),
  oSwitch("Spread as played", "sequentialQuantize", {sub: true}),
  oSwitch("Spread in braces", "curlyQuantizes", {sub: true}),
  oSwitch("Classic order", "classicChordOrder", {group: true}),
  row("Shifted keys", oSeg("shifts", [[0, "First"], [1, "Last"], [2, "By pitch"]]), {stack: true}),
  oSwitch("Rhythm separators", "tempoMarks", {group: true}),
  oSwitch("Tempo changes", "bpmChanges", {group: true}),
  row("Style", oSeg("bpmStyle", [[0, "Detailed"], [1, "Arrows"]], {inline: true}), {sub: true, when: () => o().bpmChanges}),
  row("Minimum change", oStep("minSpeedChange", {min: 0, max: 100, step: 5, unit: "%"}), {sub: true, when: () => o().bpmChanges}),
  row("Default tempo", oStep("missingBpm", {min: 20, max: 400, whole: false, unit: "BPM"})),
  oSwitch("Out of range", "showOutOfRange", {group: true}),
  row("Placement", oSeg("outOfRangePlace", [[0, "First"], [1, "Last"], [2, "By pitch"]]), {stack: true, sub: true, when: () => o().showOutOfRange}),
  oSwitch("Marks", "outOfRangeMarks", {sub: true, when: () => o().showOutOfRange}),
  textRow("Mark", () => o().outOfRangeSeparator, v => { o().outOfRangeSeparator = v; },
          {sub: true, center: true, maxlength: 7, spaces: true, when: () => o().showOutOfRange && o().outOfRangeMarks}),
];
$("panel").append(fold("sheet", "Sheet", "sheet", sheetRows), fold("header", "Header", "heading", headerRows), fold("look", "Text", "type", lookRows),
                  fold("colours", "Colours", "palette", colourRows), fold("notation", "Notation", "music", notationRows), picker);
// The header on the paper opens its settings at the title.
const paperHeading = $("heading");
paperHeading.tabIndex = 0;
paperHeading.setAttribute("role", "button");
paperHeading.setAttribute("aria-label", "Header");
const editHeader = () => openFold("header", titleRow._input);
paperHeading.addEventListener("click", () => { const s = getSelection(); if (!s || s.isCollapsed) editHeader(); });
paperHeading.addEventListener("keydown", e => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); editHeader(); } });
)js",
R"js(
// Presets: the two the page knows and the user's own, by name.
const canon = value => Array.isArray(value) ? "[" + value.map(canon).join(",") + "]" :
  value && typeof value === "object" ? "{" + Object.keys(value).sort().map(k => JSON.stringify(k) + ":" + canon(value[k])).join(",") + "}" : JSON.stringify(value);
const BUILT_IN = [["Default", () => full({})], ["Compact", () => full({options: {breaks: 0}, page: {fontSize: 9, lineHeight: 115}})]];
let presets = readPresets();
const presetBox = $("presets");
// The arranger is the user's name, not a style: presets and Reset keep it.
const takeStyle = s => { keepSlot(); s.header.arrangerName = S.header.arrangerName; S.options = s.options; S.page = s.page; S.look = s.look; S.header = s.header; };
const presetKey = style => { const s = full(style); delete s.header.arrangerName; return canon(s); };
// The built-in presets keep the look's colours and background.
const KEEP = ["theme", "ground", "fit", "dim", "oneColour", "background", "text", "comment", "heading", "pedalDown", "pedalUp", "rhythm"];
function withLook(style) {
  const s = full(copy(style));
  for (const k of KEEP) s.look[k] = copy(L()[k]);
  return s;
}
// A preset kept from when the title had a switch, with it off, leaves it off.
function usePreset(style, builtIn) {
  change(() => { if (style.header && style.header.title === false) words.title = ""; takeStyle(builtIn ? withLook(style) : full(copy(style))); }, true);
}
// A pill per preset, on while the style is the preset's; the user's own can
// be removed. Save preset names the style as it is.
let drawnPresets = "";
function presetPill(name, style, builtIn) {
  const button = pill(name, null, () => usePreset(style, builtIn)), key = builtIn ? "" : presetKey(style);
  button._key = () => key || presetKey(withLook(style));
  return button;
}
function addButton() {
  const add = make("button", {type: "button", id: "add-preset"}, [icon("plus"), "Save preset"]);
  add.onclick = nameStyle;
  return add;
}
function renderPresets() {
  drawnPresets = JSON.stringify(presets);
  presetBox.replaceChildren(...BUILT_IN.map(([name, style]) => presetPill(name, style(), true)),
    ...presets.map((p, i) => make("span", {class: "split"}, [presetPill(p.name, p.style),
      iconButton("Remove " + p.name, "x", () => change(() => { presets.splice(i, 1); }, true), "x")])), addButton());
  markPresets();
}
function markPresets() {
  const current = presetKey(styleOf());
  for (const button of presetBox.querySelectorAll(".pill")) button.setAttribute("aria-pressed", button._key() === current);
}
// Enter or Save keeps the name; Escape, leaving the field or no name keeps
// nothing.
function nameStyle() {
  const input = make("input", {type: "text", class: "field", id: "preset-name", maxlength: 40, autocomplete: "off", spellcheck: "false", "aria-label": "Preset name"});
  input.value = "Preset " + (presets.length + 1);
  const save = make("button", {type: "button"}, [icon("check"), "Save"]);
  let open = true;
  const done = (keep, refocus) => {
    if (!open) return;
    open = false;
    const name = input.value.trim();
    input.replaceWith(addButton());
    save.remove();
    if (keep && name) change(() => { presets = presets.filter(p => p.name !== name).concat([{name, style: copy(styleOf())}]); }, true);
    if (refocus) $("add-preset").focus();
  };
  input.addEventListener("keydown", e => {
    if (e.key === "Enter") { e.preventDefault(); done(true, true); }
    else if (e.key === "Escape") { e.stopPropagation(); done(false, true); }
  });
  input.addEventListener("blur", e => { if (e.relatedTarget !== save) done(false, false); });
  save.onclick = () => done(true, true);
  $("add-preset").replaceWith(input, save);
  input.focus();
  input.select();
}
renderPresets();
// Undo, Redo, and Reset to what the page came with.
$("undo").replaceChildren(icon("undo"));
$("redo").replaceChildren(icon("redo"));
$("reset").prepend(icon("reset"));
$("print").replaceChildren(icon("printer"));
$("selection-close").prepend(icon("check"));
$("undo").onclick = () => undo();
$("redo").onclick = () => redo();
$("reset").onclick = () => change(() => {
  takeStyle(own());
  image = ownImage();
  regions = data.regions.map(unpackRegion);
  words = ownWords();
}, true);
// Ctrl+Z undoes and Ctrl+Y or Ctrl+Shift+Z redoes, except in a field, which
// has its own.
document.addEventListener("keydown", e => {
  if (!(e.ctrlKey || e.metaKey) || e.altKey || (e.target.closest && e.target.closest("input,textarea"))) return;
  const key = e.key.toLowerCase();
  if (key === "z" && !e.shiftKey) { e.preventDefault(); undo(); }
  else if (key === "y" || key === "z") { e.preventDefault(); redo(); }
});
// On a phone one view shows at a time: the sheet, or its settings.
const viewSwitch = segment(() => document.body.dataset.view, showView, [["sheet", "Sheet", {icon: "sheet"}], ["settings", "Settings", {icon: "settings"}]], {label: "View", direct: true});
const markView = updaters[updaters.length - 1];
$("view").append(viewSwitch);
function showView(v) { document.body.dataset.view = v; writeStore(VIEW, v); markView(); }
)js",
R"js(
// A look's background as CSS, the current one's unless given, for the page
// and its picture.
function backgroundCss(look) {
  look = look || L();
  const css = {"background-color": hex(look.background)};
  if (look.ground === 1 && image) {
    const shade = "rgba(" + rgbOf(look.background).join(",") + "," + look.dim / 100 + ")";
    css["background-image"] = "linear-gradient(" + shade + "," + shade + "),url(" + image + ")";
    Object.assign(css, look.fit === 1 ? {"background-repeat": "repeat"} : {"background-size": "cover", "background-position": "center", "background-repeat": "no-repeat"});
  }
  return css;
}
const cssText = css => Object.entries(css).map(([k, v]) => k + ":" + v).join(";");
let drawnBackground = "";
function applyLook(look) {
  look = look || L();
  const page = $("page"), background = cssText(backgroundCss(look));
  if (background !== drawnBackground) { page.setAttribute("style", background); drawnBackground = background; }
  page.style.setProperty("--comment", look.comment);
  page.style.color = look.text;
  document.body.style.setProperty("--sheet-bg", hex(look.background));
  for (const id of ["sheet", "heading"]) $(id).style.fontFamily = SheetCore.FONTS[look.font];
  $("sheet").style.color = look.text;
  $("heading").style.color = look.heading;
}
// The sheet in a look and from a result, the current ones unless a preview
// or a peek gives its own. Each chord carries its rhythm (data-r) and each
// pedal band whether the pedal is down (data-p d) or up (u), as toHtml
// painted them.
function render(look, r) {
  look = look || L();
  r = r || result;
  const sheet = $("sheet");
  sheet.innerHTML = SheetCore.toHtml(r, look);
  sheet.style.fontSize = S.page.fontSize + "pt";
  sheet.style.lineHeight = S.page.lineHeight + "%";
  applyLook(look);
  for (const span of sheet.querySelectorAll(".chord")) {
    const it = r.items[+span.dataset.i];
    span.classList.toggle("in-section", regions.some(x => it.ms / 1000 >= x.from && it.msEnd / 1000 <= x.to));
    span.dataset.r = it.rhythm;
    for (const band of span.querySelectorAll(":scope>span[style]"))
      band.dataset.p = (band.previousSibling ? (it.pedalHeld ? it.pedal : 0) : (it.pedal || 0)) > 0 ? "d" : "u";
  }
  markPicked();
  renderHooks.forEach(hook => hook(look, r));
}
// A look shown on the sheet alone: nothing is kept, undone or updated.
// showLook() shows the current one again.
function showLook(look) { render(look ? Object.assign({}, L(), look) : L()); }
// The result with only these pedal marks: 1 down, 2 up.
function peekPedals(marks) {
  const r = Object.assign({}, result, {items: result.items.map(it => Object.assign({}, it))});
  SheetCore.markPedals(r, pedals, Object.assign({}, S.options, {pedalMarks: marks}));
  return r;
}
function draw() {
  played = SheetCore.heldByPedal(SheetCore.applyRegions(notes, regions), pedals);
  result = SheetCore.style(played, data.mapping, tempos, meters, S.options, SheetCore.transposeSections(regions));
  SheetCore.markPedals(result, pedals, S.options);
  render();
  // The sheet is new under the selection that named the part: the part's
  // marks show it now.
  const selection = getSelection();
  if (pending && selection && selection.rangeCount && $("page").contains(selection.anchorNode)) selection.removeAllRanges();
  const count = [plural(result.notes, "note", "notes"), plural(result.groups, "chord", "chords")];
  if (result.merged) count.push(result.merged + " merged");
  if (result.unmapped) count.push(result.unmapped + " unmapped");
  if (result.hidden) count.push(result.hidden + " out of range");
  if (result.difficulty) count.push("Difficulty " + result.difficulty + " of 10");
  $("count").textContent = count.join(" · ");
  const title = songText().title.trim() || data.title || "";
  $("title").textContent = title;
  document.title = title;
  const heading = $("heading");
  heading.textContent = headingLines().join("\n");
  heading.style.fontSize = S.page.fontSize + "pt";
  heading.style.textAlign = S.header.centre ? "center" : "";
  drawRuns();
  updaters.forEach(update => update());
  $("undo").disabled = !history.length;
  $("redo").disabled = !future.length;
  $("reset").disabled = atStart();
  if (JSON.stringify(presets) !== drawnPresets) renderPresets(); else markPresets();
  placeToolbar();
  drawHooks.forEach(hook => hook());
}
// Sections: every run of one transposition when there is more than one,
// then the parts the user transposed or shifted. A transposition is the
// sheet's number; a shift is the notes' own. A run of the user's can be
// removed; Keep sections makes the found runs the user's.
let shownRuns = [], drawnRuns = "";
function drawRuns() {
  const runs = [], many = result.sections.length > 1;
  const say = semitones => "Transpose by " + show(-semitones, true);
  if (many) for (const s of result.sections)
    runs.push({from: s.from, to: s.to, say: say(s.semitones), remove: regions.find(r => r.kind === SheetCore.TRANSPOSE && sameRun(r, s))});
  for (const r of regions) {
    if (r.kind !== SheetCore.TRANSPOSE) runs.push({from: r.from, to: r.to, say: "Shift notes " + show(r.semitones, true), remove: r});
    else if (!many || !result.sections.some(s => sameRun(r, s))) runs.push({from: r.from, to: r.to, say: say(r.semitones), remove: r});
  }
  runs.sort((a, b) => a.from - b.from || a.to - b.to);
  runCount = runs.length;
  shownRuns = runs;
  // The rows stay while they read the same, so a press on one is not lost
  // to a draw.
  const key = JSON.stringify(runs.map(run => [run.from, run.to, run.say, !!run.remove]));
  if (key === drawnRuns) return;
  drawnRuns = key;
  sectionList.replaceChildren(...runs.map((run, i) => {
    const button = make("button", {type: "button", class: "run", "data-run": run.from + "/" + run.to},
                        [make("span", {text: time(run.from) + " – " + time(run.to)}), make("span", {class: "result", text: run.say})]);
    button.onclick = () => { showView("sheet"); openPart(shownRuns[i], button); };
    const remove = run.remove ? iconButton("Remove", "x", () => change(() => { const gone = shownRuns[i].remove; regions = regions.filter(r => r !== gone); }, true), "x flat") : make("span");
    return make("li", {}, [button, remove]);
  }));
}
// The kept parts hold every chord; Fixed starts from the first, as the sheet does.
keepButton.onclick = () => change(() => {
  regions = regions.filter(r => r.kind !== SheetCore.TRANSPOSE)
    .concat(result.sections.map(s => ({from: s.from, to: s.to, semitones: s.semitones, kind: SheetCore.TRANSPOSE})));
  S.options.transpose = result.transposition;
  S.options.autoTranspose = S.options.autoSections = false;
}, true);
)js",
R"js(
// The part card: a part of the sheet, named by a selection over it, a row
// of Sections or Transpose a part, with its transposition and its shift.
const toolbar = $("selection");
let pending = null, partOpener = null;
const phone = () => matchMedia("(max-width:720px)").matches;
// A selection over the sheet names a part: from the first chord's onset to
// the last chord's last note, so every note in the chords selected moves.
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
// The part's chords are marked. The card sits above the first of them, or
// below the last when there is no room; on a phone it keeps to the bottom.
const inPart = it => !!pending && it.ms / 1000 >= pending.from && it.msEnd / 1000 <= pending.to;
function markPicked() {
  for (const span of $("sheet").querySelectorAll(".chord")) span.classList.toggle("picked", inPart(result.items[+span.dataset.i]));
}
function placeToolbar() {
  if (toolbar.hidden) return;
  if (phone()) { toolbar.style.left = toolbar.style.top = ""; return; }
  const picked = $("sheet").querySelectorAll(".picked");
  if (!picked.length) return;
  const first = picked[0].getBoundingClientRect(), last = picked[picked.length - 1].getBoundingClientRect();
  const width = toolbar.offsetWidth, height = toolbar.offsetHeight, bar = document.querySelector("header").getBoundingClientRect().bottom;
  let top = first.top - height - 8;
  if (top < bar + 8) top = last.bottom + 8;
  toolbar.style.left = Math.max(8, Math.min(innerWidth - width - 8, first.left)) + "px";
  toolbar.style.top = Math.max(8, Math.min(innerHeight - height - 8, top)) + "px";
}
// Transpose by is the sheet's number over the part: the part's own, or the
// one in force where it starts. Shift notes moves its notes, by their own sign.
const partRegion = () => regions.find(r => r.kind === SheetCore.TRANSPOSE && sameRun(r, pending));
const partShift = () => regions.find(r => r.kind !== SheetCore.TRANSPOSE && sameRun(r, pending));
const cardStart = updaters.length;
const partTranspose = stepper(() => { if (!pending) return 0; const r = partRegion(); return -(r ? r.semitones : shiftAt(pending.from)); }, v => {
  if (!pending) return;
  const r = partRegion(), semitones = v ? -v : 0;
  if (r) r.semitones = semitones; else regions.push({from: pending.from, to: pending.to, semitones, kind: SheetCore.TRANSPOSE});
}, {min: -24, max: 24, signed: true, unit: SEMITONES});
const partNotes = stepper(() => { const r = pending && partShift(); return r ? r.semitones : 0; }, v => {
  if (!pending) return;
  const r = partShift();
  if (r) r.semitones = v; else regions.push({from: pending.from, to: pending.to, semitones: v, kind: SheetCore.NOTES_SHIFTED});
  regions = regions.filter(r => r.kind === SheetCore.TRANSPOSE || r.semitones !== 0);
}, {min: -24, max: 24, signed: true, unit: SEMITONES});
$("selection-rows").append(row("Transpose by", partTranspose), row("Shift", partNotes));
const cardUpdaters = updaters.slice(cardStart);
// Opened by a button, the card takes the cursor; closed, it gives it back.
function openPart(region, opener) {
  closeColour(false);
  pending = {from: region.from, to: region.to};
  partOpener = opener || null;
  toolbar.hidden = false;
  $("selection-range").textContent = time(pending.from) + " – " + time(pending.to);
  document.body.classList.add("part-open");
  markPicked();
  cardUpdaters.forEach(update => update());
  placeToolbar();
  // A button brings the part into view; a selection is where the user is.
  const first = opener && $("sheet").querySelector(".picked");
  if (first) {
    const rect = first.getBoundingClientRect(), top = phone() ? $("view").getBoundingClientRect().bottom : document.querySelector("main").getBoundingClientRect().top;
    if (rect.top < top || rect.bottom > innerHeight) { first.scrollIntoView({block: "center"}); placeToolbar(); }
  }
  if (opener) partTranspose._input.focus({preventScroll: true});
}
function closePart(returnFocus) {
  const opener = partOpener;
  toolbar.hidden = true;
  pending = partOpener = null;
  markPicked();
  const selection = getSelection();
  if (selection) selection.removeAllRanges();
  document.body.classList.remove("part-open");
  if (!returnFocus) return;
  // A row of Sections is drawn again on every change: back to its new self.
  // On a phone the opener sits in Settings, so the view switch takes it.
  let to = opener && (opener.isConnected ? opener : opener.dataset.run && sectionList.querySelector('[data-run="' + opener.dataset.run + '"]'));
  if (!to || !to.getClientRects().length) to = pickButton.getClientRects().length ? pickButton : $("view").querySelector("[aria-checked=true]");
  if (to) to.focus();
}
document.addEventListener("selectionchange", () => {
  const region = selectionRegion();
  if (region) { if (!pending || !sameRun(region, pending)) openPart(region); return; }
  const selection = getSelection();
  if (!toolbar.hidden && selection && selection.anchorNode && $("sheet").contains(selection.anchorNode) && !toolbar.contains(document.activeElement)) closePart(false);
});
document.querySelector("main").addEventListener("scroll", placeToolbar);
window.addEventListener("scroll", placeToolbar);
window.addEventListener("resize", placeToolbar);
// Escape closes the card once a field has had its own Escape.
document.addEventListener("keydown", e => { if (e.key === "Escape" && !toolbar.hidden) closePart(true); });
$("selection-close").onclick = () => closePart(true);
// Transpose a part takes the first line in view, or the first line.
pickButton.onclick = () => {
  showView("sheet");
  const spans = $("sheet").querySelectorAll(".chord");
  if (!spans.length) return;
  const top = phone() ? $("view").getBoundingClientRect().bottom : document.querySelector("main").getBoundingClientRect().top;
  let first = -1;
  for (let i = 0; i < spans.length && first < 0; ++i) { const rect = spans[i].getBoundingClientRect(); if (rect.top >= top && rect.bottom <= innerHeight) first = i; }
  if (first < 0) { first = 0; spans[0].scrollIntoView({block: "center"}); }
  const lineTop = spans[first].getBoundingClientRect().top;
  let last = first;
  while (last + 1 < spans.length && spans[last + 1].getBoundingClientRect().top < lineTop + 2) ++last;
  let from = Infinity, to = -Infinity;
  for (let i = first; i <= last; ++i) { const it = result.items[+spans[i].dataset.i]; from = Math.min(from, it.ms / 1000); to = Math.max(to, it.msEnd / 1000); }
  openPart({from, to}, pickButton);
};
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
// The output buttons say how it went for a moment, in their own place.
const sayCopy = sayButton($("copy"), ["Copy sheet", "Copied", "Copy failed"], "copy");
const sayImage = sayButton($("image"), ["Save image", "Saved", "Save failed"], "imageDown");
const saySave = sayButton($("save"), ["Save page", "Saved", "Save failed"], "fileDown");
$("copy").onclick = () => copyText(sheetText()).then(ok => sayCopy(ok ? 1 : 2));
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
// A save that was cancelled says nothing.
function report(say, promise) {
  promise.then(ok => { if (ok) say(1); }, e => { say(2); console.error(e); });
}
$("save").onclick = () => {
  closePart(false);
  closeColour(false);
  spot(null);
  const saved = Object.assign({}, data, {saved: true, options: S.options, page: S.page, look: Object.assign({}, S.look, L().ground === 1 && image ? {image} : {}),
                                         header: S.header, song: songText(), regions: regions.map(r => [r.from, r.to, r.semitones, r.kind]), expected: result.text});
  delete saved.styled;
  const element = $("sheet-data");
  const before = element.textContent;
  element.textContent = JSON.stringify(saved).replace(/<\//g, "<\\/");
  // The background is drawn again from the data; the image goes in once.
  const pageStyle = $("page").getAttribute("style");
  $("page").removeAttribute("style");
  const html = "<!doctype html>\n" + document.documentElement.outerHTML;
  $("page").setAttribute("style", pageStyle || "");
  element.textContent = before;
  report(saySave, saveBlob(new Blob([html], {type: "text/html"}), (data.title || "Sheet") + ".html", "Sheet page", {"text/html": [".html"]}));
};
// The sheet as a picture: the sheet's own markup, drawn by the browser
// inside an SVG image and copied to a canvas at twice the size so the text
// stays sharp. The heading goes on top, as Print shows it.
function sheetImage() {
  const sheet = $("sheet"), shown = document.body.dataset, view = shown.view, y = scrollY;
  // On a phone in Settings the sheet is out of view: it is measured as shown.
  shown.view = "sheet";
  const width = Math.ceil(sheet.getBoundingClientRect().width);
  shown.view = view;
  if (scrollY !== y) scrollTo(scrollX, y);
  const look = L();
  const css = "font-family:" + SheetCore.FONTS[look.font] + ";font-size:" + S.page.fontSize + "pt;line-height:" + S.page.lineHeight + "%;color:" + look.text + ";" +
    "white-space:pre-wrap;" + cssText(backgroundCss()) + ";padding:40px 48px;box-sizing:border-box;width:" + (width + 96) + "px;margin:0";
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
  const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="' + (width + 96) + '" height="' + height + '">' +
    '<foreignObject width="100%" height="100%">' + html + "</foreignObject></svg>";
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => {
      // Twice the size, or as large as the browser's canvas allows for a
      // long sheet: 32767 pixels a side and 268 million in all.
      const scale = Math.min(2, 32767 / height, 32767 / (width + 96), Math.sqrt(268000000 / ((width + 96) * height)));
      const canvas = document.createElement("canvas");
      canvas.width = Math.floor((width + 96) * scale);
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
  closePart(false);
  closeColour(false);
  spot(null);
  report(sayImage, sheetImage().then(blob => saveBlob(blob, (data.title || "Sheet") + ".png", "Sheet image", {"image/png": [".png"]})));
};
$("print").onclick = () => { closeColour(false); spot(null); window.print(); };
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
    $("parity").append(iconButton("Close", "x", () => $("parity").classList.remove("shown"), "x flat"));
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
        "<header><div class=\"titles\"><h1 id=\"title\">" + detail::EscapeHtml(in.title) + "</h1><span class=\"count\" id=\"count\"></span></div>"
        "<div class=\"tools\" id=\"history\"><button type=\"button\" id=\"undo\" class=\"icon\" aria-label=\"Undo\"></button>"
        "<button type=\"button\" id=\"redo\" class=\"icon\" aria-label=\"Redo\"></button><button type=\"button\" id=\"reset\">Reset</button></div>"
        "<div class=\"tools\" id=\"outputs\"><button type=\"button\" id=\"image\">Save image</button><button type=\"button\" id=\"save\">Save page</button>"
        "<button type=\"button\" id=\"print\" class=\"icon\" aria-label=\"Print\"></button><button type=\"button\" id=\"copy\" class=\"primary\">Copy sheet</button></div></header>\n"
        "<nav id=\"view\" aria-label=\"View\"></nav>\n"
        "<div id=\"layout\"><aside><div id=\"presets\"></div><div id=\"panel\"></div></aside>\n"
        "<main><p id=\"parity\">This sheet differs from the app's.</p>"
        "<div id=\"page\"><div id=\"heading\"></div><div id=\"sheet\">";
    // Pre-rendered so the sheet shows before the script runs.
    html += detail::SheetBody(initial, in.look);
    html += "</div></div></main></div>\n"
        "<div id=\"selection\" role=\"toolbar\" aria-label=\"Part\" hidden><div class=\"card-top\"><span id=\"selection-range\"></span>"
        "<button type=\"button\" id=\"selection-close\">Done</button></div><div id=\"selection-rows\"></div></div>\n"
        "<div id=\"colour\" role=\"dialog\" aria-labelledby=\"colour-name\" hidden></div>\n"
        "<script id=\"sheet-data\" type=\"application/json\">" + detail::PageJson(in, initial.text) + "</script>\n"
        "<script>" + detail::kPageCore;
    for (const char* part : detail::kPageUi) html += part;
    html += "</script>\n</body></html>\n";
    return html;
}

} // namespace sheet
