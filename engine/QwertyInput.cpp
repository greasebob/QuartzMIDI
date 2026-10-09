#include "QwertyInput.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "InputHeader.h"

#include <atomic>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace {

// Set 1 scancodes of the keys the mappings use.
uint16_t ScancodeForKey(char key) {
    static constexpr char kRow1[] = "1234567890";
    static constexpr char kRow2[] = "qwertyuiop";
    static constexpr char kRow3[] = "asdfghjkl";
    static constexpr char kRow4[] = "zxcvbnm";
    for (int i = 0; kRow1[i]; ++i) if (kRow1[i] == key) return static_cast<uint16_t>(0x02 + i);
    for (int i = 0; kRow2[i]; ++i) if (kRow2[i] == key) return static_cast<uint16_t>(0x10 + i);
    for (int i = 0; kRow3[i]; ++i) if (kRow3[i] == key) return static_cast<uint16_t>(0x1E + i);
    for (int i = 0; kRow4[i]; ++i) if (kRow4[i] == key) return static_cast<uint16_t>(0x2C + i);
    return 0;
}

// The unshifted key for a mapping's character, and whether it needs Shift.
std::pair<char, bool> CanonicalKey(char key) {
    if (key >= 'A' && key <= 'Z') return {static_cast<char>(key + 32), true};
    static constexpr char kShiftedDigits[] = ")!@#$%^&*(";
    for (int i = 0; kShiftedDigits[i]; ++i)
        if (kShiftedDigits[i] == key) return {static_cast<char>('0' + i), true};
    return {key, false};
}

int NoteFromName(const std::string& name) {
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#",
                                     "G", "G#", "A", "A#", "B"};
    if (name.size() < 2) return -1;
    const std::string letters = name.substr(0, name.size() - 1);
    const char octaveDigit = name.back();
    if (octaveDigit < '0' || octaveDigit > '9') return -1;
    for (int i = 0; i < 12; ++i) {
        if (letters == kNames[i]) return (octaveDigit - '0' + 1) * 12 + i;
    }
    return -1;
}

constexpr uint32_t kSlashScan = 0x35, kEnterScan = 0x1C, kEscScan = 0x01;

} // namespace

QwertyNoteMap QwertyNoteMapFrom(const std::map<std::string, std::string>& keyMappings) {
    QwertyNoteMap map;
    for (const auto& [name, key] : keyMappings) {
        if (key.empty() || key.find("alt+") != std::string::npos) continue;
        const int note = NoteFromName(name);
        if (note < 0 || note > 127) continue;
        const auto [canonical, shifted] = CanonicalKey(key.back());
        const uint16_t scan = ScancodeForKey(canonical);
        if (!scan) continue;
        const bool ctrl = key.find("ctrl+") != std::string::npos;
        auto& table = ctrl ? map.ctrl : shifted ? map.shift : map.plain;
        // Near the middle of the keyboard, where most playing is.
        if (table[scan] < 0 || std::abs(note - 66) < std::abs(table[scan] - 66))
            table[scan] = static_cast<int16_t>(note);
    }
    return map;
}

QwertyKeyResult QwertyKeyStep(QwertyKeyState& state, const QwertyNoteMap& map,
                              uint32_t vk, uint32_t scan, bool extended, bool up,
                              bool robloxFront) {
    QwertyKeyResult result;
    const bool intercepting = map.armed && robloxFront && !state.chat && !state.alt;

    // Shift and Ctrl choose the note, so the game never sees them: a held
    // Shift would turn every note it reads into its sharp. Alt passes, since
    // Alt and a key set the game's velocity.
    const auto modifier = [&](bool& held, bool& kept) {
        if (up) { held = false; result.swallow = std::exchange(kept, false); return result; }
        if (!held) kept = intercepting;
        held = true;
        result.swallow = kept;
        return result;
    };
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return modifier(state.shift, state.shiftKept);
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return modifier(state.ctrl, state.ctrlKept);
    case VK_MENU: case VK_LMENU: case VK_RMENU: state.alt = !up; return result;
    default: break;
    }
    if (scan >= state.held.size() || extended) return result;

    uint8_t& held = state.held[scan];
    if (up) {
        result.swallow = held == 2;
        held = 0;
        if (state.sounding[scan] >= 0) {
            result.note = std::exchange(state.sounding[scan], int16_t{-1});
            result.on = false;
        }
        return result;
    }
    // A held key repeating goes where its first press went.
    if (held) { result.swallow = held == 2; return result; }
    held = 1;
    if (state.chat) {
        if (scan == kEnterScan || scan == kEscScan) state.chat = false;
        return result;
    }
    if (!intercepting) return result;
    // '/' opens the game's chat, which then takes typing as text.
    if (scan == kSlashScan) { state.chat = true; return result; }

    const auto& table = state.ctrl ? map.ctrl : state.shift ? map.shift : map.plain;
    const int note = table[scan] >= 0 ? table[scan] : map.plain[scan];
    if (note < 0) return result;
    held = 2;
    state.sounding[scan] = static_cast<int16_t>(note);
    result.swallow = true;
    result.note = note;
    result.on = true;
    return result;
}

namespace {

std::mutex g_mapMutex;
QwertyNoteMap g_map;
std::atomic<uint32_t> g_mapRevision{0};

class QwertyMidiInput;
// The input whose hook is installed. One keyboard hook at a time.
std::atomic<QwertyMidiInput*> g_hooked{nullptr};

// The hook decides and queues; a thread of its own plays the notes. Playing
// injects keys, which come back through this hook, so the hook's thread must
// never wait on an injection.
class QwertyMidiInput final : public IMidiInput {
public:
    ~QwertyMidiInput() override { close(); }

    MidiBackend backend() const noexcept override { return MidiBackend::Qwerty; }

    std::vector<MidiInputDevice> enumerate() override {
        MidiInputDevice device;
        device.id = kQwertyDeviceId;
        device.name = L"QWERTY keyboard";
        device.backend = MidiBackend::Qwerty;
        device.group = device.name;
        return {device};
    }

    bool open(const std::wstring& deviceId, MidiInputCallback callback) override {
        close();
        if (deviceId != kQwertyDeviceId || !callback) return false;
        QwertyMidiInput* expected = nullptr;
        if (!g_hooked.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) return false;
        m_callback = std::move(callback);
        m_keys = QwertyKeyState{};
        m_mapRevision = ~0u;
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
        m_stop.store(false, std::memory_order_relaxed);
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_wake) { g_hooked.store(nullptr, std::memory_order_release); return false; }
        m_worker = std::thread([this] { Deliver(); });

        std::promise<bool> started;
        auto hooked = started.get_future();
        m_hookThread = std::thread([this, &started] { HookLoop(started); });
        m_starting = true;
        if (!hooked.get()) { close(); return false; }
        m_id = deviceId;
        m_open = true;
        return true;
    }

    void close() override {
        if (!m_starting) return;
        m_starting = false;
        if (m_hookThread.joinable()) {
            if (const DWORD id = m_hookThreadId.load(std::memory_order_acquire))
                PostThreadMessageW(id, WM_QUIT, 0, 0);
            m_hookThread.join();
        }
        m_hookThreadId.store(0, std::memory_order_relaxed);
        // The hook is gone, so its state is this thread's: let go of what it held.
        const uint64_t now = Now();
        for (auto& note : m_keys.sounding)
            if (note >= 0) Queue(now, static_cast<uint8_t>(std::exchange(note, int16_t{-1})), false);
        m_stop.store(true, std::memory_order_release);
        SetEvent(m_wake);
        if (m_worker.joinable()) m_worker.join();
        CloseHandle(m_wake);
        m_wake = nullptr;
        QwertyMidiInput* self = this;
        g_hooked.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
        m_callback = nullptr;
        m_open = false;
        m_id.clear();
    }

    bool isOpen() const noexcept override { return m_open; }
    const std::wstring& openedDeviceId() const noexcept override { return m_id; }

    static LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam) {
        if (code == HC_ACTION) {
            const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            // Injected keys include this app's own notes.
            if (!(key.flags & LLKHF_INJECTED))
                if (auto* self = g_hooked.load(std::memory_order_acquire); self && self->OnKey(key))
                    return 1;
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

private:
    static uint64_t Now() {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<uint64_t>(counter.QuadPart);
    }

    void HookLoop(std::promise<bool>& started) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // the queue WM_QUIT is posted to
        m_hookThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, GetModuleHandleW(nullptr), 0);
        started.set_value(hook != nullptr);
        if (!hook) return;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
        UnhookWindowsHookEx(hook);
    }

    // On the hook's thread, which every key on the system waits for.
    bool OnKey(const KBDLLHOOKSTRUCT& key) {
        const uint64_t at = Now();
        if (const uint32_t revision = g_mapRevision.load(std::memory_order_acquire); revision != m_mapRevision) {
            std::lock_guard lock(g_mapMutex);
            m_map = g_map;
            m_mapRevision = revision;
        }
        const bool front = m_map.armed && ForegroundProbe().roblox;
        const QwertyKeyResult result = QwertyKeyStep(m_keys, m_map, key.vkCode, key.scanCode,
                                                     (key.flags & LLKHF_EXTENDED) != 0,
                                                     (key.flags & LLKHF_UP) != 0, front);
        if (result.note >= 0) {
            Queue(at, static_cast<uint8_t>(result.note), result.on);
            SetEvent(m_wake);
        }
        return result.swallow;
    }

    struct Queued { uint64_t at; uint8_t note; bool on; };

    void Queue(uint64_t at, uint8_t note, bool on) {
        const uint32_t head = m_head.load(std::memory_order_relaxed);
        if (head - m_tail.load(std::memory_order_acquire) >= m_queue.size()) return;
        m_queue[head % m_queue.size()] = {at, note, on};
        m_head.store(head + 1, std::memory_order_release);
    }

    void Deliver() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        for (;;) {
            WaitForSingleObject(m_wake, INFINITE);
            const bool stopping = m_stop.load(std::memory_order_acquire);
            uint32_t tail = m_tail.load(std::memory_order_relaxed);
            while (tail != m_head.load(std::memory_order_acquire)) {
                const Queued item = m_queue[tail % m_queue.size()];
                const uint8_t message[3] = {static_cast<uint8_t>(item.on ? 0x90 : 0x80), item.note,
                                            item.on ? kQwertyVelocity : uint8_t{0}};
                m_callback(item.at, message, 3);
                m_tail.store(++tail, std::memory_order_release);
            }
            if (stopping) return;
        }
    }

    MidiInputCallback m_callback;
    std::wstring m_id;
    bool m_open = false;
    bool m_starting = false;
    std::thread m_hookThread, m_worker;
    std::atomic<DWORD> m_hookThreadId{0};
    HANDLE m_wake = nullptr;
    std::atomic<bool> m_stop{false};

    // The hook thread's own.
    QwertyKeyState m_keys;
    QwertyNoteMap m_map;
    uint32_t m_mapRevision = ~0u;

    std::array<Queued, 256> m_queue{};
    std::atomic<uint32_t> m_head{0}, m_tail{0};
};

} // namespace

void SetQwertyNoteMap(const QwertyNoteMap& map) {
    std::lock_guard lock(g_mapMutex);
    g_map = map;
    g_mapRevision.fetch_add(1, std::memory_order_release);
}

std::unique_ptr<IMidiInput> CreateQwertyInput() {
    return std::make_unique<QwertyMidiInput>();
}
