#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace shell {
// The log as it stood at one moment, in shared chunks that never change, so a
// snapshot is a list of pointers and not a copy of the text. Chunks end at a
// line's end but the last; a later view may hold a longer copy of the last one.
struct LogView {
    std::vector<std::shared_ptr<const std::string>> chunks;
    // The number of chunks[0], counted from the app's start; a chunk cut or
    // cleared away never gives its number to another.
    uint64_t first = 0;
    bool empty() const { return chunks.empty(); }
    size_t size() const { size_t total = 0; for (const auto& chunk : chunks) total += chunk->size(); return total; }
    std::string Text() const {
        std::string text;
        text.reserve(size());
        for (const auto& chunk : chunks) text += *chunk;
        return text;
    }
};

// Shared by the native host and engine snapshots. The host installs the stream
// redirect (CaptureShellLog), so test runners keep their console output.
class ShellLog {
public:
    static constexpr size_t Capacity = 256 * 1024;
    // A chunk takes lines until it is this long; only the last is copied to grow.
    static constexpr size_t ChunkSize = 16 * 1024;
    // The log file's size before it starts again, the one before kept beside it.
    static constexpr uint64_t FileCapacity = 1024 * 1024;
    static ShellLog& Instance() { static ShellLog log; return log; }
    ~ShellLog() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    // Also writes the log to path as it comes, so it outlives the app and a
    // crash report can quote it. Past FileCapacity the file moves to
    // <name>.old<extension> and a new one starts. An empty path stops it.
    void SetFile(const std::filesystem::path& path) {
        std::lock_guard lock(mutex_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        path_ = path;
        if (path_.empty()) return;
        OpenFile(OPEN_ALWAYS);
        SYSTEMTIME now; GetLocalTime(&now);
        char started[64];
        snprintf(started, sizeof(started), "---- started %04u-%02u-%02u %02u:%02u:%02u ----\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        WriteFileText(started);
    }
    std::filesystem::path OldFile() const {
        auto old = path_;
        old.replace_filename(path_.stem().wstring() + L".old" + path_.extension().wstring());
        return old;
    }
    void Append(const std::string& text) {
        if (text.empty()) return;
        std::lock_guard lock(mutex_);
        WriteFileText(text);
        // The last chunk grows by a copy while it is short or ends mid-line;
        // views that hold the old one keep it.
        if (!chunks_.empty() && (chunks_.back()->size() < ChunkSize || chunks_.back()->back() != '\n')) {
            auto grown = std::make_shared<std::string>();
            grown->reserve(chunks_.back()->size() + text.size());
            *grown += *chunks_.back();
            *grown += text;
            chunks_.back() = std::move(grown);
        } else chunks_.push_back(std::make_shared<const std::string>(text));
        size_ += text.size();
        // Past Capacity the oldest chunks go whole.
        while (size_ > Capacity && chunks_.size() > 1) {
            size_ -= chunks_.front()->size();
            chunks_.erase(chunks_.begin());
            ++first_;
        }
        if (size_ > Capacity) {
            // One chunk past it is trimmed to 7/8 of Capacity, from a line's
            // start where there is one, and numbered as a new chunk.
            const auto whole = chunks_.front();
            size_t cut = whole->size() - (Capacity - Capacity / 8);
            const auto newline = whole->find('\n', cut);
            if (newline != std::string::npos && newline + 1 < whole->size()) cut = newline + 1;
            while (cut < whole->size() && (static_cast<unsigned char>((*whole)[cut]) & 0xc0) == 0x80) ++cut;
            chunks_.front() = std::make_shared<const std::string>(whole->substr(cut));
            size_ = chunks_.front()->size();
            ++first_;
        }
        cached_.reset();
    }
    void Clear() {
        std::lock_guard lock(mutex_);
        first_ += chunks_.size();
        chunks_.clear();
        size_ = 0;
        cached_.reset();
    }
    std::shared_ptr<const LogView> Snapshot() {
        std::lock_guard lock(mutex_);
        if (!cached_) cached_ = std::make_shared<const LogView>(LogView{chunks_, first_});
        return cached_;
    }
private:
    void OpenFile(DWORD disposition) {
        file_ = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size{};
        fileSize_ = file_ != INVALID_HANDLE_VALUE && GetFileSizeEx(file_, &size) ? static_cast<uint64_t>(size.QuadPart) : 0;
    }
    // Under mutex_. Written through at once, so what a crash leaves is on disk.
    void WriteFileText(const std::string& text) {
        if (file_ == INVALID_HANDLE_VALUE) return;
        if (fileSize_ && fileSize_ + text.size() > FileCapacity) {
            CloseHandle(file_);
            MoveFileExW(path_.c_str(), OldFile().c_str(), MOVEFILE_REPLACE_EXISTING);
            OpenFile(CREATE_ALWAYS);
            if (file_ == INVALID_HANDLE_VALUE) return;
        }
        DWORD written = 0;
        WriteFile(file_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        fileSize_ += written;
    }
    std::mutex mutex_;
    std::vector<std::shared_ptr<const std::string>> chunks_;
    uint64_t first_ = 0;
    size_t size_ = 0;
    std::shared_ptr<const LogView> cached_;
    std::filesystem::path path_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t fileSize_ = 0;
};

template<class Char> class LogBuffer final : public std::basic_streambuf<Char> {
    using Traits = std::char_traits<Char>;
    using Int = typename Traits::int_type;
    std::mutex mutex_;
    std::basic_string<Char> pending_;
    bool error_;
    bool lineStart_ = true;
    void Write(const Char* data, size_t count) {
        std::lock_guard lock(mutex_);
        pending_.append(data, count);
        std::string text;
        if constexpr (std::is_same_v<Char, char>) { text = std::move(pending_); pending_.clear(); }
        else {
            size_t complete = pending_.size();
            if (complete && pending_.back() >= 0xd800 && pending_.back() <= 0xdbff) --complete;
            const int length = WideCharToMultiByte(CP_UTF8, 0, pending_.data(), static_cast<int>(complete), nullptr, 0, nullptr, nullptr);
            text.resize(length);
            if (length) WideCharToMultiByte(CP_UTF8, 0, pending_.data(), static_cast<int>(complete), text.data(), length, nullptr, nullptr);
            pending_.erase(0, complete);
        }
        std::string tagged;
        for (char c : text) {
            if (lineStart_ && c != '\n' && c != '\r') {
                SYSTEMTIME now; GetLocalTime(&now);
                char stamp[40];
                snprintf(stamp, sizeof(stamp), "[%02u:%02u:%02u]%s ", now.wHour, now.wMinute, now.wSecond, error_ ? " [error]" : "");
                tagged += stamp;
                lineStart_ = false;
            }
            tagged += c;
            if (c == '\n') lineStart_ = true;
        }
        ShellLog::Instance().Append(tagged);
    }
    Int overflow(Int c) override {
        if (!Traits::eq_int_type(c, Traits::eof())) { const Char ch = Traits::to_char_type(c); Write(&ch, 1); }
        return Traits::not_eof(c);
    }
    std::streamsize xsputn(const Char* data, std::streamsize count) override {
        if (count > 0) Write(data, static_cast<size_t>(count));
        return count;
    }
    int sync() override { return 0; }
public:
    explicit LogBuffer(bool error) : error_(error) {}
};

// Construct before ShellEngine, destroy after its worker and MIDI callbacks
// have joined. Never replace stream buffers while engine threads are running.
class CaptureShellLog {
    LogBuffer<char> out_{false}, err_{true};
    LogBuffer<wchar_t> wideOut_{false}, wideErr_{true};
    std::streambuf *outOld_, *errOld_, *clogOld_;
    std::wstreambuf *wideOutOld_, *wideErrOld_, *wideClogOld_;
public:
    CaptureShellLog() : outOld_(std::cout.rdbuf(&out_)), errOld_(std::cerr.rdbuf(&err_)),
        clogOld_(std::clog.rdbuf(&err_)), wideOutOld_(std::wcout.rdbuf(&wideOut_)),
        wideErrOld_(std::wcerr.rdbuf(&wideErr_)), wideClogOld_(std::wclog.rdbuf(&wideErr_)) {}
    ~CaptureShellLog() {
        std::cout.rdbuf(outOld_); std::cerr.rdbuf(errOld_); std::clog.rdbuf(clogOld_);
        std::wcout.rdbuf(wideOutOld_); std::wcerr.rdbuf(wideErrOld_); std::wclog.rdbuf(wideClogOld_);
    }
    CaptureShellLog(const CaptureShellLog&) = delete;
    CaptureShellLog& operator=(const CaptureShellLog&) = delete;
};
}
