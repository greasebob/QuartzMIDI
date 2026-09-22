#pragma once

// SkinDraw: ImGui rendering for Skin.hpp.
//
// ApplyStyle() maps fills, borders and radii onto ImGuiStyle. ImGui has no
// notion of elevation, so the shadows and highlights on raised and recessed
// surfaces are drawn into the draw list as rounded rects and lines.

#include "../engine/Skin.hpp"
#include "imgui.h"

namespace skin {

// Maps a Skin onto ImGuiStyle: colours, radii and spacing.
// Call on skin change, not per frame.
void ApplyStyle(const Skin& s, float dpi = 1.f);

// Returns a copy with geometry scaled by dpi. Font sizes stay in design pixels;
// ImGuiStyle::FontScaleDpi scales them, so they must not be scaled here too.
Skin ScaleGeometry(const Skin& s, float dpi);

// Raised surface (card, control, popover): ambient and contact shadows, fill,
// hairline border and a 1 px highlight inside the top edge. The ambient shadow
// is approximated with a few stacked translucent rounded rects.
void RaisedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                const Skin& s, ImU32 fill, bool topHighlight = true);

// Recessed surface (fields, lists, track table, curve graph, slider grooves):
// fill, inner shadow along the top edge, hairline border. Pass shadow = false
// when content will cover the top edge and let RoundCorners draw it on top.
void RecessedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding,
                  const Skin& s, bool shadow = true);

// Wrappers using the current window's draw list; rects are in screen space.
void RaisedPanel(ImVec2 min, ImVec2 max, const Skin& s);
void RecessedField(ImVec2 min, ImVec2 max, const Skin& s, bool shadow = true);

// Call after drawing square-cornered content inside a rounded field (rows,
// headers, scrollbar). Masks the corners with `outside` (the surrounding
// colour), optionally draws the inner shadow, then the hairline. Use the same
// draw list as the content.
void RoundCorners(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, ImU32 outside, const Skin& s,
                  bool shadow = false);

} // namespace skin
