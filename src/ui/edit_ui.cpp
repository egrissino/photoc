#include "color/blend.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui
{

    /* The Edit > Fill and Edit > Stroke dialogs (Photoshop's, without History and Script
       contents). */

    const char* const FILL_IDS[] = {
        "foreground", "background", "color", "contentAware", "pattern", "black", "gray", "white"};
    const char FILL_LABELS[] = "Foreground Color\0Background Color\0Color…\0Content-Aware\0Pattern\0Black\0"
                               "50% Gray\0White\0";
    const char* const STROKE_LOCATIONS[] = {"inside", "center", "outside"};

    /* The blend mode combo over the layer modes; `mode` indexes color::LAYER_MODES. */
    void blend_combo (const char* id, int* mode, float width)
    {
        ImGui::SetNextItemWidth (width);
        if (!ImGui::BeginCombo (id, color::blend_label (color::LAYER_MODES[*mode])))
        {
            return;
        }
        for (int i = 0; i < (int) color::LAYER_MODE_COUNT; i++)
        {
            if (ImGui::Selectable (color::blend_label (color::LAYER_MODES[i]), i == *mode))
            {
                *mode = i;
            }
        }
        ImGui::EndCombo ();
    }

    /* A hex "#rrggbb" for an RGB colour in 0..1. */
    void hex_color (const float c[3], char out[8])
    {
        snprintf (out, 8, "#%02x%02x%02x", (int) lroundf (c[0] * 255), (int) lroundf (c[1] * 255),
            (int) lroundf (c[2] * 255));
    }

    void App::fill_dialog ()
    {
        if (!show_fill_)
        {
            return;
        }
        ImGui::OpenPopup ("Fill###fill");
        if (!ImGui::BeginPopupModal ("Fill###fill", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k = ui_scale_;
        ImGui::SeparatorText ("Contents");
        ImGui::SetNextItemWidth (200 * k);
        ImGui::Combo ("Contents", &fill_contents_, FILL_LABELS);
        if (fill_contents_ == 2)
        {
            ImGui::SameLine ();
            ImGui::ColorEdit3 ("##fillcolor", fill_color_, ImGuiColorEditFlags_NoInputs);
        }
        if (fill_contents_ == 4)
        {
            pattern_combo ("Custom Pattern", &fill_pattern_, 200 * k);
        }
        if (fill_contents_ == 3)
        {
            ImGui::TextDisabled ("Fills the selection from the image around it.");
        }
        ImGui::SeparatorText ("Blending");
        blend_combo ("Mode", &fill_mode_, 200 * k);
        ImGui::SetNextItemWidth (200 * k);
        ImGui::DragFloat (
            "Opacity", &fill_dialog_opacity_, 0.5f, 0.0f, 100.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
        ImGui::Checkbox ("Preserve Transparency", &fill_preserve_);
        ImGui::Spacing ();
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
               || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok)
        {
            engine::Value p = {{"contents", FILL_IDS[fill_contents_]},
                {"mode", color::blend_label (color::LAYER_MODES[fill_mode_])}, {"opacity", fill_dialog_opacity_},
                {"preserveTransparency", fill_preserve_}};
            if (fill_contents_ == 2)
            {
                char hex[8];
                hex_color (fill_color_, hex);
                p["color"] = hex;
            }
            if (fill_contents_ == 4)
            {
                p["pattern"] = fill_pattern_;
            }
            show_fill_ = false;
            ImGui::CloseCurrentPopup ();
            run_command ("edit.fill", p);
        }
        else if (cancel)
        {
            show_fill_ = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    void App::stroke_dialog ()
    {
        if (!show_stroke_)
        {
            return;
        }
        ImGui::OpenPopup ("Stroke###stroke");
        if (!ImGui::BeginPopupModal ("Stroke###stroke", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k = ui_scale_;
        ImGui::SeparatorText ("Stroke");
        ImGui::SetNextItemWidth (120 * k);
        ImGui::DragInt ("Width", &stroke_width_, 0.25f, 1, 250, "%d px", ImGuiSliderFlags_AlwaysClamp);
        ImGui::ColorEdit3 ("Color", stroke_color_, ImGuiColorEditFlags_NoInputs);
        ImGui::SeparatorText ("Location");
        ImGui::RadioButton ("Inside", &stroke_location_, 0);
        ImGui::SameLine ();
        ImGui::RadioButton ("Center", &stroke_location_, 1);
        ImGui::SameLine ();
        ImGui::RadioButton ("Outside", &stroke_location_, 2);
        ImGui::SeparatorText ("Blending");
        blend_combo ("Mode", &stroke_mode_, 200 * k);
        ImGui::SetNextItemWidth (200 * k);
        ImGui::DragFloat ("Opacity", &stroke_opacity_, 0.5f, 0.0f, 100.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
        ImGui::Checkbox ("Preserve Transparency", &stroke_preserve_);
        ImGui::Spacing ();
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
               || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok)
        {
            char hex[8];
            hex_color (stroke_color_, hex);
            engine::Value p = {{"width", stroke_width_}, {"color", hex},
                {"location", STROKE_LOCATIONS[stroke_location_]},
                {"mode", color::blend_label (color::LAYER_MODES[stroke_mode_])}, {"opacity", stroke_opacity_},
                {"preserveTransparency", stroke_preserve_}};
            show_stroke_    = false;
            ImGui::CloseCurrentPopup ();
            run_command ("edit.stroke", p);
        }
        else if (cancel)
        {
            show_stroke_ = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Edit > Fade: the last step's result blended back over what it replaced. */
    void App::fade_dialog ()
    {
        if (!show_fade_)
        {
            return;
        }
        ImGui::OpenPopup ("Fade###fade");
        if (!ImGui::BeginPopupModal ("Fade###fade", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k = ui_scale_;
        ImGui::SetNextItemWidth (200 * k);
        ImGui::SliderFloat ("Opacity", &fade_opacity_, 0.0f, 100.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
        blend_combo ("Mode", &fade_mode_, 200 * k);
        ImGui::Spacing ();
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
               || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok || cancel)
        {
            show_fade_ = false;
            ImGui::CloseCurrentPopup ();
            if (ok)
            {
                run_command ("edit.fade", engine::Value{{"opacity", fade_opacity_},
                                              {"mode", color::blend_label (color::LAYER_MODES[fade_mode_])}});
            }
        }
        ImGui::EndPopup ();
    }

} // namespace ui
