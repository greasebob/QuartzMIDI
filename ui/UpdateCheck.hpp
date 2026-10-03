#pragma once
#include "json.hpp"
#include <cctype>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace shell {
// The app's version. tools\make-release.ps1 reads it from here for the zip and
// exe names, so a release bumps it in this one place.
inline constexpr const char* kAppVersion = "1.4.0";

// Once at start, off the UI thread, the app asks GitHub for the public repo's
// latest release and offers it in the status bar when it is newer. Nothing is
// sent but the request, nothing is downloaded, and a failure shows nothing.
inline constexpr const wchar_t* kReleaseHost = L"api.github.com";
inline constexpr const wchar_t* kLatestReleasePath = L"/repos/greasebob/QuartzMIDI/releases/latest";
inline constexpr const char* kReleasePageBase = "https://github.com/greasebob/QuartzMIDI/releases/tag/";

// A release newer than this build: its version as shown ("1.0.4") and its page.
// Empty when there is none, or the check failed.
struct AvailableUpdate {
    std::string version, page;
    bool operator==(const AvailableUpdate&) const = default;
};

// The numbers of a version such as "1.0.4" or "v1.0.4"; empty for anything else,
// a pre-release suffix included, since those are never offered.
inline std::vector<int> VersionNumbers(std::string_view text) {
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
    std::vector<int> numbers;
    int number = -1;
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            number = (number < 0 ? 0 : number) * 10 + (c - '0');
            if (number > 99999) return {};
        } else if (c == '.' && number >= 0) { numbers.push_back(number); number = -1; }
        else return {};
    }
    if (number < 0 || numbers.size() >= 4) return {};
    numbers.push_back(number);
    return numbers;
}

// True when `tag` names a later version than `current`, part by part, a missing part counting as 0.
inline bool NewerVersion(std::string_view tag, std::string_view current) {
    auto theirs = VersionNumbers(tag), ours = VersionNumbers(current);
    if (theirs.empty() || ours.empty()) return false;
    theirs.resize(4); ours.resize(4);
    return theirs > ours;
}

// Reads GitHub's answer for the latest release: its tag_name, when it is a plain
// version newer than `current`. The page is built from the tag rather than taken
// from the answer, so the app only ever opens the repo's own release page.
inline AvailableUpdate ReadLatestRelease(const std::string& body, std::string_view current) {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object()) return {};
    const auto tag = json.value("tag_name", std::string());
    if (json.value("draft", false) || json.value("prerelease", false) || !NewerVersion(tag, current)) return {};
    const bool prefixed = tag[0] == 'v' || tag[0] == 'V';
    return {tag.substr(prefixed ? 1 : 0), kReleasePageBase + tag};
}

// The whole check with the request behind `fetch`, which returns the answer's
// body or nothing; the app passes FetchLatestRelease, the tests a canned answer.
inline AvailableUpdate CheckForUpdate(const std::function<std::string()>& fetch, std::string_view current = kAppVersion) {
    try {
        const auto body = fetch();
        return body.empty() ? AvailableUpdate{} : ReadLatestRelease(body, current);
    } catch (...) { return {}; }
}

// GET kLatestReleasePath over HTTPS with WinHTTP: a User-Agent, no cookies, a few
// seconds' timeouts and no retry. The body when GitHub answers 200, else empty.
std::string FetchLatestRelease();
}
