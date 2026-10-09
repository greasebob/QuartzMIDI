#pragma once
#include <cstdint>

// A MIDI file's division with the top bit set is SMPTE time: the high byte is
// minus the frames per second and the low byte the ticks per frame, so a tick
// is a fixed time and tempo changes do not move it. The frame rate is taken as
// written, 29 included, as playback takes it.
namespace midi_timing {

inline bool IsSmpte(uint16_t division) { return (division & 0x8000) != 0; }

// Ticks per second of an SMPTE division; 0 for a ticks-per-quarter division,
// or for a frame rate or ticks per frame of 0.
inline double SmpteTicksPerSecond(uint16_t division) {
    if (!IsSmpte(division)) return 0;
    const int framesPerSecond = -static_cast<int8_t>(division >> 8);
    const int ticksPerFrame = division & 0xFF;
    return framesPerSecond > 0 && ticksPerFrame > 0 ? static_cast<double>(framesPerSecond) * ticksPerFrame : 0;
}

// Seconds per tick at this tempo; 0 for a division that times nothing.
inline double SecondsPerTick(uint16_t division, uint32_t microsecondsPerQuarter) {
    if (IsSmpte(division)) {
        const double perSecond = SmpteTicksPerSecond(division);
        return perSecond > 0 ? 1 / perSecond : 0;
    }
    return division == 0 ? 0 : microsecondsPerQuarter / 1e6 / division;
}

}
