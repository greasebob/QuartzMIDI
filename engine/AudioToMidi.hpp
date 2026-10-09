#pragma once

// Runs the audio-to-MIDI converter (tools/mp3-to-midi/convert.py) out of process
// and parses its line-based status output. Transkun runs under Python on ONNX
// Runtime with CUDA or DirectML, beside yt-dlp; the resulting .mid is written to the
// MIDI folder and picked up by the normal folder scan.
//
// Each Job runs on its own thread and puts the process tree in a job object, so
// Cancel also kills child processes. Nothing here touches the message loop.
// The YouTube window (browse.py) also runs as a Job, with a stdin pipe the app
// writes its messages to; see Start's input.
//
// Header-only; its one project dependency is the vendored json.hpp.

#include <windows.h>
#include "json.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace audio_to_midi {

// The YouTube window protocol the app speaks: browse.py prints "hello: 2" and
// then qm: lines, and reads JSON lines on stdin. Without --hello it speaks the
// first protocol (pick: lines, no stdin).
inline constexpr int kProtocol = 2;

struct Status {
    // Saved: one playlist item written, more to come. Finished: playlist done.
    // Append new kinds only; the engine passes Kind as an integer.
    enum class Kind { Step, Done, Error, Text, Saved, Finished };
    Kind kind = Kind::Text;
    std::string text;  // UTF-8; for Done and Saved, the path of the .mid
};

inline bool IsFinal(Status::Kind kind) {
    return kind == Status::Kind::Done || kind == Status::Kind::Error || kind == Status::Kind::Finished;
}

inline Status ParseLine(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    const auto take = [&](std::string_view prefix, Status::Kind kind, Status& out) {
        if (line.substr(0, prefix.size()) != prefix) return false;
        out = {kind, std::string(line.substr(prefix.size()))};
        return true;
    };
    Status status;
    if (take("step: ", Status::Kind::Step, status) || take("done: ", Status::Kind::Done, status) ||
        take("error: ", Status::Kind::Error, status) || take("saved: ", Status::Kind::Saved, status) ||
        take("finished: ", Status::Kind::Finished, status))
        return status;
    return {Status::Kind::Text, std::string(line)};
}

// Removes prefix from the front of text, if text starts with it.
inline bool TakePrefix(std::string_view& text, std::string_view prefix) {
    if (!text.starts_with(prefix)) return false;
    text.remove_prefix(prefix.size());
    return true;
}

// A whole decimal number, and nothing else.
inline bool ParseNumber(std::string_view text, int& number) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    return !text.empty() && error == std::errc() && end == text.data() + text.size();
}

// The lines below all arrive as Text from ParseLine, since none starts with a
// status prefix. hello: is browse.py's first line; item:, failed:, exists: and
// blocked: come from convert.py during a list or playlist run.

// N for "hello: N", else 0.
inline int ParseHello(std::string_view line) {
    int protocol = 0;
    return TakePrefix(line, "hello: ") && ParseNumber(line, protocol) && protocol > 0 ? protocol : 0;
}

// "item: n\tT\tID\tTITLE": convert.py starts item n of T. The title is the
// rest of the line, tabs and all.
struct ItemLine {
    int number = 0, total = 0;
    std::string id, title;
};

inline std::optional<ItemLine> ParseItemLine(std::string_view line) {
    if (!TakePrefix(line, "item: ")) return std::nullopt;
    std::string_view fields[3];
    for (auto& field : fields) {
        const size_t tab = line.find('\t');
        if (tab == std::string_view::npos) return std::nullopt;
        field = line.substr(0, tab);
        line.remove_prefix(tab + 1);
    }
    ItemLine item;
    if (!ParseNumber(fields[0], item.number) || !ParseNumber(fields[1], item.total) || item.number < 1 ||
        item.number > item.total || fields[2].empty())
        return std::nullopt;
    item.id = fields[2];
    item.title = line;
    return item;
}

// "failed: ID\tREASON" and "exists: ID\tPATH".
struct IdText {
    std::string id, text;
};

inline std::optional<IdText> ParseIdText(std::string_view line, std::string_view prefix) {
    if (!TakePrefix(line, prefix)) return std::nullopt;
    const size_t tab = line.find('\t');
    if (tab == std::string_view::npos || tab == 0) return std::nullopt;
    return IdText{std::string(line.substr(0, tab)), std::string(line.substr(tab + 1))};
}

inline std::optional<IdText> ParseFailedLine(std::string_view line) { return ParseIdText(line, "failed: "); }
inline std::optional<IdText> ParseExistsLine(std::string_view line) { return ParseIdText(line, "exists: "); }

// "blocked: signin": YouTube refused the run as a bot. convert.py prints it
// just before its error: line.
inline bool IsBlockedLine(std::string_view line) { return line.starts_with("blocked: "); }

// A message from the YouTube window: "qm: " and a JSON object with a string
// "type". Anything else, bad JSON included, is not one.
inline std::optional<nlohmann::json> ParseWindowLine(std::string_view line) {
    if (!TakePrefix(line, "qm: ")) return std::nullopt;
    auto message = nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
    if (!message.is_object()) return std::nullopt;
    const auto type = message.find("type");
    if (type == message.end() || !type->is_string()) return std::nullopt;
    return message;
}

inline bool IsLink(std::wstring_view source) {
    const auto starts = [&](std::wstring_view prefix) {
        if (source.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (towlower(source[i]) != prefix[i]) return false;
        return true;
    };
    return starts(L"http://") || starts(L"https://");
}

// True for a YouTube URL with a list= query parameter (a playlist page, or a
// video opened from a playlist). A Mix (list=RD...) is made up per viewer and
// cannot be read as a playlist, so its link is its video's.
inline bool IsPlaylistLink(std::string_view link) {
    for (size_t at = link.find("list="); at != std::string_view::npos; at = link.find("list=", at + 1))
        if (at > 0 && (link[at - 1] == '?' || link[at - 1] == '&') && link.substr(at + 5, 2) != "RD") return true;
    return false;
}

// True for a YouTube channel link (/@name, /channel/, /c/ or /user/), which
// always converts as a playlist: the piano videos of its Videos tab.
inline bool IsChannelLink(std::string_view link) {
    for (std::string_view scheme : {"https://", "http://"}) {
        if (!link.starts_with(scheme)) continue;
        auto rest = link.substr(scheme.size());
        for (std::string_view sub : {"www.", "m."})
            if (rest.starts_with(sub)) rest.remove_prefix(sub.size());
        if (!rest.starts_with("youtube.com/")) return false;
        rest.remove_prefix(12);
        if (rest.size() > 1 && rest[0] == '@') return true;
        for (std::string_view kind : {"channel/", "c/", "user/"})
            if (rest.size() > kind.size() && rest.starts_with(kind)) return true;
        return false;
    }
    return false;
}

// True for trimmed link-field text that is a file path, which the converter
// takes as well: a network one (\\server) or a drive letter's (C:\ or C:/).
inline bool IsPastedPath(std::string_view trimmed) {
    const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    return trimmed.starts_with("\\\\") ||
           (trimmed.size() > 2 && letter(trimmed[0]) && trimmed[1] == ':' && (trimmed[2] == '\\' || trimmed[2] == '/'));
}

// The link field's text without its outer spaces. A pasted path without the
// quotes Explorer's Copy as path puts around it; other quoted text keeps them.
inline std::string LinkText(std::string_view text) {
    const auto trim = [](std::string_view part) {
        const size_t first = part.find_first_not_of(" \t"), last = part.find_last_not_of(" \t");
        return first == std::string_view::npos ? std::string_view() : part.substr(first, last - first + 1);
    };
    const auto trimmed = trim(text);
    if (trimmed.size() > 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        const auto inner = trim(trimmed.substr(1, trimmed.size() - 2));
        if (IsPastedPath(inner)) return std::string(inner);
    }
    return std::string(trimmed);
}

// The lower-cased host, the path and the query of an http(s) link, without
// its fragment. The host is empty for anything else.
struct LinkParts {
    std::string host;
    std::string_view path, query;
};

inline LinkParts SplitLink(std::string_view link) {
    const auto lower = [](std::string_view text) {
        std::string out(text);
        for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return out;
    };
    LinkParts parts;
    const size_t scheme = link.find("://");
    if (scheme == std::string_view::npos || (lower(link.substr(0, scheme)) != "https" && lower(link.substr(0, scheme)) != "http"))
        return parts;
    auto rest = link.substr(scheme + 3);
    rest = rest.substr(0, rest.find('#'));
    const size_t hostEnd = rest.find_first_of("/?");
    parts.host = lower(rest.substr(0, hostEnd));
    if (hostEnd == std::string_view::npos) return parts;
    rest.remove_prefix(hostEnd);
    const size_t query = rest.find('?');
    parts.path = rest.substr(0, query);
    if (query != std::string_view::npos) parts.query = rest.substr(query + 1);
    return parts;
}

// The value of name= in a query, or empty.
inline std::string_view QueryValue(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const size_t end = query.find('&');
        const auto pair = query.substr(0, end);
        if (pair.size() > name.size() && pair.starts_with(name) && pair[name.size()] == '=') return pair.substr(name.size() + 1);
        if (end == std::string_view::npos) break;
        query.remove_prefix(end + 1);
    }
    return {};
}

// The hosts browse.py's clean() accepts, besides youtu.be.
inline bool IsYouTubeHost(std::string_view host) {
    return host == "youtube.com" || host == "www.youtube.com" || host == "m.youtube.com" || host == "music.youtube.com";
}

inline bool IsVideoId(std::string_view id) {
    return id.size() == 11 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

// A channel id, UC and 22 more: the artist a dest was chosen for.
inline bool IsChannelId(std::string_view id) {
    return id.size() == 24 && id.starts_with("UC") && std::all_of(id.begin() + 2, id.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

// The video of a watch, youtu.be, Shorts, live or YouTube Music link.
inline std::optional<std::string> VideoId(std::string_view link) {
    const auto parts = SplitLink(link);
    const auto segment = [&](std::string_view prefix) -> std::string_view {
        if (!parts.path.starts_with(prefix)) return {};
        const auto rest = parts.path.substr(prefix.size());
        return rest.substr(0, rest.find('/'));
    };
    std::string_view id;
    if (parts.host == "youtu.be") id = segment("/");
    else if (IsYouTubeHost(parts.host)) {
        if (parts.path == "/watch") id = QueryValue(parts.query, "v");
        else if (parts.path.starts_with("/shorts/")) id = segment("/shorts/");
        else if (parts.path.starts_with("/live/")) id = segment("/live/");
    }
    if (!IsVideoId(id)) return std::nullopt;
    return std::string(id);
}

// "@name", "channel/UC...", "c/name" or "user/name" of a link IsChannelLink
// accepts, without a tab such as /videos.
inline std::string_view ChannelPath(std::string_view link) {
    const size_t host = link.find("youtube.com/");
    if (host == std::string_view::npos) return {};
    auto rest = link.substr(host + 12);
    rest = rest.substr(0, rest.find_first_of("?#"));
    const size_t slash = rest.find('/');
    if (rest.starts_with('@') || slash == std::string_view::npos) return rest.substr(0, slash);
    return rest.substr(0, rest.find('/', slash + 1));
}

// A queue entry's key, the same the YouTube window uses for what it sends, so
// one video is queued once however it was picked: v:ID for a video, p:LIST for
// a playlist link with no video, c:ID for a /channel/ link, the key the window
// gives a channel; c:@name, c:c/name or c:user/name for the others, which the
// app cannot resolve to an id; else link:LINK.
inline std::string ItemKey(std::string_view link) {
    if (const auto id = VideoId(link)) return "v:" + *id;
    const auto parts = SplitLink(link);
    const auto list = QueryValue(parts.query, "list");
    if (IsYouTubeHost(parts.host) && !list.empty() && !list.starts_with("RD")) return "p:" + std::string(list);
    if (IsChannelLink(link)) {
        auto path = ChannelPath(link);
        if (path.starts_with("channel/")) path.remove_prefix(8);
        return "c:" + std::string(path);
    }
    return "link:" + std::string(link);
}

// An audio file's key: file: and its path in UTF-8.
inline std::string FileKey(const std::filesystem::path& path) {
    const std::wstring& wide = path.native();
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return "file:" + text;
}

// Quotes one argument per the CommandLineToArgvW / CRT rules: backslashes are
// doubled only when followed by a quote.
inline std::wstring QuoteArgument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        quoted.push_back(c);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

// A dest names a folder in the MIDI folder by up to this many folder names
// joined by "/"; "" names the MIDI folder itself. convert.py's DEST_DEPTH.
inline constexpr size_t kDestDepth = 4;

// The format characters (Unicode category Cf) of Python 3.12, so a name the
// app takes is one convert.py and the page take too.
inline bool IsFormatCharacter(char32_t c) {
    static constexpr std::pair<char32_t, char32_t> kRanges[]{
        {0xAD, 0xAD}, {0x600, 0x605}, {0x61C, 0x61C}, {0x6DD, 0x6DD}, {0x70F, 0x70F}, {0x890, 0x891},
        {0x8E2, 0x8E2}, {0x180E, 0x180E}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064},
        {0x2066, 0x206F}, {0xFEFF, 0xFEFF}, {0xFFF9, 0xFFFB}, {0x110BD, 0x110BD}, {0x110CD, 0x110CD},
        {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0001, 0xE0001}, {0xE0020, 0xE007F}};
    return std::any_of(std::begin(kRanges), std::end(kRanges), [c](const auto& range) { return c >= range.first && c <= range.second; });
}

// Whether a name is a device name, with or without an extension, as convert.py's
// RESERVED has them after upper(). Windows opens the device for such a name.
inline bool IsDeviceName(std::wstring_view name) {
    std::wstring stem(name.substr(0, name.find(L'.')));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    // The only letters upper() makes these names of: ASCII, and the dotless i.
    for (auto& c : stem) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        else if (c == 0x131) c = L'I';
    }
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" || stem == L"CONOUT$")
        return true;
    // COM and LPT with a digit from 1 to 9, or a superscript 1, 2 or 3.
    return stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
           ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == 0xB9 || stem[3] == 0xB2 || stem[3] == 0xB3);
}

// Whether a folder name may be part of a dest, as convert.py's valid_segment
// has it: 1 to 255 UTF-16 units, none of <>:"/\|?* or a control, format or
// lone surrogate character, not . or .., no space first, no space or dot last,
// and not IsDeviceName().
inline bool ValidSegment(std::wstring_view name) {
    if (name.empty() || name.size() > 255 || name == L"." || name == L".." || name.front() == L' ' ||
        name.back() == L' ' || name.back() == L'.')
        return false;
    for (size_t i = 0; i < name.size(); ++i) {
        char32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < name.size() && name[i + 1] >= 0xDC00 && name[i + 1] <= 0xDFFF)
            c = 0x10000 + ((c - 0xD800) << 10) + (name[++i] - 0xDC00);
        else if (c >= 0xD800 && c <= 0xDFFF)
            return false;
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || IsFormatCharacter(c) ||
            (c < 0x80 && std::wstring_view(L"<>:\"/\\|?*").find(static_cast<wchar_t>(c)) != std::wstring_view::npos))
            return false;
    }
    return !IsDeviceName(name);
}

// The folder names of a dest in UTF-8 (none for the MIDI folder itself), or
// nothing when it is not one: too deep, not UTF-8, or a name ValidSegment refuses.
inline std::optional<std::vector<std::wstring>> ParseDest(std::string_view dest) {
    std::vector<std::wstring> names;
    if (dest.empty()) return names;
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dest.data(), static_cast<int>(dest.size()), nullptr, 0);
    if (size <= 0) return std::nullopt;
    std::wstring text(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dest.data(), static_cast<int>(dest.size()), text.data(), size);
    for (size_t start = 0;;) {
        const size_t slash = text.find(L'/', start);
        names.push_back(text.substr(start, slash - start));
        if (names.size() > kDestDepth || !ValidSegment(names.back())) return std::nullopt;
        if (slash == std::wstring::npos) return names;
        start = slash + 1;
    }
}

// Whether a folder with these attributes and reparse tag leads somewhere else:
// a junction, a symlink or another name surrogate. A cloud placeholder
// (OneDrive's, say) is a reparse point that stays where it is.
inline bool Redirects(DWORD attributes, DWORD tag) {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag);
}

// The longest file path Windows takes without the \\?\ prefix, in UTF-16 units:
// MAX_PATH less its closing null. convert.py's LONGEST.
inline constexpr size_t kLongestPath = 259;

// Python's whitespace, which convert.py's SPACES matches.
inline bool IsSpace(wchar_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// name with each run of whitespace as one space, as SPACES.sub(" ", name).
inline std::wstring Spaced(std::wstring_view name) {
    std::wstring spaced;
    bool space = false;
    for (const wchar_t c : name) {
        if (!IsSpace(c)) spaced.push_back(c);
        else if (!space) spaced.push_back(L' ');
        space = IsSpace(c);
    }
    return spaced;
}

// A folder name as a dest matches it, as convert.py's folded has it: Spaced and
// case folded by ICU as Python folds. Without icu.dll (Windows 10 before 1903)
// it is lowercased.
inline std::wstring Folded(std::wstring_view name) {
    using FoldCase = int(__cdecl*)(wchar_t*, int, const wchar_t*, int, unsigned, int*);
    static const FoldCase fold = [] {
        const HMODULE icu = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return icu ? reinterpret_cast<FoldCase>(GetProcAddress(icu, "u_strFoldCase")) : nullptr;
    }();
    const std::wstring spaced = Spaced(name);
    // Full folding makes at most three units of one.
    std::wstring folded(spaced.size() * 3, L'\0');
    int error = 0;
    const int size = fold ? fold(folded.data(), static_cast<int>(folded.size()), spaced.data(), static_cast<int>(spaced.size()), 0, &error)
                          : 0;
    if (!fold || error > 0)
        folded.resize(std::max(0, LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, spaced.data(), static_cast<int>(spaced.size()),
                                                folded.data(), static_cast<int>(folded.size()), nullptr, nullptr, 0)));
    else
        folded.resize(static_cast<size_t>(size));
    return folded;
}

// The folder a new dest name is made as, as convert.py's FolderName makes it of
// a name ValidSegment() takes, which has none of the characters it drops: Spaced,
// no space or dot first or last, at most 80 characters, and "_" after a device name.
inline std::wstring NewFolderName(std::wstring_view name) {
    std::wstring folder = Spaced(name);
    const auto stripped = [](wchar_t c) { return c == L' ' || c == L'.'; };
    folder.erase(folder.begin(), std::find_if_not(folder.begin(), folder.end(), stripped));
    // Python cuts by code point, so a surrogate pair counts once.
    const auto cut = [&] {
        size_t end = 0;
        for (size_t count = 0; end < folder.size() && count < 80; ++count)
            end += IS_HIGH_SURROGATE(folder[end]) && end + 1 < folder.size() && IS_LOW_SURROGATE(folder[end + 1]) ? 2 : 1;
        folder.resize(end);
        while (!folder.empty() && stripped(folder.back())) folder.pop_back();
    };
    cut();
    if (IsDeviceName(folder)) {
        folder.insert(std::min(folder.find(L'.'), folder.size()), 1, L'_');
        cut();
    }
    return folder.empty() ? L"YouTube" : folder;
}

// path as the file functions take it past MAX_PATH, which the app's manifest
// does not lift: absolute, behind \\?\ or, for a share, \\?\UNC\.
inline std::wstring LongPath(const std::filesystem::path& path) {
    std::error_code error;
    const auto full = std::filesystem::absolute(path, error);
    const std::wstring text = (error ? path : full).lexically_normal().native();
    if (text.starts_with(L"\\\\?\\")) return text;
    if (text.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + text.substr(2);
    return L"\\\\?\\" + text;
}

// The entry of folder that name matches, as convert.py's child_named finds it:
// by Folded(), its exact name first and then the lowest; else the one
// NewFolderName(name) matches, which making name would reach. False when folder
// cannot be listed.
inline bool ChildNamed(const std::filesystem::path& folder, const std::wstring& name, std::optional<WIN32_FIND_DATAW>& child) {
    std::wstring pattern = LongPath(folder);
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    std::vector<std::pair<WIN32_FIND_DATAW, std::wstring>> entries;
    WIN32_FIND_DATAW entry{};
    const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        do
            if (std::wstring_view(entry.cFileName) != L"." && std::wstring_view(entry.cFileName) != L"..")
                entries.emplace_back(entry, Folded(entry.cFileName));
        while (FindNextFileW(find, &entry));
        const DWORD error = GetLastError();
        FindClose(find);
        if (error != ERROR_NO_MORE_FILES) return false;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {   // a drive's empty root lists nothing
        return false;
    }
    // Python orders names by code point, which UTF-16 order differs from only
    // where a surrogate meets U+E000 to U+FFFF.
    const auto point = [](wchar_t c) -> unsigned { return c >= 0xE000 ? c - 0x800u : c >= 0xD800 ? c + 0x2000u : c; };
    for (const auto& wanted : {name, NewFolderName(name)}) {
        const auto key = Folded(wanted);
        const auto before = [&](std::wstring_view a, std::wstring_view b) {
            if ((a == wanted) != (b == wanted)) return a == wanted;
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [&](wchar_t x, wchar_t y) { return point(x) < point(y); });
        };
        const WIN32_FIND_DATAW* best = nullptr;
        for (const auto& [found, folded] : entries)
            if (folded == key && (!best || before(found.cFileName, best->cFileName))) best = &found;
        if (best) {
            child = *best;
            return true;
        }
    }
    child.reset();
    return true;
}

// The folder a dest's names lead to in root, as convert.py's resolve_dest finds
// it, or nothing when it would leave root: a name that matches a file or a
// folder that Redirects(), or a real path not inside root's. A matched folder
// keeps its own name and a new one is NewFolderName()'s; nothing is made. Every
// lookup goes through LongPath(), so a path past MAX_PATH is checked too.
inline std::optional<std::filesystem::path> SafeDestination(const std::filesystem::path& root, const std::vector<std::wstring>& names) {
    if (names.size() > kDestDepth || !std::all_of(names.begin(), names.end(), [](const std::wstring& name) { return ValidSegment(name); }))
        return std::nullopt;
    std::filesystem::path folder = root, existing = root;
    bool found = true;
    for (const auto& name : names) {
        std::optional<WIN32_FIND_DATAW> child;
        if (found && !ChildNamed(folder, name, child)) return std::nullopt;
        // dwReserved0 holds the reparse tag when the attribute is set.
        if (child && (!(child->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Redirects(child->dwFileAttributes, child->dwReserved0)))
            return std::nullopt;
        found = child.has_value();
        folder /= found ? std::wstring(child->cFileName) : NewFolderName(name);
        if (found) existing = folder;
    }
    std::error_code error;
    const auto real = std::filesystem::canonical(LongPath(existing), error);
    if (error) return std::nullopt;
    const auto top = std::filesystem::canonical(LongPath(root), error);
    if (error) return std::nullopt;
    auto part = real.begin();
    for (const auto& want : top) {
        if (part == real.end() || CompareStringOrdinal(part->c_str(), -1, want.c_str(), -1, TRUE) != CSTR_EQUAL) return std::nullopt;
        ++part;
    }
    return folder;
}

inline std::wstring CommandLine(const std::filesystem::path& python, const std::filesystem::path& script,
                                std::wstring_view source, const std::filesystem::path& outputFolder,
                                bool playlist = false, int cpuPercent = 100, const std::filesystem::path& listFile = {},
                                bool again = false, const std::wstring& subfolder = {},
                                const std::optional<std::wstring>& dest = std::nullopt, std::wstring_view artist = {}) {
    // --cpu is passed only below 100; convert.py uses it to size the thread
    // pool and lowers the process priority. A list file (the videos picked in
    // the YouTube window) takes the source's place. --again converts what the
    // ledger says is done; --subfolder=NAME saves a single link into that
    // folder, as one argument so a name starting with '-' is not an option.
    // --dest=A/B saves into a dest instead (--dest= into the MIDI folder
    // itself), and --artist=UC... is the channel it was chosen for.
    return QuoteArgument(python.native()) + L" -u " + QuoteArgument(script.native()) +
           (listFile.empty() ? L" " + QuoteArgument(source) : L" --list " + QuoteArgument(listFile.native())) +
           L" --out-dir " + QuoteArgument(outputFolder.native()) +
           (playlist ? L" --playlist" : L"") +
           (cpuPercent < 100 ? L" --cpu " + std::to_wstring(std::max(1, cpuPercent)) : L"") +
           (again ? L" --again" : L"") +
           (dest ? L" " + QuoteArgument(L"--dest=" + *dest)
                 : subfolder.empty() ? L"" : L" " + QuoteArgument(L"--subfolder=" + subfolder)) +
           (artist.empty() ? L"" : L" " + QuoteArgument(L"--artist=" + std::wstring(artist)));
}

// Converter location. Python comes from MIDIPP_CONVERTER_PYTHON or a bundled
// interpreter beside convert.py. Fields are empty when not found; the app treats
// that as "not installed".
struct Install {
    std::filesystem::path python;
    std::filesystem::path script;
    std::filesystem::path signin;  // signin.py beside convert.py, if present
    std::filesystem::path browse;  // browse.py, the YouTube window, if present
    // setup.ps1 beside convert.py; downloads Python, the packages, the model,
    // FFmpeg and Deno.
    std::filesystem::path setup;
    // Why an add-on that is there is not Found(), for the log: every reason, in
    // the order FindInstall checks them. Empty when Found() or no add-on is there.
    std::vector<std::string> unready;
    // The ONNX Runtime engine setup.ps1 installed, "cuda13", "cuda12" or
    // "directml", and the ones this PC's GPU can run, best first, from its
    // engine.json. A converter set up before engine.json runs DirectML, and
    // could run CUDA when the NVIDIA driver is installed.
    std::string engine;
    std::vector<std::string> engines;
    bool Found() const { return !python.empty() && !script.empty(); }
    bool CanSetUp() const { return !Found() && !setup.empty(); }
    // signin.py and the YouTube window write the YouTube session to cookies.txt
    // in the Netscape format; convert.py reads it. Signed in means a YouTube
    // session cookie there that has not expired, so a file left by an expired
    // session does not count.
    bool SignedIn() const {
        if (script.empty()) return false;
        std::ifstream file(script.parent_path() / L"cookies.txt", std::ios::binary);
        const long long now = static_cast<long long>(std::time(nullptr));
        for (std::string text; std::getline(file, text);) {
            if (!text.empty() && text.back() == '\r') text.pop_back();
            std::string_view line = text;
            // Other lines starting with # are comments.
            if (!TakePrefix(line, "#HttpOnly_") && line.starts_with('#')) continue;
            // Domain, flag, path, secure, expiry and name; the value is the rest.
            std::string_view fields[6];
            size_t count = 0;
            for (; count < 6; ++count) {
                const size_t tab = line.find('\t');
                if (tab == std::string_view::npos) break;
                fields[count] = line.substr(0, tab);
                line.remove_prefix(tab + 1);
            }
            const auto name = fields[5];
            if (count < 6 || !fields[0].ends_with("youtube.com") ||
                (name != "LOGIN_INFO" && name != "SID" && name != "__Secure-3PSID"))
                continue;
            long long expiry = 0;
            const auto [end, error] = std::from_chars(fields[4].data(), fields[4].data() + fields[4].size(), expiry);
            if (!fields[4].empty() && error == std::errc() && end == fields[4].data() + fields[4].size() &&
                (expiry == 0 || expiry > now))
                return true;
        }
        return false;
    }
};

// Runs signin.py, which takes no arguments.
inline std::wstring SignInCommandLine(const std::filesystem::path& python, const std::filesystem::path& script) {
    return QuoteArgument(python.native()) + L" -u " + QuoteArgument(script.native());
}

// Runs browse.py in the current protocol. A browse.py from before it ignores
// the argument and speaks the first.
inline std::wstring BrowseCommandLine(const std::filesystem::path& python, const std::filesystem::path& browse) {
    return QuoteArgument(python.native()) + L" -u " + QuoteArgument(browse.native()) + L" --hello " +
           std::to_wstring(kProtocol);
}

// The engines setup.ps1 installs, best first, and their names.
inline constexpr std::array<std::string_view, 3> kEngines{"cuda13", "cuda12", "directml"};
inline std::string EngineName(std::string_view engine) {
    return engine == "cuda13" ? "CUDA 13" : engine == "cuda12" ? "CUDA 12" : engine == "directml" ? "DirectML" : "";
}

// Runs setup.ps1 under the System32 PowerShell with -ExecutionPolicy Bypass.
// It emits the same step:/done:/error: lines as convert.py. Without an engine
// it picks the best one this PC runs.
inline std::wstring SetupCommandLine(const std::filesystem::path& setup, std::string_view engine = {}) {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const auto shell = std::filesystem::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    auto line = QuoteArgument(shell.native()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
                QuoteArgument(setup.native());
    if (std::find(kEngines.begin(), kEngines.end(), engine) != kEngines.end())
        line += L" -Engine " + std::wstring(engine.begin(), engine.end());
    return line;
}

// The percentage at the end of a step line ("Transcribing on the GPU, 40%"),
// or -1 when it has none.
inline int StepPercent(std::string_view step) {
    if (step.empty() || step.back() != '%') return -1;
    size_t digits = step.size() - 1;
    while (digits > 0 && step[digits - 1] >= '0' && step[digits - 1] <= '9') --digits;
    const auto number = step.substr(digits, step.size() - 1 - digits);
    if (number.empty() || number.size() > 3 || digits < 2 || step.substr(digits - 2, 2) != ", ") return -1;
    int value = 0;
    for (const char c : number) value = value * 10 + (c - '0');
    return value <= 100 ? value : -1;
}

// What a step line is doing, for the YouTube window: "reading" a playlist or
// channel, "download", "transcribe", or "" for anything else. A list item's
// step starts "n of T, title: ", so only the text after the last ": " is read,
// and a title such as "Reading Rainbow" does not count.
inline std::string PhaseOf(std::string_view step) {
    if (const size_t colon = step.rfind(": "); colon != std::string_view::npos) step.remove_prefix(colon + 2);
    const auto has = [&](std::string_view word) { return step.find(word) != std::string_view::npos; };
    if (has("Reading")) return "reading";
    if (has("Downloading") || has("Trying again")) return "download";
    if (has("Transcribing")) return "transcribe";
    return "";
}

// The skin the YouTube window themes itself with: the keys of ThemeColours
// and ThemeNumbers in ui/ThemeModel.hpp, which the tests hold equal to these.
inline constexpr std::array<std::string_view, 24> kSkinColours{
    "canvas", "structure", "card", "elevated", "elevatedHot", "recessed", "inkPrimary", "inkSecondary",
    "inkTertiary", "accent", "ok", "okInk", "warn", "bad", "accentSoft", "accentLine",
    "okSoft", "okBorder", "hairline", "strong", "topHighlight", "contact", "ambient", "inner"};
inline constexpr std::array<std::string_view, 18> kSkinShape{
    "scale", "controlHeight", "textBody", "textMeta", "textHeading", "textTitle", "radiusWindow", "radiusCard", "radiusControl",
    "radiusElement", "windowPad", "panelPad", "contactOffset", "contactBlur", "ambientOffset", "ambientBlur", "innerOffset", "innerBlur"};

// The skin object of the window's init and theme messages:
// {"colours": {key: "#RRGGBB" or "#RRGGBBAA"}, "dark": bool, "shape": {key: number}}.
// Only the keys above are written; a colour that is not that text, or a
// measure that is not a finite number, is left out.
inline std::string ThemeMessage(const std::vector<std::pair<std::string, std::string>>& colours,
                                const std::vector<std::pair<std::string, float>>& shape, bool dark) {
    const auto named = [](const auto& keys, std::string_view key) { return std::find(keys.begin(), keys.end(), key) != keys.end(); };
    const auto colourText = [](std::string_view text) {
        return (text.size() == 7 || text.size() == 9) && text[0] == '#' &&
               std::all_of(text.begin() + 1, text.end(), [](char c) { return isxdigit(static_cast<unsigned char>(c)) != 0; });
    };
    nlohmann::json skin = nlohmann::json::object();
    skin["dark"] = dark;
    skin["colours"] = nlohmann::json::object();
    skin["shape"] = nlohmann::json::object();
    for (const auto& [key, text] : colours)
        if (named(kSkinColours, key) && colourText(text)) skin["colours"][key] = text;
    // Rounded to thousandths, so a float's 0.85 is written 0.85 and not
    // 0.8500000238418579.
    for (const auto& [key, value] : shape)
        if (named(kSkinShape, key) && std::isfinite(value)) skin["shape"][key] = std::round(value * 1000.0) / 1000.0;
    return skin.dump();
}

inline Install FindInstall(const std::filesystem::path& exeFolder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    Install found;
    // Search addons\converter, then the legacy converter\ folder. The source
    // tree's tools folder is used only when the exe sits under build\, so an
    // installed copy never runs scripts from an unrelated parent directory.
    // The tests set MIDIPP_NO_SOURCE_CONVERTER so a set-up tools folder is not found.
    const bool built = exeFolder.parent_path().filename() == L"build" && !GetEnvironmentVariableW(L"MIDIPP_NO_SOURCE_CONVERTER", nullptr, 0);
    for (const auto& folder : {exeFolder / L"addons" / L"converter", exeFolder / L"converter",
                               built ? exeFolder.parent_path().parent_path() / L"tools" / L"mp3-to-midi" : fs::path()})
        if (!folder.empty() && fs::is_regular_file(folder / L"convert.py", ec)) {
            found.script = folder / L"convert.py";
            if (fs::is_regular_file(folder / L"signin.py", ec)) found.signin = folder / L"signin.py";
            if (fs::is_regular_file(folder / L"browse.py", ec)) found.browse = folder / L"browse.py";
            if (fs::is_regular_file(folder / L"setup.ps1", ec)) found.setup = folder / L"setup.ps1";
            break;
        }
    if (const DWORD size = GetEnvironmentVariableW(L"MIDIPP_CONVERTER_PYTHON", nullptr, 0)) {
        std::wstring value(size, L'\0');
        value.resize(GetEnvironmentVariableW(L"MIDIPP_CONVERTER_PYTHON", value.data(), size));
        if (fs::is_regular_file(value, ec)) found.python = value;
    }
    if (found.script.empty()) return found;
    const auto folder = found.script.parent_path();
    // setup.ps1 creates setup.partial while it runs; treat that install as
    // incomplete. So is one with no model, which a converter from before ONNX
    // Runtime has, so Install sets it up again.
    const bool unfinished = fs::exists(folder / L"setup.partial", ec);
    const bool noModel = !fs::is_regular_file(folder / L"model" / L"scorer.onnx", ec);
    // Bundled or setup.ps1-created interpreter beside convert.py, looked for
    // even in an unfinished install so the log can say whether it is there.
    if (found.python.empty())
        for (const auto& candidate : {folder / L"python" / L"python.exe", folder / L".venv" / L"Scripts" / L"python.exe"})
            if (fs::is_regular_file(candidate, ec)) { found.python = candidate; break; }
    if (unfinished) found.unready.push_back("setup.ps1 has not finished, since setup.partial is there");
    if (noModel) found.unready.push_back("model\\scorer.onnx is missing");
    if (found.python.empty()) found.unready.push_back("python\\python.exe is missing");
    if (unfinished || noModel) found.python.clear();
    const auto known = [](const std::string& engine) { return std::find(kEngines.begin(), kEngines.end(), engine) != kEngines.end(); };
    std::ifstream engineFile(folder / L"engine.json", std::ios::binary);
    const auto engineJson = nlohmann::json::parse(engineFile, nullptr, false);
    if (engineJson.is_object() && engineJson.value("engine", nlohmann::json()).is_string() &&
        engineJson.value("engines", nlohmann::json()).is_array()) {
        found.engine = engineJson["engine"].get<std::string>();
        for (const auto& engine : engineJson["engines"])
            if (engine.is_string() && known(engine.get<std::string>())) found.engines.push_back(engine.get<std::string>());
        if (!known(found.engine)) found.engine.clear();
    } else {
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        found.engine = "directml";
        if (fs::is_regular_file(fs::path(system) / L"nvidia-smi.exe", ec)) found.engines = {"cuda13", "cuda12"};
        found.engines.push_back("directml");
    }
    return found;
}

// Lines waiting for the YouTube window's stdin. Only the latest of each kind
// matters (an entry's state, the theme), so a line replaces a waiting one with
// the same key where it stands; an empty key is never replaced. Not locked:
// Job guards it.
class LatestByType {
public:
    // A window that stops reading cannot grow it without end: past the limit
    // the oldest list child's line goes, as the least of what is waiting.
    static constexpr size_t kLimit = 2000;

    void Push(std::string key, std::string line) {
        if (!key.empty())
            for (auto& entry : entries_)
                if (entry.first == key) { entry.second = std::move(line); return; }
        entries_.emplace_back(std::move(key), std::move(line));
        if (entries_.size() <= kLimit) return;
        const auto child = std::find_if(entries_.begin(), entries_.end(),
                                        [](const auto& entry) { return entry.first.starts_with("child:"); });
        entries_.erase(child != entries_.end() ? child : entries_.begin());
    }

    std::optional<std::string> Pop() {
        if (entries_.empty()) return std::nullopt;
        std::string line = std::move(entries_.front().second);
        entries_.pop_front();
        return line;
    }

    size_t Size() const { return entries_.size(); }

private:
    std::deque<std::pair<std::string, std::string>> entries_;
};

class Job {
public:
    using Sink = std::function<void(const Status&)>;

    Job() = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    // Cancel ends the process tree, which breaks the stdin pipe under a writer
    // still blocked on it, so both joins return.
    ~Job() {
        Cancel();
        if (worker_.joinable()) worker_.join();
        if (writer_.joinable()) writer_.join();
    }

    // Returns false if the process could not start; the sink is then never called.
    // Otherwise the sink runs on the job's thread and the last status is always
    // Done, Finished or Error.
    //
    // With input, the child's stdin is a pipe that Write feeds, through a
    // thread of its own so the caller never waits on the child. Without it the
    // child gets the app's own stdin, as before.
    bool Start(const std::wstring& commandLine, Sink sink, bool input = false) {
        // The previous run has already reported its final status, but its
        // process tree can take seconds to exit (Python unloading its libraries, a
        // closing sign-in window). Joining without Cancel waits for all of it.
        if (worker_.joinable()) { Cancel(); worker_.join(); }
        // Its writer has nothing left to write once Cancel has run, and its
        // child is gone, so this returns at once.
        if (writer_.joinable()) writer_.join();
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inherit, 0)) return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        // The child inherits the read end. The write end is not inheritable,
        // so a converter started later cannot hold the window's stdin open.
        HANDLE inRead = nullptr, inWrite = nullptr;
        if (input) {
            if (!CreatePipe(&inRead, &inWrite, &inherit, 0)) {
                CloseHandle(read);
                CloseHandle(write);
                return false;
            }
            SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
        }

        HANDLE group = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (group) SetInformationJobObject(group, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        PROCESS_INFORMATION process{};
        std::wstring mutableLine = commandLine;
        BOOL started = FALSE;
        if (input) started = CreateWithInput(mutableLine, write, inRead, process);
        else {
            STARTUPINFOW startup{sizeof(startup)};
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = write;
            startup.hStdError = write;
            started = CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
        }
        CloseHandle(write);
        if (inRead) CloseHandle(inRead);
        if (!started) {
            CloseHandle(read);
            if (inWrite) CloseHandle(inWrite);
            if (group) CloseHandle(group);
            return false;
        }
        if (group) AssignProcessToJobObject(group, process.hProcess);
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        {
            std::lock_guard lock(mutex_);
            process_ = process.hProcess;
            group_ = group;
            cancelled_ = false;
        }
        {
            std::lock_guard lock(inputMutex_);
            pending_ = {};
            inputOpen_ = input;
        }
        if (input) writer_ = std::thread([this, inWrite] { WriteInput(inWrite); });
        worker_ = std::thread([this, read, sink = std::move(sink)] { Pump(read, sink); });
        return true;
    }

    // Queues one line (without its newline) for the child's stdin, replacing a
    // waiting line with the same non-empty key. Never waits on the child.
    // False when the job has no input, or it was closed or broke.
    bool Write(std::string key, std::string line) {
        {
            std::lock_guard lock(inputMutex_);
            if (!inputOpen_) return false;
            pending_.Push(std::move(key), std::move(line));
        }
        inputWake_.notify_all();
        return true;
    }

    // Writes what is waiting, then closes stdin so the child reads its end.
    // Returns at once; the writer finishes on its own.
    void CloseInput() { StopInput(false); }

    // Waiting lines are dropped. The ended child breaks the pipe, which ends a
    // write in progress.
    void Cancel() {
        StopInput(true);
        std::lock_guard lock(mutex_);
        if (!process_) return;
        cancelled_ = true;
        if (group_) TerminateJobObject(group_, 1);
        else TerminateProcess(process_, 1);
    }

    bool Running() const {
        std::lock_guard lock(mutex_);
        return process_ != nullptr;
    }

private:
    // Starts the child with stdout and stdin redirected, letting it inherit
    // those two handles alone (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), whatever
    // else in the app is inheritable.
    static BOOL CreateWithInput(std::wstring& commandLine, HANDLE output, HANDLE input, PROCESS_INFORMATION& process) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<char> storage(size);
        const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return FALSE;
        HANDLE inherited[] = {output, input};
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = output;
        startup.StartupInfo.hStdError = output;
        startup.lpAttributeList = attributes;
        const BOOL started =
            UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) &&
            CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                           &startup.StartupInfo, &process);
        DeleteProcThreadAttributeList(attributes);
        return started;
    }

    // Takes Write's lines no more; drop says whether the waiting ones go too.
    void StopInput(bool drop) {
        {
            std::lock_guard lock(inputMutex_);
            inputOpen_ = false;
            if (drop) pending_ = {};
        }
        inputWake_.notify_all();
    }

    // The writer thread: one line at a time, written outside the lock, so only
    // this thread waits when the child stops reading. It ends when the input
    // is closed with nothing left, or a write fails, and closes the pipe.
    void WriteInput(HANDLE pipe) {
        std::unique_lock lock(inputMutex_);
        for (;;) {
            inputWake_.wait(lock, [this] { return pending_.Size() > 0 || !inputOpen_; });
            auto line = pending_.Pop();
            if (!line) break;
            lock.unlock();
            line->push_back('\n');
            bool wrote = true;
            for (size_t at = 0; wrote && at < line->size();) {
                DWORD count = 0;
                wrote = WriteFile(pipe, line->data() + at, static_cast<DWORD>(line->size() - at), &count, nullptr) && count;
                at += count;
            }
            lock.lock();
            if (!wrote) break;
        }
        // A broken pipe drops whatever is left, and Write says so from now on.
        inputOpen_ = false;
        pending_ = {};
        lock.unlock();
        CloseHandle(pipe);
    }

    void Pump(HANDLE read, const Sink& sink) {
        std::string pending;
        bool finished = false;
        const auto emit = [&](std::string_view line) {
            auto status = ParseLine(line);
            if (status.kind == Status::Kind::Text && status.text.empty()) return;
            if (IsFinal(status.kind)) finished = true;
            sink(status);
        };
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) {
            pending.append(buffer, got);
            for (size_t end; (end = pending.find_first_of("\r\n")) != std::string::npos;) {
                emit(std::string_view(pending).substr(0, end));
                pending.erase(0, end + 1);
            }
        }
        if (!pending.empty()) emit(pending);
        CloseHandle(read);

        HANDLE process, group;
        bool cancelled;
        {
            std::lock_guard lock(mutex_);
            process = process_;
            group = group_;
            cancelled = cancelled_;
        }
        WaitForSingleObject(process, INFINITE);
        // Nothing can read the child's stdin any more.
        StopInput(true);
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        {
            std::lock_guard lock(mutex_);
            process_ = nullptr;
            group_ = nullptr;
        }
        CloseHandle(process);
        if (group) CloseHandle(group);
        // Emit exactly one final status. A Cancel after the converter reported
        // done (as the destructor does) is not reported as a cancellation.
        if (finished) return;
        sink({Status::Kind::Error, cancelled ? std::string("Conversion cancelled.")
                                             : "The converter stopped with exit code " + std::to_string(code) + "."});
    }

    mutable std::mutex mutex_;
    HANDLE process_ = nullptr;
    HANDLE group_ = nullptr;
    bool cancelled_ = false;
    std::thread worker_;
    // The stdin side. inputMutex_ is never held with mutex_, nor during a write.
    std::mutex inputMutex_;
    std::condition_variable inputWake_;
    LatestByType pending_;
    bool inputOpen_ = false;
    std::thread writer_;
};

}  // namespace audio_to_midi
