#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "LibraryModel.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <vector>

namespace shell {
// A monitor as placement sees it: its whole area, its work area (without the
// taskbar) and its DPI scale.
struct Screen { RECT area{}; RECT work{}; float dpi = 1.f; };

// The screen holding most of `rect`, else the nearest, as MonitorFromRect with
// MONITOR_DEFAULTTONEAREST picks. `screens` must not be empty.
inline size_t ScreenOf(const RECT& rect, const std::vector<Screen>& screens) {
    size_t best = 0;
    long long most = -1, nearest = -1;
    for (size_t i = 0; i < screens.size(); ++i) {
        const RECT& area = screens[i].area;
        const long long width = std::min(rect.right, area.right) - std::max(rect.left, area.left);
        const long long height = std::min(rect.bottom, area.bottom) - std::max(rect.top, area.top);
        const long long shared = width > 0 && height > 0 ? width * height : 0;
        const long long dx = std::max({0L, area.left - rect.right, rect.left - area.right});
        const long long dy = std::max({0L, area.top - rect.bottom, rect.top - area.bottom});
        const long long distance = dx * dx + dy * dy;
        if (shared > most || (shared == 0 && most == 0 && distance < nearest)) { best = i; most = shared; nearest = distance; }
    }
    return best;
}

// Fits a window rect into a work area: no larger than it, then moved inside
// it, so neither the title bar nor the status bar ends up off screen or under
// the taskbar.
inline RECT FitToWork(const RECT& target, const RECT& work) {
    const LONG width = std::min(target.right - target.left, work.right - work.left);
    const LONG height = std::min(target.bottom - target.top, work.bottom - work.top);
    const LONG left = std::clamp(target.left, work.left, work.right - width);
    const LONG top = std::clamp(target.top, work.top, work.bottom - height);
    return {left, top, left + width, top + height};
}

// Full and mini are one window in one place: each hangs from the shared
// top-right corner at its own size, onto the screen holding most of it, and is
// fitted onto that screen only as it is shown. `size` is the window's at
// `dpi`; `rect` is at the landing screen's DPI, which WM_DPICHANGED gives the
// window, so SetWindowPos takes its size divided by `landing`.
struct Placement { RECT rect; size_t screen; float landing; };
inline Placement PlaceAtCorner(POINT corner, SIZE size, float dpi, const std::vector<Screen>& screens) {
    const RECT hung{corner.x - size.cx, corner.y, corner.x, corner.y + size.cy};
    const size_t screen = ScreenOf(hung, screens);
    const float landing = screens[screen].dpi / dpi;
    const LONG width = static_cast<LONG>(size.cx * landing), height = static_cast<LONG>(size.cy * landing);
    return {FitToWork({corner.x - width, corner.y, corner.x, corner.y + height}, screens[screen].work), screen, landing};
}

// Where the user left the window: the corner both modes hang from, and the
// full window's size at the DPI it was measured at. Placing a mode never moves
// it, so a mode fitted onto the screen comes back to the same place; only the
// window being moved or sized away from where the app put it does.
struct WindowPlace {
    POINT corner{};
    SIZE full{};
    float fullDpi = 1.f;
    // The window's rect as the app last placed or sized it.
    RECT placed{};
    // Takes the window's rect `now` as the place when it is not where the app
    // put it; true when it was taken.
    bool Follow(const RECT& now, bool fullMode, float dpi) {
        if (Same(now, placed)) return false;
        corner = {now.right, now.top};
        if (fullMode) { full = {now.right - now.left, now.bottom - now.top}; fullDpi = dpi; }
        placed = now;
        return true;
    }
    // The app sized the window in place from `before` to `now`: the corner
    // follows only when the window was at it, not fitted away from it.
    void Resized(const RECT& before, const RECT& now, bool fullMode, float dpi) {
        if (before.right == corner.x && before.top == corner.y) corner = {now.right, now.top};
        if (fullMode) { full = {now.right - now.left, now.bottom - now.top}; fullDpi = dpi; }
        placed = now;
    }
    static bool Same(const RECT& a, const RECT& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
};

// The time between frames on a display refreshing `hz` times a second; a rate
// Windows leaves unreported (0 or 1, the hardware default) counts as 60.
inline std::chrono::microseconds RefreshInterval(unsigned hz) {
    return std::chrono::microseconds(1'000'000 / (hz > 1 ? hz : 60));
}

// While the window is dragged or sized, a step of it draws a frame only once a
// refresh has passed since the last, so a fast mouse draws no more frames than
// the display shows. An eighth of a refresh early still counts, so a timer
// tick of Windows' coarse clock lands on every refresh.
inline bool MoveFrameDue(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point last,
                         std::chrono::microseconds interval) {
    return now - last >= interval - interval / 8;
}

// What a drop of several files on the window does. With songs among them, the
// first song opens and the others join the queue in drop order; folders and
// other files are skipped. Without a song, the first item opens as one dropped
// alone does (a folder, a theme). `directory` tells a folder.
struct DropPlan {
    std::filesystem::path open;
    std::vector<std::filesystem::path> queue;
};
inline DropPlan PlanDrop(const std::vector<std::filesystem::path>& dropped,
                         const std::function<bool(const std::filesystem::path&)>& directory) {
    DropPlan plan;
    for (const auto& path : dropped) {
        if (directory(path) || !LibraryFile(path)) continue;
        if (plan.open.empty()) plan.open = path;
        else plan.queue.push_back(path);
    }
    if (plan.open.empty() && !dropped.empty()) plan.open = dropped.front();
    return plan;
}
}
