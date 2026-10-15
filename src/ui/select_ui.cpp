#include "ui/app.h"
#include <imgui.h>
#include <math.h>
#include <string.h>

namespace ui
{

    /* The selection tools beyond the marquees: Lasso (drag a freehand outline), Polygonal Lasso
       (click corners; click the first point, double-click or Enter closes, Escape cancels) and the
       Magic Wand. Shift adds, Alt subtracts, both intersect. Also the Select > Modify and Color
       Range dialogs. */

    const char* const RANGE_PRESETS[] = {"sampledColors", "reds", "yellows", "greens", "cyans", "blues", "magentas",
        "highlights", "midtones", "shadows"};
    const char RANGE_LABELS[] =
        "Sampled Colors (foreground)\0Reds\0Yellows\0Greens\0Cyans\0Blues\0Magentas\0Highlights\0Midtones\0Shadows\0";

    const char* const SELECTION_MODES[4] = {"replace", "add", "subtract", "intersect"};

    /* The options bar's mode, unless a modifier says otherwise while the selection is made. */
    const char* selection_mode (const ImGuiIO& io, int bar_mode)
    {
        if (io.KeyShift && io.KeyAlt)
        {
            return "intersect";
        }
        if (io.KeyShift)
        {
            return "add";
        }
        if (io.KeyAlt)
        {
            return "subtract";
        }
        return SELECTION_MODES[bar_mode];
    }

    /* Quick Selection only adds or subtracts (New becomes Add after the first stroke). */
    const char* quick_mode (const ImGuiIO& io, int bar_mode)
    {
        return io.KeyAlt ? "subtract" : io.KeyShift || bar_mode == 1 ? "add" : bar_mode == 2 ? "subtract" : "replace";
    }

    void App::finish_lasso (bool polygon)
    {
        ImGuiIO& io   = ImGui::GetIO ();
        lasso_active_ = false;
        if (lasso_points_.size () < 6)
        {
            lasso_points_.clear ();
            const engine::DocState* d = session_.active ();
            if (!polygon && d != NULL && d->doc->has_selection)
            {
                run_command ("select.deselect", engine::Value::object ());
            }
            return;
        }
        engine::Value pts = engine::Value::array ();
        for (size_t i = 0; i + 1 < lasso_points_.size (); i += 2)
        {
            pts.push_back ({lasso_points_[i], lasso_points_[i + 1]});
        }
        lasso_points_.clear ();
        run_command ("select.lasso", engine::Value{{"points", pts}, {"mode", selection_mode (io, selection_mode_)},
                                         {"antiAlias", sel_anti_alias_}, {"feather", sel_feather_}});
    }

    /* Handles the pointer for the lasso and wand tools; false for other tools. */
    bool App::select_tool_input (float ox, float oy)
    {
        if (tool_ != Tool::Lasso && tool_ != Tool::PolygonLasso && tool_ != Tool::MagicWand
            && tool_ != Tool::QuickSelection)
        {
            lasso_active_ = false;
            lasso_points_.clear ();
            quick_active_ = false;
            return false;
        }
        ImGuiIO& io = ImGui::GetIO ();
        float mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        if (tool_ == Tool::QuickSelection)
        {
            /* Paint over what to select: the selection grows as the stroke is dragged (each update
               cuts the stroke's newest part) and the stroke is one history step on release. Shift
               adds, Alt subtracts; after a New stroke the options bar switches to Add, as in
               Photoshop. */
            if (activated)
            {
                quick_mode_now_ = quick_mode (io, selection_mode_);
                engine::Error err;
                quick_active_ =
                    engine::quick_stroke_begin (session_, quick_size_, quick_mode_now_, quick_all_layers_, &err);
                quick_points_.clear ();
                quick_sent_ = 0;
                if (!quick_active_)
                {
                    set_error (err.message);
                }
            }
            if (!quick_active_)
            {
                return true;
            }
            if (activated || active)
            {
                size_t n = quick_points_.size ();
                float dx = n >= 2 ? mx - quick_points_[n - 2] : 1e9f, dy = n >= 2 ? my - quick_points_[n - 1] : 0.0f;
                if (dx * dx + dy * dy >= 1.0f)
                {
                    quick_points_.push_back (mx);
                    quick_points_.push_back (my);
                }
                /* Cut again once the brush has moved a quarter of its size (and on the first press). */
                float step  = fmaxf (quick_size_ * 0.25f, 2.0f);
                size_t sent = quick_sent_;
                bool due    = sent == 0;
                if (!due && quick_points_.size () >= 2)
                {
                    float ex = quick_points_[quick_points_.size () - 2] - quick_points_[sent - 2],
                          ey = quick_points_[quick_points_.size () - 1] - quick_points_[sent - 1];
                    due      = ex * ex + ey * ey >= step * step;
                }
                if (due)
                {
                    quick_update ();
                }
            }
            if (released)
            {
                quick_active_ = false;
                quick_update ();
                clear_preview ();
                engine::Value pts = engine::Value::array ();
                for (size_t i = 0; i + 1 < quick_points_.size (); i += 2)
                {
                    pts.push_back ({quick_points_[i], quick_points_[i + 1]});
                }
                quick_points_.clear ();
                if (session_.quick.active)
                {
                    run_command (
                        "select.quick", engine::Value{{"points", pts}, {"size", quick_size_}, {"mode", quick_mode_now_},
                                            {"sampleAllLayers", quick_all_layers_}, {"progressive", true}});
                }
                if (selection_mode_ == 0)
                {
                    selection_mode_ = 1;
                }
            }
            return true;
        }
        if (tool_ == Tool::MagicWand)
        {
            if (activated)
            {
                run_command ("select.magicWand",
                    engine::Value{{"x", mx}, {"y", my}, {"tolerance", wand_tolerance_},
                        {"contiguous", wand_contiguous_}, {"antiAlias", sel_anti_alias_},
                        {"sampleAllLayers", wand_all_layers_}, {"mode", selection_mode (io, selection_mode_)}});
            }
            return true;
        }
        if (tool_ == Tool::Lasso)
        {
            if (activated)
            {
                lasso_active_ = true;
                lasso_points_.clear ();
                lasso_points_.push_back (mx);
                lasso_points_.push_back (my);
            }
            if (!lasso_active_)
            {
                return true;
            }
            if (active)
            {
                size_t n = lasso_points_.size ();
                float dx = mx - lasso_points_[n - 2], dy = my - lasso_points_[n - 1];
                if (dx * dx + dy * dy >= 0.25f)
                {
                    lasso_points_.push_back (mx);
                    lasso_points_.push_back (my);
                }
            }
            if (released)
            {
                finish_lasso (false);
            }
            return true;
        }
        /* Polygonal Lasso. */
        if (lasso_active_ && ImGui::IsKeyPressed (ImGuiKey_Escape))
        {
            lasso_active_ = false;
            lasso_points_.clear ();
            return true;
        }
        if (lasso_active_ && (ImGui::IsKeyPressed (ImGuiKey_Enter) || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter)))
        {
            finish_lasso (true);
            return true;
        }
        if (activated)
        {
            if (lasso_active_ && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            {
                finish_lasso (true);
                return true;
            }
            if (lasso_active_ && lasso_points_.size () >= 6)
            {
                float fx = (lasso_points_[0] - mx) * zoom_, fy = (lasso_points_[1] - my) * zoom_;
                if (fx * fx + fy * fy <= 36.0f)
                {
                    finish_lasso (true);
                    return true;
                }
            }
            if (!lasso_active_)
            {
                lasso_points_.clear ();
            }
            lasso_active_ = true;
            lasso_points_.push_back (mx);
            lasso_points_.push_back (my);
        }
        return true;
    }

    /* A line drawn black over a white halo, readable on any image. */
    void cursor_line (ImDrawList* dl, ImVec2 a, ImVec2 b, float k)
    {
        dl->AddLine (a, b, IM_COL32 (255, 255, 255, 230), 3.0f * k);
        dl->AddLine (a, b, IM_COL32 (0, 0, 0, 255), 1.0f * k);
    }

    /* The points not yet sent go to the engine's stroke; the canvas shows the selection so far. */
    void App::quick_update ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL || quick_sent_ >= quick_points_.size ())
        {
            return;
        }
        std::vector<float> fresh (quick_points_.begin () + (ptrdiff_t) quick_sent_, quick_points_.end ());
        quick_sent_ = quick_points_.size ();
        engine::Error err;
        if (!engine::quick_stroke_extend (session_, fresh, &err))
        {
            quick_active_ = false;
            set_error (err.message);
            clear_preview ();
            return;
        }
        std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
        copy->selection                     = session_.quick.preview;
        copy->has_selection                 = session_.quick.preview.tile_count () > 0;
        preview_doc_                        = copy;
        preview_serial_++;
    }

    /* The mode badge: + adds, - subtracts, x intersects (nothing for a new selection). */
    void mode_badge (ImDrawList* dl, ImVec2 c, const char* mode, float k)
    {
        float r = 3.0f * k;
        if (strcmp (mode, "add") == 0 || strcmp (mode, "subtract") == 0)
        {
            cursor_line (dl, ImVec2 (c.x - r, c.y), ImVec2 (c.x + r, c.y), k);
        }
        if (strcmp (mode, "add") == 0)
        {
            cursor_line (dl, ImVec2 (c.x, c.y - r), ImVec2 (c.x, c.y + r), k);
        }
        if (strcmp (mode, "intersect") == 0)
        {
            cursor_line (dl, ImVec2 (c.x - r, c.y - r), ImVec2 (c.x + r, c.y + r), k);
            cursor_line (dl, ImVec2 (c.x - r, c.y + r), ImVec2 (c.x + r, c.y - r), k);
        }
    }

    /* Over the canvas the selection tools show Photoshop's cursors: a crosshair for the marquees,
       the tool's icon for the lassos and the wand (its point at the pointer), each with the badge of
       the mode the next click will use. */
    void App::selection_cursor ()
    {
        bool marquee = tool_ == Tool::RectMarquee || tool_ == Tool::EllipseMarquee;
        bool other   = tool_ == Tool::Lasso || tool_ == Tool::PolygonLasso || tool_ == Tool::MagicWand;
        if ((!marquee && !other) || !ImGui::IsItemHovered () || ImGui::IsKeyDown (ImGuiKey_Space)
            || session_.active () == NULL)
        {
            return;
        }
        ImGuiIO& io = ImGui::GetIO ();
        ImGui::SetMouseCursor (ImGuiMouseCursor_None);
        ImDrawList* dl = ImGui::GetForegroundDrawList ();
        float k        = ui_scale_;
        ImVec2 m (floorf (io.MousePos.x) + 0.5f, floorf (io.MousePos.y) + 0.5f);
        const char* mode = selection_mode (io, selection_mode_);
        if (marquee)
        {
            float a = 7.0f * k, gap = 2.0f * k;
            cursor_line (dl, ImVec2 (m.x - a, m.y), ImVec2 (m.x - gap, m.y), k);
            cursor_line (dl, ImVec2 (m.x + gap, m.y), ImVec2 (m.x + a, m.y), k);
            cursor_line (dl, ImVec2 (m.x, m.y - a), ImVec2 (m.x, m.y - gap), k);
            cursor_line (dl, ImVec2 (m.x, m.y + gap), ImVec2 (m.x, m.y + a), k);
            mode_badge (dl, ImVec2 (m.x + 11.0f * k, m.y + 11.0f * k), mode, k);
            return;
        }
        /* The icon's working point sits at the pointer: the lassos' tail (bottom left), the wand's
           tip (top left). */
        float g          = 18.0f * k;
        const char* icon = tool_ == Tool::Lasso ? "lasso" : tool_ == Tool::PolygonLasso ? "pentagon" : "wand-sparkles";
        ImVec2 at        = tool_ == Tool::MagicWand ? ImVec2 (m.x - 2.0f * k, m.y - 2.0f * k)
                                                    : ImVec2 (m.x - 2.0f * k, m.y - g + 2.0f * k);
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                if (dx != 0 || dy != 0)
                {
                    draw_icon (dl, &icons_, icon, ImVec2 (at.x + (float) dx, at.y + (float) dy), g,
                        IM_COL32 (255, 255, 255, 220));
                }
            }
        }
        draw_icon (dl, &icons_, icon, at, g, IM_COL32 (0, 0, 0, 255));
        mode_badge (dl, ImVec2 (at.x + g + 5.0f * k, tool_ == Tool::MagicWand ? at.y + g : at.y + 4.0f * k), mode, k);
    }

    void App::select_tool_overlay (float ox, float oy)
    {
        selection_cursor ();
        if (tool_ == Tool::QuickSelection)
        {
            /* The brush outline, and the stroke so far. */
            ImDrawList* dl = ImGui::GetWindowDrawList ();
            float rad      = quick_size_ * 0.5f * zoom_;
            for (size_t i = 0; i + 1 < quick_points_.size (); i += 2)
            {
                dl->AddCircleFilled (ImVec2 (ox + quick_points_[i] * zoom_, oy + quick_points_[i + 1] * zoom_), rad,
                    IM_COL32 (120, 170, 255, 40));
            }
            if (ImGui::IsItemHovered () || quick_active_)
            {
                ImVec2 m = ImGui::GetIO ().MousePos;
                dl->AddCircle (m, rad, IM_COL32 (0, 0, 0, 200), 0, 2.0f);
                dl->AddCircle (m, rad, IM_COL32 (255, 255, 255, 255), 0, 1.0f);
                /* The brush shows + or - inside it, as Photoshop's does. */
                const char* mode = quick_mode (ImGui::GetIO (), selection_mode_);
                mode_badge (dl, m, strcmp (mode, "replace") == 0 ? "none" : mode, ui_scale_);
            }
            return;
        }
        if (!lasso_active_ || lasso_points_.size () < 2)
        {
            return;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        std::vector<ImVec2> pts;
        for (size_t i = 0; i + 1 < lasso_points_.size (); i += 2)
        {
            pts.push_back (ImVec2 (ox + lasso_points_[i] * zoom_, oy + lasso_points_[i + 1] * zoom_));
        }
        if (tool_ == Tool::PolygonLasso)
        {
            pts.push_back (ImGui::GetIO ().MousePos);
        }
        dl->AddPolyline (pts.data (), (int) pts.size (), IM_COL32 (0, 0, 0, 220), 0, 2.0f);
        dl->AddPolyline (pts.data (), (int) pts.size (), IM_COL32 (255, 255, 255, 255), 0, 1.0f);
    }

    void App::select_options ()
    {
        bool marquee = tool_ == Tool::RectMarquee || tool_ == Tool::EllipseMarquee;
        bool lasso   = tool_ == Tool::Lasso || tool_ == Tool::PolygonLasso;
        if (!marquee && !lasso && tool_ != Tool::MagicWand && tool_ != Tool::QuickSelection)
        {
            return;
        }
        float k = ui_scale_, box = ImGui::GetFrameHeight ();
        const char* icons[4] = {"square", "square-plus", "squares-subtract", "copy"};
        const char* tips[4]  = {"New Selection", "Add to Selection  (Shift)", "Subtract from Selection  (Alt)",
             "Intersect with Selection  (Shift+Alt)"};
        int modes            = tool_ == Tool::QuickSelection ? 3 : 4;
        if (selection_mode_ >= modes)
        {
            selection_mode_ = 0;
        }
        for (int i = 0; i < modes; i++)
        {
            char id[16];
            snprintf (id, sizeof id, "##selmode%d", i);
            if (icon_button (id, icons[i], box, selection_mode_ == i, tips[i]))
            {
                selection_mode_ = i;
            }
            ImGui::SameLine (0, 2 * k);
        }
        ImGui::SameLine (0, 14 * k);
        if (tool_ == Tool::QuickSelection)
        {
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ("Size:");
            ImGui::SameLine ();
            ImGui::SetNextItemWidth (90 * k);
            ImGui::DragFloat ("##quicksize", &quick_size_, 0.5f, 1.0f, 5000.0f, "%.0f px",
                ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine (0, 14 * k);
            ImGui::Checkbox ("Sample All Layers", &quick_all_layers_);
            select_and_mask_button ();
            return;
        }
        if (tool_ == Tool::MagicWand)
        {
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ("Tolerance:");
            ImGui::SameLine ();
            ImGui::SetNextItemWidth (80 * k);
            ImGui::DragInt ("##tolerance", &wand_tolerance_, 0.5f, 0, 255, "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine (0, 14 * k);
            ImGui::Checkbox ("Anti-alias", &sel_anti_alias_);
            ImGui::SameLine (0, 14 * k);
            ImGui::Checkbox ("Contiguous", &wand_contiguous_);
            ImGui::SameLine (0, 14 * k);
            ImGui::Checkbox ("Sample All Layers", &wand_all_layers_);
            select_and_mask_button ();
            return;
        }
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Feather:");
        ImGui::SameLine ();
        ImGui::SetNextItemWidth (90 * k);
        ImGui::DragFloat ("##feather", &sel_feather_, 1.0f, 0.0f, 250.0f, "%.1f px",
            ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        if (tool_ != Tool::RectMarquee)
        {
            ImGui::SameLine (0, 14 * k);
            ImGui::Checkbox ("Anti-alias", &sel_anti_alias_);
        }
        select_and_mask_button ();
    }

    /* The selection tools' "Select and Mask…" button, as in Photoshop's options bar. */
    void App::select_and_mask_button ()
    {
        ImGui::SameLine (0, 18 * ui_scale_);
        std::string why;
        const engine::CommandSpec* spec = session_.find_command ("select.selectAndMask");
        bool enabled                    = spec != NULL && session_.command_enabled (*spec, &why);
        if (!enabled)
        {
            ImGui::BeginDisabled ();
        }
        if (ImGui::Button ("Select and Mask…"))
        {
            open_select_and_mask ();
        }
        if (!enabled)
        {
            ImGui::EndDisabled ();
        }
        if (!enabled && ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip ("%s", why.c_str ());
        }
    }

    /* Select > Modify > Expand / Contract / Border / Smooth / Feather. */
    void App::modify_dialog ()
    {
        if (modify_command_.empty ())
        {
            return;
        }
        const engine::CommandSpec* spec = session_.find_command (modify_command_.c_str ());
        if (spec == NULL)
        {
            modify_command_.clear ();
            return;
        }
        char title[128];
        snprintf (title, sizeof title, "%s###modify", spec->label);
        ImGui::OpenPopup (title);
        if (!ImGui::BeginPopupModal (title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        bool feather = modify_command_ == "select.modify.feather", border = modify_command_ == "select.modify.border";
        float mx = feather ? 1000.0f : border ? 200.0f : 500.0f;
        ImGui::SetNextItemWidth (200.0f);
        ImGui::InputFloat (feather  ? "Feather Radius (px)"
                           : border ? "Width (px)"
                                    : "By (px)",
            &modify_radius_, 1.0f, 10.0f, "%.1f");
        if (modify_radius_ < (feather ? 0.1f : 1.0f))
        {
            modify_radius_ = feather ? 0.1f : 1.0f;
        }
        if (modify_radius_ > mx)
        {
            modify_radius_ = mx;
        }
        if (!border)
        {
            ImGui::Checkbox ("Apply effect at canvas bounds", &modify_at_bounds_);
        }
        bool ok = ImGui::Button ("OK") || ImGui::IsKeyPressed (ImGuiKey_Enter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel") || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok)
        {
            std::string id  = modify_command_;
            engine::Value p = {{"radius", modify_radius_}};
            if (!border)
            {
                p["applyAtCanvasBounds"] = modify_at_bounds_;
            }
            modify_command_.clear ();
            ImGui::CloseCurrentPopup ();
            run_command (id.c_str (), p);
        }
        else if (cancel)
        {
            modify_command_.clear ();
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Select > Color Range: what to select, fuzziness and invert; Sampled Colors match the
       foreground colour (Alt-click with the Eyedropper is not ported yet). */
    void App::color_range_dialog ()
    {
        if (!show_color_range_)
        {
            return;
        }
        ImGui::OpenPopup ("Color Range###colorRange");
        if (!ImGui::BeginPopupModal ("Color Range###colorRange", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::SetNextItemWidth (240.0f);
        ImGui::Combo ("Select", &range_select_, RANGE_LABELS);
        bool tones = range_select_ >= 7;
        bool hue   = range_select_ >= 1 && range_select_ <= 6;
        if (!hue)
        {
            ImGui::SetNextItemWidth (240.0f);
            ImGui::SliderFloat (
                "Fuzziness", &range_fuzziness_, 0.0f, tones ? 100.0f : 200.0f, tones ? "%.0f%%" : "%.0f");
        }
        ImGui::Checkbox ("Invert", &range_invert_);
        bool ok = ImGui::Button ("OK") || ImGui::IsKeyPressed (ImGuiKey_Enter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel") || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok)
        {
            engine::Value p = {{"select", RANGE_PRESETS[range_select_]}, {"invert", range_invert_}};
            if (!hue)
            {
                p["fuzziness"] = tones && range_fuzziness_ > 100.0f ? 100.0f : range_fuzziness_;
            }
            if (range_select_ == 0)
            {
                const float* fg = session_.tools.foreground;
                p["colors"]     = engine::Value::array ();
                char hex[8];
                snprintf (hex, sizeof hex, "#%02x%02x%02x", (int) lroundf (fg[0] * 255.0f),
                    (int) lroundf (fg[1] * 255.0f), (int) lroundf (fg[2] * 255.0f));
                p["colors"].push_back (hex);
            }
            show_color_range_ = false;
            ImGui::CloseCurrentPopup ();
            run_command ("select.colorRange", p);
        }
        else if (cancel)
        {
            show_color_range_ = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

} // namespace ui

namespace ui
{

    /* Select > Save Selection: into a new channel (named here) or combined into an existing one. */
    void App::save_selection_dialog ()
    {
        if (!show_save_selection_)
        {
            return;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            show_save_selection_ = false;
            return;
        }
        ImGui::OpenPopup ("Save Selection###savesel");
        if (!ImGui::BeginPopupModal ("Save Selection###savesel", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k                                     = ui_scale_;
        const std::vector<doc::AlphaChannel>& chans = d->doc->channels;
        if (save_channel_ > (int) chans.size ())
        {
            save_channel_ = 0;
        }
        ImGui::SeparatorText ("Destination");
        ImGui::SetNextItemWidth (200 * k);
        if (ImGui::BeginCombo ("Channel", save_channel_ == 0 ? "New" : chans[(size_t) save_channel_ - 1].name.c_str ()))
        {
            if (ImGui::Selectable ("New", save_channel_ == 0))
            {
                save_channel_ = 0;
            }
            for (size_t i = 0; i < chans.size (); i++)
            {
                ImGui::PushID ((int) i);
                if (ImGui::Selectable (chans[i].name.c_str (), save_channel_ == (int) i + 1))
                {
                    save_channel_ = (int) i + 1;
                }
                ImGui::PopID ();
            }
            ImGui::EndCombo ();
        }
        if (save_channel_ == 0)
        {
            ImGui::SetNextItemWidth (200 * k);
            ImGui::InputText ("Name", channel_name_, sizeof channel_name_);
        }
        ImGui::SeparatorText ("Operation");
        const char* ops[4] = {"Replace Channel", "Add to Channel", "Subtract from Channel", "Intersect with Channel"};
        for (int i = 0; i < 4; i++)
        {
            ImGui::BeginDisabled (save_channel_ == 0 && i > 0);
            ImGui::RadioButton (ops[i], &save_operation_, i);
            ImGui::EndDisabled ();
        }
        ImGui::Spacing ();
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
               || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok)
        {
            engine::Value p = {{"channel", save_channel_ - 1},
                {"operation", SELECTION_MODES[save_channel_ == 0 ? 0 : save_operation_]}};
            if (save_channel_ == 0 && channel_name_[0] != 0)
            {
                p["name"] = channel_name_;
            }
            show_save_selection_ = false;
            ImGui::CloseCurrentPopup ();
            run_command ("select.save", p);
        }
        else if (cancel)
        {
            show_save_selection_ = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Select > Load Selection: a channel, optionally inverted, combined with the selection. */
    void App::load_selection_dialog ()
    {
        if (!show_load_selection_)
        {
            return;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            show_load_selection_ = false;
            return;
        }
        ImGui::OpenPopup ("Load Selection###loadsel");
        if (!ImGui::BeginPopupModal ("Load Selection###loadsel", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k                                     = ui_scale_;
        const std::vector<doc::AlphaChannel>& chans = d->doc->channels;
        if (load_channel_ >= (int) chans.size ())
        {
            load_channel_ = 0;
        }
        ImGui::SeparatorText ("Source");
        ImGui::SetNextItemWidth (200 * k);
        if (ImGui::BeginCombo ("Channel", chans.empty () ? "" : chans[(size_t) load_channel_].name.c_str ()))
        {
            for (size_t i = 0; i < chans.size (); i++)
            {
                ImGui::PushID ((int) i);
                if (ImGui::Selectable (chans[i].name.c_str (), load_channel_ == (int) i))
                {
                    load_channel_ = (int) i;
                }
                ImGui::PopID ();
            }
            ImGui::EndCombo ();
        }
        ImGui::Checkbox ("Invert", &load_invert_);
        ImGui::SeparatorText ("Operation");
        const char* ops[4] = {
            "New Selection", "Add to Selection", "Subtract from Selection", "Intersect with Selection"};
        for (int i = 0; i < 4; i++)
        {
            ImGui::BeginDisabled (!d->doc->has_selection && i > 0);
            ImGui::RadioButton (ops[i], &load_operation_, i);
            ImGui::EndDisabled ();
        }
        ImGui::Spacing ();
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
               || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok && !chans.empty ())
        {
            engine::Value p      = {{"channel", load_channel_}, {"invert", load_invert_},
                     {"mode", SELECTION_MODES[d->doc->has_selection ? load_operation_ : 0]}};
            show_load_selection_ = false;
            ImGui::CloseCurrentPopup ();
            run_command ("select.load", p);
        }
        else if (cancel || chans.empty ())
        {
            show_load_selection_ = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

} // namespace ui
