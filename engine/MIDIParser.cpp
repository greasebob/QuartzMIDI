#include "midi_parser.h"
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstring>
#include <climits>
#include <fstream>

// Maximum size of meta and SysEx event data.
constexpr uint32_t MAX_EVENT_LENGTH = 0x100000; // 1 MB

//==========================================================================
// Byte-swap helpers (SMF is big-endian)
//==========================================================================
constexpr uint32_t MidiParser::swapUint32(uint32_t value) noexcept {
    return ((value >> 24) & 0x000000FF) | ((value >> 8) & 0x0000FF00) |
        ((value << 8) & 0x00FF0000) | ((value << 24) & 0xFF000000);
}

constexpr uint16_t MidiParser::swapUint16(uint16_t value) noexcept {
    return (value >> 8) | (value << 8);
}

//==========================================================================
// File-stream readers (header and chunk framing)
//==========================================================================
void MidiParser::reset() {
    if (file.is_open()) {
        file.close();
    }
    file.clear();
}

bool MidiParser::readInt32(uint32_t& value) {
    if (!file.read(reinterpret_cast<char*>(&value), 4))
        return false;
    value = swapUint32(value);
    return true;
}

bool MidiParser::readInt16(uint16_t& value) {
    if (!file.read(reinterpret_cast<char*>(&value), 2))
        return false;
    value = swapUint16(value);
    return true;
}

bool MidiParser::readChunk(char* buffer, size_t size) {
    return file.read(buffer, size).good();
}

//==========================================================================
// Bounds-checked readers over an in-memory track buffer
//==========================================================================
namespace {
    // Reads one byte from the buffer; throws if out-of-range.
    inline uint8_t readByte(const char*& ptr, const char* end) {
        if (ptr >= end)
            throw std::runtime_error("Unexpected end of track data while reading a byte");
        return static_cast<uint8_t>(*ptr++);
    }
    inline void readVarLenFromBuffer(const char*& ptr, const char* end, uint32_t& value) {
        value = 0;
        uint8_t byte;
        int count = 0;
        do {
            if (count++ >= 4)
                throw std::runtime_error("Variable-length quantity exceeds maximum allowed length");
            byte = readByte(ptr, end);
            if (value > (UINT32_MAX >> 7))
                throw std::runtime_error("Variable-length quantity overflow");
            value = (value << 7) | (byte & 0x7F);
        } while (byte & 0x80);
    }
} 
bool hasPathTraversal(const std::string& path) {
    std::string normalizedPath = path;
    std::transform(normalizedPath.begin(), normalizedPath.end(), normalizedPath.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    size_t i = 0;
    while (i < normalizedPath.size()) {
        while (i < normalizedPath.size() && (normalizedPath[i] == '/' || normalizedPath[i] == '\\'))
            ++i;
        if (i >= normalizedPath.size())
            break;
        size_t j = i;
        while (j < normalizedPath.size() && (normalizedPath[j] != '/' && normalizedPath[j] != '\\'))
            ++j;
        std::string segment = normalizedPath.substr(i, j - i);
        if (segment == "..")
            return true;
        i = j;
    }
    const char* reservedNames[] = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
    };
    size_t lastSlash = normalizedPath.find_last_of("/\\");
    std::string filename = normalizedPath.substr(
        lastSlash == std::string::npos ? 0 : lastSlash + 1);

    for (const auto& name : reservedNames) {
        size_t dotPos = filename.find('.');
        std::string baseName = (dotPos == std::string::npos) ?
            filename : filename.substr(0, dotPos);

        if (baseName == name)
            return true;
    }
    if (filename.find(':') != std::string::npos)
        return true;

    // Accept a rooted Windows drive prefix, but reject other colons including
    // alternate data streams and drive-relative paths.
    const size_t colon = normalizedPath.find(':');
    if (colon != std::string::npos &&
        (colon != 1 || normalizedPath.size() < 3 ||
         normalizedPath[0] < 'a' || normalizedPath[0] > 'z' ||
         (normalizedPath[2] != '/' && normalizedPath[2] != '\\') ||
         normalizedPath.find(':', 2) != std::string::npos))
        return true;
    const char invalidChars[] = { '<', '>', '"', '|', '?', '*' };
    for (char c : invalidChars) {
        if (normalizedPath.find(c) != std::string::npos)
            return true;
    }
    if (!filename.empty() && (filename.back() == ' ' || filename.back() == '.'))
        return true;

    return false;
}

void MidiParser::parseMetaEvent(MidiEvent& event, MidiFile& midiFile, uint32_t absoluteTick,
    const char* trackEnd, const char*& ptr) {
    uint8_t metaType = readByte(ptr, trackEnd);
    uint32_t length = 0;
    readVarLenFromBuffer(ptr, trackEnd, length);
    if (length > MAX_EVENT_LENGTH)
        throw std::runtime_error("Meta event length exceeds maximum allowed value");
    if (static_cast<size_t>(trackEnd - ptr) < length)
        throw std::runtime_error("Meta event data length exceeds track data");

    event.metaData.resize(length);
    std::memcpy(event.metaData.data(), ptr, length);
    ptr += length;

    event.status = 0xFF;
    event.data1 = metaType;

    switch (metaType) {
    case 0x51: { // Tempo event
        if (length == 3) {
            uint32_t microsecondsPerQuarter = (static_cast<uint8_t>(event.metaData[0]) << 16) |
                (static_cast<uint8_t>(event.metaData[1]) << 8) |
                (static_cast<uint8_t>(event.metaData[2]));
            midiFile.tempoChanges.push_back({ absoluteTick, microsecondsPerQuarter });
        }
        break;
    }
    case 0x58: { // Time Signature
        if (length == 4) {
            if (event.metaData[1] >= 8)
                throw std::runtime_error("Invalid time signature denominator");
            midiFile.timeSignatures.push_back({
                absoluteTick,
                static_cast<uint8_t>(event.metaData[0]),
                static_cast<uint8_t>(1 << (event.metaData[1])), // stored as a power of 2
                static_cast<uint8_t>(event.metaData[2]),
                static_cast<uint8_t>(event.metaData[3])
                });
        }
        break;
    }
    case 0x59: { // Key Signature
        if (length == 2) {
            midiFile.keySignatures.push_back({
                absoluteTick,
                static_cast<int8_t>(event.metaData[0]),
                static_cast<uint8_t>(event.metaData[1])
                });
        }
        break;
    }
    default:
        // Other meta events keep their raw data.
        break;
    }
}

//==========================================================================
// parse()
//==========================================================================
MidiFile MidiParser::parse(const std::string& filename) {
    reset();
    if (hasPathTraversal(filename))
        throw std::runtime_error("Invalid filename path");
    int wlen = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, NULL, 0);
    if (wlen == 0) {
        throw std::runtime_error("Failed to convert filename to wide string");
    }
    std::vector<wchar_t> wfilename(wlen);
    if (MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wfilename.data(), wlen) == 0) {
        throw std::runtime_error("Failed to convert filename to wide string");
    }
    file.open(wfilename.data(), std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Unable to open file: " + filename);
    }

    MidiFile midiFile;
    char headerChunk[4];
    if (!readChunk(headerChunk, 4) || std::string(headerChunk, 4) != "MThd")
        throw std::runtime_error("Invalid MIDI file: Missing MThd header");

    uint32_t headerLength;
    if (!readInt32(headerLength) || headerLength != 6)
        throw std::runtime_error("Invalid MIDI header length");

    if (!readInt16(midiFile.format) || !readInt16(midiFile.numTracks) || !readInt16(midiFile.division))
        throw std::runtime_error("Error reading MIDI header fields");

    if (midiFile.division == 0)
        throw std::runtime_error("Invalid MIDI time division: 0");

    if (midiFile.format > 2)
        throw std::runtime_error("Invalid MIDI format");

    if (midiFile.numTracks == 0)
        throw std::runtime_error("Invalid number of tracks");

    for (int i = 0; i < midiFile.numTracks; ++i) {
        char trackChunk[4];
        if (!readChunk(trackChunk, 4) || std::string(trackChunk, 4) != "MTrk")
            throw std::runtime_error("Invalid MIDI file: Missing MTrk header for track " + std::to_string(i));

        uint32_t trackLength;
        if (!readInt32(trackLength))
            throw std::runtime_error("Error reading track length for track " + std::to_string(i));

        // Reject a chunk length larger than the rest of the file before allocating.
        const auto here = file.tellg();
        file.seekg(0, std::ios::end);
        const auto left = file.tellg() - here;
        file.seekg(here);
        if (left < 0 || static_cast<uint64_t>(left) < trackLength)
            throw std::runtime_error("Track " + std::to_string(i) + " is longer than the file");

        std::vector<char> trackData(trackLength);
        if (!file.read(trackData.data(), trackLength))
            throw std::runtime_error("Error reading track data for track " + std::to_string(i));

        const char* ptr = trackData.data();
        if (trackLength > std::numeric_limits<size_t>::max() - reinterpret_cast<size_t>(ptr))
            throw std::runtime_error("Track length causes pointer arithmetic overflow");
        const char* trackEnd = ptr + trackLength;
        MidiTrack track;
        uint32_t absoluteTick = 0;
        uint8_t lastStatus = 0;
        track.events.reserve(1000);

        while (ptr < trackEnd) {
            uint32_t deltaTime = 0;
            readVarLenFromBuffer(ptr, trackEnd, deltaTime);
            if (UINT32_MAX - absoluteTick < deltaTime)
                throw std::runtime_error("Absolute tick counter overflow");
            absoluteTick += deltaTime;

            uint8_t status = readByte(ptr, trackEnd);
            // Running status: a data byte (< 0x80) reuses the previous status.
            // Only channel messages set it. Some files continue a run of notes
            // after a meta or SysEx event, and 0xFF as the running status would
            // read the next note as a meta event.
            if (status < 0x80) {
                if (lastStatus == 0)
                    throw std::runtime_error("Running status encountered with no previous status");
                status = lastStatus;
                ptr--;
            }
            else if (status < 0xF0) {
                lastStatus = status;
            }

            MidiEvent event;
            event.absoluteTick = absoluteTick;
            event.status = status;

            // Channel voice messages with two data bytes
            if ((status & 0xF0) == 0x80 || (status & 0xF0) == 0x90 ||
                (status & 0xF0) == 0xA0 || (status & 0xF0) == 0xB0 ||
                (status & 0xF0) == 0xE0) {
                if (static_cast<size_t>(trackEnd - ptr) < 2)
                    throw std::runtime_error("Unexpected end of track data reading channel event");
                event.data1 = readByte(ptr, trackEnd);
                event.data2 = readByte(ptr, trackEnd);
                event.data1 = std::min(event.data1, static_cast<uint8_t>(127));
                event.data2 = std::min(event.data2, static_cast<uint8_t>(127));
                track.events.push_back(std::move(event));
            }
            // Channel voice messages with one data byte
            else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
                if (static_cast<size_t>(trackEnd - ptr) < 1)
                    throw std::runtime_error("Unexpected end of track data reading channel event (1 data byte)");
                event.data1 = readByte(ptr, trackEnd);
                event.data2 = 0;
                event.data1 = std::min(event.data1, static_cast<uint8_t>(127));
                track.events.push_back(std::move(event));
            }
            // System Exclusive events (F0 and F7)
            else if (status == 0xF0 || status == 0xF7) {
                uint32_t length = 0;
                readVarLenFromBuffer(ptr, trackEnd, length);
                if (length > MAX_EVENT_LENGTH)
                    throw std::runtime_error("SysEx event length exceeds maximum allowed value");
                if (static_cast<size_t>(trackEnd - ptr) < length)
                    throw std::runtime_error("SysEx event length exceeds track data");
                event.metaData.resize(length);
                std::copy_n(ptr, length, event.metaData.begin());
                ptr += length;
                track.events.push_back(std::move(event));
            }
            // Meta events (FF)
            else if (status == 0xFF) {
                parseMetaEvent(event, midiFile, absoluteTick, trackEnd, ptr);
                track.events.push_back(std::move(event));
            }
            // System common and realtime events (F1, F2, F3, F6, F8, FA, FB, FC, FE)
            else if (status >= 0xF0) {
                uint8_t dataCount = 0;
                switch (status) {
                case 0xF1: dataCount = 1; break; // MIDI Time Code Quarter Frame
                case 0xF2: dataCount = 2; break; // Song Position Pointer
                case 0xF3: dataCount = 1; break; // Song Select
                case 0xF6: dataCount = 0; break; // Tune Request
                    // Real-time messages (F8, FA, FB, FC, FE) have no data bytes.
                case 0xF8:
                case 0xFA:
                case 0xFB:
                case 0xFC:
                case 0xFE:
                    dataCount = 0;
                    break;
                default:
                    dataCount = 0;
                    break;
                }
                for (uint8_t j = 0; j < dataCount; ++j) {
                    if (ptr >= trackEnd) break;
                    readByte(ptr, trackEnd);
                }
                continue;
            }
            else {
                // Unreachable; skip a byte defensively.
                readByte(ptr, trackEnd);
                continue;
            }
        }
        midiFile.tracks.push_back(std::move(track));
    }
    file.close();
    return midiFile;
}