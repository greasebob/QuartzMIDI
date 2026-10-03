#include "Panels.hpp"
#include "IconData.hpp"
#include "GameKeyMaps.hpp"
#include "SavedFiles.hpp"
#include "imgui_internal.h"
#include "json.hpp"
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <fstream>
#include <cmath>
#include <numeric>
#include <unordered_map>
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
                  Mini, Expand, Copy, Rename, Check, Sort, Undo, Redo, Anchor, Clear,
                  Draw, Delete,
                  Hold, Tap, Audio, Discord, Roblox, Help,
                  Repeat, Repeat1, Stop, Restart, More, Warning, Upgrade };

// Icons are Lucide, flattened to polylines by tools/gen-icons.py into
// ui/IconData.hpp. Vector rather than a raster atlas so they stay crisp at
// 150% and 200%. `turn` rotates the icon clockwise about its centre, in radians.
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 min, float side, ImU32 ink, float dpi, float turn = 0) {
    // On a whole pixel, so both sides of a symmetric glyph blur alike.
    min = ImVec2(std::round(min.x), std::round(min.y));
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
        // Lucide's line caps are round and reach half a stroke past each end;
        // ImGui cuts a stroke flat at its end, which left a trash lid no wider
        // than the can. An open path is lengthened by that half stroke instead.
        if (!path.closed) {
            const auto extend = [&](int end, int inner) {
                ImVec2& at = dl->_Path[end];
                const ImVec2 from = dl->_Path[inner];
                const float dx = at.x - from.x, dy = at.y - from.y, length = std::sqrt(dx * dx + dy * dy);
                if (length > 0) at = ImVec2(at.x + dx / length * thickness / 2, at.y + dy / length * thickness / 2);
            };
            extend(0, 1);
            extend(dl->_Path.Size - 1, dl->_Path.Size - 2);
        }
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

// While held, buttons have no fill or frame until hovered or on, so the one
// filled button beside them leads. The primary button keeps its own.
struct BareButtons {
    BareButtons() { for (const auto colour : {ImGuiCol_Button, ImGuiCol_Border, ImGuiCol_BorderShadow}) ImGui::PushStyleColor(colour, IM_COL32(0, 0, 0, 0)); }
    ~BareButtons() { ImGui::PopStyleColor(3); }
    BareButtons(const BareButtons&) = delete;
    BareButtons& operator=(const BareButtons&) = delete;
};

// Icon and label are centred as one unit. `active` marks a toggle that is on,
// styled as in IconButton. `reserve` lists every label the button can show, so
// it keeps the widest width and neighbouring controls don't shift.
bool TransportBody(const char* id, const Icon* icon, const char* label,
                   const skin::Skin& s, float dpi, bool primary, bool active = false,
                   std::initializer_list<const char*> reserve = {}) {
    ImVec2 min = ImGui::GetCursorScreenPos();
    const float pad = primary ? s.spacing.s4 : s.spacing.s3;
    const float side = 16 * dpi;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    float reserved = labelWidth;
    for (const char* other : reserve) reserved = std::max(reserved, ImGui::CalcTextSize(other).x);
    // Without a label, the icon alone is centred.
    const float lead = icon ? side + (reserved > 0 ? s.spacing.s2 : 0.f) : 0.f;
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
// `iconOnly` leaves the word to the caller's tooltip, as in mini.
bool PlayButton(const char* id, bool playing, int countdown, const skin::Skin& s, float dpi, bool iconOnly = false) {
    if (iconOnly) return TransportButton(id, playing ? Icon::Pause : countdown ? Icon::Stop : Icon::Play, "", s, dpi, true);
    return TransportButton(id, playing ? Icon::Pause : countdown ? Icon::Stop : Icon::Play,
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
// muted draws the preview greyed, as a device waiting to come back. A preview
// too long for the frame ends in an ellipsis before the chevron.
void DrawEllipsis(const std::string& text, float width, ImVec2 min, ImDrawList* dl = nullptr, ImU32 ink = 0);
bool ChevronCombo(const char* id, const char* preview, bool muted = false) {
    const ImVec2 min = ImGui::GetCursorScreenPos(), pad = ImGui::GetStyle().FramePadding;
    const float width = ImGui::CalcItemWidth(), height = ImGui::GetFrameHeight();
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), quiet = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const bool open = ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton);
    const float side = height * .5f;
    DrawEllipsis(preview, width - pad.x - height, ImVec2(min.x + pad.x, min.y + pad.y), list, muted ? quiet : ink);
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
    const float depth = 6 * dpi;
    const ImVec2 track(min.x, min.y + (height - depth) / 2);
    skin::RecessedRect(draw, track, ImVec2(track.x + width, track.y + depth), 3 * dpi, s);
    const float fill = high > low ? std::clamp((*value - low) / (high - low), 0.f, 1.f) * width : 0;
    if (fill > 0) draw->AddRectFilled(track, ImVec2(track.x + fill, track.y + depth), Faded(s.accent.accent), 3 * dpi);
    // A groove without a resting handle fades it in on hover and out on leave.
    const float show = thumb ? 1.f : Ease(ImHashStr("thumb", 0, ImGui::GetItemID()), ImGui::IsItemActive() || ImGui::IsItemHovered() ? 1.f : 0.f);
    if (show > 0) {
        const float x = std::clamp(track.x + fill, track.x + 6 * dpi, track.x + width - 6 * dpi);
        const int first = draw->VtxBuffer.Size;
        // At either end the handle is flush with the groove, which a panel clips at
        // its edge, so its shadow may spill past the clip by its own spread.
        const float reach = std::max(s.ambient.blur, s.contact.blur) + std::max(s.ambient.offsetY, s.contact.offsetY);
        const ImVec2 clipMin = draw->GetClipRectMin(), clipMax = draw->GetClipRectMax();
        draw->PushClipRect(ImVec2(clipMin.x - reach, clipMin.y - reach), ImVec2(clipMax.x + reach, clipMax.y + reach));
        skin::RaisedRect(draw, ImVec2(x - 6 * dpi, track.y - 4 * dpi), ImVec2(x + 6 * dpi, track.y + depth + 4 * dpi),
                         3 * dpi, s, Faded(s.surface.elevated));
        draw->PopClipRect();
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
int Segments(const char* id, const std::vector<const char*>& labels, int selected, const skin::Skin& s, float dpi,
             int gapAfter = -1, const std::vector<Icon>& icons = {}) {
    const float height = s.metric.controlHeight, inset = s.spacing.s1;
    const float divide = gapAfter >= 0 ? 3 * inset : 0;
    const float iconSide = 14 * dpi, iconGap = 6 * dpi;
    float segment = 0;
    for (const char* label : labels) segment = std::max(segment, ImGui::CalcTextSize(label).x);
    if (icons.size()) segment += iconSide + iconGap;
    segment += s.spacing.s4 + s.spacing.s1;
    const ImVec2 well = ImGui::GetCursorScreenPos();
    const float width = 2 * inset + segment * labels.size() + inset * (labels.size() - 1) + divide;
    auto* draw = ImGui::GetWindowDrawList();
    skin::RecessedRect(draw, well, ImVec2(well.x + width, well.y + height), s.radius.control, s);
    ImGui::PushID(id);
    const auto xOf = [&](int i) { return well.x + inset + i * (segment + inset) + (i > gapAfter ? divide : 0); };
    // The selection is one raised thumb that slides between segments; the buttons
    // over it are transparent.
    // Unchosen segments are bare words in the well, parted by short dividers that
    // fade out beside the thumb, so the control reads as one piece.
    const bool thumb = selected >= 0 && selected < static_cast<int>(labels.size());
    float thumbX = 0, at = -2;
    if (thumb) {
        at = Ease(ImGui::GetID("##thumb"), static_cast<float>(selected));
        const int from = static_cast<int>(std::floor(at)), to = std::min(from + 1, static_cast<int>(labels.size()) - 1);
        thumbX = xOf(from) + (xOf(to) - xOf(from)) * (at - from);
    }
    for (int i = 1; i < static_cast<int>(labels.size()); ++i) {
        const float x = xOf(i) - (i == gapAfter + 1 ? (divide + inset) / 2 : inset / 2);
        const float clear = thumb ? std::max({thumbX - x, x - thumbX - segment, 0.f}) : segment;
        const float show = std::clamp((clear - inset) / (segment / 2), 0.f, 1.f);
        // The gap's divider stays at full strength; the others are a hairline's.
        const skin::Argb ink = i == gapAfter + 1 ? s.ink.tertiary : s.border.hairline;
        if (show > 0)
            draw->AddLine(ImVec2(x, well.y + 2.5f * inset), ImVec2(x, well.y + height - 2.5f * inset),
                          Faded(Mix(ink & 0xFFFFFFu, ink, show)), dpi);
    }
    if (thumb) {
        const int first = draw->VtxBuffer.Size;
        skin::RaisedRect(draw, ImVec2(thumbX, well.y + inset), ImVec2(thumbX + segment, well.y + height - inset),
                         s.radius.element, s, Colour(s.surface.elevated));
        FadeVertices(draw, first, std::clamp(ImGui::GetStyle().Alpha, 0.f, 1.f));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.radius.element);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    int clicked = -1, index = 0;
    for (const char* label : labels) {
        const bool chosen = index == selected;
        const float x = xOf(index);
        ImGui::SetCursorScreenPos(ImVec2(x, well.y + inset));
        const ImGuiID key = ImGui::GetID(index);
        const float hover = EaseHover(key);
        // Hover fills only the segments the thumb is not on.
        const float under = 1.f - std::clamp(std::abs(at - index), 0.f, 1.f);
        const ImU32 fill = Colour(Mix(0, StyleArgb(ImGuiCol_ButtonHovered), hover * (1.f - under)));
        ImGui::PushStyleColor(ImGuiCol_Button, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, fill);
        ImGui::PushStyleColor(ImGuiCol_Text, Colour(Mix(s.ink.secondary, s.ink.primary, std::max(hover, under))));
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
        ImGui::PopStyleColor(4);
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

// Segments' width without icons or a gap, as Segments measures it.
float SegmentsWidth(const std::vector<const char*>& labels, const skin::Skin& s) {
    float segment = 0;
    for (const char* label : labels) segment = std::max(segment, ImGui::CalcTextSize(label).x);
    segment += s.spacing.s4 + s.spacing.s1;
    return 2 * s.spacing.s1 + segment * labels.size() + s.spacing.s1 * (labels.size() - 1);
}

// A setting with a few choices: its label, and the segments at the row's right
// end as a switch's rail is, or under the label when the row has no room.
int SettingSegments(const char* label, const char* id, const std::vector<const char*>& labels, int selected,
                    const skin::Skin& s, float dpi) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float room = ImGui::GetContentRegionAvail().x, width = SegmentsWidth(labels, s);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (ImGui::CalcTextSize(label).x + s.spacing.s4 + width <= room) {
        ImGui::SameLine(0, 0);
        ImGui::SetCursorScreenPos(ImVec2(start.x + room - width, start.y));
    }
    return Segments(id, labels, selected, s, dpi);
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
                       [&](const PerformerControl& control) { return control.isChoice && control.panel && state.performer.Drawn(control); });
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

void DrawEllipsis(const std::string& text, float width, ImVec2 min, ImDrawList* dl, ImU32 ink) {
    const float height = ImGui::GetTextLineHeight();
    if (!dl) dl = ImGui::GetWindowDrawList();
    if (!ink) ink = ImGui::GetColorU32(ImGuiCol_Text);
    dl->PushClipRect(min, ImVec2(min.x + std::max(1.f, width), min.y + height), true);
    const float measured = ImGui::CalcTextSize(text.c_str()).x;
    if (measured > width && width > ImGui::CalcTextSize("...").x) {
        const float available = width - ImGui::CalcTextSize("...").x;
        dl->PushClipRect(min, ImVec2(min.x + available, min.y + height), true);
        dl->AddText(min, ink, text.c_str());
        dl->PopClipRect();
        dl->AddText(ImVec2(min.x + available, min.y), ink, "...");
    } else dl->AddText(min, ink, text.c_str());
    dl->PopClipRect();
}

// The Files panel's title, which opens its list menu: the open list's name and
// a chevron, no frame, and the chevron brightening on hover as the headings' does.
bool ListHeading(const char* id, const std::string& label, float room, bool open,
                 const Fonts& fonts, const skin::Skin& design, const skin::Skin& s, float dpi) {
    FontScope font(fonts, design, design.type.heading * SpecFontScale(design), Weight::Semibold);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float side = 16 * dpi, height = s.metric.controlHeight;
    const float labelWidth = std::max(0.f, std::min(ImGui::CalcTextSize(label.c_str()).x, room - side - s.spacing.s1));
    for (const auto colour : {ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive, ImGuiCol_Border, ImGuiCol_BorderShadow})
        ImGui::PushStyleColor(colour, IM_COL32(0, 0, 0, 0));
    const bool clicked = ImGui::Button(id, ImVec2(labelWidth + s.spacing.s1 + side, height));
    ImGui::PopStyleColor(5);
    const bool keyboardFocus = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    const float lit = Ease(ImHashStr("hover", 0, ImGui::GetID(id)), ImGui::IsItemHovered() || keyboardFocus || open ? 1.f : 0.f);
    DrawEllipsis(label, labelWidth, ImVec2(min.x, min.y + (height - ImGui::GetTextLineHeight()) / 2));
    DrawIcon(ImGui::GetWindowDrawList(), Icon::Down, ImVec2(min.x + labelWidth + s.spacing.s1, min.y + (height - side) / 2), side,
             ImGui::GetColorU32(Colour(Mix(s.ink.secondary, s.ink.primary, lit))), dpi);
    return clicked;
}

// A favourite's star, stroked like the icons, and filled once set.
void DrawStar(ImDrawList* draw, ImVec2 min, float side, ImU32 ink, bool filled) {
    ImVec2 points[10];
    const ImVec2 centre(min.x + side / 2, min.y + side * .53f);
    const float outer = side * .46f, inner = outer * .45f;
    for (int i = 0; i < 10; ++i) {
        const float angle = -IM_PI / 2 + i * IM_PI / 5, radius = i % 2 ? inner : outer;
        points[i] = ImVec2(centre.x + std::cos(angle) * radius, centre.y + std::sin(angle) * radius);
    }
    if (filled) draw->AddConcavePolyFilled(points, 10, ink);
    draw->AddPolyline(points, 10, ink, ImDrawFlags_Closed, std::max(1.f, side * 2 / 24));
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

// A rounded rectangle's outline in dashes of `dash`, `gap` apart.
void DashedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, ImU32 ink, float thickness, float dash, float gap) {
    dl->PathRect(min, max, rounding);
    std::vector<ImVec2> points(dl->_Path.begin(), dl->_Path.end());
    dl->PathClear();
    if (points.size() < 2) return;
    points.push_back(points.front());
    bool drawing = true;
    float left = dash;
    dl->PathLineTo(points[0]);
    for (size_t i = 1; i < points.size(); ++i) {
        ImVec2 from = points[i - 1];
        const ImVec2 to = points[i];
        float length = std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y));
        while (length > left) {
            const float t = left / length;
            from = ImVec2(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
            length -= left;
            dl->PathLineTo(from);
            // The end of a dash is stroked; the end of a gap starts the next dash.
            if (drawing) dl->PathStroke(ink, 0, thickness);
            drawing = !drawing;
            left = drawing ? dash : gap;
        }
        left -= length;
        if (drawing) dl->PathLineTo(to);
    }
    if (drawing) dl->PathStroke(ink, 0, thickness);
    else dl->PathClear();
}

bool StatePill(const char* label, bool on, const Fonts& fonts, const skin::Skin& design,
               float dpi, float padding, bool enabled = true, const char* tip = nullptr,
               const char* stateText = nullptr, bool choice = false) {
    auto s = skin::ScaleGeometry(design, dpi);
    // A choice pill (the keyboard's size) has no off: it is always set, green
    // like the others, and its label names what is chosen.
    if (choice) on = true;
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
    // A pill that can't be pressed is flat, with a dashed outline and the tertiary
    // ink, so it doesn't look like an off pill.
    if (enabled) skin::RaisedRect(dl, min, ImVec2(min.x + size.x, min.y + size.y), s.radius.control, s, fill);
    else DashedRect(dl, ImVec2(min.x + .5f * dpi, min.y + .5f * dpi), ImVec2(min.x + size.x - .5f * dpi, min.y + size.y - .5f * dpi),
                    s.radius.control, Colour(s.border.strong), dpi, s.spacing.s1, s.spacing.s1 * .75f);
    dl->AddText(ImVec2(min.x + (size.x - text.x) / 2, min.y + (size.y - text.y) / 2),
                Colour(Mix(enabled ? s.ink.primary : s.ink.tertiary, s.accent.okInk, onT)), label);

    // On state isn't carried by colour alone: an on pill is semibold, an off pill
    // regular.
    if (!choice && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s: %s%s%s", label, stateText ? stateText : on ? "On" : "Off",
                          tip ? "\n" : "", tip ? tip : "");
    return clicked;
}

// The pills shown: all of them, or with `onlyAvailable` (mini) not those that
// need a MIDI input while none is chosen, nor Velocity under MIDI output.
// Sustain, unavailable only while a song plays, always shows.
std::vector<const char*> PillLabels(const EngineSnapshot& state, bool onlyAvailable) {
    const bool input = !state.liveDevice.empty() || !onlyAvailable;
    std::vector<const char*> labels;
    if (input) labels.push_back("Midi2Key");
    if (!state.outputMidi || !onlyAvailable) labels.push_back("Velocity");
    labels.push_back("Sustain");
    labels.push_back(state.eightyEightKeys ? "88 Keys" : "61 Keys");
    if (input) labels.push_back("MidiConnect");
    labels.push_back("AutoVol");
    return labels;
}

// The pills' row at their natural padding, measured as StatePill sizes each
// one: the label in the on weight, plus 2 dp.
float PillsWidth(const Fonts& fonts, const skin::Skin& design, float dpi, const std::vector<const char*>& labels, float pad) {
    FontScope bold(fonts, design, design.type.body * SpecFontScale(design), Weight::Semibold);
    float width = (labels.size() - 1) * ImGui::GetStyle().ItemSpacing.x;
    for (const char* label : labels) width += ImGui::CalcTextSize(label).x + 2 * dpi + 2 * pad;
    return width;
}

// With a row width, the pills' padding stretches or shrinks so the row ends
// exactly there, since text widths don't scale exactly with DPI or theme; 0
// keeps each pill's natural width.
bool StatePills(const Fonts& fonts, const skin::Skin& design, float dpi, ShellEngine& engine, float rowWidth,
                bool onlyAvailable = false) {
    const auto state = engine.Snapshot();
    const auto labels = PillLabels(*state, onlyAvailable);
    const auto shown = [&](std::string_view label) {
        return std::any_of(labels.begin(), labels.end(), [&](const char* each) { return label == each; });
    };
    float pad = 8.f * dpi;
    if (rowWidth > 0)
        pad = std::max(4 * dpi, (rowWidth - PillsWidth(fonts, design, dpi, labels, 0)) / (2 * labels.size()));
    if (shown("Midi2Key")) {
        if (StatePill("Midi2Key", state->liveActive, fonts, design, dpi, pad,
                      !state->liveDevice.empty(), nullptr))
            engine.Send({ShellEngine::Action::LiveActive, {}, 0, 0, !state->liveActive});
        ImGui::SameLine();
    }
    const bool velocityAvailable = !state->outputMidi;
    // Same label in both states, so switching output to MIDI doesn't resize it.
    if (shown("Velocity")) {
        if (StatePill("Velocity", velocityAvailable && state->velocity, fonts, design, dpi, pad, velocityAvailable,
                      nullptr, velocityAvailable ? nullptr : "Unavailable"))
            engine.Send({ShellEngine::Action::Velocity, {}, 0, 0, !state->velocity});
        ImGui::SameLine();
    }
    const bool sustainEnabled = !state->playing && !state->liveActive;
    if (StatePill("Sustain", state->sustain, fonts, design, dpi, pad, sustainEnabled,
                  nullptr))
        engine.Send({ShellEngine::Action::Sustain, {}, 0, 0, !state->sustain});
    ImGui::SameLine();
    if (StatePill(state->eightyEightKeys ? "88 Keys" : "61 Keys", true,
                  fonts, design, dpi, pad, true, nullptr, nullptr, true))
        engine.Send({ShellEngine::Action::EightyEightKeys, {}, 0, 0, !state->eightyEightKeys});
    ImGui::SameLine();
    if (shown("MidiConnect")) {
        if (StatePill("MidiConnect", state->midiConnect, fonts, design, dpi, pad, !state->liveDevice.empty(),
            nullptr))
            engine.Send({ShellEngine::Action::MidiConnect, {}, 0, 0, !state->midiConnect});
        ImGui::SameLine();
    }
    // Like every other pill, the label is just the name; the fill carries state.
    return StatePill("AutoVol", state->autoVolume, fonts, design, dpi, pad, true, nullptr);
}

// muted draws the name greyed, as an input waiting to come back. bare draws the
// name and a chevron with no fill until hovered, as in mini.
bool DevicePill(const std::string& name, float maxWidth, const skin::Skin& s, float dpi, bool muted = false, bool bare = false) {
    const auto min = ImGui::GetCursorScreenPos();
    const float side = 16 * dpi, inset = bare ? s.spacing.s2 : s.spacing.s3;
    const float trail = bare ? s.spacing.s1 + side + s.spacing.s2 : s.spacing.s6 - s.spacing.s3;
    const float width = std::min(maxWidth, ImGui::CalcTextSize(name.c_str()).x + (bare ? inset + trail : 26 * dpi));
    // The button comes first so the fill can lift on hover and press in while held.
    const bool clicked = ImGui::InvisibleButton("##device-pill", ImVec2(width, s.metric.controlHeight));
    const float hot = Ease(ImHashStr("hover", 0, ImGui::GetItemID()), ImGui::IsItemHovered() ? 1.f : 0.f);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + width, min.y + s.metric.controlHeight);
    if (bare) {
        const ImU32 fill = ImGui::IsItemActive() ? ImGui::GetColorU32(ImGuiCol_ButtonActive) : ImGui::GetColorU32(ImGuiCol_ButtonHovered, hot);
        if (ImGui::IsItemActive() || hot > 0) draw->AddRectFilled(min, max, fill, s.radius.control);
        DrawIcon(draw, Icon::Down, ImVec2(max.x - s.spacing.s2 - side, min.y + (s.metric.controlHeight - side) / 2), side, Colour(s.ink.secondary), dpi);
    } else {
        const ImU32 fill = ImGui::IsItemActive() ? Colour(s.surface.recessed) : Colour(Mix(s.surface.card, s.surface.elevatedHot, hot));
        skin::RaisedRect(draw, min, max, s.radius.control, s, fill);
    }
    if (muted) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    DrawEllipsis(name, width - inset - trail, ImVec2(min.x + inset,
        min.y + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    if (muted) ImGui::PopStyleColor();
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

// A song time's x on a seek groove, mapped as the slider maps it (2 px in from each end).
float SeekX(double time, double duration, float left, float width) {
    return left + 2 + static_cast<float>(std::clamp(time / std::max(.001, duration), 0.0, 1.0)) * (width - 4);
}

// The loop section's two handles on a seek groove at `min`. Called before the
// groove is submitted, so a handle takes the pointer where the two overlap.
// `dragging` is the handle held (0 the start, 1 the end), or 2 more once let go
// until the engine has its time, and `at` its time. Returns the handle let go,
// with its time in `at`, or -1.
int SectionHandles(ImVec2 min, float width, float height, const EngineSnapshot& state, float dpi, int& dragging, float& at) {
    // Neither handle passes the other; the engine keeps them a quarter second apart.
    constexpr double shortest = .25;
    if (dragging >= 2 && std::abs((dragging == 3 ? state.loopEnd : state.loopStart) - at) < .01) dragging = -1;
    int released = -1;
    for (int i = 0; i < 2; ++i) {
        const double time = dragging % 2 == i && dragging >= 0 ? at : i ? state.loopEnd : state.loopStart;
        const float side = 12 * dpi;
        ImGui::SetCursorScreenPos(ImVec2(SeekX(time, state.duration, min.x, width) - side / 2, min.y));
        ImGui::InvisibleButton(i ? "##loop-end" : "##loop-start", ImVec2(side, height));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActivated()) { dragging = i; at = static_cast<float>(time); }
        if (dragging != i) continue;
        if (ImGui::IsItemActive()) {
            const double share = std::clamp((ImGui::GetIO().MousePos.x - min.x - 2) / std::max(1.f, width - 4), 0.f, 1.f);
            at = static_cast<float>(i ? std::clamp(share * state.duration, std::min(state.duration, state.loopStart + shortest), state.duration)
                                      : std::clamp(share * state.duration, 0.0, std::max(0.0, state.loopEnd - shortest)));
            ImGui::SetTooltip("%s", Time(at).c_str());
        } else { released = i; dragging = i + 2; }
    }
    return released;
}

// The section drawn over the groove: a tint across the track between two
// upright handles in the accent.
void DrawSection(ImDrawList* draw, ImVec2 min, float width, float height, const EngineSnapshot& state,
                 int dragging, float at, const skin::Skin& s, float dpi) {
    const float start = SeekX(dragging == 0 || dragging == 2 ? at : state.loopStart, state.duration, min.x, width);
    const float end = SeekX(dragging == 1 || dragging == 3 ? at : state.loopEnd, state.duration, min.x, width);
    const float middle = min.y + height / 2;
    draw->AddRectFilled(ImVec2(start, middle - 6 * dpi), ImVec2(end, middle + 6 * dpi),
                        Faded((s.accent.accent & 0xFFFFFFu) | 0x38000000u), 3 * dpi);
    for (const float x : {start, end})
        draw->AddRectFilled(ImVec2(x - 2 * dpi, middle - 9 * dpi), ImVec2(x + 2 * dpi, middle + 9 * dpi), Faded(s.accent.accent), 2 * dpi);
}
}

bool MotionPending() { return g_motion; }
void SettleMotion() { g_settleFrame = ImGui::GetFrameCount(); }

bool Panels::LoopSection(const EngineSnapshot& state) {
    if (loopHandleGeneration_ != state.generation) { loopHandle_ = -1; loopHandleGeneration_ = state.generation; }
    return state.loop == 2 && state.duration > 0 && !state.loaded.empty();
}

std::string Panels::SectionBounds(const EngineSnapshot& state) const {
    const double start = loopHandle_ == 0 || loopHandle_ == 2 ? loopHandleAt_ : state.loopStart;
    const double end = loopHandle_ == 1 || loopHandle_ == 3 ? loopHandleAt_ : state.loopEnd;
    return Time(start) + "\xe2\x80\x93" + Time(end);
}

void Panels::TimeReadout(ImDrawList* draw, const EngineSnapshot& state, bool section, float left, float right, float y,
                         const skin::Skin& s) const {
    const std::string position = Time(seeking_ ? seekPosition_ : state.position);
    std::string time = state.playbackCountdown ? "Starts in " + std::to_string(state.playbackCountdown) + "s" :
        position + " / " + Time(state.duration);
    // A looped section's bounds, following a handle while it is dragged, go before
    // the time; the song's length gives way to them first.
    std::string bounds;
    if (section && !state.playbackCountdown) {
        bounds = SectionBounds(state);
        const float boundsWidth = ImGui::CalcTextSize(bounds.c_str()).x + s.spacing.s3;
        if (right - ImGui::CalcTextSize(time.c_str()).x - boundsWidth >= left) {}
        else if (right - ImGui::CalcTextSize(position.c_str()).x - boundsWidth >= left) time = position;
        else bounds.clear();
    }
    const float timeX = right - ImGui::CalcTextSize(time.c_str()).x;
    draw->AddText(ImVec2(timeX, y), Colour(s.ink.secondary), time.c_str());
    if (!bounds.empty())
        draw->AddText(ImVec2(timeX - s.spacing.s3 - ImGui::CalcTextSize(bounds.c_str()).x, y), Colour(s.accent.accent), bounds.c_str());
}

void Panels::SendLoopHandle(int released, ShellEngine& engine, const EngineSnapshot& state) const {
    if (released >= 0)
        engine.Send({released ? ShellEngine::Action::LoopEnd : ShellEngine::Action::LoopStart, {}, state.generation, 0, false, loopHandleAt_});
}

void Panels::LoadPreferences(const std::filesystem::path& path) {
    // Tracks starts closed on first run and is restored after that. Set before
    // looking for the file, since a first run has none.
    tracksExpanded = false;
    // Read value by value, so one of the wrong kind falls back alone; a file cut
    // short keeps what came before the cut. Themes are loaded from, and later
    // saved beside, this file whether or not it exists yet.
    bool damaged = false;
    const auto json = ReadSaved(path, damaged);
    const auto value = [&](const char* key, auto fallback) { return SavedValue(json, key, fallback, damaged); };
    if (json.is_object()) {
        // Legacy skin index: Blue, Blue Dark, Orange, Orange Dark.
        const int legacy = std::clamp(value("skin", 0), 0, 3);
        preferences.theme = value("theme", std::string(legacy < 2 ? "blue" : "orange"));
        preferences.dark = value("dark", (legacy & 1) != 0);
        preferences.autoSolo = value("autoSoloPiano", false);
        preferences.alwaysOnTop = value("alwaysOnTop", false);
        preferences.blockAltF4 = value("blockAltF4", true);
        preferences.hideFromTaskbar = value("hideFromTaskbar", false);
        preferences.opacity = std::clamp(value("opacity", 100), 40, 100);
        preferences.converterCpu = std::clamp(value("converterCpu", 75), 25, 100);
        convertPlaylist_ = value("convertPlaylist", false);
        preferences.folders = value("folders", true);
        browse_ = value("openFolder", std::string());
        preferences.startMini = value("mini", false);
        miniAutoplay = value("miniAutoplay", false);
        tracksExpanded = value("tracksOpen", false);
        velocityExpanded = value("velocityOpen", false);
        curveTool_ = std::clamp(value("curveTool", 0), 0, 1);
        preferences.windowX = value("windowX", 0);
        preferences.windowY = value("windowY", 0);
        preferences.windowWidth = value("windowWidth", 0);
        // Bounded so an edited file can't overflow the window size; the work area
        // clamps it further.
        preferences.windowExtra = std::clamp(value("windowExtra", 0.f), 0.f, 16384.f);
        preferences.maximized = value("maximized", false);
        preferences.miniX = value("miniX", 0);
        preferences.miniY = value("miniY", 0);
        preferences.miniSaved = value("miniSaved", false);
        const auto folder = value("midiFolder", std::string());
        preferences.folder = std::filesystem::path(std::u8string(folder.begin(), folder.end()));
        const auto song = value("song", std::string());
        preferences.lastSong = std::filesystem::path(std::u8string(song.begin(), song.end()));
        // A settings file without the tour key predates the tour, so it hasn't been seen.
        preferences.tourSeen = value("tourSeen", false);
        preferences.helpBuild = value("helpBuild", std::string());
        preferences.hideFromCapture = value("hideFromCapture", false);
        preferences.checkForUpdates = value("checkForUpdates", true);
    } else { preferences = {}; preferences.tourSeen = false; }
    if (damaged) {
        const auto aside = SetAside(path);
        ReportError("Some settings could not be read.", Utf8(path.filename()) + " could not be read whole" +
                    (aside.empty() ? std::string() : "; kept as " + Utf8(aside.filename())) + ".");
    }
    themesPath_ = path.parent_path() / L"themes.json";
    themes.Load(themesPath_);
}

void Panels::SavePreferences(const std::filesystem::path& path, bool exiting) const {
    if (exiting) SaveThemes();
    nlohmann::json json{{"theme", preferences.theme}, {"dark", preferences.dark}, {"autoSoloPiano", preferences.autoSolo},
                        {"midiFolder", Utf8(preferences.folder)},
                        {"song", Utf8(preferences.lastSong)},
                        {"alwaysOnTop", preferences.alwaysOnTop},
                        {"blockAltF4", preferences.blockAltF4},
                        {"opacity", preferences.opacity}, {"converterCpu", preferences.converterCpu},
                        {"convertPlaylist", convertPlaylist_},
                        {"folders", preferences.folders}, {"openFolder", browse_}, {"mini", miniMode},
                        {"miniAutoplay", miniAutoplay},
                        {"tracksOpen", tracksExpanded}, {"velocityOpen", velocityExpanded},
                        {"curveTool", curveTool_},
                        {"windowExtra", preferences.windowExtra}, {"maximized", preferences.maximized},
                        {"miniX", preferences.miniX}, {"miniY", preferences.miniY}, {"miniSaved", preferences.miniSaved},
                        {"tourSeen", preferences.tourSeen}, {"helpBuild", preferences.helpBuild},
                        {"windowX", preferences.windowX}, {"windowY", preferences.windowY}, {"windowWidth", preferences.windowWidth},
                        {"hideFromCapture", preferences.hideFromCapture}, {"hideFromTaskbar", preferences.hideFromTaskbar},
                        {"checkForUpdates", preferences.checkForUpdates}};
    const auto text = json.dump(2);
    if (text == savedPreferences_) return;
    // Written through to the disk and renamed over, so an interrupted write can't
    // leave a truncated file that the next start reads as no settings.
    if (WriteDurably(path, text + '\n')) { savedPreferences_ = text; return; }
    // A failed write is retried on every call, so it is reported once per change.
    if (text != unsavedPreferences_) ReportError("Could not save the settings.", "Could not write " + Utf8(path.filename()) + ".");
    unsavedPreferences_ = text;
}

// Saves only when there are user themes, or a file to update after the last
// one was deleted.
void Panels::SaveThemes() const {
    if (themesPath_.empty()) return;
    if ((themes.All().size() > 2 || std::filesystem::exists(themesPath_)) && !themes.Save(themesPath_))
        ReportError("Could not save the themes.", "Could not write " + Utf8(themesPath_.filename()) + ".");
}

void Panels::CaptureRing(ImDrawList* draw, const skin::Skin& s, float dpi) const {
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const bool refused = captureRefusedAt >= 0 && ImGui::GetTime() - captureRefusedAt < kRefusedFlash;
    if (refused) draw->AddRectFilled(min, max, Colour((s.accent.warn & 0xFFFFFFu) | 0x38000000u), s.radius.control);
    draw->AddRect(min, max, Colour(refused ? s.accent.warn : s.accent.accent), s.radius.control, 0, 2 * dpi);
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
    ImGui::SetCursorScreenPos(at(width - s.spacing.s2 - s.metric.controlHeight, (title - s.metric.controlHeight) / 2));
    if (IconButton("##close-mapping", Icon::Close, "Close key mapping", s, dpi)) {
        preferences.keyMappingOpen = false; mappingArmed_ = false;
    }
    const float rowY = title + pad;
    // Selected note, an arrow, and its key as a keycap. When armed, the cap is
    // empty inside the accent ring, the standard look for a field awaiting a key.
    if (selectedNote_ >= 0) {
        FontScope font(fonts, design, design.type.meta * SpecFontScale(design));
        const auto found = state->keyMappings.find(NoteName(selectedNote_));
        const std::string key = mappingArmed_ || found == state->keyMappings.end() ? std::string() : ShownKey(found->second);
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
    const float frameY = rowY + s.metric.controlHeight + s.spacing.s3;
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
            ImGui::SetTooltip("%s: %s", NoteName(hoveredNote).c_str(), found == state->keyMappings.end() ? "Unassigned" : ShownKey(found->second).c_str());
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
            const std::string label = ShownKey(found->second);
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
            // The key is kept by its position, which the layout's character names.
            if (!io.KeyCtrl) for (ImWchar c : io.InputQueueCharacters)
                if (c > 32 && c != 127 && !(key = StoredKey(static_cast<wchar_t>(c))).empty()) break;
            if (!key.empty()) {
                engine.Send({ShellEngine::Action::Remap, {}, 0, static_cast<size_t>(selectedNote_), false, 0, key});
                mappingArmed_ = false;
            }
        }
    } else if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) mappingArmed_ = false;
    const float statusY = frameY + 114 * dpi + pad;
    draw->AddRectFilled(at(0, statusY), at(width, height * dpi), Colour(s.surface.structure), corner, ImDrawFlags_RoundCornersBottom);
    draw->AddLine(at(0, statusY), at(width, statusY), Colour(s.border.hairline), dpi);
    ImGui::SetCursorScreenPos(at(pad, statusY + s.spacing.s2));
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
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s.spacing.s4, s.spacing.s4));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
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
            if (const int variant = Segments("##variant", {"Light", "Dark"}, preferences.dark ? 1 : 0, s, dpi); variant >= 0)
                preferences.dark = variant == 1;
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
        ImGui::SameLine(0, s.spacing.s4);
        rebuilt |= ThemeSwatch("Text##start", text, false, s, dpi);
        ImGui::SameLine(0, s.spacing.s4);
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s.spacing.s4, s.spacing.s4));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
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
        ImGui::SetNextItemWidth(-s.metric.controlHeight - s.spacing.s2);
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
    // Mini sizes to its content: across, the pills it shows plus window padding,
    // or the top strip's floor when they need less; down, the 88 dp strip and rows
    // 8 dp apart, with no status bar. Autoplay adds a row and the up-next line,
    // and a performer another row. Each height leaves 8 dp below the last row.
    if (miniMode) {
        const int rows = miniAutoplay ? (performerRow_ ? 5 : 4) : 3;
        const float floor = 416 + 2 * grow.window + 34 * std::max(0.f, grow.text);
        return ImVec2(miniPillsWidth_ > 0 ? std::max(floor, std::ceil(miniPillsWidth_)) : 528 + 2 * grow.window + 40 * std::max(0.f, grow.text),
                      (miniAutoplay ? (performerRow_ ? 262.f : 222.f) + std::max(0.f, grow.meta) : 136.f) + rows * grow.control);
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
    case MidiBackend::NamedPort: return "QuartzMIDI port";
    default: return "WinRT";
    }
}
bool InputWaiting(const EngineSnapshot& state) { return state.liveDevice.empty() && !state.liveWaiting.empty(); }
std::string DeviceName(const EngineSnapshot& state) {
    if (state.liveDevice.empty() && !state.liveWaiting.empty())
        return state.liveWaitingName.empty() ? "MIDI input" : state.liveWaitingName;
    if (state.liveDevice.empty()) return "No MIDI input";
    for (const auto& device : state.devices) if (device.id == state.liveDevice) return device.name;
    return "Connected MIDI input";
}
std::string OutputDeviceName(const EngineSnapshot& state) {
    if (state.outputDevice.empty() && !state.outputWaiting.empty())
        return state.outputWaitingName.empty() ? "MIDI output" : state.outputWaitingName;
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
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
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + control + s.spacing.s3));
    // A modified curve shows a filled Save button. It saves into the user's own
    // curve; a built-in can't be overwritten, so saving one prompts for a name.
    const bool changed = !state->comparingCurve && (!editor_.anchors.empty() || editor_.sensitivity != 0 || editor_.contrast != 0);
    const float saveWidth = changed ? ImGui::CalcTextSize("Save").x + 2 * s.spacing.s4 + s.spacing.s2 : 0.f;
    ImGui::AlignTextToFramePadding();
    Ellipsis(state->ActiveVelocityName(), width - 4 * control - 32 * dpi - saveWidth);
    if (changed) {
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - 4 * control - s.spacing.s6 - saveWidth, start.y + control + s.spacing.s3));
        if (TransportButton("##save-curve", "Save", s, dpi, true)) {
            if (state->curve.preset < midi::kBuiltinVelocityCurves) openName(3);
            else engine.Send({ShellEngine::Action::CurveRename, {}, 0, 0, false, 0, state->curves[state->curve.preset].name});
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x + width - 4 * control - s.spacing.s6, start.y + control + s.spacing.s3));
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
    float workspaceY = start.y + 2 * control + s.spacing.s6;
    if (nameOperation_) {
        ImGui::SetCursorScreenPos(ImVec2(start.x, workspaceY));
        ImGui::SetNextItemWidth(width - 2 * control - s.spacing.s4);
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
        workspaceY += control + s.spacing.s3;
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
                mainWidth = width - presetsWidth - s.spacing.s3;
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
    const float macroY = graphMax.y + s.spacing.s3, macroWidth = (mainWidth - s.spacing.s3) / 2;
    ImGui::BeginDisabled(state->comparingCurve);
    for (int i = 0; i < 2; ++i) {
        const float x = start.x + i * (macroWidth + s.spacing.s3);
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
    const ImVec2 listMin(start.x + mainWidth + s.spacing.s3, workspaceY);
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
    ImGui::SetCursorScreenPos(ImVec2(start.x, mainBottom + s.spacing.s1)); ImGui::Dummy(ImVec2(width, 1));
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    const auto groups = GroupDevices(state->devices);
    const auto* selectedGroup = SelectedGroup(groups, state->liveDevice);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - s.metric.controlHeight - s.spacing.s2);
    // An input waiting to come back shows greyed.
    const bool inputWaiting = InputWaiting(*state);
    const bool deviceOpen = ChevronCombo("##midi-input", DeviceName(*state).c_str(), inputWaiting);
    if (deviceOpen) {
        if (ImGui::Selectable("No MIDI input", state->liveDevice.empty() && !inputWaiting)) engine.Send({ShellEngine::Action::LiveOpen});
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
    if (const int target = Segments("##output-target", {"Keystrokes", "MIDI"}, state->outputMidi ? 1 : 0, s, dpi); target >= 0)
        engine.Send({ShellEngine::Action::OutputTarget, {}, 0, 0, target == 1});

    const auto outputGroups = GroupDevices(state->outputDevices);
    const auto* selectedOutputGroup = SelectedGroup(outputGroups, state->outputDevice);
    ImGui::BeginDisabled(!state->outputMidi);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - s.metric.controlHeight - s.spacing.s2);
    // An output waiting to come back shows greyed.
    const bool outputWaiting = state->outputDevice.empty() && !state->outputWaiting.empty();
    const bool outputOpen = ChevronCombo("##midi-output", OutputDeviceName(*state).c_str(), outputWaiting);
    if (outputOpen) {
        if (ImGui::Selectable("No MIDI output", state->outputDevice.empty() && !outputWaiting))
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
    if (BackendForOutputId(state->outputDevice) == MidiBackend::NamedPort && selectedOutputGroup) {
        // The port has one transport; its name is what other apps list.
        ImGui::BeginDisabled(!state->outputMidi);
        ImGui::TextUnformatted("Port name");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##output-port-name", portName_, sizeof(portName_));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ShellEngine::Command command{ShellEngine::Action::OutputPortName};
            command.key = portName_; engine.Send(std::move(command));
        }
        if (!ImGui::IsItemActive() && state->outputPortName != portName_)
            snprintf(portName_, sizeof(portName_), "%s", state->outputPortName.c_str());
        ImGui::EndDisabled();
    } else if (selectedOutputGroup) {
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
        const std::array<float, 4> current{
            static_cast<float>(state->wootingTriggerThreshold),
            static_cast<float>(state->wootingShiftAmount),
            static_cast<float>(state->wootingVelocitySensitivity),
            static_cast<float>(state->wootingMinVelocity)};
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
        setting(2, "Velocity sensitivity", "##wooting-velocity", .1f, 10.f, .1f, 1.f,
                ShellEngine::Action::WootingVelocitySensitivity, "%.1f");
        setting(3, "Minimum velocity", "##wooting-min-velocity", 1.f, 126.f, 1.f, 1.f,
                ShellEngine::Action::WootingMinVelocity, "%.0f");
        // Pedal keys, laid out as the hotkeys in Settings. The key is learnt
        // from the Wooting, so the cap listens to it and not to typing.
        const char* pedals[3]{"Sustain pedal", "Sostenuto pedal", "Soft pedal"};
        const float height = s.metric.controlHeight, gap = s.spacing.s2, capWidth = 112 * dpi;
        auto* draw = ImGui::GetWindowDrawList();
        for (int i = 0; i < 3; ++i) {
            ImGui::PushID(i);
            const auto min = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const bool armed = wootingPedalCapture == i, bound = state->wootingPedalKeys[i] != 0;
            DrawEllipsis(pedals[i], width - capWidth - height - 2 * gap,
                         ImVec2(min.x, min.y + (height - ImGui::GetTextLineHeight()) / 2));
            ImGui::SetCursorScreenPos(ImVec2(min.x + width - height - gap - capWidth, min.y));
            const std::string cap = armed ? std::string() : ScancodeLabel(state->wootingPedalKeys[i]);
            if (EasedButton("##pedal-cap", ImVec2(capWidth, height))) wootingPedalCapture = armed ? -1 : i;
            if (!cap.empty()) {
                const ImVec2 capMin = ImGui::GetItemRectMin(), size = ImGui::CalcTextSize(cap.c_str(), nullptr, false);
                draw->AddText(ImVec2(capMin.x + (capWidth - size.x) / 2, capMin.y + (height - size.y) / 2),
                              ImGui::GetColorU32(ImGuiCol_Text), cap.c_str());
            }
            if (armed) draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Colour(s.accent.accent), s.radius.control, 0, 2 * dpi);
            ImGui::SameLine(0, gap);
            if (!bound) ImGui::Dummy(ImVec2(height, height));
            else if (IconButton("##pedal-unbind", Icon::Close, "Unbind", s, dpi)) {
                ShellEngine::Command command{ShellEngine::Action::WootingPedalKey};
                command.track = static_cast<size_t>(i); command.amount = 0;
                engine.Send(std::move(command));
                wootingPedalCapture = -1;
            }
            ImGui::PopID();
        }
        ImGui::Separator();
    } else {
        wootingEditing_.fill(false);
        wootingPending_.fill(false);
        wootingPedalCapture = -1;
    }
    ImGui::TextUnformatted("MIDI channel");
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    section("Playback");
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
    bool shuffle = state->shuffle;
    if (SettingSwitch("Shuffle play", shuffle, nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::Shuffle, {}, 0, 0, shuffle});
    bool holdBehind = state->holdBehind;
    ImGui::BeginDisabled(state->outputMidi);
    if (SettingSwitch("Pause when game loses focus", holdBehind, nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::HoldBehind, {}, 0, 0, holdBehind});
    ImGui::EndDisabled();
    if (SettingSwitch("Solo piano tracks on load", preferences.autoSolo, nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::AutoSolo, {}, 0, 0, preferences.autoSolo});
    if (revealSettingsSwitches) ImGui::SetScrollHereY(0.f);
    bool detectDrums = state->detectDrums;
    if (SettingSwitch("Detect drum tracks", detectDrums, nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::DetectDrums, {}, 0, 0, detectDrums});
    if (const int chosen = SettingSegments("Transpose on load", "##transpose-on-load", {"Off", "Best key"},
                                           state->autoTranspose ? 1 : 0, s, dpi); chosen >= 0)
        engine.Send({ShellEngine::Action::AutoTranspose, {}, 0, 0, chosen == 1});
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
            if (!performer.presetName.empty()) { ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(performer.presetName.c_str()); }
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
    ImGui::Separator();
    section("Keyboard");
    bool outRange = state->outRange;
    if (SettingSwitch("Fold out-of-range notes onto the keys", outRange,
        nullptr, fonts, design, dpi))
        engine.Send({ShellEngine::Action::OutRange, {}, 0, 0, outRange});
    if (const int octaves = SettingSegments("Octaves", "##octaves", {"1", "2", "3", "4", "5"}, state->octaves - 1, s, dpi); octaves >= 0)
        engine.Send({ShellEngine::Action::Octaves, {}, 0, 0, false, static_cast<double>(octaves + 1)});
    ImGui::Separator();
    section("Velocity");
    // The modifier only matters while velocity is on.
    const int modifier = state->velocityModifier == "ctrl" ? 1 : state->velocityModifier == "shift" ? 2 : 0;
    ImGui::BeginDisabled(!state->velocity || state->outputMidi);
    if (const int chosen = SettingSegments("Modifier key", "##velocity-modifier", {"Alt", "Ctrl", "Shift"}, modifier, s, dpi); chosen >= 0) {
        ShellEngine::Command command{ShellEngine::Action::VelocityModifier};
        command.key = chosen == 1 ? "ctrl" : chosen == 2 ? "shift" : "alt";
        engine.Send(std::move(command));
    }
    ImGui::EndDisabled();
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
    ImGui::Separator();
    section("Window");
    SettingSwitch("Always on top", preferences.alwaysOnTop, nullptr, fonts, design, dpi);
    {
        // The show/hide key is the way back to a window with no taskbar button, so
        // the switch works only while that key is bound and registered. Clicked
        // greyed, it arms that key's capture, and the key taken turns it on.
        const bool canHide = !state->hotkeys[kShowHideHotkey].empty() && transportKeysAvailable[kShowHideHotkey];
        if (hideOnceShowHideWorks && canHide && hotkeyCapture < 0) { preferences.hideFromTaskbar = true; hideOnceShowHideWorks = false; }
        bool hidden = preferences.hideFromTaskbar && canHide;
        ImGui::BeginDisabled(!canHide);
        if (SettingSwitch("Hide from taskbar and Alt+Tab", hidden, nullptr, fonts, design, dpi)) preferences.hideFromTaskbar = hidden;
        ImGui::EndDisabled();
        if (!canHide && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            hotkeyCapture = static_cast<int>(kShowHideHotkey);
            hideOnShowHideKey = revealShowHideKey_ = true;
        }
    }
    if (captureExclusionOffered) SettingSwitch("Hide from screen capture", preferences.hideFromCapture, nullptr, fonts, design, dpi);
    ImGui::Separator();
    section("Hotkeys");
    if (revealSettingsHotkeys) ImGui::SetScrollHereY(0.f);
    SettingSwitch("Block Alt+F4 while playing", preferences.blockAltF4, nullptr, fonts, design, dpi);
    {
        // Action, its key as a keycap, and a bare cross to unbind it. When armed, the
        // cap says "Press a key" inside the accent ring. A key held by another program
        // has the legend's dead-key ink and a warning mark before it.
        const char* actions[kAppHotkeys]{"Play/Pause", "Skip back", "Skip forward", "Stop", "Previous song", "Next song"};
        const char* laterActions[kLaterHotkeyFields.size()]{"Show/hide window"};
        const float height = s.metric.controlHeight, gap = s.spacing.s2, capWidth = 112 * dpi, mark = 14 * dpi;
        auto* draw = ImGui::GetWindowDrawList();
        // A cap's text, centred on its face, with the warning mark before it when the key is taken.
        const auto capText = [&](ImVec2 faceMin, ImVec2 faceMax, const std::string& text, bool dead, bool placeholder) {
            const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false);
            const float lead = dead ? mark + s.spacing.s1 : 0, middle = (faceMin.y + faceMax.y) / 2;
            const float x = faceMin.x + (faceMax.x - faceMin.x - size.x - lead) / 2;
            if (dead)
                DrawIcon(draw, Icon::Warning, ImVec2(x, middle - mark / 2), mark,
                         ImGui::GetColorU32(Colour(s.accent.warn)), dpi);
            draw->AddText(ImVec2(x + lead, middle - size.y / 2),
                          ImGui::GetColorU32(Colour(dead ? s.ink.tertiary : placeholder ? s.ink.secondary : s.ink.primary)),
                          text.c_str());
        };
        // Returns 1 when the cap is clicked and 2 when the unbind cross is.
        const auto keyRow = [&](const std::string& action, const std::string& key, bool armed, bool available) {
            int clicked = 0;
            const auto min = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const bool bound = !key.empty();
            DrawEllipsis(action, width - capWidth - height - 2 * gap,
                         ImVec2(min.x, min.y + (height - ImGui::GetTextLineHeight()) / 2));
            ImGui::SetCursorScreenPos(ImVec2(min.x + width - height - gap - capWidth, min.y));
            const bool dead = bound && !armed && !available;
            // Draw the key name separately rather than as the button label: a key named
            // "#" would make the label "###cap", which ImGui treats as empty.
            if (EasedButton("##cap", ImVec2(capWidth, height))) clicked = 1;
            const ImVec2 faceMin = ImGui::GetItemRectMin(), faceMax = ImGui::GetItemRectMax();
            if (armed) capText(faceMin, faceMax, "Press a key", false, true);
            else if (bound) capText(faceMin, faceMax, HotkeyLabel(key), dead, false);
            if (armed) CaptureRing(draw, s, dpi);
            ImGui::SameLine(0, gap);
            // The cross keeps its place on every row, greyed while there is nothing to unbind.
            ImGui::BeginDisabled(!bound);
            { BareButtons quiet;
              if (IconButton("##unbind", Icon::Close, "Unbind", s, dpi)) clicked = 2; }
            ImGui::EndDisabled();
            return clicked;
        };
        const auto hotkeyRow = [&](size_t i, const std::string& action) {
            ImGui::PushID(static_cast<int>(i));
            const bool armed = hotkeyCapture == static_cast<int>(i);
            const int clicked = keyRow(action, state->hotkeys[i], armed, transportKeysAvailable[i]);
            if (clicked == 1) hotkeyCapture = armed ? -1 : static_cast<int>(i);
            if (clicked == 2) {
                ShellEngine::Command command{ShellEngine::Action::Hotkey};
                command.track = i; engine.Send(std::move(command));
                hotkeyCapture = -1;
            }
            ImGui::PopID();
        };
        for (size_t i = 0; i < kAppHotkeys; ++i) hotkeyRow(i, actions[i]);
        for (size_t i = 0; i < kLaterHotkeyFields.size(); ++i) {
            hotkeyRow(kFirstLaterHotkey + i, laterActions[i]);
            if (kFirstLaterHotkey + i == kShowHideHotkey && revealShowHideKey_) { ImGui::SetScrollHereY(1.f); revealShowHideKey_ = false; }
        }
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
            const float cross = 12 * dpi, capPad = s.spacing.s3, chipGap = s.spacing.s2;
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
                const bool dead = !armed && !transportKeysAvailable[i];
                const std::string name = armed ? std::string("Press a key") : HotkeyLabel(state->hotkeys[i]);
                const float nameWidth = ImGui::CalcTextSize(name.c_str()).x + (dead ? mark + s.spacing.s1 : 0);
                const float width = armed ? 2 * capPad + nameWidth : 2 * capPad + nameWidth + s.spacing.s1 + cross;
                place(width);
                const ImVec2 min = ImGui::GetCursorScreenPos();
                const bool pressed = EasedButton("##cap", ImVec2(width, height));
                const ImVec2 faceMin = ImGui::GetItemRectMin(), faceMax = ImGui::GetItemRectMax();
                const bool overCross = !armed && ImGui::IsItemHovered() && ImGui::GetMousePos().x >= min.x + width - capPad - cross - s.spacing.s1;
                if (armed) {
                    capText(faceMin, faceMax, name, false, true);
                    CaptureRing(draw, s, dpi);
                } else {
                    const float middle = (faceMin.y + faceMax.y) / 2;
                    float textX = min.x + capPad;
                    if (dead) {
                        DrawIcon(draw, Icon::Warning, ImVec2(textX, middle - mark / 2), mark,
                                 ImGui::GetColorU32(Colour(s.accent.warn)), dpi);
                        textX += mark + s.spacing.s1;
                    }
                    draw->AddText(ImVec2(textX, middle - ImGui::GetTextLineHeight() / 2),
                                  ImGui::GetColorU32(Colour(dead ? s.ink.tertiary : s.ink.primary)), name.c_str());
                    DrawIcon(draw, Icon::Close, ImVec2(min.x + width - capPad - cross, middle - cross / 2), cross,
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
        // The performer's action keys, which work whatever the trigger.
        if (state->performer.on)
            for (const auto& action : state->performer.actions) hotkeyRow(action.key, action.name);
    }
    ImGui::Separator();
    section("Appearance");
    SettingSlider("Window opacity", "##window-opacity", &preferences.opacity, 40, 100, "%d%%", s, dpi, nullptr, 100);
    // All themes by name, built-ins first. Customise opens the editor on the chosen
    // theme, or on a copy of a built-in.
    {
        const Theme& active = themes.Active(preferences.theme);
        const bool custom = !active.builtin;
        const float button = ImGui::CalcTextSize("Customise").x + s.spacing.s6, icon = s.metric.controlHeight, gap = s.spacing.s2;
        ImGui::TextUnformatted("Theme");
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
    ImGui::Separator();
    section("About");
    SettingSwitch("Check for updates", preferences.checkForUpdates, nullptr, fonts, design, dpi);
    {
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
                             ShellEngine& engine, ImVec2 popupPosition, float popupMaxHeight, bool bare) {
    const auto s = skin::ScaleGeometry(design, dpi);
    const float popupWidth = PopoverWidth(design) * dpi;
    // Show the button as active while its popover is open.
    { std::optional<BareButtons> quiet;
      if (bare) quiet.emplace();
      if (IconButton("##settings", Icon::Settings, "Settings", s, dpi, ImGui::IsPopupOpen("Settings")))
          ImGui::OpenPopup("Settings"); }

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
    } else wootingPedalCapture = -1;
    // Set right before the popup they apply to: a closed BeginPopup discards them,
    // so with the device popup in between, Settings would open unplaced and full height.
    ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0), ImVec2(popupWidth, popupMaxHeight));
    ImGui::SetNextWindowPos(popupPosition);
    if (ImGui::BeginPopup("Settings")) {
        DrawSettings(fonts, design, dpi, engine);
        ImGui::EndPopup();
    } else {
        hotkeyCapture = -1;
        hideOnceShowHideWorks = false;
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
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
    for (const char* label : {"Convert", "Cancel", "Close", "Sign in", "Sign in again", "Install"})
        actionWidth = std::max(actionWidth, ImGui::CalcTextSize(label).x + 2 * s.spacing.s3);
    const float rowStart = ImGui::GetCursorPosX();
    // Progress: a bar while running, filled to the step's percentage when it
    // gives one and moving back and forth otherwise, then the converter's last
    // line, styled as an error when the run failed.
    const auto progress = [&] {
        if (busy && !state->signingIn) {
            convertBarDrawn_ = true;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float height = 4 * dpi;
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), Colour(s.surface.recessed), height / 2);
            if (const int percent = audio_to_midi::StepPercent(state->conversionStatus); percent >= 0) {
                if (percent > 0)
                    draw->AddRectFilled(min, ImVec2(min.x + width * percent / 100.f, min.y + height), Colour(s.accent.accent), height / 2);
            } else {
                const float span = width * .3f;
                const float travel = static_cast<float>(std::fmod(ImGui::GetTime() * .6, 1.0)) * (width + span) - span;
                draw->AddRectFilled(ImVec2(min.x + std::max(0.f, travel), min.y),
                                    ImVec2(min.x + std::min(width, travel + span), min.y + height), Colour(s.accent.accent), height / 2);
            }
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
    // Add-on present but not installed: show the install row on the same grid;
    // the popover becomes the converter once it's installed. One install runs
    // on any DirectX 12 GPU, and on the CPU without one.
    if (!state->converterInstalled && (state->converterCanSetUp || state->settingUp)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Converter, 610 MB");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetCursorPosX(rowStart + width - actionWidth);
        if (EasedButton(busy ? "Cancel" : "Install", ImVec2(actionWidth, s.metric.controlHeight)))
            engine.Send({busy ? ShellEngine::Action::ConvertCancel : ShellEngine::Action::ConverterSetUp});
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
        const float segmentsWidth = SegmentsWidth({"25%", "50%", "75%", "100%"}, s);
        ImGui::SetCursorPosX(rowStart + width - segmentsWidth);
        if (const int picked = Segments("##converter-cpu", {"25%", "50%", "75%", "100%"}, chosen, s, dpi); picked >= 0)
            preferences.converterCpu = kShares[picked];
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
    const bool failed = !panelError_.empty() || !state.error.empty();
    if (!panelError_.empty()) fields.push_back(panelError_);
    else if (!state.error.empty()) fields.push_back(state.error);
    else if (state.busy) fields.push_back("Loading...");
    else {
        // The sheet export result comes first while there is one.
        if (!sheetNote_.empty()) fields.push_back(sheetNote_);
        fields.push_back(state.playing ? "Playing" : state.midiConnect ? "MidiConnect" : state.liveActive ? "Live" : "Ready");
        // A conversion outlives its popover, so the status bar reports it.
        if (state.converting) fields.push_back(state.settingUp ? "Installing the converter"
                                                                : state.signingIn ? "Signing in to YouTube" : "Converting audio");
        // The curve, the input and the keyboard size show in their own controls.
        fields.push_back(state.outputMidi ? "Output MIDI" : "Output Keystrokes");
    }
    // With no song open there are no tracks to count.
    const auto tracks = state.rows.empty() ? std::string() :
        std::to_string(SilentTracks(state.rows)) + " of " + std::to_string(state.rows.size()) + " tracks silent";
    // Sized from the label so the caller's frame padding can't clip "Log".
    const float logWidth = ImGui::CalcTextSize("Log").x + s.spacing.s6;
    const float suffix = logWidth + ImGui::CalcTextSize(tracks.c_str()).x + s.spacing.s6;
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
        const float gap = x > text.x ? s.spacing.s6 : 0.f;
        const float fieldWidth = ImGui::CalcTextSize(field.c_str()).x;
        if (fieldWidth > end - (x + gap) && end - (x + gap) <= ImGui::CalcTextSize("...").x) { x = end; continue; }
        if (gap > 0)
            draw->AddLine(ImVec2(x + s.spacing.s3, text.y + 2 * dpi),
                          ImVec2(x + s.spacing.s3, text.y + ImGui::GetTextLineHeight() - 2 * dpi), Colour(s.border.hairline), dpi);
        x += gap;
        // A failure is the first field, in the warning colour.
        const bool warn = failed && &field == &fields.front();
        if (warn) ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.accent.warn));
        DrawEllipsis(field, end - x, ImVec2(x, text.y));
        if (warn) ImGui::PopStyleColor();
        x += fieldWidth;
    }
    draw->AddText(ImVec2(min.x + width - s.spacing.windowPad - logWidth - s.spacing.s2 - ImGui::CalcTextSize(tracks.c_str()).x, text.y), Colour(s.ink.secondary), tracks.c_str());
    draw->PopClipRect();
    ImGui::SetCursorScreenPos(text);
    ImGui::InvisibleButton("##status", ImVec2(width - 2 * s.spacing.windowPad - logWidth, ImGui::GetTextLineHeight()));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", summary.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::SetCursorScreenPos(ImVec2(min.x + width - s.spacing.windowPad - logWidth, min.y + 2 * dpi));
    // Styled as an active IconButton while the log is open, easing like one. Under
    // the log window that styling would only show at its corner.
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
    ImGui::SetNextWindowSize(ImVec2(std::min(900 * dpi, limit.x), std::min(480 * dpi, limit.y)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x / 2,
                                  viewport->WorkPos.y + viewport->WorkSize.y / 2), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    // Dragged outside the main window, the log becomes an OS window of its own and
    // needs TopMost while the window is on top, or it opens behind the main window.
    // No NoAutoMerge, so it still opens inside the main window.
    ImGuiWindowClass logClass;
    logClass.ViewportFlagsOverrideSet = Topmost() ? ImGuiViewportFlags_TopMost : 0;
    ImGui::SetNextWindowClass(&logClass);
    // No collapse arrow: ImGui's title-bar triangle isn't from the icon set, and
    // the log has no use for a collapsed state. No docking: dropped on the app, it
    // would dock into the whole shell.
    if (ImGui::Begin("Log", &logOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        const auto state = engine.Snapshot();
        if (IconButton("##clear-log", Icon::Clear, "Clear Log", s, dpi)) engine.Send({ShellEngine::Action::ClearLog});
        ImGui::SameLine();
        if (IconButton("##copy-log", Icon::Copy, "Copy Log", s, dpi)) CopyUtf8ToClipboard(hwnd, *state->log);
        // Lines wrap to the panel: a long line (a Python warning, a path) would
        // otherwise run off the right edge.
        ImGui::BeginChild("##log-output", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4 * dpi;
        const std::string& text = *state->log;
        const float width = std::max(ImGui::GetContentRegionAvail().x, 40 * dpi), size = ImGui::GetFontSize();
        // A wrapped line's later rows are indented, so they don't read as new lines.
        const float indent = 2 * size;
        const auto continues = [&](size_t row) { return row > 0 && text[row - 1] != '\n'; };
        if (logWrappedText != state->log || logWrappedWidth != width || logWrappedFont != size) {
            logWrappedText = state->log; logWrappedWidth = width; logWrappedFont = size;
            logRows.clear();
            const char* const begin = text.data();
            for (size_t start = 0; start < text.size();) {
                size_t end = text.find('\n', start);
                const size_t next = end == std::string::npos ? text.size() : end + 1;
                if (end == std::string::npos) end = text.size();
                if (end > start && text[end - 1] == '\r') --end;
                const char* p = begin + start;
                const char* const lineEnd = begin + end;
                if (p == lineEnd) logRows.emplace_back(start, start);
                while (p < lineEnd) {
                    const char* cut = ImGui::GetFont()->CalcWordWrapPosition(size, p, lineEnd,
                                                                             p == begin + start ? width : width - indent);
                    if (cut <= p) { // one glyph wider than the panel still takes a row
                        cut = p + 1;
                        while (cut < lineEnd && (static_cast<unsigned char>(*cut) & 0xc0) == 0x80) ++cut;
                    }
                    logRows.emplace_back(p - begin, cut - begin);
                    p = cut;
                    while (p < lineEnd && *p == ' ') ++p;
                }
                start = next;
            }
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(logRows.size()), ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                if (continues(logRows[i].first)) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                ImGui::TextUnformatted(text.data() + logRows[i].first, text.data() + logRows[i].second);
            }
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
    Icon icons[kHotkeys]{Icon::Play, Icon::Back, Icon::Forward, Icon::Stop, Icon::Left, Icon::Right};
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (control - ImGui::GetTextLineHeight()) / 2));
    // Two rows (device pill, then state pills) with equal 8 dp gaps above, between
    // and below, as elsewhere in mini.
    const float gap = s.spacing.s2, stripPad = gap;
    const float strip = 3 * stripPad + 2 * control;
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + strip), Colour(s.surface.structure));
    draw->AddLine(ImVec2(origin.x, origin.y + strip), ImVec2(origin.x + size.x, origin.y + strip), Colour(s.border.hairline));
    // A bare icon at an end of a row hangs its empty frame past the window's
    // padding, so the glyph, not the frame, lines up with the boxes above and below.
    const float hang = std::floor((control - 16 * dpi) / 2);
    // Three bare utility slots, a step apart.
    const float utilityX = origin.x + size.x - pad + hang - 3 * control - 2 * s.spacing.s1;
    const float segmentX = utilityX - 124 * dpi - s.spacing.s3;
    // The device name is bare too, its text on the window's edge and its hover
    // fill a step out from it.
    const float deviceX = origin.x + pad - s.spacing.s2;
    ImGui::SetCursorScreenPos(ImVec2(deviceX, origin.y + stripPad));
    // The same devices menu in both modes; Live names the keyboard played, and
    // Autoplay where its notes go.
    { FontScope deviceFont(fonts, design, design.type.body * SpecFontScale(design), Weight::Medium);
      const std::string device = !miniAutoplay ? DeviceName(*state) : state->outputMidi ? OutputDeviceName(*state) : "Keystrokes";
      const bool waiting = !miniAutoplay ? InputWaiting(*state) : state->outputMidi && state->outputDevice.empty() && !state->outputWaiting.empty();
      if (DevicePill(device, std::max(40 * dpi, segmentX - s.spacing.s3 - deviceX), s, dpi, waiting, true)) ImGui::OpenPopup("MIDI devices"); }
    ImGui::SetCursorScreenPos(ImVec2(segmentX, origin.y + stripPad));
    {
        FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
        const auto well = ImGui::GetCursorScreenPos();
        const float segmentWidth = 124 * dpi;
        skin::RecessedRect(draw, well, ImVec2(well.x + segmentWidth, well.y + control), s.radius.control, s);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.radius.element);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        // One raised thumb slides between the modes under transparent buttons, and
        // is outlined after them; the other mode is a bare word, as in Segments.
        const float at = Ease(ImGui::GetID("##mini-mode-thumb"), miniAutoplay ? 1.f : 0.f);
        const ImVec2 thumbMin(well.x + 4 * dpi + at * 60 * dpi, well.y + 4 * dpi);
        const ImVec2 thumbMax(thumbMin.x + 56 * dpi, thumbMin.y + (control - 8 * dpi));
        skin::RaisedRect(draw, thumbMin, thumbMax, s.radius.element, s, Colour(s.surface.elevated));
        for (int mode = 0; mode < 2; ++mode) {
            ImGui::SetCursorScreenPos(ImVec2(well.x + 4 * dpi + mode * 60 * dpi, well.y + 4 * dpi));
            const float under = 1.f - std::abs(at - mode);
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, Colour(Mix(s.ink.secondary, s.ink.primary, under)));
            if (EasedButton(mode ? "Auto" : "Live", ImVec2(56 * dpi, control - 8 * dpi))) miniAutoplay = mode == 1;
            ImGui::PopStyleColor(3);
        }
        draw->AddRect(thumbMin, thumbMax, Colour(s.accent.accent), s.radius.element, 0, dpi);
        ImGui::PopStyleVar(2);
        ImGui::SetCursorScreenPos(ImVec2(well.x + segmentWidth + s.spacing.s2, well.y));
    }
    ImGui::SetCursorScreenPos(ImVec2(utilityX, origin.y + stripPad));
    { BareButtons quiet;
      if (IconButton("##restore-full", Icon::Expand, "Full window", s, dpi)) miniMode = false;
      ImGui::SameLine(0, s.spacing.s1);
      ImGui::BeginDisabled(!themes.Active(preferences.theme).paired);
      if (IconButton("##mini-theme", s.dark ? Icon::Moon : Icon::Sun, s.dark ? "Switch to light" : "Switch to dark", s, dpi)) preferences.dark = !preferences.dark;
      ImGui::EndDisabled(); }
    ImGui::SameLine(0, s.spacing.s1);
    SettingsControl(fonts, design, dpi, engine,
                    ImVec2(origin.x + size.x - PopoverWidth(design) * dpi - s.spacing.windowPad, origin.y + stripPad + control + s.spacing.s1), 544 * dpi, true);
    // Mini leaves out the pills that can't be pressed for a while and narrows to
    // fit the rest; they always span the row, so it ends on the window's edges.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, origin.y + 2 * stripPad + control));
    const float pillsWidth = PillsWidth(fonts, design, dpi, PillLabels(*state, true), 8 * dpi);
    miniPillsWidth_ = (pillsWidth + 2 * pad) / dpi;
    if (StatePills(fonts, design, dpi, engine, size.x - 2 * pad, true)) autoVolumeOpen = true;
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
        // The groove gives up width before the curve's name is cut.
        const float free = size.x - 2 * pad - labels - 48 * dpi - 4 * spacing;
        const float grooveWidth = std::clamp(free - 150 * dpi, 72 * dpi, 132 * dpi);
        label("Curve");
        CurveCombo("##mini-curve", free - grooveWidth, *state, engine);
        ImGui::SameLine();
        label("Transpose");
        float transpose = static_cast<float>(state->transpose);
        if (Groove("##mini-transpose", &transpose, -12, 12, grooveWidth, control, s, dpi, true))
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
        // The song is the row's one box; Solo Piano and Add are bare icons after it.
        // Solo Piano is left out for a song that is all piano, where it has nothing
        // to mute, as the pills that can't be pressed are.
        const bool solo = state->rows.empty() || !AllPiano(state->rows);
        ImGui::SetNextItemWidth(size.x - 2 * pad + hang - (solo ? 2 : 1) * (control + s.spacing.s1));
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
        if (solo) {
            ImGui::SameLine(0, s.spacing.s1); ImGui::BeginDisabled(state->rows.empty() || state->busy);
            { BareButtons quiet;
              const bool applied = SoloPianoApplied(state->rows);
              if (IconButton("##mini-solo-piano", Icon::Piano, "Solo Piano", s, dpi, applied))
                  engine.Send({applied ? ShellEngine::Action::UnmuteAll : ShellEngine::Action::SoloPiano, {}, state->generation});
              if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Solo Piano"); }
            ImGui::EndDisabled();
        }
        ImGui::SameLine(0, s.spacing.s1);
        // The two ways to open files, as behind the full window's plus button.
        bool add = false;
        { BareButtons quiet; add = IconButton("##mini-open", Icon::Plus, "Add MIDI files", s, dpi); }
        if (add) ImGui::OpenPopup("Add MIDI files");
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
        // The song Next loads, under the open one, on the same edges as the times under it.
        const float nextLine = (16 + std::max(0.f, GrowthOf(design).meta)) * dpi, nextY = row + control + gap / 2;
        // A looped section's bounds end that line, above the groove its handles are on,
        // as the transport row has no room for them.
        std::string bounds = LoopSection(*state) ? SectionBounds(*state) : std::string();
        if (!state->upNext.empty() || !bounds.empty()) {
            FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
            const float x = origin.x + pad, y = nextY + (nextLine - ImGui::GetTextLineHeight()) / 2;
            const float boundsWidth = bounds.empty() ? 0.f : ImGui::CalcTextSize(bounds.c_str()).x;
            if (!bounds.empty()) draw->AddText(ImVec2(origin.x + size.x - pad - boundsWidth, y), Colour(s.accent.accent), bounds.c_str());
            if (!state->upNext.empty()) {
                draw->AddText(ImVec2(x, y), Colour(s.ink.tertiary), "Up next");
                const float label = ImGui::CalcTextSize("Up next").x + s.spacing.s2;
                ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
                DrawEllipsis(Utf8(state->upNext.filename()), origin.x + size.x - pad - x - label - (boundsWidth > 0 ? boundsWidth + s.spacing.s3 : 0.f),
                             ImVec2(x + label, y));
                ImGui::PopStyleColor();
            }
        }
        const bool noSong = state->loaded.empty() || state->rows.empty() || state->busy;
        ImGui::BeginDisabled(noSong);
        const float seekHeight = 22 * dpi, seekY = nextY + nextLine + gap / 2, transportY = seekY + seekHeight + gap;
        // The time played, or the countdown, before the groove and the song's
        // length after it, as on a music player.
        const std::string length = Time(state->duration);
        const std::string played = state->playbackCountdown ? "Starts in " + std::to_string(state->playbackCountdown) + "s"
                                                            : Time(seeking_ ? seekPosition_ : state->position);
        float playedWidth = 0, lengthWidth = 0;
        { FontScope meta(fonts, design, design.type.meta * SpecFontScale(design));
          playedWidth = std::max(ImGui::CalcTextSize(played.c_str()).x, ImGui::CalcTextSize(length.c_str()).x);
          lengthWidth = ImGui::CalcTextSize(length.c_str()).x;
          const float textY = seekY + (seekHeight - ImGui::GetTextLineHeight()) / 2;
          const ImU32 ink = Colour(noSong ? s.ink.tertiary : s.ink.secondary);
          draw->AddText(ImVec2(origin.x + pad + playedWidth - ImGui::CalcTextSize(played.c_str()).x, textY), ink, played.c_str());
          draw->AddText(ImVec2(origin.x + size.x - pad - lengthWidth, textY), ink, length.c_str()); }
        const ImVec2 seekMin(origin.x + pad + playedWidth + s.spacing.s3, seekY);
        const float seekWidth = size.x - 2 * pad - playedWidth - lengthWidth - 2 * s.spacing.s3;
        const bool section = LoopSection(*state);
        if (section) SendLoopHandle(SectionHandles(seekMin, seekWidth, seekHeight, *state, dpi, loopHandle_, loopHandleAt_), engine, *state);
        ImGui::SetCursorScreenPos(seekMin);
        if (!seeking_ || seekGeneration_ != state->generation) { seekPosition_ = static_cast<float>(state->position); seeking_ = false; }
        const bool changed = Groove("##mini-seek", &seekPosition_, 0, static_cast<float>(std::max(.001, state->duration)),
            seekWidth, seekHeight, s, dpi, false);
        if (section) DrawSection(ImGui::GetWindowDrawList(), seekMin, seekWidth, seekHeight, *state, loopHandle_, loopHandleAt_, s, dpi);
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
        ImGui::EndDisabled();
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
        // Spread across the window, Play in the middle and the only filled button:
        // the song's own controls beside it, the files' outside them and Stop at
        // the end, bare. The buttons' total width is last frame's, kept in the
        // window's storage.
        const std::string back = SeekLabel(state->seekStep, false), forward = SeekLabel(state->seekStep, true);
        const ImGuiID buttonsId = ImGui::GetID("##mini-transport-buttons");
        const float span = size.x - 2 * pad + 2 * hang;
        const float buttons = ImGui::GetStateStorage()->GetFloat(buttonsId, span);
        const float spread = std::max(s.spacing.s1, std::floor((span - buttons) / 6));
        float measured = 0;
        const auto measure = [&] { measured += ImGui::GetItemRectSize().x; };
        // Whole pixels between the buttons, the rest split between the ends.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad - hang + std::max(0.f, std::floor((span - buttons - 6 * spread) / 2)), transportY));
        const bool noFiles = state->files->empty() || state->busy;
        {
        BareButtons quiet;
        ImGui::BeginDisabled(noSong);
        if (IconButton("##mini-restart", Icon::Restart, "Restart", s, dpi)) engine.Send({ShellEngine::Action::Restart, {}, state->generation});
        measure(); ImGui::EndDisabled();
        ImGui::SameLine(0, spread); ImGui::BeginDisabled(noFiles);
        if (IconButton("##mini-prev", Icon::Left, tipped("Previous MIDI file", 4).c_str(), s, dpi)) engine.Send({ShellEngine::Action::Previous, {}, state->generation});
        measure(); ImGui::EndDisabled();
        ImGui::SameLine(0, spread); ImGui::BeginDisabled(noSong);
        // Labelled, as in the full window, so the seek buttons look the same in both layouts.
        if (TransportButton("##mini-back10", back.c_str(), s, dpi)) engine.Send({ShellEngine::Action::Back10, {}, state->generation});
        measure(); under(1);
        ImGui::SameLine(0, spread);
        if (PlayButton("##mini-play", state->playing, state->playbackCountdown, s, dpi, true))
            engine.Send({ShellEngine::Action::PlayCountdown, {}, state->generation});
        measure();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", tipped(state->playing ? "Pause" : state->playbackCountdown ? "Cancel" : "Play", 0).c_str());
        ImGui::SameLine(0, spread);
        if (TransportButton("##mini-forward10", forward.c_str(), s, dpi)) engine.Send({ShellEngine::Action::Forward10, {}, state->generation});
        measure(); under(2);
        ImGui::EndDisabled();
        ImGui::SameLine(0, spread); ImGui::BeginDisabled(noFiles);
        if (IconButton("##mini-next", Icon::Right, tipped("Next MIDI file", 5).c_str(), s, dpi)) engine.Send({ShellEngine::Action::Next, {}, state->generation});
        measure(); ImGui::EndDisabled(); ImGui::SameLine(0, spread);
        if (IconButton("##mini-stop", Icon::Stop, tipped("Stop all output and cancel countdown", 3).c_str(), s, dpi)) engine.Send({ShellEngine::Action::Stop});
        measure();
        }
        ImGui::GetStateStorage()->SetFloat(buttonsId, measured);
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
    ImGui::PopStyleVar();
    // No status bar: what only it said comes up as a toast.
    DrawMiniNotice(fonts, design, dpi, *state, origin, size);
    DrawUpdateToast(fonts, design, dpi, origin, size, origin.y + size.y);
}

// Help: search over every question, a chip filtering to this build's additions,
// and otherwise the questions in folders that expand like Settings' sections.
void Panels::DrawHelp(const Fonts& fonts, const skin::Skin& design, float dpi, const EngineSnapshot& state) {
    const auto s = skin::ScaleGeometry(design, dpi);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    const auto& entries = HelpEntries();
    const std::string chip = "What's new";
    const bool haveChip = HelpAdded(entries, kHelpBuild, state.addonsFolder);
    if (!haveChip) helpAdded = false;
    const float chipWidth = haveChip ? 2 * s.spacing.s3 + ImGui::CalcTextSize(chip.c_str()).x + s.spacing.s2 : 0.f;
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    last = tourStop + 1 == stops;
    skip = TransportBody("##tour-skip", nullptr, "Skip", s, dpi, false) || (keys && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    // Next keeps its position on the last stop, where it reads Done.
    const float nextWidth = 2 * s.spacing.s4 + std::max(ImGui::CalcTextSize("Next").x, ImGui::CalcTextSize("Done").x);
    const float backWidth = 2 * s.spacing.s3 + ImGui::CalcTextSize("Back").x;
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

float Panels::UpdateToastHeight(const skin::Skin& design, float dpi) const {
    if (!preferences.checkForUpdates || update.version.empty() || updateDismissed_ || tourStop >= 0) return 0;
    const auto s = skin::ScaleGeometry(design, dpi);
    return s.metric.controlHeight + 2 * s.spacing.s2;
}

void Panels::DrawMiniNotice(const Fonts& fonts, const skin::Skin& design, float dpi, const EngineSnapshot& state, ImVec2 origin, ImVec2 size) {
    // The status bar's failure, else a conversion, which outlives its popover.
    const bool failed = !panelError_.empty() || !state.error.empty();
    const std::string text = !panelError_.empty() ? panelError_ : !state.error.empty() ? state.error
        : !state.converting ? std::string() : state.settingUp ? "Installing the converter" : state.signingIn ? "Signing in to YouTube" : "Converting audio";
    // Closed until it says something else; the same failure coming back later shows again.
    if (text.empty()) noticeClosed_.clear();
    if (text.empty() || text == noticeClosed_ || tourStop >= 0) { noticeFade_ = 0; return; }
    const auto s = skin::ScaleGeometry(design, dpi);
    // In over 200 ms, rising a few pixels as it fades in, as the update toast does.
    noticeFade_ = std::min(1.f, noticeFade_ + ImGui::GetIO().DeltaTime / .2f);
    const float fade = TourEase(noticeFade_);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    const float pad = s.spacing.s2, side = 16 * dpi, margin = s.spacing.s3, control = s.metric.controlHeight;
    const float logWidth = 2 * s.spacing.s3 + ImGui::CalcTextSize("Log").x;
    const float width = size.x - 2 * margin, height = control + 2 * pad;
    const float under = UpdateToastHeight(design, dpi);
    const float bottom = origin.y + size.y - margin - (under > 0 ? under + s.spacing.s2 : 0.f);
    ImGui::SetNextWindowPos(ImVec2(origin.x + margin, bottom - height + (1 - fade) * 8 * dpi));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    const bool open = ImGui::Begin("##mini-notice", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    if (open) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        auto* draw = ImGui::GetWindowDrawList();
        const int firstVertex = draw->VtxBuffer.Size;
        const ImVec2 min = ImGui::GetWindowPos(), max(min.x + width, min.y + height);
        skin::RaisedRect(draw, min, max, s.radius.card, s, Colour(s.surface.elevated));
        DrawIcon(draw, failed ? Icon::Warning : Icon::Audio, ImVec2(min.x + pad + s.spacing.s2, min.y + (height - side) / 2), side,
                 Colour(failed ? s.accent.warn : s.accent.accent), dpi);
        const float textX = min.x + pad + s.spacing.s2 + side + s.spacing.s2;
        const float textEnd = max.x - pad - control - s.spacing.s1 - logWidth - s.spacing.s2;
        DrawEllipsis(text, textEnd - textX, ImVec2(textX, min.y + (height - ImGui::GetTextLineHeight()) / 2), draw);
        // The whole message, for when it is cut.
        ImGui::SetCursorScreenPos(ImVec2(textX, min.y + pad));
        ImGui::InvisibleButton("##notice-text", ImVec2(std::max(1.f, textEnd - textX), control));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", text.c_str());
        ImGui::SetCursorScreenPos(ImVec2(textEnd + s.spacing.s2, min.y + pad));
        if (TransportBody("##notice-log", nullptr, "Log", s, dpi, false, logOpen)) logOpen = !logOpen;
        ImGui::SameLine(0, s.spacing.s1);
        bool close = false;
        { BareButtons quiet; close = IconButton("##notice-close", Icon::Close, "Close", s, dpi); }
        if (close) noticeClosed_ = text;
        if (fade < 1) FadeVertices(draw, firstVertex, fade);
    }
    ImGui::End();
}

void Panels::DrawUpdateToast(const Fonts& fonts, const skin::Skin& design, float dpi, ImVec2 origin, ImVec2 size, float bottom) {
    if (UpdateToastHeight(design, dpi) == 0) return;
    const auto s = skin::ScaleGeometry(design, dpi);
    // In over 200 ms, rising a few pixels as it fades in.
    updateFade_ = std::min(1.f, updateFade_ + ImGui::GetIO().DeltaTime / .2f);
    const float fade = TourEase(updateFade_);
    FontScope font(fonts, design, design.type.body * SpecFontScale(design));
    const std::string title = "QuartzMIDI " + update.version;
    float titleWidth;
    { FontScope semibold(fonts, design, design.type.body * SpecFontScale(design), Weight::Semibold);
      titleWidth = ImGui::CalcTextSize(title.c_str()).x; }
    const float pad = s.spacing.s2, side = 16 * dpi, gap = s.spacing.s2;
    const float laterWidth = 2 * s.spacing.s3 + ImGui::CalcTextSize("Later").x;
    const float updateWidth = 2 * s.spacing.s4 + ImGui::CalcTextSize("Update").x;
    const float width = pad + s.spacing.s2 + side + s.spacing.s2 + titleWidth + s.spacing.s4 + laterWidth + gap + updateWidth + pad;
    const float height = s.metric.controlHeight + 2 * pad, margin = s.spacing.s3;
    ImGui::SetNextWindowPos(ImVec2(origin.x + size.x - margin - width, bottom - margin - height + (1 - fade) * 8 * dpi));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    const bool open = ImGui::Begin("##update-toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    if (open) {
        // Above the main window even after a click there brings it forward.
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        auto* draw = ImGui::GetWindowDrawList();
        const int firstVertex = draw->VtxBuffer.Size;
        const ImVec2 min = ImGui::GetWindowPos(), max(min.x + width, min.y + height);
        skin::RaisedRect(draw, min, max, s.radius.card, s, Colour(s.surface.elevated));
        DrawIcon(draw, Icon::Upgrade, ImVec2(min.x + pad + s.spacing.s2, min.y + (height - side) / 2), side, Colour(s.accent.accent), dpi);
        { FontScope semibold(fonts, design, design.type.body * SpecFontScale(design), Weight::Semibold);
          draw->AddText(ImVec2(min.x + pad + s.spacing.s2 + side + s.spacing.s2, min.y + (height - ImGui::GetTextLineHeight()) / 2),
                        ImGui::GetColorU32(ImGuiCol_Text), title.c_str()); }
        ImGui::SetCursorScreenPos(ImVec2(max.x - pad - updateWidth - gap - laterWidth, min.y + pad));
        const bool later = TransportBody("##update-later", nullptr, "Later", s, dpi, false);
        ImGui::SameLine(0, gap);
        const bool go = TransportBody("##update-go", nullptr, "Update", s, dpi, true);
        if (fade < 1) FadeVertices(draw, firstVertex, fade);
        if (go) {
            const std::wstring wide(update.page.begin(), update.page.end());
            ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        updateDismissed_ = later || go;
    }
    ImGui::End();
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
    // A saved file found damaged at start lasts as a panel failure does, past
    // the start's own commands, which clear the engine's error.
    if (!resetShown_ && !state->resetNotice.empty()) {
        panelError_ = panelError_.empty() ? state->resetNotice : panelError_ + " " + state->resetNotice;
        resetShown_ = true;
    }
    if (!scannedLive_ && hwnd) { engine.Send({ShellEngine::Action::LiveScan}); scannedLive_ = true; }
    if (!scannedOutput_ && hwnd) { engine.Send({ShellEngine::Action::OutputScan}); scannedOutput_ = true; }
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
    const float stripPad = s.spacing.s3;
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
      const float deviceEnd = utilityX - s.spacing.s3;
      const auto name = DeviceName(*state);
      const float room = std::max(40 * dpi, deviceEnd - ImGui::GetCursorScreenPos().x);
      const float width = std::min(room, ImGui::CalcTextSize(name.c_str()).x + 26 * dpi);
      ImGui::SetCursorScreenPos(ImVec2(deviceEnd - width, origin.y + stripPad));
      if (DevicePill(name, room, s, dpi, InputWaiting(*state))) ImGui::OpenPopup("MIDI devices");
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    // The title names the list shown and opens the others: the folder, the favourites,
    // the queue while it holds a song, then the playlists.
    const int list = state->openList;
    const auto& library = *state->library;
    const std::string listName = list == kFavouritesList ? "Favourites" : list == kQueueList ? "Queue"
        : list >= 0 && static_cast<size_t>(list) < library.playlists.size() ? library.playlists[static_cast<size_t>(list)].name : "MIDI Files";
    const float buttonsX = ImGui::GetWindowWidth() - 3 * s.metric.controlHeight - 2 * s.spacing.s2;
    if (ListHeading("##lists-heading", listName, buttonsX - ImGui::GetCursorPosX() - s.spacing.s2, ImGui::IsPopupOpen("##lists"),
                    fonts, design, s, dpi))
        ImGui::OpenPopup("##lists");
    const ImVec2 headingMin = ImGui::GetItemRectMin(), headingMax = ImGui::GetItemRectMax();
    MenuUnderLastItem("##lists", s.spacing.s1);
    // A song dragged onto the title opens the menu, so it can be dropped on a playlist.
    if (ImGui::BeginDragDropTarget()) {
        if (!ImGui::IsPopupOpen("##lists")) { ImGui::OpenPopup("##lists"); listsByDrag_ = true; }
        ImGui::EndDragDropTarget();
    }
    if (!ImGui::IsPopupOpen("##lists")) listsByDrag_ = false;
    if (ImGui::BeginPopup("##lists")) {
        if (listsByDrag_ && !ImGui::GetDragDropPayload()) ImGui::CloseCurrentPopup();
        float itemWidth = ImGui::CalcTextSize("New playlist...").x;
        for (const auto& playlist : library.playlists) itemWidth = std::max(itemWidth, ImGui::CalcTextSize(playlist.name.c_str()).x);
        itemWidth += 44 * dpi;
        const auto open = [&](int wanted) {
            if (wanted == list) return;
            engine.Send({ShellEngine::Action::OpenList, {}, 0, 0, false, static_cast<double>(wanted)});
            search_[0] = 0;
        };
        // A dragged song dropped on a playlist is added to it, and on New playlist starts one.
        const auto dropped = [&]() -> std::filesystem::path {
            std::filesystem::path song;
            if (ImGui::BeginDragDropTarget()) {
                if (const auto* payload = ImGui::AcceptDragDropPayload("QM_SONG")) song = static_cast<const wchar_t*>(payload->Data);
                ImGui::EndDragDropTarget();
            }
            return song;
        };
        if (TickedItem("MIDI Files", list == kFolderList, s, dpi, itemWidth)) open(kFolderList);
        if (TickedItem("Favourites", list == kFavouritesList, s, dpi, itemWidth)) open(kFavouritesList);
        // The queue is always listed, so it is found before it holds anything;
        // empty, it is greyed.
        ImGui::BeginDisabled(library.queue.empty() && list != kQueueList);
        if (TickedItem("Queue", list == kQueueList, s, dpi, itemWidth)) open(kQueueList);
        ImGui::EndDisabled();
        if (!library.playlists.empty()) ImGui::Separator();
        for (size_t i = 0; i < library.playlists.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (TickedItem(library.playlists[i].name.c_str(), list == static_cast<int>(i), s, dpi, itemWidth)) open(static_cast<int>(i));
            if (const auto song = dropped(); !song.empty()) {
                engine.Send({ShellEngine::Action::AddToList, song, 0, 0, false, static_cast<double>(i)});
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::BeginPopupContextItem("##playlist-menu")) {
                if (ImGui::MenuItem("Rename...")) renamePlaylist_ = static_cast<int>(i);
                if (ImGui::MenuItem("Delete")) deletePlaylist_ = static_cast<int>(i);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::Selectable("New playlist...", false, 0, ImVec2(itemWidth, 0))) namePlaylist_ = true;
        if (const auto song = dropped(); !song.empty()) { playlistFirstSong_ = song; namePlaylist_ = true; }
        if (namePlaylist_ || renamePlaylist_ >= 0 || deletePlaylist_ >= 0) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine(buttonsX);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s2, ImGui::GetStyle().FramePadding.y));
    // Convert audio button, alongside the panel's other buttons. Shown active
    // while a conversion runs, since the popover may be closed and this is where
    // to reopen it. Uses a waveform icon; the speaker icon is Mute's in Tracks.
    if (IconButton("##convert-audio", Icon::Audio, "Convert audio to MIDI", s, dpi, state->converting)) openConvert = true;
    const ImVec2 convertMin = ImGui::GetItemRectMin(), convertMax = ImGui::GetItemRectMax();
    ImGui::SameLine();
    if (IconButton("##sort-files", Icon::Sort, "Sort files", s, dpi))
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
    const std::string searchHint = list == kFavouritesList ? std::string("Search favourites")
        : list != kFolderList ? "Search in " + listName
        : browse_.empty() ? std::string("Search MIDI files")
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
        const float width = 2 * s.spacing.s3 + 16 * dpi + s.spacing.s2 + ImGui::CalcTextSize(kChoose).x;
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
        // The other lists are flat, each song with its folder, as search results are. A
        // playlist and the queue keep their own order, and a song can be dragged within it.
        const bool browsing = list == kFolderList;
        const auto* songs = library.Songs(list);
        const std::vector<MidiEntry>& source = songs ? listEntries_ : *state->files;
        const bool reorder = songs && query.empty();
        if (filteredFiles_ != state->files || filteredQuery_ != query || filteredFolders_ != preferences.folders ||
            filteredLibrary_ != state->library || filteredList_ != list) {
            filteredFolders_ = preferences.folders;
            fileFilter_.clear();
            folderRows_.clear();
            listEntries_.clear();
            if (songs) {
                std::unordered_map<std::wstring, size_t> listed;
                for (size_t i = 0; i < state->files->size(); ++i) listed.emplace((*state->files)[i].path.native(), i);
                for (const auto& song : *songs) {
                    const auto found = listed.find(song.native());
                    std::error_code missing;
                    const auto bytes = found == listed.end() ? std::filesystem::file_size(song, missing) : 0;
                    listEntries_.push_back(found != listed.end() ? (*state->files)[found->second]
                        : MidiEntry{song, Utf8(song), missing ? 0 : bytes});
                }
                fileFilter_ = SearchFolder(listEntries_, "", query);
            } else if (!browsing) {
                for (const size_t i : SearchFolder(*state->files, "", query))
                    if (library.Favourite((*state->files)[i].path)) fileFilter_.push_back(i);
            } else if (query.empty() && !preferences.folders) {
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
            filteredLibrary_ = state->library;
            filteredList_ = list;
        }
        if (openTopResult && !query.empty() && !fileFilter_.empty() && !state->busy) load(source[fileFilter_.front()].path);
        // A song that Next, Previous, a hotkey or shuffle loads is scrolled into view
        // once, if its row is listed here; a row click loads a row already in view.
        if (state->loaded != followedFile_) {
            if (state->loaded != clickedFile_ && revealFile_.empty()) revealFile_ = state->loaded;
            followedFile_ = state->loaded;
            clickedFile_.clear();
        }
        // Row order: the up row, folders, then files. The first two appear only while
        // browsing; a search lists files alone.
        const int upRows = browsing && query.empty() && !browse_.empty() ? 1 : 0;
        const int folderCount = static_cast<int>(folderRows_.size());
        const int fileStart = upRows + folderCount;
        bool moved = false;
        std::string moveTo;
        const float rowPitch = s.metric.controlHeight + s.spacing.s2;
        // A just-located file: its folder is open by now, so scroll its row to a third
        // of the way down, leaving context around it. A row already in view stays put.
        if (!revealFile_.empty()) {
            for (size_t row = 0; row < fileFilter_.size(); ++row)
                if (source[fileFilter_[row]].path == revealFile_) {
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
            const size_t row = static_cast<size_t>(i - fileStart);
            const auto& file = source[fileFilter_[row]];
            ImGui::PushID(static_cast<int>(fileFilter_[row]));
            ImGui::BeginDisabled(state->busy);
            // The star is its own button over the row's right end.
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::Selectable("##file", file.path == state->loaded, 0, ImVec2(width, s.metric.controlHeight))) {
                load(file.path);
                clickedFile_ = file.path;
            }
            ImGui::EndDisabled();
            const bool rowHovered = ImGui::IsItemHovered();
            const bool rowHot = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem | ImGuiHoveredFlags_AllowWhenDisabled);
            const bool outside = std::filesystem::path(std::u8string(file.name.begin(), file.name.end())).is_absolute();
            // Any song can be dragged onto a playlist in the list menu.
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("QM_SONG", file.path.c_str(), (file.path.native().size() + 1) * sizeof(wchar_t));
                ImGui::TextUnformatted(Utf8(file.path.stem()).c_str());
                ImGui::EndDragDropSource();
            }
            // Within a playlist or the queue, a song dropped on a row goes before or after
            // it, by the half it lands on, and a line shows where.
            if (reorder && ImGui::BeginDragDropTarget()) {
                const bool after = ImGui::GetIO().MousePos.y > pos.y + s.metric.controlHeight / 2;
                if (const auto* payload = ImGui::AcceptDragDropPayload("QM_SONG", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                    const float lineY = after ? pos.y + s.metric.controlHeight + s.spacing.s1 : pos.y - s.spacing.s1;
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x + s.spacing.s2, lineY), ImVec2(pos.x + width - s.spacing.s2, lineY),
                                                        Colour(s.accent.accent), 2 * dpi);
                    if (payload->IsDelivery()) {
                        ShellEngine::Command move{ShellEngine::Action::MoveInList, static_cast<const wchar_t*>(payload->Data), 0, row + (after ? 1 : 0),
                                                  false, static_cast<double>(list)};
                        engine.Send(std::move(move));
                    }
                }
                ImGui::EndDragDropTarget();
            }
            // Like Explorer's search results: go to the file's location, which ends the
            // search and opens its folder at its row. The other lists go there too.
            if (ImGui::BeginPopupContextItem("##file-menu")) {
                if (ImGui::MenuItem("Play next")) engine.Send({ShellEngine::Action::AddToList, file.path, 0, 0, false, static_cast<double>(kQueueList)});
                if (ImGui::BeginMenu("Add to playlist")) {
                    for (size_t p = 0; p < library.playlists.size(); ++p) {
                        const auto& playlist = library.playlists[p];
                        ImGui::PushID(static_cast<int>(p));
                        const bool has = std::find(playlist.songs.begin(), playlist.songs.end(), file.path) != playlist.songs.end();
                        if (ImGui::MenuItem(playlist.name.c_str(), nullptr, false, !has))
                            engine.Send({ShellEngine::Action::AddToList, file.path, 0, 0, false, static_cast<double>(p)});
                        ImGui::PopID();
                    }
                    if (!library.playlists.empty()) ImGui::Separator();
                    if (ImGui::MenuItem("New playlist...")) { playlistFirstSong_ = file.path; namePlaylist_ = true; }
                    ImGui::EndMenu();
                }
                if (songs && ImGui::MenuItem(list == kQueueList ? "Remove from queue" : "Remove from playlist"))
                    engine.Send({ShellEngine::Action::RemoveFromList, file.path, 0, 0, false, static_cast<double>(list)});
                ImGui::Separator();
                if ((!query.empty() || !browsing) && !outside && preferences.folders && ImGui::MenuItem("Open file location")) {
                    moved = true;
                    moveTo = FolderOf(file.name);
                    revealFile_ = file.path;
                    search_[0] = 0;
                    if (!browsing) engine.Send({ShellEngine::Action::OpenList, {}, 0, 0, false, static_cast<double>(kFolderList)});
                }
                if (ImGui::MenuItem("Show in Explorer"))
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + file.path.native() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) engine.Send({ShellEngine::Action::DeleteSong, file.path});
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
            // A favourite's star stays; the open song's row and a hovered one show an empty one.
            const bool favourite = state->library->Favourite(file.path);
            const float starSide = 16 * dpi, starX = pos.x + width - s.spacing.s3 - starSide;
            ImGui::SetCursorScreenPos(ImVec2(starX - s.spacing.s1, pos.y));
            if (ImGui::InvisibleButton("##star", ImVec2(starSide + 2 * s.spacing.s1, s.metric.controlHeight)))
                engine.Send({ShellEngine::Action::Favourite, file.path, 0, 0, !favourite});
            const bool starHot = ImGui::IsItemHovered();
            if (favourite || rowHot || selected)
                DrawStar(listDraw, ImVec2(starX, pos.y + (s.metric.controlHeight - starSide) / 2), starSide,
                         Colour(favourite ? s.accent.accent : starHot ? s.ink.primary : s.ink.secondary), favourite);
            // The row's menu is on a button too, in the size's place while the row is hovered.
            const float moreX = starX - s.spacing.s2 - starSide;
            // Always submitted, like the star: pressing it takes the row's hover away,
            // and a button that vanished before the release would never be clicked.
            const bool menuOpen = ImGui::IsPopupOpen("##file-menu");
            ImGui::SetCursorScreenPos(ImVec2(moreX - s.spacing.s1, pos.y));
            if (ImGui::InvisibleButton("##more", ImVec2(starSide + 2 * s.spacing.s1, s.metric.controlHeight)))
                ImGui::OpenPopup("##file-menu");
            const bool moreHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            const bool showMore = rowHot || menuOpen || moreHot;
            if (showMore)
                DrawIcon(listDraw, Icon::More, ImVec2(moreX, pos.y + (s.metric.controlHeight - starSide) / 2), starSide,
                         Colour(moreHot || menuOpen ? s.ink.primary : s.ink.secondary), dpi);
            FontScope rowFont(fonts, design, design.type.body * SpecFontScale(design), selected ? Weight::Semibold : Weight::Regular);
            const auto bytes = std::to_string((file.bytes + 1023) / 1024) + " KB";
            const float sizeWidth = ImGui::CalcTextSize(bytes.c_str()).x;
            const float textY = pos.y + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2;
            // Search results and favourites show their folder; browsed rows are already under
            // theirs. The song name comes first and the folder takes the remaining width.
            const std::string shown = file.name.substr(browsing ? browse_.size() : 0);
            const size_t cut = shown.find_last_of("\\/");
            const std::string leaf = cut == std::string::npos ? shown : shown.substr(cut + 1);
            const float room = width - sizeWidth - starSide - s.spacing.s2 - 3 * s.spacing.s3;
            DrawEllipsis(leaf, room, ImVec2(pos.x + s.spacing.s3, textY));
            const float used = ImGui::CalcTextSize(leaf.c_str()).x + s.spacing.s3;
            if (cut != std::string::npos && room - used > 3 * ImGui::CalcTextSize("...").x) {
                ImGui::PushStyleColor(ImGuiCol_Text, Colour(s.ink.secondary));
                DrawEllipsis(shown.substr(0, cut), room - used, ImVec2(pos.x + s.spacing.s3 + used, textY));
                ImGui::PopStyleColor();
            }
            if (!showMore) listDraw->AddText(ImVec2(starX - s.spacing.s2 - sizeWidth, textY), Colour(s.ink.secondary), bytes.c_str());
            if (rowHovered) ImGui::SetTooltip("%s\n%llu bytes", file.name.c_str(), static_cast<unsigned long long>(file.bytes));
            ImGui::PopID();
        }
        if (fileFilter_.empty() && fileStart == 0)
            ImGui::TextDisabled(!query.empty() ? "No matching files" : songs ? "No songs" : "No favourites");
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
    // Naming a playlist, new or renamed, and deleting one, under the title. A playlist
    // made from a song's menu starts with it and leaves the list shown as it was.
    if (namePlaylist_ || renamePlaylist_ >= 0) {
        renamingPlaylist_ = namePlaylist_ ? -1 : renamePlaylist_;
        if (renamingPlaylist_ < 0) playlistName_[0] = 0;
        else if (static_cast<size_t>(renamingPlaylist_) < library.playlists.size())
            strncpy_s(playlistName_, library.playlists[static_cast<size_t>(renamingPlaylist_)].name.c_str(), _TRUNCATE);
        if (renamingPlaylist_ >= 0) playlistFirstSong_.clear();
        namePlaylist_ = false;
        renamePlaylist_ = -1;
        ImGui::OpenPopup("##playlist-name");
    }
    const float nameButton = 96 * dpi;
    if (ImGui::IsPopupOpen("##playlist-name")) ImGui::SetNextWindowPos(ImVec2(headingMin.x, headingMax.y + s.spacing.s1), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##playlist-name")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(2 * nameButton + s.spacing.s2);
        const bool entered = ImGui::InputTextWithHint("##playlist-name-field", "Playlist name", playlistName_, sizeof(playlistName_),
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        const bool named = std::string_view(playlistName_).find_first_not_of(" \t") != std::string_view::npos;
        ImGui::BeginDisabled(!named);
        const bool done = EasedButton(renamingPlaylist_ < 0 ? "Create" : "Rename", ImVec2(nameButton, s.metric.controlHeight)) || (entered && named);
        ImGui::EndDisabled();
        ImGui::SameLine(0, s.spacing.s2);
        if (EasedButton("Cancel", ImVec2(nameButton, s.metric.controlHeight))) ImGui::CloseCurrentPopup();
        if (done) {
            ShellEngine::Command name{renamingPlaylist_ < 0 ? ShellEngine::Action::NewPlaylist : ShellEngine::Action::RenamePlaylist,
                                      playlistFirstSong_, 0, 0, playlistFirstSong_.empty(), static_cast<double>(renamingPlaylist_)};
            name.key = playlistName_;
            engine.Send(std::move(name));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!ImGui::IsPopupOpen("##playlist-name")) playlistFirstSong_.clear();
    if (deletePlaylist_ >= 0) { deletingPlaylist_ = deletePlaylist_; deletePlaylist_ = -1; ImGui::OpenPopup("##confirm-delete-playlist"); }
    if (ImGui::IsPopupOpen("##confirm-delete-playlist"))
        ImGui::SetNextWindowPos(ImVec2(headingMin.x, headingMax.y + s.spacing.s1), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##confirm-delete-playlist")) {
        if (deletingPlaylist_ < 0 || static_cast<size_t>(deletingPlaylist_) >= library.playlists.size()) ImGui::CloseCurrentPopup();
        else {
            ImGui::Text("Delete %s?", library.playlists[static_cast<size_t>(deletingPlaylist_)].name.c_str());
            const bool remove = EasedButton("Delete", ImVec2(nameButton, s.metric.controlHeight));
            ImGui::SameLine(0, s.spacing.s2);
            if (EasedButton("Cancel", ImVec2(nameButton, s.metric.controlHeight))) ImGui::CloseCurrentPopup();
            if (remove) {
                engine.Send({ShellEngine::Action::DeletePlaylist, {}, 0, 0, false, static_cast<double>(deletingPlaylist_)});
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(); ImGui::PopFont();
    ImGui::EndChild();

    const float right = leftMax.x + s.spacing.s3;
    const float edge = origin.x + size.x - s.spacing.windowPad;
    // File name and sheet action above the seek groove, rows 8 dp apart. The
    // hotkey hints share the title row to save a row for Tracks at the smallest
    // window; sheet results go to the status bar for the same reason.
    const float titleHeight = s.metric.controlHeight;
    const float rowGap = s.spacing.s2, seekHeight = 22 * dpi;
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
    const float sheetButtonWidth = state->sheetsAddon ? 2 * s.spacing.s3 + 16 * dpi + s.spacing.s2 + ImGui::CalcTextSize(sheetLabel).x : -s.spacing.s2;
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
    const ImVec2 seekMin(content.x, content.y + titleHeight + rowGap);
    // Looping a section, its handles sit on the seek groove.
    const bool section = LoopSection(*state);
    if (section) SendLoopHandle(SectionHandles(seekMin, contentWidth, seekHeight, *state, dpi, loopHandle_, loopHandleAt_), engine, *state);
    ImGui::SetCursorScreenPos(seekMin);
    if (!seeking_ || seekGeneration_ != state->generation) {
        seekPosition_ = static_cast<float>(state->position);
        seeking_ = false;
    }
    const bool seekChanged = Groove("##seek", &seekPosition_, 0, static_cast<float>(std::max(.001, state->duration)),
                                     contentWidth, seekHeight, s, dpi, false);
    if (section) DrawSection(ImGui::GetWindowDrawList(), seekMin, contentWidth, seekHeight, *state, loopHandle_, loopHandleAt_, s, dpi);
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
    if (IconButton("##restart", Icon::Restart, "Restart", s, dpi)) send(ShellEngine::Action::Restart);
    ImGui::SameLine();
    if (TransportButton("##back10", SeekLabel(state->seekStep, false).c_str(), s, dpi)) send(ShellEngine::Action::Back10);
    ImGui::SameLine();
    if (TransportButton("##forward10", SeekLabel(state->seekStep, true).c_str(), s, dpi)) send(ShellEngine::Action::Forward10);
    ImGui::EndDisabled();
    // Off, the song, then the section between the seek groove's handles.
    ImGui::SameLine();
    {
        static constexpr const char* kLoopNames[] = {"Loop", "Loop song", "Loop section"};
        const int loop = std::clamp(state->loop, 0, 2);
        if (IconButton("##loop", loop == 1 ? Icon::Repeat1 : Icon::Repeat, kLoopNames[loop], s, dpi, loop != 0)) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + s.spacing.s1));
            ImGui::OpenPopup("##loop-menu");
        }
        if (ImGui::BeginPopup("##loop-menu")) {
            static constexpr const char* kLoopChoices[] = {"Off", "Song", "Section"};
            for (int choice = 0; choice < 3; ++choice)
                if (ImGui::MenuItem(kLoopChoices[choice], nullptr, loop == choice) && choice != loop)
                    engine.Send({ShellEngine::Action::Loop, {}, 0, static_cast<size_t>(choice)});
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine(); ImGui::BeginDisabled(state->files->empty() || state->busy);
    if (IconButton("##previous", Icon::Left, "Previous MIDI file", s, dpi)) send(ShellEngine::Action::Previous);
    ImGui::SameLine();
    if (IconButton("##next", Icon::Right, "Next MIDI file", s, dpi)) send(ShellEngine::Action::Next);
    ImGui::EndDisabled(); ImGui::SameLine();
    if (IconButton("##stop", Icon::Stop, "Stop all output and cancel countdown", s, dpi)) send(ShellEngine::Action::Stop);
    const float transportEnd = ImGui::GetItemRectMax().x;
    ImGui::BeginDisabled(state->loaded.empty() || state->rows.empty() || state->busy);
    dl = ImGui::GetWindowDrawList();
    TimeReadout(dl, *state, section, transportEnd + s.spacing.s3, content.x + contentWidth,
                transportY + (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2, s);
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
    // The grooves give up width before anything in the row is cut.
    float grooveWidth = 132 * dpi;
    {
        FontScope font(fonts, design, design.type.meta * SpecFontScale(design));
        const float fixed = ImGui::CalcTextSize("Speed").x + ImGui::CalcTextSize("Transpose").x + 2 * 48 * dpi +
                            5 * ImGui::GetStyle().ItemSpacing.x;
        grooveWidth = std::clamp((contentWidth - fixed) / 2, 64 * dpi, 132 * dpi);
    }
    label("Speed");
    float speedValue = static_cast<float>(state->speed);
    if (Groove("##speed", &speedValue, static_cast<float>(state->speedMin), static_cast<float>(state->speedMax),
               grooveWidth, s.metric.controlHeight, s, dpi, true))
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
    if (Groove("##transpose", &transpose, -12, 12, grooveWidth, s.metric.controlHeight, s, dpi, true))
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    if (DisclosureHeading("##tracks-disclosure", tracksOpen, "Tracks",
                          state->rows.empty() ? std::string() : std::to_string(state->rows.size()), fonts, design, s, dpi))
        tracksExpanded = !tracksExpanded;
    // One toggle: on means the rows match what Solo Piano leaves; clicking again
    // restores every track.
    const bool applied = SoloPianoApplied(state->rows);
    const bool allPiano = AllPiano(state->rows);
    const float actionsWidth = 2 * s.spacing.s3 + 16 * dpi + s.spacing.s2 + ImGui::CalcTextSize("Solo Piano").x;
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.spacing.s3, (s.metric.controlHeight - ImGui::GetTextLineHeight()) / 2));
    const ImVec2 tableMin = ImGui::GetCursorScreenPos();
    ImVec2 tableSize = ImGui::GetContentRegionAvail();
    // No recessed fill: rows are card-coloured, so it would only show as a sliver
    // under the last row. The frame and corners are drawn after the table.
    ImDrawList* tableDraw = nullptr;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(s.spacing.s2, s.spacing.s1));
    // Row rules are drawn above each row after the table; BordersInnerH also adds
    // one under the last row, which looks like a clipped row.
    std::vector<float> rules;
    float headerBottom = tableMin.y, headerHeight = 0.f;
    const float rowHeight = s.metric.controlHeight + 2 * s.spacing.s1;
    // With more rows than fit, the table ends on a whole row rather than cutting
    // the last one; with fewer, it fills the panel.
    { FontScope font(fonts, design, tableHeading, Weight::Semibold);
      const float header = ImGui::GetTextLineHeight() + 2 * s.spacing.s1;
      const float rows = std::floor((tableSize.y - header) / rowHeight);
      if (rows >= 1 && rows < static_cast<float>(state->rows.size())) tableSize.y = header + rows * rowHeight; }
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
    DrawUpdateToast(fonts, design, dpi, origin, size, origin.y + size.y - status);
    // Drawn last, and only in the full window; mini has no tour.
    DrawTour(fonts, design, dpi, origin, size);
}
}
