#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui
{

    /* Patch, Content-Aware Move and Red Eye on the canvas. Patch and Content-Aware Move draw a
       freehand selection when pressed outside one (Shift adds, Alt subtracts), and drag the
       selection when pressed inside it: on release Patch heals the selection from where it was
       dragged (Source) or heals its content into there (Destination); Content-Aware Move puts the
       content there and fills where it was. Red Eye fixes the red pupil under a click. Also the
       pattern picker shared by Pattern Stamp and Fill. */

    const char* selection_mode (const ImGuiIO& io, int bar_mode);

    bool App::pattern_combo (const char* id, int* index, float width)
    {
        engine::Value r;
        engine::Error e;
        if (!session_.execute ("patterns.list", engine::Value::object (), &r, &e))
        {
            return false;
        }
        const engine::Value& list = r["patterns"];
        if (*index >= (int) list.size ())
        {
            *index = 0;
        }
        ImGui::SetNextItemWidth (width);
        bool changed    = false;
        std::string cur = list.empty () ? "" : list[(size_t) *index]["name"].get<std::string> ();
        if (ImGui::BeginCombo (id, cur.c_str ()))
        {
            for (size_t i = 0; i < list.size (); i++)
            {
                char label[160];
                snprintf (label, sizeof label, "%s  (%dx%d)##p%zu", list[i]["name"].get<std::string> ().c_str (),
                    list[i]["width"].get<int> (), list[i]["height"].get<int> (), i);
                if (ImGui::Selectable (label, (int) i == *index))
                {
                    *index  = (int) i;
                    changed = true;
                }
            }
            ImGui::EndCombo ();
        }
        return changed;
    }

    bool App::heal_tool_input (float ox, float oy)
    {
        if (tool_ != Tool::Patch && tool_ != Tool::ContentAwareMove && tool_ != Tool::RedEye)
        {
            return false;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return true;
        }
        ImGuiIO& io = ImGui::GetIO ();
        float mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        if (tool_ == Tool::RedEye)
        {
            if (activated)
            {
                run_command ("paint.redEye",
                    engine::Value{{"x", mx}, {"y", my}, {"pupilSize", redeye_pupil_}, {"darken", redeye_darken_}});
            }
            return true;
        }
        if (activated)
        {
            heal_press_[0] = heal_last_[0] = mx;
            heal_press_[1] = heal_last_[1] = my;
            bool inside                    = d->doc->has_selection
                       && d->doc->selection.sample_channel ((int32_t) floorf (mx), (int32_t) floorf (my), 0) >= 0.5f;
            heal_drag_ = inside && !io.KeyShift && !io.KeyAlt ? 2 : 1;
            heal_points_.assign (1, mx);
            heal_points_.push_back (my);
            return true;
        }
        if (heal_drag_ == 0)
        {
            return true;
        }
        if (active)
        {
            heal_last_[0] = mx;
            heal_last_[1] = my;
            if (heal_drag_ == 1)
            {
                size_t n = heal_points_.size ();
                float dx = mx - heal_points_[n - 2], dy = my - heal_points_[n - 1];
                if (dx * dx + dy * dy >= 1.0f)
                {
                    heal_points_.push_back (mx);
                    heal_points_.push_back (my);
                }
            }
        }
        if (released || !ImGui::IsMouseDown (ImGuiMouseButton_Left))
        {
            int drag   = heal_drag_;
            heal_drag_ = 0;
            if (drag == 1)
            {
                if (heal_points_.size () < 6)
                {
                    if (d->doc->has_selection)
                    {
                        run_command ("select.deselect", engine::Value::object ());
                    }
                }
                else
                {
                    engine::Value pts = engine::Value::array ();
                    for (size_t i = 0; i + 1 < heal_points_.size (); i += 2)
                    {
                        pts.push_back ({heal_points_[i], heal_points_[i + 1]});
                    }
                    run_command ("select.lasso", engine::Value{{"points", pts}, {"mode", selection_mode (io, 0)},
                                                     {"antiAlias", true}, {"feather", 0}});
                }
            }
            else
            {
                int dx = (int) lroundf (heal_last_[0] - heal_press_[0]),
                    dy = (int) lroundf (heal_last_[1] - heal_press_[1]);
                if (dx != 0 || dy != 0)
                {
                    if (tool_ == Tool::Patch)
                    {
                        run_command ("paint.patch",
                            engine::Value{{"offset", {dx, dy}}, {"patch", patch_dest_ ? "destination" : "source"}});
                    }
                    else
                    {
                        run_command ("paint.contentAwareMove",
                            engine::Value{{"offset", {dx, dy}}, {"mode", cam_extend_ ? "extend" : "move"}});
                    }
                }
            }
            heal_points_.clear ();
        }
        return true;
    }

    void App::heal_tool_overlay (float ox, float oy)
    {
        if ((tool_ != Tool::Patch && tool_ != Tool::ContentAwareMove) || heal_drag_ == 0)
        {
            return;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        if (heal_drag_ == 1 && heal_points_.size () >= 4)
        {
            std::vector<ImVec2> pts;
            for (size_t i = 0; i + 1 < heal_points_.size (); i += 2)
            {
                pts.push_back (ImVec2 (ox + heal_points_[i] * zoom_, oy + heal_points_[i + 1] * zoom_));
            }
            dl->AddPolyline (pts.data (), (int) pts.size (), IM_COL32 (0, 0, 0, 220), 0, 2.0f);
            dl->AddPolyline (pts.data (), (int) pts.size (), IM_COL32 (255, 255, 255, 255), 0, 1.0f);
            return;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL || !d->doc->has_selection)
        {
            return;
        }
        /* Where the selection would land: its bounds, moved with the pointer. */
        geom::Rect b = d->doc->selection.content_bounds ();
        float dx = heal_last_[0] - heal_press_[0], dy = heal_last_[1] - heal_press_[1];
        ImVec2 a (ox + ((float) b.x0 + dx) * zoom_, oy + ((float) b.y0 + dy) * zoom_),
            c (ox + ((float) b.x1 + dx) * zoom_, oy + ((float) b.y1 + dy) * zoom_);
        dl->AddRect (a, c, IM_COL32 (0, 0, 0, 220), 0, 0, 3.0f);
        dl->AddRect (a, c, IM_COL32 (255, 255, 255, 255), 0, 0, 1.0f);
    }

    void App::heal_options ()
    {
        float k = ui_scale_;
        if (tool_ == Tool::Patch)
        {
            opt_label ("Patch:");
            int dest = patch_dest_ ? 1 : 0;
            ImGui::SetNextItemWidth (110 * k);
            if (ImGui::Combo ("##patchmode", &dest, "Source\0Destination\0"))
            {
                patch_dest_ = dest == 1;
            }
            opt_gap ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextDisabled ("Draw a selection, then drag it.");
        }
        else if (tool_ == Tool::ContentAwareMove)
        {
            opt_label ("Mode:");
            int ext = cam_extend_ ? 1 : 0;
            ImGui::SetNextItemWidth (90 * k);
            if (ImGui::Combo ("##cammode", &ext, "Move\0Extend\0"))
            {
                cam_extend_ = ext == 1;
            }
            opt_gap ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextDisabled ("Draw a selection, then drag it.");
        }
        else if (tool_ == Tool::RedEye)
        {
            opt_label ("Pupil Size:");
            ImGui::SetNextItemWidth (80 * k);
            ImGui::DragFloat ("##pupil", &redeye_pupil_, 0.4f, 1.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            opt_label ("Darken Amount:");
            ImGui::SetNextItemWidth (80 * k);
            ImGui::DragFloat ("##darken", &redeye_darken_, 0.4f, 1.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        }
    }

} // namespace ui
