#include "algo/deform.h"
#include "engine/transform.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <string.h>

namespace ui
{

    /* Puppet Warp and Perspective Warp: canvas modes like Free Transform. Puppet Warp lays a mesh
       over the active layer; a click adds a pin, dragging a pin pulls the image with it, Alt+click
       removes one. Perspective Warp first lays out a plane (drag its corners onto the image's
       perspective), then in Warp drags the same corners to where they should go. The canvas
       previews the result; Enter commits one command (Enter in Layout goes on to Warp), Escape
       cancels. */

    const char* const PIN_MODES[] = {"rigid", "normal", "distort"};

    /* The active layer's frame, refusing with the commit's own check (locks, smart objects). */
    bool warp_mode_target (engine::Session& s, const char* cmd, const engine::Value& probe, doc::LayerId* layer,
        geom::Rect* bounds, std::string* why)
    {
        const engine::DocState* d = s.active ();
        if (d == NULL)
        {
            return false;
        }
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l == NULL)
        {
            *why = "no active layer";
            return false;
        }
        *bounds = engine::transform_bounds (*d->doc, *l);
        if (bounds->is_empty ())
        {
            *why = "nothing to warp";
            return false;
        }
        doc::Document copy = *d->doc;
        engine::Value p    = probe;
        p["layer"]         = l->id;
        engine::Error err;
        if (!engine::apply_deform_command (s, &copy, cmd, p, &err))
        {
            *why = err.message;
            return false;
        }
        *layer = l->id;
        return true;
    }

    bool App::begin_puppet_warp ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL || stroking_ || moving_)
        {
            return false;
        }
        std::string why;
        geom::Rect b;
        if (!warp_mode_target (session_, "edit.puppetWarp", {{"pins", engine::Value::array ()}}, &pw_.layer, &b, &why))
        {
            if (!why.empty ())
            {
                set_error (why);
            }
            return false;
        }
        pw_.active   = true;
        pw_.doc      = d->doc->id;
        pw_.revision = d->revision;
        pw_.rect[0]  = b.x0;
        pw_.rect[1]  = b.y0;
        pw_.rect[2]  = b.x1;
        pw_.rect[3]  = b.y1;
        pw_.pins.clear ();
        pw_.drag = -1;
        return true;
    }

    bool App::begin_perspective_warp ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL || stroking_ || moving_)
        {
            return false;
        }
        std::string why;
        geom::Rect b;
        engine::Value unit = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        if (!warp_mode_target (session_, "edit.perspectiveWarp", {{"from", unit}, {"to", unit}}, &vw_.layer, &b, &why))
        {
            if (!why.empty ())
            {
                set_error (why);
            }
            return false;
        }
        vw_.active   = true;
        vw_.doc      = d->doc->id;
        vw_.revision = d->revision;
        vw_.warping  = false;
        double x0 = b.x0, y0 = b.y0, x1 = b.x1, y1 = b.y1;
        double q[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        memcpy (vw_.from, q, sizeof q);
        memcpy (vw_.to, q, sizeof q);
        vw_.drag = -1;
        return true;
    }

    bool App::warp_mode_active () const
    {
        return pw_.active || vw_.active;
    }

    engine::Value puppet_params (const PuppetWarp& pw, bool preview)
    {
        engine::Value pins = engine::Value::array ();
        for (size_t i = 0; i + 3 < pw.pins.size (); i += 4)
        {
            pins.push_back ({pw.pins[i], pw.pins[i + 1], pw.pins[i + 2], pw.pins[i + 3]});
        }
        engine::Value p    = {{"layer", pw.layer}, {"pins", pins}, {"mode", PIN_MODES[pw.mode]}};
        p["interpolation"] = preview && pw.drag >= 0 ? "bilinear" : "bicubic";
        return p;
    }

    engine::Value perspective_params (const PerspectiveWarp& vw)
    {
        engine::Value from = engine::Value::array (), to = engine::Value::array ();
        for (int i = 0; i < 4; i++)
        {
            from.push_back ({vw.from[i][0], vw.from[i][1]});
            to.push_back ({vw.to[i][0], vw.to[i][1]});
        }
        return {{"layer", vw.layer}, {"from", from}, {"to", to}};
    }

    bool puppet_moved (const PuppetWarp& pw)
    {
        for (size_t i = 0; i + 3 < pw.pins.size (); i += 4)
        {
            if (fabs (pw.pins[i] - pw.pins[i + 2]) > 1e-9 || fabs (pw.pins[i + 1] - pw.pins[i + 3]) > 1e-9)
            {
                return true;
            }
        }
        return false;
    }

    void App::cancel_warp_mode ()
    {
        if (!warp_mode_active ())
        {
            return;
        }
        pw_.active = false;
        vw_.active = false;
        clear_preview ();
    }

    void App::commit_warp_mode ()
    {
        if (pw_.active)
        {
            pw_.active = false;
            clear_preview ();
            if (puppet_moved (pw_))
            {
                run_command ("edit.puppetWarp", puppet_params (pw_, false));
            }
            return;
        }
        if (vw_.active)
        {
            if (!vw_.warping)
            {
                vw_.warping = true;
                memcpy (vw_.to, vw_.from, sizeof vw_.to);
                return;
            }
            vw_.active = false;
            clear_preview ();
            if (memcmp (vw_.from, vw_.to, sizeof vw_.to) != 0)
            {
                run_command ("edit.perspectiveWarp", perspective_params (vw_));
            }
        }
    }

    /* The warped document on a copy. */
    void App::warp_mode_preview ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        if (vw_.active && !vw_.warping)
        {
            clear_preview ();
            return;
        }
        std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
        engine::Error err;
        bool ok = pw_.active ? engine::apply_deform_command (
                                   session_, copy.get (), "edit.puppetWarp", puppet_params (pw_, true), &err)
                             : engine::apply_deform_command (
                                   session_, copy.get (), "edit.perspectiveWarp", perspective_params (vw_), &err);
        if (!ok)
        {
            return;
        }
        preview_doc_ = copy;
        preview_serial_++;
    }

    algo::Deform puppet_deform (const PuppetWarp& pw)
    {
        algo::Deform f = algo::Deform::patch_identity (pw.rect);
        f.kind         = algo::DeformKind::Pins;
        f.mode         = (algo::PinMode) pw.mode;
        for (size_t i = 0; i + 3 < pw.pins.size (); i += 4)
        {
            f.from.push_back (pw.pins[i]);
            f.from.push_back (pw.pins[i + 1]);
            f.to.push_back (pw.pins[i + 2]);
            f.to.push_back (pw.pins[i + 3]);
        }
        return f;
    }

    /* Where the shown point (x, y) came from: the warp undone by fixed-point iteration. */
    void puppet_unmap (const PuppetWarp& pw, double x, double y, double* sx, double* sy)
    {
        *sx = x;
        *sy = y;
        if (pw.pins.empty ())
        {
            return;
        }
        algo::Deform f = puppet_deform (pw);
        for (int it = 0; it < 30; it++)
        {
            double fx, fy;
            algo::deform_point (f, *sx, *sy, &fx, &fy);
            double ex = x - fx, ey = y - fy;
            *sx += ex;
            *sy += ey;
            if (fabs (ex) + fabs (ey) < 1e-3)
            {
                break;
            }
        }
    }

    void App::puppet_input (float ox, float oy)
    {
        ImGuiIO& io = ImGui::GetIO ();
        double mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        double reach   = 8.0 / zoom_;
        int over       = -1;
        double best    = reach;
        for (size_t i = 0; i + 3 < pw_.pins.size (); i += 4)
        {
            double dd = hypot (mx - pw_.pins[i + 2], my - pw_.pins[i + 3]);
            if (dd <= best)
            {
                best = dd;
                over = (int) (i / 4);
            }
        }
        if (ImGui::IsItemHovered () && pw_.drag < 0)
        {
            ImGui::SetMouseCursor (over >= 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
        }
        if (activated)
        {
            if (io.KeyAlt)
            {
                if (over >= 0)
                {
                    pw_.pins.erase (pw_.pins.begin () + over * 4, pw_.pins.begin () + over * 4 + 4);
                    warp_mode_preview ();
                }
                return;
            }
            if (over < 0)
            {
                double sx, sy;
                puppet_unmap (pw_, mx, my, &sx, &sy);
                pw_.pins.push_back (sx);
                pw_.pins.push_back (sy);
                pw_.pins.push_back (mx);
                pw_.pins.push_back (my);
                over = (int) (pw_.pins.size () / 4) - 1;
            }
            pw_.drag    = over;
            pw_.press_x = mx;
            pw_.press_y = my;
            pw_.grab_x  = pw_.pins[(size_t) over * 4 + 2];
            pw_.grab_y  = pw_.pins[(size_t) over * 4 + 3];
        }
        if (pw_.drag < 0)
        {
            return;
        }
        if (active)
        {
            double nx = pw_.grab_x + mx - pw_.press_x, ny = pw_.grab_y + my - pw_.press_y;
            size_t k = (size_t) pw_.drag * 4;
            if (nx != pw_.pins[k + 2] || ny != pw_.pins[k + 3])
            {
                pw_.pins[k + 2] = nx;
                pw_.pins[k + 3] = ny;
                warp_mode_preview ();
            }
        }
        if (released)
        {
            pw_.drag = -1;
            warp_mode_preview ();
        }
    }

    /* The mesh (the frame's grid carried by the pins) and the pins, the dragged one filled. */
    void App::puppet_overlay (float ox, float oy)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        if (pw_.show_mesh)
        {
            algo::Deform f = puppet_deform (pw_);
            double w = pw_.rect[2] - pw_.rect[0], h = pw_.rect[3] - pw_.rect[1];
            int nx = (int) fmax (2.0, fmin (24.0, w / 24.0)), ny = (int) fmax (2.0, fmin (24.0, h / 24.0));
            const int STEPS = 24;
            for (int dir = 0; dir < 2; dir++)
            {
                int lines = dir == 0 ? ny : nx;
                for (int line = 0; line <= lines; line++)
                {
                    ImVec2 pts[STEPS + 1];
                    for (int i = 0; i <= STEPS; i++)
                    {
                        double u = dir == 0 ? (double) i / STEPS : (double) line / lines,
                               v = dir == 0 ? (double) line / lines : (double) i / STEPS, x, y;
                        algo::deform_point (f, pw_.rect[0] + u * w, pw_.rect[1] + v * h, &x, &y);
                        pts[i] = ImVec2 (ox + (float) x * zoom_, oy + (float) y * zoom_);
                    }
                    dl->AddPolyline (pts, STEPS + 1, IM_COL32 (0, 0, 0, 90), 0, 1.0f);
                }
            }
        }
        for (size_t i = 0; i + 3 < pw_.pins.size (); i += 4)
        {
            ImVec2 c (ox + (float) pw_.pins[i + 2] * zoom_, oy + (float) pw_.pins[i + 3] * zoom_);
            bool sel = pw_.drag == (int) (i / 4);
            dl->AddCircleFilled (c, 5.0f, sel ? IM_COL32 (40, 40, 40, 255) : IM_COL32 (255, 220, 0, 255));
            dl->AddCircle (c, 5.0f, IM_COL32 (0, 0, 0, 255), 0, 1.5f);
        }
    }

    /* Drag a corner of the plane (Layout) or of where it goes (Warp). */
    void App::perspective_input (float ox, float oy)
    {
        ImGuiIO& io = ImGui::GetIO ();
        double mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        double (*q)[2] = vw_.warping ? vw_.to : vw_.from;
        double reach = 9.0 / zoom_, best = reach;
        int over = -1;
        for (int i = 0; i < 4; i++)
        {
            double dd = hypot (mx - q[i][0], my - q[i][1]);
            if (dd <= best)
            {
                best = dd;
                over = i;
            }
        }
        if (ImGui::IsItemHovered () && vw_.drag < 0 && over >= 0)
        {
            ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
        }
        if (activated && over >= 0)
        {
            vw_.drag    = over;
            vw_.press_x = mx;
            vw_.press_y = my;
            vw_.grab_x  = q[over][0];
            vw_.grab_y  = q[over][1];
        }
        if (vw_.drag < 0)
        {
            return;
        }
        if (active)
        {
            double nx = vw_.grab_x + mx - vw_.press_x, ny = vw_.grab_y + my - vw_.press_y;
            /* Shift keeps the corner on its row or column. */
            if (io.KeyShift)
            {
                if (fabs (mx - vw_.press_x) > fabs (my - vw_.press_y))
                {
                    ny = vw_.grab_y;
                }
                else
                {
                    nx = vw_.grab_x;
                }
            }
            if (nx != q[vw_.drag][0] || ny != q[vw_.drag][1])
            {
                q[vw_.drag][0] = nx;
                q[vw_.drag][1] = ny;
                if (vw_.warping)
                {
                    warp_mode_preview ();
                }
            }
        }
        if (released)
        {
            vw_.drag = -1;
            if (vw_.warping)
            {
                warp_mode_preview ();
            }
        }
    }

    /* The plane with a perspective grid, its corners as handles. */
    void App::perspective_overlay (float ox, float oy)
    {
        ImDrawList* dl       = ImGui::GetWindowDrawList ();
        const double (*q)[2] = vw_.warping ? vw_.to : vw_.from;
        double unit[4]       = {0, 0, 1, 1};
        algo::Homography h;
        bool grid = algo::Homography::rect_to_quad (unit, q, &h);
        ImU32 col = vw_.warping ? IM_COL32 (255, 200, 60, 230) : IM_COL32 (60, 140, 255, 230);
        for (int line = 0; grid && line <= 6; line++)
        {
            double t = line / 6.0, ax, ay, bx, by;
            h.apply (t, 0, &ax, &ay);
            h.apply (t, 1, &bx, &by);
            dl->AddLine (ImVec2 (ox + (float) ax * zoom_, oy + (float) ay * zoom_),
                ImVec2 (ox + (float) bx * zoom_, oy + (float) by * zoom_), col, 1.0f);
            h.apply (0, t, &ax, &ay);
            h.apply (1, t, &bx, &by);
            dl->AddLine (ImVec2 (ox + (float) ax * zoom_, oy + (float) ay * zoom_),
                ImVec2 (ox + (float) bx * zoom_, oy + (float) by * zoom_), col, 1.0f);
        }
        for (int i = 0; i < 4; i++)
        {
            ImVec2 a (ox + (float) q[i][0] * zoom_, oy + (float) q[i][1] * zoom_),
                b (ox + (float) q[(i + 1) % 4][0] * zoom_, oy + (float) q[(i + 1) % 4][1] * zoom_);
            dl->AddLine (a, b, IM_COL32 (0, 0, 0, 220), 3.0f);
            dl->AddLine (a, b, col, 2.0f);
        }
        for (int i = 0; i < 4; i++)
        {
            ImVec2 c (ox + (float) q[i][0] * zoom_, oy + (float) q[i][1] * zoom_);
            dl->AddCircleFilled (c, 5.0f, IM_COL32 (255, 255, 255, 255));
            dl->AddCircle (c, 5.0f, IM_COL32 (0, 0, 0, 255), 0, 1.5f);
        }
    }

    void App::warp_mode_options ()
    {
        float k = ui_scale_;
        if (pw_.active)
        {
            opt_label ("Mode:");
            ImGui::SetNextItemWidth (100 * k);
            if (ImGui::Combo ("##pinmode", &pw_.mode, "Rigid\0Normal\0Distort\0"))
            {
                warp_mode_preview ();
            }
            opt_gap ();
            ImGui::Checkbox ("Show Mesh", &pw_.show_mesh);
            opt_gap ();
            if (ImGui::Button ("Remove All Pins"))
            {
                pw_.pins.clear ();
                clear_preview ();
            }
            opt_gap ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextDisabled (
                "%zu pins. Click: add pin. Drag: move pin. Alt+click: remove pin.", pw_.pins.size () / 4);
        }
        else
        {
            int phase = vw_.warping ? 1 : 0;
            if (ImGui::RadioButton ("Layout", phase == 0) && vw_.warping)
            {
                vw_.warping = false;
                clear_preview ();
            }
            ImGui::SameLine ();
            if (ImGui::RadioButton ("Warp", phase == 1) && !vw_.warping)
            {
                commit_warp_mode ();
            }
            if (vw_.warping)
            {
                opt_gap ();
                if (ImGui::Button ("Straighten Vertical"))
                {
                    for (int e = 0; e < 2; e++)
                    {
                        int a = e == 0 ? 0 : 1, b = e == 0 ? 3 : 2;
                        double x     = (vw_.to[a][0] + vw_.to[b][0]) / 2.0;
                        vw_.to[a][0] = vw_.to[b][0] = x;
                    }
                    warp_mode_preview ();
                }
                ImGui::SameLine ();
                if (ImGui::Button ("Straighten Horizontal"))
                {
                    for (int e = 0; e < 2; e++)
                    {
                        int a = e == 0 ? 0 : 3, b = e == 0 ? 1 : 2;
                        double y     = (vw_.to[a][1] + vw_.to[b][1]) / 2.0;
                        vw_.to[a][1] = vw_.to[b][1] = y;
                    }
                    warp_mode_preview ();
                }
            }
            opt_gap ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextDisabled (vw_.warping
                                     ? "Drag the corners to where they should go. Shift: keep to a row or column."
                                     : "Drag the corners onto the plane's perspective; Enter goes on to Warp.");
        }
        opt_gap ();
        if (ImGui::Button ("Cancel"))
        {
            cancel_warp_mode ();
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Commit"))
        {
            if (vw_.active && !vw_.warping)
            {
                vw_.warping = true;
            }
            commit_warp_mode ();
        }
    }

} // namespace ui
