// Sheets add-on: builds sheet text, the editor page and sheet files from a score.
// Single entry point qm_sheets, UTF-8 JSON request and reply; see Answer for the
// request kinds. Style options are read from a page saved by the sheet editor.
#include "SheetPage.hpp"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
using Json = nlohmann::json;

std::filesystem::path PathOf(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// Parsed from the page's <script id="sheet-data"> JSON: "options", "page" and "look".
struct SheetStyle { sheet::StyleOptions options; sheet::Look look; sheet::Header header; };

SheetStyle StyleFromPage(const std::filesystem::path& page) {
    std::ifstream file(page, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read the style page " + Utf8(page) + ".");
    const std::string html((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string open = "<script id=\"sheet-data\" type=\"application/json\">";
    const auto start = html.find(open);
    const auto end = start == std::string::npos ? start : html.find("</script>", start);
    if (end == std::string::npos) throw std::runtime_error(Utf8(page.filename()) + " is not a page saved from the sheet editor.");
    Json data;
    try { data = Json::parse(html.substr(start + open.size(), end - start - open.size())); }
    catch (const std::exception&) { throw std::runtime_error(Utf8(page.filename()) + " is not a page saved from the sheet editor."); }
    if (!data.is_object()) throw std::runtime_error(Utf8(page.filename()) + " is not a page saved from the sheet editor.");
    SheetStyle style;
    auto& s = style.options;
    auto o = data.value("options", Json::object());
    if (!o.is_object()) o = Json::object();
    s.quantizeMs = o.value("quantizeMs", s.quantizeMs);
    s.sequentialQuantize = o.value("sequentialQuantize", s.sequentialQuantize);
    s.curlyQuantizes = o.value("curlyQuantizes", s.curlyQuantizes);
    s.classicChordOrder = o.value("classicChordOrder", s.classicChordOrder);
    s.shifts = static_cast<sheet::Place>(std::clamp(o.value("shifts", static_cast<int>(s.shifts)), 0, 2));
    s.outOfRangePlace = static_cast<sheet::Place>(std::clamp(o.value("outOfRangePlace", static_cast<int>(s.outOfRangePlace)), 0, 2));
    s.showOutOfRange = o.value("showOutOfRange", s.showOutOfRange);
    s.outOfRangeMarks = o.value("outOfRangeMarks", s.outOfRangeMarks);
    s.outOfRangeSeparator = o.value("outOfRangeSeparator", s.outOfRangeSeparator);
    s.tempoMarks = o.value("tempoMarks", s.tempoMarks);
    s.bpmChanges = o.value("bpmChanges", s.bpmChanges);
    s.bpmStyle = static_cast<sheet::BpmStyle>(std::clamp(o.value("bpmStyle", static_cast<int>(s.bpmStyle)), 0, 1));
    s.minSpeedChange = o.value("minSpeedChange", s.minSpeedChange);
    s.breaks = static_cast<sheet::Breaks>(std::clamp(o.value("breaks", static_cast<int>(s.breaks)), 0, 3));
    s.beats = o.value("beats", s.beats);
    s.missingBpm = o.value("missingBpm", s.missingBpm);
    s.transpose = o.value("transpose", s.transpose);
    s.autoTranspose = o.value("autoTranspose", s.autoTranspose);
    s.resilience = o.value("resilience", s.resilience);
    s.autoSections = o.value("autoSections", s.autoSections);
    s.sectionSwitchCost = o.value("sectionSwitchCost", s.sectionSwitchCost);
    s.sectionMinSeconds = o.value("sectionMinSeconds", s.sectionMinSeconds);
    s.sectionRestMs = o.value("sectionRestMs", s.sectionRestMs);
    s.sectionRange = o.value("sectionRange", s.sectionRange);
    // A page saved the day pedal marks were a switch holds true or false.
    if (const auto marks = o.find("pedalMarks"); marks != o.end() && marks->is_boolean())
        s.pedalMarks = marks->get<bool>() ? sheet::PedalMarks::Normal : sheet::PedalMarks::Off;
    else
        s.pedalMarks = static_cast<sheet::PedalMarks>(std::clamp(o.value("pedalMarks", static_cast<int>(s.pedalMarks)), 0, 3));
    const auto p = data.value("page", Json::object());
    if (p.is_object()) {
        style.look.fontSizePt = std::clamp(p.value("fontSize", style.look.fontSizePt), 4.0, 48.0);
        style.look.lineHeightPercent = std::clamp(p.value("lineHeight", style.look.lineHeightPercent), 80.0, 400.0);
    }
    // Each part of the look on its own: one of the wrong kind keeps its default.
    const auto l = data.value("look", Json::object());
    if (l.is_object()) {
        auto& look = style.look;
        const auto number = [&](const char* key, int fallback, int low, int high) {
            const auto it = l.find(key);
            return it != l.end() && it->is_number() ? std::clamp(static_cast<int>(it->get<double>()), low, high) : fallback;
        };
        const auto colour = [&](const char* key, std::string& into) {
            const auto it = l.find(key);
            if (it != l.end() && it->is_string() && sheet::IsLookColour(it->get<std::string>())) into = it->get<std::string>();
        };
        look.theme = number("theme", look.theme, 0, 3);
        look.ground = static_cast<sheet::Look::Ground>(number("ground", look.ground, 0, 2));
        look.fit = static_cast<sheet::Look::Fit>(number("fit", look.fit, 0, 1));
        look.dim = number("dim", look.dim, 0, 100);
        look.grain = number("grain", look.grain, 0, 100);
        look.font = static_cast<sheet::Look::Font>(number("font", look.font, 0, 2));
        if (const auto it = l.find("oneColour"); it != l.end() && it->is_boolean()) look.oneColour = it->get<bool>();
        colour("background", look.background);
        colour("text", look.text);
        colour("comment", look.comment);
        colour("heading", look.heading);
        colour("pedalDown", look.pedalDown);
        colour("pedalUp", look.pedalUp);
        if (const auto it = l.find("rhythm"); it != l.end() && it->is_array())
            for (size_t i = 0; i < look.rhythm.size() && i < it->size(); ++i)
                if ((*it)[i].is_string() && sheet::IsLookColour((*it)[i].get<std::string>())) look.rhythm[i] = (*it)[i].get<std::string>();
        if (const auto it = l.find("image"); it != l.end() && it->is_string() && it->get<std::string>().rfind("data:image/", 0) == 0) look.image = it->get<std::string>();
    }
    // Which header lines show. The song's own text stays with its song.
    const auto h = data.value("header", Json::object());
    if (h.is_object()) {
        auto& header = style.header;
        const auto flag = [&](const char* key, bool& into) { if (const auto it = h.find(key); it != h.end() && it->is_boolean()) into = it->get<bool>(); };
        flag("title", header.title);
        flag("subtitle", header.subtitle);
        flag("artist", header.artist);
        flag("arranger", header.arranger);
        flag("tempo", header.tempo);
        flag("key", header.key);
        flag("difficulty", header.difficulty);
        flag("date", header.date);
        flag("centre", header.centre);
        if (const auto it = h.find("arrangerName"); it != h.end() && it->is_string()) header.arrangerName = it->get<std::string>();
    }
    return style;
}

void ApplyStyle(sheet::PageInput& page, const SheetStyle& style) {
    page.options = style.options;
    page.look = style.look;
    page.header = style.header;
}

// The sheet's text with the header the style asks for.
std::string TextOf(const sheet::PageInput& page, const sheet::StyledResult& result) {
    return sheet::SheetText(result, page.header, sheet::FileSongText(page), sheet::FactsOf(page), page.title);
}

// Notes arrive in seconds ("notes") or in ticks ("ticks") when the file was
// never loaded by the player, as [onset, note] or [onset, note, release];
// tempo and meter changes are always in ticks.
sheet::PageInput PageFrom(const Json& json) {
    sheet::PageInput page;
    page.title = json.value("title", "");
    page.mapping = json.value("mapping", std::map<std::string, std::string>());
    const auto division = json.value("division", uint16_t{480});
    std::vector<sheet::TickTempo> tempos;
    for (const auto& tempo : json.value("tempos", Json::array())) tempos.push_back({tempo.at(0).get<uint64_t>(), tempo.at(1).get<uint32_t>()});
    std::stable_sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    std::vector<sheet::TickMeter> meters;
    for (const auto& meter : json.value("meters", Json::array())) meters.push_back({meter.at(0).get<uint64_t>(), meter.at(1).get<int>()});
    for (const auto& note : json.value("notes", Json::array()))
        page.notes.push_back({note.at(0).get<double>(), note.at(1).get<int>(), note.size() > 2 ? note.at(2).get<double>() : -1.0});
    for (const auto& note : json.value("ticks", Json::array()))
        page.notes.push_back({sheet::SecondsAtTick(note.at(0).get<uint64_t>(), tempos, division), note.at(1).get<int>(),
                              note.size() > 2 ? sheet::SecondsAtTick(note.at(2).get<uint64_t>(), tempos, division) : -1.0});
    // Sustain pedal values as [time, value, track], like the notes.
    for (const auto& pedal : json.value("pedals", Json::array()))
        page.pedals.push_back({pedal.at(0).get<double>(), pedal.at(1).get<int>(), pedal.at(2).get<int>()});
    for (const auto& pedal : json.value("pedalTicks", Json::array()))
        page.pedals.push_back({sheet::SecondsAtTick(pedal.at(0).get<uint64_t>(), tempos, division), pedal.at(1).get<int>(), pedal.at(2).get<int>()});
    page.tempos = sheet::TempoMarksFromTicks(tempos, division);
    page.meters = sheet::MeterMarksFromTicks(meters, tempos, division);
    return page;
}

template <class Result> Json Counts(const Result& result, const std::string& text) {
    return {{"text", text}, {"notes", result.notes}, {"groups", result.groups}, {"merged", result.merged}, {"unmapped", result.unmapped}};
}

Json Answer(const Json& request) {
    const auto what = request.value("do", "");
    if (what == "text") {
        std::vector<sheet::Note> notes;
        for (const auto& note : request.value("notes", Json::array())) notes.push_back({note.at(0).get<double>(), note.at(1).get<std::string>()});
        sheet::Options options;
        options.beatSeconds = request.value("beatSeconds", options.beatSeconds);
        const auto result = sheet::ToVirtualPiano(std::move(notes), request.value("mapping", std::map<std::string, std::string>()), options);
        return Counts(result, result.text);
    }
    if (what == "page") {
        // Drawn with the style page the files are saved with, so the editor
        // opens on what Save sheet files writes.
        auto page = PageFrom(request.at("page"));
        const auto stylePage = request.value("style", "");
        if (!stylePage.empty()) { ApplyStyle(page, StyleFromPage(PathOf(stylePage))); page.styled = true; }
        sheet::StyledResult result;
        const auto html = sheet::ToEditorHtml(page, &result);
        auto reply = Counts(result, TextOf(page, result));
        reply["html"] = html;
        reply["difficulty"] = result.difficulty;
        return reply;
    }
    if (what == "style") { StyleFromPage(PathOf(request.value("style", ""))); return Json::object(); }
    if (what == "files") {
        // Writes <stem>.html/.txt/.png; the page is rendered once and supplies the text.
        auto page = PageFrom(request.at("page"));
        const auto stylePage = request.value("style", "");
        if (!stylePage.empty()) ApplyStyle(page, StyleFromPage(PathOf(stylePage)));
        const auto stem = PathOf(request.at("stem").get<std::string>());
        const auto outputs = request.value("outputs", Json::object());
        std::error_code ignored;
        std::filesystem::create_directories(stem.parent_path(), ignored);
        sheet::StyledResult result;
        const auto html = sheet::ToEditorHtml(page, &result);
        auto wrote = Json::array();
        const auto write = [&](const wchar_t* extension, const std::string& text) {
            auto path = stem; path += extension;
            std::ofstream output(path, std::ios::binary);
            output << text;
            output.flush();
            if (!output) throw std::runtime_error("Cannot write " + Utf8(path) + ".");
            wrote.push_back(Utf8(path));
        };
        if (outputs.value("page", true)) write(L".html", html);
        if (outputs.value("text", true)) write(L".txt", TextOf(page, result));
        if (outputs.value("image", true)) {
            auto path = stem; path += L".png";
            sheet::SavePng(result, {sheet::HeadingLines(result, page.header, sheet::FileSongText(page), sheet::FactsOf(page)), page.header.centre}, page.look, path);
            wrote.push_back(Utf8(path));
        }
        auto reply = Counts(result, TextOf(page, result));
        reply["wrote"] = std::move(wrote);
        reply["difficulty"] = result.difficulty;
        return reply;
    }
    throw std::runtime_error("The sheets add-on does not know \"" + what + "\".");
}
}

extern "C" {
using qm_reply = void (*)(const char* json, void* user);
// Calls reply exactly once before returning, with the result or {"error": ...}.
// No exception crosses the DLL boundary.
__declspec(dllexport) void qm_sheets(const char* request, qm_reply reply, void* user) {
    std::string text;
    try { text = Answer(Json::parse(request)).dump(-1, ' ', false, Json::error_handler_t::replace); }
    catch (const std::exception& error) { text = Json{{"error", error.what()}}.dump(-1, ' ', false, Json::error_handler_t::replace); }
    catch (...) { text = "{\"error\":\"The sheets add-on failed.\"}"; }
    reply(text.c_str(), user);
}
}
