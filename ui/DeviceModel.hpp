#pragma once
#include "../engine/MidiInput.hpp"
#include <algorithm>
#include <map>

namespace shell {
struct LiveDevice {
    std::wstring id;
    std::string name;
    std::wstring group;
    MidiBackend backend = MidiBackend::WinRT;
};
struct DeviceGroup {
    std::wstring key;
    std::string name;
    std::vector<LiveDevice> inputs;
    bool ambiguous = false;
};
inline std::vector<DeviceGroup> GroupDevices(const std::vector<LiveDevice>& devices) {
    std::map<std::wstring, std::map<MidiBackend, size_t>> counts;
    for (const auto& device : devices) if (!device.group.empty()) ++counts[device.group][device.backend];
    std::vector<DeviceGroup> groups;
    for (const auto& device : devices) {
        const auto& backends = counts[device.group];
        const bool ambiguous = std::any_of(backends.begin(), backends.end(), [](const auto& item) { return item.second > 1; });
        // Several same-named ports on one backend can't be matched across backends,
        // so each keeps its own id-keyed group.
        const auto key = device.group.empty() || ambiguous ? L"id:" + device.id : L"group:" + device.group;
        auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) { return group.key == key; });
        if (found == groups.end()) {
            groups.push_back({key, device.name, {}, ambiguous});
            found = std::prev(groups.end());
        }
        found->inputs.push_back(device);
    }
    return groups;
}
inline const DeviceGroup* SelectedGroup(const std::vector<DeviceGroup>& groups, const std::wstring& selected) {
    for (const auto& group : groups)
        for (const auto& input : group.inputs) if (input.id == selected) return &group;
    return nullptr;
}
inline std::wstring PreferredInput(const DeviceGroup& group, const std::wstring& selected) {
    for (const auto& input : group.inputs) if (input.id == selected) return selected;
    for (const auto backend : {MidiBackend::KernelStreaming, MidiBackend::WinRT, MidiBackend::WinMM, MidiBackend::WootingAnalog})
        for (const auto& input : group.inputs) if (input.backend == backend) return input.id;
    return {};
}
}
