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
// The Trash is a hidden folder of this name at the top of the MIDI folder,
// where the user can find it and where it goes with the songs. The library,
// its watcher and Scan pass over a folder so named wherever it is.
inline constexpr wchar_t kTrashFolder[] = L".trash";
inline bool TrashFolderName(const std::filesystem::path& name) {
    auto text = name.native();
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text == kTrashFolder;
}
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
// The files Previous, Next and shuffle choose from: those in the open file's
// folder, or the whole list for a file from outside the library.
inline std::vector<MidiEntry> SongsBeside(const std::vector<MidiEntry>& files, const std::filesystem::path& open) {
    std::vector<MidiEntry> beside;
    const auto folder = open.parent_path();
    for (const auto& file : files)
        if (file.path.parent_path() == folder) beside.push_back(file);
    return beside.empty() ? files : beside;
}

// The lists the Files panel shows besides the folder, kept by path in
// library.json beside config.json. Playlists are numbered from 0.
enum ListId : int { kFolderList = -1, kFavouritesList = -2, kQueueList = -3, kTrashList = -4 };
struct Playlist {
    std::string name;
    std::vector<std::filesystem::path> songs;
    bool operator==(const Playlist&) const = default;
};
// A file moved away from where it was: a song in the Trash, or one a scan
// moved into the library.
struct MovedSong {
    std::filesystem::path file, origin;
    bool operator==(const MovedSong&) const = default;
};
struct LibraryLists {
    std::vector<std::filesystem::path> favourites;
    std::vector<Playlist> playlists;
    // Played before the rest, first in first out.
    std::vector<std::filesystem::path> queue;
    // Songs removed from the library, in the MIDI folder's Trash (kTrashFolder).
    std::vector<MovedSong> trash;
    // Files a scan moved into the library, which Put back returns.
    std::vector<MovedSong> scanned;
    bool operator==(const LibraryLists&) const = default;
    bool Favourite(const std::filesystem::path& path) const {
        return std::find(favourites.begin(), favourites.end(), path) != favourites.end();
    }
    const MovedSong* Scanned(const std::filesystem::path& path) const {
        const auto found = std::find_if(scanned.begin(), scanned.end(), [&](const MovedSong& song) { return song.file == path; });
        return found == scanned.end() ? nullptr : &*found;
    }
    // Whether the song once at `path` is in the Trash.
    bool Trashed(const std::filesystem::path& path) const {
        return std::any_of(trash.begin(), trash.end(), [&](const MovedSong& song) { return song.origin == path; });
    }
    // Every list follows a song that moved.
    void Renamed(const std::filesystem::path& from, const std::filesystem::path& to) {
        const auto follow = [&](std::vector<std::filesystem::path>& songs) { std::replace(songs.begin(), songs.end(), from, to); };
        follow(favourites);
        follow(queue);
        for (auto& playlist : playlists) follow(playlist.songs);
    }
    // A song gone for good leaves every list.
    void Forget(const std::filesystem::path& path) {
        const auto drop = [&](std::vector<std::filesystem::path>& songs) { songs.erase(std::remove(songs.begin(), songs.end(), path), songs.end()); };
        drop(favourites);
        drop(queue);
        for (auto& playlist : playlists) drop(playlist.songs);
        std::erase_if(scanned, [&](const MovedSong& song) { return song.file == path; });
    }
    // The songs a playlist or the queue holds, in order; null for any other list.
    const std::vector<std::filesystem::path>* Songs(int list) const {
        if (list == kQueueList) return &queue;
        if (list >= 0 && static_cast<size_t>(list) < playlists.size()) return &playlists[static_cast<size_t>(list)].songs;
        return nullptr;
    }
    std::vector<std::filesystem::path>* Songs(int list) {
        return const_cast<std::vector<std::filesystem::path>*>(static_cast<const LibraryLists&>(*this).Songs(list));
    }
};
// The songs Previous, Next, shuffle and up next choose from: the favourites or
// a playlist while it is shown, else the songs beside the open one (SongsBeside).
// A playlist's song outside the library is named by its file.
inline std::vector<MidiEntry> SongsFollowed(const std::vector<MidiEntry>& files, const std::filesystem::path& open,
                                            const LibraryLists& lists, int openList) {
    std::vector<MidiEntry> songs;
    if (openList == kFavouritesList)
        for (const auto& file : files)
            if (lists.Favourite(file.path)) songs.push_back(file);
    if (const auto* listed = openList >= 0 ? lists.Songs(openList) : nullptr)
        for (const auto& song : *listed) {
            const auto found = std::find_if(files.begin(), files.end(), [&](const MidiEntry& file) { return file.path == song; });
            const auto name = song.filename().u8string();
            songs.push_back(found != files.end() ? *found : MidiEntry{song, std::string(name.begin(), name.end())});
        }
    return songs.empty() ? SongsBeside(files, open) : songs;
}
// Moves `song` within `songs` to before the song at `index`, counted before the
// move, or to the end past the last; false when it is not there.
inline bool MoveSong(std::vector<std::filesystem::path>& songs, const std::filesystem::path& song, size_t index) {
    const auto found = std::find(songs.begin(), songs.end(), song);
    if (found == songs.end()) return false;
    const auto from = static_cast<size_t>(found - songs.begin());
    index = std::min(index, songs.size());
    if (index > from) --index;
    songs.erase(found);
    songs.insert(songs.begin() + static_cast<std::ptrdiff_t>(index), song);
    return true;
}
// `wanted`, or the first of "name (2).mid", "name (3).mid" and on that is free.
inline std::filesystem::path FreeName(const std::filesystem::path& wanted) {
    std::error_code ignored;
    if (!std::filesystem::exists(wanted, ignored)) return wanted;
    for (int n = 2;; ++n) {
        auto candidate = wanted.parent_path() / (wanted.stem().wstring() + L" (" + std::to_wstring(n) + L")" + wanted.extension().wstring());
        if (!std::filesystem::exists(candidate, ignored)) return candidate;
    }
}
}
