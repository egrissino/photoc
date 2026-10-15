#include "compose/masks.h"
#include "engine/layers.h"
#include "engine/session.h"
#include <math.h>
#include <string.h>

namespace engine
{

    /* Layer > Layer Mask. Masks are grayscale at the document's depth (white reveals); density and
       feather are non-destructive properties. */

    using geom::Rect;

    /* The mask's surface in the document's sample depth. */
    doc::LayerMask mask_at_depth (doc::LayerMask m, color::SampleType depth)
    {
        color::PixelFormat fmt = m.surface.format ().with_sample (depth);
        if (m.surface.format () != fmt)
        {
            m.surface = m.surface.convert (fmt, NULL);
        }
        return m;
    }

    /* The Background can't have a mask: it becomes "Layer 0" first, as in Photoshop. */
    void background_to_layer (doc::Document* d, doc::LayerId id)
    {
        if (!d->is_background (id))
        {
            return;
        }
        doc::Layer* l         = d->layer_mut (id);
        l->name               = "Layer 0";
        l->locks.transparency = false;
        l->locks.position     = false;
    }

    /* The layers a new mask goes on: the "layer" param or the active layer, or with several layers
       selected those that can take one (no mask yet, not the Background). */
    bool mask_targets (const Session& s, const Value& p, std::vector<doc::LayerId>* out, Error* err)
    {
        if (!layer_targets (s, p, out, err))
        {
            return false;
        }
        if (out->size () < 2)
        {
            return true;
        }
        const doc::Document& d = *s.active ()->doc;
        std::vector<doc::LayerId> fit;
        for (size_t i = 0; i < out->size (); i++)
        {
            const doc::Layer* l = d.layer ((*out)[i]);
            if (l != NULL && !l->has_mask && !d.is_background (l->id))
            {
                fit.push_back (l->id);
            }
        }
        if (fit.empty ())
        {
            return fail (err, ErrorKind::Other, "none of the selected layers can take a layer mask");
        }
        *out = fit;
        return true;
    }

    bool add_mask (Session& s, const Value& p, const doc::LayerMask& mask, bool deselect, Error* err)
    {
        std::vector<doc::LayerId> ids;
        if (!mask_targets (s, p, &ids, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::LayerMask m = mask_at_depth (mask, d->depth);
        for (size_t i = 0; i < ids.size (); i++)
        {
            background_to_layer (d, ids[i]);
            doc::Layer* l = d->layer_mut (ids[i]);
            if (l == NULL)
            {
                return no_layer (err, ids[i]);
            }
            l->has_mask = true;
            l->mask     = m;
        }
        if (deselect)
        {
            d->has_selection = false;
            d->selection     = raster::Surface (color::PixelFormat::gray8 ());
        }
        return e.commit ("Add Layer Mask", err);
    }

    bool cmd_mask_reveal_all (Session& s, const Value& p, Value* result, Error* err)
    {
        return add_mask (s, p, doc::LayerMask::reveal_all (), false, err);
    }

    bool cmd_mask_hide_all (Session& s, const Value& p, Value* result, Error* err)
    {
        return add_mask (s, p, doc::LayerMask::hide_all (), false, err);
    }

    /* The selection's grayscale values over the canvas, inverted for Hide Selection. */
    raster::Surface selection_mask (const doc::Document& d, bool invert)
    {
        float fill          = invert ? 1.0f : 0.0f;
        raster::Surface out = raster::Surface::with_default (color::PixelFormat::gray8 (), &fill);
        Rect bounds         = d.bounds ();
        std::vector<float> v;
        if (!d.selection.read_region (bounds, &v))
        {
            return out;
        }
        size_t ch = d.selection.channels ();
        size_t n  = v.size () / ch;
        std::vector<float> g (n);
        for (size_t i = 0; i < n; i++)
        {
            g[i] = invert ? 1.0f - v[i * ch] : v[i * ch];
        }
        out.write_region (bounds, g.data (), g.size ());
        out.prune ();
        return out;
    }

    bool cmd_mask_reveal_selection (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerMask m = doc::LayerMask::reveal_all ();
        m.surface        = selection_mask (*s.active ()->doc, false);
        return add_mask (s, p, m, false, err);
    }

    bool cmd_mask_hide_selection (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerMask m = doc::LayerMask::reveal_all ();
        m.surface        = selection_mask (*s.active ()->doc, true);
        return add_mask (s, p, m, true, err);
    }

    bool cmd_mask_delete (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        if (!l->has_mask)
        {
            return fail (err, ErrorKind::Other, "the layer has no layer mask");
        }
        l->has_mask = false;
        l->mask     = doc::LayerMask::reveal_all ();
        return e.commit ("Delete Layer Mask", err);
    }

    /* Apply: the mask multiplies into the layer's alpha and goes away. */
    bool cmd_mask_apply (Session& s, const Value& p, Value* result, Error* err)
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
        if (!l->has_mask)
        {
            return fail (err, ErrorKind::Other, "the layer has no layer mask");
        }
        if (!check_pixels_unlocked (*d, id, err))
        {
            return false;
        }
        raster::Surface* surf = l->surface_mut ();
        if (surf == NULL)
        {
            return fail (err, ErrorKind::Other, "not a pixel layer");
        }
        Rect r                 = surf->content_bounds ();
        color::PixelFormat fmt = surf->format ();
        if (!r.is_empty () && fmt.alpha)
        {
            /* A feathered mask bakes the softened coverage the canvas shows. */
            std::vector<float> px, m;
            bool have = compose::has_feather (l->mask)
                          ? compose::feathered_mask (l->mask, d->bounds ().unite (r)).read_region (r, &m)
                          : l->mask.values (r, &m);
            if (surf->read_region (r, &px) && have)
            {
                size_t n = fmt.channels ();
                for (size_t i = 0; i < m.size (); i++)
                {
                    px[i * n + n - 1] *= m[i];
                }
                surf->write_region (r, px.data (), px.size ());
                surf->prune ();
            }
        }
        l->has_mask = false;
        l->mask     = doc::LayerMask::reveal_all ();
        return e.commit ("Apply Layer Mask", err);
    }

    /* From Transparency: the layer's alpha becomes its mask and the pixels go opaque. */
    bool cmd_mask_from_transparency (Session& s, const Value& p, Value* result, Error* err)
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
        if (!check_pixels_unlocked (*d, id, err))
        {
            return false;
        }
        raster::Surface* surf = l->surface_mut ();
        if (surf == NULL)
        {
            return fail (err, ErrorKind::Other, "not a pixel layer");
        }
        color::PixelFormat fmt = surf->format ();
        doc::LayerMask mask    = doc::LayerMask::hide_all ();
        Rect r                 = surf->content_bounds ();
        if (!r.is_empty () && fmt.alpha)
        {
            std::vector<float> px;
            if (surf->read_region (r, &px))
            {
                size_t n     = fmt.channels ();
                size_t count = px.size () / n;
                std::vector<float> alpha (count);
                for (size_t i = 0; i < count; i++)
                {
                    alpha[i]          = px[i * n + n - 1];
                    px[i * n + n - 1] = 1.0f;
                }
                mask.surface.write_region (r, alpha.data (), alpha.size ());
                mask.surface.prune ();
                surf->write_region (r, px.data (), px.size ());
            }
        }
        l->has_mask = true;
        l->mask     = mask_at_depth (mask, d->depth);
        return e.commit ("From Transparency", err);
    }

    /* Properties > Masks: density (percent) and feather (pixels); the mask pixels stay as they are. */
    bool cmd_mask_edit (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.layerMask.edit";
        double density = -1.0, feather = -1.0;
        if (param_present (p, "density")
            && (!param_f64 (p, "density", &density) || !isfinite (density) || density < 0.0 || density > 100.0))
        {
            return bad_params (err, cmd, "`density` must be a finite number within 0..100");
        }
        if (param_present (p, "feather")
            && (!param_f64 (p, "feather", &feather) || !isfinite (feather) || feather < 0.0 || feather > 1000.0))
        {
            return bad_params (err, cmd, "`feather` must be a finite number within 0..1000");
        }
        if (density < 0.0 && feather < 0.0)
        {
            return bad_params (err, cmd, "density or feather is required");
        }
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        const doc::Layer* cur = s.active ()->doc->layer (id);
        if (cur == NULL)
        {
            return no_layer (err, id);
        }
        if (!cur->has_mask)
        {
            return bad_params (err, cmd, "the target layer has no pixel mask");
        }
        float dn = density >= 0.0 ? (float) (density / 100.0) : cur->mask.density;
        float fe = feather >= 0.0 ? (float) feather : cur->mask.feather;
        if (dn != cur->mask.density || fe != cur->mask.feather)
        {
            Edit e (s);
            doc::Layer* l   = e.doc ()->layer_mut (id);
            l->mask.density = dn;
            l->mask.feather = fe;
            if (!e.commit ("Edit Layer Mask", err))
            {
                return false;
            }
        }
        *result = Value{{"layer", id}, {"density", dn * 100.0f}, {"feather", fe}};
        return true;
    }

    bool toggle_mask (Session& s, const Value& p, const char* key, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        bool enabled_key = strcmp (key, "enabled") == 0;
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        if (!l->has_mask)
        {
            return fail (err, ErrorKind::Other, "the layer has no layer mask");
        }
        bool* slot = enabled_key ? &l->mask.enabled : &l->mask.linked;
        bool v;
        *slot    = param_bool (p, key, &v) ? v : !*slot;
        bool now = *slot;
        if (!e.commit (enabled_key ? "Enable/Disable Layer Mask" : "Link/Unlink Layer Mask", err))
        {
            return false;
        }
        *result = Value{{key, now}};
        return true;
    }

    bool cmd_mask_enabled (Session& s, const Value& p, Value* result, Error* err)
    {
        return toggle_mask (s, p, "enabled", result, err);
    }

    bool cmd_mask_linked (Session& s, const Value& p, Value* result, Error* err)
    {
        return toggle_mask (s, p, "linked", result, err);
    }

    bool has_mask (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d = s.active ();
        if (d->doc->layer (d->active_layer)->has_mask)
        {
            return true;
        }
        *why = "the active layer has no layer mask";
        return false;
    }

    bool has_raster_layer (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d = s.active ();
        if (d->doc->layer (d->active_layer)->kind == doc::LayerKind::Raster)
        {
            return true;
        }
        *why = "the active layer is not a pixel layer";
        return false;
    }

    bool has_raster_with_mask (const Session& s, std::string* why)
    {
        return has_raster_layer (s, why) && has_mask (s, why);
    }

    bool has_selection_layer (const Session& s, std::string* why)
    {
        return has_layer (s, why) && has_selection (s, why);
    }

    const CommandSpec MASK_COMMANDS[] = {
        {"layer.layerMask.revealAll", "Reveal All", "Layer/Layer Mask", NULL,
            "{\"layer\":id?} (no layer: every selected layer that can take a mask)", has_layer, cmd_mask_reveal_all,
            true},
        {"layer.layerMask.hideAll", "Hide All", "Layer/Layer Mask", NULL,
            "{\"layer\":id?} (no layer: every selected layer that can take a mask)", has_layer, cmd_mask_hide_all,
            true},
        {"layer.layerMask.revealSelection", "Reveal Selection", "Layer/Layer Mask", NULL,
            "{\"layer\":id?} (no layer: every selected layer that can take a mask)", has_selection_layer,
            cmd_mask_reveal_selection, true},
        {"layer.layerMask.hideSelection", "Hide Selection", "Layer/Layer Mask", NULL,
            "{\"layer\":id?} (no layer: every selected layer that can take a mask)", has_selection_layer,
            cmd_mask_hide_selection, true},
        {"layer.layerMask.fromTransparency", "From Transparency", "Layer/Layer Mask", NULL, "{\"layer\":id?}",
            has_raster_layer, cmd_mask_from_transparency, true},
        {"layer.layerMask.delete", "Delete", "Layer/Layer Mask", NULL, "{\"layer\":id?}", has_layer, cmd_mask_delete,
            true},
        {"layer.layerMask.apply", "Apply", "Layer/Layer Mask", NULL, "{\"layer\":id?}", has_raster_with_mask,
            cmd_mask_apply, true},
        {"layer.layerMask.edit", "Edit Layer Mask", "", NULL,
            "{\"layer\":id?,\"density\":0..100?,\"feather\":0..1000? (pixels)} → {layer,density,feather}", has_doc,
            cmd_mask_edit, true},
        {"layer.layerMask.enabled", "Disable Layer Mask", "Layer/Layer Mask", NULL,
            "{\"layer\":id?,\"enabled\":bool? (default: toggle)}", has_mask, cmd_mask_enabled, true},
        {"layer.layerMask.linked", "Unlink Layer Mask", "Layer/Layer Mask", NULL,
            "{\"layer\":id?,\"linked\":bool? (default: toggle)}", has_mask, cmd_mask_linked, true},
    };

    void register_mask_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), MASK_COMMANDS, MASK_COMMANDS + sizeof MASK_COMMANDS / sizeof MASK_COMMANDS[0]);
    }

} // namespace engine
