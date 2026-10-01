#pragma once

#define NOMINMAX

#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <string_view>
#include <vector>

#include "MidiInput.hpp"

#include <windows.h>

#include "PlaybackSystem.hpp"

struct PrecomputedKeyEvents;

// Turns live MIDI input into keystrokes, or forwards it to the MIDI output target.
class MIDI2Key {
public:
    MIDI2Key(VirtualPianoPlayer* player);
    ~MIDI2Key();

    void OpenDevice(const std::wstring& deviceId);
    void CloseDevice();
    void SetMidiChannel(int channel);

    bool IsActive() const;
    void SetActive(bool active);

    const std::wstring& GetSelectedDevice() const;
    int GetSelectedChannel() const;
    // Whether the last OpenDevice failed because another program holds the port.
    bool Busy() const { return m_busy; }

    // Releases every note this instance holds and clears the scancode
    // bookkeeping. Registered as the player's live release hook, so changing
    // the output target releases live keys along with autoplay's before
    // anything is sent on the new target. Safe to call when nothing is held.
    void ReleaseHeldKeys();

private:
    void ProcessMidiMessage(uint64_t timestampQpc, const uint8_t* data, size_t length);

    // Rebuilds the Wooting scancode-to-note map from whichever layout is
    // active, 88-key or 61-key. Does nothing for any other backend.
    void ApplyWootingLayout(const std::wstring& deviceId);

    // Transport, chosen per device id
    std::unique_ptr<IMidiInput> m_input;
    std::wstring m_selectedDevice;
    bool m_busy = false;
    // Whether this object holds the player's live timer tick: from the
    // device's open to its close.
    bool m_timerHeld = false;
    // Whether the sustain pedal last read as down, for the cutoff's hysteresis.
    std::atomic<bool> m_pedalDown{false};

    // Active Sensing: set once the device sends it, with when it was last
    // heard from, in steady_clock ticks. m_sensingWatch lets go of every key
    // when a device that sends it falls silent for 300 ms.
    std::atomic<bool> m_sensing{false};
    std::atomic<int64_t> m_lastHeard{0};
    std::jthread m_sensingWatch;
    // Held by SetActive, SetMidiChannel and LetGo, so a let-go between
    // callbacks never re-arms a path that is being turned off.
    std::mutex m_control;
    // Releases every key between callbacks, from a thread other than theirs.
    void LetGo();
    // Notes and pedals live play has sent down on the MIDI target and not yet
    // up, one bit per channel, so a let-go can end them on the port.
    std::array<std::atomic<uint16_t>, 128> m_portNotes{};
    std::array<std::atomic<uint16_t>, 3> m_portPedals{};
    // Sends a note-off for each of those notes and lifts each of those pedals.
    void ReleasePortNotes();


    // Rejects new callbacks and waits for in-flight ones to finish, so the
    // tables they read can be rebuilt. Leaves the path inactive.
    void Quiesce();

    std::atomic<int> m_selectedChannel;
    std::atomic<bool> m_isActive;
    // Callbacks inside ProcessMidiMessage. Incremented before m_isActive is
    // read, so Quiesce cannot see zero while one is about to enter.
    std::atomic<int> m_inFlight{0};
    VirtualPianoPlayer* m_player; // not owned; its sent_velocity is the level last sent

    // For each note [0..127], track if it is pressed
    alignas(64) std::array<std::atomic<bool>, 128> pressed;
};

