#include "MIDIConnect.hpp"
#include "InputHeader.h"
#include <iostream>
#include <cstring>

namespace {
    static const uint8_t div12[128] = {
        0,0,0,0,0,0,0,0,0,0,0,0, 1,1,1,1,1,1,1,1,1,1,1,1, 2,2,2,2,2,2,2,2,2,2,2,2,
        3,3,3,3,3,3,3,3,3,3,3,3, 4,4,4,4,4,4,4,4,4,4,4,4, 5,5,5,5,5,5,5,5,5,5,5,5,
        6,6,6,6,6,6,6,6,6,6,6,6, 7,7,7,7,7,7,7,7,7,7,7,7, 8,8,8,8,8,8,8,8,8,8,8,8,
        9,9,9,9,9,9,9,9,9,9,9,9, 10,10,10,10,10,10,10,10
    };

    static const uint8_t mod12[128] = {
        0,1,2,3,4,5,6,7,8,9,10,11, 0,1,2,3,4,5,6,7,8,9,10,11, 0,1,2,3,4,5,6,7,8,9,10,11,
        0,1,2,3,4,5,6,7,8,9,10,11, 0,1,2,3,4,5,6,7,8,9,10,11, 0,1,2,3,4,5,6,7,8,9,10,11,
        0,1,2,3,4,5,6,7,8,9,10,11, 0,1,2,3,4,5,6,7,8,9,10,11, 0,1,2,3,4,5,6,7,8,9,10,11,
        0,1,2,3,4,5,6,7,8,9,10,11, 0,1,2,3,4,5,6,7
    };

    INPUT __forceinline MakeKeyboardInput(WORD scanCode, DWORD flags) {
        INPUT inp{};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = 0;
        inp.ki.wScan = scanCode;
        inp.ki.dwFlags = flags;
        inp.ki.time = 0;
        inp.ki.dwExtraInfo = 0;
        return inp;
    }
}

// The key tables must stay small enough to remain in cache on the injection
// path (about 21KB today).
static_assert(sizeof(MIDIConnect) < 32 * 1024,
              "MIDIConnect is back to carrying a table too large to stay in cache");

HANDLE MIDIConnect::s_mmcssHandle = NULL;
DWORD MIDIConnect::s_mmcssTaskIndex = 0;
DWORD_PTR MIDIConnect::s_originalAffinity = 0;
ULONG MIDIConnect::s_timerResolution = 0;

MIDIConnect::MIDIConnect()
    : m_selectedDevice()
    , m_isActive(false)
{
    OptimizeSystem();
    constexpr DWORD SC_FLAG = KEYEVENTF_SCANCODE;
    constexpr DWORD KU_FLAG = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    m_prefix = { MakeKeyboardInput(0x37, SC_FLAG),   // Numpad *
                 MakeKeyboardInput(0x37, KU_FLAG) };

    // Each quad types index / 12 then index % 12 on the numpad. Notes and
    // values use the same encoding, so one table serves both.
    const auto quad = [](int index) {
        const auto octave = NUMPAD_SCANCODES[index / 12];
        const auto value = NUMPAD_SCANCODES[index % 12];
        return Quad{ MakeKeyboardInput(octave.down, KEYEVENTF_SCANCODE),
                     MakeKeyboardInput(octave.up, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP),
                     MakeKeyboardInput(value.down, KEYEVENTF_SCANCODE),
                     MakeKeyboardInput(value.up, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP) };
    };
    for (int index = 0; index < 128; ++index) m_keys[index] = quad(index);
    m_sustainKeys = quad(SUSTAIN_NOTE);
}


size_t MIDIConnect::Compose(INPUT* out, const Quad& selector, const Quad& value) const {
    std::memcpy(out, m_prefix.data(), sizeof(m_prefix));
    std::memcpy(out + PREFIX_INPUTS, selector.data(), sizeof(Quad));
    std::memcpy(out + PREFIX_INPUTS + QUAD_INPUTS, value.data(), sizeof(Quad));
    return MESSAGE_INPUTS;
}
MIDIConnect::~MIDIConnect() {
    CloseDevice();
    RestoreSystemDefaults();
}

bool MIDIConnect::OptimizeSystem() {
    TIMECAPS tc;
    timeGetDevCaps(&tc, sizeof(TIMECAPS));
    s_timerResolution = std::min(std::max(tc.wPeriodMin, (UINT)1), tc.wPeriodMax);
    timeBeginPeriod(s_timerResolution);
    // The priority class is left alone: raising it would put every thread of
    // the app ahead of the game receiving the keys.

    if (HANDLE hProcess = GetCurrentProcess()) {
        const ULONG EXECUTION_SPEED_MASK = 0x1;
        struct PowerThrottlingState {
            ULONG Version;
            ULONG ControlMask;
            ULONG StateMask;
        };
        PowerThrottlingState powerThrottling{};
        powerThrottling.Version = 0x1;
        powerThrottling.ControlMask = EXECUTION_SPEED_MASK;
        powerThrottling.StateMask = 0; // opt out of EcoQoS execution-speed throttling
        typedef BOOL(WINAPI* SetProcessInfoPtr)(HANDLE, INT, LPVOID, DWORD);
        if (HMODULE kernel32 = GetModuleHandleA("kernel32.dll")) {
            if (auto SetProcessInformation = (SetProcessInfoPtr)GetProcAddress(kernel32, "SetProcessInformation")) {
                SetProcessInformation(hProcess, 4, &powerThrottling, sizeof(powerThrottling));
            }
        }
    }
    return true;
}

void MIDIConnect::RestoreSystemDefaults() {
    if (s_mmcssHandle != NULL) {
        AvRevertMmThreadCharacteristics(s_mmcssHandle);
        s_mmcssHandle = NULL;
    }

    if (s_timerResolution > 0) {
        timeEndPeriod(s_timerResolution);
        s_timerResolution = 0;
    }

    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
}

void MIDIConnect::SetCallbackThreadPriority() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcssHandle != NULL) {
        if (AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_CRITICAL)) {
            s_mmcssHandle = mmcssHandle;
            s_mmcssTaskIndex = taskIndex;
        }
        else {
            AvRevertMmThreadCharacteristics(mmcssHandle);
        }
    }

    DWORD_PTR systemMask;
    if (GetProcessAffinityMask(GetCurrentProcess(), &s_originalAffinity, &systemMask)) {
        SetThreadAffinityMask(GetCurrentThread(), 1); // pin to CPU 0
    }

    typedef enum _THREADINFOCLASS {
        ThreadIoPriority = 31,
    } THREADINFOCLASS;
    typedef enum _IO_PRIORITY_HINT {
        IoPriorityCritical = 3
    } IO_PRIORITY_HINT;
    typedef DWORD(WINAPI* NtSetThreadInfoPtr)(HANDLE, THREADINFOCLASS, PVOID, ULONG);
    if (HMODULE ntdll = GetModuleHandleA("ntdll.dll")) {
        if (auto NtSetInformationThread = (NtSetThreadInfoPtr)GetProcAddress(ntdll, "NtSetInformationThread")) {
            IO_PRIORITY_HINT ioPriority = IoPriorityCritical;
            NtSetInformationThread(GetCurrentThread(), ThreadIoPriority, &ioPriority, sizeof(ioPriority));
        }
    }
}
void MIDIConnect::OpenDevice(const std::wstring& deviceId) {
    CloseDevice();
    if (deviceId.empty()) return;

    m_input = CreateMidiInput(BackendForDeviceId(deviceId));
    if (!m_input) return;

    const bool opened = m_input->open(deviceId,
        [this](uint64_t timestampQpc, const uint8_t* data, size_t length) {
            this->HandleMessage(timestampQpc, data, length);
        });
    if (!opened) {
        m_input.reset();
        return;
    }
    // Do not call SetCallbackThreadPriority here: this is the engine worker,
    // not the thread messages arrive on.
    m_selectedDevice = deviceId;
}

void MIDIConnect::CloseDevice() {
    if (m_input) {
        m_input->close();
        m_input.reset();
    }
    m_selectedDevice.clear();
}

void MIDIConnect::SetActive(bool active) {
    bool wasActive = m_isActive.load(std::memory_order_relaxed);
    m_isActive.store(active, std::memory_order_release);

    if (active && !wasActive) {
    }
}

void MIDIConnect::ReleaseAllNumpadKeys() {
    INPUT inputs[12] = {};
    static const WORD numpadScans[12] = {
        0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x37, 0x4A
    };

    for (int i = 0; i < 12; ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wScan = numpadScans[i];
        inputs[i].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    }
    input_latency::send(12, inputs, sizeof(INPUT));
}

void MIDIConnect::HandleMessage(uint64_t timestampQpc, const uint8_t* data, size_t length)
{
    if (!data || length < 3) return;
    if (!m_isActive.load(std::memory_order_relaxed)) return;
    if (data[1] > 127 || data[2] > 127) return;

    const uint8_t status = data[0];
    const uint8_t data1 = data[1];
    const uint8_t data2 = data[2];
    const uint8_t cmd = status & 0xF0;
    if (cmd != 0x90 && cmd != 0x80 && !(cmd == 0xB0 && data1 == 64)) return;
    input_latency::Trace trace(input_latency::Source::MidiConnect,
        cmd == 0xB0 ? input_latency::Kind::Sustain :
        cmd == 0x90 && data2 > 0 ? input_latency::Kind::NoteOn : input_latency::Kind::NoteOff,
        timestampQpc);

    // Local so concurrent callbacks do not share a buffer.
    INPUT batched[MAX_BATCH_INPUTS];
    size_t inputCount = 0;

    switch (cmd) {
    case 0x90: // Note On
        // A velocity-0 note-on sends value 0, the same as a note-off.
        inputCount = Compose(batched, m_keys[data1], m_keys[data2]);
        break;
    case 0x80: // Note Off; release velocity is ignored
        inputCount = Compose(batched, m_keys[data1], m_keys[0]);
        break;
    case 0xB0: // Control Change
        if (data1 == 64) // Sustain pedal
            inputCount = Compose(batched, m_sustainKeys, m_keys[data2]);
        break;
    }

    if (inputCount > 0) input_latency::send(static_cast<UINT>(inputCount), batched, sizeof(INPUT));
}
