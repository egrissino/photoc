#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <string.h>

namespace ui
{

    /* The Gradient tool (drag from start to end; Shift snaps to 45°) and the Paint Bucket (click),
       on the active layer or the targeted mask, and the Magic Eraser (click), which shares the
       bucket's options. */

    const char* const GRADIENT_STYLES[] = {"linear", "radial", "angle", "reflected", "diamond"};

    bool App::fill_tool_input (float ox, float oy)
    {
        if (tool_ != Tool::Gradient && tool_ != Tool::PaintBucket && tool_ != Tool::MagicEraser)
        {
            gradient_dragging_ = false;
            return false;
        }
        ImGuiIO& io = ImGui::GetIO ();
        float mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        if (tool_ == Tool::MagicEraser)
        {
            if (activated)
            {
                run_command (
                    "paint.magicEraser", engine::Value{{"x", mx}, {"y", my}, {"tolerance", bucket_tolerance_},
                                             {"contiguous", bucket_contiguous_}, {"antiAlias", sel_anti_alias_},
                                             {"sampleAllLayers", bucket_all_layers_}, {"opacity", fill_opacity_}});
            }
            return true;
        }
        if (tool_ == Tool::PaintBucket)
        {
            if (activated)
            {
                engine::Value p = {{"x", mx}, {"y", my}, {"tolerance", bucket_tolerance_},
                    {"contiguous", bucket_contiguous_}, {"antiAlias", sel_anti_alias_},
                    {"sampleAllLayers", bucket_all_layers_}, {"opacity", fill_opacity_}};
                if (mask_targeted ())
                {
                    p["target"] = "mask";
                }
                run_command ("paint.bucket", p);
            }
            return true;
        }
        if (activated)
        {
            gradient_dragging_ = true;
            gradient_from_[0] = gradient_to_[0] = mx;
            gradient_from_[1] = gradient_to_[1] = my;
        }
        if (!gradient_dragging_)
        {
            return true;
        }
        float dx = mx - gradient_from_[0], dy = my - gradient_from_[1];
        if (io.KeyShift)
        {
            float len = sqrtf (dx * dx + dy * dy);
            float a   = roundf (atan2f (dy, dx) / ((float) M_PI / 4.0f)) * ((float) M_PI / 4.0f);
            dx        = len * cosf (a);
            dy        = len * sinf (a);
        }
        gradient_to_[0] = gradient_from_[0] + dx;
        gradient_to_[1] = gradient_from_[1] + dy;
        if (released)
        {
            gradient_dragging_ = false;
            if ((dx * dx + dy * dy) * zoom_ * zoom_ < 4.0f)
            {
                return true;
            }
            engine::Value p = {{"from", {gradient_from_[0], gradient_from_[1]}},
                {"to", {gradient_to_[0], gradient_to_[1]}}, {"style", GRADIENT_STYLES[gradient_style_]},
                {"reverse", gradient_reverse_}, {"dither", gradient_dither_}, {"opacity", fill_opacity_}};
            if (mask_targeted ())
            {
                p["target"] = "mask";
            }
            run_command ("paint.gradient", p);
        }
        return true;
    }

    void App::fill_tool_overlay (float ox, float oy)
    {
        if (!gradient_dragging_ || tool_ != Tool::Gradient)
        {
            return;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        ImVec2 a (ox + gradient_from_[0] * zoom_, oy + gradient_from_[1] * zoom_),
            b (ox + gradient_to_[0] * zoom_, oy + gradient_to_[1] * zoom_);
        dl->AddLine (a, b, IM_COL32 (0, 0, 0, 220), 3.0f);
        dl->AddLine (a, b, IM_COL32 (255, 255, 255, 255), 1.0f);
        dl->AddCircleFilled (a, 3.0f, IM_COL32 (255, 255, 255, 255));
        dl->AddCircleFilled (b, 3.0f, IM_COL32 (255, 255, 255, 255));
    }

    void App::fill_tool_options ()
    {
        if (tool_ != Tool::Gradient && tool_ != Tool::PaintBucket && tool_ != Tool::MagicEraser)
        {
            return;
        }
        float k = ui_scale_;
        if (tool_ == Tool::Gradient)
        {
            const char* styles[5] = {"rectangle-horizontal", "circle", "compass", "toggle-left", "diamond"};
            const char* tips[5]   = {
                "Linear Gradient", "Radial Gradient", "Angle Gradient", "Reflected Gradient", "Diamond Gradient"};
            float box = ImGui::GetFrameHeight ();
            for (int i = 0; i < 5; i++)
            {
                char id[16];
                snprintf (id, sizeof id, "##gstyle%d", i);
                if (icon_button (id, styles[i], box, gradient_style_ == i, tips[i]))
                {
                    gradient_style_ = i;
                }
                ImGui::SameLine (0, 2 * k);
            }
            opt_gap ();
            ImGui::Checkbox ("Reverse", &gradient_reverse_);
            opt_gap ();
            ImGui::Checkbox ("Dither", &gradient_dither_);
            opt_gap ();
        }
        else
        {
            opt_label ("Tolerance:");
            ImGui::SetNextItemWidth (80 * k);
            ImGui::DragInt ("##btolerance", &bucket_tolerance_, 0.5f, 0, 255, "%d", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            ImGui::Checkbox ("Anti-alias", &sel_anti_alias_);
            opt_gap ();
            ImGui::Checkbox ("Contiguous", &bucket_contiguous_);
            opt_gap ();
            ImGui::Checkbox ("All Layers", &bucket_all_layers_);
            opt_gap ();
        }
        opt_label ("Opacity:");
        ImGui::SetNextItemWidth (90 * k);
        ImGui::DragFloat ("##fopacity", &fill_opacity_, 0.4f, 1.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    }

} // namespace ui
