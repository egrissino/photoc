#include "compose/compose.h"
#include "compose/effects.h"
#include "engine/layers.h"
#include "engine/session.h"
#include "engine/text.h"
#include <math.h>
#include <string.h>

namespace engine
{

    /* Layer menu basics and Image > Image Rotation. */

    using geom::Rect;

    /* "layer" from the params, else the active layer. */
    bool layer_param (const Session& s, const Value& p, doc::LayerId* out, Error* err)
    {
        uint64_t id;
        if (param_u64 (p, "layer", &id))
        {
            *out = id;
            return true;
        }
        const DocState* d = s.active ();
        if (d == NULL || d->active_layer == doc::NO_LAYER)
        {
            return fail (err, ErrorKind::Other, "no active layer");
        }
        *out = d->active_layer;
        return true;
    }

    bool no_layer (Error* err, doc::LayerId id)
    {
        return fail (err, ErrorKind::NoLayer, "no such layer %llu", (unsigned long long) id);
    }

    /* The Layers panel rows (top to bottom, expanded groups open) around `target`: the rows below
       it first, then the ones above, nearest first. Delete makes the first that survives active. */
    std::vector<doc::LayerId> deletion_neighbours (const doc::Document& d, doc::LayerId target)
    {
        std::vector<doc::LayerId> order;
        for (int pass = 0; pass < 2; pass++)
        {
            bool expand_all = pass == 1;
            order.clear ();
            std::vector<const doc::Layer*> stack;
            for (size_t i = 0; i < d.layers.size (); i++)
            {
                stack.push_back (&d.layers[i]);
            }
            while (!stack.empty ())
            {
                const doc::Layer* l = stack.back ();
                stack.pop_back ();
                order.push_back (l->id);
                if (l->kind == doc::LayerKind::Group && (expand_all || l->expanded))
                {
                    for (size_t i = 0; i < l->children.size (); i++)
                    {
                        stack.push_back (&l->children[i]);
                    }
                }
            }
            for (size_t i = 0; i < order.size (); i++)
            {
                if (order[i] != target)
                {
                    continue;
                }
                std::vector<doc::LayerId> out (order.begin () + (ptrdiff_t) i + 1, order.end ());
                for (size_t k = i; k-- > 0;)
                {
                    out.push_back (order[k]);
                }
                return out;
            }
        }
        return std::vector<doc::LayerId> ();
    }

    /* With several layers selected (and no "layer" param), every one of them goes in one step. */
    bool cmd_layer_delete (Session& s, const Value& p, Value* result, Error* err)
    {
        std::vector<doc::LayerId> targets;
        if (!layer_targets (s, p, &targets, err))
        {
            return false;
        }
        bool multi = targets.size () > 1;
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::LayerId* active                 = e.active_layer ();
        std::vector<doc::LayerId> neighbours = deletion_neighbours (*d, multi ? *active : targets[0]);
        std::vector<doc::LayerId> ids        = top_level (*d, targets);
        ClipBefore before                    = clip_before (*d, ids);
        for (size_t i = 0; i < ids.size (); i++)
        {
            if (!d->remove (ids[i], NULL))
            {
                return no_layer (err, ids[i]);
            }
        }
        clip_settle (d, std::vector<doc::LayerId> (), before, false);
        if (d->layers.empty ())
        {
            return fail (err, ErrorKind::Other, "a document must keep at least one layer");
        }
        if (multi || (*active != doc::NO_LAYER && d->layer (*active) == NULL))
        {
            *active = doc::NO_LAYER;
            for (size_t i = 0; i < neighbours.size (); i++)
            {
                if (d->layer (neighbours[i]) != NULL)
                {
                    *active = neighbours[i];
                    break;
                }
            }
        }
        if (!e.commit (multi ? "Delete Layers" : "Delete Layer", err))
        {
            return false;
        }
        *result = Value{{"deleted", ids.size ()}};
        return true;
    }

    /* A copy of the Background is an ordinary, unlocked layer (Photoshop). */
    doc::Layer layer_copy (doc::IdSource& ids, const doc::Document& d, const doc::Layer& src)
    {
        doc::Layer dup = src.duplicate (ids);
        dup.name       = d.copy_name (dup.name);
        if (d.is_background (src.id))
        {
            memset (&dup.locks, 0, sizeof dup.locks);
        }
        return dup;
    }

    /* Each copy goes above its original; with several layers selected the copies become the
       selection. */
    bool cmd_layer_duplicate (Session& s, const Value& p, Value* result, Error* err)
    {
        std::vector<doc::LayerId> targets;
        if (!layer_targets (s, p, &targets, err))
        {
            return false;
        }
        std::string name;
        bool has_name = targets.size () == 1 && param_str (p, "name", &name) && !name.empty ();
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::LayerId old_active       = *e.active_layer ();
        std::vector<doc::LayerId> ids = top_level (*d, targets);
        std::vector<doc::LayerId> copies;
        doc::LayerId new_active = doc::NO_LAYER;
        for (size_t i = 0; i < ids.size (); i++)
        {
            const doc::Layer* src = d->layer (ids[i]);
            if (src == NULL)
            {
                return no_layer (err, ids[i]);
            }
            doc::Layer dup = layer_copy (s.ids, *d, *src);
            if (has_name)
            {
                dup.name = name;
            }
            doc::LayerId nid = d->insert_above (ids[i], dup);
            if (ids[i] == old_active)
            {
                new_active = nid;
            }
            copies.push_back (nid);
        }
        if (new_active == doc::NO_LAYER)
        {
            new_active = copies.back ();
        }
        e.select (copies, new_active);
        if (!e.commit (copies.size () > 1 ? "Duplicate Layers" : "Duplicate Layer", err))
        {
            return false;
        }
        *result = Value{{"layer", new_active}, {"layers", copies}};
        return true;
    }

    bool cmd_layer_set_props (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.setProps";
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        bool only_visibility = p.is_object () && p.contains ("visible");
        if (p.is_object ())
        {
            for (Value::const_iterator it = p.begin (); it != p.end (); ++it)
            {
                if (it.key () != "layer" && it.key () != "visible" && it.key () != "coalesce")
                {
                    only_visibility = false;
                }
            }
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::Layer* l = d->layer_mut (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }

        std::string str;
        bool b;
        double v;
        if (param_str (p, "name", &str))
        {
            l->name = str;
        }
        if (param_bool (p, "visible", &b))
        {
            l->visible = b;
        }
        if (param_f64 (p, "opacity", &v))
        {
            l->opacity = (float) (v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v);
        }
        if (param_f64 (p, "fill", &v))
        {
            l->fill_opacity = (float) (v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v);
        }
        if (param_str (p, "blend", &str) && !blend_from_str (str.c_str (), &l->blend))
        {
            return bad_params (err, cmd, "unknown blend mode `%s`", str.c_str ());
        }
        if (param_bool (p, "clipped", &b))
        {
            l->clipped = b;
        }
        if (param_bool (p, "locked", &b))
        {
            l->locks.all = b;
        }
        const Value* channels = param (p, "channels");
        if (channels != NULL && channels->is_array ())
        {
            uint32_t mask = 0;
            for (size_t i = 0; i < channels->size () && i < 32; i++)
            {
                if ((*channels)[i].is_boolean () && !(*channels)[i].get<bool> ())
                {
                    mask |= 1u << i;
                }
            }
            l->excluded_channels = mask;
        }
        const Value* locks = param (p, "locks");
        if (locks != NULL && locks->is_object ())
        {
            for (Value::const_iterator it = locks->begin (); it != locks->end (); ++it)
            {
                bool on              = it.value ().is_boolean () && it.value ().get<bool> ();
                const std::string& k = it.key ();
                if (k == "transparency")
                {
                    l->locks.transparency = on;
                }
                else if (k == "pixels")
                {
                    l->locks.pixels = on;
                }
                else if (k == "position")
                {
                    l->locks.position = on;
                }
                else if (k == "artboard")
                {
                    l->locks.artboard = on;
                }
                else if (k == "all")
                {
                    l->locks.all = on;
                }
                else
                {
                    return bad_params (err, cmd, "unknown lock `%s`", k.c_str ());
                }
            }
        }
        return e.commit (only_visibility ? "Layer Visibility" : "Layer Properties", err);
    }

    /* The composite of a document's visible layers, over the union of their bounds on the canvas. */
    raster::Surface composite_layers (
        const doc::Document& solo, color::PixelFormat format, const color::CmykSpace* cmyk)
    {
        Rect canvas = solo.bounds ();
        Rect area;
        for (size_t i = 0; i < solo.layers.size (); i++)
        {
            if (!solo.layers[i].visible)
            {
                continue;
            }
            Rect b = compose::layer_bounds (solo.layers[i], canvas);
            /* Layer styles reach past the layer's pixels. */
            if (!b.is_empty () && compose::has_effects (solo.layers[i]))
            {
                b = b.inflate (compose::effects_margin (solo.layers[i]));
            }
            area = area.unite (b);
        }
        area = area.intersect (canvas);
        if (area.is_empty ())
        {
            return raster::Surface (format);
        }
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = cmyk;
        return compose::composite_to_surface (solo, area, format, NULL, settings);
    }

    /* Merge Down replaces the lower layer, keeping its id, name, blend mode and locks (a merged
       Background stays opaque and position-locked). */
    bool cmd_layer_merge_down (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::LayerPath path;
        if (!d->path_of (id, &path))
        {
            return no_layer (err, id);
        }
        if (path.back () == 0)
        {
            return fail (err, ErrorKind::Other, "no layer below to merge into");
        }
        doc::LayerPath below = path;
        below.back () -= 1;
        const doc::Layer* lower_p = d->layer_at (below);
        const doc::Layer* upper_p = d->layer (id);
        if (lower_p == NULL || upper_p == NULL)
        {
            return no_layer (err, id);
        }
        doc::Layer lower = *lower_p;
        doc::Layer upper = *upper_p;

        color::PixelFormat format = d->pixel_format ();
        doc::Document tmp         = doc::Document::make (s.ids, "merge", d->size, format.mode, format.sample);
        tmp.icc_profile           = d->icc_profile;
        tmp.layers.push_back (lower);
        tmp.layers.push_back (upper);
        doc::Layer merged = doc::Layer::raster (s.ids, lower.name, format);
        merged.pixels     = composite_layers (tmp, format, s.cmyk);
        merged.id         = lower.id;
        merged.blend      = lower.blend;
        merged.opacity    = 1.0f;
        merged.locks      = lower.locks;

        d->remove (id, NULL);
        doc::Layer* slot = d->layer_at_mut (below);
        if (slot == NULL)
        {
            return no_layer (err, lower.id);
        }
        *slot              = merged;
        *e.active_layer () = lower.id;
        return e.commit ("Merge Down", err);
    }

    bool cmd_layer_flatten_image (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        color::PixelFormat format        = d->pixel_format ();
        doc::Layer bg                    = doc::Layer::raster (s.ids, "Background", format);
        bg.locks.transparency            = true;
        bg.locks.position                = true;
        float white[3]                   = {1.0f, 1.0f, 1.0f};
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = s.cmyk;
        bg.pixels                        = compose::composite_to_surface (*d, d->bounds (), format, white, settings);
        *e.active_layer ()               = bg.id;
        d->layers.clear ();
        d->layers.push_back (bg);
        return e.commit ("Flatten Image", err);
    }

    enum class Turn
    {
        FlipHorizontal,
        FlipVertical,
        Rotate180,
        Cw90,
        Ccw90,
    };

    /* Where pixel (x, y) of a w x h canvas lands. */
    void turn_point (Turn t, int32_t w, int32_t h, int32_t x, int32_t y, int32_t* nx, int32_t* ny)
    {
        switch (t)
        {
        case Turn::FlipHorizontal:
            *nx = w - 1 - x;
            *ny = y;
            break;
        case Turn::FlipVertical:
            *nx = x;
            *ny = h - 1 - y;
            break;
        case Turn::Rotate180:
            *nx = w - 1 - x;
            *ny = h - 1 - y;
            break;
        case Turn::Cw90:
            *nx = h - 1 - y;
            *ny = x;
            break;
        case Turn::Ccw90:
            *nx = y;
            *ny = w - 1 - x;
            break;
        }
    }

    /* The surface with every non-default pixel moved by the turn (layers may reach past the canvas;
       the mapping covers them too). */
    raster::Surface turn_surface (const raster::Surface& src, Turn t, int32_t w, int32_t h)
    {
        float dp[raster::MAX_CHANNELS];
        src.default_pixel (dp);
        raster::Surface out = raster::Surface::with_default (src.format (), dp);
        Rect content        = src.content_bounds ();
        if (content.is_empty ())
        {
            return out;
        }
        int32_t ax, ay, bx, by;
        turn_point (t, w, h, content.x0, content.y0, &ax, &ay);
        turn_point (t, w, h, content.x1 - 1, content.y1 - 1, &bx, &by);
        Rect dst = {ax < bx ? ax : bx, ay < by ? ay : by, (ax > bx ? ax : bx) + 1, (ay > by ? ay : by) + 1};
        std::vector<float> in;
        std::vector<float> moved;
        if (!src.read_region (content, &in))
        {
            return out;
        }
        size_t n = src.channels ();
        moved.resize (in.size ());
        size_t dw = dst.width ();
        size_t cw = content.width ();
        for (int32_t y = content.y0; y < content.y1; y++)
        {
            for (int32_t x = content.x0; x < content.x1; x++)
            {
                int32_t nx, ny;
                turn_point (t, w, h, x, y, &nx, &ny);
                const float* s = &in[(((size_t) (y - content.y0)) * cw + (size_t) (x - content.x0)) * n];
                float* o       = &moved[(((size_t) (ny - dst.y0)) * dw + (size_t) (nx - dst.x0)) * n];
                memcpy (o, s, n * sizeof (float));
            }
        }
        out.write_region (dst, moved.data (), moved.size ());
        out.prune ();
        return out;
    }

    void turn_layers (std::vector<doc::Layer>* layers, Turn t, int32_t w, int32_t h)
    {
        for (size_t i = 0; i < layers->size (); i++)
        {
            doc::Layer& l = (*layers)[i];
            if (l.kind == doc::LayerKind::Raster)
            {
                l.pixels = turn_surface (l.pixels, t, w, h);
            }
            if (l.has_mask)
            {
                l.mask.surface = turn_surface (l.mask.surface, t, w, h);
            }
            turn_layers (&l.children, t, w, h);
        }
    }

    bool canvas_turn (Session& s, const char* label, Turn t, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        int32_t w = (int32_t) d->size.width, h = (int32_t) d->size.height;
        turn_layers (&d->layers, t, w, h);
        if (d->has_selection)
        {
            d->selection = turn_surface (d->selection, t, w, h);
        }
        for (size_t i = 0; i < d->channels.size (); i++)
        {
            d->channels[i].surface = turn_surface (d->channels[i].surface, t, w, h);
        }
        if (t == Turn::Cw90 || t == Turn::Ccw90)
        {
            geom::Size swapped = {d->size.height, d->size.width};
            d->size            = swapped;
        }
        /* Type turns with the canvas (continuous coordinates: x -> w - x). */
        double m[6] = {1, 0, 0, 1, 0, 0};
        if (t == Turn::FlipHorizontal)
        {
            m[0] = -1, m[4] = w;
        }
        else if (t == Turn::FlipVertical)
        {
            m[3] = -1, m[5] = h;
        }
        else if (t == Turn::Rotate180)
        {
            m[0] = -1, m[3] = -1, m[4] = w, m[5] = h;
        }
        else if (t == Turn::Cw90)
        {
            m[0] = 0, m[1] = 1, m[2] = -1, m[3] = 0, m[4] = h;
        }
        else
        {
            m[0] = 0, m[1] = -1, m[2] = 1, m[3] = 0, m[5] = w;
        }
        follow_document_transform (s, d, m);
        return e.commit (label, err);
    }

    bool cmd_flip_canvas_h (Session& s, const Value& p, Value* result, Error* err)
    {
        return canvas_turn (s, "Flip Canvas Horizontal", Turn::FlipHorizontal, err);
    }

    bool cmd_flip_canvas_v (Session& s, const Value& p, Value* result, Error* err)
    {
        return canvas_turn (s, "Flip Canvas Vertical", Turn::FlipVertical, err);
    }

    bool cmd_rotate_180 (Session& s, const Value& p, Value* result, Error* err)
    {
        return canvas_turn (s, "Rotate Canvas", Turn::Rotate180, err);
    }

    bool cmd_rotate_cw (Session& s, const Value& p, Value* result, Error* err)
    {
        return canvas_turn (s, "Rotate Canvas", Turn::Cw90, err);
    }

    bool cmd_rotate_ccw (Session& s, const Value& p, Value* result, Error* err)
    {
        return canvas_turn (s, "Rotate Canvas", Turn::Ccw90, err);
    }

    const CommandSpec LAYER_COMMANDS[] = {
        {"layer.delete", "Delete Layer", "Layer/Delete", NULL, "{\"layer\":id?} (no layer: every selected layer)",
            has_layer, cmd_layer_delete, true},
        {"layer.duplicate", "Duplicate Layer…", "Layer", NULL,
            "{\"layer\":id?,\"name\":str?} (no layer: every selected layer)", has_layer, cmd_layer_duplicate, true},
        {"layer.setProps", "Layer Properties", "", NULL,
            "{\"layer\":id?,\"name\":str?,\"visible\":bool?,\"opacity\":0..1?,\"fill\":0..1?,\"blend\":\"Multiply|…\"?,"
            "\"clipped\":bool?,\"locked\":bool?,"
            "\"locks\":{\"transparency\",\"pixels\",\"position\",\"artboard\",\"all\":bool}?,\"channels\":[bool,…]?}",
            has_layer, cmd_layer_set_props, true},
        {"layer.mergeDown", "Merge Down", "Layer", NULL, "{\"layer\":id?}", has_layer, cmd_layer_merge_down, true},
        {"layer.flattenImage", "Flatten Image", "Layer", NULL, "{}", has_doc, cmd_layer_flatten_image, true},
        {"image.imageRotation.flipCanvasHorizontal", "Flip Canvas Horizontal", "Image/Image Rotation", NULL, "{}",
            has_doc, cmd_flip_canvas_h, true},
        {"image.imageRotation.flipCanvasVertical", "Flip Canvas Vertical", "Image/Image Rotation", NULL, "{}", has_doc,
            cmd_flip_canvas_v, true},
        {"image.imageRotation.180", "180°", "Image/Image Rotation", NULL, "{}", has_doc, cmd_rotate_180, true},
        {"image.imageRotation.90cw", "90° Clockwise", "Image/Image Rotation", NULL, "{}", has_doc, cmd_rotate_cw, true},
        {"image.imageRotation.90ccw", "90° Counter Clockwise", "Image/Image Rotation", NULL, "{}", has_doc,
            cmd_rotate_ccw, true},
    };

    void register_layer_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), LAYER_COMMANDS, LAYER_COMMANDS + sizeof LAYER_COMMANDS / sizeof LAYER_COMMANDS[0]);
    }

} // namespace engine
