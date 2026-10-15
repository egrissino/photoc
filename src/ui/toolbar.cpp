#include "ui/app.h"
#include "ui/widgets.h"
#include <float.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <math.h>
#include <string.h>

namespace ui
{

    /* The toolbar (Photoshop's single column of slots: each slot a flyout group showing its last
       used tool, right-click lists the group), the colour chips under it, and the options bar along
       the top, which shows the current tool's settings in a row. */

    struct ToolInfo
    {
        const char* label;
        const char* icon;
        const char* key;
    };

    /* Indexed by Tool. */
    const ToolInfo TOOL_INFO[TOOL_COUNT] = {
        {"Move Tool", "move", "V"},
        {"Rectangular Marquee Tool", "square-dashed", "M"},
        {"Elliptical Marquee Tool", "circle-dashed", "M"},
        {"Lasso Tool", "lasso", "L"},
        {"Polygonal Lasso Tool", "pentagon", "L"},
        {"Magic Wand Tool", "wand-sparkles", "W"},
        {"Gradient Tool", "blend", "G"},
        {"Paint Bucket Tool", "paint-bucket", "G"},
        {"Crop Tool", "crop", "C"},
        {"Brush Tool", "brush", "B"},
        {"Pencil Tool", "pencil", "B"},
        {"Eraser Tool", "eraser", "E"},
        {"Clone Stamp Tool", "stamp", "S"},
        {"Spot Healing Brush Tool", "bandage", "J"},
        {"Healing Brush Tool", "bandage", "J"},
        {"History Brush Tool", "clock", "Y"},
        {"Dodge Tool", "lollipop", "O"},
        {"Burn Tool", "flame", "O"},
        {"Sponge Tool", "cloud", "O"},
        {"Blur Tool", "droplet", ""},
        {"Sharpen Tool", "triangle", ""},
        {"Smudge Tool", "pointer", ""},
        {"Hand Tool", "hand", "H"},
        {"Zoom Tool", "zoom-in", "Z"},
        {"Magic Eraser Tool", "eraser-magic", "E"},
        {"Eyedropper Tool", "pipette", "I"},
        {"Quick Selection Tool", "lasso-select", "W"},
        {"Horizontal Type Tool", "type", "T"},
        {"Pen Tool", "pen-tool", "P"},
        {"Path Selection Tool", "pointer", "A"},
        {"Direct Selection Tool", "direct-select", "A"},
        {"Rectangle Tool", "square", "U"},
        {"Ellipse Tool", "circle", "U"},
        {"Polygon Tool", "pentagon", "U"},
        {"Line Tool", "slash", "U"},
        {"Color Replacement Tool", "brush-replace", "B"},
        {"Mixer Brush Tool", "palette", "B"},
        {"Pattern Stamp Tool", "stamp", "S"},
        {"Art History Brush Tool", "sparkles", "Y"},
        {"Background Eraser Tool", "eraser-background", "E"},
        {"Remove Tool", "bandage-sparkle", "J"},
        {"Patch Tool", "square-dashed-mouse-pointer", "J"},
        {"Content-Aware Move Tool", "scaling", "J"},
        {"Red Eye Tool", "eye", "J"},
    };

    /* The slots in Photoshop's order (only the tools this build has). */
    const int SLOT_DEPTH                          = 6;
    const Tool SLOT_TOOLS[SLOT_COUNT][SLOT_DEPTH] = {
        {Tool::Move},
        {Tool::RectMarquee, Tool::EllipseMarquee},
        {Tool::Lasso, Tool::PolygonLasso},
        {Tool::QuickSelection, Tool::MagicWand},
        {Tool::Crop},
        {Tool::Eyedropper},
        {Tool::SpotHealing, Tool::Remove, Tool::HealingBrush, Tool::Patch, Tool::ContentAwareMove, Tool::RedEye},
        {Tool::Brush, Tool::Pencil, Tool::ColorReplacement, Tool::MixerBrush},
        {Tool::CloneStamp, Tool::PatternStamp},
        {Tool::HistoryBrush, Tool::ArtHistory},
        {Tool::Eraser, Tool::BackgroundEraser, Tool::MagicEraser},
        {Tool::Gradient, Tool::PaintBucket},
        {Tool::Blur, Tool::Sharpen, Tool::Smudge},
        {Tool::Dodge, Tool::Burn, Tool::Sponge},
        {Tool::Pen},
        {Tool::Type},
        {Tool::PathSelect, Tool::DirectSelect},
        {Tool::RectShape, Tool::EllipseShape, Tool::PolygonShape, Tool::LineShape},
        {Tool::Hand},
        {Tool::Zoom},
    };
    const int SLOT_SIZES[SLOT_COUNT] = {1, 2, 2, 2, 1, 1, 6, 4, 2, 2, 3, 2, 3, 3, 1, 1, 2, 4, 1, 1};

    const char* tool_label (Tool t)
    {
        return TOOL_INFO[(int) t].label;
    }

    int slot_of (Tool t)
    {
        for (int s = 0; s < SLOT_COUNT; s++)
        {
            for (int i = 0; i < SLOT_SIZES[s]; i++)
            {
                if (SLOT_TOOLS[s][i] == t)
                {
                    return s;
                }
            }
        }
        return -1;
    }

    /* Options-bar rows: a label before the next control, and the gap between groups of settings. */
    void opt_label (const char* text)
    {
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted (text);
        ImGui::SameLine ();
    }

    void opt_gap ()
    {
        ImGui::SameLine (0, 14.0f * ImGui::GetStyle ().FontScaleMain);
    }

    /* Choosing a tool also makes it its slot's (and its shortcut group's) remembered tool. */
    void App::select_tool (Tool t)
    {
        if (type_active_ && t != Tool::Type)
        {
            commit_type_edit ();
        }
        if (t != Tool::Pen)
        {
            end_pen_drawing ();
        }
        tool_ = t;
        int s = slot_of (t);
        if (s >= 0)
        {
            slot_last_[s] = t;
        }
        if (t == Tool::Brush || t == Tool::Pencil || t == Tool::ColorReplacement || t == Tool::MixerBrush)
        {
            brush_tool_ = t;
        }
        if (t == Tool::Lasso || t == Tool::PolygonLasso)
        {
            lasso_tool_ = t;
        }
        if (t == Tool::Gradient || t == Tool::PaintBucket)
        {
            fill_tool_ = t;
        }
        if (t == Tool::SpotHealing || t == Tool::HealingBrush || t == Tool::Remove || t == Tool::Patch
            || t == Tool::ContentAwareMove || t == Tool::RedEye)
        {
            heal_tool_ = t;
        }
        if (t == Tool::Dodge || t == Tool::Burn || t == Tool::Sponge)
        {
            tone_tool_ = t;
        }
    }

    /* A square icon button: the selected tool on the soft accent, hover highlighted. */
    bool App::icon_button (const char* id, const char* icon, float box, bool selected, const char* tooltip)
    {
        ImVec2 p       = ImGui::GetCursorScreenPos ();
        bool pressed   = ImGui::InvisibleButton (id, ImVec2 (box, box));
        bool hovered   = ImGui::IsItemHovered ();
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        ImVec2 q (p.x + box, p.y + box);
        if (selected)
        {
            dl->AddRectFilled (p, q, theme_.accent_soft, theme_.radius_sm);
        }
        else if (hovered)
        {
            dl->AddRectFilled (p, q, theme_.hover, theme_.radius_sm);
        }
        float glyph = roundf (box * 2.0f / 3.0f);
        draw_icon (dl, &icons_, icon, ImVec2 (p.x + (box - glyph) * 0.5f, p.y + (box - glyph) * 0.5f), glyph,
            selected ? theme_.text : theme_.icon);
        if (hovered && tooltip != NULL && tooltip[0] != 0)
        {
            ImGui::SetTooltip ("%s", tooltip);
        }
        return pressed;
    }

    void App::toolbar (ImVec2 pos, ImVec2 size)
    {
        ImGui::SetNextWindowPos (pos);
        ImGui::SetNextWindowSize (size);
        ImGui::PushStyleColor (ImGuiCol_WindowBg, theme_.chrome);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (5.0f * ui_scale_, 8.0f * ui_scale_));
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (2.0f * ui_scale_, 3.0f * ui_scale_));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking
                               | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::Begin ("##toolbar", NULL, flags);
        float box = 30.0f * ui_scale_;
        for (int s = 0; s < SLOT_COUNT; s++)
        {
            int si               = slot_of (tool_);
            Tool shown           = si == s ? tool_ : slot_last_[s];
            const ToolInfo& info = TOOL_INFO[(int) shown];
            char tip[96], id[16];
            if (info.key[0] != 0)
            {
                snprintf (tip, sizeof tip, "%s  (%s)", info.label, info.key);
            }
            else
            {
                snprintf (tip, sizeof tip, "%s", info.label);
            }
            snprintf (id, sizeof id, "##slot%d", s);
            if (icon_button (id, info.icon, box, si == s, tip))
            {
                select_tool (shown);
            }
            ImVec2 a = ImGui::GetItemRectMin (), b = ImGui::GetItemRectMax ();
            if (SLOT_SIZES[s] > 1)
            {
                /* The flyout corner, and the group on right-click. */
                ImDrawList* dl = ImGui::GetWindowDrawList ();
                float k        = ui_scale_;
                dl->AddTriangleFilled (ImVec2 (b.x - 2 * k, b.y - 2 * k), ImVec2 (b.x - 6 * k, b.y - 2 * k),
                    ImVec2 (b.x - 2 * k, b.y - 6 * k), theme_.text_faint);
                char pid[16];
                snprintf (pid, sizeof pid, "flyout%d", s);
                if (ImGui::IsItemClicked (ImGuiMouseButton_Right))
                {
                    ImGui::OpenPopup (pid);
                }
                ImGui::SetNextWindowPos (ImVec2 (b.x + 6 * k, a.y));
                if (ImGui::BeginPopup (pid))
                {
                    for (int i = 0; i < SLOT_SIZES[s]; i++)
                    {
                        Tool t             = SLOT_TOOLS[s][i];
                        const ToolInfo& ti = TOOL_INFO[(int) t];
                        ImGui::PushID (i);
                        ImVec2 rp = ImGui::GetCursorScreenPos ();
                        float rw = 230.0f * k, rh = 26.0f * k;
                        if (ImGui::Selectable ("##item", false, 0, ImVec2 (rw, rh)))
                        {
                            select_tool (t);
                        }
                        ImDrawList* pl = ImGui::GetWindowDrawList ();
                        if (t == shown)
                        {
                            pl->AddRectFilled (ImVec2 (rp.x + 2 * k, rp.y + rh * 0.5f - 2 * k),
                                ImVec2 (rp.x + 6 * k, rp.y + rh * 0.5f + 2 * k), theme_.text);
                        }
                        float g = 16.0f * k;
                        draw_icon (
                            pl, &icons_, ti.icon, ImVec2 (rp.x + 14 * k, rp.y + (rh - g) * 0.5f), g, theme_.icon);
                        float th = ImGui::GetTextLineHeight ();
                        pl->AddText (ImVec2 (rp.x + 40 * k, rp.y + (rh - th) * 0.5f), theme_.text, ti.label);
                        float kw = ImGui::CalcTextSize (ti.key).x;
                        pl->AddText (ImVec2 (rp.x + rw - kw - 8 * k, rp.y + (rh - th) * 0.5f), theme_.text_dim, ti.key);
                        ImGui::PopID ();
                    }
                    ImGui::EndPopup ();
                }
            }
        }
        ImGui::Dummy (ImVec2 (box, 6.0f * ui_scale_));
        color_chips ();
        ImGui::Dummy (ImVec2 (box, 2.0f * ui_scale_));
        if (icon_button ("##quickmask", "layer-mask", box, quick_mask_,
                quick_mask_ ? "Edit in Standard Mode  (Q)" : "Edit in Quick Mask Mode  (Q)"))
        {
            toggle_quick_mask ();
        }
        ImGui::End ();
        ImGui::PopStyleVar (3);
        ImGui::PopStyleColor ();
    }

    /* Photoshop's colour chips: foreground over background; the arrow swaps them, the small pair
       resets black and white. Clicking a chip opens its picker. */
    void App::color_chips ()
    {
        float k = ui_scale_, chip = 18.0f * k, off = 9.0f * k;
        ImVec2 p        = ImGui::GetCursorScreenPos ();
        ImDrawList* dl  = ImGui::GetWindowDrawList ();
        const float *fg = session_.tools.foreground, *bg = session_.tools.background;
        ImVec2 bg0 (p.x + off + 2 * k, p.y + off + 4 * k), fg0 (p.x + 2 * k, p.y + 4 * k);
        ImGui::SetCursorScreenPos (bg0);
        if (ImGui::InvisibleButton ("##bgchip", ImVec2 (chip, chip)))
        {
            open_color_picker (true);
        }
        if (ImGui::IsItemHovered ())
        {
            ImGui::SetTooltip ("Set Background Color");
        }
        ImGui::SetCursorScreenPos (fg0);
        if (ImGui::InvisibleButton ("##fgchip", ImVec2 (chip, chip)))
        {
            open_color_picker (false);
        }
        if (ImGui::IsItemHovered ())
        {
            ImGui::SetTooltip ("Set Foreground Color");
        }
        ImU32 border = IM_COL32 (0, 0, 0, 255), inner = IM_COL32 (255, 255, 255, 255);
        dl->AddRectFilled (bg0, ImVec2 (bg0.x + chip, bg0.y + chip),
            ImGui::ColorConvertFloat4ToU32 (ImVec4 (bg[0], bg[1], bg[2], 1.0f)));
        dl->AddRect (bg0, ImVec2 (bg0.x + chip, bg0.y + chip), border);
        dl->AddRect (ImVec2 (bg0.x + 1, bg0.y + 1), ImVec2 (bg0.x + chip - 1, bg0.y + chip - 1), inner);
        dl->AddRectFilled (fg0, ImVec2 (fg0.x + chip, fg0.y + chip),
            ImGui::ColorConvertFloat4ToU32 (ImVec4 (fg[0], fg[1], fg[2], 1.0f)));
        dl->AddRect (fg0, ImVec2 (fg0.x + chip, fg0.y + chip), border);
        dl->AddRect (ImVec2 (fg0.x + 1, fg0.y + 1), ImVec2 (fg0.x + chip - 1, fg0.y + chip - 1), inner);
        /* Swap (top right) and default (bottom left). */
        float small = 9.0f * k;
        ImVec2 sw (fg0.x + chip + 1 * k, fg0.y - 3 * k);
        ImGui::SetCursorScreenPos (sw);
        if (ImGui::InvisibleButton ("##swap", ImVec2 (small, small)))
        {
            run_command ("tools.swapColors", engine::Value::object ());
        }
        if (ImGui::IsItemHovered ())
        {
            ImGui::SetTooltip ("Switch Foreground and Background Colors  (X)");
        }
        draw_icon (dl, &icons_, "arrow-left-right", sw, small, theme_.icon);
        ImVec2 df (fg0.x - 1 * k, bg0.y + chip - small + 2 * k);
        ImGui::SetCursorScreenPos (df);
        if (ImGui::InvisibleButton ("##default", ImVec2 (small, small)))
        {
            run_command ("tools.defaultColors", engine::Value::object ());
        }
        if (ImGui::IsItemHovered ())
        {
            ImGui::SetTooltip ("Default Foreground and Background Colors  (D)");
        }
        float m = small * 0.55f;
        dl->AddRectFilled (ImVec2 (df.x + small - m, df.y + small - m), ImVec2 (df.x + small, df.y + small),
            IM_COL32 (255, 255, 255, 255));
        dl->AddRect (ImVec2 (df.x + small - m, df.y + small - m), ImVec2 (df.x + small, df.y + small), border);
        dl->AddRectFilled (df, ImVec2 (df.x + m, df.y + m), IM_COL32 (0, 0, 0, 255));
        dl->AddRect (df, ImVec2 (df.x + m, df.y + m), theme_.icon);
        ImGui::SetCursorScreenPos (ImVec2 (p.x, bg0.y + chip + 4 * k));
        ImGui::Dummy (ImVec2 (1, 1));
    }

    /* A round brush tip: solid to `hardness` of its radius, fading to the edge (rings from the
       outside in, each as opaque as the tip is at its radius). */
    void brush_tip_glyph (ImDrawList* dl, ImVec2 c, float radius, float hardness, ImU32 col)
    {
        const int RINGS = 10;
        ImVec4 base     = ImGui::ColorConvertU32ToFloat4 (col);
        for (int i = 0; i < RINGS; i++)
        {
            float f = 1.0f - (float) i / RINGS;
            float a = f <= hardness || hardness >= 1.0f ? 1.0f : 1.0f - (f - hardness) / (1.0f - hardness);
            dl->AddCircleFilled (c, fmaxf (radius * f, 0.5f),
                ImGui::ColorConvertFloat4ToU32 (
                    ImVec4 (base.x, base.y, base.z, base.w * a * 0.35f + (a >= 1.0f ? 0.65f * base.w : 0.0f))),
                32);
        }
    }

    /* Photoshop's General Brushes: hard round tips, then soft. */
    struct BrushPreset
    {
        float size;
        float hardness;
    };

    const BrushPreset BRUSH_PRESETS[] = {{1, 1}, {3, 1}, {5, 1}, {9, 1}, {13, 1}, {19, 1}, {5, 0}, {9, 0}, {13, 0},
        {17, 0}, {21, 0}, {27, 0}, {35, 0}, {45, 0}, {65, 0}, {100, 0}, {200, 0}, {300, 0}, {500, 0}, {900, 0}, {25, 1},
        {50, 1}, {100, 1}, {200, 1}};
    const int BRUSH_PRESET_COUNT      = sizeof BRUSH_PRESETS / sizeof BRUSH_PRESETS[0];

    /* The brush preset picker, as in Photoshop's options bar: the tip with its size under it and a
       dropdown arrow; the popup has Size and Hardness and the preset tips. Sets *active while a
       slider is held and *done when one is let go (a preset sets the brush at once). */
    void App::brush_picker (bool pencil, bool* active, bool* done)
    {
        float k   = ui_scale_;
        float bar = ImGui::GetWindowHeight ();
        float w = 52.0f * k, h = fminf (bar - 4.0f * k, 40.0f * k);
        ImVec2 p = ImGui::GetCursorScreenPos ();
        float y0 = ImGui::GetWindowPos ().y + (bar - h) * 0.5f;
        ImGui::SetCursorScreenPos (ImVec2 (p.x, y0));
        bool open      = ImGui::InvisibleButton ("##brushpicker", ImVec2 (w, h));
        bool hovered   = ImGui::IsItemHovered ();
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        ImVec2 a (p.x, y0), b (p.x + w, y0 + h);
        if (hovered)
        {
            dl->AddRectFilled (a, b, ImGui::GetColorU32 (ImGuiCol_FrameBgHovered), 3.0f * k);
        }
        /* The tip, larger for larger brushes (log scale), and the size under it. */
        float tip_area = h * 0.62f;
        float t        = logf (fmaxf (brush_edit_.size, 1.0f)) / logf (1000.0f);
        float r        = fmaxf (1.5f * k, fminf (t, 1.0f) * tip_area * 0.5f);
        float tip_w    = w - 12.0f * k;
        ImVec2 c (a.x + tip_w * 0.5f + 2.0f * k, a.y + 2.0f * k + tip_area * 0.5f);
        brush_tip_glyph (dl, c, r, pencil ? 1.0f : brush_edit_.hardness, theme_.icon);
        char num[16];
        snprintf (num, sizeof num, "%.0f", brush_edit_.size);
        float fs  = 10.5f * k;
        ImVec2 ts = fonts_.regular->CalcTextSizeA (fs, FLT_MAX, 0.0f, num);
        dl->AddText (fonts_.regular, fs, ImVec2 (a.x + 2.0f * k + (tip_w - ts.x) * 0.5f, b.y - ts.y - 1.0f * k),
            theme_.icon, num);
        /* The dropdown arrow. */
        float ax = b.x - 8.0f * k, ay = (a.y + b.y) * 0.5f;
        dl->AddTriangleFilled (ImVec2 (ax - 3.5f * k, ay - 1.5f * k), ImVec2 (ax + 3.5f * k, ay - 1.5f * k),
            ImVec2 (ax, ay + 2.5f * k), theme_.icon);
        if (hovered && !ImGui::IsPopupOpen ("##brushpopup"))
        {
            ImGui::SetTooltip ("Click to open the Brush Preset picker");
        }
        if (open)
        {
            ImGui::OpenPopup ("##brushpopup");
        }
        ImGui::SetCursorScreenPos (ImVec2 (b.x, p.y));
        ImGui::Dummy (ImVec2 (0, 0));
        ImGui::SetNextWindowPos (ImVec2 (a.x, b.y + 2.0f * k));
        if (!ImGui::BeginPopup ("##brushpopup"))
        {
            return;
        }
        float hardness = brush_edit_.hardness * 100.0f;
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Size:");
        ImGui::SameLine (80 * k);
        ImGui::SetNextItemWidth (200 * k);
        ImGui::SliderFloat ("##pickersize", &brush_edit_.size, 1.0f, 5000.0f, "%.0f px",
            ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        *active = *active || ImGui::IsItemActive ();
        *done   = *done || ImGui::IsItemDeactivatedAfterEdit ();
        if (pencil)
        {
            ImGui::BeginDisabled ();
        }
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Hardness:");
        ImGui::SameLine (80 * k);
        ImGui::SetNextItemWidth (200 * k);
        ImGui::SliderFloat ("##pickerhardness", &hardness, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        *active = *active || ImGui::IsItemActive ();
        *done   = *done || ImGui::IsItemDeactivatedAfterEdit ();
        if (pencil)
        {
            ImGui::EndDisabled ();
        }
        brush_edit_.hardness = hardness / 100.0f;
        ImGui::Separator ();
        /* The preset tips, six to a row, each with its size. */
        float cell = 46.0f * k;
        for (int i = 0; i < BRUSH_PRESET_COUNT; i++)
        {
            const BrushPreset& bp = BRUSH_PRESETS[i];
            if (i % 6 != 0)
            {
                ImGui::SameLine (0, 2 * k);
            }
            ImGui::PushID (i);
            ImVec2 q = ImGui::GetCursorScreenPos ();
            bool chosen =
                fabsf (bp.size - brush_edit_.size) < 0.5f && fabsf (bp.hardness - brush_edit_.hardness) < 0.01f;
            if (ImGui::InvisibleButton ("##preset", ImVec2 (cell, cell)))
            {
                brush_edit_.size     = bp.size;
                brush_edit_.hardness = bp.hardness;
                *done                = true;
            }
            ImDrawList* pl = ImGui::GetWindowDrawList ();
            if (chosen)
            {
                pl->AddRect (q, ImVec2 (q.x + cell, q.y + cell), theme_.accent, 2.0f * k, 0, 1.5f * k);
            }
            else if (ImGui::IsItemHovered ())
            {
                pl->AddRectFilled (
                    q, ImVec2 (q.x + cell, q.y + cell), ImGui::GetColorU32 (ImGuiCol_FrameBgHovered), 2.0f * k);
            }
            float pt = logf (fmaxf (bp.size, 1.0f)) / logf (1000.0f);
            float pr = fmaxf (1.0f * k, fminf (pt, 1.0f) * cell * 0.32f);
            brush_tip_glyph (pl, ImVec2 (q.x + cell * 0.5f, q.y + cell * 0.40f), pr, bp.hardness, theme_.icon);
            char label[16];
            snprintf (label, sizeof label, "%.0f", bp.size);
            ImVec2 ls = fonts_.regular->CalcTextSizeA (fs, FLT_MAX, 0.0f, label);
            pl->AddText (fonts_.regular, fs, ImVec2 (q.x + (cell - ls.x) * 0.5f, q.y + cell - ls.y - 2.0f * k),
                theme_.icon, label);
            if (ImGui::IsItemHovered ())
            {
                ImGui::SetTooltip ("%s Round, %.0f px", bp.hardness >= 1.0f ? "Hard" : "Soft", bp.size);
            }
            ImGui::PopID ();
        }
        ImGui::EndPopup ();
    }

    /* The brush settings in the options bar: shown from the session, edited in a local copy while
       a slider is held, then set with one tools.setBrush. */
    void App::brush_options ()
    {
        float k = ui_scale_;
        if (!brush_editing_)
        {
            brush_edit_ = session_.tools.brush;
        }
        bool active = false, done = false;
        float hardness = brush_edit_.hardness * 100.0f, opacity = brush_edit_.opacity * 100.0f,
              flow = brush_edit_.flow * 100.0f;
        if (tool_ == Tool::Eraser)
        {
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ("Mode:");
            ImGui::SameLine ();
            ImGui::SetNextItemWidth (90 * k);
            ImGui::Combo ("##erasermode", &eraser_mode_, "Brush\0Pencil\0Block\0");
            ImGui::SameLine (0, 14 * k);
        }
        bool pencil = tool_ == Tool::Pencil || (tool_ == Tool::Eraser && eraser_mode_ == 1);
        bool block  = tool_ == Tool::Eraser && eraser_mode_ == 2;
        if (block)
        {
            ImGui::BeginDisabled ();
        }
        brush_edit_.hardness = hardness / 100.0f;
        brush_picker (pencil, &active, &done);
        hardness = brush_edit_.hardness * 100.0f;
        ImGui::SameLine (0, 14 * k);
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Opacity:");
        ImGui::SameLine ();
        ImGui::SetNextItemWidth (90 * k);
        ImGui::DragFloat ("##opacity", &opacity, 0.4f, 1.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        active = active || ImGui::IsItemActive ();
        done   = done || ImGui::IsItemDeactivatedAfterEdit ();
        if (!pencil)
        {
            ImGui::SameLine (0, 14 * k);
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ("Flow:");
            ImGui::SameLine ();
            ImGui::SetNextItemWidth (90 * k);
            ImGui::DragFloat ("##flow", &flow, 0.4f, 1.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            active = active || ImGui::IsItemActive ();
            done   = done || ImGui::IsItemDeactivatedAfterEdit ();
        }
        if (block)
        {
            ImGui::EndDisabled ();
        }
        brush_edit_.hardness = hardness / 100.0f;
        brush_edit_.opacity  = opacity / 100.0f;
        brush_edit_.flow     = flow / 100.0f;
        brush_editing_       = active;
        if (done)
        {
            run_command ("tools.setBrush", engine::Value{{"size", brush_edit_.size}, {"hardness", brush_edit_.hardness},
                                               {"opacity", brush_edit_.opacity}, {"flow", brush_edit_.flow}});
        }
        if (tool_ == Tool::Pencil)
        {
            ImGui::SameLine (0, 14 * k);
            ImGui::Checkbox ("Auto Erase", &pencil_auto_erase_);
        }
        if (block)
        {
            ImGui::SameLine (0, 14 * k);
            ImGui::AlignTextToFramePadding ();
            ImGui::TextDisabled ("Block: a 16 px square on screen");
        }
        if (mask_targeted ())
        {
            ImGui::SameLine (0, 14 * k);
            ImGui::AlignTextToFramePadding ();
            ImGui::TextColored (ImGui::ColorConvertU32ToFloat4 (theme_.accent), "Painting on the layer mask");
        }
    }

    bool uses_brush (Tool t);

    /* An align / distribute glyph: a guide line and two bars against it (kinds 0-5: top, vertical
       centre, bottom, left, horizontal centre, right), or three bars evenly spaced (6 across, 7
       down). */
    void align_glyph (ImDrawList* dl, ImVec2 p, float s, int kind, ImU32 col)
    {
        float t = s * 0.1f;
        if (kind >= 6)
        {
            for (int i = 0; i < 3; i++)
            {
                float o = s * (0.15f + 0.3f * (float) i);
                if (kind == 6)
                {
                    dl->AddRectFilled (
                        ImVec2 (p.x + o, p.y + s * 0.25f), ImVec2 (p.x + o + s * 0.12f, p.y + s * 0.75f), col);
                }
                else
                {
                    dl->AddRectFilled (
                        ImVec2 (p.x + s * 0.25f, p.y + o), ImVec2 (p.x + s * 0.75f, p.y + o + s * 0.12f), col);
                }
            }
            return;
        }
        bool v        = kind < 3;
        int at        = kind % 3;
        float line    = at == 0 ? s * 0.1f : at == 1 ? s * 0.5f : s * 0.9f;
        float lens[2] = {s * 0.6f, s * 0.35f};
        for (int i = 0; i < 2; i++)
        {
            float a = s * (0.22f + 0.36f * (float) i), w = s * 0.2f, len = lens[i];
            float start = at == 0 ? line : at == 1 ? line - len * 0.5f : line - len;
            if (v)
            {
                dl->AddRectFilled (ImVec2 (p.x + a, p.y + start), ImVec2 (p.x + a + w, p.y + start + len), col);
            }
            else
            {
                dl->AddRectFilled (ImVec2 (p.x + start, p.y + a), ImVec2 (p.x + start + len, p.y + a + w), col);
            }
        }
        if (v)
        {
            dl->AddLine (ImVec2 (p.x + s * 0.05f, p.y + line), ImVec2 (p.x + s * 0.95f, p.y + line), col, t);
        }
        else
        {
            dl->AddLine (ImVec2 (p.x + line, p.y + s * 0.05f), ImVec2 (p.x + line, p.y + s * 0.95f), col, t);
        }
    }

    /* The Move tool's options: align the selected layers (to each other, or one layer to the
       selection or canvas) and distribute three or more. */
    void App::move_options ()
    {
        float k = ui_scale_, box = ImGui::GetFrameHeight ();
        const char* const ALIGN[6] = {"layer.align.topEdges", "layer.align.verticalCenters", "layer.align.bottomEdges",
            "layer.align.leftEdges", "layer.align.horizontalCenters", "layer.align.rightEdges"};
        const char* const TIPS[8]  = {"Align top edges", "Align vertical centers", "Align bottom edges",
             "Align left edges", "Align horizontal centers", "Align right edges", "Distribute horizontally (equal gaps)",
             "Distribute vertically (equal gaps)"};
        const engine::DocState* d  = session_.active ();
        size_t count               = d != NULL ? d->selected ().size () : 0;
        for (int i = 0; i < 8; i++)
        {
            if (i == 6)
            {
                opt_gap ();
            }
            else if (i > 0)
            {
                ImGui::SameLine (0, 2 * k);
            }
            bool enabled = i < 6 ? count >= 1 : count >= 3;
            if (!enabled)
            {
                ImGui::BeginDisabled ();
            }
            char id[16];
            snprintf (id, sizeof id, "##align%d", i);
            ImVec2 p       = ImGui::GetCursorScreenPos ();
            bool pressed   = ImGui::InvisibleButton (id, ImVec2 (box, box));
            ImDrawList* dl = ImGui::GetWindowDrawList ();
            if (ImGui::IsItemHovered ())
            {
                dl->AddRectFilled (p, ImVec2 (p.x + box, p.y + box), theme_.hover, theme_.radius_sm);
                ImGui::SetTooltip ("%s", TIPS[i]);
            }
            float g = box * 0.7f;
            align_glyph (dl, ImVec2 (p.x + (box - g) * 0.5f, p.y + (box - g) * 0.5f), g, i,
                enabled ? theme_.icon : theme_.text_faint);
            if (!enabled)
            {
                ImGui::EndDisabled ();
            }
            if (pressed)
            {
                run_command (i < 6    ? ALIGN[i]
                             : i == 6 ? "layer.distribute.horizontally"
                                      : "layer.distribute.vertically",
                    engine::Value{{"to", "auto"}});
            }
        }
    }

    /* The options bar: the tool's icon, then its settings in a row. */
    void App::options_bar (ImVec2 pos, ImVec2 size)
    {
        float k = ui_scale_;
        ImGui::SetNextWindowPos (pos);
        ImGui::SetNextWindowSize (size);
        ImGui::PushStyleColor (ImGuiCol_WindowBg, theme_.chrome);
        ImGui::PushStyleVar (
            ImGuiStyleVar_WindowPadding, ImVec2 (8.0f * k, (size.y - ImGui::GetFrameHeight ()) * 0.5f));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking
                               | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing
                               | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::Begin ("##options", NULL, flags);
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        ImVec2 p       = ImGui::GetCursorScreenPos ();
        float fh       = ImGui::GetFrameHeight ();
        float g        = 18.0f * k;
        draw_icon (
            dl, &icons_, TOOL_INFO[(int) tool_].icon, ImVec2 (p.x + 2 * k, p.y + (fh - g) * 0.5f), g, theme_.icon);
        ImGui::Dummy (ImVec2 (26 * k, fh));
        ImGui::SameLine (0, 6 * k);
        ImGui::SeparatorEx (ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine (0, 12 * k);
        if (sam_.active)
        {
            sam_options ();
        }
        else if (xf_.active)
        {
            free_transform_options ();
        }
        else if (warp_mode_active ())
        {
            warp_mode_options ();
        }
        else
        {
            select_options ();
            fill_tool_options ();
            crop_tool_options ();
            retouch_options ();
            heal_options ();
            eyedropper_options ();
            type_options ();
            path_options ();
            if (uses_brush (tool_))
            {
                brush_options ();
            }
            if (tool_ == Tool::Move)
            {
                move_options ();
            }
            if (tool_ == Tool::Hand || tool_ == Tool::Zoom)
            {
                if (ImGui::Button ("100%"))
                {
                    run_command ("view.actualPixels", engine::Value::object ());
                }
                ImGui::SameLine ();
                if (ImGui::Button ("Fit Screen"))
                {
                    run_command ("view.fitOnScreen", engine::Value::object ());
                }
            }
        }
        ImGui::End ();
        ImGui::PopStyleVar (2);
        ImGui::PopStyleColor ();
    }

} // namespace ui
