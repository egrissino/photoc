#include "ui/theme.h"

#include <string.h>

extern "C"
{
    extern const unsigned char pc_font_inter_regular[], pc_font_inter_regular_end[];
    extern const unsigned char pc_font_inter_medium[], pc_font_inter_medium_end[];
    extern const unsigned char pc_font_inter_semibold[], pc_font_inter_semibold_end[];
    extern const unsigned char pc_font_mono[], pc_font_mono_end[];
}

namespace ui
{

    /* The original's "Pro" design system: Photoshop's four interface brightness levels (flat
       charcoal panels, tab strips, a blue accent), Inter for text and JetBrains Mono for numbers.
       Widgets read the tokens instead of hard-coding colours, so every theme applies everywhere. */

    const ThemeKind THEMES[THEME_COUNT] = {
        ThemeKind::ProDark, ThemeKind::ProMedium, ThemeKind::ProLightGray, ThemeKind::ProLight};

    const char* theme_label (ThemeKind k)
    {
        switch (k)
        {
        case ThemeKind::ProDark:
            return "Pro (Dark)";
        case ThemeKind::ProMedium:
            return "Pro (Medium Gray)";
        case ThemeKind::ProLightGray:
            return "Pro (Light Gray)";
        case ThemeKind::ProLight:
            return "Pro (Light)";
        }
        return "";
    }

    const char* theme_id (ThemeKind k)
    {
        switch (k)
        {
        case ThemeKind::ProDark:
            return "pro";
        case ThemeKind::ProMedium:
            return "proMedium";
        case ThemeKind::ProLightGray:
            return "proLightGray";
        case ThemeKind::ProLight:
            return "proLight";
        }
        return "";
    }

    bool theme_from_id (const char* id, ThemeKind* out)
    {
        for (int i = 0; i < THEME_COUNT; i++)
        {
            if (strcmp (id, theme_id (THEMES[i])) == 0)
            {
                *out = THEMES[i];
                return true;
            }
        }
        return false;
    }

    ImU32 rgb (int r, int g, int b)
    {
        return IM_COL32 (r, g, b, 255);
    }

    /* Pro (Dark) is the base; the other brightness levels override its surfaces and text (sampled
       from Photoshop 2026 by the original). */
    Tokens tokens_for (ThemeKind k)
    {
        Tokens t;
        t.kind         = k;
        t.chrome       = rgb (50, 50, 50);
        t.canvas       = rgb (40, 40, 40);
        t.dock         = rgb (30, 30, 30);
        t.card         = rgb (50, 50, 50);
        t.card_border  = rgb (30, 30, 30);
        t.field        = rgb (36, 36, 36);
        t.field_border = rgb (74, 74, 74);
        t.hover        = rgb (66, 66, 66);
        t.pressed      = rgb (78, 78, 78);
        t.text         = rgb (222, 222, 222);
        t.text_dim     = rgb (178, 178, 178);
        t.text_faint   = rgb (128, 128, 128);
        t.icon         = rgb (200, 200, 200);
        t.accent       = rgb (55, 142, 240);
        t.accent_soft  = rgb (78, 78, 78);
        t.accent_text  = rgb (255, 255, 255);
        t.separator    = rgb (30, 30, 30);
        t.shadow       = IM_COL32 (0, 0, 0, 150);
        t.danger       = rgb (236, 91, 98);
        t.warning      = rgb (232, 176, 70);
        t.tab_strip    = rgb (38, 38, 38);
        t.row_selected = rgb (82, 82, 82);
        t.radius_sm    = 3.0f;
        t.radius       = 4.0f;
        t.radius_lg    = 6.0f;
        switch (k)
        {
        case ThemeKind::ProDark:
            break;
        case ThemeKind::ProMedium:
            t.chrome       = rgb (83, 83, 83);
            t.dock         = rgb (66, 66, 66);
            t.card         = rgb (83, 83, 83);
            t.card_border  = rgb (66, 66, 66);
            t.field        = rgb (69, 69, 69);
            t.field_border = rgb (104, 104, 104);
            t.hover        = rgb (98, 98, 98);
            t.pressed      = rgb (112, 112, 112);
            t.text         = rgb (238, 238, 238);
            t.text_dim     = rgb (212, 212, 212);
            t.text_faint   = rgb (160, 160, 160);
            t.icon         = rgb (226, 226, 226);
            t.accent_soft  = rgb (110, 110, 110);
            t.separator    = rgb (62, 62, 62);
            t.tab_strip    = rgb (74, 74, 74);
            t.row_selected = rgb (107, 107, 107);
            break;
        case ThemeKind::ProLightGray:
            t.chrome       = rgb (184, 184, 184);
            t.canvas       = rgb (160, 160, 160);
            t.dock         = rgb (168, 168, 168);
            t.card         = rgb (184, 184, 184);
            t.card_border  = rgb (160, 160, 160);
            t.field        = rgb (214, 214, 214);
            t.field_border = rgb (140, 140, 140);
            t.hover        = rgb (198, 198, 198);
            t.pressed      = rgb (170, 170, 170);
            t.text         = rgb (20, 20, 20);
            t.text_dim     = rgb (52, 52, 52);
            t.text_faint   = rgb (96, 96, 96);
            t.icon         = rgb (38, 38, 38);
            t.accent_soft  = rgb (208, 208, 208);
            t.accent_text  = rgb (0, 66, 150);
            t.separator    = rgb (158, 158, 158);
            t.shadow       = IM_COL32 (0, 0, 0, 70);
            t.tab_strip    = rgb (170, 170, 170);
            t.row_selected = rgb (212, 212, 212);
            break;
        case ThemeKind::ProLight:
            t.chrome       = rgb (240, 240, 240);
            t.canvas       = rgb (214, 214, 214);
            t.dock         = rgb (226, 226, 226);
            t.card         = rgb (240, 240, 240);
            t.card_border  = rgb (214, 214, 214);
            t.field        = rgb (255, 255, 255);
            t.field_border = rgb (186, 186, 186);
            t.hover        = rgb (226, 226, 226);
            t.pressed      = rgb (206, 206, 206);
            t.text         = rgb (20, 20, 20);
            t.text_dim     = rgb (64, 64, 64);
            t.text_faint   = rgb (120, 120, 120);
            t.icon         = rgb (46, 46, 46);
            t.accent_soft  = rgb (214, 214, 214);
            t.accent_text  = rgb (16, 98, 196);
            t.separator    = rgb (214, 214, 214);
            t.shadow       = IM_COL32 (0, 0, 0, 50);
            t.tab_strip    = rgb (226, 226, 226);
            t.row_selected = rgb (204, 204, 204);
            break;
        }
        return t;
    }

    ImVec4 vec (ImU32 c)
    {
        return ImGui::ColorConvertU32ToFloat4 (c);
    }

    ImVec4 with_alpha (ImU32 c, float a)
    {
        ImVec4 v = ImGui::ColorConvertU32ToFloat4 (c);
        v.w      = a;
        return v;
    }

    /* The tokens onto Dear ImGui's style: flat panels, field-coloured inputs with a border, the
       blue accent for sliders, checks and focus, Photoshop's small radii and tight spacing. */
    void apply_theme (const Tokens& t, float scale)
    {
        ImGuiStyle& s                         = ImGui::GetStyle ();
        s                                     = ImGuiStyle ();
        s.WindowPadding                       = ImVec2 (8, 8);
        s.FramePadding                        = ImVec2 (8, 4);
        s.CellPadding                         = ImVec2 (6, 3);
        s.ItemSpacing                         = ImVec2 (8, 6);
        s.ItemInnerSpacing                    = ImVec2 (6, 4);
        s.IndentSpacing                       = 14;
        s.ScrollbarSize                       = 10;
        s.GrabMinSize                         = 10;
        s.WindowBorderSize                    = 1;
        s.ChildBorderSize                     = 1;
        s.PopupBorderSize                     = 1;
        s.FrameBorderSize                     = 1;
        s.TabBorderSize                       = 0;
        s.WindowRounding                      = t.radius_lg;
        s.ChildRounding                       = t.radius;
        s.FrameRounding                       = t.radius_sm;
        s.PopupRounding                       = t.radius;
        s.ScrollbarRounding                   = 6;
        s.GrabRounding                        = t.radius_sm;
        s.TabRounding                         = t.radius_sm;
        s.WindowTitleAlign                    = ImVec2 (0.0f, 0.5f);
        s.SeparatorTextBorderSize             = 1;
        s.DockingSeparatorSize                = 2;
        ImVec4* c                             = s.Colors;
        c[ImGuiCol_Text]                      = vec (t.text);
        c[ImGuiCol_TextDisabled]              = vec (t.text_faint);
        c[ImGuiCol_WindowBg]                  = vec (t.card);
        c[ImGuiCol_ChildBg]                   = vec (t.card);
        c[ImGuiCol_PopupBg]                   = vec (t.card);
        c[ImGuiCol_Border]                    = vec (t.card_border);
        c[ImGuiCol_BorderShadow]              = ImVec4 (0, 0, 0, 0);
        c[ImGuiCol_FrameBg]                   = vec (t.field);
        c[ImGuiCol_FrameBgHovered]            = vec (t.hover);
        c[ImGuiCol_FrameBgActive]             = vec (t.pressed);
        c[ImGuiCol_TitleBg]                   = vec (t.tab_strip);
        c[ImGuiCol_TitleBgActive]             = vec (t.tab_strip);
        c[ImGuiCol_TitleBgCollapsed]          = vec (t.tab_strip);
        c[ImGuiCol_MenuBarBg]                 = vec (t.chrome);
        c[ImGuiCol_ScrollbarBg]               = ImVec4 (0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]             = vec (t.field_border);
        c[ImGuiCol_ScrollbarGrabHovered]      = vec (t.text_faint);
        c[ImGuiCol_ScrollbarGrabActive]       = vec (t.text_dim);
        c[ImGuiCol_CheckMark]                 = vec (t.text);
        c[ImGuiCol_SliderGrab]                = vec (t.text_dim);
        c[ImGuiCol_SliderGrabActive]          = vec (t.accent);
        c[ImGuiCol_Button]                    = vec (t.field);
        c[ImGuiCol_ButtonHovered]             = vec (t.hover);
        c[ImGuiCol_ButtonActive]              = vec (t.pressed);
        c[ImGuiCol_Header]                    = vec (t.row_selected);
        c[ImGuiCol_HeaderHovered]             = vec (t.hover);
        c[ImGuiCol_HeaderActive]              = vec (t.pressed);
        c[ImGuiCol_Separator]                 = vec (t.separator);
        c[ImGuiCol_SeparatorHovered]          = vec (t.accent);
        c[ImGuiCol_SeparatorActive]           = vec (t.accent);
        c[ImGuiCol_ResizeGrip]                = ImVec4 (0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered]         = with_alpha (t.accent, 0.6f);
        c[ImGuiCol_ResizeGripActive]          = vec (t.accent);
        c[ImGuiCol_InputTextCursor]           = vec (t.text);
        c[ImGuiCol_Tab]                       = vec (t.tab_strip);
        c[ImGuiCol_TabHovered]                = vec (t.hover);
        c[ImGuiCol_TabSelected]               = vec (t.card);
        c[ImGuiCol_TabSelectedOverline]       = vec (t.card);
        c[ImGuiCol_TabDimmed]                 = vec (t.tab_strip);
        c[ImGuiCol_TabDimmedSelected]         = vec (t.card);
        c[ImGuiCol_TabDimmedSelectedOverline] = vec (t.card);
        c[ImGuiCol_DockingPreview]            = with_alpha (t.accent, 0.5f);
        c[ImGuiCol_DockingEmptyBg]            = vec (t.dock);
        c[ImGuiCol_PlotLines]                 = vec (t.text_dim);
        c[ImGuiCol_PlotLinesHovered]          = vec (t.accent);
        c[ImGuiCol_PlotHistogram]             = vec (t.text_dim);
        c[ImGuiCol_PlotHistogramHovered]      = vec (t.accent);
        c[ImGuiCol_TableHeaderBg]             = vec (t.tab_strip);
        c[ImGuiCol_TableBorderStrong]         = vec (t.separator);
        c[ImGuiCol_TableBorderLight]          = vec (t.separator);
        c[ImGuiCol_TableRowBg]                = ImVec4 (0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt]             = ImVec4 (0, 0, 0, 0);
        c[ImGuiCol_TextLink]                  = vec (t.accent);
        c[ImGuiCol_TextSelectedBg]            = with_alpha (t.accent, 0.4f);
        c[ImGuiCol_TreeLines]                 = vec (t.separator);
        c[ImGuiCol_DragDropTarget]            = vec (t.accent);
        c[ImGuiCol_NavCursor]                 = vec (t.accent);
        c[ImGuiCol_NavWindowingHighlight]     = with_alpha (t.accent, 0.7f);
        c[ImGuiCol_NavWindowingDimBg]         = ImVec4 (0, 0, 0, 0.3f);
        /* Dialogs don't dim the window behind them (as in Photoshop), so previews show their true
           effect. */
        c[ImGuiCol_ModalWindowDimBg] = ImVec4 (0, 0, 0, 0.0f);
        s.ScaleAllSizes (scale);
    }

    /* Inter at the Pro layout's 12-13 px (ImGui sizes fonts by line height, so 13 here matches the
       original's 12 pt), Medium and SemiBold for headings and labels, JetBrains Mono for numbers.
       The data lives in the program (embedded.S), so the atlas must not free it. */
    void load_fonts (Fonts* out)
    {
        ImGuiIO& io = ImGui::GetIO ();
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.OversampleH          = 2;
        cfg.OversampleV          = 2;
        out->regular             = io.Fonts->AddFontFromMemoryTTF (
            (void*) pc_font_inter_regular, (int) (pc_font_inter_regular_end - pc_font_inter_regular), 13.0f, &cfg);
        out->medium = io.Fonts->AddFontFromMemoryTTF (
            (void*) pc_font_inter_medium, (int) (pc_font_inter_medium_end - pc_font_inter_medium), 13.0f, &cfg);
        out->semibold = io.Fonts->AddFontFromMemoryTTF (
            (void*) pc_font_inter_semibold, (int) (pc_font_inter_semibold_end - pc_font_inter_semibold), 13.0f, &cfg);
        out->mono =
            io.Fonts->AddFontFromMemoryTTF ((void*) pc_font_mono, (int) (pc_font_mono_end - pc_font_mono), 13.0f, &cfg);
        io.FontDefault = out->regular;
    }

} // namespace ui
