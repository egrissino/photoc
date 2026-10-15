#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <string.h>

namespace ui
{

    /* The Eyedropper: click or drag to pick the foreground colour (Alt: the background). The Brush,
       Pencil, Paint Bucket and Gradient pick with Alt-click, and while the Color Picker is open a
       click on the canvas picks into it. Sample Size and Sample come from the options bar. */

    const int EYEDROP_SIZES[7]          = {1, 3, 5, 11, 31, 51, 101};
    const char EYEDROP_SIZE_LABELS[]    = "Point Sample\0"
                                          "3 by 3 Average\0"
                                          "5 by 5 Average\0"
                                          "11 by 11 Average\0"
                                          "31 by 31 Average\0"
                                          "51 by 51 Average\0"
                                          "101 by 101 Average\0";
    const char* const EYEDROP_LAYERS[3] = {"current", "currentAndBelow", "all"};
    const char EYEDROP_LAYER_LABELS[]   = "Current Layer\0Current & Below\0All Layers\0";

    /* The colour under document point (mx, my) with the options bar's settings; false off the
       canvas or on a transparent pixel. */
    bool App::sample_at (float mx, float my, float out[4])
    {
        engine::Value result;
        engine::Error err;
        engine::Value p = {{"x", mx}, {"y", my}, {"size", EYEDROP_SIZES[eyedrop_size_]},
            {"sampleLayer", EYEDROP_LAYERS[eyedrop_layers_]}};
        if (!session_.execute ("document.sampleColor", p, &result, &err) || !result.is_array () || result.size () < 4)
        {
            return false;
        }
        for (int c = 0; c < 4; c++)
        {
            out[c] = result[(size_t) c].get<float> ();
        }
        return out[3] > 0.0f;
    }

    /* True when the pointer is picking a colour this frame (the canvas tools then stay out). */
    bool App::eyedropper_input (float mx, float my)
    {
        ImGuiIO& io    = ImGui::GetIO ();
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        float c[4];
        if (picker_open_)
        {
            if ((activated || active) && sample_at (mx, my, c))
            {
                picker_set_rgb (c);
            }
            return true;
        }
        bool alt_tool =
            tool_ == Tool::Brush || tool_ == Tool::Pencil || tool_ == Tool::PaintBucket || tool_ == Tool::Gradient;
        if (activated && (tool_ == Tool::Eyedropper || (alt_tool && io.KeyAlt)))
        {
            eyedropping_ = true;
            eyedrop_bg_  = tool_ == Tool::Eyedropper && io.KeyAlt;
        }
        if (!eyedropping_)
        {
            return false;
        }
        float* target = eyedrop_bg_ ? session_.tools.background : session_.tools.foreground;
        if ((activated || active) && sample_at (mx, my, c))
        {
            target[0] = c[0];
            target[1] = c[1];
            target[2] = c[2];
            target[3] = 1.0f;
        }
        if (released)
        {
            eyedropping_ = false;
            /* One journaled step for the colour picked. */
            run_command ("tools.setColors",
                engine::Value{{eyedrop_bg_ ? "background" : "foreground", {target[0], target[1], target[2], 1.0}}});
        }
        return true;
    }

    void App::eyedropper_options ()
    {
        if (tool_ != Tool::Eyedropper)
        {
            return;
        }
        float k = ui_scale_;
        opt_label ("Sample Size:");
        ImGui::SetNextItemWidth (150 * k);
        ImGui::Combo ("##eyesize", &eyedrop_size_, EYEDROP_SIZE_LABELS);
        opt_gap ();
        opt_label ("Sample:");
        ImGui::SetNextItemWidth (140 * k);
        ImGui::Combo ("##eyelayers", &eyedrop_layers_, EYEDROP_LAYER_LABELS);
    }

} // namespace ui
