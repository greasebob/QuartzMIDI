#pragma once

// SheetImage: renders a StyledResult to PNG as the editor page displays it
// (midi-converter's dark background, Verdana, rhythm colours), without a browser.
// Uses GDI+ for text and PNG encoding.
//
// Header only, Windows only, no project dependencies.

#include "SheetExport.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <algorithm>
#include <cmath>
#include <windows.h>
// gdiplus.h uses unqualified min and max, which NOMINMAX removes.
using std::max;
using std::min;
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace sheet {

// Page appearance settings, stored in the page data as fontSize and lineHeight.
struct Look { double fontSizePt = 10; double lineHeightPercent = 135; };

namespace detail {

inline void EnsureGdiplus() {
    static const ULONG_PTR token = [] {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR started = 0;
        Gdiplus::GdiplusStartup(&started, &input, nullptr);
        // GDI+ is never shut down; pin gdiplus.dll so it outlives the add-on DLL.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"gdiplus.dll", &pinned);
        return started;
    }();
    (void)token;
}

inline bool PngEncoder(CLSID& clsid) {
    UINT count = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) return false;
    std::vector<unsigned char> buffer(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i)
        if (std::wstring(codecs[i].MimeType) == L"image/png") { clsid = codecs[i].Clsid; return true; }
    return false;
}

inline std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

inline Gdiplus::Color ColourFromCss(const char* css) {
    if (css[0] != '#') return Gdiplus::Color(255, 255, 255, 255);   // "white"
    const auto hex = [&](int at) { return static_cast<BYTE>(std::stoi(std::string(css + at, 2), nullptr, 16)); };
    return Gdiplus::Color(255, hex(1), hex(3), hex(5));
}

// Run: text of one colour and weight. Word: a chord plus its separator, never
// split across lines. Line: a sheet line, wrapped to width when drawn.
struct ImageRun { std::wstring text; Gdiplus::Color colour; bool outOfRange = false; };
struct ImageWord { std::vector<ImageRun> runs; };
struct ImageLine { std::vector<ImageWord> words; };

inline std::vector<ImageLine> ImageLines(const StyledResult& r) {
    using Kind = StyledItem::Kind;
    const Gdiplus::Color comment(255, 0xc8, 0xc4, 0xcc);
    std::vector<ImageLine> lines(1);
    const auto blank = [&] { if (!lines.back().words.empty()) lines.emplace_back(); };
    for (size_t i = 0; i < r.items.size(); ++i) {
        const auto& item = r.items[i];
        if (item.kind == Kind::Chord) {
            ImageWord word;
            const auto colour = ColourFromCss(RhythmColour(item.rhythm));
            for (const auto& segment : item.segments) word.runs.push_back({Wide(segment.text), colour, segment.outOfRange});
            if (!item.separator.empty()) word.runs.push_back({Wide(item.separator), colour, false});
            lines.back().words.push_back(std::move(word));
        } else if (item.kind == Kind::Comment) {
            // Match the text output: blank line, comment, blank line.
            blank();
            lines.emplace_back();
            lines.back().words.push_back({{{Wide(item.text), comment, false}}});
            lines.emplace_back();
            lines.emplace_back();
        } else {
            const bool nearComment = (i > 0 && r.items[i - 1].kind == Kind::Comment) ||
                (i + 1 < r.items.size() && r.items[i + 1].kind == Kind::Comment);
            if (!nearComment) lines.emplace_back();
        }
    }
    // Drop leading, trailing and repeated blank lines, as TidyLines does for text.
    std::vector<ImageLine> kept;
    for (auto& line : lines) {
        if (line.words.empty() && (kept.empty() || kept.back().words.empty())) continue;
        kept.push_back(std::move(line));
    }
    while (!kept.empty() && kept.back().words.empty()) kept.pop_back();
    return kept;
}

} // namespace detail

// Writes the sheet as a PNG and returns its size in pixels. scale is device
// pixels per CSS pixel (2 keeps text sharp when zoomed); maxWidth is the wrap
// width in CSS pixels. Throws when the file cannot be written.
inline std::pair<int, int> SavePng(const StyledResult& r, const std::string& title, const Look& look,
                                   const std::filesystem::path& file, double scale = 2, int maxWidth = 1100) {
    detail::EnsureGdiplus();
    const float fontPx = static_cast<float>(look.fontSizePt * 96 / 72 * scale);
    const float lineHeight = static_cast<float>(fontPx * look.lineHeightPercent / 100);
    const float pad = static_cast<float>(16 * scale);
    const float wrapWidth = static_cast<float>(maxWidth * scale);
    Gdiplus::Font regular(L"Verdana", fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font heavy(L"Verdana", fontPx, Gdiplus::FontStyleBold | Gdiplus::FontStyleUnderline, Gdiplus::UnitPixel);
    if (!regular.IsAvailable() || !heavy.IsAvailable()) throw std::runtime_error("Verdana is not installed, so the sheet image cannot be drawn.");
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces | Gdiplus::StringFormatFlagsNoWrap);

    // Measure on a 1x1 scratch bitmap and lay out before sizing the real one.
    Gdiplus::Bitmap ruler(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics measure(&ruler);
    measure.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    const auto widthOf = [&](const std::wstring& text, bool outOfRange) {
        Gdiplus::RectF bounds;
        measure.MeasureString(text.c_str(), -1, outOfRange ? &heavy : &regular, Gdiplus::PointF(0, 0), &format, &bounds);
        return bounds.Width;
    };
    struct Placed { float x; float y; detail::ImageRun run; };
    std::vector<Placed> placed;
    float y = pad, widest = 0;
    const std::wstring heading = detail::Wide(title);
    if (!heading.empty()) {
        placed.push_back({pad, y, {heading, Gdiplus::Color(255, 0xaa, 0xa4, 0xb3), false}});
        widest = (std::max)(widest, widthOf(heading, false));
        y += lineHeight * 2;
    }
    for (const auto& line : detail::ImageLines(r)) {
        float x = pad;
        for (const auto& word : line.words) {
            float wordWidth = 0;
            for (const auto& run : word.runs) wordWidth += widthOf(run.text, run.outOfRange);
            if (x > pad && x - pad + wordWidth > wrapWidth) { x = pad; y += lineHeight; }
            for (const auto& run : word.runs) {
                placed.push_back({x, y, run});
                x += widthOf(run.text, run.outOfRange);
            }
            widest = (std::max)(widest, x - pad);
        }
        y += lineHeight;
    }
    const int width = static_cast<int>(std::ceil((std::min)(widest, wrapWidth) + 2 * pad));
    const int height = static_cast<int>(std::ceil(y + pad));

    Gdiplus::Bitmap bitmap((std::max)(width, 1), (std::max)(height, 1), PixelFormat32bppARGB);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) throw std::runtime_error("The sheet is too long for one picture.");
    Gdiplus::Graphics g(&bitmap);
    g.Clear(Gdiplus::Color(255, 0x2D, 0x2A, 0x32));
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    for (const auto& p : placed) {
        Gdiplus::SolidBrush brush(p.run.colour);
        g.DrawString(p.run.text.c_str(), -1, p.run.outOfRange ? &heavy : &regular, Gdiplus::PointF(p.x, p.y), &format, &brush);
    }
    CLSID png;
    if (!detail::PngEncoder(png)) throw std::runtime_error("No PNG encoder is available.");
    if (bitmap.Save(file.c_str(), &png, nullptr) != Gdiplus::Ok) {
        // path::string() throws on characters outside the ANSI code page; use UTF-8.
        const auto name = file.u8string();
        throw std::runtime_error("Cannot write " + std::string(name.begin(), name.end()) + ".");
    }
    return {width, height};
}

} // namespace sheet
