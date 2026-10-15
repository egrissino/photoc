#include "algo/select_ops.h"
#include "algo/selection.h"
#include "color/blend.h"
#include "engine/layers.h"
#include "engine/retouch.h"
#include "engine/session.h"
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <strings.h>

namespace engine
{

    /* Select > marquee shapes, Edit > Fill and Clear, and the toolbox colours. */

    using color::BlendMode;
    using color::PixelFormat;
    using geom::Rect;

    /* A colour param: [r, g, b(, a)] in 0..1 or "#rrggbb(aa)"; `fallback` when absent or unreadable. */
    void color_param (const Value& p, const char* key, const float fallback[4], float out[4])
    {
        memcpy (out, fallback, sizeof (float) * 4);
        const Value* v = param (p, key);
        if (v == NULL)
        {
            return;
        }
        if (v->is_array () && v->size () >= 3)
        {
            for (size_t i = 0; i < 4; i++)
            {
                const Value* c = i < v->size () ? &(*v)[i] : NULL;
                out[i]         = c != NULL && c->is_number () ? (float) c->get<double> () : (i == 3 ? 1.0f : 0.0f);
            }
        }
        else if (v->is_string ())
        {
            parse_hex (v->get_ref<const std::string&> ().c_str (), out);
        }
    }

    /* An integer coordinate; floats are floored, out-of-range values clamped. */
    bool coord_param (const Value& p, const char* cmd, const char* key, int32_t* out, Error* err)
    {
        double d;
        if (!param_f64 (p, key, &d))
        {
            return bad_params (err, cmd, "missing `%s`", key);
        }
        if (!isfinite (d))
        {
            return bad_params (err, cmd, "`%s` must be a finite number", key);
        }
        d = floor (d);
        if (d < (double) INT32_MIN)
        {
            d = (double) INT32_MIN;
        }
        if (d > (double) INT32_MAX)
        {
            d = (double) INT32_MAX;
        }
        *out = (int32_t) d;
        return true;
    }

    /* A new selection shape combined with the current selection by `mode` (replace, add, subtract,
       intersect). Nothing left on the canvas deselects. */
    void apply_selection (doc::Document* d, const raster::Surface& shape, const std::string& mode)
    {
        Rect area = d->bounds ();
        raster::Surface combined (PixelFormat::gray8 ());
        bool had = d->has_selection;
        if (mode == "add" && had)
        {
            combined = algo::combine_surfaces (d->selection, shape, area, algo::select_max);
        }
        else if (mode == "subtract" && had)
        {
            combined = algo::combine_surfaces (d->selection, shape, area, algo::select_subtract);
        }
        else if (mode == "intersect" && had)
        {
            combined = algo::combine_surfaces (d->selection, shape, area, algo::select_min);
        }
        else if (mode == "subtract" || mode == "intersect")
        {
            combined = raster::Surface (PixelFormat::gray8 ());
        }
        else
        {
            combined = shape;
        }
        d->has_selection = !combined.content_bounds ().intersect (area).is_empty ();
        d->selection     = d->has_selection ? combined : raster::Surface (PixelFormat::gray8 ());
    }

    /* Rectangular / Elliptical / Single Row / Single Column Marquee. A marquee dragged past the
       canvas stops at its edge; the ellipse keeps its shape and is only cut there. */
    bool cmd_select_rect (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "select.rect";
        int32_t x, y, w, h;
        if (!coord_param (p, cmd, "x", &x, err) || !coord_param (p, cmd, "y", &y, err)
            || !coord_param (p, cmd, "width", &w, err) || !coord_param (p, cmd, "height", &h, err))
        {
            return false;
        }
        Rect r           = Rect::from_xywh (x, y, (uint32_t) (w < 0 ? 0 : w), (uint32_t) (h < 0 ? 0 : h));
        std::string mode = "replace";
        param_str (p, "mode", &mode);
        bool ellipse = false;
        param_bool (p, "ellipse", &ellipse);
        bool aa = true;
        param_bool (p, "antiAlias", &aa);
        double feather = 0.0;
        if (param_present (p, "feather")
            && (!param_f64 (p, "feather", &feather) || !(feather >= 0.0 && feather <= 1000.0)))
        {
            return bad_params (err, cmd, "feather must be a number in 0..1000");
        }
        const char* step;
        std::string line;
        if (param_str (p, "line", &line))
        {
            if (line == "row")
            {
                step = "Single Row Marquee";
            }
            else if (line == "column")
            {
                step = "Single Column Marquee";
            }
            else
            {
                return bad_params (err, cmd, "unknown `line` \"%s\" (row|column)", line.c_str ());
            }
        }
        else
        {
            step = ellipse ? "Elliptical Marquee" : "Rectangular Marquee";
        }

        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        Rect area = d->bounds ();
        raster::Surface shape (PixelFormat::gray8 ());
        if (ellipse)
        {
            shape = algo::ellipse_surface (r, area, aa);
        }
        else
        {
            float one = 1.0f;
            shape.fill_rect (r.intersect (area), &one);
        }
        if (feather > 0.0)
        {
            std::vector<float> m = algo::mask_from_surface (&shape, area);
            shape =
                algo::mask_to_surface (algo::feather_mask (m, area.width (), area.height (), (float) feather), area);
        }
        apply_selection (d, shape, mode);
        return e.commit (step, err);
    }

    /* One tile of `surf` (bytes of a tile at `origin`) blended with the fill colour over `tr`,
       weighted by the selection when there is one. */
    void fill_tile (uint8_t* bytes, PixelFormat fmt, Rect origin, Rect tr, const raster::Surface* sel,
        const FillBlend& b, const color::CmykSpace* cmyk)
    {
        size_t n        = fmt.channels ();
        size_t bpp      = fmt.bytes_per_pixel ();
        BlendMode blend = b.mode == BlendMode::Dissolve ? BlendMode::Normal : b.mode;
        float px[raster::MAX_CHANNELS];
        float out[raster::MAX_CHANNELS];
        for (int32_t y = tr.y0; y < tr.y1; y++)
        {
            size_t row = (size_t) (y - origin.y0) * (size_t) geom::TILE_SIZE;
            for (int32_t x = tr.x0; x < tr.x1; x++)
            {
                float k = (sel != NULL ? sel->sample_channel (x, y, 0) : 1.0f) * b.opacity;
                if (k <= 0.0f)
                {
                    continue;
                }
                uint8_t* pb = bytes + (row + (size_t) (x - origin.x0)) * bpp;
                for (size_t c = 0; c < n; c++)
                {
                    px[c] = color::read_sample (pb, fmt.sample, c);
                }
                float d[4];
                raster::to_rgba (fmt, px, cmyk, d);
                float c[4] = {b.color[0], b.color[1], b.color[2], b.color[3]};
                if (b.mode == BlendMode::Dissolve)
                {
                    k    = color::dither_noise (x, y) < c[3] * k ? 1.0f : 0.0f;
                    c[3] = 1.0f;
                    if (k <= 0.0f)
                    {
                        continue;
                    }
                }
                float o[4];
                color::composite (blend, d, c, k, o);
                if (b.keep_alpha || !fmt.alpha)
                {
                    o[3] = d[3];
                }
                raster::from_rgba (fmt, o, cmyk, out);
                raster::encode_pixel (fmt, out, pb);
            }
        }
    }

    /* `area` of `surf` filled through the selection. An opaque Normal fill at 100% without a
       selection becomes shared solid tiles (copy-on-write), which costs nothing per pixel. */
    void fill_surface (
        raster::Surface* surf, Rect area, const raster::Surface* sel, const FillBlend& b, const color::CmykSpace* cmyk)
    {
        PixelFormat fmt = surf->format ();
        if (sel == NULL && b.color[3] >= 1.0f && b.mode == BlendMode::Normal && b.opacity >= 1.0f
            && (!b.keep_alpha || !fmt.alpha))
        {
            float px[raster::MAX_CHANNELS];
            raster::from_rgba (fmt, b.color, cmyk, px);
            surf->fill_rect (area, px);
            return;
        }
        std::vector<geom::TileCoord> coords = area.tiles ();
        std::vector<raster::Tile*> tiles;
        for (size_t i = 0; i < coords.size (); i++)
        {
            tiles.push_back (surf->tile_mut (coords[i]));
        }
#pragma omp parallel for schedule(dynamic)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) coords.size (); i++)
        {
            Rect origin = coords[(size_t) i].rect ();
            fill_tile (tiles[(size_t) i]->data.data (), fmt, origin, origin.intersect (area), sel, b, cmyk);
        }
        surf->prune ();
    }

    const char* const FILL_CONTENTS[] = {
        "foreground", "background", "color", "contentAware", "pattern", "history", "black", "gray", "white", NULL};

    /* The layer's pixels, or an error naming what the layer is instead. */
    raster::Surface* pixel_target (doc::Document* d, doc::LayerId id, Error* err)
    {
        doc::Layer* l = d->layer_mut (id);
        if (l == NULL)
        {
            fail (err, ErrorKind::NoLayer, "no such layer %llu", (unsigned long long) id);
            return NULL;
        }
        if (!check_pixels_unlocked (*d, id, err))
        {
            return NULL;
        }
        raster::Surface* s = l->surface_mut ();
        if (s == NULL)
        {
            fail (err, ErrorKind::Other, "the active layer is %s %s layer, not a pixel layer",
                doc::layer_kind_article (l->kind), doc::layer_kind_name (l->kind));
            return NULL;
        }
        return s;
    }

    /* Edit > Fill: the selection (or the whole canvas) filled with Foreground / Background / Color
       / Black / 50% Gray / White, blended with `mode` at `opacity` (percent). Pattern and History
       contents are not ported yet; Content-Aware is out of this build's scope. */
    bool cmd_edit_fill (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd      = "edit.fill";
        std::string contents = "color";
        if (param_present (p, "contents"))
        {
            std::string c;
            if (!param_str (p, "contents", &c))
            {
                return bad_params (err, cmd, "`contents` must be a string");
            }
            bool found = false;
            for (size_t i = 0; FILL_CONTENTS[i] != NULL; i++)
            {
                if (strcasecmp (c.c_str (), FILL_CONTENTS[i]) == 0)
                {
                    contents = FILL_CONTENTS[i];
                    found    = true;
                }
            }
            if (!found)
            {
                return bad_params (err, cmd, "unknown contents `%s`", c.c_str ());
            }
        }
        FillBlend b;
        b.mode = BlendMode::Normal;
        if (param_present (p, "mode"))
        {
            std::string m;
            if (!param_str (p, "mode", &m) || !blend_from_str (m.c_str (), &b.mode) || b.mode == BlendMode::PassThrough)
            {
                return bad_params (err, cmd, "unknown blend mode");
            }
        }
        b.opacity = 1.0f;
        if (param_present (p, "opacity"))
        {
            double o;
            if (!param_f64 (p, "opacity", &o) || !isfinite (o))
            {
                return bad_params (err, cmd, "`opacity` must be a number 0..100");
            }
            b.opacity = (float) ((o < 0.0 ? 0.0 : o > 100.0 ? 100.0 : o) / 100.0);
        }
        bool preserve = false;
        if (param_present (p, "preserveTransparency") && !param_bool (p, "preserveTransparency", &preserve))
        {
            return bad_params (err, cmd, "`preserveTransparency` must be true or false");
        }

        float level = -1.0f;
        if (contents == "black")
        {
            level = 0.0f;
        }
        if (contents == "gray")
        {
            level = 128.0f / 255.0f;
        }
        if (contents == "white")
        {
            level = 1.0f;
        }
        if (level >= 0.0f)
        {
            b.color[0] = b.color[1] = b.color[2] = level;
            b.color[3]                           = 1.0f;
        }
        else if (contents == "foreground")
        {
            memcpy (b.color, s.tools.foreground, sizeof b.color);
        }
        else if (contents == "background")
        {
            memcpy (b.color, s.tools.background, sizeof b.color);
        }
        else if (contents == "color")
        {
            color_param (p, "color", s.tools.foreground, b.color);
        }
        else if (contents == "contentAware" || contents == "pattern")
        {
            doc::LayerId lid;
            if (!layer_param (s, p, &lid, err))
            {
                return false;
            }
            Edit e (s);
            if (e.doc () == NULL)
            {
                return fail (err, ErrorKind::NoDocument, "no active document");
            }
            bool ok = contents == "pattern" ? fill_pattern (s, &e, lid, p, b.opacity, b.mode, err)
                                            : fill_content_aware (s, &e, lid, b.opacity, err);
            if (!ok)
            {
                return false;
            }
            *result = Value{{"contents", contents}};
            return e.commit ("Fill", err);
        }
        else
        {
            return fail (err, ErrorKind::Other, "%s fill is not ported yet", contents.c_str ());
        }

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
        Rect area = (d->has_selection ? d->selection.content_bounds () : d->bounds ()).intersect (d->bounds ());
        *result   = Value{{"contents", contents}};
        if (area.is_empty ())
        {
            return true;
        }
        raster::Surface* surf = pixel_target (d, id, err);
        if (surf == NULL)
        {
            return false;
        }
        b.keep_alpha = preserve || d->effective_locks (id).transparency;
        fill_surface (surf, area, d->has_selection ? &d->selection : NULL, b, s.cmyk);
        return e.commit ("Fill", err);
    }

    /* The band Edit > Stroke paints around the edge of mask `m` (w x h): `width` pixels inside it,
       outside it, or centred on it. The mask is padded with unselected pixels so an edge on the
       canvas border strokes too. */
    std::vector<float> stroke_band (
        const std::vector<float>& m, size_t w, size_t h, float width, const std::string& location)
    {
        size_t pad = (size_t) ceilf (width) + 2;
        size_t pw = w + 2 * pad, ph = h + 2 * pad;
        std::vector<float> padded (pw * ph, 0.0f);
        for (size_t y = 0; y < h; y++)
        {
            memcpy (&padded[(y + pad) * pw + pad], &m[y * w], w * sizeof (float));
        }
        std::vector<float> grown, shrunk;
        if (location == "inside")
        {
            grown  = padded;
            shrunk = algo::contract_mask (padded, pw, ph, width);
        }
        else if (location == "outside")
        {
            grown  = algo::expand_mask (padded, pw, ph, width);
            shrunk = padded;
        }
        else
        {
            grown  = algo::expand_mask (padded, pw, ph, width * 0.5f);
            shrunk = algo::contract_mask (padded, pw, ph, width * 0.5f);
        }
        std::vector<float> out (w * h);
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                size_t i       = (y + pad) * pw + pad + x;
                float v        = grown[i] - shrunk[i];
                out[y * w + x] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            }
        }
        return out;
    }

    /* Edit > Stroke: a band around the selection's edge (or, with no selection, around the
       layer's opaque content) filled with a colour, blended with `mode` at `opacity` percent. */
    bool cmd_edit_stroke (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "edit.stroke";
        double width    = 1.0;
        if (param_present (p, "width") && (!param_f64 (p, "width", &width) || !(width >= 1.0 && width <= 250.0)))
        {
            return bad_params (err, cmd, "`width` must be a number in 1..250");
        }
        std::string location = "center";
        param_str (p, "location", &location);
        if (location != "inside" && location != "center" && location != "outside")
        {
            return bad_params (err, cmd, "`location` must be inside, center or outside");
        }
        FillBlend b;
        b.mode = BlendMode::Normal;
        if (param_present (p, "mode"))
        {
            std::string m;
            if (!param_str (p, "mode", &m) || !blend_from_str (m.c_str (), &b.mode) || b.mode == BlendMode::PassThrough)
            {
                return bad_params (err, cmd, "unknown blend mode");
            }
        }
        b.opacity = 1.0f;
        double o;
        if (param_f64 (p, "opacity", &o) && isfinite (o))
        {
            b.opacity = (float) ((o < 0.0 ? 0.0 : o > 100.0 ? 100.0 : o) / 100.0);
        }
        bool preserve = false;
        param_bool (p, "preserveTransparency", &preserve);
        color_param (p, "color", s.tools.foreground, b.color);

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
        Rect area             = d->bounds ();
        raster::Surface* surf = pixel_target (d, id, err);
        if (surf == NULL)
        {
            return false;
        }
        std::vector<float> m;
        if (d->has_selection)
        {
            m = algo::mask_from_surface (&d->selection, area);
        }
        else
        {
            std::vector<float> px ((size_t) area.width () * area.height () * 4);
            surf->read_rgba (area, s.cmyk, px.data ());
            m.resize (px.size () / 4);
            for (size_t i = 0; i < m.size (); i++)
            {
                m[i] = px[i * 4 + 3];
            }
            if (d->is_background (id))
            {
                return fail (err, ErrorKind::Other, "make a selection to stroke on the Background");
            }
        }
        raster::Surface band =
            algo::mask_to_surface (stroke_band (m, area.width (), area.height (), (float) width, location), area);
        Rect r  = band.content_bounds ().intersect (area);
        *result = Value{{"stroked", !r.is_empty ()}};
        if (r.is_empty ())
        {
            return true;
        }
        b.keep_alpha = preserve || d->effective_locks (id).transparency;
        fill_surface (surf, r, &band, b, s.cmyk);
        return e.commit ("Stroke", err);
    }

    /* Edit > Clear (and Cut, Layer via Cut): transparency through the selection, or the background
       colour on the Background layer (which can't be transparent). */
    bool clear_selected (
        doc::Document* d, doc::LayerId id, const float background[4], const color::CmykSpace* cmyk, Error* err)
    {
        Rect area  = (d->has_selection ? d->selection.content_bounds () : d->bounds ()).intersect (d->bounds ());
        bool is_bg = d->is_background (id);
        raster::Surface* surf = pixel_target (d, id, err);
        if (surf == NULL)
        {
            return false;
        }
        const raster::Surface* sel = d->has_selection ? &d->selection : NULL;
        if (is_bg)
        {
            FillBlend b = {BlendMode::Normal, 1.0f, true, {background[0], background[1], background[2], 1.0f}};
            fill_surface (surf, area, sel, b, cmyk);
        }
        else if (sel == NULL)
        {
            float dp[raster::MAX_CHANNELS];
            surf->default_pixel (dp);
            surf->fill_rect (area, dp);
        }
        else
        {
            PixelFormat fmt                     = surf->format ();
            size_t n                            = fmt.channels ();
            std::vector<geom::TileCoord> coords = area.tiles ();
            std::vector<raster::Tile*> tiles;
            for (size_t i = 0; i < coords.size (); i++)
            {
                tiles.push_back (surf->tile_mut (coords[i]));
            }
#pragma omp parallel for schedule(dynamic)
            for (ptrdiff_t i = 0; i < (ptrdiff_t) coords.size (); i++)
            {
                Rect origin    = coords[(size_t) i].rect ();
                Rect tr        = origin.intersect (area);
                uint8_t* bytes = tiles[(size_t) i]->data.data ();
                for (int32_t y = tr.y0; y < tr.y1; y++)
                {
                    for (int32_t x = tr.x0; x < tr.x1; x++)
                    {
                        float k = sel->sample_channel (x, y, 0);
                        if (k <= 0.0f)
                        {
                            continue;
                        }
                        size_t at =
                            ((size_t) (y - origin.y0) * (size_t) geom::TILE_SIZE + (size_t) (x - origin.x0)) * n;
                        float a = color::read_sample (bytes, fmt.sample, at + n - 1) * (1.0f - k);
                        if (a <= 0.0f)
                        {
                            for (size_t c = 0; c < n; c++)
                            {
                                color::write_sample (bytes, fmt.sample, at + c, 0.0f);
                            }
                        }
                        else
                        {
                            color::write_sample (bytes, fmt.sample, at + n - 1, a);
                        }
                    }
                }
            }
        }
        surf->prune ();
        return true;
    }

    bool cmd_edit_clear (Session& s, const Value& p, Value* result, Error* err)
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
        if (!clear_selected (d, id, s.tools.background, s.cmyk, err))
        {
            return false;
        }
        return e.commit ("Clear", err);
    }

    bool cmd_tools_set_colors (Session& s, const Value& p, Value* result, Error* err)
    {
        float fg[4], bg[4];
        color_param (p, "foreground", s.tools.foreground, fg);
        color_param (p, "background", s.tools.background, bg);
        memcpy (s.tools.foreground, fg, sizeof fg);
        memcpy (s.tools.background, bg, sizeof bg);
        return true;
    }

    bool cmd_tools_swap_colors (Session& s, const Value& p, Value* result, Error* err)
    {
        float t[4];
        memcpy (t, s.tools.foreground, sizeof t);
        memcpy (s.tools.foreground, s.tools.background, sizeof t);
        memcpy (s.tools.background, t, sizeof t);
        return true;
    }

    bool cmd_tools_default_colors (Session& s, const Value& p, Value* result, Error* err)
    {
        float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        memcpy (s.tools.foreground, black, sizeof black);
        memcpy (s.tools.background, white, sizeof white);
        return true;
    }

    const CommandSpec EDIT_COMMANDS[] = {
        {"select.rect", "Rectangular Selection", "", NULL,
            "{\"x\":i32,\"y\":i32,\"width\":u32,\"height\":u32,\"mode\":\"replace|add|subtract|intersect\"=\"replace\","
            "\"ellipse\":bool=false,"
            "\"line\":\"row|column\"?,\"antiAlias\":bool=true}",
            has_doc, cmd_select_rect, true},
        {"edit.fill", "Fill…", "Edit", "Shift+F5",
            "{\"contents\":\"foreground|background|color|black|gray|white\"=\"color\",\"color\":\"#rrggbb|[r,g,b,a]\"="
            "foreground,"
            "\"mode\":\"normal|multiply|…\"=\"normal\",\"opacity\":0..100=100,\"preserveTransparency\":bool=false,"
            "\"layer\":id?}",
            has_layer, cmd_edit_fill, true},
        {"edit.stroke", "Stroke…", "Edit", NULL,
            "{\"width\":1..250=1,\"color\":\"#rrggbb|[r,g,b,a]\"=foreground,\"location\":\"inside|center|outside\"="
            "\"center\",\"mode\":\"normal|multiply|…\"=\"normal\","
            "\"opacity\":0..100=100,\"preserveTransparency\":bool=false,\"layer\":id?}",
            has_layer, cmd_edit_stroke, true},
        {"edit.clear", "Clear", "Edit", "Delete", "{\"layer\":id?}", has_layer, cmd_edit_clear, true},
        {"tools.setColors", "Set Colors", "", NULL, "{\"foreground\":\"#rrggbb\"?,\"background\":\"#rrggbb\"?}", always,
            cmd_tools_set_colors, true},
        {"tools.swapColors", "Switch Foreground and Background Colors", "", "X", "{}", always, cmd_tools_swap_colors,
            true},
        {"tools.defaultColors", "Default Foreground and Background Colors", "", "D", "{}", always,
            cmd_tools_default_colors, true},
    };

    void register_edit_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), EDIT_COMMANDS, EDIT_COMMANDS + sizeof EDIT_COMMANDS / sizeof EDIT_COMMANDS[0]);
    }

} // namespace engine
