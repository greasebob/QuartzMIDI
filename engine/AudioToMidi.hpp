#pragma once

// Runs the audio-to-MIDI converter (tools/mp3-to-midi/convert.py) out of process
// and parses its line-based status output. Transkun is a PyTorch model with no
// clean ONNX export, so it runs under Python; the resulting .mid is written to
// the MIDI folder and picked up by the normal folder scan.
//
// Each Job runs on its own thread and puts the process tree in a job object, so
// Cancel also kills child processes. Nothing here touches the message loop.
//
// Header-only with no project dependencies.

#include <windows.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace audio_to_midi {

struct Status {
    // Saved: one playlist item written, more to come. Finished: playlist done.
    // Append new kinds only; the engine passes Kind as an integer.
    enum class Kind { Step, Done, Error, Text, Saved, Finished };
    Kind kind = Kind::Text;
    std::string text;  // UTF-8; for Done and Saved, the path of the .mid
};

inline bool IsFinal(Status::Kind kind) {
    return kind == Status::Kind::Done || kind == Status::Kind::Error || kind == Status::Kind::Finished;
}

inline Status ParseLine(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    const auto take = [&](std::string_view prefix, Status::Kind kind, Status& out) {
        if (line.substr(0, prefix.size()) != prefix) return false;
        out = {kind, std::string(line.substr(prefix.size()))};
        return true;
    };
    Status status;
    if (take("step: ", Status::Kind::Step, status) || take("done: ", Status::Kind::Done, status) ||
        take("error: ", Status::Kind::Error, status) || take("saved: ", Status::Kind::Saved, status) ||
        take("finished: ", Status::Kind::Finished, status))
        return status;
    return {Status::Kind::Text, std::string(line)};
}

inline bool IsLink(std::wstring_view source) {
    const auto starts = [&](std::wstring_view prefix) {
        if (source.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (towlower(source[i]) != prefix[i]) return false;
        return true;
    };
    return starts(L"http://") || starts(L"https://");
}

// True for a YouTube URL with a list= query parameter (a playlist page, or a
// video opened from a playlist).
inline bool IsPlaylistLink(std::string_view link) {
    for (size_t at = link.find("list="); at != std::string_view::npos; at = link.find("list=", at + 1))
        if (at > 0 && (link[at - 1] == '?' || link[at - 1] == '&')) return true;
    return false;
}

// Quotes one argument per the CommandLineToArgvW / CRT rules: backslashes are
// doubled only when followed by a quote.
inline std::wstring QuoteArgument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        quoted.push_back(c);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

inline std::wstring CommandLine(const std::filesystem::path& python, const std::filesystem::path& script,
                                std::wstring_view source, const std::filesystem::path& outputFolder,
                                bool playlist = false, int cpuPercent = 100) {
    // --cpu is passed only below 100; convert.py uses it to size the thread
    // pool and lowers the process priority.
    return QuoteArgument(python.native()) + L" -u " + QuoteArgument(script.native()) + L" " +
           QuoteArgument(source) + L" --out-dir " + QuoteArgument(outputFolder.native()) +
           (playlist ? L" --playlist" : L"") +
           (cpuPercent < 100 ? L" --cpu " + std::to_wstring(std::max(1, cpuPercent)) : L"");
}

// Converter location. Python comes from MIDIPP_CONVERTER_PYTHON or a bundled
// interpreter beside convert.py. Fields are empty when not found; the app treats
// that as "not installed".
struct Install {
    std::filesystem::path python;
    std::filesystem::path script;
    std::filesystem::path signin;  // signin.py beside convert.py, if present
    // setup.ps1 beside convert.py; downloads Python, the packages, FFmpeg and Deno.
    std::filesystem::path setup;
    // setup.ps1 -Nvidia installed a CUDA build of PyTorch (its dist-info is
    // torch-<version>+cu<nnn>); false for the CPU build.
    bool gpu = false;
    bool Found() const { return !python.empty() && !script.empty(); }
    bool CanSetUp() const { return !Found() && !setup.empty(); }
    // An install setup.ps1 made can be run again to swap the CPU and GPU builds;
    // a Python from MIDIPP_CONVERTER_PYTHON is not setup.ps1's to change.
    bool CanSwitch() const {
        return Found() && !setup.empty() && python == script.parent_path() / L"python" / L"python.exe";
    }
    // signin.py writes the YouTube session to cookies.txt; convert.py reads it.
    bool SignedIn() const {
        std::error_code ec;
        return !script.empty() && std::filesystem::is_regular_file(script.parent_path() / L"cookies.txt", ec);
    }
};

inline std::wstring SignInCommandLine(const std::filesystem::path& python, const std::filesystem::path& script) {
    return QuoteArgument(python.native()) + L" -u " + QuoteArgument(script.native());
}

// Runs setup.ps1 under the System32 PowerShell with -ExecutionPolicy Bypass.
// It emits the same step:/done:/error: lines as convert.py. nvidia selects
// PyTorch's CUDA build.
inline std::wstring SetupCommandLine(const std::filesystem::path& setup, bool nvidia = false) {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const auto shell = std::filesystem::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    return QuoteArgument(shell.native()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
           QuoteArgument(setup.native()) + (nvidia ? L" -Nvidia" : L"");
}

// setup.ps1 -Nvidia names the card with nvidia-smi, which NVIDIA's driver puts
// in System32; without it the GPU build cannot be installed.
inline bool HasNvidiaCard() {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(system) / L"nvidia-smi.exe", ec);
}

inline Install FindInstall(const std::filesystem::path& exeFolder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    Install found;
    // Search addons\converter, then the legacy converter\ folder. The source
    // tree's tools folder is used only when the exe sits under build\, so an
    // installed copy never runs scripts from an unrelated parent directory.
    const bool built = exeFolder.parent_path().filename() == L"build";
    for (const auto& folder : {exeFolder / L"addons" / L"converter", exeFolder / L"converter",
                               built ? exeFolder.parent_path().parent_path() / L"tools" / L"mp3-to-midi" : fs::path()})
        if (!folder.empty() && fs::is_regular_file(folder / L"convert.py", ec)) {
            found.script = folder / L"convert.py";
            if (fs::is_regular_file(folder / L"signin.py", ec)) found.signin = folder / L"signin.py";
            if (fs::is_regular_file(folder / L"setup.ps1", ec)) found.setup = folder / L"setup.ps1";
            break;
        }
    if (const DWORD size = GetEnvironmentVariableW(L"MIDIPP_CONVERTER_PYTHON", nullptr, 0)) {
        std::wstring value(size, L'\0');
        value.resize(GetEnvironmentVariableW(L"MIDIPP_CONVERTER_PYTHON", value.data(), size));
        if (fs::is_regular_file(value, ec)) found.python = value;
    }
    // setup.ps1 creates setup.partial while it runs; treat that install as incomplete.
    const bool partial = !found.script.empty() && fs::exists(found.script.parent_path() / L"setup.partial", ec);
    if (partial) found.python.clear();
    if (found.python.empty() && !found.script.empty() && !partial)
        // Bundled or setup.ps1-created interpreter beside convert.py.
        for (const auto& candidate : {found.script.parent_path() / L"python" / L"python.exe",
                                      found.script.parent_path() / L".venv" / L"Scripts" / L"python.exe"})
            if (fs::is_regular_file(candidate, ec)) { found.python = candidate; break; }
    if (!found.python.empty())
        for (const auto& packages : {found.python.parent_path() / L"Lib" / L"site-packages",
                                     found.python.parent_path().parent_path() / L"Lib" / L"site-packages"})
            for (fs::directory_iterator entry(packages, ec), end; !ec && entry != end; entry.increment(ec)) {
                const auto name = entry->path().filename().wstring();
                if (name.starts_with(L"torch-") && name.ends_with(L".dist-info") && name.find(L"+cu") != std::wstring::npos)
                    found.gpu = true;
            }
    return found;
}

class Job {
public:
    using Sink = std::function<void(const Status&)>;

    Job() = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    ~Job() { Cancel(); if (worker_.joinable()) worker_.join(); }

    // Returns false if the process could not start; the sink is then never called.
    // Otherwise the sink runs on the job's thread and the last status is always
    // Done, Finished or Error.
    bool Start(const std::wstring& commandLine, Sink sink) {
        // The previous run has already reported its final status, but its
        // process tree can take seconds to exit (Python unloading torch, a
        // closing sign-in window). Joining without Cancel waits for all of it.
        if (worker_.joinable()) { Cancel(); worker_.join(); }
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inherit, 0)) return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);

        HANDLE group = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (group) SetInformationJobObject(group, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = write;
        startup.hStdError = write;
        PROCESS_INFORMATION process{};
        std::wstring mutableLine = commandLine;
        const BOOL started = CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
        CloseHandle(write);
        if (!started) {
            CloseHandle(read);
            if (group) CloseHandle(group);
            return false;
        }
        if (group) AssignProcessToJobObject(group, process.hProcess);
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        {
            std::lock_guard lock(mutex_);
            process_ = process.hProcess;
            group_ = group;
            cancelled_ = false;
        }
        worker_ = std::thread([this, read, sink = std::move(sink)] { Pump(read, sink); });
        return true;
    }

    void Cancel() {
        std::lock_guard lock(mutex_);
        if (!process_) return;
        cancelled_ = true;
        if (group_) TerminateJobObject(group_, 1);
        else TerminateProcess(process_, 1);
    }

    bool Running() const {
        std::lock_guard lock(mutex_);
        return process_ != nullptr;
    }

private:
    void Pump(HANDLE read, const Sink& sink) {
        std::string pending;
        bool finished = false;
        const auto emit = [&](std::string_view line) {
            auto status = ParseLine(line);
            if (status.kind == Status::Kind::Text && status.text.empty()) return;
            if (IsFinal(status.kind)) finished = true;
            sink(status);
        };
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) {
            pending.append(buffer, got);
            for (size_t end; (end = pending.find_first_of("\r\n")) != std::string::npos;) {
                emit(std::string_view(pending).substr(0, end));
                pending.erase(0, end + 1);
            }
        }
        if (!pending.empty()) emit(pending);
        CloseHandle(read);

        HANDLE process, group;
        bool cancelled;
        {
            std::lock_guard lock(mutex_);
            process = process_;
            group = group_;
            cancelled = cancelled_;
        }
        WaitForSingleObject(process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        {
            std::lock_guard lock(mutex_);
            process_ = nullptr;
            group_ = nullptr;
        }
        CloseHandle(process);
        if (group) CloseHandle(group);
        // Emit exactly one final status. A Cancel after the converter reported
        // done (as the destructor does) is not reported as a cancellation.
        if (finished) return;
        sink({Status::Kind::Error, cancelled ? std::string("Conversion cancelled.")
                                             : "The converter stopped with exit code " + std::to_string(code) + "."});
    }

    mutable std::mutex mutex_;
    HANDLE process_ = nullptr;
    HANDLE group_ = nullptr;
    bool cancelled_ = false;
    std::thread worker_;
};

}  // namespace audio_to_midi
