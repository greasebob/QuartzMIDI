#include "InputLatency.hpp"
#include "InputHeader.h"

#include <thread>

namespace input_latency {
namespace {
static_assert(sizeof(ULONG_PTR) == sizeof(uint64_t), "Latency tags require the x64 build");
Ring<Record, 8192> records;
std::atomic<bool> running{false};
std::atomic<DWORD> lastHookError{0};
std::atomic<uint64_t> nextId{1};
std::atomic<uint64_t> sessionFirstId{1};
std::atomic<uint64_t> tagPrefix{0};
std::thread hookThread;
DWORD hookThreadId = 0;
thread_local Trace* currentTrace = nullptr;
constexpr uint64_t PrefixMask = 0xFFFFFFFF00000000ULL;

LRESULT CALLBACK keyboardHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if ((event.flags & LLKHF_INJECTED) && isOurTag(event.dwExtraInfo)) {
            Record record;
            record.type = RecordType::Observation;
            record.submission.id = event.dwExtraInfo;
            record.submission.t2 = nowQpc();
            records.push(record);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
} // namespace

uint64_t nowQpc() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

uint64_t frequency() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

bool isOurTag(ULONG_PTR tag) noexcept {
    const auto prefix = tagPrefix.load(std::memory_order_relaxed);
    return prefix != 0 && (tag & PrefixMask) == prefix && (tag & ~PrefixMask) != 0;
}

bool enabled() noexcept { return running.load(std::memory_order_acquire); }
DWORD hookError() noexcept { return lastHookError.load(std::memory_order_relaxed); }
uint64_t dropped() noexcept { return records.dropped(); }

bool start() noexcept {
    if (enabled()) return true;
    if (hookThread.joinable()) hookThread.join();
    lastHookError.store(0, std::memory_order_relaxed);
    sessionFirstId.store(nextId.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (tagPrefix.load(std::memory_order_relaxed) == 0) {
        const uint64_t cookie = (nowQpc() ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 12)) & 0xFFFFFFFFULL;
        tagPrefix.store((cookie ? cookie : 0x4D50504CULL) << 32, std::memory_order_relaxed);
    }
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) { lastHookError.store(GetLastError()); return false; }
    try {
        hookThread = std::thread([ready] {
            // Create this thread's message queue before publishing readiness.
            MSG message{};
            PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            hookThreadId = GetCurrentThreadId();
            HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, GetModuleHandleW(nullptr), 0);
            if (!hook) lastHookError.store(GetLastError(), std::memory_order_relaxed);
            running.store(hook != nullptr, std::memory_order_release);
            SetEvent(ready);
            if (!hook) return;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            UnhookWindowsHookEx(hook);
            running.store(false, std::memory_order_release);
        });
    }
    catch (...) {
        CloseHandle(ready);
        lastHookError.store(ERROR_NOT_ENOUGH_MEMORY, std::memory_order_relaxed);
        return false;
    }
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    if (!enabled()) hookThread.join();
    return enabled();
}

void stop() noexcept {
    running.store(false, std::memory_order_release);
    if (hookThread.joinable()) {
        PostThreadMessageW(hookThreadId, WM_QUIT, 0, 0);
        hookThread.join();
    }
}

Trace::Trace(Source source, Kind kind, uint64_t t0) noexcept : previous_(currentTrace) {
    currentTrace = this;
    if (!enabled()) return;
    const auto id = nextId.fetch_add(1, std::memory_order_relaxed);
    // Never reuse a tag if the 32-bit sequence is exhausted in this process.
    if (id > 0xFFFFFFFFULL) return;
    submission_.id = tagPrefix.load(std::memory_order_relaxed) | id;
    submission_.t0 = t0 ? t0 : nowQpc();
    submission_.source = source;
    submission_.kind = kind;
}

Trace::~Trace() {
    currentTrace = previous_;
    if (!submission_.id) return;
    Record record;
    record.submission = submission_;
    records.push(record);
}

UINT send(UINT count, const INPUT* inputs, int cbSize) noexcept {
    auto* trace = currentTrace;
    if (!trace || !trace->submission_.id)
        return InjectInput(count, const_cast<INPUT*>(inputs), cbSize);

    auto& s = trace->submission_;
    INPUT tagged[64];
    bool canTag = inputs && count <= std::size(tagged) && cbSize == sizeof(INPUT);
    if (canTag) {
        for (UINT i = 0; i < count; ++i) {
            if (inputs[i].type != INPUT_KEYBOARD || inputs[i].ki.dwExtraInfo != 0) {
                canTag = false;
                break;
            }
            tagged[i] = inputs[i];
            tagged[i].ki.dwExtraInfo = static_cast<ULONG_PTR>(s.id);
        }
    }
    if (!canTag) s.untagged += count;
    s.requested += count;
    ++s.calls;
    SetLastError(ERROR_SUCCESS);
    const auto before = nowQpc();
    const UINT returned = InjectInput(count,
        canTag ? tagged : const_cast<INPUT*>(inputs), cbSize);
    const auto after = nowQpc();
    const DWORD error = GetLastError();
    if (!s.t1) s.t1 = before;
    s.t2 = after;
    s.callTicks += after - before;
    // A return greater than count is a failure, never evidence of delivered input.
    s.accepted += returned <= count ? returned : 0;
    if (returned != count) {
        ++s.failures;
        s.error = returned > count ? ERROR_INVALID_DATA : error;
    }
    SetLastError(error);
    return returned;
}

void poll(Collector& collector) {
    const auto now = nowQpc();
    Record record;
    // Bound the UI work even if producers keep filling the ring during a poll.
    const auto firstId = sessionFirstId.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 8192 && records.pop(record); ++i) {
        if ((record.submission.id & ~PrefixMask) >= firstId)
            collector.ingest(record, now);
    }
    collector.expire(now, frequency() * 2);
}

} // namespace input_latency
