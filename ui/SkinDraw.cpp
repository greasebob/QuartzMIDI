#include "SkinDraw.hpp"
#include <algorithm>
#include <cmath>

namespace skin {
namespace {

// Skin stores 0xAARRGGBB; ImGui packs 0xAABBGGRR.
ImU32 ToImU32(Argb c) {
    const uint32_t a = (c >> 24) & 0xFF, r = (c >> 16) & 0xFF,
                   g = (c >> 8) & 0xFF,  b = c & 0xFF;
    return IM_COL32(r, g, b, a);
}

ImVec4 ToVec4(Argb c) {
    return ImVec4(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f,
                  (c & 0xFF) / 255.f, ((c >> 24) & 0xFF) / 255.f);
}

// Scale an ImU32's alpha, for stacking shadow layers.
ImU32 Fade(ImU32 c, float factor) {
    const uint32_t a = static_cast<uint32_t>(((c >> IM_COL32_A_SHIFT) & 0xFF) * factor);
    return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// Blur approximated by concentric translucent rounded rects; more than three
// layers makes no visible difference at these radii.
void StackedShadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                   const Shadow& sh) {
    const int layers = 3;
    const ImU32 base = ToImU32(sh.colour);
    for (int i = layers; i >= 1; --i) {
        const float spread = sh.blur * (static_cast<float>(i) / layers);
        dl->AddRectFilled(ImVec2(min.x - spread, min.y - spread + sh.offsetY),
                          ImVec2(max.x + spread, max.y + spread + sh.offsetY),
                          Fade(base, 1.f / layers), rounding + spread);
    }
}

} // namespace

Skin ScaleGeometry(const Skin& source, float dpi) {
    Skin s = source;
    s.radius = {s.radius.window * dpi, s.radius.card * dpi,
                s.radius.control * dpi, s.radius.element * dpi};
    s.spacing = {s.spacing.s1 * dpi, s.spacing.s2 * dpi, s.spacing.s3 * dpi,
                 s.spacing.s4 * dpi, s.spacing.s6 * dpi,
                 s.spacing.windowPad * dpi, s.spacing.panelPad * dpi};
    s.metric = {s.metric.controlHeight * dpi, s.metric.innerHeight * dpi};
    for (Shadow* shadow : {&s.contact, &s.ambient, &s.inner}) {
        shadow->offsetY *= dpi;
        shadow->blur *= dpi;
    }
    return s;
}

void ApplyStyle(const Skin& s, float dpi) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle(); // Reset first so ScaleAllSizes never compounds.
    st.FontSizeBase = s.type.body;
    st.FontScaleDpi = dpi;

    // Concentric radii: a nested element's radius is its container's minus the inset.
    st.WindowRounding = s.radius.window;
    st.ChildRounding  = s.radius.card;
    st.FrameRounding  = s.radius.control;
    st.PopupRounding  = s.radius.card;
    st.GrabRounding   = s.radius.element;
    st.TabRounding    = s.radius.element;
    st.ScrollbarRounding = s.radius.element;

    // Spacing from the 4 px scale.
    st.WindowPadding    = ImVec2(s.spacing.windowPad, s.spacing.windowPad);
    st.FramePadding     = ImVec2(s.spacing.s3, (s.metric.controlHeight - s.type.body) * 0.5f);
    st.ItemSpacing      = ImVec2(s.spacing.s2, s.spacing.s2);
    st.ItemInnerSpacing = ImVec2(s.spacing.s2, s.spacing.s1);
    st.CellPadding      = ImVec2(s.spacing.s3, s.spacing.s1);
    st.IndentSpacing    = s.spacing.s4;

    // 1 px translucent hairline borders.
    st.WindowBorderSize = 1.f;
    st.ChildBorderSize  = 1.f;
    st.FrameBorderSize  = 1.f;
    st.PopupBorderSize  = 1.f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]        = ToVec4(s.surface.canvas);
    c[ImGuiCol_ChildBg]         = ToVec4(s.surface.card);
    c[ImGuiCol_PopupBg]         = ToVec4(s.surface.card);
    c[ImGuiCol_MenuBarBg]       = ToVec4(s.surface.structure);
    c[ImGuiCol_Border]          = ToVec4(s.border.hairline);
    c[ImGuiCol_BorderShadow]    = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text]            = ToVec4(s.ink.primary);
    c[ImGuiCol_TextDisabled]    = ToVec4(s.ink.tertiary);

    // Frames (inputs) use the recessed tier.
    c[ImGuiCol_FrameBg]         = ToVec4(s.surface.recessed);
    c[ImGuiCol_FrameBgHovered]  = ToVec4(s.surface.elevatedHot);
    c[ImGuiCol_FrameBgActive]   = ToVec4(s.surface.recessed);

    // Buttons use the elevated tier.
    c[ImGuiCol_Button]          = ToVec4(s.surface.elevated);
    c[ImGuiCol_ButtonHovered]   = ToVec4(s.surface.elevatedHot);
    c[ImGuiCol_ButtonActive]    = ToVec4(s.surface.recessed);
    c[ImGuiCol_Header]          = ToVec4(s.accent.accentSoft);
    c[ImGuiCol_HeaderHovered]   = ToVec4(s.surface.elevatedHot);
    c[ImGuiCol_HeaderActive]    = ToVec4(s.accent.accentSoft);

    // Accent for selection, focus and fills only.
    c[ImGuiCol_CheckMark]       = ToVec4(s.accent.okInk);
    c[ImGuiCol_CheckboxSelectedBg] = ToVec4(s.accent.okSoft);
    c[ImGuiCol_SliderGrab]      = ToVec4(s.surface.elevated);
    c[ImGuiCol_SliderGrabActive]= ToVec4(s.surface.elevated);
    c[ImGuiCol_NavCursor]       = ToVec4(s.accent.accent);
    c[ImGuiCol_PlotLines]       = ToVec4(s.accent.accent);
    c[ImGuiCol_PlotHistogram]   = ToVec4(s.accent.accent);
    c[ImGuiCol_TextSelectedBg]  = ToVec4(s.accent.accentSoft);

    c[ImGuiCol_TitleBg]         = ToVec4(s.surface.structure);
    c[ImGuiCol_TitleBgActive]   = ToVec4(s.surface.structure);
    c[ImGuiCol_TitleBgCollapsed]= ToVec4(s.surface.structure);
    // Resize grip is invisible at rest and a soft accent on hover/drag.
    c[ImGuiCol_ResizeGrip]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = ToVec4(s.accent.accentSoft);
    c[ImGuiCol_ResizeGripActive]  = ToVec4(s.accent.accentSoft);
    c[ImGuiCol_Separator]       = ToVec4(s.border.hairline);
    c[ImGuiCol_TableBorderLight]= ToVec4(s.border.hairline);
    c[ImGuiCol_TableBorderStrong]= ToVec4(s.border.strong);
    c[ImGuiCol_TableHeaderBg]   = ToVec4(s.surface.structure);
    c[ImGuiCol_TableRowBg]      = ToVec4(s.surface.card);
    c[ImGuiCol_TableRowBgAlt]   = ToVec4(s.surface.card);
    c[ImGuiCol_ScrollbarBg]     = ToVec4(s.surface.recessed);
    c[ImGuiCol_ScrollbarGrab]   = ToVec4(s.surface.elevated);
    c[ImGuiCol_ScrollbarGrabHovered] = ToVec4(s.surface.elevatedHot);
    c[ImGuiCol_ScrollbarGrabActive] = ToVec4(s.ink.tertiary);
    st.ScaleAllSizes(dpi);
}

void RaisedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                const Skin& s, ImU32 fill, bool topHighlight) {
    StackedShadow(dl, min, max, rounding, s.ambient);
    StackedShadow(dl, min, max, rounding, s.contact);
    dl->AddRectFilled(min, max, fill, rounding);
    dl->AddRect(min, max, ToImU32(s.border.hairline), rounding);
    if (topHighlight) {
        // Follows both rounded top corners as well as the straight top edge.
        const float radius = std::max(0.f, rounding - 1.f);
        dl->PathArcTo(ImVec2(min.x + rounding, min.y + rounding), radius,
                      3.14159265f, 4.71238898f);
        dl->PathArcTo(ImVec2(max.x - rounding, min.y + rounding), radius,
                      4.71238898f, 6.28318531f);
        dl->PathStroke(ToImU32(s.border.topHighlight), 0, 1.f);
    }
}

// Inner shadow: three darker 1 px bands inside the top edge. Each band follows
// the rounded top corners (like RaisedRect's highlight), then continues down
// each side with a fade so it doesn't end abruptly where the arc meets the side.
static void InnerShadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, const Skin& s) {
    constexpr float kHalf = 3.14159265f, kThreeQuarter = 4.71238898f, kFull = 6.28318531f;
    const ImU32 shade = ToImU32(s.inner.colour);
    const int segments = std::max(6, static_cast<int>(rounding));
    const float tailEnd = std::min(max.y - rounding, min.y + 2.f * std::max(rounding, 4.f));
    for (int i = 0; i < 3; ++i) {
        const float radius = std::max(0.f, rounding - 1.f - i);
        const ImU32 band = Fade(shade, 1.f - i * 0.3f);
        // Concentric with the border's corners, so band i is i+1 px inside
        // along the whole curve.
        const float centreY = min.y + std::max(rounding, 1.f + i);
        dl->PathArcTo(ImVec2(min.x + rounding, centreY), radius, kHalf, kThreeQuarter, segments);
        dl->PathArcTo(ImVec2(max.x - rounding, centreY), radius, kThreeQuarter, kFull, segments);
        dl->PathStroke(band, 0, 1.f);
        if (tailEnd > centreY) {
            const ImU32 clear = band & ~IM_COL32_A_MASK;
            const float left = min.x + rounding - radius, right = max.x - rounding + radius;
            dl->AddRectFilledMultiColor(ImVec2(left - 0.5f, centreY), ImVec2(left + 0.5f, tailEnd), band, band, clear, clear);
            dl->AddRectFilledMultiColor(ImVec2(right - 0.5f, centreY), ImVec2(right + 0.5f, tailEnd), band, band, clear, clear);
        }
    }
}

void RecessedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                  const Skin& s, bool shadow) {
    dl->AddRectFilled(min, max, ToImU32(s.surface.recessed), rounding);
    if (shadow) InnerShadow(dl, min, max, rounding, s);
    dl->AddRect(min, max, ToImU32(s.border.hairline), rounding);
}

void RoundCorners(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, ImU32 outside, const Skin& s, bool shadow) {
    if (rounding > 0) {
        constexpr float kQuarter = 1.57079633f;
        const ImVec2 corners[4]{min, ImVec2(max.x, min.y), max, ImVec2(min.x, max.y)};
        const ImVec2 centres[4]{ImVec2(min.x + rounding, min.y + rounding), ImVec2(max.x - rounding, min.y + rounding),
                                ImVec2(max.x - rounding, max.y - rounding), ImVec2(min.x + rounding, max.y - rounding)};
        // Fill each corner sliver with a triangle fan from the square corner to
        // the arc; the sliver is star-shaped from that corner, so the fan covers
        // it exactly (PathFillConcave leaves a chamfer). Anti-aliased fill is
        // disabled to avoid seams between triangles; the hairline smooths the edge.
        const int segments = std::max(8, static_cast<int>(rounding));
        const ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        dl->PushClipRect(min, max, false);
        for (int c = 0; c < 4; ++c) {
            const float start = kQuarter * (c + 2);
            ImVec2 previous(centres[c].x + rounding * std::cos(start), centres[c].y + rounding * std::sin(start));
            for (int i = 1; i <= segments; ++i) {
                const float angle = start + kQuarter * i / segments;
                const ImVec2 next(centres[c].x + rounding * std::cos(angle), centres[c].y + rounding * std::sin(angle));
                dl->AddTriangleFilled(corners[c], previous, next, outside);
                previous = next;
            }
        }
        dl->PopClipRect();
        dl->Flags = flags;
    }
    // Drawn over the content so a highlighted first row can't hide it.
    if (shadow) InnerShadow(dl, min, max, rounding, s);
    dl->AddRect(min, max, ToImU32(s.border.hairline), rounding);
}

void RaisedPanel(ImVec2 min, ImVec2 max, const Skin& s) {
    RaisedRect(ImGui::GetWindowDrawList(), min, max, s.radius.card, s,
               ToImU32(s.surface.card));
}

void RecessedField(ImVec2 min, ImVec2 max, const Skin& s, bool shadow) {
    RecessedRect(ImGui::GetWindowDrawList(), min, max, s.radius.element, s, shadow);
}

} // namespace skin
