#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>

#include "InputHeader.h"

#pragma comment(lib, "dbghelp.lib")

// When the app dies it must not leave keys held in the game: a key the game
// saw go down and never come up keeps sounding, or keeps a modifier held for
// everything typed after. A crash sends a list of key-ups made ahead of time,
// since nothing can be trusted to allocate by then, and writes a minidump and
// the log's last lines to crash files in the app's data folder.
//
// An unhandled exception, including one on a thread the app made, reaches the
// filter below; a C++ exception nothing caught ends in std::terminate, whose
// handler MSVC keeps per thread (GuardThread), and then in abort, whose
// SIGABRT handler is the process's.
namespace crash {

inline constexpr size_t kMaxKeyUps = 512;
inline INPUT g_keyUps[kMaxKeyUps]{};
inline std::atomic<size_t> g_keyUpCount{ 0 };
inline std::atomic_flag g_keyUpsWriting = ATOMIC_FLAG_INIT;
inline wchar_t g_folder[MAX_PATH * 2]{};
inline wchar_t g_log[MAX_PATH * 2]{};
inline std::atomic<bool> g_crashed{ false };

// The key-ups a crash sends. Replaces the list; at most kMaxKeyUps are kept.
inline void SetKeyUps(const INPUT* inputs, size_t count) noexcept {
    while (g_keyUpsWriting.test_and_set(std::memory_order_acquire)) {}
    count = count < kMaxKeyUps ? count : kMaxKeyUps;
    g_keyUpCount.store(0, std::memory_order_release);
    if (count) std::memcpy(g_keyUps, inputs, count * sizeof(INPUT));
    g_keyUpCount.store(count, std::memory_order_release);
    g_keyUpsWriting.clear(std::memory_order_release);
}

inline size_t KeyUpCount() noexcept { return g_keyUpCount.load(std::memory_order_acquire); }

// Sends the key-ups through the app's injection path, which lets key-ups
// through whatever window is in front.
inline void ReleaseKeys() noexcept {
    const size_t count = KeyUpCount();
    if (count && InjectInput) InjectInput(static_cast<ULONG>(count), g_keyUps, sizeof(INPUT));
}

inline void WriteText(HANDLE file, const char* text, size_t length) noexcept {
    DWORD written = 0;
    if (length) WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
}
inline void WriteText(HANDLE file, const char* text) noexcept { WriteText(file, text, std::strlen(text)); }

// Writes crash-<date>-<time>.dmp and .txt into the folder given to Install:
// the dump, and why the app stopped with the log's last lines. Returns the
// text file's path, or an empty string when nothing was written.
inline const wchar_t* WriteReport(EXCEPTION_POINTERS* exception, const char* reason) noexcept {
    static wchar_t text[MAX_PATH * 2 + 64];
    text[0] = 0;
    if (!g_folder[0]) return text;
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stem[MAX_PATH * 2 + 48];
    swprintf_s(stem, L"%s\\crash-%04u%02u%02u-%02u%02u%02u", g_folder, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    wchar_t dump[MAX_PATH * 2 + 64];
    swprintf_s(dump, L"%s.dmp", stem);
    const HANDLE dumpFile = CreateFileW(dump, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool dumped = false;
    if (dumpFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{ GetCurrentThreadId(), exception, FALSE };
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dumpFile,
                                   static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                                   exception ? &info : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(dumpFile);
    }
    swprintf_s(text, L"%s.txt", stem);
    const HANDLE file = CreateFileW(text, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { text[0] = 0; return text; }
    char line[512];
    std::snprintf(line, sizeof(line), "QuartzMIDI stopped on %04u-%02u-%02u at %02u:%02u:%02u: %s\r\n",
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, reason ? reason : "unknown");
    WriteText(file, line);
    WriteText(file, dumped ? "A minidump of it is beside this file.\r\n" : "No minidump could be written.\r\n");
    // The last lines of the log file, which the log writes as it goes.
    const HANDLE log = g_log[0] ? CreateFileW(g_log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    if (log != INVALID_HANDLE_VALUE) {
        static char tail[16384];
        LARGE_INTEGER size{};
        GetFileSizeEx(log, &size);
        const LONGLONG from = size.QuadPart > static_cast<LONGLONG>(sizeof(tail)) ? size.QuadPart - static_cast<LONGLONG>(sizeof(tail)) : 0;
        LARGE_INTEGER at{};
        at.QuadPart = from;
        SetFilePointerEx(log, at, nullptr, FILE_BEGIN);
        DWORD read = 0;
        ReadFile(log, tail, sizeof(tail), &read, nullptr);
        CloseHandle(log);
        size_t start = 0;
        // Begun partway into the file: from the first whole line.
        if (from > 0) while (start < read && tail[start++] != '\n') {}
        WriteText(file, "\r\nThe last lines of the log:\r\n");
        WriteText(file, tail + start, read - start);
    }
    CloseHandle(file);
    return text;
}

// Once, whichever way the app goes down first.
inline void Report(EXCEPTION_POINTERS* exception, const char* reason) noexcept {
    if (g_crashed.exchange(true)) return;
    ReleaseKeys();
    WriteReport(exception, reason);
}

inline LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* exception) {
    char reason[96];
    std::snprintf(reason, sizeof(reason), "exception 0x%08lX at %p",
                  exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionCode : 0ul,
                  exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionAddress : nullptr);
    Report(exception, reason);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] inline void OnTerminate() noexcept {
    // Copied while the exception is alive: the object goes with the exception
    // pointer at the end of the try, and a pointer to its what() then read
    // freed memory, so a report could name its reason as a few stray bytes.
    char reason[256] = "an error nothing handled";
    try {
        if (const auto current = std::current_exception()) std::rethrow_exception(current);
    }
    catch (const std::exception& error) { std::snprintf(reason, sizeof(reason), "%s", error.what()); }
    catch (...) {}
    Report(nullptr, reason);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

inline void OnAbort(int) {
    Report(nullptr, "the app was aborted");
    TerminateProcess(GetCurrentProcess(), 3);
}

// std::terminate's handler for the calling thread; every thread the app makes
// that runs its own code calls this first.
inline void GuardThread() noexcept { std::set_terminate(&OnTerminate); }

// Writes crash files into folder with the tail of the log file at log.
inline void Install(const std::filesystem::path& folder, const std::filesystem::path& log) noexcept {
    wcsncpy_s(g_folder, folder.c_str(), _TRUNCATE);
    wcsncpy_s(g_log, log.c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(&OnUnhandledException);
    std::signal(SIGABRT, &OnAbort);
    GuardThread();
}

} // namespace crash
