#pragma once
#include <string>
#include <vector>
#include <array>
#include <set>
#include <map>
#include <algorithm>
#include <cstdlib>
#include "midi_parser.h"

// The Transpose, from -12 to 12, that puts the most of `notes` on the keys,
// which is every note when any shift can. Among equals it keeps the shift
// nearest `from`, then nearest 0, then the higher. onKeys(n) says whether
// note n has a key.
template <class OnKeys> int FitToKeys(const std::vector<int>& notes, int from, OnKeys onKeys) {
    from = std::clamp(from, -12, 12);
    std::vector<int> shifts;
    for (int shift = -12; shift <= 12; ++shift) shifts.push_back(shift);
    std::stable_sort(shifts.begin(), shifts.end(), [&](int a, int b) {
        if (std::abs(a - from) != std::abs(b - from)) return std::abs(a - from) < std::abs(b - from);
        if (std::abs(a) != std::abs(b)) return std::abs(a) < std::abs(b);
        return a > b;
    });
    int best = from;
    size_t bestCount = 0;
    bool counted = false;
    for (const int shift : shifts) {
        size_t count = 0;
        for (const int note : notes) count += onKeys(note + shift) ? 1 : 0;
        if (!counted || count > bestCount) { best = shift; bestCount = count; counted = true; }
    }
    return best;
}

class TransposeEngine {
public:
    [[nodiscard]] std::string estimateKey(const std::vector<int>& notes,
        const std::vector<double>& durations) const;
    [[nodiscard]] std::string detectGenre(const MidiFile& midiFile) const;
    [[nodiscard]] int findBestTranspose(const std::vector<int>& notes,
        const std::vector<double>& durations,
        const std::string& detectedKey,
        const std::string& genre) const;

    [[nodiscard]] std::pair<std::vector<int>, std::vector<double>>
        extractNotesAndDurations(const MidiFile& midiFile) const;

private:
    static constexpr std::array<const char*, 12> NOTE_NAMES = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    static constexpr std::array<int, 12> KEY_COMPLEXITY = { 0, 5, 2, 7, 4, 1, 6, 3, 8, 5, 2, 7 };

    const std::vector<int> commonClassicalKeys = { 0, 2, 4, 5, 7, 9, 11 };
    const std::vector<int> commonJazzKeys = { 0, 5, 7, 10, 3 };
    const std::vector<int> commonPopKeys = { 0, 5, 7, 2, 9 };
    const std::vector<int> commonContemporaryKeys = { 0, 2, 4, 5, 7, 9, 11 };

    [[nodiscard]] static constexpr int getPitchClass(int midiNote) noexcept {
        return midiNote % 12;
    }
    [[nodiscard]] std::vector<double> computePitchClassDistribution(
        const std::vector<int>& notes,
        const std::vector<double>& durations) const;
    void adjustKeyEstimate(const std::vector<double>& pitchDistribution,
        int& bestTonic,
        std::string& bestMode) const;

    [[nodiscard]] double calculateRhythmComplexity(const std::vector<double>& durations) const;
    [[nodiscard]] double calculateNoteDistributionEntropy(const std::vector<int>& notes,
        int transpose) const;
    [[nodiscard]] double calculateIntervalComplexity(const std::vector<int>& notes,
        int transpose) const;
    [[nodiscard]] double calculateVoiceLeadingSmoothness(const std::vector<int>& notes,
        int transpose) const;
    [[nodiscard]] double calculateHarmonicSmoothness(const std::vector<int>& notes,
        int transpose) const;
    [[nodiscard]] double calculateMicrotonalPenalty(const std::vector<int>& notes,
        int transpose) const;
    [[nodiscard]] double calculateGenreSpecificTransposeScore(int newKeyIndex,
        int keySignatureComplexity,
        const std::string& genre,
        int transpose) const;
    [[nodiscard]] double calculateCenterAlignmentScore(int minNote, int maxNote) const;

    [[nodiscard]] double calculateAdaptiveTuningAdjustment(const std::vector<int>& notes,
        int transpose) const;
    [[nodiscard]] double calculatePsychoacousticBalance(const std::vector<int>& notes,
        int transpose) const;

    struct TransposeScore {
        double baseScore = 0.0;
        double entropy = 0.0;
        double intervals = 0.0;
        double voiceLeading = 0.0;
        double harmonicSmoothness = 0.0;
        double microtonal = 0.0;
        double adaptiveTuning = 0.0;
        double psychoacoustic = 0.0;
        double genre = 0.0;
        double center = 0.0;
        double total = 0.0;
    };

    [[nodiscard]] TransposeScore evaluateTranspose(const std::vector<int>& notes,
        int transpose,
        const std::string& genre) const;
};
