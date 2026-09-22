// KernelStreamingInput.cpp: an IMidiInput that opens the KS MIDI capture pin directly.
//
// WinMM and WinRT both sit on Kernel Streaming and add a marshalling hop
// (midiInProc on a system thread, or a WinRT event plus an IBuffer). Opening
// the pin ourselves removes that hop.
//
// A KS MIDI capture pin has no ring buffer: KSMUSICFORMAT events land in the
// buffer each read supplies, so there is no buffer size or count to configure.
//
// ks.h and ksmedia.h are in the Windows SDK's shared/ directory and ksuser.lib
// in its um/x64 lib directory.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "MidiInput.hpp"
#include "MidiStreamSplit.hpp"

#include <windows.h>
// WIN32_LEAN_AND_MEAN leaves out winioctl.h, and ks.h builds its IOCTL codes
// out of CTL_CODE, METHOD_NEITHER and FILE_ANY_ACCESS.
#include <winioctl.h>
#include <setupapi.h>

// The KS headers define their GUIDs only where INITGUID precedes the include.
// This is the only translation unit that uses them.
#define INITGUID
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ksuser.lib")

// From ksuser.dll. Declared here because ksproxy.h pulls in the DirectShow headers.
extern "C" __declspec(dllimport) DWORD __stdcall KsCreatePin(
    HANDLE FilterHandle, PKSPIN_CONNECT Connect, ACCESS_MASK DesiredAccess, PHANDLE ConnectionHandle);

namespace {

// midi_stream::KsMusicHeader mirrors KSMUSICFORMAT so the parser can be tested
// without the KS headers. Fail the build if the two layouts diverge.
static_assert(sizeof(KSMUSICFORMAT) == sizeof(midi_stream::KsMusicHeader),
              "KSMUSICFORMAT and KsMusicHeader must be the same 8 bytes");
static_assert(offsetof(KSMUSICFORMAT, ByteCount) == offsetof(midi_stream::KsMusicHeader, byteCount),
              "ByteCount must sit at the same offset in both");

constexpr wchar_t kKsPrefix[] = L"ks:";

inline uint64_t nowQpc() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

// A KS id is "ks:<pin>|<device interface path>". The path can contain almost
// anything, so the pin index comes first and the '|' is found from the front.
bool ParseKsId(const std::wstring& id, ULONG& pin, std::wstring& path) {
    const size_t prefix = wcslen(kKsPrefix);
    if (id.size() < prefix || id.compare(0, prefix, kKsPrefix) != 0) return false;
    const size_t bar = id.find(L'|', prefix);
    if (bar == std::wstring::npos) return false;
    const std::wstring digits = id.substr(prefix, bar - prefix);
    if (digits.empty()) return false;
    for (wchar_t c : digits) if (c < L'0' || c > L'9') return false;
    pin = static_cast<ULONG>(_wtoi(digits.c_str()));
    path = id.substr(bar + 1);
    return true;
}

std::wstring MakeKsId(ULONG pin, const std::wstring& path) {
    return kKsPrefix + std::to_wstring(pin) + L"|" + path;
}

// Synchronous IOCTL_KS_PROPERTY on a handle opened with FILE_FLAG_OVERLAPPED.
bool KsProperty(HANDLE handle, const GUID& set, ULONG id, ULONG flags,
                void* out, ULONG outBytes, ULONG* returned = nullptr) {
    KSPROPERTY property{};
    property.Set = set;
    property.Id = id;
    property.Flags = flags;
    DWORD bytes = 0;
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) return false;
    BOOL ok = DeviceIoControl(handle, IOCTL_KS_PROPERTY, &property, sizeof(property),
                              out, outBytes, &bytes, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(handle, &overlapped, &bytes, TRUE);
    CloseHandle(overlapped.hEvent);
    if (returned) *returned = bytes;
    return ok != FALSE;
}

// Closes the handle on scope exit.
struct ScopedHandle {
    HANDLE value;
    ~ScopedHandle() { if (value) CloseHandle(value); }
};

// Overlapped pin-property request on a fresh OVERLAPPED. The event must be
// reset first: callers reuse it for a size probe and then a fetch, and a
// manual-reset event left signalled makes GetOverlappedResult return before
// the driver has filled the new buffer.
bool PinProperty(HANDLE filter, KSP_PIN& request, void* out, ULONG outBytes,
                 DWORD& returned, HANDLE event) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    ResetEvent(event);
    returned = 0;
    BOOL ok = DeviceIoControl(filter, IOCTL_KS_PROPERTY, &request, sizeof(request),
                              out, outBytes, &returned, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(filter, &overlapped, &returned, TRUE);
    return ok != FALSE;
}

// A pin carries MIDI if any data range is music/MIDI. SUBTYPE_MIDI_BUS also
// counts: several class drivers advertise it and the payload framing is the same.
bool PinCarriesMidi(HANDLE filter, ULONG pin) {
    KSP_PIN request{};
    request.Property.Set = KSPROPSETID_Pin;
    request.Property.Id = KSPROPERTY_PIN_DATARANGES;
    request.Property.Flags = KSPROPERTY_TYPE_GET;
    request.PinId = pin;

    ScopedHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) return false;

    // The size probe is expected to fail; only the returned size matters.
    DWORD size = 0;
    PinProperty(filter, request, nullptr, 0, size, event.value);
    if (size < sizeof(KSMULTIPLE_ITEM) || size > (1u << 20)) return false;

    std::vector<uint8_t> blob(size);
    DWORD bytes = 0;
    if (!PinProperty(filter, request, blob.data(), size, bytes, event.value)) return false;
    if (bytes < sizeof(KSMULTIPLE_ITEM)) return false;

    const auto* items = reinterpret_cast<const KSMULTIPLE_ITEM*>(blob.data());
    const uint8_t* cursor = blob.data() + sizeof(KSMULTIPLE_ITEM);
    const uint8_t* end = blob.data() + (bytes < items->Size ? bytes : items->Size);
    for (ULONG i = 0; i < items->Count; ++i) {
        if (cursor + sizeof(KSDATARANGE) > end) break;
        const auto* range = reinterpret_cast<const KSDATARANGE*>(cursor);
        if (range->FormatSize < sizeof(KSDATARANGE) || cursor + range->FormatSize > end) break;
        if (IsEqualGUID(range->MajorFormat, KSDATAFORMAT_TYPE_MUSIC) &&
            (IsEqualGUID(range->SubFormat, KSDATAFORMAT_SUBTYPE_MIDI) ||
             IsEqualGUID(range->SubFormat, KSDATAFORMAT_SUBTYPE_MIDI_BUS)))
            return true;
        // Ranges are packed on an 8-byte boundary.
        cursor += (range->FormatSize + 7) & ~7u;
    }
    return false;
}

bool PinIsCapture(HANDLE filter, ULONG pin) {
    KSP_PIN request{};
    request.Property.Set = KSPROPSETID_Pin;
    request.Property.Flags = KSPROPERTY_TYPE_GET;
    request.PinId = pin;

    const auto query = [&](ULONG id, ULONG& value) {
        request.Property.Id = id;
        DWORD bytes = 0;
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        BOOL ok = DeviceIoControl(filter, IOCTL_KS_PROPERTY, &request, sizeof(request),
                                  &value, sizeof(value), &bytes, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(filter, &overlapped, &bytes, TRUE);
        CloseHandle(overlapped.hEvent);
        return ok != FALSE;
    };

    ULONG dataflow = 0, communication = 0;
    if (!query(KSPROPERTY_PIN_DATAFLOW, dataflow)) return false;
    if (!query(KSPROPERTY_PIN_COMMUNICATION, communication)) return false;
    // A capture pin: data flows out of the device, and the pin can be
    // instantiated by a client (sink or both).
    return dataflow == KSPIN_DATAFLOW_OUT &&
           (communication == KSPIN_COMMUNICATION_SINK || communication == KSPIN_COMMUNICATION_BOTH);
}

std::wstring PinName(HANDLE filter, ULONG pin, const std::wstring& fallback) {
    KSP_PIN request{};
    request.Property.Set = KSPROPSETID_Pin;
    request.Property.Id = KSPROPERTY_PIN_NAME;
    request.Property.Flags = KSPROPERTY_TYPE_GET;
    request.PinId = pin;

    ScopedHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) return fallback;

    DWORD size = 0;
    PinProperty(filter, request, nullptr, 0, size, event.value);
    std::wstring name;
    if (size >= sizeof(wchar_t) && size < 4096) {
        // One zeroed wchar_t of slack in case the driver does not terminate the string.
        std::vector<uint8_t> blob(size + sizeof(wchar_t), 0);
        DWORD bytes = 0;
        if (PinProperty(filter, request, blob.data(), size, bytes, event.value))
            name = reinterpret_cast<const wchar_t*>(blob.data());
    }
    return name.empty() ? fallback : name;
}

std::wstring FriendlyName(HDEVINFO set, SP_DEVICE_INTERFACE_DATA& interfaceData) {
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, nullptr, 0, &needed, &info);
    if (needed == 0) return {};
    std::vector<uint8_t> buffer(needed);
    auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, detail, needed, nullptr, &info)) return {};

    wchar_t name[256]{};
    if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, nullptr,
                                          reinterpret_cast<PBYTE>(name), sizeof(name), nullptr) ||
        SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC, nullptr,
                                          reinterpret_cast<PBYTE>(name), sizeof(name), nullptr))
        return name;
    return {};
}

// The device interface path, which is what CreateFile opens.
std::wstring InterfacePath(HDEVINFO set, SP_DEVICE_INTERFACE_DATA& interfaceData) {
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, nullptr, 0, &needed, nullptr);
    if (needed == 0) return {};
    std::vector<uint8_t> buffer(needed);
    auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, detail, needed, nullptr, nullptr)) return {};
    return detail->DevicePath;
}

HANDLE OpenFilter(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
}

class KernelStreamingMidiInput final : public IMidiInput {
public:
    ~KernelStreamingMidiInput() override { close(); }

    MidiBackend backend() const noexcept override { return MidiBackend::KernelStreaming; }

    std::vector<MidiInputDevice> enumerate() override {
        std::vector<MidiInputDevice> out;
        HDEVINFO set = SetupDiGetClassDevsW(&KSCATEGORY_CAPTURE, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return out;

        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        for (DWORD index = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &KSCATEGORY_CAPTURE, index, &interfaceData); ++index) {
            const std::wstring path = InterfacePath(set, interfaceData);
            if (path.empty()) continue;
            const std::wstring friendly = FriendlyName(set, interfaceData);

            HANDLE filter = OpenFilter(path);
            if (filter == INVALID_HANDLE_VALUE) continue;

            ULONG pins = 0;
            if (KsProperty(filter, KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES,
                           KSPROPERTY_TYPE_GET, &pins, sizeof(pins))) {
                for (ULONG pin = 0; pin < pins; ++pin) {
                    if (!PinIsCapture(filter, pin) || !PinCarriesMidi(filter, pin)) continue;
                    const std::wstring shown = PinName(filter, pin, friendly.empty() ? L"MIDI pin" : friendly);
                    out.push_back({MakeKsId(pin, path), shown, MidiBackend::KernelStreaming});
                }
            }
            CloseHandle(filter);
        }
        SetupDiDestroyDeviceInfoList(set);
        return out;
    }

    bool open(const std::wstring& deviceId, MidiInputCallback callback) override {
        close();
        ULONG pin = 0;
        std::wstring path;
        // Open exactly the named pin or fail; never fall back to another one.
        // The other backends follow the same rule.
        if (!ParseKsId(deviceId, pin, path)) return false;

        filter_ = OpenFilter(path);
        if (filter_ == INVALID_HANDLE_VALUE) { filter_ = nullptr; return false; }

        struct Connect {
            KSPIN_CONNECT connect;
            KSDATAFORMAT format;
        } request{};
        request.connect.Interface.Set = KSINTERFACESETID_Standard;
        request.connect.Interface.Id = KSINTERFACE_STANDARD_STREAMING;
        request.connect.Medium.Set = KSMEDIUMSETID_Standard;
        request.connect.Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
        request.connect.PinId = pin;
        request.connect.PinToHandle = nullptr;
        request.connect.Priority.PriorityClass = KSPRIORITY_NORMAL;
        request.connect.Priority.PrioritySubClass = 1;
        request.format.FormatSize = sizeof(KSDATAFORMAT);
        request.format.MajorFormat = KSDATAFORMAT_TYPE_MUSIC;
        request.format.SubFormat = KSDATAFORMAT_SUBTYPE_MIDI;
        request.format.Specifier = KSDATAFORMAT_SPECIFIER_NONE;

        HANDLE pinHandle = nullptr;
        if (KsCreatePin(filter_, &request.connect, GENERIC_READ, &pinHandle) != ERROR_SUCCESS || !pinHandle) {
            closeHandles();
            return false;
        }
        pin_ = pinHandle;

        // Several drivers reject a direct transition to RUN.
        if (!setState(KSSTATE_ACQUIRE) || !setState(KSSTATE_PAUSE) || !setState(KSSTATE_RUN)) {
            closeHandles();
            return false;
        }

        callback_ = std::move(callback);
        openedId_ = deviceId;
        stop_.store(false, std::memory_order_release);
        reader_ = std::thread([this] { readLoop(); });
        return true;
    }

    void close() override {
        if (reader_.joinable()) {
            stop_.store(true, std::memory_order_release);
            // The reader is blocked in GetOverlappedResult, and an idle pin
            // never completes the read, so cancel it.
            if (pin_) CancelIoEx(pin_, nullptr);
            reader_.join();
        }
        if (pin_) { setState(KSSTATE_PAUSE); setState(KSSTATE_STOP); }
        closeHandles();
        callback_ = nullptr;
        openedId_.clear();
    }

    bool isOpen() const noexcept override { return pin_ != nullptr; }
    const std::wstring& openedDeviceId() const noexcept override { return openedId_; }

private:
    void closeHandles() {
        if (pin_) { CloseHandle(pin_); pin_ = nullptr; }
        if (filter_) { CloseHandle(filter_); filter_ = nullptr; }
    }

    bool setState(KSSTATE state) {
        if (!pin_) return false;
        KSPROPERTY property{};
        property.Set = KSPROPSETID_Connection;
        property.Id = KSPROPERTY_CONNECTION_STATE;
        property.Flags = KSPROPERTY_TYPE_SET;
        DWORD bytes = 0;
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        BOOL ok = DeviceIoControl(pin_, IOCTL_KS_PROPERTY, &property, sizeof(property),
                                  &state, sizeof(state), &bytes, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(pin_, &overlapped, &bytes, TRUE);
        CloseHandle(overlapped.hEvent);
        return ok != FALSE;
    }

    void readLoop() {
        // One outstanding read at a time. A MIDI pin does not need a primed
        // queue, and a single read keeps events in order.
        alignas(8) uint8_t buffer[4096];
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) return;
        midi_stream::Splitter splitter;

        while (!stop_.load(std::memory_order_acquire)) {
            KSSTREAM_HEADER header{};
            header.Size = sizeof(header);
            header.PresentationTime.Numerator = 1;
            header.PresentationTime.Denominator = 1;
            header.FrameExtent = sizeof(buffer);
            header.Data = buffer;

            // Zero the buffer before every read. A read writes only the bytes
            // it produces, so a driver that overstates DataUsed would replay
            // the previous read's events. A zeroed event has ByteCount 0,
            // which ends the walk.
            std::memset(buffer, 0, sizeof(buffer));

            OVERLAPPED overlapped{};
            overlapped.hEvent = event;
            ResetEvent(event);
            DWORD bytes = 0;
            BOOL ok = DeviceIoControl(pin_, IOCTL_KS_READ_STREAM, nullptr, 0,
                                      &header, sizeof(header), &bytes, &overlapped);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
                ok = GetOverlappedResult(pin_, &overlapped, &bytes, TRUE);
            if (stop_.load(std::memory_order_acquire)) break;
            if (!ok) {
                // ERROR_OPERATION_ABORTED is close() cancelling the read. Any
                // other error means the device went away: report it and stop.
                const DWORD error = GetLastError();
                if (error != ERROR_OPERATION_ABORTED)
                    std::wcerr << L"Kernel Streaming read failed, live input has stopped. Error "
                               << error << std::endl;
                break;
            }

            // Latency t0, taken before parsing to match the other backends.
            const uint64_t timestamp = nowQpc();
            // DataUsed comes from the driver and bounds the walk, so clamp it
            // to the buffer.
            const ULONG used = (std::min)(header.DataUsed, static_cast<ULONG>(sizeof(buffer)));
            deliver(buffer, used, timestamp, splitter);
        }
        CloseHandle(event);
    }

    // The payload is a run of KSMUSICFORMAT headers, each followed by its bytes
    // padded to a 4-byte boundary. TimeDeltaMs is ignored: events are stamped
    // on arrival, and mixing the driver's clock in would skew the latency
    // figures. The walk lives in midi_stream::FeedKsEvents so it can be unit tested.
    void deliver(const uint8_t* data, ULONG used, uint64_t timestamp, midi_stream::Splitter& splitter) {
        if (!callback_) return;
        midi_stream::FeedKsEvents(splitter, data, used,
            [&](const uint8_t* message, size_t length) {
                if (callback_) callback_(timestamp, message, length);
            });
    }

    HANDLE filter_ = nullptr;
    HANDLE pin_ = nullptr;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    MidiInputCallback callback_;
    std::wstring openedId_;
};

} // namespace

std::unique_ptr<IMidiInput> CreateKernelStreamingInput() {
    return std::make_unique<KernelStreamingMidiInput>();
}

bool KernelStreamingIdentifies(const std::wstring& deviceId) {
    const size_t prefix = wcslen(kKsPrefix);
    return deviceId.size() >= prefix && deviceId.compare(0, prefix, kKsPrefix) == 0;
}
