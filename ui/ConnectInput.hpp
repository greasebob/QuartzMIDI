#pragma once
#include <functional>
#include <memory>
#include <string>

namespace shell {
// The native host supplies MIDIConnect; tests can supply an input whose
// activation and release are observable without producing desktop keystrokes.
class ConnectInput {
public:
    virtual ~ConnectInput() = default;
    virtual bool Open(const std::wstring& id) = 0;
    virtual void Activate(bool active) = 0;
    virtual void Close() = 0;
};
using ConnectFactory = std::function<std::unique_ptr<ConnectInput>()>;
}
