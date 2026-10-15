#include "algo/deform.h"
#include "algo/transform.h"
#include "engine/layers.h"
#include "engine/session.h"
#include "engine/smart.h"
#include "engine/transform.h"
#include "engine/vector.h"
#include <algorithm>
#include <math.h>
#include <string.h>

namespace engine
{

    /* Edit > Transform > Warp, Puppet Warp, Perspective Warp and Content-Aware Scale. Warp, Puppet
       and Perspective Warp are forward maps of the plane (algo/deform.cpp) applied to the layer's
       pixels (or the selected part of them), its linked mask, a shape layer's path, or, for Warp
       and Perspective Warp, a smart object's placement (kept editable). Type layers ask to be
       rasterized (Warp Text is the type's own warp). */

    using geom::Rect;

    /* 16 control points, row by row: [[x, y], …]. */
    bool read_points (const Value& p, const char* key, double out[][2], size_t n, const char* cmd, Error* err)
    {
        const Value* a = param (p, key);
        if (a == NULL || !a->is_array () || a->size () != n)
        {
            return bad_params (err, cmd, "`%s` must be %zu points [x, y]", key, n);
        }
        for (size_t i = 0; i < n; i++)
        {
            const Value& v = (*a)[i];
            if (!v.is_array () || v.size () < 2 || !v[0].is_number () || !v[1].is_number ())
            {
                return bad_params (err, cmd, "`%s` must be %zu points [x, y]", key, n);
            }
            out[i][0] = v[0].get<double> ();
            out[i][1] = v[1].get<double> ();
        }
        return true;
    }

    /* The frame a deformation is laid over: "rect", else the layer's content (within the
       selection). */
    bool deform_frame (const doc::Document& d, const doc::Layer& l, const Value& p, double rect[4])
    {
        const Value* r = param (p, "rect");
        if (r != NULL && r->is_array () && r->size () == 4)
        {
            for (size_t i = 0; i < 4; i++)
            {
                rect[i] = (*r)[i].is_number () ? (*r)[i].get<double> () : 0.0;
            }
            return rect[2] > rect[0] && rect[3] > rect[1];
        }
        Rect b = transform_bounds (d, l);
        if (b.is_empty ())
        {
            return false;
        }
        rect[0] = b.x0;
        rect[1] = b.y0;
        rect[2] = b.x1;
        rect[3] = b.y1;
        return true;
    }

    void map_path_deform (doc::Path* path, const algo::Deform& f)
    {
        for (size_t i = 0; i < path->subpaths.size (); i++)
        {
            for (size_t k = 0; k < path->subpaths[i].knots.size (); k++)
            {
                doc::Knot& n = path->subpaths[i].knots[k];
                algo::deform_point (f, n.anchor[0], n.anchor[1], &n.anchor[0], &n.anchor[1]);
                algo::deform_point (f, n.in_ctrl[0], n.in_ctrl[1], &n.in_ctrl[0], &n.in_ctrl[1]);
                algo::deform_point (f, n.out_ctrl[0], n.out_ctrl[1], &n.out_ctrl[0], &n.out_ctrl[1]);
            }
        }
    }

    /* A smart object's contents placed through f after where they are now: the new placement fitted
       as a Bezier patch over the contents. */
    void deform_smart (const doc::Document& d, doc::Layer* l, const algo::Deform& f)
    {
        const doc::SmartContents* c = d.smart_contents (l->smart.uuid);
        if (c == NULL || c->width == 0)
        {
            return;
        }
        double cr[4]     = {0, 0, (double) c->width, (double) c->height};
        algo::Deform now = algo::Deform::patch_identity (cr);
        algo::Homography h;
        if (l->smart.warped)
        {
            memcpy (now.pts, l->smart.warp, sizeof now.pts);
        }
        else
        {
            now.kind = algo::DeformKind::Homography;
            algo::Homography::rect_to_quad (cr, l->smart.quad, &now.h);
        }
        const int S = 9;
        std::vector<double> t ((size_t) S * S * 2);
        for (int j = 0; j < S; j++)
        {
            for (int i = 0; i < S; i++)
            {
                double x, y;
                algo::deform_point (now, cr[2] * i / (S - 1), cr[3] * j / (S - 1), &x, &y);
                algo::deform_point (f, x, y, &t[((size_t) j * S + i) * 2], &t[((size_t) j * S + i) * 2 + 1]);
            }
        }
        algo::fit_patch (cr, t, S, l->smart.warp);
        l->smart.warped = true;
        /* The corners follow, for whatever reads them. */
        for (int k = 0; k < 4; k++)
        {
            const int idx[4]    = {0, 3, 15, 12};
            l->smart.quad[k][0] = l->smart.warp[idx[k]][0];
            l->smart.quad[k][1] = l->smart.warp[idx[k]][1];
        }
    }

    /* The deformation applied to layer `id`. */
    bool deform_layer (Session& s, doc::Document* d, doc::LayerId id, const algo::Deform& f, algo::Interp interp,
        bool smart_ok, const char* what, Error* err)
    {
        if (d->layer (id) == NULL)
        {
            return no_layer (err, id);
        }
        /* The Background becomes a normal layer first, as in Photoshop. */
        if (d->is_background (id))
        {
            background_to_layer (d, id);
        }
        doc::Layer* l    = d->layer_mut (id);
        doc::Locks locks = d->effective_locks (id);
        if (locks.position || locks.all)
        {
            return fail (err, ErrorKind::Other, "layer \"%s\" is locked", l->name.c_str ());
        }
        if (l->kind == doc::LayerKind::Text)
        {
            return fail (err, ErrorKind::Other, "type layers can't take %s; rasterize \"%s\" first (or use Warp Text)",
                what, l->name.c_str ());
        }
        if (l->is_group () || l->kind == doc::LayerKind::Adjustment || l->kind == doc::LayerKind::Fill)
        {
            return fail (err, ErrorKind::Other, "%s works on pixel, shape and smart object layers", what);
        }
        background_to_layer (d, id);
        l          = d->layer_mut (id);
        Rect frame = {(int32_t) floor (f.rect[0]), (int32_t) floor (f.rect[1]), (int32_t) ceil (f.rect[2]),
            (int32_t) ceil (f.rect[3])};
        if (l->has_mask && l->mask.linked)
        {
            l->mask.surface =
                algo::mesh_warp (l->mask.surface, l->mask.surface.content_bounds ().unite (frame), f, interp);
        }
        if (l->has_vector_mask && l->vector_mask.linked)
        {
            map_path_deform (&l->vector_mask.path, f);
            render_vector_mask (*d, l);
        }
        if (l->kind == doc::LayerKind::Shape)
        {
            map_path_deform (&l->shape.path, f);
            l->shape.live.kind = doc::LiveKind::None;
            render_shape_layer (s, *d, l);
            return true;
        }
        if (l->kind == doc::LayerKind::Smart)
        {
            if (!smart_ok)
            {
                return fail (err, ErrorKind::Other, "%s on a smart object isn't supported yet; rasterize \"%s\" first",
                    what, l->name.c_str ());
            }
            deform_smart (*d, l, f);
            render_smart (s, *d, l);
            return true;
        }
        raster::Surface* surf = l->surface_mut ();
        if (surf == NULL)
        {
            return fail (err, ErrorKind::Other, "%s works on pixel layers", what);
        }
        Rect area = surf->content_bounds ();
        if (d->has_selection)
        {
            /* The selected part is lifted, deformed and laid back over the rest. */
            raster::Surface lifted, rest;
            algo::split_selected (*surf, d->selection, &lifted, &rest);
            raster::Surface moved = algo::mesh_warp (lifted, lifted.content_bounds (), f, interp);
            algo::composite_over (&rest, moved);
            rest.prune ();
            *surf = rest;
        }
        else
        {
            *surf = algo::mesh_warp (*surf, area, f, interp);
        }
        return true;
    }

    algo::Interp interp_param (const Value& p)
    {
        std::string ip = "bicubic";
        param_str (p, "interpolation", &ip);
        algo::Interp i = algo::interp_from_str (ip.c_str ());
        /* The mesh renderer samples bilinearly at most. */
        return i == algo::Interp::Nearest ? i : algo::Interp::Bilinear;
    }

    /* Warp: a 4 x 4 Bezier control net ("points", row by row, where the frame's grid points go),
       or a preset "style" with bend and distortions (percent). */
    bool cmd_warp_on (Session& s, doc::Document* d, const Value& p, Error* err)
    {
        const char* cmd = "edit.warp";
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        const doc::Layer* l = d->layer (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        double rect[4];
        if (!deform_frame (*d, *l, p, rect))
        {
            return fail (err, ErrorKind::Other, "nothing to warp");
        }
        algo::Deform f = algo::Deform::patch_identity (rect);
        std::string style;
        if (param_str (p, "style", &style))
        {
            if (!algo::warp_style_from_id (style.c_str (), &f.style))
            {
                return bad_params (err, cmd, "unknown warp style `%s`", style.c_str ());
            }
            f.kind   = algo::DeformKind::Style;
            double v = 50;
            param_f64 (p, "bend", &v);
            f.bend = fmax (fmin (v, 100.0), -100.0) / 100.0;
            v      = 0;
            param_f64 (p, "horizontal", &v);
            f.hdist = fmax (fmin (v, 100.0), -100.0) / 100.0;
            v       = 0;
            param_f64 (p, "vertical", &v);
            f.vdist       = fmax (fmin (v, 100.0), -100.0) / 100.0;
            std::string o = "horizontal";
            param_str (p, "orientation", &o);
            f.vertical = o == "vertical";
        }
        else if (!read_points (p, "points", f.pts, 16, cmd, err))
        {
            return false;
        }
        return deform_layer (s, d, id, f, interp_param (p), true, "Warp", err);
    }

    bool cmd_warp (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        if (e.doc () == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!cmd_warp_on (s, e.doc (), p, err))
        {
            return false;
        }
        return e.commit ("Warp", err);
    }

    /* Puppet Warp: pins [[x, y, toX, toY], …]; the image around them moves as rigidly (Rigid),
       naturally (Normal) or freely (Distort) as the pins allow. */
    bool cmd_puppet_warp_on (Session& s, doc::Document* d, const Value& p, Error* err)
    {
        const char* cmd = "edit.puppetWarp";
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        const doc::Layer* l = d->layer (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        double rect[4];
        if (!deform_frame (*d, *l, p, rect))
        {
            return fail (err, ErrorKind::Other, "nothing to warp");
        }
        algo::Deform f    = algo::Deform::patch_identity (rect);
        f.kind            = algo::DeformKind::Pins;
        const Value* pins = param (p, "pins");
        if (pins == NULL || !pins->is_array ())
        {
            return bad_params (err, cmd, "`pins` must be [[x, y, toX, toY], …]");
        }
        for (size_t i = 0; i < pins->size (); i++)
        {
            const Value& v = (*pins)[i];
            if (!v.is_array () || v.size () != 4)
            {
                return bad_params (err, cmd, "a pin is [x, y, toX, toY]");
            }
            for (int k = 0; k < 4; k++)
            {
                if (!v[(size_t) k].is_number ())
                {
                    return bad_params (err, cmd, "a pin is [x, y, toX, toY]");
                }
            }
            f.from.push_back (v[0].get<double> ());
            f.from.push_back (v[1].get<double> ());
            f.to.push_back (v[2].get<double> ());
            f.to.push_back (v[3].get<double> ());
        }
        std::string mode = "normal";
        param_str (p, "mode", &mode);
        if (mode == "rigid")
        {
            f.mode = algo::PinMode::Rigid;
        }
        else if (mode == "distort")
        {
            f.mode = algo::PinMode::Distort;
        }
        else if (mode != "normal")
        {
            return bad_params (err, cmd, "`mode` must be rigid, normal or distort");
        }
        return deform_layer (s, d, id, f, interp_param (p), false, "Puppet Warp", err);
    }

    bool cmd_puppet_warp (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        if (e.doc () == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!cmd_puppet_warp_on (s, e.doc (), p, err))
        {
            return false;
        }
        return e.commit ("Puppet Warp", err);
    }

    /* Perspective Warp: the plane laid out as quad "from" pushed to quad "to". */
    bool cmd_perspective_warp_on (Session& s, doc::Document* d, const Value& p, Error* err)
    {
        const char* cmd = "edit.perspectiveWarp";
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        const doc::Layer* l = d->layer (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        double from[4][2], to[4][2];
        if (!read_points (p, "from", from, 4, cmd, err) || !read_points (p, "to", to, 4, cmd, err))
        {
            return false;
        }
        double unit[4] = {0, 0, 1, 1};
        algo::Homography a, b, ai;
        if (!algo::Homography::rect_to_quad (unit, from, &a) || !algo::Homography::rect_to_quad (unit, to, &b)
            || !a.inverse (&ai))
        {
            return bad_params (err, cmd, "the quads must not be degenerate");
        }
        double rect[4];
        if (!deform_frame (*d, *l, p, rect))
        {
            return fail (err, ErrorKind::Other, "nothing to warp");
        }
        algo::Deform f = algo::Deform::patch_identity (rect);
        f.kind         = algo::DeformKind::Homography;
        f.h            = b.mul (ai);
        return deform_layer (s, d, id, f, interp_param (p), true, "Perspective Warp", err);
    }

    bool cmd_perspective_warp (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        if (e.doc () == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!cmd_perspective_warp_on (s, e.doc (), p, err))
        {
            return false;
        }
        return e.commit ("Perspective Warp", err);
    }

    /* Content-Aware Scale: the layer's content rescaled into "rect" by removing or doubling its
       least noticeable seams, so subjects keep their shape. "protect" names an alpha channel whose
       white areas are kept; "protectSkinTones" keeps skin-coloured areas. */
    bool cmd_content_aware_scale_on (Session& s, doc::Document* d, const Value& p, Error* err)
    {
        const char* cmd = "edit.contentAwareScale";
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        double r[4];
        const Value* rv = param (p, "rect");
        if (rv == NULL || !rv->is_array () || rv->size () != 4)
        {
            return bad_params (err, cmd, "`rect` must be [x0, y0, x1, y1] (where the content goes)");
        }
        for (size_t i = 0; i < 4; i++)
        {
            r[i] = (*rv)[i].is_number () ? (*rv)[i].get<double> () : 0.0;
        }
        Rect dst = {(int32_t) lround (r[0]), (int32_t) lround (r[1]), (int32_t) lround (r[2]), (int32_t) lround (r[3])};
        if (dst.is_empty ())
        {
            return bad_params (err, cmd, "`rect` is empty");
        }
        if (!check_pixels_unlocked (*d, id, err))
        {
            return false;
        }
        background_to_layer (d, id);
        doc::Layer* l         = d->layer_mut (id);
        raster::Surface* surf = l != NULL ? l->surface_mut () : NULL;
        if (surf == NULL)
        {
            return fail (err, ErrorKind::Other, "Content-Aware Scale works on pixel layers");
        }
        Rect src = surf->content_bounds ().intersect (d->bounds ());
        if (src.is_empty ())
        {
            return fail (err, ErrorKind::Other, "the layer is empty");
        }
        if (dst.width () > src.width () * 3 || dst.height () > src.height () * 3)
        {
            return bad_params (err, cmd, "Content-Aware Scale grows content up to three times");
        }
        std::vector<float> px;
        surf->read_region (src, &px);
        size_t n = surf->channels (), w = src.width (), h = src.height ();
        std::vector<float> prot;
        int64_t ch;
        if (param_i64 (p, "protect", &ch))
        {
            if (ch < 0 || (size_t) ch >= d->channels.size ())
            {
                return bad_params (err, cmd, "no alpha channel %lld", (long long) ch);
            }
            d->channels[(size_t) ch].surface.read_region (src, &prot);
        }
        bool skin = false;
        param_bool (p, "protectSkinTones", &skin);
        if (skin)
        {
            if (prot.empty ())
            {
                prot.assign (w * h, 0.0f);
            }
            for (size_t i = 0; i < w * h; i++)
            {
                float rgba[4];
                raster::to_rgba (surf->format (), &px[i * n], s.cmyk, rgba);
                float rr = rgba[0], gg = rgba[1], bb = rgba[2];
                /* A loose skin test: warm, red over green over blue, not too dark or grey. */
                if (rr > 0.35f && rr > gg && gg > bb && rr - bb > 0.1f && rr - gg < 0.45f)
                {
                    prot[i] = 1.0f;
                }
            }
        }
        std::vector<float> out;
        algo::seam_carve (px, w, h, n, dst.width (), dst.height (), prot.empty () ? NULL : &prot, &out);
        raster::Surface fresh (surf->format ());
        fresh.write_region (dst, out.data (), out.size ());
        fresh.prune ();
        *surf = fresh;
        return true;
    }

    bool cmd_content_aware_scale (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        if (e.doc () == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!cmd_content_aware_scale_on (s, e.doc (), p, err))
        {
            return false;
        }
        return e.commit ("Content-Aware Scale", err);
    }

    /* Any of the four on a document (the window's previews run them on a copy). */
    bool apply_deform_command (Session& s, doc::Document* d, const char* cmd, const Value& p, Error* err)
    {
        if (strcmp (cmd, "edit.warp") == 0)
        {
            return cmd_warp_on (s, d, p, err);
        }
        if (strcmp (cmd, "edit.puppetWarp") == 0)
        {
            return cmd_puppet_warp_on (s, d, p, err);
        }
        if (strcmp (cmd, "edit.perspectiveWarp") == 0)
        {
            return cmd_perspective_warp_on (s, d, p, err);
        }
        if (strcmp (cmd, "edit.contentAwareScale") == 0)
        {
            return cmd_content_aware_scale_on (s, d, p, err);
        }
        return fail (err, ErrorKind::Other, "unknown deformation `%s`", cmd);
    }

    bool has_deform_target (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l != NULL
            && (l->kind == doc::LayerKind::Raster || l->kind == doc::LayerKind::Shape
                || l->kind == doc::LayerKind::Smart))
        {
            return true;
        }
        *why = "the active layer can't be warped (type: rasterize it or use Warp Text)";
        return false;
    }

    bool can_content_scale (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l != NULL && l->kind == doc::LayerKind::Raster)
        {
            return true;
        }
        *why = "Content-Aware Scale works on pixel layers";
        return false;
    }

    const CommandSpec WARP_COMMANDS[] = {
        {"edit.warp", "Warp", "", NULL,
            "{\"layer\":id?,\"rect\":[x0,y0,x1,y1]?,\"points\":[16 x [x,y]] | "
            "\"style\":\"arc|arcLower|arcUpper|arch|bulge|shellLower|shellUpper|flag|wave|fish|rise|fisheye|"
            "inflate|squeeze|twist\",\"bend\":-100..100=50,\"horizontal\":-100..100,\"vertical\":-100..100,"
            "\"orientation\":\"horizontal|vertical\"}",
            has_deform_target, cmd_warp, true},
        {"edit.puppetWarp", "Puppet Warp", "Edit", NULL,
            "{\"layer\":id?,\"pins\":[[x,y,toX,toY],…],\"mode\":\"rigid|normal|distort\"=\"normal\"}",
            has_deform_target, cmd_puppet_warp, true},
        {"edit.perspectiveWarp", "Perspective Warp", "Edit", NULL,
            "{\"layer\":id?,\"from\":[4 x [x,y]],\"to\":[4 x [x,y]]}", has_deform_target, cmd_perspective_warp, true},
        {"edit.contentAwareScale", "Content-Aware Scale", "Edit", "Cmd+Alt+Shift+C",
            "{\"layer\":id?,\"rect\":[x0,y0,x1,y1],\"protect\":alpha channel?,\"protectSkinTones\":bool=false}",
            can_content_scale, cmd_content_aware_scale, true},
    };

    void register_warp_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), WARP_COMMANDS, WARP_COMMANDS + sizeof WARP_COMMANDS / sizeof WARP_COMMANDS[0]);
    }

} // namespace engine
