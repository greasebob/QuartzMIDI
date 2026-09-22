#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace shell {
struct GameWindow {
    uintptr_t id = 0;
    uint32_t process = 0;
    std::string title;
    // Known piano game; sorted first and preselected when AutoVol opens.
    bool game = false;
};

// Known games first, then by title.
inline void OrderGameWindows(std::vector<GameWindow>& windows) {
    std::stable_sort(windows.begin(), windows.end(), [](const GameWindow& a, const GameWindow& b) {
        return a.game != b.game ? a.game : a.title < b.title;
    });
}

// Window operations are abstracted so tests can record injection without
// focusing or sending input to another application.
class AutoVolumeHost {
public:
    virtual ~AutoVolumeHost() = default;
    virtual std::vector<GameWindow> Windows() = 0;
    virtual bool Focus(const GameWindow& window) = 0;
    virtual bool IsForeground(const GameWindow& window) = 0;
};
}
