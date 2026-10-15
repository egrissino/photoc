#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <string.h>

namespace ui
{

    /* The Crop tool (drag a frame, drag its corners, edges or inside to adjust; Enter or a
       double-click inside crops, Escape cancels) and the Image Size and Canvas Size dialogs. */

    const char* const SIZE_METHODS[] = {"bicubic", "automatic", "preserveDetails", "bicubicSmoother", "bicubicSharper",
        "bilinear", "nearest", "lanczos"};
    const char SIZE_METHOD_LABELS[]  = "Bicubic\0Automatic\0Preserve Details\0Bicubic Smoother (enlargement)\0Bicubic "
                                       "Sharper (reduction)\0Bilinear\0Nearest Neighbor\0Lanczos\0";
    const char* const ANCHORS[]      = {
        "topLeft", "top", "topRight", "left", "center", "right", "bottomLeft", "bottom", "bottomRight"};
    const char* const EXTENSIONS[] = {"background", "foreground", "white", "black", "transparent"};

    void App::commit_crop ()
    {
        if (!crop_active_)
        {
            return;
        }
        crop_active_ = false;
        int32_t x0   = (int32_t) lroundf (fminf (crop_rect_[0], crop_rect_[2])),
                y0   = (int32_t) lroundf (fminf (crop_rect_[1], crop_rect_[3]));
        int32_t x1   = (int32_t) lroundf (fmaxf (crop_rect_[0], crop_rect_[2])),
                y1   = (int32_t) lroundf (fmaxf (crop_rect_[1], crop_rect_[3]));
        if (x1 - x0 < 1 || y1 - y0 < 1)
        {
            return;
        }
        run_command ("image.crop", engine::Value{{"x", x0}, {"y", y0}, {"width", x1 - x0}, {"height", y1 - y0},
                                       {"deleteCroppedPixels", crop_delete_}});
    }

    /* Handles: 0-3 corners (x0y0, x1y0, x1y1, x0y1), 4-7 edges (top, right, bottom, left), 8
       inside. */
    bool App::crop_tool_input (float ox, float oy)
    {
        if (tool_ != Tool::Crop)
        {
            crop_active_ = false;
            return false;
        }
        ImGuiIO& io = ImGui::GetIO ();
        float mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        if (crop_active_ && (ImGui::IsKeyPressed (ImGuiKey_Enter) || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter)))
        {
            commit_crop ();
            return true;
        }
        if (crop_active_ && ImGui::IsKeyPressed (ImGuiKey_Escape))
        {
            crop_active_ = false;
            return true;
        }
        float* r = crop_rect_;
        float x0 = fminf (r[0], r[2]), x1 = fmaxf (r[0], r[2]), y0 = fminf (r[1], r[3]), y1 = fmaxf (r[1], r[3]);
        float reach = 7.0f / zoom_;
        int over    = -1;
        if (crop_active_)
        {
            float cx[4] = {x0, x1, x1, x0}, cy[4] = {y0, y0, y1, y1};
            for (int i = 0; i < 4 && over < 0; i++)
            {
                if (fabsf (mx - cx[i]) <= reach && fabsf (my - cy[i]) <= reach)
                {
                    over = i;
                }
            }
            if (over < 0 && mx > x0 && mx < x1 && fabsf (my - y0) <= reach)
            {
                over = 4;
            }
            if (over < 0 && my > y0 && my < y1 && fabsf (mx - x1) <= reach)
            {
                over = 5;
            }
            if (over < 0 && mx > x0 && mx < x1 && fabsf (my - y1) <= reach)
            {
                over = 6;
            }
            if (over < 0 && my > y0 && my < y1 && fabsf (mx - x0) <= reach)
            {
                over = 7;
            }
            if (over < 0 && mx > x0 && mx < x1 && my > y0 && my < y1)
            {
                over = 8;
            }
        }
        if (activated)
        {
            if (over == 8 && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            {
                commit_crop ();
                return true;
            }
            if (over < 0)
            {
                crop_active_ = true;
                r[0] = r[2] = roundf (mx);
                r[1] = r[3] = roundf (my);
                over        = 2;
            }
            else
            {
                r[0] = x0;
                r[1] = y0;
                r[2] = x1;
                r[3] = y1;
            }
            crop_drag_     = over;
            crop_press_[0] = mx;
            crop_press_[1] = my;
            memcpy (crop_start_, r, sizeof crop_start_);
        }
        if (crop_drag_ >= 0 && active)
        {
            float dx = roundf (mx - crop_press_[0]), dy = roundf (my - crop_press_[1]);
            memcpy (r, crop_start_, sizeof crop_start_);
            int h = crop_drag_;
            if (h == 8)
            {
                r[0] += dx;
                r[2] += dx;
                r[1] += dy;
                r[3] += dy;
            }
            else
            {
                bool left = h == 0 || h == 3 || h == 7, right = h == 1 || h == 2 || h == 5;
                bool top = h == 0 || h == 1 || h == 4, bottom = h == 2 || h == 3 || h == 6;
                if (left)
                {
                    r[0] += dx;
                }
                if (right)
                {
                    r[2] += dx;
                }
                if (top)
                {
                    r[1] += dy;
                }
                if (bottom)
                {
                    r[3] += dy;
                }
            }
        }
        if (released)
        {
            crop_drag_ = -1;
        }
        return true;
    }

    void App::crop_tool_overlay (float ox, float oy)
    {
        if (!crop_active_ || tool_ != Tool::Crop)
        {
            return;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        float x0 = fminf (crop_rect_[0], crop_rect_[2]), x1 = fmaxf (crop_rect_[0], crop_rect_[2]);
        float y0 = fminf (crop_rect_[1], crop_rect_[3]), y1 = fmaxf (crop_rect_[1], crop_rect_[3]);
        ImVec2 a (ox + x0 * zoom_, oy + y0 * zoom_), b (ox + x1 * zoom_, oy + y1 * zoom_);
        ImVec2 clip0 = dl->GetClipRectMin (), clip1 = dl->GetClipRectMax ();
        ImU32 shade = IM_COL32 (0, 0, 0, 120);
        dl->AddRectFilled (clip0, ImVec2 (clip1.x, a.y), shade);
        dl->AddRectFilled (ImVec2 (clip0.x, b.y), clip1, shade);
        dl->AddRectFilled (ImVec2 (clip0.x, a.y), ImVec2 (a.x, b.y), shade);
        dl->AddRectFilled (ImVec2 (b.x, a.y), ImVec2 (clip1.x, b.y), shade);
        dl->AddRect (a, b, IM_COL32 (255, 255, 255, 255), 0.0f, 0, 1.0f);
        for (int i = 1; i < 3; i++)
        {
            float tx = a.x + (b.x - a.x) * (float) i / 3.0f, ty = a.y + (b.y - a.y) * (float) i / 3.0f;
            dl->AddLine (ImVec2 (tx, a.y), ImVec2 (tx, b.y), IM_COL32 (255, 255, 255, 90));
            dl->AddLine (ImVec2 (a.x, ty), ImVec2 (b.x, ty), IM_COL32 (255, 255, 255, 90));
        }
        ImVec2 hs[8] = {a, ImVec2 (b.x, a.y), b, ImVec2 (a.x, b.y), ImVec2 ((a.x + b.x) * 0.5f, a.y),
            ImVec2 (b.x, (a.y + b.y) * 0.5f), ImVec2 ((a.x + b.x) * 0.5f, b.y), ImVec2 (a.x, (a.y + b.y) * 0.5f)};
        for (int i = 0; i < 8; i++)
        {
            dl->AddRectFilled (
                ImVec2 (hs[i].x - 4, hs[i].y - 4), ImVec2 (hs[i].x + 4, hs[i].y + 4), IM_COL32 (255, 255, 255, 255));
            dl->AddRect (ImVec2 (hs[i].x - 4, hs[i].y - 4), ImVec2 (hs[i].x + 4, hs[i].y + 4), IM_COL32 (0, 0, 0, 255));
        }
        char label[64];
        snprintf (label, sizeof label, "%d x %d", (int) lroundf (x1 - x0), (int) lroundf (y1 - y0));
        dl->AddText (ImVec2 (b.x + 6, b.y + 4), IM_COL32 (255, 255, 255, 255), label);
    }

    void App::crop_tool_options ()
    {
        if (tool_ != Tool::Crop)
        {
            return;
        }
        ImGui::Checkbox ("Delete Cropped Pixels", &crop_delete_);
        opt_gap ();
        if (crop_active_)
        {
            if (ImGui::Button ("Cancel"))
            {
                crop_active_ = false;
            }
            ImGui::SameLine ();
            if (ImGui::Button ("Commit"))
            {
                commit_crop ();
            }
        }
        else
        {
            ImGui::AlignTextToFramePadding ();
            ImGui::TextDisabled ("Drag a frame on the canvas.");
        }
    }

    void App::image_size_dialog ()
    {
        if (!show_image_size_)
        {
            return;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            show_image_size_ = false;
            return;
        }
        ImGui::OpenPopup ("Image Size###imageSize");
        if (!ImGui::BeginPopupModal ("Image Size###imageSize", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        int ow = (int) d->doc->size.width, oh = (int) d->doc->size.height;
        ImGui::Text ("Current: %d x %d px", ow, oh);
        bool resample = size_method_ >= 0;
        ImGui::Checkbox ("Resample", &resample);
        if (!resample)
        {
            size_method_ = -1;
        }
        else if (size_method_ < 0)
        {
            size_method_ = 0;
        }
        if (!resample)
        {
            ImGui::BeginDisabled ();
        }
        ImGui::SetNextItemWidth (160.0f);
        if (ImGui::InputInt ("Width (px)", &size_width_, 1, 10) && size_constrain_)
        {
            size_height_ = (int) lround ((double) size_width_ * oh / (ow > 0 ? ow : 1));
        }
        ImGui::SetNextItemWidth (160.0f);
        if (ImGui::InputInt ("Height (px)", &size_height_, 1, 10) && size_constrain_)
        {
            size_width_ = (int) lround ((double) size_height_ * ow / (oh > 0 ? oh : 1));
        }
        ImGui::Checkbox ("Constrain Proportions", &size_constrain_);
        int method = size_method_ < 0 ? 0 : size_method_;
        ImGui::SetNextItemWidth (260.0f);
        if (ImGui::Combo ("Method", &method, SIZE_METHOD_LABELS))
        {
            size_method_ = method;
        }
        if (!resample)
        {
            ImGui::EndDisabled ();
        }
        ImGui::SetNextItemWidth (160.0f);
        ImGui::InputFloat ("Resolution (ppi)", &size_resolution_, 1.0f, 10.0f, "%.1f");
        if (size_width_ < 1)
        {
            size_width_ = 1;
        }
        if (size_height_ < 1)
        {
            size_height_ = 1;
        }
        bool ok = ImGui::Button ("OK");
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel") || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok)
        {
            engine::Value p  = {{"width", size_width_}, {"height", size_height_}, {"resolution", size_resolution_},
                 {"resample", resample ? SIZE_METHODS[size_method_] : "none"}};
            show_image_size_ = false;
            ImGui::CloseCurrentPopup ();
            run_command ("image.imageSize", p);
        }
        else if (cancel)
        {
            show_image_size_ = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    void App::canvas_size_dialog ()
    {
        if (!show_canvas_size_)
        {
            return;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            show_canvas_size_ = false;
            return;
        }
        ImGui::OpenPopup ("Canvas Size###canvasSize");
        if (!ImGui::BeginPopupModal ("Canvas Size###canvasSize", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        int ow = (int) d->doc->size.width, oh = (int) d->doc->size.height;
        ImGui::Text ("Current: %d x %d px", ow, oh);
        if (ImGui::Checkbox ("Relative", &canvas_relative_))
        {
            canvas_width_  = canvas_relative_ ? canvas_width_ - ow : canvas_width_ + ow;
            canvas_height_ = canvas_relative_ ? canvas_height_ - oh : canvas_height_ + oh;
        }
        ImGui::SetNextItemWidth (160.0f);
        ImGui::InputInt ("Width (px)", &canvas_width_, 1, 10);
        ImGui::SetNextItemWidth (160.0f);
        ImGui::InputInt ("Height (px)", &canvas_height_, 1, 10);
        ImGui::TextUnformatted ("Anchor:");
        for (int i = 0; i < 9; i++)
        {
            ImGui::PushID (i);
            if (i % 3 != 0)
            {
                ImGui::SameLine ();
            }
            if (ImGui::RadioButton ("##a", canvas_anchor_ == i))
            {
                canvas_anchor_ = i;
            }
            ImGui::PopID ();
        }
        ImGui::SetNextItemWidth (160.0f);
        ImGui::Combo ("Extension", &canvas_extension_, "Background\0Foreground\0White\0Black\0Transparent\0");
        bool ok = ImGui::Button ("OK");
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel") || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok)
        {
            engine::Value p   = {{"width", canvas_width_}, {"height", canvas_height_}, {"relative", canvas_relative_},
                  {"anchor", ANCHORS[canvas_anchor_]}, {"extensionColor", EXTENSIONS[canvas_extension_]}};
            show_canvas_size_ = false;
            ImGui::CloseCurrentPopup ();
            run_command ("image.canvasSize", p);
        }
        else if (cancel)
        {
            show_canvas_size_ = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

} // namespace ui
