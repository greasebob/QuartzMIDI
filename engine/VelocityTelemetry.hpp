#pragma once

// VelocityTelemetry: histogram and last value of played velocities, for the
// velocity curve graph. The input path writes; the UI reads.
//
// Header-only with no project dependencies.
//
// Record live input only. Autoplay velocities come from the curve being
// edited, so recording them would just plot the curve against itself.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace velocity_telemetry {

// Matches the 32 samples of a velocity curve, so each bar is one curve step.
inline constexpr int kBuckets = 32;

struct Snapshot {
    std::array<uint32_t, kBuckets> buckets{};
    uint32_t total = 0;
    // 0 means nothing played since the last reset (velocity 0 is a note off).
    uint8_t last = 0;
    // Incremented on every record and reset; readers can skip redraws when unchanged.
    uint64_t revision = 0;
};

namespace detail {
inline std::array<std::atomic<uint32_t>, kBuckets> buckets{};
inline std::atomic<uint8_t> last{0};
inline std::atomic<uint64_t> revision{0};
} // namespace detail

inline int bucketFor(uint8_t velocity) noexcept { return velocity * kBuckets / 128; }

// Called on the latency-critical MIDI callback thread: no allocation, no locks,
// no blocking. Relaxed atomics plus a release increment of the revision.
inline void record(uint8_t velocity) noexcept {
    if (velocity == 0 || velocity > 127) return;
    detail::buckets[bucketFor(velocity)].fetch_add(1, std::memory_order_relaxed);
    detail::last.store(velocity, std::memory_order_relaxed);
    detail::revision.fetch_add(1, std::memory_order_release);
}

// Records a complete MIDI message if it is a sounding note on. Channel
// filtering is the caller's job.
inline void observe(const uint8_t* data, size_t length) noexcept {
    if (!data || length < 3) return;
    if ((data[0] & 0xF0) != 0x90) return;  // note on only
    if (data[1] > 127 || data[2] > 127) return;
    record(data[2]);                       // record() drops velocity 0 (note off)
}

// Not atomic as a whole: buckets are read one by one, so a snapshot taken
// during playing may be off by a note or two. Acceptable for a histogram, and
// avoids a lock on the callback thread.
inline Snapshot snapshot() noexcept {
    Snapshot result;
    result.revision = detail::revision.load(std::memory_order_acquire);
    result.last = detail::last.load(std::memory_order_relaxed);
    for (int i = 0; i < kBuckets; ++i) {
        result.buckets[i] = detail::buckets[i].load(std::memory_order_relaxed);
        result.total += result.buckets[i];
    }
    return result;
}

// Clears the histogram, e.g. when a new piece starts.
inline void reset() noexcept {
    for (auto& bucket : detail::buckets) bucket.store(0, std::memory_order_relaxed);
    detail::last.store(0, std::memory_order_relaxed);
    detail::revision.fetch_add(1, std::memory_order_release);
}

} // namespace velocity_telemetry
