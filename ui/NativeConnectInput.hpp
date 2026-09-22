#pragma once
#include "ConnectInput.hpp"
#include "MIDIConnect.hpp"

namespace shell {
class NativeConnectInput final : public ConnectInput {
    MIDIConnect input_;
    bool active_ = false;
public:
    bool Open(const std::wstring& id) override {
        input_.OpenDevice(id);
        return input_.GetSelectedDevice() == id;
    }
    void Activate(bool active) override { input_.SetActive(active); active_ = active; }
    void Close() override {
        input_.SetActive(false);
        input_.CloseDevice(); // Join callbacks before the release sweep.
        if (active_) input_.ReleaseAllNumpadKeys();
        active_ = false;
    }
    ~NativeConnectInput() override { Close(); }
};
}
