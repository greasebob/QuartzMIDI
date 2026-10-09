// Winsock 2 before anything brings in windows.h, which would pull in the old winsock.h.
#define NOMINMAX
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#include "ShellEngine.hpp"
#include "GameKeyMaps.hpp"
#include "../engine/AudioToMidi.hpp"
#include "PlaybackSystem.hpp"
#include "MIDI2Key.hpp"
#include "MidiOutput.hpp"
#include "MidiTiming.hpp"
#include "WootingAnalog.hpp"
#include "../engine/AddonHost.hpp"
#include "../engine/CrashGuard.hpp"
#include "AddonKey.hpp"
#include "Bundle.hpp"
#include "SavedFiles.hpp"
#include <shlobj.h>
#include <cfgmgr32.h>
#include <tlhelp32.h>
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#include <fstream>
#include <cmath>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <intrin.h>
#include <random>

namespace {
} // namespace

// Globals the engine sources expect their host to define.
VirtualPianoPlayer* g_player = nullptr;
std::atomic<int> g_sustainCutoff{64};

namespace shell {
namespace {
class NativeAutoVolumeHost final : public AutoVolumeHost {
    static bool Valid(const GameWindow& window) {
        const auto hwnd = reinterpret_cast<HWND>(window.id);
        DWORD process = 0;
        GetWindowThreadProcessId(hwnd, &process);
        wchar_t title[1024]{};
        GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        return window.id && process == window.process && process != GetCurrentProcessId() &&
            IsWindow(hwnd) && IsWindowVisible(hwnd) && Utf8(std::filesystem::path(title)) == window.title;
    }
public:
    std::vector<GameWindow> Windows() override {
        std::vector<GameWindow> result;
        EnumWindows([](HWND hwnd, LPARAM context) -> BOOL {
            DWORD process = 0;
            GetWindowThreadProcessId(hwnd, &process);
            if (!IsWindowVisible(hwnd) || process == GetCurrentProcessId() || GetWindow(hwnd, GW_OWNER)) return TRUE;
            wchar_t title[1024]{};
            if (GetWindowTextW(hwnd, title, static_cast<int>(std::size(title))) == 0) return TRUE;
            reinterpret_cast<std::vector<GameWindow>*>(context)->push_back(
                {reinterpret_cast<uintptr_t>(hwnd), process, Utf8(std::filesystem::path(title)), IsRobloxWindow(hwnd)});
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));
        OrderGameWindows(result);
        return result;
    }
    bool Focus(const GameWindow& window) override {
        if (!Valid(window)) return false;
        const auto hwnd = reinterpret_cast<HWND>(window.id);
        if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
        return SetForegroundWindow(hwnd) != FALSE;
    }
    bool IsForeground(const GameWindow& window) override {
        return Valid(window) && GetForegroundWindow() == reinterpret_cast<HWND>(window.id);
    }
};

// The YouTube window this app started, through a venv launcher or not.
// Another copy's window of the same title is left alone.
HWND OwnYouTubeWindow() {
    struct Search { std::map<DWORD, DWORD> parents; HWND found = nullptr; } search;
    if (const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{sizeof(entry)};
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
            search.parents[entry.th32ProcessID] = entry.th32ParentProcessID;
        CloseHandle(snapshot);
    }
    EnumWindows([](HWND hwnd, LPARAM context) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(context);
        wchar_t title[64]{};
        GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        if (std::wstring_view(title) != L"YouTube for QuartzMIDI") return TRUE;
        DWORD process = 0;
        GetWindowThreadProcessId(hwnd, &process);
        for (int level = 0; level < 4; ++level) {
            const auto parent = search.parents.find(process);
            if (parent == search.parents.end()) break;
            process = parent->second;
            if (process == GetCurrentProcessId()) { search.found = hwnd; return FALSE; }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}
}
std::string NoteName(int note) {
    static constexpr const char* names[]{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[note % 12]) + std::to_string(note / 12 - 1);
}
std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// "<MIDI folder> sheets" beside the MIDI folder, or Documents\QuartzMIDI sheets
// when no MIDI folder is set.
std::filesystem::path DefaultSheetsFolder(const std::filesystem::path& midiFolder) {
    if (!midiFolder.empty()) {
        auto folder = midiFolder;
        if (folder.filename().empty()) folder = folder.parent_path();
        return folder.parent_path() / (folder.filename().wstring() + L" sheets");
    }
    std::filesystem::path documents;
    PWSTR text = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &text))) { documents = text; CoTaskMemFree(text); }
    return documents / L"QuartzMIDI sheets";
}

namespace {
std::filesystem::path PathFromJson(const nlohmann::json& json, const char* key) {
    const auto text = json.value(key, std::string());
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::wstring Wide(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
}

// Sheet output path without extension: root plus the file's path relative to
// midiFolder. Files outside midiFolder go directly under root.
std::filesystem::path SheetTarget(const std::filesystem::path& root, const std::filesystem::path& midiFolder, const std::filesystem::path& midi) {
    std::error_code ignored;
    auto relative = midiFolder.empty() ? std::filesystem::path() : std::filesystem::relative(midi, midiFolder, ignored);
    if (relative.empty() || *relative.begin() == L"..") relative = midi.filename();
    auto target = root / relative;
    // Keep a .midi or .kar extension so a.mid, a.midi and a.kar don't produce the same sheet name.
    auto extension = target.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".midi" && extension != L".kar") target.replace_extension();
    return target;
}

// The shortest section a loop plays, in seconds.
constexpr double kShortestLoop = .25;
} // namespace

// The bar holding `at` and the point `bars` bars after its start, in seconds,
// by the file's tempo map and time signatures (4/4 before the first). Bars are
// counted in quarter notes, which the tempo sets in an SMPTE file too, where a
// tick is a fixed time. A division that times nothing gets eight seconds from
// `at`.
std::pair<double, double> BarsFrom(const MidiFile& file, double at, int bars) {
    const double ticksPerSecond = midi_timing::SmpteTicksPerSecond(file.division);
    if (file.division == 0 || (midi_timing::IsSmpte(file.division) && ticksPerSecond == 0)) return {at, at + 8};
    std::vector<TempoChange> tempos = file.tempoChanges;
    // A tempo of 0 is ignored, as the parser ignores it.
    std::erase_if(tempos, [](const TempoChange& tempo) { return tempo.microsecondsPerQuarter == 0; });
    std::stable_sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    if (tempos.empty() || tempos.front().tick > 0) tempos.insert(tempos.begin(), {0, 500000});
    // Each tempo's stretch: where it starts in ticks, seconds and quarter
    // notes, and the length of a quarter in seconds and in ticks.
    struct Stretch { double tick, seconds, quarters, secondsPerQuarter, ticksPerQuarter; };
    std::vector<Stretch> stretches;
    for (const auto& tempo : tempos) {
        Stretch stretch{static_cast<double>(tempo.tick), 0, 0, tempo.microsecondsPerQuarter / 1e6, 0};
        stretch.ticksPerQuarter = ticksPerSecond ? ticksPerSecond * stretch.secondsPerQuarter : file.division;
        if (!stretches.empty()) {
            const auto& before = stretches.back();
            const double quarters = (stretch.tick - before.tick) / before.ticksPerQuarter;
            stretch.quarters = before.quarters + quarters;
            stretch.seconds = before.seconds + quarters * before.secondsPerQuarter;
        }
        stretches.push_back(stretch);
    }
    // The stretch holding a point: the last to start at or before it.
    const auto holding = [&](auto starts) {
        auto found = stretches.begin();
        for (auto it = stretches.begin(); it != stretches.end(); ++it) if (starts(*it)) found = it;
        return *found;
    };
    const auto quartersAtTick = [&](double tick) {
        const auto& s = holding([&](const Stretch& it) { return it.tick <= tick; });
        return s.quarters + (tick - s.tick) / s.ticksPerQuarter;
    };
    const auto seconds = [&](double quarters) {
        const auto& s = holding([&](const Stretch& it) { return it.quarters <= quarters; });
        return s.seconds + (quarters - s.quarters) * s.secondsPerQuarter;
    };
    const auto& playing = holding([&](const Stretch& it) { return it.seconds <= at; });
    const double quarter = playing.quarters + (at - playing.seconds) / playing.secondsPerQuarter;
    // The meter in force at a point, as where it starts and its bar, in quarters.
    const auto meterAt = [&](double at) {
        double tick = -1, from = 0, bar = 4;
        for (const auto& signature : file.timeSignatures) {
            const double starts = quartersAtTick(signature.tick);
            if (starts <= at + 1e-9 && signature.tick >= tick && signature.numerator && signature.denominator) {
                tick = signature.tick; from = starts; bar = 4.0 * signature.numerator / signature.denominator;
            }
        }
        return std::pair{from, bar};
    };
    const auto [from, bar] = meterAt(quarter);
    // A point a rounding error short of a bar line is on it.
    const double start = from + std::floor((quarter - from) / bar + 1e-9) * bar;
    // Each bar after it is as long as the meter where it starts says, and a
    // meter changing partway through a bar starts a new one there.
    double end = start;
    for (int i = 0; i < bars; ++i) {
        double next = end + meterAt(end).second;
        for (const auto& signature : file.timeSignatures) {
            const double starts = quartersAtTick(signature.tick);
            if (starts > end + 1e-9 && starts < next - 1e-9 && signature.numerator && signature.denominator) next = starts;
        }
        end = next;
    }
    return {seconds(start), seconds(end)};
}

namespace {

// qm_sheets takes a UTF-8 JSON request and calls reply once, synchronously,
// with a JSON object. A reply containing "error" is thrown.
using SheetsCall = void(const char* request, void (*reply)(const char* json, void* user), void* user);
nlohmann::json AskSheets(SheetsCall* call, const nlohmann::json& request) {
    if (!call) throw std::runtime_error("The sheets add-on is not installed.");
    std::string text;
    call(request.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str(),
         [](const char* json, void* user) { *static_cast<std::string*>(user) = json; }, &text);
    auto answer = nlohmann::json::parse(text, nullptr, false);
    if (!answer.is_object()) throw std::runtime_error("The sheets add-on did not answer.");
    if (answer.contains("error")) throw std::runtime_error(answer.value("error", "The sheets add-on failed."));
    return answer;
}

// Takes the sheet editor's style for the app to keep. The editor is a file in
// the user's browser with no other way back, so it posts each change to a
// port Windows picks on 127.0.0.1, with a token only that page carries.
class SheetStyleListener {
public:
    explicit SheetStyleListener(std::function<void(std::string)> take) : take_(std::move(take)) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
        started_ = true;
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(address);
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || ::listen(socket_, 8) != 0 ||
            getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return;
        }
        std::random_device random;
        char token[40];
        snprintf(token, sizeof(token), "%08x%08x%08x%08x", random(), random(), random(), random());
        target_ = std::string("/style?k=") + token;
        url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + target_;
        thread_ = std::thread([this] { Serve(); });
    }
    ~SheetStyleListener() {
        stopping_ = true;
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
        if (thread_.joinable()) thread_.join();
        if (started_) WSACleanup();
    }
    SheetStyleListener(const SheetStyleListener&) = delete;
    SheetStyleListener& operator=(const SheetStyleListener&) = delete;
    // Empty when no port could be had; the editor then keeps its style in the browser alone.
    const std::string& Url() const { return url_; }
private:
    void Serve() {
        while (!stopping_) {
            const SOCKET client = accept(socket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (stopping_ || WSAGetLastError() == WSAENOTSOCK || WSAGetLastError() == WSAEINVAL) return;
                continue;
            }
            Answer(client);
            closesocket(client);
        }
    }
    void Answer(SOCKET client) {
        const DWORD wait = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wait), sizeof(wait));
        std::string request;
        size_t headerEnd = std::string::npos;
        char buffer[16384];
        while ((headerEnd = request.find("\r\n\r\n")) == std::string::npos && request.size() < 16384) {
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) return;
            request.append(buffer, static_cast<size_t>(got));
        }
        if (headerEnd == std::string::npos) return;
        const auto reply = [&](const char* status) {
            const std::string text = std::string("HTTP/1.1 ") + status +
                "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: POST\r\nAccess-Control-Allow-Headers: Content-Type\r\n"
                "Access-Control-Allow-Private-Network: true\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client, text.data(), static_cast<int>(text.size()), 0);
        };
        const auto line = request.substr(0, request.find("\r\n"));
        const bool post = line == "POST " + target_ + " HTTP/1.1" || line == "POST " + target_ + " HTTP/1.0";
        if (line == "OPTIONS " + target_ + " HTTP/1.1") { reply("204 No Content"); return; }
        if (!post) { reply("404 Not Found"); return; }
        std::string headers = request.substr(0, headerEnd);
        std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto length = headers.find("\r\ncontent-length:");
        if (length == std::string::npos) { reply("411 Length Required"); return; }
        const size_t size = std::strtoull(headers.c_str() + length + 17, nullptr, 10);
        // A style with a background picture is a few megabytes at most.
        if (size > (64u << 20)) { reply("413 Content Too Large"); return; }
        std::string body = request.substr(headerEnd + 4);
        while (body.size() < size) {
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) return;
            body.append(buffer, static_cast<size_t>(got));
        }
        body.resize(size);
        reply("204 No Content");
        take_(std::move(body));
    }
    std::function<void(std::string)> take_;
    bool started_ = false;
    SOCKET socket_ = INVALID_SOCKET;
    std::atomic<bool> stopping_{false};
    std::string target_, url_;
    std::thread thread_;
};

// Sheet request for a file that isn't loaded (library export): every non-drum
// note-on with its release and sustain pedal value in ticks, with the tempo
// map and time signatures.
nlohmann::json PageForFile(const MidiFile& file, const std::string& title, const std::map<std::string, std::string>& mapping) {
    nlohmann::json page{{"title", title}, {"mapping", mapping}, {"division", file.division}};
    auto& tempos = page["tempos"] = nlohmann::json::array();
    for (const auto& change : file.tempoChanges) tempos.push_back({change.tick, change.microsecondsPerQuarter});
    auto& meters = page["meters"] = nlohmann::json::array();
    for (const auto& signature : file.timeSignatures) meters.push_back({signature.tick, signature.numerator});
    auto& ticks = page["ticks"] = nlohmann::json::array();
    auto& pedals = page["pedalTicks"] = nlohmann::json::array();
    for (const auto& row : DescribeTracks(file)) {
        if (row.drums || row.index >= file.tracks.size()) continue;
        // Each note-on's release is the next note-off of its channel and note.
        std::map<std::pair<int, int>, std::deque<size_t>> sounding;
        for (const auto& event : file.tracks[row.index].events) {
            const std::pair key{event.status & 0x0F, int{event.data1}};
            if ((event.status & 0xF0) == 0x90 && event.data2 != 0) {
                sounding[key].push_back(ticks.size());
                ticks.push_back({event.absoluteTick, event.data1});
            } else if ((event.status & 0xF0) == 0x80 || (event.status & 0xF0) == 0x90) {
                if (auto& on = sounding[key]; !on.empty()) { ticks[on.front()].push_back(event.absoluteTick); on.pop_front(); }
            }
            if ((event.status & 0xF0) == 0xB0 && PedalForController(event.data1) == 0)
                pedals.push_back({event.absoluteTick, event.data2, row.index});
        }
    }
    return page;
}

// A folder with more MIDI files than this is something like a drive root.
constexpr size_t kMaxLibraryFiles = 20000;
// The YouTube window's Save to lists at most this many folders.
constexpr size_t kMaxLibraryFolders = 2000;

// Whether a folder may be offered as a dest: not hidden, and not a junction,
// symlink or other reparse point that leads elsewhere.
bool OfferedFolder(const std::filesystem::path& folder) {
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_HIDDEN))
        return false;
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
    WIN32_FIND_DATAW entry{};
    const HANDLE find = FindFirstFileExW(folder.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find);
    return !audio_to_midi::Redirects(entry.dwFileAttributes, entry.dwReserved0);
}

// Whether folder a, by its names, sorts before b: name by name without case,
// then as written, and a folder before what is in it.
bool FolderBefore(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const int order = CompareStringOrdinal(a[i].c_str(), -1, b[i].c_str(), -1, TRUE);
        if (order != CSTR_EQUAL) return order == CSTR_LESS_THAN;
    }
    return a.size() != b.size() ? a.size() < b.size() : a < b;
}

// The MIDI files below root, unsorted, each named by its path relative to root.
// Stops at kMaxLibraryFiles. Null when root cannot be read. folders, when given,
// gets the folders the YouTube window may save into: each as a dest (names
// joined by "/"), empty ones too, at most kDestDepth deep, every name one
// ValidSegment takes, none below a folder that is not listed, OfferedFolder()
// only, sorted by FolderBefore and kMaxLibraryFolders at most.
std::shared_ptr<std::vector<MidiEntry>> WalkLibrary(const std::filesystem::path& root, std::stop_token stop, std::error_code& error,
                                                    std::vector<std::string>* folders = nullptr) {
    auto files = std::make_shared<std::vector<MidiEntry>>();
    // Skip permission-denied folders. Directory symlinks are not
    // followed, so a junction to its own parent can't recurse forever.
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) return nullptr;
    const std::filesystem::recursive_directory_iterator end;
    // Bound the walk for pathological trees.
    constexpr int kMaxDepth = 32;
    // The names of the folder last met at each depth, and whether it is
    // listed; the walk meets a folder just before what is in it.
    std::array<std::wstring, audio_to_midi::kDestDepth> names;
    std::array<bool, audio_to_midi::kDestDepth> listed{};
    std::vector<std::vector<std::wstring>> found;
    while (it != end && files->size() < kMaxLibraryFiles) {
        if (stop.stop_requested()) break;
        const auto& entry = *it;
        std::error_code entryError;
        if (const auto depth = static_cast<size_t>(it.depth());
            folders && depth < audio_to_midi::kDestDepth && entry.is_directory(entryError) && !entryError) {
            names[depth] = entry.path().filename().native();
            listed[depth] = (depth == 0 || listed[depth - 1]) && audio_to_midi::ValidSegment(names[depth]) && OfferedFolder(entry.path());
            if (listed[depth] && found.size() < kMaxLibraryFiles) found.emplace_back(names.begin(), names.begin() + depth + 1);
        }
        if (entry.is_regular_file(entryError) && !entryError && LibraryFile(entry.path())) {
            std::error_code sizeError, timeError;
            const auto bytes = entry.file_size(sizeError);
            // The iterator builds every path as root / ..., so the name is
            // lexical; std::filesystem::relative would open the file and root.
            auto shown = entry.path().lexically_relative(root);
            if (shown.empty()) shown = entry.path().filename();
            const auto modified = entry.last_write_time(timeError);
            files->push_back({entry.path(), Utf8(shown), sizeError ? 0 : bytes,
                timeError ? std::filesystem::file_time_type{} : modified});
        }
        if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
        std::error_code step;
        it.increment(step);
        // A failed increment may not advance; continuing would spin.
        if (step) break;
    }
    if (folders) {
        // A folder sorts before what is in it, so the cut never leaves a name without its folder.
        std::sort(found.begin(), found.end(), FolderBefore);
        if (found.size() > kMaxLibraryFolders) found.resize(kMaxLibraryFolders);
        folders->clear();
        for (const auto& folder : found) {
            std::wstring dest;
            for (const auto& name : folder) dest += (dest.empty() ? L"" : L"/") + name;
            folders->push_back(Utf8(dest));
        }
    }
    return files;
}

// To the last note on or off; 120 bpm until the first tempo, as playback has it.
double SongSeconds(const MidiFile& midi) {
    uint32_t last = 0;
    for (const auto& track : midi.tracks)
        for (const auto& event : track.events)
            if (const int kind = event.status & 0xF0; kind == 0x80 || kind == 0x90) last = std::max(last, event.absoluteTick);
    if (midi.division & 0x8000) {
        const int perSecond = -static_cast<int8_t>(midi.division >> 8) * (midi.division & 0xFF);
        return perSecond > 0 ? static_cast<double>(last) / perSecond : 0;
    }
    if (midi.division == 0) return 0;
    auto tempos = midi.tempoChanges;
    std::stable_sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    double seconds = 0, perQuarter = 500000;
    uint32_t tick = 0;
    for (const auto& change : tempos) {
        if (change.tick >= last) break;
        seconds += (change.tick - tick) * perQuarter / (1e6 * midi.division);
        tick = change.tick;
        perQuarter = change.microsecondsPerQuarter;
    }
    return seconds + (last - tick) * perQuarter / (1e6 * midi.division);
}

} // namespace

double SongSeconds(const std::filesystem::path& song) {
    try {
        MidiParser parser;
        return SongSeconds(parser.parse(Utf8(std::filesystem::absolute(song))));
    } catch (const std::exception&) { return -1; }
}

namespace {

// lengths.json: each song's path, then its size, time and seconds as last read.
struct KnownLength { uintmax_t bytes; std::filesystem::file_time_type modified; double seconds; };
PathMap<KnownLength> ReadLengths(const nlohmann::json& saved) {
    PathMap<KnownLength> lengths;
    if (!saved.is_object()) return lengths;
    for (const auto& [song, known] : saved.items()) {
        if (!known.is_array() || known.size() != 3 || !known[0].is_number_unsigned() || !known[1].is_number_integer() ||
            !known[2].is_number()) continue;
        const std::filesystem::file_time_type modified{std::filesystem::file_time_type::duration(known[1].get<int64_t>())};
        lengths.emplace(std::filesystem::path(std::u8string(song.begin(), song.end())),
                        KnownLength{known[0].get<uintmax_t>(), modified, known[2].get<double>()});
    }
    return lengths;
}

bool WatchFolder(const std::filesystem::path& root, HANDLE directory, std::stop_token stop, HANDLE woken, HANDLE leavingDrive,
                 const std::function<bool(const std::filesystem::path&)>& listed, const std::function<void()>& changed);

// What the drive under a watched folder says, set from a system thread:
// leaving (asked to eject, or gone), closed (the watcher's answer), stayed
// (the eject was refused) and removed.
struct DriveSignals { HANDLE leaving, closed, stayed, removed; };

DWORD CALLBACK DriveChanged(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD) {
    const auto& signals = *static_cast<const DriveSignals*>(context);
    switch (action) {
    case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
        // The eject waits on this answer, so the watch lets go first.
        SetEvent(signals.leaving);
        WaitForSingleObject(signals.closed, 5000);
        break;
    case CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED: SetEvent(signals.stayed); break;
    case CM_NOTIFY_ACTION_DEVICEREMOVEPENDING:
    case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE: SetEvent(signals.removed); SetEvent(signals.leaving); break;
    default: break;
    }
    return ERROR_SUCCESS;
}

// Waits for MIDI files below root, or folders holding them, to be added,
// removed, renamed or rewritten, and calls changed once they settle. listed
// says whether a path that is gone held listed files. The folder is let go
// while its drive is asked to eject, and watched again if the eject is refused.
// Returns when stop is requested or root can no longer be watched.
void WatchLibrary(const std::filesystem::path& root, std::stop_token stop,
                  const std::function<bool(const std::filesystem::path&)>& listed, const std::function<void()>& changed) {
    const HANDLE woken = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DriveSignals signals{CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr),
                         CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (woken && signals.leaving && signals.closed && signals.stayed && signals.removed) {
        std::stop_callback wake(stop, [&] { SetEvent(woken); });
        for (bool again = false; !stop.stop_requested(); again = true) {
            const HANDLE directory = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (directory == INVALID_HANDLE_VALUE) break;
            for (const HANDLE signal : {signals.leaving, signals.closed, signals.stayed, signals.removed}) ResetEvent(signal);
            CM_NOTIFY_FILTER filter{};
            filter.cbSize = sizeof filter;
            filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
            filter.u.DeviceHandle.hTarget = directory;
            HCMNOTIFICATION notification = nullptr;
            if (CM_Register_Notification(&filter, &signals, DriveChanged, &notification) != CR_SUCCESS) notification = nullptr;
            // What changed while the folder was let go.
            if (again) changed();
            const bool leaving = WatchFolder(root, directory, stop, woken, signals.leaving, listed, changed);
            CloseHandle(directory);
            SetEvent(signals.closed);
            DWORD outcome = WAIT_FAILED;
            if (leaving) {
                const HANDLE answers[]{woken, signals.stayed, signals.removed};
                outcome = WaitForMultipleObjects(3, answers, FALSE, INFINITE);
            }
            // Not from inside DriveChanged: this waits for it to return.
            if (notification) CM_Unregister_Notification(notification);
            if (outcome != WAIT_OBJECT_0 + 1) break;
        }
    }
    for (const HANDLE event : {woken, signals.leaving, signals.closed, signals.stayed, signals.removed}) if (event) CloseHandle(event);
}

// WatchLibrary's reading of one open directory. True when its drive is
// leaving, false when stop is requested or the folder can no longer be read.
bool WatchFolder(const std::filesystem::path& root, HANDLE directory, std::stop_token stop, HANDLE woken, HANDLE leavingDrive,
                 const std::function<bool(const std::filesystem::path&)>& listed, const std::function<void()>& changed) {
    using namespace std::chrono_literals;
    bool leaving = false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent) {
        // DWORD-aligned, and no more than the 64 KB a network share accepts.
        std::vector<DWORD> buffer(16384);
        const auto read = [&] {
            ResetEvent(overlapped.hEvent);
            return ReadDirectoryChangesW(directory, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(DWORD)), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                nullptr, &overlapped, nullptr) != FALSE;
        };
        bool reading = read();
        bool pending = false;
        std::chrono::steady_clock::time_point first{}, due{};
        while (reading && !stop.stop_requested()) {
            // Checked before waiting, so a stream of other changes cannot hold it off.
            const auto now = std::chrono::steady_clock::now();
            if (pending && now >= due) { pending = false; changed(); continue; }
            const DWORD timeout = pending
                ? static_cast<DWORD>(std::chrono::ceil<std::chrono::milliseconds>(due - now).count()) : INFINITE;
            const HANDLE handles[]{woken, leavingDrive, overlapped.hEvent};
            const DWORD woke = WaitForMultipleObjects(3, handles, FALSE, timeout);
            if (woke == WAIT_TIMEOUT) continue;
            if (woke == WAIT_OBJECT_0 + 1) { leaving = true; break; }
            if (woke != WAIT_OBJECT_0 + 2) break;
            reading = false;
            DWORD bytes = 0;
            // An overflowed buffer names nothing, so it counts as a change.
            if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE) && GetLastError() != ERROR_NOTIFY_ENUM_DIR) break;
            bool relevant = bytes == 0;
            for (size_t offset = 0; bytes && !relevant;) {
                const auto* record = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(reinterpret_cast<const char*>(buffer.data()) + offset);
                const std::filesystem::path name(std::wstring(record->FileName, record->FileNameLength / sizeof(wchar_t)));
                const auto path = root / name;
                std::error_code ignored;
                relevant = LibraryFile(path) ||
                    ((record->Action == FILE_ACTION_ADDED || record->Action == FILE_ACTION_RENAMED_NEW_NAME) && std::filesystem::is_directory(path, ignored)) ||
                    ((record->Action == FILE_ACTION_REMOVED || record->Action == FILE_ACTION_RENAMED_OLD_NAME) && listed(path));
                if (!record->NextEntryOffset) break;
                offset += record->NextEntryOffset;
            }
            if (relevant) {
                const auto seen = std::chrono::steady_clock::now();
                if (!pending) first = seen;
                pending = true;
                // Settle for half a second, but show a long copy as it goes.
                due = std::min(seen + 500ms, first + 2s);
            }
            reading = read();
        }
        // The buffer must outlive a cancelled read.
        if (reading && CancelIoEx(directory, &overlapped)) { DWORD ignored = 0; GetOverlappedResult(directory, &overlapped, &ignored, TRUE); }
    }
    if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
    return leaving;
}

// library.json: the favourites by path and the list the panel shows.
std::filesystem::path PathFromText(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
// A saved file's entries are read one by one, and one of the wrong kind is
// left out alone, setting damaged.
std::vector<std::filesystem::path> PathsFromJson(const nlohmann::json& json, const char* key, bool& damaged) {
    std::vector<std::filesystem::path> paths;
    const auto found = json.find(key);
    if (found == json.end()) return paths;
    if (!found->is_array()) { damaged = true; return paths; }
    for (const auto& each : *found)
        if (each.is_string() && !each.get<std::string>().empty()) paths.push_back(PathFromText(each.get<std::string>()));
        else damaged = true;
    return paths;
}
// A string member, or empty when it is missing or of another kind.
std::string TextOf(const nlohmann::json& json, const char* key) {
    const auto found = json.find(key);
    return found != json.end() && found->is_string() ? found->get<std::string>() : std::string();
}
// Whether the panel can show a list: the queue only while it holds a song.
bool ListShown(const LibraryLists& lists, int list) {
    return list == kFolderList || list == kFavouritesList || (list == kQueueList && !lists.queue.empty()) ||
        (list >= 0 && static_cast<size_t>(list) < lists.playlists.size());
}
// The app's window, which owns Windows' prompt so it comes to the front.
std::atomic<HWND> recycleOwner{nullptr};
// Sends files to the Recycle Bin in one operation, never deleting them outright:
// where the bin cannot take them, Windows asks first. On a thread of its own,
// since the shell wants a single-threaded apartment.
bool RecycleBin(const std::vector<std::filesystem::path>& files) {
    bool recycled = false;
    const HWND owner = recycleOwner.load(std::memory_order_acquire);
    std::thread([&] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        // Each null-terminated, and the list double-null-terminated.
        std::wstring from;
        for (const auto& file : files) { from += file.native(); from.push_back(L'\0'); }
        SHFILEOPSTRUCTW operation{};
        operation.hwnd = IsWindow(owner) ? owner : nullptr;
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
        recycled = SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
        if (SUCCEEDED(com)) CoUninitialize();
    }).join();
    return recycled;
}
std::mutex recyclerMutex;
ShellEngine::Recycler recycler = RecycleBin;
// Through SetRecycler's choice, the Recycle Bin unless the tests replaced it.
bool Recycle(const std::vector<std::filesystem::path>& files) {
    ShellEngine::Recycler recycle;
    { std::lock_guard lock(recyclerMutex); recycle = recycler; }
    return recycle(files);
}
// Songs in the MIDI folder are kept by their path in it, below "folder", so
// the lists follow the folder when it moves; files from before kept whole paths.
void ReadLibrary(const nlohmann::json& json, LibraryLists& lists, int& openList, std::filesystem::path& root, bool& damaged) {
    if (const auto folder = json.find("folder"); folder != json.end() && !folder->is_string()) damaged = true;
    root = PathFromText(TextOf(json, "folder"));
    const auto whole = [&](std::filesystem::path& path) { if (!root.empty() && path.is_relative()) path = root / path; };
    lists.favourites = PathsFromJson(json, "favourites", damaged);
    lists.queue = PathsFromJson(json, "queue", damaged);
    if (const auto playlists = json.find("playlists"); playlists != json.end() && !playlists->is_array()) damaged = true;
    else if (playlists != json.end())
        for (const auto& each : *playlists)
            if (const auto name = TextOf(each, "name"); !name.empty()) lists.playlists.push_back({name, PathsFromJson(each, "songs", damaged)});
            else damaged = true;
    for (auto& song : lists.favourites) whole(song);
    for (auto& song : lists.queue) whole(song);
    for (auto& playlist : lists.playlists) for (auto& song : playlist.songs) whole(song);
    lists.Deduplicate();
    // What songs sent to the Recycle Bin were in (LibraryLists::Delete).
    if (const auto deleted = json.find("deleted"); deleted != json.end() && !deleted->is_array()) damaged = true;
    else if (deleted != json.end())
        for (const auto& each : *deleted) {
            DeletedSong record{each.is_object() ? PathFromText(TextOf(each, "song")) : std::filesystem::path()};
            if (record.path.empty()) { damaged = true; continue; }
            whole(record.path);
            const auto favourite = each.find("favourite");
            record.favourite = favourite != each.end() && favourite->is_boolean() && favourite->get<bool>();
            if (const auto in = each.find("playlists"); in != each.end() && in->is_array())
                for (const auto& playlist : *in) {
                    const auto at = playlist.is_object() ? playlist.find("at") : playlist.end();
                    if (const auto name = TextOf(playlist, "name"); !name.empty() && at != playlist.end() && at->is_number_unsigned())
                        record.playlists.emplace_back(name, at->get<size_t>());
                }
            if (const auto settings = each.find("settings"); settings != each.end() && settings->is_object()) record.settings = settings->dump();
            lists.deleted.push_back(std::move(record));
        }
    if (lists.deleted.size() > kMaxDeletedSongs) lists.deleted.erase(lists.deleted.begin(), lists.deleted.end() - kMaxDeletedSongs);
    // Songs a build before Delete kept in its Trash go to the Recycle Bin when
    // their folder opens; the stars and playlists kept for them go now, unless
    // a song is at their path again. Nothing else of the Trash is read.
    if (const auto trash = json.find("trash"); trash != json.end() && trash->is_array())
        for (const auto& song : *trash)
            if (auto origin = PathFromText(TextOf(song, "origin")); !origin.empty()) {
                whole(origin);
                if (std::error_code error; !std::filesystem::exists(origin, error) && !error) lists.Forget(origin);
            }
    const auto open = json.find("open");
    if (open != json.end() && !open->is_number_integer()) damaged = true;
    const int shown = open != json.end() && open->is_number_integer() ? open->get<int>() : kFolderList;
    openList = ListShown(lists, shown) ? shown : kFolderList;
}
nlohmann::json WriteLibrary(const LibraryLists& lists, int openList, const std::filesystem::path& root) {
    const auto text = [&](const std::filesystem::path& path) { const auto rest = Below(path, root); return Utf8(rest.empty() ? path : rest); };
    const auto paths = [&](const std::vector<std::filesystem::path>& songs) {
        auto list = nlohmann::json::array();
        for (const auto& song : songs) list.push_back(text(song));
        return list;
    };
    auto playlists = nlohmann::json::array();
    for (const auto& playlist : lists.playlists) playlists.push_back({{"name", playlist.name}, {"songs", paths(playlist.songs)}});
    nlohmann::json json{{"favourites", paths(lists.favourites)}, {"playlists", std::move(playlists)},
                        {"queue", paths(lists.queue)}, {"open", openList}};
    if (!root.empty()) json["folder"] = Utf8(root);
    if (!lists.deleted.empty()) {
        auto& deleted = json["deleted"] = nlohmann::json::array();
        for (const auto& record : lists.deleted) {
            nlohmann::json each{{"song", text(record.path)}};
            if (record.favourite) each["favourite"] = true;
            for (const auto& [name, at] : record.playlists) each["playlists"].push_back({{"name", name}, {"at", at}});
            if (!record.settings.empty()) each["settings"] = nlohmann::json::parse(record.settings, nullptr, false);
            deleted.push_back(std::move(each));
        }
    }
    return json;
}

std::mutex addonsMutex;
std::filesystem::path addonsFolder;
addon::PublicKey addonsKey = kOwnerKey;
} // namespace

void ShellEngine::SetAddons(std::filesystem::path folder, const std::array<std::uint8_t, 72>& key) {
    static_assert(std::tuple_size_v<addon::PublicKey> == 72);
    std::lock_guard lock(addonsMutex);
    addonsFolder = std::move(folder);
    addonsKey = key;
}

void ShellEngine::SetRecycler(Recycler recycle) {
    std::lock_guard lock(recyclerMutex);
    recycler = recycle ? std::move(recycle) : Recycler(RecycleBin);
}

ShellEngine::ShellEngine(std::filesystem::path config, std::shared_ptr<AutoVolumeHost> volumeHost,
                         bool requireTypingAcknowledgement, ConnectFactory connectFactory)
    : config_(std::move(config)), volumeHost_(volumeHost ? std::move(volumeHost) : std::make_shared<NativeAutoVolumeHost>()),
      requireTypingAcknowledgement_(requireTypingAcknowledgement),
      connectFactory_(std::move(connectFactory)),
      worker_([this](std::stop_token stop) { Run(stop); }) {}

ShellEngine::~ShellEngine() {
    // Request the stop under the lock so it can't land between the worker's
    // predicate check and its wait, losing the notification.
    { std::lock_guard lock(mutex_); worker_.request_stop(); }
    wake_.notify_all();
    worker_.join(); // Key release and player destruction also happen on the worker.
}

void ShellEngine::Send(Command command) {
    // Stop lets the keys go at once, on the caller's thread: the worker may
    // be parsing a song for seconds. Its own sweep follows in order.
    if (command.action == Action::Stop) crash::ReleaseKeys();
    { std::lock_guard lock(mutex_); commands_.push_back(std::move(command)); }
    wake_.notify_one();
}

std::shared_ptr<const EngineSnapshot> ShellEngine::Snapshot() const {
    const auto log = ShellLog::Instance().Snapshot();
    std::lock_guard lock(mutex_);
    if (log != snapshot_->log) {
        auto copy = std::make_shared<EngineSnapshot>(*snapshot_);
        copy->log = log;
        snapshot_ = std::move(copy);
    }
    return snapshot_;
}

void ShellEngine::Publish(const EngineSnapshot& state) {
    // Only the worker publishes, so the error check and the store can be
    // separate critical sections, keeping the log append outside the lock the
    // UI thread takes every frame.
    bool newError = false;
    { std::lock_guard lock(mutex_); newError = !state.error.empty() && state.error != snapshot_->error; }
    if (newError) ShellLog::Instance().Append("[error] " + state.error + "\n");
    auto copy = std::make_shared<EngineSnapshot>(state);
    // Fill the log on the worker; otherwise Snapshot() sees it differ and deep
    // copies the whole state on the UI thread.
    copy->log = ShellLog::Instance().Snapshot();
    // The new snapshot carries the position; the snapshot before it keeps
    // its own meanwhile, which is no newer.
    state.clock->store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
    { std::lock_guard lock(mutex_); snapshot_ = std::move(copy); }
    // Wake the on-demand render loop.
    if (void* window = wakeWindow_.load(std::memory_order_acquire)) PostMessageW(static_cast<HWND>(window), WM_NULL, 0, 0);
}

void ShellEngine::PublishPosition(const EngineSnapshot& state) {
    if (state.clock->exchange(state.position, std::memory_order_relaxed) == state.position) return;
    // A minimized or hidden window draws nothing, so it is not woken for it.
    const auto window = static_cast<HWND>(wakeWindow_.load(std::memory_order_acquire));
    if (window && IsWindowVisible(window) && !IsIconic(window)) PostMessageW(window, WM_NULL, 0, 0);
}

void ShellEngine::SetWakeWindow(void* window) {
    wakeWindow_.store(window, std::memory_order_release);
    recycleOwner.store(static_cast<HWND>(window), std::memory_order_release);
}

void ShellEngine::SetKeyProbe(std::function<bool(int)> probe) {
    std::lock_guard lock(keyProbeMutex_);
    keyProbe_ = std::move(probe);
}

// Fields of a section the app keeps for itself: the switch, the Player row's
// Custom and saved Custom, and a song's pin.
static bool ReservedField(const std::string& field) {
    return field == "ENABLED" || field == "CUSTOM" || field == "PINNED" || field.starts_with("CUSTOM_");
}

// A performer's Player row: a preset by its index, or Custom on that preset's
// habits with the row's own amounts, and the Custom kept while a preset is in use.
struct PlayerRow {
    int preset = 0;
    bool custom = false;
    std::map<std::string, double> values;
    std::optional<PerformerCustom> saved;
};

// The number saved under `key`, if one is.
static std::optional<double> SavedNumber(const nlohmann::json& saved, const std::string& key) {
    const auto found = saved.find(key);
    if (found == saved.end() || !found->is_number()) return std::nullopt;
    const double number = found->get<double>();
    return std::isfinite(number) ? std::optional(number) : std::nullopt;
}

// Parses a performer's `describe` JSON (engine/Performer.hpp). Throws when the
// name or config key is missing, a control id is missing or duplicated, a
// preset's value is not a whole number from 0 or is another preset's, a field
// is one the app keeps, a preset sets what is not a row slider, or `per_song`
// is on what cannot be kept per song.
static PerformerSection ReadPerformerSection(const nlohmann::json& described) {
    PerformerSection section;
    section.name = described.at("name").get<std::string>();
    section.tag = described.value("tag", std::string());
    section.config = described.at("config").get<std::string>();
    if (section.name.empty() || section.config.empty()) throw std::runtime_error("it has no name or no place in config.json.");
    if (const auto presets = described.find("presets"); presets != described.end()) {
        section.presetId = presets->at("id").get<std::string>();
        section.presetField = presets->at("field").get<std::string>();
        section.presetName = presets->value("name", std::string());
        for (const auto& choice : presets->at("choices")) {
            // A preset is saved and sent as its value, so one can be added
            // among the others and what was saved keeps its meaning.
            const auto declared = choice.find("value");
            const auto value = declared == choice.end() ? nlohmann::json(section.presets.size()) : *declared;
            if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() > std::numeric_limits<int>::max())
                throw std::runtime_error("a preset's value is not a whole number from 0.");
            if (section.PresetOf(value.get<int>()) >= 0) throw std::runtime_error("two presets have the same value.");
            section.presets.push_back(choice.at("name").get<std::string>());
            section.presetValues.push_back(choice.value("values", std::map<std::string, double>{}));
            section.presetNumbers.push_back(value.get<int>());
            section.presetEarlier.push_back(choice.value("earlier", std::vector<std::map<std::string, double>>{}));
        }
        if (ReservedField(section.presetField)) throw std::runtime_error("the presets' field is one the app keeps.");
    }
    const auto read = [&](const nlohmann::json& controls, int trigger) {
        for (const auto& each : controls) {
            PerformerControl control;
            control.id = each.at("id").get<std::string>();
            control.field = each.at("field").get<std::string>();
            control.name = each.at("name").get<std::string>();
            const auto type = each.at("type").get<std::string>();
            control.isSwitch = type == "switch";
            control.isChoice = type == "choice";
            control.trigger = trigger;
            control.shownWith = each.value("shown_with", std::string());
            if (control.id.empty() || control.field.empty() || section.Find(control.id)) throw std::runtime_error("a control has no id of its own.");
            if (ReservedField(control.field)) throw std::runtime_error("a control's field is one the app keeps.");
            control.perSong = each.value("per_song", false);
            if (control.perSong && (control.isSwitch || trigger >= 0)) throw std::runtime_error("per_song is on a switch or a trigger's control.");
            if (control.isChoice) {
                control.choices = each.at("choices").get<std::vector<std::string>>();
                if (control.choices.size() < 2) throw std::runtime_error("a choice has nothing to choose between.");
                control.panel = each.value("panel", false);
                control.value = std::clamp(each.value("value", 0), 0, static_cast<int>(control.choices.size()) - 1);
                control.estimateName = each.value("estimate", std::string());
            } else if (control.isSwitch) {
                control.value = each.value("value", false) ? 1 : 0;
                control.estimates = each.value("estimates", std::string());
                control.remembers = each.value("remembers", false);
                control.holds = each.value("holds", false);
            } else {
                control.estimateName = each.value("estimate", std::string());
                control.value = control.estimateName.empty() ? std::clamp(each.value("value", 0.0), 0.0, 1.0) : -1;
                if (each.value("shows", std::string()) == "note") {
                    control.showsNote = true;
                    control.low = std::clamp(each.value("low", 21), 0, 126);
                    control.high = std::clamp(each.value("high", 108), control.low + 1, 127);
                }
            }
            control.rest = control.value;
            section.controls.push_back(std::move(control));
        }
    };
    read(described.value("controls", nlohmann::json::array()), -1);
    size_t nextKey = kAppHotkeys;
    for (const auto& each : described.value("triggers", nlohmann::json::array())) {
        PerformerTrigger trigger;
        trigger.name = each.at("name").get<std::string>();
        // The first trigger is always the clock.
        if (!section.triggers.empty()) { trigger.plays = each.value("plays", false); trigger.steps = !trigger.plays && each.value("steps", false); }
        if (const auto keys = each.find("keys"); keys != each.end() && (trigger.plays || trigger.steps)) {
            trigger.keysName = keys->at("name").get<std::string>();
            trigger.keysAdd = keys->value("add", std::string());
            trigger.keyFields = keys->at("fields").get<std::vector<std::string>>();
            trigger.keyDefaults = keys->value("defaults", std::vector<std::string>{});
            trigger.firstKey = nextKey;
            nextKey += trigger.keyFields.size();
            if (nextKey > kFirstLaterHotkey) throw std::runtime_error("it asks for more keys than there are places.");
        }
        read(each.value("controls", nlohmann::json::array()), static_cast<int>(section.triggers.size()));
        section.triggers.push_back(std::move(trigger));
    }
    for (const auto& each : described.value("actions", nlohmann::json::array())) {
        PerformerAction action;
        action.id = each.at("id").get<std::string>();
        action.name = each.at("name").get<std::string>();
        action.field = each.at("field").get<std::string>();
        action.keyDefault = each.value("default", std::string());
        action.schedules = each.value("schedules", false);
        if (action.id.empty() || action.field.empty()) throw std::runtime_error("an action has no id or no place in config.json.");
        action.key = nextKey++;
        if (nextKey > kFirstLaterHotkey) throw std::runtime_error("it asks for more keys than there are places.");
        section.actions.push_back(std::move(action));
    }
    // A preset sets only row sliders: a section slider with no estimate that is
    // not kept per song. Those are the row, in draw order.
    for (size_t preset = 0; preset < section.presets.size(); ++preset) {
        auto sets = section.presetEarlier[preset];
        sets.push_back(section.presetValues[preset]);
        for (const auto& set : sets)
            for (const auto& [id, value] : set) {
                const auto* control = section.Find(id);
                if (!control || control->isSwitch || control->isChoice || !control->estimateName.empty() || control->trigger >= 0 || control->perSong)
                    throw std::runtime_error("a preset sets what is not a row slider.");
            }
    }
    for (const auto& control : section.controls)
        if (std::any_of(section.presetValues.begin(), section.presetValues.end(), [&](const auto& values) { return values.contains(control.id); }))
            section.rowIds.push_back(control.id);
    if (!section.presets.empty())
        for (const auto& id : section.rowIds) section.Find(id)->value = section.Target(0, id);
    section.loaded = true;
    return section;
}

void ShellEngine::Run(std::stop_token stop) {
    using namespace std::chrono_literals;
    EngineSnapshot state;
    state.typingAcknowledged = !requireTypingAcknowledgement_;
    std::chrono::steady_clock::time_point playbackDue{};
    std::mt19937 random(std::random_device{}());
    bool loadAutoSolo = false;
    bool shuffleAdvancePending = false;
    // A quiet rescan that came during playback, run once playback stops.
    bool rescanOwed = false;
    // Songs left for another, oldest first, which Previous with Shuffle Play
    // walks back through.
    std::vector<std::filesystem::path> played;
    // Each sub-folder of the add-on folder is one add-on, identified by its
    // exports. Declared before the player, which calls into the performer, so
    // they unload after it.
    addon::Loaded sheetsAddon, performerAddon;
    {
        std::filesystem::path folder;
        addon::PublicKey key;
        { std::lock_guard lock(addonsMutex); folder = addonsFolder; key = addonsKey; }
        if (folder.empty()) folder = bundle::DataDirectory() / L"addons";
        std::error_code ignored;
        state.addonsFolder = std::filesystem::is_directory(folder, ignored);
        for (std::filesystem::directory_iterator each(folder, ignored), end; !ignored && each != end; each.increment(ignored)) {
            if (!std::filesystem::exists(each->path() / L"addon.json", ignored)) continue;
            std::string why;
            auto loaded = addon::Loaded::Load(each->path(), key, &why);
            if (!loaded) { ShellLog::Instance().Append("[addons] " + Utf8(each->path().filename()) + ": " + why + "\n"); continue; }
            if (!sheetsAddon && loaded.Find<SheetsCall>("qm_sheets")) sheetsAddon = std::move(loaded);
            else if (!performerAddon && loaded.Find<int(qm_performer_api*)>("qm_performer")) performerAddon = std::move(loaded);
        }
    }
    SheetsCall* const sheetsCall = sheetsAddon.Find<SheetsCall>("qm_sheets");
    state.sheetsAddon = sheetsCall != nullptr;
    // A performer whose section fails to parse is treated as absent.
    qm_performer_api performerApi{};
    if (const auto fill = performerAddon.Find<int(qm_performer_api*)>("qm_performer"); fill && fill(&performerApi) && performerApi.describe) {
        try { state.performer = ReadPerformerSection(nlohmann::json::parse(performerApi.describe())); }
        catch (const std::exception& error) {
            state.performer = {};
            ShellLog::Instance().Append(std::string("[addons] the performer's section: ") + error.what() + "\n");
        }
    }
    if (!state.performer.loaded) performerApi = {};
    // Optional: what the section's action keys play.
    qm_performer_action_call* const performerAction = state.performer.loaded ? performerAddon.Find<qm_performer_action_call>("qm_performer_action") : nullptr;
    if (!performerAction) state.performer.actions.clear();
    auto& section = state.performer;
    // The Player row is kept by the same keys in config.json, where it is the
    // setup every song plays, and in a pinned song's entry: the preset by its
    // value, CUSTOM, the amounts in play (read only under Custom, so a lit
    // preset plays its values as they are now), and the saved Custom under
    // CUSTOM_ and the same names.
    const auto fieldOf = [&](const std::string& id) -> const std::string& { return section.Find(id)->field; };
    // The preset saved under `key`, or -1.
    const auto presetIn = [&](const nlohmann::json& saved, const std::string& key) {
        const auto number = SavedNumber(saved, key);
        return number && *number >= 0 && *number <= std::numeric_limits<int>::max() ? section.PresetOf(static_cast<int>(*number)) : -1;
    };
    // The row saved in `saved`; one whose preset no value names is on `fallback`.
    const auto readRow = [&](const nlohmann::json& saved, int fallback) {
        PlayerRow row;
        row.preset = presetIn(saved, section.presetField);
        if (row.preset < 0) row.preset = fallback;
        row.custom = !section.rowIds.empty() && SavedNumber(saved, "CUSTOM").value_or(0) == 1;
        for (const auto& id : section.rowIds) {
            const auto amount = row.custom ? SavedNumber(saved, fieldOf(id)) : std::nullopt;
            row.values[id] = amount ? std::clamp(*amount, 0.0, 1.0) : section.Target(row.preset, id);
        }
        const bool kept = saved.contains("CUSTOM_" + section.presetField) ||
                          std::any_of(section.rowIds.begin(), section.rowIds.end(), [&](const auto& id) { return saved.contains("CUSTOM_" + fieldOf(id)); });
        if (!row.custom && kept) {
            PerformerCustom custom;
            custom.preset = presetIn(saved, "CUSTOM_" + section.presetField);
            if (custom.preset < 0) custom.preset = row.preset;
            for (const auto& id : section.rowIds) {
                const auto amount = SavedNumber(saved, "CUSTOM_" + fieldOf(id));
                custom.values[id] = amount ? std::clamp(*amount, 0.0, 1.0) : section.Target(custom.preset, id);
            }
            row.saved = std::move(custom);
        }
        return row;
    };
    const auto writeRow = [&](nlohmann::json& saved, const PlayerRow& row) {
        saved[section.presetField] = section.presetNumbers[static_cast<size_t>(row.preset)];
        saved["CUSTOM"] = row.custom ? 1 : 0;
        saved.erase("CUSTOM_" + section.presetField);
        for (const auto& id : section.rowIds) {
            saved[fieldOf(id)] = row.values.at(id);
            saved.erase("CUSTOM_" + fieldOf(id));
        }
        if (row.custom || !row.saved) return;
        saved["CUSTOM_" + section.presetField] = section.presetNumbers[static_cast<size_t>(row.saved->preset)];
        for (const auto& id : section.rowIds) {
            const auto amount = row.saved->values.find(id);
            saved["CUSTOM_" + fieldOf(id)] = amount != row.saved->values.end() ? amount->second : section.Target(row.saved->preset, id);
        }
    };
    // The row on screen, and putting one there.
    const auto shownRow = [&] {
        PlayerRow row{section.preset, section.custom, {}, section.customSaved};
        for (const auto& id : section.rowIds) row.values[id] = section.Find(id)->value;
        return row;
    };
    const auto applyRow = [&](const PlayerRow& row) {
        section.preset = row.preset;
        section.custom = row.custom;
        for (const auto& id : section.rowIds) section.Find(id)->value = row.values.at(id);
        section.customSaved = row.saved;
    };
    // HOTKEY_SETTINGS field and default per hotkey index: the app's, the
    // performer's, then the app's later ones, unbound by default.
    std::array<std::string, kHotkeys> hotkeyFields, hotkeyDefaults;
    for (size_t i = 0; i < kAppHotkeys; ++i) { hotkeyFields[i] = kHotkeyFields[i]; hotkeyDefaults[i] = kHotkeyDefaults[i]; }
    for (size_t i = 0; i < kLaterHotkeyFields.size(); ++i) hotkeyFields[kFirstLaterHotkey + i] = kLaterHotkeyFields[i];
    for (const auto& trigger : section.triggers)
        for (size_t k = 0; k < trigger.keyFields.size(); ++k) {
            hotkeyFields[trigger.firstKey + k] = trigger.keyFields[k];
            if (k < trigger.keyDefaults.size()) hotkeyDefaults[trigger.firstKey + k] = trigger.keyDefaults[k];
        }
    for (const auto& action : section.actions) { hotkeyFields[action.key] = action.field; hotkeyDefaults[action.key] = action.keyDefault; }
    state.hotkeys = hotkeyDefaults;
    std::unique_ptr<VirtualPianoPlayer> player;
    // Declared after the player it points at, so destroyed first.
    std::unique_ptr<MIDI2Key> live;
    std::unique_ptr<ConnectInput> connect;
    // Reports through Send. Its destructor kills the converter's process tree
    // so Transkun never outlives the app.
    audio_to_midi::Job converter;
    // The YouTube window, closed with the app the same way.
    audio_to_midi::Job browser;
    uint64_t liveMappings = 0;
    int liveTranspose = 0;
    int liveOctaves = 1;
    std::chrono::steady_clock::time_point volumeDue{};
    bool volumePending = false;
    std::vector<std::chrono::nanoseconds> scoreTimes;
    // config.json is parsed once; all reads and edits go through this copy,
    // and writes are debounced.
    nlohmann::json configJson;
    // Set only by a successful parse. Don't test configJson.is_object():
    // operator[] turns a null document into an object, and saving it would
    // overwrite a config that failed to parse.
    bool configLoaded = false;
    bool configDirty = false;
    std::chrono::steady_clock::time_point configDue{};
    // Debounce so a burst of edits becomes one write.
    constexpr auto configSettle = 400ms;
    // The performer's section in config.json, made an object if it is not one.
    const auto setupJson = [&]() -> nlohmann::json& {
        auto& saved = configJson[section.config];
        if (!saved.is_object()) saved = nlohmann::json::object();
        return saved;
    };
    // Set when the setup was read into a preset from before Custom: songs
    // pinned by the same upgrade may then give it its saved Custom.
    bool setupOnPreset = false;
    // Reads a section saved before Custom into the row it played: its preset
    // when the amounts sit on it, otherwise Custom on it with those amounts.
    // A slider following its estimate is saved as -1. True if it changed.
    const auto upgradeSetup = [&](nlohmann::json& saved) {
        if (!saved.is_object() || saved.contains("CUSTOM")) return false;
        bool changed = false;
        if (!section.presets.empty()) {
            PlayerRow row;
            row.preset = std::max(0, presetIn(saved, section.presetField));
            for (const auto& id : section.rowIds) {
                const auto amount = SavedNumber(saved, fieldOf(id));
                row.values[id] = amount ? std::clamp(*amount, 0.0, 1.0) : section.Target(row.preset, id);
            }
            row.custom = !section.OnPreset(row.preset, row.values);
            if (!row.custom)
                for (const auto& id : section.rowIds) row.values[id] = section.Target(row.preset, id);
            setupOnPreset = !row.custom;
            writeRow(saved, row);
            changed = true;
        }
        for (const auto& control : section.controls)
            if (!control.isSwitch && !control.isChoice && section.Estimated(control))
                if (const auto amount = SavedNumber(saved, control.field); amount && *amount != -1) { saved[control.field] = -1.0; changed = true; }
        return changed;
    };
    // The default, from inside the exe, where there is no config yet, and in
    // place of one that cannot be read: not JSON, cut short by a power cut, or
    // without both layouts' key mappings, which every note and save needs. That
    // one is kept aside as config.json.damaged, and the status bar says so.
    if (const auto defaults = bundle::DefaultConfig(); !defaults.empty()) {
        std::error_code unknown;
        if (!std::filesystem::exists(config_, unknown) && !unknown) WriteDurably(config_, defaults);
        else if (!unknown) {
            nlohmann::json saved;
            { std::ifstream stream(config_, std::ios::binary); saved = nlohmann::json::parse(stream, nullptr, false); }
            bool readable = saved.is_object();
            try {
                for (const char* layout : {"FULL", "LIMITED"})
                    if (readable) saved.at("KEY_MAPPINGS").at(layout).get<decltype(state.keyMappings)>();
            } catch (const std::exception&) { readable = false; }
            if (!readable)
                if (const auto aside = SetAside(config_); !aside.empty() && WriteDurably(config_, defaults)) {
                    state.resetNotice = "Settings were damaged and have been reset.";
                    ShellLog::Instance().Append("[settings] " + Utf8(config_.filename()) + " could not be read; kept as " +
                                                Utf8(aside.filename()) + ", and the defaults are in its place.\n");
                }
        }
    }
    // Each value is read on its own: one of the wrong kind falls back to its
    // default alone, named in the log and the status bar, and the values
    // around it still load.
    std::vector<std::string> unreadSettings;
    const auto setting = [&]<typename T>(const nlohmann::json& json, const std::string& key, T fallback) {
        bool wrong = false;
        auto value = SavedValue(json, key.c_str(), std::move(fallback), wrong);
        if (wrong) unreadSettings.push_back(key);
        return value;
    };
    // A member holding settings of its own, or null where it is missing or not an object.
    const auto group = [&](const char* key) -> const nlohmann::json* {
        const auto found = configJson.find(key);
        if (found == configJson.end()) return nullptr;
        if (!found->is_object()) { unreadSettings.push_back(key); return nullptr; }
        return &*found;
    };
    try {
        std::ifstream stream(config_);
        configJson = nlohmann::json::parse(stream);
        if (!configJson.is_object()) throw std::runtime_error("config.json is not a JSON object.");
        configLoaded = true;
        state.eightyEightKeys = setting(configJson, "SHELL_88_KEYS", true);
        // Read mappings before any field a hand-edited file might break;
        // without them the player's mappings are cleared and no note types.
        state.keyMappings = configJson.at("KEY_MAPPINGS").at(state.eightyEightKeys ? "FULL" : "LIMITED").get<decltype(state.keyMappings)>();
    } catch (const std::exception& error) { state.error = error.what(); }
    if (configLoaded) try {
        state.outRange = setting(configJson, "SHELL_OUT_RANGE", false);
        state.playbackDelay = std::clamp(setting(configJson, "SHELL_PLAYBACK_DELAY", 3), 0, 10);
        state.seekStep = std::clamp(setting(configJson, "SHELL_SEEK_STEP", 10), 1, 60);
        // The player accepts speeds from 0.05 to 8.
        state.speedMin = std::clamp(setting(configJson, "SHELL_SPEED_MIN", .25), .05, 1.0);
        state.speedMax = std::clamp(setting(configJson, "SHELL_SPEED_MAX", 2.0), 1.0, 8.0);
        state.shuffle = setting(configJson, "SHELL_SHUFFLE", false);
        state.loop = std::clamp(setting(configJson, "SHELL_LOOP", 0), 0, 2);
        state.holdBehind = setting(configJson, "SHELL_HOLD_BEHIND", true);
        if (const auto keys = configJson.find("HOTKEY_SETTINGS"); keys != configJson.end() && !keys->is_object()) unreadSettings.push_back("HOTKEY_SETTINGS");
        else if (keys != configJson.end())
            for (size_t i = 0; i < kHotkeys; ++i) {
                if (hotkeyFields[i].empty()) continue;
                state.hotkeys[i] = setting(*keys, hotkeyFields[i], hotkeyDefaults[i]);
                // A key the app types, saved before capture passed over it, is unbound.
                if (const int vk = NameToVK(state.hotkeys[i]); vk != 0 && IsNoteKey(vk, state.keyMappings)) {
                    state.hotkeys[i].clear();
                    if (keys->contains(hotkeyFields[i])) (*keys)[hotkeyFields[i]] = std::string();
                    continue;
                }
                // A default yields to a key the user bound elsewhere.
                if (keys->contains(hotkeyFields[i])) continue;
                for (size_t j = 0; j < kHotkeys; ++j)
                    if (j != i && !hotkeyFields[j].empty() && keys->contains(hotkeyFields[j]) &&
                        NameToVK(TextOf(*keys, hotkeyFields[j].c_str())) == NameToVK(state.hotkeys[i]))
                        state.hotkeys[i].clear();
            }
        state.sheetsFolder = PathFromText(setting(configJson, "SHELL_SHEETS_FOLDER", std::string()));
        state.sheetStylePage = PathFromText(setting(configJson, "SHELL_SHEET_STYLE_PAGE", std::string()));
        if (const auto* files = group("SHELL_SHEET_FILES")) {
            state.sheetImage = setting(*files, "image", true);
            state.sheetTextFile = setting(*files, "text", true);
            state.sheetPageFile = setting(*files, "page", true);
        }
        if (const auto* midi = group("MIDI_SETTINGS")) state.detectDrums = setting(*midi, "DETECT_DRUMS", true);
        if (const auto* transpose = group("AUTO_TRANSPOSE")) state.autoTranspose = setting(*transpose, "ENABLED", false);
        state.octaves = std::clamp(setting(configJson, "SHELL_OCTAVES", 1), 1, 5);
        state.fileSort = static_cast<FileSort>(std::clamp(setting(configJson, "SHELL_FILE_SORT", 0), 0, 2));
        state.descendingFiles = setting(configJson, "SHELL_FILE_DESCENDING", false);
        // Upstream's TIMING_VARIATION, NOTE_SKIP_CHANCE and EXTRA_DELAY keys are
        // ignored and left in the file.
        // Performer values live under section.config. Without the add-on they
        // are left untouched in the file.
        if (section.loaded) {
            const auto* found = group(section.config.c_str());
            const auto saved = found ? *found : nlohmann::json::object();
            section.on = setting(saved, "ENABLED", false);
            // The switches; the row and the other controls are shown from this
            // and the open song's entry once songs.json is read (showSong).
            for (auto& control : section.controls) {
                const auto value = saved.find(control.field);
                if (control.isSwitch && value != saved.end() && value->is_boolean()) control.value = value->get<bool>() ? 1 : 0;
            }
            if (found && upgradeSetup(configJson[section.config])) configDirty = true;
        }
        state.rememberPerSong = section.Remembers();
        // Triggers belong to the performer; with it off the trigger is the clock.
        state.trigger = section.on ? std::clamp(setting(configJson, "SHELL_TRIGGER", 0), 0, std::max(0, static_cast<int>(section.triggers.size()) - 1)) : 0;
        // Session state from the last run. Devices are reopened later, once a
        // player exists.
        if (const auto* session = group("SHELL_SESSION")) {
            state.velocity = setting(*session, "velocity", true);
            state.sustain = setting(*session, "sustain", true);
            state.liveChannel = std::clamp(setting(*session, "liveChannel", -1), -1, 15);
            state.speed = std::clamp(setting(*session, "speed", 1.0), state.speedMin, state.speedMax);
        }
        state.velocityModifier = setting(configJson, "VELOCITY_MODIFIER", std::string("alt"));
        if (state.velocityModifier != "alt" && state.velocityModifier != "ctrl" && state.velocityModifier != "shift") {
            state.velocityModifier = "alt";
            state.error = "VELOCITY_MODIFIER must be alt, ctrl or shift.";
        }
    } catch (const std::exception& error) { state.error = error.what(); }
    if (!unreadSettings.empty()) {
        std::string names;
        for (const auto& name : unreadSettings) names += (names.empty() ? "" : ", ") + name;
        ShellLog::Instance().Append("[settings] " + Utf8(config_.filename()) + ": " + names + " could not be read; the defaults are used.\n");
        if (state.error.empty()) state.error = "Some settings could not be read, and their defaults are used: " + names + ".";
    }
    const auto touchConfig = [&] {
        configDirty = true;
        configDue = std::chrono::steady_clock::now() + configSettle;
    };
    // Per-song settings keyed by file name. Kept out of config.json because it
    // grows with the library and config.json is hand-edited.
    const auto songsPath = config_.parent_path() / "songs.json";
    nlohmann::json songsJson = nlohmann::json::object();
    bool songsDirty = false;
    // A saved file read only in part is kept aside as .damaged and saved again
    // with what it held, and the status bar says so. Until a copy is kept, the
    // file is never saved over; each save tries to keep it first. False when
    // it could not be kept yet.
    const auto damagedFile = [&](const std::filesystem::path& path, const std::string& notice) {
        const auto aside = SetAside(path);
        ShellLog::Instance().Append("[settings] " + Utf8(path.filename()) + " could not be read whole" +
                                    (aside.empty() ? "; it could not be kept aside, so it is not saved over until it can be"
                                                   : "; kept as " + Utf8(aside.filename())) + ".\n");
        state.resetNotice += (state.resetNotice.empty() ? "" : " ") + notice;
        configDue = std::chrono::steady_clock::now();
        return !aside.empty();
    };
    // Set while a damaged file waits to be kept aside before it is saved over.
    bool songsAside = false, libraryAside = false;
    // Keeps a damaged file aside, if it still waits to be, before it is saved over.
    const auto keptAside = [](bool& waiting, const std::filesystem::path& path) {
        if (!waiting) return true;
        const auto aside = SetAside(path);
        if (aside.empty()) return false;
        ShellLog::Instance().Append("[settings] " + Utf8(path.filename()) + " kept as " + Utf8(aside.filename()) + ".\n");
        waiting = false;
        return true;
    };
    {
        // Every setting is a number; a song's entry of another kind, or a
        // setting, is left out alone.
        bool damaged = false;
        const auto saved = ReadSaved(songsPath, damaged);
        if (saved.is_object())
            for (const auto& [song, settings] : saved.items()) {
                if (!settings.is_object()) { damaged = true; continue; }
                auto& kept = songsJson[song] = nlohmann::json::object();
                for (const auto& [field, value] : settings.items())
                    if (value.is_number()) kept[field] = value;
                    else damaged = true;
            }
        if (damaged) { songsDirty = true; songsAside = !damagedFile(songsPath, "Some song settings could not be read."); }
    }
    // A song control at rest: its declared value, or -1 for a slider with an estimate.
    const auto atRest = [&](const PerformerControl& control, double value) {
        if (control.isChoice) return std::lround(value) == std::lround(control.rest);
        if (!control.estimateName.empty()) return value < 0;
        return std::lround(value * 100) == std::lround(control.rest * 100);
    };
    // The file name a key with a folder part falls back to (songSettings), or empty.
    const auto nameOf = [](const std::string& key) {
        auto name = Utf8(PathFromText(key).filename());
        return name == key ? std::string() : name;
    };
    // An entry left empty is erased, unless the song would then read the one
    // kept under its file name: then it stays, empty.
    const auto settleEntry = [&](const std::string& key) {
        const auto found = songsJson.find(key);
        if (found == songsJson.end() || !found->is_object() || !found->empty()) return;
        if (const auto name = nameOf(key); !name.empty() && songsJson.contains(name)) return;
        songsJson.erase(found);
        songsDirty = true;
    };
    // The setup's row: a song's row whose preset no value names is on its
    // preset, and a song's row the same as it was a copy of it.
    const auto setupRow = [&] {
        const auto setup = configJson.find(section.config);
        return readRow(setup != configJson.end() && setup->is_object() ? *setup : nlohmann::json::object(), 0);
    };
    // Reads an entry saved before pins, a copy of the whole row: one on its
    // preset, or the same as the setup's row, follows the setup, and one off
    // them pins the song to Custom with those amounts. Song controls at rest
    // are removed, and fields no control declares are left. True if it
    // changed anything; `pinned` is set when it pinned the song.
    const auto upgradeEntry = [&](nlohmann::json& entry, bool* pinned) {
        if (!entry.is_object()) return false;
        bool changed = false;
        if (!section.presets.empty() && !entry.contains("PINNED") &&
            (entry.contains(section.presetField) ||
             std::any_of(section.rowIds.begin(), section.rowIds.end(), [&](const auto& id) { return entry.contains(fieldOf(id)); }))) {
            const auto setup = setupRow();
            PlayerRow row;
            row.preset = presetIn(entry, section.presetField);
            if (row.preset < 0) row.preset = setup.preset;
            for (const auto& id : section.rowIds) {
                const auto amount = SavedNumber(entry, fieldOf(id));
                row.values[id] = amount ? std::clamp(*amount, 0.0, 1.0) : section.Target(row.preset, id);
            }
            // To the whole percent, as OnPreset compares.
            const bool asSetup = row.preset == setup.preset && std::all_of(section.rowIds.begin(), section.rowIds.end(), [&](const auto& id) {
                return std::lround(row.values.at(id) * 100) == std::lround(setup.values.at(id) * 100);
            });
            if (asSetup || section.OnPreset(row.preset, row.values)) {
                entry.erase(section.presetField);
                for (const auto& id : section.rowIds) entry.erase(fieldOf(id));
            } else {
                row.custom = true;
                writeRow(entry, row);
                entry["PINNED"] = 1;
                if (pinned) *pinned = true;
            }
            changed = true;
        }
        for (const auto& control : section.controls)
            if (section.PerSong(control))
                if (const auto value = SavedNumber(entry, control.field); value && atRest(control, *value)) {
                    entry.erase(control.field);
                    changed = true;
                }
        return changed;
    };
    if (section.loaded) {
        std::vector<std::string> keys, pinned;
        for (auto entry = songsJson.begin(); entry != songsJson.end(); ++entry) {
            keys.push_back(entry.key());
            bool pins = false;
            if (upgradeEntry(entry.value(), &pins)) songsDirty = true;
            if (pins) pinned.push_back(entry.key());
        }
        // Names first, so a path's entry left empty knows whether one is kept.
        for (const auto& key : keys) if (nameOf(key).empty()) settleEntry(key);
        for (const auto& key : keys) if (!nameOf(key).empty()) settleEntry(key);
        // A setup read into a preset takes as its saved Custom the song this
        // pinned farthest from every preset's values, earlier ones too, over
        // the row in whole percents; past half a percent, the first on a tie.
        if (const auto setup = configJson.find(section.config); setupOnPreset && setup != configJson.end() && !readRow(*setup, 0).saved) {
            const auto distance = [&](const std::map<std::string, double>& values) {
                long nearest = std::numeric_limits<long>::max();
                for (int preset = 0; preset < static_cast<int>(section.presets.size()); ++preset) {
                    auto sets = section.presetEarlier[static_cast<size_t>(preset)];
                    sets.emplace_back();
                    for (const auto& set : sets) {
                        long apart = 0;
                        for (const auto& id : section.rowIds) {
                            const auto earlier = set.find(id);
                            apart += std::abs(std::lround(values.at(id) * 100) -
                                              std::lround((earlier == set.end() ? section.Target(preset, id) : earlier->second) * 100));
                        }
                        nearest = std::min(nearest, apart);
                    }
                }
                return nearest;
            };
            std::optional<PlayerRow> farthest;
            long farthestBy = 0;
            for (const auto& key : pinned) {
                const auto row = readRow(songsJson[key], 0);
                if (const long by = distance(row.values); by > farthestBy) { farthest = row; farthestBy = by; }
            }
            if (farthest) {
                auto row = readRow(*setup, 0);
                row.saved = PerformerCustom{farthest->preset, farthest->values};
                writeRow(setupJson(), row);
                configDirty = true;
            }
        }
    }
    // Favourites, playlists and the queue by path, in library.json for the same reason.
    const auto libraryPath = config_.parent_path() / "library.json";
    bool libraryDirty = false;
    // The MIDI folder library.json keeps its songs below; empty until a file or a scan names one.
    std::filesystem::path libraryRoot;
    {
        bool damaged = false;
        const auto saved = ReadSaved(libraryPath, damaged);
        if (saved.is_object()) {
            auto lists = std::make_shared<LibraryLists>();
            ReadLibrary(saved, *lists, state.openList, libraryRoot, damaged);
            state.library = std::move(lists);
        }
        if (damaged) { libraryDirty = true; libraryAside = !damagedFile(libraryPath, "Some favourites and playlists could not be read."); }
    }
    // Song lengths, read once and kept in lengths.json beside it: each stands
    // while its file keeps the size and time it was read at. Only a record of
    // what reading found, so a damaged one is read again from the songs.
    const auto lengthsPath = config_.parent_path() / "lengths.json";
    auto lengths = [&] { bool damaged = false; return ReadLengths(ReadSaved(lengthsPath, damaged)); }();
    bool lengthsDirty = false;
    // The songs a length was read for whose lengths are not yet back, and the
    // run reading them; the songs from outside the library read this session.
    PathSet lengthsPending, outsideRead;
    uint64_t lengthsRun = 0;
    // Each song in `files` takes the length known for it, and one from
    // outside the library its size and time too; true when any changed.
    const auto knownLengths = [&](std::vector<MidiEntry>& files) {
        bool changed = false;
        for (auto& file : files) {
            const auto known = lengths.find(file.path);
            if (known == lengths.end()) continue;
            const auto& [bytes, modified, seconds] = known->second;
            if (!file.outside && (bytes != file.bytes || modified != file.modified)) continue;
            if (file.seconds == seconds && file.bytes == bytes && file.modified == modified) continue;
            file.bytes = bytes; file.modified = modified; file.seconds = seconds;
            changed = true;
        }
        return changed;
    };
    const auto touchLibrary = [&] {
        libraryDirty = true;
        configDue = std::chrono::steady_clock::now() + configSettle;
    };
    // Changes go to a copy, so a snapshot the panel holds never changes under it.
    const auto editLibrary = [&](const std::function<void(LibraryLists&)>& edit) {
        auto lists = std::make_shared<LibraryLists>(*state.library);
        edit(*lists);
        if (*lists == *state.library) return;
        state.library = std::move(lists);
        // An emptied queue closes, and the panel shows the folder again.
        if (!ListShown(*state.library, state.openList)) state.openList = kFolderList;
        touchLibrary();
    };
    // Sends to the Recycle Bin on a thread of its own, one request after
    // another: where the bin cannot take a file, Windows asks first, and until
    // that is answered Stop and every other command would wait on the worker.
    // Each request reports through Send; stopped and joined when Run returns.
    std::mutex recycleMutex;
    std::condition_variable_any recycleWake;
    std::deque<std::function<void()>> recycleJobs;
    std::jthread recycling([&](std::stop_token token) {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(recycleMutex);
                if (!recycleWake.wait(lock, token, [&] { return !recycleJobs.empty(); })) return;
                job = std::move(recycleJobs.front());
                recycleJobs.pop_front();
            }
            job();
        }
    });
    const auto recycleAside = [&](std::function<void()> job) {
        { std::lock_guard lock(recycleMutex); recycleJobs.push_back(std::move(job)); }
        recycleWake.notify_one();
    };
    // Builds before Delete kept removed songs in a Trash: a hidden .trash folder
    // in the MIDI folder, and before that a Trash folder beside the config. As a
    // MIDI folder opens, what is left in either goes to the Recycle Bin, and each
    // folder goes once it is empty; a file the bin does not take stays, with its
    // folder, and is never deleted outright.
    const auto emptyOldTrash = [&](const std::filesystem::path& folder) {
        std::vector<std::filesystem::path> trashes, songs;
        for (const auto& trash : {folder.empty() ? std::filesystem::path() : folder / L".trash", config_.parent_path() / L"Trash"}) {
            std::error_code error;
            if (trash.empty() || !std::filesystem::is_directory(trash, error)) continue;
            trashes.push_back(trash);
            for (std::filesystem::directory_iterator each(trash, error), end; !error && each != end; each.increment(error))
                if (std::error_code kind; each->is_regular_file(kind)) songs.push_back(each->path());
        }
        if (trashes.empty()) return;
        recycleAside([trashes, songs] {
            // In one operation, so Windows asks once at most. Where some went and
            // some did not, a file in use stopped it: the rest go one at a time,
            // so it keeps only itself back.
            if (!songs.empty() && !Recycle(songs)) {
                std::vector<std::filesystem::path> left;
                for (const auto& song : songs) if (std::error_code error; std::filesystem::exists(song, error)) left.push_back(song);
                if (left.size() < songs.size()) for (const auto& song : left) Recycle({song});
            }
            // Only an empty folder is removed.
            for (const auto& trash : trashes) { std::error_code error; std::filesystem::remove(trash, error); }
        });
    };
    // Runs at the settle deadline, before anything rereads config.json, and on
    // shutdown. Each file reaches the disk before it replaces the old one
    // (WriteDurably), so a power cut leaves the one or the other whole; the
    // debounce keeps that wait to one per burst of edits. Each file is tried
    // whatever became of the others, and what failed is thrown together.
    const auto flushConfig = [&] {
        std::string failed;
        const auto fail = [&](const char* why) { failed += (failed.empty() ? "" : " ") + std::string(why); };
        if (songsDirty) {
            if (!keptAside(songsAside, songsPath)) fail("Cannot save the song settings until the damaged songs.json is kept aside.");
            else if (WriteDurably(songsPath, songsJson.dump(1) + '\n')) songsDirty = false;
            else fail("Cannot save the song settings.");
        }
        if (libraryDirty) {
            if (!keptAside(libraryAside, libraryPath)) fail("Cannot save the favourites and playlists until the damaged library.json is kept aside.");
            else if (WriteDurably(libraryPath, WriteLibrary(*state.library, state.openList, libraryRoot).dump(1) + '\n')) libraryDirty = false;
            else fail("Cannot save the favourites and playlists.");
        }
        // Only the songs listed now are kept. A failure is not shown: the
        // lengths are read again next time.
        if (lengthsDirty) {
            nlohmann::json saved = nlohmann::json::object();
            const auto keep = [&](const std::vector<MidiEntry>& files) {
                for (const auto& file : files)
                    if (const auto known = lengths.find(file.path); known != lengths.end())
                        saved[Utf8(file.path)] = {known->second.bytes, known->second.modified.time_since_epoch().count(), known->second.seconds};
            };
            keep(*state.files);
            keep(*state.outside);
            WriteDurably(lengthsPath, saved.dump() + '\n');
            lengthsDirty = false;
        }
        // Never overwrite a config that failed to parse.
        if (configDirty && !configLoaded) fail("The configuration was not loaded, so it cannot be saved.");
        else if (configDirty) {
            if (WriteDurably(config_, configJson.dump(4) + '\n')) configDirty = false;
            else fail("Cannot save the configuration.");
        }
        if (!failed.empty()) throw std::runtime_error(failed);
    };
    const auto stopPlayback = [&] {
        state.playbackCountdown = 0;
        shuffleAdvancePending = false;
        if (!player) return;
        player->should_stop.store(true, std::memory_order_release);
        SetEvent(player->command_event);
        player->playback_cv.notify_all();
        if (player->playback_thread && player->playback_thread->joinable()) player->playback_thread->join();
        player->playback_thread.reset();
        if (state.playing)
            state.position = std::clamp(player->get_adjusted_time().count() / 1e9, 0.0, state.duration);
        player->paused.store(true, std::memory_order_release);
        player->release_all_keys();
        state.playing = false;
    };
    const auto stopConnect = [&] {
        if (connect) { connect->Close(); connect.reset(); }
        state.midiConnect = false;
    };
    const auto stopLive = [&] {
        if (live) {
            live->SetActive(false);
            live->CloseDevice();
            if (state.liveActive && player) player->release_every_mapped_key();
            live.reset();
        }
        state.liveActive = false;
    };
    const auto cancelVolume = [&] {
        volumePending = false;
        state.autoVolumeCountdown = 0;
        state.autoVolumeFocusing = false;
    };
    const auto invalidateVolume = [&] {
        if (state.autoVolume || volumePending) state.autoVolumeNeedsCalibration = true;
        cancelVolume();
        state.autoVolume = false;
        if (player) player->enable_volume_adjustment.store(false, std::memory_order_release);
    };
    // The player swaps in performer settings between events, so applying them
    // doesn't stop playback.
    std::vector<qm_score_event> score;
    const auto applyPerformer = [&] {
        if (!player) return;
        nlohmann::json settings = nlohmann::json::object();
        if (!section.presetId.empty()) settings[section.presetId] = section.PresetNumber();
        bool holds = true;
        for (const auto& control : section.controls) {
            if (control.isSwitch) settings[control.id] = control.value != 0;
            // A choice hidden for this song rests on its first entry.
            else if (control.isChoice) settings[control.id] = control.noEstimate ? 0 : static_cast<int>(control.value);
            else settings[control.id] = section.Shown(control);
            if (control.holds) holds = control.value != 0;
        }
        // The host's own: whether the sustain pedal reaches the game or the port.
        settings["sustainHeard"] = state.sustain;
        player->set_performer_settings(settings.dump());
        player->trigger.store(state.TriggerSteps() ? VirtualPianoPlayer::Trigger::Tap : VirtualPianoPlayer::Trigger::Auto,
                              std::memory_order_release);
        player->tap_holds_notes.store(holds, std::memory_order_release);
    };
    // Recompute slider estimates for the current score. A negative estimate
    // means none for this song, and the slider or choice is hidden.
    const auto estimateSong = [&] {
        for (auto& control : section.controls) {
            if (control.estimateName.empty()) continue;
            const double estimate = performerApi.estimate
                ? performerApi.estimate(control.estimateName.c_str(), score.data(), score.size(), state.speed) : 0.0;
            control.noEstimate = estimate < 0 && !score.empty();
            control.estimate = std::clamp(estimate, 0.0, 1.0);
        }
    };
    // True if any slider follows its estimate, so a new estimate changes the take.
    const auto atEstimate = [&] {
        return std::any_of(section.controls.begin(), section.controls.end(), [](const auto& control) { return !control.isSwitch && control.value < 0; });
    };
    // A song's settings in songs.json are kept under its path in the MIDI
    // folder, or its whole path outside it; files from before kept them under
    // its file name alone, which a song of that name still reads.
    const auto songKey = [&](const std::filesystem::path& song) {
        const auto rest = Below(song, state.folder);
        return Utf8(rest.empty() ? song : rest);
    };
    // The settings kept for `song`, or null: under its key, its path in other
    // letter case, or its file name. Only with `own` does anything change: one
    // in other letter case moves to its key, one under its file name is copied
    // to its key, and a song with none gets an empty entry.
    const auto songSettings = [&](const std::filesystem::path& song, bool own) -> nlohmann::json* {
        const auto key = songKey(song);
        if (const auto found = songsJson.find(key); found != songsJson.end()) return found->is_object() ? &*found : nullptr;
        const auto path = PathFromText(key);
        for (auto each = songsJson.begin(); each != songsJson.end(); ++each)
            if (SamePath(PathFromText(each.key()), path)) {
                if (!own) return each->is_object() ? &*each : nullptr;
                auto settings = std::move(*each);
                songsJson.erase(each);
                songsDirty = true;
                auto& moved = songsJson[key] = std::move(settings);
                return moved.is_object() ? &moved : nullptr;
            }
        const auto named = songsJson.find(Utf8(song.filename()));
        if (!own) return named != songsJson.end() && named->is_object() ? &*named : nullptr;
        auto settings = named != songsJson.end() && named->is_object() ? *named : nlohmann::json::object();
        return &(songsJson[key] = std::move(settings));
    };
    // Settings kept under a file name alone, from before songs were kept by
    // path, move to the one listed song of that name, unless it has its own.
    // A name a song at the top of the folder has, or two songs share, stays.
    const auto adoptNamedEntries = [&] {
        std::vector<std::string> names;
        for (auto entry = songsJson.begin(); entry != songsJson.end(); ++entry)
            if (nameOf(entry.key()).empty()) names.push_back(entry.key());
        for (const auto& name : names) {
            const auto path = PathFromText(name);
            std::string target;
            int matches = 0;
            bool top = false;
            for (const auto& file : *state.files) {
                const auto key = songKey(file.path);
                if (SamePath(PathFromText(key), path)) { top = true; break; }
                if (Utf8(file.path.filename()) == name) { ++matches; target = key; }
            }
            if (top || matches != 1) continue;
            bool kept = false;
            for (auto each = songsJson.begin(); each != songsJson.end() && !kept; ++each) kept = SamePath(PathFromText(each.key()), PathFromText(target));
            if (kept) continue;
            auto settings = std::move(songsJson[name]);
            songsJson.erase(name);
            songsJson[target] = std::move(settings);
            songsDirty = true;
        }
    };
    // The open song's own entry, made if it has none (songSettings).
    const auto ownEntry = [&]() -> nlohmann::json& {
        auto* kept = songSettings(state.loaded, true);
        return kept ? *kept : (songsJson[songKey(state.loaded)] = nlohmann::json::object());
    };
    // The open song's entry while songs keep their own, or null.
    const auto openEntry = [&]() -> const nlohmann::json* {
        return state.rememberPerSong && !state.loaded.empty() ? songSettings(state.loaded, false) : nullptr;
    };
    // The section in config.json, or an empty one.
    const auto setupSaved = [&]() -> const nlohmann::json& {
        static const nlohmann::json none = nlohmann::json::object();
        const auto found = configJson.find(section.config);
        return found != configJson.end() && found->is_object() ? *found : none;
    };
    // What a control other than a switch or a row slider shows. A song control
    // takes the open song's value or its rest while songs keep their own, and
    // the setup's otherwise; a slider following its estimate shows -1 there,
    // and one whose estimate is off always takes the setup's value. Others
    // take the setup's value or their own.
    const auto shownValue = [&](const PerformerControl& control, const nlohmann::json* entry) {
        const auto read = [&](const nlohmann::json& saved) -> std::optional<double> {
            const auto value = SavedNumber(saved, control.field);
            if (!value) return std::nullopt;
            if (control.isChoice) return std::clamp(std::round(*value), 0.0, static_cast<double>(control.choices.size() - 1));
            return std::clamp(*value, control.estimateName.empty() ? 0.0 : -1.0, 1.0);
        };
        const bool estimated = !control.isChoice && !control.estimateName.empty();
        if (estimated && !section.Estimated(control)) return std::max(0.0, read(setupSaved()).value_or(0));
        if (section.PerSong(control) && state.rememberPerSong) return (entry ? read(*entry) : std::nullopt).value_or(control.rest);
        if (estimated) return -1.0;
        return read(setupSaved()).value_or(control.rest);
    };
    // Shows what the files hold for the open song, and writes nothing: the
    // setup's Player row or, while the song is pinned, its own, with the keys
    // it lacks taken from the setup's and a saved Custom of its own; and the
    // song controls kept for it.
    const auto showSong = [&] {
        if (!section.loaded) return;
        const auto& setup = setupSaved();
        const auto* entry = openEntry();
        section.pinned = !section.presets.empty() && entry && SavedNumber(*entry, "PINNED").value_or(0) == 1;
        if (!section.presets.empty()) {
            const auto setupRow = readRow(setup, 0);
            if (!section.pinned) applyRow(setupRow);
            else {
                auto merged = *entry;
                std::vector<std::string> keys{section.presetField, "CUSTOM"};
                for (const auto& id : section.rowIds) keys.push_back(fieldOf(id));
                for (const auto& key : keys) if (!merged.contains(key) && setup.contains(key)) merged[key] = setup[key];
                applyRow(readRow(merged, setupRow.preset));
            }
        }
        for (auto& control : section.controls)
            if (!control.isSwitch && !section.InRow(control.id)) control.value = shownValue(control, entry);
    };
    // A change to the Player row: kept in the open song's entry while it is
    // pinned, otherwise in the setup.
    const auto saveRow = [&] {
        if (section.pinned && !state.loaded.empty()) {
            auto& entry = ownEntry();
            writeRow(entry, shownRow());
            entry["PINNED"] = 1;
            songsDirty = true;
        } else {
            writeRow(setupJson(), shownRow());
            touchConfig();
        }
        applyPerformer();
    };
    // One key of the setup.
    const auto saveSetting = [&](const std::string& field, nlohmann::json value) {
        setupJson()[field] = std::move(value);
        touchConfig();
    };
    // A change to a control other than a switch or a row slider. A song
    // control is kept in the open song's entry while songs keep their own,
    // removed from it at rest, and kept nowhere with no song open; otherwise
    // there is one value for every song. A slider following its estimate has
    // no such value, and one whose estimate is off has only that one.
    const auto saveControl = [&](const PerformerControl& control) {
        const bool estimated = !control.isChoice && !control.estimateName.empty();
        const auto value = control.isChoice ? nlohmann::json(static_cast<int>(control.value)) : nlohmann::json(control.value);
        if (estimated && !section.Estimated(control)) saveSetting(control.field, value);
        else if (!section.PerSong(control) || !state.rememberPerSong) { if (!estimated) saveSetting(control.field, value); }
        else if (!state.loaded.empty()) {
            const auto key = songKey(state.loaded);
            const bool had = songsJson.contains(key);
            auto& entry = ownEntry();
            const auto before = entry;
            if (atRest(control, control.value)) entry.erase(control.field);
            else entry[control.field] = value;
            if (!had || entry != before) songsDirty = true;
            settleEntry(key);
        }
        applyPerformer();
    };
    // The first snapshot shows the Player row the files hold.
    showSong();
    Publish(state);
    // The playback thread reads the loop as it plays, so a change applies at once.
    const auto applyLoop = [&] {
        if (!player) return;
        player->loop_start_ns.store(static_cast<int64_t>(state.loopStart * 1e9), std::memory_order_release);
        player->loop_end_ns.store(static_cast<int64_t>(state.loopEnd * 1e9), std::memory_order_release);
        player->loop.store(state.loop == 1 ? VirtualPianoPlayer::Loop::Song : state.loop == 2 ? VirtualPianoPlayer::Loop::Section
                                                                                                : VirtualPianoPlayer::Loop::Off, std::memory_order_release);
    };
    // Only the worker writes the player's clock fields.
    // Hold key state as last read by the poller below.
    std::atomic<bool> holdDown{false};
    // `fresh` is false where playback carries on after a seek, which keeps the
    // window the song was sending keys to; a song that starts finds its own.
    const auto startPlayback = [&](bool fresh = true) {
        if (!state.typingAcknowledged) throw std::runtime_error("Read the typing warning in the app before starting output.");
        if (!player || state.loaded.empty() || state.rows.empty() || state.duration <= 0) return;
        // With a Hold trigger, play only while the key is down, whatever the
        // caller (Play, countdown, resumed load, shuffle).
        if (state.TriggerPlays() && !holdDown.load(std::memory_order_acquire)) return;
        stopConnect();
        if (fresh) player->hold_target.store(nullptr, std::memory_order_release);
        // Speed scales the player's clock, not the event times; a change while
        // playing is a single atomic store the playback thread picks up.
        applyPerformer();
        applyLoop();
        player->requested_speed.store(state.speed, std::memory_order_release);
        player->current_speed = state.speed;
        player->total_adjusted_time = std::chrono::nanoseconds(static_cast<int64_t>(state.position * 1e9));
        const auto next = std::lower_bound(player->note_events.begin(), player->note_events.end(),
            player->total_adjusted_time, [](const auto& event, auto time) { return event.time < time; });
        player->buffer_index.store(static_cast<size_t>(next - player->note_events.begin()));
        player->last_resume_tsc = __rdtsc();
        player->playback_start_time = player->last_resume_tsc;
        // Cleared before the end check can see this start: a song that ended
        // leaves it set, and a replay or the next song would stop at once.
        player->song_done.store(false, std::memory_order_release);
        player->playback_started.store(true, std::memory_order_release);
        player->should_stop.store(false, std::memory_order_release);
        player->paused.store(true, std::memory_order_release);
        ResetEvent(player->command_event);
        player->toggle_play_pause();
        state.playing = true;
    };
    // Whether a note, transposed, has a key of its own in the layout.
    const auto onKeys = [&](int note) {
        if (note < KeysLow(state.eightyEightKeys) || note > KeysHigh(state.eightyEightKeys)) return false;
        const auto found = state.keyMappings.find(NoteName(note));
        return found != state.keyMappings.end() && !found->second.empty();
    };
    const auto applyMappings = [&] {
        if (!player) return;
        // Release under the old map before reaching here. Both attacks and
        // releases retain the same source-note identity after transposition.
        for (int note = 0; note < 128; ++note) {
            // The fold is here, on the transposed note, in both layouts: notes
            // Transpose pushes off the keys fold back as well as the file's own.
            int target = note + state.transpose;
            if (state.outRange)
                target = FoldOntoKeys(std::clamp(target, 0, 127), KeysLow(state.eightyEightKeys), KeysHigh(state.eightyEightKeys));
            const auto found = target >= 21 && target <= 108 ? state.keyMappings.find(NoteName(target)) : state.keyMappings.end();
            auto& mappings = state.eightyEightKeys ? player->full_key_mappings : player->limited_key_mappings;
            mappings[NoteName(note)] = found == state.keyMappings.end() ? "" : found->second;
            player->pressed_keys.try_emplace(NoteName(note), false);
            // Octave doubles, by the note whose key each lands on. One off the
            // keys is dropped, never folded.
            static constexpr int kOctaveOffsets[5][4]{{}, {12}, {-12, 12}, {-12, 12, 24}, {-24, -12, 12, 24}};
            auto& doubles = player->octave_doubles[note];
            doubles.fill(-1);
            size_t count = 0;
            for (const int offset : kOctaveOffsets[std::clamp(state.octaves, 1, 5) - 1]) {
                const int doubled = note + offset;
                if (offset && doubled >= 0 && doubled < 128 && onKeys(doubled + state.transpose))
                    doubles[count++] = static_cast<int8_t>(doubled);
            }
        }
    };
    const auto applyVelocityModifier = [&] {
        if (!player) return;
        midi::Config::getInstance().playback.velocityModifier = state.velocityModifier;
        player->apply_velocity_modifier();
        state.velocityModifierConflicts = player->velocity_modifier_conflicts();
    };
    // Live input needs a player without a file loaded: the mappings and velocity
    // settings come from the config, not from the score.
    const auto ensurePlayer = [&] {
        if (!player) {
            // The player parses config.json itself; flush pending edits first.
            // Only config.json has to reach the disk; another file that could
            // not is tried again at the next flush and reported then.
            try { flushConfig(); } catch (const std::exception&) { if (configDirty) throw; }
            player = std::make_unique<VirtualPianoPlayer>(false, config_);
            player->enable_velocity_keypress = state.velocity;
            player->currentSustainMode = state.sustain ? SustainMode::SPACE_DOWN : SustainMode::IG;
            player->eightyEightKeyModeActive = state.eightyEightKeys;
            // applyMappings folds; the player's own fold, before Transpose, would fold twice.
            player->ENABLE_OUT_OF_RANGE_TRANSPOSE = false;
            if (section.loaded) player->set_performer(&performerApi);
            player->performer_action = performerAction;
            player->performer_on = section.on;
            player->hold_behind = state.holdBehind;
            // An output left on MIDI waits for its port there, typing nothing.
            if (state.outputMidi) player->set_output_target(VirtualPianoPlayer::OutputTarget::MidiDevice);
            applyMappings();
            applyVelocityModifier();
        }
    };
    const auto applyWootingSettings = [&] {
        const auto& configured = midi::Config::getInstance().wooting;
        state.wootingTriggerThreshold = configured.TRIGGER_THRESHOLD;
        state.wootingShiftAmount = configured.SHIFT_AMOUNT;
        state.wootingVelocitySensitivity = configured.VELOCITY_SENSITIVITY;
        state.wootingMinVelocity = configured.MIN_VELOCITY;
        state.wootingPedalKeys = {configured.SUSTAIN_PEDAL_KEY, configured.SOSTENUTO_PEDAL_KEY, configured.SOFT_PEDAL_KEY};
        SetWootingAnalogSettings(WootingAnalogSettingsFromConfig());
    };
    const auto applyCurve = [&] {
        if (!player || state.curves.empty()) return;
        auto& custom = midi::Config::getInstance().playback.customVelocityCurves;
        custom.clear();
        for (size_t i = midi::kBuiltinVelocityCurves; i < state.curves.size(); ++i)
            custom.push_back({state.curves[i].name, state.curves[i].thresholds});
        const auto& edit = state.comparingCurve ? state.previousCurve : state.curve;
        if (VelocityEdited(edit) || state.comparingCurve) {
            const auto& preset = state.comparingCurve ? state.previousPreset : state.curves[edit.preset];
            custom.push_back({"Shell preview", VelocityThresholds(preset, edit)});
            player->setVelocityCurveIndex(midi::kBuiltinVelocityCurves + custom.size() - 1);
        } else player->setVelocityCurveIndex(edit.preset);
        g_sustainCutoff = state.sustainCutoff;
    };
    // Derive the built-in curves from the player's mapping API so there is no
    // second copy of the preset constants.
    try {
        ensurePlayer();
        applyWootingSettings();
        state.volumeDownKey = midi::Config::getInstance().hotkeys.VOLUME_DOWN_KEY;
        state.volumeUpKey = midi::Config::getInstance().hotkeys.VOLUME_UP_KEY;
        state.sustainKey = midi::Config::getInstance().hotkeys.SUSTAIN_KEY;
        state.sostenutoKey = midi::Config::getInstance().hotkeys.SOSTENUTO_KEY;
        state.volumeInitial = midi::Config::getInstance().volume.INITIAL_VOLUME;
        const std::string keys = "1234567890qwertyuiopasdfghjklzxc";
        for (size_t i = 0; i < midi::kBuiltinVelocityCurves; ++i) {
            VelocityPreset preset{player->getVelocityCurveName(static_cast<midi::VelocityCurveType>(i))};
            player->setVelocityCurveIndex(i);
            for (int input = 1; input <= 127; ++input) {
                const size_t output = keys.find(player->getVelocityKey(input));
                for (size_t bucket = output; bucket < 32; ++bucket) preset.thresholds[bucket] = input;
            }
            state.curves.push_back(std::move(preset));
        }
        for (const auto& custom : midi::Config::getInstance().playback.customVelocityCurves)
            state.curves.push_back({custom.name, custom.velocityValues});
        if (configJson.contains("SHELL_VELOCITY")) {
            const auto& saved = configJson.at("SHELL_VELOCITY");
            // Custom presets are indexed after the built-ins. "builtins" records
            // how many there were when saved (5 if absent), so rebase the index.
            size_t preset = saved.value("preset", size_t{1});
            const size_t builtins = saved.value("builtins", size_t{5});
            if (preset >= builtins) preset = preset - builtins + midi::kBuiltinVelocityCurves;
            state.curve.preset = std::min(preset, state.curves.size() - 1);
            state.curve.sensitivity = std::clamp(saved.value("sensitivity", 0.f), -50.f, 50.f);
            state.curve.contrast = std::clamp(saved.value("contrast", 0.f), 0.f, 100.f);
            if (saved.contains("anchors")) {
                for (const auto& point : saved.at("anchors")) {
                    if (!point.is_array() || point.size() != 2) throw std::runtime_error("Invalid saved velocity anchor.");
                    state.curve.anchors.push_back({point[0].get<float>(), point[1].get<float>()});
                }
                state.curve.anchors = VelocityLegalAnchors(std::move(state.curve.anchors));
            } else if (saved.contains("samples")) {
                // Legacy format: 32 sampled values, converted to anchors.
                const auto samples = saved.at("samples").get<std::array<float, 32>>();
                for (int i = 0; i < 32; ++i) {
                    if (!std::isfinite(samples[i])) throw std::runtime_error("Invalid saved velocity response.");
                    state.curve.anchors.push_back({i / 31.f, std::clamp(samples[i], 0.f, 1.f)});
                }
                state.curve.anchors = VelocityLegalAnchors(
                    velocity_detail::Simplify(state.curve.anchors, .004f));
            }
            state.sustainCutoff = std::clamp(saved.value("sustainCutoff", 64), 0, 127);
        }
        applyCurve();
    } catch (const std::exception& error) {
        state.curve = {};
        if (state.curves.size() >= midi::kBuiltinVelocityCurves) applyCurve();
        state.error = error.what();
    }
    // MIDI input and output from the last session, reopened when the panel's
    // first scan lists them. A missing device stays saved without raising an error.
    std::wstring restoreInput, restoreOutput;
    // Their groups, which find one plugged into another USB port.
    std::wstring restoreInputGroup, restoreOutputGroup;
    bool restoreInputActive = true, restoreOutputMidi = false;
    // The input was on MidiConnect, which it reopens through.
    bool restoreInputConnect = false;
    if (const auto session = configJson.find("SHELL_SESSION"); session != configJson.end() && session->is_object()) {
        try {
            restoreInput = PathFromJson(*session, "liveDevice").wstring();
            restoreInputGroup = PathFromJson(*session, "liveDeviceGroup").wstring();
            restoreInputActive = session->value("liveActive", true);
            restoreInputConnect = session->value("midiConnect", false);
            restoreOutput = PathFromJson(*session, "outputDevice").wstring();
            restoreOutputGroup = PathFromJson(*session, "outputDeviceGroup").wstring();
            restoreOutputMidi = session->value("outputMidi", false);
            if (const auto name = NamedMidiPortName(NamedMidiPortId(PathFromJson(*session, "outputPortName").wstring()));
                !name.empty()) state.outputPortName = Utf8(std::filesystem::path(name));
        } catch (const std::exception&) { restoreInput.clear(); restoreOutput.clear(); }
    }
    // An output left on MIDI comes back on MIDI, waiting for its port.
    state.outputMidi = restoreOutputMidi && !restoreOutput.empty();
    // The user's own input and output choices, counted. A scan's reopen that
    // finds a choice made after it was queued leaves that choice alone.
    // noReopen: no scan's reopen is queued.
    constexpr uint64_t noReopen = ~uint64_t{0};
    uint64_t inputChoices = 0, outputChoices = 0, inputReopenAt = noReopen, outputReopenAt = noReopen;
    // A device whose reopen failed and said so. Each scan tries it again, quietly,
    // until a reopen or a choice succeeds.
    std::wstring quietInput, quietOutput;
    // The chosen input's and output's groups, which tell a failed enumeration
    // from an unplugged device.
    KeptGroup liveGroup, outputGroup;
    // The output and the input a scan has queued to reopen, each waiting until
    // that reopen runs.
    std::wstring reopeningOutput, reopeningInput;
    // An output that stopped waiting when Keystrokes was chosen, with its
    // group; choosing MIDI waits for it again. It stays saved meanwhile, and
    // one saved on Keystrokes that the first scan does not list stops waiting.
    std::wstring pausedOutput, pausedOutputGroup;
    bool startupOutput = !restoreOutput.empty() && !restoreOutputMidi;
    static constexpr const char* kInputBusy = "That MIDI input is in use by another program.";
    // An output whose port goes away waits to reopen like one missing at
    // startup. Its target stays, so on MIDI the song and live playing are
    // dropped rather than typed into whatever window is in front.
    const auto loseOutput = [&](const std::wstring& group) {
        ensurePlayer();
        restoreOutput = state.outputDevice;
        restoreOutputGroup = group;
        restoreOutputMidi = state.outputMidi;
        player->drop_midi_output();
        state.outputDevice.clear();
        state.error = "MIDI output disconnected.";
    };
    // An input that goes away mid-session waits in restoreInput the same way.
    // A transport that stops delivering reports its id from its own thread,
    // and the next scan closes that input as gone.
    std::mutex lostMutex;
    std::wstring lostReport;
    SetMidiInputLostHandler([&](const std::wstring& id) {
        { std::lock_guard lock(lostMutex); lostReport = id; }
        Send({Action::LiveScan});
    });
    struct LostHandlerGuard { ~LostHandlerGuard() { SetMidiInputLostHandler({}); } } lostHandlerGuard;
    VelocityHistory curveHistory;
    curveHistory.Reset(state.curve);
    // The converter ships separately: a release installs it by unzipping into
    // addons, a source build by running its setup script.
    static constexpr const char* kConverterMissing =
        "The converter is an add-on. Unzip it into the addons folder, or for a build from source run tools\\mp3-to-midi\\setup.cmd.";
    const auto converterInstall = [] { return audio_to_midi::FindInstall(bundle::DataDirectory()); };
    // The converter thread reports through Send and never touches state.
    const auto reportConversion = [this](const audio_to_midi::Status& status) {
        Send({Action::ConvertProgress, {}, 0, static_cast<size_t>(status.kind), false, 0, status.text});
    };
    // Files saved so far by a playlist conversion, reported on cancel.
    size_t convertedCount = 0;
    // The log line last written about the converter: one is written at start
    // and on each change, so a report says where the converter was looked for
    // and why Install came back.
    std::string converterLogged;
    const auto readConverter = [&] {
        const auto install = converterInstall();
        state.youtubeSignedIn = install.SignedIn();
        state.converterInstalled = install.Found();
        state.converterCanSetUp = install.CanSetUp();
        state.converterEngine = install.engine;
        state.converterEngines = install.engines;
        std::string line;
        if (install.Found()) line = "Installed in " + Utf8(install.script.parent_path()) + ".";
        else if (install.script.empty()) line = "No converter add-on in " + Utf8(bundle::DataDirectory() / L"addons" / L"converter") + ".";
        else {
            line = "Not installed in " + Utf8(install.script.parent_path()) + ": ";
            for (size_t i = 0; i < install.unready.size(); ++i) line += (i ? "; " : "") + install.unready[i];
            line += ".";
        }
        if (line != converterLogged) ShellLog::Instance().Append("[convert] " + line + "\n");
        converterLogged = std::move(line);
    };
    readConverter();
    // The YouTube window reports through Send, as the converter does.
    const auto reportBrowse = [this](const audio_to_midi::Status& status) {
        Send({Action::BrowseProgress, {}, 0, static_cast<size_t>(status.kind), false, 0, status.text});
    };
    // What waits to convert, from the YouTube window, the link field and Choose
    // audio file, in order. key is the window's (audio_to_midi::ItemKey, or
    // FileKey for a file), so one video is queued once however it was picked.
    // A list's videos are items, written to listFile for --list; a link with
    // playlist set is read whole by convert.py. folder is the list's own folder.
    // dest is the folder in the MIDI folder that Save to chose ("" for the MIDI
    // folder itself), which goes before folder; artist is the channel it was
    // chosen for.
    struct QueueEntry {
        std::string key, title, kind, link;
        std::filesystem::path file, listFile;
        std::string folder;
        std::optional<std::string> dest;
        std::string artist;
        bool playlist = false, again = false;
        std::vector<std::pair<std::string, std::string>> items;
    };
    std::deque<QueueEntry> waitingConversions;
    std::optional<QueueEntry> running;
    // The running entry's progress: done of total list videos, the one being
    // converted (childId), those finished, and how it is to end. phase,
    // percent and children are what the window was last told, for a window
    // that asks again (a duplicate pick, the catch-up after its hello).
    struct RunState {
        int done = 0, total = 0, failed = 0, existing = 0;
        std::string childId;
        std::set<std::string> finished;
        bool blocked = false, dropped = false;
        std::string shortReason;
        std::string phase;
        int percent = -1;
        std::map<std::string, nlohmann::json> children;
    } run;
    // The window's protocol, 0 until its hello; its skin, as BrowseTheme sent
    // it; and the CPU share a run starts with.
    int browseProtocol = 0;
    std::string browseSkin;
    // The folders Save to offers, as the last walk of the MIDI folder found them.
    std::vector<std::string> libraryFolders;
    // The skin and folders the open window's init carried, and words for an
    // open window that has not said hello; each is sent after it does.
    std::string initSkin;
    std::vector<std::string> initFolders;
    std::string browseSearch;
    int converterCpu = 75;
    // The YouTube button started the setup, or was pressed during the app's
    // sign-in: the window opens once that ends, searching for the words.
    bool browseAfterSetup = false;
    std::string browseAfterSetupWords;
    // Numbers the list files, from the clock so another instance's are not reused.
    uint64_t listNumber = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto wide = [](const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())).native(); };
    // Titles from the converter may not be UTF-8; they are replaced, never thrown on.
    const auto jsonText = [](const nlohmann::json& json) { return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace); };
    // A line for the window's stdin, in protocol 2 only: an older window reads none.
    const auto tellWindow = [&](const std::string& key, const nlohmann::json& message) {
        if (state.browsingYouTube && browseProtocol == 2) browser.Write(key, jsonText(message));
    };
    const auto tellItem = [&](const QueueEntry& entry, const char* itemState, nlohmann::json fields) {
        fields["type"] = "item";
        fields["key"] = entry.key;
        fields["kind"] = entry.kind;
        fields["title"] = entry.title;
        fields["state"] = itemState;
        if (entry.dest) fields["dest"] = *entry.dest;
        tellWindow("item:" + entry.key, fields);
    };
    // The running entry's item fields as they stand.
    const auto runningFields = [&] {
        nlohmann::json fields{{"position", 0}, {"phase", run.phase},
                              {"percent", run.percent >= 0 ? nlohmann::json(run.percent) : nlohmann::json()}};
        if (run.total) {
            fields["done"] = run.done;
            fields["total"] = run.total;
            fields["failed"] = run.failed;
        }
        return fields;
    };
    // One video of the list keyed parent. A finished one of the running list
    // is kept for the catch-up.
    const auto tellChild = [&](const std::string& parent, const std::string& id, const char* childState, nlohmann::json fields) {
        fields["type"] = "child";
        fields["parent"] = parent;
        fields["key"] = "v:" + id;
        fields["state"] = childState;
        if (running && running->key == parent && std::string_view(childState) != "converting") run.children[id] = fields;
        tellWindow("child:" + parent + "/v:" + id, fields);
    };
    const auto tellPositions = [&] {
        state.conversionsWaiting = waitingConversions.size();
        for (size_t i = 0; i < waitingConversions.size(); ++i) tellItem(waitingConversions[i], "queued", {{"position", i + 1}});
    };
    // What a window that has just said hello missed.
    const auto tellAll = [&] {
        if (running) {
            tellItem(*running, "converting", runningFields());
            for (const auto& [id, message] : run.children) tellWindow("child:" + running->key + "/v:" + id, message);
        }
        tellPositions();
        if (state.conversionsPaused) tellWindow("pause", {{"type", "paused"}, {"reason", state.pausedReason}});
        // Folders found since the init.
        if (libraryFolders != initFolders) {
            tellWindow("folders", {{"type", "folders"}, {"folders", libraryFolders}});
            initFolders = libraryFolders;
        }
        // A theme or words sent before the hello.
        if (browseSkin != initSkin) {
            const auto skin = nlohmann::json::parse(browseSkin, nullptr, false);
            if (skin.is_object()) tellWindow("theme", {{"type", "theme"}, {"skin", skin}});
            initSkin = browseSkin;
        }
        if (!browseSearch.empty()) tellWindow("search", {{"type", "search"}, {"q", std::exchange(browseSearch, {})}});
    };
    // The queue waits for a sign-in or a MIDI folder.
    const auto pause = [&](const char* reason) {
        state.conversionsPaused = true;
        state.pausedReason = reason;
        tellWindow("pause", {{"type", "paused"}, {"reason", reason}});
    };
    const auto removeList = [](const std::filesystem::path& listFile) {
        if (listFile.empty()) return;
        std::error_code error;
        std::filesystem::remove(listFile, error);
        std::filesystem::remove(listFile.native() + L".skip", error);
    };
    // Starts an entry; the converter is free. One that cannot start is told failed.
    const auto startEntry = [&](QueueEntry entry) {
        const auto fail = [&](const std::string& status, const char* reason = "Conversion failed.") {
            state.conversionFailed = true;
            state.conversionStatus = status;
            tellItem(entry, "failed", {{"reason", reason}});
        };
        const auto install = converterInstall();
        if (!install.Found()) {
            readConverter();   // refreshes converterCanSetUp for the popup
            fail(kConverterMissing);
            return;
        }
        // A dest that would leave the MIDI folder, through a junction say, or
        // leave no room for a file name is refused before anything runs;
        // convert.py checks both again.
        if (entry.dest) {
            const auto names = audio_to_midi::ParseDest(*entry.dest);
            const auto folder = names ? audio_to_midi::SafeDestination(state.folder, *names) : std::nullopt;
            if (!folder) {
                fail("Folder not allowed.", "Folder not allowed.");
                return;
            }
            if ((*folder / L"_.mid").native().size() > audio_to_midi::kLongestPath) {
                fail("Path too long.", "Path too long.");
                return;
            }
        }
        // A list's videos go to convert.py in a file:
        // {"title", "folder", "dest", "artist", "items": [{"id", "title"}]},
        // with dest and artist only when the pick had them.
        if (!entry.items.empty()) {
            nlohmann::json list{{"title", entry.title}, {"folder", entry.folder.empty() ? nlohmann::json() : nlohmann::json(entry.folder)},
                                {"items", nlohmann::json::array()}};
            if (entry.dest) list["dest"] = *entry.dest;
            if (!entry.artist.empty()) list["artist"] = entry.artist;
            for (const auto& [id, title] : entry.items) list["items"].push_back(nlohmann::json{{"id", id}, {"title", title}});
            std::error_code error;
            entry.listFile = std::filesystem::temp_directory_path(error) / (L"quartzmidi-list-" + std::to_wstring(++listNumber) + L".json");
            std::ofstream file(entry.listFile, std::ios::binary | std::ios::trunc);
            file << jsonText(list);
            file.close();
            if (!file) {
                removeList(entry.listFile);
                fail("The converter could not start.");
                return;
            }
        }
        const bool list = !entry.listFile.empty();
        convertedCount = 0;
        run = {};
        // The CPU share is the one set now, not when the entry was picked. A
        // list's folder, dest and artist are in its file.
        const bool started = converter.Start(
            audio_to_midi::CommandLine(install.python, install.script, entry.link.empty() ? entry.file.native() : wide(entry.link),
                                       state.folder, entry.playlist, converterCpu, entry.listFile, entry.again,
                                       list ? std::wstring() : wide(entry.folder),
                                       list || !entry.dest ? std::nullopt : std::optional<std::wstring>(wide(*entry.dest)),
                                       list ? std::wstring() : wide(entry.artist)),
            reportConversion);
        state.signingIn = false;
        state.converting = started;
        if (!started) {
            removeList(entry.listFile);
            fail("The converter could not start.");
            return;
        }
        state.conversionFailed = false;
        state.conversionStatus = !entry.playlist ? "Starting the converter..."
            : audio_to_midi::IsChannelLink(entry.link) ? "Reading the channel..." : "Reading the playlist...";
        run.phase = entry.playlist ? "reading" : list ? "list" : "download";
        if (list) run.total = static_cast<int>(entry.items.size());
        running = std::move(entry);
        tellItem(*running, "converting", runningFields());
    };
    // Starts the next waiting entry, unless the converter is busy or the queue
    // is paused. With no MIDI folder the queue waits for one.
    const auto startWaiting = [&] {
        while (!state.converting && !state.conversionsPaused && !waitingConversions.empty()) {
            if (state.folder.empty()) {
                state.conversionFailed = false;
                state.conversionStatus = "Choose a MIDI folder.";
                pause("folder");
                break;
            }
            auto next = std::move(waitingConversions.front());
            waitingConversions.pop_front();
            startEntry(std::move(next));
            tellPositions();
        }
        state.conversionsWaiting = waitingConversions.size();
    };
    const auto resumeQueue = [&] {
        state.conversionsPaused = false;
        state.pausedReason.clear();
        tellWindow("pause", {{"type", "resumed"}});
        startWaiting();
    };
    // Queues an entry once: a key already running or waiting is told its state
    // again, and a waiting one takes the folder picked last.
    const auto enqueue = [&](QueueEntry entry) {
        const auto waiting = std::find_if(waitingConversions.begin(), waitingConversions.end(),
                                          [&](const QueueEntry& other) { return other.key == entry.key; });
        if (running && running->key == entry.key) tellItem(*running, "converting", runningFields());
        else if (waiting != waitingConversions.end()) {
            if (entry.dest) {
                waiting->dest = std::move(entry.dest);
                waiting->artist = std::move(entry.artist);
            }
            tellItem(*waiting, "queued", {{"position", waiting - waitingConversions.begin() + 1}});
        } else {
            waitingConversions.push_back(std::move(entry));
            tellItem(waitingConversions.back(), "queued", {{"position", waitingConversions.size()}});
            state.conversionsWaiting = waitingConversions.size();
        }
        startWaiting();
    };
    // A link from the link field, or from an older window's pick lines. A
    // channel is always read whole, a playlist when asked.
    const auto linkEntry = [](const std::string& link, bool wholePlaylist) {
        QueueEntry entry;
        entry.key = audio_to_midi::ItemKey(link);
        entry.title = entry.link = link;
        const bool channel = audio_to_midi::IsChannelLink(link);
        entry.playlist = channel || (wholePlaylist && audio_to_midi::IsPlaylistLink(link));
        // A whole playlist is p:LIST even when its link names a video, the
        // key the window gives it, so it is not taken for that one video.
        if (entry.playlist && !channel) {
            const auto list = audio_to_midi::QueryValue(audio_to_midi::SplitLink(link).query, "list");
            if (!list.empty() && !list.starts_with("RD")) entry.key = "p:" + std::string(list);
        }
        entry.kind = channel ? "channel" : entry.playlist ? "playlist" : audio_to_midi::VideoId(link) ? "video" : "link";
        return entry;
    };
    const auto pickLink = [&](const std::string& link, bool playlist) { enqueue(linkEntry(link, playlist)); };
    const auto findWaiting = [&](const std::string& key) {
        return std::find_if(waitingConversions.begin(), waitingConversions.end(), [&](const QueueEntry& entry) { return entry.key == key; });
    };
    // A qm: message from the YouTube window (protocol 2). Nothing in one can
    // throw: a malformed message is logged and dropped.
    const auto handleWindow = [&](const nlohmann::json& message) {
        const auto text = [](const nlohmann::json& object, const char* name) {
            const auto at = object.find(name);
            return at != object.end() && at->is_string() ? at->get<std::string>() : std::string();
        };
        // A message's dest and artist: none when absent or null, else a dest
        // ParseDest takes and an artist that is a channel id. False for any
        // other, or for an artist without a dest.
        const auto destOf = [](const nlohmann::json& object, std::optional<std::string>& dest, std::string& artist) {
            const auto at = object.find("dest"), by = object.find("artist");
            if (at != object.end() && !at->is_null()) {
                if (!at->is_string() || !audio_to_midi::ParseDest(at->get_ref<const std::string&>())) return false;
                dest = at->get<std::string>();
            }
            if (by != object.end() && !by->is_null()) {
                if (!by->is_string() || !audio_to_midi::IsChannelId(by->get_ref<const std::string&>())) return false;
                artist = by->get<std::string>();
            }
            // An artist goes only with the dest chosen for it.
            return dest || artist.empty();
        };
        const auto type = text(message, "type");
        if (type == "pick") {
            // browse.py checked it; the kind and the ids are checked again, and a
            // link must be a web address so it cannot pass as an option.
            QueueEntry entry;
            entry.key = text(message, "key");
            entry.kind = text(message, "kind");
            entry.title = text(message, "title");
            entry.link = text(message, "url");
            entry.folder = text(message, "folder");
            const auto again = message.find("again");
            entry.again = again != message.end() && again->is_boolean() && again->get<bool>();
            bool valid = !entry.key.empty() && (entry.link.empty() || entry.link.starts_with("https://") || entry.link.starts_with("http://")) &&
                         destOf(message, entry.dest, entry.artist);
            if (const auto items = message.find("items"); items != message.end()) {
                valid = valid && items->is_array() && items->size() <= 5000;
                if (valid)
                    for (const auto& item : *items) {
                        const auto id = item.is_object() ? text(item, "id") : std::string();
                        if (!audio_to_midi::IsVideoId(id)) { valid = false; break; }
                        entry.items.emplace_back(id, text(item, "title"));
                    }
            }
            const auto video = audio_to_midi::VideoId(entry.link);
            const bool list = !entry.items.empty();
            if (entry.kind == "video") valid = valid && video.has_value() && entry.key == "v:" + *video && !list;
            else if (entry.kind == "playlist" || entry.kind == "channel") valid = valid && (list || !entry.link.empty());
            else if (entry.kind == "mix" || entry.kind == "selection") valid = valid && list;
            else if (entry.kind == "link") valid = valid && !entry.link.empty() && !list;
            else valid = false;
            if (!valid) {
                ShellLog::Instance().Append("[youtube] A pick was refused: " + jsonText(message) + "\n");
                return;
            }
            // A playlist or channel picked with no videos listed is read whole.
            entry.playlist = !list && (entry.kind == "playlist" || entry.kind == "channel" ||
                                       (entry.kind == "link" && (audio_to_midi::IsChannelLink(entry.link) ||
                                                                 (audio_to_midi::IsPlaylistLink(entry.link) && !video))));
            if (entry.title.empty()) entry.title = entry.link;
            enqueue(std::move(entry));
        } else if (type == "drop") {
            const auto key = text(message, "key"), parent = text(message, "parent");
            if (parent.empty()) {
                if (const auto waiting = findWaiting(key); waiting != waitingConversions.end()) {
                    const auto entry = std::move(*waiting);
                    waitingConversions.erase(waiting);
                    tellItem(entry, "removed", nlohmann::json::object());
                    tellPositions();
                } else if (running && running->key == key) {
                    // Told removed once the converter has stopped.
                    run.dropped = true;
                    converter.Cancel();
                }
                return;
            }
            // One video of a list, skipped wherever the list is.
            const auto id = key.starts_with("v:") ? key.substr(2) : std::string();
            if (!audio_to_midi::IsVideoId(id)) return;
            if (const auto waiting = findWaiting(parent); waiting != waitingConversions.end()) {
                // A list read whole lists no videos to take one from; it stays as it is.
                if (waiting->items.empty()) {
                    tellItem(*waiting, "queued", {{"position", waiting - waitingConversions.begin() + 1}});
                    return;
                }
                std::erase_if(waiting->items, [&](const auto& item) { return item.first == id; });
                tellChild(parent, id, "skipped", nlohmann::json::object());
                // A list left with no videos has nothing to convert.
                if (waiting->items.empty()) {
                    const auto entry = std::move(*waiting);
                    waitingConversions.erase(waiting);
                    tellItem(entry, "removed", nlohmann::json::object());
                    tellPositions();
                }
                return;
            }
            if (!running || running->key != parent) return;
            // Nor does a running one read whole; it is told it goes on.
            if (running->listFile.empty()) {
                tellItem(*running, "converting", runningFields());
                return;
            }
            tellChild(parent, id, "skipped", nlohmann::json::object());
            // Never converted again if the list starts over.
            run.finished.insert(id);
            if (id != run.childId) {
                // convert.py reads the skip file before each video.
                std::ofstream skip(running->listFile.native() + L".skip", std::ios::binary | std::ios::app);
                skip << id << '\n';
                return;
            }
            // The video being converted: the list stops, and starts again
            // without it and without those finished.
            auto rest = *running;
            std::erase_if(rest.items, [&](const auto& item) { return run.finished.contains(item.first); });
            rest.listFile.clear();
            run.dropped = true;
            converter.Cancel();
            if (!rest.items.empty()) {
                waitingConversions.push_front(std::move(rest));
                tellPositions();
            }
        } else if (type == "order") {
            const auto keys = message.find("keys");
            if (keys == message.end() || !keys->is_array()) return;
            std::deque<QueueEntry> ordered;
            for (const auto& key : *keys) {
                if (!key.is_string()) continue;
                if (const auto waiting = findWaiting(key.get<std::string>()); waiting != waitingConversions.end()) {
                    ordered.push_back(std::move(*waiting));
                    waitingConversions.erase(waiting);
                }
            }
            // Any it did not name keep their order after those it did.
            for (auto& entry : waitingConversions) ordered.push_back(std::move(entry));
            waitingConversions = std::move(ordered);
            tellPositions();
        } else if (type == "dest") {
            // Save to for one entry: a waiting one goes to the new folder; a
            // running one keeps the folder it started with and is told so.
            const auto key = text(message, "key");
            std::optional<std::string> dest;
            std::string artist;
            // It must name a folder: "" is the MIDI folder.
            if (!destOf(message, dest, artist) || !dest) {
                ShellLog::Instance().Append("[youtube] A folder was refused: " + jsonText(message) + "\n");
                return;
            }
            if (const auto waiting = findWaiting(key); waiting != waitingConversions.end()) {
                waiting->dest = std::move(dest);
                waiting->artist = std::move(artist);
                tellItem(*waiting, "queued", {{"position", waiting - waitingConversions.begin() + 1}});
            } else if (running && running->key == key) {
                tellItem(*running, "converting", runningFields());
            }
        } else if (type == "resume") {
            resumeQueue();
        } else if (type == "choose-folder") {
            state.folderWanted = true;
        } else if (type == "signed") {
            state.youtubeSignedIn = converterInstall().SignedIn();
            const auto in = message.find("in");
            if (in != message.end() && in->is_boolean() && in->get<bool>() && state.conversionsPaused && state.pausedReason == "signin")
                resumeQueue();
        } else if (type == "mount") {
            ShellLog::Instance().Append("[youtube] Mounted " + text(message, "surface") + ": " + text(message, "detail") + "\n");
        } else {
            ShellLog::Instance().Append("[youtube] " + jsonText(message) + "\n");
        }
    };
    // Runs the add-on's setup.ps1, for Install and for the YouTube button, and
    // with an engine to switch to it.
    const auto startSetup = [&](const audio_to_midi::Install& install, std::string_view engine = {}) {
        const bool started = converter.Start(audio_to_midi::SetupCommandLine(install.setup, engine), reportConversion);
        state.settingUp = state.converting = started;
        state.signingIn = false;
        state.conversionFailed = !started;
        state.conversionStatus = started ? "Starting the setup..." : "The setup could not start.";
        if (!started) browseAfterSetup = false;
    };
    Publish(state);
    // Polls the active trigger's keys every 1 ms on its own thread: a tap is a
    // note and can't wait for the worker loop, and WM_HOTKEY reports no key-up.
    // Taps go straight to the player; Hold sends Play/Pause to this worker.
    // Declared after the player so it stops first.
    std::atomic<int> pollKind{0};   // 0 the clock, 1 plays, 2 steps
    std::array<std::atomic<int>, kAddonHotkeys> pollKeys{};
    std::atomic<VirtualPianoPlayer*> tapTarget{nullptr};
    const auto syncPoller = [&] {
        const int kind = state.TriggerPlays() ? 1 : state.TriggerSteps() ? 2 : 0;
        pollKind.store(kind, std::memory_order_release);
        const PerformerTrigger none;
        const auto& keys = kind ? section.triggers[static_cast<size_t>(state.trigger)] : none;
        for (size_t i = 0; i < kAddonHotkeys; ++i) {
            const int vk = i < keys.keyFields.size() ? NameToVK(state.hotkeys[keys.firstKey + i]) : 0;
            pollKeys[i].store(vk, std::memory_order_release);
        }
        tapTarget.store(state.playing && kind == 2 ? player.get() : nullptr, std::memory_order_release);
    };
    std::jthread poller([&](std::stop_token token) {
        std::array<bool, kAddonHotkeys> held{};
        bool playing = false;
        while (!token.stop_requested()) {
            const int kind = pollKind.load(std::memory_order_acquire);
            if (kind != 1) { holdDown.store(false, std::memory_order_release); playing = false; }
            if (kind == 0) { held.fill(false); std::this_thread::sleep_for(50ms); continue; }
            std::function<bool(int)> probe;
            { std::lock_guard lock(keyProbeMutex_); probe = keyProbe_; }
            bool any = false;
            for (size_t i = 0; i < kAddonHotkeys; ++i) {
                const int vk = pollKeys[i].load(std::memory_order_acquire);
                const bool down = vk != 0 && probe && probe(vk);
                any |= down;
                if (down == held[i]) continue;
                held[i] = down;
                if (kind == 2)
                    if (auto* target = tapTarget.load(std::memory_order_acquire)) target->tap(static_cast<int>(i), down);
            }
            if (kind == 1 && any != playing) {
                playing = any;
                // Store before queueing the command that reads it.
                holdDown.store(any, std::memory_order_release);
                Send({any ? Action::Play : Action::Pause, {}, Snapshot()->generation});
            }
            std::this_thread::sleep_for(1ms);
        }
    });
    // Every 2 s while an input or output is chosen or waits to come back, lists
    // the devices here, off the worker and the note threads, and scans when the
    // chosen one has left the list or the waiting one is back (PresenceChanged).
    std::jthread presence([this](std::stop_token token) {
        const auto rows = [](const std::vector<MidiInputDevice>& devices) {
            std::vector<LiveDevice> listed;
            for (const auto& device : devices) listed.push_back({device.id, {}, device.group, device.backend});
            return listed;
        };
        while (!token.stop_requested()) {
            for (int tick = 0; tick < 40 && !token.stop_requested(); ++tick) std::this_thread::sleep_for(50ms);
            if (token.stop_requested()) break;
            const auto state = Snapshot();
            if (!state->liveDevice.empty() || !state->liveWaiting.empty()) {
                const auto now = rows(EnumerateMidiInputs());
                if (PresenceChanged(state->devices, now, state->liveDevice, state->liveWaiting,
                                    BackendForDeviceId(state->liveWaiting), Wide(state->liveWaitingName)))
                    Send({Action::LiveScan});
            }
            if (!state->outputDevice.empty() || !state->outputWaiting.empty()) {
                auto now = rows(EnumerateMidiOutputs());
                if (NamedMidiPortAvailable())
                    now.push_back({NamedMidiPortId(Wide(state->outputPortName)), {}, {}, MidiBackend::NamedPort});
                if (PresenceChanged(state->outputDevices, now, state->outputDevice, state->outputWaiting,
                                    BackendForOutputId(state->outputWaiting), Wide(state->outputWaitingName)))
                    Send({Action::OutputScan});
            }
        }
    });
    // Curves are committed on slider release and written immediately, not
    // debounced, so a failed save is reported at once.
    const auto saveCurves = [&](const EngineSnapshot& next) {
        configJson["CUSTOM_VELOCITY_CURVES"] = nlohmann::json::array();
        for (size_t i = midi::kBuiltinVelocityCurves; i < next.curves.size(); ++i)
            configJson["CUSTOM_VELOCITY_CURVES"].push_back({{"name", next.curves[i].name}, {"values", next.curves[i].thresholds}});
        auto& saved = configJson["SHELL_VELOCITY"];
        saved = {{"preset", next.curve.preset}, {"builtins", midi::kBuiltinVelocityCurves}, {"sensitivity", next.curve.sensitivity},
                 {"contrast", next.curve.contrast}, {"sustainCutoff", next.sustainCutoff}};
        if (!next.curve.anchors.empty()) {
            saved["anchors"] = nlohmann::json::array();
            for (const auto& point : next.curve.anchors) saved["anchors"].push_back({point.x, point.y});
        }
        touchConfig();
        flushConfig();
    };
    const auto applyTracks = [&] {
        if (!player) return;
        for (const auto& row : state.rows) {
            player->set_track_mute(row.index, row.muted);
            player->set_track_solo(row.index, row.solo);
        }
    };
    const auto invalidateSheet = [&] {
        state.sheetText = std::make_shared<const std::string>();
        state.sheetNotes = state.sheetGroups = state.sheetMerged = state.sheetUnmapped = 0;
        state.sheetReady = false;
        state.sheetSaved.clear();
        state.sheetFilesSaved.clear();
        if (!state.sheetBatchRunning) state.sheetBatchStatus.clear();
    };
    // Sheet request for the loaded file: audible note-ons with their releases
    // and sustain pedal values in seconds, with the tempo map and time signatures.
    const auto playerPage = [&] {
        const bool anySolo = AnySolo(state.rows);
        const auto audible = [&](int track) {
            const auto row = std::find_if(state.rows.begin(), state.rows.end(),
                [&](const TrackRow& candidate) { return candidate.index == static_cast<size_t>(track); });
            return row != state.rows.end() && TrackAudible(*row, anySolo);
        };
        nlohmann::json page{{"title", Utf8(state.loaded.stem())}, {"mapping", state.keyMappings}, {"division", player->midi_file.division}};
        auto& notes = page["notes"] = nlohmann::json::array();
        auto& pedals = page["pedals"] = nlohmann::json::array();
        // Each press's release is the next release of its track and note, so
        // the sheet does not read a held chord as a rest.
        std::map<std::pair<int, std::string_view>, std::deque<size_t>> sounding;
        // Use scoreTimes: event.time may be rescaled by the playback speed.
        for (size_t i = 0; i < player->note_events.size(); ++i) {
            const auto& event = player->note_events[i];
            if (!audible(event.trackIndex)) continue;
            const auto time = i < scoreTimes.size() ? scoreTimes[i] : event.time;
            const double seconds = static_cast<double>(time.count()) / 1e9;
            // A pedal's velocity carries its value, and its channel above it.
            if (PedalForName(event.note_or_control) == 0) { pedals.push_back({seconds, event.velocity & 0xFF, event.trackIndex}); continue; }
            if (PedalForName(event.note_or_control) >= 0) continue;
            const std::pair key{event.trackIndex, std::string_view(event.note_or_control)};
            if (event.action == EventType::Release) {
                if (auto& on = sounding[key]; !on.empty()) { notes[on.front()].push_back(seconds); on.pop_front(); }
                continue;
            }
            if (event.action != EventType::Press) continue;
            const int midi = MidiNumberForNoteName(std::string(event.note_or_control).c_str());
            if (midi >= 0) { sounding[key].push_back(notes.size()); notes.push_back({seconds, midi}); }
        }
        auto& tempos = page["tempos"] = nlohmann::json::array();
        for (const auto& change : player->midi_file.tempoChanges) tempos.push_back({change.tick, change.microsecondsPerQuarter});
        auto& meters = page["meters"] = nlohmann::json::array();
        for (const auto& signature : player->midi_file.timeSignatures) meters.push_back({signature.tick, signature.numerator});
        return page;
    };
    const auto sheetsRoot = [&] { return state.sheetsFolder.empty() ? DefaultSheetsFolder(state.folder) : state.sheetsFolder; };
    const auto sheetOutputs = [&] { return nlohmann::json{{"image", state.sheetImage}, {"text", state.sheetTextFile}, {"page", state.sheetPageFile}}; };
    const auto takeSheet = [&](const nlohmann::json& answer) {
        state.sheetText = std::make_shared<const std::string>(answer.value("text", ""));
        state.sheetNotes = answer.value("notes", size_t{0});
        state.sheetGroups = answer.value("groups", size_t{0});
        state.sheetMerged = answer.value("merged", size_t{0});
        state.sheetUnmapped = answer.value("unmapped", size_t{0});
        state.sheetReady = true;
        ++state.sheetRevision;
    };
    // Library sheet export. Reports through Send; stopped and joined when Run returns.
    std::jthread sheetBatch;
    // The style the sheet editor last showed, which sheets with no style page
    // are drawn with. The editor sends it through the listener, started with
    // the first editor and stopped when Run returns.
    const auto keptSheetStyle = Utf8(config_.parent_path() / L"sheet-style.json");
    std::unique_ptr<SheetStyleListener> sheetStyleListener;
    // Watches the open folder on its own thread and walks it again when its
    // MIDI files change, so the list follows the disk even during playback.
    // Restarted by each Scan that is not quiet; stopped and joined when Run returns.
    std::jthread libraryWatch;
    const auto watchLibrary = [&](const std::filesystem::path& root) {
        // The folders Save to offers, as this thread's walks last found them,
        // so one that goes empty is noticed too.
        libraryWatch = std::jthread([this, root, folders = libraryFolders](std::stop_token token) mutable {
            const auto listed = [this, &root, &folders](const std::filesystem::path& gone) {
                const auto files = Snapshot()->files;
                const auto& prefix = gone.native();
                return std::any_of(files->begin(), files->end(), [&](const MidiEntry& file) {
                    const auto& path = file.path.native();
                    return path.size() > prefix.size() && (path[prefix.size()] == L'\\' || path[prefix.size()] == L'/') &&
                        _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
                }) || std::ranges::find(folders, Utf8(gone.lexically_relative(root).generic_wstring())) != folders.end();
            };
            WatchLibrary(root, token, listed, [&] {
                std::error_code error;
                auto walked = std::make_shared<std::vector<std::string>>();
                auto files = WalkLibrary(root, token, error, walked.get());
                if (!files || token.stop_requested()) return;
                folders = *walked;
                Command found{Action::Scan, root, 0, 0, true};
                found.files = std::move(files);
                found.folders = std::move(walked);
                Send(std::move(found));
            });
        });
    };
    // Reads song lengths on its own thread and sends them back a batch at a
    // time (Action::Lengths), so the list shows at once and fills in. Started
    // again when what is listed changes; stopped and joined when Run returns.
    std::jthread lengthsWork;
    std::shared_ptr<const std::vector<MidiEntry>> lengthsFiles;
    std::shared_ptr<const LibraryLists> lengthsLibrary;
    // The songs listed now that want a length read: a library file not read at its
    // size and time, and a song from outside the library not read this session.
    // True when the songs from outside changed.
    const auto measureLengths = [&] {
        lengthsFiles = state.files;
        lengthsLibrary = state.library;
        PathSet inLibrary;
        inLibrary.reserve(state.files->size());
        for (const auto& file : *state.files) inLibrary.insert(file.path);
        PathMap<size_t> had;
        for (size_t i = 0; i < state.outside->size(); ++i) had.emplace((*state.outside)[i].path, i);
        auto outside = std::make_shared<std::vector<MidiEntry>>();
        PathSet seen;
        const auto list = [&](const std::vector<std::filesystem::path>& songs) {
            for (const auto& song : songs) {
                if (inLibrary.contains(song) || !seen.insert(song).second) continue;
                if (const auto found = had.find(song); found != had.end()) outside->push_back((*state.outside)[found->second]);
                else {
                    MidiEntry entry{song, Utf8(song)};
                    entry.outside = true;
                    outside->push_back(std::move(entry));
                }
            }
        };
        list(state.library->queue);
        for (const auto& playlist : state.library->playlists) list(playlist.songs);
        knownLengths(*outside);
        const bool changed = *outside != *state.outside;
        if (changed) state.outside = std::move(outside);
        std::vector<MidiEntry> wanted;
        for (const auto& file : *state.files) {
            const auto known = lengths.find(file.path);
            if (known == lengths.end() || known->second.bytes != file.bytes || known->second.modified != file.modified) wanted.push_back(file);
        }
        for (const auto& song : *state.outside) if (!outsideRead.contains(song.path)) wanted.push_back(song);
        // A run already reading every one of them goes on.
        if (std::all_of(wanted.begin(), wanted.end(), [&](const MidiEntry& song) { return lengthsPending.contains(song.path); })) return changed;
        lengthsPending.clear();
        for (const auto& song : wanted) lengthsPending.insert(song.path);
        lengthsWork = std::jthread([this, wanted = std::move(wanted), run = ++lengthsRun](std::stop_token token) {
            auto batch = std::make_shared<std::vector<MidiEntry>>();
            auto sent = std::chrono::steady_clock::now();
            const auto send = [&](bool last) {
                Command read{Action::Lengths, {}, run, 0, last};
                read.files = std::exchange(batch, std::make_shared<std::vector<MidiEntry>>());
                Send(std::move(read));
                sent = std::chrono::steady_clock::now();
            };
            for (const auto& song : wanted) {
                if (token.stop_requested()) return;
                std::error_code sizeError, timeError;
                MidiEntry read{song.path, {}, std::filesystem::file_size(song.path, sizeError),
                               std::filesystem::last_write_time(song.path, timeError)};
                // A song gone is read as one that cannot be read.
                if (sizeError || timeError) read = MidiEntry{song.path};
                // One from outside read in an earlier session is read again only if it changed.
                else if (song.outside && song.seconds >= 0 && read.bytes == song.bytes && read.modified == song.modified) read.seconds = song.seconds;
                else read.seconds = SongSeconds(song.path);
                batch->push_back(std::move(read));
                if (std::chrono::steady_clock::now() - sent >= 250ms) send(false);
            }
            send(true);
        });
        return changed;
    };
    // The file Previous or Next goes to among `files`: the open song's neighbour,
    // or for Next with Shuffle Play another drawn at random, as the advance at
    // the end of a song does; past files removed or renamed since the scan.
    const auto neighbour = [&](const std::vector<MidiEntry>& files, bool previous) {
        const auto found = std::find_if(files.begin(), files.end(), [&](const auto& file) { return file.path == state.loaded; });
        size_t index = previous ? files.size() - 1 : 0;
        const bool drawn = state.shuffle && !previous && files.size() > 1;
        const auto step = [&](size_t from) { return previous ? (from + files.size() - 1) % files.size() : (from + 1) % files.size(); };
        if (found != files.end()) {
            const auto current = static_cast<size_t>(found - files.begin());
            index = step(current);
            if (drawn) {
                index = std::uniform_int_distribution<size_t>(0, files.size() - 2)(random);
                if (index >= current) ++index;
            }
        } else if (drawn) index = std::uniform_int_distribution<size_t>(0, files.size() - 1)(random);
        // A drawn file steps on to another one.
        std::error_code gone;
        for (size_t tried = 1; tried < files.size() && !std::filesystem::is_regular_file(files[index].path, gone); ++tried) {
            index = step(index);
            if (drawn && files[index].path == state.loaded) index = step(index);
        }
        return files[index].path;
    };
    // What upNext was chosen for. A song Shuffle Play drew stays up next while the
    // open song and Shuffle Play stay, and it is still in the folder.
    std::filesystem::path nextAfter;
    std::shared_ptr<const std::vector<MidiEntry>> nextAmong;
    bool nextShuffled = false;
    // And the lists it was chosen from, as the list shown changes what follows.
    std::shared_ptr<const LibraryLists> nextLibrary;
    int nextList = kFolderList;
    // The first queued song still on disk, which comes before any other.
    const auto firstQueued = [&] {
        std::error_code gone;
        for (const auto& song : state.library->queue) if (std::filesystem::is_regular_file(song, gone)) return song;
        return std::filesystem::path();
    };
    bool nextQueued = false;
    // A playlist or Favourites shown plays on through itself at a song's end,
    // with or without Shuffle Play, while it holds a song other than the open
    // one; without Shuffle Play it stops after its last song (listEnds), though
    // Next pressed there goes round to its first, as in the folder.
    // `files` is SongsFollowed's, which for the favourites shown holds only them.
    const auto listHeld = [&](const std::vector<MidiEntry>& files) {
        if (state.openList != kFavouritesList && state.openList < 0) return false;
        const auto* listed = state.library->Songs(state.openList);
        return state.openList == kFavouritesList ? !files.empty() : listed && !listed->empty();
    };
    // Its last song is the last still on disk, so one deleted after
    // the open one does not send it round to its first.
    const auto listEnds = [&](const std::vector<MidiEntry>& files) {
        if (state.shuffle || !listHeld(files)) return false;
        std::error_code gone;
        const auto last = std::find_if(files.rbegin(), files.rend(), [&](const MidiEntry& file) {
            return SamePath(file.path, state.loaded) || std::filesystem::is_regular_file(file.path, gone); });
        return last == files.rend() || SamePath(last->path, state.loaded);
    };
    const auto listPlaysOn = [&] {
        const auto files = SongsFollowed(*state.files, state.loaded, *state.library, state.openList);
        std::error_code gone;
        return listHeld(files) && !listEnds(files) && std::any_of(files.begin(), files.end(), [&](const MidiEntry& file) {
            return !SamePath(file.path, state.loaded) && std::filesystem::is_regular_file(file.path, gone); });
    };
    const auto chooseNext = [&] {
        std::filesystem::path next = firstQueued();
        const bool queued = !next.empty();
        const auto files = SongsFollowed(*state.files, state.loaded, *state.library, state.openList);
        if (!queued && !files.empty()) {
            const bool listed = std::any_of(files.begin(), files.end(), [&](const MidiEntry& file) { return file.path == state.upNext; });
            const bool kept = state.shuffle && nextShuffled && !nextQueued && state.loaded == nextAfter && !state.upNext.empty() && listed;
            next = kept ? state.upNext : listEnds(files) ? std::filesystem::path() : neighbour(files, false);
        }
        if (next == state.loaded) next.clear();
        nextAfter = state.loaded; nextAmong = state.files; nextShuffled = state.shuffle;
        nextLibrary = state.library; nextList = state.openList; nextQueued = queued;
        if (next == state.upNext) return false;
        state.upNext = std::move(next);
        return true;
    };
    while (!stop.stop_requested()) {
        Command command{Action::Stop};
        bool hasCommand = false;
        // True while a scan's reopen runs, so the catch below can tell its
        // failure from one in the ticks that follow it.
        bool reopening = false;
        const bool ticking = state.playing || volumePending || state.playbackCountdown;
        {
            std::unique_lock lock(mutex_);
            const auto ready = [&] { return stop.stop_requested() || !commands_.empty(); };
            if (ticking) wake_.wait_for(lock, 25ms, ready);
            else if (state.outputMidi && !state.outputDevice.empty()) wake_.wait_for(lock, 250ms, ready);
            else if (configDirty || libraryDirty || songsDirty) wake_.wait_until(lock, configDue, ready);
            else wake_.wait(lock, ready);
            if (stop.stop_requested()) break;
            // Coalesce commands that carry an absolute target: only the last
            // queued one matters, and each Load parses a whole score.
            while (!commands_.empty()) {
                command = std::move(commands_.front());
                commands_.pop_front();
                const bool overtaken =
                    (command.action == Action::Load || command.action == Action::Seek ||
                     command.action == Action::Speed || command.action == Action::Transpose ||
                     command.action == Action::LiveScan || command.action == Action::OutputScan ||
                     command.action == Action::WootingTriggerThreshold ||
                     command.action == Action::WootingShiftAmount ||
                     command.action == Action::WootingVelocitySensitivity ||
                     command.action == Action::WootingMinVelocity ||
                     command.action == Action::CopySheet || command.action == Action::OpenSheetEditor) &&
                    std::any_of(commands_.begin(), commands_.end(),
                                [&](const Command& queued) { return queued.action == command.action; });
                if (overtaken) continue;
                hasCommand = true;
                break;
            }
        }
        // A pass that only polled the MIDI output publishes nothing, so an
        // idle window stops drawing. A tick that only moved the position
        // publishes that alone (PublishPosition); one that changes more says so.
        bool stateChanged = hasCommand;
        try {
            if (hasCommand) {
                const bool scoreCommand = command.action != Action::Scan && command.action != Action::Load &&
                    command.action != Action::Stop && command.action != Action::Velocity && command.action != Action::Sustain &&
                    command.action != Action::Remap && command.action != Action::LiveScan &&
                    command.action != Action::LiveOpen && command.action != Action::LiveActive &&
                    command.action != Action::LiveChannel && command.action != Action::OutputTarget &&
                    command.action != Action::OutputScan && command.action != Action::OutputOpen &&
                    command.action != Action::OutputPortName && command.action != Action::VelocityModifier &&
                    command.action < Action::CurveSelect;
                if (scoreCommand && command.generation != state.generation) continue;
                reopening = command.automatic;
                if (!state.typingAcknowledged &&
                    (command.action == Action::LiveOpen && !command.device.empty() ||
                     command.action == Action::LiveActive && command.value ||
                     command.action == Action::MidiConnect && command.value ||
                     command.action == Action::AutoVolumeCalibrate))
                    throw std::runtime_error("Read the typing warning in the app before starting output.");
                // Any user command cancels an armed calibration before it can
                // focus another window; only Calibrate starts a sweep. Progress
                // reports from worker threads neither cancel it nor clear the error.
                // Nor do the panel's own reports for the YouTube window and the
                // converter (its theme, the CPU share, the folder picker opened).
                const bool fromConverter = command.action == Action::ConvertProgress || command.action == Action::BrowseProgress ||
                    command.action == Action::SheetBatchProgress ||
                    command.action == Action::Lengths || command.action == Action::SongRecycled ||
                    command.action == Action::KeepSheetStyle || command.action == Action::BrowseTheme ||
                    command.action == Action::ConverterCpu || command.action == Action::FolderAsked;
                // Nor do device scans, which also run whenever a device comes or goes.
                const bool deviceScan = command.action == Action::LiveScan || command.action == Action::OutputScan;
                // The reopens they send leave the calibration too, but clear the
                // error, so a device that comes back takes its disconnect message.
                if (volumePending && command.action != Action::AutoVolumeCalibrate &&
                    command.action != Action::AutoVolumeScan && command.action != Action::Scan && !fromConverter && !deviceScan &&
                    !command.automatic)
                    cancelVolume();
                // A quiet rescan leaves the error shown as well.
                const bool quietScan = command.action == Action::Scan && command.value;
                // Nor does a reopen tried again after it said why it failed.
                const bool quietReopen = command.automatic &&
                    (command.action == Action::LiveOpen && !quietInput.empty() && command.device == quietInput ||
                     command.action == Action::OutputOpen && !quietOutput.empty() && command.device == quietOutput);
                if (!fromConverter && !quietScan && !deviceScan && !quietReopen) state.error.clear();
                if (!command.automatic) {
                    if (command.action == Action::LiveOpen || command.action == Action::LiveActive ||
                        command.action == Action::MidiConnect) { ++inputChoices; quietInput.clear(); }
                    if (command.action == Action::OutputOpen || command.action == Action::OutputTarget) { ++outputChoices; quietOutput.clear(); }
                }
                switch (command.action) {
                case Action::MidiConnect:
                    if (!command.value) { stopConnect(); break; }
                    if (state.liveDevice.empty()) throw std::runtime_error("Choose a MIDI input before enabling MidiConnect.");
                    stopPlayback();
                    stopLive();
                    stopConnect();
                    if (!connectFactory_) throw std::runtime_error("MidiConnect is unavailable in this host.");
                    connect = connectFactory_();
                    if (!connect || !connect->Open(state.liveDevice)) {
                        const bool busy = connect && connect->Busy();
                        stopConnect();
                        throw std::runtime_error(busy ? kInputBusy : "Cannot open that MIDI input for MidiConnect.");
                    }
                    connect->Activate(true);
                    state.midiConnect = true;
                    break;
                case Action::Performer:
                    if (!section.loaded) break;
                    section.on = command.value;
                    if (player && player->performer_on != command.value) player->toggle_performer();
                    setupJson()["ENABLED"] = command.value;
                    // Turning the performer off resets the trigger to the clock,
                    // stopping playback if a Hold trigger was running it.
                    if (!command.value && state.trigger != 0) {
                        if (state.TriggerPlays() && state.playing) stopPlayback();
                        state.trigger = 0;
                        configJson["SHELL_TRIGGER"] = 0;
                    }
                    applyPerformer();
                    touchConfig();
                    break;
                case Action::PerformerPreset: {
                    if (section.presets.empty()) break;
                    // One past the last preset is Custom, which needs a slider
                    // the presets set; a lit Custom stays as it is.
                    if (command.track >= section.presets.size()) {
                        if (section.custom || section.rowIds.empty()) break;
                        // The saved Custom, which then is the one in use, or the
                        // preset's values on its habits.
                        if (section.customSaved) {
                            section.preset = section.customSaved->preset;
                            for (const auto& id : section.rowIds) {
                                const auto amount = section.customSaved->values.find(id);
                                section.Find(id)->value = amount != section.customSaved->values.end() ? amount->second : section.Target(section.preset, id);
                            }
                            section.customSaved.reset();
                        } else
                            for (const auto& id : section.rowIds) section.Find(id)->value = section.Target(section.preset, id);
                        section.custom = true;
                    } else {
                        // Custom in use is kept as the saved one.
                        if (section.custom) {
                            PerformerCustom saved{section.preset, {}};
                            for (const auto& id : section.rowIds) saved.values[id] = section.Find(id)->value;
                            section.customSaved = std::move(saved);
                        }
                        section.preset = static_cast<int>(command.track);
                        section.custom = false;
                        for (const auto& id : section.rowIds) section.Find(id)->value = section.Target(section.preset, id);
                    }
                    saveRow();
                    break;
                }
                case Action::PerformerPin: {
                    if (section.presets.empty() || !state.rememberPerSong || state.loaded.empty() || command.value == section.pinned) break;
                    auto& entry = ownEntry();
                    // A song pinned before takes back its row; one never pinned
                    // takes the setup's, saved Custom too. Unpinned, it keeps it.
                    if (command.value && !entry.contains("PINNED")) writeRow(entry, readRow(setupSaved(), 0));
                    entry["PINNED"] = command.value ? 1 : 0;
                    songsDirty = true;
                    showSong();
                    applyPerformer();
                    break;
                }
                case Action::PerformerValue: {
                    auto* control = section.Find(command.key);
                    if (!control || !std::isfinite(command.amount)) break;
                    if (control->isChoice) {
                        control->value = std::clamp(static_cast<int>(std::lround(command.amount)), 0, static_cast<int>(control->choices.size()) - 1);
                        saveControl(*control);
                        break;
                    }
                    if (!control->isSwitch) {
                        // Negative returns the slider to its estimate, if it has one.
                        control->value = command.amount < 0 ? (section.Estimated(*control) ? -1 : 0) : std::min(command.amount, 1.0);
                        if (!section.InRow(control->id)) { saveControl(*control); break; }
                        // Moving a row slider lights Custom, which replaces a saved one.
                        section.custom = true;
                        section.customSaved.reset();
                        saveRow();
                        break;
                    }
                    const double wanted = command.amount != 0 ? 1 : 0;
                    if (control->value == wanted) break;
                    control->value = wanted;
                    saveSetting(control->field, wanted != 0);
                    // Disabling the estimate keeps what its slider showed, to the
                    // whole percent, as the one value for every song, or 0 where it
                    // followed an estimate of no song; re-enabling shows the song's
                    // own value or the estimate again.
                    if (auto* slider = section.Find(control->estimates); slider && !control->estimates.empty()) {
                        if (wanted == 0)
                            slider->value = slider->noEstimate || (slider->value < 0 && state.loaded.empty())
                                ? 0 : std::lround(section.Shown(*slider) * 100) / 100.0;
                        saveSetting(slider->field, wanted == 0 ? slider->value : -1.0);
                        if (wanted != 0) slider->value = shownValue(*slider, openEntry());
                    }
                    if (control->remembers) {
                        state.rememberPerSong = wanted != 0;
                        showSong();
                    }
                    applyPerformer();
                    break;
                }
                case Action::Trigger: {
                    if (!section.on || section.triggers.empty()) break;
                    const int wanted = static_cast<int>(std::min(command.track, section.triggers.size() - 1));
                    // A Hold trigger owns play/pause; stop when switching away from it.
                    if (state.TriggerPlays() && wanted != state.trigger && state.playing) stopPlayback();
                    state.trigger = wanted;
                    configJson["SHELL_TRIGGER"] = state.trigger;
                    touchConfig();
                    applyPerformer();
                    break;
                }
                case Action::Shuffle:
                    state.shuffle = command.value;
                    configJson["SHELL_SHUFFLE"] = command.value;
                    touchConfig();
                    break;
                case Action::Loop: {
                    const int wanted = static_cast<int>(std::min<size_t>(command.track, 2));
                    // A section starts as the bar being played and the three after it.
                    if (wanted == 2 && state.loop != 2 && player && state.duration > 0) {
                        const double at = state.playing ? std::clamp(player->get_adjusted_time().count() / 1e9, 0.0, state.duration)
                                                        : state.position < state.duration ? state.position : 0;
                        const auto [start, end] = BarsFrom(player->midi_file, at, 4);
                        state.loopStart = std::clamp(start, 0.0, state.duration);
                        state.loopEnd = std::clamp(end, state.loopStart, state.duration);
                        if (state.loopEnd - state.loopStart < kShortestLoop) { state.loopStart = 0; state.loopEnd = state.duration; }
                    }
                    state.loop = wanted;
                    applyLoop();
                    configJson["SHELL_LOOP"] = state.loop;
                    touchConfig();
                    break;
                }
                case Action::LoopStart:
                case Action::LoopEnd:
                    if (command.generation != state.generation || !std::isfinite(command.amount) || state.duration <= 0) break;
                    if (command.action == Action::LoopStart)
                        state.loopStart = std::clamp(command.amount, 0.0, std::max(0.0, state.loopEnd - kShortestLoop));
                    else state.loopEnd = std::clamp(command.amount, std::min(state.duration, state.loopStart + kShortestLoop), state.duration);
                    applyLoop();
                    break;
                case Action::HoldBehind:
                    state.holdBehind = command.value;
                    if (player) player->hold_behind.store(command.value, std::memory_order_release);
                    configJson["SHELL_HOLD_BEHIND"] = command.value;
                    touchConfig();
                    break;
                case Action::AutoSolo: loadAutoSolo = command.value; break;
                case Action::Favourite:
                    if (command.path.empty()) break;
                    editLibrary([&](LibraryLists& lists) {
                        auto& songs = lists.favourites;
                        if (command.value && !lists.Favourite(command.path)) songs.push_back(command.path);
                        if (!command.value) std::erase_if(songs, [&](const std::filesystem::path& song) { return SamePath(song, command.path); });
                    });
                    break;
                case Action::OpenList: {
                    if (!std::isfinite(command.amount)) break;
                    const int list = static_cast<int>(command.amount);
                    if (list == state.openList || !ListShown(*state.library, list)) break;
                    state.openList = list;
                    touchLibrary();
                    break;
                }
                case Action::NewPlaylist:
                case Action::RenamePlaylist: {
                    const auto first = command.key.find_first_not_of(" \t");
                    const auto name = first == std::string::npos ? std::string() : command.key.substr(first, command.key.find_last_not_of(" \t") - first + 1);
                    const int list = static_cast<int>(std::isfinite(command.amount) ? command.amount : -1);
                    if (name.empty()) break;
                    for (size_t i = 0; i < state.library->playlists.size(); ++i)
                        if (state.library->playlists[i].name == name && (command.action == Action::NewPlaylist || static_cast<int>(i) != list))
                            throw std::runtime_error("There is a playlist called " + name + " already.");
                    if (command.action == Action::RenamePlaylist) {
                        if (list < 0 || static_cast<size_t>(list) >= state.library->playlists.size()) break;
                        editLibrary([&](LibraryLists& lists) { lists.playlists[static_cast<size_t>(list)].name = name; });
                        break;
                    }
                    editLibrary([&](LibraryLists& lists) {
                        lists.playlists.push_back({name, {}});
                        if (!command.path.empty()) lists.playlists.back().songs.push_back(command.path);
                    });
                    if (command.value) state.openList = static_cast<int>(state.library->playlists.size()) - 1;
                    break;
                }
                case Action::DeletePlaylist: {
                    const int list = static_cast<int>(std::isfinite(command.amount) ? command.amount : -1);
                    if (list < 0 || static_cast<size_t>(list) >= state.library->playlists.size()) break;
                    // The lists after it move up one, and the one shown with them.
                    if (state.openList == list) state.openList = kFolderList;
                    else if (state.openList > list) --state.openList;
                    editLibrary([&](LibraryLists& lists) { lists.playlists.erase(lists.playlists.begin() + list); });
                    break;
                }
                case Action::DeleteSong: {
                    std::error_code error;
                    if (!std::filesystem::is_regular_file(command.path, error)) throw std::runtime_error(Utf8(command.path.filename()) + ": Unable to find the file.");
                    // To the Recycle Bin, where it can be restored; where the bin
                    // cannot take it, Windows asks first. SongRecycled follows.
                    recycleAside([this, path = command.path] {
                        Recycle({path});
                        Send({Action::SongRecycled, path});
                    });
                    break;
                }
                case Action::SongRecycled: {
                    std::error_code error;
                    if (std::filesystem::exists(command.path, error)) throw std::runtime_error(Utf8(command.path.filename()) + " was kept: it could not go to the Recycle Bin.");
                    // Out of the list and every other at once; the folder watcher's walk agrees.
                    auto kept = std::make_shared<std::vector<MidiEntry>>();
                    for (const auto& file : *state.files) if (file.path != command.path) kept->push_back(file);
                    if (kept->size() != state.files->size()) state.files = std::move(kept);
                    // Its settings go with the record of what it was in, for a
                    // restore from the bin (Scan); one kept by file name alone stays.
                    std::string settings;
                    const auto key = PathFromText(songKey(command.path));
                    for (auto each = songsJson.begin(); each != songsJson.end(); ++each)
                        if (SamePath(PathFromText(each.key()), key)) {
                            if (each->is_object() && !each->empty()) settings = each->dump();
                            songsJson.erase(each);
                            songsDirty = true;
                            configDue = std::chrono::steady_clock::now() + configSettle;
                            break;
                        }
                    editLibrary([&](LibraryLists& lists) { lists.Delete(command.path, std::move(settings)); });
                    break;
                }
                case Action::AddToList:
                case Action::RemoveFromList:
                case Action::MoveInList: {
                    const int list = static_cast<int>(std::isfinite(command.amount) ? command.amount : kFolderList);
                    if (command.path.empty() || !state.library->Songs(list)) break;
                    editLibrary([&](LibraryLists& lists) {
                        auto& songs = *lists.Songs(list);
                        const auto same = [&](const std::filesystem::path& song) { return SamePath(song, command.path); };
                        if (command.action == Action::AddToList && std::none_of(songs.begin(), songs.end(), same)) songs.push_back(command.path);
                        if (command.action == Action::RemoveFromList) std::erase_if(songs, same);
                        if (command.action == Action::MoveInList) MoveSong(songs, command.path, command.track);
                    });
                    break;
                }
                case Action::Lengths: {
                    for (const auto& song : *command.files) {
                        lengths.insert_or_assign(song.path, KnownLength{song.bytes, song.modified, song.seconds});
                        lengthsPending.erase(song.path);
                        outsideRead.insert(song.path);
                    }
                    if (command.value && command.generation == lengthsRun) lengthsPending.clear();
                    if (command.files->empty()) break;
                    lengthsDirty = true;
                    configDue = std::chrono::steady_clock::now() + configSettle;
                    // A copy, so a snapshot the panel holds never changes under it.
                    auto files = std::make_shared<std::vector<MidiEntry>>(*state.files);
                    if (knownLengths(*files)) {
                        if (state.fileSort == FileSort::Length)
                            std::sort(files->begin(), files->end(), [&](const auto& a, const auto& b) {
                                return FileBefore(a, b, state.fileSort, state.descendingFiles);
                            });
                        state.files = std::move(files);
                    }
                    if (auto outside = std::make_shared<std::vector<MidiEntry>>(*state.outside); knownLengths(*outside))
                        state.outside = std::move(outside);
                    break;
                }
                case Action::SortFiles: {
                    if (!std::isfinite(command.amount)) break;
                    state.fileSort = static_cast<FileSort>(static_cast<int>(std::clamp(command.amount, 0.0, 2.0)));
                    state.descendingFiles = command.value;
                    auto files = std::make_shared<std::vector<MidiEntry>>(*state.files);
                    std::sort(files->begin(), files->end(), [&](const auto& a, const auto& b) {
                        return FileBefore(a, b, state.fileSort, state.descendingFiles);
                    });
                    state.files = std::move(files);
                    configJson["SHELL_FILE_SORT"] = static_cast<int>(state.fileSort);
                    configJson["SHELL_FILE_DESCENDING"] = state.descendingFiles;
                    touchConfig();
                    break;
                }
                case Action::ClearLog: ShellLog::Instance().Clear(); break;
                case Action::AcknowledgeTyping: state.typingAcknowledged = true; break;
                case Action::PlaybackDelay:
                    if (std::isfinite(command.amount)) {
                        state.playbackDelay = static_cast<int>(std::clamp(command.amount, 0.0, 10.0));
                        configJson["SHELL_PLAYBACK_DELAY"] = state.playbackDelay;
                        touchConfig();
                        state.playbackCountdown = 0;
                    }
                    break;
                case Action::SeekStep:
                    if (std::isfinite(command.amount)) {
                        state.seekStep = static_cast<int>(std::clamp(command.amount, 1.0, 60.0));
                        configJson["SHELL_SEEK_STEP"] = state.seekStep;
                        touchConfig();
                    }
                    break;
                case Action::SpeedMin:
                case Action::SpeedMax: {
                    if (!std::isfinite(command.amount)) break;
                    if (command.action == Action::SpeedMin) state.speedMin = std::clamp(command.amount, .05, 1.0);
                    else state.speedMax = std::clamp(command.amount, 1.0, 8.0);
                    configJson["SHELL_SPEED_MIN"] = state.speedMin;
                    configJson["SHELL_SPEED_MAX"] = state.speedMax;
                    touchConfig();
                    // Clamp the current speed into the new range.
                    const double held = std::clamp(state.speed, state.speedMin, state.speedMax);
                    if (held != state.speed) {
                        state.speed = held;
                        if (player) player->requested_speed.store(state.speed, std::memory_order_release);
                        estimateSong();
                        if (atEstimate()) applyPerformer();
                    }
                    break;
                }
                case Action::Hotkey: {
                    if (command.track >= kHotkeys || hotkeyFields[command.track].empty()) break;
                    if (!command.key.empty() && NameToVK(command.key) == 0)
                        throw std::runtime_error("That key cannot be a hotkey.");
                    // Capture passes over note keys; one sent anyway is ignored.
                    if (!command.key.empty() && IsNoteKey(NameToVK(command.key), state.keyMappings)) break;
                    if (!command.key.empty() && command.key.rfind("VK_", 0) != 0) command.key.insert(0, "VK_");
                    if (state.hotkeys[command.track] == command.key) break;
                    const int vk = NameToVK(command.key);
                    for (size_t i = 0; i < kHotkeys; ++i) {
                        // Compare virtual-key codes: VK_a and VK_A are the same key.
                        const bool taken = i != command.track && vk != 0 && NameToVK(state.hotkeys[i]) == vk;
                        if (taken) state.hotkeys[i].clear();
                        if (taken || i == command.track) configJson["HOTKEY_SETTINGS"][hotkeyFields[i]] = taken ? std::string() : command.key;
                    }
                    state.hotkeys[command.track] = command.key;
                    ++state.hotkeyRevision;
                    touchConfig();
                    break;
                }
                case Action::PlayCountdown:
                    if (command.generation != state.generation) break;
                    if (state.playing || state.playbackCountdown) { stopPlayback(); break; }
                    if (!state.typingAcknowledged) throw std::runtime_error("Read the typing warning in the app before starting output.");
                    if (state.loaded.empty() || state.rows.empty()) break;
                    // A Hold trigger starts the song itself; no countdown.
                    if (state.TriggerPlays()) break;
                    if (state.position >= state.duration) state.position = 0;
                    if (state.playbackDelay == 0) startPlayback();
                    else {
                        state.playbackCountdown = state.playbackDelay;
                        playbackDue = std::chrono::steady_clock::now() + std::chrono::seconds(state.playbackDelay);
                    }
                    break;
                case Action::Scan: {
                    // value marks a quiet rescan of the open folder, from the
                    // folder watcher with the files it found or after a
                    // conversion: the list stays usable and the error stays.
                    // During playback only the watcher's own walk applies; a
                    // walk here waits until playback stops.
                    if (state.playing && !command.files) {
                        if (command.value) { rescanOwed = true; break; }
                        state.error = "Stop playback before changing the MIDI folder.";
                        break;
                    }
                    if (command.value && command.path != state.folder) break;
                    if (!command.value) {
                        state.busy = true;
                        Publish(state);
                        emptyOldTrash(command.path);
                    }
                    // Scan recursively; entries are named relative to the chosen
                    // folder. The panel browses one folder at a time (BrowseFolder
                    // in LibraryModel.hpp) while search covers the whole library.
                    auto files = std::move(command.files);
                    auto folders = std::move(command.folders);
                    if (!files) {
                        std::error_code error;
                        folders = std::make_shared<std::vector<std::string>>();
                        files = WalkLibrary(command.path, stop, error, folders.get());
                        if (!files) {
                            if (command.value) break;
                            throw std::runtime_error("Cannot read MIDI folder: " + error.message());
                        }
                        // Stop with an error if the folder is something like a drive root.
                        if (!command.value && files->size() >= kMaxLibraryFiles)
                            state.error = "Stopped at " + std::to_string(kMaxLibraryFiles) +
                                          " files. Choose a folder with fewer sub-folders in it.";
                    }
                    // Lengths read before show at once; the rest are read after
                    // the list shows (measureLengths).
                    knownLengths(*files);
                    std::sort(files->begin(), files->end(), [&](const auto& a, const auto& b) {
                        return FileBefore(a, b, state.fileSort, state.descendingFiles);
                    });
                    // The same files again keep the list the panel already shows.
                    const bool changed = *files != *state.files;
                    if (changed) state.files = std::move(files);
                    rescanOwed = false;
                    // Save to's folders, told to the window when they or the MIDI folder changed.
                    if (folders && (*folders != libraryFolders || command.path != state.folder)) {
                        libraryFolders = std::move(*folders);
                        tellWindow("folders", {{"type", "folders"}, {"folders", libraryFolders}});
                    }
                    if (!command.value) {
                        state.folder = command.path;
                        // A queue waiting for a folder goes on.
                        if (state.conversionsPaused && state.pausedReason == "folder") resumeQueue();
                        // A MIDI folder that moved takes its songs' lists and records along
                        // (LibraryLists::Rebased); files from before take it as theirs.
                        if (libraryRoot != state.folder) {
                            const auto from = libraryRoot;
                            editLibrary([&](LibraryLists& lists) { lists.Rebased(from, state.folder); });
                            libraryRoot = state.folder;
                            if (*state.library != LibraryLists{}) touchLibrary();
                        }
                        if (section.loaded) adoptNamedEntries();
                    }
                    // Songs listed in other letter case than their files take
                    // the files' own, as the panel matches them exactly.
                    if ((changed || !command.value) && !state.files->empty()) {
                        std::unordered_map<std::wstring, std::filesystem::path> actual;
                        actual.reserve(state.files->size());
                        for (const auto& file : *state.files) actual.emplace(PathKey(file.path), file.path);
                        editLibrary([&](LibraryLists& lists) { lists.Recased(actual); });
                        // A deleted song restored from the bin takes its star,
                        // playlists and settings back.
                        if (!state.library->deleted.empty()) {
                            std::unordered_set<std::wstring> present;
                            for (const auto& [key, path] : actual) present.insert(key);
                            std::vector<DeletedSong> restored;
                            editLibrary([&](LibraryLists& lists) { restored = lists.Restore(present); });
                            for (const auto& record : restored) {
                                const auto key = songKey(record.path);
                                if (record.settings.empty() || songsJson.contains(key)) continue;
                                auto settings = nlohmann::json::parse(record.settings, nullptr, false);
                                if (!settings.is_object()) continue;
                                // Read the way the songs' entries were at the start;
                                // one left empty is kept only where the song would
                                // otherwise read the entry under its file name.
                                if (section.loaded) upgradeEntry(settings, nullptr);
                                if (const auto name = nameOf(key); settings.empty() && (name.empty() || !songsJson.contains(name))) continue;
                                songsJson[key] = std::move(settings);
                                songsDirty = true;
                            }
                        }
                    }
                    if (!command.value) watchLibrary(state.folder);
                    break;
                }
                case Action::Previous:
                case Action::Next: {
                    // The queue comes first: Next, and the end of a song even without
                    // Shuffle Play, load its first song still on disk, which then
                    // leaves the queue. The rest follow the list shown (SongsFollowed),
                    // and a playlist or Favourites shown goes on at a song's end without it too.
                    const bool queued = command.action == Action::Next && !firstQueued().empty();
                    if (command.amount == 1 && (!shuffleAdvancePending || (!state.shuffle && !queued && !listPlaysOn()))) break;
                    if (command.generation != state.generation) break;
                    std::error_code gone;
                    // Queued songs gone from disk leave the queue.
                    if (command.action == Action::Next && !state.library->queue.empty())
                        editLibrary([&](LibraryLists& lists) {
                            std::erase_if(lists.queue, [&](const std::filesystem::path& song) { return !std::filesystem::is_regular_file(song, gone); });
                        });
                    const auto files = SongsFollowed(*state.files, state.loaded, *state.library, state.openList);
                    const auto first = command.action == Action::Next ? firstQueued() : std::filesystem::path();
                    // Next loads the song named up next while it is still there.
                    const bool named = command.action == Action::Next && !state.upNext.empty() &&
                        (state.upNext == first || (first.empty() &&
                         std::any_of(files.begin(), files.end(), [&](const MidiEntry& file) { return file.path == state.upNext; }))) &&
                        std::filesystem::is_regular_file(state.upNext, gone);
                    if (!named && first.empty() && files.empty()) break;
                    command.path = named ? state.upNext : !first.empty() ? first : neighbour(files, command.action == Action::Previous);
                    // With Shuffle Play, Previous goes back through the songs
                    // played before this one that are still in its folder,
                    // then to the neighbour in the list.
                    if (command.action == Action::Previous && state.shuffle)
                        while (!played.empty()) {
                            const auto back = std::move(played.back());
                            played.pop_back();
                            if (back != state.loaded && std::filesystem::is_regular_file(back, gone) &&
                                std::any_of(files.begin(), files.end(), [&](const MidiEntry& file) { return file.path == back; })) {
                                command.path = back;
                                break;
                            }
                        }
                    command.amount = state.playing || command.amount == 1 ? 1 : 0;
                    command.value = loadAutoSolo;
                    [[fallthrough]];
                }
                // Previous and Next fall through to Load, so each body checks
                // the action. DetectDrums and AutoTranspose apply at load, so
                // they reload the open file, resuming if it was playing.
                case Action::DetectDrums:
                case Action::AutoTranspose:
                    if (command.action == Action::DetectDrums) {
                        state.detectDrums = command.value;
                        // process_tracks reads the singleton, not the file.
                        midi::Config::getInstance().midi.DETECT_DRUMS = command.value;
                        configJson["MIDI_SETTINGS"]["DETECT_DRUMS"] = command.value;
                    } else if (command.action == Action::AutoTranspose) {
                        state.autoTranspose = command.value;
                        configJson["AUTO_TRANSPOSE"]["ENABLED"] = command.value;
                    }
                    if (command.action == Action::DetectDrums || command.action == Action::AutoTranspose) {
                        touchConfig();
                        flushConfig();
                        if (state.loaded.empty()) break;
                        command.path = state.loaded;
                        command.amount = state.playing ? 1 : 0;
                        command.value = loadAutoSolo;
                    }
                    [[fallthrough]];
                case Action::Load: {
                    const bool resumeAfterLoad = command.action != Action::Load && command.amount == 1;
                    // A settings reload keeps the playback position.
                    const bool sameFile = command.action == Action::DetectDrums || command.action == Action::AutoTranspose;
                    const double keepPosition = sameFile ? state.position : 0;
                    // A file removed or renamed since the scan loses its row and
                    // the open song plays on; the parse would throw, and the
                    // catch below stops playback.
                    if (std::error_code gone; !std::filesystem::is_regular_file(command.path, gone)) {
                        state.error = Utf8(command.path.filename()) + ": Unable to open file.";
                        auto kept = std::make_shared<std::vector<MidiEntry>>();
                        for (const auto& file : *state.files) if (file.path != command.path) kept->push_back(file);
                        if (kept->size() != state.files->size()) state.files = std::move(kept);
                        break;
                    }
                    // AutoVol stays calibrated across a load: the calibration
                    // lives in the player, which outlives the file.
                    // Stop before the potentially slow parse so nothing keeps
                    // injecting while the worker is busy.
                    stopPlayback();
                    state.busy = true;
                    Publish(state);
                    MidiParser parser;
                    auto file = parser.parse(Utf8(std::filesystem::absolute(command.path)));
                    if (file.format == 2) throw std::runtime_error("MIDI format 2 contains independent sequences. Use a format 0 or 1 file.");
                    auto rows = DescribeTracks(file);
                    // Transpose belongs to the song: another song starts at 0, or
                    // at Auto-transpose's fit below, and the open one keeps it.
                    if (!sameFile && command.path != state.loaded) state.transpose = 0;
                    ensurePlayer();
                    applyMappings();
                    // A settings reload carries the rows' mutes and solos over.
                    const auto keptRows = sameFile ? state.rows : std::vector<TrackRow>{};
                    // Previous with Shuffle Play walks back through these
                    // without adding to them.
                    if (!state.loaded.empty() && command.path != state.loaded && !(command.action == Action::Previous && state.shuffle)) {
                        played.push_back(state.loaded);
                        if (played.size() > 100) played.erase(played.begin());
                    }
                    state.loaded.clear();
                    state.rows.clear();
                    state.duration = state.position = 0;
                    invalidateSheet();
                    ++state.generation;
                    // Drum detection only labels tracks; detected drums are
                    // excluded from Solo Piano below.
                    //
                    // Auto-transpose is applied through Transpose below, so the
                    // config flag is cleared to stop play_notes typing the game's
                    // arrow keys. The setting itself comes from state.
                    auto& config = midi::Config::getInstance();
                    const bool autoTranspose = state.autoTranspose;
                    config.auto_transpose.ENABLED = false;
                    player->performer_on = section.on;
                    player->enable_velocity_keypress = state.velocity;
                    applyCurve();
                    player->currentSustainMode = state.sustain ? SustainMode::SPACE_DOWN : SustainMode::IG;
                    player->process_tracks(file);
                    scoreTimes.clear();
                    for (const auto& event : player->note_events) scoreTimes.push_back(event.time);
                    player->midi_file = std::move(file);
                    // drum_flags is only rewritten while detection is on; otherwise
                    // it is stale.
                    if (config.midi.DETECT_DRUMS) {
                        for (auto& row : rows) {
                            if (row.drums || row.index >= player->drum_flags.size() || !player->drum_flags[row.index]) continue;
                            row.drums = true; row.piano = false;
                            row.instrument += " (Drums)";
                        }
                    }
                    if (autoTranspose) {
                        state.transpose = std::clamp(player->toggle_transpose_adjustment(), -12, 12);
                        applyMappings();
                    }
                    player->trackMuted.clear();
                    player->trackSoloed.clear();
                    for (size_t i = 0; i < player->midi_file.tracks.size(); ++i) {
                        player->trackMuted.push_back(std::make_shared<std::atomic<bool>>(false));
                        player->trackSoloed.push_back(std::make_shared<std::atomic<bool>>(false));
                    }
                    state.rows = std::move(rows);
                    // A settings reload keeps Solo Piano when that is what the
                    // rows were, so newly detected drums follow it, and the
                    // user's own mutes and solos otherwise.
                    if (sameFile && SoloPianoApplied(keptRows)) SoloPiano(state.rows);
                    else if (sameFile && SilentTracks(keptRows) > 0) {
                        for (auto& row : state.rows)
                            for (const auto& kept : keptRows)
                                if (kept.index == row.index) { row.muted = kept.muted; row.solo = kept.solo; }
                    } else if (command.value && (!sameFile || AllPiano(keptRows))) SoloPiano(state.rows);
                    applyTracks();
                    if (!player->note_events.empty())
                        state.duration = static_cast<double>(player->note_events.back().time.count()) / 1e9;
                    if (sameFile) state.position = std::clamp(keepPosition, 0.0, state.duration);
                    // Another song's section is the whole song until its handles move.
                    if (sameFile && state.loopEnd > state.loopStart && state.loopStart < state.duration)
                        state.loopEnd = std::min(state.loopEnd, state.duration);
                    else { state.loopStart = 0; state.loopEnd = state.duration; }
                    applyLoop();
                    player->midiFileSelected = true;
                    state.loaded = command.path;
                    // A queued song opened any other way has had its turn; one
                    // reloaded for a setting has not.
                    if (!sameFile && LibraryLists::Holds(state.library->queue, command.path))
                        editLibrary([&](LibraryLists& lists) {
                            std::erase_if(lists.queue, [&](const std::filesystem::path& song) { return SamePath(song, command.path); });
                        });
                    score = player->score();
                    if (!sameFile) showSong();
                    estimateSong();
                    applyPerformer();
                    loadAutoSolo = command.value;
                    if (resumeAfterLoad) startPlayback();
                    break;
                }
                case Action::TogglePlayPause:
                    if (state.playbackCountdown) { stopPlayback(); break; }
                    if (state.playing) { stopPlayback(); break; }
                    [[fallthrough]];
                case Action::Play:
                    state.playbackCountdown = 0;
                    if (!state.playing) {
                        if (state.position >= state.duration) state.position = 0;
                        startPlayback();
                    }
                    break;
                case Action::Pause: stopPlayback(); break;
                case Action::Speed:
                    // A single store; playback continues at the new rate without
                    // losing its position or held notes.
                    if (!std::isfinite(command.amount)) break;
                    state.speed = std::clamp(command.amount, state.speedMin, state.speedMax);
                    if (player) player->requested_speed.store(state.speed, std::memory_order_release);
                    estimateSong();
                    if (atEstimate()) applyPerformer();
                    break;
                case Action::Restart:
                case Action::Seek:
                case Action::Back10:
                case Action::Forward10:
                case Action::Transpose:
                    if (player && std::isfinite(command.amount)) {
                        const bool resume = state.playing;
                        stopPlayback();
                        switch (command.action) {
                        case Action::Restart: state.position = 0; break;
                        case Action::Seek: state.position = command.amount; break;
                        case Action::Back10: state.position -= state.seekStep; break;
                        case Action::Forward10: state.position += state.seekStep; break;
                        case Action::Transpose:
                            state.transpose = static_cast<int>(std::round(std::clamp(command.amount, -12.0, 12.0)));
                            applyMappings(); break;
                        default: break;
                        }
                        state.position = std::clamp(state.position, 0.0, state.duration);
                        if (resume && state.position < state.duration) startPlayback(false);
                    }
                    break;
                case Action::Octaves: {
                    // As Transpose: held keys come up under the old doubles.
                    if (!std::isfinite(command.amount)) break;
                    const int octaves = std::clamp(static_cast<int>(std::lround(command.amount)), 1, 5);
                    if (octaves == state.octaves) break;
                    const bool resume = state.playing;
                    stopPlayback();
                    state.octaves = octaves;
                    applyMappings();
                    configJson["SHELL_OCTAVES"] = octaves;
                    touchConfig();
                    if (resume && state.position < state.duration) startPlayback();
                    break;
                }
                case Action::Remap: {
                    if (command.track < 21 || command.track > 108) break;
                    if (!state.eightyEightKeys && (command.track < 36 || command.track > 96))
                        throw std::runtime_error("The 61-key layout covers C2 to C7. Switch to 88 keys to map this note.");
                    std::string key = command.key;
                    if (key.starts_with("ctrl+")) key.erase(0, 5);
                    if (key.size() != 1 || std::string("1234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()").find(key[0]) == std::string::npos)
                        throw std::runtime_error("Use a letter, number, or shifted number for this mapping.");
                    stopPlayback();
                    // Apply immediately; the debounced save writes the file.
                    // Editing the parsed config in place preserves every other field.
                    const auto note = NoteName(static_cast<int>(command.track));
                    configJson["KEY_MAPPINGS"][state.eightyEightKeys ? "FULL" : "LIMITED"][note] = command.key;
                    touchConfig();
                    state.keyMappings[note] = command.key;
                    ++state.mappingRevision;
                    applyMappings();
                    applyVelocityModifier();
                    invalidateSheet();
                    break;
                }
                case Action::GameKeyMap: {
                    if (command.track >= std::size(kGameKeyMaps)) break;
                    stopPlayback();
                    const auto& map = kGameKeyMaps[command.track];
                    configJson["KEY_MAPPINGS"]["LIMITED"] = GameKeyTable(map, false);
                    configJson["KEY_MAPPINGS"]["FULL"] = GameKeyTable(map, true);
                    touchConfig();
                    state.keyMappings = GameKeyTable(map, state.eightyEightKeys);
                    ++state.mappingRevision;
                    applyMappings();
                    applyVelocityModifier();
                    invalidateSheet();
                    break;
                }
                case Action::LiveScan: {
                    // The chosen row's group, from the list about to be replaced
                    // or, after a failed listing left the row out, the list before.
                    const std::wstring chosenGroup = liveGroup.Update(state.devices, state.liveDevice);
                    state.devices.clear();
                    for (const auto& device : EnumerateMidiInputs())
                        state.devices.push_back({device.id, Utf8(std::filesystem::path(device.group.empty() ? device.name : device.group)),
                            device.group, device.backend});
                    // An input that was unplugged, or whose transport stopped,
                    // closes and waits to reopen like one missing at startup.
                    std::wstring reported;
                    { std::lock_guard lock(lostMutex); reported.swap(lostReport); }
                    std::wstring closed;
                    const std::wstring closedGroup = chosenGroup;
                    const bool closedActive = state.liveActive, closedConnect = state.midiConnect;
                    if (!state.liveDevice.empty() && restoreInput.empty() &&
                        (reported == state.liveDevice ||
                         DeviceGone(state.devices, state.liveDevice, BackendForDeviceId(state.liveDevice), chosenGroup))) {
                        closed = state.liveDevice;
                        stopLive();
                        stopConnect();
                        state.liveDevice.clear();
                    }
                    if (const auto listed = ReturnedId(state.devices, restoreInput, BackendForDeviceId(restoreInput), restoreInputGroup);
                        !listed.empty()) restoreInput = listed;
                    if (!restoreInput.empty() && state.liveDevice.empty() && !state.midiConnect &&
                        std::any_of(state.devices.begin(), state.devices.end(), [&](const LiveDevice& device) { return device.id == restoreInput; })) {
                        // MidiConnect stops a song, so one started meanwhile keeps it off.
                        const bool reconnect = restoreInputConnect && !state.playing;
                        if (!reconnect && !restoreInputActive) {
                            // An input left off comes back chosen and closed, as the config
                            // holds it: opening it only to close it again would let go of a
                            // playing song's keys. LiveActive opens it when it is turned on.
                            stopLive();
                            state.liveDevice = restoreInput;
                        } else {
                            Command open{Action::LiveOpen}; open.device = restoreInput; open.value = reconnect; open.automatic = true;
                            inputReopenAt = inputChoices;
                            Send(open);
                            reopeningInput = restoreInput;
                        }
                        // The group stays for a reopen that fails and waits again.
                        restoreInput.clear();
                        restoreInputConnect = false;
                    }
                    // Set after the restore above, so an input that is still listed
                    // reopens at a later scan rather than straight into what stopped it.
                    if (!closed.empty()) {
                        restoreInput = closed;
                        restoreInputGroup = closedGroup;
                        restoreInputActive = closedActive;
                        restoreInputConnect = closedConnect;
                    }
                    break;
                }
                case Action::LiveOpen: {
                    reopeningInput.clear();
                    if (command.automatic) {
                        const uint64_t queuedAt = std::exchange(inputReopenAt, noReopen);
                        if (queuedAt != noReopen && queuedAt != inputChoices) break;
                    }
                    // value opens the device through MidiConnect, restoring a
                    // session that left it on.
                    const bool connectRoute = state.midiConnect || command.value;
                    stopConnect();
                    stopLive();
                    if (command.device.empty()) {
                        if (live) { live->SetActive(false); live->CloseDevice(); }
                        state.liveDevice.clear();
                        state.liveActive = false;
                        break;
                    }
                    if (connectRoute) {
                        if (!connectFactory_) throw std::runtime_error("MidiConnect is unavailable in this host.");
                        state.liveDevice.clear();
                        connect = connectFactory_();
                        if (!connect || !connect->Open(command.device)) {
                            const bool busy = connect && connect->Busy();
                            stopConnect();
                            throw std::runtime_error(busy ? kInputBusy : "Cannot open that MIDI input for MidiConnect.");
                        }
                        state.liveDevice = command.device;
                        connect->Activate(true);
                        state.midiConnect = true;
                        break;
                    }
                    ensurePlayer();
                    if (!live) live = std::make_unique<MIDI2Key>(player.get());
                    live->SetMidiChannel(state.liveChannel);
                    live->OpenDevice(command.device);
                    state.liveDevice = live->GetSelectedDevice();
                    if (state.liveDevice.empty()) throw std::runtime_error(live->Busy() ? kInputBusy : "Cannot open that MIDI input.");
                    live->SetActive(true);
                    state.liveActive = true;
                    liveMappings = state.mappingRevision;
                    liveTranspose = state.transpose;
                    liveOctaves = state.octaves;
                    break;
                }
                case Action::LiveActive:
                    if (command.value) stopConnect();
                    if (state.liveDevice.empty()) break;
                    if (!live && command.value) {
                        ensurePlayer();
                        live = std::make_unique<MIDI2Key>(player.get());
                        live->SetMidiChannel(state.liveChannel);
                        live->OpenDevice(state.liveDevice);
                        if (live->GetSelectedDevice().empty()) throw std::runtime_error(live->Busy() ? kInputBusy : "Cannot reopen that MIDI input.");
                    }
                    if (!live) break;
                    live->SetActive(command.value);
                    state.liveActive = command.value;
                    if (!command.value) {
                        live->CloseDevice();
                        player->release_every_mapped_key();
                        live.reset();
                    }
                    break;
                case Action::LiveChannel:
                    state.liveChannel = std::clamp(static_cast<int>(command.amount), -1, 15);
                    if (live) live->SetMidiChannel(state.liveChannel);
                    break;
                case Action::OutputTarget:
                    ensurePlayer();
                    player->set_output_target(command.value ? VirtualPianoPlayer::OutputTarget::MidiDevice
                                                            : VirtualPianoPlayer::OutputTarget::Keystrokes);
                    state.outputMidi = command.value;
                    // An output waiting to come back returns on the target chosen meanwhile.
                    restoreOutputMidi = command.value;
                    if (!state.outputDevice.empty()) break;
                    if (!command.value) {
                        // Keystrokes stops the wait. This choice already
                        // outdates a reopen a scan has queued.
                        if (auto waiting = restoreOutput.empty() ? reopeningOutput : restoreOutput; !waiting.empty()) {
                            pausedOutput = std::move(waiting);
                            pausedOutputGroup = restoreOutputGroup;
                            restoreOutput.clear();
                            reopeningOutput.clear();
                        }
                    } else if (restoreOutput.empty()) {
                        // MIDI waits again, for a paused output or one whose
                        // queued reopen this choice outdated.
                        if (!pausedOutput.empty()) {
                            restoreOutput = std::exchange(pausedOutput, {});
                            restoreOutputGroup = std::exchange(pausedOutputGroup, {});
                        } else restoreOutput = std::exchange(reopeningOutput, {});
                        if (!restoreOutput.empty()) Send({Action::OutputScan});
                    }
                    break;
                case Action::OutputScan: {
                    // The chosen row's group, kept as for inputs.
                    const std::wstring chosenGroup = outputGroup.Update(state.outputDevices, state.outputDevice);
                    state.outputDevices.clear();
                    for (const auto& device : EnumerateMidiOutputs())
                        state.outputDevices.push_back({device.id, Utf8(std::filesystem::path(device.name)),
                            device.group, device.backend});
                    if (NamedMidiPortAvailable())
                        state.outputDevices.push_back({NamedMidiPortId(Wide(state.outputPortName)), state.outputPortName,
                            {}, MidiBackend::NamedPort});
                    // Reopened by the restore below once a scan lists it again.
                    if (!state.outputDevice.empty() &&
                        DeviceGone(state.outputDevices, state.outputDevice, BackendForOutputId(state.outputDevice), chosenGroup))
                        loseOutput(chosenGroup);
                    if (const auto listed = ReturnedId(state.outputDevices, restoreOutput, BackendForOutputId(restoreOutput), restoreOutputGroup);
                        !listed.empty()) restoreOutput = listed;
                    if (!restoreOutput.empty() && state.outputDevice.empty() &&
                        std::any_of(state.outputDevices.begin(), state.outputDevices.end(), [&](const LiveDevice& device) { return device.id == restoreOutput; })) {
                        // One command, so a failed open leaves the output on keystrokes.
                        Command open{Action::OutputOpen}; open.device = restoreOutput; open.value = restoreOutputMidi; open.automatic = true;
                        outputReopenAt = outputChoices;
                        Send(open);
                        reopeningOutput = std::exchange(restoreOutput, {});
                    }
                    if (std::exchange(startupOutput, false) && !restoreOutput.empty() && !state.outputMidi) {
                        pausedOutput = std::exchange(restoreOutput, {});
                        pausedOutputGroup = restoreOutputGroup;
                    }
                    break;
                }
                case Action::OutputOpen: {
                    reopeningOutput.clear();
                    if (!command.automatic) pausedOutput.clear();
                    if (command.automatic) {
                        const uint64_t queuedAt = std::exchange(outputReopenAt, noReopen);
                        if (queuedAt != noReopen && queuedAt != outputChoices) break;
                    }
                    // value also switches to the MIDI target, restoring a session that left it on.
                    ensurePlayer();
                    const bool resumeMidi = state.outputMidi || command.value;
                    // The target stays while the port changes, so a song on MIDI types nothing meanwhile.
                    player->drop_midi_output();
                    state.outputDevice.clear();
                    if (command.device.empty()) {
                        player->close_midi_output();
                        state.outputMidi = false;
                        break;
                    }
                    if (!player->open_midi_output(command.device) || (state.outputDevice = player->opened_midi_output()).empty()) {
                        const bool busy = player->midi_output_busy();
                        player->drop_midi_output();
                        state.outputDevice.clear();
                        // A reopen that fails waits on MIDI for the next scan; a choice that fails types.
                        if (command.automatic && resumeMidi) {
                            player->set_output_target(VirtualPianoPlayer::OutputTarget::MidiDevice);
                            state.outputMidi = true;
                            throw std::runtime_error(busy ? "That MIDI output is in use by another program." : "Cannot open that MIDI output.");
                        }
                        player->set_output_target(VirtualPianoPlayer::OutputTarget::Keystrokes);
                        state.outputMidi = false;
                        throw std::runtime_error(busy ? "That MIDI output is in use by another program; using keystrokes."
                                                      : "Cannot open that MIDI output; using keystrokes.");
                    }
                    if (resumeMidi) {
                        player->set_output_target(VirtualPianoPlayer::OutputTarget::MidiDevice);
                        state.outputMidi = true;
                    }
                    break;
                }
                case Action::OutputPortName: {
                    // A blank name keeps the one before.
                    const std::wstring id = NamedMidiPortId(Wide(command.key));
                    if (id.empty()) break;
                    const std::string name = Utf8(std::filesystem::path(NamedMidiPortName(id)));
                    if (name == state.outputPortName) break;
                    state.outputPortName = name;
                    for (auto& device : state.outputDevices)
                        if (device.backend == MidiBackend::NamedPort) { device.id = id; device.name = name; }
                    if (configLoaded) {
                        configJson["SHELL_SESSION"]["outputPortName"] = name;
                        touchConfig();
                    }
                    if (BackendForOutputId(state.outputDevice) == MidiBackend::NamedPort) {
                        Command open{Action::OutputOpen};
                        open.device = id;
                        Send(std::move(open));
                    }
                    break;
                }
                case Action::WootingTriggerThreshold:
                case Action::WootingShiftAmount:
                case Action::WootingVelocitySensitivity:
                case Action::WootingMinVelocity: {
                    if (!std::isfinite(command.amount)) break;
                    if (!configJson.is_object())
                        throw std::runtime_error("The configuration was not loaded, so it cannot be saved.");
                    auto& configured = midi::Config::getInstance().wooting;
                    const char* field = nullptr;
                    if (command.action == Action::WootingTriggerThreshold) {
                        configured.TRIGGER_THRESHOLD = std::clamp(std::round(command.amount * 100.0) / 100.0, 0.01, 1.0);
                        field = "TRIGGER_THRESHOLD";
                        configJson["WOOTING_ANALOG"][field] = configured.TRIGGER_THRESHOLD;
                    } else if (command.action == Action::WootingShiftAmount) {
                        configured.SHIFT_AMOUNT = static_cast<int>(std::round(std::clamp(command.amount, -127.0, 127.0)));
                        field = "SHIFT_AMOUNT";
                        configJson["WOOTING_ANALOG"][field] = configured.SHIFT_AMOUNT;
                    } else if (command.action == Action::WootingVelocitySensitivity) {
                        configured.VELOCITY_SENSITIVITY = std::clamp(std::round(command.amount * 10.0) / 10.0, 0.1, 10.0);
                        field = "VELOCITY_SENSITIVITY";
                        configJson["WOOTING_ANALOG"][field] = configured.VELOCITY_SENSITIVITY;
                        // Upstream's number, which this one replaced; left in
                        // the file it would read as a setting that does nothing.
                        configJson["WOOTING_ANALOG"].erase("VELOCITY_SCALE");
                    } else {
                        configured.MIN_VELOCITY = static_cast<int>(std::round(std::clamp(command.amount, 1.0, 126.0)));
                        field = "MIN_VELOCITY";
                        configJson["WOOTING_ANALOG"][field] = configured.MIN_VELOCITY;
                    }
                    configured.validate();
                    applyWootingSettings();
                    touchConfig();
                    // While dragging, the save is debounced. On release (value)
                    // flush now so a save failure shows in Settings.
                    if (command.value) flushConfig();
                    break;
                }
                case Action::WootingPedalKey: {
                    if (command.track >= 3 || !std::isfinite(command.amount)) break;
                    if (!configJson.is_object())
                        throw std::runtime_error("The configuration was not loaded, so it cannot be saved.");
                    const int key = static_cast<int>(command.amount);
                    if (key < 0 || key > 0xE0FF || (key > 0xFF && key < 0xE000)) break;
                    auto& configured = midi::Config::getInstance().wooting;
                    std::array<int*, 3> fields{&configured.SUSTAIN_PEDAL_KEY, &configured.SOSTENUTO_PEDAL_KEY, &configured.SOFT_PEDAL_KEY};
                    constexpr std::array<const char*, 3> names{"SUSTAIN_PEDAL_KEY", "SOSTENUTO_PEDAL_KEY", "SOFT_PEDAL_KEY"};
                    for (size_t pedal = 0; pedal < 3; ++pedal) {
                        if (pedal == command.track) *fields[pedal] = key;
                        else if (key != 0 && *fields[pedal] == key) *fields[pedal] = 0;
                        else continue;
                        configJson["WOOTING_ANALOG"][names[pedal]] = *fields[pedal];
                    }
                    applyWootingSettings();
                    touchConfig();
                    flushConfig();
                    break;
                }
                case Action::SostenutoKey: {
                    if (!configJson.is_object())
                        throw std::runtime_error("The configuration was not loaded, so it cannot be saved.");
                    const int vk = NameToVK(command.key);
                    // Capture passes over these; one sent anyway is ignored.
                    if (state.SostenutoKeyRefused(vk)) break;
                    if (command.key.rfind("VK_", 0) != 0) command.key.insert(0, "VK_");
                    if (NameToVK(state.sostenutoKey) == vk) break;
                    // The app types the key, which would fire a hotkey on it.
                    for (size_t i = 0; i < kHotkeys; ++i) {
                        if (hotkeyFields[i].empty() || NameToVK(state.hotkeys[i]) != vk) continue;
                        state.hotkeys[i].clear();
                        configJson["HOTKEY_SETTINGS"][hotkeyFields[i]] = std::string();
                        ++state.hotkeyRevision;
                    }
                    midi::Config::getInstance().hotkeys.SOSTENUTO_KEY = command.key;
                    configJson["HOTKEY_SETTINGS"]["SOSTENUTO_KEY"] = command.key;
                    state.sostenutoKey = command.key;
                    if (player) player->set_sostenuto_key(static_cast<WORD>(vk));
                    touchConfig();
                    flushConfig();
                    break;
                }
                // Stop ends with a sweep that trusts no record of what is down,
                // so a stuck key comes up on Stop's key. The song's own keys
                // come up first, in stopPlayback.
                case Action::Stop:
                    stopPlayback();
                    state.position = 0;
                    stopLive();
                    stopConnect();
                    // An input waiting to come back comes back off, as Stop leaves an open one.
                    restoreInputActive = false;
                    restoreInputConnect = false;
                    if (player) player->panic();
                    break;
                case Action::Mute:
                case Action::Solo:
                    for (auto& row : state.rows) {
                        if (row.index != command.track) continue;
                        if (command.action == Action::Mute) row.muted = command.value;
                        else row.solo = command.value;
                    }
                    applyTracks();
                    invalidateSheet();
                    break;
                // Report a no-op in the status bar.
                case Action::SoloPiano: {
                    const auto before = state.rows;
                    SoloPiano(state.rows); applyTracks(); invalidateSheet();
                    if (before == state.rows) state.error = "Every track is piano, so Solo Piano muted nothing.";
                    break;
                }
                case Action::UnmuteAll: {
                    const auto before = state.rows;
                    UnmuteAll(state.rows); applyTracks(); invalidateSheet();
                    if (before == state.rows) state.error = "No track was muted or soloed.";
                    break;
                }
                case Action::ConvertAudio: {
                    // Queued, also while a conversion runs. value requests the
                    // whole playlist when the link has one.
                    if (!command.key.empty()) enqueue(linkEntry(command.key, command.value));
                    else if (!command.path.empty()) {
                        QueueEntry entry;
                        entry.key = audio_to_midi::FileKey(command.path);
                        entry.kind = "file";
                        entry.title = Utf8(command.path.filename());
                        entry.file = command.path;
                        enqueue(std::move(entry));
                    }
                    break;
                }
                case Action::YouTubeBrowse: {
                    if (state.browsingYouTube) {
                        // Words become a search in the open window, once it has said hello.
                        if (!command.key.empty()) {
                            if (browseProtocol == 2) tellWindow("search", {{"type", "search"}, {"q", command.key}});
                            else browseSearch = command.key;
                        }
                        if (const HWND window = OwnYouTubeWindow()) {
                            if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
                            SetForegroundWindow(window);
                        }
                        break;
                    }
                    if (state.signingIn) {
                        // The window opens once the sign-in window closes, with its words.
                        browseAfterSetup = true;
                        browseAfterSetupWords = command.key;
                        break;
                    }
                    const auto install = converterInstall();
                    if (!install.Found() && install.CanSetUp()) {
                        // Installs the converter, then opens the window.
                        browseAfterSetup = true;
                        browseAfterSetupWords = command.key;
                        if (!state.converting) startSetup(install);
                        break;
                    }
                    if (!install.Found()) {
                        readConverter();
                        state.conversionFailed = true;
                        state.conversionStatus = kConverterMissing;
                        break;
                    }
                    if (install.browse.empty()) {
                        state.conversionFailed = true;
                        state.conversionStatus = "This converter add-on has no YouTube window. Unzip the latest one into the addons folder.";
                        break;
                    }
                    // Its stdin is a pipe, written first with init. A browse.py of
                    // the first protocol never reads it.
                    browseProtocol = 0;
                    state.youtubeWindowSignsIn = false;
                    initSkin = browseSkin;
                    initFolders = libraryFolders;
                    browseSearch.clear();
                    state.youtubeSignedIn = install.SignedIn();
                    state.browsingYouTube = browser.Start(audio_to_midi::BrowseCommandLine(install.python, install.browse), reportBrowse, true);
                    if (!state.browsingYouTube) {
                        state.conversionFailed = true;
                        state.conversionStatus = "The YouTube window could not start.";
                        break;
                    }
                    auto skin = nlohmann::json::parse(browseSkin, nullptr, false);
                    if (!skin.is_object()) skin = nullptr;
                    browser.Write("", jsonText({{"type", "init"}, {"protocol", audio_to_midi::kProtocol},
                                                {"pid", static_cast<unsigned>(GetCurrentProcessId())}, {"skin", skin},
                                                {"signedIn", state.youtubeSignedIn}, {"folders", libraryFolders},
                                                {"search", command.key.empty() ? nlohmann::json() : nlohmann::json(command.key)}}));
                    break;
                }
                case Action::BrowseProgress: {
                    using Kind = audio_to_midi::Status::Kind;
                    const auto kind = static_cast<Kind>(command.track);
                    if (kind == Kind::Text) {
                        // hello: 2 starts the second protocol: qm: messages
                        // from the window, and its stdin told the queue.
                        if (audio_to_midi::ParseHello(command.key) == 2) {
                            browseProtocol = 2;
                            state.youtubeWindowSignsIn = true;
                            tellAll();
                            break;
                        }
                        if (const auto message = audio_to_midi::ParseWindowLine(command.key)) {
                            handleWindow(*message);
                            break;
                        }
                        // The first protocol: pick:, pick-list: and pick-channel:
                        // carry a link to convert; signed: follows a new YouTube
                        // session written to cookies.txt.
                        const auto after = [&](std::string_view prefix) {
                            return command.key.rfind(prefix, 0) == 0 ? command.key.substr(prefix.size()) : std::string();
                        };
                        if (const auto link = after("pick-list: "); !link.empty()) pickLink(link, true);
                        else if (const auto channel = after("pick-channel: "); !channel.empty()) pickLink(channel, true);
                        else if (const auto video = after("pick: "); !video.empty()) pickLink(video, false);
                        else if (command.key.rfind("signed: ", 0) == 0) state.youtubeSignedIn = converterInstall().SignedIn();
                        else ShellLog::Instance().Append("[youtube] " + command.key + "\n");
                        break;
                    }
                    if (kind == Kind::Step || kind == Kind::Saved) break;
                    // Any final status: the window closed.
                    browseProtocol = 0;
                    state.youtubeWindowSignsIn = false;
                    browseSearch.clear();
                    state.browsingYouTube = false;
                    state.youtubeSignedIn = converterInstall().SignedIn();
                    if (kind == Kind::Error && command.key != "Conversion cancelled.") {
                        ShellLog::Instance().Append("[error] YouTube window: " + command.key + "\n");
                        if (!state.converting) {
                            state.conversionFailed = true;
                            state.conversionStatus = command.key.rfind("The converter stopped", 0) == 0
                                ? "The YouTube window stopped unexpectedly." : command.key;
                        }
                    }
                    break;
                }
                case Action::YouTubeSignIn: {
                    // The open window signs in itself, also while a conversion runs.
                    if (state.browsingYouTube && browseProtocol == 2) {
                        tellWindow("signin", {{"type", "signin"}});
                        break;
                    }
                    if (state.converting) { state.error = "Wait for the conversion to finish before signing in."; break; }
                    if (state.browsingYouTube) { state.error = "Sign in from the YouTube window."; break; }
                    const auto install = converterInstall();
                    if (!install.Found() || install.signin.empty()) {
                        readConverter();
                        state.conversionFailed = true;
                        state.conversionStatus = kConverterMissing;
                        break;
                    }
                    const bool started = converter.Start(audio_to_midi::SignInCommandLine(install.python, install.signin), reportConversion);
                    state.signingIn = state.converting = started;
                    state.conversionFailed = !started;
                    state.conversionStatus = started ? "Sign in to YouTube in the window that opened." : "The sign-in window could not start.";
                    break;
                }
                case Action::ConverterSetUp: {
                    if (state.converting) break;
                    const auto install = converterInstall();
                    if (!install.CanSetUp()) { readConverter(); break; }
                    startSetup(install);
                    break;
                }
                case Action::ConverterEngine: {
                    if (state.converting) break;
                    const auto install = converterInstall();
                    if (!install.Found() || install.setup.empty() || audio_to_midi::EngineName(command.key).empty() ||
                        command.key == install.engine) { readConverter(); break; }
                    ShellLog::Instance().Append("[convert] Switching to " + audio_to_midi::EngineName(command.key) + "\n");
                    startSetup(install, command.key);
                    break;
                }
                case Action::ConvertCancel:
                    // Close on the sign-in window ends it alone; what waits, and a pause for a sign-in, stay.
                    if (state.signingIn) { converter.Cancel(); break; }
                    // Cancel stops everything: the running entry and those waiting.
                    if (running) tellItem(*running, "removed", nlohmann::json::object());
                    for (const auto& entry : waitingConversions) tellItem(entry, "removed", nlohmann::json::object());
                    waitingConversions.clear();
                    state.conversionsWaiting = 0;
                    if (state.conversionsPaused) {
                        state.conversionsPaused = false;
                        state.pausedReason.clear();
                        tellWindow("pause", {{"type", "resumed"}});
                    }
                    converter.Cancel();
                    if (running) {
                        removeList(running->listFile);
                        running.reset();
                    }
                    break;
                case Action::ConvertProgress: {
                    using Kind = audio_to_midi::Status::Kind;
                    const auto kind = static_cast<Kind>(command.track);
                    // A list run: a playlist or channel read whole, or a list file.
                    const bool list = running && (running->playlist || !running->listFile.empty());
                    if (kind == Kind::Text) {
                        // Plain converter output goes to the log; a list's lines
                        // also tell the window about its videos.
                        ShellLog::Instance().Append("[convert] " + command.key + "\n");
                        if (!running) break;
                        if (const auto item = audio_to_midi::ParseItemLine(command.key)) {
                            run.childId = item->id;
                            run.total = item->total;
                            run.done = item->number - 1;
                            run.phase = "list";
                            run.percent = -1;
                            tellChild(running->key, item->id, "converting", nlohmann::json::object());
                            tellItem(*running, "converting", runningFields());
                        } else if (const auto failed = audio_to_midi::ParseFailedLine(command.key)) {
                            run.shortReason = failed->text;
                            run.finished.insert(failed->id);
                            ++run.failed;
                            if (list) tellChild(running->key, failed->id, "failed", {{"reason", failed->text}});
                        } else if (const auto exists = audio_to_midi::ParseExistsLine(command.key)) {
                            run.finished.insert(exists->id);
                            ++run.existing;
                            if (list) {
                                tellChild(running->key, exists->id, "done", {{"path", exists->text}, {"already", true}});
                            } else {
                                // It stays where it was saved, whatever folder was picked.
                                auto shown = *running;
                                shown.dest.reset();
                                tellItem(shown, "done", {{"path", exists->text}, {"already", true}});
                            }
                        } else if (audio_to_midi::IsBlockedLine(command.key)) {
                            run.blocked = true;
                        }
                        break;
                    }
                    if (kind == Kind::Step) {
                        state.conversionStatus = command.key;
                        if (running) {
                            if (auto phase = audio_to_midi::PhaseOf(command.key); !phase.empty()) run.phase = std::move(phase);
                            run.percent = audio_to_midi::StepPercent(command.key);
                            const auto percent = run.percent >= 0 ? nlohmann::json(run.percent) : nlohmann::json();
                            tellItem(*running, "converting", runningFields());
                            if (!run.childId.empty())
                                tellChild(running->key, run.childId, "converting", {{"phase", run.phase}, {"percent", percent}});
                        }
                        break;
                    }
                    if (kind == Kind::Saved) {
                        // One playlist item saved; rescan and keep going.
                        ++convertedCount;
                        ShellLog::Instance().Append("[convert] Saved " + command.key + "\n");
                        if (running && !run.childId.empty()) {
                            run.finished.insert(run.childId);
                            tellChild(running->key, run.childId, "done", {{"path", command.key}});
                        }
                        Send({Action::Scan, state.folder, 0, 0, true});
                        break;
                    }
                    state.converting = false;
                    state.conversionFailed = kind == Kind::Error;
                    if (state.settingUp) {
                        // Setup is resumable; downloaded packages are kept.
                        state.settingUp = false;
                        readConverter();
                        // setup.ps1 says which engine it runs on.
                        state.conversionStatus = kind == Kind::Done ? (command.key.empty() ? "The converter is installed." : command.key)
                            : command.key == "Conversion cancelled." ? "Setup cancelled." : command.key;
                        if (kind == Kind::Error) ShellLog::Instance().Append("[error] Converter setup failed: " + command.key + "\n");
                        // The YouTube button that started it opens the window now.
                        if (std::exchange(browseAfterSetup, false) && kind == Kind::Done)
                            Send({Action::YouTubeBrowse, {}, 0, 0, false, 0, std::exchange(browseAfterSetupWords, {})});
                        startWaiting();
                        break;
                    }
                    if (state.signingIn) {
                        state.signingIn = false;
                        state.youtubeSignedIn = converterInstall().SignedIn();
                        state.conversionStatus = kind == Kind::Done ? "Signed in to YouTube. Links can be converted now."
                            : command.key == "Conversion cancelled." ? "Sign-in cancelled." : command.key;
                        tellWindow("signed", {{"type", "signed"}, {"in", state.youtubeSignedIn}});
                        if (state.youtubeSignedIn && state.conversionsPaused && state.pausedReason == "signin") resumeQueue();
                        else startWaiting();
                        // The YouTube button pressed meanwhile opens the window now, however it ended.
                        if (std::exchange(browseAfterSetup, false))
                            Send({Action::YouTubeBrowse, {}, 0, 0, false, 0, std::exchange(browseAfterSetupWords, {})});
                        break;
                    }
                    if (!running) {
                        // A run the queue no longer has, as after Cancel.
                        if (kind == Kind::Error) {
                            state.conversionStatus = command.key;
                            if (convertedCount)
                                state.conversionStatus += " " + std::to_string(convertedCount) +
                                    (convertedCount == 1 ? " file was" : " files were") + " saved before it stopped.";
                            ShellLog::Instance().Append("[error] Conversion failed: " + command.key + "\n");
                        } else {
                            state.conversionStatus = kind == Kind::Finished ? command.key
                                : "Converted " + Utf8(std::filesystem::path(std::u8string(command.key.begin(), command.key.end())).filename()) + ".";
                            Send({Action::Scan, state.folder, 0, 0, true});
                        }
                        startWaiting();
                        break;
                    }
                    auto entry = std::move(*running);
                    running.reset();
                    removeList(entry.listFile);
                    if (kind == Kind::Error && run.blocked) {
                        // YouTube wants a sign-in: what is left goes first again,
                        // once signed in or on Retry.
                        ShellLog::Instance().Append("[error] Conversion failed: " + command.key + "\n");
                        std::erase_if(entry.items, [&](const auto& item) { return run.finished.contains(item.first); });
                        entry.listFile.clear();
                        waitingConversions.push_front(std::move(entry));
                        state.conversionFailed = false;
                        state.conversionStatus = "YouTube asked to sign in.";
                        pause("signin");
                        tellPositions();
                        break;
                    }
                    if (kind == Kind::Error && run.dropped) {
                        // Stopped from the window. A list that lost the video it
                        // was converting is waiting first, to start again.
                        if (waitingConversions.empty() || waitingConversions.front().key != entry.key)
                            tellItem(entry, "removed", nlohmann::json::object());
                        state.conversionFailed = false;
                        state.conversionStatus = "Stopped.";
                    } else if (kind == Kind::Error) {
                        // convert.py refused the folder before any video. A list
                        // that saved or found some videos before it stopped did
                        // convert some.
                        const bool folderReason = command.key == "Folder not allowed." || command.key == "Path too long.";
                        tellItem(entry, "failed", {{"reason", folderReason ? command.key
                                                          : list ? (convertedCount + run.existing ? "Conversion failed." : "No videos converted.")
                                                          : !run.shortReason.empty() ? run.shortReason : "Conversion failed."}});
                        state.conversionStatus = command.key;
                        if (convertedCount)
                            state.conversionStatus += " " + std::to_string(convertedCount) +
                                (convertedCount == 1 ? " file was" : " files were") + " saved before it stopped.";
                        ShellLog::Instance().Append("[error] Conversion failed: " + command.key + "\n");
                    } else if (kind == Kind::Finished) {
                        // A single link already converted was told so by its exists: line.
                        if (list)
                            tellItem(entry, "done", {{"done", static_cast<int>(convertedCount) + run.existing},
                                                     {"total", run.total}, {"failed", run.failed}});
                        else if (!run.existing) tellItem(entry, "done", nlohmann::json::object());
                        state.conversionStatus = command.key;
                        Send({Action::Scan, state.folder, 0, 0, true});
                    } else {
                        tellItem(entry, "done", {{"path", command.key}});
                        const auto name = Utf8(std::filesystem::path(std::u8string(command.key.begin(), command.key.end())).filename());
                        state.conversionStatus = "Converted " + name + ".";
                        Send({Action::Scan, state.folder, 0, 0, true});
                    }
                    // What was picked meanwhile goes next.
                    startWaiting();
                    break;
                }
                case Action::BrowseTheme: {
                    // Kept for the next window's init, and sent to an open one.
                    browseSkin = command.key;
                    const auto skin = nlohmann::json::parse(browseSkin, nullptr, false);
                    if (skin.is_object()) tellWindow("theme", {{"type", "theme"}, {"skin", skin}});
                    break;
                }
                case Action::ConverterCpu:
                    converterCpu = std::clamp(static_cast<int>(command.amount), 25, 100);
                    break;
                case Action::FolderAsked:
                    state.folderWanted = false;
                    break;
                case Action::CopySheet: {
                    if (!player || state.loaded.empty()) break;
                    const bool anySolo = AnySolo(state.rows);
                    const auto audible = [&](int track) {
                        const auto row = std::find_if(state.rows.begin(), state.rows.end(),
                            [&](const TrackRow& candidate) { return candidate.index == static_cast<size_t>(track); });
                        return row != state.rows.end() && TrackAudible(*row, anySolo);
                    };
                    auto notes = nlohmann::json::array();
                    for (size_t i = 0; i < player->note_events.size(); ++i) {
                        const auto& event = player->note_events[i];
                        if (event.action != EventType::Press || PedalForName(event.note_or_control) >= 0 || !audible(event.trackIndex)) continue;
                        const auto time = i < scoreTimes.size() ? scoreTimes[i] : event.time;
                        notes.push_back({static_cast<double>(time.count()) / 1e9, std::string(event.note_or_control)});
                    }
                    nlohmann::json request{{"do", "text"}, {"mapping", state.keyMappings}};
                    if (!player->midi_file.tempoChanges.empty()) {
                        const auto tempo = std::min_element(player->midi_file.tempoChanges.begin(), player->midi_file.tempoChanges.end(),
                            [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });
                        request["beatSeconds"] = static_cast<double>(tempo->microsecondsPerQuarter) / 1e6;
                    }
                    request["notes"] = std::move(notes);
                    const auto answer = AskSheets(sheetsCall, request);
                    state.sheetSaved.clear();
                    state.sheetFilesSaved.clear();
                    takeSheet(answer);
                    break;
                }
                case Action::OpenSheetEditor: {
                    if (!player || state.loaded.empty()) break;
                    // Write the self-contained editor page to %TEMP%\QuartzMIDI sheets
                    // for the panel to open; nothing is written to the MIDI folder.
                    // It opens with the style Save sheet files uses and sends its changes back.
                    if (!sheetStyleListener && sheetsCall)
                        sheetStyleListener = std::make_unique<SheetStyleListener>([this](std::string style) {
                            Command keep{Action::KeepSheetStyle};
                            keep.key = std::move(style);
                            Send(std::move(keep));
                        });
                    const auto answer = AskSheets(sheetsCall, {{"do", "page"}, {"page", playerPage()}, {"style", Utf8(state.sheetStylePage)},
                                                               {"kept", keptSheetStyle}, {"keepUrl", sheetStyleListener ? sheetStyleListener->Url() : std::string()}});
                    state.sheetSaved.clear();
                    state.sheetFilesSaved.clear();
                    if (answer.value("notes", size_t{0}) > 0) {
                        std::error_code ignored;
                        const auto folder = std::filesystem::temp_directory_path(ignored) / L"QuartzMIDI sheets";
                        std::filesystem::create_directories(folder, ignored);
                        auto path = folder / state.loaded.filename(); path.replace_extension(L".html");
                        std::ofstream output(path, std::ios::binary);
                        output << answer.value("html", "");
                        output.flush();
                        if (!output) throw std::runtime_error("Cannot write " + Utf8(path) + ".");
                        state.sheetSaved = path;
                    }
                    takeSheet(answer);
                    break;
                }
                case Action::SaveSheetFiles: {
                    if (!player || state.loaded.empty()) break;
                    // Rendered with the style page's settings, or the editor's last, to SheetTarget.
                    const auto stem = SheetTarget(sheetsRoot(), state.folder, state.loaded);
                    const auto answer = AskSheets(sheetsCall, {{"do", "files"}, {"page", playerPage()}, {"style", Utf8(state.sheetStylePage)},
                                                               {"kept", keptSheetStyle}, {"outputs", sheetOutputs()}, {"stem", Utf8(stem)}});
                    state.sheetSaved.clear();
                    state.sheetFilesSaved = stem.parent_path();
                    takeSheet(answer);
                    break;
                }
                case Action::SheetsFolder:
                    state.sheetsFolder = command.path;
                    configJson["SHELL_SHEETS_FOLDER"] = Utf8(command.path);
                    touchConfig();
                    flushConfig();
                    break;
                case Action::SheetStylePage:
                    // Validate the style page once, up front.
                    if (!command.path.empty()) AskSheets(sheetsCall, {{"do", "style"}, {"style", Utf8(command.path)}});
                    state.sheetStylePage = command.path;
                    configJson["SHELL_SHEET_STYLE_PAGE"] = Utf8(command.path);
                    touchConfig();
                    flushConfig();
                    break;
                case Action::KeepSheetStyle:
                    AskSheets(sheetsCall, {{"do", "keep"}, {"style", command.key}, {"kept", keptSheetStyle}});
                    break;
                case Action::SheetFiles:
                    if (command.key == "image") state.sheetImage = command.value;
                    else if (command.key == "text") state.sheetTextFile = command.value;
                    else if (command.key == "page") state.sheetPageFile = command.value;
                    else throw std::runtime_error("No sheet file is called " + command.key + ".");
                    configJson["SHELL_SHEET_FILES"] = {{"image", state.sheetImage}, {"text", state.sheetTextFile}, {"page", state.sheetPageFile}};
                    touchConfig();
                    flushConfig();
                    break;
                case Action::SaveLibrarySheets: {
                    if (state.sheetBatchRunning) throw std::runtime_error("The library is already being saved. Stop that first.");
                    if (state.files->empty()) throw std::runtime_error("Choose a MIDI folder first.");
                    // Runs on its own thread and parses each file directly; it
                    // never touches the player.
                    if (!sheetsCall) throw std::runtime_error("The sheets add-on is not installed.");
                    const auto style = Utf8(state.sheetStylePage);
                    const auto files = state.files;
                    const auto folder = state.folder;
                    const auto root = sheetsRoot();
                    const auto mapping = state.keyMappings;
                    const auto outputs = sheetOutputs();
                    state.sheetBatchRunning = true;
                    state.sheetBatchDone = state.sheetBatchFailed = 0;
                    state.sheetBatchTotal = files->size();
                    state.sheetBatchStatus = "Saving sheets: 0 of " + std::to_string(files->size()) + ".";
                    sheetBatch = std::jthread([this, sheetsCall, style, kept = keptSheetStyle, files, folder, root, mapping, outputs](std::stop_token stopBatch) {
                        size_t done = 0, failed = 0;
                        for (const auto& entry : *files) {
                            if (stopBatch.stop_requested()) break;
                            std::filesystem::path failedFile;
                            std::string why;
                            try {
                                MidiParser parser;
                                const auto file = parser.parse(Utf8(std::filesystem::absolute(entry.path)));
                                if (file.format == 2) throw std::runtime_error("MIDI format 2 contains independent sequences.");
                                AskSheets(sheetsCall, {{"do", "files"}, {"page", PageForFile(file, Utf8(entry.path.stem()), mapping)}, {"style", style},
                                                       {"kept", kept}, {"outputs", outputs}, {"stem", Utf8(SheetTarget(root, folder, entry.path))}});
                            } catch (const std::exception& error) {
                                ++failed;
                                failedFile = entry.path;
                                why = error.what();
                            }
                            ++done;
                            Send({Action::SheetBatchProgress, failedFile, 0, done, false, static_cast<double>(failed), why});
                        }
                        Send({Action::SheetBatchProgress, {}, 0, done, true, static_cast<double>(failed), stopBatch.stop_requested() ? "stopped" : ""});
                    });
                    break;
                }
                case Action::SheetBatchCancel:
                    sheetBatch.request_stop();
                    break;
                case Action::SheetBatchProgress: {
                    if (!state.sheetBatchRunning) break;
                    state.sheetBatchDone = command.track;
                    state.sheetBatchFailed = static_cast<size_t>(command.amount);
                    if (!command.path.empty())
                        ShellLog::Instance().Append("[sheets] " + Utf8(command.path) + ": " + command.key + "\n");
                    const auto total = std::to_string(state.sheetBatchTotal);
                    if (!command.value) {
                        state.sheetBatchStatus = "Saving sheets: " + std::to_string(state.sheetBatchDone) + " of " + total + ".";
                        break;
                    }
                    state.sheetBatchRunning = false;
                    state.sheetBatchStatus = std::string(command.key == "stopped" ? "Stopped. " : "") + "Saved sheets for " +
                        std::to_string(state.sheetBatchDone - state.sheetBatchFailed) + " of " + total + " files";
                    if (state.sheetBatchFailed)
                        state.sheetBatchStatus += ", " + std::to_string(state.sheetBatchFailed) + " failed (the log says why)";
                    state.sheetBatchStatus += " in " + Utf8(sheetsRoot()) + ".";
                    break;
                }
                case Action::CurveSelect:
                case Action::CurveAdjust:
                case Action::CurveEdit:
                case Action::CurveUndo:
                case Action::CurveRedo:
                case Action::CurveCompare:
                case Action::CurveNew:
                case Action::CurveDuplicate:
                case Action::CurveRename:
                case Action::CurveDelete:
                case Action::SustainCutoff: {
                    if (state.curves.size() < midi::kBuiltinVelocityCurves || !std::isfinite(command.amount)) break;
                    // Live input reads the cutoff at each pedal message, so while
                    // nothing plays a new cutoff needs no rebuild, and the held
                    // notes and the open port are left alone.
                    if (command.action == Action::SustainCutoff && !state.playing) {
                        auto next = state;
                        next.sustainCutoff = static_cast<int>(std::clamp(command.amount, 0.0, 127.0));
                        saveCurves(next);
                        state.sustainCutoff = next.sustainCutoff;
                        g_sustainCutoff = state.sustainCutoff;
                        break;
                    }
                    auto next = state;
                    auto nextHistory = curveHistory;
                    bool changedCurve = false;
                    const auto remember = [&] {
                        next.previousCurve = state.comparingCurve ? state.previousCurve : state.curve;
                        next.previousPreset = state.comparingCurve ? state.previousPreset : state.curves[state.curve.preset];
                        next.hasPreviousCurve = true; next.comparingCurve = false;
                    };
                    if (command.action == Action::CurveCompare) {
                        if (!next.hasPreviousCurve) break;
                        next.comparingCurve = !next.comparingCurve;
                    } else if (command.action == Action::SustainCutoff) {
                        next.sustainCutoff = static_cast<int>(std::clamp(command.amount, 0.0, 127.0));
                    } else if (command.action == Action::CurveSelect) {
                        if (command.track >= next.curves.size()) break;
                        remember(); next.curve = {}; next.curve.preset = command.track; changedCurve = true;
                    } else if (command.action == Action::CurveAdjust) {
                        remember(); next.curve.anchors.clear();
                        if (command.key == "sensitivity") next.curve.sensitivity = static_cast<float>(std::clamp(command.amount, -50.0, 50.0));
                        else if (command.key == "contrast") next.curve.contrast = static_cast<float>(std::clamp(command.amount, 0.0, 100.0));
                        else break;
                        changedCurve = true;
                    } else if (command.action == Action::CurveEdit) {
                        if (command.anchors.size() < 2 || command.anchors.size() > 256) break;
                        remember();
                        next.curve.anchors = VelocityLegalAnchors(command.anchors);
                        next.curve.sensitivity = next.curve.contrast = 0;
                        changedCurve = true;
                    } else if (command.action == Action::CurveUndo || command.action == Action::CurveRedo) {
                        remember();
                        const bool changed = command.action == Action::CurveUndo ? nextHistory.Undo() : nextHistory.Redo();
                        if (!changed) break;
                        next.curve = nextHistory.current;
                        changedCurve = true;
                    } else if (command.action == Action::CurveDelete) {
                        const size_t index = command.track;
                        if (index < midi::kBuiltinVelocityCurves || index >= next.curves.size()) break;
                        next.curves.erase(next.curves.begin() + static_cast<std::ptrdiff_t>(index));
                        // Deleting the active curve falls back to the default
                        // built-in, which can't be deleted, so a second click
                        // can't take another curve with it. Later curves move
                        // down one place.
                        const auto rebase = [&](VelocityEdit& edit) { if (edit.preset > index) --edit.preset; };
                        if (next.curve.preset == index) next.curve = {};
                        else rebase(next.curve);
                        if (next.previousCurve.preset == index) next.hasPreviousCurve = false;
                        else rebase(next.previousCurve);
                        next.comparingCurve = false;
                        // The curve can't come back, so undo steps on it are
                        // dropped; the rest keep working on the renumbered list.
                        for (auto* steps : {&nextHistory.undo, &nextHistory.redo}) {
                            std::erase_if(*steps, [&](const VelocityEdit& edit) { return edit.preset == index; });
                            for (auto& edit : *steps) rebase(edit);
                            steps->erase(std::unique(steps->begin(), steps->end()), steps->end());
                            if (!steps->empty() && steps->back() == next.curve) steps->pop_back();
                        }
                        nextHistory.current = next.curve;
                    } else {
                        auto name = command.key;
                        const auto first = name.find_first_not_of(" \t\r\n"), last = name.find_last_not_of(" \t\r\n");
                        if (first == std::string::npos) throw std::runtime_error("Enter a curve name.");
                        name = name.substr(first, last - first + 1);
                        if (name.size() > 120 || name.find_first_of("\r\n\t") != std::string::npos)
                            throw std::runtime_error("Use a curve name of at most 120 bytes on one line.");
                        const bool rename = command.action == Action::CurveRename && next.curve.preset >= midi::kBuiltinVelocityCurves;
                        for (size_t i = 0; i < next.curves.size(); ++i)
                            if ((!rename || i != next.curve.preset) && next.curves[i].name == name)
                                throw std::runtime_error("A curve already has that name.");
                        remember();
                        const auto values = command.action == Action::CurveNew ? next.curves[1].thresholds :
                            VelocityThresholds(next.curves[next.curve.preset], next.curve);
                        size_t index = next.curve.preset;
                        if (rename) next.curves[index] = {name, values};
                        else { index = next.curves.size(); next.curves.push_back({name, values}); }
                        next.curve = {}; next.curve.preset = index;
                        changedCurve = true;
                    }
                    if (changedCurve && command.action != Action::CurveUndo && command.action != Action::CurveRedo)
                        nextHistory.Commit(next.curve);
                    next.canUndoCurve = !nextHistory.undo.empty();
                    next.canRedoCurve = !nextHistory.redo.empty();
                    // Save before committing `next`: a save failure throws and
                    // leaves the active curve untouched.
                    if (command.action != Action::CurveCompare) saveCurves(next);
                    const bool resume = state.playing;
                    const auto device = state.liveDevice;
                    const bool active = state.liveActive;
                    // An input the user turned off stays closed.
                    const bool hadLive = live != nullptr;
                    if (live && !device.empty()) {
                        live->SetActive(false);
                        live->CloseDevice(); // Close the port before rebuilding its lookup.
                    }
                    stopPlayback();
                    if (live && !device.empty()) {
                        // Live note ownership is internal to MIDI2Key. Release its
                        // mapped keys before replacing that object and its caches.
                        if (active && player) player->release_every_mapped_key();
                        live.reset();
                    }
                    next.position = state.position; next.playing = false;
                    state = std::move(next); curveHistory = std::move(nextHistory); ++state.curveRevision;
                    applyCurve();
                    // MidiConnect holds the port while it is on.
                    if (hadLive && !device.empty() && !state.midiConnect) {
                        live = std::make_unique<MIDI2Key>(player.get());
                        live->SetMidiChannel(state.liveChannel);
                        live->OpenDevice(device);
                        state.liveDevice = live->GetSelectedDevice();
                        state.liveActive = active && !state.liveDevice.empty();
                        live->SetActive(state.liveActive);
                        if (state.liveDevice.empty()) state.error = "Velocity saved; MIDI input could not reopen.";
                    }
                    if (resume && state.position < state.duration) startPlayback();
                    break;
                }
                case Action::EightyEightKeys:
                case Action::OutRange: {
                    const bool layoutChange = command.action == Action::EightyEightKeys;
                    if ((layoutChange ? state.eightyEightKeys : state.outRange) == command.value) break;
                    const bool layout88 = layoutChange ? command.value : state.eightyEightKeys;
                    auto mappings = configJson.at("KEY_MAPPINGS").at(layout88 ? "FULL" : "LIMITED")
                        .get<decltype(state.keyMappings)>();
                    // The player is null if startup threw; this rethrows and
                    // the error is reported instead of dereferencing null.
                    ensurePlayer();
                    const auto device = state.liveDevice;
                    const bool active = state.liveActive;
                    const bool hadLive = live != nullptr;
                    // Closing joins callbacks before any lookup changes. Live
                    // note ownership is private, so release the outgoing map
                    // in full and replace its owner before building new caches.
                    if (live) { live->SetActive(false); live->CloseDevice(); }
                    stopPlayback();
                    if (live) { player->release_every_mapped_key(); live.reset(); }
                    state.liveActive = false;
                    if (layoutChange) state.eightyEightKeys = command.value;
                    else state.outRange = command.value;
                    state.keyMappings = std::move(mappings);
                    player->eightyEightKeyModeActive = state.eightyEightKeys;
                    applyMappings();
                    applyVelocityModifier();
                    ++state.mappingRevision;
                    invalidateSheet();
                    configJson[layoutChange ? "SHELL_88_KEYS" : "SHELL_OUT_RANGE"] = command.value;
                    touchConfig();
                    if (hadLive && !device.empty() && !state.midiConnect) {
                        live = std::make_unique<MIDI2Key>(player.get());
                        live->SetMidiChannel(state.liveChannel);
                        live->OpenDevice(device);
                        state.liveDevice = live->GetSelectedDevice();
                        state.liveActive = active && !state.liveDevice.empty();
                        live->SetActive(state.liveActive);
                        liveMappings = state.mappingRevision;
                        liveTranspose = state.transpose;
                        liveOctaves = state.octaves;
                        if (state.liveDevice.empty()) state.error = "Layout changed; MIDI input could not reopen.";
                    }
                    break;
                }
                case Action::AutoVolumeScan:
                    state.volumeWindows = volumeHost_->Windows();
                    break;
                case Action::AutoVolumeCalibrate: {
                    if (command.generation != state.generation) break;
                    const auto windows = volumeHost_->Windows();
                    const auto found = std::find_if(windows.begin(), windows.end(), [&](const auto& window) {
                        return window.id == command.window.id && window.process == command.window.process &&
                            window.title == command.window.title;
                    });
                    if (found == windows.end()) throw std::runtime_error("That game window changed or closed. Refresh the list and select it again.");
                    ensurePlayer();
                    const auto& volume = midi::Config::getInstance().volume;
                    if (volume.VOLUME_STEP <= 0 || volume.MIN_VOLUME < 0 || volume.MAX_VOLUME > 1000 ||
                        volume.MAX_VOLUME < volume.MIN_VOLUME || volume.INITIAL_VOLUME < volume.MIN_VOLUME ||
                        volume.INITIAL_VOLUME > volume.MAX_VOLUME)
                        throw std::runtime_error("Invalid AutoVol range or step in config.json.");
                    invalidateVolume();
                    const auto device = state.liveDevice;
                    const bool hadLive = live != nullptr;
                    if (live) { live->SetActive(false); live->CloseDevice(); }
                    stopPlayback();
                    if (live) { player->release_every_mapped_key(); live.reset(); }
                    state.liveActive = false;
                    if (hadLive && !device.empty() && !state.midiConnect) {
                        live = std::make_unique<MIDI2Key>(player.get());
                        live->SetMidiChannel(state.liveChannel);
                        live->OpenDevice(device);
                        state.liveDevice = live->GetSelectedDevice();
                        if (state.liveDevice.empty()) throw std::runtime_error("MIDI input could not reopen. Calibration was not started.");
                    }
                    state.volumeTarget = *found;
                    state.autoVolumeNeedsCalibration = true;
                    state.autoVolumeCountdown = 3;
                    volumePending = true;
                    volumeDue = std::chrono::steady_clock::now() + 3s;
                    break;
                }
                case Action::AutoVolumeOff:
                    invalidateVolume();
                    state.autoVolumeNeedsCalibration = false;
                    break;
                case Action::AutoVolumeCancel:
                    cancelVolume();
                    break;
                case Action::Velocity:
                    state.velocity = command.value;
                    if (player) player->enable_velocity_keypress = command.value;
                    break;
                case Action::VelocityModifier:
                    if (command.key != "alt" && command.key != "ctrl" && command.key != "shift")
                        throw std::runtime_error("Velocity modifier must be Alt, Ctrl or Shift.");
                    if (state.velocityModifier == command.key) break;
                    state.velocityModifier = command.key;
                    ensurePlayer();
                    applyVelocityModifier();
                    configJson["VELOCITY_MODIFIER"] = state.velocityModifier;
                    touchConfig();
                    break;
                case Action::PerformerAction:
                    if (!section.on || !state.playing || !player || command.track >= section.actions.size()) break;
                    player->perform_action(section.actions[command.track].id, section.actions[command.track].schedules);
                    break;
                case Action::Sustain:
                    // Change pedal mode only while stopped; its engine state is
                    // owned by dispatch while playing.
                    if (!state.playing) {
                        state.sustain = command.value;
                        if (player) player->currentSustainMode = command.value ? SustainMode::SPACE_DOWN : SustainMode::IG;
                        // A performer may play otherwise when the pedal is not heard.
                        applyPerformer();
                    }
                    break;
                }
                // Save session state only on explicit user commands, so a scan
                // that finds a device unplugged doesn't erase the saved one.
                switch (command.action) {
                case Action::Velocity: case Action::Sustain: case Action::LiveOpen: case Action::LiveActive:
                case Action::MidiConnect:
                case Action::LiveChannel: case Action::OutputTarget: case Action::OutputOpen:
                case Action::Speed:   // Transpose is per song and starts at 0, so it isn't saved
                    if (configLoaded) {
                        // Choosing a device cancels the pending restore; a device
                        // still pending restore is not overwritten.
                        if (command.action == Action::LiveOpen || command.action == Action::LiveActive ||
                            command.action == Action::MidiConnect) restoreInput.clear();
                        if (command.action == Action::OutputOpen) restoreOutput.clear();
                        auto& session = configJson["SHELL_SESSION"];
                        session["velocity"] = state.velocity;
                        session["sustain"] = state.sustain;
                        session["liveChannel"] = state.liveChannel;
                        session["speed"] = state.speed;
                        const auto groupOf = [](const std::vector<LiveDevice>& devices, const std::wstring& id) {
                            const auto found = std::find_if(devices.begin(), devices.end(), [&](const LiveDevice& device) { return device.id == id; });
                            return Utf8(std::filesystem::path(found == devices.end() ? std::wstring() : found->group));
                        };
                        if (restoreInput.empty()) {
                            session["liveDevice"] = Utf8(std::filesystem::path(state.liveDevice));
                            session["liveDeviceGroup"] = groupOf(state.devices, state.liveDevice);
                            session["liveActive"] = state.liveActive;
                            session["midiConnect"] = state.midiConnect;
                        }
                        if (restoreOutput.empty() && state.outputDevice.empty() && !pausedOutput.empty()) {
                            session["outputDevice"] = Utf8(std::filesystem::path(pausedOutput));
                            session["outputDeviceGroup"] = Utf8(std::filesystem::path(pausedOutputGroup));
                        } else if (restoreOutput.empty()) {
                            session["outputDevice"] = Utf8(std::filesystem::path(state.outputDevice));
                            session["outputDeviceGroup"] = groupOf(state.outputDevices, state.outputDevice);
                        }
                        // The target stays while its output waits, so it is saved either way.
                        session["outputMidi"] = state.outputMidi;
                        touchConfig();
                    }
                    break;
                default: break;
                }
                state.busy = false;
                // Live input caches full_key_mappings on SetActive; re-arm it
                // after a transpose, remap or Octaves change.
                if (live && state.liveActive &&
                    (state.mappingRevision != liveMappings || state.transpose != liveTranspose ||
                     state.octaves != liveOctaves)) {
                    liveMappings = state.mappingRevision;
                    liveTranspose = state.transpose;
                    liveOctaves = state.octaves;
                    live->SetActive(true);
                }
                if (reopening && command.action == Action::LiveOpen) quietInput.clear();
                if (reopening && command.action == Action::OutputOpen) quietOutput.clear();
                // It came back, so its failure is no longer news.
                if (quietReopen) state.error.clear();
                reopening = false;
            }
            if (volumePending && !stop.stop_requested()) {
                stateChanged = true;
                const auto now = std::chrono::steady_clock::now();
                if (!state.autoVolumeFocusing && now >= volumeDue) {
                    state.autoVolumeCountdown = 0;
                    if (!volumeHost_->Focus(state.volumeTarget))
                        throw std::runtime_error("Could not focus the selected game. AutoVol remains off; select the game and try again.");
                    state.autoVolumeFocusing = true;
                    volumeDue = now + 500ms;
                } else if (!state.autoVolumeFocusing) {
                    state.autoVolumeCountdown = std::max(1, static_cast<int>(std::ceil(
                        std::chrono::duration<double>(volumeDue - now).count())));
                }
                if (state.autoVolumeFocusing) {
                    if (volumeHost_->IsForeground(state.volumeTarget)) {
                        // This calibrates; calling calibrate_volume too would
                        // repeat the key sweep.
                        player->toggle_volume_adjustment();
                        state.autoVolume = true;
                        state.autoVolumeNeedsCalibration = false;
                        ++state.autoVolumeRevision;
                        cancelVolume();
                    } else if (now >= volumeDue) {
                        throw std::runtime_error("The selected game did not keep focus. AutoVol remains off.");
                    }
                }
            }
            if (state.playbackCountdown && !stop.stop_requested()) {
                const auto remaining = std::chrono::duration<double>(playbackDue - std::chrono::steady_clock::now()).count();
                if (remaining <= 0) { state.playbackCountdown = 0; startPlayback(); stateChanged = true; }
                else if (const int seconds = static_cast<int>(std::ceil(remaining)); seconds != state.playbackCountdown) {
                    state.playbackCountdown = seconds;
                    stateChanged = true;
                }
            }
            if (state.playing) {
                state.position = std::clamp(player->get_adjusted_time().count() / 1e9, 0.0, state.duration);
                // The playback thread reports the end itself. The take can add
                // keys to the score and is rebuilt while playing, so its size and
                // the index into it, read here, need not belong to the same take.
                if (player->playback_started.load(std::memory_order_acquire) &&
                    player->song_done.load(std::memory_order_acquire)) {
                    // A song that stopped on an error stays where it stopped and
                    // nothing plays next; the log has what went wrong.
                    const bool failed = player->song_failed.load(std::memory_order_acquire);
                    stateChanged = true;
                    stopPlayback();
                    if (failed) state.error = "Playback stopped after an error.";
                    else state.position = state.duration;
                    // Shuffle Play draws the next song, and a queued song or the next in a playlist or
                    // Favourites shown comes next without it; a song on Loop plays on, and its end is never reached.
                    if (!failed && state.loop == 0 &&((state.shuffle && !state.files->empty()) || !firstQueued().empty() || listPlaysOn())) {
                        shuffleAdvancePending = true;
                        Send({Action::Next, {}, state.generation, 0, loadAutoSolo, 1});
                    }
                }
            }
            if (rescanOwed && !state.playing && !state.playbackCountdown && !shuffleAdvancePending) {
                rescanOwed = false;
                Send({Action::Scan, state.folder, 0, 0, true});
            }
            if (state.outputMidi && !state.outputDevice.empty() && player &&
                player->opened_midi_output() != state.outputDevice) {
                loseOutput(outputGroup.Update(state.outputDevices, state.outputDevice));
                stateChanged = true;
            }
        } catch (const std::exception& error) {
            stateChanged = true;
            // A scan's reopen that fails leaves an armed calibration and a
            // playing song alone, and its device waits for the next scan, so a
            // later save does not erase it.
            if (volumePending && !reopening) invalidateVolume();
            bool quiet = false;
            if (reopening) {
                if (command.action == Action::LiveOpen) {
                    restoreInput = command.device; restoreInputConnect = command.value;
                    quiet = std::exchange(quietInput, command.device) == command.device;
                }
                if (command.action == Action::OutputOpen) {
                    restoreOutput = command.device; restoreOutputMidi = command.value;
                    quiet = std::exchange(quietOutput, command.device) == command.device;
                }
            }
            // A failed command that never drives the player leaves the song
            // playing. The curve actions stop and resume it themselves.
            const bool leavesPlayer = reopening || (hasCommand &&
                (command.action == Action::CopySheet || command.action == Action::OpenSheetEditor ||
                 command.action == Action::SaveSheetFiles || command.action == Action::SheetsFolder ||
                 command.action == Action::SheetStylePage || command.action == Action::SheetFiles || command.action == Action::KeepSheetStyle ||
                 command.action == Action::SaveLibrarySheets || command.action == Action::Hotkey ||
                 command.action == Action::VelocityModifier ||
                 command.action == Action::Favourite || command.action == Action::OpenList ||
                 command.action == Action::NewPlaylist || command.action == Action::RenamePlaylist ||
                 command.action == Action::DeletePlaylist || command.action == Action::AddToList ||
                 command.action == Action::RemoveFromList || command.action == Action::MoveInList ||
                 command.action == Action::DeleteSong || command.action == Action::SongRecycled ||
                 (command.action >= Action::CurveSelect && command.action <= Action::SustainCutoff)));
            if (!leavesPlayer) stopPlayback();
            if (!quiet) state.error = error.what();
            // A failed load leaves the previous file open, so name the file
            // that failed. Previous and Next load through the same path.
            if (hasCommand && (command.action == Action::Load || command.action == Action::Previous ||
                               command.action == Action::Next) && !command.path.empty())
                state.error = Utf8(command.path.filename()) + ": " + state.error;
            state.busy = false;
        }
        if ((configDirty || libraryDirty || songsDirty || lengthsDirty) && std::chrono::steady_clock::now() >= configDue) {
            try { flushConfig(); }
            catch (const std::exception& error) {
                stateChanged = true;
                state.error = error.what();
                // Back off 5 s so the wait doesn't spin on a past deadline.
                // A config that never loaded can never be saved.
                if (!configLoaded) configDirty = false;
                configDue = std::chrono::steady_clock::now() + 5s;
            }
        }
        syncPoller();
        // What waits to come back, shown greyed in the device pickers.
        if (const auto waiting = state.outputDevice.empty() ? (restoreOutput.empty() ? reopeningOutput : restoreOutput) : std::wstring();
            waiting != state.outputWaiting) {
            state.outputWaiting = waiting;
            state.outputWaitingName = Utf8(std::filesystem::path(WaitingName(waiting, restoreOutputGroup)));
            stateChanged = true;
        }
        if (const auto waiting = state.liveDevice.empty() ? (restoreInput.empty() ? reopeningInput : restoreInput) : std::wstring();
            waiting != state.liveWaiting) {
            state.liveWaiting = waiting;
            state.liveWaitingName = Utf8(std::filesystem::path(WaitingName(waiting, restoreInputGroup)));
            stateChanged = true;
        }
        if ((state.loaded != nextAfter || state.files != nextAmong || state.shuffle != nextShuffled ||
             state.library != nextLibrary || state.openList != nextList) && chooseNext()) stateChanged = true;
        if ((state.files != lengthsFiles || state.library != lengthsLibrary) && measureLengths()) stateChanged = true;
        if (stateChanged) Publish(state);
        else if (ticking) PublishPosition(state);
    }
    lengthsWork.request_stop();
    poller.request_stop();
    poller.join();
    stopPlayback();
    stopLive();
    stopConnect();
    // The YouTube window reads its stdin's end and closes.
    browser.CloseInput();
    // Flush pending edits. A failure can't reach the status bar at shutdown, so
    // the log keeps it; the atomic rename leaves the previous file intact.
    try { flushConfig(); }
    catch (const std::exception& error) { ShellLog::Instance().Append(std::string("[settings] at exit: ") + error.what() + "\n"); }
    // Gives the window up to two seconds to close on its own before ~Job ends what is left.
    for (const auto until = std::chrono::steady_clock::now() + 2s; browser.Running() && std::chrono::steady_clock::now() < until;)
        std::this_thread::sleep_for(20ms);
}
}
