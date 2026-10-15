#pragma once

#include <imgui.h>

namespace ui
{

    enum class ThemeKind
    {
        ProDark,
        ProMedium,
        ProLightGray,
        ProLight,
    };

    const int THEME_COUNT = 4;

    struct Tokens
    {
        ThemeKind kind;
        ImU32 chrome;
        ImU32 canvas;
        ImU32 dock;
        ImU32 card;
        ImU32 card_border;
        ImU32 field;
        ImU32 field_border;
        ImU32 hover;
        ImU32 pressed;
        ImU32 text;
        ImU32 text_dim;
        ImU32 text_faint;
        ImU32 icon;
        ImU32 accent;
        ImU32 accent_soft;
        ImU32 accent_text;
        ImU32 separator;
        ImU32 shadow;
        ImU32 danger;
        ImU32 warning;
        ImU32 tab_strip;
        ImU32 row_selected;
        float radius_sm;
        float radius;
        float radius_lg;
    };

    struct Fonts
    {
        ImFont* regular;
        ImFont* medium;
        ImFont* semibold;
        ImFont* mono;
    };

    const char* theme_label (ThemeKind k);
    const char* theme_id (ThemeKind k);
    bool theme_from_id (const char* id, ThemeKind* out);
    Tokens tokens_for (ThemeKind k);
    void apply_theme (const Tokens& t, float scale);
    void load_fonts (Fonts* out);

} // namespace ui
