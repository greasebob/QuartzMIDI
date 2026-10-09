#pragma once
#define NOMINMAX
#include "midi_structures.h"
#include <fstream>
#include <string>
#include <cstdint>
#include <windows.h>

class MidiParser {
public:
    // A larger file, or one of more events or tracks, is refused as too large
    // to play: the parse and the player's copy of it would take gigabytes.
    static constexpr uint64_t kMaxFileBytes = 64ull << 20;
    static constexpr size_t kMaxEvents = 4'000'000;
    static constexpr size_t kMaxTracks = 65535;
    void reset();
    [[nodiscard]] MidiFile parse(const std::string& filename);
private:
    mutable std::ifstream file;
    static constexpr uint32_t swapUint32(uint32_t value) noexcept;
    static constexpr uint16_t swapUint16(uint16_t value) noexcept;
    [[nodiscard]] bool readInt32(uint32_t& value);
    [[nodiscard]] bool readInt16(uint16_t& value);  // For unsigned fields
    [[nodiscard]] bool readInt16(int16_t& value);   // For signed division
    [[nodiscard]] bool readChunk(char* buffer, size_t size);
    // False for an event to leave out: a tempo of 0, or a time signature
    // whose denominator is past 2^7.
    bool parseMetaEvent(MidiEvent& event, MidiFile& midiFile, uint32_t absoluteTick,
        const char* trackEnd, const char*& ptr);
};