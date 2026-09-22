#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace shell {
// The config stores, for each of 32 output steps, the maximum input velocity
// that maps to it. The editor works on a smooth curve and emits these thresholds.
struct VelocityPreset {
    std::string name;
    std::array<int, 32> thresholds{};
};

struct VelocityPoint {
    float x = 0;
    float y = 0;
    bool operator==(const VelocityPoint&) const = default;
};

struct VelocityEdit {
    size_t preset = 1;
    float sensitivity = 0;
    float contrast = 0;
    // Empty: the curve is the preset plus sensitivity/contrast. Otherwise the
    // anchors are the curve, for both point editing and free draw.
    std::vector<VelocityPoint> anchors;
    bool operator==(const VelocityEdit&) const = default;
};

inline int VelocityBucket(const std::array<int, 32>& thresholds, int input) {
    int i = 0;
    while (i < 31 && thresholds[i] < input) ++i;
    return i;
}

// Bucket i covers inputs up to thresholds[i], so the curve passes through
// (thresholds[i]/127, i/31) and is linear between those points. Do not
// resample onto a uniform 32-point grid: 127/31 doesn't align with the integer
// thresholds and flattens parts of the curve.
//
// Inputs run 1..127, so leading zero thresholds are unreachable steps; the
// curve starts at the first reachable step instead of rising from the origin.
inline float VelocityCurveAt(const VelocityPreset& preset, float x) {
    const float input = std::clamp(x, 0.f, 1.f) * 127;
    float previousInput = 0, previousOutput = 0;
    for (int i = 0; i < 32; ++i) {
        const float edge = static_cast<float>(preset.thresholds[i]);
        if (edge <= previousInput) {
            // Capped at step 31, so an all-zero table maps every input there.
            if (previousInput == 0) previousOutput = std::min(i + 1, 31) / 31.f;
            continue;
        }
        if (input <= edge) {
            const float span = edge - previousInput;
            const float t = span > 0 ? (input - previousInput) / span : 1.f;
            return previousOutput + (i / 31.f - previousOutput) * t;
        }
        previousInput = edge;
        previousOutput = i / 31.f;
    }
    return previousOutput;
}

inline std::vector<VelocityPoint> VelocityLegalAnchors(std::vector<VelocityPoint> points) {
    points.erase(std::remove_if(points.begin(), points.end(), [](const auto& p) {
        return !std::isfinite(p.x) || !std::isfinite(p.y);
    }), points.end());
    for (auto& p : points) { p.x = std::clamp(p.x, 0.f, 1.f); p.y = std::clamp(p.y, 0.f, 1.f); }
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.x < b.x; });

    // Collapse points at the same x to the last one (a stroke can revisit an input).
    std::vector<VelocityPoint> unique;
    for (const auto& point : points) {
        if (!unique.empty() && std::abs(point.x - unique.back().x) < 1e-5f) unique.back() = point;
        else unique.push_back(point);
    }
    if (unique.empty()) return {{0, 0}, {1, 1}};
    if (unique.front().x > 0) unique.insert(unique.begin(), {0, unique.front().y});
    if (unique.back().x < 1) unique.push_back({1, unique.back().y});

    // Pool-adjacent-violators: the least-squares non-decreasing fit to the anchors.
    struct Block { size_t first, last; float sum; size_t count; };
    std::vector<Block> blocks;
    for (size_t i = 0; i < unique.size(); ++i) {
        blocks.push_back({i, i, unique[i].y, 1});
        while (blocks.size() > 1) {
            const auto& a = blocks[blocks.size() - 2];
            const auto& b = blocks.back();
            if (a.sum / a.count <= b.sum / b.count) break;
            Block joined{a.first, b.last, a.sum + b.sum, a.count + b.count};
            blocks.pop_back(); blocks.back() = joined;
        }
    }
    for (const auto& block : blocks) {
        const float value = std::clamp(block.sum / block.count, 0.f, 1.f);
        for (size_t i = block.first; i <= block.last; ++i) unique[i].y = value;
    }
    return unique;
}

inline float VelocityPchipPrepared(const std::vector<VelocityPoint>& points, float x) {
    if (points.empty()) return 0;
    if (points.size() == 1) return points.front().y;
    x = std::clamp(x, 0.f, 1.f);
    const auto upper = std::upper_bound(points.begin(), points.end(), x,
        [](float value, const VelocityPoint& point) { return value < point.x; });
    const size_t segment = upper == points.begin() ? 0 :
        std::min(static_cast<size_t>(upper - points.begin() - 1), points.size() - 2);
    const size_t n = points.size();
    const auto h = [&](size_t i) { return std::max(points[i + 1].x - points[i].x, 1e-6f); };
    const auto delta = [&](size_t i) { return (points[i + 1].y - points[i].y) / h(i); };
    const auto endpoint = [](float h0, float h1, float d0, float d1) {
        float m = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
        if (m * d0 <= 0) return 0.f;
        if (d0 * d1 < 0 && std::abs(m) > std::abs(3 * d0)) return 3 * d0;
        return m;
    };
    const auto slope = [&](size_t i) {
        if (n == 2) return delta(0);
        if (i == 0) return endpoint(h(0), h(1), delta(0), delta(1));
        if (i + 1 == n) return endpoint(h(n - 2), h(n - 3), delta(n - 2), delta(n - 3));
        const float before = delta(i - 1), after = delta(i);
        if (before <= 0 || after <= 0) return 0.f;
        const float w1 = 2 * h(i) + h(i - 1), w2 = h(i) + 2 * h(i - 1);
        return (w1 + w2) / (w1 / before + w2 / after);
    };
    const float span = h(segment);
    const float t = std::clamp((x - points[segment].x) / span, 0.f, 1.f);
    const float t2 = t * t, t3 = t2 * t;
    return std::clamp((2 * t3 - 3 * t2 + 1) * points[segment].y +
                      (t3 - 2 * t2 + t) * span * slope(segment) +
                      (-2 * t3 + 3 * t2) * points[segment + 1].y +
                      (t3 - t2) * span * slope(segment + 1), 0.f, 1.f);
}

// Fritsch-Carlson PCHIP. Secants with either a sign change or a plateau get a
// zero tangent, and endpoint tangents use the one-sided shape-preserving rule.
// VelocityLegalAnchors sorts and repairs the input, so the result is monotone
// for any supplied anchors.
inline float VelocityPchip(const std::vector<VelocityPoint>& supplied, float x) {
    return VelocityPchipPrepared(VelocityLegalAnchors(supplied), x);
}

inline float VelocityShape(const VelocityPreset& preset, const VelocityEdit& edit, float x) {
    if (!edit.anchors.empty()) return VelocityPchipPrepared(edit.anchors, x);
    const float shifted = std::clamp(x + edit.sensitivity * .0028f, 0.f, 1.f);
    const float y = VelocityCurveAt(preset, shifted);
    return std::clamp(y + (y * y * (3 - 2 * y) - y) * edit.contrast / 100, 0.f, 1.f);
}

inline bool VelocityEdited(const VelocityEdit& edit) {
    return !edit.anchors.empty() || edit.sensitivity != 0 || edit.contrast != 0;
}

namespace velocity_detail {
inline float LineDistance(const VelocityPoint& p, const VelocityPoint& a, const VelocityPoint& b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length2 = dx * dx + dy * dy;
    if (length2 <= 1e-9f) return std::hypot(p.x - a.x, p.y - a.y);
    const float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length2, 0.f, 1.f);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}
inline void SimplifyRange(const std::vector<VelocityPoint>& points, size_t first, size_t last,
                          float tolerance, std::vector<bool>& keep) {
    float distance = 0; size_t farthest = first;
    for (size_t i = first + 1; i < last; ++i) {
        const float candidate = LineDistance(points[i], points[first], points[last]);
        if (candidate > distance) { distance = candidate; farthest = i; }
    }
    if (distance <= tolerance) return;
    keep[farthest] = true;
    SimplifyRange(points, first, farthest, tolerance, keep);
    SimplifyRange(points, farthest, last, tolerance, keep);
}
inline std::vector<VelocityPoint> Simplify(const std::vector<VelocityPoint>& points, float tolerance) {
    if (points.size() <= 2) return points;
    std::vector<bool> keep(points.size()); keep.front() = keep.back() = true;
    SimplifyRange(points, 0, points.size() - 1, tolerance, keep);
    std::vector<VelocityPoint> result;
    for (size_t i = 0; i < points.size(); ++i) if (keep[i]) result.push_back(points[i]);
    return result;
}
inline float SmoothStep(float value) { value = std::clamp(value, 0.f, 1.f); return value * value * (3 - 2 * value); }
} // namespace velocity_detail

inline std::vector<VelocityPoint> VelocityAnchorsFor(const VelocityPreset& preset, const VelocityEdit& edit) {
    if (!edit.anchors.empty()) return VelocityLegalAnchors(edit.anchors);
    std::vector<VelocityPoint> dense;
    for (int i = 0; i <= 127; ++i) dense.push_back({i / 127.f, VelocityShape(preset, edit, i / 127.f)});
    // Greedy fit against the PCHIP result rather than polyline simplification
    // (which leaves plateaus PCHIP renders badly): each pass adds the sample
    // with the largest error until every error is under ~2/3 of an output step.
    std::vector<VelocityPoint> anchors{dense.front(), dense.back()};
    constexpr float tolerance = .02f;
    constexpr size_t most = 12;
    while (anchors.size() < most) {
        float worst = tolerance; size_t at = dense.size();
        for (size_t i = 1; i + 1 < dense.size(); ++i) {
            const float miss = std::abs(VelocityPchipPrepared(anchors, dense[i].x) - dense[i].y);
            if (miss > worst) { worst = miss; at = i; }
        }
        if (at == dense.size()) break;
        anchors.insert(std::upper_bound(anchors.begin(), anchors.end(), dense[at].x,
            [](float x, const VelocityPoint& point) { return x < point.x; }), dense[at]);
    }
    return VelocityLegalAnchors(std::move(anchors));
}

inline size_t VelocityAddAnchor(std::vector<VelocityPoint>& anchors, float x, float y) {
    anchors = VelocityLegalAnchors(std::move(anchors));
    anchors.push_back({std::clamp(x, 0.f, 1.f), std::clamp(y, 0.f, 1.f)});
    anchors = VelocityLegalAnchors(std::move(anchors));
    size_t nearest = 0; float distance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < anchors.size(); ++i) if (std::abs(anchors[i].x - x) < distance) {
        nearest = i; distance = std::abs(anchors[i].x - x);
    }
    return nearest;
}

inline size_t VelocityMoveAnchor(std::vector<VelocityPoint>& anchors, size_t index, VelocityPoint point) {
    anchors = VelocityLegalAnchors(std::move(anchors));
    if (index >= anchors.size()) return anchors.size();
    const bool first = index == 0, last = index + 1 == anchors.size();
    if (first) point.x = 0;
    if (last) point.x = 1;
    point.x = std::clamp(point.x, 0.f, 1.f); point.y = std::clamp(point.y, 0.f, 1.f);
    anchors.erase(anchors.begin() + index);
    anchors.push_back(point);
    std::stable_sort(anchors.begin(), anchors.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
    size_t moved = 0; float distance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < anchors.size(); ++i) {
        const float candidate = std::abs(anchors[i].x - point.x) + std::abs(anchors[i].y - point.y);
        if (candidate < distance) { distance = candidate; moved = i; }
    }
    // Keep the dragged point where it is; push neighbours to stay monotone.
    for (size_t i = 0; i < moved; ++i) anchors[i].y = std::min(anchors[i].y, anchors[moved].y);
    for (size_t i = moved + 1; i < anchors.size(); ++i) anchors[i].y = std::max(anchors[i].y, anchors[moved].y);
    return moved;
}

inline float VelocityStrokeAt(std::vector<VelocityPoint> stroke, float x) {
    std::stable_sort(stroke.begin(), stroke.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
    if (stroke.empty()) return 0;
    if (x <= stroke.front().x) return stroke.front().y;
    if (x >= stroke.back().x) return stroke.back().y;
    const auto upper = std::upper_bound(stroke.begin(), stroke.end(), x,
        [](float value, const VelocityPoint& point) { return value < point.x; });
    const auto& b = *upper; const auto& a = *(upper - 1);
    const float span = std::max(b.x - a.x, 1e-6f);
    return a.y + (b.y - a.y) * (x - a.x) / span;
}

inline std::vector<VelocityPoint> VelocityApplySweep(const std::vector<VelocityPoint>& suppliedBase,
                                                      const std::vector<VelocityPoint>& suppliedStroke) {
    auto base = VelocityLegalAnchors(suppliedBase);
    std::vector<VelocityPoint> stroke;
    for (auto point : suppliedStroke) if (std::isfinite(point.x) && std::isfinite(point.y)) {
        point.x = std::clamp(point.x, 0.f, 1.f); point.y = std::clamp(point.y, 0.f, 1.f); stroke.push_back(point);
    }
    if (stroke.size() < 2) return base;
    const auto [lowIt, highIt] = std::minmax_element(stroke.begin(), stroke.end(),
        [](const auto& a, const auto& b) { return a.x < b.x; });
    const float low = lowIt->x, high = highIt->x;
    if (high - low < 1e-4f) return base;

    constexpr int samples = 65;
    std::vector<VelocityPoint> drawn(samples);
    for (int i = 0; i < samples; ++i) {
        const float x = low + (high - low) * i / (samples - 1.f);
        drawn[i] = {x, VelocityStrokeAt(stroke, x)};
    }
    // Light symmetric smoothing, applied on release only; the UI draws the raw
    // stroke during the gesture.
    for (int pass = 0; pass < 2; ++pass) {
        auto source = drawn;
        for (int i = 2; i + 2 < samples; ++i)
            drawn[i].y = (source[i - 2].y + 2 * source[i - 1].y + 3 * source[i].y +
                          2 * source[i + 1].y + source[i + 2].y) / 9.f;
    }
    const float edge = std::max((high - low) * .12f, .01f);
    const float floor = VelocityPchipPrepared(base, low), ceiling = VelocityPchipPrepared(base, high);
    for (auto& point : drawn) {
        const float blend = velocity_detail::SmoothStep(std::min((point.x - low) / edge, (high - point.x) / edge));
        point.y = std::lerp(VelocityPchipPrepared(base, point.x), point.y, blend);
        point.y = std::clamp(point.y, floor, ceiling);
    }
    // Repair only the replaced interval; pinning the join values keeps the
    // curve outside [low, high] unchanged.
    drawn.front().y = floor; drawn.back().y = ceiling;
    drawn = VelocityLegalAnchors(std::move(drawn));
    drawn.erase(std::remove_if(drawn.begin(), drawn.end(), [&](const auto& point) {
        return point.x < low - 1e-5f || point.x > high + 1e-5f;
    }), drawn.end());
    drawn = velocity_detail::Simplify(drawn, .004f);

    std::vector<VelocityPoint> result;
    const float guard = std::min(.002f, (high - low) / 16);
    for (const auto& point : base) if (point.x < low - guard || point.x > high + guard) result.push_back(point);
    if (low > guard) result.push_back({low - guard, VelocityPchipPrepared(base, low - guard)});
    result.insert(result.end(), drawn.begin(), drawn.end());
    if (high < 1 - guard) result.push_back({high + guard, VelocityPchipPrepared(base, high + guard)});
    return VelocityLegalAnchors(std::move(result));
}

inline std::array<int, 32> VelocityThresholds(const VelocityPreset& preset, const VelocityEdit& edit) {
    if (!VelocityEdited(edit)) return preset.thresholds; // Preserve built-ins exactly.
    std::array<int, 32> result{};
    for (int input = 1; input <= 127; ++input) {
        const int output = std::clamp(static_cast<int>(std::round(VelocityShape(preset, edit, input / 127.f) * 31)), 0, 31);
        for (int bucket = output; bucket < 32; ++bucket) result[bucket] = input;
    }
    result[31] = 127;
    return result;
}

struct VelocityHistory {
    VelocityEdit current;
    std::vector<VelocityEdit> undo;
    std::vector<VelocityEdit> redo;
    void Reset(VelocityEdit edit) { current = std::move(edit); undo.clear(); redo.clear(); }
    bool Commit(VelocityEdit edit) {
        if (edit == current) return false;
        undo.push_back(current); current = std::move(edit); redo.clear(); return true;
    }
    bool Undo() {
        if (undo.empty()) return false;
        redo.push_back(current); current = std::move(undo.back()); undo.pop_back(); return true;
    }
    bool Redo() {
        if (redo.empty()) return false;
        undo.push_back(current); current = std::move(redo.back()); redo.pop_back(); return true;
    }
};

inline std::string VelocityName(const std::vector<VelocityPreset>& presets, const VelocityEdit& edit) {
    if (edit.preset >= presets.size()) return "Linear Fine";
    return presets[edit.preset].name + (VelocityEdited(edit) ? " (edited)" : "");
}
} // namespace shell
