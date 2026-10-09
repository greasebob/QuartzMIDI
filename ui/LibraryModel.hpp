#pragma once
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace shell {
struct MidiEntry {
    std::filesystem::path path;
    std::string name;
    uintmax_t bytes = 0;
    std::filesystem::file_time_type modified{};
    // Seconds to the last note, through the song's tempo changes; below 0
    // until it is read off the worker, and for a file that cannot be read.
    double seconds = -1;
    // Listed by a playlist or the queue from outside the library.
    bool outside = false;
    bool operator==(const MidiEntry&) const = default;
};
// Whether two paths name the same file as Windows sees them, letters in either
// case and either slash.
inline bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    const auto& x = a.native();
    const auto& y = b.native();
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) {
        wchar_t c = x[i], d = y[i];
        if (c == d) continue;
        if (c == L'/') c = L'\\';
        if (d == L'/') d = L'\\';
        if (std::towupper(c) != std::towupper(d)) return false;
    }
    return true;
}
// The same for every path SamePath takes as this one.
inline std::wstring PathKey(const std::filesystem::path& path) {
    std::wstring key = path.native();
    for (auto& c : key) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towupper(c));
    return key;
}
// Hashes a path as SamePath compares it, in place, so a lookup copies nothing.
struct PathHash {
    size_t operator()(const std::filesystem::path& path) const noexcept {
        size_t hash = 14695981039346656037ull;
        for (wchar_t c : path.native()) {
            if (c == L'/') c = L'\\';
            hash = (hash ^ static_cast<size_t>(std::towupper(c))) * 1099511628211ull;
        }
        return hash;
    }
};
struct PathEqual {
    bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const { return SamePath(a, b); }
};
using PathSet = std::unordered_set<std::filesystem::path, PathHash, PathEqual>;
template <class T> using PathMap = std::unordered_map<std::filesystem::path, T, PathHash, PathEqual>;
// What is left of `path` below the folder `root`, or empty when it is not inside it.
inline std::filesystem::path Below(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto& text = path.native();
    auto folder = root.native();
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    if (folder.empty() || text.size() <= folder.size() + 1) return {};
    if (text[folder.size()] != L'\\' && text[folder.size()] != L'/') return {};
    if (!SamePath(std::filesystem::path(text.substr(0, folder.size())), std::filesystem::path(folder))) return {};
    return std::filesystem::path(text.substr(folder.size() + 1));
}
// Files the library lists: Standard MIDI Files, karaoke .kar ones included.
inline bool LibraryFile(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return extension == L".mid" || extension == L".midi" || extension == L".kar";
}
enum class FileSort { Name, Length, Modified };
inline bool FileBefore(const MidiEntry& a, const MidiEntry& b, FileSort sort, bool descending) {
    // A song with no length yet goes last either way round.
    if (sort == FileSort::Length && (a.seconds < 0) != (b.seconds < 0)) return b.seconds < 0;
    // Case-insensitive in place, in the order of the lowercased names compared as
    // unsigned bytes; a scan's sort would otherwise copy two names per comparison.
    const auto less = [](const std::string& x, const std::string& y) {
        return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(),
            [](unsigned char c, unsigned char d) { return std::tolower(c) < std::tolower(d); });
    };
    int order = 0;
    if (sort == FileSort::Length && a.seconds != b.seconds) order = a.seconds < b.seconds ? -1 : 1;
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
// Letters and digits make words; any other byte, such as a space, _ or -,
// separates them. Bytes of non-ASCII text count as letters.
inline bool SearchWordByte(unsigned char c) { return c >= 0x80 || std::isalnum(c); }
inline std::vector<std::string> SearchWords(const std::string& query) {
    std::vector<std::string> words(1);
    for (const unsigned char c : query) {
        if (SearchWordByte(c)) words.back() += static_cast<char>(c);
        else if (!words.back().empty()) words.emplace_back();
    }
    if (words.back().empty()) words.pop_back();
    return words;
}
// Searches the open folder recursively. `query` must be lowercase. Only the
// path below `prefix` is matched, so the open folder's own name never matches.
// As in Explorer, each word of the query, in any order, matches the start of a
// word in the name, so "a minor" finds "Waltz_in_A_minor". A word with non-ASCII
// text matches anywhere, since such names often have no spaces.
inline std::vector<size_t> SearchFolder(const std::vector<MidiEntry>& files, const std::string& prefix,
                                        const std::string& query) {
    struct Word { std::string text; bool anywhere; };
    std::vector<Word> words;
    for (auto& text : SearchWords(query)) {
        const bool anywhere = std::any_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x80; });
        words.push_back({std::move(text), anywhere});
    }
    // A query of only separators looks for itself.
    if (words.empty() && !query.empty()) words.push_back({query, true});
    std::vector<size_t> found;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& name = files[i].name;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        // Matched in place, without a lowercased copy of every name per keystroke.
        const auto below = name.begin() + static_cast<std::ptrdiff_t>(prefix.size());
        const auto has = [&](const Word& word) {
            for (auto at = below;; ++at) {
                at = std::search(at, name.end(), word.text.begin(), word.text.end(),
                    [](unsigned char c, unsigned char q) { return std::tolower(c) == q; });
                if (at == name.end()) return false;
                if (word.anywhere || at == below || !SearchWordByte(static_cast<unsigned char>(*(at - 1)))) return true;
            }
        };
        if (std::all_of(words.begin(), words.end(), has)) found.push_back(i);
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
enum ListId : int { kFolderList = -1, kFavouritesList = -2, kQueueList = -3 };
struct Playlist {
    std::string name;
    std::vector<std::filesystem::path> songs;
    bool operator==(const Playlist&) const = default;
};
// What a song sent to the Recycle Bin was in, so one restored from it takes
// its star, playlists and settings back (LibraryLists::Restore).
struct DeletedSong {
    std::filesystem::path path;
    bool favourite = false;
    // Each playlist it was in by name, with its place there.
    std::vector<std::pair<std::string, size_t>> playlists;
    // Its songs.json entry as JSON text; empty when it had none.
    std::string settings;
    bool operator==(const DeletedSong&) const = default;
};
// The records kept; the oldest go first.
inline constexpr size_t kMaxDeletedSongs = 200;
struct LibraryLists {
    std::vector<std::filesystem::path> favourites;
    std::vector<Playlist> playlists;
    // Played before the rest, first in first out.
    std::vector<std::filesystem::path> queue;
    // Songs sent to the Recycle Bin, oldest first.
    std::vector<DeletedSong> deleted;
    bool operator==(const LibraryLists&) const = default;
    static bool Holds(const std::vector<std::filesystem::path>& songs, const std::filesystem::path& path) {
        return std::any_of(songs.begin(), songs.end(), [&](const std::filesystem::path& song) { return SamePath(song, path); });
    }
    bool Favourite(const std::filesystem::path& path) const { return Holds(favourites, path); }
    // Every list calls each song once; a song listed twice keeps its first place.
    void Deduplicate() {
        const auto once = [](std::vector<std::filesystem::path>& songs) {
            std::unordered_set<std::wstring> seen;
            std::erase_if(songs, [&](const std::filesystem::path& song) { return !seen.insert(PathKey(song)).second; });
        };
        once(favourites);
        once(queue);
        for (auto& playlist : playlists) once(playlist.songs);
    }
    // A song gone for good leaves every list.
    void Forget(const std::filesystem::path& path) {
        const auto drop = [&](std::vector<std::filesystem::path>& songs) {
            std::erase_if(songs, [&](const std::filesystem::path& song) { return SamePath(song, path); });
        };
        drop(favourites);
        drop(queue);
        for (auto& playlist : playlists) drop(playlist.songs);
    }
    // A song sent to the Recycle Bin leaves every list, and what it was in is
    // kept, with `settings`, until it is back (Restore).
    void Delete(const std::filesystem::path& path, std::string settings) {
        DeletedSong record{path, Favourite(path), {}, std::move(settings)};
        for (const auto& playlist : playlists)
            for (size_t i = 0; i < playlist.songs.size(); ++i)
                if (SamePath(playlist.songs[i], path)) { record.playlists.emplace_back(playlist.name, i); break; }
        Forget(path);
        std::erase_if(deleted, [&](const DeletedSong& each) { return SamePath(each.path, path); });
        if (record.favourite || !record.playlists.empty() || !record.settings.empty()) deleted.push_back(std::move(record));
        if (deleted.size() > kMaxDeletedSongs) deleted.erase(deleted.begin(), deleted.end() - kMaxDeletedSongs);
    }
    // Deleted songs whose paths are in `present` (by PathKey) take their star
    // and playlists back, each at its place or the end; their records go and
    // are returned, for their settings.
    std::vector<DeletedSong> Restore(const std::unordered_set<std::wstring>& present) {
        std::vector<DeletedSong> restored;
        std::erase_if(deleted, [&](DeletedSong& record) {
            if (!present.contains(PathKey(record.path))) return false;
            if (record.favourite && !Favourite(record.path)) favourites.push_back(record.path);
            for (const auto& [name, at] : record.playlists)
                for (auto& playlist : playlists)
                    if (playlist.name == name && !Holds(playlist.songs, record.path))
                        playlist.songs.insert(playlist.songs.begin() + static_cast<std::ptrdiff_t>(std::min(at, playlist.songs.size())), record.path);
            restored.push_back(std::move(record));
            return true;
        });
        return restored;
    }
    // The MIDI folder is now `to`, not `from`: each song of `from` found at the
    // same place in `to` is taken to have moved with it, and its lists follow;
    // the rest are left, for a folder only switched away from.
    // A folder named again in other letter case takes every song along.
    void Rebased(const std::filesystem::path& from, const std::filesystem::path& to) {
        if (from.empty() || to.empty() || from == to) return;
        const bool recased = SamePath(from, to);
        const auto moved = [&](const std::filesystem::path& path) {
            const auto rest = Below(path, from);
            return rest.empty() ? path : to / rest;
        };
        const auto there = [&](const std::filesystem::path& path) {
            std::error_code error;
            return recased || std::filesystem::exists(path, error);
        };
        const auto follow = [&](std::vector<std::filesystem::path>& songs) {
            for (auto& song : songs)
                if (auto file = moved(song); file != song && there(file)) song = std::move(file);
        };
        follow(favourites);
        follow(queue);
        for (auto& playlist : playlists) follow(playlist.songs);
        Deduplicate();
    }
    // Each listed song named in other letter case than its file takes the
    // file's own, from `files` by PathKey.
    void Recased(const std::unordered_map<std::wstring, std::filesystem::path>& files) {
        const auto follow = [&](std::filesystem::path& song) {
            if (const auto found = files.find(PathKey(song)); found != files.end() && found->second != song) song = found->second;
        };
        for (auto& song : favourites) follow(song);
        for (auto& song : queue) follow(song);
        for (auto& playlist : playlists) for (auto& song : playlist.songs) follow(song);
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
// Each file's place in `files` by path, so a library of thousands is searched
// once per scan rather than a song at a time.
inline PathMap<size_t> IndexFiles(const std::vector<MidiEntry>& files) {
    PathMap<size_t> index;
    index.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) index.emplace(files[i].path, i);
    return index;
}
// The rows of a playlist or the queue, in its order: each song's entry in
// `files` (by `index`), else in `outside`, else one named by its full path.
inline std::vector<MidiEntry> ListEntries(const std::vector<std::filesystem::path>& songs, const std::vector<MidiEntry>& files,
                                          const PathMap<size_t>& index, const std::vector<MidiEntry>& outside) {
    const auto outsideIndex = IndexFiles(outside);
    std::vector<MidiEntry> entries;
    entries.reserve(songs.size());
    for (const auto& song : songs) {
        if (const auto found = index.find(song); found != index.end()) entries.push_back(files[found->second]);
        else if (const auto away = outsideIndex.find(song); away != outsideIndex.end()) entries.push_back(outside[away->second]);
        else {
            const auto name = song.u8string();
            MidiEntry entry{song, std::string(name.begin(), name.end())};
            entry.outside = true;
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}
// The songs Previous, Next, shuffle and up next choose from: the favourites or
// a playlist while it is shown, none when it is empty, else the songs beside
// the open one (SongsBeside). A playlist's song outside the library is named by its file.
inline std::vector<MidiEntry> SongsFollowed(const std::vector<MidiEntry>& files, const std::filesystem::path& open,
                                            const LibraryLists& lists, int openList) {
    std::vector<MidiEntry> songs;
    if (openList == kFavouritesList) {
        const PathSet favourites(lists.favourites.begin(), lists.favourites.end());
        for (const auto& file : files)
            if (favourites.contains(file.path)) songs.push_back(file);
        return songs;
    }
    if (const auto* listed = openList >= 0 ? lists.Songs(openList) : nullptr) {
        const auto index = IndexFiles(files);
        for (const auto& song : *listed) {
            if (const auto found = index.find(song); found != index.end()) { songs.push_back(files[found->second]); continue; }
            const auto name = song.filename().u8string();
            MidiEntry entry{song, std::string(name.begin(), name.end())};
            entry.outside = true;
            songs.push_back(std::move(entry));
        }
        return songs;
    }
    return SongsBeside(files, open);
}
// Moves `song` within `songs` to before the song at `index`, counted before the
// move, or to the end past the last; false when it is not there. A list calls
// each song once (LibraryLists::Deduplicate), so the song is the one moved.
inline bool MoveSong(std::vector<std::filesystem::path>& songs, const std::filesystem::path& song, size_t index) {
    const auto found = std::find_if(songs.begin(), songs.end(), [&](const std::filesystem::path& each) { return SamePath(each, song); });
    if (found == songs.end()) return false;
    const auto from = static_cast<size_t>(found - songs.begin());
    const auto moved = *found;
    index = std::min(index, songs.size());
    if (index > from) --index;
    songs.erase(found);
    songs.insert(songs.begin() + static_cast<std::ptrdiff_t>(index), moved);
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
