#include "algo/deform.h"
#include "engine/transform.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <string.h>

namespace ui
{

    /* Free Transform (Ctrl+T): a box around the active layer's content (or the targeted unlinked
       mask) with eight handles, in document coordinates. Drag a corner to scale (proportionally;
       Shift for free), an edge to stretch, inside to move, outside to rotate (Shift snaps to 15°);
       Ctrl+drag a corner to distort, Ctrl+Shift+drag an edge to skew, Ctrl+Alt+Shift+drag a corner
       for perspective; Alt scales about the centre. Edit > Transform's Scale, Rotate, Skew, Distort
       and Perspective make every handle do that; Warp shows a Bezier mesh (16 points, or a preset
       style); Content-Aware Scale rescales by seam carving. The canvas previews the result; Enter
       commits one command, Escape cancels. */

    const char* const XF_INTERP[] = {"bicubic", "bilinear", "nearest"};

    void quad_center (const double q[4][2], double* cx, double* cy)
    {
        *cx = (q[0][0] + q[1][0] + q[2][0] + q[3][0]) / 4.0;
        *cy = (q[0][1] + q[1][1] + q[2][1] + q[3][1]) / 4.0;
    }

    /* Inside a convex quad, either winding. */
    bool quad_contains (const double q[4][2], double x, double y)
    {
        int pos = 0, neg = 0;
        for (int i = 0; i < 4; i++)
        {
            const double *a = q[i], *b = q[(i + 1) % 4];
            double c = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
            if (c > 0)
            {
                pos++;
            }
            if (c < 0)
            {
                neg++;
            }
        }
        return pos == 0 || neg == 0;
    }

    /* Solve d = a U + b V. */
    bool solve2 (double ux, double uy, double vx, double vy, double dx, double dy, double* a, double* b)
    {
        double det = ux * vy - uy * vx;
        if (fabs (det) < 1e-12)
        {
            return false;
        }
        *a = (dx * vy - dy * vx) / det;
        *b = (ux * dy - uy * dx) / det;
        return true;
    }

    const double CORNER_S[4] = {0, 1, 1, 0};
    const double CORNER_T[4] = {0, 0, 1, 1};

    void App::begin_free_transform ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l == NULL)
        {
            set_error ("no active layer");
            return;
        }
        if (stroking_ || moving_)
        {
            return;
        }
        bool mask    = mask_targeted () && !l->mask.linked;
        geom::Rect b = mask ? engine::target_bounds (*d->doc, l->mask.surface) : engine::transform_bounds (*d->doc, *l);
        if (b.is_empty ())
        {
            set_error ("nothing to transform");
            return;
        }
        xf_.active  = true;
        xf_.layer   = l->id;
        xf_.mask    = mask;
        xf_.rect[0] = b.x0;
        xf_.rect[1] = b.y0;
        xf_.rect[2] = b.x1;
        xf_.rect[3] = b.y1;
        for (int i = 0; i < 4; i++)
        {
            xf_.quad[i][0] = CORNER_S[i] > 0 ? b.x1 : b.x0;
            xf_.quad[i][1] = CORNER_T[i] > 0 ? b.y1 : b.y0;
        }
        xf_.drag     = TransformDrag::None;
        xf_.handle   = -1;
        xf_.dirty    = false;
        xf_.mode     = XF_FREE;
        xf_.doc      = d->doc->id;
        xf_.revision = d->revision;
        /* A locked layer refuses before the box opens, with the commit's own check. */
        doc::Document probe = *d->doc;
        engine::Value p     = {{"layer", l->id}, {"matrix", {1, 0, 0, 1, 0, 0}}};
        if (mask)
        {
            p["target"] = "mask";
        }
        double rect[4];
        engine::Error err;
        if (!engine::apply_transform (session_, &probe, l->id, p, rect, &err))
        {
            xf_.active = false;
            set_error (err.message);
        }
    }

    /* Free Transform in one of Edit > Transform's modes. */
    void App::begin_transform_mode (int mode)
    {
        begin_free_transform ();
        if (!xf_.active)
        {
            return;
        }
        xf_.mode = mode;
        if (mode == XF_WARP)
        {
            algo::Deform d = algo::Deform::patch_identity (xf_.rect);
            memcpy (xf_.warp, d.pts, sizeof xf_.warp);
            xf_.warp_style = -1;
            xf_.bend       = 50.0f;
            xf_.hdist = xf_.vdist = 0.0f;
            xf_.warp_vertical     = false;
            xf_.warp_custom       = true;
        }
        if (mode == XF_CAS)
        {
            xf_.cas_protect = -1;
            xf_.cas_skin    = false;
        }
    }

    /* A preset style (or -1, Custom) laid on the mesh. */
    void App::set_warp_style (int style)
    {
        xf_.warp_style = style;
        if (style < 0)
        {
            return;
        }
        algo::Deform st = algo::Deform::patch_identity (xf_.rect);
        st.kind         = algo::DeformKind::Style;
        st.style        = (algo::WarpStyle) style;
        st.bend         = xf_.bend / 100.0;
        st.hdist        = xf_.hdist / 100.0;
        st.vdist        = xf_.vdist / 100.0;
        st.vertical     = xf_.warp_vertical;
        algo::Deform patch;
        algo::style_to_patch (st, &patch);
        memcpy (xf_.warp, patch.pts, sizeof xf_.warp);
    }

    /* The command the box amounts to: edit.transform, edit.warp or edit.contentAwareScale. */
    engine::Value App::transform_params (bool preview)
    {
        engine::Value p    = {{"layer", xf_.layer}, {"rect", {xf_.rect[0], xf_.rect[1], xf_.rect[2], xf_.rect[3]}}};
        p["interpolation"] = preview && xf_.drag != TransformDrag::None ? "bilinear" : XF_INTERP[xf_.interp];
        if (xf_.mode == XF_WARP)
        {
            engine::Value pts = engine::Value::array ();
            for (int i = 0; i < 16; i++)
            {
                pts.push_back ({xf_.warp[i][0], xf_.warp[i][1]});
            }
            p["points"] = pts;
        }
        else if (xf_.mode == XF_CAS)
        {
            double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
            for (int i = 0; i < 4; i++)
            {
                x0 = fmin (x0, xf_.quad[i][0]);
                y0 = fmin (y0, xf_.quad[i][1]);
                x1 = fmax (x1, xf_.quad[i][0]);
                y1 = fmax (y1, xf_.quad[i][1]);
            }
            p["rect"] = {x0, y0, x1, y1};
            if (xf_.cas_protect >= 0)
            {
                p["protect"] = xf_.cas_protect;
            }
            p["protectSkinTones"] = xf_.cas_skin;
        }
        else
        {
            engine::Value quad = engine::Value::array ();
            for (int i = 0; i < 4; i++)
            {
                quad.push_back ({xf_.quad[i][0], xf_.quad[i][1]});
            }
            p["quad"] = quad;
        }
        if (xf_.mask)
        {
            p["target"] = "mask";
        }
        return p;
    }

    void App::cancel_free_transform ()
    {
        if (!xf_.active)
        {
            return;
        }
        xf_.active = false;
        clear_preview ();
    }

    void App::commit_free_transform ()
    {
        if (!xf_.active)
        {
            return;
        }
        xf_.active = false;
        clear_preview ();
        bool moved = false;
        for (int i = 0; i < 4; i++)
        {
            moved = moved || fabs (xf_.quad[i][0] - (CORNER_S[i] > 0 ? xf_.rect[2] : xf_.rect[0])) > 1e-9
                 || fabs (xf_.quad[i][1] - (CORNER_T[i] > 0 ? xf_.rect[3] : xf_.rect[1])) > 1e-9;
        }
        if (xf_.mode == XF_WARP)
        {
            algo::Deform id = algo::Deform::patch_identity (xf_.rect);
            moved           = memcmp (id.pts, xf_.warp, sizeof xf_.warp) != 0;
        }
        if (!moved)
        {
            return;
        }
        const char* cmd = xf_.mode == XF_WARP ? "edit.warp"
                        : xf_.mode == XF_CAS  ? "edit.contentAwareScale"
                                              : "edit.transform";
        run_command (cmd, transform_params (false));
    }

    /* The transformed document on a copy, with bilinear sampling while dragging (Content-Aware
       Scale only once the handle is let go: carving takes a moment). */
    void App::free_transform_preview ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        if (xf_.mode == XF_CAS && xf_.drag != TransformDrag::None)
        {
            return;
        }
        std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
        engine::Value p                     = transform_params (true);
        engine::Error err;
        double rect[4];
        bool ok = xf_.mode == XF_WARP ? engine::apply_deform_command (session_, copy.get (), "edit.warp", p, &err)
                : xf_.mode == XF_CAS
                    ? engine::apply_deform_command (session_, copy.get (), "edit.contentAwareScale", p, &err)
                    : engine::apply_transform (session_, copy.get (), xf_.layer, p, rect, &err);
        if (!ok)
        {
            return;
        }
        preview_doc_ = copy;
        preview_serial_++;
    }

    void App::free_transform_input (float ox, float oy)
    {
        if (xf_.mode == XF_WARP)
        {
            warp_input (ox, oy);
            return;
        }
        ImGuiIO& io = ImGui::GetIO ();
        double mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        double reach   = 7.0 / zoom_;

        /* What the pointer is over: a corner, an edge's middle, inside, or outside. */
        TransformDrag over = TransformDrag::Rotate;
        int handle         = -1;
        for (int i = 0; i < 4 && handle < 0; i++)
        {
            if (fabs (mx - xf_.quad[i][0]) <= reach && fabs (my - xf_.quad[i][1]) <= reach)
            {
                over   = TransformDrag::Corner;
                handle = i;
            }
        }
        for (int i = 0; i < 4 && handle < 0; i++)
        {
            double ex = (xf_.quad[i][0] + xf_.quad[(i + 1) % 4][0]) / 2.0,
                   ey = (xf_.quad[i][1] + xf_.quad[(i + 1) % 4][1]) / 2.0;
            if (fabs (mx - ex) <= reach && fabs (my - ey) <= reach)
            {
                over   = TransformDrag::Edge;
                handle = i;
            }
        }
        if (handle < 0 && quad_contains (xf_.quad, mx, my))
        {
            over = TransformDrag::Move;
        }
        /* Rotate mode turns with any drag; Scale and Content-Aware Scale don't rotate. */
        if (xf_.mode == XF_ROTATE)
        {
            over = TransformDrag::Rotate;
        }
        if ((xf_.mode == XF_SCALE || xf_.mode == XF_CAS) && over == TransformDrag::Rotate)
        {
            over = TransformDrag::Move;
        }
        if (ImGui::IsItemHovered () && xf_.drag == TransformDrag::None)
        {
            ImGui::SetMouseCursor (over == TransformDrag::Move     ? ImGuiMouseCursor_ResizeAll
                                   : over == TransformDrag::Rotate ? ImGuiMouseCursor_Hand
                                   : over == TransformDrag::Edge
                                       ? (handle % 2 == 0 ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW)
                                       : (handle % 2 == 0 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW));
        }
        if (activated)
        {
            if (ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left) && over == TransformDrag::Move)
            {
                commit_free_transform ();
                return;
            }
            xf_.drag    = over;
            xf_.handle  = handle;
            xf_.press_x = mx;
            xf_.press_y = my;
            memcpy (xf_.start, xf_.quad, sizeof xf_.start);
        }
        if (xf_.drag == TransformDrag::None)
        {
            return;
        }
        if (active)
        {
            const double (*s)[2] = xf_.start;
            double q[4][2];
            memcpy (q, s, sizeof q);
            double dx = mx - xf_.press_x, dy = my - xf_.press_y;
            double ux = s[1][0] - s[0][0], uy = s[1][1] - s[0][1], vx = s[3][0] - s[0][0], vy = s[3][1] - s[0][1];
            if (xf_.drag == TransformDrag::Move)
            {
                for (int i = 0; i < 4; i++)
                {
                    q[i][0] += dx;
                    q[i][1] += dy;
                }
            }
            else if (xf_.drag == TransformDrag::Rotate)
            {
                double cx, cy;
                quad_center (s, &cx, &cy);
                double a = atan2 (my - cy, mx - cx) - atan2 (xf_.press_y - cy, xf_.press_x - cx);
                if (io.KeyShift)
                {
                    a = round (a / (M_PI / 12.0)) * (M_PI / 12.0);
                }
                double c = cos (a), sn = sin (a);
                for (int i = 0; i < 4; i++)
                {
                    double px = s[i][0] - cx, py = s[i][1] - cy;
                    q[i][0] = cx + px * c - py * sn;
                    q[i][1] = cy + px * sn + py * c;
                }
            }
            else if (xf_.drag == TransformDrag::Corner
                     && (xf_.mode == XF_PERSPECTIVE || (xf_.mode == XF_FREE && io.KeyCtrl && io.KeyAlt && io.KeyShift)))
            {
                /* Perspective: the corner and its partner move apart symmetrically, along the frame
                   axis the drag follows most. */
                int h = xf_.handle;
                double a, b;
                if (solve2 (ux, uy, vx, vy, dx, dy, &a, &b))
                {
                    bool horizontal = fabs (a * hypot (ux, uy)) >= fabs (b * hypot (vx, vy));
                    int partner     = horizontal ? (h ^ 1) : 3 - h;
                    double mx2 = horizontal ? a * ux : b * vx, my2 = horizontal ? a * uy : b * vy;
                    q[h][0] += mx2;
                    q[h][1] += my2;
                    q[partner][0] -= mx2;
                    q[partner][1] -= my2;
                }
            }
            else if (xf_.drag == TransformDrag::Corner
                     && (xf_.mode == XF_DISTORT || (xf_.mode == XF_FREE && io.KeyCtrl)))
            {
                q[xf_.handle][0] += dx;
                q[xf_.handle][1] += dy;
            }
            else if (xf_.drag == TransformDrag::Edge && xf_.mode == XF_DISTORT)
            {
                int h = xf_.handle;
                for (int k = 0; k < 2; k++)
                {
                    q[(h + k) % 4][0] += dx;
                    q[(h + k) % 4][1] += dy;
                }
            }
            else if ((xf_.drag == TransformDrag::Edge
                         && (xf_.mode == XF_SKEW || xf_.mode == XF_PERSPECTIVE
                             || (xf_.mode == XF_FREE && io.KeyCtrl && io.KeyShift)))
                     || (xf_.drag == TransformDrag::Corner && xf_.mode == XF_SKEW))
            {
                /* Skew: an edge slides along itself (a corner along the edge it follows most). */
                int h = xf_.handle, a0 = h, a1 = (h + 1) % 4;
                if (xf_.drag == TransformDrag::Corner)
                {
                    int nb1 = (h + 1) % 4, nb2 = (h + 3) % 4;
                    double e1x = s[nb1][0] - s[h][0], e1y = s[nb1][1] - s[h][1], e2x = s[nb2][0] - s[h][0],
                           e2y = s[nb2][1] - s[h][1];
                    double d1  = fabs ((dx * e1x + dy * e1y) / fmax (hypot (e1x, e1y), 1e-9)),
                           d2  = fabs ((dx * e2x + dy * e2y) / fmax (hypot (e2x, e2y), 1e-9));
                    a0         = h;
                    a1         = d1 >= d2 ? nb1 : nb2;
                    double ex = s[a1][0] - s[a0][0], ey = s[a1][1] - s[a0][1], l2 = ex * ex + ey * ey;
                    double t = l2 > 0 ? (dx * ex + dy * ey) / l2 : 0;
                    q[h][0] += ex * t;
                    q[h][1] += ey * t;
                }
                else
                {
                    double ex = s[a1][0] - s[a0][0], ey = s[a1][1] - s[a0][1], l2 = ex * ex + ey * ey;
                    double t = l2 > 0 ? (dx * ex + dy * ey) / l2 : 0;
                    for (int k = 0; k < 2; k++)
                    {
                        q[(h + k) % 4][0] += ex * t;
                        q[(h + k) % 4][1] += ey * t;
                    }
                }
            }
            else
            {
                /* Scale in the frame's own axes about the opposite corner or edge (or the centre with
                   Alt). Corners scale proportionally unless Shift is held. */
                int h = xf_.handle;
                double si, ti, so, to;
                bool corner = xf_.drag == TransformDrag::Corner;
                if (corner)
                {
                    si = CORNER_S[h];
                    ti = CORNER_T[h];
                    so = CORNER_S[(h + 2) % 4];
                    to = CORNER_T[(h + 2) % 4];
                }
                else
                {
                    si = (CORNER_S[h] + CORNER_S[(h + 1) % 4]) / 2.0;
                    ti = (CORNER_T[h] + CORNER_T[(h + 1) % 4]) / 2.0;
                    so = 1.0 - si;
                    to = 1.0 - ti;
                }
                if (io.KeyAlt)
                {
                    so = 0.5;
                    to = 0.5;
                }
                double ax = s[0][0] + so * ux + to * vx, ay = s[0][1] + so * uy + to * vy;
                double a, b;
                if (solve2 (ux, uy, vx, vy, mx - ax, my - ay, &a, &b))
                {
                    double fs = fabs (si - so) > 1e-9 ? a / (si - so) : 1.0;
                    double ft = fabs (ti - to) > 1e-9 ? b / (ti - to) : 1.0;
                    if (!corner)
                    {
                        if (fabs (si - so) < 1e-9)
                        {
                            fs = 1.0;
                        }
                        if (fabs (ti - to) < 1e-9)
                        {
                            ft = 1.0;
                        }
                    }
                    else if (!io.KeyShift)
                    {
                        double f = fabs (fs) > fabs (ft) ? fs : ft;
                        fs = ft = f;
                    }
                    for (int i = 0; i < 4; i++)
                    {
                        double ls = so + (CORNER_S[i] - so) * fs, lt = to + (CORNER_T[i] - to) * ft;
                        q[i][0] = s[0][0] + ls * ux + lt * vx;
                        q[i][1] = s[0][1] + ls * uy + lt * vy;
                    }
                }
            }
            if (memcmp (q, xf_.quad, sizeof q) != 0)
            {
                memcpy (xf_.quad, q, sizeof q);
                free_transform_preview ();
            }
        }
        if (released)
        {
            xf_.drag = TransformDrag::None;
            free_transform_preview ();
        }
    }

    void App::free_transform_overlay (float ox, float oy)
    {
        if (xf_.mode == XF_WARP)
        {
            warp_overlay (ox, oy);
            return;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        ImVec2 p[4];
        for (int i = 0; i < 4; i++)
        {
            p[i] = ImVec2 (ox + (float) xf_.quad[i][0] * zoom_, oy + (float) xf_.quad[i][1] * zoom_);
        }
        for (int i = 0; i < 4; i++)
        {
            dl->AddLine (p[i], p[(i + 1) % 4], IM_COL32 (0, 0, 0, 220), 2.0f);
            dl->AddLine (p[i], p[(i + 1) % 4], IM_COL32 (255, 255, 255, 220), 1.0f);
        }
        for (int i = 0; i < 8; i++)
        {
            ImVec2 c =
                i < 4 ? p[i] : ImVec2 ((p[i - 4].x + p[(i - 3) % 4].x) * 0.5f, (p[i - 4].y + p[(i - 3) % 4].y) * 0.5f);
            dl->AddRectFilled (ImVec2 (c.x - 4, c.y - 4), ImVec2 (c.x + 4, c.y + 4), IM_COL32 (255, 255, 255, 255));
            dl->AddRect (ImVec2 (c.x - 4, c.y - 4), ImVec2 (c.x + 4, c.y + 4), IM_COL32 (0, 0, 0, 255));
        }
        double cx, cy;
        quad_center (xf_.quad, &cx, &cy);
        ImVec2 c (ox + (float) cx * zoom_, oy + (float) cy * zoom_);
        dl->AddCircle (c, 4.0f, IM_COL32 (0, 0, 0, 220), 0, 1.5f);
    }

    /* The options while transforming: the size and angle it amounts to, interpolation, commit and
       cancel. */
    void App::free_transform_options ()
    {
        float k = ui_scale_;
        opt_label ("Mode:");
        ImGui::SetNextItemWidth (130 * k);
        int mode = xf_.mode;
        if (ImGui::Combo ("##xfmode", &mode,
                "Free Transform\0Scale\0Rotate\0Skew\0Distort\0Perspective\0Warp\0Content-Aware Scale\0")
            && mode != xf_.mode)
        {
            cancel_free_transform ();
            begin_transform_mode (mode);
            return;
        }
        opt_gap ();
        if (xf_.mode == XF_WARP)
        {
            opt_label ("Warp:");
            ImGui::SetNextItemWidth (120 * k);
            std::string cur = xf_.warp_style < 0 ? "Custom" : algo::WARP_STYLE_LABELS[xf_.warp_style];
            if (ImGui::BeginCombo ("##warpstyle", cur.c_str ()))
            {
                if (ImGui::Selectable ("Custom", xf_.warp_style < 0))
                {
                    xf_.warp_style = -1;
                }
                for (int i = 0; i < algo::WARP_STYLE_COUNT; i++)
                {
                    if (ImGui::Selectable (algo::WARP_STYLE_LABELS[i], i == xf_.warp_style))
                    {
                        set_warp_style (i);
                        free_transform_preview ();
                    }
                }
                ImGui::EndCombo ();
            }
            if (xf_.warp_style >= 0)
            {
                bool changed = false;
                opt_gap ();
                changed |= ImGui::Checkbox ("Vertical", &xf_.warp_vertical);
                opt_gap ();
                opt_label ("Bend:");
                ImGui::SetNextItemWidth (70 * k);
                changed |= ImGui::DragFloat (
                    "##bend", &xf_.bend, 0.5f, -100.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                opt_gap ();
                opt_label ("H:");
                ImGui::SetNextItemWidth (60 * k);
                changed |= ImGui::DragFloat (
                    "##hdist", &xf_.hdist, 0.5f, -100.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                opt_gap ();
                opt_label ("V:");
                ImGui::SetNextItemWidth (60 * k);
                changed |= ImGui::DragFloat (
                    "##vdist", &xf_.vdist, 0.5f, -100.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                if (changed)
                {
                    set_warp_style (xf_.warp_style);
                    free_transform_preview ();
                }
            }
        }
        else if (xf_.mode == XF_CAS)
        {
            const engine::DocState* d = session_.active ();
            opt_label ("Protect:");
            ImGui::SetNextItemWidth (120 * k);
            std::string cur = xf_.cas_protect < 0 || d == NULL || (size_t) xf_.cas_protect >= d->doc->channels.size ()
                                ? "None"
                                : d->doc->channels[(size_t) xf_.cas_protect].name;
            if (ImGui::BeginCombo ("##casprotect", cur.c_str ()))
            {
                if (ImGui::Selectable ("None", xf_.cas_protect < 0))
                {
                    xf_.cas_protect = -1;
                    free_transform_preview ();
                }
                for (size_t i = 0; d != NULL && i < d->doc->channels.size (); i++)
                {
                    if (ImGui::Selectable (d->doc->channels[i].name.c_str (), (int) i == xf_.cas_protect))
                    {
                        xf_.cas_protect = (int) i;
                        free_transform_preview ();
                    }
                }
                ImGui::EndCombo ();
            }
            opt_gap ();
            if (ImGui::Checkbox ("Protect Skin Tones", &xf_.cas_skin))
            {
                free_transform_preview ();
            }
        }
        else
        {
            double w0 = xf_.rect[2] - xf_.rect[0], h0 = xf_.rect[3] - xf_.rect[1];
            double w     = hypot (xf_.quad[1][0] - xf_.quad[0][0], xf_.quad[1][1] - xf_.quad[0][1]);
            double h     = hypot (xf_.quad[3][0] - xf_.quad[0][0], xf_.quad[3][1] - xf_.quad[0][1]);
            double angle = atan2 (xf_.quad[1][1] - xf_.quad[0][1], xf_.quad[1][0] - xf_.quad[0][0]) * 180.0 / M_PI;
            ImGui::AlignTextToFramePadding ();
            ImGui::Text ("W: %.1f%%   H: %.1f%%   Angle: %.1f°", w0 > 0 ? w * 100.0 / w0 : 0.0,
                h0 > 0 ? h * 100.0 / h0 : 0.0, angle);
        }
        opt_gap ();
        if (xf_.mode != XF_CAS)
        {
            opt_label ("Interpolation:");
            ImGui::SetNextItemWidth (140 * k);
            if (ImGui::Combo ("##interp", &xf_.interp, "Bicubic\0Bilinear\0Nearest Neighbor\0"))
            {
                free_transform_preview ();
            }
            opt_gap ();
        }
        if (ImGui::Button ("Cancel"))
        {
            cancel_free_transform ();
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Commit"))
        {
            commit_free_transform ();
        }
        if (xf_.mode == XF_FREE)
        {
            opt_gap ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextDisabled (
                "Shift: free scale / snap angle. Ctrl: distort. Ctrl+Shift edge: skew. Ctrl+Alt+Shift: "
                "perspective. Alt: from centre.");
        }
    }

    /* ---- Warp ---- */

    /* Drag one of the mesh's 16 points (corners, edge handles and the four inner points); a preset
       style becomes Custom once a point is moved. */
    void App::warp_input (float ox, float oy)
    {
        ImGuiIO& io = ImGui::GetIO ();
        double mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        double reach   = 7.0 / zoom_;
        if (activated)
        {
            int best  = -1;
            double bd = reach;
            for (int i = 0; i < 16; i++)
            {
                double d = hypot (mx - xf_.warp[i][0], my - xf_.warp[i][1]);
                if (d <= bd)
                {
                    bd   = d;
                    best = i;
                }
            }
            xf_.handle  = best;
            xf_.press_x = mx;
            xf_.press_y = my;
            memcpy (xf_.warp_start, xf_.warp, sizeof xf_.warp);
            xf_.drag = best >= 0 ? TransformDrag::Corner : TransformDrag::Move;
        }
        if (xf_.drag == TransformDrag::None)
        {
            return;
        }
        if (active)
        {
            double dx = mx - xf_.press_x, dy = my - xf_.press_y;
            memcpy (xf_.warp, xf_.warp_start, sizeof xf_.warp);
            if (xf_.handle >= 0)
            {
                /* A corner carries its two handles with it. */
                int i = xf_.handle, col = i % 4, row = i / 4;
                bool corner = (col == 0 || col == 3) && (row == 0 || row == 3);
                xf_.warp[i][0] += dx;
                xf_.warp[i][1] += dy;
                if (corner)
                {
                    int hc = col == 0 ? 1 : 2, hr = row == 0 ? 1 : 2;
                    int a = row * 4 + hc, b = hr * 4 + col;
                    xf_.warp[a][0] += dx;
                    xf_.warp[a][1] += dy;
                    xf_.warp[b][0] += dx;
                    xf_.warp[b][1] += dy;
                }
            }
            else
            {
                /* Dragging the mesh itself pulls the four inner points. */
                const int inner[4] = {5, 6, 9, 10};
                for (int k = 0; k < 4; k++)
                {
                    xf_.warp[inner[k]][0] += dx;
                    xf_.warp[inner[k]][1] += dy;
                }
            }
            xf_.warp_style = -1;
            free_transform_preview ();
        }
        if (released)
        {
            xf_.drag = TransformDrag::None;
            free_transform_preview ();
        }
    }

    /* The mesh: the patch's grid curves at thirds, handle lines, and the 16 points. */
    void App::warp_overlay (float ox, float oy)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        algo::Deform d = algo::Deform::patch_identity (xf_.rect);
        memcpy (d.pts, xf_.warp, sizeof d.pts);
        const int STEPS = 24;
        for (int dir = 0; dir < 2; dir++)
        {
            for (int line = 0; line <= 3; line++)
            {
                ImVec2 pts[STEPS + 1];
                for (int i = 0; i <= STEPS; i++)
                {
                    double u = dir == 0 ? (double) i / STEPS : line / 3.0,
                           v = dir == 0 ? line / 3.0 : (double) i / STEPS, x, y;
                    algo::deform_point (d, xf_.rect[0] + u * (xf_.rect[2] - xf_.rect[0]),
                        xf_.rect[1] + v * (xf_.rect[3] - xf_.rect[1]), &x, &y);
                    pts[i] = ImVec2 (ox + (float) x * zoom_, oy + (float) y * zoom_);
                }
                dl->AddPolyline (pts, STEPS + 1, IM_COL32 (0, 0, 0, 200), 0, 2.0f);
                dl->AddPolyline (pts, STEPS + 1, IM_COL32 (255, 255, 255, 230), 0, 1.0f);
            }
        }
        for (int i = 0; i < 16; i++)
        {
            ImVec2 c (ox + (float) xf_.warp[i][0] * zoom_, oy + (float) xf_.warp[i][1] * zoom_);
            int col = i % 4, row = i / 4;
            bool corner = (col == 0 || col == 3) && (row == 0 || row == 3);
            if (!corner && (col == 0 || col == 3 || row == 0 || row == 3))
            {
                /* An edge handle: a line back to its corner. */
                int cc = col == 1 ? 0 : col == 2 ? 3 : col, cr = row == 1 ? 0 : row == 2 ? 3 : row;
                ImVec2 a (ox + (float) xf_.warp[cr * 4 + cc][0] * zoom_, oy + (float) xf_.warp[cr * 4 + cc][1] * zoom_);
                dl->AddLine (a, c, IM_COL32 (60, 140, 255, 220), 1.0f);
            }
            float r = corner ? 4.0f : 3.0f;
            dl->AddRectFilled (ImVec2 (c.x - r, c.y - r), ImVec2 (c.x + r, c.y + r),
                corner ? IM_COL32 (255, 255, 255, 255) : IM_COL32 (60, 140, 255, 255));
            dl->AddRect (ImVec2 (c.x - r, c.y - r), ImVec2 (c.x + r, c.y + r), IM_COL32 (0, 0, 0, 255));
        }
    }

} // namespace ui
