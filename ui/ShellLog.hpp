#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

namespace shell {
// Shared by the native host and engine snapshots. The host installs the stream
// redirect (CaptureShellLog), so test runners keep their console output.
class ShellLog {
public:
    static constexpr size_t Capacity = 256 * 1024;
    static ShellLog& Instance() { static ShellLog log; return log; }
    void Append(const std::string& text) {
        if (text.empty()) return;
        std::lock_guard lock(mutex_);
        text_ += text;
        if (text_.size() > Capacity) {
            // Trim to 7/8 of Capacity so a full log isn't shifted on every append.
            size_t first = text_.size() - (Capacity - Capacity / 8);
            const auto newline = text_.find('\n', first);
            if (newline != std::string::npos) first = newline + 1;
            while (first < text_.size() && (static_cast<unsigned char>(text_[first]) & 0xc0) == 0x80) ++first;
            text_.erase(0, first);
        }
        cached_.reset();
    }
    void Clear() { std::lock_guard lock(mutex_); text_.clear(); cached_.reset(); }
    std::shared_ptr<const std::string> Snapshot() {
        std::lock_guard lock(mutex_);
        if (!cached_) cached_ = std::make_shared<const std::string>(text_);
        return cached_;
    }
private:
    std::mutex mutex_;
    std::string text_;
    std::shared_ptr<const std::string> cached_;
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
