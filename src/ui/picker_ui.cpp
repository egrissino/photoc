#include "color/convert.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ui
{

    /* The Color Picker dialog (Photoshop's, without the colour libraries and web-safe options): a
       large saturation/brightness field with the hue strip, the new colour over the current one, and
       HSB, Lab, RGB, CMYK and hex fields that all edit the same colour. OK sets the foreground or
       background; Add to Swatches stores it. A click on the canvas picks the colour under it. */

    void App::open_color_picker (bool background)
    {
        const float* c = background ? session_.tools.background : session_.tools.foreground;
        picker_open_   = true;
        picker_focus_  = true;
        picker_bg_     = background;
        memcpy (picker_old_, c, sizeof picker_old_);
        rgb_to_hsv (c, &picker_hsv_[0], &picker_hsv_[1], &picker_hsv_[2]);
        if (picker_hsv_[1] < 0.0001f || picker_hsv_[2] < 0.0001f)
        {
            picker_hsv_[0] = color_hue_;
        }
    }

    /* Sets the picker to `rgb`, keeping the hue of a grey. */
    void App::picker_set_rgb (const float rgb[3])
    {
        float c[3];
        for (int i = 0; i < 3; i++)
        {
            c[i] = fminf (fmaxf (rgb[i], 0.0f), 1.0f);
        }
        float h, s, v;
        rgb_to_hsv (c, &h, &s, &v);
        if (s > 0.0001f && v > 0.0001f)
        {
            picker_hsv_[0] = h;
        }
        picker_hsv_[1] = s;
        picker_hsv_[2] = v;
    }

    void App::color_picker_dialog ()
    {
        if (!picker_open_)
        {
            return;
        }
        const char* title = picker_bg_ ? "Color Picker (Background Color)###colorpicker"
                                       : "Color Picker (Foreground Color)###colorpicker";
        /* Not modal, so a click on the canvas can pick a colour into it (eyedropper_input). */
        if (picker_focus_)
        {
            ImGui::SetNextWindowFocus ();
            ImVec2 c = ImGui::GetMainViewport ()->GetCenter ();
            ImGui::SetNextWindowPos (c, ImGuiCond_Always, ImVec2 (0.5f, 0.5f));
            picker_focus_ = false;
        }
        if (!ImGui::Begin (title, NULL,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking
                    | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::End ();
            return;
        }
        float k = ui_scale_;
        float rgb[3];
        hsv_to_rgb (picker_hsv_[0], picker_hsv_[1], picker_hsv_[2], rgb);

        /* The field and strip. */
        ImVec2 p   = ImGui::GetCursorScreenPos ();
        float side = 256 * k, strip = 18 * k;
        sv_hue_field (p, side, side, strip, &picker_hsv_[0], &picker_hsv_[1], &picker_hsv_[2]);
        hsv_to_rgb (picker_hsv_[0], picker_hsv_[1], picker_hsv_[2], rgb);

        /* New over current, then the buttons. */
        float col      = p.x + side + strip + 30 * k;
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        float sw = 64 * k, sh = 32 * k;
        ImGui::SetCursorScreenPos (ImVec2 (col, p.y));
        ImGui::TextUnformatted ("new");
        ImVec2 a (col, ImGui::GetCursorScreenPos ().y);
        dl->AddRectFilled (a, ImVec2 (a.x + sw, a.y + sh), rgb_u32 (rgb));
        dl->AddRectFilled (ImVec2 (a.x, a.y + sh), ImVec2 (a.x + sw, a.y + 2 * sh), rgb_u32 (picker_old_));
        dl->AddRect (ImVec2 (a.x - 1, a.y - 1), ImVec2 (a.x + sw + 1, a.y + 2 * sh + 1), theme_.card_border);
        ImGui::SetCursorScreenPos (ImVec2 (a.x, a.y + sh));
        if (ImGui::InvisibleButton ("##current", ImVec2 (sw, sh)))
        {
            picker_set_rgb (picker_old_);
        }
        if (ImGui::IsItemHovered ())
        {
            ImGui::SetTooltip ("Click to restore the current color");
        }
        ImGui::SetCursorScreenPos (ImVec2 (col, a.y + 2 * sh + 2 * k));
        ImGui::TextUnformatted ("current");

        float bw = 130 * k;
        float bx = col + sw + 24 * k;
        ImGui::SetCursorScreenPos (ImVec2 (bx, p.y));
        bool ok = ImGui::Button ("OK", ImVec2 (bw, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
               || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        ImGui::SetCursorScreenPos (ImVec2 (bx, ImGui::GetCursorScreenPos ().y));
        bool cancel = ImGui::Button ("Cancel", ImVec2 (bw, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        ImGui::SetCursorScreenPos (ImVec2 (bx, ImGui::GetCursorScreenPos ().y));
        if (ImGui::Button ("Add to Swatches", ImVec2 (bw, 0)))
        {
            char hex[8];
            snprintf (hex, sizeof hex, "#%02x%02x%02x", (int) lroundf (rgb[0] * 255), (int) lroundf (rgb[1] * 255),
                (int) lroundf (rgb[2] * 255));
            run_command ("swatches.add", engine::Value{{"color", hex}});
        }

        /* HSB and Lab on the left, RGB and CMYK on the right, hex below. */
        float fw     = 54 * k;
        float top    = a.y + 2 * sh + ImGui::GetTextLineHeightWithSpacing () + 12 * k;
        float c2     = col + 120 * k;
        float hsb[3] = {picker_hsv_[0] * 360.0f, picker_hsv_[1] * 100.0f, picker_hsv_[2] * 100.0f};
        float lab[3];
        color::srgb_to_lab (rgb, lab);
        float rgb255[3] = {roundf (rgb[0] * 255), roundf (rgb[1] * 255), roundf (rgb[2] * 255)};
        float cmyk[4];
        color::rgb_to_cmyk (session_.cmyk, rgb, cmyk);
        for (int i = 0; i < 4; i++)
        {
            cmyk[i] = roundf (cmyk[i] * 100.0f);
        }
        const char *hsb_names[3] = {"H:", "S:", "B:"}, *hsb_units[3] = {"°", "%", "%"};
        const char* lab_names[3]  = {"L:", "a:", "b:"};
        const char* rgb_names[3]  = {"R:", "G:", "B:"};
        const char* cmyk_names[4] = {"C:", "M:", "Y:", "K:"};
        float row                 = ImGui::GetFrameHeightWithSpacing ();
        float lw                  = ImGui::CalcTextSize ("M:").x + 6 * k;
        for (int i = 0; i < 3; i++)
        {
            char id[16];
            snprintf (id, sizeof id, "##hsb%d", i);
            ImGui::SetCursorScreenPos (ImVec2 (col, top + row * (float) i));
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted (hsb_names[i]);
            ImGui::SameLine (col - ImGui::GetWindowPos ().x + lw);
            ImGui::SetNextItemWidth (fw);
            if (ImGui::DragFloat (
                    id, &hsb[i], 0.5f, 0.0f, i == 0 ? 360.0f : 100.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
            {
                picker_hsv_[i] = hsb[i] / (i == 0 ? 360.0f : 100.0f);
                if (i == 0 && picker_hsv_[0] >= 1.0f)
                {
                    picker_hsv_[0] = 0.0f;
                }
            }
            ImGui::SameLine (0, 4 * k);
            ImGui::TextUnformatted (hsb_units[i]);

            ImGui::SetCursorScreenPos (ImVec2 (c2, top + row * (float) i));
            snprintf (id, sizeof id, "##rgb%d", i);
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted (rgb_names[i]);
            ImGui::SameLine (c2 - ImGui::GetWindowPos ().x + lw);
            ImGui::SetNextItemWidth (fw);
            if (ImGui::DragFloat (id, &rgb255[i], 0.5f, 0.0f, 255.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
            {
                float c[3] = {rgb255[0] / 255.0f, rgb255[1] / 255.0f, rgb255[2] / 255.0f};
                picker_set_rgb (c);
            }
        }
        float top2 = top + row * 3 + 8 * k;
        for (int i = 0; i < 4; i++)
        {
            char id[16];
            if (i < 3)
            {
                ImGui::SetCursorScreenPos (ImVec2 (col, top2 + row * (float) i));
                snprintf (id, sizeof id, "##lab%d", i);
                ImGui::AlignTextToFramePadding ();
                ImGui::TextUnformatted (lab_names[i]);
                ImGui::SameLine (col - ImGui::GetWindowPos ().x + lw);
                ImGui::SetNextItemWidth (fw);
                float lv = roundf (lab[i]);
                if (ImGui::DragFloat (id, &lv, 0.5f, i == 0 ? 0.0f : -128.0f, i == 0 ? 100.0f : 127.0f, "%.0f",
                        ImGuiSliderFlags_AlwaysClamp))
                {
                    float nl[3] = {lab[0], lab[1], lab[2]}, c[3];
                    nl[i]       = lv;
                    color::lab_to_srgb (nl, c);
                    picker_set_rgb (c);
                }
            }
            ImGui::SetCursorScreenPos (ImVec2 (c2, top2 + row * (float) i));
            snprintf (id, sizeof id, "##cmyk%d", i);
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted (cmyk_names[i]);
            ImGui::SameLine (c2 - ImGui::GetWindowPos ().x + lw);
            ImGui::SetNextItemWidth (fw);
            if (ImGui::DragFloat (id, &cmyk[i], 0.5f, 0.0f, 100.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
            {
                float q[4] = {cmyk[0] / 100.0f, cmyk[1] / 100.0f, cmyk[2] / 100.0f, cmyk[3] / 100.0f}, c[3];
                color::cmyk_to_rgb (session_.cmyk, q, c);
                picker_set_rgb (c);
            }
            ImGui::SameLine (0, 4 * k);
            ImGui::TextUnformatted ("%");
        }
        ImGui::SetCursorScreenPos (ImVec2 (col, top2 + row * 3));
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("#");
        ImGui::SameLine (col - ImGui::GetWindowPos ().x + lw);
        char hex[8];
        snprintf (hex, sizeof hex, "%02x%02x%02x", (int) rgb255[0], (int) rgb255[1], (int) rgb255[2]);
        ImGui::PushFont (fonts_.mono, 0.0f);
        ImGui::SetNextItemWidth (70 * k);
        if (ImGui::InputText ("##pickhex", hex, sizeof hex,
                ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)
            && strlen (hex) == 6)
        {
            unsigned long v = strtoul (hex, NULL, 16);
            float c[3]      = {
                (float) ((v >> 16) & 255) / 255.0f, (float) ((v >> 8) & 255) / 255.0f, (float) (v & 255) / 255.0f};
            picker_set_rgb (c);
            ok = false;
        }
        ImGui::PopFont ();
        ImGui::SetCursorScreenPos (ImVec2 (p.x, fmaxf (p.y + side, ImGui::GetCursorScreenPos ().y) + 4 * k));
        ImGui::Dummy (ImVec2 (1, 1));

        if (ok)
        {
            hsv_to_rgb (picker_hsv_[0], picker_hsv_[1], picker_hsv_[2], rgb);
            color_hue_   = picker_hsv_[0];
            color_sat_   = picker_hsv_[1];
            picker_open_ = false;
            run_command ("tools.setColors",
                engine::Value{{picker_bg_ ? "background" : "foreground", {rgb[0], rgb[1], rgb[2], 1.0}}});
        }
        else if (cancel)
        {
            picker_open_ = false;
        }
        ImGui::End ();
    }

} // namespace ui
