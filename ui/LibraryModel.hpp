#pragma once
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace shell {
struct MidiEntry {
    std::filesystem::path path;
    std::string name;
    uintmax_t bytes = 0;
    std::filesystem::file_time_type modified{};
    bool operator==(const MidiEntry&) const = default;
};
// Files the library lists: Standard MIDI Files, karaoke .kar ones included.
inline bool LibraryFile(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return extension == L".mid" || extension == L".midi" || extension == L".kar";
}
enum class FileSort { Name, Size, Modified };
inline bool FileBefore(const MidiEntry& a, const MidiEntry& b, FileSort sort, bool descending) {
    // Case-insensitive in place, in the order of the lowercased names compared as
    // unsigned bytes; a scan's sort would otherwise copy two names per comparison.
    const auto less = [](const std::string& x, const std::string& y) {
        return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(),
            [](unsigned char c, unsigned char d) { return std::tolower(c) < std::tolower(d); });
    };
    int order = 0;
    if (sort == FileSort::Size && a.bytes != b.bytes) order = a.bytes < b.bytes ? -1 : 1;
    else if (sort == FileSort::Modified && a.modified != b.modified) order = a.modified < b.modified ? -1 : 1;
    else {
        if (less(a.name, b.name)) order = -1;
        else if (less(b.name, a.name)) order = 1;
        else if (a.path != b.path) order = a.path < b.path ? -1 : 1;
    }
    return descending ? order > 0 : order < 0;
}

// Folder browsing over a flat scan. Each file's name is its path relative to
// the library root, so a folder is a name prefix: empty for the root, else
// ending in a separator. Folders with no MIDI files below them never appear.
struct FolderView {
    std::vector<std::string> folders;
    std::vector<size_t> files;
};
inline FolderView BrowseFolder(const std::vector<MidiEntry>& files, const std::string& prefix) {
    FolderView view;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& name = files[i].name;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        const auto cut = name.find_first_of("\\/", prefix.size());
        if (cut == std::string::npos) view.files.push_back(i);
        else view.folders.push_back(name.substr(prefix.size(), cut - prefix.size()));
    }
    const auto lower = [](std::string name) {
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return name;
    };
    std::sort(view.folders.begin(), view.folders.end(), [&](const auto& a, const auto& b) {
        const auto first = lower(a), second = lower(b);
        return first != second ? first < second : a < b;
    });
    view.folders.erase(std::unique(view.folders.begin(), view.folders.end()), view.folders.end());
    return view;
}
// Searches the open folder recursively. `query` must be lowercase. Only the
// path below `prefix` is matched, so the open folder's own name never matches.
inline std::vector<size_t> SearchFolder(const std::vector<MidiEntry>& files, const std::string& prefix,
                                        const std::string& query) {
    std::vector<size_t> found;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& name = files[i].name;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        // Matched in place, without a lowercased copy of every name per keystroke.
        const auto below = name.begin() + static_cast<std::ptrdiff_t>(prefix.size());
        if (std::search(below, name.end(), query.begin(), query.end(),
                [](unsigned char c, unsigned char q) { return std::tolower(c) == q; }) != name.end())
            found.push_back(i);
    }
    return found;
}
// Folder prefix containing the named file.
inline std::string FolderOf(const std::string& name) {
    const auto cut = name.find_last_of("\\/");
    return cut == std::string::npos ? std::string() : name.substr(0, cut + 1);
}
inline std::string ParentFolder(const std::string& prefix) {
    if (prefix.size() < 2) return {};
    const auto cut = prefix.find_last_of("\\/", prefix.size() - 2);
    return cut == std::string::npos ? std::string() : prefix.substr(0, cut + 1);
}
}
