#include "Panels.hpp"
#include "IconData.hpp"
#include "GameKeyMaps.hpp"
#include "imgui_internal.h"
#include "json.hpp"
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <fstream>
#include <cmath>
#include <numeric>
#include "MidiInput.hpp"
#include "MidiOutput.hpp"
#include "config.hpp"
#include "../engine/AudioToMidi.hpp"

namespace shell {
namespace {
ImU32 Colour(skin::Argb c) { return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, (c >> 24) & 255); }
// Colour for custom drawing inside BeginDisabled, which only fades what ImGui
// draws itself; applies the same style alpha. Outside a disabled block it
// returns Colour unchanged.
ImU32 Faded(skin::Argb c) {
    const float alpha = std::clamp(ImGui::GetStyle().Alpha, 0.f, 1.f);
    return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, static_cast<int>(((c >> 24) & 255) * alpha + .5f));
}
ImU32 OpaqueTint(skin::Argb tint, skin::Argb surface) {
    const unsigned alpha = tint >> 24;
    const auto channel = [&](int shift) { return (((tint >> shift) & 255) * alpha +
        ((surface >> shift) & 255) * (255 - alpha) + 127) / 255; };
    return IM_COL32(channel(16), channel(8), channel(0), 255);
}
// An ImGui colour (IM_COL32 order) as the skin's 0xAARRGGBB.
skin::Argb ToArgb(ImU32 c) { return (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF); }
skin::Argb StyleArgb(ImGuiCol idx) { return ToArgb(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(idx))); }

// Control state changes (hover, on and off, selection, open) ease out over
// 160 ms. Only the drawing eases: state, hit areas and presses change at once.
// Set while an eased value is still moving, so the on-demand renderer keeps
// drawing until it settles.
bool g_motion = false;
// Frame on which every eased value takes its target (SettleMotion).
int g_settleFrame = -1;

// Eases toward `target` by an exponential approach that is within 1% of it
// after 155 ms at any frame rate, and snaps there. A value that wasn't drawn
// last frame (a popup reopened, a row scrolled in) starts at its target. The
// keys are hashed from `id`, so they never meet ImGui's own entries in the
// window's storage.
float Ease(ImGuiID id, float target) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID valueKey = ImHashStr("motion", 0, id), frameKey = ImHashStr("motion-frame", 0, id);
    const int frame = ImGui::GetFrameCount(), stamp = storage->GetInt(frameKey, -2);
    float value = storage->GetFloat(valueKey, target);
    if (stamp == frame) return value;
    if (stamp < frame - 1 || frame == g_settleFrame) value = target;
    else {
        value += (target - value) * (1 - std::exp(-30.f * ImGui::GetIO().DeltaTime));
        if (std::abs(target - value) < .01f) value = target;
        else g_motion = true;
    }
    storage->SetFloat(valueKey, value);
    storage->SetInt(frameKey, frame);
    return value;
}

// A fill that ImGui draws is chosen before its item exists, so it eases toward
// whether the item was hovered last frame, as NoteHover recorded after it.
float EaseHover(ImGuiID key) {
    const bool was = ImGui::GetStateStorage()->GetInt(ImHashStr("hovered", 0, key), -2) == ImGui::GetFrameCount() - 1;
    return Ease(ImHashStr("hot", 0, key), was ? 1.f : 0.f);
}
void NoteHover(ImGuiID key) {
    if (ImGui::IsItemHovered()) ImGui::GetStateStorage()->SetInt(ImHashStr("hovered", 0, key), ImGui::GetFrameCount());
}

// Blends in premultiplied alpha, so a transparent end only fades the other
// colour instead of darkening it. The ends come back exactly, so a settled
// control draws as it would without motion.
skin::Argb Mix(skin::Argb a, skin::Argb b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    const float from = (a >> 24) / 255.f, to = (b >> 24) / 255.f, alpha = from + (to - from) * t;
    if (alpha <= 0) return 0;
    const auto channel = [&](int shift) {
        const float mixed = (((a >> shift) & 255) * from * (1 - t) + ((b >> shift) & 255) * to * t) / alpha;
        return static_cast<skin::Argb>(mixed + .5f) << shift;
    };
    return (static_cast<skin::Argb>(alpha * 255 + .5f) << 24) | channel(16) | channel(8) | channel(0);
}

// Scales the alpha of everything drawn into `draw` since vertex `first`, to fade
// drawing that has no single colour, such as a shadowed shape.
void FadeVertices(ImDrawList* draw, int first, float alpha) {
    for (int i = first; i < draw->VtxBuffer.Size; ++i) {
        ImU32& colour = draw->VtxBuffer[i].col;
        const auto faded = static_cast<ImU32>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        colour = (colour & ~IM_COL32_A_MASK) | (faded << IM_COL32_A_SHIFT);
    }
}

// Browser CSS sizes use the font em; stb_truetype uses ascent minus descent.
// The shipped IBM Plex hhea/head ratio is 1300/1000.
float SpecFontScale(const skin::Skin&) { return 1.3f; }
enum class Icon { Folder, Open, Refresh, Settings, Sun, Moon, Play, Pause, Back, Forward,
                  Minus, Plus, Left, Right, Down, Up, Close, Keyboard, Speaker, Muted, Solo, Piano,
                  Mini, Expand, Copy, Rename, Check, SortDown, SortUp, Undo, Redo, Anchor, Clear,
                  Draw, Delete,
                  Hold, Tap, Audio, Discord, Roblox, Help };

// Icons are Lucide, flattened to polylines by tools/gen-icons.py into
// ui/IconData.hpp. Vector rather than a raster atlas so they stay crisp at
// 150% and 200%. `turn` rotates the icon clockwise about its centre, in radians.
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 min, float side, ImU32 ink, float dpi, float turn = 0) {
    const auto& glyph = icon_data::kGlyphs[static_cast<int>(icon)];
    const float scale = side / (icon_data::kGrid * icon_data::kUnit);
    // Lucide strokes at width 2 on a 24-unit grid. Clamp to one pixel; thinner
    // strokes render as a grey smear.
    const float thickness = std::max(1.f, side * icon_data::kStrokeWidth / icon_data::kGrid);
    const float cosine = std::cos(turn), sine = std::sin(turn), half = side / 2;
    const auto point = [&](const short* xy) {
        const ImVec2 at(min.x + xy[0] * scale, min.y + xy[1] * scale);
        if (turn == 0) return at;
        const float dx = at.x - min.x - half, dy = at.y - min.y - half;
        return ImVec2(min.x + half + dx * cosine - dy * sine, min.y + half + dx * sine + dy * cosine);
    };
    for (unsigned short p = 0; p < glyph.count; ++p) {
        const auto& path = icon_data::kPaths[glyph.first + p];
        // Lucide dots flatten to a single point, and a one-point stroke draws nothing,
        // so draw them as filled circles.
        if (path.count == 1) {
            dl->AddCircleFilled(point(&icon_data::kPoints[2 * path.first]), thickness * .6f, ink);
            continue;
        }
        for (unsigned short i = 0; i < path.count; ++i)
            dl->PathLineTo(point(&icon_data::kPoints[2 * (path.first + i)]));
        dl->PathStroke(ink, path.closed ? ImDrawFlags_Closed : 0, thickness);
    }
}

// Disclosure chevron: Right turns a quarter clockwise to Down as `open` goes
// from 0 to 1. The ends draw the glyphs themselves.
void DrawChevron(ImDrawList* dl, ImVec2 min, float side, ImU32 ink, float dpi, float open) {
    if (open <= 0 || open >= 1) DrawIcon(dl, open >= 1 ? Icon::Down : Icon::Right, min, side, ink, dpi);
    else DrawIcon(dl, Icon::Right, min, side, ink, dpi, open * 1.5707963f);
}

// Brand marks are filled in the brand's colour: the first path is filled and
// later paths are cut out of it. Coverage is computed per pixel, even-odd, from
// 8x8 samples, because ImGui's concave fill mangles these shapes. It is cached
// per mark and size, so redraws cost only the rects.
void DrawMark(ImDrawList* dl, Icon icon, ImVec2 min, float side, ImU32 fill) {
    static std::map<std::pair<int, int>, std::vector<unsigned char>> cache;
    const int pixels = std::max(1, static_cast<int>(std::round(side)));
    auto [entry, fresh] = cache.try_emplace({static_cast<int>(icon), pixels});
    auto& coverage = entry->second;
    if (fresh) {
        constexpr int kSamples = 8;
        const auto& glyph = icon_data::kGlyphs[static_cast<int>(icon)];
        const float unit = icon_data::kGrid * icon_data::kUnit / pixels;
        coverage.resize(static_cast<size_t>(pixels) * pixels);
        for (int y = 0; y < pixels; ++y)
            for (int x = 0; x < pixels; ++x) {
                int inside = 0;
                for (int sy = 0; sy < kSamples; ++sy)
                    for (int sx = 0; sx < kSamples; ++sx) {
                        const float px = (x + (sx + .5f) / kSamples) * unit, py = (y + (sy + .5f) / kSamples) * unit;
                        bool in = false;
                        for (unsigned short p = 0; p < glyph.count; ++p) {
                            const auto& path = icon_data::kPaths[glyph.first + p];
                            for (unsigned short i = 0, j = path.count - 1; i < path.count; j = i++) {
                                const short* a = &icon_data::kPoints[2 * (path.first + i)];
                                const short* b = &icon_data::kPoints[2 * (path.first + j)];
                                if ((a[1] > py) != (b[1] > py) && px < a[0] + (py - a[1]) * (b[0] - a[0]) / (b[1] - a[1])) in = !in;
                            }
                        }
                        inside += in;
                    }
                coverage[static_cast<size_t>(y) * pixels + x] = static_cast<unsigned char>(inside * 255 / (kSamples * kSamples));
            }
    }
    const ImVec2 origin(std::floor(min.x), std::floor(min.y));
    const ImU32 alpha = fill >> IM_COL32_A_SHIFT & 0xFF;
    const ImDrawListFlags flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (int y = 0; y < pixels; ++y)
        for (int x = 0; x < pixels; ++x)
            if (const ImU32 c = coverage[static_cast<size_t>(y) * pixels + x] * alpha / 255)
                dl->AddRectFilled(ImVec2(origin.x + x, origin.y + y), ImVec2(origin.x + x + 1, origin.y + y + 1),
                                  (fill & ~IM_COL32_A_MASK) | (c << IM_COL32_A_SHIFT));
    dl->Flags = flags;
}

bool IconButton(const char* id, Icon icon, const char* tip, const skin::Skin& s, float dpi, bool active = false) {
    const float height = s.metric.controlHeight;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImGuiID key = ImGui::GetID(id);
    const float on = Ease(ImHashStr("on", 0, key), active ? 1.f : 0.f);
    // Hover and on ease; ButtonActive is left alone so a press shows at once.
    const ImU32 fill = Colour(Mix(Mix(StyleArgb(ImGuiCol_Button), s.accent.accentSoft, on), StyleArgb(ImGuiCol_ButtonHovered), EaseHover(key)));
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
    const bool clicked = ImGui::Button(id, ImVec2(height, height));
    ImGui::PopStyleColor(2);
    NoteHover(key);
    // The icon is custom-drawn, so fade it by hand for a disabled button.
    const ImU32 ink = Colour(Mix(s.ink.secondary, s.accent.accent, on));
    const ImU32 alpha = static_cast<ImU32>((ink >> IM_COL32_A_SHIFT & 0xFF) * ImGui::GetStyle().Alpha);
    DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(min.x + (height - 16.f * dpi) / 2,
             min.y + (height - 16.f * dpi) / 2), 16.f * dpi,
             (ink & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT), dpi);
    // An active toggle gets an outline, so its state isn't carried by colour alone;
    // this matches the selected segment of the mini Live/Autoplay control.
    if (on > 0)
        ImGui::GetWindowDrawList()->AddRect(min, ImVec2(min.x + height, min.y + height),
                                            Faded(Mix(s.accent.accent & 0xFFFFFFu, s.accent.accent, on)), s.radius.control, 0, dpi);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// Icon and label are centred as one unit. `active` marks a toggle that is on,
// styled as in IconButton. `reserve` lists every label the button can show, so
// it keeps the widest width and neighbouring controls don't shift.
bool TransportBody(const char* id, const Icon* icon, const char* label,
                   const skin::Skin& s, float dpi, bool primary, bool active = false,
                   std::initializer_list<const char*> reserve = {}) {
    ImVec2 min = ImGui::GetCursorScreenPos();
    const float pad = (primary ? 16.f : 12.f) * dpi;
    const float side = 16 * dpi;
    const float lead = icon ? side + s.spacing.s2 : 0.f;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    float reserved = labelWidth;
    for (const char* other : reserve) reserved = std::max(reserved, ImGui::CalcTextSize(other).x);
    const float width = 2 * pad + lead + reserved;
    const float frameX = min.x;
    min.x += std::floor((reserved - labelWidth) / 2);
    const ImGuiID key = ImGui::GetID(id);
    const float on = Ease(ImHashStr("on", 0, key), active ? 1.f : 0.f), hot = EaseHover(key);
    // The primary button is filled with the accent, with ink chosen from white or
    // near-black to contrast with it.
    const auto shade = [&](float by) {
        const auto channel = [&](int shift) { return static_cast<skin::Argb>(std::clamp(((s.accent.accent >> shift) & 255) * by, 0.f, 255.f)); };
        return 0xFF000000u | channel(16) << 16 | channel(8) << 8 | channel(0);
    };
    // Hover and on ease; ButtonActive is left alone so a press shows at once.
    const ImU32 fill = Colour(primary ? Mix(shade(1.f), shade(1.1f), hot)
        : Mix(Mix(StyleArgb(ImGuiCol_Button), s.accent.accentSoft, on), StyleArgb(ImGuiCol_ButtonHovered), hot));
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Colour(shade(.9f)));
        ImGui::PushStyleColor(ImGuiCol_Border, Colour(shade(1.f)));
    }
    const bool clicked = ImGui::Button(id, ImVec2(width, s.metric.controlHeight));
    ImGui::PopStyleColor(primary ? 4 : 2);
    NoteHover(key);
    auto* draw = ImGui::GetWindowDrawList();
    // By contrast ratio against the accent (WCAG relative luminance): the (16, 18,
    // 24) ink wins once the accent's luminance passes about 0.19.
    const auto linear = [&](int shift) { return static_cast<float>(theme_detail::ToLinear(((s.accent.accent >> shift) & 255) / 255.0)); };
    const float luminance = .2126f * linear(16) + .7152f * linear(8) + .0722f * linear(0);
    const bool darkInk = (luminance + .05f) / .0561f > 1.05f / (luminance + .05f);
    const ImU32 onAccent = ImGui::GetColorU32(darkInk ? IM_COL32(16, 18, 24, 255) : IM_COL32(255, 255, 255, 255));
    const ImU32 ink = primary ? onAccent : on > 0 ? Faded(Mix(StyleArgb(ImGuiCol_Text), s.accent.accent, on)) : ImGui::GetColorU32(ImGuiCol_Text);
    if (icon)
        DrawIcon(draw, *icon, ImVec2(min.x + pad, min.y + (s.metric.controlHeight - side) / 2), side, ink, dpi);
    draw->AddText(ImVec2(min.x + pad + lead, min.y + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2), ink, label);
    if (on > 0)
        draw->AddRect(ImVec2(frameX, min.y), ImVec2(frameX + width, min.y + s.metric.controlHeight),
                      Faded(Mix(s.accent.accent & 0xFFFFFFu, s.accent.accent, on)), s.radius.control, 0, dpi);
    return clicked;
}

bool TransportButton(const char* id, Icon icon, const char* label, const skin::Skin& s,
                     float dpi, bool primary = false, bool active = false,
                     std::initializer_list<const char*> reserve = {}) {
    return TransportBody(id, &icon, label, s, dpi, primary, active, reserve);
}

// Collapsible panel heading: semibold label after a chevron, no frame until
// hovered. `count`, when given, follows the name in the quiet ink.
bool DisclosureHeading(const char* id, bool open, const char* label, const std::string& count,
                       const Fonts& fonts, const skin::Skin& design, const skin::Skin& s, float dpi) {
    FontScope font(fonts, design, design.type.heading * SpecFontScale(design), Weight::Semibold);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float side = 16 * dpi, height = s.metric.controlHeight;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    const float countWidth = count.empty() ? 0 : s.spacing.s2 + ImGui::CalcTextSize(count.c_str()).x;
    const float width = side + s.spacing.s1 + labelWidth + countWidth;
    // No fill in any state, so the chevron aligns with the panel's left edge;
    // hover only brightens the chevron.
    for (const auto colour : {ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive, ImGuiCol_Border, ImGuiCol_BorderShadow})
        ImGui::PushStyleColor(colour, IM_COL32(0, 0, 0, 0));
    const bool clicked = ImGui::Button(id, ImVec2(width, height));
    ImGui::PopStyleColor(5);
    // A mouse click also takes focus; only the keyboard's shows.
    const bool keyboardFocus = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    const bool hot = ImGui::IsItemHovered() || keyboardFocus;
    const ImGuiID key = ImGui::GetID(id);
    const float spin = Ease(ImHashStr("chevron", 0, key), open ? 1.f : 0.f);
    const float lit = Ease(ImHashStr("hover", 0, key), hot ? 1.f : 0.f);
    auto* draw = ImGui::GetWindowDrawList();
    const float textY = min.y + (height - ImGui::GetTextLineHeight()) / 2;
    DrawChevron(draw, ImVec2(min.x, min.y + (height - side) / 2), side,
                ImGui::GetColorU32(Colour(Mix(s.ink.secondary, s.ink.primary, lit))), dpi, spin);
    draw->AddText(ImVec2(min.x + side + s.spacing.s1, textY), ImGui::GetColorU32(ImGuiCol_Text), label);
    if (!count.empty())
        draw->AddText(ImVec2(min.x + side + s.spacing.s1 + labelWidth + s.spacing.s2, textY),
                      ImGui::GetColorU32(Colour(s.ink.tertiary)), count.c_str());
    return clicked;
}

// Play, Pause and Cancel share one button with a fixed width.
bool PlayButton(const char* id, bool playing, int countdown, const skin::Skin& s, float dpi) {
    return TransportButton(id, playing ? Icon::Pause : countdown ? Icon::Close : Icon::Play,
        playing ? "Pause" : countdown ? "Cancel" : "Play", s, dpi, true, false, {"Play", "Pause", "Cancel"});
}

// Label-only button for the seek controls.
bool TransportButton(const char* id, const char* label, const skin::Skin& s,
                     float dpi, bool primary = false) {
    return TransportBody(id, nullptr, label, s, dpi, primary);
}

// Replaces ImGui's filled combo arrow with a stroked chevron to match the
// Lucide icons. Sized from the combo's own height so callers need no skin.
// The back label uses a true minus sign, as the transpose control does; the
// hotkey hints use a hyphen because they're set in the monospace meta face.
std::string SeekLabel(int seconds, bool forward) {
    return (forward ? "+" : "\xe2\x88\x92") + std::to_string(seconds) + "s";
}

void ComboChevron() {
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float height = max.y - min.y;
    const float side = height * .5f;
    DrawIcon(ImGui::GetWindowDrawList(), Icon::Down,
             ImVec2(max.x - side - height * .28f, min.y + (height - side) / 2),
             side, ImGui::GetColorU32(ImGuiCol_Text), 1.f);
}

// Checkable menu item with the skin's tick in the accent at its right edge.
// `width` sizes a menu that holds only ticked items; 0 spans the menu.
bool TickedItem(const char* label, bool on, const skin::Skin& s, float dpi, float width = 0) {
    const bool clicked = ImGui::Selectable(label, false, 0, ImVec2(width, 0));
    if (on) {
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const float side = 16 * dpi;
        DrawIcon(ImGui::GetWindowDrawList(), Icon::Check, ImVec2(max.x - side - 2 * dpi, min.y + (max.y - min.y - side) / 2),
                 side, Colour(s.accent.accent), dpi);
    }
    return clicked;
}

// Combo with the drawn chevron. After BeginCombo opens, the last item belongs
// to the popup, so capture the combo's rect first and draw the chevron on the
// combo's own window.
bool ChevronCombo(const char* id, const char* preview) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::CalcItemWidth(), height = ImGui::GetFrameHeight();
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_NoArrowButton);
    const float side = height * .5f;
    DrawIcon(list, Icon::Down, ImVec2(min.x + width - side - height * .28f, min.y + (height - side) / 2), side, ink, 1.f);
    return open;
}

// ImGui::Button whose hover fill eases in and out over the caller's resting fill;
// a press still shows at once. Keyed by position, since some labels change with
// their state.
bool EasedButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    const ImGuiID key = ImGui::GetCurrentWindow()->GetIDFromPos(ImGui::GetCursorScreenPos());
    const ImU32 fill = Colour(Mix(StyleArgb(ImGuiCol_Button), StyleArgb(ImGuiCol_ButtonHovered), EaseHover(key)));
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(2);
    NoteHover(key);
    return clicked;
}

// Clickable readout: left click steps down, right steps up, middle resets to
// the default. Returns true when the value should change.
bool ReadoutClick(const char* id, ImVec2 size, double step, double rest, double* value) {
    const ImGuiID key = ImGui::GetID(id);
    // Transparent at rest; the hover fill fades in over it.
    const ImU32 fill = Colour(Mix(0, StyleArgb(ImGuiCol_ButtonHovered), EaseHover(key)));
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
    const bool left = ImGui::Button(id, size);
    ImGui::PopStyleColor(2);
    NoteHover(key);
    if (left) { *value -= step; return true; }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { *value += step; return true; }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle)) { *value = rest; return true; }
    return false;
}

// Six-pixel groove with a full-size mouse/keyboard hit target. ImGui handles
// drag, focus, navigation and clamping; only the frame is drawn here. With a
// `rest`, a middle click puts the value back to it, as on the readouts.
bool Groove(const char* id, float* value, float low, float high, float width, float height,
            const skin::Skin& s, float dpi, bool thumb, std::optional<float> rest = {}) {
    const auto min = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, (height - ImGui::GetTextLineHeight()) / 2));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 0);
    for (const auto colour : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive, ImGuiCol_Border})
        ImGui::PushStyleColor(colour, IM_COL32(0, 0, 0, 0));
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::SliderFloat(id, value, low, high, "", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
    ImGui::PopStyleColor(6); ImGui::PopStyleVar(2);
    if (rest && ImGui::IsItemClicked(ImGuiMouseButton_Middle) && *value != *rest) { *value = *rest; changed = true; }
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 track(min.x, min.y + (height - 6 * dpi) / 2);
    skin::RecessedRect(draw, track, ImVec2(track.x + width, track.y + 6 * dpi), 3 * dpi, s);
    const float fill = high > low ? std::clamp((*value - low) / (high - low), 0.f, 1.f) * width : 0;
    if (fill > 0) draw->AddRectFilled(track, ImVec2(track.x + fill, track.y + 6 * dpi), Faded(s.accent.accent), 3 * dpi);
    // A groove without a resting handle fades it in on hover and out on leave.
    const float show = thumb ? 1.f : Ease(ImHashStr("thumb", 0, ImGui::GetItemID()), ImGui::IsItemActive() || ImGui::IsItemHovered() ? 1.f : 0.f);
    if (show > 0) {
        const float x = std::clamp(track.x + fill, track.x + 6 * dpi, track.x + width - 6 * dpi);
        const int first = draw->VtxBuffer.Size;
        skin::RaisedRect(draw, ImVec2(x - 6 * dpi, track.y - 4 * dpi), ImVec2(x + 6 * dpi, track.y + 10 * dpi),
                         3 * dpi, s, Faded(s.surface.elevated));
        if (show < 1) FadeVertices(draw, first, show);
    }
    return changed;
}

// Integer setting row: name, value on the right, and the same groove as the
// transpose and sustain controls. `shown` replaces the formatted number when
// the step isn't the displayed unit, such as a speed in twentieths. `rest` is
// the default a middle click restores.
bool SettingSlider(const char* label, const char* id, int* value, int low, int high, const char* format,
                   const skin::Skin& s, float dpi, const char* shown = nullptr, std::optional<int> rest = {}) {
    char text[32]; snprintf(text, sizeof(text), format, *value);
    if (shown) snprintf(text, sizeof(text), "%s", shown);
    const float width = ImGui::GetContentRegionAvail().x;
    const float left = ImGui::GetCursorPosX();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(); ImGui::SetCursorPosX(left + width - ImGui::CalcTextSize(text).x);
    ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    float position = static_cast<float>(*value);
    Groove(id, &position, static_cast<float>(low), static_cast<float>(high), width, 22 * dpi, s, dpi, true,
           rest ? std::optional<float>(static_cast<float>(*rest)) : std::nullopt);
    const int next = std::clamp(static_cast<int>(std::lround(position)), low, high);
    if (next == *value) return false;
    *value = next;
    return true;
}

// Slider seeded with an estimate. The estimate stays as a tick labelled
// Estimated once the user moves the handle. A negative `value` means "use the
// estimate"; double-click or middle click resets to it.
bool EstimatedSlider(const char* label, const char* id, float* value, float estimate, float low, float high,
                     const char* text, const Fonts& fonts, const skin::Skin& design, const skin::Skin& s, float dpi) {
    const float width = ImGui::GetContentRegionAvail().x;
    const float left = ImGui::GetCursorPosX();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(); ImGui::SetCursorPosX(left + width - ImGui::CalcTextSize(text).x);
    ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float grooveHeight = 22 * dpi;
    float position = *value < low ? estimate : *value;
    bool changed = Groove(id, &position, low, high, width, grooveHeight, s, dpi, true);
    if ((ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) || ImGui::IsItemClicked(ImGuiMouseButton_Middle)) {
        *value = low - 1; changed = true;
    }
    else if (changed) *value = position;
    {
        FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
        auto* draw = ImGui::GetWindowDrawList();
        const float share = high > low ? std::clamp((estimate - low) / (high - low), 0.f, 1.f) : 0.f;
        const float x = min.x + share * width;
        const float trackBottom = min.y + (grooveHeight + 6 * dpi) / 2;
        // The handle extends 4 below the track; start the tick clear of it.
        draw->AddLine(ImVec2(x, trackBottom + 6 * dpi), ImVec2(x, trackBottom + 11 * dpi), Colour(s.ink.tertiary), dpi);
        const char* word = "Estimated";
        const float wordWidth = ImGui::CalcTextSize(word).x;
        const float wordX = std::clamp(x - wordWidth / 2, min.x, min.x + width - wordWidth);
        draw->AddText(ImVec2(wordX, trackBottom + 12 * dpi), Colour(s.ink.tertiary), word);
        ImGui::Dummy(ImVec2(width, 12 * dpi + ImGui::GetTextLineHeight() - (grooveHeight - 6 * dpi) / 2));
    }
    return changed;
}

// Recessed segmented control; the selected segment is raised and outlined.
// Returns the clicked index, or -1. gapAfter separates segments up to that
// index from the rest with a hairline. `icons`, one per label, draws an icon
// before each word.
// A segment's hairline, left out where the sliding thumb covers it: drawn under
// the thumb it would cross it mid-glide.
void SegmentHairline(ImDrawList* draw, ImVec2 a, ImVec2 b, float thumbLeft, float thumbRight, ImU32 hairline, float radius, float dpi) {
    const float border = ImGui::GetStyle().FrameBorderSize;
    if (border <= 0 || !(hairline & IM_COL32_A_MASK)) return;
    if (thumbRight <= thumbLeft) { draw->AddRect(a, b, hairline, radius, 0, border); return; }
    for (const auto& [left, right] : {std::pair{a.x - dpi, thumbLeft}, std::pair{thumbRight, b.x + dpi}}) {
        if (right <= left) continue;
        draw->PushClipRect(ImVec2(left, a.y - dpi), ImVec2(right, b.y + dpi), true);
        draw->AddRect(a, b, hairline, radius, 0, border);
        draw->PopClipRect();
    }
}

int Segments(const char* id, const std::vector<const char*>& labels, int selected, const skin::Skin& s, float dpi,
             int gapAfter = -1, const std::vector<Icon>& icons = {}) {
    const float height = s.metric.controlHeight, inset = 4 * dpi;
    const float divide = gapAfter >= 0 ? 3 * inset : 0;
    const float iconSide = 14 * dpi, iconGap = 6 * dpi;
    float segment = 0;
    for (const char* label : labels) segment = std::max(segment, ImGui::CalcTextSize(label).x);
    if (icons.size()) segment += iconSide + iconGap;
    segment += 20 * dpi;
    const ImVec2 well = ImGui::GetCursorScreenPos();
    const float width = 2 * inset + segment * labels.size() + inset * (labels.size() - 1) + divide;
    auto* draw = ImGui::GetWindowDrawList();
    skin::RecessedRect(draw, well, ImVec2(well.x + width, well.y + height), s.radius.control, s);
    ImGui::PushID(id);
    const auto xOf = [&](int i) { return well.x + inset + i * (segment + inset) + (i > gapAfter ? divide : 0); };
    // The selection is one raised thumb that slides between segments; the buttons
    // over it are transparent.
    const bool thumb = selected >= 0 && selected < static_cast<int>(labels.size());
    float thumbX = 0;
    if (thumb) {
        const float at = Ease(ImGui::GetID("##thumb"), static_cast<float>(selected));
        const int from = static_cast<int>(std::floor(at)), to = std::min(from + 1, static_cast<int>(labels.size()) - 1);
        thumbX = xOf(from) + (xOf(to) - xOf(from)) * (at - from);
        draw->AddRectFilled(ImVec2(thumbX, well.y + inset), ImVec2(thumbX + segment, well.y + inset + (height - 2 * inset)),
                            Faded(s.surface.elevated), s.radius.element);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.radius.element);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    const ImU32 hairline = ImGui::GetColorU32(ImGuiCol_Border);
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    int clicked = -1, index = 0;
    for (const char* label : labels) {
        const bool chosen = index == selected;
        const float x = xOf(index);
        if (gapAfter >= 0 && index == gapAfter + 1)
            draw->AddLine(ImVec2(x - (divide + inset) / 2, well.y + 2 * inset), ImVec2(x - (divide + inset) / 2, well.y + height - 2 * inset),
                          Faded(s.ink.tertiary), dpi);
        ImGui::SetCursorScreenPos(ImVec2(x, well.y + inset));
        const ImGuiID key = ImGui::GetID(index);
        const ImU32 fill = Colour(Mix(0, StyleArgb(ImGuiCol_ButtonHovered), EaseHover(key)));
        ImGui::PushStyleColor(ImGuiCol_Button, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
        if (icons.size()) {
            // The button is blank; icon and label are drawn over it, centred as one.
            ImGui::PushID(index);
            if (ImGui::Button("##segment", ImVec2(segment, height - 2 * inset)) && !chosen) clicked = index;
            ImGui::PopID();
            const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            const ImVec2 text = ImGui::CalcTextSize(label);
            const float left = min.x + (max.x - min.x - iconSide - iconGap - text.x) / 2;
            const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
            DrawIcon(draw, *(icons.begin() + index), ImVec2(left, (min.y + max.y - iconSide) / 2), iconSide, ink, dpi);
            draw->AddText(ImVec2(left + iconSide + iconGap, (min.y + max.y - text.y) / 2), ink, label);
        } else if (ImGui::Button(label, ImVec2(segment, height - 2 * inset)) && !chosen) clicked = index;
        NoteHover(key);
        ImGui::PopStyleColor(2);
        SegmentHairline(draw, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), thumb ? thumbX : 0.f, thumb ? thumbX + segment : 0.f,
                        hairline, s.radius.element, dpi);
        ++index;
    }
    ImGui::PopStyleColor();
    // Outlined after the buttons so their hairlines don't cover it.
    if (thumb)
        draw->AddRect(ImVec2(thumbX, well.y + inset), ImVec2(thumbX + segment, well.y + inset + (height - 2 * inset)),
                      Faded(s.accent.accent), s.radius.element, 0, dpi);
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(well);
    ImGui::Dummy(ImVec2(width, height));
    return clicked;
}

// Performer triggers with icons for how each drives the song: clock, key held,
// key tapped.
int TriggerSegments(const char* id, const EngineSnapshot& state, const skin::Skin& s, float dpi) {
    std::vector<const char*> names;
    std::vector<Icon> icons;
    for (const auto& trigger : state.performer.triggers) {
        names.push_back(trigger.name.c_str());
        icons.push_back(trigger.plays ? Icon::Hold : trigger.steps ? Icon::Tap : Icon::Play);
    }
    return Segments(id, names, state.trigger, s, dpi, -1, icons);
}

// A performer that is on gets its own panel row when it has a choice to show
// or more than one trigger.
bool PerformerRow(const EngineSnapshot& state) {
    if (!state.performer.on) return false;
    if (state.performer.triggers.size() > 1) return true;
    return std::any_of(state.performer.controls.begin(), state.performer.controls.end(),
                       [](const PerformerControl& control) { return control.isChoice && control.panel; });
}

// The performer row: each choice after its name, the first (resting) choice set
// apart from the others, then the triggers. `label` draws a name, or nothing
// when there's no room for names.
template <class Label>
void PerformerPanelRow(const char* id, const EngineSnapshot& state, ShellEngine& engine, const skin::Skin& s, float dpi, Label&& label) {
    ImGui::PushID(id);
    bool first = true;
    for (const auto& control : state.performer.controls) {
        if (!control.isChoice || !control.panel || !state.performer.Drawn(control)) continue;
        if (!first) ImGui::SameLine(0, s.spacing.s3);
        first = false;
        label(control.name.c_str());
        std::vector<const char*> names;
        for (const auto& choice : control.choices) names.push_back(choice.c_str());
        if (const int chosen = Segments(control.id.c_str(), names, static_cast<int>(control.value), s, dpi, 0); chosen >= 0) {
            ShellEngine::Command command{ShellEngine::Action::PerformerValue, {}, 0, 0, false, static_cast<double>(chosen)};
            command.key = control.id; engine.Send(std::move(command));
        }
    }
    if (state.performer.triggers.size() > 1) {
        if (!first) ImGui::SameLine(0, s.spacing.s3);
        label("Trigger");
        if (const int trigger = TriggerSegments("##trigger", state, s, dpi); trigger >= 0)
            engine.Send({ShellEngine::Action::Trigger, {}, 0, static_cast<size_t>(trigger)});
    }
    ImGui::PopID();
}

// Collapsible section drawn like Velocity Response: a chevron and the label
// inside the content margins. Open state lives in window storage, as with
// CollapsingHeader.
bool SettingSection(const char* label, const skin::Skin& s, float dpi) {
    auto* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID(label);
    bool open = storage->GetBool(key, false);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, s.metric.controlHeight);
    ImGui::PushID(label);
    if (ImGui::InvisibleButton("##section", size)) { open = !open; storage->SetBool(key, open); }
    ImGui::PopID();
    auto* draw = ImGui::GetWindowDrawList();
    // One step wider than the row, like the switches' fill, and in their colours.
    // A mouse click also takes focus; only the keyboard's shows.
    const float bleed = std::min(8 * dpi, ImGui::GetStyle().WindowPadding.x / 2);
    const bool keyboardFocus = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    const bool hovered = ImGui::IsItemHovered() || keyboardFocus;
    const float hot = Ease(ImHashStr("hover", 0, key), hovered ? 1.f : 0.f);
    const float spin = Ease(ImHashStr("chevron", 0, key), open ? 1.f : 0.f);
    if (hovered && ImGui::IsItemActive())
        draw->AddRectFilled(ImVec2(min.x - bleed, min.y), ImVec2(min.x + size.x + bleed, min.y + size.y), ImGui::GetColorU32(ImGuiCol_ButtonActive), s.radius.control);
    else if (hot > 0)
        draw->AddRectFilled(ImVec2(min.x - bleed, min.y), ImVec2(min.x + size.x + bleed, min.y + size.y), ImGui::GetColorU32(ImGuiCol_ButtonHovered, hot), s.radius.control);
    const float side = 16 * dpi;
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    DrawChevron(draw, ImVec2(min.x + 4 * dpi, min.y + (size.y - side) / 2), side, ink, dpi, spin);
    draw->AddText(ImVec2(min.x + 4 * dpi + side + s.spacing.s2, min.y + (size.y - ImGui::GetTextLineHeight()) / 2), ink, label);
    return open;
}

// Opens a button's menu below the button inside the window. Set only while
// open: BeginPopup discards the next-window data when the popup is closed.
void MenuUnderLastItem(const char* popup, float gap, bool alignRight = false) {
    if (!ImGui::IsPopupOpen(popup)) return;
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(alignRight ? max.x : min.x, max.y + gap), ImGuiCond_Appearing, ImVec2(alignRight ? 1.f : 0.f, 0.f));
}

void DrawEllipsis(const std::string& text, float width, ImVec2 min) {
    const float height = ImGui::GetTextLineHeight();
    auto* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(min, ImVec2(min.x + std::max(1.f, width), min.y + height), true);
    const float measured = ImGui::CalcTextSize(text.c_str()).x;
    if (measured > width && width > ImGui::CalcTextSize("...").x) {
        const float available = width - ImGui::CalcTextSize("...").x;
        dl->PushClipRect(min, ImVec2(min.x + available, min.y + height), true);
        dl->AddText(min, ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
        dl->PopClipRect();
        dl->AddText(ImVec2(min.x + available, min.y), ImGui::GetColorU32(ImGuiCol_Text), "...");
    } else dl->AddText(min, ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    dl->PopClipRect();
}

void Ellipsis(const std::string& text, float width) {
    const float height = ImGui::GetTextLineHeight();
    const float measured = ImGui::CalcTextSize(text.c_str()).x;
    auto min = ImGui::GetCursorScreenPos();
    const float baseline = ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;
    min.y += baseline;
    DrawEllipsis(text, width, min);
    ImGui::Dummy(ImVec2(std::max(1.f, width), height + baseline));
    if (measured > width && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str());
}

bool StatePill(const char* label, bool on, const Fonts& fonts, const skin::Skin& design,
               float dpi, float padding, bool enabled = true, const char* tip = nullptr,
               const char* stateText = nullptr) {
    auto s = skin::ScaleGeometry(design, dpi);
    // Size with the on (semibold) weight in every state so toggling doesn't shift
    // the pills after it.
    float widest = 0;
    { FontScope bold(fonts, design, design.type.body * SpecFontScale(design), Weight::Semibold);
      widest = ImGui::CalcTextSize(label).x; }
    FontScope font(fonts, design, design.type.body * SpecFontScale(design), on ? Weight::Semibold : Weight::Regular);
    const auto min = ImGui::GetCursorScreenPos();
    const ImVec2 text = ImGui::CalcTextSize(label);
    const ImVec2 size(std::max(widest, text.x) + 2 * padding + 2 * dpi, s.metric.controlHeight);
    // Keyed by position: the 88/61 Keys pill changes its label with its state.
    const ImGuiID key = ImGui::GetCurrentWindow()->GetIDFromPos(min);
    const float onT = Ease(ImHashStr("on", 0, key), on ? 1.f : 0.f);
    s.border.hairline = Mix(s.border.hairline, s.accent.okBorder, onT);

    // One drawing path for every pill, interactive or not, so labels share a
    // baseline.
    bool clicked = false;
    // Own ID scope: the Velocity pill and panel share the shell window, and
    // hashing the bare word made keyboard activation of the pill hit the panel.
    ImGui::PushID("pill");
    if (enabled) clicked = ImGui::InvisibleButton(label, size);
    else ImGui::Dummy(size);
    ImGui::PopID();
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool held = enabled && ImGui::IsItemActive();
    const float hot = Ease(ImHashStr("hover", 0, key), hovered ? 1.f : 0.f);

    // CSS clips the outside shadow at the control edge. Composite the tint first,
    // at rest and on hover, so the stacked shadow can't darken the translucent on
    // surface.
    const skin::Argb rest = Mix(s.surface.elevated, ToArgb(OpaqueTint(s.accent.okSoft, s.surface.structure)), onT);
    const skin::Argb lifted = Mix(s.surface.elevatedHot, ToArgb(OpaqueTint(s.accent.okSoft, s.surface.elevatedHot)), onT);
    const ImU32 fill = held ? Colour(s.surface.recessed) : Colour(Mix(rest, lifted, hot));
    auto* dl = ImGui::GetWindowDrawList();
    skin::RaisedRect(dl, min, ImVec2(min.x + size.x, min.y + size.y), s.radius.control, s, fill);
    // A pill that can't be pressed uses the tertiary ink so it doesn't look like
    // an off pill.
    dl->AddText(ImVec2(min.x + (size.x - text.x) / 2, min.y + (size.y - text.y) / 2),
                Colour(Mix(enabled ? s.ink.primary : s.ink.tertiary, s.accent.okInk, onT)), label);

    // On state isn't carried by colour alone: an on pill is semibold, an off pill
    // regular.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s: %s%s%s", label, stateText ? stateText : on ? "On" : "Off",
                          tip ? "\n" : "", tip ? tip : "");
    return clicked;
}

// With a row width, the pills' padding stretches or shrinks so the row ends
// exactly there, since text widths don't scale exactly with DPI or theme; 0
// keeps each pill's natural width.
bool StatePills(const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine, float rowWidth) {
    const auto state = engine.Snapshot();
    float pad = 8.f * dpi;
    if (rowWidth > 0) {
        // Measured as StatePill sizes each one: the label in the on weight, plus 2 dp.
        float labels = 0;
        { FontScope bold(fonts, design, design.type.body * SpecFontScale(design), Weight::Semibold);
          for (const char* label : {"Midi2Key", "Velocity", "Sustain", state->eightyEightKeys ? "88 Keys" : "61 Keys", "MidiConnect", "AutoVol"})
              labels += ImGui::CalcTextSize(label).x + 2 * dpi; }
        pad = std::max(4 * dpi, (rowWidth - 5 * ImGui::GetStyle().ItemSpacing.x - labels) / 12);
    }
    if (StatePill("Midi2Key", state->liveActive, fonts, design, dpi, pad,
                  !state->liveDevice.empty(), nullptr))
        engine.Send({ShellEngine::Action::LiveActive, {}, 0, 0, !state->liveActive});
    ImGui::SameLine();
    const bool velocityAvailable = !state->outputMidi;
    // Same label in both states, so switching output to MIDI doesn't resize it.
    if (StatePill("Velocity", velocityAvailable && state->velocity, fonts, design, dpi, pad, velocityAvailable,
                  nullptr, velocityAvailable ? nullptr : "Unavailable"))
        engine.Send({ShellEngine::Action::Velocity, {}, 0, 0, !state->velocity});
    ImGui::SameLine();
    const bool sustainEnabled = !state->playing && !state->liveActive;
    if (StatePill("Sustain", state->sustain, fonts, design, dpi, pad, sustainEnabled,
                  nullptr))
        engine.Send({ShellEngine::Action::Sustain, {}, 0, 0, !state->sustain});
    ImGui::SameLine();
    if (StatePill(state->eightyEightKeys ? "88 Keys" : "61 Keys", state->eightyEightKeys,
                  fonts, design, dpi, pad, true, nullptr))
        engine.Send({ShellEngine::Action::EightyEightKeys, {}, 0, 0, !state->eightyEightKeys});
    ImGui::SameLine();
    if (StatePill("MidiConnect", state->midiConnect, fonts, design, dpi, pad, !state->liveDevice.empty(),
        nullptr))
        engine.Send({ShellEngine::Action::MidiConnect, {}, 0, 0, !state->midiConnect});
    {
        ImGui::SameLine();
        // Like every other pill, the label is just the name; the fill carries state.
        return StatePill("AutoVol", state->autoVolume, fonts, design, dpi, pad, true,
            nullptr);
    }
    return false;
}

bool DevicePill(const std::string& name, float maxWidth, const skin::Skin& s, float dpi) {
    const auto min = ImGui::GetCursorScreenPos();
    const float width = std::min(maxWidth, ImGui::CalcTextSize(name.c_str()).x + 26 * dpi);
    // The button comes first so the fill can lift on hover and press in while held.
    const bool clicked = ImGui::InvisibleButton("##device-pill", ImVec2(width, s.metric.controlHeight));
    const float hot = Ease(ImHashStr("hover", 0, ImGui::GetItemID()), ImGui::IsItemHovered() ? 1.f : 0.f);
    const ImU32 fill = ImGui::IsItemActive() ? Colour(s.surface.recessed) : Colour(Mix(s.surface.card, s.surface.elevatedHot, hot));
    skin::RaisedRect(ImGui::GetWindowDrawList(), min, ImVec2(min.x + width, min.y + s.metric.controlHeight),
                     s.radius.control, s, fill);
    DrawEllipsis(name, width - 24 * dpi, ImVec2(min.x + 12 * dpi,
        min.y + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    // Full name, for when the pill truncates it. A click opens Settings at the input.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", name.c_str());
    return clicked;
}

bool SettingSwitch(const char* label, bool& value, const char* description,
                   const Fonts& fonts, const skin::Skin& design, float dpi) {
    const auto s = skin::ScaleGeometry(design, dpi);
    const auto min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(label);
    // Draw the hover fill here, a step wider than the row on both sides into the
    // popover's padding; the button's own fill stops flush with the label and
    // rail. The clip rect allows half of that padding.
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    const bool clicked = ImGui::Button("##switch", ImVec2(width, s.metric.controlHeight));
    ImGui::PopStyleColor(4);
    if (clicked) value = !value;
    auto* draw = ImGui::GetWindowDrawList();
    // A mouse click also takes focus; only the keyboard's shows. The hover fill
    // fades; a press shows at once.
    const bool keyboardFocus = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    const bool hovered = ImGui::IsItemHovered() || keyboardFocus;
    const float hot = Ease(ImGui::GetID("##hover"), hovered ? 1.f : 0.f);
    if ((hovered && ImGui::IsItemActive()) || hot > 0) {
        const float bleed = std::min(8 * dpi, ImGui::GetStyle().WindowPadding.x / 2);
        draw->AddRectFilled(ImVec2(min.x - bleed, min.y), ImVec2(min.x + width + bleed, min.y + s.metric.controlHeight),
                            hovered && ImGui::IsItemActive() ? ImGui::GetColorU32(ImGuiCol_ButtonActive) : ImGui::GetColorU32(ImGuiCol_ButtonHovered, hot),
                            s.radius.control);
    }
    DrawEllipsis(label, width - 44 * dpi, ImVec2(min.x, min.y + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    // The knob slides and the rail recolours as the value flips.
    const float on = Ease(ImGui::GetID("##knob"), value ? 1.f : 0.f);
    const ImVec2 rail(min.x + width - 32 * dpi, min.y + (s.metric.controlHeight - 16 * dpi) / 2);
    draw->AddRectFilled(rail, ImVec2(rail.x + 32 * dpi, rail.y + 16 * dpi), Faded(Mix(s.surface.recessed, s.accent.okSoft, on)), 8 * dpi);
    draw->AddRect(rail, ImVec2(rail.x + 32 * dpi, rail.y + 16 * dpi), Faded(Mix(s.border.strong, s.accent.okBorder, on)), 8 * dpi, 0, dpi);
    draw->AddCircleFilled(ImVec2(rail.x + (8 + 16 * on) * dpi, rail.y + 8 * dpi), 5 * dpi, Faded(Mix(s.ink.secondary, s.accent.okInk, on)));
    ImGui::PopID();
    // Switches whose label says enough pass no description and take one row.
    if (description && *description) {
        FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
        ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
        ImGui::TextWrapped("%s", description);
        ImGui::PopStyleColor();
    }
    return clicked;
}

// Radio button drawn like the switch's rail: a 16px anti-aliased ring with a
// dot, green when selected. Returns true on a click that changes the choice.
bool SettingRadio(const char* label, bool selected, const skin::Skin& design, float dpi) {
    const auto s = skin::ScaleGeometry(design, dpi);
    const float diameter = 16 * dpi;
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    const auto min = ImGui::GetCursorScreenPos();
    const float height = s.metric.controlHeight;
    ImGui::PushID(label);
    // Keep the hit area invisible in every state so only the ring reacts to hover;
    // otherwise the button's hover and press fills draw a box around the label.
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    const bool clicked = ImGui::Button("##radio", ImVec2(diameter + 8 * dpi + labelSize.x, height));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor(4);
    // The dot grows from the centre as the ring turns green.
    const float chosen = Ease(ImGui::GetID("##selected"), selected ? 1.f : 0.f);
    const float hot = Ease(ImGui::GetID("##hover"), hovered ? 1.f : 0.f);
    ImGui::PopID();
    auto* draw = ImGui::GetWindowDrawList();
    const ImDrawListFlags flags = draw->Flags;
    draw->Flags |= ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;
    const ImVec2 centre(min.x + diameter / 2, min.y + height / 2);
    draw->AddCircleFilled(centre, diameter / 2, Faded(Mix(Mix(s.surface.recessed, s.surface.elevatedHot, hot), s.accent.okSoft, chosen)));
    draw->AddCircle(centre, diameter / 2, Faded(Mix(s.border.strong, s.accent.okBorder, chosen)), 0, dpi);
    if (chosen > 0) draw->AddCircleFilled(centre, 4 * dpi * chosen, Faded(s.accent.okInk));
    draw->Flags = flags;
    draw->AddText(ImVec2(min.x + diameter + 8 * dpi, min.y + (height - labelSize.y) / 2), Faded(s.ink.primary), label);
    return clicked && !selected;
}

void BeginPanel(const char* id, ImVec2 min, ImVec2 max, const skin::Skin& s, ImGuiWindowFlags flags = 0) {
    skin::RaisedPanel(min, max, s);
    ImGui::SetCursorScreenPos(ImVec2(min.x + s.spacing.panelPad, min.y + s.spacing.panelPad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild(id, ImVec2(max.x - min.x - 2 * s.spacing.panelPad,
                     max.y - min.y - 2 * s.spacing.panelPad), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | flags);
    ImGui::PopStyleVar();
}

// Uses IFileOpenDialog, like PickFolder. The legacy GetOpenFileNameW dialog
// returns without showing under this window when it is layered (opacity) or
// topmost.
enum class PickKind { Midi, Audio, Page };
std::filesystem::path PickFile(HWND hwnd, PickKind kind = PickKind::Midi) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    const COMDLG_FILTERSPEC midi[]{{L"MIDI files", L"*.mid;*.midi;*.kar"}, {L"All files", L"*.*"}};
    const COMDLG_FILTERSPEC sound[]{{L"Audio files", L"*.mp3;*.wav;*.flac;*.ogg;*.oga;*.opus;*.m4a;*.aac"}, {L"All files", L"*.*"}};
    const COMDLG_FILTERSPEC page[]{{L"Saved sheet pages", L"*.html;*.htm"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, kind == PickKind::Audio ? sound : kind == PickKind::Page ? page : midi);
    std::filesystem::path path;
    if (SUCCEEDED(dialog->Show(hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR text = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &text))) { path = text; CoTaskMemFree(text); }
            item->Release();
        }
    }
    dialog->Release();
    return path;
}

// Theme file picker: opens one, or with `save`, picks where to write `name`.
std::filesystem::path PickTheme(HWND hwnd, bool save, const std::string& name) {
    IFileDialog* dialog = nullptr;
    if (FAILED(save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))
                    : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    const COMDLG_FILTERSPEC themes[]{{L"QuartzMIDI themes", L"*.qmtheme"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, themes);
    dialog->SetDefaultExtension(L"qmtheme");
    if (save) {
        // Default file name: the theme name with characters invalid in file names replaced.
        std::wstring file = L"Theme";
        try { file = std::filesystem::path(std::u8string(name.begin(), name.end())).wstring(); } catch (const std::exception&) {}
        for (auto& c : file) if (c < 0x20 || wcschr(L"<>:\"/\\|?*", c)) c = L'-';
        dialog->SetFileName(file.c_str());
    }
    std::filesystem::path path;
    if (SUCCEEDED(dialog->Show(hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR text = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &text))) { path = text; CoTaskMemFree(text); }
            item->Release();
        }
    }
    dialog->Release();
    return path;
}

std::filesystem::path PickFolder(HWND hwnd) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    std::filesystem::path path;
    if (SUCCEEDED(dialog->Show(hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR text = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &text))) { path = text; CoTaskMemFree(text); }
            item->Release();
        }
    }
    dialog->Release();
    return path;
}

bool CopyUtf8ToClipboard(HWND hwnd, const std::string& text) {
    if (text.empty() || text.size() > static_cast<size_t>(INT_MAX)) return false;
    // No MB_ERR_INVALID_CHARS: the log holds system error text in the ANSI code
    // page, and one invalid byte would make the whole conversion fail.
    const int characters = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                                static_cast<int>(text.size()), nullptr, 0);
    if (characters <= 0) return false;
    const size_t bytes = (static_cast<size_t>(characters) + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
    if (!destination) { GlobalFree(memory); return false; }
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), destination, characters);
    destination[characters] = L'\0';
    GlobalUnlock(memory);
    if (!OpenClipboard(hwnd)) { GlobalFree(memory); return false; }
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }
    CloseClipboard();
    return true; // The clipboard owns memory after SetClipboardData succeeds.
}

std::string Time(double seconds) {
    const int total = static_cast<int>(std::max(0.0, seconds));
    char text[32];
    snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}
}

bool MotionPending() { return g_motion; }
void SettleMotion() { g_settleFrame = ImGui::GetFrameCount(); }

void Panels::LoadPreferences(const std::filesystem::path& path) {
    // Tracks starts closed on first run and is restored after that. Set before
    // looking for the file, since a first run has none.
    tracksExpanded = false;
    try {
        std::ifstream stream(path);
        // Don't return: themes are loaded from, and later saved beside, this file
        // whether or not it exists yet.
        if (!stream) throw std::runtime_error("no settings file yet");
        const auto json = nlohmann::json::parse(stream);
        // Legacy skin index: Blue, Blue Dark, Orange, Orange Dark.
        const int legacy = std::clamp(json.value("skin", 0), 0, 3);
        preferences.theme = json.value("theme", std::string(legacy < 2 ? "blue" : "orange"));
        preferences.dark = json.value("dark", (legacy & 1) != 0);
        preferences.autoSolo = json.value("autoSoloPiano", false);
        preferences.alwaysOnTop = json.value("alwaysOnTop", false);
        preferences.mediaKeys = json.value("mediaKeys", true);
        preferences.opacity = std::clamp(json.value("opacity", 100), 40, 100);
        preferences.converterCpu = std::clamp(json.value("converterCpu", 75), 25, 100);
        convertPlaylist_ = json.value("convertPlaylist", false);
        timingSource_ = std::clamp(json.value("timingSource", 0), 0, 1);
        preferences.folders = json.value("folders", true);
        browse_ = json.value("openFolder", std::string());
        preferences.startMini = json.value("mini", false);
        miniAutoplay = json.value("miniAutoplay", false);
        tracksExpanded = json.value("tracksOpen", false);
        velocityExpanded = json.value("velocityOpen", false);
        curveTool_ = std::clamp(json.value("curveTool", 0), 0, 1);
        preferences.windowX = json.value("windowX", 0);
        preferences.windowY = json.value("windowY", 0);
        preferences.windowWidth = json.value("windowWidth", 0);
        // Bounded so an edited file can't overflow the window size; the work area
        // clamps it further.
        preferences.windowExtra = std::clamp(json.value("windowExtra", 0.f), 0.f, 16384.f);
        preferences.maximized = json.value("maximized", false);
        preferences.miniX = json.value("miniX", 0);
        preferences.miniY = json.value("miniY", 0);
        preferences.miniSaved = json.value("miniSaved", false);
        const auto folder = json.value("midiFolder", std::string());
        preferences.folder = std::filesystem::path(std::u8string(folder.begin(), folder.end()));
        const auto song = json.value("song", std::string());
        preferences.lastSong = std::filesystem::path(std::u8string(song.begin(), song.end()));
        // A settings file without the tour key predates the tour, so it hasn't been seen.
        preferences.tourSeen = json.value("tourSeen", false);
        preferences.helpBuild = json.value("helpBuild", std::string());
    } catch (const std::exception&) { preferences = {}; preferences.tourSeen = false; }
    themesPath_ = path.parent_path() / L"themes.json";
    themes.Load(themesPath_);
}

void Panels::SavePreferences(const std::filesystem::path& path, bool exiting) const {
    if (exiting) SaveThemes();
    nlohmann::json json{{"theme", preferences.theme}, {"dark", preferences.dark}, {"autoSoloPiano", preferences.autoSolo},
                        {"midiFolder", Utf8(preferences.folder)},
                        {"song", Utf8(preferences.lastSong)},
                        {"alwaysOnTop", preferences.alwaysOnTop}, {"mediaKeys", preferences.mediaKeys},
                        {"opacity", preferences.opacity}, {"converterCpu", preferences.converterCpu},
                        {"convertPlaylist", convertPlaylist_}, {"timingSource", timingSource_},
                        {"folders", preferences.folders}, {"openFolder", browse_}, {"mini", miniMode},
                        {"miniAutoplay", miniAutoplay},
                        {"tracksOpen", tracksExpanded}, {"velocityOpen", velocityExpanded},
                        {"curveTool", curveTool_},
                        {"windowExtra", preferences.windowExtra}, {"maximized", preferences.maximized},
                        {"miniX", preferences.miniX}, {"miniY", preferences.miniY}, {"miniSaved", preferences.miniSaved},
                        {"tourSeen", preferences.tourSeen}, {"helpBuild", preferences.helpBuild},
                        {"windowX", preferences.windowX}, {"windowY", preferences.windowY}, {"windowWidth", preferences.windowWidth}};
    const auto text = json.dump(2);
    if (text == savedPreferences_) return;
    // Write to a temporary file and rename over, so an interrupted write can't
    // leave a truncated file that the next start reads as no settings.
    auto temporary = path; temporary += L".tmp";
    // A failed write is retried on every call, so it is reported once per change.
    const auto failed = [&] {
        if (text != unsavedPreferences_) ReportError("Could not save the settings.", "Could not write " + Utf8(path.filename()) + ".");
        unsavedPreferences_ = text;
    };
    // Closed before the check: the text fits the stream's buffer, so the write
    // itself happens at close.
    { std::ofstream stream(temporary); stream << text << '\n'; stream.close(); if (stream.fail()) { failed(); return; } }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (!error) savedPreferences_ = text;
    else failed();
}

// Saves only when there are user themes, or a file to update after the last
// one was deleted.
void Panels::SaveThemes() const {
    if (themesPath_.empty()) return;
    if ((themes.All().size() > 2 || std::filesystem::exists(themesPath_)) && !themes.Save(themesPath_))
        ReportError("Could not save the themes.", "Could not write " + Utf8(themesPath_.filename()) + ".");
}

void Panels::ReportError(const std::string& result, const std::string& detail) const {
    panelError_ = result;
    ShellLog::Instance().Append("[error] " + (detail.empty() ? result : detail) + "\n");
}

void Panels::OpenFolder(const std::filesystem::path& path, ShellEngine& engine) {
    // During playback the engine refuses the scan and says so; the list keeps
    // the folder it shows.
    if (!engine.Snapshot()->playing) { preferences.folder = path; browse_.clear(); }
    engine.Send({ShellEngine::Action::Scan, path});
}

void Panels::ImportTheme(const std::filesystem::path& path) {
    if (const Theme* imported = themes.Import(path)) { preferences.theme = imported->id; SaveThemes(); }
    else ReportError("Could not import the theme.", Utf8(path.filename()) + " is not a QuartzMIDI theme.");
}

void Panels::DrawKeyMapping(const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine) {
    const bool platform = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
    if (platform && mappingDpi_ > 0) dpi = mappingDpi_;
    // Built-in height plus the theme's growth: panel edges above the row and under
    // the piano, the row's control, and the status line's text.
    const auto grow = GrowthOf(design);
    const float height = 268.390625f + 2 * grow.panel + grow.control + 2 * std::max(0.f, grow.meta);
    const auto* main = ImGui::GetMainViewport();
    if (!platform || mappingDpi_ == 0)
        ImGui::SetNextWindowPos(ImVec2(main->Pos.x + (main->Size.x - 840 * dpi) / 2, main->Pos.y + main->Size.y - (height + 40) * dpi));
    ImGui::SetNextWindowSize(ImVec2(840 * dpi, height * dpi));
    const ImGuiWindowClass windowClass = OwnWindowClass();
    ImGui::SetNextWindowClass(&windowClass);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    const bool visible = ImGui::Begin("Key Mapping", &preferences.keyMappingOpen,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (!visible) { ImGui::End(); return; }
    // The monitor's scale times the theme's Size, as UiScale gives the rest of the app.
    if (platform) dpi = ImGui::GetWindowViewport()->DpiScale * design.scale;
    mappingDpi_ = dpi;
    const ImGuiStyle previousStyle = ImGui::GetStyle();
    skin::ApplyStyle(design, dpi);
    const auto s = skin::ScaleGeometry(design, dpi);
    ImGui::PushFont(fonts.Get(design), design.type.body * SpecFontScale(design));
    const auto state = engine.Snapshot();
    const auto origin = ImGui::GetWindowPos();
    if (mappingLayout88_ != state->eightyEightKeys) {
        mappingLayout88_ = state->eightyEightKeys;
        mappingArmed_ = false;
        selectedNote_ = -1;
    }
    auto* draw = ImGui::GetWindowDrawList();
    const float title = 44 * dpi, width = 840 * dpi, pad = s.spacing.panelPad;
    const auto at = [&](float x, float y) { return ImVec2(origin.x + x, origin.y + y); };
    // An OS window of its own is square, so only the in-viewport window rounds its corners.
    const float corner = platform ? 0.f : s.radius.window;
    draw->AddRectFilled(origin, at(width, height * dpi), Colour(s.surface.canvas), corner);
    draw->AddRectFilled(origin, at(width, title), Colour(s.surface.structure), corner, ImDrawFlags_RoundCornersTop);
    draw->AddLine(at(0, title), at(width, title), Colour(s.border.hairline), dpi);
    draw->AddText(at(pad, (title - ImGui::GetTextLineHeight()) / 2), Colour(s.ink.primary), "Key Mapping");
    ImGui::SetCursorScreenPos(at(width - 8 * dpi - s.metric.controlHeight, (title - s.metric.controlHeight) / 2));
    if (IconButton("##close-mapping", Icon::Close, "Close key mapping", s, dpi)) {
        preferences.keyMappingOpen = false; mappingArmed_ = false;
    }
    const float rowY = title + pad;
    // Selected note, an arrow, and its key as a keycap. When armed, the cap is
    // empty inside the accent ring, the standard look for a field awaiting a key.
    if (selectedNote_ >= 0) {
        FontScope font(fonts, design, design.type.meta * SpecFontScale(design));
        const auto found = state->keyMappings.find(NoteName(selectedNote_));
        const std::string key = mappingArmed_ || found == state->keyMappings.end() ? std::string() : found->second;
        const std::string note = NoteName(selectedNote_);
        const float line = ImGui::GetTextLineHeight(), textY = rowY + (s.metric.controlHeight - line) / 2;
        const float noteWidth = ImGui::CalcTextSize(note.c_str()).x, side = 14 * dpi;
        draw->AddText(at(pad, textY), Colour(s.ink.primary), note.c_str());
        DrawIcon(draw, Icon::Right, at(pad + noteWidth + s.spacing.s1, rowY + (s.metric.controlHeight - side) / 2), side, Colour(s.ink.tertiary), dpi);
        const float capX = pad + noteWidth + side + 2 * s.spacing.s1;
        const float capWidth = std::max(28 * dpi, ImGui::CalcTextSize(key.c_str()).x + 12 * dpi);
        const ImVec2 capMin = at(capX, textY - 3 * dpi), capMax = at(capX + capWidth, textY + line + 3 * dpi);
        draw->AddRectFilled(capMin, capMax, Colour(s.surface.elevated), 4 * dpi);
        draw->AddRect(capMin, capMax, Colour(mappingArmed_ ? s.accent.accent : s.border.hairline), 4 * dpi, 0, mappingArmed_ ? 2 * dpi : dpi);
        draw->AddText(at(capX + (capWidth - ImGui::CalcTextSize(key.c_str()).x) / 2, textY), Colour(s.ink.primary), key.c_str());
    }
    // The game's key maps, at the row's right. The shown map is derived from the
    // binds, so a hand-remapped key shows Custom and only the binds are saved.
    {
        static uint64_t revision = ~0ull; static bool layout88 = false; static int current = -1;
        if (revision != state->mappingRevision || layout88 != state->eightyEightKeys) {
            revision = state->mappingRevision; layout88 = state->eightyEightKeys; current = -1;
            for (size_t i = 0; i < std::size(kGameKeyMaps) && current < 0; ++i)
                if (state->keyMappings == GameKeyTable(kGameKeyMaps[i], layout88)) current = static_cast<int>(i);
        }
        const float comboWidth = 168 * dpi;
        ImGui::SetCursorScreenPos(at(width - pad - comboWidth, rowY));
        ImGui::SetNextItemWidth(comboWidth);
        const bool mapOpen = ChevronCombo("##game-key-map", current < 0 ? "Custom" : kGameKeyMaps[current].name);
        if (mapOpen) {
            for (size_t i = 0; i < std::size(kGameKeyMaps); ++i)
                if (ImGui::Selectable(kGameKeyMaps[i].name, current == static_cast<int>(i))) {
                    mappingArmed_ = false;
                    engine.Send({ShellEngine::Action::GameKeyMap, {}, 0, i});
                }
            ImGui::EndCombo();
        }
    }
    // Only the game's 61 bindable keys: notes below C2 and above C7 are typed with
    // Ctrl from the game's fixed table and can't be rebound.
    const float frameY = rowY + s.metric.controlHeight + 12 * dpi;
    skin::RecessedRect(draw, at(pad, frameY), at(width - pad, frameY + 114 * dpi), s.radius.element, s);
    const float pianoX = pad + 9 * dpi, pianoY = frameY + 9 * dpi, pianoWidth = width - 2 * pad - 18 * dpi;
    draw->AddRectFilled(at(pianoX, pianoY), at(pianoX + pianoWidth, pianoY + 96 * dpi), IM_COL32(29, 27, 24, 255), 3 * dpi);
    const auto white = [](int note) { const int n = note % 12; return n != 1 && n != 3 && n != 6 && n != 8 && n != 10; };
    // 36 white keys: the game's 61 keys, C2 to C7, typed 1 to m.
    std::vector<int> whites;
    for (int note = 36; note <= 96; ++note)
        if (white(note)) whites.push_back(note);
    const float whiteWidth = (pianoWidth - 8 * dpi) / static_cast<float>(whites.size());
    struct Key { int note; ImVec2 min, max; bool black; };
    std::vector<Key> keys;
    for (size_t i = 0; i < whites.size(); ++i) {
        const float x = pianoX + 4 * dpi + i * whiteWidth;
        keys.push_back({whites[i], at(x, pianoY + 4 * dpi), at(x + whiteWidth, pianoY + 96 * dpi), false});
    }
    for (size_t i = 0; i + 1 < whites.size(); ++i) {
        if (white(whites[i] + 1)) continue;
        const float x = pianoX + 4 * dpi + (i + 1) * whiteWidth - whiteWidth * .31f;
        keys.push_back({whites[i] + 1, at(x, pianoY + 4 * dpi), at(x + whiteWidth * .62f, pianoY + 57 * dpi), true});
    }
    ImGui::SetCursorScreenPos(at(pianoX, pianoY));
    ImGui::InvisibleButton("##piano", ImVec2(pianoWidth, 96 * dpi));
    int hoveredNote = -1;
    if (ImGui::IsItemHovered()) {
        const auto mouse = ImGui::GetIO().MousePos;
        // Black keys are hit-tested last so they take precedence over the whites beneath.
        for (const auto& key : keys)
            if (mouse.x >= key.min.x && mouse.x < key.max.x && mouse.y >= key.min.y && mouse.y < key.max.y) hoveredNote = key.note;
        if (hoveredNote >= 0) {
            const auto found = state->keyMappings.find(NoteName(hoveredNote));
            ImGui::SetTooltip("%s: %s", NoteName(hoveredNote).c_str(), found == state->keyMappings.end() ? "Unassigned" : found->second.c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selectedNote_ = hoveredNote; mappingArmed_ = true;
                engine.Send({ShellEngine::Action::Pause, {}, state->generation});
            }
        }
    }
    { FontScope font(fonts, design, 9.f * SpecFontScale(design));
      for (const auto& key : keys) {
        const ImU32 top = key.black ? IM_COL32(49, 47, 44, 255) : IM_COL32(255, 254, 251, 255);
        const ImU32 bottom = key.black ? IM_COL32(22, 21, 19, 255) : IM_COL32(242, 238, 229, 255);
        if (key.black) draw->AddRectFilled(ImVec2(key.min.x, key.min.y + 2 * dpi), ImVec2(key.max.x + dpi, key.max.y + 3 * dpi), IM_COL32(0, 0, 0, 60), 2 * dpi);
        draw->AddRectFilledMultiColor(key.min, key.max, top, top, bottom, bottom);
        draw->AddRect(key.min, key.max, key.black ? IM_COL32(0, 0, 0, 180) : IM_COL32(101, 87, 63, 100), 2 * dpi, 0, dpi);
        if (key.note == selectedNote_) {
            draw->AddRect(key.min, key.max, Colour(s.accent.accent), 2 * dpi, 0, 2 * dpi);
            // Selection gets a notch as well as its themed outline.
            draw->AddTriangleFilled(ImVec2(key.min.x + 2 * dpi, key.min.y + 2 * dpi),
                ImVec2(key.min.x + 8 * dpi, key.min.y + 2 * dpi), ImVec2(key.min.x + 2 * dpi, key.min.y + 8 * dpi),
                key.black ? IM_COL32_WHITE : IM_COL32_BLACK);
        }
        const auto found = state->keyMappings.find(NoteName(key.note));
        if (found != state->keyMappings.end()) {
            const std::string& label = found->second;
            const auto ink = key.black ? IM_COL32(221, 177, 106, 255) : IM_COL32(51, 47, 41, 255);
            const float center = (key.min.x + key.max.x) / 2;
            const float y = key.max.y - 8 * dpi - ImGui::GetTextLineHeight();
            draw->PushClipRect(key.min, key.max, true);
            draw->AddText(ImVec2(center - ImGui::CalcTextSize(label.c_str()).x / 2, y), ink, label.c_str());
            draw->PopClipRect();
        }
      }
    }
    if (mappingArmed_ && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        ImGui::SetNextFrameWantCaptureKeyboard(true);
        auto& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) mappingArmed_ = false;
        else if (!io.KeyAlt && !io.KeySuper) {
            std::string key;
            // Ctrl belongs to the game's notes outside these keys; never bind it here.
            if (!io.KeyCtrl) for (ImWchar c : io.InputQueueCharacters)
                if (c > 32 && c < 127) { key = static_cast<char>(c); break; }
            if (!key.empty()) {
                engine.Send({ShellEngine::Action::Remap, {}, 0, static_cast<size_t>(selectedNote_), false, 0, key});
                mappingArmed_ = false;
            }
        }
    } else if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) mappingArmed_ = false;
    const float statusY = frameY + 114 * dpi + pad;
    draw->AddRectFilled(at(0, statusY), at(width, height * dpi), Colour(s.surface.structure), corner, ImDrawFlags_RoundCornersBottom);
    draw->AddLine(at(0, statusY), at(width, statusY), Colour(s.border.hairline), dpi);
    ImGui::SetCursorScreenPos(at(pad, statusY + 8 * dpi));
    { FontScope font(fonts, design, design.type.meta * SpecFontScale(design));
      const std::string status = !state->error.empty() ? state->error :
          NoteName(whites.front()) + "\xe2\x80\x93" + NoteName(whites.back());
      Ellipsis(status, width - 2 * pad); }
    ImGui::PopFont();
    ImGui::GetStyle() = previousStyle;
    ImGui::End();
}

namespace {
// First free name of the form "Blue custom", "Blue custom 2", ...
std::string NewThemeName(const ThemeStore& themes, const std::string& from) {
    const std::string base = from + " custom";
    for (int number = 1;; ++number) {
        const std::string name = number == 1 ? base : base + " " + std::to_string(number);
        if (std::none_of(themes.All().begin(), themes.All().end(), [&](const Theme& theme) { return theme.name == name; })) return name;
    }
}

// Colour swatch with its role name beside it, opening a custom picker: a
// saturation/value field, a hue bar, an alpha bar where the colour has alpha,
// and a hex text field. Custom-drawn so the swatch and picker match the app's
// rounding and styling. Returns true while the colour is being changed.
//
// The picker keeps its own HSV for as long as the colour still matches it.
// Rebuilding HSV from the 8-bit colour each frame makes the hue drift with
// rounding and jump near grey, where hue is barely defined.
bool ThemeSwatch(const char* label, skin::Argb& colour, bool alpha, const skin::Skin& s, float dpi) {
    struct Picker { ImGuiID id = 0; float h = 0, sat = 0, v = 0, a = 1; skin::Argb result = 0; char text[12]{}; };
    static Picker picker;
    const ImGuiID id = ImGui::GetID(label);
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const float side = s.metric.controlHeight, gap = s.spacing.s2;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float labelWidth = ImGui::CalcTextSize(label, labelEnd).x;
    auto* draw = ImGui::GetWindowDrawList();
    const auto packed = [](skin::Argb argb) { return IM_COL32(argb >> 16 & 0xFF, argb >> 8 & 0xFF, argb & 0xFF, argb >> 24); };

    ImGui::PushID(label);
    // The name is part of the button, giving it the larger hit target.
    const bool clicked = ImGui::InvisibleButton("##swatch", ImVec2(side + gap + labelWidth, side));
    // A mouse click also takes focus; only the keyboard's shows.
    const bool keyboardFocus = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    const bool hot = ImGui::IsItemHovered() || keyboardFocus || ImGui::IsPopupOpen("##picker");
    const ImVec2 max(min.x + side, min.y + side);
    // Paint the card surface first so a translucent colour shows as it will on a card.
    draw->AddRectFilled(min, max, Colour(s.surface.card), s.radius.control);
    draw->AddRectFilled(min, max, packed(colour), s.radius.control);
    // Outline in the text colour so the swatch stays visible against any background.
    draw->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Text, hot ? .55f : .28f), s.radius.control, 0, dpi);
    draw->AddText(ImVec2(max.x + gap, min.y + (side - ImGui::GetTextLineHeight()) / 2), ImGui::GetColorU32(ImGuiCol_Text), label, labelEnd);
    if (clicked) ImGui::OpenPopup("##picker");

    bool changed = false;
    ImGui::SetNextWindowPos(ImVec2(min.x, max.y + s.spacing.s1), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s.spacing.s3, s.spacing.s3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(s.spacing.s2, s.spacing.s3));
    if (ImGui::BeginPopup("##picker")) {
        const auto toText = [&] { snprintf(picker.text, sizeof(picker.text), "%s", ColourText(colour).c_str()); };
        if (picker.id != id || picker.result != colour) {
            ImGui::ColorConvertRGBtoHSV((colour >> 16 & 0xFF) / 255.f, (colour >> 8 & 0xFF) / 255.f, (colour & 0xFF) / 255.f,
                                        picker.h, picker.sat, picker.v);
            picker.a = (colour >> 24) / 255.f;
            picker.id = id;
            picker.result = colour;
            toText();
        }
        auto* layer = ImGui::GetWindowDrawList();
        const float width = 232 * dpi, fieldHeight = 148 * dpi, barHeight = 14 * dpi;
        const auto hsv = [](float h, float sat, float v, float a = 1.f) {
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(h, sat, v, r, g, b);
            return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
        };
        // Draw the hue as one-pixel columns, each trimmed to the rounded corner above
        // it: a gradient rect has square corners, and a rounded rect has no inner
        // vertices to carry the gradient.
        const auto columns = [&](ImVec2 origin, float height, float radius, const auto& paint) {
            for (float x = 0; x < width; x += 1.f) {
                const float centre = x + .5f;
                const float into = centre < radius ? radius - centre : centre > width - radius ? centre - (width - radius) : 0.f;
                const float inset = into > 0 ? radius - std::sqrt(std::max(0.f, radius * radius - into * into)) : 0.f;
                paint(x / (width - 1), ImVec2(origin.x + x, origin.y + inset), ImVec2(origin.x + std::min(x + 1.f, width), origin.y + height - inset),
                      inset / height);
            }
        };
        const auto handle = [&](ImVec2 at, float radius, ImU32 fill) {
            layer->AddCircleFilled(at, radius, fill);
            layer->AddCircle(at, radius, IM_COL32(255, 255, 255, 255), 0, 2 * dpi);
            layer->AddCircle(at, radius + dpi, IM_COL32(0, 0, 0, 90), 0, dpi);
        };
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        const ImVec2 field = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##field", ImVec2(width, fieldHeight));
        if (ImGui::IsItemActive()) {
            picker.sat = std::clamp((mouse.x - field.x) / width, 0.f, 1.f);
            picker.v = 1.f - std::clamp((mouse.y - field.y) / fieldHeight, 0.f, 1.f);
            changed = true;
        }
        columns(field, fieldHeight, s.radius.element, [&](float t, ImVec2 a, ImVec2 b, float cut) {
            const ImU32 top = hsv(picker.h, t, 1.f - cut), bottom = hsv(picker.h, t, cut);
            layer->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
        });
        layer->AddRect(field, ImVec2(field.x + width, field.y + fieldHeight), Colour(s.border.strong), s.radius.element, 0, dpi);
        handle(ImVec2(field.x + picker.sat * width, field.y + (1.f - picker.v) * fieldHeight), 6 * dpi, hsv(picker.h, picker.sat, picker.v));

        const auto bar = [&](const char* name, float& value, const auto& paint, ImU32 knob) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(name, ImVec2(width, barHeight));
            if (ImGui::IsItemActive()) { value = std::clamp((mouse.x - origin.x) / width, 0.f, 1.f); changed = true; }
            columns(origin, barHeight, barHeight / 2, paint);
            layer->AddRect(origin, ImVec2(origin.x + width, origin.y + barHeight), Colour(s.border.strong), barHeight / 2, 0, dpi);
            handle(ImVec2(origin.x + std::clamp(value * width, barHeight / 2, width - barHeight / 2), origin.y + barHeight / 2), barHeight / 2 + dpi, knob);
        };
        bar("##hue", picker.h, [&](float t, ImVec2 a, ImVec2 b, float) { layer->AddRectFilled(a, b, hsv(t, 1, 1)); }, hsv(picker.h, 1, 1));
        if (alpha)
            bar("##alpha", picker.a, [&](float t, ImVec2 a, ImVec2 b, float) {
                // Draw over a light/dark checkerboard so alpha reads as transparency, not as
                // a paler colour.
                const float square = barHeight / 2, mid = (a.y + b.y) / 2;
                const bool odd = static_cast<int>((a.x - field.x) / square) % 2 != 0;
                layer->AddRectFilled(a, ImVec2(b.x, mid), odd ? IM_COL32(200, 200, 200, 255) : IM_COL32(120, 120, 120, 255));
                layer->AddRectFilled(ImVec2(a.x, mid), b, odd ? IM_COL32(120, 120, 120, 255) : IM_COL32(200, 200, 200, 255));
                layer->AddRectFilled(a, b, hsv(picker.h, picker.sat, picker.v, t));
            }, hsv(picker.h, picker.sat, picker.v));

        if (changed) {
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(picker.h, picker.sat, picker.v, r, g, b);
            const auto channel = [](float value) { return static_cast<skin::Argb>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f)); };
            colour = (alpha ? channel(picker.a) : 0xFFu) << 24 | channel(r) << 16 | channel(g) << 8 | channel(b);
            picker.result = colour;
            toText();
        }
        // Hex text for copy and paste. A partially typed value is ignored until it parses.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputText("##text", picker.text, sizeof(picker.text), ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_AutoSelectAll)) {
            std::string typed = picker.text;
            if (!typed.empty() && typed[0] != '#') typed.insert(typed.begin(), '#');
            skin::Argb read = 0;
            if (ParseColour(typed, read) && (alpha || typed.size() == 7)) {
                colour = read;
                ImGui::ColorConvertRGBtoHSV((read >> 16 & 0xFF) / 255.f, (read >> 8 & 0xFF) / 255.f, (read & 0xFF) / 255.f, picker.h, picker.sat, picker.v);
                picker.a = (read >> 24) / 255.f;
                picker.result = colour;
                changed = true;
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return changed;
}
}

// Theme editor. The theme being edited is the active one, so the app itself is
// the preview. Three base colours at the top regenerate the whole palette;
// below them is every colour in the skin, named by what it paints.
void Panels::DrawThemeEditor(const Fonts& fonts, const skin::Skin& design, float dpi) {
    Theme* theme = themes.Find(preferences.theme);
    if (!themeEditorOpen || !theme || theme->builtin) {
        if (themeEditorWasOpen_) {
            // A copy Customise made and nothing changed is dropped, and its built-in
            // is chosen again if the copy still was.
            const Theme* copy = customisedFrom_.empty() ? nullptr : themes.Find(customisedCopy_.id);
            if (copy && ThemeStore::ThemeJson(*copy) == ThemeStore::ThemeJson(customisedCopy_)) {
                if (preferences.theme == customisedCopy_.id) preferences.theme = customisedFrom_;
                themes.Remove(customisedCopy_.id);
            }
            customisedFrom_.clear();
            SaveThemes();
        }
        themeEditorWasOpen_ = themeEditorOpen = false;
        return;
    }
    if (!themeEditorWasOpen_) {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Pos.x + 24 * dpi, ImGui::GetMainViewport()->Pos.y + 112 * dpi));
        themeEditorWasOpen_ = true;
    }
    if (themeNameFor_ != theme->id) {
        snprintf(themeName_, sizeof(themeName_), "%s", theme->name.c_str());
        themeNameFor_ = theme->id;
    }
    const ImGuiWindowClass windowClass = OwnWindowClass();
    ImGui::SetNextWindowClass(&windowClass);
    // Wide enough for the three base swatches and their labels, which the edited
    // theme may have enlarged.
    const auto grow = GrowthOf(design);
    const float editorWidth = (360 + 3 * std::max(0.f, grow.control) + 14 * std::max(0.f, grow.text)) * dpi;
    ImGui::SetNextWindowSizeConstraints(ImVec2(editorWidth, 0), ImVec2(editorWidth, 640 * dpi));
    const auto s = skin::ScaleGeometry(design, dpi);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * dpi, 16 * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    if (ImGui::Begin("Theme", &themeEditorOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        const auto section = [&](const char* label) {
            FontScope meta(fonts, design, design.type.meta * SpecFontScale(design), Weight::Semibold);
            ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
            ImGui::TextUnformatted(label); ImGui::PopStyleColor();
        };
        ImGui::SetNextItemWidth(-1);
        // Only accept a name that survives a save/load round trip; an all-space name
        // would be dropped with its theme at the next start.
        if (ImGui::InputText("##theme-name", themeName_, sizeof(themeName_)))
            if (const auto typed = ThemeNameFrom(themeName_); !typed.empty()) theme->name = typed;
        // Out of the field, it shows the name as kept: trimmed, cut to length, or the last one accepted.
        if (!ImGui::IsItemActive() && theme->name != themeName_) snprintf(themeName_, sizeof(themeName_), "%s", theme->name.c_str());

        bool paired = theme->paired;
        if (SettingSwitch("Light and dark", paired, nullptr, fonts, design, dpi)) {
            if (paired) {
                // The current palette becomes the source and the other variant is generated
                // from it, so the pair starts complete.
                preferences.dark = theme->onlyDark;
                theme->paired = true;
                theme->SetAutomatic(true, preferences.dark);
            } else {
                theme->onlyDark = theme->ShowsDark(preferences.dark);
                theme->paired = false;
            }
        }
        if (theme->paired) {
            if (SettingRadio("Light", !preferences.dark, design, dpi)) preferences.dark = false;
            ImGui::SameLine(0, 16 * dpi);
            if (SettingRadio("Dark", preferences.dark, design, dpi)) preferences.dark = true;
            bool automatic = theme->automatic;
            // Label the switch with the variant it generates: when on, the non-source
            // variant; when off, the one turning it on would generate.
            const bool generatesDark = theme->automatic ? !theme->sourceDark : !preferences.dark;
            const char* generate = generatesDark ? "Auto-generate Dark" : "Auto-generate Light";
            if (SettingSwitch(generate, automatic, nullptr, fonts, design, dpi)) theme->SetAutomatic(automatic, preferences.dark);
        }
        ImGui::Separator();

        skin::Skin& shown = theme->Shown(preferences.dark);
        section("Start from");
        skin::Argb background = shown.surface.canvas, text = shown.ink.primary, accent = shown.accent.accent;
        bool rebuilt = ThemeSwatch("Background", background, false, s, dpi);
        ImGui::SameLine(0, 16 * dpi);
        rebuilt |= ThemeSwatch("Text##start", text, false, s, dpi);
        ImGui::SameLine(0, 16 * dpi);
        rebuilt |= ThemeSwatch("Accent##start", accent, false, s, dpi);
        if (rebuilt) {
            // A dark background makes a dark palette, so switch the editor to the dark
            // variant rather than filing it under Light.
            skin::Skin made = GenerateSkin(background, text, accent);
            CopyShape(shown, made);
            if (theme->paired) preferences.dark = made.dark; else theme->onlyDark = made.dark;
            theme->Shown(preferences.dark) = made;
            if (theme->paired && theme->automatic) theme->SetAutomatic(true, preferences.dark);
        }
        ImGui::Separator();

        skin::Skin& edited = theme->Shown(preferences.dark);
        // Slider for a theme measure in `step` units. With `share`, the value shows as
        // a percentage of the built-in value.
        const auto slide = [&](const char* label, float& value, float low, float high, float step, bool share) {
            int index = static_cast<int>(std::lround((value - low) / step));
            char text[16];
            snprintf(text, sizeof(text), share ? "%.0f%%" : "%.0f", share ? (low + index * step) * 100 : low + index * step);
            if (!SettingSlider(label, "##measure", &index, 0, static_cast<int>(std::lround((high - low) / step)), "%d", s, dpi, text)) return false;
            value = low + index * step;
            return true;
        };
        bool reshaped = false;
        section("Shape");
        for (const auto& dial : ThemeDials()) {
            ImGui::PushID(dial.label);
            // Size rescales the whole editor, including this slider, so apply it only when
            // the slider is released.
            const bool size = std::string_view(dial.label) == "Size";
            float value = size && themeSizeDrag_ > 0 ? themeSizeDrag_ : dial.get(edited);
            if (slide(dial.label, value, dial.low, dial.high, dial.step, dial.step < 1)) {
                if (size) themeSizeDrag_ = value;
                else { dial.set(edited, value); reshaped = true; }
            }
            if (size && themeSizeDrag_ > 0 && !ImGui::IsItemActive()) { dial.set(edited, themeSizeDrag_); themeSizeDrag_ = 0; reshaped = true; }
            ImGui::PopID();
        }
        ImGui::Separator();

        const skin::Argb accentBefore = edited.accent.accent, okBefore = edited.accent.ok;
        bool changed = false;
        const char* group = "";
        bool detail = false, detailOpen = false;
        for (const auto& colour : ThemeColours()) {
            if (colour.derived && !detail) {
                detail = true;
                detailOpen = SettingSection("Fine detail", s, dpi);
            }
            if (colour.derived && !detailOpen) continue;
            if (!colour.derived && std::string_view(group) != colour.group) { group = colour.group; section(group); }
            ImGui::PushID(colour.key);
            changed |= ThemeSwatch(colour.label, colour.at(edited), colour.derived, s, dpi);
            ImGui::PopID();
        }
        // The remaining measures, each with its own slider; Size is handled above.
        if (detailOpen)
            for (const auto& number : ThemeNumbers()) {
                if (std::string_view(number.key) == "scale") continue;
                if (std::string_view(group) != number.group) { group = number.group; section(group); }
                ImGui::PushID(number.key);
                reshaped |= slide(number.label, number.at(edited), number.low, number.high, number.step, false);
                ImGui::PopID();
            }
        if (reshaped) theme->Reshaped(preferences.dark);
        if (changed) {
            // Fills and outlines are the accent at an alpha, so they follow a new accent
            // and keep their alpha.
            if (edited.accent.accent != accentBefore) {
                edited.accent.accentSoft = WithAlphaOf(edited.accent.accent, edited.accent.accentSoft);
                edited.accent.accentLine = WithAlphaOf(edited.accent.accent, edited.accent.accentLine);
            }
            if (edited.accent.ok != okBefore) {
                edited.accent.okSoft = WithAlphaOf(edited.accent.ok, edited.accent.okSoft);
                edited.accent.okBorder = WithAlphaOf(edited.accent.ok, edited.accent.okBorder);
            }
            theme->Edited(preferences.dark);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void Panels::DrawAutoVolume(const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine) {
    using A = ShellEngine::Action;
    const auto state = engine.Snapshot();
    const bool pending = state->autoVolumeCountdown > 0 || state->autoVolumeFocusing;
    if (!autoVolumeOpen) {
        if (volumeWasOpen_ && pending) engine.Send({A::AutoVolumeCancel});
        volumeWasOpen_ = false;
        return;
    }
    if (!volumeWasOpen_) {
        engine.Send({A::AutoVolumeScan});
        volumeWindow_ = state->volumeTarget;
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Pos.x + 24 * dpi,
                                      ImGui::GetMainViewport()->Pos.y + 112 * dpi));
        volumeWasOpen_ = true;
    }
    const ImGuiWindowClass windowClass = OwnWindowClass();
    ImGui::SetNextWindowClass(&windowClass);
    ImGui::SetNextWindowSize(ImVec2(440 * dpi, 0));
    const auto s = skin::ScaleGeometry(design, dpi);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * dpi, 16 * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    if (ImGui::Begin("AutoVol", &autoVolumeOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        ImGui::Spacing();
        if (state->autoVolume) {
            ImGui::TextUnformatted("AutoVol is on");
            ImGui::TextWrapped("Calibrated for: %s", state->volumeTarget.title.c_str());
        } else if (pending) {
            if (state->autoVolumeFocusing) ImGui::TextUnformatted("Focusing the selected game...");
            else ImGui::Text("Calibration starts in %d", state->autoVolumeCountdown);
        } else if (state->autoVolumeNeedsCalibration) ImGui::TextUnformatted("AutoVol is off until it is calibrated");
        else ImGui::TextUnformatted("AutoVol is off");
        ImGui::Spacing();
        ImGui::BeginDisabled(pending);
        ImGui::TextUnformatted("Game window");
        ImGui::SetNextItemWidth(-s.metric.controlHeight - 8 * dpi);
        const auto listed = [&] {
            return std::any_of(state->volumeWindows.begin(), state->volumeWindows.end(), [&](const auto& window) {
                return window.id == volumeWindow_.id && window.process == volumeWindow_.process && window.title == volumeWindow_.title;
            });
        };
        // Default to the first listed window when it is a known game.
        if (!listed() && !state->volumeWindows.empty() && state->volumeWindows.front().game)
            volumeWindow_ = state->volumeWindows.front();
        const bool selected = listed();
        if (ImGui::BeginCombo("##volume-window", selected ? volumeWindow_.title.c_str() : "Select the game window", ImGuiComboFlags_NoArrowButton)) {
            for (const auto& window : state->volumeWindows) {
                ImGui::PushID(reinterpret_cast<void*>(window.id));
                if (ImGui::Selectable(window.title.c_str(), window.id == volumeWindow_.id)) volumeWindow_ = window;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ComboChevron(); ImGui::SameLine();
        if (IconButton("##volume-refresh", Icon::Refresh, "Refresh game windows", s, dpi)) engine.Send({A::AutoVolumeScan});
        ImGui::Spacing();
        ImGui::BeginDisabled(!selected);
        if (EasedButton("Focus game and calibrate", ImVec2(-1, s.metric.controlHeight))) {
            ShellEngine::Command command{A::AutoVolumeCalibrate, {}, state->generation};
            command.window = volumeWindow_;
            engine.Send(std::move(command));
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (pending) {
            if (EasedButton("Cancel calibration", ImVec2(-1, s.metric.controlHeight))) engine.Send({A::AutoVolumeCancel});
        } else if (state->autoVolume || state->autoVolumeNeedsCalibration) {
            if (EasedButton("Turn AutoVol off", ImVec2(-1, s.metric.controlHeight))) engine.Send({A::AutoVolumeOff});
        }
        if (!state->error.empty()) ImGui::TextWrapped("%s", state->error.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (!autoVolumeOpen && pending) engine.Send({A::AutoVolumeCancel});
}

std::function<std::filesystem::path(HWND)> PickMidiFile = [](HWND hwnd) { return PickFile(hwnd); };
std::function<std::filesystem::path(HWND, bool, const std::string&)> PickThemeFile =
    [](HWND hwnd, bool save, const std::string& name) { return PickTheme(hwnd, save, name); };

Panels::~Panels() { if (measuring_) input_latency::stop(); }

// How far a theme's shape moves the layout from the built-ins, before DPI: a
// control's height, the window edge, a panel edge and the body text. Sizes
// below are the built-in number plus this growth, so a theme with taller
// controls or wider edges gets a larger window.
LayoutGrowth GrowthOf(const skin::Skin& design) {
    skin::Skin plain{};
    skin::Shape(plain);
    return {design.metric.controlHeight - plain.metric.controlHeight, design.spacing.windowPad - plain.spacing.windowPad,
            design.spacing.panelPad - plain.spacing.panelPad, design.type.body - plain.type.body, design.type.meta - plain.type.meta};
}

// Files column floor (240 for built-ins): two panel edges, the heading and the
// three buttons beside it. The heading grows with the body text or with its own
// size (14 for built-ins), whichever is larger.
float LeftColumnFloor(const skin::Skin& design) {
    const auto grow = GrowthOf(design);
    return 240.f + 2 * grow.panel + 3 * std::max(0.f, grow.control) + 8 * std::max(0.f, std::max(grow.text, design.type.heading - 14.f));
}

// Right column floor (600 for built-ins): two panel edges, plus the six
// controls and text of its widest row.
float RightColumnFloor(const skin::Skin& design) {
    const auto grow = GrowthOf(design);
    return 600.f + 2 * grow.panel + 6 * std::max(0.f, grow.control) + 24 * std::max(0.f, grow.text);
}

namespace {
// Settings, Help and MIDI devices popover width (344 for built-ins): two window
// edges and the text of Settings' longest switch label.
float PopoverWidth(const skin::Skin& design) {
    const auto grow = GrowthOf(design);
    return 344.f + 2 * std::max(0.f, grow.window) + 17 * std::max(0.f, grow.text);
}
}

ImVec2 Panels::DesiredSize() const {
    const skin::Skin design = ActiveSkin();
    const auto grow = GrowthOf(design);
    // Mini sizes to its content: the state pills plus window padding across, and
    // the 88 dp strip, rows 8 dp apart and the status bar down. Autoplay adds a
    // row, and a performer another. Each height leaves 8 dp below the last row.
    if (miniMode) {
        const int rows = miniAutoplay ? (performerRow_ ? 5 : 4) : 3;
        return ImVec2(528 + 2 * grow.window + 40 * std::max(0.f, grow.text),
                      (miniAutoplay ? (performerRow_ ? 274.f : 234.f) : 164.f) + rows * grow.control);
    }
    return ImVec2(940 + LeftColumnFloor(design) - 240 + RightColumnFloor(design) - 600 + 2 * grow.window, FullHeight());
}

float Panels::HeightGrowth(bool tracksOpen, bool velocityOpen) const {
    const auto grow = GrowthOf(ActiveSkin());
    // The strip; Playback's title and rows; Tracks' header plus three rows when
    // open; Velocity Response's header and its name row.
    const int controls = 1 + 1 + (performerRow_ ? 3 : 2) + (tracksOpen ? 4 : 1) + (velocityOpen ? 2 : 1);
    return 2 * grow.window + 6 * grow.panel + controls * grow.control;
}

float Panels::FullHeight() const {
    // The performer row on the Playback card adds 44.
    const float row = (performerRow_ ? 0.f : -44.f) + HeightGrowth(tracksExpanded, velocityExpanded);
    if (tracksExpanded) return (velocityExpanded ? 1018.f : 644.f) + row;
    // Closing Tracks removes its 200-unit table, and the window shrinks to match.
    return (velocityExpanded ? 862.f : 488.f) + row;
}

ImVec2 Panels::MinimumSize() const {
    const skin::Skin design = ActiveSkin();
    const auto grow = GrowthOf(design);
    return ImVec2(884 + LeftColumnFloor(design) - 240 + RightColumnFloor(design) - 600 + 2 * grow.window, 604 + HeightGrowth(true, false));
}

void Panels::SyncLayout(const EngineSnapshot& state) { performerRow_ = PerformerRow(state); }

namespace {
const char* BackendName(MidiBackend backend) {
    switch (backend) {
    case MidiBackend::WinMM: return "WinMM";
    case MidiBackend::KernelStreaming: return "Kernel Streaming";
    case MidiBackend::WootingAnalog: return "Wooting Analog";
    default: return "WinRT";
    }
}
std::string DeviceName(const EngineSnapshot& state) {
    if (state.liveDevice.empty()) return "No MIDI input";
    for (const auto& device : state.devices) if (device.id == state.liveDevice) return device.name;
    return "Connected MIDI input";
}
std::string OutputDeviceName(const EngineSnapshot& state) {
    if (state.outputDevice.empty()) return "No MIDI output";
    for (const auto& device : state.outputDevices) if (device.id == state.outputDevice) return device.name;
    return "Connected MIDI output";
}
void CurveCombo(const char* id, float width, const EngineSnapshot& state, ShellEngine& engine) {
    ImGui::SetNextItemWidth(width);
    const auto& edit = state.comparingCurve ? state.previousCurve : state.curve;
    // Clip the preview text before the chevron (the last 0.8 of a control height);
    // the frame's own clip rect would let it run under the chevron.
    std::string preview = state.ActiveVelocityName();
    const float room = width - 2 * ImGui::GetStyle().FramePadding.x - ImGui::GetFrameHeight() * .8f;
    if (ImGui::CalcTextSize(preview.c_str()).x > room) {
        while (!preview.empty() && ImGui::CalcTextSize((preview + "...").c_str()).x > room) {
            // Drop one UTF-8 character: its continuation bytes, then the lead byte.
            while (!preview.empty()) {
                const unsigned char last = static_cast<unsigned char>(preview.back());
                preview.pop_back();
                if ((last & 0xC0) != 0x80) break;
            }
        }
        preview += "...";
    }
    const bool curveOpen = ChevronCombo(id, preview.c_str());
    if (curveOpen) {
        for (size_t i = 0; i < state.curves.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(state.curves[i].name.c_str(), i == edit.preset))
                engine.Send({ShellEngine::Action::CurveSelect, {}, 0, i});
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}
void DrawCurveLine(ImDrawList* draw, const VelocityPreset& preset, const VelocityEdit& edit,
                   ImVec2 min, ImVec2 max, ImU32 color, float thickness) {
    for (int i = 0; i <= 96; ++i) {
        const float x = i / 96.f;
        draw->PathLineTo(ImVec2(min.x + x * (max.x - min.x), max.y - VelocityShape(preset, edit, x) * (max.y - min.y)));
    }
    draw->PathStroke(color, 0, thickness);
}

std::vector<float> PlayedVelocityTargets(const velocity_telemetry::Snapshot& played) {
    if (!played.total) return {};
    std::vector<float> targets;
    const auto quantile = [&](float fraction) {
        const uint32_t target = std::max(1u, static_cast<uint32_t>(std::ceil(played.total * fraction)));
        uint32_t count = 0;
        for (size_t i = 0; i < played.buckets.size(); ++i) {
            count += played.buckets[i];
            if (count >= target) return std::clamp((static_cast<float>(i) + .5f) * 4.f / 127.f, 0.f, 1.f);
        }
        return 1.f;
    };
    for (const float fraction : {.1f, .5f, .9f}) {
        const float value = quantile(fraction);
        if (targets.empty() || std::abs(value - targets.back()) > .02f) targets.push_back(value);
    }
    return targets;
}
}

void Panels::DrawVelocity(const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine,
                          ImVec2 min, ImVec2 max) {
    const auto state = engine.Snapshot();
    const auto s = skin::ScaleGeometry(design, dpi);
    BeginPanel("Velocity", min, max, s);
    ImGui::PushFont(fonts.Get(design), design.type.body * SpecFontScale(design));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    const auto start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, control = s.metric.controlHeight;
    const bool expanded = velocityExpanded;
    if (DisclosureHeading("##curve-disclosure", expanded, "Velocity Response", {}, fonts, design, s, dpi))
        velocityExpanded = !velocityExpanded;
    float comboEnd = start.x;
    if (!expanded) {
        ImGui::SameLine();
        // At least as wide as the longest preset name plus the chevron.
        float longest = 0;
        for (const auto& curve : state->curves) longest = std::max(longest, ImGui::CalcTextSize(curve.name.c_str()).x);
        // ComboChevron draws within the last 0.78 of the control height. Capped so a
        // long custom name (up to 120 bytes) can't push Sustain cutoff off the panel;
        // CurveCombo truncates the preview to fit.
        const float floor = std::min(longest + 2 * ImGui::GetStyle().FramePadding.x + control * .8f, width * .42f);
        CurveCombo("##collapsed-curve", std::max(floor, width - 430 * dpi), *state, engine);
        comboEnd = ImGui::GetItemRectMax().x;
    }
    // Sustain cutoff takes the remaining width; its groove shrinks first, down to 40px.
    float cutoffLabelWidth = 0;
    { FontScope meta(fonts, design, design.type.meta * SpecFontScale(design)); cutoffLabelWidth = ImGui::CalcTextSize("Sustain cutoff").x; }
    const float cutoffValueWidth = ImGui::CalcTextSize("127").x;
    // The group ends at the panel's right edge, the value right-aligned in its slot.
    const float cutoffFixed = cutoffLabelWidth + cutoffValueWidth + 2 * ImGui::GetStyle().ItemSpacing.x;
    const float grooveWidth = std::clamp(start.x + width - (comboEnd + s.spacing.s3) - cutoffFixed, 40 * dpi, 120 * dpi);
    const float cutoffX = std::max(comboEnd + s.spacing.s3, start.x + width - cutoffFixed - grooveWidth);
    ImGui::SetCursorScreenPos(ImVec2(cutoffX, start.y));
    // Centred on the row in the quiet ink, like the Playback row's labels.
    { FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
      const auto pos = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, pos.y + (control - ImGui::GetTextLineHeight()) / 2),
                                          Colour(s.ink.secondary), "Sustain cutoff");
      ImGui::Dummy(ImVec2(cutoffLabelWidth, control)); }
    ImGui::SameLine();
    // Commit on release, like the macro sliders.
    if (cutoffPending_ && (state->sustainCutoff == static_cast<int>(std::lround(cutoffPreview_)) || state->error != cutoffPendingError_))
        cutoffPending_ = false;
    float cutoff = (cutoffEditing_ || cutoffPending_) ? cutoffPreview_ : static_cast<float>(state->sustainCutoff);
    if (Groove("##sustain-cutoff", &cutoff, 0, 127, grooveWidth, control, s, dpi, true, 64.f)) {
        if (!ImGui::IsItemActive() || ImGui::IsItemDeactivatedAfterEdit()) {
            engine.Send({ShellEngine::Action::SustainCutoff, {}, 0, 0, false, std::round(cutoff)});
            cutoffPending_ = true; cutoffPendingError_ = state->error;
        }
        // A middle click resets the value without activating the slider, so only
        // a drag holds the preview; the pending hold covers the rest.
        cutoffPreview_ = cutoff; cutoffEditing_ = ImGui::IsItemActive();
    }
    if (cutoffEditing_ && ImGui::IsItemDeactivatedAfterEdit()) {
        engine.Send({ShellEngine::Action::SustainCutoff, {}, 0, 0, false, std::round(cutoffPreview_)});
        cutoffEditing_ = false; cutoffPending_ = true; cutoffPendingError_ = state->error;
    }
    char cutoffText[8]; snprintf(cutoffText, sizeof(cutoffText), "%.0f", cutoff);
    ImGui::SameLine(); ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cutoffValueWidth - ImGui::CalcTextSize(cutoffText).x);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(cutoffText);
    if (!expanded || state->curves.empty()) { ImGui::PopStyleVar(); ImGui::PopFont(); ImGui::EndChild(); return; }
    if (editorRevision_ != state->curveRevision || (!state->error.empty() && state->error != editorError_)) {
        editor_ = state->curve; editorRevision_ = state->curveRevision;
        cutoffEditing_ = false;
        // The curve was replaced mid-gesture; continuing would commit the dragged
        // point against an empty anchor list.
        curveGesture_ = false; activeAnchor_ = -1; freeDraw_.clear();
        if (state->curve.preset != namePreset_) nameOperation_ = 0;
    }
    editorError_ = state->error;
    const auto openName = [&](int operation) {
        nameOperation_ = operation; focusCurveName_ = true; namePreset_ = state->curve.preset;
        auto name = operation == 1 ? "New Curve" : state->curves[state->curve.preset].name;
        if (operation == 2 || (operation == 3 && state->curve.preset < midi::kBuiltinVelocityCurves)) name += " Copy";
        snprintf(curveName_, sizeof(curveName_), "%s", name.c_str());
    };
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + control + 12 * dpi));
    // A modified curve shows a filled Save button. It saves into the user's own
    // curve; a built-in can't be overwritten, so saving one prompts for a name.
    const bool changed = !state->comparingCurve && (!editor_.anchors.empty() || editor_.sensitivity != 0 || editor_.contrast != 0);
    const float saveWidth = changed ? ImGui::CalcTextSize("Save").x + 32 * dpi + 8 * dpi : 0.f;
    ImGui::AlignTextToFramePadding();
    Ellipsis(state->ActiveVelocityName(), width - 4 * control - 32 * dpi - saveWidth);
    if (changed) {
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - 4 * control - 24 * dpi - saveWidth, start.y + control + 12 * dpi));
        if (TransportButton("##save-curve", "Save", s, dpi, true)) {
            if (state->curve.preset < midi::kBuiltinVelocityCurves) openName(3);
            else engine.Send({ShellEngine::Action::CurveRename, {}, 0, 0, false, 0, state->curves[state->curve.preset].name});
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x + width - 4 * control - 24 * dpi, start.y + control + 12 * dpi));
    // The name row acts on the edited curve, which is hidden while comparing.
    ImGui::BeginDisabled(state->comparingCurve);
    if (IconButton("##duplicate-curve", Icon::Copy, "Duplicate curve", s, dpi)) openName(2);
    ImGui::SameLine();
    if (IconButton("##rename-curve", Icon::Rename, "Rename curve", s, dpi)) openName(3);
    ImGui::SameLine();
    ImGui::BeginDisabled(state->curve.preset < midi::kBuiltinVelocityCurves);
    if (IconButton("##delete-curve", Icon::Delete, "Delete curve", s, dpi)) {
        engine.Send({ShellEngine::Action::CurveDelete, {}, 0, state->curve.preset}); nameOperation_ = 0;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (IconButton("##new-curve", Icon::Plus, "New curve", s, dpi)) openName(1);
    ImGui::EndDisabled();
    if (state->comparingCurve) nameOperation_ = 0;
    float workspaceY = start.y + 2 * control + 24 * dpi;
    if (nameOperation_) {
        ImGui::SetCursorScreenPos(ImVec2(start.x, workspaceY));
        ImGui::SetNextItemWidth(width - 2 * control - 16 * dpi);
        if (focusCurveName_) { ImGui::SetKeyboardFocusHere(); focusCurveName_ = false; }
        const bool enter = ImGui::InputTextWithHint("##curve-name", "Curve name", curveName_, sizeof(curveName_),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool cancel = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::SameLine();
        const bool save = IconButton("##save-curve-name", Icon::Check, "Save curve name", s, dpi);
        ImGui::SameLine();
        if (IconButton("##cancel-curve-name", Icon::Close, "Cancel curve name", s, dpi) || cancel) nameOperation_ = 0;
        if ((enter || save) && nameOperation_ && curveName_[0] && namePreset_ == state->curve.preset) {
            const auto action = nameOperation_ == 1 ? ShellEngine::Action::CurveNew :
                nameOperation_ == 2 ? ShellEngine::Action::CurveDuplicate : ShellEngine::Action::CurveRename;
            engine.Send({action, {}, 0, 0, false, 0, curveName_}); nameOperation_ = 0;
        }
        workspaceY += control + 12 * dpi;
    }
    // Wide enough that no built-in name is cut short: the list's two 4px insets,
    // the 48 before a name, the check's 18 and its gap, the scrollbar a custom
    // curve brings, and 2 for the list's whole-pixel width.
    float longestPreset = 0;
    for (size_t i = 0; i < std::min(state->curves.size(), midi::kBuiltinVelocityCurves); ++i)
        longestPreset = std::max(longestPreset, ImGui::CalcTextSize(state->curves[i].name.c_str()).x);
    const float presetScrollbar = state->curves.size() > midi::kBuiltinVelocityCurves ? ImGui::GetStyle().ScrollbarSize : 0.f;
    // The cap limits only the proportional share, so a larger theme's type still fits.
    const float presetsWidth = std::max(std::min(236 * dpi, width * .36f), longestPreset + 76 * dpi + s.spacing.s2 + presetScrollbar),
                mainWidth = width - presetsWidth - 12 * dpi;
    const float graphHeight = 208 * dpi;
    const auto& shown = state->comparingCurve ? state->previousCurve : editor_;
    const auto& preset = state->comparingCurve ? state->previousPreset : state->curves[shown.preset];
    const ImVec2 graphMin(start.x, workspaceY), graphMax(start.x + mainWidth, workspaceY + graphHeight);
    auto* draw = ImGui::GetWindowDrawList();
    skin::RecessedRect(draw, graphMin, graphMax, s.radius.element, s);
    const ImVec2 plotMin(graphMin.x + 12 * dpi, graphMin.y + 40 * dpi), plotMax(graphMax.x - 12 * dpi, graphMax.y - 28 * dpi);

    // Selected graph tool gets an outline as well as the accent colour.
    ImGui::SetCursorScreenPos(ImVec2(graphMin.x + 8 * dpi, graphMin.y + 4 * dpi));
    ImGui::BeginDisabled(state->comparingCurve);
    if (IconButton("##anchor-tool", Icon::Anchor, "Edit anchors", s, dpi, curveTool_ == 0)) curveTool_ = 0;
    ImGui::SameLine();
    if (IconButton("##draw-tool", Icon::Draw, "Free draw", s, dpi, curveTool_ == 1)) curveTool_ = 1;
    ImGui::SameLine(0, 14 * dpi);
    ImGui::BeginDisabled(!state->canUndoCurve);
    if (IconButton("##curve-undo", Icon::Undo, "Undo curve edit", s, dpi)) engine.Send({ShellEngine::Action::CurveUndo});
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!state->canRedoCurve);
    if (IconButton("##curve-redo", Icon::Redo, "Redo curve edit", s, dpi)) engine.Send({ShellEngine::Action::CurveRedo});
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    // Only while the main window is focused: Key Mapping can bind "ctrl+z", and
    // that press must not also undo the curve.
    if (!state->comparingCurve && !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z))
            engine.Send({ImGui::GetIO().KeyShift ? ShellEngine::Action::CurveRedo : ShellEngine::Action::CurveUndo});
        else if (ImGui::IsKeyPressed(ImGuiKey_Y)) engine.Send({ShellEngine::Action::CurveRedo});
    }

    // Read here, not from the snapshot: each live note changes it, and only
    // this open graph draws it.
    const auto played = velocity_telemetry::snapshot();
    if (histogramRevision_ != played.revision) {
        histogramRevision_ = played.revision;
        const auto largest = *std::max_element(played.buckets.begin(), played.buckets.end());
        histogramVisible_ = played.total != 0 && largest != 0;
        for (size_t i = 0; i < histogramHeights_.size(); ++i)
            histogramHeights_[i] = largest ? static_cast<float>(played.buckets[i]) / largest : 0.f;
    }
    if (histogramVisible_) {
        const float barWidth = (plotMax.x - plotMin.x) / histogramHeights_.size();
        for (size_t i = 0; i < histogramHeights_.size(); ++i) {
            const float height = histogramHeights_[i] * (plotMax.y - plotMin.y) * .32f;
            if (height <= 0) continue;
            draw->AddRectFilled(ImVec2(plotMin.x + i * barWidth + dpi, plotMax.y - height),
                                ImVec2(plotMin.x + (i + 1) * barWidth - dpi, plotMax.y),
                                Colour(s.accent.accentSoft), 1.5f * dpi);
        }
    }
    for (int i = 1; i < 4; ++i) {
        const float x = plotMin.x + (plotMax.x - plotMin.x) * i / 4;
        const float y = plotMin.y + (plotMax.y - plotMin.y) * i / 4;
        draw->AddLine(ImVec2(x, plotMin.y), ImVec2(x, plotMax.y), Colour(s.border.hairline), dpi);
        draw->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), Colour(s.border.hairline), dpi);
    }
    // The identity response, drawn as a grid hairline since it's a reference, not
    // a series.
    draw->AddLine(ImVec2(plotMin.x, plotMax.y), ImVec2(plotMax.x, plotMin.y), Colour(s.border.hairline), dpi);

    // Most-played velocities, which anchors snap to. Drawn only while an anchor is
    // being dragged (below) to keep the graph uncluttered.
    const auto snapTargets = PlayedVelocityTargets(played);

    ImGui::SetCursorScreenPos(plotMin);
    ImGui::BeginDisabled(state->comparingCurve);
    ImGui::InvisibleButton("##curve-graph", ImVec2(plotMax.x - plotMin.x, plotMax.y - plotMin.y));
    const auto mousePoint = [&] {
        return VelocityPoint{
            std::clamp((ImGui::GetIO().MousePos.x - plotMin.x) / (plotMax.x - plotMin.x), 0.f, 1.f),
            std::clamp((plotMax.y - ImGui::GetIO().MousePos.y) / (plotMax.y - plotMin.y), 0.f, 1.f)};
    };
    const auto snapX = [&](float x) {
        float result = x, distance = 10 * dpi / (plotMax.x - plotMin.x);
        for (const float target : snapTargets) if (std::abs(target - x) <= distance) {
            result = target; distance = std::abs(target - x);
        }
        return result;
    };
    if (ImGui::IsItemActivated()) {
        curveGestureBase_ = editor_;
        curveGestureBase_.anchors = VelocityAnchorsFor(preset, editor_);
        curveGestureBase_.sensitivity = curveGestureBase_.contrast = 0;
        editor_ = curveGestureBase_;
        curveGesture_ = false;
        if (curveTool_ == 0) {
            const auto mouse = mousePoint();
            float nearest = 10 * dpi; activeAnchor_ = -1;
            for (size_t i = 0; i < editor_.anchors.size(); ++i) {
                const float dx = (editor_.anchors[i].x - mouse.x) * (plotMax.x - plotMin.x);
                const float dy = (editor_.anchors[i].y - mouse.y) * (plotMax.y - plotMin.y);
                const float distance = std::hypot(dx, dy);
                if (distance < nearest) { nearest = distance; activeAnchor_ = static_cast<int>(i); }
            }
            const float curveY = VelocityShape(preset, editor_, mouse.x);
            if (activeAnchor_ < 0 && std::abs(curveY - mouse.y) * (plotMax.y - plotMin.y) <= 10 * dpi)
                activeAnchor_ = static_cast<int>(VelocityAddAnchor(editor_.anchors, snapX(mouse.x), curveY));
            curveGesture_ = activeAnchor_ >= 0;
            // Move the anchor within the list captured at drag start. Moving it in the
            // previous frame's result lets a merge at a shared x shift the index onto a
            // neighbour, and keeps every flattening the drag passed through.
            dragAnchors_ = editor_.anchors; dragIndex_ = activeAnchor_;
        } else {
            freeDraw_.clear(); freeDraw_.push_back(mousePoint()); curveGesture_ = true;
        }
    }
    if (ImGui::IsItemActive() && curveGesture_) {
        auto point = mousePoint();
        if (curveTool_ == 0 && activeAnchor_ >= 0) {
            const auto mouse = ImGui::GetIO().MousePos;
            const bool inside = mouse.x >= plotMin.x && mouse.x <= plotMax.x && mouse.y >= plotMin.y && mouse.y <= plotMax.y;
            if (inside) {
                point.x = snapX(point.x);
                editor_.anchors = dragAnchors_;
                activeAnchor_ = static_cast<int>(VelocityMoveAnchor(editor_.anchors, dragIndex_, point));
            }
        } else if (curveTool_ == 1) {
            const auto& last = freeDraw_.back();
            const float dx = (last.x - point.x) * (plotMax.x - plotMin.x);
            const float dy = (last.y - point.y) * (plotMax.y - plotMin.y);
            if (std::hypot(dx, dy) >= 2 * dpi) freeDraw_.push_back(point);
        }
    }
    if (ImGui::IsItemActive() && curveGesture_ && curveTool_ == 0)
        for (const float target : snapTargets) {
            const float x = plotMin.x + target * (plotMax.x - plotMin.x);
            draw->AddLine(ImVec2(x, plotMin.y), ImVec2(x, plotMax.y), Colour(s.accent.accentSoft), 2 * dpi);
        }
    if (ImGui::IsItemDeactivated() && curveGesture_) {
        if (curveTool_ == 0 && activeAnchor_ >= 0) {
            const auto mouse = ImGui::GetIO().MousePos;
            const bool outside = mouse.x < plotMin.x || mouse.x > plotMax.x || mouse.y < plotMin.y || mouse.y > plotMax.y;
            if (outside && editor_.anchors.size() > 2 && activeAnchor_ > 0 &&
                activeAnchor_ + 1 < static_cast<int>(editor_.anchors.size()))
                editor_.anchors.erase(editor_.anchors.begin() + activeAnchor_);
        } else if (curveTool_ == 1 && freeDraw_.size() >= 2) {
            editor_.anchors = VelocityApplySweep(curveGestureBase_.anchors, freeDraw_);
        }
        // A click that moved nothing isn't an edit. Sending it would mark an untouched
        // built-in as "(edited)", stop playback and reopen the MIDI device.
        const auto& before = curveGestureBase_.anchors;
        const bool changed = editor_.anchors.size() != before.size() ||
            !std::equal(before.begin(), before.end(), editor_.anchors.begin(), [](const VelocityPoint& a, const VelocityPoint& b) {
                return std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f; });
        if (changed) {
            ShellEngine::Command command{ShellEngine::Action::CurveEdit};
            command.anchors = editor_.anchors; engine.Send(std::move(command));
        } else editor_ = state->curve;
        activeAnchor_ = -1; freeDraw_.clear(); curveGesture_ = false;
    } else if (ImGui::IsItemDeactivated()) {
        // Pressed on empty space: the press baked the preset into anchors and zeroed
        // both sliders, so restore them.
        editor_ = state->curve;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(curveTool_ == 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeAll);
    }
    ImGui::EndDisabled();

    // The 32 velocity steps the game receives, as a single soft fill whose top
    // edge is the staircase.
    const auto thresholds = VelocityThresholds(preset, shown);
    int previousThreshold = 0;
    const auto ghost = Colour((s.ink.tertiary & 0x00ffffffu) | 0x22000000u);
    for (int bucket = 0; bucket < 32; ++bucket) {
        const int edge = thresholds[bucket];
        if (edge <= previousThreshold) continue;
        const float x0 = plotMin.x + previousThreshold / 127.f * (plotMax.x - plotMin.x);
        const float x1 = plotMin.x + edge / 127.f * (plotMax.x - plotMin.x);
        const float y = plotMax.y - bucket / 31.f * (plotMax.y - plotMin.y);
        draw->AddRectFilled(ImVec2(x0, y), ImVec2(x1, plotMax.y), ghost);
        previousThreshold = edge;
    }
    DrawCurveLine(draw, preset, shown, plotMin, plotMax, Colour(s.accent.accent), 2 * dpi);
    if (!state->comparingCurve && curveTool_ == 0) for (size_t i = 0; i < editor_.anchors.size(); ++i) {
        const auto& anchor = editor_.anchors[i];
        const ImVec2 point(plotMin.x + anchor.x * (plotMax.x - plotMin.x),
                           plotMax.y - anchor.y * (plotMax.y - plotMin.y));
        const float radius = static_cast<int>(i) == activeAnchor_ ? 6 * dpi : 4.5f * dpi;
        draw->AddCircleFilled(point, radius, Colour(s.surface.elevated));
        draw->AddCircle(point, radius, Colour(s.accent.accent), 16, static_cast<int>(i) == activeAnchor_ ? 2 * dpi : dpi);
    }
    if (curveTool_ == 1 && curveGesture_ && freeDraw_.size() > 1) {
        for (const auto& point : freeDraw_)
            draw->PathLineTo(ImVec2(plotMin.x + point.x * (plotMax.x - plotMin.x),
                                    plotMax.y - point.y * (plotMax.y - plotMin.y)));
        draw->PathStroke(Colour(s.accent.accent), 0, 2.5f * dpi);
    }
    { FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
      if (played.last != 0) {
          const int input = played.last;
          const int output = VelocityBucket(thresholds, input);
          char readout[64]; snprintf(readout, sizeof(readout), "%d played > step %d", input, output + 1);
          draw->AddText(ImVec2(graphMax.x - 12 * dpi - ImGui::CalcTextSize(readout).x, graphMin.y + 8 * dpi), Colour(s.ink.primary), readout);
          const float response = VelocityShape(preset, shown, input / 127.f);
          const ImVec2 dot(plotMin.x + input / 127.f * (plotMax.x - plotMin.x),
                           plotMax.y - response * (plotMax.y - plotMin.y));
          draw->AddCircleFilled(dot, 7 * dpi, Colour(s.surface.card));
          draw->AddCircle(dot, 7 * dpi, Colour(s.accent.accent), 20, 2 * dpi);
          draw->AddCircleFilled(dot, 2.7f * dpi, Colour(s.accent.accent));
      }
      draw->AddText(ImVec2(plotMin.x, graphMax.y - 20 * dpi), Colour(s.ink.tertiary), "Gentle");
      const char* label = "How hard you play";
      draw->AddText(ImVec2(graphMin.x + (mainWidth - ImGui::CalcTextSize(label).x) / 2, graphMax.y - 20 * dpi), Colour(s.ink.secondary), label);
      draw->AddText(ImVec2(plotMax.x - ImGui::CalcTextSize("Firm").x, graphMax.y - 20 * dpi), Colour(s.ink.tertiary), "Firm"); }
    const float macroY = graphMax.y + 12 * dpi, macroWidth = (mainWidth - 12 * dpi) / 2;
    ImGui::BeginDisabled(state->comparingCurve);
    for (int i = 0; i < 2; ++i) {
        const float x = start.x + i * (macroWidth + 12 * dpi);
        skin::RecessedRect(draw, ImVec2(x, macroY), ImVec2(x + macroWidth, macroY + 80 * dpi), s.radius.element, s);
        const char* label = i ? "Contrast" : "Sensitivity";
        draw->AddText(ImVec2(x + 12 * dpi, macroY + 8 * dpi), Colour(s.ink.primary), label);
        const float value = i ? editor_.contrast : editor_.sensitivity;
        const char* description = i ? (value >= 72 ? "Dramatic" : value >= 42 ? "Clear dynamics" : value >= 16 ? "Gentle contrast" : "Even response") :
            (value >= 15 ? "Light touch" : value >= 5 ? "Slightly lighter" : value <= -15 ? "Firm touch" : value <= -5 ? "Slightly firmer" : "Neutral");
        { FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
          draw->AddText(ImVec2(x + 12 * dpi, macroY + 30 * dpi), Colour(s.ink.secondary), description); }
        ImGui::SetCursorScreenPos(ImVec2(x + 12 * dpi, macroY + 48 * dpi));
        float* target = i ? &editor_.contrast : &editor_.sensitivity;
        const bool changed = Groove(i ? "##contrast" : "##sensitivity", target, i ? 0.f : -50.f, i ? 100.f : 50.f,
            macroWidth - 24 * dpi, 24 * dpi, s, dpi, true, 0.f);
        if (changed) editor_.anchors.clear();
        if (ImGui::IsItemDeactivatedAfterEdit() || (changed && !ImGui::IsItemActive()))
            engine.Send({ShellEngine::Action::CurveAdjust, {}, 0, 0, false, *target, i ? "contrast" : "sensitivity"});
    }
    ImGui::EndDisabled();
    const float mainBottom = macroY + 80 * dpi;
    const ImVec2 listMin(start.x + mainWidth + 12 * dpi, workspaceY);
    skin::RecessedRect(draw, listMin, ImVec2(start.x + width, mainBottom), s.radius.element, s);
    ImGui::SetCursorScreenPos(ImVec2(listMin.x + 8 * dpi, listMin.y + 8 * dpi));
    { FontScope meta(fonts, design, design.type.meta * SpecFontScale(design)); ImGui::TextUnformatted("Starting points"); }
    ImGui::SetCursorScreenPos(ImVec2(listMin.x + 4 * dpi, listMin.y + 32 * dpi));
    // No item spacing between rows so all six built-ins fit without a scrollbar;
    // custom curves still scroll.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
    ImGui::BeginChild("##curve-list", ImVec2(presetsWidth - 8 * dpi, mainBottom - listMin.y - 36 * dpi), 0, ImGuiWindowFlags_NoBackground);
    for (size_t i = 0; i < state->curves.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const auto row = ImGui::GetCursorScreenPos();
        const float rowWidth = ImGui::GetContentRegionAvail().x, rowHeight = 40 * dpi;
        const bool selected = i == shown.preset;
        if (ImGui::Selectable("##preset", selected, 0, ImVec2(rowWidth, rowHeight))) {
            engine.Send({ShellEngine::Action::CurveSelect, {}, 0, i}); nameOperation_ = 0;
        }
        auto* listDraw = ImGui::GetWindowDrawList();
        VelocityEdit plain; plain.preset = i;
        DrawCurveLine(listDraw, state->curves[i], plain, ImVec2(row.x + 4 * dpi, row.y + 8 * dpi),
            ImVec2(row.x + 40 * dpi, row.y + 32 * dpi), Colour(selected ? s.accent.accent : s.ink.tertiary), 1.5f * dpi);
        // Every row keeps the check's room and a gap, so a name truncates the
        // same whether or not its row is selected.
        DrawEllipsis(state->curves[i].name, rowWidth - 48 * dpi - 18 * dpi - s.spacing.s2,
                     ImVec2(row.x + 48 * dpi, row.y + (rowHeight - ImGui::GetTextLineHeight()) / 2));
        if (selected) DrawIcon(listDraw, Icon::Check, ImVec2(row.x + rowWidth - 18 * dpi, row.y + 12 * dpi), 16 * dpi, Colour(s.accent.accent), dpi);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", state->curves[i].name.c_str(), i >= midi::kBuiltinVelocityCurves ? " (custom)" : "");
        if (selected && listRevision_ != state->curveRevision) ImGui::SetScrollHereY(.5f);
        ImGui::PopID();
    }
    listRevision_ = state->curveRevision;
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(start.x, mainBottom + 4 * dpi)); ImGui::Dummy(ImVec2(width, 1));
    ImGui::PopStyleVar(); ImGui::PopFont();
    ImGui::EndChild();
}

// Device popover: input and output, the Wooting's three settings while a
// Wooting is the input, and Midi2Key with its channel.
void Panels::DrawMidiDevices(const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine) {
    const auto state = engine.Snapshot();
    const auto s = skin::ScaleGeometry(design, dpi);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    const auto section = [&](const char* label) {
        FontScope meta(fonts, design, design.type.meta * SpecFontScale(design), Weight::Semibold);
        ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
        ImGui::TextUnformatted(label); ImGui::PopStyleColor();
    };
    section("MIDI input");
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    const auto groups = GroupDevices(state->devices);
    const auto* selectedGroup = SelectedGroup(groups, state->liveDevice);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - s.metric.controlHeight - 8 * dpi);
    const bool deviceOpen = ChevronCombo("##midi-input", DeviceName(*state).c_str());
    if (deviceOpen) {
        if (ImGui::Selectable("No MIDI input", state->liveDevice.empty())) engine.Send({ShellEngine::Action::LiveOpen});
        for (size_t i = 0; i < groups.size(); ++i) {
            const auto& group = groups[i];
            ImGui::PushID(static_cast<int>(i));
            const auto name = group.name + (group.ambiguous ? " (port " + std::to_string(i + 1) + ")" : "");
            if (ImGui::Selectable(name.c_str(), selectedGroup == &group)) {
                ShellEngine::Command command{ShellEngine::Action::LiveOpen};
                command.device = PreferredInput(group, state->liveDevice); engine.Send(std::move(command));
            }
            if (group.ambiguous && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s", BackendName(group.inputs.front().backend),
                    Utf8(std::filesystem::path(group.inputs.front().id)).c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (IconButton("##scan-midi", Icon::Refresh, "Scan MIDI inputs", s, dpi)) engine.Send({ShellEngine::Action::LiveScan});
    // Always shown so Kernel Streaming is reachable before a keyboard is chosen;
    // with none, it shows the transport the first keyboard will open on.
    {
        ImGui::BeginDisabled(!selectedGroup);
        ImGui::TextUnformatted("Transport");
        ImGui::SetNextItemWidth(-1);
        const bool transportOpen = ChevronCombo("##device-transport",
            BackendName(selectedGroup ? BackendForDeviceId(state->liveDevice) : MidiBackend::KernelStreaming));
        if (transportOpen && !selectedGroup) ImGui::EndCombo();
        else if (transportOpen) {
            for (const auto& input : selectedGroup->inputs) {
                if (ImGui::Selectable(BackendName(input.backend), input.id == state->liveDevice)) {
                    ShellEngine::Command command{ShellEngine::Action::LiveOpen}; command.device = input.id; engine.Send(std::move(command));
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    section("MIDI output");
    if (SettingRadio("Keystrokes", !state->outputMidi, design, dpi))
        engine.Send({ShellEngine::Action::OutputTarget, {}, 0, 0, false});
    ImGui::SameLine(0, 16 * dpi);
    if (SettingRadio("MIDI", state->outputMidi, design, dpi))
        engine.Send({ShellEngine::Action::OutputTarget, {}, 0, 0, true});

    const auto outputGroups = GroupDevices(state->outputDevices);
    const auto* selectedOutputGroup = SelectedGroup(outputGroups, state->outputDevice);
    ImGui::BeginDisabled(!state->outputMidi);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - s.metric.controlHeight - 8 * dpi);
    const bool outputOpen = ChevronCombo("##midi-output", OutputDeviceName(*state).c_str());
    if (outputOpen) {
        if (ImGui::Selectable("No MIDI output", state->outputDevice.empty()))
            engine.Send({ShellEngine::Action::OutputOpen});
        for (size_t i = 0; i < outputGroups.size(); ++i) {
            const auto& group = outputGroups[i];
            ImGui::PushID(static_cast<int>(i));
            const auto name = group.name + (group.ambiguous ? " (port " + std::to_string(i + 1) + ")" : "");
            if (ImGui::Selectable(name.c_str(), selectedOutputGroup == &group)) {
                ShellEngine::Command command{ShellEngine::Action::OutputOpen};
                command.device = PreferredInput(group, state->outputDevice); engine.Send(std::move(command));
            }
            if (group.ambiguous && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s", BackendName(group.inputs.front().backend),
                    Utf8(std::filesystem::path(group.inputs.front().id)).c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (IconButton("##scan-midi-output", Icon::Refresh, "Scan MIDI outputs", s, dpi))
        engine.Send({ShellEngine::Action::OutputScan});
    if (selectedOutputGroup) {
        ImGui::BeginDisabled(!state->outputMidi);
        ImGui::TextUnformatted("Transport");
        ImGui::SetNextItemWidth(-1);
        const bool outputTransportOpen = ChevronCombo("##output-transport",
            BackendName(BackendForOutputId(state->outputDevice)));
        if (outputTransportOpen) {
            for (const auto& output : selectedOutputGroup->inputs) {
                if (ImGui::Selectable(BackendName(output.backend), output.id == state->outputDevice)) {
                    ShellEngine::Command command{ShellEngine::Action::OutputOpen};
                    command.device = output.id; engine.Send(std::move(command));
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    const bool wootingSelected = state->liveDevice == L"wooting:analog" &&
        BackendForDeviceId(state->liveDevice) == MidiBackend::WootingAnalog;
    if (wootingSelected) {
        section("Wooting Analog");
        const std::array<float, 3> current{
            static_cast<float>(state->wootingTriggerThreshold),
            static_cast<float>(state->wootingShiftAmount),
            static_cast<float>(state->wootingVelocityScale)};
        const auto closeEnough = [](float a, float b) { return std::abs(a - b) < .001f; };
        for (size_t i = 0; i < current.size(); ++i) {
            if (wootingPending_[i] && closeEnough(current[i], wootingPreview_[i])) wootingPending_[i] = false;
            if (!wootingEditing_[i] && !wootingPending_[i]) wootingPreview_[i] = current[i];
        }
        const auto setting = [&](size_t index, const char* label, const char* id, float low, float high, float step, float rest,
                                 ShellEngine::Action action, const char* format) {
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            const float rounded = std::clamp(std::round(wootingPreview_[index] / step) * step, low, high);
            char text[32]; snprintf(text, sizeof(text), format, rounded);
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(text).x));
            ImGui::TextUnformatted(text);
            float value = wootingPreview_[index];
            const bool changed = Groove(id, &value, low, high, ImGui::GetContentRegionAvail().x,
                                        22 * dpi, s, dpi, true, rest);
            value = std::clamp(std::round(value / step) * step, low, high);
            if (changed) {
                wootingPreview_[index] = value;
                ShellEngine::Command command{action}; command.amount = value;
                command.value = !ImGui::IsItemActive();
                engine.Send(std::move(command));
                wootingEditing_[index] = ImGui::IsItemActive();
                wootingPending_[index] = !wootingEditing_[index];
            }
            if (wootingEditing_[index] && ImGui::IsItemDeactivatedAfterEdit()) {
                ShellEngine::Command command{action}; command.amount = wootingPreview_[index]; command.value = true;
                engine.Send(std::move(command));
                wootingEditing_[index] = false;
                wootingPending_[index] = true;
            }
        };
        setting(0, "Note trigger threshold", "##wooting-trigger", .01f, 1.f, .01f, .25f,
                ShellEngine::Action::WootingTriggerThreshold, "%.2f");
        setting(1, "Shift amount", "##wooting-shift", -127.f, 127.f, 1.f, 1.f,
                ShellEngine::Action::WootingShiftAmount, "%+.0f semitones");
        setting(2, "Velocity scale", "##wooting-velocity", .1f, 20.f, .1f, 2.f,
                ShellEngine::Action::WootingVelocityScale, "%.1f");
        ImGui::Separator();
    } else {
        wootingEditing_.fill(false);
        wootingPending_.fill(false);
    }
    bool active = state->liveActive;
    ImGui::BeginDisabled(state->liveDevice.empty());
    if (SettingSwitch("Midi2Key", active, nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::LiveActive, {}, 0, 0, active});
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1);
    const auto channel = state->liveChannel < 0 ? "Every channel" : "Channel " + std::to_string(state->liveChannel + 1);
    const bool channelOpen = ChevronCombo("##live-channel", channel.c_str());
    if (channelOpen) {
        for (int i = -1; i < 16; ++i) {
            const auto label = i < 0 ? "Every channel" : "Channel " + std::to_string(i + 1);
            if (ImGui::Selectable(label.c_str(), state->liveChannel == i))
                engine.Send({ShellEngine::Action::LiveChannel, {}, 0, 0, false, static_cast<double>(i)});
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleVar();
}

void Panels::DrawSettings(const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine) {
    const auto state = engine.Snapshot();
    const auto s = skin::ScaleGeometry(design, dpi);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    const auto section = [&](const char* label) {
        FontScope meta(fonts, design, design.type.meta * SpecFontScale(design), Weight::Semibold);
        ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
        ImGui::TextUnformatted(label); ImGui::PopStyleColor();
    };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    // Closed by default since it's a diagnostic; a measurement keeps running while
    // the header is closed.
    if (SettingSection("Keyboard timing", s, dpi)) {
    bool measure = measuring_;
    if (SettingSwitch("Measure keyboard timing", measure, nullptr, fonts, design, dpi)) {
        if (measure) { timing_ = input_latency::Collector{}; timingSummary_ = {}; measuring_ = input_latency::start(); }
        else { input_latency::stop(); measuring_ = false; timingSummary_ = {}; }
    }
    if (!measuring_ && input_latency::hookError()) ImGui::Text("Hook error %lu", input_latency::hookError());
    ImGui::SetNextItemWidth(-1);
    const char* sourceLabels[]{"Live input", "Autoplay"};
    if (ChevronCombo("##timing-source", sourceLabels[timingSource_])) {
        for (int i = 0; i < 2; ++i)
            if (ImGui::Selectable(sourceLabels[i], timingSource_ == i) && timingSource_ != i) { timingSource_ = i; timingSummary_ = {}; nextTimingPoll_ = 0; }
        ImGui::EndCombo();
    }
    if (measuring_) {
        const auto& t = timingSummary_;
        if (t.callbackToHookMs.count) {
            ImGui::Text("Callback to hook: %.3f ms median", t.callbackToHookMs.p50);
            ImGui::Text("p95 %.3f ms   p99 %.3f ms", t.callbackToHookMs.p95, t.callbackToHookMs.p99);
            ImGui::Text("Preparation %.3f ms   Calls %.3f ms", t.preparationMs.p50, t.callsMs.p50);
            ImGui::Text("%zu notes   %.2f events/note", t.notes, t.eventsPerNote);
            const auto graph = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            auto* draw = ImGui::GetWindowDrawList();
            skin::RecessedRect(draw, graph, ImVec2(graph.x + width, graph.y + 8 * dpi), 4 * dpi, s);
            const float observed = static_cast<float>(t.callbackToHookMs.count) / std::max(size_t{1}, t.notes);
            if (observed > 0) draw->AddRectFilled(graph, ImVec2(graph.x + width * observed, graph.y + 8 * dpi), Colour(s.accent.okInk), 4 * dpi);
            ImGui::Dummy(ImVec2(width, 8 * dpi));
            ImGui::Text("%zu of %zu notes fully observed", t.callbackToHookMs.count, t.notes);
        }
        ImGui::Text("%zu incomplete   %llu failures   %llu dropped", t.incomplete,
            static_cast<unsigned long long>(t.failures), static_cast<unsigned long long>(input_latency::dropped()));
    }
    }
    ImGui::Separator();
    section("Behaviour");
    ImGui::BeginDisabled(state->eightyEightKeys);
    bool outRange = state->outRange;
    if (SettingSwitch("Fold out-of-range notes onto the keys", outRange,
        nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::OutRange, {}, 0, 0, outRange});
    ImGui::EndDisabled();
    int playbackDelay = state->playbackDelay;
    if (SettingSlider("Play button countdown", "##playback-delay", &playbackDelay, 0, 10, "%d seconds", s, dpi, nullptr, 3))
        engine.Send({ShellEngine::Action::PlaybackDelay, {}, 0, 0, false, static_cast<double>(playbackDelay)});
    int seekStep = state->seekStep;
    if (SettingSlider("Skip step", "##seek-step", &seekStep, 1, 60, "%d seconds", s, dpi, nullptr, 10))
        engine.Send({ShellEngine::Action::SeekStep, {}, 0, 0, false, static_cast<double>(seekStep)});
    // Speed slider range in 0.05 steps. It always spans 1 so the reset-to-1 button
    // stays in range.
    for (const bool top : {false, true}) {
        int steps = static_cast<int>(std::lround((top ? state->speedMax : state->speedMin) * 20));
        char shown[16]; snprintf(shown, sizeof(shown), "%.2f\xc3\x97", steps / 20.0);
        if (SettingSlider(top ? "Maximum speed" : "Minimum speed", top ? "##speed-max" : "##speed-min", &steps,
                          top ? 20 : 1, top ? 160 : 20, "%d", s, dpi, shown, top ? 40 : 5))
            engine.Send({top ? ShellEngine::Action::SpeedMax : ShellEngine::Action::SpeedMin, {}, 0, 0, false, steps / 20.0});
    }
    // The pill already shows on/off; only calibration pending needs a suffix.
    if (EasedButton(state->autoVolumeNeedsCalibration ? "AutoVol: calibrate" : "AutoVol", ImVec2(-1, s.metric.controlHeight))) {
        autoVolumeOpen = true;
        ImGui::CloseCurrentPopup();
    }
    if (SettingSwitch("Solo piano tracks on load", preferences.autoSolo, nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::AutoSolo, {}, 0, 0, preferences.autoSolo});
    if (revealSettingsSwitches) ImGui::SetScrollHereY(0.f);
    // Performer switch and section, only when the add-on is present.
    const auto& performer = state->performer;
    const auto performerControl = [&](const PerformerControl& control) {
        const auto send = [&](double amount) {
            ShellEngine::Command command{ShellEngine::Action::PerformerValue, {}, 0, 0, false, amount};
            command.key = control.id; engine.Send(std::move(command));
        };
        // Skip controls shown on the panel, not drawn for this song, or awaiting a choice.
        if ((control.isChoice && control.panel) || !performer.Drawn(control)) return;
        if (control.isSwitch) {
            bool on = control.value != 0;
            if (SettingSwitch(control.name.c_str(), on, nullptr, fonts, design, dpi)) send(on ? 1 : 0);
            return;
        }
        const std::string id = "##performer-" + control.id;
        if (control.isChoice) {
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(control.name.c_str());
            std::vector<const char*> names;
            for (const auto& choice : control.choices) names.push_back(choice.c_str());
            if (const int chosen = Segments(id.c_str(), names, static_cast<int>(control.value), s, dpi, 0); chosen >= 0) send(chosen);
            return;
        }
        const float shown = static_cast<float>(performer.Shown(control));
        // Displayed as a note: the groove moves in semitones and the value is the note name.
        if (control.showsNote) {
            const float span = static_cast<float>(control.high - control.low);
            const auto note = [&](double share) { return static_cast<int>(std::lround(control.low + share * span)); };
            float value = control.value < 0 ? control.low - 1.f : static_cast<float>(note(control.value));
            if (!performer.Estimated(control)) {
                int semitone = note(shown);
                if (SettingSlider(control.name.c_str(), id.c_str(), &semitone, control.low, control.high, "%d", s, dpi, NoteName(semitone).c_str()))
                    send((semitone - control.low) / span);
            } else if (EstimatedSlider(control.name.c_str(), id.c_str(), &value, static_cast<float>(note(control.estimate)),
                                static_cast<float>(control.low), static_cast<float>(control.high), NoteName(note(shown)).c_str(), fonts, design, s, dpi))
                send(value < control.low ? -1.0 : (std::round(value) - control.low) / span);
            return;
        }
        // With its estimate off, this is an ordinary slider.
        if (performer.Estimated(control)) {
            float value = static_cast<float>(control.value);
            char text[16]; snprintf(text, sizeof(text), "%d%%", static_cast<int>(std::lround(shown * 100)));
            if (EstimatedSlider(control.name.c_str(), id.c_str(), &value, static_cast<float>(control.estimate), 0.f, 1.f, text, fonts, design, s, dpi))
                send(static_cast<double>(value));
        } else {
            int percent = static_cast<int>(std::lround(shown * 100));
            if (SettingSlider(control.name.c_str(), id.c_str(), &percent, 0, 100, "%d%%", s, dpi)) send(percent / 100.0);
        }
    };
    if (performer.loaded) {
        bool on = performer.on;
        const ImVec2 switchMin = ImGui::GetCursorScreenPos();
        const float labelWidth = ImGui::CalcTextSize(performer.name.c_str()).x;
        if (SettingSwitch(performer.name.c_str(), on, nullptr, fonts, design, dpi))
            engine.Send({ShellEngine::Action::Performer, {}, 0, 0, on});
        if (!performer.tag.empty()) {
            // A one-word warning in the warning colour.
            FontScope meta(fonts, design, design.type.meta * SpecFontScale(design), Weight::Semibold);
            const ImVec2 text = ImGui::CalcTextSize(performer.tag.c_str());
            const float padX = 6 * dpi, height = text.y + 4 * dpi;
            const ImVec2 tagMin(switchMin.x + labelWidth + s.spacing.s2, switchMin.y + (s.metric.controlHeight - height) / 2);
            const ImVec2 tagMax(tagMin.x + text.x + 2 * padX, tagMin.y + height);
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRect(tagMin, tagMax, Colour(s.accent.warn), height / 2, 0, dpi);
            draw->AddText(ImVec2(tagMin.x + padX, tagMin.y + 2 * dpi), Colour(s.accent.warn), performer.tag.c_str());
        }
    }
    if (performer.on) {
        // Choosing a preset sets the sliders to its values; after that the user owns them.
        if (!performer.presets.empty()) {
            std::vector<const char*> names;
            for (const auto& preset : performer.presets) names.push_back(preset.c_str());
            if (const int preset = Segments("##performer-preset", names, performer.preset, s, dpi); preset >= 0)
                engine.Send({ShellEngine::Action::PerformerPreset, {}, 0, static_cast<size_t>(preset)});
        }
        for (const auto& control : performer.controls) if (control.trigger < 0) performerControl(control);
    }
    // Controls specific to the chosen trigger.
    if (state->trigger > 0)
        for (const auto& control : performer.controls) if (control.trigger == state->trigger) performerControl(control);
    bool shuffle = state->shuffle;
    if (SettingSwitch("Shuffle Play", shuffle, nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::Shuffle, {}, 0, 0, shuffle});
    bool detectDrums = state->detectDrums;
    if (SettingSwitch("Detect drum tracks", detectDrums, nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::DetectDrums, {}, 0, 0, detectDrums});
    bool autoTranspose = state->autoTranspose;
    if (SettingSwitch("Auto-transpose on load", autoTranspose, nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::AutoTranspose, {}, 0, 0, autoTranspose});
    bool velocity = state->velocity;
    const std::string modifierName = state->velocityModifier == "ctrl" ? "Ctrl" :
        state->velocityModifier == "shift" ? "Shift" : "Alt";
    ImGui::BeginDisabled(state->outputMidi);
    if (SettingSwitch("Velocity hotkeys", velocity,
        nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::Velocity, {}, 0, 0, velocity});
    ImGui::EndDisabled();
    ImGui::TextUnformatted("Velocity modifier");
    ImGui::SetNextItemWidth(-1);
    const bool modifierOpen = ChevronCombo("##velocity-modifier", modifierName.c_str());
    if (modifierOpen) {
        for (const auto& [value, label] : {std::pair{"alt", "Alt"}, std::pair{"ctrl", "Ctrl"}, std::pair{"shift", "Shift"}}) {
            if (ImGui::Selectable(label, state->velocityModifier == value)) {
                ShellEngine::Command command{ShellEngine::Action::VelocityModifier};
                command.key = value; engine.Send(std::move(command));
            }
        }
        ImGui::EndCombo();
    }
    if (!state->velocityModifierConflicts.empty()) {
        // Combinations this modifier shares with mapped notes, listed in the warning
        // colour under the control.
        std::string warning;
        for (size_t i = 0; i < state->velocityModifierConflicts.size(); ++i) {
            if (i) warning += "   ";
            warning += state->velocityModifierConflicts[i];
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.accent.warn));
        ImGui::TextWrapped("%s", warning.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(!state->hasPreviousCurve);
    if (SettingSection("Curve comparison", s, dpi)) {
        if (EasedButton(state->comparingCurve ? "Return to edited curve" : "Hear previous curve",
                          ImVec2(-1, s.metric.controlHeight)))
            engine.Send({ShellEngine::Action::CurveCompare});
    }
    ImGui::EndDisabled();
    SettingSwitch("Always on top", preferences.alwaysOnTop, nullptr, fonts, design, dpi);
    ImGui::Separator();
    section("Hotkeys");
    if (revealSettingsHotkeys) ImGui::SetScrollHereY(0.f);
    SettingSwitch("Media keys", preferences.mediaKeys, nullptr, fonts, design, dpi);
    {
        // Action, its key as a keycap, and a button to unbind it. When armed, the cap
        // is empty inside the accent ring, as in Key Mapping. A key held by another
        // program uses the legend's dead-key ink.
        const char* actions[kAppHotkeys]{"Play/Pause", "Skip back", "Skip forward", "Stop", "Previous song", "Next song"};
        const float height = s.metric.controlHeight, gap = 8 * dpi, capWidth = 112 * dpi;
        auto* draw = ImGui::GetWindowDrawList();
        const auto hotkeyRow = [&](size_t i, const std::string& action) {
            ImGui::PushID(static_cast<int>(i));
            const auto min = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const bool armed = hotkeyCapture == static_cast<int>(i), bound = !state->hotkeys[i].empty();
            DrawEllipsis(action, width - capWidth - height - 2 * gap,
                         ImVec2(min.x, min.y + (height - ImGui::GetTextLineHeight()) / 2));
            ImGui::SetCursorScreenPos(ImVec2(min.x + width - height - gap - capWidth, min.y));
            const bool dead = bound && !armed && !transportKeysAvailable[i];
            if (dead) ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.tertiary));
            // Draw the key name separately rather than as the button label: a key named
            // "#" would make the label "###cap", which ImGui treats as empty.
            const std::string cap = armed ? std::string() : HotkeyLabel(state->hotkeys[i]);
            if (EasedButton("##cap", ImVec2(capWidth, height))) hotkeyCapture = armed ? -1 : static_cast<int>(i);
            if (!cap.empty()) {
                const ImVec2 capMin = ImGui::GetItemRectMin(), size = ImGui::CalcTextSize(cap.c_str(), nullptr, false);
                draw->AddText(ImVec2(capMin.x + (capWidth - size.x) / 2, capMin.y + (height - size.y) / 2),
                              ImGui::GetColorU32(ImGuiCol_Text), cap.c_str());
            }
            if (dead) ImGui::PopStyleColor();
            if (armed) draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Colour(s.accent.accent), s.radius.control, 0, 2 * dpi);
            ImGui::SameLine(0, gap);
            // No unbind button for an unbound key.
            if (!bound) ImGui::Dummy(ImVec2(height, height));
            else if (IconButton("##unbind", Icon::Close, "Unbind", s, dpi)) {
                ShellEngine::Command command{ShellEngine::Action::Hotkey};
                command.track = i; engine.Send(std::move(command));
                hotkeyCapture = -1;
            }
            ImGui::PopID();
        };
        for (size_t i = 0; i < kAppHotkeys; ++i) hotkeyRow(i, actions[i]);
        // Performer trigger keys, shown while the performer is on. A single key gets a
        // row like the app's hotkeys. A key set is one wrapping row: a cap per key with
        // a remove cross, then a plus that captures the next key.
        for (const auto& trigger : state->performer.triggers) {
            if (!state->performer.on || trigger.keyFields.empty()) continue;
            if (trigger.keyFields.size() == 1) { hotkeyRow(trigger.firstKey, trigger.keysName); continue; }
            const size_t firstKey = trigger.firstKey, endKey = trigger.firstKey + trigger.keyFields.size();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(trigger.keysName.c_str());
            const float left = ImGui::GetCursorScreenPos().x, rowWidth = ImGui::GetContentRegionAvail().x;
            const float cross = 12 * dpi, capPad = 10 * dpi, chipGap = 6 * dpi;
            float x = left;
            const auto place = [&](float width) {
                if (x > left && x + width > left + rowWidth) x = left; // wrap to the next row without SameLine
                else if (x > left) ImGui::SameLine(0, chipGap);
                x += width + chipGap;
            };
            int firstFree = -1;
            bool anyArmed = false;
            for (size_t i = firstKey; i < endKey; ++i) {
                const bool armed = hotkeyCapture == static_cast<int>(i), bound = !state->hotkeys[i].empty();
                anyArmed |= armed;
                if (!bound && !armed) { if (firstFree < 0) firstFree = static_cast<int>(i); continue; }
                ImGui::PushID(static_cast<int>(i));
                const std::string name = armed ? std::string() : HotkeyLabel(state->hotkeys[i]);
                const float width = armed ? 56 * dpi : 2 * capPad + ImGui::CalcTextSize(name.c_str()).x + s.spacing.s1 + cross;
                place(width);
                const ImVec2 min = ImGui::GetCursorScreenPos();
                const bool pressed = EasedButton("##cap", ImVec2(width, height));
                const bool overCross = !armed && ImGui::IsItemHovered() && ImGui::GetMousePos().x >= min.x + width - capPad - cross - s.spacing.s1;
                if (armed) draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Colour(s.accent.accent), s.radius.control, 0, 2 * dpi);
                else {
                    const bool dead = !transportKeysAvailable[i];
                    draw->AddText(ImVec2(min.x + capPad, min.y + (height - ImGui::GetTextLineHeight()) / 2),
                                  ImGui::GetColorU32(Colour(dead ? s.ink.tertiary : s.ink.primary)), name.c_str());
                    DrawIcon(draw, Icon::Close, ImVec2(min.x + width - capPad - cross, min.y + (height - cross) / 2), cross,
                             ImGui::GetColorU32(Colour(overCross ? s.ink.primary : s.ink.tertiary)), dpi);
                    if (overCross) ImGui::SetTooltip("Unbind");
                }
                if (pressed && overCross) {
                    ShellEngine::Command command{ShellEngine::Action::Hotkey};
                    command.track = i; engine.Send(std::move(command));
                    hotkeyCapture = -1;
                } else if (pressed) hotkeyCapture = armed ? -1 : static_cast<int>(i);
                ImGui::PopID();
            }
            if (firstFree >= 0 && !anyArmed) {
                place(height);
                ImGui::PushID(static_cast<int>(firstKey));
                if (IconButton("##add-key", Icon::Plus, trigger.keysAdd.c_str(), s, dpi)) hotkeyCapture = firstFree;
                ImGui::PopID();
            }
            if (revealSettingsTapKeys) ImGui::SetScrollHereY(1.f);
        }
    }
    ImGui::Separator();
    section("Appearance");
    SettingSlider("Window opacity", "##window-opacity", &preferences.opacity, 40, 100, "%d%%", s, dpi, nullptr, 100);
    // All themes by name, built-ins first. Customise opens the editor on the chosen
    // theme, or on a copy of a built-in.
    {
        const Theme& active = themes.Active(preferences.theme);
        const bool custom = !active.builtin;
        const float button = ImGui::CalcTextSize("Customise").x + 24 * dpi, icon = s.metric.controlHeight, gap = 8 * dpi;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - gap - (custom ? icon + gap : 0));
        const bool open = ChevronCombo("##theme", active.name.c_str());
        if (open) {
            for (const auto& theme : themes.All()) {
                ImGui::PushID(theme.id.c_str());
                if (ImGui::Selectable(theme.name.c_str(), theme.id == active.id)) preferences.theme = theme.id;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, gap);
        if (EasedButton("Customise", ImVec2(button, s.metric.controlHeight))) {
            // Add can reallocate the list `active` points into, so its id is read first.
            if (active.builtin) {
                customisedFrom_ = active.id;
                customisedCopy_ = themes.Add(active, NewThemeName(themes, active.name));
                preferences.theme = customisedCopy_.id;
            }
            themeEditorOpen = true;
            ImGui::CloseCurrentPopup();
        }
        if (custom) {
            ImGui::SameLine(0, gap);
            if (IconButton("##delete-theme", Icon::Clear, "Delete theme", s, dpi)) ImGui::OpenPopup("##confirm-delete-theme");
            if (ImGui::BeginPopup("##confirm-delete-theme")) {
                ImGui::Text("Delete %s?", active.name.c_str());
                bool remove = EasedButton("Delete", ImVec2(96 * dpi, s.metric.controlHeight));
                ImGui::SameLine(0, gap);
                if (EasedButton("Cancel", ImVec2(96 * dpi, s.metric.controlHeight))) ImGui::CloseCurrentPopup();
                if (remove) {
                    ImGui::CloseCurrentPopup();
                    themes.Remove(preferences.theme);
                    preferences.theme = "blue";
                    themeEditorOpen = false;
                    SaveThemes();
                }
                ImGui::EndPopup();
            }
        }
        // Export a theme as a standalone file. Themes are data only (colours and
        // measures, clamped to range on load), so importing one runs nothing.
        const HWND owner = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
        const float half = (ImGui::GetContentRegionAvail().x - gap) / 2;
        if (EasedButton("Import", ImVec2(half, s.metric.controlHeight))) {
            const auto path = PickThemeFile(owner, false, {});
            if (!path.empty()) ImportTheme(path);
        }
        ImGui::SameLine(0, gap);
        if (EasedButton("Export", ImVec2(half, s.metric.controlHeight))) {
            const Theme& chosen = themes.Active(preferences.theme);
            const auto path = PickThemeFile(owner, true, chosen.name);
            if (!path.empty() && !ThemeStore::Export(chosen, path))
                ReportError("Could not export the theme.", "Could not write " + Utf8(path.filename()) + ".");
        }
    }
    // Sync About's open state with the render hook so later scenarios are unaffected.
    if (revealSettingsAbout != aboutRevealed_) ImGui::GetStateStorage()->SetBool(ImGui::GetID("About"), aboutRevealed_ = revealSettingsAbout);
    if (SettingSection("About", s, dpi)) {
        // Author credit, followed inline by the Discord and Roblox marks at text
        // height; the libraries credit below is set in quieter text.
        ImGui::TextUnformatted("QuartzMIDI by BobGrease");
        for (const Icon mark : {Icon::Discord, Icon::Roblox}) {
            // Discord blurple; the Roblox mark is black, or white on a dark background.
            const float side = 16 * dpi;
            ImGui::SameLine(0, s.spacing.s2);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            DrawMark(ImGui::GetWindowDrawList(), mark, ImVec2(min.x, min.y + std::round((ImGui::GetTextLineHeight() - side) / 2)), side,
                     mark == Icon::Discord ? IM_COL32(0x58, 0x65, 0xF2, 255) : s.dark ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255));
            // Each opens the author's profile; Discord's link needs the numeric user id.
            if (ImGui::InvisibleButton(mark == Icon::Discord ? "##about-discord" : "##about-roblox", ImVec2(side, ImGui::GetTextLineHeight())))
                ShellExecuteW(nullptr, L"open", mark == Icon::Discord ? L"https://discord.com/users/340623853816512515"
                                                                      : L"https://www.roblox.com/users/profile?username=BobGrease",
                              nullptr, nullptr, SW_SHOWNORMAL);
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", mark == Icon::Discord ? "Discord" : "Roblox");
            }
        }
        ImGui::Dummy(ImVec2(0, s.spacing.s1));
        {
            FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
            ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
            ImGui::TextWrapped("Based on Zephkek/MIDIPlusPlus (GPLv3)");
            ImGui::TextWrapped("Dear ImGui and RtMidi (MIT)");
            ImGui::TextWrapped("Sheet notation from ArijanJ/midi-converter (MIT)");
            ImGui::TextWrapped("IBM Plex Sans (SIL Open Font License 1.1)");
            ImGui::PopStyleColor();
        }
        if (revealSettingsAbout) ImGui::SetScrollHereY(1.f);
    }
    ImGui::PopStyleVar();
}

void Panels::SettingsControl(const Fonts& fonts, const skin::Skin& design, float dpi,
                             ShellEngine& engine, ImVec2 popupPosition, float popupMaxHeight) {
    const auto s = skin::ScaleGeometry(design, dpi);
    const float popupWidth = PopoverWidth(design) * dpi;
    // Show the button as active while its popover is open.
    if (IconButton("##settings", Icon::Settings, "Settings", s, dpi, ImGui::IsPopupOpen("Settings")))
        ImGui::OpenPopup("Settings");

    // An explicit position isn't clamped by ImGui, so keep the panel on screen
    // here: in mini mode at the bottom edge, its own window would open off the monitor.
    for (const auto& monitor : ImGui::GetPlatformIO().Monitors) {
        const ImVec2 min = monitor.WorkPos, max(monitor.WorkPos.x + monitor.WorkSize.x, monitor.WorkPos.y + monitor.WorkSize.y);
        if (popupPosition.x < min.x - popupWidth || popupPosition.x >= max.x || popupPosition.y < min.y || popupPosition.y >= max.y) continue;
        popupMaxHeight = std::min(popupMaxHeight, max.y - min.y);
        popupPosition.y = std::max(min.y, std::min(popupPosition.y, max.y - popupMaxHeight));
        popupPosition.x = std::clamp(popupPosition.x, min.x, std::max(min.x, max.x - popupWidth));
        break;
    }
    // The device popup takes Settings' position and height. Anchored at the pill
    // it overflowed the window, and a popup that leaves the window becomes its own
    // OS window.
    ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0), ImVec2(popupWidth, popupMaxHeight));
    ImGui::SetNextWindowPos(popupPosition);
    if (ImGui::BeginPopup("MIDI devices")) {
        // Not every port that appears changes the device tree, so opening the
        // popup rescans the lists the first frame scanned.
        if (ImGui::IsWindowAppearing() && scannedLive_) {
            engine.Send({ShellEngine::Action::LiveScan});
            engine.Send({ShellEngine::Action::OutputScan});
        }
        DrawMidiDevices(fonts, design, dpi, engine);
        ImGui::EndPopup();
    }
    // Set right before the popup they apply to: a closed BeginPopup discards them,
    // so with the device popup in between, Settings would open unplaced and full height.
    ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0), ImVec2(popupWidth, popupMaxHeight));
    ImGui::SetNextWindowPos(popupPosition);
    if (ImGui::BeginPopup("Settings")) {
        DrawSettings(fonts, design, dpi, engine);
        ImGui::EndPopup();
    } else {
        hotkeyCapture = -1;
        if (measuring_) {
            input_latency::stop();
            measuring_ = false;
            timingSummary_ = {};
        }
    }
}

// Convert audio popover. The primary action sits at the end of the link field
// and becomes Cancel while its run is going. Choosing a file is a secondary
// icon at the end of the field; sign-in is the row below. Errors use the bad
// colour; the accent is reserved for selection.
void Panels::DrawConvert(HWND hwnd, const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine) {
    const auto state = engine.Snapshot();
    const auto s = skin::ScaleGeometry(design, dpi);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(s.spacing.s2, s.spacing.s2));
    const float width = ImGui::GetContentRegionAvail().x;
    { FontScope title(fonts, design, design.type.heading * SpecFontScale(design), Weight::Semibold);
      ImGui::TextUnformatted("Convert audio to MIDI"); }

    const bool busy = state->converting; // true while the sign-in window is open too
    const bool ready = !state->folder.empty();
    const bool playlistLink = audio_to_midi::IsPlaylistLink(convertLink_);
    // Two rows on one grid (source, then account), each ending in a button of the
    // same width at the same edge. The width fits the widest label either shows,
    // so nothing moves when Convert becomes Cancel.
    float actionWidth = 0;
    for (const char* label : {"Convert", "Cancel", "Close", "Sign in", "Sign in again", "Install", "Use CPU", "Use GPU"})
        actionWidth = std::max(actionWidth, ImGui::CalcTextSize(label).x + 2 * 12 * dpi);
    const float rowStart = ImGui::GetCursorPosX();
    // Progress: an indeterminate bar while running, then the converter's last line,
    // styled as an error when the run failed.
    const auto progress = [&] {
        if (busy && !state->signingIn) {
            convertBarDrawn_ = true;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float height = 4 * dpi;
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), Colour(s.surface.recessed), height / 2);
            const float span = width * .3f;
            const float travel = static_cast<float>(std::fmod(ImGui::GetTime() * .6, 1.0)) * (width + span) - span;
            draw->AddRectFilled(ImVec2(min.x + std::max(0.f, travel), min.y),
                                ImVec2(min.x + std::min(width, travel + span), min.y + height), Colour(s.accent.accent), height / 2);
            ImGui::Dummy(ImVec2(width, height));
        }
        if (!state->conversionStatus.empty()) {
            const auto line = (state->conversionFailed ? "Failed: " : "") + state->conversionStatus;
            ImGui::PushStyleColor(ImGuiCol_Text, Colour(state->conversionFailed ? s.accent.bad : s.ink.primary));
            ImGui::PushTextWrapPos(rowStart + width);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    };
    // Add-on present but not installed: show the install rows on the same grid;
    // the popover becomes the converter once it's installed.
    if (!state->converterInstalled && (state->converterCanSetUp || state->settingUp)) {
        // Two installs, one row each: CPU, and GPU (PyTorch's CUDA build) when an
        // NVIDIA driver is there. While one runs, only its row stays, with Cancel.
        const auto row = [&](const char* label, bool nvidia) {
            ImGui::PushID(label);
            ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::SetCursorPosX(rowStart + width - actionWidth);
            if (EasedButton(busy ? "Cancel" : "Install", ImVec2(actionWidth, s.metric.controlHeight))) {
                if (busy) engine.Send({ShellEngine::Action::ConvertCancel});
                else {
                    installNvidia_ = nvidia;
                    engine.Send({ShellEngine::Action::ConverterSetUp, {}, 0, 0, nvidia});
                }
            }
            ImGui::PopID();
        };
        if (!busy || !installNvidia_) row("CPU, 1.2 GB", false);
        if (busy ? installNvidia_ : state->nvidiaCard) row("GPU, 3.7 GB", true);
        progress();
        ImGui::PopStyleVar(2);
        return;
    }
    ImGui::BeginDisabled(busy || !ready);
    ImGui::SetNextItemWidth(width - actionWidth - s.metric.controlHeight - 2 * s.spacing.s2);
    ImGui::InputTextWithHint("##convert-link", "Paste a YouTube or audio link", convertLink_, sizeof(convertLink_));
    ImGui::SameLine();
    // Audio file picker, at the end of the link field.
    if (IconButton("##convert-file", Icon::Open, "Choose an audio file", s, dpi)) {
        const auto path = PickFile(hwnd, PickKind::Audio);
        if (!path.empty()) engine.Send({ShellEngine::Action::ConvertAudio, path, 0, 0, false, static_cast<double>(preferences.converterCpu)});
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (busy) {
        if (EasedButton(state->signingIn ? "Close" : "Cancel", ImVec2(actionWidth, s.metric.controlHeight)))
            engine.Send({ShellEngine::Action::ConvertCancel});
    } else {
        ImGui::BeginDisabled(!ready || !convertLink_[0]);
        if (EasedButton("Convert", ImVec2(actionWidth, s.metric.controlHeight)))
            engine.Send({ShellEngine::Action::ConvertAudio, {}, 0, 0, playlistLink && convertPlaylist_,
                         static_cast<double>(preferences.converterCpu), convertLink_});
        ImGui::EndDisabled();
    }
    // Only for links that name a playlist. Unchecked, a video opened from a
    // playlist converts alone.
    if (playlistLink) {
        ImGui::BeginDisabled(busy || !ready);
        SettingSwitch("Whole playlist", convertPlaylist_, nullptr, fonts, design, dpi);
        ImGui::EndDisabled();
    }

    // CPU share a conversion may use. Read when a conversion starts; a running one
    // keeps its setting.
    {
        static constexpr int kShares[]{25, 50, 75, 100};
        int chosen = 3;
        for (int i = 0; i < 4; ++i) if (kShares[i] == preferences.converterCpu) chosen = i;
        ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Processor");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const float segmentsWidth = 2 * 4 * dpi + 4 * (ImGui::CalcTextSize("100%").x + 20 * dpi) + 3 * 4 * dpi;
        ImGui::SetCursorPosX(rowStart + width - segmentsWidth);
        if (const int picked = Segments("##converter-cpu", {"25%", "50%", "75%", "100%"}, chosen, s, dpi); picked >= 0)
            preferences.converterCpu = kShares[picked];
    }

    // The build the converter runs on, and a setup that swaps it; a CPU install
    // offers the GPU only when an NVIDIA driver is there.
    if (state->converterCanSwitch && (state->converterGpu || state->nvidiaCard)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(state->converterGpu ? "Runs on the GPU" : "Runs on the CPU");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetCursorPosX(rowStart + width - actionWidth);
        ImGui::BeginDisabled(busy);
        if (EasedButton(state->converterGpu ? "Use CPU" : "Use GPU", ImVec2(actionWidth, s.metric.controlHeight))) {
            installNvidia_ = !state->converterGpu;
            engine.Send({ShellEngine::Action::ConverterSetUp, {}, 0, 0, installNvidia_});
        }
        ImGui::EndDisabled();
    }

    // Account status, in quieter ink than the button beside it.
    ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(state->youtubeSignedIn ? "Signed in to YouTube" : "Not signed in to YouTube");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetCursorPosX(rowStart + width - actionWidth);
    ImGui::BeginDisabled(busy);
    if (EasedButton(state->youtubeSignedIn ? "Sign in again" : "Sign in", ImVec2(actionWidth, s.metric.controlHeight)))
        engine.Send({ShellEngine::Action::YouTubeSignIn});
    ImGui::EndDisabled();

    progress();
    ImGui::PopStyleVar(2);
}

void Panels::DrawStatus(const Fonts& fonts, const skin::Skin& design, float dpi, const EngineSnapshot& state,
                       ImVec2 min, float width, float height) {
    const auto s = skin::ScaleGeometry(design, dpi);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), Colour(s.surface.structure));
    draw->AddLine(min, ImVec2(min.x + width, min.y), Colour(s.border.hairline), dpi);
    draw->PushClipRect(ImVec2(min.x, min.y + dpi), ImVec2(min.x + width, min.y + height), true);
    FontScope font(fonts, design, design.type.meta * SpecFontScale(design));
    const ImVec2 text(min.x + s.spacing.windowPad, min.y + (height - ImGui::GetTextLineHeight()) / 2);
    std::vector<std::string> fields;
    // A failure on the panel side is the newest result while it lasts; Draw clears it.
    if (!panelError_.empty()) fields.push_back(panelError_);
    else if (!state.error.empty()) fields.push_back(state.error);
    else if (state.busy) fields.push_back("Loading...");
    else {
        // The sheet export result comes first while there is one; mini has no Export.
        if (!miniMode && !sheetNote_.empty()) fields.push_back(sheetNote_);
        fields.push_back(state.playing ? "Playing" : state.midiConnect ? "MidiConnect" : state.liveActive ? "Live" : "Ready");
        // A conversion outlives its popover, so the status bar reports it.
        if (state.converting) fields.push_back(state.settingUp ? "Installing the converter"
                                                                : state.signingIn ? "Signing in to YouTube" : "Converting audio");
        fields.push_back("Curve " + state.ActiveVelocityName());
        std::string input = "Input ";
        input += state.liveDevice.empty() ? "None" : BackendName(BackendForDeviceId(state.liveDevice));
        if (!state.liveDevice.empty() && !state.liveActive) input += " (off)";
        fields.push_back(input);
        fields.push_back(state.outputMidi ? "Output MIDI" : "Output Keystrokes");
    }
    // With no song open there are no tracks to count.
    const auto tracks = (state.rows.empty() ? std::string() :
        std::to_string(SilentTracks(state.rows)) + " of " + std::to_string(state.rows.size()) + " tracks silent \xc2\xb7 ") +
        (state.eightyEightKeys ? "88-key" : "61-key");
    // Sized from the label so the caller's frame padding can't clip "Log" in mini mode.
    const float logWidth = ImGui::CalcTextSize("Log").x + 24 * dpi;
    const float suffix = logWidth + (miniMode ? 0 : ImGui::CalcTextSize(tracks.c_str()).x + 24 * dpi);
    const float end = min.x + width - s.spacing.windowPad - suffix;
    float x = text.x;
    std::string summary;
    for (const auto& field : fields) {
        if (!summary.empty()) summary += "\n";
        summary += field;
        if (x >= end) continue;
        // A field with no room for its ellipsis is left out with its separator,
        // rather than a hairline with nothing after it or a cut glyph. It stays
        // in the tooltip.
        const float gap = x > text.x ? 24 * dpi : 0.f;
        const float fieldWidth = ImGui::CalcTextSize(field.c_str()).x;
        if (fieldWidth > end - (x + gap) && end - (x + gap) <= ImGui::CalcTextSize("...").x) { x = end; continue; }
        if (gap > 0)
            draw->AddLine(ImVec2(x + 12 * dpi, text.y + 2 * dpi),
                          ImVec2(x + 12 * dpi, text.y + ImGui::GetTextLineHeight() - 2 * dpi), Colour(s.border.hairline), dpi);
        x += gap;
        DrawEllipsis(field, end - x, ImVec2(x, text.y));
        x += fieldWidth;
    }
    if (!miniMode) draw->AddText(ImVec2(min.x + width - s.spacing.windowPad - logWidth - 8 * dpi - ImGui::CalcTextSize(tracks.c_str()).x, text.y), Colour(s.ink.secondary), tracks.c_str());
    draw->PopClipRect();
    ImGui::SetCursorScreenPos(text);
    ImGui::InvisibleButton("##status", ImVec2(width - 2 * s.spacing.windowPad - logWidth, ImGui::GetTextLineHeight()));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", summary.c_str());
    ImGui::SetCursorScreenPos(ImVec2(min.x + width - s.spacing.windowPad - logWidth, min.y + 2 * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    // Styled as an active IconButton while the log is open, easing like one. Under
    // the log window, as in mini, that styling would only show at its corner.
    const ImRect logButton(min.x + width - s.spacing.windowPad - logWidth, min.y + 2 * dpi,
                           min.x + width - s.spacing.windowPad, min.y + height - 2 * dpi);
    const ImGuiWindow* logWindow = ImGui::FindWindowByName("Log");
    const bool logShown = logOpen && !(logWindow && logWindow->WasActive && logWindow->Rect().Overlaps(logButton));
    const float on = Ease(ImHashStr("on", 0, ImGui::GetID("##log-on")), logShown ? 1.f : 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button, Colour(Mix(StyleArgb(ImGuiCol_Button), s.accent.accentSoft, on)));
    ImGui::PushStyleColor(ImGuiCol_Text, Colour(Mix(StyleArgb(ImGuiCol_Text), s.accent.accent, on)));
    const bool clicked = EasedButton("Log", ImVec2(logWidth, height - 4 * dpi));
    ImGui::PopStyleColor(2);
    if (on > 0)
        draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Faded(Mix(s.accent.accent & 0xFFFFFFu, s.accent.accent, on)),
                      s.radius.control, 0, dpi);
    if (clicked) logOpen = !logOpen;
    ImGui::PopStyleVar();
}

void Panels::DrawLog(HWND hwnd, const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine) {
    if (!logOpen) return;
    const auto s = skin::ScaleGeometry(design, dpi);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    const auto* viewport = ImGui::GetMainViewport();
    const ImVec2 limit(std::max(320 * dpi, viewport->WorkSize.x - 32 * dpi),
                       std::max(160 * dpi, viewport->WorkSize.y - 32 * dpi));
    ImGui::SetNextWindowSizeConstraints(ImVec2(320 * dpi, 160 * dpi), limit);
    ImGui::SetNextWindowSize(ImVec2(std::min(600 * dpi, limit.x), std::min(320 * dpi, limit.y)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x / 2,
                                  viewport->WorkPos.y + viewport->WorkSize.y / 2), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    // Dragged outside the main window, the log becomes an OS window of its own and
    // needs TopMost with always-on-top, or it opens behind the main window. No
    // NoAutoMerge, so it still opens inside the main window.
    ImGuiWindowClass logClass;
    logClass.ViewportFlagsOverrideSet = preferences.alwaysOnTop ? ImGuiViewportFlags_TopMost : 0;
    ImGui::SetNextWindowClass(&logClass);
    // No collapse arrow: ImGui's title-bar triangle isn't from the icon set, and
    // the log has no use for a collapsed state. No docking: dropped on the app, it
    // would dock into the whole shell.
    if (ImGui::Begin("Log", &logOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        const auto state = engine.Snapshot();
        if (IconButton("##clear-log", Icon::Clear, "Clear Log", s, dpi)) engine.Send({ShellEngine::Action::ClearLog});
        ImGui::SameLine();
        if (IconButton("##copy-log", Icon::Copy, "Copy Log", s, dpi)) CopyUtf8ToClipboard(hwnd, *state->log);
        ImGui::BeginChild("##log-output", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4 * dpi;
        if (!state->log->empty()) ImGui::TextUnformatted(state->log->data(), state->log->data() + state->log->size());
        if (atBottom) ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
    }
    ImGui::End();
}

// A keycap joined to its transport button's icon in one outline, with the key
// as the filled end. A key held by another program is drawn as a dead key,
// flat and in the faintest ink.
float Panels::DrawTransportHints(ImDrawList* draw, const skin::Skin& s, float dpi, ImVec2 origin, const EngineSnapshot& state, size_t tapCaps, bool performerOnly) const {
    // No seconds here; the seek buttons carry the number. A double chevron seeks
    // within the song, a single one skips between songs. A performer's keys show
    // while it's the trigger: a key that plays stands in for the play key, and a
    // key that steps shows as a note.
    Icon icons[kHotkeys]{Icon::Play, Icon::Back, Icon::Forward, Icon::Close, Icon::Left, Icon::Right};
    const bool plays = state.TriggerPlays();
    const PerformerTrigger none;
    const auto& chosen = plays || state.TriggerSteps() ? state.performer.triggers[static_cast<size_t>(state.trigger)] : none;
    const size_t groupFirst = chosen.keyFields.empty() ? kHotkeys : chosen.firstKey, groupEnd = groupFirst + chosen.keyFields.size();
    for (size_t i = groupFirst; i < groupEnd; ++i) icons[i] = plays ? Icon::Play : Icon::Piano;
    // A key set is drawn as one group; a single key stands alone.
    const auto grouped = [&](size_t i) { return i >= groupFirst && i < groupEnd && chosen.keyFields.size() > 1; };
    const float line = ImGui::GetTextLineHeight(), capPad = 5 * dpi, capHeight = line + 2 * dpi;
    const float pairGap = s.spacing.s2;
    float x = origin.x;
    // Group caps sit side by side with a single icon after the last. If they don't
    // all fit, show the first tapCaps and a "+N" count.
    size_t firstTap = kHotkeys, lastTap = kHotkeys, taps = 0;
    for (size_t i = groupFirst; i < groupEnd; ++i)
        if (grouped(i) && !transportKeys[i].empty() && ++taps <= tapCaps) { if (firstTap == kHotkeys) firstTap = i; lastTap = i; }
    const std::string more = taps > tapCaps ? " +" + std::to_string(taps - tapCaps) : std::string();
    for (size_t i = 0; i < transportKeys.size(); ++i) {
        if (transportKeys[i].empty() || (grouped(i) && (i < firstTap || i > lastTap))) continue;
        const std::string cap = transportKeys[i] + (i == lastTap ? more : std::string());
        if (i >= kAppHotkeys && (i < groupFirst || i >= groupEnd)) continue;
        if (performerOnly && i < kAppHotkeys) continue;
        // A performer key that plays replaces the play key.
        if (i == 0 && plays && groupFirst < kHotkeys && !transportKeys[groupFirst].empty()) continue;
        const float capWidth = ImGui::CalcTextSize(cap.c_str()).x + 2 * capPad;
        const bool available = transportKeysAvailable[i];
        const ImU32 ink = Colour(available ? s.ink.secondary : s.ink.tertiary);
        const bool joined = grouped(i) && i != lastTap; // another cap of the group follows
        const float labelWidth = joined ? 0 : capPad + line + capPad;
        if (draw) {
            const ImVec2 min(x, origin.y - dpi), capMax(x + capWidth, origin.y - dpi + capHeight), max(capMax.x + labelWidth, capMax.y);
            const bool opens = !grouped(i) || i == firstTap;
            const ImDrawFlags corners = (opens ? ImDrawFlags_RoundCornersLeft : 0) | (joined ? 0 : ImDrawFlags_RoundCornersRight);
            if (available) draw->AddRectFilled(min, capMax, Colour(s.surface.elevated), 4 * dpi,
                                               opens ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersNone);
            draw->AddRect(min, max, Colour(s.border.hairline), 4 * dpi, corners ? corners : ImDrawFlags_RoundCornersNone, dpi);
            // In the light skins the key fill is close to the card colour, so the divider
            // line separates key from action.
            if (!joined) draw->AddLine(ImVec2(capMax.x, min.y), ImVec2(capMax.x, max.y), Colour(s.border.hairline), dpi);
            draw->AddText(ImVec2(x + capPad, origin.y), Colour(available ? s.ink.primary : s.ink.tertiary), cap.c_str());
            // Lucide's play glyph fills 18 of 24 units against a chevron's 12, so shrink it
            // to match the chevrons visually.
            const float side = icons[i] == Icon::Play ? line * .78f : line;
            if (!joined) DrawIcon(draw, icons[i], ImVec2(capMax.x + capPad + (line - side) / 2, origin.y + (line - side) / 2), side, ink, dpi);
        }
        // A joined cap shares its right edge with the next cap's left.
        x += joined ? capWidth - dpi : capWidth + labelWidth + pairGap;
    }
    return std::max(0.f, x - origin.x - pairGap);
}

void Panels::DrawMini(HWND hwnd, const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine,
                     ImVec2 origin, ImVec2 size) {
    const auto state = engine.Snapshot();
    const auto s = skin::ScaleGeometry(design, dpi);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    const float control = s.metric.controlHeight, pad = s.spacing.windowPad;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (control - ImGui::GetTextLineHeight()) / 2));
    // Two rows (device pill, then state pills) with equal 8 dp gaps above, between
    // and below, as elsewhere in mini.
    const float gap = 8 * dpi, stripPad = gap;
    const float strip = 3 * stripPad + 2 * control, status = 28 * dpi;
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + strip), Colour(s.surface.structure));
    draw->AddLine(ImVec2(origin.x, origin.y + strip), ImVec2(origin.x + size.x, origin.y + strip), Colour(s.border.hairline));
    // Three utility slots.
    const float utilityX = origin.x + size.x - pad - 3 * control - 2 * s.spacing.s2;
    const float segmentX = utilityX - 172 * dpi;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, origin.y + stripPad));
    { FontScope deviceFont(fonts, design, design.type.body * SpecFontScale(design), Weight::Medium);
      if (DevicePill(DeviceName(*state), std::max(40 * dpi, segmentX - s.spacing.s3 - origin.x - pad), s, dpi)) ImGui::OpenPopup("MIDI devices"); }
    ImGui::SetCursorScreenPos(ImVec2(segmentX, origin.y + stripPad));
    {
        FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
        const auto well = ImGui::GetCursorScreenPos();
        const float segmentWidth = 164 * dpi;
        skin::RecessedRect(draw, well, ImVec2(well.x + segmentWidth, well.y + control), s.radius.control, s);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.radius.element);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        // One raised thumb slides between the modes under transparent buttons, and
        // is outlined after them so their hairlines don't cover it.
        const float at = Ease(ImGui::GetID("##mini-mode-thumb"), miniAutoplay ? 1.f : 0.f);
        const ImVec2 thumbMin(well.x + 4 * dpi + at * 80 * dpi, well.y + 4 * dpi);
        const ImVec2 thumbMax(thumbMin.x + 76 * dpi, thumbMin.y + (control - 8 * dpi));
        draw->AddRectFilled(thumbMin, thumbMax, Colour(s.surface.elevated), s.radius.element);
        const ImU32 hairline = ImGui::GetColorU32(ImGuiCol_Border);
        for (int mode = 0; mode < 2; ++mode) {
            ImGui::SetCursorScreenPos(ImVec2(well.x + 4 * dpi + mode * 80 * dpi, well.y + 4 * dpi));
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            if (EasedButton(mode ? "Autoplay" : "Live", ImVec2(76 * dpi, control - 8 * dpi))) miniAutoplay = mode == 1;
            ImGui::PopStyleColor(2);
            SegmentHairline(draw, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), thumbMin.x, thumbMax.x, hairline, s.radius.element, dpi);
        }
        draw->AddRect(thumbMin, thumbMax, Colour(s.accent.accent), s.radius.element, 0, dpi);
        ImGui::PopStyleVar(2);
        ImGui::SetCursorScreenPos(ImVec2(well.x + segmentWidth + 8 * dpi, well.y));
    }
    ImGui::SetCursorScreenPos(ImVec2(utilityX, origin.y + stripPad));
    if (IconButton("##restore-full", Icon::Expand, "Full window", s, dpi)) miniMode = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(!themes.Active(preferences.theme).paired);
    if (IconButton("##mini-theme", s.dark ? Icon::Moon : Icon::Sun, s.dark ? "Switch to light" : "Switch to dark", s, dpi)) preferences.dark = !preferences.dark;
    ImGui::EndDisabled();
    ImGui::SameLine();
    SettingsControl(fonts, design, dpi, engine,
                    ImVec2(origin.x + size.x - PopoverWidth(design) * dpi - s.spacing.windowPad, origin.y + stripPad + control + 4 * dpi), 544 * dpi);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, origin.y + 2 * stripPad + control));
    if (StatePills(fonts, design, dpi, engine, size.x - 2 * pad)) autoVolumeOpen = true;
    const float row = origin.y + strip + gap;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, row));
    const auto number = [&](ShellEngine::Action action, double value) { engine.Send({action, {}, state->generation, 0, false, value}); };
    if (!miniAutoplay) {
        // Transpose as on the Playback card: meta label, 132 groove and the 48
        // readout button. The curve takes the rest.
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        float labels = 0;
        { FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
          labels = ImGui::CalcTextSize("Curve").x + ImGui::CalcTextSize("Transpose").x; }
        const auto label = [&](const char* text) {
            FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
            const auto pos = ImGui::GetCursorScreenPos();
            draw->AddText(ImVec2(pos.x, pos.y + (control - ImGui::GetTextLineHeight()) / 2), Colour(s.ink.secondary), text);
            ImGui::Dummy(ImVec2(ImGui::CalcTextSize(text).x, control)); ImGui::SameLine();
        };
        label("Curve");
        CurveCombo("##mini-curve", size.x - 2 * pad - labels - (132 + 48) * dpi - 4 * spacing, *state, engine);
        ImGui::SameLine();
        label("Transpose");
        float transpose = static_cast<float>(state->transpose);
        if (Groove("##mini-transpose", &transpose, -12, 12, 132 * dpi, control, s, dpi, true))
            number(ShellEngine::Action::Transpose, std::round(transpose));
        ImGui::SameLine();
        char transposeText[16]; snprintf(transposeText, sizeof(transposeText), "%+d", static_cast<int>(std::round(transpose)));
        const auto transposeMin = ImGui::GetCursorScreenPos();
        double clicked = std::round(transpose);
        if (ReadoutClick("##mini-transpose-reset", ImVec2(48 * dpi, control), 1, 0, &clicked))
            number(ShellEngine::Action::Transpose, clicked);
        draw->AddText(ImVec2(transposeMin.x + std::floor((48 * dpi - ImGui::CalcTextSize(transposeText).x) / 2),
                             transposeMin.y + (control - ImGui::GetTextLineHeight()) / 2), Colour(s.ink.primary), transposeText);
    } else {
        ImGui::SetNextItemWidth(size.x - 2 * pad - 2 * control - 16 * dpi);
        const auto fileName = state->loaded.empty() ? "Choose MIDI file" : Utf8(state->loaded.filename());
        const bool fileOpen = ChevronCombo("##mini-file", fileName.c_str());
        if (fileOpen) {
            // Only the visible rows, as in the full window's list, so large libraries
            // don't cost a row per file every frame.
            // It opens at the loaded song: on the first frame the clipper also submits
            // that row, whose default focus scrolls the list to it.
            int loadedIndex = -1;
            if (ImGui::IsWindowAppearing())
                for (size_t i = 0; i < state->files->size(); ++i)
                    if ((*state->files)[i].path == state->loaded) { loadedIndex = static_cast<int>(i); break; }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(state->files->size()));
            if (loadedIndex >= 0) clipper.IncludeItemByIndex(loadedIndex);
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& file = (*state->files)[static_cast<size_t>(i)];
                    ImGui::PushID(i);
                    if (ImGui::Selectable(file.name.c_str(), file.path == state->loaded))
                        engine.Send({ShellEngine::Action::Load, file.path, 0, 0, preferences.autoSolo});
                    if (i == loadedIndex) ImGui::SetItemDefaultFocus();
                    ImGui::PopID();
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        // The two ways to open files, as behind the full window's plus button.
        if (IconButton("##mini-open", Icon::Plus, "Add MIDI files", s, dpi)) ImGui::OpenPopup("Add MIDI files");
        MenuUnderLastItem("Add MIDI files", s.spacing.s1, true);
        if (ImGui::BeginPopup("Add MIDI files")) {
            if (ImGui::MenuItem("Open MIDI file...")) {
                const auto path = PickMidiFile(hwnd);
                if (!path.empty()) engine.Send({ShellEngine::Action::Load, path, 0, 0, preferences.autoSolo});
            }
            if (ImGui::MenuItem("Choose MIDI folder...", nullptr, false, !state->playing && !state->busy)) {
                const auto path = PickFolder(hwnd);
                if (!path.empty()) { preferences.folder = path; browse_.clear(); engine.Send({ShellEngine::Action::Scan, path}); }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(); ImGui::BeginDisabled(state->rows.empty() || state->busy || AllPiano(state->rows));
        { const bool applied = SoloPianoApplied(state->rows);
          if (IconButton("##mini-solo-piano", Icon::Piano, applied ? "Unmute all" : "Solo Piano", s, dpi, applied))
              engine.Send({applied ? ShellEngine::Action::UnmuteAll : ShellEngine::Action::SoloPiano, {}, state->generation}); }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(state->loaded.empty() || state->rows.empty() || state->busy);
        const float seekHeight = 22 * dpi, transportY = row + control + 2 * gap + seekHeight;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, row + control + gap));
        if (!seeking_ || seekGeneration_ != state->generation) { seekPosition_ = static_cast<float>(state->position); seeking_ = false; }
        const bool changed = Groove("##mini-seek", &seekPosition_, 0, static_cast<float>(std::max(.001, state->duration)),
            size.x - 2 * pad, seekHeight, s, dpi, false);
        if (ImGui::IsItemActivated()) { seeking_ = true; seekGeneration_ = state->generation; }
        if (seeking_ && ImGui::IsItemDeactivatedAfterEdit()) { number(ShellEngine::Action::Seek, seekPosition_); seeking_ = false; }
        else if (changed && !ImGui::IsItemActive()) number(ShellEngine::Action::Seek, seekPosition_);
        // As in the full window: the dragged time, else the time a click under the
        // pointer seeks to, mapped as the slider maps it (2 px in from each end).
        if (seeking_) ImGui::SetTooltip("%s", Time(seekPosition_).c_str());
        else if (ImGui::IsItemHovered()) {
            const auto barMin = ImGui::GetItemRectMin(), barMax = ImGui::GetItemRectMax();
            const float at = std::clamp((ImGui::GetIO().MousePos.x - barMin.x - 2) / std::max(1.f, barMax.x - barMin.x - 4), 0.f, 1.f);
            ImGui::SetTooltip("%s", Time(at * state->duration).c_str());
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, transportY));
        if (PlayButton("##mini-play", state->playing, state->playbackCountdown, s, dpi))
            engine.Send({ShellEngine::Action::PlayCountdown, {}, state->generation});
        // No legend in mini: each button's hotkey is in its tooltip. A performer key
        // that plays belongs to Play.
        const PerformerTrigger noTrigger;
        const auto& chosenTrigger = state->performer.triggers.empty() ? noTrigger : state->performer.triggers[static_cast<size_t>(state->trigger)];
        const auto keyOf = [&](size_t hotkey) -> const std::string& {
            const bool plays = hotkey == 0 && state->TriggerPlays() && !chosenTrigger.keyFields.empty() && !transportKeys[chosenTrigger.firstKey].empty();
            return transportKeys[plays ? chosenTrigger.firstKey : hotkey];
        };
        const auto tipped = [&](const char* tip, size_t hotkey) { return keyOf(hotkey).empty() ? std::string(tip) : std::string(tip) + " (" + keyOf(hotkey) + ")"; };
        const auto under = [&](size_t hotkey) {
            if (!keyOf(hotkey).empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", keyOf(hotkey).c_str());
        };
        under(0);
        ImGui::SameLine();
        if (IconButton("##mini-restart", Icon::Refresh, "Restart", s, dpi)) engine.Send({ShellEngine::Action::Restart, {}, state->generation});
        ImGui::SameLine();
        // Labelled, as in the full window, so the seek buttons look the same in both layouts.
        if (TransportButton("##mini-back10", SeekLabel(state->seekStep, false).c_str(), s, dpi)) engine.Send({ShellEngine::Action::Back10, {}, state->generation});
        under(1);
        ImGui::SameLine();
        if (TransportButton("##mini-forward10", SeekLabel(state->seekStep, true).c_str(), s, dpi)) engine.Send({ShellEngine::Action::Forward10, {}, state->generation});
        under(2);
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(state->files->empty() || state->busy);
        if (IconButton("##mini-prev", Icon::Left, tipped("Previous MIDI file", 4).c_str(), s, dpi)) engine.Send({ShellEngine::Action::Previous, {}, state->generation});
        ImGui::SameLine();
        if (IconButton("##mini-next", Icon::Right, tipped("Next MIDI file", 5).c_str(), s, dpi)) engine.Send({ShellEngine::Action::Next, {}, state->generation});
        ImGui::EndDisabled(); ImGui::SameLine();
        if (IconButton("##mini-stop", Icon::Close, tipped("Stop all output and cancel countdown", 3).c_str(), s, dpi)) engine.Send({ShellEngine::Action::Stop});
        const auto time = state->playbackCountdown ? "Starts in " + std::to_string(state->playbackCountdown) + "s" :
            Time(seeking_ ? seekPosition_ : state->position) + " / " + Time(state->duration);
        draw->AddText(ImVec2(origin.x + size.x - pad - ImGui::CalcTextSize(time.c_str()).x,
            transportY + (control - ImGui::GetTextLineHeight()) / 2), Colour(s.ink.secondary), time.c_str());
        // Performer row, as on the Playback card, without labels to fit the state
        // pills' width.
        if (PerformerRow(*state)) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, transportY + control + gap));
            ImGui::BeginDisabled(state->loaded.empty() || state->busy);
            PerformerPanelRow("##mini-performer", *state, engine, s, dpi, [](const char*) {});
            // Stepping keys, shown where their trigger is chosen.
            if (state->TriggerSteps() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                std::string keys;
                for (size_t i = chosenTrigger.firstKey; i < chosenTrigger.firstKey + chosenTrigger.keyFields.size(); ++i)
                    if (!transportKeys[i].empty()) keys += (keys.empty() ? "" : "  ") + transportKeys[i];
                if (!keys.empty()) ImGui::SetTooltip("%s", keys.c_str());
            }
            ImGui::EndDisabled();
        }
    }
    DrawStatus(fonts, design, dpi, *state, ImVec2(origin.x, origin.y + size.y - status), size.x, status);
    ImGui::PopStyleVar();
}

// Help: search over every question, a chip filtering to this build's additions,
// and otherwise the questions in folders that expand like Settings' sections.
void Panels::DrawHelp(const Fonts& fonts, const skin::Skin& design, float dpi, const EngineSnapshot& state) {
    const auto s = skin::ScaleGeometry(design, dpi);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    const auto& entries = HelpEntries();
    const std::string chip = "What's new";
    const bool haveChip = HelpAdded(entries, kHelpBuild, state.addonsFolder);
    if (!haveChip) helpAdded = false;
    const float chipWidth = haveChip ? 2 * 12 * dpi + ImGui::CalcTextSize(chip.c_str()).x + s.spacing.s2 : 0.f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - chipWidth);
    ImGui::InputTextWithHint("##help-search", "Search Help", helpSearch_, sizeof(helpSearch_));
    if (haveChip) {
        ImGui::SameLine(0, s.spacing.s2);
        if (TransportBody("##help-added", nullptr, chip.c_str(), s, dpi, false, helpAdded)) helpAdded = !helpAdded;
    }
    const auto found = FilterHelp(entries, helpSearch_, helpAdded, kHelpBuild, state.addonsFolder);
    // When searched or filtered, matches are listed under their folder names,
    // without expanders.
    const bool flat = helpSearch_[0] != 0 || helpAdded;
    const auto tourButton = [&] {
        ImGui::BeginDisabled(state.playing || state.playbackCountdown);
        if (TransportButton("##help-tour", Icon::Play, "Tour", s, dpi)) { ImGui::CloseCurrentPopup(); StartTour(); }
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, s.spacing.s1));
    };
    const bool tourFound = !helpAdded && HelpFindsTour(helpSearch_);
    if (tourFound) tourButton();
    else if (found.empty()) ImGui::TextDisabled("No matching questions");
    if (revealHelpFolder != helpFolderRevealed_) {
        for (int i = 0; i < static_cast<int>(std::size(kHelpFolders)); ++i)
            ImGui::GetStateStorage()->SetBool(ImGui::GetID(kHelpFolders[i]), i == revealHelpFolder);
        helpFolderRevealed_ = revealHelpFolder;
    }
    const float inset = 20 * dpi + s.spacing.s2;
    for (size_t folder = 0; folder < std::size(kHelpFolders); ++folder) {
        const std::string_view name = kHelpFolders[folder];
        std::vector<size_t> rows;
        bool added = false;
        for (const size_t i : found) if (name == entries[i].folder) {
            rows.push_back(i);
            added = added || std::string_view(kHelpBuild) == entries[i].build;
        }
        if (rows.empty()) continue;
        bool open = true;
        if (flat) {
            FontScope meta(fonts, design, design.type.meta * SpecFontScale(design), Weight::Semibold);
            ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
            ImGui::TextUnformatted(kHelpFolders[folder]); ImGui::PopStyleColor();
        } else {
            open = SettingSection(kHelpFolders[folder], s, dpi);
            // Marks a folder that contains something new in this build.
            if (added && haveChip) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(max.x - 8 * dpi, (min.y + max.y) / 2), 3 * dpi, Colour(s.accent.accent));
            }
        }
        if (!open) continue;
        if (!flat) ImGui::Indent(inset);
        // Replay-tour button in the first folder.
        if (!flat && folder == 0) tourButton();
        for (const size_t i : rows) {
            { FontScope question(fonts, design, design.type.body * SpecFontScale(design), Weight::Medium);
              ImGui::TextWrapped("%s", entries[i].question); }
            ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
            ImGui::TextWrapped("%s", entries[i].answer);
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, s.spacing.s1));
        }
        if (!flat) ImGui::Unindent(inset);
    }
    ImGui::PopStyleVar();
}

void Panels::StartTour(int stop) {
    tourStop = std::clamp(stop, 0, static_cast<int>(TourStop::Count) - 1);
    tourDrawn_ = -1;
    tourGlide_ = 1;
    tourClosing_ = false;
}

void Panels::EndTour() {
    tourStop = tourDrawn_ = -1;
    tourGlide_ = 1;
    tourClosing_ = false;
    preferences.tourSeen = true;
    // The tour covers the whole app, so it also counts as seeing this build's additions.
    preferences.helpBuild = kHelpBuild;
    if (tourDim_ > 0) ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg].w = tourDim_;
    tourDim_ = 0;
}

// Tour: the window dimmed except for a cutout around the current control, a
// card beside it, and both animating between stops. The card is a modal so
// nothing under the dim is clickable. ImGui's modal dim can't have a hole, so
// the dim is drawn here in the card's draw list, beneath the card.
void Panels::DrawTour(const Fonts& fonts, const skin::Skin& design, float dpi, ImVec2 origin, ImVec2 size) {
    if (tourStop < 0) return;
    const auto s = skin::ScaleGeometry(design, dpi);
    const TourRect window{origin.x, origin.y, origin.x + size.x, origin.y + size.y};
    const float grow = 6 * dpi;
    const TourRect& drawn = tourRects_[static_cast<size_t>(tourStop)];
    const TourRect target{std::max(window.x0, drawn.x0 - grow), std::max(window.y0, drawn.y0 - grow),
                          std::min(window.x1, drawn.x1 + grow), std::min(window.y1, drawn.y1 + grow)};
    const float width = 300 * dpi;
    const float pad = s.spacing.panelPad;
    // The card's height for a stop, measured up front: an auto-sized window
    // lags a frame behind new content.
    const auto cardHeight = [&](int index) {
        FontScope body(fonts, design, design.type.body * SpecFontScale(design));
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        return 2 * pad + ImGui::GetTextLineHeight() + spacing +
               ImGui::CalcTextSize(kTour[index].text, nullptr, false, width - 2 * pad).y + spacing +
               s.spacing.s1 + spacing + s.metric.controlHeight;
    };
    const float height = cardHeight(tourStop);
    const TourRect cardTarget = PlaceTourCard(target, width, height, window, s.spacing.s3);
    if (tourDrawn_ != tourStop) {
        const bool first = tourDrawn_ < 0;
        tourFrom_ = first ? target : tourShown_;
        tourCardFrom_ = first ? cardTarget : tourCardShown_;
        tourHeightFrom_ = first ? height : tourHeightShown_;
        if (first) { tourTextStop_ = tourStop; tourText_ = 1; }
        tourGlide_ = first ? 1.f : 0.f;
        if (first) tourFade_ = 0;
        tourDrawn_ = tourStop;
    }
    // The shell caps the first frame after an idle wait, so this still animates.
    const float step = ImGui::GetIO().DeltaTime;
    tourGlide_ = std::min(1.f, tourGlide_ + step / .28f);
    // In over 200 ms when the tour starts, out over 160 ms when it ends.
    tourFade_ = tourClosing_ ? std::max(0.f, tourFade_ - step / .16f) : std::min(1.f, tourFade_ + step / .2f);
    // The highlight and the card glide together, each between its own start and
    // end, so the card never re-picks its side mid-glide.
    tourShown_ = TourBetween(tourFrom_, target, tourGlide_);
    tourCardShown_ = TourBetween(tourCardFrom_, cardTarget, tourGlide_);
    tourHeightShown_ = tourHeightFrom_ + (height - tourHeightFrom_) * TourEase(tourGlide_);
    const float fade = TourEase(tourFade_);
    // The text crossfades in the glide's time: the text on screen out over its
    // first half, this stop's in over the second.
    TourTextStep(tourStop, step / .14f, tourTextStop_, tourText_);
    const bool crossing = tourTextStop_ != tourStop || tourText_ < 1;
    const int shownStop = tourTextStop_;
    const float textAlpha = TourEase(tourText_);

    // Save the style's own dim alpha (the one read at render time) and zero it;
    // EndTour restores it.
    if (auto& dim = ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg]; dim.w > 0) { tourDim_ = dim.w; dim.w = 0; }
    if (!ImGui::IsPopupOpen("##tour")) ImGui::OpenPopup("##tour");
    ImGui::SetNextWindowPos(ImVec2(tourCardShown_.x0, tourCardShown_.y0));
    ImGui::SetNextWindowSize(ImVec2(width, tourHeightShown_));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    // No keyboard navigation: the card reads its own keys, and nav would also
    // press whichever button it had focused.
    const bool open = ImGui::BeginPopupModal("##tour", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    if (!open) return;
    // Keys are ignored on the first frame, where the Enter that pressed Help's
    // Tour button still reads as pressed, and while the tour fades out.
    const bool keys = !ImGui::IsWindowAppearing() && !tourClosing_;
    auto* draw = ImGui::GetWindowDrawList();
    const int firstVertex = draw->VtxBuffer.Size;
    const ImU32 dim = IM_COL32(0, 0, 0, 150);
    const ImVec2 holeMin(std::floor(tourShown_.x0), std::floor(tourShown_.y0)), holeMax(std::ceil(tourShown_.x1), std::ceil(tourShown_.y1));
    draw->PushClipRect(ImVec2(window.x0, window.y0), ImVec2(window.x1, window.y1), false);
    draw->AddRectFilled(ImVec2(window.x0, window.y0), ImVec2(window.x1, holeMin.y), dim);
    draw->AddRectFilled(ImVec2(window.x0, holeMax.y), ImVec2(window.x1, window.y1), dim);
    draw->AddRectFilled(ImVec2(window.x0, holeMin.y), ImVec2(holeMin.x, holeMax.y), dim);
    draw->AddRectFilled(ImVec2(holeMax.x, holeMin.y), ImVec2(window.x1, holeMax.y), dim);
    // Fill the cutout's corners back to the card radius, then draw its ring.
    skin::RoundCorners(draw, holeMin, holeMax, s.radius.card, dim, s, false);
    draw->AddRect(holeMin, holeMax, Colour(s.accent.accent), s.radius.card, 0, 2 * dpi);
    const ImVec2 cardMin = ImGui::GetWindowPos(), cardSize = ImGui::GetWindowSize();
    skin::RaisedRect(draw, cardMin, ImVec2(cardMin.x + cardSize.x, cardMin.y + cardSize.y), s.radius.card, s, Colour(s.surface.elevated));
    draw->PopClipRect();
    // Fade the dim, ring and card in when the tour starts and out when it ends.
    if (fade < 1) FadeVertices(draw, firstVertex, fade);

    bool skip = false, back = false, next = false, last = false;
    { // The font must be popped before the popup ends.
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    const int stops = static_cast<int>(TourStop::Count);
    const auto& stop = kTour[shownStop];
    const float buttonsY = tourHeightShown_ - pad - s.metric.controlHeight;
    // The outgoing text may be taller than the card as it shrinks; keep it off the buttons.
    ImGui::PushClipRect(cardMin, ImVec2(cardMin.x + cardSize.x, cardMin.y + buttonsY - ImGui::GetStyle().ItemSpacing.y), true);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade * textAlpha);
    { FontScope title(fonts, design, design.type.body * SpecFontScale(design), Weight::Semibold);
      ImGui::TextUnformatted(stop.title); }
    { FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
      const std::string count = std::to_string(shownStop + 1) + " of " + std::to_string(stops);
      ImGui::SameLine(ImGui::GetWindowWidth() - pad - ImGui::CalcTextSize(count.c_str()).x);
      ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.tertiary));
      ImGui::TextUnformatted(count.c_str()); ImGui::PopStyleColor(); }
    ImGui::TextWrapped("%s", stop.text);
    if (!crossing) tourTextRoom = buttonsY - ImGui::GetCursorPosY();
    ImGui::PopStyleVar();
    ImGui::PopClipRect();
    // Pinned to the card's bottom edge so they move with its height, not with the text.
    ImGui::SetCursorPosY(buttonsY);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    last = tourStop + 1 == stops;
    skip = TransportBody("##tour-skip", nullptr, "Skip", s, dpi, false) || (keys && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    // Next keeps its position on the last stop, where it reads Done.
    const float nextWidth = 2 * 16 * dpi + std::max(ImGui::CalcTextSize("Next").x, ImGui::CalcTextSize("Done").x);
    const float backWidth = 2 * 12 * dpi + ImGui::CalcTextSize("Back").x;
    ImGui::SameLine(ImGui::GetWindowWidth() - pad - nextWidth - s.spacing.s2 - backWidth);
    ImGui::BeginDisabled(tourStop == 0);
    back = TransportBody("##tour-back", nullptr, "Back", s, dpi, false) || (keys && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false));
    ImGui::EndDisabled();
    ImGui::SameLine(0, s.spacing.s2);
    next = TransportBody("##tour-next", nullptr, last ? "Done" : "Next", s, dpi, true, false, {"Next", "Done"}) ||
        (keys && (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)));
    ImGui::PopStyleVar(2);
    }
    // While the tour fades out, the card stays as it was and its buttons do nothing.
    if (!tourClosing_) {
        if (back && tourStop > 0) --tourStop;
        else if (next && !last) ++tourStop;
        tourClosing_ = skip || (next && last);
    }
    if (tourClosing_ && tourFade_ <= 0) { EndTour(); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void Panels::Draw(HWND hwnd, const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine) {
    // Set again by any transition still moving this frame.
    g_motion = false;
    convertBarDrawn_ = false;
    const auto s = skin::ScaleGeometry(design, dpi);
    auto state = engine.Snapshot();
    SyncLayout(*state);
    fileSort_ = state->fileSort;
    descendingFiles_ = state->descendingFiles;
    if (state->sheetReady && state->sheetRevision != handledSheetRevision_) {
        handledSheetRevision_ = state->sheetRevision;
        sheetStatusGeneration_ = state->generation;
        sheetPending_ = false;
        if (!state->sheetSaved.empty()) {
            // The engine provides the URL; the panel opens it, so a test engine never
            // launches a browser.
            const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", state->sheetSaved.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            sheetStatus_ = opened > 32 ? "Opened the sheet editor in your browser."
                                       : "Could not open a browser. The page is at " + Utf8(state->sheetSaved) + ".";
        }
        else if (!state->sheetFilesSaved.empty()) sheetStatus_ = "Saved the sheet in " + Utf8(state->sheetFilesSaved) + ".";
        else if (state->sheetText->empty() || state->sheetNotes == 0) sheetStatus_ = "No mapped notes to copy.";
        else if (CopyUtf8ToClipboard(hwnd, *state->sheetText))
            sheetStatus_ = "Copied " + std::to_string(state->sheetNotes) + " notes.";
        else sheetStatus_ = "Clipboard is busy. Try again.";
    }
    if (sheetStatusGeneration_ != state->generation) { sheetStatus_.clear(); sheetPending_ = false; }
    // A failed save (e.g. a read-only folder) raises an engine error instead of a
    // sheet, shown in the status bar, so clear the "Writing..." line.
    else if (sheetPending_ && !state->error.empty()) { sheetStatus_.clear(); sheetPending_ = false; }
    else if (!state->sheetReady && !sheetPending_) sheetStatus_.clear();
    // A panel failure lasts until the next click or a newer engine error. Cleared
    // before anything below can report one this frame.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || (!state->error.empty() && state->error != errorSeen_)) panelError_.clear();
    errorSeen_ = state->error;
    if (!scannedLive_ && hwnd) { engine.Send({ShellEngine::Action::LiveScan}); scannedLive_ = true; }
    if (!scannedOutput_ && hwnd) { engine.Send({ShellEngine::Action::OutputScan}); scannedOutput_ = true; }
    if (measuring_ && ImGui::GetTime() >= nextTimingPoll_) {
        input_latency::poll(timing_);
        timingSummary_ = timing_.summarize(timingSource_ ? input_latency::Source::Autoplay : input_latency::Source::LiveKeys,
                                          input_latency::frequency());
        nextTimingPoll_ = ImGui::GetTime() + .2;
    }
    const auto send = [&](ShellEngine::Action action, size_t track = 0, bool value = false) {
        engine.Send({action, {}, state->generation, track, value});
    };
    const auto load = [&](const std::filesystem::path& path) {
        if (!path.empty()) engine.Send({ShellEngine::Action::Load, path, 0, 0, preferences.autoSolo});
    };

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (miniMode) {
        DrawMini(hwnd, fonts, design, dpi, engine, origin, size);
        DrawAutoVolume(fonts, design, dpi, engine);
        DrawThemeEditor(fonts, design, dpi);
        DrawLog(hwnd, fonts, design, dpi, engine);
        mappingArmed_ = false;
        return;
    }
    auto* dl = ImGui::GetWindowDrawList();
    // One row: the state pills, then the device pill in the space before the
    // utility buttons.
    const float stripPad = 12 * dpi;
    const float strip = 2 * stripPad + s.metric.controlHeight, status = 28 * dpi;
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + strip), Colour(s.surface.structure));
    dl->AddLine(ImVec2(origin.x, origin.y + strip), ImVec2(origin.x + size.x, origin.y + strip), Colour(s.border.hairline));
    const float utilityX = origin.x + size.x - s.spacing.windowPad - 5 * s.metric.controlHeight - 4 * s.spacing.s2;
    const auto tourRect = [&](TourStop stop, ImVec2 min, ImVec2 max) {
        tourRects_[static_cast<size_t>(stop)] = {min.x, min.y, max.x, max.y};
    };
    ImGui::SetCursorScreenPos(ImVec2(origin.x + s.spacing.windowPad, origin.y + stripPad));
    if (StatePills(fonts, design, dpi, engine, 0)) autoVolumeOpen = true;
    tourRect(TourStop::Pills, ImVec2(origin.x + s.spacing.windowPad, origin.y + stripPad), ImGui::GetItemRectMax());
    ImGui::SameLine(0, s.spacing.s3);
    // Right-aligned against the utility buttons, next to the Settings button it
    // opens, so its width doesn't move anything else.
    { FontScope font(fonts, design, design.type.body * SpecFontScale(design), Weight::Medium);
      const auto name = DeviceName(*state);
      const float room = std::max(40 * dpi, utilityX - s.spacing.s3 - ImGui::GetCursorScreenPos().x);
      const float width = std::min(room, ImGui::CalcTextSize(name.c_str()).x + 26 * dpi);
      ImGui::SetCursorScreenPos(ImVec2(utilityX - s.spacing.s3 - width, origin.y + stripPad));
      if (DevicePill(name, room, s, dpi)) ImGui::OpenPopup("MIDI devices");
      tourRect(TourStop::Device, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); }
    ImGui::SetCursorScreenPos(ImVec2(utilityX, origin.y + stripPad));
    if (IconButton("##mini-mode", Icon::Mini, "Mini mode", s, dpi)) miniMode = true;
    ImGui::SameLine();
    if (IconButton("##key-mapping", Icon::Keyboard, "Key Mapping", s, dpi, preferences.keyMappingOpen))
        preferences.keyMappingOpen = !preferences.keyMappingOpen;
    ImGui::SameLine();
    // A theme with a single palette has no other variant to switch to.
    ImGui::BeginDisabled(!themes.Active(preferences.theme).paired);
    if (IconButton("##theme", s.dark ? Icon::Moon : Icon::Sun, s.dark ? "Switch to light" : "Switch to dark", s, dpi))
        preferences.dark = !preferences.dark;
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Popovers open one step below the strip's buttons, at any theme shape, and
    // end one step above the status bar.
    const float popupTop = stripPad + s.metric.controlHeight + s.spacing.s1;
    const float popupWidth = PopoverWidth(design) * dpi, popupMaxHeight = size.y - popupTop - s.spacing.s1 - status;
    const ImVec2 popupPosition(origin.x + size.x - popupWidth - s.spacing.windowPad, origin.y + popupTop);
    const ImVec2 settingsMin = ImGui::GetCursorScreenPos();
    SettingsControl(fonts, design, dpi, engine, popupPosition, popupMaxHeight);
    // Help, last in the strip. Its popup takes Settings' position, as the device
    // popup does.
    ImGui::SetCursorScreenPos(ImVec2(settingsMin.x + s.metric.controlHeight + s.spacing.s2, settingsMin.y));
    if (IconButton("##help", Icon::Help, "Help", s, dpi, ImGui::IsPopupOpen("Help"))) openHelp = true;
    tourRect(TourStop::Settings, settingsMin, ImGui::GetItemRectMax());
    // On first run, start the tour (never over a playing song); after an update,
    // open Help on this build's additions once. Skipped without a window: tests
    // start what they draw themselves.
    if (hwnd && !tourChecked_ && tourStop < 0 && !state->playing && !state->playbackCountdown) {
        tourChecked_ = true;
        if (!preferences.tourSeen) StartTour();
        else if (preferences.helpBuild != kHelpBuild) {
            if (HelpAdded(HelpEntries(), kHelpBuild, state->addonsFolder)) openHelp = helpAdded = true;
            preferences.helpBuild = kHelpBuild;
        }
    }
    if (openHelp) { ImGui::OpenPopup("Help"); openHelp = false; }
    ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0), ImVec2(popupWidth, popupMaxHeight));
    ImGui::SetNextWindowPos(popupPosition);
    if (ImGui::BeginPopup("Help")) {
        DrawHelp(fonts, design, dpi, *state);
        ImGui::EndPopup();
    }

    const float top = origin.y + strip + s.spacing.windowPad;
    const float bottom = origin.y + size.y - status - s.spacing.windowPad;
    // The right column gets its 600 first, or the velocity row's sustain value
    // runs off the panel. Files gets the rest, 240 to 336.
    const auto grow = GrowthOf(design);
    const float leftWidth = std::clamp(size.x - 2 * s.spacing.windowPad - s.spacing.s3 - RightColumnFloor(design) * dpi,
                                       LeftColumnFloor(design) * dpi, (LeftColumnFloor(design) + 96) * dpi);
    const ImVec2 leftMin(origin.x + s.spacing.windowPad, top);
    const ImVec2 leftMax(leftMin.x + leftWidth, bottom);
    tourRect(TourStop::Files, leftMin, leftMax);
    BeginPanel("Files", leftMin, leftMax, s);
    ImGui::PushFont(fonts.Get(design), design.type.body * SpecFontScale(design));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    { FontScope font(fonts, design, design.type.heading * SpecFontScale(design), Weight::Semibold);
      // Centred on the control height, so a heading larger than the body text keeps the row's height.
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
      ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("MIDI Files");
      ImGui::PopStyleVar(); }
    ImGui::SameLine(ImGui::GetWindowWidth() - 3 * s.metric.controlHeight - 2 * s.spacing.s2);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * dpi, ImGui::GetStyle().FramePadding.y));
    // Convert audio button, alongside the panel's other buttons. Shown active
    // while a conversion runs, since the popover may be closed and this is where
    // to reopen it. Uses a waveform icon; the speaker icon is Mute's in Tracks.
    if (IconButton("##convert-audio", Icon::Audio, "Convert audio to MIDI", s, dpi, state->converting)) openConvert = true;
    const ImVec2 convertMin = ImGui::GetItemRectMin(), convertMax = ImGui::GetItemRectMax();
    ImGui::SameLine();
    if (IconButton("##sort-files", descendingFiles_ ? Icon::SortUp : Icon::SortDown, "Sort files", s, dpi))
        ImGui::OpenPopup("File sort");
    MenuUnderLastItem("File sort", s.spacing.s1);
    if (ImGui::BeginPopup("File sort")) {
        const char* labels[]{"Name", "Size", "Date modified"};
        // Widest label, a gap, then the tick.
        const float itemWidth = ImGui::CalcTextSize("Date modified").x + 44 * dpi;
        for (int i = 0; i < 3; ++i) {
            if (TickedItem(labels[i], fileSort_ == static_cast<FileSort>(i), s, dpi, itemWidth)) {
                engine.Send({ShellEngine::Action::SortFiles, {}, 0, 0, descendingFiles_, static_cast<double>(i)});
            }
        }
        ImGui::Separator();
        if (TickedItem("Ascending", !descendingFiles_, s, dpi, itemWidth))
            engine.Send({ShellEngine::Action::SortFiles, {}, 0, 0, false, static_cast<double>(fileSort_)});
        if (TickedItem("Descending", descendingFiles_, s, dpi, itemWidth))
            engine.Send({ShellEngine::Action::SortFiles, {}, 0, 0, true, static_cast<double>(fileSort_)});
        // Off, the list shows every file flat.
        ImGui::Separator();
        if (TickedItem("Folders", preferences.folders, s, dpi, itemWidth)) preferences.folders = !preferences.folders;
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(state->playing || state->busy || state->folder.empty());
    if (IconButton("##refresh-files", Icon::Refresh, "Refresh MIDI files", s, dpi)) engine.Send({ShellEngine::Action::Scan, state->folder});
    ImGui::EndDisabled(); ImGui::PopStyleVar();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - s.metric.controlHeight - s.spacing.s2);
    // The hint names the search scope: the open folder and its sub-folders.
    const std::string searchHint = browse_.empty() ? std::string("Search MIDI files")
        : "Search in " + browse_.substr(ParentFolder(browse_).size(), browse_.size() - ParentFolder(browse_).size() - 1);
    // Enter opens the top result, loaded below once the list has this frame's text.
    const bool openTopResult = ImGui::InputTextWithHint("##search", searchHint.c_str(), search_, sizeof(search_),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    // One button for both ways of opening files.
    if (IconButton("##add-files", Icon::Plus, "Add MIDI files", s, dpi))
        ImGui::OpenPopup("Add MIDI files");
    MenuUnderLastItem("Add MIDI files", s.spacing.s1);
    const bool convertRequested = openConvert;
    openConvert = false;
    if (ImGui::BeginPopup("Add MIDI files")) {
        if (ImGui::MenuItem("Open MIDI file...")) load(PickMidiFile(hwnd));
        if (ImGui::MenuItem("Choose MIDI folder...", nullptr, false, !state->playing && !state->busy)) {
            const auto path = PickFolder(hwnd);
            if (!path.empty()) { preferences.folder = path; browse_.clear(); engine.Send({ShellEngine::Action::Scan, path}); }
        }
        ImGui::EndPopup();
    }
    if (convertRequested) ImGui::OpenPopup("Convert audio");
    ImGui::SetNextWindowSizeConstraints(ImVec2(400 * dpi, 0), ImVec2(400 * dpi, 10000 * dpi));
    // Under its button, as the sort and add menus open under theirs.
    if (ImGui::IsPopupOpen("Convert audio"))
        ImGui::SetNextWindowPos(ImVec2(convertMin.x, convertMax.y + s.spacing.s1), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("Convert audio")) {
        DrawConvert(hwnd, fonts, design, dpi, engine);
        ImGui::EndPopup();
    }
    const ImVec2 listMin = ImGui::GetCursorScreenPos();
    const ImVec2 listSize = ImGui::GetContentRegionAvail();
    // The shadow is drawn after the rows, by RoundCorners below.
    skin::RecessedField(listMin, ImVec2(listMin.x + listSize.x, listMin.y + listSize.y), s, false);
    ImGui::BeginChild("##file-list", listSize, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    if (state->files->empty()) {
        // An empty list offers the action that fills it, centred in the well.
        static constexpr const char* kChoose = "Choose MIDI folder";
        const ImVec2 area = ImGui::GetContentRegionAvail();
        const float width = 2 * 12 * dpi + 16 * dpi + s.spacing.s2 + ImGui::CalcTextSize(kChoose).x;
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + std::max(0.f, (area.x - width) / 2),
                                   ImGui::GetCursorPosY() + std::max(0.f, (area.y - s.metric.controlHeight) / 2)));
        ImGui::BeginDisabled(state->playing || state->busy);
        if (TransportButton("##choose-folder", Icon::Open, kChoose, s, dpi)) {
            const auto path = PickFolder(hwnd);
            if (!path.empty()) { preferences.folder = path; browse_.clear(); engine.Send({ShellEngine::Action::Scan, path}); }
        }
        ImGui::EndDisabled();
    } else {
        std::string query(search_);
        const auto lowercase = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        query = lowercase(query);
        // Folders off: every file in one flat list, with no open folder.
        if (!preferences.folders) browse_.clear();
        if (filteredFiles_ != state->files || filteredQuery_ != query || filteredFolders_ != preferences.folders) {
            filteredFolders_ = preferences.folders;
            fileFilter_.clear();
            folderRows_.clear();
            if (query.empty() && !preferences.folders) {
                fileFilter_.resize(state->files->size());
                std::iota(fileFilter_.begin(), fileFilter_.end(), size_t{0});
            } else if (query.empty()) {
                // A rescan may have removed the open folder; fall back to the nearest ancestor
                // that still contains files.
                auto view = BrowseFolder(*state->files, browse_);
                while (!browse_.empty() && view.files.empty() && view.folders.empty()) {
                    browse_ = ParentFolder(browse_);
                    view = BrowseFolder(*state->files, browse_);
                }
                fileFilter_ = std::move(view.files);
                folderRows_ = std::move(view.folders);
            } else {
                fileFilter_ = SearchFolder(*state->files, browse_, query);
            }
            filteredFiles_ = state->files;
            filteredQuery_ = query;
        }
        if (openTopResult && !query.empty() && !fileFilter_.empty() && !state->busy) load((*state->files)[fileFilter_.front()].path);
        // A song that Next, Previous, a hotkey or shuffle loads is scrolled into view
        // once, if its row is listed here; a row click loads a row already in view.
        if (state->loaded != followedFile_) {
            if (state->loaded != clickedFile_ && revealFile_.empty()) revealFile_ = state->loaded;
            followedFile_ = state->loaded;
            clickedFile_.clear();
        }
        // Row order: the up row, folders, then files. The first two appear only while
        // browsing; a search lists files alone.
        const int upRows = query.empty() && !browse_.empty() ? 1 : 0;
        const int folderCount = static_cast<int>(folderRows_.size());
        const int fileStart = upRows + folderCount;
        bool moved = false;
        std::string moveTo;
        const float rowPitch = s.metric.controlHeight + s.spacing.s2;
        // A just-located file: its folder is open by now, so scroll its row to a third
        // of the way down, leaving context around it. A row already in view stays put.
        if (!revealFile_.empty()) {
            for (size_t row = 0; row < fileFilter_.size(); ++row)
                if ((*state->files)[fileFilter_[row]].path == revealFile_) {
                    const float rowTop = (fileStart + static_cast<int>(row)) * rowPitch;
                    if (rowTop < ImGui::GetScrollY() || rowTop + rowPitch > ImGui::GetScrollY() + listSize.y)
                        ImGui::SetScrollY(std::max(0.f, rowTop - listSize.y / 3));
                    break;
                }
            revealFile_.clear();
        }
        ImGuiListClipper clipper;
        clipper.Begin(fileStart + static_cast<int>(fileFilter_.size()), s.metric.controlHeight + s.spacing.s2);
        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto pos = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            if (i < fileStart) {
                const bool up = i < upRows;
                // The up row shows the current folder's name, not its path, which the narrow
                // list would truncate.
                const size_t parent = ParentFolder(browse_).size();
                const std::string label = up ? browse_.substr(parent, browse_.size() - parent - 1) : folderRows_[i - upRows];
                ImGui::PushID(i - fileStart);
                if (ImGui::Selectable("##folder", false, 0, ImVec2(width, s.metric.controlHeight))) {
                    moved = true;
                    moveTo = up ? ParentFolder(browse_) : browse_ + label + '\\';
                }
                FontScope rowFont(fonts, design, design.type.body * SpecFontScale(design), Weight::Regular);
                const float side = 16 * dpi;
                DrawIcon(ImGui::GetWindowDrawList(), up ? Icon::Left : Icon::Folder,
                         ImVec2(pos.x + s.spacing.s3, pos.y + (s.metric.controlHeight - side) / 2), side,
                         Colour(s.ink.secondary), dpi);
                const float inset = 2 * s.spacing.s3 + side;
                DrawEllipsis(label, width - inset - s.spacing.s3,
                             ImVec2(pos.x + inset, pos.y + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
                ImGui::PopID();
                continue;
            }
            const auto& file = (*state->files)[fileFilter_[i - fileStart]];
            ImGui::PushID(static_cast<int>(fileFilter_[i - fileStart]));
            ImGui::BeginDisabled(state->busy);
            if (ImGui::Selectable("##file", file.path == state->loaded, 0, ImVec2(width, s.metric.controlHeight))) {
                load(file.path);
                clickedFile_ = file.path;
            }
            ImGui::EndDisabled();
            // Like Explorer's search results: go to the file's location, which ends the
            // search and opens its folder at its row.
            if (ImGui::BeginPopupContextItem("##file-menu")) {
                if (!query.empty() && preferences.folders && ImGui::MenuItem("Open file location")) {
                    moved = true;
                    moveTo = FolderOf(file.name);
                    revealFile_ = file.path;
                    search_[0] = 0;
                }
                if (ImGui::MenuItem("Show in Explorer"))
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + file.path.native() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                ImGui::EndPopup();
            }
            const bool selected = file.path == state->loaded;
            auto* listDraw = ImGui::GetWindowDrawList();
            // The bar spans the row's highlight, which Selectable extends by half the item
            // spacing above and the rest below.
            if (selected) {
                const float spacing = ImGui::GetStyle().ItemSpacing.y, up = IM_TRUNC(spacing * .5f);
                listDraw->AddRectFilled(ImVec2(pos.x, pos.y - up), ImVec2(pos.x + 2 * dpi, pos.y + s.metric.controlHeight + spacing - up),
                                        Colour(s.accent.accent));
            }
            FontScope rowFont(fonts, design, design.type.body * SpecFontScale(design), selected ? Weight::Semibold : Weight::Regular);
            const auto bytes = std::to_string((file.bytes + 1023) / 1024) + " KB";
            const float sizeWidth = ImGui::CalcTextSize(bytes.c_str()).x;
            const float textY = pos.y + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2;
            // Search results show their folder; browsed rows are already under theirs. The
            // song name comes first and the folder takes the remaining width.
            const std::string shown = file.name.substr(browse_.size());
            const size_t cut = shown.find_last_of("\\/");
            const std::string leaf = cut == std::string::npos ? shown : shown.substr(cut + 1);
            const float room = width - sizeWidth - 3 * s.spacing.s3;
            DrawEllipsis(leaf, room, ImVec2(pos.x + s.spacing.s3, textY));
            const float used = ImGui::CalcTextSize(leaf.c_str()).x + s.spacing.s3;
            if (cut != std::string::npos && room - used > 3 * ImGui::CalcTextSize("...").x) {
                ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
                DrawEllipsis(shown.substr(0, cut), room - used, ImVec2(pos.x + s.spacing.s3 + used, textY));
                ImGui::PopStyleColor();
            }
            listDraw->AddText(ImVec2(pos.x + width - s.spacing.s3 - sizeWidth, textY), Colour(s.ink.secondary), bytes.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%llu bytes", file.name.c_str(), static_cast<unsigned long long>(file.bytes));
            ImGui::PopID();
        }
        if (fileFilter_.empty() && fileStart == 0) ImGui::TextDisabled("No matching files");
        if (moved) {
            browse_ = std::move(moveTo);
            filteredFiles_.reset();
            ImGui::SetScrollY(0);
        }
    }
    // Draw into the list's own draw list after EndChild has drawn its scrollbar,
    // so the corners cover both a square selected row and the scrollbar.
    ImDrawList* fileListDraw = ImGui::GetWindowDrawList();
    ImGui::EndChild();
    skin::RoundCorners(fileListDraw, listMin, ImVec2(listMin.x + listSize.x, listMin.y + listSize.y),
                       s.radius.element, Colour(s.surface.card), s, true);
    ImGui::PopStyleVar(); ImGui::PopFont();
    ImGui::EndChild();

    const float right = leftMax.x + s.spacing.s3;
    const float edge = origin.x + size.x - s.spacing.windowPad;
    // File name and sheet action above the seek groove, rows 8 dp apart. The
    // hotkey hints share the title row to save a row for Tracks at the smallest
    // window; sheet results go to the status bar for the same reason.
    const float titleHeight = s.metric.controlHeight;
    const float rowGap = 8 * dpi, seekHeight = 22 * dpi;
    // Transport, Speed and Transpose, plus the performer row while one is on.
    const int playbackRows = performerRow_ ? 3 : 2;
    const float playbackHeight = 2 * s.spacing.panelPad + titleHeight + seekHeight + (playbackRows + 1) * rowGap + playbackRows * s.metric.controlHeight;
    BeginPanel("Playback", ImVec2(right, top), ImVec2(edge, top + playbackHeight), s,
               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(fonts.Get(design), design.type.body * SpecFontScale(design));
    const auto content = ImGui::GetCursorScreenPos();
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    const char* sheetLabel = "Sheets";
    // Drawn only with the sheets add-on; otherwise the title gets the room.
    const float sheetButtonWidth = state->sheetsAddon ? 2 * 12 * dpi + 16 * dpi + s.spacing.s2 + ImGui::CalcTextSize(sheetLabel).x : -s.spacing.s2;
    // The title gives way first, truncated with an ellipsis: six keys at the
    // smallest window take 60% of this row. Past two thirds the legend is omitted,
    // since bare caps without actions say nothing useful.
    const std::string title = state->loaded.empty() ? "Playback" : Utf8(state->loaded.stem());
    float titleWidth = 0;
    { FontScope font(fonts, design, design.type.title * SpecFontScale(design), Weight::Medium);
      titleWidth = ImGui::CalcTextSize(title.c_str()).x; }
    float hintsWidth = 0;
    { FontScope font(fonts, design, design.type.meta * SpecFontScale(design));
      const float room = contentWidth - sheetButtonWidth - s.spacing.s2 - titleWidth - s.spacing.s3;
      // Every tap key's cap if they fit, else the first two and a count.
      size_t tapCaps = kAddonHotkeys;
      float width = DrawTransportHints(nullptr, s, dpi, {}, *state, tapCaps);
      if (width > std::max(room, contentWidth * .67f)) width = DrawTransportHints(nullptr, s, dpi, {}, *state, tapCaps = 2);
      if (width > 0 && width <= std::max(room, contentWidth * .67f)) {
          hintsWidth = width + s.spacing.s3;
          DrawTransportHints(ImGui::GetWindowDrawList(), s, dpi,
              ImVec2(content.x + contentWidth - sheetButtonWidth - hintsWidth + s.spacing.s3 - s.spacing.s2,
                     content.y + (titleHeight - ImGui::GetTextLineHeight()) / 2), *state, tapCaps);
      } }
    { FontScope font(fonts, design, design.type.title * SpecFontScale(design), Weight::Medium);
      DrawEllipsis(title,
                   contentWidth - sheetButtonWidth - hintsWidth - s.spacing.s2, ImVec2(content.x, content.y + (titleHeight - ImGui::GetTextLineHeight()) / 2)); }
    ImGui::SetCursorScreenPos(ImVec2(content.x + contentWidth - sheetButtonWidth, content.y));
    const bool haveFile = !state->loaded.empty() && !state->rows.empty();
    ImGui::BeginDisabled(state->busy || (!haveFile && state->files->empty()));
    if (state->sheetsAddon && TransportButton("##export", Icon::Down, sheetLabel, s, dpi)) ImGui::OpenPopup("Export MIDI");
    if (state->sheetsAddon) MenuUnderLastItem("Export MIDI", s.spacing.s1, true);
    if (ImGui::BeginPopup("Export MIDI")) {
        const auto request = [&](ShellEngine::Action action, const char* status) {
            send(action);
            sheetStatusGeneration_ = state->generation;
            sheetStatus_ = status;
            sheetPending_ = true;
        };
        // Copy sheet gives quick text for chat. The sheet editor is midi-converter's
        // browser page, where all sheet customisation happens; the app stores no sheet
        // settings. Saved files are the same sheet under the sheets folder, mirroring
        // the MIDI folder's sub-folders and styled by a page saved from the editor,
        // for the open file or the whole list.
        ImGui::BeginDisabled(!haveFile);
        if (ImGui::MenuItem("Copy sheet to clipboard")) request(ShellEngine::Action::CopySheet, "Preparing sheet...");
        if (ImGui::MenuItem("Open sheet editor in browser")) request(ShellEngine::Action::OpenSheetEditor, "Opening the sheet editor...");
        if (ImGui::MenuItem("Save sheet files for this MIDI")) request(ShellEngine::Action::SaveSheetFiles, "Saving sheet files...");
        ImGui::EndDisabled();
        if (state->sheetBatchRunning) {
            if (ImGui::MenuItem("Stop saving the library")) engine.Send({ShellEngine::Action::SheetBatchCancel});
        } else if (ImGui::MenuItem("Save sheet files for every MIDI in the list...", nullptr, false, !state->files->empty())) {
            openLibrarySave = true;
        }
        ImGui::Separator();
        ImGui::TextDisabled("Files to save");
        const auto output = [&](const char* label, bool on, const char* key) {
            if (TickedItem(label, on, s, dpi)) engine.Send({ShellEngine::Action::SheetFiles, {}, 0, 0, !on, 0, key});
        };
        output("Image (.png)", state->sheetImage, "image");
        output("Text (.txt)", state->sheetTextFile, "text");
        output("Editor page (.html)", state->sheetPageFile, "page");
        ImGui::Separator();
        const auto sheetsFolder = state->sheetsFolder.empty() ? DefaultSheetsFolder(state->folder) : state->sheetsFolder;
        if (ImGui::MenuItem(("Save to: " + Utf8(sheetsFolder) + "...").c_str())) {
            const auto path = PickFolder(hwnd);
            if (!path.empty()) engine.Send({ShellEngine::Action::SheetsFolder, path});
        }
        const std::string styleLabel = state->sheetStylePage.empty() ? "Sheet style: editor defaults..."
                                                                       : "Sheet style: " + Utf8(state->sheetStylePage.filename()) + "...";
        if (ImGui::MenuItem(styleLabel.c_str())) {
            const auto path = PickFile(hwnd, PickKind::Page);
            if (!path.empty()) engine.Send({ShellEngine::Action::SheetStylePage, path});
        }
        if (!state->sheetStylePage.empty() && ImGui::MenuItem("Back to the editor's defaults")) engine.Send({ShellEngine::Action::SheetStylePage, {}});
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    // Saving the library writes files for every MIDI in the list, often hundreds,
    // so the menu item opens a confirmation stating how many, what and where. It
    // is opened here, as a sibling of the menu, because the menu closes on click.
    if (openLibrarySave) { ImGui::OpenPopup("Save library sheets"); openLibrarySave = false; }
    ImGui::SetNextWindowSizeConstraints(ImVec2(440 * dpi, 0), ImVec2(440 * dpi, 10000 * dpi));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save library sheets", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        const size_t total = state->files->size();
        const auto count = std::to_string(total);
        std::vector<std::string> kinds;
        if (state->sheetImage) kinds.push_back("an image");
        if (state->sheetTextFile) kinds.push_back("a text file");
        if (state->sheetPageFile) kinds.push_back("an editor page");
        std::string what;
        for (size_t i = 0; i < kinds.size(); ++i)
            what += (i == 0 ? "" : i + 1 == kinds.size() ? " and " : ", ") + kinds[i];
        if (!what.empty()) what[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(what[0])));
        const auto sheetsFolder = state->sheetsFolder.empty() ? DefaultSheetsFolder(state->folder) : state->sheetsFolder;
        { FontScope font(fonts, design, design.type.body * SpecFontScale(design), Weight::Semibold);
          ImGui::TextUnformatted("Save sheet files for every MIDI in the list?"); }
        ImGui::Spacing();
        // What and how many, then the destination on its own line. The button is
        // disabled when nothing is ticked.
        if (!kinds.empty()) ImGui::TextWrapped("%s", (what + (total == 1 ? " for the one MIDI file" : " for each of the " + count + " MIDI files")).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
        ImGui::TextWrapped("%s", Utf8(sheetsFolder).c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::BeginDisabled(kinds.empty());
        if (TransportButton("##library-save-go", (total == 1 ? "Save 1 sheet" : "Save " + count + " sheets").c_str(), s, dpi, true)) {
            engine.Send({ShellEngine::Action::SaveLibrarySheets});
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (TransportButton("##library-save-cancel", "Cancel", s, dpi)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    sheetNote_ = sheetStatus_;
    if (sheetNote_.empty()) sheetNote_ = state->sheetBatchStatus;
    if (state->sheetReady && state->sheetMerged)
        sheetNote_ += " " + std::to_string(state->sheetMerged) + " shared notes merged.";
    if (state->sheetReady && state->sheetUnmapped)
        sheetNote_ += " " + std::to_string(state->sheetUnmapped) + " unmapped notes dropped.";
    const auto number = [&](ShellEngine::Action action, double amount) {
        engine.Send({action, {}, state->generation, 0, false, amount});
    };
    ImGui::BeginDisabled(state->loaded.empty() || state->rows.empty() || state->busy);
    ImGui::SetCursorScreenPos(ImVec2(content.x, content.y + titleHeight + rowGap));
    if (!seeking_ || seekGeneration_ != state->generation) {
        seekPosition_ = static_cast<float>(state->position);
        seeking_ = false;
    }
    const bool seekChanged = Groove("##seek", &seekPosition_, 0, static_cast<float>(std::max(.001, state->duration)),
                                     contentWidth, seekHeight, s, dpi, false);
    if (ImGui::IsItemActivated()) { seeking_ = true; seekGeneration_ = state->generation; }
    if (seeking_ && ImGui::IsItemDeactivatedAfterEdit()) {
        if (seekGeneration_ == state->generation) number(ShellEngine::Action::Seek, seekPosition_);
        seeking_ = false;
    } else if (seekChanged && !ImGui::IsItemActive()) number(ShellEngine::Action::Seek, seekPosition_);
    // The dragged time, else the time a click under the pointer seeks to, mapped
    // as the slider maps it (2 px in from each end).
    if (seeking_) ImGui::SetTooltip("%s", Time(seekPosition_).c_str());
    else if (ImGui::IsItemHovered()) {
        const auto barMin = ImGui::GetItemRectMin(), barMax = ImGui::GetItemRectMax();
        const float at = std::clamp((ImGui::GetIO().MousePos.x - barMin.x - 2) / std::max(1.f, barMax.x - barMin.x - 4), 0.f, 1.f);
        ImGui::SetTooltip("%s", Time(at * state->duration).c_str());
    }
    const float transportY = content.y + titleHeight + seekHeight + 2 * rowGap;
    // From the title row (with the hotkeys) down to the transport.
    tourRect(TourStop::Play, content, ImVec2(content.x + contentWidth, transportY + s.metric.controlHeight));
    ImGui::SetCursorScreenPos(ImVec2(content.x, transportY));
    if (PlayButton("##play", state->playing, state->playbackCountdown, s, dpi))
        send(ShellEngine::Action::PlayCountdown);
    ImGui::SameLine();
    if (IconButton("##restart", Icon::Refresh, "Restart", s, dpi)) send(ShellEngine::Action::Restart);
    ImGui::SameLine();
    if (TransportButton("##back10", SeekLabel(state->seekStep, false).c_str(), s, dpi)) send(ShellEngine::Action::Back10);
    ImGui::SameLine();
    if (TransportButton("##forward10", SeekLabel(state->seekStep, true).c_str(), s, dpi)) send(ShellEngine::Action::Forward10);
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(state->files->empty() || state->busy);
    if (IconButton("##previous", Icon::Left, "Previous MIDI file", s, dpi)) send(ShellEngine::Action::Previous);
    ImGui::SameLine();
    if (IconButton("##next", Icon::Right, "Next MIDI file", s, dpi)) send(ShellEngine::Action::Next);
    ImGui::EndDisabled(); ImGui::SameLine();
    if (IconButton("##stop", Icon::Close, "Stop all output and cancel countdown", s, dpi)) send(ShellEngine::Action::Stop);
    ImGui::BeginDisabled(state->loaded.empty() || state->rows.empty() || state->busy);
    const std::string time = state->playbackCountdown ? "Starts in " + std::to_string(state->playbackCountdown) + "s" :
        Time(seeking_ ? seekPosition_ : state->position) + " / " + Time(state->duration);
    dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(content.x + contentWidth - ImGui::CalcTextSize(time.c_str()).x,
                transportY + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2), Colour(s.ink.secondary), time.c_str());
    ImGui::SetCursorScreenPos(ImVec2(content.x, transportY + s.metric.controlHeight + rowGap));
    const auto label = [&](const char* text) {
        FontScope font(fonts, design, design.type.meta * SpecFontScale(design));
        const auto pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::CalcTextSize(text).x;
        ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, pos.y + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2), Colour(s.ink.secondary), text);
        ImGui::Dummy(ImVec2(width, s.metric.controlHeight)); ImGui::SameLine();
    };
    // Speed scales the player's clock, so it can be dragged during playback. Steps
    // of 0.05; the readout button steps by 0.25 and middle-click resets to 1.
    label("Speed");
    float speedValue = static_cast<float>(state->speed);
    if (Groove("##speed", &speedValue, static_cast<float>(state->speedMin), static_cast<float>(state->speedMax),
               132 * dpi, s.metric.controlHeight, s, dpi, true))
        number(ShellEngine::Action::Speed, std::round(speedValue * 20) / 20);
    ImGui::SameLine();
    {
        char speed[32]; snprintf(speed, sizeof(speed), "%.2f\xc3\x97", std::round(speedValue * 20) / 20);
        const auto speedMin = ImGui::GetCursorScreenPos();
        double clicked = std::round(speedValue * 20) / 20;
        if (ReadoutClick("##speed-reset", ImVec2(48 * dpi, s.metric.controlHeight), 0.25, 1.0, &clicked))
            number(ShellEngine::Action::Speed, clicked);
        ImGui::GetWindowDrawList()->AddText(ImVec2(speedMin.x + std::floor((48 * dpi - ImGui::CalcTextSize(speed).x) / 2),
            speedMin.y + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2), Colour(s.ink.primary), speed);
    }
    ImGui::SameLine();
    label("Transpose");
    float transpose = static_cast<float>(state->transpose);
    if (Groove("##transpose", &transpose, -12, 12, 132 * dpi, s.metric.controlHeight, s, dpi, true))
        number(ShellEngine::Action::Transpose, std::round(transpose));
    ImGui::SameLine();
    // Same readout as Speed, a button that resets to the default, at the same width
    // and distance from its groove.
    {
        char transposeText[16]; snprintf(transposeText, sizeof(transposeText), "%+d", static_cast<int>(std::round(transpose)));
        const auto transposeMin = ImGui::GetCursorScreenPos();
        double clicked = std::round(transpose);
        if (ReadoutClick("##transpose-reset", ImVec2(48 * dpi, s.metric.controlHeight), 1, 0, &clicked))
            number(ShellEngine::Action::Transpose, clicked);
        ImGui::GetWindowDrawList()->AddText(ImVec2(transposeMin.x + std::floor((48 * dpi - ImGui::CalcTextSize(transposeText).x) / 2),
            transposeMin.y + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2), Colour(s.ink.primary), transposeText);
        tourRect(TourStop::Speed, ImVec2(content.x, transportY + s.metric.controlHeight + rowGap), ImGui::GetItemRectMax());
    }
    // Performer row: its panel controls and triggers. These change per song and
    // mid-song, so they live here rather than in Settings.
    if (performerRow_) {
        ImGui::SetCursorScreenPos(ImVec2(content.x, transportY + 2 * (s.metric.controlHeight + rowGap)));
        PerformerPanelRow("##performer-row", *state, engine, s, dpi, label);
    }
    ImGui::EndDisabled();
    ImGui::PopFont();
    ImGui::EndChild();
    dl = ImGui::GetWindowDrawList();

    const float trackTop = top + playbackHeight + s.spacing.s3;
    const float collapsedHeight = 2 * s.spacing.panelPad + s.metric.controlHeight;
    const float requestedCurveHeight = velocityExpanded ?
        (428.f + 2 * grow.panel + 2 * grow.control + (nameOperation_ ? 44.f + grow.control : 0.f)) * dpi : collapsedHeight;
    // Tracks collapses like Velocity Response; when closed, Velocity Response moves
    // up into the table's space.
    const bool tracksOpen = tracksExpanded; // this frame's state; the button below changes the next
    const float tracksFloor = tracksOpen ? (168 + 2 * grow.panel + 3 * grow.control) * dpi : collapsedHeight + s.spacing.s3;
    const float curveHeight = std::min(requestedCurveHeight, std::max(collapsedHeight, bottom - trackTop - tracksFloor));
    const float curveTop = tracksOpen ? bottom - curveHeight : trackTop + collapsedHeight + s.spacing.s3;
    tourRect(TourStop::Tracks, ImVec2(right, trackTop), ImVec2(edge, curveTop - s.spacing.s3));
    BeginPanel("Tracks", ImVec2(right, trackTop), ImVec2(edge, curveTop - s.spacing.s3), s,
               tracksOpen ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(fonts.Get(design), design.type.body * SpecFontScale(design));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    if (DisclosureHeading("##tracks-disclosure", tracksOpen, "Tracks",
                          state->rows.empty() ? std::string() : std::to_string(state->rows.size()), fonts, design, s, dpi))
        tracksExpanded = !tracksExpanded;
    // One toggle: on means the rows match what Solo Piano leaves; clicking again
    // restores every track.
    const bool applied = SoloPianoApplied(state->rows);
    const bool allPiano = AllPiano(state->rows);
    const float actionsWidth = 2 * 12 * dpi + 16 * dpi + s.spacing.s2 + ImGui::CalcTextSize("Solo Piano").x;
    ImGui::SameLine(ImGui::GetWindowWidth() - actionsWidth);
    ImGui::BeginDisabled(state->rows.empty() || state->busy || allPiano);
    if (TransportButton("##solo-piano", Icon::Piano, "Solo Piano", s, dpi, false, applied))
        send(applied ? ShellEngine::Action::UnmuteAll : ShellEngine::Action::SoloPiano);
    if (applied && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Unmute all");
    ImGui::EndDisabled();
    // One size up from body text, for headings and rows alike, since rows are as
    // tall as their buttons.
    const float tableText = (design.type.body + 1) * SpecFontScale(design);
    const float tableHeading = (design.type.meta + 1) * SpecFontScale(design);
    ImGui::PushFont(fonts.Get(design), tableText);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * dpi, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    const ImVec2 tableMin = ImGui::GetCursorScreenPos();
    const ImVec2 tableSize = ImGui::GetContentRegionAvail();
    // No recessed fill: rows are card-coloured, so it would only show as a sliver
    // under the last row. The frame and corners are drawn after the table.
    ImDrawList* tableDraw = nullptr;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(s.spacing.s2, s.spacing.s1));
    // Row rules are drawn above each row after the table; BordersInnerH also adds
    // one under the last row, which looks like a clipped row.
    std::vector<float> rules;
    float headerBottom = tableMin.y, headerHeight = 0.f;
    const float rowHeight = s.metric.controlHeight + 2 * s.spacing.s1;
    // The two text columns split the width according to this file's contents.
    // The table isn't resizable, so ImGui reapplies the weights every frame and one
    // table serves every load; a new load starts its rows at the top.
    float nameWeight = ImGui::CalcTextSize("TRACK").x, instrumentWeight = ImGui::CalcTextSize("INSTRUMENT").x;
    for (const auto& row : state->rows) {
        nameWeight = std::max(nameWeight, ImGui::CalcTextSize(row.name.c_str()).x * 1.06f);
        instrumentWeight = std::max(instrumentWeight, ImGui::CalcTextSize(row.instrument.c_str()).x +
            (row.piano ? 14 * dpi + s.spacing.s1 : 0.f));
    }
    // The scroll is set for the table's own scrolling window, which BeginTable begins
    // unless this window skips its items; then nothing may be left to the next window.
    if (tracksOpen && tracksGeneration_ != state->generation && !ImGui::GetCurrentWindowRead()->SkipItems)
        ImGui::SetNextWindowScroll(ImVec2(0, 0));
    // PadOuterX, or the # column sits flush against the frame's left edge.
    if (tracksOpen && ImGui::BeginTable("##tracks", 7, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings, tableSize)) {
        tracksGeneration_ = state->generation;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 20 * dpi);
        ImGui::TableSetupColumn("TRACK", ImGuiTableColumnFlags_WidthStretch, nameWeight);
        ImGui::TableSetupColumn("INSTRUMENT", ImGuiTableColumnFlags_WidthStretch, instrumentWeight);
        ImGui::TableSetupColumn("CH", ImGuiTableColumnFlags_WidthFixed, 28 * dpi);
        ImGui::TableSetupColumn("NOTES", ImGuiTableColumnFlags_WidthFixed, 48 * dpi);
        ImGui::TableSetupColumn("##mute-heading", ImGuiTableColumnFlags_WidthFixed, s.metric.controlHeight + s.spacing.s2);
        ImGui::TableSetupColumn("##solo-heading", ImGuiTableColumnFlags_WidthFixed, s.metric.controlHeight + s.spacing.s2);
        std::array<ImVec2, 2> actionHeaderMin, actionHeaderMax;
        { FontScope font(fonts, design, tableHeading, Weight::Semibold);
          // Explicit height in the heading font so the header's bottom is exact;
          // TableGetHeaderRowHeight() measures in the body font.
          headerHeight = ImGui::GetTextLineHeight() + 2 * ImGui::GetStyle().CellPadding.y;
          ImGui::TableNextRow(ImGuiTableRowFlags_Headers, headerHeight);
          for (int column = 0; column < 7; ++column) {
              ImGui::TableSetColumnIndex(column);
              // Always pass the column's name, never "": an empty label takes its ID from the
              // parent, so the two icon columns would collide. The mute and solo names start
              // with ## and draw nothing; their labels are painted over the columns below.
              ImGui::TableHeader(ImGui::TableGetColumnName(column));
          } }
        const auto* table = ImGui::GetCurrentTable();
        // Use the real header row rather than a guessed offset, so both labels share
        // the baseline of the five headers beside them.
        for (int action = 0; action < 2; ++action) {
            actionHeaderMin[action] = ImVec2(table->Columns[5 + action].WorkMinX, table->RowPosY1);
            actionHeaderMax[action] = ImVec2(table->Columns[5 + action].WorkMinX + s.metric.controlHeight,
                table->RowPosY1 + headerHeight);
        }
        headerBottom = table->RowPosY1 + headerHeight;
        const bool anySolo = AnySolo(state->rows);
        ImGuiListClipper tracks;
        tracks.Begin(static_cast<int>(state->rows.size()), rowHeight);
        while (tracks.Step()) for (int rowIndex = tracks.DisplayStart; rowIndex < tracks.DisplayEnd; ++rowIndex) {
            const auto& row = state->rows[rowIndex];
            const bool audible = TrackAudible(row, anySolo);
            ImGui::PushID(static_cast<int>(row.index));
            ImGui::TableNextRow(0, rowHeight);
            rules.push_back(table->RowPosY1);
            ImGui::PushStyleColor(ImGuiCol_Text, Colour(audible ? s.ink.primary : s.ink.tertiary));
            if (!audible) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, Colour(s.surface.recessed));
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::Text("%zu", row.index + 1);
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
            { FontScope font(fonts, design, tableText, audible ? Weight::Medium : Weight::Regular);
              Ellipsis(row.name, ImGui::GetContentRegionAvail().x); }
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
            if (row.piano) {
                auto p = ImGui::GetCursorScreenPos();
                p.y += ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset + (ImGui::GetTextLineHeight() - 14 * dpi) / 2;
                DrawIcon(ImGui::GetWindowDrawList(), Icon::Piano, p, 14 * dpi, Colour(s.ink.secondary), dpi);
                ImGui::Dummy(ImVec2(14 * dpi, ImGui::GetTextLineHeight())); ImGui::SameLine(0, s.spacing.s1);
            }
            Ellipsis(row.instrument, ImGui::GetContentRegionAvail().x);
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); Ellipsis(row.channels, ImGui::GetContentRegionAvail().x);
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::Text("%zu", row.notes);
            ImGui::PopStyleColor();
            ImGui::BeginDisabled(state->busy);
            ImGui::TableNextColumn();
            if (IconButton("##mute", row.muted ? Icon::Muted : Icon::Speaker,
                           row.muted ? "Unmute track" : "Mute track", s, dpi, row.muted)) send(ShellEngine::Action::Mute, row.index, !row.muted);
            ImGui::TableNextColumn();
            if (IconButton("##solo", Icon::Solo, row.solo ? "Clear solo" : "Solo track", s, dpi, row.solo))
                send(ShellEngine::Action::Solo, row.index, !row.solo);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (state->rows.empty()) {
            ImGui::TableNextRow(0, rowHeight);
            rules.push_back(table->RowPosY1);
            ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding();
            // Say so when a loaded file has nothing to play; with no file, show nothing.
            if (!state->loaded.empty()) ImGui::TextUnformatted("No note tracks");
        }
        { FontScope font(fonts, design, tableHeading, Weight::Semibold);
          auto* headers = ImGui::GetWindowDrawList();
          for (int action = 0; action < 2; ++action) {
              const char* label = action ? "SOLO" : "MUTE";
              const float textWidth = ImGui::CalcTextSize(label).x;
              // "MUTE" is wider than its button, so the clip rect includes the cell padding
              // on both sides to avoid cutting the M.
              const float spill = ImGui::GetStyle().CellPadding.x;
              headers->PushClipRect(ImVec2(actionHeaderMin[action].x - spill, actionHeaderMin[action].y),
                                    ImVec2(actionHeaderMax[action].x + spill, actionHeaderMax[action].y), false);
              // Same top padding and ink as TableHeader, so all seven headings share one
              // baseline and colour.
              headers->AddText(ImVec2(std::floor(actionHeaderMin[action].x +
                                          (actionHeaderMax[action].x - actionHeaderMin[action].x - textWidth) / 2),
                                      actionHeaderMin[action].y + ImGui::GetStyle().CellPadding.y),
                               ImGui::GetColorU32(ImGuiCol_Text), label);
              headers->PopClipRect();
          } }
        // The scrolling table draws into its own inner window, rendered over this
        // panel, so the frame must go into that window's draw list, after the rows.
        tableDraw = ImGui::GetCurrentTable()->InnerWindow->DrawList;
        ImGui::EndTable();
    }
    if (tableDraw) {
        const float left = tableMin.x, right = tableMin.x + tableSize.x;
        tableDraw->AddLine(ImVec2(left, headerBottom), ImVec2(right, headerBottom), ImGui::GetColorU32(ImGuiCol_TableBorderStrong));
        // Clip below the header so a row scrolled under it can't draw its rule across
        // the headings; the first row's rule is the header's own.
        tableDraw->PushClipRect(ImVec2(left, headerBottom + 1), ImVec2(right, tableMin.y + tableSize.y), false);
        for (const float y : rules)
            if (y > headerBottom + 0.5f)
                tableDraw->AddLine(ImVec2(left, y), ImVec2(right, y), ImGui::GetColorU32(ImGuiCol_TableBorderLight));
        tableDraw->PopClipRect();
        skin::RoundCorners(tableDraw, tableMin, ImVec2(tableMin.x + tableSize.x, tableMin.y + tableSize.y),
                           s.radius.element, Colour(s.surface.card), s, true);
    }
    ImGui::PopStyleVar(2); // the table's cell and frame padding
    ImGui::PopFont();
    ImGui::PopStyleVar(); ImGui::PopFont();
    ImGui::EndChild();

    DrawVelocity(fonts, design, dpi, engine, ImVec2(right, curveTop), ImVec2(edge, curveTop + curveHeight));
    DrawStatus(fonts, design, dpi, *state, ImVec2(origin.x, origin.y + size.y - status), size.x, status);
    // These are OS windows of their own, above the tour's dim and out of its
    // modal's reach, so they wait out the tour and come back as they were.
    if (preferences.keyMappingOpen && tourStop < 0) DrawKeyMapping(fonts, design, dpi, engine);
    else mappingArmed_ = false;
    if (tourStop < 0) {
        DrawAutoVolume(fonts, design, dpi, engine);
        DrawThemeEditor(fonts, design, dpi);
    }
    DrawLog(hwnd, fonts, design, dpi, engine);
    // Drawn last, and only in the full window; mini has no tour.
    DrawTour(fonts, design, dpi, origin, size);
}
}
