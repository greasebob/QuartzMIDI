#pragma once

// Preset shapes for the velocity curve editor, kept free of Win32 so tests can
// call them directly.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace velocity_presets {

inline constexpr int kMaxVelocity = 127;

// Each entry is a threshold: the largest input velocity that maps to that
// output step, found by a loop that advances while table[idx] < target.
//
// Inputs run 1..127, so a threshold of 0, or one equal to its predecessor, is
// a step no input can reach. The generated shape is therefore forced strictly
// increasing from 1, which slightly lifts the bottom of steep curves. The
// built-in tables in PlaybackCore.cpp follow the same rule.
inline std::vector<int> Build(const std::string& preset, int pointCount) {
    std::vector<int> points(static_cast<size_t>(std::max(pointCount, 1)));
    const int last = static_cast<int>(points.size()) - 1;
    for (int i = 0; i <= last; ++i) {
        const double x = last > 0 ? static_cast<double>(i) / last : 1.0;
        double y;
        if (preset == "Logarithmic")      y = std::log(x * 9 + 1) / std::log(10.0);
        else if (preset == "Exponential") y = std::pow(x, 2);
        else if (preset == "S-Curve")     y = 0.5 + 0.5 * std::tanh((x - 0.5) * 5);
        else                              y = x;   // Linear and fallback
        points[static_cast<size_t>(i)] = static_cast<int>(kMaxVelocity * y);
    }

    // Strictly increasing from 1 so every step is reachable, capped at 127.
    for (size_t i = 0; i < points.size(); ++i) {
        const int floorValue = (i == 0) ? 1 : points[i - 1] + 1;
        points[i] = std::clamp(std::max(points[i], floorValue), 1, kMaxVelocity);
    }
    // The top step always covers the rest of the range.
    if (!points.empty()) points.back() = kMaxVelocity;
    return points;
}

} // namespace velocity_presets
