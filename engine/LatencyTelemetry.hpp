#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace input_latency {

// Multiple callback threads and the hook produce records; one UI/test thread
// consumes them. A producer makes at most eight CAS attempts, then drops only
// telemetry. It never allocates, waits for the consumer, or overwrites a reader.
template<class T, size_t Capacity>
class Ring {
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<size_t>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    struct Slot {
        std::atomic<size_t> sequence{};
        T value{};
    };
    std::array<Slot, Capacity> slots_{};
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) size_t read_ = 0;
    alignas(64) std::atomic<uint64_t> dropped_{0};

public:
    Ring() noexcept {
        for (size_t i = 0; i < Capacity; ++i)
            slots_[i].sequence.store(i, std::memory_order_relaxed);
    }

    bool push(const T& value) noexcept {
        size_t pos = write_.load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            auto& slot = slots_[pos & (Capacity - 1)];
            const auto seq = slot.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<intptr_t>(seq - pos);
            if (difference == 0) {
                if (write_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.value = value;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (difference < 0) {
                break;
            }
            else {
                pos = write_.load(std::memory_order_relaxed);
            }
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool pop(T& value) noexcept {
        auto& slot = slots_[read_ & (Capacity - 1)];
        if (slot.sequence.load(std::memory_order_acquire) != read_ + 1)
            return false;
        value = slot.value;
        slot.sequence.store(read_ + Capacity, std::memory_order_release);
        ++read_;
        return true;
    }

    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
};

enum class Source : uint8_t { LiveKeys, MidiConnect, Autoplay };
enum class Kind : uint8_t { NoteOn, NoteOff, Sustain };
enum class RecordType : uint8_t { Submission, Observation };

struct Submission {
    uint64_t id = 0;
    uint64_t t0 = 0; // backend entry, or autoplay event dispatch
    uint64_t t1 = 0; // immediately before the first injection call
    uint64_t t2 = 0; // immediately after the final injection call
    uint64_t callTicks = 0; // sum of call durations, excluding work between calls
    uint32_t requested = 0;
    uint32_t accepted = 0;
    uint32_t calls = 0;
    uint32_t failures = 0;
    uint32_t error = 0;
    uint32_t untagged = 0;
    Source source = Source::LiveKeys;
    Kind kind = Kind::NoteOn;
};

struct Record {
    RecordType type = RecordType::Submission;
    Submission submission{}; // observations use only id and t2 (hook-entry QPC)
};

struct Sample {
    Submission submission{};
    uint64_t t3 = 0; // last observed event belonging to this MIDI message
    uint32_t observed = 0;
    bool complete = false;
};

struct Percentiles {
    size_t count = 0;
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
};

inline Percentiles percentiles(std::vector<double> values) {
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    const auto at = [&](size_t percent) {
        return values[(values.size() * percent + 99) / 100 - 1];
    };
    return {values.size(), at(50), at(95), at(99)};
}

struct Summary {
    size_t notes = 0;
    size_t incomplete = 0;
    uint64_t requested = 0;
    uint64_t accepted = 0;
    uint64_t failures = 0;
    uint32_t lastError = 0;
    double eventsPerNote = 0;
    Percentiles preparationMs;
    Percentiles callsMs;
    Percentiles callbackToHookMs;
    Percentiles hookMinusReturnMs; // signed, can legitimately be negative
};

// All joining, allocation, expiry and sorting happen on the consumer thread.
// Hook records may precede or follow the submission. Both are joined by id,
// never by a scan code that another note/process could also emit.
class Collector {
    struct Pending {
        Submission submission{};
        uint64_t firstSeen = 0;
        uint64_t lastHook = 0;
        uint32_t observed = 0;
        bool submitted = false;
    };
    static constexpr size_t History = 512;
    static constexpr size_t MaxPending = 4096;
    std::unordered_map<uint64_t, Pending> pending_;
    std::array<std::deque<Sample>, 3> recent_;
    uint64_t orphaned_ = 0;

    void finish(const Pending& pending) {
        if (!pending.submitted) { ++orphaned_; return; }
        const auto& s = pending.submission;
        auto& history = recent_[static_cast<size_t>(s.source)];
        history.push_back({s, pending.lastHook, pending.observed,
            s.accepted > 0 && s.failures == 0 && s.untagged == 0 &&
            pending.observed == s.accepted && pending.lastHook >= s.t0});
        if (history.size() > History) history.pop_front();
    }

public:
    Collector() { pending_.reserve(MaxPending); }

    void ingest(const Record& record, uint64_t now) {
        const uint64_t id = record.submission.id;
        if (!id) return;
        if (pending_.size() >= MaxPending && pending_.find(id) == pending_.end()) {
            auto oldest = std::min_element(pending_.begin(), pending_.end(),
                [](const auto& a, const auto& b) { return a.second.firstSeen < b.second.firstSeen; });
            finish(oldest->second);
            pending_.erase(oldest);
        }
        auto [it, inserted] = pending_.try_emplace(id);
        auto& p = it->second;
        if (inserted) p.firstSeen = now;
        if (record.type == RecordType::Submission) {
            p.submission = record.submission;
            p.submitted = true;
        }
        else {
            p.lastHook = (std::max)(p.lastHook, record.submission.t2);
            ++p.observed;
        }
        if (p.submitted && (p.submission.accepted == 0 || p.observed >= p.submission.accepted)) {
            finish(p);
            pending_.erase(it);
        }
    }

    void expire(uint64_t now, uint64_t timeoutTicks) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (now >= it->second.firstSeen && now - it->second.firstSeen >= timeoutTicks) {
                finish(it->second);
                it = pending_.erase(it);
            }
            else ++it;
        }
    }

    const std::deque<Sample>& samples(Source source) const {
        return recent_[static_cast<size_t>(source)];
    }
    size_t pending() const { return pending_.size(); }
    uint64_t orphaned() const { return orphaned_; }

    Summary summarize(Source source, uint64_t frequency) const {
        Summary result;
        if (!frequency) return result;
        std::vector<double> prep, calls, delivery, residual;
        const double toMs = 1000.0 / static_cast<double>(frequency);
        for (const auto& sample : samples(source)) {
            const auto& s = sample.submission;
            if (s.kind != Kind::NoteOn) continue;
            ++result.notes;
            result.requested += s.requested;
            result.accepted += s.accepted;
            result.failures += s.failures;
            if (s.failures) result.lastError = s.error;
            if (s.calls && s.t1 >= s.t0) {
                prep.push_back(static_cast<double>(s.t1 - s.t0) * toMs);
                calls.push_back(static_cast<double>(s.callTicks) * toMs);
            }
            if (sample.complete) {
                delivery.push_back(static_cast<double>(sample.t3 - s.t0) * toMs);
                residual.push_back((sample.t3 >= s.t2 ? static_cast<double>(sample.t3 - s.t2) :
                    -static_cast<double>(s.t2 - sample.t3)) * toMs);
            }
            else if (s.requested) ++result.incomplete;
        }
        if (result.notes) result.eventsPerNote = static_cast<double>(result.accepted) / result.notes;
        result.preparationMs = percentiles(std::move(prep));
        result.callsMs = percentiles(std::move(calls));
        result.callbackToHookMs = percentiles(std::move(delivery));
        result.hookMinusReturnMs = percentiles(std::move(residual));
        return result;
    }
};

} // namespace input_latency
