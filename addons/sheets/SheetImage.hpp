#pragma once

// SheetImage: renders a StyledResult to PNG as the editor page displays it
// in its look (midi-converter's dark background, Verdana and rhythm colours
// by default), without a browser.
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
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

namespace sheet {

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

inline Gdiplus::Color ColourFromCss(const std::string& css) {
    if (!IsLookColour(css) || css[0] != '#') return Gdiplus::Color(255, 255, 255, 255);   // "white"
    const auto hex = [&](size_t at) { return static_cast<BYTE>(std::stoi(css.substr(at, 2), nullptr, 16)); };
    return Gdiplus::Color(255, hex(1), hex(3), hex(5));
}

// The picture in a data URL, or none when it is not one GDI+ can read.
inline std::unique_ptr<Gdiplus::Bitmap> ImageFromDataUrl(const std::string& url) {
    const auto comma = url.find(',');
    if (url.rfind("data:image/", 0) != 0 || comma == std::string::npos || url.find(";base64") > comma) return nullptr;
    std::string bytes;
    unsigned buffer = 0;
    int bits = 0;
    for (size_t i = comma + 1; i < url.size(); ++i) {
        const char c = url[i];
        const int value = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
                          c == '+' ? 62 : c == '/' ? 63 : -1;
        if (value < 0) continue;
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) { bits -= 8; bytes += static_cast<char>((buffer >> bits) & 0xFF); }
    }
    IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size()));
    if (!stream) return nullptr;
    std::unique_ptr<Gdiplus::Bitmap> decoded(Gdiplus::Bitmap::FromStream(stream));
    std::unique_ptr<Gdiplus::Bitmap> copy;
    if (decoded && decoded->GetLastStatus() == Gdiplus::Ok && decoded->GetWidth() && decoded->GetHeight()) {
        // Drawn into a bitmap of its own, as GDI+ may read the stream lazily.
        copy = std::make_unique<Gdiplus::Bitmap>(static_cast<INT>(decoded->GetWidth()), static_cast<INT>(decoded->GetHeight()), PixelFormat32bppARGB);
        Gdiplus::Graphics g(copy.get());
        g.DrawImage(decoded.get(), 0, 0, static_cast<INT>(decoded->GetWidth()), static_cast<INT>(decoded->GetHeight()));
    }
    decoded.reset();
    stream->Release();
    return copy && copy->GetLastStatus() == Gdiplus::Ok ? std::move(copy) : nullptr;
}

// The look's background over the whole picture, as the page draws it: its
// colour, or the image covering or tiled at its own size with the colour
// laid over it by dim.
inline void PaintBackground(Gdiplus::Graphics& g, const Look& look, int width, int height, double scale) {
    const auto colour = ColourFromCss(look.background);
    g.Clear(colour);
    const auto s = static_cast<Gdiplus::REAL>(scale);
    if (look.ground == Look::Image) {
        const auto image = ImageFromDataUrl(look.image);
        if (!image) return;
        const auto iw = static_cast<Gdiplus::REAL>(image->GetWidth()), ih = static_cast<Gdiplus::REAL>(image->GetHeight());
        if (look.fit == Look::Tile) {
            Gdiplus::TextureBrush brush(image.get(), Gdiplus::WrapModeTile);
            brush.ScaleTransform(s, s);
            g.FillRectangle(&brush, 0, 0, width, height);
        } else {
            const auto cover = (std::max)(width / iw, height / ih);
            g.DrawImage(image.get(), Gdiplus::RectF((width - iw * cover) / 2, (height - ih * cover) / 2, iw * cover, ih * cover));
        }
        if (look.dim > 0) {
            Gdiplus::SolidBrush shade(Gdiplus::Color(static_cast<BYTE>(std::lround(std::clamp(look.dim, 0, 100) * 2.55)), colour.GetR(), colour.GetG(), colour.GetB()));
            g.FillRectangle(&shade, 0, 0, width, height);
        }
    }
}

// Run: text of one colour and weight, and the pedal level of the highlight
// behind it, -1 for none. Word: a chord plus its separator, never split across
// lines. Line: a sheet line, wrapped to width when drawn.
struct ImageRun { std::wstring text; Gdiplus::Color colour; bool outOfRange = false; int pedalHighlight = -1; };
struct ImageWord { std::vector<ImageRun> runs; };
struct ImageLine { std::vector<ImageWord> words; };

inline std::vector<ImageLine> ImageLines(const StyledResult& r, const Look& look) {
    using Kind = StyledItem::Kind;
    const Gdiplus::Color comment = ColourFromCss(look.comment);
    std::vector<ImageLine> lines(1);
    const auto blank = [&] { if (!lines.back().words.empty()) lines.emplace_back(); };
    for (size_t i = 0; i < r.items.size(); ++i) {
        const auto& item = r.items[i];
        if (item.kind == Kind::Chord) {
            ImageWord word;
            const auto colour = ColourFromCss(ChordColour(item.rhythm, look));
            // As SheetBody draws them: the chord at its level, the separator
            // at it while the pedal is held into the next chord.
            const auto highlight = [&](int level) { return PedalHighlight(level, r.pedalMarks, look).empty() ? -1 : level; };
            for (const auto& segment : item.segments) word.runs.push_back({Wide(segment.text), colour, segment.outOfRange, highlight(item.pedal)});
            if (!item.separator.empty()) word.runs.push_back({Wide(item.separator), colour, false, highlight(item.pedalHeld ? item.pedal : 0)});
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
// The lines over the sheet, each wrapped on its own, and whether they are centred.
struct HeadingBlock { std::vector<std::string> lines; bool centre = false; };

inline std::pair<int, int> SavePng(const StyledResult& r, const HeadingBlock& heading, const Look& look,
                                   const std::filesystem::path& file, double scale = 2, int maxWidth = 1100) {
    detail::EnsureGdiplus();
    const float fontPx = static_cast<float>(look.fontSizePt * 96 / 72 * scale);
    const float lineHeight = static_cast<float>(fontPx * look.lineHeightPercent / 100);
    const float pad = static_cast<float>(16 * scale);
    const float wrapWidth = static_cast<float>(maxWidth * scale);
    // A font this Windows lacks draws in Verdana, as the page's fallback does.
    const char* fontName = kFonts[look.font >= 0 && look.font < kFontCount ? look.font : 0].name;
    if (!Gdiplus::Font(detail::Wide(fontName).c_str(), fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel).IsAvailable()) fontName = kFonts[0].name;
    const auto fontWide = detail::Wide(fontName);
    Gdiplus::Font regular(fontWide.c_str(), fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font heavy(fontWide.c_str(), fontPx, Gdiplus::FontStyleBold | Gdiplus::FontStyleUnderline, Gdiplus::UnitPixel);
    if (!regular.IsAvailable() || !heavy.IsAvailable()) throw std::runtime_error(std::string(fontName) + " is not installed, so the sheet image cannot be drawn.");
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
    // Pedal highlights, drawn as the page draws them: behind the text's cell.
    struct PedalHighlightRect { float x0; float x1; float y; int level; };
    std::vector<PedalHighlightRect> pedalHighlights;
    Gdiplus::FontFamily family;
    regular.GetFamily(&family);
    const float textBottom = fontPx * static_cast<float>(family.GetCellAscent(Gdiplus::FontStyleRegular) +
        family.GetCellDescent(Gdiplus::FontStyleRegular)) / static_cast<float>(family.GetEmHeight(Gdiplus::FontStyleRegular));
    float y = pad, widest = 0;
    // The heading's lines as placed, with their widths, to centre once the
    // picture's width is known.
    std::vector<std::pair<size_t, float>> headingPlaced;
    for (const auto& text : heading.lines) {
        const std::wstring line = detail::Wide(text);
        if (line.empty()) continue;
        // Wrapped at spaces to the sheet's width, and inside a word too long for it.
        std::vector<std::wstring> wrapped(1);
        size_t at = 0;
        while (at < line.size()) {
            const size_t space = line.find(L' ', at);
            const std::wstring word = line.substr(at, space == std::wstring::npos ? std::wstring::npos : space + 1 - at);
            at = space == std::wstring::npos ? line.size() : space + 1;
            if (!wrapped.back().empty() && widthOf(wrapped.back() + word, false) > wrapWidth) wrapped.emplace_back();
            for (const wchar_t c : word) {
                if (!wrapped.back().empty() && c != L' ' && widthOf(wrapped.back() + c, false) > wrapWidth) wrapped.emplace_back();
                wrapped.back() += c;
            }
        }
        for (const auto& part : wrapped) {
            const float partWidth = widthOf(part, false);
            headingPlaced.push_back({placed.size(), partWidth});
            placed.push_back({pad, y, {part, detail::ColourFromCss(look.heading), false}});
            widest = (std::max)(widest, partWidth);
            y += lineHeight;
        }
    }
    if (!headingPlaced.empty()) y += lineHeight;
    for (const auto& line : detail::ImageLines(r, look)) {
        float x = pad;
        for (const auto& word : line.words) {
            float wordWidth = 0;
            for (const auto& run : word.runs) wordWidth += widthOf(run.text, run.outOfRange);
            if (x > pad && x - pad + wordWidth > wrapWidth) { x = pad; y += lineHeight; }
            for (const auto& run : word.runs) {
                placed.push_back({x, y, run});
                const float width = widthOf(run.text, run.outOfRange);
                if (run.pedalHighlight >= 0) pedalHighlights.push_back({x, x + width, y, run.pedalHighlight});
                x += width;
            }
            widest = (std::max)(widest, x - pad);
        }
        y += lineHeight;
    }
    const int width = static_cast<int>(std::ceil((std::min)(widest, wrapWidth) + 2 * pad));
    const int height = static_cast<int>(std::ceil(y + pad));
    if (heading.centre)
        for (const auto& [index, lineWidth] : headingPlaced) placed[index].x = pad + (std::max)(0.0f, ((std::min)(widest, wrapWidth) - lineWidth) / 2);

    Gdiplus::Bitmap bitmap((std::max)(width, 1), (std::max)(height, 1), PixelFormat32bppARGB);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) throw std::runtime_error("The sheet is too long for one picture.");
    Gdiplus::Graphics g(&bitmap);
    detail::PaintBackground(g, look, width, height, scale);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::SolidBrush downBrush(detail::ColourFromCss(look.pedalDown)), upBrush(detail::ColourFromCss(look.pedalUp));
    for (const auto& band : pedalHighlights) {
        // Part way down fills the lower half, as the page's gradient does.
        const float top = band.level == 1 ? band.y + textBottom / 2 : band.y;
        g.FillRectangle(band.level > 0 ? &downBrush : &upBrush, band.x0, top, band.x1 - band.x0, band.y + textBottom - top);
    }
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

// With the title and difficulty over it, as before the header could say more.
inline std::pair<int, int> SavePng(const StyledResult& r, const std::string& title, const Look& look,
                                   const std::filesystem::path& file, double scale = 2, int maxWidth = 1100) {
    HeadingBlock heading;
    if (auto line = detail::Heading(title, r.difficulty); !line.empty()) heading.lines.push_back(std::move(line));
    return SavePng(r, heading, look, file, scale, maxWidth);
}

} // namespace sheet
