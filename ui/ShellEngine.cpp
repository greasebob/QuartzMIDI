#include "ShellEngine.hpp"
#include "GameKeyMaps.hpp"
#include "../engine/AudioToMidi.hpp"
#include "PlaybackSystem.hpp"
#include "MIDI2Key.hpp"
#include "MidiOutput.hpp"
#include "WootingAnalog.hpp"
#include "../engine/AddonHost.hpp"
#include "AddonKey.hpp"
#include "Bundle.hpp"
#include "SavedFiles.hpp"
#include <shlobj.h>
#include <cfgmgr32.h>
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#include <fstream>
#include <cmath>
#include <deque>
#include <map>
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

// The bar holding `at` and the point `bars` bars after its start, in seconds,
// by the file's tempo map and time signatures (4/4 before the first). A file
// timed in SMPTE frames has no bars, and gets eight seconds from `at`.
std::pair<double, double> BarsFrom(const MidiFile& file, double at, int bars) {
    if (file.division == 0 || (file.division & 0x8000)) return {at, at + 8};
    std::vector<TempoChange> tempos = file.tempoChanges;
    std::stable_sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    if (tempos.empty() || tempos.front().tick > 0) tempos.insert(tempos.begin(), {0, 500000});
    const double division = file.division;
    const auto seconds = [&](double tick) {
        double total = 0;
        for (size_t i = 0; i < tempos.size() && tempos[i].tick < tick; ++i) {
            const double until = i + 1 < tempos.size() ? std::min<double>(tempos[i + 1].tick, tick) : tick;
            total += (until - tempos[i].tick) * tempos[i].microsecondsPerQuarter / 1e6 / division;
        }
        return total;
    };
    double tick = 0;
    for (size_t i = 0; i < tempos.size(); ++i) {
        const double perTick = tempos[i].microsecondsPerQuarter / 1e6 / division;
        const double from = seconds(tempos[i].tick);
        if (i + 1 == tempos.size() || seconds(tempos[i + 1].tick) > at) { tick = tempos[i].tick + (at - from) / perTick; break; }
    }
    double signatureTick = 0, numerator = 4, denominator = 4;
    for (const auto& signature : file.timeSignatures)
        if (signature.tick <= tick && signature.tick >= signatureTick && signature.numerator && signature.denominator) {
            signatureTick = signature.tick; numerator = signature.numerator; denominator = signature.denominator;
        }
    const double bar = division * 4 * numerator / denominator;
    const double start = signatureTick + std::floor((tick - signatureTick) / bar) * bar;
    return {seconds(start), seconds(start + bars * bar)};
}

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
// The MIDI files below root, unsorted, each named by its path relative to root.
// Stops at kMaxLibraryFiles. Null when root cannot be read.
std::shared_ptr<std::vector<MidiEntry>> WalkLibrary(const std::filesystem::path& root, std::stop_token stop, std::error_code& error) {
    auto files = std::make_shared<std::vector<MidiEntry>>();
    // Skip permission-denied folders. Directory symlinks are not
    // followed, so a junction to its own parent can't recurse forever.
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) return nullptr;
    const std::filesystem::recursive_directory_iterator end;
    // Bound the walk for pathological trees.
    constexpr int kMaxDepth = 32;
    while (it != end && files->size() < kMaxLibraryFiles) {
        if (stop.stop_requested()) break;
        const auto& entry = *it;
        std::error_code entryError;
        // The Trash's songs are not the library's.
        if (TrashFolderName(entry.path().filename()) && entry.is_directory(entryError)) it.disable_recursion_pending();
        else if (entry.is_regular_file(entryError) && !entryError && LibraryFile(entry.path())) {
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
    return files;
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
                // A song going into or out of the Trash is listed by the command that moved it.
                const bool trash = std::any_of(name.begin(), name.end(), [](const std::filesystem::path& part) { return TrashFolderName(part); });
                relevant = !trash && (LibraryFile(path) ||
                    ((record->Action == FILE_ACTION_ADDED || record->Action == FILE_ACTION_RENAMED_NEW_NAME) && std::filesystem::is_directory(path, ignored)) ||
                    ((record->Action == FILE_ACTION_REMOVED || record->Action == FILE_ACTION_RENAMED_OLD_NAME) && listed(path)));
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
nlohmann::json PathsToJson(const std::vector<std::filesystem::path>& paths) {
    auto list = nlohmann::json::array();
    for (const auto& path : paths) list.push_back(Utf8(path));
    return list;
}
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
// Whether the panel can show a list: the queue and the Trash only while they hold a song.
bool ListShown(const LibraryLists& lists, int list) {
    return list == kFolderList || list == kFavouritesList || (list == kQueueList && !lists.queue.empty()) ||
        (list == kTrashList && !lists.trash.empty()) || (list >= 0 && static_cast<size_t>(list) < lists.playlists.size());
}
std::vector<MovedSong> MovedFromJson(const nlohmann::json& json, const char* key, bool& damaged) {
    std::vector<MovedSong> songs;
    const auto found = json.find(key);
    if (found == json.end()) return songs;
    if (!found->is_array()) { damaged = true; return songs; }
    for (const auto& each : *found)
        if (const auto file = TextOf(each, "file"), origin = TextOf(each, "origin"); !file.empty() && !origin.empty())
            songs.push_back({PathFromText(file), PathFromText(origin)});
        else damaged = true;
    return songs;
}
nlohmann::json MovedToJson(const std::vector<MovedSong>& songs) {
    auto list = nlohmann::json::array();
    for (const auto& song : songs) list.push_back({{"file", Utf8(song.file)}, {"origin", Utf8(song.origin)}});
    return list;
}
// Sends files to the Recycle Bin in one operation, never deleting them outright:
// where the bin cannot take them, Windows asks first. On a thread of its own,
// since the shell wants a single-threaded apartment.
bool Recycle(const std::vector<std::filesystem::path>& files) {
    bool recycled = false;
    std::thread([&] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        // Each null-terminated, and the list double-null-terminated.
        std::wstring from;
        for (const auto& file : files) { from += file.native(); from.push_back(L'\0'); }
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
        recycled = SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
        if (SUCCEEDED(com)) CoUninitialize();
    }).join();
    return recycled;
}

// A scan's line back to the engine. The engine lets go of it when the scan is
// stopped or the engine closes, so a scan still waiting on a slow drive then
// reports to no one, and nothing waits for it.
struct ScanLine {
    std::mutex mutex;
    ShellEngine* engine = nullptr;
    std::atomic<bool> stop{false};
    void Report(ShellEngine::Command command) {
        std::lock_guard lock(mutex);
        if (engine) engine->Send(std::move(command));
    }
    void Close() {
        stop = true;
        std::lock_guard lock(mutex);
        engine = nullptr;
    }
};
// What the walkers of one scan share: the files found, by content so a copy is
// found once, and the folder last read.
struct ScanShared {
    std::mutex mutex;
    std::map<std::pair<uintmax_t, uint64_t>, std::vector<std::filesystem::path>> seen;
    std::vector<MidiEntry> found;
    std::wstring folder;
};
// No MIDI file is this large; nor are the files a scan should read whole.
constexpr uintmax_t kLargestScanned = 32ull << 20;
bool ReadWhole(const std::filesystem::path& path, std::string& bytes) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return !stream.bad();
}
// A Standard MIDI File starts with its header chunk.
bool MidiHeader(const std::string& bytes) { return bytes.size() >= 14 && bytes.compare(0, 4, "MThd") == 0; }
// True the first time these bytes are seen: FNV-1a with the size, and the
// bytes compared on a match.
bool FirstCopy(ScanShared& shared, const std::filesystem::path& path, const std::string& bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : bytes) { hash ^= c; hash *= 1099511628211ull; }
    std::lock_guard lock(shared.mutex);
    auto& same = shared.seen[{bytes.size(), hash}];
    for (const auto& other : same) {
        std::string theirs;
        if (ReadWhole(other, theirs) && theirs == bytes) return false;
    }
    same.push_back(path);
    return true;
}
std::wstring Lowercase(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}
// Folders a scan passes over: Windows, the programs', the app's own and the
// library. Lowercase, each ending in a separator.
std::vector<std::wstring> ScanSkips(std::vector<std::filesystem::path> own) {
    for (const auto& id : {FOLDERID_Windows, FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_ProgramData}) {
        PWSTR text = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &text))) own.emplace_back(text);
        CoTaskMemFree(text);
    }
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    own.push_back(std::filesystem::path(executable).parent_path());
    std::vector<std::wstring> skips;
    for (const auto& folder : own) {
        if (folder.empty()) continue;
        auto text = Lowercase(folder.lexically_normal().native());
        if (text.back() != L'\\') text += L'\\';
        skips.push_back(std::move(text));
    }
    return skips;
}
// Folders a scan passes over wherever they are, by name, as Velo's scan does:
// system, program data, caches and the folders of packages and version control,
// where MIDI files belong to a game, a tool or a package, not to the user.
bool SkippedByName(std::wstring_view name) {
    static constexpr std::wstring_view names[]{
        L"windows", L"winsxs", L"system32", L"syswow64", L"$recycle.bin", L"recovery", L"system volume information",
        L"program files", L"program files (x86)", L"programdata", L"appdata", L"$windows.~bt", L"$windows.~ws",
        L"node_modules", L"__pycache__", L"site-packages", L"venv", L".venv", L".git", L".svn", L".hg",
        L".cache", L".npm", L".gradle", L".nuget", L".cargo", L".rustup", L"cache", L"caches", L"temp", L"tmp"};
    const auto lowered = Lowercase(std::wstring(name));
    return std::find(std::begin(names), std::end(names), lowered) != std::end(names);
}
// Walks one place for MIDI files, depth first, reading only what it has to.
// Links and junctions are not followed, and a file kept in the cloud is not
// fetched to be read.
void ScanPlace(const std::filesystem::path& root, const std::vector<std::wstring>& skips, ScanShared& shared, const std::atomic<bool>& stop) {
    constexpr int kMaxDepth = 40;
    std::vector<std::pair<std::wstring, int>> folders{{root.native(), 0}};
    while (!folders.empty() && !stop) {
        auto [folder, depth] = std::move(folders.back());
        folders.pop_back();
        if (folder.back() != L'\\') folder += L'\\';
        { std::lock_guard lock(shared.mutex); shared.folder = folder; }
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileExW((folder + L'*').c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring_view name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const auto path = folder + data.cFileName;
            const DWORD attributes = data.dwFileAttributes;
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                // $Recycle.Bin, System Volume Information and their kind.
                if ((attributes & FILE_ATTRIBUTE_HIDDEN) && (attributes & FILE_ATTRIBUTE_SYSTEM)) continue;
                // The app's Trash, in this MIDI folder or one used before.
                if (TrashFolderName(std::filesystem::path(name))) continue;
                if (SkippedByName(name)) continue;
                const auto lowered = Lowercase(path) + L'\\';
                if (std::any_of(skips.begin(), skips.end(), [&](const std::wstring& skip) { return lowered.starts_with(skip); })) continue;
                if (depth < kMaxDepth) folders.push_back({path, depth + 1});
                continue;
            }
            if (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) continue;
            if (!LibraryFile(std::filesystem::path(name))) continue;
            const uintmax_t size = static_cast<uintmax_t>(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
            if (size < 14 || size > kLargestScanned) continue;
            std::string bytes;
            if (!ReadWhole(path, bytes) || !MidiHeader(bytes) || !FirstCopy(shared, path, bytes)) continue;
            const std::filesystem::path found(path);
            std::lock_guard lock(shared.mutex);
            shared.found.push_back({found, Utf8(found.parent_path()), size});
        } while (!stop && FindNextFileW(find, &data));
        FindClose(find);
    }
}
// One scan: the library's own files first, so a copy of one is not new, then
// every place at once, each on its own thread so a slow drive holds up only
// itself. Reports every 150 ms, and once more with what it found.
void RunScan(std::shared_ptr<ScanLine> line, uint64_t id, std::vector<std::filesystem::path> places, std::vector<std::wstring> skips,
             std::shared_ptr<const std::vector<MidiEntry>> library) {
    using Action = ShellEngine::Action;
    ScanShared shared;
    for (const auto& file : *library) {
        if (line->stop) return;
        std::string bytes;
        if (file.bytes <= kLargestScanned && ReadWhole(file.path, bytes) && MidiHeader(bytes)) FirstCopy(shared, file.path, bytes);
    }
    std::atomic<size_t> walking = places.size();
    std::vector<std::thread> walkers;
    for (const auto& place : places)
        walkers.emplace_back([&, place] { ScanPlace(place, skips, shared, line->stop); --walking; });
    const auto report = [&](bool finished) {
        ShellEngine::Command progress{Action::ScanProgress, {}, id, 0, finished};
        std::lock_guard lock(shared.mutex);
        progress.track = shared.found.size();
        progress.key = Utf8(std::filesystem::path(shared.folder));
        if (finished) progress.files = std::make_shared<std::vector<MidiEntry>>(shared.found);
        return progress;
    };
    while (walking && !line->stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        line->Report(report(false));
    }
    for (auto& walker : walkers) walker.join();
    if (line->stop) return;
    auto finished = report(true);
    // By folder, then by name, so each folder's files are together.
    std::sort(finished.files->begin(), finished.files->end(), [](const MidiEntry& a, const MidiEntry& b) {
        const auto key = [](const MidiEntry& file) { return std::pair{Lowercase(file.path.parent_path().native()), Lowercase(file.path.filename().native())}; };
        const auto first = key(a), second = key(b);
        return first != second ? first < second : a.path < b.path;
    });
    line->Report(std::move(finished));
}
void ReadLibrary(const nlohmann::json& json, LibraryLists& lists, int& openList, bool& damaged) {
    lists.favourites = PathsFromJson(json, "favourites", damaged);
    lists.queue = PathsFromJson(json, "queue", damaged);
    // A song emptied from the Trash folder by hand is gone.
    lists.trash = MovedFromJson(json, "trash", damaged);
    std::erase_if(lists.trash, [](const MovedSong& song) { std::error_code gone; return !std::filesystem::is_regular_file(song.file, gone); });
    lists.scanned = MovedFromJson(json, "scanned", damaged);
    if (const auto playlists = json.find("playlists"); playlists != json.end() && !playlists->is_array()) damaged = true;
    else if (playlists != json.end())
        for (const auto& each : *playlists)
            if (const auto name = TextOf(each, "name"); !name.empty()) lists.playlists.push_back({name, PathsFromJson(each, "songs", damaged)});
            else damaged = true;
    const auto open = json.find("open");
    if (open != json.end() && !open->is_number_integer()) damaged = true;
    const int shown = open != json.end() && open->is_number_integer() ? open->get<int>() : kFolderList;
    openList = ListShown(lists, shown) ? shown : kFolderList;
}
nlohmann::json WriteLibrary(const LibraryLists& lists, int openList) {
    auto playlists = nlohmann::json::array();
    for (const auto& playlist : lists.playlists) playlists.push_back({{"name", playlist.name}, {"songs", PathsToJson(playlist.songs)}});
    return {{"favourites", PathsToJson(lists.favourites)}, {"playlists", std::move(playlists)},
            {"queue", PathsToJson(lists.queue)}, {"trash", MovedToJson(lists.trash)}, {"scanned", MovedToJson(lists.scanned)},
            {"open", openList}};
}

std::mutex addonsMutex;
std::filesystem::path addonsFolder;
addon::PublicKey addonsKey = kOwnerKey;
} // namespace

std::vector<std::filesystem::path> UserScanFolders() {
    std::vector<std::filesystem::path> folders;
    for (const auto& id : {FOLDERID_Downloads, FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Music, FOLDERID_SkyDrive}) {
        PWSTR text = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &text))) {
            const std::filesystem::path folder = std::filesystem::path(text).lexically_normal();
            std::error_code error;
            if (std::filesystem::is_directory(folder, error) &&
                std::none_of(folders.begin(), folders.end(), [&](const std::filesystem::path& other) { return Lowercase(other.native()) == Lowercase(folder.native()); }))
                folders.push_back(folder);
        }
        CoTaskMemFree(text);
    }
    return folders;
}

void ShellEngine::SetAddons(std::filesystem::path folder, const std::array<std::uint8_t, 72>& key) {
    static_assert(std::tuple_size_v<addon::PublicKey> == 72);
    std::lock_guard lock(addonsMutex);
    addonsFolder = std::move(folder);
    addonsKey = key;
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
    { std::lock_guard lock(mutex_); snapshot_ = std::move(copy); }
    // Wake the on-demand render loop.
    if (void* window = wakeWindow_.load(std::memory_order_acquire)) PostMessageW(static_cast<HWND>(window), WM_NULL, 0, 0);
}

void ShellEngine::SetWakeWindow(void* window) {
    wakeWindow_.store(window, std::memory_order_release);
}

void ShellEngine::SetKeyProbe(std::function<bool(int)> probe) {
    std::lock_guard lock(keyProbeMutex_);
    keyProbe_ = std::move(probe);
}

// Parses a performer's `describe` JSON (engine/Performer.hpp). Throws when the
// name or config key is missing or a control id is missing or duplicated.
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
            section.presets.push_back(choice.at("name").get<std::string>());
            section.presetValues.push_back(choice.value("values", std::map<std::string, double>{}));
        }
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
    if (!section.presetValues.empty())
        for (const auto& [id, value] : section.presetValues.front())
            if (auto* control = section.Find(id); control && !control->isSwitch && !control->isChoice) control->value = std::clamp(value, 0.0, 1.0);
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
    // Set the sliders named by the current preset.
    const auto applyPreset = [&] {
        if (section.presetValues.empty()) return;
        for (const auto& [id, value] : section.presetValues[static_cast<size_t>(section.preset)])
            if (auto* control = section.Find(id); control && !control->isSwitch && !control->isChoice) control->value = std::clamp(value, 0.0, 1.0);
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
    uint64_t liveMappings = 0;
    int liveTranspose = 0;
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
    try {
        std::ifstream stream(config_);
        configJson = nlohmann::json::parse(stream);
        if (!configJson.is_object()) throw std::runtime_error("config.json is not a JSON object.");
        configLoaded = true;
        state.eightyEightKeys = configJson.value("SHELL_88_KEYS", true);
        // Read mappings before any field a hand-edited file might break;
        // without them the player's mappings are cleared and no note types.
        state.keyMappings = configJson.at("KEY_MAPPINGS").at(state.eightyEightKeys ? "FULL" : "LIMITED").get<decltype(state.keyMappings)>();
        state.outRange = configJson.value("SHELL_OUT_RANGE", false);
        state.playbackDelay = std::clamp(configJson.value("SHELL_PLAYBACK_DELAY", 3), 0, 10);
        state.seekStep = std::clamp(configJson.value("SHELL_SEEK_STEP", 10), 1, 60);
        // The player accepts speeds from 0.05 to 8.
        state.speedMin = std::clamp(configJson.value("SHELL_SPEED_MIN", .25), .05, 1.0);
        state.speedMax = std::clamp(configJson.value("SHELL_SPEED_MAX", 2.0), 1.0, 8.0);
        state.shuffle = configJson.value("SHELL_SHUFFLE", false);
        state.loop = std::clamp(configJson.value("SHELL_LOOP", 0), 0, 2);
        state.holdBehind = configJson.value("SHELL_HOLD_BEHIND", true);
        if (const auto keys = configJson.find("HOTKEY_SETTINGS"); keys != configJson.end() && keys->is_object())
            for (size_t i = 0; i < kHotkeys; ++i) {
                if (hotkeyFields[i].empty()) continue;
                state.hotkeys[i] = keys->value(hotkeyFields[i], hotkeyDefaults[i]);
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
                        NameToVK(keys->value(hotkeyFields[j], std::string())) == NameToVK(state.hotkeys[i]))
                        state.hotkeys[i].clear();
            }
        // A song's key the app types, or one another action or song holds, is dropped.
        if (const auto songs = configJson.find("SHELL_SONG_HOTKEYS"); songs != configJson.end() && songs->is_array())
            for (const auto& each : *songs) {
                if (!each.is_object()) continue;
                SongHotkey bound{PathFromJson(each, "song"), each.value("key", std::string())};
                const int vk = NameToVK(bound.key);
                const auto holds = [&](const std::string& key) { return NameToVK(key) == vk; };
                if (bound.song.empty() || vk == 0 || IsNoteKey(vk, state.keyMappings) ||
                    std::any_of(state.hotkeys.begin(), state.hotkeys.end(), holds) ||
                    std::any_of(state.songHotkeys.begin(), state.songHotkeys.end(), [&](const SongHotkey& other) { return holds(other.key) || other.song == bound.song; }))
                    continue;
                state.songHotkeys.push_back(std::move(bound));
            }
        state.sheetsFolder = PathFromJson(configJson, "SHELL_SHEETS_FOLDER");
        state.sheetStylePage = PathFromJson(configJson, "SHELL_SHEET_STYLE_PAGE");
        if (configJson.contains("SHELL_SHEET_FILES") && configJson["SHELL_SHEET_FILES"].is_object()) {
            const auto& files = configJson["SHELL_SHEET_FILES"];
            state.sheetImage = files.value("image", true);
            state.sheetTextFile = files.value("text", true);
            state.sheetPageFile = files.value("page", true);
        }
        if (configJson.contains("MIDI_SETTINGS")) state.detectDrums = configJson["MIDI_SETTINGS"].value("DETECT_DRUMS", true);
        if (configJson.contains("AUTO_TRANSPOSE")) state.autoTranspose = configJson["AUTO_TRANSPOSE"].value("ENABLED", false);
        state.fitToKeys = configJson.value("SHELL_FIT_TO_KEYS", false);
        state.octaves = std::clamp(configJson.value("SHELL_OCTAVES", 1), 1, 5);
        state.fileSort = static_cast<FileSort>(std::clamp(configJson.value("SHELL_FILE_SORT", 0), 0, 2));
        state.descendingFiles = configJson.value("SHELL_FILE_DESCENDING", false);
        // Upstream's TIMING_VARIATION, NOTE_SKIP_CHANCE and EXTRA_DELAY keys are
        // ignored and left in the file.
        // Performer values live under section.config. Without the add-on they
        // are left untouched in the file.
        if (section.loaded) {
            const auto found = configJson.find(section.config);
            const auto saved = found != configJson.end() && found->is_object() ? *found : nlohmann::json::object();
            section.on = saved.value("ENABLED", false);
            if (!section.presets.empty())
                section.preset = std::clamp(saved.value(section.presetField, 0), 0, static_cast<int>(section.presets.size()) - 1);
            applyPreset();
            for (auto& control : section.controls) {
                const auto value = saved.find(control.field);
                if (value == saved.end()) continue;
                if (control.isSwitch) { if (value->is_boolean()) control.value = value->get<bool>() ? 1 : 0; }
                else if (control.isChoice) { if (value->is_number_integer()) control.value = std::clamp(value->get<int>(), 0, static_cast<int>(control.choices.size()) - 1); }
                else if (value->is_number()) control.value = std::clamp(value->get<double>(), control.estimateName.empty() ? 0.0 : -1.0, 1.0);
            }
            // A slider whose estimate is disabled can't follow it.
            for (auto& control : section.controls)
                if (!control.isSwitch && control.value < 0 && !section.Estimated(control)) control.value = 0;
        }
        for (const auto& control : section.controls) if (control.remembers) state.rememberPerSong = control.value != 0;
        // Triggers belong to the performer; with it off the trigger is the clock.
        state.trigger = section.on ? std::clamp(configJson.value("SHELL_TRIGGER", 0), 0, std::max(0, static_cast<int>(section.triggers.size()) - 1)) : 0;
        // Session state from the last run. Devices are reopened later, once a
        // player exists.
        if (const auto session = configJson.find("SHELL_SESSION"); session != configJson.end() && session->is_object()) {
            state.velocity = session->value("velocity", true);
            state.sustain = session->value("sustain", true);
            state.liveChannel = std::clamp(session->value("liveChannel", -1), -1, 15);
            state.speed = std::clamp(session->value("speed", 1.0), state.speedMin, state.speedMax);
        }
        state.velocityModifier = configJson.value("VELOCITY_MODIFIER", std::string("alt"));
        if (state.velocityModifier != "alt" && state.velocityModifier != "ctrl" && state.velocityModifier != "shift") {
            state.velocityModifier = "alt";
            throw std::runtime_error("VELOCITY_MODIFIER must be alt, ctrl or shift.");
        }
    } catch (const std::exception& error) { state.error = error.what(); }
    const auto touchConfig = [&] {
        configDirty = true;
        configDue = std::chrono::steady_clock::now() + configSettle;
    };
    const auto saveSongHotkeys = [&] {
        auto& saved = configJson["SHELL_SONG_HOTKEYS"] = nlohmann::json::array();
        for (const auto& bound : state.songHotkeys) saved.push_back({{"song", Utf8(bound.song)}, {"key", bound.key}});
        touchConfig();
    };
    // Per-song settings keyed by file name. Kept out of config.json because it
    // grows with the library and config.json is hand-edited.
    const auto songsPath = config_.parent_path() / "songs.json";
    nlohmann::json songsJson = nlohmann::json::object();
    bool songsDirty = false;
    // A saved file read only in part is kept aside as .damaged and saved again
    // with what it held, and the status bar says so.
    const auto damagedFile = [&](const std::filesystem::path& path, const std::string& notice) {
        const auto aside = SetAside(path);
        ShellLog::Instance().Append("[settings] " + Utf8(path.filename()) + " could not be read whole" +
                                    (aside.empty() ? std::string() : "; kept as " + Utf8(aside.filename())) + ".\n");
        state.resetNotice += (state.resetNotice.empty() ? "" : " ") + notice;
        configDue = std::chrono::steady_clock::now();
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
        if (damaged) { songsDirty = true; damagedFile(songsPath, "Some song settings could not be read."); }
    }
    // Favourites, playlists, the queue and the Trash by path, in library.json
    // for the same reason. Trashed songs wait in the MIDI folder's Trash, a
    // hidden folder the user finds beside the songs and that goes with them.
    const auto libraryPath = config_.parent_path() / "library.json";
    const auto trashFolder = [&] { return state.folder.empty() ? std::filesystem::path() : state.folder / kTrashFolder; };
    // Made when a song first goes in.
    const auto makeTrash = [&] {
        const auto folder = trashFolder();
        std::error_code error;
        if (!std::filesystem::create_directories(folder, error) || error) return folder;
        if (const DWORD attributes = GetFileAttributesW(folder.c_str()); attributes != INVALID_FILE_ATTRIBUTES)
            SetFileAttributesW(folder.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN);
        return folder;
    };
    // Where the Trash was kept before, beside the config, lost with the app's folder.
    const auto oldTrash = config_.parent_path() / "Trash";
    bool libraryDirty = false;
    {
        bool damaged = false;
        const auto saved = ReadSaved(libraryPath, damaged);
        if (saved.is_object()) {
            auto lists = std::make_shared<LibraryLists>();
            ReadLibrary(saved, *lists, state.openList, damaged);
            state.library = std::move(lists);
        }
        if (damaged) { libraryDirty = true; damagedFile(libraryPath, "Some favourites and playlists could not be read."); }
    }
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
    // A song in the Trash folder that no record names, since library.json lost
    // it, is listed again. Where it came from was lost with the record, so
    // Restore puts it in the MIDI folder.
    const auto recoverTrash = [&] {
        std::vector<MovedSong> found;
        std::error_code error;
        for (std::filesystem::directory_iterator each(trashFolder(), error), end; !error && each != end; each.increment(error)) {
            const auto& file = each->path();
            std::error_code kind;
            if (!each->is_regular_file(kind) || !LibraryFile(file)) continue;
            if (std::none_of(state.library->trash.begin(), state.library->trash.end(), [&](const MovedSong& song) {
                    std::error_code same;
                    return std::filesystem::equivalent(song.file, file, same);
                }))
                found.push_back({file, state.folder / file.filename()});
        }
        if (!found.empty()) editLibrary([&](LibraryLists& lists) { lists.trash.insert(lists.trash.end(), found.begin(), found.end()); });
    };
    // The Trash kept beside the config before moves into the MIDI folder's, each
    // record following its song; the old folder goes once it is empty.
    const auto moveOldTrash = [&] {
        std::error_code error;
        if (state.folder.empty() || !std::filesystem::is_directory(oldTrash, error)) return;
        std::vector<std::filesystem::path> songs;
        for (std::filesystem::directory_iterator each(oldTrash, error), end; !error && each != end; each.increment(error))
            if (std::error_code kind; each->is_regular_file(kind)) songs.push_back(each->path());
        editLibrary([&](LibraryLists& lists) {
            for (const auto& from : songs) {
                const auto record = std::find_if(lists.trash.begin(), lists.trash.end(), [&](const MovedSong& song) {
                    std::error_code same;
                    return std::filesystem::equivalent(song.file, from, same);
                });
                const auto target = FreeName(makeTrash() / from.filename());
                if (!MoveFileExW(from.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED)) continue;
                if (record != lists.trash.end()) record->file = target;
            }
        });
        std::filesystem::remove(oldTrash, error);
    };
    // Runs at the settle deadline, before anything rereads config.json, and on
    // shutdown. Each file reaches the disk before it replaces the old one
    // (WriteDurably), so a power cut leaves the one or the other whole; the
    // debounce keeps that wait to one per burst of edits.
    const auto flushConfig = [&] {
        if (songsDirty) {
            if (!WriteDurably(songsPath, songsJson.dump(1) + '\n')) throw std::runtime_error("Cannot save the song settings.");
            songsDirty = false;
        }
        if (libraryDirty) {
            if (!WriteDurably(libraryPath, WriteLibrary(*state.library, state.openList).dump(1) + '\n'))
                throw std::runtime_error("Cannot save the favourites and playlists.");
            libraryDirty = false;
        }
        if (!configDirty) return;
        // Never overwrite a config that failed to parse.
        if (!configLoaded) throw std::runtime_error("The configuration was not loaded, so it cannot be saved.");
        if (!WriteDurably(config_, configJson.dump(4) + '\n')) throw std::runtime_error("Cannot save the configuration.");
        configDirty = false;
    };
    Publish(state);
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
        if (!section.presetId.empty()) settings[section.presetId] = section.preset;
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
    const auto songKey = [&] { return Utf8(state.loaded.filename()); };
    // Load the open song's saved settings when per-song memory is on.
    const auto recallSong = [&] {
        // Reset estimate-driven sliders. One whose estimate is disabled holds a
        // global value that no song overrides.
        for (auto& control : section.controls)
            if (!control.isSwitch && !control.isChoice && !control.estimateName.empty() && section.Estimated(control)) control.value = -1;
        if (!state.rememberPerSong || state.loaded.empty()) return;
        const auto found = songsJson.find(songKey());
        if (found == songsJson.end() || !found->is_object()) return;
        if (section.loaded) {
            if (!section.presets.empty())
                section.preset = std::clamp(found->value(section.presetField, section.preset), 0, static_cast<int>(section.presets.size()) - 1);
            for (auto& control : section.controls) {
                if (control.isSwitch || control.trigger >= 0) continue;
                if (control.isChoice) control.value = std::clamp(found->value(control.field, static_cast<int>(control.value)), 0, static_cast<int>(control.choices.size()) - 1);
                else if (control.estimateName.empty()) control.value = std::clamp(found->value(control.field, control.value), 0.0, 1.0);
                else if (section.Estimated(control)) control.value = std::clamp(found->value(control.field, -1.0), -1.0, 1.0);
            }
        }
    };
    // Save to config.json as the default for unseen songs and, with per-song
    // memory on, to songs.json for the open song.
    const auto rememberSettings = [&] {
        const bool perSong = state.rememberPerSong && !state.loaded.empty();
        configJson["SHELL_TRIGGER"] = state.trigger;
        if (section.loaded) {
            auto& saved = configJson[section.config];
            if (!section.presets.empty()) saved[section.presetField] = section.preset;
            for (const auto& control : section.controls) {
                if (control.isSwitch) saved[control.field] = control.value != 0;
                else if (control.isChoice) saved[control.field] = static_cast<int>(control.value);
                else if (control.estimateName.empty()) saved[control.field] = control.value;
                // An estimate-driven slider is per song; it is global only
                // while its estimate is disabled.
                else if (!section.Estimated(control)) saved[control.field] = control.value;
                else if (!perSong) saved[control.field] = -1.0;
            }
        }
        // Only performer settings are stored per song.
        if (perSong && section.loaded) {
            auto& song = songsJson[songKey()];
            if (!song.is_object()) song = nlohmann::json::object();
            {
                if (!section.presets.empty()) song[section.presetField] = section.preset;
                for (const auto& control : section.controls) {
                    if (control.isSwitch || control.trigger >= 0) continue;
                    // With its estimate disabled the slider is global; keep the
                    // song's stored value for when the estimate is re-enabled.
                    if (control.isChoice) song[control.field] = static_cast<int>(control.value);
                    else if (control.estimateName.empty() || section.Estimated(control)) song[control.field] = control.value;
                    else if (!song.contains(control.field)) song[control.field] = -1.0;
                }
            }
            songsDirty = true;
        }
        touchConfig();
        applyPerformer();
    };
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
            flushConfig();
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
    const auto readConverter = [&] {
        const auto install = converterInstall();
        state.youtubeSignedIn = install.SignedIn();
        state.converterInstalled = install.Found();
        state.converterCanSetUp = install.CanSetUp();
        state.converterGpu = install.gpu;
        state.converterCanSwitch = install.CanSwitch();
        state.converterGpuUnsupported = install.gpuUnsupported;
        state.nvidiaCard = audio_to_midi::HasNvidiaCard();
    };
    readConverter();
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
    // The mini window's busy strip counts the notes of the tracks heard only.
    const auto countDensity = [&] {
        if (!player) return;
        const bool anySolo = AnySolo(state.rows);
        std::vector<double> strikes;
        for (const auto& event : player->note_events) {
            if (event.action != EventType::Press || PedalForName(event.note_or_control) >= 0) continue;
            const auto row = std::find_if(state.rows.begin(), state.rows.end(),
                [&](const TrackRow& candidate) { return candidate.index == static_cast<size_t>(event.trackIndex); });
            if (row != state.rows.end() && TrackAudible(*row, anySolo)) strikes.push_back(static_cast<double>(event.time.count()) / 1e9);
        }
        auto density = NoteDensity(strikes, state.duration);
        if (!state.density || *state.density != density) state.density = std::make_shared<const std::vector<uint16_t>>(std::move(density));
    };
    const auto applyTracks = [&] {
        if (!player) return;
        for (const auto& row : state.rows) {
            player->set_track_mute(row.index, row.muted);
            player->set_track_solo(row.index, row.solo);
        }
        countDensity();
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
    // The scan for MIDI files running, if any, and its number; let go when Run returns.
    std::shared_ptr<ScanLine> scanLine;
    uint64_t scanId = 0;
    // Watches the open folder on its own thread and walks it again when its
    // MIDI files change, so the list follows the disk even during playback.
    // Restarted by each Scan that is not quiet; stopped and joined when Run returns.
    std::jthread libraryWatch;
    const auto watchLibrary = [&](const std::filesystem::path& root) {
        libraryWatch = std::jthread([this, root](std::stop_token token) {
            const auto listed = [this](const std::filesystem::path& gone) {
                const auto files = Snapshot()->files;
                const auto& prefix = gone.native();
                return std::any_of(files->begin(), files->end(), [&](const MidiEntry& file) {
                    const auto& path = file.path.native();
                    return path.size() > prefix.size() && (path[prefix.size()] == L'\\' || path[prefix.size()] == L'/') &&
                        _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
                });
            };
            WatchLibrary(root, token, listed, [&] {
                std::error_code error;
                auto files = WalkLibrary(root, token, error);
                if (!files || token.stop_requested()) return;
                Command found{Action::Scan, root, 0, 0, true};
                found.files = std::move(files);
                Send(std::move(found));
            });
        });
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
    const auto listHeld = [&](const std::vector<MidiEntry>& files) {
        if (state.openList != kFavouritesList && state.openList < 0) return false;
        const auto* listed = state.library->Songs(state.openList);
        return state.openList == kFavouritesList ? std::any_of(files.begin(), files.end(), [&](const MidiEntry& file) {
            return state.library->Favourite(file.path); }) : listed && !listed->empty();
    };
    const auto listEnds = [&](const std::vector<MidiEntry>& files) {
        return !state.shuffle && listHeld(files) && !files.empty() && files.back().path == state.loaded;
    };
    const auto listPlaysOn = [&] {
        const auto files = SongsFollowed(*state.files, state.loaded, *state.library, state.openList);
        return listHeld(files) && !listEnds(files) &&
               std::any_of(files.begin(), files.end(), [&](const MidiEntry& file) { return file.path != state.loaded; });
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
        // idle window stops drawing.
        bool stateChanged = hasCommand || ticking;
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
                const bool fromConverter = command.action == Action::ConvertProgress || command.action == Action::SheetBatchProgress ||
                    command.action == Action::ScanProgress;
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
                    configJson[section.config]["ENABLED"] = command.value;
                    // Turning the performer off resets the trigger to the clock,
                    // stopping playback if a Hold trigger was running it.
                    if (!command.value && state.trigger != 0) {
                        if (state.TriggerPlays() && state.playing) stopPlayback();
                        state.trigger = 0;
                        rememberSettings();
                    }
                    applyPerformer();
                    touchConfig();
                    break;
                case Action::PerformerPreset:
                    if (section.presets.empty()) break;
                    section.preset = static_cast<int>(std::min(command.track, section.presets.size() - 1));
                    applyPreset();
                    rememberSettings();
                    break;
                case Action::PerformerValue: {
                    auto* control = section.Find(command.key);
                    if (!control || !std::isfinite(command.amount)) break;
                    if (control->isChoice) {
                        control->value = std::clamp(static_cast<int>(std::lround(command.amount)), 0, static_cast<int>(control->choices.size()) - 1);
                        rememberSettings();
                        break;
                    }
                    if (!control->isSwitch) {
                        // Negative returns the slider to its estimate, if it has one.
                        control->value = command.amount < 0 ? (section.Estimated(*control) ? -1 : 0) : std::min(command.amount, 1.0);
                        rememberSettings();
                        break;
                    }
                    const double wanted = command.amount != 0 ? 1 : 0;
                    if (control->value == wanted) break;
                    control->value = wanted;
                    // Disabling the estimate zeroes its slider; re-enabling returns it
                    // to the estimate.
                    if (auto* slider = section.Find(control->estimates); slider && !control->estimates.empty()) slider->value = wanted != 0 ? -1 : 0;
                    if (control->remembers) {
                        state.rememberPerSong = wanted != 0;
                        if (wanted != 0) recallSong();
                    }
                    rememberSettings();
                    break;
                }
                case Action::Trigger: {
                    if (!section.on || section.triggers.empty()) break;
                    const int wanted = static_cast<int>(std::min(command.track, section.triggers.size() - 1));
                    // A Hold trigger owns play/pause; stop when switching away from it.
                    if (state.TriggerPlays() && wanted != state.trigger && state.playing) stopPlayback();
                    state.trigger = wanted;
                    rememberSettings();
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
                        const auto found = std::find(songs.begin(), songs.end(), command.path);
                        if (command.value && found == songs.end()) songs.push_back(command.path);
                        if (!command.value && found != songs.end()) songs.erase(found);
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
                case Action::Trash: {
                    std::error_code error;
                    if (!std::filesystem::is_regular_file(command.path, error)) throw std::runtime_error(Utf8(command.path.filename()) + ": Unable to find the file.");
                    // A playlist's song from outside the MIDI folder goes to its Trash
                    // too, and Restore returns it to where it was.
                    if (state.folder.empty()) throw std::runtime_error("Choose a MIDI folder before moving a song to the Trash.");
                    const auto target = FreeName(makeTrash() / command.path.filename());
                    if (!MoveFileExW(command.path.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED))
                        throw std::runtime_error("Cannot move " + Utf8(command.path.filename()) + " to the Trash.");
                    // Out of the list at once; the folder watcher's walk agrees.
                    auto kept = std::make_shared<std::vector<MidiEntry>>();
                    for (const auto& file : *state.files) if (file.path != command.path) kept->push_back(file);
                    if (kept->size() != state.files->size()) state.files = std::move(kept);
                    // Its stars and playlists wait for a Restore, which puts it back where it was.
                    editLibrary([&](LibraryLists& lists) {
                        lists.trash.push_back({target, command.path});
                        std::erase(lists.queue, command.path);
                    });
                    break;
                }
                case Action::Restore:
                case Action::DeleteForever: {
                    const auto found = std::find_if(state.library->trash.begin(), state.library->trash.end(),
                                                    [&](const MovedSong& song) { return song.file == command.path; });
                    if (found == state.library->trash.end()) break;
                    const auto song = *found;
                    if (command.action == Action::DeleteForever) {
                        if (!Recycle({song.file})) throw std::runtime_error(Utf8(song.origin.filename()) + " was kept: it could not go to the Recycle Bin.");
                        editLibrary([&](LibraryLists& lists) { std::erase(lists.trash, song); lists.Forget(song.origin); });
                        break;
                    }
                    // Back to its folder, made again if it went; beside a file that took its name meanwhile.
                    std::error_code error;
                    std::filesystem::create_directories(song.origin.parent_path(), error);
                    const auto target = FreeName(song.origin);
                    if (!MoveFileExW(song.file.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED))
                        throw std::runtime_error("Cannot restore " + Utf8(song.origin.filename()) + " to its folder.");
                    editLibrary([&](LibraryLists& lists) { std::erase(lists.trash, song); lists.Renamed(song.origin, target); });
                    if (!state.folder.empty()) Send({Action::Scan, state.folder, 0, 0, true});
                    break;
                }
                case Action::EmptyTrash: {
                    // Every song to the Recycle Bin at once, so Windows asks at most
                    // once; each that went, or was gone already, leaves every list.
                    const auto songs = state.library->trash;
                    std::vector<std::filesystem::path> files;
                    for (const auto& song : songs) if (std::error_code gone; std::filesystem::exists(song.file, gone)) files.push_back(song.file);
                    if (!files.empty()) Recycle(files);
                    size_t kept = 0;
                    editLibrary([&](LibraryLists& lists) {
                        for (const auto& song : songs) {
                            if (std::error_code gone; std::filesystem::exists(song.file, gone)) { ++kept; continue; }
                            std::erase(lists.trash, song);
                            lists.Forget(song.origin);
                        }
                    });
                    if (kept) throw std::runtime_error(std::to_string(kept) + (kept == 1 ? " song was kept: it" : " songs were kept: they") +
                                                       " could not go to the Recycle Bin.");
                    break;
                }
                case Action::ScanDrives: {
                    if (command.paths.empty()) break;
                    if (scanLine) scanLine->Close();
                    scanLine = std::make_shared<ScanLine>();
                    scanLine->engine = this;
                    state.scanning = true;
                    state.scanFolder.clear();
                    state.scanFound = 0;
                    state.scanResults = std::make_shared<const std::vector<MidiEntry>>();
                    // A place inside another is read with it.
                    std::vector<std::filesystem::path> places;
                    for (const auto& place : command.paths) {
                        const auto lowered = Lowercase(place.lexically_normal().native());
                        if (std::none_of(command.paths.begin(), command.paths.end(), [&](const std::filesystem::path& other) {
                                auto outer = Lowercase(other.lexically_normal().native());
                                if (outer.back() != L'\\') outer += L'\\';
                                return outer != lowered + L'\\' && outer != lowered && lowered.starts_with(outer);
                            }) && std::find(places.begin(), places.end(), place) == places.end())
                            places.push_back(place);
                    }
                    // Nor is a place inside a folder ignored read at all.
                    const auto folderKey = [](const std::filesystem::path& folder) {
                        auto key = Lowercase(folder.lexically_normal().native());
                        if (key.empty() || key.back() != L'\\') key += L'\\';
                        return key;
                    };
                    std::erase_if(places, [&](const std::filesystem::path& place) {
                        return std::any_of(command.ignored.begin(), command.ignored.end(), [&](const std::filesystem::path& ignored) {
                            return !ignored.empty() && folderKey(place).starts_with(folderKey(ignored)); });
                    });
                    std::vector<std::filesystem::path> own{config_.parent_path(), bundle::DataDirectory(), state.folder};
                    own.insert(own.end(), command.ignored.begin(), command.ignored.end());
                    std::thread(RunScan, scanLine, ++scanId, std::move(places), ScanSkips(std::move(own)), state.files).detach();
                    break;
                }
                case Action::ScanCancel:
                    if (scanLine) scanLine->Close();
                    scanLine.reset();
                    state.scanning = false;
                    break;
                case Action::ScanProgress:
                    if (!state.scanning || command.generation != scanId) break;
                    state.scanFolder = command.key;
                    state.scanFound = command.track;
                    if (command.value) {
                        state.scanning = false;
                        scanLine.reset();
                        if (command.files) state.scanResults = std::move(command.files);
                        ++state.scanRevision;
                    }
                    break;
                case Action::AddScanned: {
                    if (state.folder.empty()) throw std::runtime_error("Choose a MIDI folder before adding what the scan found.");
                    // Each into a folder of the library named as the one it came
                    // from, remembered so Put back can return it.
                    size_t failed = 0;
                    std::vector<MovedSong> moved;
                    for (const auto& file : command.paths) {
                        std::error_code error;
                        if (!std::filesystem::is_regular_file(file, error)) { ++failed; continue; }
                        const auto parent = file.parent_path();
                        const auto name = parent.has_relative_path() ? parent.filename() : std::filesystem::path(parent.root_name().native().substr(0, 1));
                        std::filesystem::create_directories(state.folder / name, error);
                        const auto target = FreeName(state.folder / name / file.filename());
                        if (!MoveFileExW(file.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED)) { ++failed; continue; }
                        moved.push_back({target, file});
                    }
                    editLibrary([&](LibraryLists& lists) { lists.scanned.insert(lists.scanned.end(), moved.begin(), moved.end()); });
                    auto left = std::make_shared<std::vector<MidiEntry>>();
                    for (const auto& found : *state.scanResults)
                        if (std::none_of(moved.begin(), moved.end(), [&](const MovedSong& song) { return song.origin == found.path; })) left->push_back(found);
                    state.scanResults = std::move(left);
                    ++state.scanRevision;
                    Send({Action::Scan, state.folder, 0, 0, true});
                    if (failed) throw std::runtime_error(std::to_string(failed) + (failed == 1 ? " file" : " files") + " could not be moved.");
                    break;
                }
                case Action::PutBack: {
                    std::vector<MovedSong> songs;
                    for (const auto& song : state.library->scanned)
                        if (command.paths.empty() || std::find(command.paths.begin(), command.paths.end(), song.file) != command.paths.end()) songs.push_back(song);
                    size_t failed = 0;
                    std::vector<std::pair<MovedSong, std::filesystem::path>> returned;
                    for (const auto& song : songs) {
                        // One in the Trash waits there; Restore brings it back to the library first.
                        if (state.library->Trashed(song.file)) continue;
                        std::error_code error;
                        if (!std::filesystem::is_regular_file(song.file, error)) { ++failed; continue; }
                        std::filesystem::create_directories(song.origin.parent_path(), error);
                        const auto target = FreeName(song.origin);
                        if (!MoveFileExW(song.file.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED)) { ++failed; continue; }
                        returned.push_back({song, target});
                    }
                    editLibrary([&](LibraryLists& lists) {
                        for (const auto& [song, target] : returned) { std::erase(lists.scanned, song); lists.Renamed(song.file, target); }
                    });
                    if (!returned.empty()) {
                        auto kept = std::make_shared<std::vector<MidiEntry>>();
                        for (const auto& file : *state.files)
                            if (std::none_of(returned.begin(), returned.end(), [&](const auto& back) { return back.first.file == file.path; })) kept->push_back(file);
                        state.files = std::move(kept);
                        if (!state.folder.empty()) Send({Action::Scan, state.folder, 0, 0, true});
                    }
                    if (failed) throw std::runtime_error(std::to_string(failed) + (failed == 1 ? " file" : " files") + " could not be put back.");
                    break;
                }
                case Action::AddToList:
                case Action::RemoveFromList:
                case Action::MoveInList: {
                    const int list = static_cast<int>(std::isfinite(command.amount) ? command.amount : kFolderList);
                    if (command.path.empty() || !state.library->Songs(list)) break;
                    editLibrary([&](LibraryLists& lists) {
                        auto& songs = *lists.Songs(list);
                        const auto found = std::find(songs.begin(), songs.end(), command.path);
                        if (command.action == Action::AddToList && found == songs.end()) songs.push_back(command.path);
                        if (command.action == Action::RemoveFromList && found != songs.end()) songs.erase(found);
                        if (command.action == Action::MoveInList) MoveSong(songs, command.path, command.track);
                    });
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
                    if (vk != 0 && std::erase_if(state.songHotkeys, [&](const SongHotkey& bound) { return NameToVK(bound.key) == vk; }))
                        saveSongHotkeys();
                    state.hotkeys[command.track] = command.key;
                    ++state.hotkeyRevision;
                    touchConfig();
                    break;
                }
                case Action::SongHotkey: {
                    if (command.path.empty()) break;
                    if (!command.key.empty() && NameToVK(command.key) == 0)
                        throw std::runtime_error("That key cannot be a hotkey.");
                    if (!command.key.empty() && IsNoteKey(NameToVK(command.key), state.keyMappings)) break;
                    if (!command.key.empty() && command.key.rfind("VK_", 0) != 0) command.key.insert(0, "VK_");
                    auto& songs = state.songHotkeys;
                    const auto of = [&] { return std::find_if(songs.begin(), songs.end(), [&](const SongHotkey& bound) { return bound.song == command.path; }); };
                    if (of() != songs.end() ? of()->key == command.key : command.key.empty()) break;
                    // One key, one action: an app hotkey or another song holding it lets go.
                    if (const int vk = NameToVK(command.key); vk != 0) {
                        for (size_t i = 0; i < kHotkeys; ++i) {
                            if (hotkeyFields[i].empty() || NameToVK(state.hotkeys[i]) != vk) continue;
                            state.hotkeys[i].clear();
                            configJson["HOTKEY_SETTINGS"][hotkeyFields[i]] = std::string();
                        }
                        std::erase_if(songs, [&](const SongHotkey& bound) { return bound.song != command.path && NameToVK(bound.key) == vk; });
                    }
                    if (const auto found = of(); command.key.empty()) songs.erase(found);
                    else if (found != songs.end()) found->key = command.key;
                    else songs.push_back({command.path, command.key});
                    saveSongHotkeys();
                    ++state.hotkeyRevision;
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
                    }
                    // Scan recursively; entries are named relative to the chosen
                    // folder. The panel browses one folder at a time (BrowseFolder
                    // in LibraryModel.hpp) while search covers the whole library.
                    auto files = std::move(command.files);
                    if (!files) {
                        std::error_code error;
                        files = WalkLibrary(command.path, stop, error);
                        if (!files) {
                            if (command.value) break;
                            throw std::runtime_error("Cannot read MIDI folder: " + error.message());
                        }
                        // Stop with an error if the folder is something like a drive root.
                        if (!command.value && files->size() >= kMaxLibraryFiles)
                            state.error = "Stopped at " + std::to_string(kMaxLibraryFiles) +
                                          " files. Choose a folder with fewer sub-folders in it.";
                    }
                    std::sort(files->begin(), files->end(), [&](const auto& a, const auto& b) {
                        return FileBefore(a, b, state.fileSort, state.descendingFiles);
                    });
                    // The same files again keep the list the panel already shows.
                    if (*files != *state.files) state.files = std::move(files);
                    rescanOwed = false;
                    if (!command.value) {
                        state.folder = command.path;
                        watchLibrary(state.folder);
                        moveOldTrash();
                        recoverTrash();
                    }
                    break;
                }
                case Action::Previous:
                case Action::Next: {
                    // The queue comes first: Next, and the end of a song even without
                    // Shuffle Play, load its first song still on disk, which then
                    // leaves the queue. The rest follow the list shown (SongsFollowed),
                    // and a playlist or Favourites shown goes on at a song's end without it too.
                    const bool queued = command.action == Action::Next && !state.library->queue.empty();
                    if (command.amount == 1 && (!shuffleAdvancePending || (!state.shuffle && !queued && !listPlaysOn()))) break;
                    if (command.generation != state.generation) break;
                    std::error_code gone;
                    // Queued songs gone from disk leave the queue.
                    if (queued)
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
                // the action. DetectDrums, AutoTranspose and FitToKeys apply at
                // load, so they reload the open file, resuming if it was playing.
                case Action::DetectDrums:
                case Action::AutoTranspose:
                case Action::FitToKeys:
                    if (command.action == Action::DetectDrums) {
                        state.detectDrums = command.value;
                        // process_tracks reads the singleton, not the file.
                        midi::Config::getInstance().midi.DETECT_DRUMS = command.value;
                        configJson["MIDI_SETTINGS"]["DETECT_DRUMS"] = command.value;
                    } else if (command.action == Action::AutoTranspose) {
                        state.autoTranspose = command.value;
                        configJson["AUTO_TRANSPOSE"]["ENABLED"] = command.value;
                    } else if (command.action == Action::FitToKeys) {
                        state.fitToKeys = command.value;
                        configJson["SHELL_FIT_TO_KEYS"] = command.value;
                    }
                    if (command.action == Action::DetectDrums || command.action == Action::AutoTranspose || command.action == Action::FitToKeys) {
                        touchConfig();
                        flushConfig();
                        if (state.loaded.empty()) break;
                        command.path = state.loaded;
                        command.amount = state.playing ? 1 : 0;
                        command.value = loadAutoSolo;
                    }
                    [[fallthrough]];
                // A song's own hotkey loads it and plays it at once, as the play
                // key does; a song whose file is gone does nothing.
                case Action::PlaySong:
                    if (command.action == Action::PlaySong) {
                        if (std::error_code gone; !std::filesystem::is_regular_file(command.path, gone)) break;
                        command.amount = 1;
                        command.value = loadAutoSolo;
                    }
                    [[fallthrough]];
                case Action::Load: {
                    const bool resumeAfterLoad = command.action != Action::Load && command.amount == 1;
                    // A settings reload keeps the playback position.
                    const bool sameFile = command.action == Action::DetectDrums || command.action == Action::AutoTranspose ||
                                          command.action == Action::FitToKeys;
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
                    state.density = std::make_shared<const std::vector<uint16_t>>();
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
                    if (autoTranspose) state.transpose = std::clamp(player->toggle_transpose_adjustment(), -12, 12);
                    // From the song's Transpose, or Auto-transpose's, when a note is off the keys.
                    // Drum kits are left out: Solo Piano mutes them.
                    if (state.fitToKeys) {
                        std::vector<int> notes;
                        for (const auto& event : player->note_events) {
                            if (event.action != EventType::Press || PedalForName(event.note_or_control) >= 0) continue;
                            if (std::any_of(rows.begin(), rows.end(), [&](const TrackRow& row) { return row.drums && static_cast<int>(row.index) == event.trackIndex; }))
                                continue;
                            if (const int number = MidiNumberForNoteName(std::string(event.note_or_control).c_str()); number >= 0) notes.push_back(number);
                        }
                        state.transpose = FitToKeys(notes, state.transpose, onKeys);
                    }
                    if (autoTranspose || state.fitToKeys) applyMappings();
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
                    countDensity();
                    if (sameFile) state.position = std::clamp(keepPosition, 0.0, state.duration);
                    // Another song's section is the whole song until its handles move.
                    if (sameFile && state.loopEnd > state.loopStart && state.loopStart < state.duration)
                        state.loopEnd = std::min(state.loopEnd, state.duration);
                    else { state.loopStart = 0; state.loopEnd = state.duration; }
                    applyLoop();
                    player->midiFileSelected = true;
                    state.loaded = command.path;
                    // A queued song opened any other way has had its turn.
                    if (std::find(state.library->queue.begin(), state.library->queue.end(), command.path) != state.library->queue.end())
                        editLibrary([&](LibraryLists& lists) { lists.queue.erase(std::find(lists.queue.begin(), lists.queue.end(), command.path)); });
                    score = player->score();
                    if (!sameFile) recallSong();
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
                // A speed hotkey: steps of 0.1 from the speed now, kept on the
                // slider's 0.05 grid. Relative, so it is never coalesced.
                case Action::SpeedStep:
                    command.amount = std::round((state.speed + command.amount * .1) * 20) / 20;
                    [[fallthrough]];
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
                // Stop and Panic both end with a sweep that trusts no record of what
                // is down, so a stuck key comes up on Stop's key too. The song's
                // own keys come up first, in stopPlayback.
                case Action::Panic:
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
                    if (state.converting) { state.error = "A conversion is already running."; break; }
                    if (state.folder.empty()) { state.error = "Choose a MIDI folder first. The converted file is saved there."; break; }
                    const std::wstring source = command.key.empty() ? command.path.native()
                        : std::filesystem::path(std::u8string(command.key.begin(), command.key.end())).native();
                    if (source.empty()) break;
                    const auto install = converterInstall();
                    if (!install.Found()) {
                        readConverter();   // refreshes converterCanSetUp for the popup
                        state.conversionFailed = true;
                        state.conversionStatus = kConverterMissing;
                        break;
                    }
                    // value requests the whole playlist when the link has one.
                    const bool playlist = command.value && audio_to_midi::IsPlaylistLink(command.key);
                    convertedCount = 0;
                    // amount is the CPU limit in percent; 0 means no limit.
                    const int cpu = command.amount > 0 ? std::clamp(static_cast<int>(command.amount), 1, 100) : 100;
                    const bool started = converter.Start(
                        audio_to_midi::CommandLine(install.python, install.script, source, state.folder, playlist, cpu), reportConversion);
                    state.signingIn = false;
                    state.converting = started;
                    state.conversionFailed = !started;
                    state.conversionStatus = !started ? "The converter could not start."
                        : playlist ? "Reading the playlist..." : "Starting the converter...";
                    break;
                }
                case Action::YouTubeSignIn: {
                    if (state.converting) { state.error = "Wait for the conversion to finish before signing in."; break; }
                    const auto install = converterInstall();
                    if (!install.Found() || install.signin.empty()) {
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
                    // value asks for the GPU build; over an install it swaps the build.
                    if (!install.CanSetUp() && !install.CanSwitch()) { readConverter(); break; }
                    const bool started = converter.Start(audio_to_midi::SetupCommandLine(install.setup, command.value), reportConversion);
                    state.settingUp = state.converting = started;
                    state.signingIn = false;
                    state.conversionFailed = !started;
                    state.conversionStatus = started ? "Starting the setup..." : "The setup could not start.";
                    break;
                }
                case Action::ConvertCancel: converter.Cancel(); break;
                case Action::ConvertProgress: {
                    using Kind = audio_to_midi::Status::Kind;
                    const auto kind = static_cast<Kind>(command.track);
                    if (kind == Kind::Text) {
                        // Raw Transkun output goes to the log only.
                        ShellLog::Instance().Append("[convert] " + command.key + "\n");
                        break;
                    }
                    if (kind == Kind::Step) { state.conversionStatus = command.key; break; }
                    if (kind == Kind::Saved) {
                        // One playlist item saved; rescan and keep going.
                        ++convertedCount;
                        ShellLog::Instance().Append("[convert] Saved " + command.key + "\n");
                        Send({Action::Scan, state.folder, 0, 0, true});
                        break;
                    }
                    state.converting = false;
                    state.conversionFailed = kind == Kind::Error;
                    // A conversion may have found the GPU build cannot run on the card.
                    if (!state.settingUp && !state.signingIn) readConverter();
                    if (state.settingUp) {
                        // Setup is resumable; downloaded packages are kept.
                        state.settingUp = false;
                        readConverter();
                        state.conversionStatus = kind == Kind::Done ? "The converter is installed."
                            : command.key == "Conversion cancelled." ? "Setup cancelled." : command.key;
                        if (kind == Kind::Error) ShellLog::Instance().Append("[error] Converter setup failed: " + command.key + "\n");
                        break;
                    }
                    if (state.signingIn) {
                        state.signingIn = false;
                        state.youtubeSignedIn = converterInstall().SignedIn();
                        state.conversionStatus = kind == Kind::Done ? "Signed in to YouTube. Links can be converted now."
                            : command.key == "Conversion cancelled." ? "Sign-in cancelled." : command.key;
                        break;
                    }
                    if (kind == Kind::Error) {
                        state.conversionStatus = command.key;
                        if (convertedCount)
                            state.conversionStatus += " " + std::to_string(convertedCount) +
                                (convertedCount == 1 ? " file was" : " files were") + " saved before it stopped.";
                        ShellLog::Instance().Append("[error] Conversion failed: " + command.key + "\n");
                        break;
                    }
                    if (kind == Kind::Finished) {
                        state.conversionStatus = command.key;
                        Send({Action::Scan, state.folder, 0, 0, true});
                        break;
                    }
                    const auto name = Utf8(std::filesystem::path(std::u8string(command.key.begin(), command.key.end())).filename());
                    state.conversionStatus = "Converted " + name + ".";
                    Send({Action::Scan, state.folder, 0, 0, true});
                    break;
                }
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
                    const auto answer = AskSheets(sheetsCall, {{"do", "page"}, {"page", playerPage()}});
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
                    // Rendered with the style page's settings to SheetTarget.
                    const auto stem = SheetTarget(sheetsRoot(), state.folder, state.loaded);
                    const auto answer = AskSheets(sheetsCall, {{"do", "files"}, {"page", playerPage()}, {"style", Utf8(state.sheetStylePage)},
                                                               {"outputs", sheetOutputs()}, {"stem", Utf8(stem)}});
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
                    sheetBatch = std::jthread([this, sheetsCall, style, files, folder, root, mapping, outputs](std::stop_token stopBatch) {
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
                                                       {"outputs", outputs}, {"stem", Utf8(SheetTarget(root, folder, entry.path))}});
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
                // after a transpose or remap.
                if (live && state.liveActive &&
                    (state.mappingRevision != liveMappings || state.transpose != liveTranspose)) {
                    liveMappings = state.mappingRevision;
                    liveTranspose = state.transpose;
                    live->SetActive(true);
                }
                if (reopening && command.action == Action::LiveOpen) quietInput.clear();
                if (reopening && command.action == Action::OutputOpen) quietOutput.clear();
                // It came back, so its failure is no longer news.
                if (quietReopen) state.error.clear();
                reopening = false;
            }
            if (volumePending && !stop.stop_requested()) {
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
                if (remaining <= 0) { state.playbackCountdown = 0; startPlayback(); }
                else state.playbackCountdown = static_cast<int>(std::ceil(remaining));
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
                    stopPlayback();
                    if (failed) state.error = "Playback stopped after an error.";
                    else state.position = state.duration;
                    // Shuffle Play draws the next song, and a queued song or the next in a playlist or
                    // Favourites shown comes next without it; a song on Loop plays on, and its end is never reached.
                    if (!failed && state.loop == 0 &&((state.shuffle && !state.files->empty()) || !state.library->queue.empty() || listPlaysOn())) {
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
                 command.action == Action::SheetStylePage || command.action == Action::SheetFiles ||
                 command.action == Action::SaveLibrarySheets || command.action == Action::Hotkey || command.action == Action::SongHotkey ||
                 command.action == Action::VelocityModifier ||
                 command.action == Action::Favourite || command.action == Action::OpenList ||
                 command.action == Action::NewPlaylist || command.action == Action::RenamePlaylist ||
                 command.action == Action::DeletePlaylist || command.action == Action::AddToList ||
                 command.action == Action::RemoveFromList || command.action == Action::MoveInList ||
                 command.action == Action::Trash || command.action == Action::Restore || command.action == Action::DeleteForever ||
                 command.action == Action::EmptyTrash ||
                 command.action == Action::ScanDrives || command.action == Action::ScanCancel || command.action == Action::ScanProgress ||
                 command.action == Action::AddScanned || command.action == Action::PutBack ||
                 (command.action >= Action::CurveSelect && command.action <= Action::SustainCutoff)));
            if (!leavesPlayer) stopPlayback();
            if (!quiet) state.error = error.what();
            // A failed load leaves the previous file open, so name the file
            // that failed. Previous and Next load through the same path.
            if (hasCommand && (command.action == Action::Load || command.action == Action::Previous ||
                               command.action == Action::Next || command.action == Action::PlaySong) && !command.path.empty())
                state.error = Utf8(command.path.filename()) + ": " + state.error;
            state.busy = false;
        }
        if ((configDirty || libraryDirty || songsDirty) && std::chrono::steady_clock::now() >= configDue) {
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
        if (stateChanged) Publish(state);
    }
    if (scanLine) scanLine->Close();
    poller.request_stop();
    poller.join();
    stopPlayback();
    stopLive();
    stopConnect();
    // Flush pending edits. Failures can't be reported at shutdown; the atomic
    // rename leaves the previous config intact.
    try { flushConfig(); } catch (const std::exception&) {}
}
}
