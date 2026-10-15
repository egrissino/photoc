#include "algo/select_ops.h"
#include "compose/masks.h"
#include "engine/layers.h"
#include "engine/session.h"
#include "engine/smart.h"
#include "engine/text.h"
#include "engine/vector.h"
#include "vec/vec.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace engine
{

    /* Paths, shape layers and vector masks. Every path lives in one of four places: the work path,
       a saved path (the Paths panel), a shape layer or a layer's vector mask; commands name it with
       "target" ("work", "saved" + "index", "shape" / "vectorMask" + "layer"). The Pen and Path
       Selection tools edit a path by replacing it whole (path.set), one history step per edit.
       Shape layers and vector masks keep their rendering (pixels / a gray mask) next to the path
       and redraw it after every change. */

    const char* const OP_IDS[5]    = {"combine", "subtract", "intersect", "exclude", "join"};
    const char* const LIVE_IDS[5]  = {"none", "rect", "ellipse", "polygon", "line"};
    const char* const ALIGN_IDS[3] = {"inside", "center", "outside"};
    const char* const CAP_IDS[3]   = {"butt", "round", "square"};
    const char* const JOIN_IDS[3]  = {"miter", "round", "bevel"};
    const double MAX_COORD         = 1e7;

    int find_id (const std::string& s, const char* const* ids, int n)
    {
        for (int i = 0; i < n; i++)
        {
            if (s == ids[i])
            {
                return i;
            }
        }
        return -1;
    }

    void hex_color (const float c[3], char out[8])
    {
        snprintf (out, 8, "#%02x%02x%02x", (int) lroundf (fminf (fmaxf (c[0], 0.0f), 1.0f) * 255),
            (int) lroundf (fminf (fmaxf (c[1], 0.0f), 1.0f) * 255),
            (int) lroundf (fminf (fmaxf (c[2], 0.0f), 1.0f) * 255));
    }

    /* {evenOdd, inverted, subpaths: [{closed, op, knots: [[ax, ay, inx, iny, outx, outy, smooth]]}]} */
    Value path_json (const doc::Path& p)
    {
        Value subs = Value::array ();
        for (size_t i = 0; i < p.subpaths.size (); i++)
        {
            const doc::Subpath& s = p.subpaths[i];
            Value knots           = Value::array ();
            for (size_t k = 0; k < s.knots.size (); k++)
            {
                const doc::Knot& n = s.knots[k];
                knots.push_back (Value{
                    n.anchor[0], n.anchor[1], n.in_ctrl[0], n.in_ctrl[1], n.out_ctrl[0], n.out_ctrl[1], n.smooth});
            }
            subs.push_back (Value{{"closed", s.closed}, {"op", OP_IDS[(int) s.op]}, {"knots", knots}});
        }
        return Value{{"evenOdd", p.even_odd}, {"inverted", p.inverted}, {"subpaths", subs}};
    }

    bool coord (const Value& v, double* out)
    {
        if (!v.is_number ())
        {
            return false;
        }
        *out = v.get<double> ();
        return isfinite (*out) && fabs (*out) <= MAX_COORD;
    }

    bool path_from_json (const Value& v, doc::Path* out, const char* cmd, Error* err)
    {
        *out = doc::Path::make ();
        if (!v.is_object ())
        {
            return bad_params (err, cmd, "a path must be an object {subpaths: [...]}");
        }
        param_bool (v, "evenOdd", &out->even_odd);
        param_bool (v, "inverted", &out->inverted);
        const Value* subs = param (v, "subpaths");
        if (subs == NULL)
        {
            return true;
        }
        if (!subs->is_array ())
        {
            return bad_params (err, cmd, "`subpaths` must be an array");
        }
        for (size_t i = 0; i < subs->size (); i++)
        {
            const Value& sv = (*subs)[i];
            doc::Subpath s;
            s.closed = false;
            s.op     = doc::PathOp::Combine;
            param_bool (sv, "closed", &s.closed);
            std::string op;
            if (param_str (sv, "op", &op))
            {
                int k = find_id (op, OP_IDS, 5);
                if (k < 0)
                {
                    return bad_params (err, cmd, "`op` must be combine, subtract, intersect, exclude or join");
                }
                s.op = (doc::PathOp) k;
            }
            const Value* knots = param (sv, "knots");
            if (knots == NULL || !knots->is_array ())
            {
                return bad_params (err, cmd, "every subpath needs `knots`");
            }
            for (size_t k = 0; k < knots->size (); k++)
            {
                const Value& kv = (*knots)[k];
                if (!kv.is_array () || (kv.size () != 2 && kv.size () < 6))
                {
                    return bad_params (err, cmd, "a knot is [x, y] or [x, y, inX, inY, outX, outY, smooth?]");
                }
                double c[6];
                for (size_t j = 0; j < 6; j++)
                {
                    if (!coord (kv[j < kv.size () ? j : j % 2], &c[j]))
                    {
                        return bad_params (err, cmd, "knot coordinates must be numbers within ±1e7");
                    }
                }
                doc::Knot n   = doc::Knot::corner (c[0], c[1]);
                n.in_ctrl[0]  = c[2];
                n.in_ctrl[1]  = c[3];
                n.out_ctrl[0] = c[4];
                n.out_ctrl[1] = c[5];
                n.smooth      = kv.size () > 6 && kv[6].is_boolean () && kv[6].get<bool> ();
                s.knots.push_back (n);
            }
            out->subpaths.push_back (s);
        }
        return true;
    }

    /* ---- Rendering ---- */

    void render_shape_layer (Session& s, const doc::Document& d, doc::Layer* l)
    {
        l->pixels = vec::render_shape (l->shape, d.pixel_format (), s.cmyk, d.bounds ());
    }

    void render_vector_mask (const doc::Document& d, doc::Layer* l)
    {
        l->vector_mask.surface = vec::vector_mask_surface (l->vector_mask.path, d.bounds ());
    }

    /* Type, shapes and vector masks redrawn (after a format change or a transform). */
    void render_all_live (Session& s, doc::Document* d)
    {
        render_all_type (s, d);
        render_smart_changed (s, d, false);
        std::vector<doc::WalkEntry> walk = d->walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            doc::Layer* l = d->layer_mut (walk[i].layer->id);
            if (l == NULL)
            {
                continue;
            }
            if (l->kind == doc::LayerKind::Shape)
            {
                render_shape_layer (s, *d, l);
            }
            if (l->has_vector_mask)
            {
                render_vector_mask (*d, l);
            }
        }
    }

    void map_point (const algo::Homography& h, double p[2])
    {
        double x, y;
        h.apply (p[0], p[1], &x, &y);
        p[0] = x;
        p[1] = y;
    }

    /* Every point through h (projective maps move the control points, a close approximation). */
    void map_path (doc::Path* p, const algo::Homography& h)
    {
        for (size_t i = 0; i < p->subpaths.size (); i++)
        {
            for (size_t k = 0; k < p->subpaths[i].knots.size (); k++)
            {
                doc::Knot& n = p->subpaths[i].knots[k];
                map_point (h, n.anchor);
                map_point (h, n.in_ctrl);
                map_point (h, n.out_ctrl);
            }
        }
    }

    /* A live shape stays live through moves and scales; anything else makes it a plain path. */
    void map_live (doc::LiveShape* live, const algo::Homography& h)
    {
        if (live->kind == doc::LiveKind::None)
        {
            return;
        }
        if (h.m[1] != 0.0 || h.m[3] != 0.0 || h.m[6] != 0.0 || h.m[7] != 0.0)
        {
            live->kind = doc::LiveKind::None;
            return;
        }
        map_point (h, &live->rect[0]);
        map_point (h, &live->rect[2]);
    }

    algo::Homography translation (double dx, double dy)
    {
        algo::Homography h = algo::Homography::identity ();
        h.m[2]             = dx;
        h.m[5]             = dy;
        return h;
    }

    /* The Move tool: a shape's path and a linked vector mask go with the layer. */
    void translate_vectors (doc::Layer* l, int32_t dx, int32_t dy)
    {
        algo::Homography h = translation (dx, dy);
        if (l->kind == doc::LayerKind::Shape)
        {
            map_path (&l->shape.path, h);
            map_live (&l->shape.live, h);
            l->pixels = l->pixels.translated (dx, dy, l->pixels.content_bounds ());
        }
        if (l->kind == doc::LayerKind::Smart)
        {
            /* Moved as rendered: no need to render again. */
            map_smart (&l->smart, h);
            map_quad (l->smart.rendered, h);
            l->pixels = l->pixels.translated (dx, dy, l->pixels.content_bounds ());
        }
        if (l->has_vector_mask && l->vector_mask.linked)
        {
            map_path (&l->vector_mask.path, h);
            raster::Surface& m = l->vector_mask.surface;
            m                  = m.translated (dx, dy, m.content_bounds ());
        }
    }

    /* Whole-document changes (image size, canvas size, crop, rotation) carry every path along. */
    void follow_vectors (Session& s, doc::Document* d, const double m[6])
    {
        algo::Homography h = algo::Homography::identity ();
        h.m[0]             = m[0];
        h.m[1]             = m[2];
        h.m[2]             = m[4];
        h.m[3]             = m[1];
        h.m[4]             = m[3];
        h.m[5]             = m[5];
        for (size_t i = 0; i < d->paths.size (); i++)
        {
            map_path (&d->paths[i].path, h);
        }
        std::vector<doc::WalkEntry> all = d->walk ();
        for (size_t i = 0; i < all.size (); i++)
        {
            if (all[i].layer->kind == doc::LayerKind::Smart)
            {
                map_smart (&d->layer_mut (all[i].layer->id)->smart, h);
            }
        }
        map_path (&d->work_path, h);
        std::vector<doc::WalkEntry> walk = d->walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            doc::Layer* l = d->layer_mut (walk[i].layer->id);
            if (l->kind == doc::LayerKind::Shape)
            {
                map_path (&l->shape.path, h);
                map_live (&l->shape.live, h);
                render_shape_layer (s, *d, l);
            }
            if (l->has_vector_mask)
            {
                map_path (&l->vector_mask.path, h);
                render_vector_mask (*d, l);
            }
        }
        render_smart_changed (s, d, false);
    }

    /* ---- Path targets ---- */

    enum class PathPlace : uint8_t
    {
        Work,
        Saved,
        Shape,
        VectorMask,
    };

    struct PathRef
    {
        PathPlace place;
        size_t index;
        doc::LayerId layer;
    };

    bool path_ref (const Session& s, const Value& p, PathRef* out, const char* cmd, Error* err)
    {
        std::string t = "work";
        param_str (p, "target", &t);
        out->index             = 0;
        out->layer             = doc::NO_LAYER;
        const doc::Document& d = *s.active ()->doc;
        if (t == "work")
        {
            out->place = PathPlace::Work;
            return true;
        }
        if (t == "saved")
        {
            int64_t i;
            if (!param_i64 (p, "index", &i) || i < 0 || (size_t) i >= d.paths.size ())
            {
                return bad_params (err, cmd, "no saved path at `index`");
            }
            out->place = PathPlace::Saved;
            out->index = (size_t) i;
            return true;
        }
        if (t != "shape" && t != "vectorMask")
        {
            return bad_params (err, cmd, "`target` must be work, saved, shape or vectorMask");
        }
        if (!layer_param (s, p, &out->layer, err))
        {
            return false;
        }
        const doc::Layer* l = d.layer (out->layer);
        if (l == NULL)
        {
            return no_layer (err, out->layer);
        }
        if (t == "shape")
        {
            if (l->kind != doc::LayerKind::Shape)
            {
                return fail (err, ErrorKind::Other, "\"%s\" is not a shape layer", l->name.c_str ());
            }
            out->place = PathPlace::Shape;
        }
        else
        {
            if (!l->has_vector_mask)
            {
                return fail (err, ErrorKind::Other, "\"%s\" has no vector mask", l->name.c_str ());
            }
            out->place = PathPlace::VectorMask;
        }
        return true;
    }

    doc::Path* ref_path (doc::Document* d, const PathRef& r)
    {
        switch (r.place)
        {
        case PathPlace::Work:
            return &d->work_path;
        case PathPlace::Saved:
            return &d->paths[r.index].path;
        case PathPlace::Shape:
            return &d->layer_mut (r.layer)->shape.path;
        default:
            return &d->layer_mut (r.layer)->vector_mask.path;
        }
    }

    /* The path a command works on: inline "path", else the target (which must hold one). */
    bool source_path (const Session& s, const Value& p, doc::Path* out, const char* cmd, Error* err)
    {
        const Value* inline_path = param (p, "path");
        if (inline_path != NULL)
        {
            return path_from_json (*inline_path, out, cmd, err);
        }
        PathRef r;
        if (!path_ref (s, p, &r, cmd, err))
        {
            return false;
        }
        const doc::Document& d = *s.active ()->doc;
        if (r.place == PathPlace::Work && !d.has_work_path)
        {
            return fail (err, ErrorKind::Other, "there is no work path");
        }
        *out = *ref_path (const_cast<doc::Document*> (&d), r);
        if (out->empty ())
        {
            return fail (err, ErrorKind::Other, "the path is empty");
        }
        return true;
    }

    /* Replaces the target's path (the Pen, shape and path selection tools). */
    bool cmd_path_set (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "path.set";
        PathRef r;
        if (!path_ref (s, p, &r, cmd, err))
        {
            return false;
        }
        const Value* pv = param (p, "path");
        if (pv == NULL)
        {
            return bad_params (err, cmd, "missing `path`");
        }
        doc::Path path;
        if (!path_from_json (*pv, &path, cmd, err))
        {
            return false;
        }
        std::string label = "Edit Path";
        param_str (p, "label", &label);
        Edit e (s);
        doc::Document* d = e.doc ();
        if (r.place == PathPlace::Work)
        {
            d->has_work_path = !path.subpaths.empty ();
        }
        *ref_path (d, r) = path;
        if (r.place == PathPlace::Shape)
        {
            doc::Layer* l = d->layer_mut (r.layer);
            bool keep     = false;
            param_bool (p, "keepLive", &keep);
            if (!keep)
            {
                l->shape.live.kind = doc::LiveKind::None;
            }
            render_shape_layer (s, *d, l);
        }
        if (r.place == PathPlace::VectorMask)
        {
            render_vector_mask (*d, d->layer_mut (r.layer));
        }
        return e.commit (label.c_str (), err);
    }

    bool cmd_path_get (Session& s, const Value& p, Value* result, Error* err)
    {
        PathRef r;
        if (!path_ref (s, p, &r, "path.get", err))
        {
            return false;
        }
        doc::Document* d = const_cast<doc::Document*> (s.active ()->doc.get ());
        *result          = path_json (*ref_path (d, r));
        return true;
    }

    /* The Paths panel: saved paths by name, and whether there is a work path. */
    bool cmd_path_list (Session& s, const Value& p, Value* result, Error* err)
    {
        const doc::Document& d = *s.active ()->doc;
        Value list             = Value::array ();
        for (size_t i = 0; i < d.paths.size (); i++)
        {
            list.push_back (Value{{"name", d.paths[i].name}, {"subpaths", d.paths[i].path.subpaths.size ()}});
        }
        *result = Value{{"paths", list}, {"work", d.has_work_path}};
        return true;
    }

    std::string next_path_name (const doc::Document& d)
    {
        char buf[64];
        for (size_t n = 1;; n++)
        {
            snprintf (buf, sizeof buf, "Path %zu", n);
            bool used = false;
            for (size_t i = 0; i < d.paths.size (); i++)
            {
                used = used || d.paths[i].name == buf;
            }
            if (!used)
            {
                return buf;
            }
        }
    }

    bool name_param (const Value& p, std::string* out, const char* cmd, Error* err)
    {
        if (!param_present (p, "name"))
        {
            return true;
        }
        if (!param_str (p, "name", out) || out->empty ())
        {
            return bad_params (err, cmd, "`name` must be a non-empty string");
        }
        return true;
    }

    /* Save Path: the work path becomes a saved path. */
    bool cmd_path_save (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "path.save";
        Edit e (s);
        doc::Document* d = e.doc ();
        if (!d->has_work_path)
        {
            return fail (err, ErrorKind::Other, "there is no work path to save");
        }
        doc::NamedPath np = {next_path_name (*d), d->work_path};
        if (!name_param (p, &np.name, cmd, err))
        {
            return false;
        }
        d->paths.push_back (np);
        d->has_work_path = false;
        d->work_path     = doc::Path::make ();
        if (!e.commit ("Save Path", err))
        {
            return false;
        }
        *result = Value{{"index", s.active ()->doc->paths.size () - 1}};
        return true;
    }

    bool cmd_path_new (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d  = e.doc ();
        doc::NamedPath np = {next_path_name (*d), doc::Path::make ()};
        if (!name_param (p, &np.name, "path.new", err))
        {
            return false;
        }
        d->paths.push_back (np);
        if (!e.commit ("New Path", err))
        {
            return false;
        }
        *result = Value{{"index", s.active ()->doc->paths.size () - 1}};
        return true;
    }

    bool cmd_path_delete (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "path.delete";
        PathRef r;
        if (!path_ref (s, p, &r, cmd, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (r.place == PathPlace::Work)
        {
            if (!d->has_work_path)
            {
                return fail (err, ErrorKind::Other, "there is no work path");
            }
            d->has_work_path = false;
            d->work_path     = doc::Path::make ();
        }
        else if (r.place == PathPlace::Saved)
        {
            d->paths.erase (d->paths.begin () + (ptrdiff_t) r.index);
        }
        else
        {
            return bad_params (err, cmd, "only the work path and saved paths can be deleted");
        }
        return e.commit ("Delete Path", err);
    }

    bool cmd_path_rename (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "path.rename";
        int64_t i;
        std::string name;
        if (!param_i64 (p, "index", &i) || i < 0 || (size_t) i >= s.active ()->doc->paths.size ())
        {
            return bad_params (err, cmd, "no saved path at `index`");
        }
        if (!param_str (p, "name", &name) || name.empty ())
        {
            return bad_params (err, cmd, "`name` must be a non-empty string");
        }
        Edit e (s);
        e.doc ()->paths[(size_t) i].name = name;
        return e.commit ("Rename Path", err);
    }

    bool cmd_path_duplicate (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "path.duplicate";
        int64_t i;
        if (!param_i64 (p, "index", &i) || i < 0 || (size_t) i >= s.active ()->doc->paths.size ())
        {
            return bad_params (err, cmd, "no saved path at `index`");
        }
        Edit e (s);
        doc::Document* d  = e.doc ();
        doc::NamedPath np = d->paths[(size_t) i];
        np.name += " copy";
        if (!name_param (p, &np.name, cmd, err))
        {
            return false;
        }
        d->paths.insert (d->paths.begin () + (ptrdiff_t) i + 1, np);
        if (!e.commit ("Duplicate Path", err))
        {
            return false;
        }
        *result = Value{{"index", i + 1}};
        return true;
    }

    /* The path's coverage as a selection-style mask over the canvas, optionally hard-edged and
       feathered. */
    raster::Surface path_selection (const doc::Path& path, geom::Rect canvas, bool aa, double feather)
    {
        raster::Surface m = vec::path_mask (path, canvas);
        if (!aa || feather > 0.0)
        {
            std::vector<float> v = algo::mask_from_surface (&m, canvas);
            if (!aa)
            {
                for (size_t i = 0; i < v.size (); i++)
                {
                    v[i] = v[i] >= 0.5f ? 1.0f : 0.0f;
                }
            }
            if (feather > 0.0)
            {
                v = algo::feather_mask (v, canvas.width (), canvas.height (), (float) feather);
            }
            m = algo::mask_to_surface (v, canvas);
        }
        return m;
    }

    /* Make Selection (Ctrl+Enter, Ctrl-click a path). */
    bool cmd_path_make_selection (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "path.makeSelection";
        doc::Path path;
        if (!source_path (s, p, &path, cmd, err))
        {
            return false;
        }
        double feather = 0.0;
        if (param_present (p, "feather")
            && (!param_f64 (p, "feather", &feather) || !(feather >= 0.0 && feather <= 1000.0)))
        {
            return bad_params (err, cmd, "`feather` must be 0..1000 px");
        }
        bool aa = true;
        param_bool (p, "antialias", &aa);
        std::string mode = "replace";
        param_str (p, "mode", &mode);
        Edit e (s);
        doc::Document* d = e.doc ();
        apply_selection (d, path_selection (path, d->bounds (), aa, feather), mode);
        return e.commit ("Make Selection", err);
    }

    bool blend_params (Session& s, const Value& p, FillBlend* b, const char* cmd, Error* err)
    {
        b->mode = color::BlendMode::Normal;
        if (param_present (p, "mode"))
        {
            std::string m;
            if (!param_str (p, "mode", &m) || !blend_from_str (m.c_str (), &b->mode)
                || b->mode == color::BlendMode::PassThrough)
            {
                return bad_params (err, cmd, "unknown blend mode");
            }
        }
        b->opacity = 1.0f;
        double o;
        if (param_f64 (p, "opacity", &o) && isfinite (o))
        {
            b->opacity = (float) ((o < 0.0 ? 0.0 : o > 100.0 ? 100.0 : o) / 100.0);
        }
        color_param (p, "color", s.tools.foreground, b->color);
        std::string contents;
        if (param_str (p, "contents", &contents) && contents == "background")
        {
            memcpy (b->color, s.tools.background, sizeof b->color);
        }
        bool preserve = false;
        param_bool (p, "preserveTransparency", &preserve);
        b->keep_alpha = preserve;
        return true;
    }

    /* Paints `mask` into the target layer with the blend settings. */
    bool paint_mask (
        Session& s, const Value& p, const raster::Surface& mask, FillBlend b, const char* label, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d      = e.doc ();
        raster::Surface* surf = pixel_target (d, id, err);
        if (surf == NULL)
        {
            return false;
        }
        geom::Rect r = mask.content_bounds ().intersect (d->bounds ());
        if (r.is_empty ())
        {
            return true;
        }
        b.keep_alpha = b.keep_alpha || d->effective_locks (id).transparency;
        fill_surface (surf, r, &mask, b, s.cmyk);
        return e.commit (label, err);
    }

    /* Fill Path: the path's area painted on the active pixel layer. */
    bool cmd_path_fill (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "path.fill";
        doc::Path path;
        if (!source_path (s, p, &path, cmd, err))
        {
            return false;
        }
        FillBlend b;
        if (!blend_params (s, p, &b, cmd, err))
        {
            return false;
        }
        double feather = 0.0;
        param_f64 (p, "feather", &feather);
        bool aa = true;
        param_bool (p, "antialias", &aa);
        geom::Rect canvas = s.active ()->doc->bounds ();
        return paint_mask (s, p, path_selection (path, canvas, aa, fmax (feather, 0.0)), b, "Fill Path", err);
    }

    bool stroke_params (const Value& v, doc::ShapeStroke* st, const char* cmd, Error* err)
    {
        param_bool (v, "enabled", &st->enabled);
        double w;
        if (param_f64 (v, "width", &w))
        {
            if (!(w >= 0.0 && w <= 1000.0))
            {
                return bad_params (err, cmd, "stroke `width` must be 0..1000 px");
            }
            st->width = (float) w;
        }
        if (param_present (v, "color"))
        {
            float c[4] = {st->color[0], st->color[1], st->color[2], 1.0f};
            color_param (v, "color", c, c);
            memcpy (st->color, c, sizeof st->color);
        }
        double o;
        if (param_f64 (v, "opacity", &o))
        {
            st->opacity = (float) (fmin (fmax (o, 0.0), 100.0) / 100.0);
        }
        std::string id;
        if (param_str (v, "align", &id))
        {
            int k = find_id (id, ALIGN_IDS, 3);
            if (k < 0)
            {
                return bad_params (err, cmd, "stroke `align` must be inside, center or outside");
            }
            st->align = (doc::StrokeAlign) k;
        }
        if (param_str (v, "cap", &id))
        {
            int k = find_id (id, CAP_IDS, 3);
            if (k < 0)
            {
                return bad_params (err, cmd, "stroke `cap` must be butt, round or square");
            }
            st->cap = (doc::LineCap) k;
        }
        if (param_str (v, "join", &id))
        {
            int k = find_id (id, JOIN_IDS, 3);
            if (k < 0)
            {
                return bad_params (err, cmd, "stroke `join` must be miter, round or bevel");
            }
            st->join = (doc::LineJoin) k;
        }
        double ml;
        if (param_f64 (v, "miterLimit", &ml))
        {
            st->miter_limit = (float) fmin (fmax (ml, 1.0), 100.0);
        }
        const Value* dashes = param (v, "dashes");
        if (dashes != NULL)
        {
            if (!dashes->is_array ())
            {
                return bad_params (err, cmd, "stroke `dashes` must be an array of lengths (in stroke widths)");
            }
            st->dashes.clear ();
            for (size_t i = 0; i < dashes->size () && i < 16; i++)
            {
                st->dashes.push_back ((*dashes)[i].is_number () ? fmaxf ((*dashes)[i].get<float> (), 0.0f) : 0.0f);
            }
        }
        return true;
    }

    Value stroke_json (const doc::ShapeStroke& st)
    {
        char c[8];
        hex_color (st.color, c);
        Value dashes = Value::array ();
        for (size_t i = 0; i < st.dashes.size (); i++)
        {
            dashes.push_back (st.dashes[i]);
        }
        return Value{{"enabled", st.enabled}, {"width", st.width}, {"color", c}, {"opacity", st.opacity * 100.0f},
            {"align", ALIGN_IDS[(int) st.align]}, {"cap", CAP_IDS[(int) st.cap]}, {"join", JOIN_IDS[(int) st.join]},
            {"miterLimit", st.miter_limit}, {"dashes", dashes}};
    }

    /* Stroke Path: a band of `width` px along the path, painted on the active pixel layer. */
    bool cmd_path_stroke (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "path.stroke";
        doc::Path path;
        if (!source_path (s, p, &path, cmd, err))
        {
            return false;
        }
        FillBlend b;
        if (!blend_params (s, p, &b, cmd, err))
        {
            return false;
        }
        doc::ShapeStroke st = doc::ShapeLayer::make ().stroke;
        st.width            = s.tools.brush.size;
        st.cap              = doc::LineCap::Round;
        st.join             = doc::LineJoin::Round;
        if (!stroke_params (p, &st, cmd, err))
        {
            return false;
        }
        vec::StrokeParams sp = vec::StrokeParams::make (st.width);
        sp.cap               = st.cap;
        sp.join              = st.join;
        sp.miter_limit       = st.miter_limit;
        sp.dashes            = st.dashes;
        geom::Rect canvas    = s.active ()->doc->bounds ();
        vec::Coverage c;
        if (!vec::stroke_coverage (path, sp, canvas, &c))
        {
            return fail (err, ErrorKind::Other, "the stroke is too large");
        }
        float zero           = 0.0f;
        raster::Surface mask = raster::Surface::with_default (color::PixelFormat::gray8 (), &zero);
        if (!c.a.empty ())
        {
            mask.write_region (c.rect, c.a.data (), c.a.size ());
        }
        return paint_mask (s, p, mask, b, "Stroke Path", err);
    }

    /* ---- Make Work Path from a selection ---- */

    struct Edge
    {
        uint64_t from;
        uint64_t to;
        bool used;
    };

    bool edge_before (const Edge& a, const Edge& b)
    {
        return a.from < b.from;
    }

    uint64_t vkey (int64_t x, int64_t y)
    {
        return ((uint64_t) (uint32_t) (int32_t) y << 32) | (uint32_t) (int32_t) x;
    }

    void vxy (uint64_t k, double* x, double* y)
    {
        *x = (double) (int32_t) (uint32_t) (k & 0xffffffffu);
        *y = (double) (int32_t) (uint32_t) (k >> 32);
    }

    double seg_dist (const double* p, const double* a, const double* b)
    {
        double dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
        double t = l2 > 0.0 ? fmin (fmax (((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2, 0.0), 1.0) : 0.0;
        return hypot (a[0] + dx * t - p[0], a[1] + dy * t - p[1]);
    }

    /* Douglas-Peucker over pts[a..b], marking kept points. */
    void simplify (const std::vector<double>& pts, size_t a, size_t b, double tol, std::vector<bool>* keep)
    {
        if (b <= a + 1)
        {
            return;
        }
        double best = -1.0;
        size_t at   = a;
        for (size_t i = a + 1; i < b; i++)
        {
            double d = seg_dist (&pts[i * 2], &pts[a * 2], &pts[b * 2]);
            if (d > best)
            {
                best = d;
                at   = i;
            }
        }
        if (best <= tol)
        {
            return;
        }
        (*keep)[at] = true;
        simplify (pts, a, at, tol, keep);
        simplify (pts, at, b, tol, keep);
    }

    /* The outlines of `m` (w x h at x0, y0, selected where >= 0.5) traced along pixel edges with
       the inside on the right, simplified to within `tol` px. Holes wind the other way, so the
       nonzero rule keeps them open. */
    doc::Path trace_mask (const std::vector<float>& m, size_t w, size_t h, int32_t x0, int32_t y0, double tol)
    {
        std::vector<Edge> edges;
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                if (m[y * w + x] < 0.5f)
                {
                    continue;
                }
                int64_t X = (int64_t) x + x0, Y = (int64_t) y + y0;
                if (y == 0 || m[(y - 1) * w + x] < 0.5f)
                {
                    edges.push_back (Edge{vkey (X, Y), vkey (X + 1, Y), false});
                }
                if (x + 1 == w || m[y * w + x + 1] < 0.5f)
                {
                    edges.push_back (Edge{vkey (X + 1, Y), vkey (X + 1, Y + 1), false});
                }
                if (y + 1 == h || m[(y + 1) * w + x] < 0.5f)
                {
                    edges.push_back (Edge{vkey (X + 1, Y + 1), vkey (X, Y + 1), false});
                }
                if (x == 0 || m[y * w + x - 1] < 0.5f)
                {
                    edges.push_back (Edge{vkey (X, Y + 1), vkey (X, Y), false});
                }
            }
        }
        std::stable_sort (edges.begin (), edges.end (), edge_before);
        doc::Path path = doc::Path::make ();
        for (size_t start = 0; start < edges.size (); start++)
        {
            if (edges[start].used)
            {
                continue;
            }
            std::vector<double> pts;
            size_t e = start;
            while (true)
            {
                edges[e].used = true;
                double x, y;
                vxy (edges[e].from, &x, &y);
                pts.push_back (x);
                pts.push_back (y);
                Edge probe                     = {edges[e].to, 0, false};
                std::vector<Edge>::iterator it = std::lower_bound (edges.begin (), edges.end (), probe, edge_before);
                size_t next                    = edges.size ();
                for (; it != edges.end () && it->from == probe.from; ++it)
                {
                    if (!it->used)
                    {
                        next = (size_t) (it - edges.begin ());
                        break;
                    }
                }
                if (next == edges.size ())
                {
                    break;
                }
                e = next;
            }
            size_t n = pts.size () / 2;
            if (n < 3)
            {
                continue;
            }
            /* Closed: simplify both halves between the first point and the farthest from it. */
            size_t far  = 0;
            double best = -1.0;
            for (size_t i = 1; i < n; i++)
            {
                double d = hypot (pts[i * 2] - pts[0], pts[i * 2 + 1] - pts[1]);
                if (d > best)
                {
                    best = d;
                    far  = i;
                }
            }
            pts.push_back (pts[0]);
            pts.push_back (pts[1]);
            std::vector<bool> keep (n + 1, false);
            keep[0] = keep[far] = true;
            simplify (pts, 0, far, tol, &keep);
            simplify (pts, far, n, tol, &keep);
            doc::Subpath sp;
            sp.closed = true;
            sp.op     = doc::PathOp::Combine;
            for (size_t i = 0; i < n; i++)
            {
                if (keep[i])
                {
                    sp.knots.push_back (doc::Knot::corner (pts[i * 2], pts[i * 2 + 1]));
                }
            }
            if (sp.knots.size () >= 3)
            {
                path.subpaths.push_back (sp);
            }
        }
        return path;
    }

    bool cmd_path_from_selection (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "path.fromSelection";
        double tol      = 2.0;
        if (param_present (p, "tolerance") && (!param_f64 (p, "tolerance", &tol) || !(tol >= 0.5 && tol <= 10.0)))
        {
            return bad_params (err, cmd, "`tolerance` must be 0.5..10 px");
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        geom::Rect area  = d->selection.content_bounds ().intersect (d->bounds ());
        if (area.is_empty ())
        {
            return fail (err, ErrorKind::Other, "nothing is selected");
        }
        std::vector<float> m = algo::mask_from_surface (&d->selection, area);
        d->work_path         = trace_mask (m, area.width (), area.height (), area.x0, area.y0, tol);
        d->has_work_path     = !d->work_path.subpaths.empty ();
        if (!e.commit ("Make Work Path", err))
        {
            return false;
        }
        *result = Value{{"subpaths", s.active ()->doc->work_path.subpaths.size ()}};
        return true;
    }

    /* ---- Shape layers ---- */

    bool fill_param (const Value& p, doc::ShapeLayer* sh, const char* cmd, Error* err)
    {
        const Value* f = param (p, "fill");
        if (f == NULL)
        {
            return true;
        }
        if (f->is_null ())
        {
            sh->has_fill = false;
            return true;
        }
        float c[4] = {-1, -1, -1, -1};
        color_param (p, "fill", c, c);
        if (c[0] < 0.0f)
        {
            return bad_params (err, cmd, "`fill` must be \"#rrggbb\", [r, g, b] or null");
        }
        memcpy (sh->fill, c, sizeof sh->fill);
        sh->has_fill = true;
        return true;
    }

    bool rect4 (const Value& p, const char* key, double out[4])
    {
        const Value* r = param (p, key);
        if (r == NULL || !r->is_array () || r->size () != 4)
        {
            return false;
        }
        for (size_t i = 0; i < 4; i++)
        {
            if (!coord ((*r)[i], &out[i]))
            {
                return false;
            }
        }
        return true;
    }

    /* A new shape layer: a live rectangle / ellipse / polygon / line in "rect" ([x0, y0, x1, y1];
       for lines, the two end points), or any "path"; filled with the foreground colour unless
       "fill" says otherwise. */
    bool cmd_shape_create (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd  = "shape.create";
        std::string kind = "rect";
        param_str (p, "kind", &kind);
        doc::ShapeLayer sh = doc::ShapeLayer::make ();
        memcpy (sh.fill, s.tools.foreground, sizeof sh.fill);
        memcpy (sh.stroke.color, s.tools.background, sizeof sh.stroke.color);
        const char* base = "Shape";
        if (kind == "path")
        {
            const Value* pv = param (p, "path");
            if (pv == NULL)
            {
                return bad_params (err, cmd, "missing `path`");
            }
            if (!path_from_json (*pv, &sh.path, cmd, err))
            {
                return false;
            }
        }
        else
        {
            int k = find_id (kind, LIVE_IDS, 5);
            if (k <= 0)
            {
                return bad_params (err, cmd, "`kind` must be rect, ellipse, polygon, line or path");
            }
            sh.live.kind = (doc::LiveKind) k;
            if (!rect4 (p, "rect", sh.live.rect))
            {
                return bad_params (err, cmd, "`rect` must be [x0, y0, x1, y1]");
            }
            double v;
            sh.live.radius = param_f64 (p, "radius", &v) ? fmax (v, 0.0) : 0.0;
            sh.live.sides  = 5;
            int64_t n;
            if (param_i64 (p, "sides", &n))
            {
                sh.live.sides = (uint32_t) (n < 3 ? 3 : n > 100 ? 100 : n);
            }
            sh.live.star               = param_f64 (p, "star", &v) ? fmin (fmax (v, 0.0), 1.0) : 0.0;
            sh.live.weight             = param_f64 (p, "weight", &v) ? fmin (fmax (v, 0.1), 1000.0) : 1.0;
            sh.path                    = doc::live_path (sh.live);
            const char* const NAMES[5] = {"Shape", "Rectangle", "Ellipse", "Polygon", "Line"};
            base                       = NAMES[k];
        }
        if (!fill_param (p, &sh, cmd, err))
        {
            return false;
        }
        const Value* st = param (p, "stroke");
        if (st != NULL)
        {
            sh.stroke.enabled = true;
            if (!stroke_params (*st, &sh.stroke, cmd, err))
            {
                return false;
            }
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        std::string name = d->next_layer_name (base);
        param_str (p, "name", &name);
        doc::Layer l = doc::Layer::make (s.ids, name, doc::LayerKind::Shape);
        l.shape      = sh;
        render_shape_layer (s, *d, &l);
        doc::LayerId at = *e.active_layer () != doc::NO_LAYER ? *e.active_layer () : d->top_layer ();
        doc::LayerId id = d->insert_above (at, l);
        e.select (std::vector<doc::LayerId> (1, id), id);
        std::string label = std::string (base) + " Tool";
        if (kind == "path")
        {
            label = "Pen Tool";
        }
        if (!e.commit (label.c_str (), err))
        {
            return false;
        }
        *result = Value{{"layer", id}};
        return true;
    }

    bool shape_layer (const Session& s, const Value& p, doc::LayerId* id, Error* err)
    {
        if (!layer_param (s, p, id, err))
        {
            return false;
        }
        const doc::Layer* l = s.active ()->doc->layer (*id);
        if (l == NULL)
        {
            return no_layer (err, *id);
        }
        if (l->kind != doc::LayerKind::Shape)
        {
            return fail (err, ErrorKind::Other, "\"%s\" is not a shape layer", l->name.c_str ());
        }
        return true;
    }

    /* Fill, stroke and the live shape's settings (the options bar and Properties). */
    bool cmd_shape_set_style (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "shape.setStyle";
        doc::LayerId id;
        if (!shape_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l       = e.doc ()->layer_mut (id);
        doc::ShapeLayer& sh = l->shape;
        if (!fill_param (p, &sh, cmd, err))
        {
            return false;
        }
        const Value* st = param (p, "stroke");
        if (st != NULL)
        {
            if (st->is_null ())
            {
                sh.stroke.enabled = false;
            }
            else if (!stroke_params (*st, &sh.stroke, cmd, err))
            {
                return false;
            }
        }
        if (sh.live.kind != doc::LiveKind::None)
        {
            double v;
            bool changed = false;
            if (param_f64 (p, "radius", &v))
            {
                sh.live.radius = fmax (v, 0.0);
                changed        = true;
            }
            int64_t n;
            if (param_i64 (p, "sides", &n))
            {
                sh.live.sides = (uint32_t) (n < 3 ? 3 : n > 100 ? 100 : n);
                changed       = true;
            }
            if (param_f64 (p, "star", &v))
            {
                sh.live.star = fmin (fmax (v, 0.0), 1.0);
                changed      = true;
            }
            if (param_f64 (p, "weight", &v))
            {
                sh.live.weight = fmin (fmax (v, 0.1), 1000.0);
                changed        = true;
            }
            if (rect4 (p, "rect", sh.live.rect))
            {
                changed = true;
            }
            if (changed)
            {
                sh.path = doc::live_path (sh.live);
            }
        }
        render_shape_layer (s, *e.doc (), l);
        return e.commit ("Edit Shape", err);
    }

    bool cmd_shape_get (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!shape_layer (s, p, &id, err))
        {
            return false;
        }
        const doc::ShapeLayer& sh = s.active ()->doc->layer (id)->shape;
        char c[8];
        hex_color (sh.fill, c);
        Value v = {
            {"path", path_json (sh.path)}, {"stroke", stroke_json (sh.stroke)}, {"live", LIVE_IDS[(int) sh.live.kind]}};
        if (sh.has_fill)
        {
            v["fill"] = c;
        }
        else
        {
            v["fill"] = nullptr;
        }
        if (sh.live.kind != doc::LiveKind::None)
        {
            v["rect"]   = {sh.live.rect[0], sh.live.rect[1], sh.live.rect[2], sh.live.rect[3]};
            v["radius"] = sh.live.radius;
            v["sides"]  = sh.live.sides;
            v["star"]   = sh.live.star;
            v["weight"] = sh.live.weight;
        }
        *result = v;
        return true;
    }

    bool cmd_rasterize_shape (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!shape_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        l->kind       = doc::LayerKind::Raster;
        l->shape      = doc::ShapeLayer::make ();
        return e.commit ("Rasterize Shape", err);
    }

    /* ---- Vector masks ---- */

    doc::VectorMask new_vector_mask (const doc::Path& p)
    {
        doc::VectorMask m;
        m.path    = p;
        m.enabled = true;
        m.linked  = true;
        m.density = 1.0f;
        m.feather = 0.0f;
        return m;
    }

    bool add_vector_mask (Session& s, const Value& p, const doc::Path& path, const char* label, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        doc::Layer* l    = d->layer_mut (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        if (l->has_vector_mask)
        {
            return fail (err, ErrorKind::Other, "\"%s\" already has a vector mask", l->name.c_str ());
        }
        if (d->is_background (id))
        {
            return fail (err, ErrorKind::Other, "the Background can't have a vector mask");
        }
        if (l->kind == doc::LayerKind::Shape)
        {
            return fail (err, ErrorKind::Other, "a shape layer's path is already its vector mask");
        }
        l->has_vector_mask = true;
        l->vector_mask     = new_vector_mask (path);
        render_vector_mask (*d, l);
        return e.commit (label, err);
    }

    bool cmd_vmask_reveal_all (Session& s, const Value& p, Value* result, Error* err)
    {
        return add_vector_mask (s, p, doc::Path::make (), "Add Vector Mask", err);
    }

    bool cmd_vmask_hide_all (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::Path path = doc::Path::make ();
        path.inverted  = true;
        return add_vector_mask (s, p, path, "Add Vector Mask", err);
    }

    /* Current Path: the work path (or a saved path) as the layer's vector mask. */
    bool cmd_vmask_current_path (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::Path path;
        Value src     = Value::object ();
        std::string t = "work";
        param_str (p, "target", &t);
        src["target"] = t;
        if (param_present (p, "index"))
        {
            src["index"] = p["index"];
        }
        if (!source_path (s, src, &path, "layer.vectorMask.currentPath", err))
        {
            return false;
        }
        return add_vector_mask (s, p, path, "Add Vector Mask", err);
    }

    bool vmask_layer (const Session& s, const Value& p, doc::LayerId* id, Error* err)
    {
        if (!layer_param (s, p, id, err))
        {
            return false;
        }
        const doc::Layer* l = s.active ()->doc->layer (*id);
        if (l == NULL)
        {
            return no_layer (err, *id);
        }
        if (!l->has_vector_mask)
        {
            return fail (err, ErrorKind::Other, "\"%s\" has no vector mask", l->name.c_str ());
        }
        return true;
    }

    bool cmd_vmask_delete (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!vmask_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l      = e.doc ()->layer_mut (id);
        l->has_vector_mask = false;
        l->vector_mask     = new_vector_mask (doc::Path::make ());
        return e.commit ("Delete Vector Mask", err);
    }

    bool toggle (const Value& p, const char* key, bool cur)
    {
        bool v = !cur;
        param_bool (p, key, &v);
        return v;
    }

    bool cmd_vmask_enabled (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!vmask_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l          = e.doc ()->layer_mut (id);
        l->vector_mask.enabled = toggle (p, "enabled", l->vector_mask.enabled);
        return e.commit (l->vector_mask.enabled ? "Enable Vector Mask" : "Disable Vector Mask", err);
    }

    bool cmd_vmask_linked (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!vmask_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l         = e.doc ()->layer_mut (id);
        l->vector_mask.linked = toggle (p, "linked", l->vector_mask.linked);
        return e.commit (l->vector_mask.linked ? "Link Vector Mask" : "Unlink Vector Mask", err);
    }

    /* Properties: density (percent) and feather (px). */
    bool cmd_vmask_props (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.vectorMask.props";
        doc::LayerId id;
        if (!vmask_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::VectorMask& m = e.doc ()->layer_mut (id)->vector_mask;
        double v;
        if (param_present (p, "density"))
        {
            if (!param_f64 (p, "density", &v) || !(v >= 0.0 && v <= 100.0))
            {
                return bad_params (err, cmd, "`density` must be 0..100");
            }
            m.density = (float) (v / 100.0);
        }
        if (param_present (p, "feather"))
        {
            if (!param_f64 (p, "feather", &v) || !(v >= 0.0 && v <= 1000.0))
            {
                return bad_params (err, cmd, "`feather` must be 0..1000 px");
            }
            m.feather = (float) v;
        }
        return e.commit ("Vector Mask Properties", err);
    }

    /* Rasterize Vector Mask: the mask's values (density and feather applied) become, or multiply
       into, the layer mask. */
    bool cmd_rasterize_vmask (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!vmask_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d  = e.doc ();
        doc::Layer* l     = d->layer_mut (id);
        geom::Rect canvas = d->bounds ();
        doc::LayerMask vm = doc::vector_mask_view (l->vector_mask);
        vm.enabled        = true;
        std::vector<float> v;
        if (vm.feather > 0.0f)
        {
            compose::feathered_mask (vm, canvas).read_region (canvas, &v);
        }
        else
        {
            vm.values (canvas, &v);
        }
        float outside[raster::MAX_CHANNELS];
        vm.surface.default_pixel (outside);
        float dflt = 1.0f - vm.density * (1.0f - outside[0]);
        if (l->has_mask)
        {
            std::vector<float> old;
            l->mask.surface.read_region (canvas, &old);
            size_t ch = l->mask.surface.channels ();
            for (size_t i = 0; i < v.size (); i++)
            {
                v[i] *= old[i * ch];
            }
            float od[raster::MAX_CHANNELS];
            l->mask.surface.default_pixel (od);
            dflt *= od[0];
        }
        else
        {
            l->has_mask = true;
            l->mask     = doc::LayerMask::reveal_all ();
        }
        raster::Surface out = raster::Surface::with_default (color::PixelFormat::gray8 (), &dflt);
        out.write_region (canvas, v.data (), v.size ());
        out.prune ();
        l->mask.surface    = out;
        l->has_vector_mask = false;
        l->vector_mask     = new_vector_mask (doc::Path::make ());
        return e.commit ("Rasterize Vector Mask", err);
    }

    /* ---- Enabled checks ---- */

    bool has_work_path (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (s.active ()->doc->has_work_path)
        {
            return true;
        }
        *why = "there is no work path";
        return false;
    }

    bool has_saved_paths (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (!s.active ()->doc->paths.empty ())
        {
            return true;
        }
        *why = "there are no saved paths";
        return false;
    }

    bool has_shape_layer (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l != NULL && l->kind == doc::LayerKind::Shape)
        {
            return true;
        }
        *why = "the active layer is not a shape layer";
        return false;
    }

    bool can_add_vmask (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l == NULL || l->has_vector_mask)
        {
            *why = "the layer already has a vector mask";
            return false;
        }
        if (d->doc->is_background (l->id))
        {
            *why = "the Background can't have a vector mask";
            return false;
        }
        if (l->kind == doc::LayerKind::Shape)
        {
            *why = "a shape layer's path is already its vector mask";
            return false;
        }
        return true;
    }

    bool can_add_vmask_path (const Session& s, std::string* why)
    {
        if (!can_add_vmask (s, why))
        {
            return false;
        }
        const doc::Document& d = *s.active ()->doc;
        if (d.has_work_path || !d.paths.empty ())
        {
            return true;
        }
        *why = "there is no path";
        return false;
    }

    bool has_vmask (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l != NULL && l->has_vector_mask)
        {
            return true;
        }
        *why = "the layer has no vector mask";
        return false;
    }

    const CommandSpec VECTOR_COMMANDS[] = {
        {"path.set", "Edit Path", "", NULL,
            "{\"target\":\"work|saved|shape|vectorMask\"=\"work\",\"index\":n?,\"layer\":id?,\"path\":{evenOdd, "
            "inverted, subpaths:[{closed, op, knots:[[x,y,inX,inY,outX,outY,smooth]]}]},"
            "\"label\":str?,\"keepLive\":bool=false}",
            has_doc, cmd_path_set, true},
        {"path.get", "Path", "", NULL, "{\"target\":…,\"index\":n?,\"layer\":id?} → the path", has_doc, cmd_path_get,
            false},
        {"path.list", "Paths", "", NULL, "{} → {paths:[{name, subpaths}], work:bool}", has_doc, cmd_path_list, false},
        {"path.save", "Save Path…", "", NULL, "{\"name\":str?}", has_work_path, cmd_path_save, true},
        {"path.new", "New Path…", "", NULL, "{\"name\":str?}", has_doc, cmd_path_new, true},
        {"path.delete", "Delete Path", "", NULL, "{\"target\":\"work|saved\",\"index\":n?}", has_doc, cmd_path_delete,
            true},
        {"path.rename", "Rename Path", "", NULL, "{\"index\":n,\"name\":str}", has_saved_paths, cmd_path_rename, true},
        {"path.duplicate", "Duplicate Path", "", NULL, "{\"index\":n,\"name\":str?}", has_saved_paths,
            cmd_path_duplicate, true},
        {"path.makeSelection", "Make Selection…", "", NULL,
            "{\"target\":…|\"path\":{…},\"feather\":px=0,\"antialias\":bool=true,\"mode\":\"replace|add|subtract|"
            "intersect\"=\"replace\"}",
            has_doc, cmd_path_make_selection, true},
        {"path.fill", "Fill Path…", "", NULL,
            "{\"target\":…|\"path\":{…},\"layer\":id?,\"contents\":\"foreground|background\",\"color\":\"#rrggbb\"?,"
            "\"mode\":blend,\"opacity\":0..100,"
            "\"feather\":px=0,\"antialias\":bool=true,\"preserveTransparency\":bool=false}",
            has_doc, cmd_path_fill, true},
        {"path.stroke", "Stroke Path…", "", NULL,
            "{\"target\":…|\"path\":{…},\"layer\":id?,\"width\":px=brush "
            "size,\"color\":\"#rrggbb\"=foreground,\"cap\":str,\"join\":str,\"dashes\":[…],"
            "\"mode\":blend,\"opacity\":0..100}",
            has_doc, cmd_path_stroke, true},
        {"path.fromSelection", "Make Work Path…", "", NULL, "{\"tolerance\":0.5..10=2}", has_selection,
            cmd_path_from_selection, true},
        {"shape.create", "Shape Layer", "", NULL,
            "{\"kind\":\"rect|ellipse|polygon|line|path\",\"rect\":[x0,y0,x1,y1],\"radius\":px,\"sides\":3..100,"
            "\"star\":0..1,\"weight\":px,\"path\":{…}?,"
            "\"fill\":\"#rrggbb\"|null=foreground,\"stroke\":{enabled,width,color,opacity,align,cap,join,miterLimit,"
            "dashes}?,\"name\":str?}",
            has_doc, cmd_shape_create, true},
        {"shape.setStyle", "Edit Shape", "", NULL,
            "{\"layer\":id?,\"fill\":\"#rrggbb\"|null,\"stroke\":{…}|null,\"radius\":px,\"sides\":n,\"star\":0..1,"
            "\"weight\":px,\"rect\":[x0,y0,x1,y1]}",
            has_shape_layer, cmd_shape_set_style, true},
        {"shape.get", "Shape", "", NULL, "{\"layer\":id?} → {path, fill, stroke, live, rect?, radius?, …}",
            has_shape_layer, cmd_shape_get, false},
        {"layer.rasterize.shape", "Shape", "Layer/Rasterize", NULL, "{\"layer\":id?}", has_shape_layer,
            cmd_rasterize_shape, true},
        {"layer.vectorMask.revealAll", "Reveal All", "Layer/Vector Mask", NULL, "{\"layer\":id?}", can_add_vmask,
            cmd_vmask_reveal_all, true},
        {"layer.vectorMask.hideAll", "Hide All", "Layer/Vector Mask", NULL, "{\"layer\":id?}", can_add_vmask,
            cmd_vmask_hide_all, true},
        {"layer.vectorMask.currentPath", "Current Path", "Layer/Vector Mask", NULL,
            "{\"layer\":id?,\"target\":\"work|saved\"=\"work\",\"index\":n?}", can_add_vmask_path,
            cmd_vmask_current_path, true},
        {"layer.vectorMask.delete", "Delete", "Layer/Vector Mask", NULL, "{\"layer\":id?}", has_vmask, cmd_vmask_delete,
            true},
        {"layer.vectorMask.enabled", "Disable", "Layer/Vector Mask", NULL,
            "{\"layer\":id?,\"enabled\":bool? (default: toggle)}", has_vmask, cmd_vmask_enabled, true},
        {"layer.vectorMask.linked", "Unlink Vector Mask", "", NULL,
            "{\"layer\":id?,\"linked\":bool? (default: toggle)}", has_vmask, cmd_vmask_linked, true},
        {"layer.vectorMask.props", "Vector Mask Properties", "", NULL,
            "{\"layer\":id?,\"density\":0..100,\"feather\":px}", has_vmask, cmd_vmask_props, true},
        {"layer.rasterize.vectorMask", "Vector Mask", "Layer/Rasterize", NULL, "{\"layer\":id?}", has_vmask,
            cmd_rasterize_vmask, true},
    };

    void register_vector_commands (std::vector<CommandSpec>* out)
    {
        out->insert (
            out->end (), VECTOR_COMMANDS, VECTOR_COMMANDS + sizeof VECTOR_COMMANDS / sizeof VECTOR_COMMANDS[0]);
    }

} // namespace engine
