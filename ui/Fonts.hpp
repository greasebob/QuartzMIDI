#pragma once
#include "SkinDraw.hpp"

namespace shell {
enum class Weight { Regular, Medium, Semibold };

class Fonts {
public:
    void Load();
    ImFont* Get(const skin::Skin& skin, Weight weight = Weight::Regular) const;
private:
    ImFont* fonts_[3]{};
};

// Sizes are in design pixels. FontScaleDpi applies the monitor scale once.
class FontScope {
public:
    FontScope(const Fonts& fonts, const skin::Skin& skin, float size,
              Weight weight = Weight::Regular) { ImGui::PushFont(fonts.Get(skin, weight), size); }
    ~FontScope() { ImGui::PopFont(); }
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
};
}
