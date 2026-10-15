#include "algo/paint_fill.h"
#include "algo/select_ops.h"
#include "compose/compose.h"
#include "engine/brush.h"
#include "engine/layers.h"
#include "engine/session.h"
#include <algorithm>
#include <math.h>
#include <string.h>

namespace engine
{

    /* The Paint Bucket and the Gradient tool, on the layer's pixels or (with "target": "mask") its
       mask. Gradient presets and pattern fills are not ported yet: a gradient is the foreground to
       the background colour, "colors" evenly spaced, or explicit "stops", with optional
       "transparency" stops. */

    using geom::Rect;

    void color_param (const Value& p, const char* key, const float fallback[4], float out[4]);

    float fnum (const Value& p, const char* key, float fallback)
    {
        double v;
        return param_f64 (p, key, &v) ? (float) v : fallback;
    }

    bool fflag (const Value& p, const char* key, bool fallback)
    {
        bool v;
        return param_bool (p, key, &v) ? v : fallback;
    }

    /* 8-bit RGBA of a surface, or of the composite, over the canvas. */
    std::vector<uint8_t> sample_rgba8 (const Session& s, const doc::Document& d, const raster::Surface* surf)
    {
        Rect area = d.bounds ();
        std::vector<uint8_t> img ((size_t) area.width () * area.height () * 4);
        if (surf != NULL)
        {
            surf->read_rgba8 (area, s.cmyk, img.data ());
            return img;
        }
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = s.cmyk;
        compose::Buffer b                = compose::render (d, area, settings);
        for (size_t i = 0; i < b.px.size (); i++)
        {
            float v = b.px[i] < 0.0f ? 0.0f : b.px[i] > 1.0f ? 1.0f : b.px[i];
            img[i]  = (uint8_t) (v * 255.0f + 0.5f);
        }
        return img;
    }

    bool cmd_paint_bucket (Session& s, const Value& p, Value* result, Error* err)
    {
        float c[4];
        color_param (p, "color", s.tools.foreground, c);
        int32_t x = (int32_t) floorf (fnum (p, "x", 0.0f)), y = (int32_t) floorf (fnum (p, "y", 0.0f));
        float tol = fnum (p, "tolerance", 32.0f), opacity = fnum (p, "opacity", 100.0f) / 100.0f;
        bool contiguous = fflag (p, "contiguous", true), aa = fflag (p, "antiAlias", true);
        bool all             = fflag (p, "sampleAllLayers", false) && !is_mask_target (p);
        std::string contents = "foreground";
        param_str (p, "contents", &contents);
        if (contents != "foreground")
        {
            return bad_params (err, "paint.bucket", "only foreground fills are ported (pattern fills are not yet)");
        }
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        bool lock;
        raster::Surface* surf = stroke_surface (d, id, p, &lock, err);
        if (surf == NULL)
        {
            return false;
        }
        /* The region comes from the target's own pixels, or the visible composite. */
        std::vector<uint8_t> img = sample_rgba8 (s, *d, all ? NULL : surf);
        algo::Region region;
        bool filled = algo::wand_region (img, d->bounds (), x, y, tol, contiguous, aa, &region);
        img.clear ();
        if (filled)
        {
            algo::bucket_fill_region (surf, region, c, opacity, d->has_selection ? &d->selection : NULL, lock, s.cmyk);
            surf->prune ();
        }
        if (filled && !e.commit ("Paint Bucket", err))
        {
            return false;
        }
        *result = Value{{"filled", filled}};
        return true;
    }

    bool read_point (const Value& p, const char* key, float out[2])
    {
        const Value* a = param (p, key);
        if (a == NULL || !a->is_array () || a->size () < 2 || !(*a)[0].is_number () || !(*a)[1].is_number ())
        {
            return false;
        }
        out[0] = (*a)[0].get<float> ();
        out[1] = (*a)[1].get<float> ();
        return true;
    }

    /* A stop's colour: "#rrggbb", [r, g, b(, a)], "foreground" or "background". */
    bool stop_color (const Session& s, const Value& v, float out[4])
    {
        if (v.is_string ())
        {
            const std::string& str = v.get_ref<const std::string&> ();
            if (str == "foreground")
            {
                memcpy (out, s.tools.foreground, 4 * sizeof (float));
                return true;
            }
            if (str == "background")
            {
                memcpy (out, s.tools.background, 4 * sizeof (float));
                return true;
            }
        }
        Value wrap    = Value{{"c", v}};
        float none[4] = {-1, -1, -1, -1};
        color_param (wrap, "c", none, out);
        return out[0] >= 0.0f;
    }

    /* Opacity stops ([[location, opacity %], ...]) multiplied into the colour stops, sampled finely
       enough to follow both. */
    void apply_transparency (std::vector<algo::GradientStop>* stops, std::vector<std::pair<float, float>> os)
    {
        if (os.empty ())
        {
            return;
        }
        std::sort (os.begin (), os.end ());
        std::vector<float> ts;
        for (size_t i = 0; i < stops->size (); i++)
        {
            ts.push_back ((*stops)[i].t);
        }
        for (size_t i = 0; i < os.size (); i++)
        {
            ts.push_back (os[i].first);
        }
        ts.push_back (0.0f);
        ts.push_back (1.0f);
        std::sort (ts.begin (), ts.end ());
        std::vector<float> all;
        for (size_t i = 0; i + 1 < ts.size (); i++)
        {
            if (ts[i + 1] - ts[i] < 1e-6f)
            {
                continue;
            }
            for (int k = 0; k < 4; k++)
            {
                all.push_back (ts[i] + (ts[i + 1] - ts[i]) * (float) k / 4.0f);
            }
        }
        all.push_back (1.0f);
        std::vector<algo::GradientStop> out;
        for (size_t i = 0; i < all.size (); i++)
        {
            algo::GradientStop g;
            g.t = all[i];
            algo::sample_stops (*stops, g.t, g.color);
            float a = os[0].second;
            if (g.t >= os.back ().first)
            {
                a = os.back ().second;
            }
            for (size_t k = 0; k + 1 < os.size (); k++)
            {
                if (g.t >= os[k].first && g.t <= os[k + 1].first)
                {
                    float span = os[k + 1].first - os[k].first;
                    float u    = span > 0.0f ? (g.t - os[k].first) / span : 0.0f;
                    a          = os[k].second + (os[k + 1].second - os[k].second) * u;
                    break;
                }
            }
            g.color[3] *= a;
            out.push_back (g);
        }
        *stops = out;
    }

    bool stop_before (const algo::GradientStop& a, const algo::GradientStop& b)
    {
        return a.t < b.t;
    }

    bool cmd_paint_gradient (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "paint.gradient";
        algo::Gradient g;
        if (!read_point (p, "from", g.from))
        {
            return bad_params (err, cmd, "missing from");
        }
        if (!read_point (p, "to", g.to))
        {
            return bad_params (err, cmd, "missing to");
        }
        std::string style = "linear";
        param_str (p, "style", &style);
        if (!algo::gradient_shape_from_str (style.c_str (), &g.shape))
        {
            g.shape = algo::GradientShape::Linear;
        }
        const Value* stops  = param (p, "stops");
        const Value* colors = param (p, "colors");
        if (stops != NULL)
        {
            if (!stops->is_array () || stops->size () < 1)
            {
                return bad_params (err, cmd, "`stops` is [[location, colour], ...]");
            }
            for (size_t i = 0; i < stops->size (); i++)
            {
                const Value& st = (*stops)[i];
                algo::GradientStop gs;
                if (!st.is_array () || st.size () != 2 || !st[0].is_number () || !stop_color (s, st[1], gs.color))
                {
                    return bad_params (
                        err, cmd, "a stop is [location 0..1, \"#rrggbb\"|\"foreground\"|\"background\"]");
                }
                gs.t = fminf (fmaxf (st[0].get<float> (), 0.0f), 1.0f);
                g.stops.push_back (gs);
            }
            std::stable_sort (g.stops.begin (), g.stops.end (), stop_before);
        }
        else if (colors != NULL && colors->is_array () && !colors->empty ())
        {
            size_t n = colors->size ();
            for (size_t i = 0; i < n; i++)
            {
                algo::GradientStop gs;
                if (!stop_color (s, (*colors)[i], gs.color))
                {
                    memcpy (gs.color, s.tools.foreground, sizeof gs.color);
                }
                gs.t = n > 1 ? (float) i / (float) (n - 1) : 0.0f;
                g.stops.push_back (gs);
            }
        }
        else
        {
            algo::GradientStop a = {0.0f, {0, 0, 0, 0}}, b = {1.0f, {0, 0, 0, 0}};
            memcpy (a.color, s.tools.foreground, sizeof a.color);
            memcpy (b.color, s.tools.background, sizeof b.color);
            g.stops.push_back (a);
            g.stops.push_back (b);
        }
        const Value* tr = param (p, "transparency");
        if (tr != NULL)
        {
            if (!tr->is_array ())
            {
                return bad_params (err, cmd, "`transparency` is [[location, opacity %%], ...]");
            }
            std::vector<std::pair<float, float>> os;
            for (size_t i = 0; i < tr->size (); i++)
            {
                const Value& o = (*tr)[i];
                if (!o.is_array () || o.size () != 2 || !o[0].is_number () || !o[1].is_number ())
                {
                    return bad_params (err, cmd, "an opacity stop is [location 0..1, opacity 0..100]");
                }
                os.push_back (std::make_pair (fminf (fmaxf (o[0].get<float> (), 0.0f), 1.0f),
                    fminf (fmaxf (o[1].get<float> () / 100.0f, 0.0f), 1.0f)));
            }
            apply_transparency (&g.stops, os);
        }
        g.reverse             = fflag (p, "reverse", false);
        g.dither              = fflag (p, "dither", true);
        float opacity         = fnum (p, "opacity", 100.0f) / 100.0f;
        color::BlendMode mode = color::BlendMode::Normal;
        std::string m;
        if (param_str (p, "mode", &m) && !blend_from_str (m.c_str (), &mode))
        {
            return bad_params (err, cmd, "unknown blend mode `%s`", m.c_str ());
        }
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        bool lock;
        raster::Surface* surf = stroke_surface (d, id, p, &lock, err);
        if (surf == NULL)
        {
            return false;
        }
        Rect area = d->bounds ();
        if (d->has_selection && !d->selection.content_bounds ().is_empty ())
        {
            area = d->selection.content_bounds ().intersect (area);
        }
        algo::paint_gradient (surf, area, g, opacity, mode, d->has_selection ? &d->selection : NULL, lock, s.cmyk);
        surf->prune ();
        return e.commit ("Gradient", err);
    }

    /* The Magic Eraser: the wand's region made transparent (by `opacity`, within the selection). A
       Background becomes a normal layer first, as in Photoshop. */
    bool cmd_magic_eraser (Session& s, const Value& p, Value* result, Error* err)
    {
        int32_t x = (int32_t) floorf (fnum (p, "x", 0.0f)), y = (int32_t) floorf (fnum (p, "y", 0.0f));
        float tol = fnum (p, "tolerance", 32.0f), opacity = fnum (p, "opacity", 100.0f) / 100.0f;
        bool contiguous = fflag (p, "contiguous", true), aa = fflag (p, "antiAlias", true),
             all = fflag (p, "sampleAllLayers", false);
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (!check_pixels_unlocked (*d, id, err))
        {
            return false;
        }
        background_to_layer (d, id);
        if (d->effective_locks (id).transparency)
        {
            return fail (err, ErrorKind::Other, "the layer's transparency is locked");
        }
        doc::Layer* l         = d->layer_mut (id);
        raster::Surface* surf = l != NULL ? l->surface_mut () : NULL;
        if (surf == NULL || !surf->format ().alpha)
        {
            return fail (err, ErrorKind::Other, "the Magic Eraser works on pixel layers");
        }
        std::vector<uint8_t> img = sample_rgba8 (s, *d, all ? NULL : surf);
        algo::Region region;
        bool found = algo::wand_region (img, d->bounds (), x, y, tol, contiguous, aa, &region);
        img.clear ();
        Rect r = region.bbox.intersect (d->bounds ());
        if (found && !r.is_empty ())
        {
            std::vector<float> px;
            surf->read_region (r, &px);
            size_t n = surf->format ().channels (), rw = region.bbox.width ();
            for (int32_t yy = r.y0; yy < r.y1; yy++)
            {
                for (int32_t xx = r.x0; xx < r.x1; xx++)
                {
                    float m = region.mask[(size_t) (yy - region.bbox.y0) * rw + (size_t) (xx - region.bbox.x0)] / 255.0f
                            * opacity;
                    if (d->has_selection)
                    {
                        m *= d->selection.sample_channel (xx, yy, 0);
                    }
                    if (m <= 0.0f)
                    {
                        continue;
                    }
                    float* q = &px[((size_t) (yy - r.y0) * r.width () + (size_t) (xx - r.x0)) * n];
                    q[n - 1] *= 1.0f - m;
                }
            }
            surf->write_region (r, px.data (), px.size ());
            surf->prune ();
        }
        if (found && !e.commit ("Magic Eraser", err))
        {
            return false;
        }
        *result = Value{{"erased", found}};
        return true;
    }

    bool has_paintable (const Session& s, std::string* why);

    const CommandSpec FILL_TOOL_COMMANDS[] = {
        {"paint.magicEraser", "Magic Eraser", "", NULL,
            "{\"x\":px,\"y\":px,\"tolerance\":0..255=32,\"contiguous\":bool=true,\"antiAlias\":bool=true,"
            "\"sampleAllLayers\":bool=false,\"opacity\":1..100=100,"
            "\"layer\":id?}",
            has_layer, cmd_magic_eraser, true},
        {"paint.bucket", "Paint Bucket", "", NULL,
            "{\"x\":px,\"y\":px,\"tolerance\":0..255=32,\"contiguous\":bool=true,\"antiAlias\":bool=true,"
            "\"sampleAllLayers\":bool=false,"
            "\"contents\":\"foreground\"=\"foreground\",\"color\":\"#rrggbb\"=foreground,\"opacity\":1..100=100,"
            "\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\"}",
            has_paintable, cmd_paint_bucket, true},
        {"paint.gradient", "Gradient", "", NULL,
            "{\"from\":[x,y],\"to\":[x,y],\"style\":\"linear|radial|angle|reflected|diamond\"=\"linear\",\"colors\":["
            "\"#rrggbb\",…]? (evenly spaced),"
            "\"stops\":[[t,\"#rrggbb\"|\"foreground\"|\"background\"],…]?,\"transparency\":[[t,0..100],…]?,\"reverse\":"
            "bool=false,\"dither\":bool=true,"
            "\"opacity\":1..100=100,\"mode\":\"normal|multiply|…\"=\"normal\",\"layer\":id?,\"target\":\"pixels|mask\"="
            "\"pixels\"}",
            has_paintable, cmd_paint_gradient, true},
    };

    void register_fill_tool_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), FILL_TOOL_COMMANDS,
            FILL_TOOL_COMMANDS + sizeof FILL_TOOL_COMMANDS / sizeof FILL_TOOL_COMMANDS[0]);
    }

} // namespace engine
