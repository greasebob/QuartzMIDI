#include "Fonts.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <algorithm>
#include <map>
#include <vector>

namespace shell {
namespace {
ImFont* LoadResourceFont(int id, const char* name) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return nullptr;
    HGLOBAL loaded = LoadResource(module, resource);
    void* data = LockResource(loaded);
    const DWORD size = SizeofResource(module, resource);
    if (!data || !size || size > INT_MAX) return nullptr;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false; // The executable resource lives until exit.
    strcpy_s(config.Name, name);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(size), 14.f, &config);
}

void MergeCjk(ImFont* destination) {
    if (!destination) return;
    // The system CJK fonts are shared by all weights, and dynamic glyph loading
    // rasterizes only the characters used, including after a DPI change.
    //
    // The files are memory-mapped read-only rather than read into memory:
    // together they are ~71 MB, and a mapped view only commits the pages glyph
    // lookups touch, backed by the system file cache. Views live until exit.
    struct Source { const void* data = nullptr; size_t size = 0; };
    static std::map<std::wstring, Source> sources;
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows, MAX_PATH)) return;
    for (const wchar_t* filename : {L"msyh.ttc", L"YuGothM.ttc", L"msgothic.ttc", L"malgun.ttf", L"simsun.ttc"}) {
        auto [it, inserted] = sources.try_emplace(filename);
        if (inserted) {
            const auto path = std::filesystem::path(windows) / L"Fonts" / filename;
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER size{};
                if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= INT_MAX) {
                    if (const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr)) {
                        it->second.data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
                        if (it->second.data) it->second.size = static_cast<size_t>(size.QuadPart);
                        CloseHandle(mapping); // The view keeps the section alive.
                    }
                }
                CloseHandle(file);
            }
        }
        const auto& source = it->second;
        if (!source.data) continue;
        ImFontConfig config;
        config.MergeMode = true;
        config.DstFont = destination;
        config.FontDataOwnedByAtlas = false;
        // Merged sources only fill glyphs the base font lacks, so Latin text
        // keeps the skin's typeface.
        ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<void*>(source.data), static_cast<int>(source.size), 14.f, &config);
    }
}
}

void Fonts::Load() {
    fonts_[0] = LoadResourceFont(101, "IBM Plex Sans Regular");
    fonts_[1] = LoadResourceFont(102, "IBM Plex Sans Medium");
    fonts_[2] = LoadResourceFont(103, "IBM Plex Sans Semibold");
    ImFont* fallback = fonts_[0];
    if (!fallback) fallback = ImGui::GetIO().Fonts->AddFontDefault();
    for (auto& font : fonts_) if (!font) font = fallback;
    std::vector<ImFont*> merged;
    for (ImFont* font : fonts_) {
        if (std::find(merged.begin(), merged.end(), font) != merged.end()) continue;
        MergeCjk(font); merged.push_back(font);
    }
    ImGui::GetIO().FontDefault = fonts_[0];
}

ImFont* Fonts::Get(const skin::Skin&, Weight weight) const {
    return fonts_[static_cast<int>(weight)];
}
}
