#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "LatencyTelemetry.hpp"

namespace input_latency {

// Start/stop/poll belong to one UI or test owner. No hook exists while disabled.
bool start() noexcept;
void stop() noexcept;
bool enabled() noexcept;
DWORD hookError() noexcept;
uint64_t nowQpc() noexcept;
uint64_t frequency() noexcept;
uint64_t dropped() noexcept;
void poll(Collector& collector);
bool isOurTag(ULONG_PTR tag) noexcept;

class Trace {
    friend UINT send(UINT, const INPUT*, int) noexcept;
    Submission submission_{};
    Trace* previous_ = nullptr;
public:
    Trace(Source source, Kind kind, uint64_t t0 = 0) noexcept;
    ~Trace();
    Trace(const Trace&) = delete;
    Trace& operator=(const Trace&) = delete;
};

// Sends through InjectInput, keeping the caller's batch boundaries. Tags a
// local copy so shared precomputed INPUT tables cannot race with the measurement layer.
UINT send(UINT count, const INPUT* inputs, int cbSize = sizeof(INPUT)) noexcept;

} // namespace input_latency

