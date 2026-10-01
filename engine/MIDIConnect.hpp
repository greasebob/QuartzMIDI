#pragma once
#include "PlaybackSystem.hpp"
#include <atomic>
#include <array>
#include <vector>
#include "MidiInput.hpp"
#include <memory>
#include <string>
#include "InputHeader.h"
#include <windows.h>
#include <timeapi.h>

#define CACHE_LINE_SIZE 64

// MIDI2Key.hpp defines MAX_BATCH_INPUTS as a macro. When that header is included
// first the macro rewrites the class constant below into
// "static constexpr size_t 32 = 32;". Drop the macro before declaring the class.
#ifdef MAX_BATCH_INPUTS
#undef MAX_BATCH_INPUTS
#endif

class MIDIConnect {
public:
    MIDIConnect();
    ~MIDIConnect();

    void OpenDevice(const std::wstring& deviceId);
    void CloseDevice();
    inline bool IsActive() const { return m_isActive.load(std::memory_order_relaxed); }
    inline const std::wstring& GetSelectedDevice() const { return m_selectedDevice; }
    // Whether the last OpenDevice failed because another program holds the port.
    inline bool Busy() const { return m_busy; }
    // Off, the notes and pedal the game was told are on are turned off, as the
    // note-offs that follow are no longer passed on.
    void SetActive(bool active);
    void ReleaseAllNumpadKeys();
    // Tells the game every note it was told is on is off, and lifts the pedal
    // if it was left down. CloseDevice calls it once the transport is closed,
    // so no way of closing leaves the game's notes or pedal on.
    void ReleaseHeld();

private:
    // One message from the open transport. Safe to call concurrently: the
    // INPUT batch is a local.
    void HandleMessage(uint64_t timestampQpc, const uint8_t* data, size_t length);

    static constexpr struct {
        WORD down;
        WORD up;
    } NUMPAD_SCANCODES[12] = {
        {0x52, 0x52}, {0x4F, 0x4F}, {0x50, 0x50}, {0x51, 0x51},
        {0x4B, 0x4B}, {0x4C, 0x4C}, {0x4D, 0x4D}, {0x47, 0x47},
        {0x48, 0x48}, {0x49, 0x49}, {0x4A, 0x4A}, {0x4E, 0x4E}
    };

    static constexpr size_t MAX_BATCH_INPUTS = 32;
    // A message is composed as the two-input prefix (Numpad *), a quad
    // selected by note, then a quad selected by value. Both quads come from
    // m_keys, which keeps the tables around 20KB and in cache.
    static constexpr size_t PREFIX_INPUTS = 2;
    static constexpr size_t QUAD_INPUTS = 4;
    static constexpr size_t MESSAGE_INPUTS = PREFIX_INPUTS + 2 * QUAD_INPUTS;
    static constexpr int SUSTAIN_NOTE = 143;
    static_assert(MESSAGE_INPUTS <= MAX_BATCH_INPUTS, "a composed message must fit the batch");
    using Quad = std::array<INPUT, QUAD_INPUTS>;
    alignas(CACHE_LINE_SIZE) std::array<INPUT, PREFIX_INPUTS> m_prefix;
    alignas(CACHE_LINE_SIZE) std::array<Quad, 128> m_keys;
    alignas(CACHE_LINE_SIZE) Quad m_sustainKeys;
    size_t Compose(INPUT* out, const Quad& selector, const Quad& value) const;

    std::unique_ptr<IMidiInput> m_input;
    std::wstring m_selectedDevice;
    std::atomic<bool> m_isActive;
    bool m_busy = false;
    // The notes the game was told are on, and whether the pedal is down.
    std::array<std::atomic<bool>, 128> m_held{};
    std::atomic<bool> m_pedalDown{false};
    // Callbacks inside HandleMessage, so turning off waits out one that read
    // the switch still on before letting go of what it held.
    std::atomic<int> m_inFlight{0};

    static HANDLE s_mmcssHandle;
    static DWORD s_mmcssTaskIndex;
    static DWORD_PTR s_originalAffinity;
    static ULONG s_timerResolution;

    static bool OptimizeSystem();
    static void RestoreSystemDefaults();
    static void SetCallbackThreadPriority();
};
