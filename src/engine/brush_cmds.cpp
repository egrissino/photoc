#include "engine/brush.h"
#include "engine/layers.h"
#include "engine/session.h"
#include "engine/vector.h"
#include "paint/paint.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace engine
{

    /* paint.stroke (Brush and Eraser), paint.pencil, tools.setBrush and layer.translate (the Move
       tool). Strokes paint the layer's pixels or, with "target": "mask", its layer mask. */

    using geom::Rect;

    void color_param (const Value& p, const char* key, const float fallback[4], float out[4]);

    /* Points as [x, y, pressure?, tiltX?, tiltY?, rotation?, timeMs?, wheel?] arrays or
       {"x", "y", "pressure", ...} objects. A stroke runs dab by dab along its length, so coordinates
       are bounded. */
    bool parse_points (const Value& p, const char* cmd, std::vector<paint::StrokePoint>* out, Error* err)
    {
        const Value* arr = param (p, "points");
        if (arr == NULL || !arr->is_array ())
        {
            return bad_params (err, cmd, "missing `points`");
        }
        out->clear ();
        for (size_t i = 0; i < arr->size (); i++)
        {
            const Value& v        = (*arr)[i];
            paint::StrokePoint sp = paint::StrokePoint::make (0.0, 0.0, 1.0f);
            if (v.is_array ())
            {
                if (v.size () < 2 || !v[0].is_number () || !v[1].is_number ())
                {
                    continue;
                }
                double g[8] = {0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
                for (size_t k = 0; k < 8 && k < v.size (); k++)
                {
                    if (v[k].is_number ())
                    {
                        g[k] = v[k].get<double> ();
                    }
                }
                sp.x        = g[0];
                sp.y        = g[1];
                sp.pressure = (float) g[2];
                sp.tilt_x   = (float) g[3];
                sp.tilt_y   = (float) g[4];
                sp.rotation = (float) g[5];
                sp.time     = g[6];
                sp.wheel    = (float) g[7];
            }
            else if (v.is_object ())
            {
                double d;
                if (!param_f64 (v, "x", &sp.x) || !param_f64 (v, "y", &sp.y))
                {
                    continue;
                }
                if (param_f64 (v, "pressure", &d))
                {
                    sp.pressure = (float) d;
                }
                if (param_f64 (v, "tiltX", &d))
                {
                    sp.tilt_x = (float) d;
                }
                if (param_f64 (v, "tiltY", &d))
                {
                    sp.tilt_y = (float) d;
                }
                if (param_f64 (v, "rotation", &d))
                {
                    sp.rotation = (float) d;
                }
                if (param_f64 (v, "time", &d))
                {
                    sp.time = d;
                }
                if (param_f64 (v, "wheel", &d))
                {
                    sp.wheel = (float) d;
                }
            }
            else
            {
                continue;
            }
            if (!(fabs (sp.x) <= paint::MAX_COORD && fabs (sp.y) <= paint::MAX_COORD))
            {
                return bad_params (err, cmd, "point coordinates must be finite and within +-%.0f", paint::MAX_COORD);
            }
            out->push_back (sp);
        }
        if (out->empty ())
        {
            return bad_params (err, cmd, "`points` is empty");
        }
        return true;
    }

    bool brush_num (const Value& patch, const char* cmd, const char* key, float* field, Error* err)
    {
        const Value* v = param (patch, key);
        if (v == NULL)
        {
            return true;
        }
        if (!v->is_number ())
        {
            return bad_params (err, cmd, "invalid brush: `%s` must be a number", key);
        }
        *field = (float) v->get<double> ();
        return true;
    }

    bool brush_bool (const Value& patch, const char* cmd, const char* key, bool* field, Error* err)
    {
        const Value* v = param (patch, key);
        if (v == NULL)
        {
            return true;
        }
        if (!v->is_boolean ())
        {
            return bad_params (err, cmd, "invalid brush: `%s` must be true or false", key);
        }
        *field = v->get<bool> ();
        return true;
    }

    /* A brush settings object (the reference's camelCase field names) merged over `b`. Fields of
       brush features not ported yet are ignored. */
    bool merge_brush (paint::BrushSettings* b, const Value& patch, const char* cmd, Error* err)
    {
        if (!patch.is_object ())
        {
            return bad_params (err, cmd, "invalid brush: must be an object");
        }
        if (!brush_num (patch, cmd, "size", &b->size, err) || !brush_num (patch, cmd, "hardness", &b->hardness, err)
            || !brush_num (patch, cmd, "spacing", &b->spacing, err)
            || !brush_num (patch, cmd, "opacity", &b->opacity, err) || !brush_num (patch, cmd, "flow", &b->flow, err)
            || !brush_num (patch, cmd, "angle", &b->angle, err)
            || !brush_num (patch, cmd, "roundness", &b->roundness, err))
        {
            return false;
        }
        if (!brush_bool (patch, cmd, "pressureSize", &b->pressure_size, err)
            || !brush_bool (patch, cmd, "pressureOpacity", &b->pressure_opacity, err)
            || !brush_bool (patch, cmd, "erase", &b->erase, err) || !brush_bool (patch, cmd, "flipX", &b->flip_x, err)
            || !brush_bool (patch, cmd, "flipY", &b->flip_y, err)
            || !brush_bool (patch, cmd, "aliased", &b->aliased, err)
            || !brush_bool (patch, cmd, "square", &b->square, err))
        {
            return false;
        }
        const Value* mode = param (patch, "mode");
        if (mode != NULL)
        {
            if (!mode->is_string () || !blend_from_str (mode->get_ref<const std::string&> ().c_str (), &b->mode))
            {
                return bad_params (err, cmd, "invalid brush: unknown `mode`");
            }
        }
        color_param (patch, "color", b->color, b->color);
        color_param (patch, "background", b->background, b->background);
        uint64_t seed;
        if (param_u64 (patch, "seed", &seed))
        {
            b->seed = seed;
        }
        return true;
    }

    Value brush_json (const paint::BrushSettings& b)
    {
        return Value{{"size", b.size}, {"hardness", b.hardness}, {"spacing", b.spacing}, {"opacity", b.opacity},
            {"flow", b.flow}, {"pressureSize", b.pressure_size}, {"pressureOpacity", b.pressure_opacity},
            {"color", {b.color[0], b.color[1], b.color[2], b.color[3]}},
            {"background", {b.background[0], b.background[1], b.background[2], b.background[3]}}, {"erase", b.erase},
            {"mode", blend_variant_name (b.mode)}, {"angle", b.angle}, {"roundness", b.roundness}, {"flipX", b.flip_x},
            {"flipY", b.flip_y}, {"aliased", b.aliased}, {"square", b.square}, {"seed", b.seed}};
    }

    bool validate_brush (const paint::BrushSettings& b, const char* cmd, Error* err)
    {
        if (!isfinite (b.size) || b.size > paint::MAX_BRUSH_SIZE)
        {
            return bad_params (err, cmd, "brush size must be finite and at most %.0f px", paint::MAX_BRUSH_SIZE);
        }
        return true;
    }

    /* "clear" erases with this brush (the reference's Clear brush mode). */
    bool clear_mode (const Value& p)
    {
        std::string m;
        return param_str (p, "mode", &m) && m == "clear";
    }

    /* The tool's brush with the stroke's overrides: "brush" patch, then the top-level shortcuts, the
       colour (foreground by default), erase, and a seed derived from the points when not given. */
    bool resolve_brush (const Session& s, const Value& p, const char* cmd, paint::BrushSettings* out, Error* err)
    {
        paint::BrushSettings b = s.tools.brush;
        const Value* patch     = param (p, "brush");
        if (patch != NULL && patch->is_object () && !merge_brush (&b, *patch, cmd, err))
        {
            return false;
        }
        double v;
        if (param_f64 (p, "size", &v))
        {
            b.size = (float) fmax (v, 0.5);
        }
        if (param_f64 (p, "hardness", &v))
        {
            b.hardness = (float) fmin (fmax (v, 0.0), 1.0);
        }
        if (param_f64 (p, "opacity", &v))
        {
            b.opacity = (float) fmin (fmax (v, 0.0), 1.0);
        }
        if (param_f64 (p, "flow", &v))
        {
            b.flow = (float) fmin (fmax (v, 0.0), 1.0);
        }
        if (param_f64 (p, "spacing", &v))
        {
            b.spacing = (float) fmin (fmax (v, 0.01), 10.0);
        }
        color_param (p, "color", s.tools.foreground, b.color);
        memcpy (b.background, s.tools.background, sizeof b.background);
        bool erase = false;
        param_bool (p, "erase", &erase);
        b.erase = erase || clear_mode (p);
        std::string mode;
        if (param_str (p, "mode", &mode) && mode != "clear")
        {
            if (!blend_from_str (mode.c_str (), &b.mode) || b.mode == color::BlendMode::PassThrough)
            {
                return bad_params (err, cmd, "unknown blend mode `%s`", mode.c_str ());
            }
        }
        if (b.erase)
        {
            b.mode = color::BlendMode::Normal;
        }
        uint64_t seed;
        if (param_u64 (p, "seed", &seed))
        {
            b.seed = seed;
        }
        else
        {
            const Value* pts = param (p, "points");
            std::string text = pts != NULL ? pts->dump () : "";
            b.seed           = paint::seed_from_bytes ((const uint8_t*) text.data (), text.size ());
        }
        if (!validate_brush (b, cmd, err))
        {
            return false;
        }
        *out = b;
        return true;
    }

    bool has_paintable (const Session& s, std::string* why)
    {
        const DocState* d = s.active ();
        if (d == NULL)
        {
            *why = "no document open";
            return false;
        }
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l == NULL)
        {
            *why = "no active layer";
            return false;
        }
        if (l->kind != doc::LayerKind::Raster)
        {
            *why = std::string ("active layer is ") + doc::layer_kind_article (l->kind) + " "
                 + doc::layer_kind_name (l->kind) + " layer, not a pixel layer";
            return false;
        }
        return true;
    }

    bool is_mask_target (const Value& p)
    {
        std::string t;
        return param_str (p, "target", &t) && (t == "mask" || t == "selection");
    }

    /* Quick Mask: "target": "selection" paints the selection itself (white selects, black masks). */
    bool is_quick_mask (const Value& p)
    {
        std::string t;
        return param_str (p, "target", &t) && t == "selection";
    }

    /* The surface a stroke paints: the layer's pixels, or its mask with "target": "mask". `lock` is
       the pixels' transparency lock (masks have none). */
    raster::Surface* stroke_surface (doc::Document* d, doc::LayerId id, const Value& p, bool* lock, Error* err)
    {
        doc::Layer* l = d->layer_mut (id);
        if (l == NULL)
        {
            fail (err, ErrorKind::NoLayer, "no such layer %llu", (unsigned long long) id);
            return NULL;
        }
        if (is_quick_mask (p))
        {
            /* No selection is all selected here, so painting black masks from there. */
            if (!d->has_selection)
            {
                float one    = 1.0f;
                d->selection = raster::Surface (color::PixelFormat::gray8 ());
                d->selection.fill_rect (d->bounds (), &one);
                d->has_selection = true;
            }
            *lock = false;
            return &d->selection;
        }
        bool mask        = is_mask_target (p);
        doc::Locks locks = d->effective_locks (id);
        if (locks.all || (!mask && locks.pixels))
        {
            fail (err, ErrorKind::Other, "Could not complete your request because the layer \"%s\" is locked",
                l->name.c_str ());
            return NULL;
        }
        if (mask)
        {
            if (!l->has_mask)
            {
                fail (err, ErrorKind::Other, "layer has no mask");
                return NULL;
            }
            *lock = false;
            return &l->mask.surface;
        }
        raster::Surface* surf = l->surface_mut ();
        if (surf == NULL)
        {
            fail (err, ErrorKind::Other, "the layer has no pixels to paint");
            return NULL;
        }
        *lock = locks.transparency;
        return surf;
    }

    /* The Eraser's Block: a hard, aliased square of 16 screen pixels (16 / zoom document pixels). */
    bool block_brush (const paint::BrushSettings& b, const Value& p, paint::BrushSettings* out, Error* err)
    {
        double zoom = 1.0;
        if (param_present (p, "zoom") && (!param_f64 (p, "zoom", &zoom) || !isfinite (zoom) || zoom <= 0.0))
        {
            return bad_params (err, "paint.pencil", "`zoom` must be a positive number");
        }
        paint::BrushSettings k = paint::BrushSettings::defaults ();
        k.size                 = (float) fmin (fmax (round (16.0 / zoom), 1.0), (double) paint::MAX_BRUSH_SIZE);
        k.aliased              = true;
        k.square               = true;
        k.pressure_size        = false;
        k.pressure_opacity     = false;
        k.spacing              = 0.25f;
        memcpy (k.color, b.color, sizeof k.color);
        memcpy (k.background, b.background, sizeof k.background);
        k.erase = b.erase;
        k.seed  = b.seed;
        *out    = k;
        return true;
    }

    /* The brush a paint command strokes with: the Pencil's is aliased, hard and at full flow (or the
       Block). */
    bool tool_brush (const Session& s, const char* cmd, const Value& p, paint::BrushSettings* out, Error* err)
    {
        if (!resolve_brush (s, p, cmd, out, err))
        {
            return false;
        }
        if (strcmp (cmd, "paint.pencil") == 0)
        {
            bool block = false;
            param_bool (p, "block", &block);
            if (block)
            {
                if (!block_brush (*out, p, out, err))
                {
                    return false;
                }
            }
            else
            {
                out->aliased = true;
                if (!param_present (p, "hardness"))
                {
                    out->hardness = 1.0f;
                }
                if (!param_present (p, "flow"))
                {
                    out->flow = 1.0f;
                }
            }
        }
        return true;
    }

    /* Last adjustments against the target: erasing a mask or locked transparency (the Background)
       paints the background colour; the Pencil's Auto Erase paints the background colour when the
       stroke starts on a pixel of the foreground colour. */
    void finish_brush (paint::BrushSettings* b, const raster::Surface& surf, bool lock, bool mask, bool auto_erase,
        const paint::StrokePoint* first, const float background[4], const color::CmykSpace* cmyk)
    {
        if (b->erase && (lock || mask))
        {
            b->erase = false;
            memcpy (b->color, background, sizeof b->color);
        }
        if (!auto_erase || first == NULL)
        {
            return;
        }
        float c[4];
        surf.rgba ((int32_t) floor (first->x), (int32_t) floor (first->y), cmyk, c);
        bool same = c[3] > 0.0f;
        for (int i = 0; i < 3; i++)
        {
            same = same && fabsf (c[i] - b->color[i]) < 1.5f / 255.0f;
        }
        if (same)
        {
            memcpy (b->color, background, sizeof b->color);
        }
    }

    /* The stroke the canvas already rendered live for exactly these params on this document state
       (set by the UI just before it commits): its pixels replace rendering the whole stroke again,
       the pause at pen-up. Taken once either way. */
    bool take_live_stroke (Session& s, const char* cmd, const Value& p, LiveStroke* out)
    {
        if (!s.live_stroke.valid)
        {
            return false;
        }
        s.live_stroke.valid = false;
        const DocState* d   = s.active ();
        if (d == NULL || s.live_stroke.command != cmd || s.live_stroke.doc != d->doc->id
            || s.live_stroke.revision != d->revision || s.live_stroke.params != p)
        {
            return false;
        }
        *out                  = s.live_stroke;
        s.live_stroke.surface = raster::Surface ();
        return true;
    }

    bool stroke_with (Session& s, const char* cmd, const Value& p, const char* label, Value* result, Error* err)
    {
        LiveStroke live;
        bool from_live = take_live_stroke (s, cmd, p, &live);
        std::vector<paint::StrokePoint> pts;
        if (!parse_points (p, cmd, &pts, err))
        {
            return false;
        }
        paint::BrushSettings brush;
        if (!tool_brush (s, cmd, p, &brush, err))
        {
            return false;
        }
        if (brush.erase && !clear_mode (p))
        {
            label = "Eraser";
        }
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        bool auto_erase = false;
        param_bool (p, "autoErase", &auto_erase);
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        bool lock;
        raster::Surface* surf = stroke_surface (d, id, p, &lock, err);
        if (surf == NULL)
        {
            return false;
        }
        Rect dmg;
        if (from_live)
        {
            *surf = live.surface;
            dmg   = live.bounds;
        }
        else
        {
            finish_brush (&brush, *surf, lock, is_mask_target (p), auto_erase, pts.empty () ? NULL : &pts[0],
                s.tools.background, s.cmyk);
            dmg = paint::render_stroke (
                surf, brush, pts, d->has_selection && !is_quick_mask (p) ? &d->selection : NULL, lock, s.cmyk);
        }
        surf->prune ();
        if (is_quick_mask (p))
        {
            d->has_selection = !d->selection.content_bounds ().intersect (d->bounds ()).is_empty ();
        }
        if (!e.commit (label, err))
        {
            return false;
        }
        *result = Value{{"damage", {dmg.x0, dmg.y0, dmg.width (), dmg.height ()}}};
        if (from_live)
        {
            (*result)["fromLive"] = true;
        }
        return true;
    }

    bool cmd_paint_stroke (Session& s, const Value& p, Value* result, Error* err)
    {
        return stroke_with (s, "paint.stroke", p, "Brush Tool", result, err);
    }

    bool cmd_paint_pencil (Session& s, const Value& p, Value* result, Error* err)
    {
        return stroke_with (s, "paint.pencil", p, "Pencil", result, err);
    }

    /* The tool's brush: a settings patch ({"size": 30} or {"brush": {...}}); "reset" restores the
       defaults first. Returns the resulting brush. */
    bool cmd_tools_set_brush (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd        = "tools.setBrush";
        paint::BrushSettings b = s.tools.brush;
        bool reset             = false;
        param_bool (p, "reset", &reset);
        if (reset)
        {
            b = paint::BrushSettings::defaults ();
        }
        const Value* inner = param (p, "brush");
        if (inner != NULL && !merge_brush (&b, *inner, cmd, err))
        {
            return false;
        }
        if (p.is_object () && !merge_brush (&b, p, cmd, err))
        {
            return false;
        }
        if (!validate_brush (b, cmd, err))
        {
            return false;
        }
        s.tools.brush = b;
        *result       = brush_json (b);
        return true;
    }

    void translate_layer (doc::Layer* l, int32_t dx, int32_t dy)
    {
        if (l->has_mask && l->mask.linked)
        {
            l->mask.surface = l->mask.surface.translated (dx, dy, l->mask.surface.content_bounds ());
        }
        if (l->kind == doc::LayerKind::Raster || l->kind == doc::LayerKind::Text)
        {
            l->pixels = l->pixels.translated (dx, dy, l->pixels.content_bounds ());
        }
        /* Type moves with its rendering. */
        if (l->kind == doc::LayerKind::Text)
        {
            l->text.transform[4] += dx;
            l->text.transform[5] += dy;
        }
        translate_vectors (l, dx, dy);
        for (size_t i = 0; i < l->children.size (); i++)
        {
            translate_layer (&l->children[i], dx, dy);
        }
    }

    /* The roots with every layer linked to them, without layers whose group is also moving. */
    std::vector<doc::LayerId> move_targets (const doc::Document& d, const std::vector<doc::LayerId>& roots)
    {
        std::vector<uint64_t> groups;
        for (size_t i = 0; i < roots.size (); i++)
        {
            const doc::Layer* l = d.layer (roots[i]);
            if (l != NULL && l->link_group != 0
                && std::find (groups.begin (), groups.end (), l->link_group) == groups.end ())
            {
                groups.push_back (l->link_group);
            }
        }
        std::vector<doc::LayerId> ids = roots;
        if (!groups.empty ())
        {
            std::vector<doc::WalkEntry> walk = d.walk ();
            for (size_t i = 0; i < walk.size (); i++)
            {
                const doc::Layer* l = walk[i].layer;
                bool linked =
                    l->link_group != 0 && std::find (groups.begin (), groups.end (), l->link_group) != groups.end ();
                if (linked && std::find (ids.begin (), ids.end (), l->id) == ids.end ())
                {
                    ids.push_back (l->id);
                }
            }
        }
        return top_level (d, ids);
    }

    /* Moves `roots` (and their linked layers) by whole pixels, as the Move tool does. Aimed at the
       mask, a layer whose mask is unlinked moves that mask alone. A position-locked layer refuses. */
    bool move_layers (
        doc::Document* d, const std::vector<doc::LayerId>& roots, int32_t dx, int32_t dy, bool mask_target, Error* err)
    {
        std::vector<doc::LayerId> alone, rest;
        for (size_t i = 0; i < roots.size (); i++)
        {
            const doc::Layer* l = d->layer (roots[i]);
            if (mask_target && l != NULL && l->has_mask && !l->mask.linked)
            {
                alone.push_back (roots[i]);
            }
            else
            {
                rest.push_back (roots[i]);
            }
        }
        std::vector<doc::LayerId> ids = move_targets (*d, rest);
        if (ids.empty () && alone.empty ())
        {
            return fail (err, ErrorKind::NoLayer, "no such layer %llu", (unsigned long long) roots[0]);
        }
        for (size_t i = 0; i < alone.size (); i++)
        {
            ids.push_back (alone[i]);
        }
        for (size_t i = 0; i < ids.size (); i++)
        {
            doc::Locks locks = d->effective_locks (ids[i]);
            doc::Layer* l    = d->layer_mut (ids[i]);
            if (l == NULL)
            {
                return fail (err, ErrorKind::NoLayer, "no such layer %llu", (unsigned long long) ids[i]);
            }
            if (locks.position || locks.all)
            {
                return fail (err, ErrorKind::Other, "layer \"%s\" is position-locked", l->name.c_str ());
            }
            if (i >= ids.size () - alone.size ())
            {
                l->mask.surface = l->mask.surface.translated (dx, dy, l->mask.surface.content_bounds ());
            }
            else
            {
                translate_layer (l, dx, dy);
            }
        }
        return true;
    }

    /* The Move tool: the layer, or every selected layer, by whole pixels with their linked layers
       and linked masks; groups move their contents. */
    bool cmd_layer_translate (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.translate";
        int64_t dx = 0, dy = 0;
        if (param_present (p, "dx") && !param_i64 (p, "dx", &dx))
        {
            return bad_params (err, cmd, "`dx` must be a whole number");
        }
        if (param_present (p, "dy") && !param_i64 (p, "dy", &dy))
        {
            return bad_params (err, cmd, "`dy` must be a whole number");
        }
        if (dx < INT32_MIN || dx > INT32_MAX || dy < INT32_MIN || dy > INT32_MAX)
        {
            return bad_params (err, cmd, "`dx` and `dy` must fit in 32 bits");
        }
        if (dx == 0 && dy == 0)
        {
            return true;
        }
        std::vector<doc::LayerId> roots;
        uint64_t one;
        if (param_u64 (p, "layer", &one))
        {
            roots.push_back (one);
        }
        else if (s.active () != NULL)
        {
            roots = s.active ()->selected ();
        }
        if (roots.empty ())
        {
            return fail (err, ErrorKind::Other, "no active layer");
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!move_layers (d, roots, (int32_t) dx, (int32_t) dy, is_mask_target (p), err))
        {
            return false;
        }
        return e.commit ("Move", err);
    }

    const CommandSpec BRUSH_COMMANDS[] = {
        {"paint.stroke", "Brush Stroke", "", NULL,
            "{\"points\":[[x,y,pressure?,tiltX?,tiltY?,rotation?,timeMs?,wheel?],…],\"brush\":{…BrushSettings}?,"
            "\"size\":0.5..5000 px?,\"hardness\":0..1?,"
            "\"opacity\":0..1?,\"flow\":0..1?,\"spacing\":0..10?,\"color\":\"#rrggbb\"?=foreground,\"mode\":\"normal|"
            "multiply|screen|…|clear\"=\"normal\","
            "\"erase\":bool?,\"seed\":u64?,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\"}",
            has_paintable, cmd_paint_stroke, true},
        {"paint.pencil", "Pencil", "", NULL,
            "{\"points\":[…],\"brush\":{…}?,\"size\":0.5..5000 "
            "px?,\"opacity\":0..1?,\"color\":\"#rrggbb\"?=foreground,\"mode\":\"normal|multiply|…|clear\"=\"normal\","
            "\"erase\":bool?,\"block\":bool=false (the Eraser's Block: 16/zoom doc px square),\"zoom\":view "
            "zoom=1,\"autoErase\":bool=false,\"seed\":u64?,"
            "\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\"}",
            has_paintable, cmd_paint_pencil, true},
        {"tools.setBrush", "Set Brush", "", NULL, "{…BrushSettings fields,\"brush\":{…}?,\"reset\":bool?}", always,
            cmd_tools_set_brush, true},
        {"layer.translate", "Move Layer", "", NULL,
            "{\"layer\":id?,\"dx\":i32,\"dy\":i32,\"target\":\"pixels|mask\"=\"pixels\"} (no layer: every selected "
            "layer; linked layers follow; target mask: an "
            "unlinked mask moves alone)",
            has_layer, cmd_layer_translate, true},
    };

    void register_brush_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), BRUSH_COMMANDS, BRUSH_COMMANDS + sizeof BRUSH_COMMANDS / sizeof BRUSH_COMMANDS[0]);
    }

} // namespace engine
