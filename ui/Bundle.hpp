#pragma once
// Files carried inside the single-exe build and written beside it on start.
//
// Format, little-endian: "QMB1", u32 entry count, then per entry u16 flags,
// u32 path length, the UTF-8 path with '/' separators, u64 size and the bytes.
// tools\make-release.ps1 writes it into the exe as RCDATA "QUARTZBUNDLE".
//
// A file is written when it is missing, or replaced when it still matches what
// the last unpack wrote (recorded in bundled-files.json), so a newer exe
// updates its own files but never one the user changed or replaced.
//
// The single exe keeps all of this, and everything the app saves, in
// %APPDATA%\QuartzMIDI; the zip builds keep it beside the exe.
#include <windows.h>
#include <shlobj.h>
#include "json.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shell::bundle {

enum Flags : std::uint16_t { Directory = 1, KeepExisting = 2 };

struct Entry {
    std::string path;
    std::uint16_t flags = 0;
    std::string_view data;
};

inline std::filesystem::path FromUtf8(const std::string& text) {
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), static_cast<int>(wide.size()));
    return wide;
}

inline std::string Fnv1a(std::string_view data) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : data) { hash ^= c; hash *= 1099511628211ull; }
    char text[17];
    snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// Rejects truncated data and any path that is absolute or climbs out with "..".
inline std::optional<std::vector<Entry>> Parse(std::span<const std::uint8_t> bytes) {
    size_t at = 0;
    const auto take = [&](void* out, size_t size) {
        if (bytes.size() - at < size) return false;
        std::memcpy(out, bytes.data() + at, size);
        at += size;
        return true;
    };
    char magic[4];
    std::uint32_t count = 0;
    if (!take(magic, 4) || std::memcmp(magic, "QMB1", 4) != 0 || !take(&count, 4)) return std::nullopt;
    std::vector<Entry> entries;
    for (std::uint32_t i = 0; i < count; ++i) {
        Entry entry;
        std::uint32_t length = 0;
        std::uint64_t size = 0;
        if (!take(&entry.flags, 2) || !take(&length, 4) || bytes.size() - at < length) return std::nullopt;
        entry.path.assign(reinterpret_cast<const char*>(bytes.data() + at), length);
        at += length;
        if (!take(&size, 8) || bytes.size() - at < size) return std::nullopt;
        entry.data = {reinterpret_cast<const char*>(bytes.data() + at), static_cast<size_t>(size)};
        at += static_cast<size_t>(size);
        const std::filesystem::path relative = FromUtf8(entry.path);
        if (entry.path.empty() || relative.is_absolute() || relative.has_root_name()) return std::nullopt;
        for (const auto& part : relative) if (part == L"..") return std::nullopt;
        entries.push_back(std::move(entry));
    }
    return entries;
}

inline std::span<const std::uint8_t> FromResource(HMODULE module) {
    HRSRC found = FindResourceW(module, L"QUARTZBUNDLE", MAKEINTRESOURCEW(10));   // RT_RCDATA
    if (!found) return {};
    HGLOBAL loaded = LoadResource(module, found);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return {};
    return {static_cast<const std::uint8_t*>(data), SizeofResource(module, found)};
}

// Where config, preferences, add-ons and the midi folder live. QUARTZMIDI_DATA
// overrides it, for the release check.
inline const std::filesystem::path& DataDirectory() {
    static const std::filesystem::path directory = [] {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        std::filesystem::path chosen = std::filesystem::path(executable).parent_path();
        if (const DWORD size = GetEnvironmentVariableW(L"QUARTZMIDI_DATA", nullptr, 0)) {
            std::wstring value(size, L'\0');
            value.resize(GetEnvironmentVariableW(L"QUARTZMIDI_DATA", value.data(), size));
            chosen = value;
        } else if (!FromResource(GetModuleHandleW(nullptr)).empty()) {
            PWSTR roaming = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) chosen = std::filesystem::path(roaming) / L"QuartzMIDI";
            CoTaskMemFree(roaming);
        }
        std::error_code ignored;
        std::filesystem::create_directories(chosen, ignored);
        return chosen;
    }();
    return directory;
}

// Returns the number of files written; best effort, so a read-only folder
// leaves the app running without them.
inline size_t Unpack(std::span<const std::uint8_t> bytes, const std::filesystem::path& directory) {
    const auto entries = Parse(bytes);
    if (!entries) return 0;
    const auto recordPath = directory / L"bundled-files.json";
    nlohmann::json record = nlohmann::json::object();
    {
        std::ifstream in(recordPath, std::ios::binary);
        if (in) {
            try { record = nlohmann::json::parse(in); } catch (...) {}
            if (!record.is_object()) record = nlohmann::json::object();
        }
    }
    size_t written = 0;
    bool changed = false;
    std::error_code ignored;
    for (const auto& entry : *entries) {
        const auto target = directory / FromUtf8(entry.path);
        if (entry.flags & Directory) {
            std::filesystem::create_directories(target, ignored);
            continue;
        }
        const std::string wanted = Fnv1a(entry.data);
        std::optional<std::string> current;
        if (std::ifstream in{target, std::ios::binary}) {
            current = Fnv1a(std::string(std::istreambuf_iterator<char>(in), {}));
        }
        if (current) {
            if (*current == wanted) {
                if (record.value(entry.path, std::string()) != wanted) { record[entry.path] = wanted; changed = true; }
                continue;
            }
            if (entry.flags & KeepExisting) continue;
            if (record.value(entry.path, std::string()) != *current) continue;   // the user's copy
        }
        std::filesystem::create_directories(target.parent_path(), ignored);
        auto temporary = target;
        temporary += L".unpacking";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out.write(entry.data.data(), static_cast<std::streamsize>(entry.data.size()))) continue;
        }
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::filesystem::remove(temporary, ignored);
            continue;
        }
        record[entry.path] = wanted;
        changed = true;
        ++written;
    }
    if (changed) {
        std::ofstream out(recordPath, std::ios::binary | std::ios::trunc);
        out << record.dump(1);
    }
    return written;
}

} // namespace shell::bundle
