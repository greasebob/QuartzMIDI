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
// The id a device is listed under now, or empty when the list does not have
// it. WinMM renumbers its ports when one comes or goes, so a WinMM id that
// names its port is found by that name; one without a name is never guessed at.
inline std::wstring ListedId(const std::vector<LiveDevice>& devices, const std::wstring& id) {
    if (id.empty()) return {};
    std::vector<std::wstring> names;
    std::vector<const LiveDevice*> ports;
    for (const auto& device : devices) {
        if (device.id == id) return id;
        if (device.backend == MidiBackend::WinMM) { names.push_back(device.group); ports.push_back(&device); }
    }
    if (id.find(L'|') == std::wstring::npos) return {};
    const int port = ResolveWinMMPort(id, names);
    return port < 0 ? std::wstring() : ports[static_cast<size_t>(port)]->id;
}
// The id a device waiting to come back is listed under now. A Kernel Streaming
// or WinRT id names the USB port, so a device plugged into another port is
// found by its group on the same transport, when one row alone has it there.
inline std::wstring ReturnedId(const std::vector<LiveDevice>& devices, const std::wstring& id, MidiBackend backend,
                               const std::wstring& group) {
    if (auto listed = ListedId(devices, id); !listed.empty() || group.empty() || backend == MidiBackend::WinMM) return listed;
    const LiveDevice* match = nullptr;
    for (const auto& device : devices) {
        if (device.backend != backend || device.group != group) continue;
        if (match) return {};
        match = &device;
    }
    return match ? match->id : std::wstring();
}
// A chosen device is gone when a fresh list no longer has it. A transport that
// lists nothing may have failed to enumerate, so then its device also has to
// be missing from every other transport; group is its row's from the last list.
inline bool DeviceGone(const std::vector<LiveDevice>& devices, const std::wstring& id, MidiBackend backend,
                       const std::wstring& group) {
    if (!ListedId(devices, id).empty()) return false;
    const auto listed = [&](auto match) { return std::any_of(devices.begin(), devices.end(), match); };
    return listed([&](const LiveDevice& device) { return device.backend == backend; }) || group.empty() ||
           !listed([&](const LiveDevice& device) { return device.group == group; });
}
// The chosen device's group for DeviceGone, from the last list that had it. A
// failed listing leaves the device out of the list it rebuilds, so a second
// failed listing in a row still needs the group from before the first.
struct KeptGroup {
    std::wstring id, group;
    const std::wstring& Update(const std::vector<LiveDevice>& devices, const std::wstring& chosen) {
        const auto found = std::find_if(devices.begin(), devices.end(), [&](const LiveDevice& device) { return device.id == chosen; });
        if (found != devices.end()) group = found->group;
        else if (chosen != id) group.clear();
        id = chosen;
        return group;
    }
};
}
