#include "algo/transform.h"
#include "engine/brush.h"
#include "engine/layers.h"
#include "engine/session.h"
#include "engine/smart.h"
#include "engine/text.h"
#include "engine/transform.h"
#include "engine/vector.h"
#include <math.h>
#include <string.h>

namespace engine
{

    /* Edit > Free Transform and Edit > Transform: a layer (with its linked mask), or with a
       selection just the selected pixels, mapped by a homography given as where the corners of a
       source rectangle land ("quad") or as an affine "matrix". An unlinked mask targeted with
       "target": "mask" transforms by itself. */

    using algo::Homography;
    using algo::Interp;
    using geom::Rect;

    /* Layers a transform moves whole even with a selection (groups and type; shapes and smart
       objects once ported). */
    bool moves_whole (const doc::Layer& l)
    {
        return l.is_group () || l.kind == doc::LayerKind::Text || l.kind == doc::LayerKind::Shape
            || l.kind == doc::LayerKind::Smart;
    }

    /* The frame Free Transform starts from: the layer's content within the selection (all of it
       for layers that move whole); a layer with no pixels frames its mask. */
    Rect transform_bounds (const doc::Document& d, const doc::Layer& l)
    {
        Rect content;
        if (l.is_group ())
        {
            for (size_t i = 0; i < l.children.size (); i++)
            {
                content = content.unite (transform_bounds (d, l.children[i]));
            }
        }
        else if (l.surface () != NULL)
        {
            content = l.surface ()->content_bounds ();
        }
        if (content.is_empty () && l.has_mask)
        {
            content = l.mask.surface.content_bounds ();
        }
        if (d.has_selection && !moves_whole (l))
        {
            content = content.intersect (d.selection.content_bounds ());
        }
        return content;
    }

    /* The frame for a lone mask: its painted content (the canvas when nothing is painted), within
       the selection. */
    Rect target_bounds (const doc::Document& d, const raster::Surface& surf)
    {
        Rect content = surf.content_bounds ();
        if (content.is_empty ())
        {
            content = d.bounds ();
        }
        if (d.has_selection)
        {
            content = content.intersect (d.selection.content_bounds ());
        }
        return content;
    }

    /* The locks inherited from the groups around `id`. */
    doc::Locks group_locks (const doc::Document& d, doc::LayerId id)
    {
        doc::Locks out = {false, false, false, false, false};
        doc::LayerPath path;
        if (!d.path_of (id, &path))
        {
            return out;
        }
        for (size_t n = 1; n < path.size (); n++)
        {
            doc::LayerPath prefix (path.begin (), path.begin () + (ptrdiff_t) n);
            const doc::Layer* l = d.layer_at (prefix);
            if (l != NULL)
            {
                out = out.unite (l->locks);
            }
        }
        return out;
    }

    /* A layer's own locks with the inherited ones; the Background's position lock doesn't count
       (it becomes a normal layer first, as in Photoshop). */
    doc::Locks transform_locks (const doc::Layer& l, doc::Locks group, bool background)
    {
        doc::Locks own = l.locks;
        if (background)
        {
            own.position     = false;
            own.transparency = false;
        }
        return own.unite (group);
    }

    bool transform_layer (
        const raster::Surface* sel, doc::Locks group, doc::Layer* l, const Homography& h, Interp interp, Error* err)
    {
        doc::Locks locks = transform_locks (*l, group, false);
        if (locks.position || locks.all)
        {
            return fail (err, ErrorKind::Other, "layer \"%s\" is locked", l->name.c_str ());
        }
        if (l->is_group ())
        {
            for (size_t i = 0; i < l->children.size (); i++)
            {
                if (!transform_layer (NULL, locks, &l->children[i], h, interp, err))
                {
                    return false;
                }
            }
        }
        if (h.is_identity ())
        {
            return true;
        }
        if (l->kind == doc::LayerKind::Text)
        {
            /* Type stays editable: the transform joins its placement (re-rendered afterwards). */
            if (!h.is_affine ())
            {
                return fail (err, ErrorKind::Other,
                    "type layers can't take a perspective or distortion; rasterize \"%s\" first", l->name.c_str ());
            }
            double m[6] = {h.m[0], h.m[3], h.m[1], h.m[4], h.m[2], h.m[5]};
            double* t   = l->text.transform;
            double r[6] = {m[0] * t[0] + m[2] * t[1], m[1] * t[0] + m[3] * t[1], m[0] * t[2] + m[2] * t[3],
                m[1] * t[2] + m[3] * t[3], m[0] * t[4] + m[2] * t[5] + m[4], m[1] * t[4] + m[3] * t[5] + m[5]};
            memcpy (t, r, sizeof r);
            if (l->has_mask && l->mask.linked)
            {
                l->mask.surface = algo::warp_gray (l->mask.surface, h, interp);
            }
            if (l->has_vector_mask && l->vector_mask.linked)
            {
                map_path (&l->vector_mask.path, h);
            }
            return true;
        }
        /* A linked vector mask and a shape's path take the transform exactly (redrawn afterwards). */
        if (l->has_vector_mask && l->vector_mask.linked)
        {
            map_path (&l->vector_mask.path, h);
        }
        if (l->kind == doc::LayerKind::Smart)
        {
            /* The corners move; the contents are rendered there afresh (no loss). */
            map_smart (&l->smart, h);
            if (l->smart.warped)
            {
                memset (l->smart.rendered, 0, sizeof l->smart.rendered);
            }
            if (l->has_mask && l->mask.linked)
            {
                l->mask.surface = algo::warp_gray (l->mask.surface, h, interp);
            }
            return true;
        }
        if (l->kind == doc::LayerKind::Shape)
        {
            map_path (&l->shape.path, h);
            if (h.m[1] != 0.0 || h.m[3] != 0.0 || h.m[6] != 0.0 || h.m[7] != 0.0)
            {
                l->shape.live.kind = doc::LiveKind::None;
            }
            else if (l->shape.live.kind != doc::LiveKind::None)
            {
                double* r = l->shape.live.rect;
                h.apply (r[0], r[1], &r[0], &r[1]);
                h.apply (r[2], r[3], &r[2], &r[3]);
            }
            if (l->has_mask && l->mask.linked)
            {
                l->mask.surface = algo::warp_gray (l->mask.surface, h, interp);
            }
            return true;
        }
        const raster::Surface* mask_sel = moves_whole (*l) ? NULL : sel;
        raster::Surface* surf           = l->surface_mut ();
        if (surf != NULL)
        {
            Rect src = surf->content_bounds ();
            if (sel != NULL)
            {
                raster::Surface lifted, rest;
                algo::split_selected (*surf, *sel, &lifted, &rest);
                raster::Surface moved = algo::warp_surface (lifted, src, h, interp);
                algo::composite_over (&rest, moved);
                rest.prune ();
                *surf = rest;
            }
            else
            {
                *surf = algo::warp_surface (*surf, src, h, interp);
            }
        }
        if (l->has_mask && l->mask.linked)
        {
            l->mask.surface = mask_sel != NULL ? algo::warp_gray_selected (l->mask.surface, *mask_sel, h, interp)
                                               : algo::warp_gray (l->mask.surface, h, interp);
        }
        return true;
    }

    bool read_quad (const Value& p, double q[4][2])
    {
        const Value* a = param (p, "quad");
        if (a == NULL || !a->is_array () || a->size () != 4)
        {
            return false;
        }
        for (size_t i = 0; i < 4; i++)
        {
            const Value& c = (*a)[i];
            if (!c.is_array () || c.size () < 2 || !c[0].is_number () || !c[1].is_number ())
            {
                return false;
            }
            q[i][0] = c[0].get<double> ();
            q[i][1] = c[1].get<double> ();
        }
        return true;
    }

    /* The transform of `p` applied to layer `id` (or its unlinked mask) of the working document.
       `rect` receives the source frame. */
    bool apply_transform (Session& s, doc::Document* d, doc::LayerId id, const Value& p, double rect[4], Error* err)
    {
        const char* cmd    = "edit.transform";
        doc::Layer* target = d->layer_mut (id);
        if (target == NULL)
        {
            return no_layer (err, id);
        }
        bool lone      = is_mask_target (p) && target->has_mask && !target->mask.linked;
        const Value* r = param (p, "rect");
        if (r != NULL && r->is_array () && r->size () == 4)
        {
            for (size_t i = 0; i < 4; i++)
            {
                rect[i] = (*r)[i].is_number () ? (*r)[i].get<double> () : 0.0;
            }
        }
        else
        {
            Rect b = lone ? target_bounds (*d, target->mask.surface) : transform_bounds (*d, *target);
            if (b.is_empty ())
            {
                return fail (err, ErrorKind::Other, "nothing to transform");
            }
            rect[0] = b.x0;
            rect[1] = b.y0;
            rect[2] = b.x1;
            rect[3] = b.y1;
        }
        Homography h;
        double q[4][2];
        const Value* m = param (p, "matrix");
        if (read_quad (p, q))
        {
            if (!Homography::rect_to_quad (rect, q, &h))
            {
                return bad_params (err, cmd, "degenerate quad");
            }
        }
        else if (m != NULL)
        {
            double v[6];
            if (!m->is_array () || m->size () != 6)
            {
                return bad_params (err, cmd, "matrix must be [a, b, c, d, e, f]");
            }
            for (size_t i = 0; i < 6; i++)
            {
                if (!(*m)[i].is_number ())
                {
                    return bad_params (err, cmd, "matrix must be [a, b, c, d, e, f]");
                }
                v[i] = (*m)[i].get<double> ();
            }
            h = Homography::affine (v[0], v[1], v[2], v[3], v[4], v[5]);
        }
        else
        {
            return bad_params (err, cmd, "pass `quad` (where the rect's corners go) or `matrix`");
        }
        Homography inv;
        if (!h.inverse (&inv))
        {
            return bad_params (err, cmd, "the transform collapses the layer");
        }
        std::string ip = "bicubic";
        param_str (p, "interpolation", &ip);
        Interp interp              = algo::interp_from_str (ip.c_str ());
        const raster::Surface* sel = d->has_selection ? &d->selection : NULL;
        if (lone)
        {
            if (d->effective_locks (id).all)
            {
                return fail (err, ErrorKind::Other, "layer \"%s\" is locked", target->name.c_str ());
            }
            if (!h.is_identity ())
            {
                target->mask.surface = sel != NULL ? algo::warp_gray_selected (target->mask.surface, *sel, h, interp)
                                                   : algo::warp_gray (target->mask.surface, h, interp);
            }
        }
        else
        {
            bool is_group    = target->is_group ();
            doc::Locks group = group_locks (*d, id);
            background_to_layer (d, id);
            if (!transform_layer (is_group ? NULL : sel, group, d->layer_mut (id), h, interp, err))
            {
                return false;
            }
            render_all_live (s, d);
        }
        /* The selection outline moves with the pixels. */
        if (d->has_selection && !h.is_identity ())
        {
            d->selection = algo::warp_gray (d->selection, h, Interp::Bilinear);
            if (d->selection.content_bounds ().is_empty ())
            {
                d->has_selection = false;
                d->selection     = raster::Surface (color::PixelFormat::gray8 ());
            }
        }
        return true;
    }

    bool cmd_edit_transform (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        double rect[4];
        if (!apply_transform (s, e.doc (), id, p, rect, err))
        {
            return false;
        }
        if (!e.commit ("Free Transform", err))
        {
            return false;
        }
        *result = Value{{"rect", {rect[0], rect[1], rect[2], rect[3]}}};
        return true;
    }

    /* Rotate 180° / 90° and Flip: exact pixel permutations about the frame's centre (integer offsets
       and nearest sampling keep them on the grid). */
    bool transform_preset (Session& s, const Value& p, const char* kind, Value* result, Error* err)
    {
        const DocState* st = s.active ();
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        const doc::Layer* l = st->doc->layer (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        bool lone = is_mask_target (p) && l->has_mask && !l->mask.linked;
        Rect b    = lone ? target_bounds (*st->doc, l->mask.surface) : transform_bounds (*st->doc, *l);
        if (b.is_empty ())
        {
            return fail (err, ErrorKind::Other, "nothing to transform");
        }
        double sx = (double) b.x0 + b.x1, sy = (double) b.y0 + b.y1, cx = sx / 2.0, cy = sy / 2.0;
        double m[6];
        if (strcmp (kind, "rotate180") == 0)
        {
            double v[6] = {-1, 0, 0, -1, sx, sy};
            memcpy (m, v, sizeof m);
        }
        else if (strcmp (kind, "rotate90Cw") == 0)
        {
            double v[6] = {0, 1, -1, 0, round (cx + cy), round (cy - cx)};
            memcpy (m, v, sizeof m);
        }
        else if (strcmp (kind, "rotate90Ccw") == 0)
        {
            double v[6] = {0, -1, 1, 0, round (cx - cy), round (cx + cy)};
            memcpy (m, v, sizeof m);
        }
        else if (strcmp (kind, "flipHorizontal") == 0)
        {
            double v[6] = {-1, 0, 0, 1, sx, 0};
            memcpy (m, v, sizeof m);
        }
        else
        {
            double v[6] = {1, 0, 0, -1, 0, sy};
            memcpy (m, v, sizeof m);
        }
        Value params = {{"layer", id}, {"matrix", {m[0], m[1], m[2], m[3], m[4], m[5]}}, {"interpolation", "nearest"}};
        if (lone)
        {
            params["target"] = "mask";
        }
        return s.execute ("edit.transform", params, result, err);
    }

    bool cmd_rotate180 (Session& s, const Value& p, Value* result, Error* err)
    {
        return transform_preset (s, p, "rotate180", result, err);
    }

    bool cmd_rotate90cw (Session& s, const Value& p, Value* result, Error* err)
    {
        return transform_preset (s, p, "rotate90Cw", result, err);
    }

    bool cmd_rotate90ccw (Session& s, const Value& p, Value* result, Error* err)
    {
        return transform_preset (s, p, "rotate90Ccw", result, err);
    }

    bool cmd_flip_h (Session& s, const Value& p, Value* result, Error* err)
    {
        return transform_preset (s, p, "flipHorizontal", result, err);
    }

    bool cmd_flip_v (Session& s, const Value& p, Value* result, Error* err)
    {
        return transform_preset (s, p, "flipVertical", result, err);
    }

    /* The edit.transform params that repeat the last transform on the active layer: a quad
       transform maps the layer's current frame the same way; a matrix repeats as it is. */
    bool transform_again_params (const Session& s, Value* out, Error* err)
    {
        const Value* last = NULL;
        for (size_t i = s.journal.size (); i-- > 0;)
        {
            if (s.journal[i].id == "edit.transform")
            {
                last = &s.journal[i].params;
                break;
            }
        }
        if (last == NULL)
        {
            return fail (err, ErrorKind::Other, "there is no transform to repeat");
        }
        std::string interp = "bicubic";
        param_str (*last, "interpolation", &interp);
        *out = Value{{"interpolation", interp}};
        if (is_mask_target (*last))
        {
            (*out)["target"] = "mask";
        }
        double q[4][2];
        const Value* r = param (*last, "rect");
        const Value* m = param (*last, "matrix");
        if (r != NULL && read_quad (*last, q))
        {
            double rr[4];
            for (size_t i = 0; i < 4 && i < r->size (); i++)
            {
                rr[i] = (*r)[i].is_number () ? (*r)[i].get<double> () : 0.0;
            }
            Homography h;
            if (r->size () != 4 || !Homography::rect_to_quad (rr, q, &h))
            {
                return fail (err, ErrorKind::Other, "the last transform can't be repeated");
            }
            const DocState* st  = s.active ();
            const doc::Layer* l = st->doc->layer (st->active_layer);
            if (l == NULL)
            {
                return fail (err, ErrorKind::Other, "no active layer");
            }
            Rect b = transform_bounds (*st->doc, *l);
            if (b.is_empty ())
            {
                return fail (err, ErrorKind::Other, "nothing to transform");
            }
            double nr[4] = {(double) b.x0, (double) b.y0, (double) b.x1, (double) b.y1};
            double xs[4] = {nr[0], nr[2], nr[2], nr[0]}, ys[4] = {nr[1], nr[1], nr[3], nr[3]};
            Value quad = Value::array ();
            for (int i = 0; i < 4; i++)
            {
                double x, y;
                h.apply (xs[i], ys[i], &x, &y);
                quad.push_back ({x, y});
            }
            (*out)["rect"] = {nr[0], nr[1], nr[2], nr[3]};
            (*out)["quad"] = quad;
        }
        else if (m != NULL)
        {
            (*out)["matrix"] = *m;
        }
        else
        {
            return fail (err, ErrorKind::Other, "the last transform can't be repeated");
        }
        return true;
    }

    bool cmd_transform_again (Session& s, const Value& p, Value* result, Error* err)
    {
        Value params;
        if (!transform_again_params (s, &params, err))
        {
            return false;
        }
        return s.execute ("edit.transform", params, result, err);
    }

    /* Transform Again on a copy (step and repeat): the active layer duplicated and the copy
       transformed, as one history step. */
    bool cmd_transform_again_copy (Session& s, const Value& p, Value* result, Error* err)
    {
        Value params;
        if (!transform_again_params (s, &params, err))
        {
            return false;
        }
        doc::LayerId id = s.active ()->active_layer;
        Edit e (s);
        doc::Document* d      = e.doc ();
        const doc::Layer* src = d->layer (id);
        if (src == NULL)
        {
            return fail (err, ErrorKind::Other, "no active layer");
        }
        doc::LayerId nid = d->insert_above (id, layer_copy (s.ids, *d, *src));
        double rect[4];
        if (!apply_transform (s, d, nid, params, rect, err))
        {
            return false;
        }
        *e.active_layer () = nid;
        if (!e.commit ("Transform Again", err))
        {
            return false;
        }
        *result = Value{{"layer", nid}};
        return true;
    }

    bool can_transform (const Session& s, std::string* why)
    {
        return has_layer (s, why);
    }

    const char TRANSFORM_TARGET[] = "{\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\"}";

    const CommandSpec TRANSFORM_COMMANDS[] = {
        {"edit.transform", "Free Transform", "", NULL,
            "{\"layer\":id?,\"rect\":[x0,y0,x1,y1]? (source frame; default = layer content ∩ "
            "selection),\"quad\":[[x,y]×4]? (where the frame's corners go, "
            "clockwise from top-left),\"matrix\":[a,b,c,d,e,f]? (affine "
            "alternative),\"interpolation\":\"bicubic|bilinear|nearest\"=\"bicubic\","
            "\"target\":\"pixels|mask\"=\"pixels\" (an unlinked mask transforms alone)}",
            can_transform, cmd_edit_transform, true},
        {"edit.transform.again", "Again", "Edit/Transform", "Cmd+Shift+T", "{}", has_doc, cmd_transform_again, true},
        {"edit.transform.againCopy", "Transform Again on a Copy", "", "Cmd+Alt+Shift+T", "{}", has_doc,
            cmd_transform_again_copy, false},
        {"edit.transform.rotate180", "Rotate 180°", "Edit/Transform", NULL, TRANSFORM_TARGET, can_transform,
            cmd_rotate180, true},
        {"edit.transform.rotate90Cw", "Rotate 90° Clockwise", "Edit/Transform", NULL, TRANSFORM_TARGET, can_transform,
            cmd_rotate90cw, true},
        {"edit.transform.rotate90Ccw", "Rotate 90° Counter Clockwise", "Edit/Transform", NULL, TRANSFORM_TARGET,
            can_transform, cmd_rotate90ccw, true},
        {"edit.transform.flipHorizontal", "Flip Horizontal", "Edit/Transform", NULL, TRANSFORM_TARGET, can_transform,
            cmd_flip_h, true},
        {"edit.transform.flipVertical", "Flip Vertical", "Edit/Transform", NULL, TRANSFORM_TARGET, can_transform,
            cmd_flip_v, true},
    };

    void register_transform_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), TRANSFORM_COMMANDS,
            TRANSFORM_COMMANDS + sizeof TRANSFORM_COMMANDS / sizeof TRANSFORM_COMMANDS[0]);
    }

} // namespace engine
