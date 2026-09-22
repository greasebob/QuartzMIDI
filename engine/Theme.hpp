#pragma once
// -----------------------------------------------------------------------------
// Theme: colours, fonts and DPI metrics for the GDI controls.
// Values are `inline` so every translation unit shares one instance.
// -----------------------------------------------------------------------------
#include <windows.h>

namespace Theme {
    // --- Surfaces ---
    inline constexpr COLORREF WINDOW_BG   = RGB(243, 244, 246);
    inline constexpr COLORREF CARD_BG     = RGB(255, 255, 255);
    inline constexpr COLORREF CARD_BORDER = RGB(223, 226, 231);

    // --- Text ---
    inline constexpr COLORREF TEXT        = RGB(28, 30, 34);
    inline constexpr COLORREF TEXT_MUTED  = RGB(105, 112, 123);

    // --- Accent ---
    inline constexpr COLORREF ACCENT      = RGB(0, 120, 212);

    // --- Buttons (resting / hover / pressed) ---
    inline constexpr COLORREF BTN_BG      = RGB(252, 252, 253);
    inline constexpr COLORREF BTN_BORDER  = RGB(204, 209, 217);
    inline constexpr COLORREF BTN_HOVER   = RGB(240, 246, 253);
    inline constexpr COLORREF BTN_PRESS   = RGB(224, 233, 244);

    // --- Toggled-on state (labels stay regular weight; UIBold is not bold) ---
    inline constexpr COLORREF ON_BG       = RGB(219, 245, 228);
    inline constexpr COLORREF ON_BORDER   = RGB(38, 150, 79);
    inline constexpr COLORREF ON_TEXT     = RGB(15, 82, 40);

    // --- Sustain tri-state ---
    inline constexpr COLORREF SUS_DOWN_BG     = RGB(219, 245, 228);
    inline constexpr COLORREF SUS_DOWN_BORDER = RGB(38, 150, 79);
    inline constexpr COLORREF SUS_UP_BG       = RGB(226, 236, 253);
    inline constexpr COLORREF SUS_UP_BORDER   = RGB(0, 120, 212);

    // --- Editors / lists ---
    inline constexpr COLORREF EDIT_BG     = RGB(255, 255, 255);
    inline constexpr COLORREF EDIT_TEXT   = RGB(28, 30, 34);
    inline constexpr COLORREF LOG_BG      = RGB(250, 250, 252);
    inline constexpr COLORREF LOG_TEXT    = RGB(62, 68, 78);

    // --- Table surfaces (track list) ---
    inline constexpr COLORREF TABLE_BG        = RGB(255, 255, 255);
    inline constexpr COLORREF TABLE_HEADER_BG = RGB(248, 249, 251);
    inline constexpr COLORREF TABLE_ROW_ALT   = RGB(252, 252, 253);
    inline constexpr COLORREF TABLE_SELECTED  = RGB(232, 240, 252);
    inline constexpr COLORREF TABLE_GRID      = RGB(233, 236, 240);

    // --- DPI ---------------------------------------------------------------
    // Set once at startup, before any font or layout value is read.
    inline int g_dpi = 96;
    inline int S(int v) { return MulDiv(v, g_dpi, 96); }
    inline void SetDpi(int dpi) { g_dpi = (dpi > 0 ? dpi : 96); }

    // --- Shape (design units at 96 DPI) ---
    inline constexpr int D_CORNER_RADIUS      = 6;
    inline constexpr int D_CARD_CORNER_RADIUS = 8;
    inline int CornerRadius()     { return S(D_CORNER_RADIUS); }
    inline int CardCornerRadius() { return S(D_CARD_CORNER_RADIUS); }

    inline HFONT MakeFont(int px, int weight, const wchar_t* face) {
        LOGFONTW lf = {};
        lf.lfHeight         = -px;
        lf.lfWeight         = weight;
        lf.lfCharSet        = DEFAULT_CHARSET;
        lf.lfOutPrecision   = OUT_TT_PRECIS;
        lf.lfClipPrecision  = CLIP_DEFAULT_PRECIS;
        lf.lfQuality        = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
        wcscpy_s(lf.lfFaceName, face);
        return CreateFontIndirectW(&lf);
    }

    // Created lazily, so SetDpi must run before the first call.
    inline HFONT UI()       { static HFONT f = MakeFont(S(15), FW_NORMAL,   L"Segoe UI"); return f; }
    inline HFONT UIBold()   { static HFONT f = MakeFont(S(15), FW_NORMAL,   L"Segoe UI"); return f; }
    inline HFONT UITitle()  { static HFONT f = MakeFont(S(16), FW_SEMIBOLD, L"Segoe UI"); return f; }
    inline HFONT UIHeader() { static HFONT f = MakeFont(S(14), FW_SEMIBOLD, L"Segoe UI"); return f; }
    inline HFONT Mono()     { static HFONT f = MakeFont(S(14), FW_NORMAL,   L"Consolas");  return f; }
}
