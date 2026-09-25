#pragma once
// Themes: a named light/dark pair of skins, built-in or user-defined.
//
// No ImGui or Win32 dependency, so tests can include it. A custom theme is
// colours plus shape (corners, spacing, sizes, shadows), each clamped to a
// range the layout supports. Themes are pure data and can be exported.

#include "../engine/Skin.hpp"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace shell {

// Each skin colour with its themes.json key and editor label. `derived`
// colours (translucent accents, borders, shadows) are grouped under one
// collapsed row in the editor.
struct ThemeColour {
    const char* key;
    const char* label;
    const char* group;
    bool derived;
    skin::Argb& (*at)(skin::Skin&);
};

inline const std::array<ThemeColour, 24>& ThemeColours() {
    using S = skin::Skin;
    static const std::array<ThemeColour, 24> colours{{
        {"canvas", "Window", "Background", false, [](S& s) -> skin::Argb& { return s.surface.canvas; }},
        {"structure", "Bars", "Background", false, [](S& s) -> skin::Argb& { return s.surface.structure; }},
        {"card", "Panels", "Background", false, [](S& s) -> skin::Argb& { return s.surface.card; }},
        {"elevated", "Controls", "Background", false, [](S& s) -> skin::Argb& { return s.surface.elevated; }},
        {"elevatedHot", "Hover", "Background", false, [](S& s) -> skin::Argb& { return s.surface.elevatedHot; }},
        {"recessed", "Fields", "Background", false, [](S& s) -> skin::Argb& { return s.surface.recessed; }},
        {"inkPrimary", "Text", "Text", false, [](S& s) -> skin::Argb& { return s.ink.primary; }},
        {"inkSecondary", "Secondary text", "Text", false, [](S& s) -> skin::Argb& { return s.ink.secondary; }},
        {"inkTertiary", "Faint text", "Text", false, [](S& s) -> skin::Argb& { return s.ink.tertiary; }},
        {"accent", "Accent", "Colour", false, [](S& s) -> skin::Argb& { return s.accent.accent; }},
        {"ok", "On", "Colour", false, [](S& s) -> skin::Argb& { return s.accent.ok; }},
        {"okInk", "On text", "Colour", false, [](S& s) -> skin::Argb& { return s.accent.okInk; }},
        {"warn", "Warning", "Colour", false, [](S& s) -> skin::Argb& { return s.accent.warn; }},
        {"bad", "Error", "Colour", false, [](S& s) -> skin::Argb& { return s.accent.bad; }},
        {"accentSoft", "Accent fill", "Fine detail", true, [](S& s) -> skin::Argb& { return s.accent.accentSoft; }},
        {"accentLine", "Accent outline", "Fine detail", true, [](S& s) -> skin::Argb& { return s.accent.accentLine; }},
        {"okSoft", "On fill", "Fine detail", true, [](S& s) -> skin::Argb& { return s.accent.okSoft; }},
        {"okBorder", "On outline", "Fine detail", true, [](S& s) -> skin::Argb& { return s.accent.okBorder; }},
        {"hairline", "Edges", "Fine detail", true, [](S& s) -> skin::Argb& { return s.border.hairline; }},
        {"strong", "Popup edges", "Fine detail", true, [](S& s) -> skin::Argb& { return s.border.strong; }},
        {"topHighlight", "Top highlight", "Fine detail", true, [](S& s) -> skin::Argb& { return s.border.topHighlight; }},
        {"contact", "Edge shadow", "Fine detail", true, [](S& s) -> skin::Argb& { return s.contact.colour; }},
        {"ambient", "Soft shadow", "Fine detail", true, [](S& s) -> skin::Argb& { return s.ambient.colour; }},
        {"inner", "Inset shadow", "Fine detail", true, [](S& s) -> skin::Argb& { return s.inner.colour; }},
    }};
    return colours;
}

// Each skin measure a theme may change, with its key, label and allowed range.
// Values are clamped on every load so no file can break the layout; the render
// tests cover both ends of each range. The spacing scale, font family and
// inner height are not themeable.
struct ThemeNumber {
    const char* key;
    const char* label;
    const char* group;
    float low, high, step;
    float& (*at)(skin::Skin&);
};

inline const std::array<ThemeNumber, 18>& ThemeNumbers() {
    using S = skin::Skin;
    static const std::array<ThemeNumber, 18> numbers{{
        {"scale", "Size", "Size", .85f, 1.25f, .05f, [](S& s) -> float& { return s.scale; }},
        {"controlHeight", "Controls", "Size", 28, 38, 1, [](S& s) -> float& { return s.metric.controlHeight; }},
        {"textBody", "Text", "Size", 13, 15, 1, [](S& s) -> float& { return s.type.body; }},
        {"textMeta", "Small text", "Size", 11, 13, 1, [](S& s) -> float& { return s.type.meta; }},
        {"textHeading", "Headings", "Size", 13, 16, 1, [](S& s) -> float& { return s.type.heading; }},
        {"textTitle", "Titles", "Size", 18, 22, 1, [](S& s) -> float& { return s.type.title; }},
        {"radiusWindow", "Window", "Corners", 0, 16, 1, [](S& s) -> float& { return s.radius.window; }},
        {"radiusCard", "Panels", "Corners", 0, 16, 1, [](S& s) -> float& { return s.radius.card; }},
        {"radiusControl", "Controls", "Corners", 0, 16, 1, [](S& s) -> float& { return s.radius.control; }},
        {"radiusElement", "Small parts", "Corners", 0, 12, 1, [](S& s) -> float& { return s.radius.element; }},
        {"windowPad", "Window edge", "Spacing", 8, 24, 1, [](S& s) -> float& { return s.spacing.windowPad; }},
        {"panelPad", "Panel edge", "Spacing", 8, 24, 1, [](S& s) -> float& { return s.spacing.panelPad; }},
        {"contactOffset", "Edge shadow drop", "Shadow", 0, 3, 1, [](S& s) -> float& { return s.contact.offsetY; }},
        {"contactBlur", "Edge shadow spread", "Shadow", 0, 4, 1, [](S& s) -> float& { return s.contact.blur; }},
        {"ambientOffset", "Soft shadow drop", "Shadow", 0, 6, 1, [](S& s) -> float& { return s.ambient.offsetY; }},
        {"ambientBlur", "Soft shadow spread", "Shadow", 0, 16, 1, [](S& s) -> float& { return s.ambient.blur; }},
        {"innerOffset", "Inset shadow drop", "Shadow", 0, 3, 1, [](S& s) -> float& { return s.inner.offsetY; }},
        {"innerBlur", "Inset shadow spread", "Shadow", 0, 6, 1, [](S& s) -> float& { return s.inner.blur; }},
    }};
    return numbers;
}

// Clamps each number to its range and snaps it to its step. innerHeight is
// always controlHeight - 8, as in the built-ins.
inline void ClampShape(skin::Skin& skin) {
    for (const auto& number : ThemeNumbers()) {
        float& value = number.at(skin);
        if (!std::isfinite(value)) value = number.low;
        value = std::clamp(number.low + std::round((value - number.low) / number.step) * number.step, number.low, number.high);
    }
    skin.metric.innerHeight = skin.metric.controlHeight - 8.f;
}

// Both halves of a theme share one shape.
inline void CopyShape(skin::Skin from, skin::Skin& to) {
    for (const auto& number : ThemeNumbers()) number.at(to) = number.at(from);
    to.metric.innerHeight = from.metric.innerHeight;
}

// The editor's four sliders, each setting several numbers at once. Corners
// keeps radii concentric (control = card - 2, element = card - 4). Shadow
// scales the built-in shadow geometry from 0 to 2x.
struct ThemeDial {
    const char* label;
    float low, high, step;
    float (*get)(skin::Skin&);
    void (*set)(skin::Skin&, float);
};

inline const std::array<ThemeDial, 4>& ThemeDials() {
    using S = skin::Skin;
    static const std::array<ThemeDial, 4> dials{{
        {"Corners", 0, 16, 1, [](S& s) { return s.radius.card; },
         [](S& s, float v) { s.radius.window = s.radius.card = v; s.radius.control = std::max(0.f, v - 2); s.radius.element = std::max(0.f, v - 4); }},
        {"Spacing", 8, 24, 1, [](S& s) { return s.spacing.panelPad; }, [](S& s, float v) { s.spacing.windowPad = s.spacing.panelPad = v; }},
        {"Size", .85f, 1.25f, .05f, [](S& s) { return s.scale; }, [](S& s, float v) { s.scale = v; }},
        {"Shadow", 0, 2, .25f, [](S& s) { return s.ambient.blur / 6.f; },
         [](S& s, float v) {
             s.contact.offsetY = s.contact.blur = s.inner.offsetY = v; s.ambient.offsetY = s.inner.blur = 2 * v; s.ambient.blur = 6 * v;
         }},
    }};
    return dials;
}

// OKLCH: Björn Ottosson's OKLab in polar form. L is perceptually uniform
// (unlike HSL), so derived palettes keep their tier-to-tier contrast.
struct Lch { double l, c, h; };

namespace theme_detail {
inline double ToLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
inline double FromLinear(double v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }
inline bool LinearFromLch(const Lch& lch, double& r, double& g, double& b) {
    const double a = lch.c * std::cos(lch.h), bb = lch.c * std::sin(lch.h);
    const double l_ = lch.l + 0.3963377774 * a + 0.2158037573 * bb;
    const double m_ = lch.l - 0.1055613458 * a - 0.0638541728 * bb;
    const double s_ = lch.l - 0.0894841775 * a - 1.2914855480 * bb;
    const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    r = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    const double slack = 1e-4;
    return r >= -slack && r <= 1 + slack && g >= -slack && g <= 1 + slack && b >= -slack && b <= 1 + slack;
}
}

inline Lch ToLch(skin::Argb colour) {
    using namespace theme_detail;
    const double r = ToLinear(((colour >> 16) & 0xFF) / 255.0), g = ToLinear(((colour >> 8) & 0xFF) / 255.0),
                 b = ToLinear((colour & 0xFF) / 255.0);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double bb = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    return {L, std::hypot(a, bb), std::atan2(bb, a)};
}

// Returns an opaque colour. Out-of-gamut colours are mapped by reducing chroma
// (binary search), keeping hue and lightness; per-channel clipping shifts hue.
inline skin::Argb FromLch(Lch lch) {
    using namespace theme_detail;
    lch.l = std::clamp(lch.l, 0.0, 1.0);
    double r = 0, g = 0, b = 0;
    if (!LinearFromLch(lch, r, g, b)) {
        double low = 0, high = lch.c;
        for (int i = 0; i < 24; ++i) {
            const double mid = (low + high) / 2;
            (LinearFromLch({lch.l, mid, lch.h}, r, g, b) ? low : high) = mid;
        }
        LinearFromLch({lch.l, low, lch.h}, r, g, b);
    }
    const auto channel = [](double v) {
        return static_cast<uint32_t>(std::lround(std::clamp(FromLinear(std::clamp(v, 0.0, 1.0)), 0.0, 1.0) * 255.0));
    };
    return 0xFF000000u | (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

inline skin::Argb WithAlphaOf(skin::Argb colour, skin::Argb alphaFrom) {
    return (alphaFrom & 0xFF000000u) | (colour & 0x00FFFFFFu);
}

// Built-in skin that supplies per-role lightness for derived palettes.
inline skin::Skin ThemeTemplate(bool dark) { return dark ? skin::BlueDark() : skin::Blue(); }

// Derives the opposite light/dark half. Each colour keeps its hue and chroma
// and takes the lightness of the same role in the other mode's template, which
// preserves surface ordering (inverting L would reverse it). Translucent
// accents use the template's alpha; borders and shadows come from the template.
inline skin::Skin OppositeSkin(const skin::Skin& from) {
    skin::Skin out = ThemeTemplate(!from.dark), source = from;
    const skin::Skin pattern = out;
    out.name = {};
    skin::Skin lightness = pattern;
    for (const auto& colour : ThemeColours()) {
        if (colour.derived) continue;
        Lch lch = ToLch(colour.at(source));
        lch.l = ToLch(colour.at(lightness)).l;
        colour.at(out) = FromLch(lch);
    }
    out.accent.accentSoft = WithAlphaOf(out.accent.accent, pattern.accent.accentSoft);
    out.accent.accentLine = WithAlphaOf(out.accent.accent, pattern.accent.accentLine);
    out.accent.okSoft = WithAlphaOf(out.accent.ok, pattern.accent.okSoft);
    out.accent.okBorder = WithAlphaOf(out.accent.ok, pattern.accent.okBorder);
    CopyShape(from, out);
    return out;
}

// Builds a palette from background, text and accent. Surfaces keep the
// template's lightness offsets from the canvas; secondary and tertiary ink are
// the text mixed toward the background in the template's proportions. Status
// colours (on, warning, error) stay the template's.
inline skin::Skin GenerateSkin(skin::Argb background, skin::Argb text, skin::Argb accent) {
    const Lch ground = ToLch(background), ink = ToLch(text);
    const bool dark = ground.l < 0.5;
    skin::Skin out = ThemeTemplate(dark), pattern = out;
    out.name = {};
    const double patternGround = ToLch(pattern.surface.canvas).l, patternInk = ToLch(pattern.ink.primary).l;
    for (const auto& colour : ThemeColours()) {
        const std::string group = colour.group;
        const double role = ToLch(colour.at(pattern)).l;
        if (group == "Background")
            colour.at(out) = FromLch({ground.l + role - patternGround, ground.c, ground.h});
        else if (group == "Text") {
            const double t = (role - patternInk) / (patternGround - patternInk);
            colour.at(out) = FromLch({ink.l + (ground.l - ink.l) * t, ink.c + (ground.c - ink.c) * t, ink.h});
        }
    }
    out.accent.accent = 0xFF000000u | (accent & 0x00FFFFFFu);
    out.accent.accentSoft = WithAlphaOf(accent, pattern.accent.accentSoft);
    out.accent.accentLine = WithAlphaOf(accent, pattern.accent.accentLine);
    return out;
}

// FNV-1a over every colour and measure. Never zero, so callers can use zero
// for "nothing applied".
inline uint64_t SkinSignature(skin::Skin skin) {
    uint64_t hash = 1469598103934665603ull ^ (skin.dark ? 1u : 0u);
    for (const auto& colour : ThemeColours()) hash = (hash ^ colour.at(skin)) * 1099511628211ull;
    for (const auto& number : ThemeNumbers())
        hash = (hash ^ static_cast<uint64_t>(std::lround(number.at(skin) * 100))) * 1099511628211ull;
    return hash | 1;
}

inline std::string ColourText(skin::Argb colour) {
    char text[10];
    const unsigned alpha = colour >> 24;
    if (alpha == 0xFF) std::snprintf(text, sizeof(text), "#%06X", static_cast<unsigned>(colour & 0xFFFFFF));
    else std::snprintf(text, sizeof(text), "#%06X%02X", static_cast<unsigned>(colour & 0xFFFFFF), alpha);
    return text;
}

inline bool ParseColour(const std::string& text, skin::Argb& colour) {
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#') return false;
    uint32_t value = 0;
    for (size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        value = value << 4 | static_cast<uint32_t>(digit);
    }
    colour = text.size() == 7 ? 0xFF000000u | value : (value & 0xFF) << 24 | value >> 8;
    return true;
}

struct Theme {
    std::string id, name;
    bool builtin = false;
    // Unpaired: a single palette, light or dark per `onlyDark`. Paired and
    // automatic: the non-source half is OppositeSkin of the source half.
    bool paired = true, automatic = true, onlyDark = false;
    skin::Skin light, dark;
    // Which half is the source for automatic derivation; set to the visible
    // half when automatic is enabled.
    bool sourceDark = false;
    bool ShowsDark(bool wantDark) const { return paired ? wantDark : onlyDark; }
    const skin::Skin& Shown(bool wantDark) const { return ShowsDark(wantDark) ? dark : light; }
    skin::Skin& Shown(bool wantDark) { return ShowsDark(wantDark) ? dark : light; }
    void Derive() { (sourceDark ? light : dark) = OppositeSkin(sourceDark ? dark : light); }
    // Call after editing the visible half. Editing the source re-derives the
    // other half; editing the derived half turns automatic off so the edit sticks.
    void Edited(bool wantDark) {
        if (!paired || !automatic) return;
        if (ShowsDark(wantDark) == sourceDark) Derive();
        else automatic = false;
    }
    void SetAutomatic(bool on, bool wantDark) {
        automatic = on;
        if (on && paired) { sourceDark = ShowsDark(wantDark); Derive(); }
    }
    // Call after changing a measure: clamps it and copies the shape to the other half.
    void Reshaped(bool wantDark) {
        ClampShape(Shown(wantDark));
        CopyShape(Shown(wantDark), ShowsDark(wantDark) ? light : dark);
    }
};

// Sanitizes a theme name from a file: drops control characters, trims spaces,
// and truncates to 48 bytes on a UTF-8 character boundary.
inline std::string ThemeNameFrom(const std::string& text) {
    std::string name;
    for (const char c : text) if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) name += c;
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.size() > 48) {
        size_t cut = 48;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
        name.resize(cut);
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    return name;
}

class ThemeStore {
public:
    ThemeStore() {
        themes_.push_back({"blue", "Blue", true, true, false, false, skin::Blue(), skin::BlueDark(), false});
        themes_.push_back({"orange", "Orange", true, true, false, false, skin::Orange(), skin::OrangeDark(), false});
    }
    const std::vector<Theme>& All() const { return themes_; }
    Theme* Find(const std::string& id) {
        const auto found = std::find_if(themes_.begin(), themes_.end(), [&](const Theme& theme) { return theme.id == id; });
        return found == themes_.end() ? nullptr : &*found;
    }
    // Falls back to Blue when id is unknown.
    Theme& Active(const std::string& id) { auto* found = Find(id); return found ? *found : themes_.front(); }
    const Theme& Active(const std::string& id) const { return const_cast<ThemeStore*>(this)->Active(id); }

    // Adds an editable copy of `from` with a fresh custom-N id.
    Theme& Add(const Theme& from, std::string name) {
        Theme theme = from;
        theme.builtin = false;
        theme.name = std::move(name);
        theme.light.name = theme.dark.name = {};
        int number = 1;
        do theme.id = "custom-" + std::to_string(number++); while (Find(theme.id));
        themes_.push_back(std::move(theme));
        return themes_.back();
    }
    bool Remove(const std::string& id) {
        const auto found = std::find_if(themes_.begin(), themes_.end(), [&](const Theme& theme) { return theme.id == id && !theme.builtin; });
        if (found == themes_.end()) return false;
        themes_.erase(found);
        return true;
    }

    // Serialized form used by both themes.json and exported theme files.
    static nlohmann::json ThemeJson(const Theme& theme) {
        const auto palette = [](skin::Skin skin) {
            nlohmann::json colours = nlohmann::json::object();
            for (const auto& colour : ThemeColours()) colours[colour.key] = ColourText(colour.at(skin));
            return colours;
        };
        nlohmann::json entry{{"id", theme.id}, {"name", theme.name}, {"paired", theme.paired}};
        if (theme.paired) {
            entry["automatic"] = theme.automatic;
            entry["source"] = theme.sourceDark ? "dark" : "light";
            entry["light"] = palette(theme.light);
            entry["dark"] = palette(theme.dark);
        }
        else entry[theme.onlyDark ? "dark" : "light"] = palette(theme.onlyDark ? theme.dark : theme.light);
        nlohmann::json shape = nlohmann::json::object();
        skin::Skin shaped = theme.light;
        for (const auto& number : ThemeNumbers()) shape[number.key] = number.at(shaped);
        entry["shape"] = std::move(shape);
        return entry;
    }
    // Missing or invalid colours and measures keep the template's values;
    // measures are clamped. A theme without a "shape" gets the built-in shape.
    static bool ReadTheme(const nlohmann::json& entry, Theme& theme) {
        if (!entry.is_object()) return false;
        const auto text = [&](const char* key) {
            const auto found = entry.find(key);
            return found != entry.end() && found->is_string() ? found->get<std::string>() : std::string();
        };
        const auto flag = [&](const char* key, bool otherwise) {
            const auto found = entry.find(key);
            return found != entry.end() && found->is_boolean() ? found->get<bool>() : otherwise;
        };
        theme.id = text("id");
        theme.name = ThemeNameFrom(text("name"));
        if (theme.name.empty()) return false;
        theme.paired = flag("paired", true);
        theme.automatic = flag("automatic", false);
        theme.sourceDark = text("source") == "dark";
        const auto shape = entry.find("shape");
        const auto palette = [&](const char* half, bool dark) {
            skin::Skin skin = ThemeTemplate(dark);
            skin.name = {};
            const auto colours = entry.find(half);
            if (colours != entry.end() && colours->is_object())
                for (const auto& colour : ThemeColours()) {
                    const auto value = colours->find(colour.key);
                    // Only the fine-detail colours may be translucent, as in the
                    // editor; a see-through text or surface colour would hide the UI.
                    if (value != colours->end() && value->is_string() && ParseColour(value->get<std::string>(), colour.at(skin)) &&
                        !colour.derived)
                        colour.at(skin) |= 0xFF000000u;
                }
            if (shape != entry.end() && shape->is_object())
                for (const auto& number : ThemeNumbers()) {
                    const auto value = shape->find(number.key);
                    if (value != shape->end() && value->is_number()) number.at(skin) = value->get<float>();
                }
            ClampShape(skin);
            return skin;
        };
        theme.light = palette("light", false);
        theme.dark = palette("dark", true);
        theme.onlyDark = !theme.paired && entry.contains("dark") && !entry.contains("light");
        return true;
    }

    nlohmann::json ToJson() const {
        auto list = nlohmann::json::array();
        for (const auto& theme : themes_) if (!theme.builtin) list.push_back(ThemeJson(theme));
        return {{"themes", std::move(list)}};
    }
    // Invalid or duplicate entries are skipped.
    void FromJson(const nlohmann::json& json) {
        themes_.erase(std::remove_if(themes_.begin(), themes_.end(), [](const Theme& theme) { return !theme.builtin; }), themes_.end());
        const auto list = json.find("themes");
        if (list == json.end() || !list->is_array()) return;
        for (const auto& entry : *list) {
            Theme theme;
            if (!ReadTheme(entry, theme) || theme.id.empty() || Find(theme.id)) continue;
            themes_.push_back(std::move(theme));
        }
    }

    // Exported theme file: the theme's JSON (without id) under a format marker.
    static constexpr const char* kThemeFileMark = "quartzmidi-theme";
    static bool Export(const Theme& theme, const std::filesystem::path& path) {
        nlohmann::json entry = ThemeJson(theme);
        entry.erase("id");
        std::ofstream stream(path);
        stream << nlohmann::json{{kThemeFileMark, 1}, {"theme", std::move(entry)}}.dump(2) << '\n';
        return static_cast<bool>(stream);
    }
    // Adds the file's theme as a custom theme, suffixing the name to keep it
    // unique. Returns null if the file is not a valid theme.
    Theme* Import(const std::filesystem::path& path) {
        try {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > 64 * 1024) return nullptr;
            std::ifstream stream(path);
            const auto json = nlohmann::json::parse(stream);
            Theme theme;
            if (!json.is_object() || !json.contains(kThemeFileMark) || !json.contains("theme") || !ReadTheme(json["theme"], theme)) return nullptr;
            std::string name = theme.name;
            for (int number = 2; std::any_of(themes_.begin(), themes_.end(), [&](const Theme& other) { return other.name == name; }); ++number)
                name = theme.name + " " + std::to_string(number);
            return &Add(theme, std::move(name));
        } catch (const std::exception&) { return nullptr; }
    }
    void Load(const std::filesystem::path& path) {
        try {
            std::ifstream stream(path);
            if (stream) FromJson(nlohmann::json::parse(stream));
        } catch (const std::exception&) {}
    }
    bool Save(const std::filesystem::path& path) const {
        auto temporary = path; temporary += L".tmp";
        {
            std::ofstream stream(temporary);
            stream << ToJson().dump(2) << '\n';
            // Close before checking: the last block is written, and can fail, only here.
            stream.close();
            if (!stream) { std::error_code ignored; std::filesystem::remove(temporary, ignored); return false; }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        return !error;
    }
private:
    std::vector<Theme> themes_;
};
}
