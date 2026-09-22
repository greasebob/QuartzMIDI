#pragma once

#define NOMINMAX

#include <atomic>
#include <array>
#include <memory>
#include <string>
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


    // Rejects new callbacks and waits for in-flight ones to finish, so the
    // tables they read can be rebuilt. Leaves the path inactive.
    void Quiesce();

    std::atomic<int> m_selectedChannel;
    std::atomic<bool> m_isActive;
    // Callbacks inside ProcessMidiMessage. Incremented before m_isActive is
    // read, so Quiesce cannot see zero while one is about to enter.
    std::atomic<int> m_inFlight{0};
    VirtualPianoPlayer* m_player; // not owned

    // Last velocity key sent, shared by all instances.
    alignas(64) static char m_lastVelocityKey;


    // For each note [0..127], track if it is pressed
    alignas(64) std::array<std::atomic<bool>, 128> pressed;
};

