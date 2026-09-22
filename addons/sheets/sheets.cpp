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

// Parsed from the page's <script id="sheet-data"> JSON: "options" and "page".
struct SheetStyle { sheet::StyleOptions options; sheet::Look look; };

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
    SheetStyle style;
    auto& s = style.options;
    const auto o = data.value("options", Json::object());
    if (!o.is_object()) return style;
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
    s.breaks = static_cast<sheet::Breaks>(std::clamp(o.value("breaks", static_cast<int>(s.breaks)), 0, 2));
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
    const auto p = data.value("page", Json::object());
    if (p.is_object()) {
        style.look.fontSizePt = std::clamp(p.value("fontSize", style.look.fontSizePt), 4.0, 48.0);
        style.look.lineHeightPercent = std::clamp(p.value("lineHeight", style.look.lineHeightPercent), 80.0, 400.0);
    }
    return style;
}

// Notes arrive in seconds ("notes") or in ticks ("ticks") when the file was
// never loaded by the player; tempo and meter changes are always in ticks.
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
    for (const auto& note : json.value("notes", Json::array())) page.notes.push_back({note.at(0).get<double>(), note.at(1).get<int>()});
    for (const auto& note : json.value("ticks", Json::array()))
        page.notes.push_back({sheet::SecondsAtTick(note.at(0).get<uint64_t>(), tempos, division), note.at(1).get<int>()});
    page.tempos = sheet::TempoMarksFromTicks(tempos, division);
    page.meters = sheet::MeterMarksFromTicks(meters, tempos, division);
    return page;
}

template <class Result> Json Counts(const Result& result) {
    return {{"text", result.text}, {"notes", result.notes}, {"groups", result.groups}, {"merged", result.merged}, {"unmapped", result.unmapped}};
}

Json Answer(const Json& request) {
    const auto what = request.value("do", "");
    if (what == "text") {
        std::vector<sheet::Note> notes;
        for (const auto& note : request.value("notes", Json::array())) notes.push_back({note.at(0).get<double>(), note.at(1).get<std::string>()});
        sheet::Options options;
        options.beatSeconds = request.value("beatSeconds", options.beatSeconds);
        return Counts(sheet::ToVirtualPiano(std::move(notes), request.value("mapping", std::map<std::string, std::string>()), options));
    }
    if (what == "page") {
        sheet::StyledResult result;
        const auto html = sheet::ToEditorHtml(PageFrom(request.at("page")), &result);
        auto reply = Counts(result);
        reply["html"] = html;
        return reply;
    }
    if (what == "style") { StyleFromPage(PathOf(request.value("style", ""))); return Json::object(); }
    if (what == "files") {
        // Writes <stem>.html/.txt/.png; the page is rendered once and supplies the text.
        auto page = PageFrom(request.at("page"));
        const auto stylePage = request.value("style", "");
        if (!stylePage.empty()) { const auto style = StyleFromPage(PathOf(stylePage)); page.options = style.options; page.look = style.look; }
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
        if (outputs.value("text", true)) write(L".txt", result.text);
        if (outputs.value("image", true)) { auto path = stem; path += L".png"; sheet::SavePng(result, page.title, page.look, path); wrote.push_back(Utf8(path)); }
        auto reply = Counts(result);
        reply["wrote"] = std::move(wrote);
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
