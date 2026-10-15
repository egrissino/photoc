#include "compose/compose.h"
#include "compose/effects.h"
#include "compose/masks.h"
#include <math.h>
#include <string.h>

namespace compose
{

    /* Where two versions of a document can composite differently, from what changed between them:
       tiles are shared copy-on-write, so an unchanged tile is the same object in both. Edits replace
       only the tiles they touch, so the difference is usually a small part of the canvas. */

    using geom::Rect;

    /* Everything a layer can draw on: its pixels, or the canvas for layers without bounds of their
       own (adjustments, fills, opaque surfaces). */
    Rect layer_extent_own (const doc::Layer& l, Rect canvas);

    Rect layer_extent (const doc::Layer& l, Rect canvas)
    {
        Rect r = layer_extent_own (l, canvas);
        if (!r.is_empty () && has_effects (l))
        {
            r = r.inflate (effects_margin (l));
        }
        return r;
    }

    Rect layer_extent_own (const doc::Layer& l, Rect canvas)
    {
        if (l.kind == doc::LayerKind::Group)
        {
            Rect acc;
            for (size_t i = 0; i < l.children.size (); i++)
            {
                acc = acc.unite (layer_extent (l.children[i], canvas));
            }
            return acc;
        }
        const raster::Surface* s = l.surface ();
        if (s == NULL)
        {
            return canvas;
        }
        float dp[raster::MAX_CHANNELS];
        s->default_pixel (dp);
        if (!s->format ().alpha || dp[s->channels () - 1] > 0.0f)
        {
            return canvas;
        }
        return s->content_bounds ();
    }

    Rect list_extent (const std::vector<doc::Layer>& layers, Rect canvas)
    {
        Rect acc;
        for (size_t i = 0; i < layers.size (); i++)
        {
            acc = acc.unite (layer_extent (layers[i], canvas));
        }
        return acc;
    }

    /* The tiles that differ between two surfaces; the whole extent of both when their format or
       default pixel differ. */
    Rect surface_damage (const raster::Surface& a, const raster::Surface& b, Rect canvas)
    {
        if (a.format () != b.format () || a.channels () != b.channels ()
            || memcmp (a.default_bytes (), b.default_bytes (), a.format ().bytes_per_pixel ()) != 0)
        {
            return canvas;
        }
        const raster::TileMap &ta = a.tiles (), &tb = b.tiles ();
        Rect acc;
        raster::TileMap::const_iterator i = ta.begin (), k = tb.begin ();
        while (i != ta.end () || k != tb.end ())
        {
            if (k == tb.end () || (i != ta.end () && i->first < k->first))
            {
                acc = acc.unite (i->first.rect ());
                ++i;
            }
            else if (i == ta.end () || k->first < i->first)
            {
                acc = acc.unite (k->first.rect ());
                ++k;
            }
            else
            {
                if (i->second != k->second)
                {
                    acc = acc.unite (i->first.rect ());
                }
                ++i;
                ++k;
            }
        }
        return acc;
    }

    /* Whether two fill layers fill the same way (pattern pixels compared by size and name). */
    bool same_fill (const doc::FillLayer& a, const doc::FillLayer& b)
    {
        if (a.kind != b.kind)
        {
            return false;
        }
        if (a.kind == doc::FillKind::Solid)
        {
            return memcmp (a.color, b.color, sizeof a.color) == 0;
        }
        if (a.kind == doc::FillKind::Gradient)
        {
            if (a.style != b.style || a.angle != b.angle || a.scale != b.scale || a.reverse != b.reverse
                || a.dither != b.dither || a.align != b.align || a.stops.size () != b.stops.size ()
                || a.opacity_stops.size () != b.opacity_stops.size ())
            {
                return false;
            }
            for (size_t i = 0; i < a.stops.size (); i++)
            {
                if (a.stops[i].location != b.stops[i].location
                    || memcmp (a.stops[i].color, b.stops[i].color, sizeof a.stops[i].color) != 0)
                {
                    return false;
                }
            }
            for (size_t i = 0; i < a.opacity_stops.size (); i++)
            {
                if (a.opacity_stops[i].input != b.opacity_stops[i].input
                    || a.opacity_stops[i].output != b.opacity_stops[i].output)
                {
                    return false;
                }
            }
            return true;
        }
        return a.pattern_name == b.pattern_name && a.pattern_w == b.pattern_w && a.pattern_h == b.pattern_h
            && a.pattern_scale == b.pattern_scale && a.pattern_angle == b.pattern_angle && a.link == b.link
            && a.phase[0] == b.phase[0] && a.phase[1] == b.phase[1] && a.pattern_rgba == b.pattern_rgba;
    }

    bool same_props (const doc::Layer& a, const doc::Layer& b)
    {
        if (memcmp (&a.blend_if, &b.blend_if, sizeof a.blend_if) != 0)
        {
            return false;
        }
        if (a.kind == doc::LayerKind::Fill && !same_fill (a.fill, b.fill))
        {
            return false;
        }
        if (a.kind != b.kind || a.visible != b.visible || a.blend != b.blend || a.opacity != b.opacity
            || a.fill_opacity != b.fill_opacity || a.clipped != b.clipped || a.excluded_channels != b.excluded_channels
            || a.has_mask != b.has_mask)
        {
            return false;
        }
        if (a.has_mask
            && (a.mask.enabled != b.mask.enabled || a.mask.density != b.mask.density
                || a.mask.feather != b.mask.feather))
        {
            return false;
        }
        if (a.has_vector_mask != b.has_vector_mask)
        {
            return false;
        }
        if (a.has_vector_mask)
        {
            const doc::VectorMask &u = a.vector_mask, &v = b.vector_mask;
            if (u.enabled != v.enabled || u.density != v.density || u.feather != v.feather
                || u.path.inverted != v.path.inverted)
            {
                return false;
            }
        }
        if (a.kind == doc::LayerKind::Adjustment && a.adjustment != b.adjustment)
        {
            return false;
        }
        return doc::same_style (a.style, b.style);
    }

    Rect list_damage (const std::vector<doc::Layer>& la, const std::vector<doc::Layer>& lb, Rect canvas)
    {
        bool same_ids = la.size () == lb.size ();
        for (size_t i = 0; same_ids && i < la.size (); i++)
        {
            same_ids = la[i].id == lb[i].id;
        }
        if (!same_ids)
        {
            return list_extent (la, canvas).unite (list_extent (lb, canvas));
        }
        Rect acc;
        for (size_t i = 0; i < la.size (); i++)
        {
            const doc::Layer &a = la[i], &b = lb[i];
            if (!same_props (a, b))
            {
                acc = acc.unite (layer_extent (a, canvas)).unite (layer_extent (b, canvas));
                continue;
            }
            Rect own;
            const raster::Surface *sa = a.surface (), *sb = b.surface ();
            if (sa != NULL && sb != NULL)
            {
                own = own.unite (surface_damage (*sa, *sb, canvas));
            }
            if (a.has_mask)
            {
                Rect m = surface_damage (a.mask.surface, b.mask.surface, canvas);
                if (!m.is_empty () && has_feather (a.mask))
                {
                    m = m.inflate ((int32_t) ceilf (feather_sigma (a.mask.feather) * 3.0f) + 2);
                }
                own = own.unite (m);
            }
            if (a.has_vector_mask)
            {
                doc::LayerMask vm = doc::vector_mask_view (a.vector_mask);
                Rect m            = surface_damage (a.vector_mask.surface, b.vector_mask.surface, canvas);
                if (!m.is_empty () && has_feather (vm))
                {
                    m = m.inflate ((int32_t) ceilf (feather_sigma (vm.feather) * 3.0f) + 2);
                }
                own = own.unite (m);
            }
            own = own.unite (list_damage (a.children, b.children, canvas));
            /* A styled layer's effects reach past what changed. */
            if (!own.is_empty () && has_effects (a))
            {
                own = own.inflate (2 * effects_margin (a));
            }
            acc = acc.unite (own);
        }
        return acc;
    }

    /* The area where `b` can composite differently from `a`; false when they don't share a canvas
       (a size or mode change redraws everything). */
    bool composite_damage (const doc::Document& a, const doc::Document& b, Rect* out)
    {
        if (a.size.width != b.size.width || a.size.height != b.size.height || a.mode != b.mode || a.depth != b.depth)
        {
            return false;
        }
        if (a.light_angle != b.light_angle || a.light_altitude != b.light_altitude)
        {
            return false;
        }
        Rect canvas = b.bounds ();
        *out        = &a == &b ? Rect () : list_damage (a.layers, b.layers, canvas).intersect (canvas);
        return true;
    }

} // namespace compose
