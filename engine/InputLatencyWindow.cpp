#include "InputLatency.hpp"
#include "InputLatencyWindow.hpp"
#include "Theme.hpp"

#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

namespace {
HWND timingWindow = nullptr;
std::unique_ptr<input_latency::Collector> collector;
std::wstring lastReport;
constexpr int SourceCombo = 1, ReportEdit = 2;

void updateReport(HWND window) {
    if (!collector) return;
    input_latency::poll(*collector);
    const auto source = static_cast<input_latency::Source>(
        SendDlgItemMessageW(window, SourceCombo, CB_GETCURSEL, 0, 0));
    const auto summary = collector->summarize(source, input_latency::frequency());
    std::wostringstream report;
    report << std::fixed << std::setprecision(3);
    if (!input_latency::enabled()) {
        report << L"Keyboard hook unavailable (Windows error " << input_latency::hookError() << L").\r\n";
    }
    else if (!summary.notes) {
        report << L"Waiting for notes from this source.\r\n";
    }
    else {
        report << L"Milliseconds: median / p95 / p99\r\n\r\n";
        const auto metric = [&](const wchar_t* label, const input_latency::Percentiles& p) {
            report << label << L": ";
            if (p.count) report << p.p50 << L" / " << p.p95 << L" / " << p.p99;
            else report << L"unavailable";
            report << L"\r\n";
        };
        metric(L"Before first injection", summary.preparationMs);
        metric(L"Time inside injection calls", summary.callsMs);
        metric(source == input_latency::Source::Autoplay ? L"Dispatch to last keyboard hook" :
            L"Callback to last keyboard hook", summary.callbackToHookMs);
        metric(L"Last hook minus final call return (signed)", summary.hookMinusReturnMs);
        report << L"\r\n" << summary.notes << L" note-ons; " << std::setprecision(2)
            << summary.eventsPerNote << L" accepted events per note-on\r\n"
            << summary.accepted << L" / " << summary.requested << L" events accepted; "
            << summary.failures << L" failed or partial calls (last error " << summary.lastError << L")\r\n"
            << summary.incomplete << L" notes without complete hook observations\r\n";
    }
    report << L"\r\nTelemetry records dropped: " << input_latency::dropped()
        << L"; pending messages: " << collector->pending() << L"\r\n\r\n"
        << L"Window: last 512 MIDI messages per source; timings use note-ons only.\r\n"
        << L"The hook can run before the injection call returns.\r\n"
        << L"Game, audio, hardware and transport delay are not measured.";
    const auto text = report.str();
    if (text != lastReport) {
        SetDlgItemTextW(window, ReportEdit, text.c_str());
        lastReport = text;
    }
}

LRESULT CALLBACK timingProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const auto scale = [](int value) { return Theme::S(value); };
    switch (message) {
    case WM_CREATE: {
        collector = std::make_unique<input_latency::Collector>();
        lastReport.clear();
        HWND combo = CreateWindowW(L"combobox", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            scale(12), scale(12), scale(220), scale(160), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(SourceCombo)), nullptr, nullptr);
        for (const auto* label : {L"Live keys", L"MIDIConnect", L"Autoplay"})
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        HWND report = CreateWindowExW(WS_EX_CLIENTEDGE, L"edit", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            scale(12), scale(48), scale(646), scale(354), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ReportEdit)), nullptr, nullptr);
        SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::UI()), TRUE);
        SendMessageW(report, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::UI()), TRUE);
        input_latency::start();
        updateReport(window);
        SetTimer(window, 1, 250, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == SourceCombo && HIWORD(wParam) == CBN_SELCHANGE) updateReport(window);
        return 0;
    case WM_TIMER:
        updateReport(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        input_latency::stop();
        collector.reset();
        timingWindow = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

void ShowInputLatencyWindow(HWND owner) {
    if (timingWindow) { ShowWindow(timingWindow, SW_RESTORE); SetForegroundWindow(timingWindow); return; }
    WNDCLASSW wc{};
    wc.lpfnWndProc = timingProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"QuartzMIDI Input Timing";
    RegisterClassW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, Theme::S(670), Theme::S(414)};
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_TOOLWINDOW);
    timingWindow = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Keyboard timing", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        owner, nullptr, wc.hInstance, nullptr);
    ShowWindow(timingWindow, SW_SHOWNORMAL);
    UpdateWindow(timingWindow);
}
