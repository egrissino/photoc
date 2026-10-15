#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ui
{

    /* The Color and Swatches panels. The Color panel is Photoshop's: the foreground and background
       chips (the framed one is edited; clicking it again opens the Color Picker), a saturation/brightness field and a
       hue strip, then hex and R, G, B fields. HSB is taken on the sRGB values, as Photoshop's picker does. */

    void rgb_to_hsv (const float c[3], float* h, float* s, float* v)
    {
        float mx = fmaxf (c[0], fmaxf (c[1], c[2])), mn = fminf (c[0], fminf (c[1], c[2])), d = mx - mn;
        *v = mx;
        *s = mx > 0.0f ? d / mx : 0.0f;
        if (d <= 0.0f)
        {
            *h = 0.0f;
            return;
        }
        float hh;
        if (mx == c[0])
        {
            hh = fmodf ((c[1] - c[2]) / d + 6.0f, 6.0f);
        }
        else if (mx == c[1])
        {
            hh = (c[2] - c[0]) / d + 2.0f;
        }
        else
        {
            hh = (c[0] - c[1]) / d + 4.0f;
        }
        *h = hh / 6.0f;
    }

    void hsv_to_rgb (float h, float s, float v, float out[3])
    {
        h       = (h - floorf (h)) * 6.0f;
        int i   = (int) floorf (h) % 6;
        float f = h - floorf (h), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
        float r[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
        for (int k = 0; k < 3; k++)
        {
            out[k] = r[i][k];
        }
    }

    ImU32 rgb_u32 (const float c[3])
    {
        return ImGui::ColorConvertFloat4ToU32 (ImVec4 (c[0], c[1], c[2], 1.0f));
    }

    /* A saturation (across) by brightness (down) field for hue `h` at `fa` (fw x fh), and the hue
       strip (`strip_w` wide, red at the bottom as in Photoshop) to its right. True while dragged. */
    bool App::sv_hue_field (ImVec2 fa, float fw, float fh, float strip_w, float* h, float* s, float* v)
    {
        float k        = ui_scale_;
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        ImVec2 fb (fa.x + fw, fa.y + fh);
        float pure[3];
        hsv_to_rgb (*h, 1.0f, 1.0f, pure);
        dl->AddRectFilledMultiColor (fa, fb, IM_COL32_WHITE, rgb_u32 (pure), rgb_u32 (pure), IM_COL32_WHITE);
        dl->AddRectFilledMultiColor (
            fa, fb, IM_COL32 (0, 0, 0, 0), IM_COL32 (0, 0, 0, 0), IM_COL32_BLACK, IM_COL32_BLACK);
        dl->AddRect (ImVec2 (fa.x - 1, fa.y - 1), ImVec2 (fb.x + 1, fb.y + 1), theme_.separator);
        ImGui::SetCursorScreenPos (fa);
        ImGui::InvisibleButton ("##svfield", ImVec2 (fw, fh));
        bool changed = false;
        if (ImGui::IsItemActive ())
        {
            ImVec2 m = ImGui::GetIO ().MousePos;
            *s       = fminf (fmaxf ((m.x - fa.x) / fw, 0.0f), 1.0f);
            *v       = 1.0f - fminf (fmaxf ((m.y - fa.y) / fh, 0.0f), 1.0f);
            changed  = true;
        }
        ImVec2 knob (fa.x + *s * fw, fa.y + (1.0f - *v) * fh);
        dl->PushClipRect (ImVec2 (fa.x - 8 * k, fa.y - 8 * k), ImVec2 (fb.x + 8 * k, fb.y + 8 * k), true);
        dl->AddCircle (knob, 5.0f * k, *v > 0.6f && *s < 0.4f ? IM_COL32_BLACK : IM_COL32_WHITE, 0, 1.5f * k);
        dl->AddCircle (knob, 6.5f * k, IM_COL32 (0, 0, 0, 160), 0, 1.0f * k);
        dl->PopClipRect ();

        ImVec2 sa (fb.x + 8 * k, fa.y), sb (sa.x + strip_w, fb.y);
        for (int i = 0; i < 6; i++)
        {
            float c0[3], c1[3];
            hsv_to_rgb (1.0f - (float) i / 6.0f, 1, 1, c0);
            hsv_to_rgb (1.0f - (float) (i + 1) / 6.0f, 1, 1, c1);
            float y0 = sa.y + fh * (float) i / 6.0f, y1 = sa.y + fh * (float) (i + 1) / 6.0f;
            dl->AddRectFilledMultiColor (
                ImVec2 (sa.x, y0), ImVec2 (sb.x, y1), rgb_u32 (c0), rgb_u32 (c0), rgb_u32 (c1), rgb_u32 (c1));
        }
        ImGui::SetCursorScreenPos (sa);
        ImGui::InvisibleButton ("##huestrip", ImVec2 (strip_w + 8 * k, fh));
        if (ImGui::IsItemActive ())
        {
            float t = fminf (fmaxf ((ImGui::GetIO ().MousePos.y - sa.y) / fh, 0.0f), 0.9999f);
            *h      = 1.0f - t;
            changed = true;
        }
        float hy = sa.y + (1.0f - *h) * fh;
        if (*h <= 0.0f)
        {
            hy = sb.y;
        }
        dl->AddTriangleFilled (ImVec2 (sb.x + 1 * k, hy), ImVec2 (sb.x + 7 * k, hy - 4 * k),
            ImVec2 (sb.x + 7 * k, hy + 4 * k), theme_.text);
        return changed;
    }

    /* The field and strip editing `color` in place (the session's foreground or background). */
    void App::color_field (float* color, bool background)
    {
        float k = ui_scale_;
        float h, s, v;
        rgb_to_hsv (color, &h, &s, &v);
        /* A grey has no hue and black no saturation: keep what the field showed last. */
        if (v < 0.01f)
        {
            h = color_hue_;
            s = color_sat_;
        }
        else if (s < 0.01f)
        {
            h = color_hue_;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        float avail_w  = ImGui::GetContentRegionAvail ().x;
        float avail_h  = ImGui::GetContentRegionAvail ().y - ImGui::GetFrameHeight () - 10 * k;
        float fh       = fmaxf (fminf (avail_h, 400 * k), 80 * k);
        float chips_w = 40 * k, strip_w = 14 * k;
        ImVec2 p = ImGui::GetCursorScreenPos ();

        /* The chips: foreground over background, the edited one framed. */
        float chip = 22 * k;
        ImVec2 fg0 (p.x + 3 * k, p.y + 3 * k), bg0 (p.x + 14 * k, p.y + 14 * k);
        const float *fg = session_.tools.foreground, *bg = session_.tools.background;
        ImGui::SetCursorScreenPos (bg0);
        if (ImGui::InvisibleButton ("##bgsel", ImVec2 (chip, chip)))
        {
            if (color_bg_active_)
            {
                open_color_picker (true);
            }
            color_bg_active_ = true;
        }
        ImGui::SetCursorScreenPos (fg0);
        if (ImGui::InvisibleButton ("##fgsel", ImVec2 (chip, chip)))
        {
            if (!color_bg_active_)
            {
                open_color_picker (false);
            }
            color_bg_active_ = false;
        }
        dl->AddRectFilled (bg0, ImVec2 (bg0.x + chip, bg0.y + chip), rgb_u32 (bg), 2 * k);
        dl->AddRect (bg0, ImVec2 (bg0.x + chip, bg0.y + chip), theme_.field_border, 2 * k);
        if (color_bg_active_)
        {
            dl->AddRect (ImVec2 (bg0.x - 2 * k, bg0.y - 2 * k), ImVec2 (bg0.x + chip + 2 * k, bg0.y + chip + 2 * k),
                theme_.text_dim, 2 * k);
        }
        dl->AddRectFilled (fg0, ImVec2 (fg0.x + chip, fg0.y + chip), rgb_u32 (fg), 2 * k);
        dl->AddRect (fg0, ImVec2 (fg0.x + chip, fg0.y + chip), IM_COL32 (210, 210, 210, 255), 2 * k);
        if (!color_bg_active_)
        {
            dl->AddRect (ImVec2 (fg0.x - 2 * k, fg0.y - 2 * k), ImVec2 (fg0.x + chip + 2 * k, fg0.y + chip + 2 * k),
                theme_.text_dim, 2 * k);
        }

        /* The saturation (across) by brightness (down) field for the hue. */
        ImVec2 fa (p.x + chips_w + 8 * k, p.y);
        float fw = fmaxf (avail_w - chips_w - strip_w - 24 * k, 40 * k);
        ImVec2 fb (fa.x + fw, fa.y + fh);
        bool changed = sv_hue_field (fa, fw, fh, strip_w, &h, &s, &v);
        if (changed)
        {
            float rgb[3];
            hsv_to_rgb (h, s, v, rgb);
            memcpy (color, rgb, sizeof rgb);
            color_hue_ = h;
            color_sat_ = s;
        }

        /* Hex and R, G, B fields. */
        ImGui::SetCursorScreenPos (ImVec2 (p.x, fb.y + 8 * k));
        char hex[8];
        snprintf (hex, sizeof hex, "%02x%02x%02x", (int) lroundf (color[0] * 255), (int) lroundf (color[1] * 255),
            (int) lroundf (color[2] * 255));
        ImGui::PushFont (fonts_.mono, 0.0f);
        opt_label ("#");
        ImGui::SetNextItemWidth (62 * k);
        if (ImGui::InputText (
                "##hex", hex, sizeof hex, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)
            && strlen (hex) == 6)
        {
            unsigned long vhex = strtoul (hex, NULL, 16);
            color[0]           = (float) ((vhex >> 16) & 255) / 255.0f;
            color[1]           = (float) ((vhex >> 8) & 255) / 255.0f;
            color[2]           = (float) (vhex & 255) / 255.0f;
        }
        ImGui::PopFont ();
        const char* names[3] = {"R", "G", "B"};
        float field_w =
            fmaxf ((ImGui::GetContentRegionAvail ().x - ImGui::GetCursorPosX () - 62 * k - 3 * 26 * k) / 3.0f, 30 * k);
        for (int i = 0; i < 3; i++)
        {
            ImGui::SameLine (0, 8 * k);
            opt_label (names[i]);
            int vv = (int) lroundf (color[i] * 255.0f);
            ImGui::SetNextItemWidth (fminf (field_w, 44 * k));
            char id[8];
            snprintf (id, sizeof id, "##c%d", i);
            if (ImGui::DragInt (id, &vv, 0.5f, 0, 255, "%d", ImGuiSliderFlags_AlwaysClamp))
            {
                color[i] = (float) vv / 255.0f;
            }
        }
        (void) background;
    }

    void App::color_window ()
    {
        ImGui::Begin ("Color");
        color_field (color_bg_active_ ? session_.tools.background : session_.tools.foreground, color_bg_active_);
        ImGui::End ();
    }

    /* Swatches: groups of colour squares. Click uses one as the foreground, Alt-click as the
       background; right-click renames or deletes; the buttons add the foreground as a swatch, add a
       group, delete the selected swatch. */
    void App::swatches_window ()
    {
        ImGui::Begin ("Swatches");
        float k = ui_scale_, sq = 18 * k, gap = 3 * k;
        float buttons_h = 24.0f * k + ImGui::GetStyle ().ItemSpacing.y * 2;
        ImGui::BeginChild ("##swatchlist", ImVec2 (0, -buttons_h));
        ImGuiIO& io                              = ImGui::GetIO ();
        std::vector<engine::SwatchGroup>& groups = session_.swatches;
        std::string use_name, use_target, delete_name;
        for (size_t g = 0; g < groups.size (); g++)
        {
            ImGui::PushID ((int) g);
            float ig  = 12 * k;
            ImVec2 hp = ImGui::GetCursorScreenPos ();
            bool open = ImGui::TreeNodeEx (
                "##group", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s", "");
            ImGui::GetWindowDrawList ()->AddText (ImVec2 (hp.x + ImGui::GetTreeNodeToLabelSpacing () + ig + 6 * k,
                                                      hp.y + ImGui::GetStyle ().FramePadding.y),
                theme_.text, groups[g].name.c_str ());
            draw_icon (ImGui::GetWindowDrawList (), &icons_, "folder",
                ImVec2 (hp.x + ImGui::GetTreeNodeToLabelSpacing (), hp.y + ImGui::GetStyle ().FramePadding.y + 1 * k),
                ig, theme_.icon);
            if (open)
            {
                ImVec2 origin = ImGui::GetCursorScreenPos ();
                float wmax    = ImGui::GetContentRegionAvail ().x;
                int cols      = (int) fmaxf (1.0f, floorf ((wmax + gap) / (sq + gap)));
                size_t n      = groups[g].items.size ();
                for (size_t i = 0; i < n; i++)
                {
                    const engine::Swatch& sw = groups[g].items[i];
                    ImGui::PushID ((int) i);
                    ImVec2 a (origin.x + (float) (i % (size_t) cols) * (sq + gap),
                        origin.y + (float) (i / (size_t) cols) * (sq + gap));
                    ImGui::SetCursorScreenPos (a);
                    if (ImGui::InvisibleButton ("##sw", ImVec2 (sq, sq)))
                    {
                        use_name         = sw.name;
                        use_target       = io.KeyAlt ? "background" : "foreground";
                        selected_swatch_ = sw.name;
                    }
                    if (ImGui::IsItemHovered ())
                    {
                        ImGui::SetTooltip ("%s", sw.name.c_str ());
                    }
                    if (ImGui::BeginPopupContextItem ("##swctx"))
                    {
                        if (ImGui::MenuItem ("Rename Swatch…"))
                        {
                            rename_swatch_ = sw.name;
                            snprintf (swatch_buf_, sizeof swatch_buf_, "%s", sw.name.c_str ());
                        }
                        if (ImGui::MenuItem ("Delete Swatch"))
                        {
                            delete_name = sw.name;
                        }
                        ImGui::EndPopup ();
                    }
                    ImDrawList* dl = ImGui::GetWindowDrawList ();
                    bool sel       = selected_swatch_ == sw.name;
                    dl->AddRectFilled (a, ImVec2 (a.x + sq, a.y + sq), rgb_u32 (sw.rgb), 2 * k);
                    dl->AddRect (a, ImVec2 (a.x + sq, a.y + sq), sel ? theme_.text : theme_.card_border, 2 * k, 0,
                        sel ? 2 * k : 1);
                    ImGui::PopID ();
                }
                size_t rows = (n + (size_t) cols - 1) / (size_t) cols;
                ImGui::SetCursorScreenPos (ImVec2 (origin.x, origin.y + (float) rows * (sq + gap)));
                ImGui::Dummy (ImVec2 (1, 2 * k));
                ImGui::TreePop ();
            }
            ImGui::PopID ();
        }
        ImGui::EndChild ();
        if (!use_name.empty ())
        {
            run_command ("swatches.use", engine::Value{{"swatch", use_name}, {"target", use_target}});
        }
        if (!delete_name.empty ())
        {
            run_command ("swatches.delete", engine::Value{{"swatch", delete_name}});
        }
        float box = 24 * k;
        ImGui::SetCursorPosX (
            ImGui::GetCursorPosX () + fmaxf (ImGui::GetContentRegionAvail ().x - 3 * box - 8 * k, 0.0f));
        if (icon_button ("##newgroup", "folder-plus", box, false, "Create new group"))
        {
            run_command ("swatches.newGroup", engine::Value::object ());
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##newswatch", "plus", box, false, "Create new swatch of the foreground color"))
        {
            run_command ("swatches.add", engine::Value::object ());
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##delswatch", "trash", box, false, "Delete swatch") && !selected_swatch_.empty ())
        {
            run_command ("swatches.delete", engine::Value{{"swatch", selected_swatch_}});
            selected_swatch_.clear ();
        }
        if (!rename_swatch_.empty ())
        {
            ImGui::OpenPopup ("Rename Swatch###renameswatch");
        }
        if (ImGui::BeginPopupModal ("Rename Swatch###renameswatch", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::SetNextItemWidth (220 * k);
            if (ImGui::IsWindowAppearing ())
            {
                ImGui::SetKeyboardFocusHere ();
            }
            bool enter =
                ImGui::InputText ("Name", swatch_buf_, sizeof swatch_buf_, ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button ("OK") || enter)
            {
                run_command ("swatches.rename", engine::Value{{"swatch", rename_swatch_}, {"name", swatch_buf_}});
                rename_swatch_.clear ();
                ImGui::CloseCurrentPopup ();
            }
            ImGui::SameLine ();
            if (ImGui::Button ("Cancel") || ImGui::IsKeyPressed (ImGuiKey_Escape))
            {
                rename_swatch_.clear ();
                ImGui::CloseCurrentPopup ();
            }
            ImGui::EndPopup ();
        }
        ImGui::End ();
    }

} // namespace ui
