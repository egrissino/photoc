#include "engine/vector.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include "vec/vec.h"
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui
{

    /* The vector tools: the Pen (click for corners, drag for smooth points, click the first point to
       close; over the path it adds and deletes points, Alt converts them), Path Selection (whole
       subpaths) and Direct Selection (points and handles), the shape tools (Rectangle, Ellipse,
       Polygon, Line: a new shape layer, a subpath or pixels) and the Paths panel. The tools edit a
       local copy of the shown path and write it back whole with path.set on release, so each edit
       is one history step. The shown path is the Paths panel's selection, else the active layer's
       shape path or vector mask. */

    void opt_label (const char* text);
    void opt_gap ();
    void section (const char* title);

    const int PATH_NONE = -3;
    const int PATH_WORK = -1;

    enum PenDrag
    {
        PEN_NONE,
        PEN_HANDLES,
        PEN_ANCHORS,
        PEN_IN,
        PEN_OUT,
        PEN_SUBPATHS,
        PEN_MARQUEE,
    };

    enum HitKind
    {
        HIT_NONE,
        HIT_ANCHOR,
        HIT_IN,
        HIT_OUT,
        HIT_SEGMENT,
        HIT_INSIDE,
    };

    struct PathHit
    {
        HitKind kind;
        int sub;
        int knot;
        double t;
    };

    const ImU32 PATH_COLOR = IM_COL32 (0, 132, 255, 255);

    uint64_t knot_key (int sub, int knot)
    {
        return ((uint64_t) (uint32_t) sub << 32) | (uint32_t) knot;
    }

    bool is_path_tool (Tool t)
    {
        return t == Tool::Pen || t == Tool::PathSelect || t == Tool::DirectSelect;
    }

    bool is_shape_tool (Tool t)
    {
        return t == Tool::RectShape || t == Tool::EllipseShape || t == Tool::PolygonShape || t == Tool::LineShape;
    }

    bool contains_u64 (const std::vector<uint64_t>& v, uint64_t x)
    {
        return std::find (v.begin (), v.end (), x) != v.end ();
    }

    bool contains_int (const std::vector<int>& v, int x)
    {
        return std::find (v.begin (), v.end (), x) != v.end ();
    }

    void bez (const double* p0, const double* p1, const double* p2, const double* p3, double t, double out[2])
    {
        double u = 1.0 - t, a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        out[0] = a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0];
        out[1] = a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1];
    }

    int segment_count (const doc::Subpath& s)
    {
        int n = (int) s.knots.size ();
        if (n < 2)
        {
            return 0;
        }
        return s.closed ? n : n - 1;
    }

    /* Inside a closed subpath (even-odd over its outline). */
    bool inside_subpath (const doc::Subpath& s, double x, double y)
    {
        if (!s.closed)
        {
            return false;
        }
        std::vector<double> xy;
        vec::flatten (s, 0.5, &xy);
        size_t n = xy.size () / 2;
        bool in  = false;
        for (size_t i = 0, j = n - 1; i < n; j = i++)
        {
            double xi = xy[i * 2], yi = xy[i * 2 + 1], xj = xy[j * 2], yj = xy[j * 2 + 1];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            {
                in = !in;
            }
        }
        return in;
    }

    /* What lies under (x, y) within r: anchors first, then the handles of `handles` knots, then
       segments (with the curve parameter), then closed subpaths' insides. */
    PathHit hit_path (const doc::Path& p, double x, double y, double r, const std::vector<uint64_t>& handles)
    {
        PathHit h = {HIT_NONE, -1, -1, 0.0};
        for (int s = (int) p.subpaths.size () - 1; s >= 0; s--)
        {
            const doc::Subpath& sp = p.subpaths[(size_t) s];
            for (int k = 0; k < (int) sp.knots.size (); k++)
            {
                const doc::Knot& n = sp.knots[(size_t) k];
                if (contains_u64 (handles, knot_key (s, k)))
                {
                    if (hypot (n.out_ctrl[0] - x, n.out_ctrl[1] - y) <= r
                        && (n.out_ctrl[0] != n.anchor[0] || n.out_ctrl[1] != n.anchor[1]))
                    {
                        return PathHit{HIT_OUT, s, k, 0.0};
                    }
                    if (hypot (n.in_ctrl[0] - x, n.in_ctrl[1] - y) <= r
                        && (n.in_ctrl[0] != n.anchor[0] || n.in_ctrl[1] != n.anchor[1]))
                    {
                        return PathHit{HIT_IN, s, k, 0.0};
                    }
                }
            }
        }
        for (int s = (int) p.subpaths.size () - 1; s >= 0; s--)
        {
            const doc::Subpath& sp = p.subpaths[(size_t) s];
            for (int k = 0; k < (int) sp.knots.size (); k++)
            {
                const doc::Knot& n = sp.knots[(size_t) k];
                if (hypot (n.anchor[0] - x, n.anchor[1] - y) <= r)
                {
                    return PathHit{HIT_ANCHOR, s, k, 0.0};
                }
            }
        }
        double best = r;
        for (int s = 0; s < (int) p.subpaths.size (); s++)
        {
            const doc::Subpath& sp = p.subpaths[(size_t) s];
            int segs = segment_count (sp), n = (int) sp.knots.size ();
            for (int i = 0; i < segs; i++)
            {
                const doc::Knot &a = sp.knots[(size_t) i], &b = sp.knots[(size_t) ((i + 1) % n)];
                double prev[2]  = {a.anchor[0], a.anchor[1]};
                const int STEPS = 48;
                for (int j = 1; j <= STEPS; j++)
                {
                    double t = (double) j / STEPS, q[2];
                    bez (a.anchor, a.out_ctrl, b.in_ctrl, b.anchor, t, q);
                    double dx = q[0] - prev[0], dy = q[1] - prev[1], l2 = dx * dx + dy * dy;
                    double u = l2 > 0 ? fmin (fmax (((x - prev[0]) * dx + (y - prev[1]) * dy) / l2, 0.0), 1.0) : 0.0;
                    double d = hypot (prev[0] + dx * u - x, prev[1] + dy * u - y);
                    if (d <= best)
                    {
                        best = d;
                        h    = PathHit{HIT_SEGMENT, s, i, ((double) (j - 1) + u) / STEPS};
                    }
                    prev[0] = q[0];
                    prev[1] = q[1];
                }
            }
        }
        if (h.kind != HIT_NONE)
        {
            return h;
        }
        for (int s = (int) p.subpaths.size () - 1; s >= 0; s--)
        {
            if (inside_subpath (p.subpaths[(size_t) s], x, y))
            {
                return PathHit{HIT_INSIDE, s, -1, 0.0};
            }
        }
        return h;
    }

    void lerp2 (const double* a, const double* b, double t, double out[2])
    {
        out[0] = a[0] + (b[0] - a[0]) * t;
        out[1] = a[1] + (b[1] - a[1]) * t;
    }

    /* Splits segment `seg` of subpath `s` at t with a new smooth point (de Casteljau); returns its
       index. */
    int split_segment (doc::Subpath* s, int seg, double t)
    {
        int n         = (int) s->knots.size ();
        doc::Knot& a  = s->knots[(size_t) seg];
        doc::Knot& b  = s->knots[(size_t) ((seg + 1) % n)];
        bool straight = a.out_ctrl[0] == a.anchor[0] && a.out_ctrl[1] == a.anchor[1] && b.in_ctrl[0] == b.anchor[0]
                     && b.in_ctrl[1] == b.anchor[1];
        double p01[2], p12[2], p23[2], p012[2], p123[2], mid[2];
        lerp2 (a.anchor, a.out_ctrl, t, p01);
        lerp2 (a.out_ctrl, b.in_ctrl, t, p12);
        lerp2 (b.in_ctrl, b.anchor, t, p23);
        lerp2 (p01, p12, t, p012);
        lerp2 (p12, p23, t, p123);
        lerp2 (p012, p123, t, mid);
        doc::Knot k = doc::Knot::corner (mid[0], mid[1]);
        if (!straight)
        {
            a.out_ctrl[0] = p01[0];
            a.out_ctrl[1] = p01[1];
            b.in_ctrl[0]  = p23[0];
            b.in_ctrl[1]  = p23[1];
            k.in_ctrl[0]  = p012[0];
            k.in_ctrl[1]  = p012[1];
            k.out_ctrl[0] = p123[0];
            k.out_ctrl[1] = p123[1];
            k.smooth      = true;
        }
        s->knots.insert (s->knots.begin () + seg + 1, k);
        return seg + 1;
    }

    void move_knot (doc::Knot* k, double dx, double dy)
    {
        k->anchor[0] += dx;
        k->anchor[1] += dy;
        k->in_ctrl[0] += dx;
        k->in_ctrl[1] += dy;
        k->out_ctrl[0] += dx;
        k->out_ctrl[1] += dy;
    }

    /* A dragged handle; a smooth point turns its other handle to stay opposite (its length kept). */
    void drag_handle (doc::Knot* k, bool out, double x, double y, bool cusp)
    {
        double *h = out ? k->out_ctrl : k->in_ctrl, *o = out ? k->in_ctrl : k->out_ctrl;
        h[0] = x;
        h[1] = y;
        if (cusp)
        {
            k->smooth = false;
            return;
        }
        if (!k->smooth)
        {
            return;
        }
        double hx = h[0] - k->anchor[0], hy = h[1] - k->anchor[1], hl = hypot (hx, hy);
        double ol = hypot (o[0] - k->anchor[0], o[1] - k->anchor[1]);
        if (hl <= 0.0 || ol <= 0.0)
        {
            return;
        }
        o[0] = k->anchor[0] - hx / hl * ol;
        o[1] = k->anchor[1] - hy / hl * ol;
    }

    /* ---- Target and sync ---- */

    bool App::path_target (engine::Value* target, doc::Path* path)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return false;
        }
        const doc::Document& doc = *d->doc;
        if (path_hidden_layer_ != doc::NO_LAYER && d->active_layer != path_hidden_layer_)
        {
            path_hidden_layer_ = doc::NO_LAYER;
        }
        if (path_sel_ >= 0 && (size_t) path_sel_ < doc.paths.size ())
        {
            *target = engine::Value{{"target", "saved"}, {"index", path_sel_}};
            *path   = doc.paths[(size_t) path_sel_].path;
            return true;
        }
        if (path_sel_ == PATH_WORK && doc.has_work_path)
        {
            *target = engine::Value{{"target", "work"}};
            *path   = doc.work_path;
            return true;
        }
        path_sel_           = PATH_NONE;
        const doc::Layer* l = doc.layer (d->active_layer);
        if (l == NULL || l->id == path_hidden_layer_)
        {
            return false;
        }
        if (l->kind == doc::LayerKind::Shape)
        {
            *target = engine::Value{{"target", "shape"}, {"layer", l->id}};
            *path   = l->shape.path;
            return true;
        }
        if (l->has_vector_mask)
        {
            *target = engine::Value{{"target", "vectorMask"}, {"layer", l->id}};
            *path   = l->vector_mask.path;
            return true;
        }
        return false;
    }

    /* The local path follows the document unless a drag is editing it. */
    void App::sync_pen ()
    {
        if (pen_drag_ != PEN_NONE)
        {
            return;
        }
        const engine::DocState* d = session_.active ();
        doc::DocId id             = d != NULL ? d->doc->id : 0;
        uint64_t rev              = d != NULL ? d->revision : 0;
        engine::Value t;
        doc::Path p = doc::Path::make ();
        bool has    = path_target (&t, &p);
        if (id == pen_doc_ && rev == pen_revision_ && has == pen_has_target_ && t == pen_target_)
        {
            return;
        }
        bool same       = has && pen_has_target_ && t == pen_target_ && id == pen_doc_;
        pen_doc_        = id;
        pen_revision_   = rev;
        pen_has_target_ = has;
        pen_target_     = t;
        pen_path_       = p;
        if (!same)
        {
            pen_sub_ = -1;
            path_knots_sel_.clear ();
            path_subs_sel_.clear ();
        }
        std::vector<uint64_t> ks;
        for (size_t i = 0; i < path_knots_sel_.size (); i++)
        {
            size_t s = (size_t) (path_knots_sel_[i] >> 32), k = (size_t) (path_knots_sel_[i] & 0xffffffffu);
            if (s < p.subpaths.size () && k < p.subpaths[s].knots.size ())
            {
                ks.push_back (path_knots_sel_[i]);
            }
        }
        path_knots_sel_ = ks;
        std::vector<int> ss;
        for (size_t i = 0; i < path_subs_sel_.size (); i++)
        {
            if ((size_t) path_subs_sel_[i] < p.subpaths.size ())
            {
                ss.push_back (path_subs_sel_[i]);
            }
        }
        path_subs_sel_ = ss;
        if (pen_sub_ >= (int) p.subpaths.size () || (pen_sub_ >= 0 && p.subpaths[(size_t) pen_sub_].closed))
        {
            pen_sub_ = -1;
        }
    }

    engine::Value App::shape_style_params ()
    {
        char fill[8], stroke[8];
        snprintf (fill, sizeof fill, "#%02x%02x%02x", (int) lroundf (shape_fill_[0] * 255),
            (int) lroundf (shape_fill_[1] * 255), (int) lroundf (shape_fill_[2] * 255));
        snprintf (stroke, sizeof stroke, "#%02x%02x%02x", (int) lroundf (shape_stroke_[0] * 255),
            (int) lroundf (shape_stroke_[1] * 255), (int) lroundf (shape_stroke_[2] * 255));
        engine::Value v = engine::Value::object ();
        if (shape_fill_on_)
        {
            v["fill"] = fill;
        }
        else
        {
            v["fill"] = nullptr;
        }
        v["stroke"] = engine::Value{{"enabled", shape_stroke_on_}, {"width", shape_stroke_w_}, {"color", stroke}};
        return v;
    }

    /* Writes the local path back: to its target, else a new shape layer (Shape mode) or the work
       path. The drawing state survives the round trip. */
    void App::commit_pen (const char* label)
    {
        if (pen_has_target_)
        {
            engine::Value p = pen_target_;
            p["path"]       = engine::path_json (pen_path_);
            p["label"]      = label;
            run_command ("path.set", p);
        }
        else if (pen_mode_ == 0 && tool_ == Tool::Pen)
        {
            engine::Value p = shape_style_params ();
            p["kind"]       = "path";
            p["path"]       = engine::path_json (pen_path_);
            run_command ("shape.create", p);
        }
        else
        {
            run_command ("path.set",
                engine::Value{{"target", "work"}, {"path", engine::path_json (pen_path_)}, {"label", label}});
            path_sel_ = PATH_WORK;
        }
        int sub                  = pen_sub_;
        std::vector<uint64_t> ks = path_knots_sel_;
        std::vector<int> ss      = path_subs_sel_;
        pen_revision_            = UINT64_MAX;
        pen_has_target_          = false;
        sync_pen ();
        pen_sub_        = sub;
        path_knots_sel_ = ks;
        path_subs_sel_  = ss;
        pen_revision_   = UINT64_MAX;
        sync_pen ();
    }

    void App::end_pen_drawing ()
    {
        pen_sub_ = -1;
    }

    /* ---- Canvas input ---- */

    bool App::path_tool_input (float ox, float oy)
    {
        if (!is_path_tool (tool_) && !is_shape_tool (tool_))
        {
            return false;
        }
        if (session_.active () == NULL)
        {
            return true;
        }
        sync_pen ();
        ImGuiIO& io = ImGui::GetIO ();
        double mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        if (is_shape_tool (tool_))
        {
            shape_tool_input (mx, my, activated, active, released, io.KeyShift, io.KeyAlt);
            return true;
        }
        double r = 5.0 / zoom_;
        /* Ctrl gives the Pen the Direct Selection tool for the moment. */
        Tool t = tool_ == Tool::Pen && io.KeyCtrl && pen_drag_ == PEN_NONE ? Tool::DirectSelect : tool_;
        if (pen_drag_ != PEN_NONE)
        {
            t = pen_drag_tool_;
        }
        if (activated)
        {
            pen_drag_tool_ = t;
            pen_press_[0] = pen_last_[0] = (float) mx;
            pen_press_[1] = pen_last_[1] = (float) my;
            pen_moved_                   = false;
            if (t == Tool::Pen)
            {
                pen_press (mx, my, r, io.KeyAlt);
            }
            else
            {
                select_press (t, mx, my, r, io.KeyShift);
            }
            return true;
        }
        if (pen_drag_ == PEN_NONE)
        {
            return true;
        }
        if (active)
        {
            double dx = mx - pen_last_[0], dy = my - pen_last_[1];
            if (fabs (mx - pen_press_[0]) * zoom_ > 2.0 || fabs (my - pen_press_[1]) * zoom_ > 2.0)
            {
                pen_moved_ = true;
            }
            pen_drag_to (mx, my, dx, dy, io.KeyAlt);
            pen_last_[0] = (float) mx;
            pen_last_[1] = (float) my;
        }
        if (released || !ImGui::IsMouseDown (ImGuiMouseButton_Left))
        {
            int drag  = pen_drag_;
            pen_drag_ = PEN_NONE;
            if (drag == PEN_MARQUEE)
            {
                marquee_select (t, io.KeyShift);
            }
            else if (drag == PEN_SUBPATHS || drag == PEN_ANCHORS || drag == PEN_IN || drag == PEN_OUT)
            {
                if (pen_moved_)
                {
                    commit_pen (drag == PEN_SUBPATHS  ? "Move Path"
                                : drag == PEN_ANCHORS ? "Drag Anchor"
                                                      : "Drag Handle");
                }
            }
            else if (drag == PEN_HANDLES)
            {
                commit_pen (pen_label_.c_str ());
            }
        }
        return true;
    }

    void App::pen_press (double mx, double my, double r, bool alt)
    {
        std::vector<uint64_t> handles;
        if (pen_sub_ >= 0)
        {
            const doc::Subpath& s = pen_path_.subpaths[(size_t) pen_sub_];
            if (!s.knots.empty ())
            {
                handles.push_back (knot_key (pen_sub_, (int) s.knots.size () - 1));
            }
        }
        handles.insert (handles.end (), path_knots_sel_.begin (), path_knots_sel_.end ());
        PathHit h = hit_path (pen_path_, mx, my, r, handles);
        if (pen_sub_ >= 0)
        {
            doc::Subpath& s = pen_path_.subpaths[(size_t) pen_sub_];
            if (h.kind == HIT_ANCHOR && h.sub == pen_sub_ && h.knot == 0 && s.knots.size () >= 2)
            {
                s.closed       = true;
                pen_drag_      = PEN_HANDLES;
                pen_drag_sub_  = pen_sub_;
                pen_drag_knot_ = 0;
                pen_label_     = "Close Path";
                pen_sub_       = -1;
                return;
            }
            s.knots.push_back (doc::Knot::corner (mx, my));
            pen_drag_      = PEN_HANDLES;
            pen_drag_sub_  = pen_sub_;
            pen_drag_knot_ = (int) s.knots.size () - 1;
            pen_label_     = "Add Anchor Point";
            path_knots_sel_.assign (1, knot_key (pen_drag_sub_, pen_drag_knot_));
            return;
        }
        if (h.kind == HIT_ANCHOR)
        {
            doc::Subpath& s = pen_path_.subpaths[(size_t) h.sub];
            int last        = (int) s.knots.size () - 1;
            if (alt)
            {
                /* Convert Point: a click makes a corner, a drag pulls out new smooth handles. */
                doc::Knot& k   = s.knots[(size_t) h.knot];
                k              = doc::Knot::corner (k.anchor[0], k.anchor[1]);
                pen_drag_      = PEN_HANDLES;
                pen_drag_sub_  = h.sub;
                pen_drag_knot_ = h.knot;
                pen_label_     = "Convert Point";
                return;
            }
            if (!s.closed && (h.knot == last || h.knot == 0))
            {
                /* An open end: drawing carries on from it. */
                if (h.knot == 0 && last > 0)
                {
                    std::reverse (s.knots.begin (), s.knots.end ());
                    for (size_t i = 0; i < s.knots.size (); i++)
                    {
                        double t[2] = {s.knots[i].in_ctrl[0], s.knots[i].in_ctrl[1]};
                        memcpy (s.knots[i].in_ctrl, s.knots[i].out_ctrl, sizeof t);
                        memcpy (s.knots[i].out_ctrl, t, sizeof t);
                    }
                }
                pen_sub_ = h.sub;
                path_knots_sel_.assign (1, knot_key (h.sub, (int) s.knots.size () - 1));
                return;
            }
            /* Auto delete. */
            s.knots.erase (s.knots.begin () + h.knot);
            if (s.knots.size () < 2 && !(s.knots.size () == 1 && !s.closed))
            {
                pen_path_.subpaths.erase (pen_path_.subpaths.begin () + h.sub);
            }
            else if (s.knots.size () < 3)
            {
                s.closed = false;
            }
            path_knots_sel_.clear ();
            commit_pen ("Delete Anchor Point");
            return;
        }
        if (h.kind == HIT_SEGMENT)
        {
            int k = split_segment (&pen_path_.subpaths[(size_t) h.sub], h.knot, h.t);
            path_knots_sel_.assign (1, knot_key (h.sub, k));
            commit_pen ("Add Anchor Point");
            return;
        }
        /* A new subpath. */
        doc::Subpath s;
        s.closed = false;
        s.op     = pen_path_.subpaths.empty () ? doc::PathOp::Combine : (doc::PathOp) path_op_;
        s.knots.push_back (doc::Knot::corner (mx, my));
        pen_path_.subpaths.push_back (s);
        pen_sub_       = (int) pen_path_.subpaths.size () - 1;
        pen_drag_      = PEN_HANDLES;
        pen_drag_sub_  = pen_sub_;
        pen_drag_knot_ = 0;
        pen_label_     = pen_has_target_ ? "Add Anchor Point" : (pen_mode_ == 0 ? "Pen Tool" : "New Work Path");
        path_knots_sel_.assign (1, knot_key (pen_sub_, 0));
    }

    void App::select_press (Tool t, double mx, double my, double r, bool shift)
    {
        std::vector<uint64_t> handles = path_knots_sel_;
        PathHit h = hit_path (pen_path_, mx, my, r, t == Tool::DirectSelect ? handles : std::vector<uint64_t> ());
        if (h.kind == HIT_NONE && t == Tool::PathSelect)
        {
            /* Another shape layer's path under the pointer selects that layer. */
            doc::LayerId other        = shape_layer_at (mx, my, r);
            const engine::DocState* d = session_.active ();
            if (other != doc::NO_LAYER && other != d->active_layer)
            {
                path_sel_ = PATH_NONE;
                run_command ("layer.select", engine::Value{{"layer", other}});
                sync_pen ();
                h = hit_path (pen_path_, mx, my, r, std::vector<uint64_t> ());
            }
        }
        if (h.kind == HIT_NONE)
        {
            if (!shift)
            {
                path_knots_sel_.clear ();
                path_subs_sel_.clear ();
            }
            pen_drag_ = PEN_MARQUEE;
            return;
        }
        if (t == Tool::PathSelect)
        {
            bool on = contains_int (path_subs_sel_, h.sub);
            if (shift && on)
            {
                path_subs_sel_.erase (std::find (path_subs_sel_.begin (), path_subs_sel_.end (), h.sub));
            }
            else if (!on)
            {
                if (!shift)
                {
                    path_subs_sel_.clear ();
                }
                path_subs_sel_.push_back (h.sub);
            }
            if (!path_subs_sel_.empty ())
            {
                const doc::Subpath& s = pen_path_.subpaths[(size_t) h.sub];
                path_op_              = (int) (s.op == doc::PathOp::Join ? doc::PathOp::Combine : s.op);
            }
            pen_drag_ = PEN_SUBPATHS;
            return;
        }
        if (h.kind == HIT_IN || h.kind == HIT_OUT)
        {
            pen_drag_      = h.kind == HIT_IN ? PEN_IN : PEN_OUT;
            pen_drag_sub_  = h.sub;
            pen_drag_knot_ = h.knot;
            return;
        }
        if (h.kind == HIT_ANCHOR)
        {
            uint64_t key = knot_key (h.sub, h.knot);
            bool on      = contains_u64 (path_knots_sel_, key);
            if (shift && on)
            {
                path_knots_sel_.erase (std::find (path_knots_sel_.begin (), path_knots_sel_.end (), key));
            }
            else if (!on)
            {
                if (!shift)
                {
                    path_knots_sel_.clear ();
                }
                path_knots_sel_.push_back (key);
            }
            pen_drag_ = PEN_ANCHORS;
            return;
        }
        if (h.kind == HIT_SEGMENT)
        {
            int n = (int) pen_path_.subpaths[(size_t) h.sub].knots.size ();
            if (!shift)
            {
                path_knots_sel_.clear ();
            }
            uint64_t a = knot_key (h.sub, h.knot), b = knot_key (h.sub, (h.knot + 1) % n);
            if (!contains_u64 (path_knots_sel_, a))
            {
                path_knots_sel_.push_back (a);
            }
            if (!contains_u64 (path_knots_sel_, b))
            {
                path_knots_sel_.push_back (b);
            }
            pen_drag_ = PEN_ANCHORS;
            return;
        }
        /* Inside a closed subpath: all its points. */
        if (!shift)
        {
            path_knots_sel_.clear ();
        }
        for (int k = 0; k < (int) pen_path_.subpaths[(size_t) h.sub].knots.size (); k++)
        {
            path_knots_sel_.push_back (knot_key (h.sub, k));
        }
        pen_drag_ = PEN_ANCHORS;
    }

    void App::pen_drag_to (double mx, double my, double dx, double dy, bool alt)
    {
        if (pen_drag_ == PEN_HANDLES)
        {
            doc::Knot& k = pen_path_.subpaths[(size_t) pen_drag_sub_].knots[(size_t) pen_drag_knot_];
            if (!pen_moved_)
            {
                return;
            }
            k.out_ctrl[0] = mx;
            k.out_ctrl[1] = my;
            k.in_ctrl[0]  = 2 * k.anchor[0] - mx;
            k.in_ctrl[1]  = 2 * k.anchor[1] - my;
            k.smooth      = true;
            return;
        }
        if (pen_drag_ == PEN_IN || pen_drag_ == PEN_OUT)
        {
            drag_handle (&pen_path_.subpaths[(size_t) pen_drag_sub_].knots[(size_t) pen_drag_knot_],
                pen_drag_ == PEN_OUT, mx, my, alt);
            return;
        }
        if (pen_drag_ == PEN_ANCHORS)
        {
            for (size_t i = 0; i < path_knots_sel_.size (); i++)
            {
                size_t s = (size_t) (path_knots_sel_[i] >> 32), k = (size_t) (path_knots_sel_[i] & 0xffffffffu);
                move_knot (&pen_path_.subpaths[s].knots[k], dx, dy);
            }
            return;
        }
        if (pen_drag_ == PEN_SUBPATHS)
        {
            for (size_t i = 0; i < path_subs_sel_.size (); i++)
            {
                doc::Subpath& s = pen_path_.subpaths[(size_t) path_subs_sel_[i]];
                for (size_t k = 0; k < s.knots.size (); k++)
                {
                    move_knot (&s.knots[k], dx, dy);
                }
            }
        }
    }

    void App::marquee_select (Tool t, bool shift)
    {
        double x0 = fmin (pen_press_[0], pen_last_[0]), x1 = fmax (pen_press_[0], pen_last_[0]);
        double y0 = fmin (pen_press_[1], pen_last_[1]), y1 = fmax (pen_press_[1], pen_last_[1]);
        if (!pen_moved_)
        {
            return;
        }
        for (int s = 0; s < (int) pen_path_.subpaths.size (); s++)
        {
            const doc::Subpath& sp = pen_path_.subpaths[(size_t) s];
            for (int k = 0; k < (int) sp.knots.size (); k++)
            {
                const double* a = sp.knots[(size_t) k].anchor;
                if (a[0] < x0 || a[0] > x1 || a[1] < y0 || a[1] > y1)
                {
                    continue;
                }
                if (t == Tool::PathSelect)
                {
                    if (!contains_int (path_subs_sel_, s))
                    {
                        path_subs_sel_.push_back (s);
                    }
                }
                else if (!contains_u64 (path_knots_sel_, knot_key (s, k)))
                {
                    path_knots_sel_.push_back (knot_key (s, k));
                }
            }
        }
        (void) shift;
    }

    /* The topmost visible shape layer whose path is under (x, y). */
    doc::LayerId App::shape_layer_at (double x, double y, double r)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return doc::NO_LAYER;
        }
        std::vector<doc::WalkEntry> walk = d->doc->walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            const doc::Layer* l = walk[i].layer;
            if (l->kind != doc::LayerKind::Shape || !l->visible)
            {
                continue;
            }
            PathHit h = hit_path (l->shape.path, x, y, r, std::vector<uint64_t> ());
            if (h.kind != HIT_NONE)
            {
                return l->id;
            }
        }
        return doc::NO_LAYER;
    }

    /* Rectangle / Ellipse / Polygon / Line: drag out the shape (Shift keeps it square, or the line
       at 45°; Alt draws from the centre). */
    void App::shape_tool_input (double mx, double my, bool activated, bool active, bool released, bool shift, bool alt)
    {
        if (activated)
        {
            shape_dragging_ = true;
            shape_from_[0] = shape_to_[0] = (float) mx;
            shape_from_[1] = shape_to_[1] = (float) my;
            return;
        }
        if (!shape_dragging_)
        {
            return;
        }
        if (active)
        {
            shape_to_[0] = (float) mx;
            shape_to_[1] = (float) my;
            shape_shift_ = shift;
            shape_alt_   = alt;
        }
        if (!released && ImGui::IsMouseDown (ImGuiMouseButton_Left))
        {
            return;
        }
        shape_dragging_ = false;
        double r[4];
        shape_rect (r);
        if (fabs (r[2] - r[0]) < 1.0 && fabs (r[3] - r[1]) < 1.0)
        {
            return;
        }
        doc::LiveShape live;
        shape_live (r, &live);
        const char* KINDS[4] = {"rect", "ellipse", "polygon", "line"};
        int ki               = (int) tool_ - (int) Tool::RectShape;
        if (shape_mode_ == 0)
        {
            engine::Value p    = shape_style_params ();
            p["kind"]          = KINDS[ki];
            p["rect"]          = {r[0], r[1], r[2], r[3]};
            p["radius"]        = shape_radius_;
            p["sides"]         = shape_sides_;
            p["star"]          = shape_star_ > 0.0f ? 1.0f - shape_star_ / 100.0f : 0.0f;
            p["weight"]        = shape_weight_;
            path_hidden_layer_ = doc::NO_LAYER;
            path_sel_          = PATH_NONE;
            run_command ("shape.create", p);
            return;
        }
        doc::Path add = doc::live_path (live);
        if (shape_mode_ == 2)
        {
            run_command ("path.fill", engine::Value{{"path", engine::path_json (add)}});
            return;
        }
        /* Path mode: a new subpath on the shown path (a shape layer's included), else the work path. */
        if (!add.subpaths.empty ())
        {
            doc::Subpath s = add.subpaths[0];
            s.op           = pen_path_.subpaths.empty () ? doc::PathOp::Combine : (doc::PathOp) path_op_;
            pen_path_.subpaths.push_back (s);
            path_subs_sel_.assign (1, (int) pen_path_.subpaths.size () - 1);
            Tool keep = tool_;
            commit_pen (keep == Tool::RectShape      ? "Rectangle Tool"
                        : keep == Tool::EllipseShape ? "Ellipse Tool"
                        : keep == Tool::PolygonShape ? "Polygon Tool"
                                                     : "Line Tool");
        }
    }

    void App::shape_rect (double r[4])
    {
        double x0 = shape_from_[0], y0 = shape_from_[1], x1 = shape_to_[0], y1 = shape_to_[1];
        if (tool_ == Tool::LineShape)
        {
            if (shape_shift_)
            {
                double dx = x1 - x0, dy = y1 - y0, len = hypot (dx, dy), a = atan2 (dy, dx);
                a  = round (a / (M_PI / 4)) * (M_PI / 4);
                x1 = x0 + cos (a) * len;
                y1 = y0 + sin (a) * len;
            }
            r[0] = x0;
            r[1] = y0;
            r[2] = x1;
            r[3] = y1;
            return;
        }
        double w = x1 - x0, h = y1 - y0;
        if (shape_shift_)
        {
            double m = fmax (fabs (w), fabs (h));
            w        = w < 0 ? -m : m;
            h        = h < 0 ? -m : m;
        }
        if (shape_alt_)
        {
            r[0] = x0 - fabs (w);
            r[1] = y0 - fabs (h);
            r[2] = x0 + fabs (w);
            r[3] = y0 + fabs (h);
        }
        else
        {
            r[0] = fmin (x0, x0 + w);
            r[1] = fmin (y0, y0 + h);
            r[2] = fmax (x0, x0 + w);
            r[3] = fmax (y0, y0 + h);
        }
    }

    void App::shape_live (const double r[4], doc::LiveShape* live)
    {
        memset (live, 0, sizeof *live);
        live->kind = (doc::LiveKind) ((int) doc::LiveKind::Rect + ((int) tool_ - (int) Tool::RectShape));
        memcpy (live->rect, r, sizeof live->rect);
        live->radius = shape_radius_;
        live->sides  = (uint32_t) shape_sides_;
        live->star   = shape_star_ > 0.0f ? 1.0f - shape_star_ / 100.0f : 0.0;
        live->weight = shape_weight_;
    }

    /* Enter ends the open subpath (Ctrl+Enter loads the path as a selection), Esc ends it or hides
       the path, Delete removes the selected points or subpaths. */
    bool App::path_keys ()
    {
        if (!is_path_tool (tool_) && !is_shape_tool (tool_))
        {
            return false;
        }
        ImGuiIO& io = ImGui::GetIO ();
        if (io.WantTextInput || session_.active () == NULL)
        {
            return false;
        }
        sync_pen ();
        bool enter = ImGui::IsKeyPressed (ImGuiKey_Enter, false) || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter, false);
        if (enter && io.KeyCtrl && pen_has_target_)
        {
            engine::Value p = pen_target_;
            run_command ("path.makeSelection", p);
            return true;
        }
        if (enter && pen_sub_ >= 0)
        {
            end_pen_drawing ();
            return true;
        }
        if (ImGui::IsKeyPressed (ImGuiKey_Escape, false))
        {
            if (pen_sub_ >= 0)
            {
                end_pen_drawing ();
                return true;
            }
            if (!pen_has_target_)
            {
                return false;
            }
            const engine::DocState* d = session_.active ();
            if (path_sel_ == PATH_NONE)
            {
                path_hidden_layer_ = d->active_layer;
            }
            path_sel_ = PATH_NONE;
            path_knots_sel_.clear ();
            path_subs_sel_.clear ();
            return true;
        }
        bool del = ImGui::IsKeyPressed (ImGuiKey_Delete, false) || ImGui::IsKeyPressed (ImGuiKey_Backspace, false);
        if (!del || !pen_has_target_)
        {
            return false;
        }
        if (tool_ == Tool::PathSelect && !path_subs_sel_.empty ())
        {
            std::sort (path_subs_sel_.begin (), path_subs_sel_.end ());
            for (size_t i = path_subs_sel_.size (); i-- > 0;)
            {
                pen_path_.subpaths.erase (pen_path_.subpaths.begin () + path_subs_sel_[i]);
            }
            path_subs_sel_.clear ();
            commit_pen ("Delete Path");
            return true;
        }
        if ((tool_ == Tool::DirectSelect || tool_ == Tool::Pen) && !path_knots_sel_.empty ())
        {
            std::sort (path_knots_sel_.begin (), path_knots_sel_.end ());
            for (size_t i = path_knots_sel_.size (); i-- > 0;)
            {
                size_t s = (size_t) (path_knots_sel_[i] >> 32), k = (size_t) (path_knots_sel_[i] & 0xffffffffu);
                pen_path_.subpaths[s].knots.erase (pen_path_.subpaths[s].knots.begin () + (ptrdiff_t) k);
            }
            for (size_t s = pen_path_.subpaths.size (); s-- > 0;)
            {
                doc::Subpath& sp = pen_path_.subpaths[s];
                if (sp.knots.empty ())
                {
                    pen_path_.subpaths.erase (pen_path_.subpaths.begin () + (ptrdiff_t) s);
                }
                else if (sp.knots.size () < 3)
                {
                    sp.closed = false;
                }
            }
            path_knots_sel_.clear ();
            pen_sub_ = -1;
            commit_pen ("Delete Anchor Point");
            return true;
        }
        return false;
    }

    /* ---- Overlay ---- */

    void path_polyline (ImDrawList* dl, const doc::Subpath& s, float ox, float oy, float zoom, ImU32 col, float width)
    {
        std::vector<double> xy;
        vec::flatten (s, 0.3 / fmax (zoom, 0.01), &xy);
        std::vector<ImVec2> pts;
        for (size_t i = 0; i + 1 < xy.size (); i += 2)
        {
            pts.push_back (ImVec2 (ox + (float) xy[i] * zoom, oy + (float) xy[i + 1] * zoom));
        }
        if (pts.size () >= 2)
        {
            dl->AddPolyline (pts.data (), (int) pts.size (), col, 0, width);
        }
    }

    void App::path_tool_overlay (float ox, float oy)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        float k        = ui_scale_;
        if (is_shape_tool (tool_) && shape_dragging_)
        {
            double r[4];
            shape_rect (r);
            doc::LiveShape live;
            shape_live (r, &live);
            doc::Path p = doc::live_path (live);
            for (size_t i = 0; i < p.subpaths.size (); i++)
            {
                path_polyline (dl, p.subpaths[i], ox, oy, zoom_, PATH_COLOR, 1.0f * k);
            }
        }
        if (!is_path_tool (tool_) && !is_shape_tool (tool_))
        {
            return;
        }
        sync_pen ();
        if (!pen_has_target_ && pen_path_.subpaths.empty ())
        {
            return;
        }
        const doc::Path& p = pen_path_;
        for (size_t i = 0; i < p.subpaths.size (); i++)
        {
            path_polyline (dl, p.subpaths[i], ox, oy, zoom_, PATH_COLOR, 1.0f * k);
        }
        if (is_shape_tool (tool_))
        {
            return;
        }
        /* The rubber band from the last point to the pointer. */
        ImGuiIO& io = ImGui::GetIO ();
        if (tool_ == Tool::Pen && pen_sub_ >= 0 && pen_drag_ == PEN_NONE && ImGui::IsItemHovered ())
        {
            const doc::Subpath& s = p.subpaths[(size_t) pen_sub_];
            if (!s.knots.empty ())
            {
                const doc::Knot& last = s.knots.back ();
                double m[2]           = {(io.MousePos.x - ox) / zoom_, (io.MousePos.y - oy) / zoom_};
                doc::Subpath band;
                band.closed = false;
                band.op     = doc::PathOp::Combine;
                band.knots.push_back (last);
                band.knots.push_back (doc::Knot::corner (m[0], m[1]));
                path_polyline (dl, band, ox, oy, zoom_, IM_COL32 (0, 132, 255, 140), 1.0f * k);
            }
        }
        float hs = 3.0f * k;
        for (int s = 0; s < (int) p.subpaths.size (); s++)
        {
            const doc::Subpath& sp = p.subpaths[(size_t) s];
            bool whole             = contains_int (path_subs_sel_, s) && tool_ == Tool::PathSelect;
            for (int i = 0; i < (int) sp.knots.size (); i++)
            {
                const doc::Knot& n = sp.knots[(size_t) i];
                ImVec2 a (ox + (float) n.anchor[0] * zoom_, oy + (float) n.anchor[1] * zoom_);
                bool sel  = contains_u64 (path_knots_sel_, knot_key (s, i)) && tool_ != Tool::PathSelect;
                bool last = tool_ == Tool::Pen && s == pen_sub_ && i + 1 == (int) sp.knots.size ();
                if (sel || last)
                {
                    const double* hd[2] = {n.in_ctrl, n.out_ctrl};
                    for (int j = 0; j < 2; j++)
                    {
                        if (hd[j][0] == n.anchor[0] && hd[j][1] == n.anchor[1])
                        {
                            continue;
                        }
                        ImVec2 h (ox + (float) hd[j][0] * zoom_, oy + (float) hd[j][1] * zoom_);
                        dl->AddLine (a, h, PATH_COLOR, 1.0f * k);
                        dl->AddCircleFilled (h, 2.5f * k, PATH_COLOR);
                    }
                }
                if (tool_ == Tool::PathSelect && !whole)
                {
                    continue;
                }
                ImVec2 a0 (a.x - hs, a.y - hs), a1 (a.x + hs, a.y + hs);
                if (sel || whole)
                {
                    dl->AddRectFilled (a0, a1, PATH_COLOR);
                }
                else
                {
                    dl->AddRectFilled (a0, a1, IM_COL32 (255, 255, 255, 255));
                    dl->AddRect (a0, a1, PATH_COLOR, 0, 0, 1.0f * k);
                }
            }
        }
        if (pen_drag_ == PEN_MARQUEE && pen_moved_)
        {
            ImVec2 a (ox + pen_press_[0] * zoom_, oy + pen_press_[1] * zoom_),
                b (ox + pen_last_[0] * zoom_, oy + pen_last_[1] * zoom_);
            dl->AddRect (ImVec2 (fminf (a.x, b.x), fminf (a.y, b.y)), ImVec2 (fmaxf (a.x, b.x), fmaxf (a.y, b.y)),
                IM_COL32 (0, 0, 0, 200), 0, 0, 1.0f);
        }
    }

    /* ---- Options bar ---- */

    /* Fill and stroke settings; with a shape layer active they edit it, as in Photoshop. */
    void App::shape_style_options ()
    {
        float k                   = ui_scale_;
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d != NULL ? d->doc->layer (d->active_layer) : NULL;
        bool live                 = l != NULL && l->kind == doc::LayerKind::Shape;
        if (live && !shape_editing_)
        {
            shape_fill_on_ = l->shape.has_fill;
            memcpy (shape_fill_, l->shape.fill, sizeof shape_fill_);
            shape_stroke_on_ = l->shape.stroke.enabled;
            memcpy (shape_stroke_, l->shape.stroke.color, sizeof shape_stroke_);
            shape_stroke_w_ = l->shape.stroke.width;
        }
        bool changed = false, editing = false;
        changed |= ImGui::Checkbox ("Fill:", &shape_fill_on_);
        ImGui::SameLine ();
        changed |=
            ImGui::ColorEdit3 ("##shapefill", shape_fill_, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        editing |= ImGui::IsItemActive ();
        opt_gap ();
        changed |= ImGui::Checkbox ("Stroke:", &shape_stroke_on_);
        ImGui::SameLine ();
        changed |= ImGui::ColorEdit3 (
            "##shapestroke", shape_stroke_, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        editing |= ImGui::IsItemActive ();
        ImGui::SameLine ();
        ImGui::SetNextItemWidth (80 * k);
        ImGui::DragFloat ("##strokew", &shape_stroke_w_, 0.2f, 0.0f, 1000.0f, "%.1f px", ImGuiSliderFlags_AlwaysClamp);
        editing |= ImGui::IsItemActive ();
        changed |= ImGui::IsItemDeactivatedAfterEdit ();
        /* Colour pickers report every frame: the change is applied once they close. */
        if (changed)
        {
            shape_dirty_ = true;
        }
        shape_editing_ = editing || ImGui::IsPopupOpen ("", ImGuiPopupFlags_AnyPopupId);
        if (shape_dirty_ && !shape_editing_)
        {
            shape_dirty_ = false;
            if (live)
            {
                engine::Value p = shape_style_params ();
                p["layer"]      = l->id;
                run_command ("shape.setStyle", p);
            }
        }
    }

    void App::path_options ()
    {
        if (!is_path_tool (tool_) && !is_shape_tool (tool_))
        {
            return;
        }
        float k = ui_scale_;
        sync_pen ();
        if (tool_ == Tool::Pen || is_shape_tool (tool_))
        {
            ImGui::SetNextItemWidth (90 * k);
            if (tool_ == Tool::Pen)
            {
                ImGui::Combo ("##penmode", &pen_mode_, "Shape\0Path\0");
            }
            else
            {
                ImGui::Combo ("##shapemode", &shape_mode_, "Shape\0Path\0Pixels\0");
            }
            opt_gap ();
        }
        bool shape_mode = (tool_ == Tool::Pen && pen_mode_ == 0) || (is_shape_tool (tool_) && shape_mode_ == 0);
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d != NULL ? d->doc->layer (d->active_layer) : NULL;
        if (shape_mode || (tool_ == Tool::PathSelect && l != NULL && l->kind == doc::LayerKind::Shape))
        {
            shape_style_options ();
            opt_gap ();
        }
        if (tool_ == Tool::RectShape)
        {
            opt_label ("Radius:");
            ImGui::SetNextItemWidth (70 * k);
            ImGui::DragFloat ("##radius", &shape_radius_, 0.5f, 0.0f, 5000.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
        }
        if (tool_ == Tool::PolygonShape)
        {
            opt_label ("Sides:");
            ImGui::SetNextItemWidth (60 * k);
            ImGui::DragInt ("##sides", &shape_sides_, 0.1f, 3, 100, "%d", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            opt_label ("Star Ratio:");
            ImGui::SetNextItemWidth (70 * k);
            ImGui::DragFloat ("##star", &shape_star_, 0.5f, 0.0f, 99.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
        }
        if (tool_ == Tool::LineShape)
        {
            opt_label ("Weight:");
            ImGui::SetNextItemWidth (70 * k);
            ImGui::DragFloat ("##weight", &shape_weight_, 0.2f, 0.1f, 1000.0f, "%.1f px", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
        }
        /* Path operations: for new subpaths, and the selected ones with Path Selection. */
        opt_label ("Path:");
        ImGui::SetNextItemWidth (150 * k);
        if (ImGui::Combo ("##pathop", &path_op_,
                "Combine Shapes\0Subtract Front Shape\0Intersect Shape Areas\0Exclude Overlapping\0")
            && tool_ == Tool::PathSelect && !path_subs_sel_.empty () && pen_has_target_)
        {
            for (size_t i = 0; i < path_subs_sel_.size (); i++)
            {
                pen_path_.subpaths[(size_t) path_subs_sel_[i]].op = (doc::PathOp) path_op_;
            }
            commit_pen ("Path Operation");
        }
        if (pen_has_target_ && tool_ != Tool::LineShape)
        {
            opt_gap ();
            opt_label ("Make:");
            if (ImGui::Button ("Selection"))
            {
                run_command ("path.makeSelection", pen_target_);
            }
            ImGui::SameLine ();
            bool shape_target = pen_target_.value ("target", "") == std::string ("shape");
            if (shape_target || l == NULL || l->has_vector_mask || d->doc->is_background (l->id))
            {
                ImGui::BeginDisabled ();
            }
            if (ImGui::Button ("Mask"))
            {
                run_command ("layer.vectorMask.currentPath", pen_target_);
            }
            if (shape_target || l == NULL || l->has_vector_mask || d->doc->is_background (l->id))
            {
                ImGui::EndDisabled ();
            }
            ImGui::SameLine ();
            if (shape_target)
            {
                ImGui::BeginDisabled ();
            }
            if (ImGui::Button ("Shape"))
            {
                engine::Value p = shape_style_params ();
                p["kind"]       = "path";
                p["path"]       = engine::path_json (pen_path_);
                path_sel_       = PATH_NONE;
                run_command ("shape.create", p);
            }
            if (shape_target)
            {
                ImGui::EndDisabled ();
            }
        }
    }

    /* ---- Properties ---- */

    /* A shape layer: its fill and stroke, and a live shape's geometry (W, H, X, Y, and the radius,
       sides or weight), applied when a field is released. */
    void App::shape_properties (const doc::Layer& l)
    {
        float k                  = ui_scale_;
        const doc::LiveShape& lv = l.shape.live;
        const char* titles[5]    = {"Shape", "Rectangle", "Ellipse", "Polygon", "Line"};
        props_header ("square", lv.kind == doc::LiveKind::None ? "Shape Layer" : titles[(int) lv.kind]);
        section ("Appearance");
        shape_style_options ();
        if (lv.kind == doc::LiveKind::None)
        {
            layer_transform_properties (*session_.active ()->doc, l);
            return;
        }
        bool line = lv.kind == doc::LiveKind::Line;
        if (!shape_props_editing_)
        {
            double x0 = lv.rect[0], y0 = lv.rect[1], x1 = lv.rect[2], y1 = lv.rect[3];
            shape_props_[0] = (float) (line ? x0 : fmin (x0, x1));
            shape_props_[1] = (float) (line ? y0 : fmin (y0, y1));
            shape_props_[2] = (float) (line ? x1 : fabs (x1 - x0));
            shape_props_[3] = (float) (line ? y1 : fabs (y1 - y0));
            shape_props_[4] = (float) (lv.kind == doc::LiveKind::Rect ? lv.radius : line ? lv.weight : lv.sides);
            shape_props_[5] = (float) (lv.star > 0.0 ? (1.0 - lv.star) * 100.0 : 0.0);
        }
        section (line ? "Points" : "Bounds");
        const char* labels[4]      = {"X", "Y", "W", "H"};
        const char* line_labels[4] = {"X1", "Y1", "X2", "Y2"};
        bool active = false, done = false;
        float half = (ImGui::GetContentRegionAvail ().x - 40 * k) * 0.5f;
        for (int i = 0; i < 4; i++)
        {
            if (i % 2 == 1)
            {
                ImGui::SameLine (0, 14 * k);
            }
            opt_label (line ? line_labels[i] : labels[i]);
            ImGui::SetNextItemWidth (half - 16 * k);
            char id[8];
            snprintf (id, sizeof id, "##sp%d", i);
            ImGui::DragFloat (id, &shape_props_[i], 1.0f, -100000.0f, 100000.0f, "%.0f px");
            active = active || ImGui::IsItemActive ();
            done   = done || ImGui::IsItemDeactivatedAfterEdit ();
        }
        if (lv.kind != doc::LiveKind::Ellipse)
        {
            const char* what = lv.kind == doc::LiveKind::Rect ? "Corner Radius" : line ? "Weight" : "Sides";
            section (what);
            ImGui::SetNextItemWidth (half);
            if (lv.kind == doc::LiveKind::Polygon)
            {
                ImGui::DragFloat ("##sp4", &shape_props_[4], 0.1f, 3.0f, 100.0f, "%.0f");
            }
            else
            {
                ImGui::DragFloat ("##sp4", &shape_props_[4], 0.5f, 0.0f, 5000.0f, "%.1f px");
            }
            active = active || ImGui::IsItemActive ();
            done   = done || ImGui::IsItemDeactivatedAfterEdit ();
            if (lv.kind == doc::LiveKind::Polygon)
            {
                ImGui::SameLine (0, 14 * k);
                opt_label ("Star");
                ImGui::SetNextItemWidth (half - 40 * k);
                ImGui::DragFloat ("##sp5", &shape_props_[5], 0.5f, 0.0f, 99.0f, "%.0f%%");
                active = active || ImGui::IsItemActive ();
                done   = done || ImGui::IsItemDeactivatedAfterEdit ();
            }
        }
        shape_props_editing_ = active;
        if (done)
        {
            float* v        = shape_props_;
            engine::Value p = {{"layer", l.id}};
            if (line)
            {
                p["rect"] = {v[0], v[1], v[2], v[3]};
            }
            else
            {
                p["rect"] = {v[0], v[1], v[0] + fmaxf (v[2], 1.0f), v[1] + fmaxf (v[3], 1.0f)};
            }
            if (lv.kind == doc::LiveKind::Rect)
            {
                p["radius"] = fmaxf (v[4], 0.0f);
            }
            if (line)
            {
                p["weight"] = fmaxf (v[4], 0.1f);
            }
            if (lv.kind == doc::LiveKind::Polygon)
            {
                p["sides"] = (int) lroundf (v[4]);
                p["star"]  = v[5] > 0.0f ? 1.0f - v[5] / 100.0f : 0.0f;
            }
            run_command ("shape.setStyle", p);
        }
    }

    /* A vector mask: density and feather (previewed while dragged), and its buttons. */
    void App::vector_mask_properties (const doc::Layer& l)
    {
        float k = ui_scale_;
        section ("Vector Mask");
        if (!vmask_editing_)
        {
            vmask_props_[0] = l.vector_mask.density * 100.0f;
            vmask_props_[1] = l.vector_mask.feather;
        }
        bool changed = false, active = false, done = false;
        float w = ImGui::GetContentRegionAvail ().x - 70 * k;
        opt_label ("Density");
        ImGui::SetNextItemWidth (w);
        changed = ImGui::SliderFloat ("##vdensity", &vmask_props_[0], 0.0f, 100.0f, "%.0f%%") || changed;
        active  = active || ImGui::IsItemActive ();
        done    = done || ImGui::IsItemDeactivatedAfterEdit ();
        opt_label ("Feather");
        ImGui::SetNextItemWidth (w);
        changed =
            ImGui::SliderFloat ("##vfeather", &vmask_props_[1], 0.0f, 1000.0f, "%.1f px", ImGuiSliderFlags_Logarithmic)
            || changed;
        active         = active || ImGui::IsItemActive ();
        done           = done || ImGui::IsItemDeactivatedAfterEdit ();
        vmask_editing_ = active;
        if (changed && active)
        {
            std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*session_.active ()->doc);
            doc::Layer* target                  = copy->layer_mut (l.id);
            target->vector_mask.density         = vmask_props_[0] / 100.0f;
            target->vector_mask.feather         = vmask_props_[1];
            preview_doc_                        = copy;
            preview_serial_++;
        }
        if (done)
        {
            clear_preview ();
            run_command ("layer.vectorMask.props",
                engine::Value{{"layer", l.id}, {"density", vmask_props_[0]}, {"feather", vmask_props_[1]}});
        }
        float box = 24 * k;
        if (icon_button ("##vlink", l.vector_mask.linked ? "link" : "unlink", box, l.vector_mask.linked,
                l.vector_mask.linked ? "Unlink vector mask from layer" : "Link vector mask to layer"))
        {
            run_command ("layer.vectorMask.linked", engine::Value{{"layer", l.id}});
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##vraster", "check", box, false, "Rasterize vector mask"))
        {
            run_command ("layer.rasterize.vectorMask", engine::Value{{"layer", l.id}});
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##venable", l.vector_mask.enabled ? "eye" : "eye-off", box, false,
                l.vector_mask.enabled ? "Disable vector mask" : "Enable vector mask"))
        {
            run_command ("layer.vectorMask.enabled", engine::Value{{"layer", l.id}});
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##vdelete", "trash", box, false, "Delete vector mask"))
        {
            run_command ("layer.vectorMask.delete", engine::Value{{"layer", l.id}});
        }
    }

    /* ---- Paths panel ---- */

    /* A path's outline scaled into a square at `a` (the document's aspect kept). */
    void App::path_thumb (const doc::Document& d, const doc::Path& p, ImVec2 a, float size)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        float sc       = size / fmaxf ((float) d.size.width, (float) d.size.height);
        float iw = (float) d.size.width * sc, ih = (float) d.size.height * sc;
        ImVec2 ia (a.x + floorf ((size - iw) * 0.5f), a.y + floorf ((size - ih) * 0.5f));
        dl->AddRectFilled (ia, ImVec2 (ia.x + iw, ia.y + ih), IM_COL32 (255, 255, 255, 255));
        dl->AddRect (ImVec2 (ia.x - 1, ia.y - 1), ImVec2 (ia.x + iw + 1, ia.y + ih + 1), theme_.card_border);
        dl->PushClipRect (ia, ImVec2 (ia.x + iw, ia.y + ih), true);
        for (size_t i = 0; i < p.subpaths.size (); i++)
        {
            path_polyline (dl, p.subpaths[i], ia.x, ia.y, sc, IM_COL32 (60, 60, 60, 255), 1.0f);
        }
        dl->PopClipRect ();
    }

    void App::paths_window ()
    {
        ImGuiWindow* channels = ImGui::FindWindowByName ("Channels");
        if (channels != NULL && channels->DockId != 0)
        {
            ImGui::SetNextWindowDockID (channels->DockId, ImGuiCond_FirstUseEver);
        }
        ImGui::Begin ("Paths");
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            ImGui::TextDisabled ("No document");
            ImGui::End ();
            return;
        }
        std::shared_ptr<const doc::Document> doc = d->doc;
        sync_pen ();
        float k         = ui_scale_;
        float buttons_h = 24.0f * k + ImGui::GetStyle ().ItemSpacing.y * 2;
        ImGui::PushStyleColor (ImGuiCol_ChildBg, theme_.field);
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (0, 0));
        ImGui::BeginChild ("##paths", ImVec2 (0, -buttons_h), ImGuiChildFlags_Borders);
        float rh = roundf (36 * k), thumb = roundf (28 * k);
        const doc::Layer* al = doc->layer (d->active_layer);
        bool layer_path      = al != NULL && (al->kind == doc::LayerKind::Shape || al->has_vector_mask);
        /* Rows: the active layer's shape path or vector mask, the work path, then saved paths. */
        int rows   = (int) doc->paths.size () + (doc->has_work_path ? 1 : 0) + (layer_path ? 1 : 0);
        int action = 0, action_path = PATH_NONE;
        for (int r = 0; r < rows; r++)
        {
            int which;
            std::string label;
            const doc::Path* path;
            if (layer_path && r == 0)
            {
                which = PATH_NONE - 1;
                label = al->name + (al->kind == doc::LayerKind::Shape ? " Shape Path" : " Vector Mask");
                path  = al->kind == doc::LayerKind::Shape ? &al->shape.path : &al->vector_mask.path;
            }
            else if (doc->has_work_path && r == (layer_path ? 1 : 0))
            {
                which = PATH_WORK;
                label = "Work Path";
                path  = &doc->work_path;
            }
            else
            {
                which = r - (layer_path ? 1 : 0) - (doc->has_work_path ? 1 : 0);
                label = doc->paths[(size_t) which].name;
                path  = &doc->paths[(size_t) which].path;
            }
            ImGui::PushID (r);
            ImVec2 p       = ImGui::GetCursorScreenPos ();
            float w        = ImGui::GetContentRegionAvail ().x;
            ImDrawList* dl = ImGui::GetWindowDrawList ();
            bool selected  = which == PATH_NONE - 1 ? (path_sel_ == PATH_NONE && pen_has_target_) : path_sel_ == which;
            if (selected)
            {
                dl->AddRectFilled (p, ImVec2 (p.x + w, p.y + rh), theme_.row_selected);
            }
            dl->AddLine (ImVec2 (p.x, p.y + rh - 1), ImVec2 (p.x + w, p.y + rh - 1), theme_.separator);
            bool editing = which >= 0 && rename_path_ == which;
            if (!editing && ImGui::InvisibleButton ("##row", ImVec2 (w, rh)))
            {
                if (ImGui::GetIO ().KeyCtrl)
                {
                    action      = 1;
                    action_path = which;
                }
                else
                {
                    path_sel_          = which == PATH_NONE - 1 ? PATH_NONE : which;
                    path_hidden_layer_ = doc::NO_LAYER;
                }
            }
            if (!editing && ImGui::IsItemHovered () && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            {
                if (which == PATH_WORK)
                {
                    action = 6;
                }
                else if (which >= 0)
                {
                    rename_path_  = which;
                    rename_focus_ = true;
                    snprintf (rename_buf_, sizeof rename_buf_, "%s", label.c_str ());
                }
            }
            if (!editing && ImGui::BeginPopupContextItem ("##pathctx"))
            {
                if (which >= 0 && ImGui::MenuItem ("Duplicate Path"))
                {
                    action = 7, action_path = which;
                }
                if (which >= PATH_WORK && ImGui::MenuItem ("Delete Path"))
                {
                    action = 8, action_path = which;
                }
                if (which == PATH_WORK && ImGui::MenuItem ("Save Path…"))
                {
                    action = 6;
                }
                if (which >= 0 && ImGui::MenuItem ("Rename Path…"))
                {
                    rename_path_  = which;
                    rename_focus_ = true;
                    snprintf (rename_buf_, sizeof rename_buf_, "%s", label.c_str ());
                }
                ImGui::Separator ();
                if (ImGui::MenuItem ("Make Selection"))
                {
                    action = 1, action_path = which;
                }
                if (ImGui::MenuItem ("Fill Path"))
                {
                    action = 2, action_path = which;
                }
                if (ImGui::MenuItem ("Stroke Path"))
                {
                    action = 3, action_path = which;
                }
                if (which != PATH_NONE - 1 && ImGui::MenuItem ("Create Vector Mask"))
                {
                    action = 5, action_path = which;
                }
                ImGui::EndPopup ();
            }
            path_thumb (*doc, *path, ImVec2 (p.x + 8 * k, p.y + (rh - thumb) * 0.5f), thumb);
            float lx = p.x + 8 * k + thumb + 10 * k, th = ImGui::GetTextLineHeight ();
            if (editing)
            {
                ImGui::SetCursorScreenPos (ImVec2 (lx, p.y + (rh - ImGui::GetFrameHeight ()) * 0.5f));
                ImGui::SetNextItemWidth (w - (lx - p.x) - 8 * k);
                if (rename_focus_)
                {
                    ImGui::SetKeyboardFocusHere ();
                    rename_focus_ = false;
                }
                bool done = ImGui::InputText ("##rename", rename_buf_, sizeof rename_buf_,
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (done && rename_buf_[0] != 0)
                {
                    run_command ("path.rename", engine::Value{{"index", which}, {"name", rename_buf_}});
                }
                if (done || ImGui::IsItemDeactivated () || ImGui::IsKeyPressed (ImGuiKey_Escape))
                {
                    rename_path_ = -1;
                }
            }
            else
            {
                /* Temporary paths in italics in Photoshop; dimmer here. */
                dl->AddText (
                    ImVec2 (lx, p.y + (rh - th) * 0.5f), which < 0 ? theme_.text_dim : theme_.text, label.c_str ());
            }
            ImGui::SetCursorScreenPos (ImVec2 (p.x, p.y + rh));
            ImGui::PopID ();
        }
        ImVec2 rest = ImGui::GetContentRegionAvail ();
        if (rest.y > 0 && ImGui::InvisibleButton ("##empty", ImVec2 (fmaxf (rest.x, 1.0f), rest.y)))
        {
            path_sel_ = PATH_NONE;
            if (al != NULL)
            {
                path_hidden_layer_ = al->id;
            }
        }
        ImGui::EndChild ();
        ImGui::PopStyleVar ();
        ImGui::PopStyleColor ();

        /* The panel's buttons. */
        bool any  = pen_has_target_;
        float box = 24.0f * k;
        struct PathButton
        {
            const char* icon;
            const char* tip;
            int action;
            bool needs_path;
        };
        const PathButton buttons[] = {
            {"paint-bucket", "Fill path with foreground color", 2, true},
            {"circle", "Stroke path with brush", 3, true},
            {"circle-dashed", "Load path as a selection", 1, true},
            {"pen-tool", "Make work path from selection", 4, false},
            {"layer-mask", "Add a vector mask", 5, true},
            {"file-plus", "Create new path", 9, false},
            {"trash", "Delete current path", 8, true},
        };
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
        {
            if (i > 0)
            {
                ImGui::SameLine ();
            }
            bool disabled = (buttons[i].needs_path && !any) || (buttons[i].action == 4 && !doc->has_selection)
                         || (buttons[i].action == 8 && path_sel_ < PATH_WORK);
            if (disabled)
            {
                ImGui::BeginDisabled ();
            }
            char id[16];
            snprintf (id, sizeof id, "##pb%zu", i);
            if (icon_button (id, buttons[i].icon, box, false, buttons[i].tip))
            {
                action      = buttons[i].action;
                action_path = path_sel_ == PATH_NONE ? PATH_NONE - 1 : path_sel_;
            }
            if (disabled)
            {
                ImGui::EndDisabled ();
            }
        }
        ImGui::End ();

        /* Actions after the rows, which read the document. */
        engine::Value target;
        if (action_path >= 0)
        {
            target = engine::Value{{"target", "saved"}, {"index", action_path}};
        }
        else if (action_path == PATH_WORK)
        {
            target = engine::Value{{"target", "work"}};
        }
        else if (pen_has_target_)
        {
            target = pen_target_;
        }
        else
        {
            target = engine::Value::object ();
        }
        switch (action)
        {
        case 1:
            run_command ("path.makeSelection", target);
            break;
        case 2:
            run_command ("path.fill", target);
            break;
        case 3:
            run_command ("path.stroke", target);
            break;
        case 4:
            run_command ("path.fromSelection", engine::Value::object ());
            path_sel_ = PATH_WORK;
            break;
        case 5:
            run_command ("layer.vectorMask.currentPath", target);
            break;
        case 6:
            {
                size_t n = doc->paths.size ();
                run_command ("path.save", engine::Value::object ());
                path_sel_ = (int) n;
                break;
            }
        case 7:
            run_command ("path.duplicate", engine::Value{{"index", action_path}});
            break;
        case 8:
            if (action_path >= PATH_WORK)
            {
                run_command ("path.delete", target);
            }
            path_sel_ = PATH_NONE;
            break;
        case 9:
            {
                size_t n = doc->paths.size ();
                run_command ("path.new", engine::Value::object ());
                path_sel_ = (int) n;
                break;
            }
        default:
            break;
        }
    }

} // namespace ui
