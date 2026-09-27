#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "UpdateCheck.hpp"
#include <windows.h>
#include <winhttp.h>
#include <memory>

namespace shell {
std::string FetchLatestRelease() {
    const auto close = [](HINTERNET handle) { if (handle) WinHttpCloseHandle(handle); };
    using Handle = std::unique_ptr<void, decltype(close)>;
    const Handle session(WinHttpOpen(L"QuartzMIDI", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), close);
    if (!session) return {};
    // Resolving, connecting, sending and each wait for the answer.
    WinHttpSetTimeouts(session.get(), 4000, 4000, 4000, 6000);
    const Handle connection(WinHttpConnect(session.get(), kReleaseHost, INTERNET_DEFAULT_HTTPS_PORT, 0), close);
    if (!connection) return {};
    const Handle request(WinHttpOpenRequest(connection.get(), L"GET", kLatestReleasePath, nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE), close);
    if (!request) return {};
    DWORD cookies = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &cookies, sizeof(cookies));
    if (!WinHttpAddRequestHeaders(request.get(), L"Accept: application/vnd.github+json", static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD) ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        return {};
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
        return {};
    // A release's answer is a few kilobytes; more than a megabyte is not one.
    std::string body;
    for (DWORD available = 0; WinHttpQueryDataAvailable(request.get(), &available) && available > 0;) {
        const size_t at = body.size();
        if (at + available > (1u << 20)) return {};
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), body.data() + at, available, &read)) return {};
        body.resize(at + read);
    }
    return body;
}
}
