#pragma once
// Writing the files the app keeps, so that none is ever left half written,
// and reading one that was anyway.
#include <windows.h>
#include "LibraryModel.hpp"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace shell {

// Writes text to a temporary file beside path, through to the disk, then
// renames it over path, so a power cut leaves the old file or the new one
// whole. False when either step fails; path is then as it was.
inline bool WriteDurably(const std::filesystem::path& path, std::string_view text) {
    auto temporary = path;
    temporary += L".shell-tmp";
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool whole = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                       written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!whole || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

// Keeps a copy of a file that could not be read whole, as "<name>.damaged", or
// "<name> (2).damaged" and on when older ones are there, for the caller to
// replace it. Empty when it could not be copied.
inline std::filesystem::path SetAside(const std::filesystem::path& path) {
    auto wanted = path;
    wanted += L".damaged";
    const auto aside = FreeName(wanted);
    if (!CopyFileW(path.c_str(), aside.c_str(), TRUE)) return {};
    return aside;
}

// A saved JSON object, read as far as it goes: a file cut short or broken
// partway keeps every entry before the damage, a half-read one included for
// the caller to check entry by entry. damaged is set when the file is there
// but not a whole object. Null when there is no file.
inline nlohmann::json ReadSaved(const std::filesystem::path& path, bool& damaged) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return nullptr;
    const std::string text(std::istreambuf_iterator<char>(stream), {});
    auto whole = nlohmann::json::parse(text, nullptr, false);
    if (whole.is_object()) return whole;
    damaged = true;
    // The parser that builds the document, left with what it had read when it
    // stopped rather than discarding it.
    nlohmann::json partial;
    nlohmann::detail::json_sax_dom_parser<nlohmann::json> reader(partial, false);
    nlohmann::json::sax_parse(text, &reader);
    return partial.is_object() ? partial : nlohmann::json::object();
}

// A saved value, or fallback where there is none; one of the wrong kind falls
// back alone, setting damaged, and the values around it are still read.
template <typename T>
T SavedValue(const nlohmann::json& json, const char* key, T fallback, bool& damaged) {
    const auto found = json.find(key);
    if (found == json.end()) return fallback;
    try { return found->get<T>(); }
    catch (const nlohmann::json::exception&) { damaged = true; return fallback; }
}

} // namespace shell
