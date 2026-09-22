#pragma once

// MidiStreamSplit: splits a raw MIDI byte stream into complete messages.
//
// A Kernel Streaming pin delivers bytes, while IMidiInput callbacks receive one
// complete message each. Header-only with no project dependencies so running
// status, interleaved realtime bytes and sysex can be unit tested directly.

#include <cstddef>
#include <cstring>
#include <cstdint>

namespace midi_stream {

// How many data bytes follow a status byte.
inline int DataBytesFor(uint8_t status) {
    if (status >= 0xF8) return 0;              // realtime: status only
    switch (status & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    case 0xC0: case 0xD0: return 1;
    case 0xF0:
        switch (status) {
        case 0xF1: case 0xF3: return 1;        // quarter frame, song select
        case 0xF2: return 2;                   // song position
        default: return 0;                     // tune request, and the rest
        }
    default: return 0;
    }
}

class Splitter {
public:
    // emit(const uint8_t* message, size_t length) is called once per complete
    // message, in arrival order. The pointer is valid only for that call.
    template <class Emit>
    void feed(const uint8_t* data, size_t length, Emit&& emit) {
        for (size_t i = 0; i < length; ++i) {
            const uint8_t byte = data[i];

            // Realtime bytes may appear between any two bytes of another
            // message; emit them without disturbing the one being assembled.
            if (byte >= 0xF8) { emit(&byte, size_t{1}); continue; }

            if (byte & 0x80) {
                // Sysex is dropped; callers accept only short messages. F7 ends
                // it, and so does any other status byte (an abandoned dump).
                if (byte == 0xF0) { inSysex_ = true; pending_ = 0; continue; }
                inSysex_ = false;
                if (byte == 0xF7) { pending_ = 0; continue; }

                // A System Common status cancels running status; a channel
                // status becomes the new one.
                status_ = byte < 0xF0 ? byte : uint8_t{0};
                message_[0] = byte;
                pending_ = 1;
                expected_ = static_cast<size_t>(DataBytesFor(byte)) + 1;
                if (pending_ == expected_) { emit(message_, pending_); pending_ = 0; }
                continue;
            }

            if (inSysex_) continue;             // sysex payload
            if (pending_ == 0) {
                // Running status: a data byte with no status in front of it
                // repeats the last channel status.
                if (!status_) continue;         // data before any status at all
                message_[0] = status_;
                pending_ = 1;
                expected_ = static_cast<size_t>(DataBytesFor(status_)) + 1;
            }
            message_[pending_++] = byte;
            if (pending_ >= expected_) { emit(message_, pending_); pending_ = 0; }
        }
    }

    void reset() { status_ = 0; pending_ = 0; expected_ = 0; inSysex_ = false; }

    // For tests. State carries across feed() calls because a driver can
    // split one message across two reads.
    bool assembling() const { return pending_ != 0; }

private:
    uint8_t message_[3]{};
    uint8_t status_ = 0;     // last channel status, for running status
    size_t pending_ = 0;
    size_t expected_ = 0;
    bool inSysex_ = false;
};

// A Kernel Streaming MIDI pin delivers a run of KSMUSICFORMAT events: an
// 8-byte header (TimeDeltaMs, ByteCount) followed by ByteCount bytes padded to
// a 4-byte boundary.
//
// The read buffer is reused, so bytes past this read's data may be stale
// events; walking into them would replay old notes. The caller zeroes the
// buffer before each read, so stale space reads as ByteCount 0, and a count
// that overruns `used` ends the walk because the framing is already wrong.
struct KsMusicHeader {
    uint32_t timeDeltaMs;
    uint32_t byteCount;
};

template <class Emit>
void FeedKsEvents(Splitter& splitter, const uint8_t* data, size_t used, Emit&& emit) {
    if (!data) return;
    size_t offset = 0;
    while (offset + sizeof(KsMusicHeader) <= used) {
        KsMusicHeader music{};
        std::memcpy(&music, data + offset, sizeof(music));
        offset += sizeof(KsMusicHeader);
        if (music.byteCount == 0 || music.byteCount > used - offset) return;
        splitter.feed(data + offset, music.byteCount, emit);
        offset += (static_cast<size_t>(music.byteCount) + 3) & ~size_t{3};
    }
}

} // namespace midi_stream
