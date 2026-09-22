#pragma once

// Skin: the built-in visual parameter sets as runtime data.
//
// No ImGui, GDI+ or Win32 dependency, so tests can include it. Nothing is
// constexpr because skins switch at runtime and user themes are further instances.
//
// Design rules:
//   * Surface tiers: canvas, structure, card, elevated, and recessed (inputs,
//     lists, tracks, fill areas).
//   * Raised surfaces get two shadows and a 1 px top highlight; recessed
//     surfaces get an inner shadow. Borders are translucent hairlines.
//   * 4 px spacing scale with concentric radii: an element inset 4 px inside a
//     12 px container has an 8 px radius.
//   * Accent marks selection, focus, slider fills and the live curve, and is
//     never the only indicator of a state.

#include <array>
#include <cstdint>
#include <string_view>

namespace skin {

// 0xAARRGGBB, as GDI+ and ImGui's ImColor take a packed value.
using Argb = uint32_t;

constexpr Argb Rgb(uint32_t rgb) { return 0xFF000000u | rgb; }
constexpr Argb Rgba(uint32_t rgb, double alpha) {
    return (static_cast<uint32_t>(alpha * 255.0 + 0.5) << 24) | rgb;
}

// One shadow layer. Raised surfaces use two: a tight contact shadow and a soft
// ambient one, drawn as a few stacked translucent rounded rects.
struct Shadow {
    float offsetY;
    float blur;
    Argb  colour;
};

struct Surfaces {
    Argb canvas;      // window ground
    Argb structure;   // strip, status bar
    Argb card;
    Argb elevated;    // controls
    Argb elevatedHot; // hover
    Argb recessed;    // fields, lists, tracks, slider grooves
};

struct Ink {
    Argb primary;
    Argb secondary;
    Argb tertiary;
};

struct Accents {
    Argb accent, accentSoft, accentLine;
    Argb ok, okInk, okSoft, okBorder;
    Argb warn, bad;
};

struct Borders {
    Argb hairline;    // ordinary edges
    Argb strong;      // popovers and anything floating
    Argb topHighlight;
};

struct Radii {
    float window, card, control, element;
};

// The 4 px spacing scale; all UI spacing comes from it.
struct Spacing {
    float s1 = 4.f, s2 = 8.f, s3 = 12.f, s4 = 16.f, s6 = 24.f;
    float windowPad, panelPad;
};

struct Metrics {
    float controlHeight;
    float innerHeight;    // for elements nested inside a control
};

struct TypeScale {
    float meta, body, heading, title;
    std::string_view family;
};

struct Skin {
    std::string_view name;
    bool     dark;
    Surfaces surface;
    Ink      ink;
    Accents  accent;
    Borders  border;
    Radii    radius;
    Spacing  spacing;
    Metrics  metric;
    TypeScale type;
    Shadow   contact;   // tight, defines the edge
    Shadow   ambient;   // soft, conveys elevation
    Shadow   inner;     // recessed surfaces
    // Uniform UI scale, applied like a DPI factor to everything including the window.
    float    scale = 1.f;
};

// Geometry shared by every built-in skin; built-in skins differ only in colour.
// User themes may change these within ThemeNumbers' ranges.
inline void Shape(Skin& s) {
    s.radius  = { 12.f, 12.f, 10.f, 8.f };
    s.spacing = { 4.f, 8.f, 12.f, 16.f, 24.f, 16.f, 16.f };
    s.metric  = { 32.f, 24.f };
    s.type    = { 12.f, 14.f, 14.f, 20.f, "IBM Plex Sans" };
}

// Skins are named for their accent colour.
inline Skin Blue() {
    Skin s{};
    s.name = "Blue";
    s.dark = false;
    Shape(s);
    // No surface is pure white; the tiers keep their relative order for depth.
    s.surface = { Rgb(0xE0E4E9), Rgb(0xEBEEF2), Rgb(0xF5F7F9),
                  Rgb(0xFAFBFC), Rgb(0xF0F3F6), Rgb(0xE3E7EC) };
    s.ink = { Rgb(0x1B1E24), Rgb(0x5B636F), Rgb(0x8B929E) };
    s.accent = { Rgb(0x0B6EC4), Rgba(0x0B6EC4, .10), Rgba(0x0B6EC4, .35),
                 Rgb(0x1A7F45), Rgb(0x12652F), Rgba(0x1A7F45, .12), Rgba(0x1A7F45, .34),
                 Rgb(0x9A6C0D), Rgb(0xB23F2F) };
    s.border = { Rgba(0x11161F, .09), Rgba(0x11161F, .16), Rgba(0xFFFFFF, .85) };
    // Shadow geometry is the same in every skin; only the colour varies.
    s.contact = { 1.f, 1.f, Rgba(0x11161F, .05) };
    s.ambient = { 2.f, 6.f, Rgba(0x11161F, .04) };
    s.inner   = { 1.f, 2.f, Rgba(0x11161F, .09) };
    return s;
}

inline Skin BlueDark() {
    Skin s = Blue();
    s.name = "Blue Dark";
    s.dark = true;
    s.surface = { Rgb(0x191C21), Rgb(0x20242A), Rgb(0x272B31),
                  Rgb(0x2E333A), Rgb(0x363C44), Rgb(0x14171B) };
    s.ink = { Rgb(0xE7E9EC), Rgb(0xA3AAB5), Rgb(0x79818D) };
    s.accent = { Rgb(0x4EA3EA), Rgba(0x4EA3EA, .14), Rgba(0x4EA3EA, .40),
                 Rgb(0x4FB277), Rgb(0x8FDCAA), Rgba(0x4FB277, .16), Rgba(0x4FB277, .40),
                 Rgb(0xD0A259), Rgb(0xE0685A) };
    s.border = { Rgba(0xFFFFFF, .08), Rgba(0xFFFFFF, .14), Rgba(0xFFFFFF, .06) };
    s.contact = { 1.f, 1.f, Rgba(0x000000, .30) };
    s.ambient = { 2.f, 6.f, Rgba(0x000000, .22) };
    s.inner   = { 1.f, 2.f, Rgba(0x000000, .45) };
    return s;
}

inline Skin Orange() {
    Skin s{};
    s.name = "Orange";
    s.dark = false;
    Shape(s);
    s.surface = { Rgb(0xDFDAD2), Rgb(0xEAE6E0), Rgb(0xF5F3EF),
                  Rgb(0xFAF8F5), Rgb(0xEFEBE5), Rgb(0xE5E0D8) };
    s.ink = { Rgb(0x1D1B19), Rgb(0x6F675E), Rgb(0x948B81) };
    s.accent = { Rgb(0xB5443A), Rgba(0xB5443A, .10), Rgba(0xB5443A, .32),
                 Rgb(0x3F7A55), Rgb(0x245939), Rgba(0x3F7A55, .12), Rgba(0x3F7A55, .30),
                 Rgb(0xA8781F), Rgb(0xB5443A) };
    s.border = { Rgba(0x2D261E, .09), Rgba(0x2D261E, .16), Rgba(0xFFFFFF, .90) };
    s.contact = { 1.f, 1.f, Rgba(0x2D261E, .05) };
    s.ambient = { 2.f, 6.f, Rgba(0x2D261E, .04) };
    s.inner   = { 1.f, 2.f, Rgba(0x2D261E, .08) };
    return s;
}

inline Skin OrangeDark() {
    Skin s = Orange();
    s.name = "Orange Dark";
    s.dark = true;
    s.surface = { Rgb(0x17140F), Rgb(0x1F1B15), Rgb(0x26221B),
                  Rgb(0x2E2921), Rgb(0x372F26), Rgb(0x120F0B) };
    s.ink = { Rgb(0xEDE9E4), Rgb(0x9D938A), Rgb(0x7B736B) };
    s.accent = { Rgb(0xE0705F), Rgba(0xE0705F, .14), Rgba(0xE0705F, .36),
                 Rgb(0x6FBF8D), Rgb(0x8FD0A8), Rgba(0x6FBF8D, .14), Rgba(0x6FBF8D, .34),
                 Rgb(0xC99A4A), Rgb(0xD4574B) };
    s.border = { Rgba(0xFFFFFF, .07), Rgba(0xFFFFFF, .13), Rgba(0xFFFFFF, .05) };
    s.contact = { 1.f, 1.f, Rgba(0x000000, .35) };
    s.ambient = { 2.f, 6.f, Rgba(0x000000, .25) };
    s.inner   = { 1.f, 2.f, Rgba(0x000000, .50) };
    return s;
}

// Order is load-bearing: preferences store the index, `skin ^= 1` toggles
// light/dark, and `skin >= 2` selects the colour. Keep each light/dark pair adjacent.
inline std::array<Skin, 4> All() {
    return { Blue(), BlueDark(), Orange(), OrangeDark() };
}

// Piano key colours are fixed across skins; only the selection highlight is themed.
namespace piano {
    inline constexpr Argb WhiteKeyTop    = 0xFFFFFEFB;
    inline constexpr Argb WhiteKeyBottom = 0xFFF2EEE5;
    inline constexpr Argb WhiteKeyEdge   = 0xFFB6B0A4;
    inline constexpr Argb BlackKeyTop    = 0xFF2C2926;
    inline constexpr Argb BlackKeyBottom = 0xFF131110;
    inline constexpr Argb Case           = 0xFF1F1C1A;
}

} // namespace skin
