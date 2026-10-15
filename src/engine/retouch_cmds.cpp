#include "algo/poisson.h"
#include "compose/compose.h"
#include "engine/brush.h"
#include "engine/layers.h"
#include "engine/retouch.h"
#include "engine/session.h"
#include <math.h>
#include <string.h>

namespace engine
{

    /* The retouching tools: Clone Stamp, Healing Brush, Spot Healing Brush, History Brush, Dodge,
       Burn, Sponge, Blur, Sharpen and Smudge. Each takes a brush (points, size, hardness, opacity,
       flow, spacing), respects the selection and the transparency lock, and paints the layer's
       pixels or its mask ("target": "mask"). Spot Healing's Content-Aware and Create Texture types
       are not part of this build; Proximity Match is. Clone sources are a translation (no
       scale, rotation or flips yet). */

    using geom::Rect;

    float pnum (const Value& p, const char* key, float fallback)
    {
        double v;
        return param_f64 (p, key, &v) ? (float) v : fallback;
    }

    bool pflag (const Value& p, const char* key, bool fallback)
    {
        bool v;
        return param_bool (p, key, &v) ? v : fallback;
    }

    float clampf (float v, float lo, float hi);

    /* The tool's brush with the stroke's params (percentages, as the options bar shows them). */
    bool retouch_brush (const Session& s, const Value& p, const char* cmd, paint::BrushSettings* brush,
        std::vector<paint::StrokePoint>* pts, doc::LayerId* id, Error* err)
    {
        if (!parse_points (p, cmd, pts, err))
        {
            return false;
        }
        paint::BrushSettings b = s.tools.brush;
        b.size                 = fmaxf (pnum (p, "size", b.size), 1.0f);
        b.hardness             = clampf (pnum (p, "hardness", b.hardness * 100.0f), 0.0f, 100.0f) / 100.0f;
        b.opacity              = clampf (pnum (p, "opacity", 100.0f), 1.0f, 100.0f) / 100.0f;
        b.flow                 = clampf (pnum (p, "flow", 100.0f), 1.0f, 100.0f) / 100.0f;
        b.spacing              = clampf (pnum (p, "spacing", b.spacing * 100.0f), 1.0f, 1000.0f) / 100.0f;
        b.erase                = false;
        b.mode                 = color::BlendMode::Normal;
        if (!(b.size <= paint::MAX_BRUSH_SIZE))
        {
            return bad_params (err, cmd, "brush size must be at most %.0f px", paint::MAX_BRUSH_SIZE);
        }
        *brush = b;
        return layer_param (s, p, id, err);
    }

    bool blend_param (const Value& p, const char* cmd, color::BlendMode* out, Error* err)
    {
        *out = color::BlendMode::Normal;
        std::string m;
        if (param_str (p, "mode", &m) && !blend_from_str (m.c_str (), out))
        {
            return bad_params (err, cmd, "unknown blend mode `%s`", m.c_str ());
        }
        return true;
    }

    bool sample_layers_param (const Value& p, const char* cmd, SampleLayers* out, Error* err)
    {
        std::string v = "current";
        param_str (p, "sampleLayer", &v);
        if (v == "current")
        {
            *out = SampleLayers::Current;
        }
        else if (v == "currentAndBelow")
        {
            *out = SampleLayers::CurrentAndBelow;
        }
        else if (v == "all")
        {
            *out = SampleLayers::All;
        }
        else
        {
            return bad_params (err, cmd, "unknown sampleLayer `%s` (current|currentAndBelow|all)", v.c_str ());
        }
        return true;
    }

    /* The composite of rect in the surface's format: the whole document, or the target layer and
       everything below it. */
    paint::Region composite_region (const doc::Document& pre, doc::LayerId id, SampleLayers which, Rect rect,
        color::PixelFormat fmt, const color::CmykSpace* cmyk)
    {
        doc::Document d = pre;
        if (which == SampleLayers::CurrentAndBelow && id != doc::NO_LAYER)
        {
            std::vector<doc::WalkEntry> walk = pre.walk ();
            bool after                       = false;
            for (size_t i = 0; i < walk.size (); i++)
            {
                if (after)
                {
                    doc::Layer* l = d.layer_at_mut (walk[i].path);
                    if (l != NULL)
                    {
                        l->visible = false;
                    }
                }
                if (walk[i].layer->id == id)
                {
                    after = true;
                }
            }
        }
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = cmyk;
        compose::Buffer buf              = compose::render (d, rect, settings);
        size_t n                         = fmt.channels ();
        paint::Region out                = paint::Region::make (rect, n);
        for (size_t i = 0; i < buf.pixel_count (); i++)
        {
            raster::from_rgba (fmt, &buf.px[i * 4], cmyk, &out.data[i * n]);
        }
        return out;
    }

    paint::Region sample_region (const doc::Document& pre, doc::LayerId id, const raster::Surface& surf,
        SampleLayers which, Rect rect, const color::CmykSpace* cmyk)
    {
        if (which == SampleLayers::Current)
        {
            return paint::Region::read (surf, rect);
        }
        return composite_region (pre, id, which, rect, surf.format (), cmyk);
    }

    /* Integer depths store 0..1; float depths keep values above 1; alpha is always 0..1. */
    void clamp_samples (color::PixelFormat fmt, std::vector<float>* data)
    {
        size_t n = fmt.channels ();
        int a    = paint::alpha_index (fmt);
        bool fl  = fmt.sample == color::SampleType::F32;
        for (size_t i = 0; i < data->size (); i++)
        {
            float& v = (*data)[i];
            if ((int) (i % n) == a || !fl)
            {
                v = clampf (v, 0.0f, 1.0f);
            }
            else if (v < 0.0f)
            {
                v = 0.0f;
            }
        }
    }

    /* Where a clone stroke samples: "offset", or "source" (sampled under the first point). */
    bool clone_offset (
        const Value& p, const paint::StrokePoint& first, const char* cmd, int32_t* dx, int32_t* dy, Error* err)
    {
        const Value* o   = param (p, "offset");
        const Value* src = param (p, "source");
        if (o != NULL && o->is_array () && o->size () == 2 && (*o)[0].is_number () && (*o)[1].is_number ())
        {
            *dx = (int32_t) lround ((*o)[0].get<double> ());
            *dy = (int32_t) lround ((*o)[1].get<double> ());
            return true;
        }
        if (src != NULL && src->is_array () && src->size () == 2 && (*src)[0].is_number () && (*src)[1].is_number ())
        {
            *dx = (int32_t) lround ((*src)[0].get<double> () - first.x);
            *dy = (int32_t) lround ((*src)[1].get<double> () - first.y);
            return true;
        }
        return bad_params (err, cmd, "pass `source` [x, y] or `offset` [dx, dy] (Alt-click to set a source)");
    }

    Value clone_result (Rect dmg, int32_t dx, int32_t dy, bool aligned, const Value& p, const paint::StrokePoint& first)
    {
        Value next = Value ();
        if (!aligned)
        {
            const Value* src = param (p, "source");
            if (src != NULL)
            {
                next = *src;
            }
            else
            {
                next = Value{first.x + dx, first.y + dy};
            }
        }
        return Value{{"damage", {dmg.x0, dmg.y0, dmg.width (), dmg.height ()}}, {"offset", {dx, dy}},
            {"aligned", aligned}, {"nextSource", next}};
    }

    bool begin_target (Edit* e, doc::LayerId id, const Value& p, StrokeTarget* t, Error* err)
    {
        doc::Document* d = e->doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        t->pre  = std::make_shared<const doc::Document> (*d);
        t->surf = stroke_surface (d, id, p, &t->lock, err);
        if (t->surf == NULL)
        {
            return false;
        }
        t->sel = d->has_selection ? &d->selection : NULL;
        return true;
    }

    bool cmd_clone_stamp (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "paint.cloneStamp";
        paint::BrushSettings brush;
        std::vector<paint::StrokePoint> pts;
        doc::LayerId id;
        if (!retouch_brush (s, p, cmd, &brush, &pts, &id, err))
        {
            return false;
        }
        SampleLayers which;
        color::BlendMode mode;
        int32_t dx, dy;
        if (!sample_layers_param (p, cmd, &which, err) || !blend_param (p, cmd, &mode, err)
            || !clone_offset (p, pts[0], cmd, &dx, &dy, err))
        {
            return false;
        }
        bool aligned = pflag (p, "aligned", true);
        Edit e (s);
        StrokeTarget t;
        if (!begin_target (&e, id, p, &t, err))
        {
            return false;
        }
        Rect bounds;
        std::vector<float> cov;
        paint::stroke_coverage (brush, pts, &bounds, &cov);
        paint::Region paint_px = sample_region (*t.pre, id, *t.surf, which, bounds.translate (dx, dy), s.cmyk);
        paint_px.rect          = bounds;
        Rect dmg = paint::apply_coverage (t.surf, bounds, cov, brush.opacity, t.sel, t.lock, paint_px, mode, s.cmyk);
        if (!e.commit ("Clone Stamp", err))
        {
            return false;
        }
        *result = clone_result (dmg, dx, dy, aligned, p, pts[0]);
        return true;
    }

    /* Seamless-clone src into dst over the mask (same rect); the solve runs on the mask's bounds
       plus a one-pixel Dirichlet ring. */
    paint::Region heal_region (
        color::PixelFormat fmt, const paint::Region& src, const paint::Region& dst, const std::vector<bool>& mask)
    {
        size_t w = dst.width ();
        Rect bbox;
        for (size_t i = 0; i < mask.size (); i++)
        {
            if (!mask[i])
            {
                continue;
            }
            int32_t x = dst.rect.x0 + (int32_t) (i % w), y = dst.rect.y0 + (int32_t) (i / w);
            bbox = bbox.unite (Rect::from_xywh (x, y, 1, 1));
        }
        paint::Region out = dst;
        if (bbox.is_empty ())
        {
            return out;
        }
        Rect r           = bbox.inflate (1).intersect (dst.rect);
        paint::Region sc = src.crop (r), dc = dst.crop (r);
        std::vector<bool> m (sc.width () * sc.height ());
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                m[(size_t) (y - r.y0) * r.width () + (size_t) (x - r.x0)] = mask[dst.index (x, y)];
            }
        }
        std::vector<float> data =
            algo::seamless_clone (dc.width (), dc.height (), dc.ch, sc.data.data (), dc.data.data (), m);
        clamp_samples (fmt, &data);
        size_t n = dst.ch, rw = r.width ();
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            memcpy (out.px_mut (r.x0, y), &data[(size_t) (y - r.y0) * rw * n], rw * n * sizeof (float));
        }
        return out;
    }

    /* A coverage map grown by m pixels of zero coverage (a Dirichlet boundary for the solve). */
    void pad_coverage (Rect bounds, const std::vector<float>& cov, int32_t m, Rect* g, std::vector<float>* out)
    {
        *g       = bounds.inflate (m);
        size_t w = bounds.width (), gw = g->width ();
        out->assign (gw * g->height (), 0.0f);
        for (size_t i = 0; i < cov.size (); i++)
        {
            (*out)[(i / w + (size_t) m) * gw + i % w + (size_t) m] = cov[i];
        }
    }

    bool cmd_healing_brush (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "paint.healingBrush";
        paint::BrushSettings brush;
        std::vector<paint::StrokePoint> pts;
        doc::LayerId id;
        if (!retouch_brush (s, p, cmd, &brush, &pts, &id, err))
        {
            return false;
        }
        SampleLayers which;
        color::BlendMode mode;
        int32_t dx, dy;
        if (!sample_layers_param (p, cmd, &which, err) || !blend_param (p, cmd, &mode, err)
            || !clone_offset (p, pts[0], cmd, &dx, &dy, err))
        {
            return false;
        }
        bool aligned = pflag (p, "aligned", true);
        Edit e (s);
        StrokeTarget t;
        if (!begin_target (&e, id, p, &t, err))
        {
            return false;
        }
        Rect bounds, g;
        std::vector<float> cov0, cov;
        paint::stroke_coverage (brush, pts, &bounds, &cov0);
        pad_coverage (bounds, cov0, 2, &g, &cov);
        color::PixelFormat fmt = t.surf->format ();
        paint::Region src      = sample_region (*t.pre, id, *t.surf, which, g.translate (dx, dy), s.cmyk);
        src.rect               = g;
        /* The texture is fitted to the layer, or to what is visible when sampling several layers. */
        paint::Region dst = which == SampleLayers::Current ? paint::Region::read (*t.surf, g)
                                                           : composite_region (*t.pre, id, which, g, fmt, s.cmyk);
        std::vector<bool> mask (cov.size ());
        for (size_t i = 0; i < cov.size (); i++)
        {
            mask[i] = cov[i] > 0.0f;
        }
        paint::Region healed = heal_region (fmt, src, dst, mask);
        Rect dmg = paint::apply_coverage (t.surf, g, cov, brush.opacity, t.sel, t.lock, healed, mode, s.cmyk);
        if (!e.commit ("Healing Brush", err))
        {
            return false;
        }
        *result = clone_result (dmg, dx, dy, aligned, p, pts[0]);
        return true;
    }

    /* The History Brush's source: the target (pixels or mask) as it was in history state "state"
       (default the oldest state held, normally the Open snapshot). */
    bool history_source (
        const Session& s, doc::LayerId id, const Value& p, const char* cmd, raster::Surface* out, Error* err)
    {
        const DocState* st = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        ops::DocRef src;
        uint64_t i;
        if (param_u64 (p, "state", &i))
        {
            if (i == st->history.past_len ())
            {
                src = st->doc;
            }
            else
            {
                src = st->history.state ((size_t) i);
            }
            if (!src)
            {
                return bad_params (
                    err, cmd, "no history state %llu (0..=%zu)", (unsigned long long) i, st->history.past_len ());
            }
        }
        else
        {
            src = st->history.state (0);
            if (!src)
            {
                src = st->doc;
            }
        }
        const doc::Layer* l = src->layer (id);
        if (l == NULL)
        {
            return fail (err, ErrorKind::Other, "the layer did not exist in that history state");
        }
        if (is_mask_target (p))
        {
            if (!l->has_mask)
            {
                return fail (err, ErrorKind::Other, "the layer had no mask in that history state");
            }
            *out = l->mask.surface;
        }
        else
        {
            if (l->surface () == NULL)
            {
                return fail (err, ErrorKind::Other, "the layer had no pixels in that history state");
            }
            *out = *l->surface ();
        }
        return true;
    }

    bool cmd_history_brush (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "paint.historyBrush";
        paint::BrushSettings brush;
        std::vector<paint::StrokePoint> pts;
        doc::LayerId id;
        if (!retouch_brush (s, p, cmd, &brush, &pts, &id, err))
        {
            return false;
        }
        color::BlendMode mode;
        if (!blend_param (p, cmd, &mode, err))
        {
            return false;
        }
        raster::Surface src;
        if (!history_source (s, id, p, cmd, &src, err))
        {
            return false;
        }
        Edit e (s);
        StrokeTarget t;
        if (!begin_target (&e, id, p, &t, err))
        {
            return false;
        }
        if (src.format () != t.surf->format ())
        {
            src = src.convert (t.surf->format (), s.cmyk);
        }
        Rect bounds;
        std::vector<float> cov;
        paint::stroke_coverage (brush, pts, &bounds, &cov);
        paint::Region paint_px = paint::Region::read (src, bounds);
        Rect dmg = paint::apply_coverage (t.surf, bounds, cov, brush.opacity, t.sel, t.lock, paint_px, mode, s.cmyk);
        if (!e.commit ("History Brush", err))
        {
            return false;
        }
        *result = Value{{"damage", {dmg.x0, dmg.y0, dmg.width (), dmg.height ()}}};
        return true;
    }

    /* A mask dilated by r pixels (square structuring element). */
    std::vector<bool> dilate (size_t w, size_t h, const std::vector<bool>& m, size_t r)
    {
        std::vector<bool> tmp (w * h, false), out (w * h, false);
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                if (!m[y * w + x])
                {
                    continue;
                }
                for (size_t xx = x >= r ? x - r : 0; xx < x + r + 1 && xx < w; xx++)
                {
                    tmp[y * w + xx] = true;
                }
            }
        }
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                if (!tmp[y * w + x])
                {
                    continue;
                }
                for (size_t yy = y >= r ? y - r : 0; yy < y + r + 1 && yy < h; yy++)
                {
                    out[yy * w + x] = true;
                }
            }
        }
        return out;
    }

    /* Content-Aware Fill's sampling window around a hole: about four times its size, inside the
       canvas. */
    Rect sampling_window (Rect hole, Rect canvas)
    {
        hole = hole.intersect (canvas);
        if (hole.is_empty () || canvas.is_empty ())
        {
            return Rect ();
        }
        double w = hole.width () > 50 ? hole.width () : 50, h = hole.height () > 50 ? hole.height () : 50;
        int64_t side = (int64_t) llround (4.0 * sqrt (w * h));
        int32_t out[4];
        for (int axis = 0; axis < 2; axis++)
        {
            int64_t c2     = axis == 0 ? (int64_t) hole.x0 + hole.x1 : (int64_t) hole.y0 + hole.y1;
            int64_t extent = axis == 0 ? hole.width () : hole.height ();
            int64_t lo = axis == 0 ? canvas.x0 : canvas.y0, hi = axis == 0 ? canvas.x1 : canvas.y1;
            int64_t sd = side > extent ? side : extent;
            int64_t a0, a1;
            if (sd >= hi - lo)
            {
                a0 = lo;
                a1 = hi;
            }
            else
            {
                a0 = (c2 - sd) / 2;
                if (a0 < lo)
                {
                    a0 = lo;
                }
                if (a0 > hi - sd)
                {
                    a0 = hi - sd;
                }
                a1 = a0 + sd;
            }
            out[axis]     = (int32_t) a0;
            out[axis + 2] = (int32_t) a1;
        }
        Rect r = {out[0], out[1], out[2], out[3]};
        return r;
    }

    /* Spot Healing (Proximity Match): the stroke's hole filled from the best-matching nearby patch
       (or a membrane fill when none fits), then blended into its surroundings in the gradient
       domain. Sample All Layers heals what is visible. */
    bool cmd_spot_healing (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "paint.spotHealing";
        paint::BrushSettings brush;
        std::vector<paint::StrokePoint> pts;
        doc::LayerId id;
        if (!retouch_brush (s, p, cmd, &brush, &pts, &id, err))
        {
            return false;
        }
        std::string type = "proximityMatch";
        param_str (p, "type", &type);
        if (type == "contentAware")
        {
            return remove_stroke (s, p, cmd, "Spot Healing Brush", result, err);
        }
        if (type == "createTexture")
        {
            return bad_params (err, cmd, "Create Texture is not ported yet (use proximityMatch)");
        }
        if (type != "proximityMatch")
        {
            return bad_params (err, cmd, "unknown type `%s` (proximityMatch|contentAware)", type.c_str ());
        }
        bool all = pflag (p, "sampleAllLayers", false) && !is_mask_target (p);
        Edit e (s);
        StrokeTarget t;
        if (!begin_target (&e, id, p, &t, err))
        {
            return false;
        }
        Rect bounds;
        std::vector<float> cov;
        paint::stroke_coverage (brush, pts, &bounds, &cov);
        Rect win = sampling_window (bounds, t.pre->bounds ());
        Rect dmg;
        if (!win.is_empty ())
        {
            color::PixelFormat fmt = t.surf->format ();
            paint::Region img      = all ? composite_region (*t.pre, doc::NO_LAYER, SampleLayers::All, win, fmt, s.cmyk)
                                         : paint::Region::read (*t.surf, win);
            size_t w = img.width (), h = img.height (), ch = img.ch, bw = bounds.width ();
            std::vector<float> scov (w * h, 0.0f);
            for (size_t i = 0; i < cov.size (); i++)
            {
                int32_t x = bounds.x0 + (int32_t) (i % bw), y = bounds.y0 + (int32_t) (i / bw);
                if (x >= win.x0 && y >= win.y0 && x < win.x1 && y < win.y1)
                {
                    scov[img.index (x, y)] = cov[i];
                }
            }
            std::vector<bool> hole (w * h);
            bool any = false;
            for (size_t i = 0; i < hole.size (); i++)
            {
                hole[i] = scov[i] > 0.0f;
                any     = any || hole[i];
            }
            if (any)
            {
                std::vector<bool> domain = dilate (w, h, hole, 2), heal_mask = dilate (w, h, hole, 1);
                size_t ring    = (size_t) clampf (brush.size / 8.0f, 3.0f, 16.0f);
                int32_t margin = (int32_t) ((win.width () > win.height () ? win.width () : win.height ()) / 2);
                int32_t dx, dy;
                std::vector<float> filled;
                if (algo::best_offset (w, h, ch, img.data.data (), domain, ring, margin, &dx, &dy))
                {
                    filled = img.data;
                    for (size_t i = 0; i < w * h; i++)
                    {
                        if (!domain[i])
                        {
                            continue;
                        }
                        size_t x = i % w + (size_t) dx, y = i / w + (size_t) dy;
                        memcpy (&filled[i * ch], &img.data[(y * w + x) * ch], ch * sizeof (float));
                    }
                }
                else
                {
                    filled = algo::membrane_fill (w, h, ch, img.data.data (), domain);
                }
                paint::Region src    = img;
                src.data             = filled;
                paint::Region healed = heal_region (fmt, src, img, heal_mask);
                dmg                  = bounds.intersect (win);
                std::vector<float> dcov ((size_t) dmg.width () * dmg.height ());
                for (int32_t y = dmg.y0; y < dmg.y1; y++)
                {
                    for (int32_t x = dmg.x0; x < dmg.x1; x++)
                    {
                        dcov[(size_t) (y - dmg.y0) * dmg.width () + (size_t) (x - dmg.x0)] = scov[img.index (x, y)];
                    }
                }
                dmg = paint::apply_coverage (
                    t.surf, dmg, dcov, brush.opacity, t.sel, t.lock, healed, color::BlendMode::Normal, s.cmyk);
            }
        }
        if (!e.commit ("Spot Healing Brush", err))
        {
            return false;
        }
        *result = Value{{"damage", {dmg.x0, dmg.y0, dmg.width (), dmg.height ()}}};
        return true;
    }

    /* Per-dab strength such that one pass over a point totals about e, whatever the spacing. */
    float per_dab (float e, float spacing)
    {
        return 1.0f - powf (1.0f - clampf (e, 0.0f, 1.0f), clampf (spacing, 0.01f, 1.0f));
    }

    int32_t DabTool::halo () const
    {
        return kind == DabKind::Focus ? (int32_t) ceilf (sigma * 3.0f) + 1 : 0;
    }

    bool make_dab_tool (const Session& s, const char* cmd, const Value& p, const paint::BrushSettings& brush,
        color::PixelFormat fmt, DabTool* t, Error* err)
    {
        t->fmt         = fmt;
        t->cmyk        = s.cmyk;
        t->spacing     = brush.spacing;
        t->has_visible = false;
        t->all_layers  = false;
        std::string v;
        if (strcmp (cmd, "paint.dodge") == 0 || strcmp (cmd, "paint.burn") == 0)
        {
            t->kind  = DabKind::Tone;
            t->burn  = strcmp (cmd, "paint.burn") == 0;
            t->label = t->burn ? "Burn Tool" : "Dodge Tool";
            v        = "midtones";
            param_str (p, "range", &v);
            if (v == "shadows")
            {
                t->range = algo::ToneRange::Shadows;
            }
            else if (v == "midtones")
            {
                t->range = algo::ToneRange::Midtones;
            }
            else if (v == "highlights")
            {
                t->range = algo::ToneRange::Highlights;
            }
            else
            {
                return bad_params (err, cmd, "unknown range `%s` (shadows|midtones|highlights)", v.c_str ());
            }
            t->exposure = clampf (pnum (p, "exposure", 50.0f), 1.0f, 100.0f) / 100.0f;
            t->protect  = pflag (p, "protectTones", true);
        }
        else if (strcmp (cmd, "paint.sponge") == 0)
        {
            t->kind  = DabKind::Sponge;
            t->label = "Sponge Tool";
            v        = "desaturate";
            param_str (p, "mode", &v);
            if (v != "desaturate" && v != "saturate")
            {
                return bad_params (err, cmd, "unknown mode `%s` (desaturate|saturate)", v.c_str ());
            }
            t->saturate = v == "saturate";
            t->vibrance = pflag (p, "vibrance", true);
        }
        else if (strcmp (cmd, "paint.blur") == 0 || strcmp (cmd, "paint.sharpen") == 0)
        {
            t->kind       = DabKind::Focus;
            t->sharpen    = strcmp (cmd, "paint.sharpen") == 0;
            t->label      = t->sharpen ? "Sharpen Tool" : "Blur Tool";
            t->strength   = clampf (pnum (p, "strength", 50.0f), 1.0f, 100.0f) / 100.0f;
            t->protect    = pflag (p, "protectDetail", true);
            t->sigma      = clampf (brush.size * 0.03f, 1.0f, 4.0f);
            t->all_layers = pflag (p, "sampleAllLayers", false) && !is_mask_target (p);
        }
        else if (strcmp (cmd, "paint.smudge") == 0)
        {
            t->kind     = DabKind::Smudge;
            t->label    = "Smudge Tool";
            t->strength = clampf (pnum (p, "strength", 50.0f), 1.0f, 100.0f) / 100.0f;
            float finger[raster::MAX_CHANNELS];
            bool use_finger = pflag (p, "fingerPainting", false);
            if (use_finger)
            {
                raster::from_rgba (fmt, s.tools.foreground, s.cmyk, finger);
            }
            t->smudge = paint::Smudge::make (
                t->strength, use_finger ? finger : NULL, fmt.channels (), brush.size, paint::alpha_index (fmt));
            t->all_layers = pflag (p, "sampleAllLayers", false) && !is_mask_target (p);
        }
        else
        {
            return make_brush_tool (s, cmd, p, fmt, t, err);
        }
        return true;
    }

    /* A colour transform of each covered pixel (through straight RGBA, so any colour model). */
    void sponge_dab (DabTool* t, paint::Region* work, const paint::Footprint& fp)
    {
        Rect r   = fp.rect.intersect (work->rect);
        size_t n = t->fmt.channels ();
        int a    = paint::alpha_index (t->fmt);
        float enc[raster::MAX_CHANNELS];
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                float k = per_dab (fp.at (x, y) * 0.5f, t->spacing);
                if (k <= 0.0f)
                {
                    continue;
                }
                float* px = work->px_mut (x, y);
                float rgba[4], o[4];
                raster::to_rgba (t->fmt, px, t->cmyk, rgba);
                algo::sponge (rgba, k, t->saturate, t->vibrance, o);
                o[3] = rgba[3];
                raster::from_rgba (t->fmt, o, t->cmyk, enc);
                for (size_t c = 0; c < n; c++)
                {
                    if ((int) c != a)
                    {
                        px[c] = enc[c];
                    }
                }
            }
        }
    }

    /* Dodge and Burn tone each pixel once from its original colour, mixed in by the stroke's
       coverage there (built up dab by dab like a brush stroke's, capped at full), so overlapping
       dabs don't compound the curve. The coverage and originals live in 64 px cells. */
    void tone_dab (DabTool* t, paint::Region* work, const paint::Footprint& fp)
    {
        const int32_t CELL = 64;
        Rect r             = fp.rect.intersect (work->rect);
        if (r.is_empty ())
        {
            return;
        }
        size_t n = t->fmt.channels ();
        int a    = paint::alpha_index (t->fmt);
        float enc[raster::MAX_CHANNELS];
        for (int32_t cy = geom::floor_div (r.y0, CELL); cy <= geom::floor_div (r.y1 - 1, CELL); cy++)
        {
            for (int32_t cx = geom::floor_div (r.x0, CELL); cx <= geom::floor_div (r.x1 - 1, CELL); cx++)
            {
                Rect cr   = {cx * CELL, cy * CELL, cx * CELL + CELL, cy * CELL + CELL};
                Rect part = r.intersect (cr);
                if (part.is_empty ())
                {
                    continue;
                }
                int64_t key    = ((int64_t) cx << 32) ^ (int64_t) (uint32_t) cy;
                ToneCell& cell = t->cells[key];
                if (cell.cov.empty ())
                {
                    cell.cov.assign ((size_t) CELL * CELL, -1.0f);
                    cell.orig.assign ((size_t) CELL * CELL * n, 0.0f);
                }
                for (int32_t y = part.y0; y < part.y1; y++)
                {
                    for (int32_t x = part.x0; x < part.x1; x++)
                    {
                        float k = clampf (fp.at (x, y), 0.0f, 1.0f);
                        if (k <= 0.0f)
                        {
                            continue;
                        }
                        size_t i    = (size_t) ((y - cr.y0) * CELL + (x - cr.x0));
                        float& prev = cell.cov[i];
                        float* src  = &cell.orig[i * n];
                        if (prev < 0.0f)
                        {
                            memcpy (src, work->px (x, y), n * sizeof (float));
                            prev = 0.0f;
                        }
                        float nk = prev + k * (1.0f - prev);
                        if (nk <= prev)
                        {
                            continue;
                        }
                        prev = nk;
                        float o[4], tc[3];
                        raster::to_rgba (t->fmt, src, t->cmyk, o);
                        algo::dodge_burn (o, t->exposure, t->range, t->burn, t->protect, tc);
                        float m[4] = {
                            o[0] + (tc[0] - o[0]) * nk, o[1] + (tc[1] - o[1]) * nk, o[2] + (tc[2] - o[2]) * nk, o[3]};
                        raster::from_rgba (t->fmt, m, t->cmyk, enc);
                        float* px = work->px_mut (x, y);
                        for (size_t c = 0; c < n; c++)
                        {
                            if ((int) c != a)
                            {
                                px[c] = enc[c];
                            }
                        }
                    }
                }
            }
        }
    }

    void focus_dab (DabTool* t, paint::Region* work, const paint::Footprint& fp)
    {
        Rect r = fp.rect.intersect (work->rect);
        if (r.is_empty ())
        {
            return;
        }
        size_t w = work->width (), h = work->height (), n = work->ch;
        size_t x0 = (size_t) (r.x0 - work->rect.x0), y0 = (size_t) (r.y0 - work->rect.y0),
               x1 = (size_t) (r.x1 - work->rect.x0), y1 = (size_t) (r.y1 - work->rect.y0);
        int a = paint::alpha_index (t->fmt);
        std::vector<float> out;
        if (t->sharpen)
        {
            algo::local_sharpen (work->data.data (), w, h, n, a, x0, y0, x1, y1, 1.0f, t->protect, &out);
        }
        else
        {
            algo::local_blur (work->data.data (), w, h, n, a, x0, y0, x1, y1, t->sigma, &out);
        }
        size_t rw = r.width ();
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                float k = per_dab (fp.at (x, y) * t->strength, t->spacing);
                if (k <= 0.0f)
                {
                    continue;
                }
                const float* o = &out[((size_t) (y - r.y0) * rw + (size_t) (x - r.x0)) * n];
                float* px      = work->px_mut (x, y);
                for (size_t c = 0; c < n; c++)
                {
                    px[c] += (o[c] - px[c]) * k;
                }
            }
        }
    }

    void run_dab (DabTool* t, paint::Region* work, const paint::Footprint& fp)
    {
        switch (t->kind)
        {
        case DabKind::Tone:
            tone_dab (t, work, fp);
            break;
        case DabKind::Sponge:
            sponge_dab (t, work, fp);
            break;
        case DabKind::Focus:
            focus_dab (t, work, fp);
            break;
        case DabKind::Smudge:
            t->smudge.dab (work, fp);
            break;
        default:
            brush_tool_dab (t, work, fp);
            break;
        }
    }

    /* One dab of a sequential-dab tool. With Sample All Layers the effect runs on a copy of the
       visible composite (kept across dabs) and the result is laid onto the layer under the dab. */
    void dab_tool_effect (paint::Region* work, const paint::Footprint& fp, void* user)
    {
        DabTool* t = (DabTool*) user;
        if (!t->all_layers || !t->pre)
        {
            run_dab (t, work, fp);
            return;
        }
        if (!t->has_visible || !(t->visible.rect == work->rect))
        {
            t->visible     = composite_region (*t->pre, doc::NO_LAYER, SampleLayers::All, work->rect, t->fmt, t->cmyk);
            t->has_visible = true;
        }
        run_dab (t, &t->visible, fp);
        Rect r = fp.rect.intersect (work->rect);
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                if (fp.at (x, y) > 0.0f)
                {
                    memcpy (work->px_mut (x, y), t->visible.px (x, y), work->ch * sizeof (float));
                }
            }
        }
    }

    bool dab_cmd (Session& s, const Value& p, const char* cmd, Value* result, Error* err)
    {
        paint::BrushSettings brush;
        std::vector<paint::StrokePoint> pts;
        doc::LayerId id;
        if (!retouch_brush (s, p, cmd, &brush, &pts, &id, err))
        {
            return false;
        }
        Edit e (s);
        StrokeTarget t;
        if (!begin_target (&e, id, p, &t, err))
        {
            return false;
        }
        std::unique_ptr<DabTool> tool (new DabTool ());
        if (!make_dab_tool (s, cmd, p, brush, t.surf->format (), tool.get (), err))
        {
            return false;
        }
        tool->pre = t.pre;
        Rect dmg =
            paint::apply_dab_stroke (t.surf, brush, pts, t.sel, t.lock, tool->halo (), dab_tool_effect, tool.get ());
        if (!e.commit (tool->label, err))
        {
            return false;
        }
        *result = Value{{"damage", {dmg.x0, dmg.y0, dmg.width (), dmg.height ()}}};
        return true;
    }

    bool cmd_dodge (Session& s, const Value& p, Value* result, Error* err)
    {
        return dab_cmd (s, p, "paint.dodge", result, err);
    }

    bool cmd_burn (Session& s, const Value& p, Value* result, Error* err)
    {
        return dab_cmd (s, p, "paint.burn", result, err);
    }

    bool cmd_sponge (Session& s, const Value& p, Value* result, Error* err)
    {
        return dab_cmd (s, p, "paint.sponge", result, err);
    }

    bool cmd_blur (Session& s, const Value& p, Value* result, Error* err)
    {
        return dab_cmd (s, p, "paint.blur", result, err);
    }

    bool cmd_sharpen (Session& s, const Value& p, Value* result, Error* err)
    {
        return dab_cmd (s, p, "paint.sharpen", result, err);
    }

    bool cmd_smudge (Session& s, const Value& p, Value* result, Error* err)
    {
        return dab_cmd (s, p, "paint.smudge", result, err);
    }

    /* ---- live previews ---- */

    raster::Surface* LiveRetouch::surface ()
    {
        doc::Layer* l = doc->layer_mut (id_);
        if (l == NULL)
        {
            return NULL;
        }
        return mask_ ? &l->mask.surface : l->surface_mut ();
    }

    /* A Clone Stamp, Healing Brush (its texture; the blend happens on release) or History Brush
       stroke drawn as it goes: each changed area is redrawn from the pre-stroke pixels through the
       stroke's coverage so far, as the command composites it. */
    bool LiveRetouch::begin (Session& s, const char* cmd, const Value& p, Error* err)
    {
        session_ = &s;
        paint::BrushSettings brush;
        std::vector<paint::StrokePoint> pts;
        if (!retouch_brush (s, p, cmd, &brush, &pts, &id_, err))
        {
            return false;
        }
        history_kind_ = strcmp (cmd, "paint.historyBrush") == 0 || strcmp (cmd, "paint.patternStamp") == 0;
        if (!blend_param (p, cmd, &mode_, err))
        {
            return false;
        }
        if (strcmp (cmd, "paint.patternStamp") == 0)
        {
            /* The pattern over the canvas plays the part of the history state. */
            const doc::Layer* l       = s.active ()->doc->layer (id_);
            const raster::Surface* ls = l != NULL ? l->surface () : NULL;
            bool aligned              = pflag (p, "aligned", true);
            if (ls == NULL
                || !pattern_source (s, p, cmd, ls->format (), s.active ()->doc->bounds (), aligned ? 0.0 : pts[0].x,
                    aligned ? 0.0 : pts[0].y, &history_, err))
            {
                return false;
            }
        }
        else if (history_kind_)
        {
            if (!history_source (s, id_, p, cmd, &history_, err))
            {
                return false;
            }
        }
        else
        {
            if (!sample_layers_param (p, cmd, &which_, err) || !clone_offset (p, pts[0], cmd, &dx_, &dy_, err))
            {
                return false;
            }
        }
        pre_                  = s.active ()->doc;
        doc                   = std::make_shared<doc::Document> (*pre_);
        mask_                 = is_mask_target (p);
        raster::Surface* surf = stroke_surface (doc.get (), id_, p, &lock_, err);
        if (surf == NULL)
        {
            return false;
        }
        pre_surf_ = *surf;
        if (history_kind_ && history_.format () != pre_surf_.format ())
        {
            history_ = history_.convert (pre_surf_.format (), s.cmyk);
        }
        opacity_ = brush.opacity;
        renderer_.reset (new paint::StrokeRenderer (brush));
        push (pts.data (), pts.size ());
        return true;
    }

    geom::Rect LiveRetouch::push (const paint::StrokePoint* pts, size_t n)
    {
        renderer_->push (pts, n);
        Rect r = renderer_->take_dirty_rect ();
        if (r.is_empty ())
        {
            return r;
        }
        std::vector<float> cov;
        renderer_->coverage_in (r, &cov);
        paint::Region paint_px;
        if (history_kind_)
        {
            paint_px = paint::Region::read (history_, r);
        }
        else
        {
            paint_px      = sample_region (*pre_, id_, pre_surf_, which_, r.translate (dx_, dy_), session_->cmyk);
            paint_px.rect = r;
        }
        raster::Surface* surf = surface ();
        if (surf == NULL)
        {
            return Rect ();
        }
        std::vector<float> orig;
        pre_surf_.read_region (r, &orig);
        surf->write_region (r, orig.data (), orig.size ());
        paint::apply_coverage (surf, r, cov, opacity_, doc->has_selection ? &doc->selection : NULL, lock_, paint_px,
            mode_, session_->cmyk);
        return r;
    }

    raster::Surface* LiveDab::surface ()
    {
        doc::Layer* l = doc->layer_mut (id_);
        if (l == NULL)
        {
            return NULL;
        }
        return mask_ ? &l->mask.surface : l->surface_mut ();
    }

    /* A Dodge, Burn, Sponge, Blur, Sharpen or Smudge stroke drawn as it goes: each new dab runs on a
       working copy, in order, and the changed area is mixed back over the original at the stroke's
       opacity, as the command does on release. */
    bool LiveDab::begin (Session& s, const char* cmd, const Value& p, Error* err)
    {
        session_ = &s;
        paint::BrushSettings brush;
        std::vector<paint::StrokePoint> pts;
        if (!retouch_brush (s, p, cmd, &brush, &pts, &id_, err))
        {
            return false;
        }
        doc                   = std::make_shared<doc::Document> (*s.active ()->doc);
        mask_                 = is_mask_target (p);
        raster::Surface* surf = stroke_surface (doc.get (), id_, p, &lock_, err);
        if (surf == NULL)
        {
            return false;
        }
        tool_.reset (new DabTool ());
        if (!make_dab_tool (s, cmd, p, brush, surf->format (), tool_.get (), err))
        {
            return false;
        }
        tool_->pre = s.active ()->doc;
        pre_surf_  = *surf;
        work_      = *surf;
        opacity_   = brush.opacity;
        gen_.reset (new paint::DabGenerator (brush));
        ctx_.reset (new paint::StrokeRenderer (brush));
        done_ = 0;
        push (pts.data (), pts.size ());
        return true;
    }

    geom::Rect LiveDab::push (const paint::StrokePoint* pts, size_t n)
    {
        std::vector<paint::Dab> dabs;
        gen_->push (pts, n, &dabs);
        Rect dmg;
        for (size_t i = 0; i < dabs.size (); i++)
        {
            paint::Footprint fp;
            fp.dab   = dabs[i];
            fp.index = done_ + i;
            ctx_->footprint (dabs[i], &fp.rect, &fp.cov);
            if (fp.rect.is_empty ())
            {
                continue;
            }
            paint::Region region = paint::Region::read (work_, fp.rect.inflate (tool_->halo ()));
            dab_tool_effect (&region, fp, tool_.get ());
            region.crop (fp.rect).write (&work_);
            dmg = dmg.unite (fp.rect);
        }
        done_ += dabs.size ();
        raster::Surface* surf = surface ();
        if (dmg.is_empty () || surf == NULL)
        {
            return dmg;
        }
        paint::mix_back (surf, paint::Region::read (pre_surf_, dmg), paint::Region::read (work_, dmg), dmg, opacity_,
            doc->has_selection ? &doc->selection : NULL, lock_);
        return dmg;
    }

    bool has_retouch_target (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l->kind == doc::LayerKind::Raster || l->has_mask)
        {
            return true;
        }
        *why = std::string ("active layer is ") + doc::layer_kind_article (l->kind) + " "
             + doc::layer_kind_name (l->kind) + " layer, not a pixel layer";
        return false;
    }


    const CommandSpec RETOUCH_COMMANDS[] = {
        {"paint.cloneStamp", "Clone Stamp", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px=tool size,\"hardness\":0..100=tool "
            "hardness,\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 "
            "%,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\""
            ",\"source\":[sx,sy] (sampled under the first point) | "
            "\"offset\":[dx,dy],\"aligned\":bool=true,\"sampleLayer\":\"current|currentAndBelow|all\"=\"current\","
            "\"mode\":\"normal|multiply|…\"=\"normal\"} → {damage,offset,aligned,nextSource}",
            has_retouch_target, cmd_clone_stamp, true},
        {"paint.healingBrush", "Healing Brush", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px=tool size,\"hardness\":0..100=tool "
            "hardness,\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 "
            "%,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\""
            ",\"source\":[sx,sy] | "
            "\"offset\":[dx,dy],\"aligned\":bool=true,\"sampleLayer\":\"current|currentAndBelow|all\"=\"current\","
            "\"mode\":\"normal|…\"=\"normal\"}",
            has_retouch_target, cmd_healing_brush, true},
        {"paint.spotHealing", "Spot Healing Brush", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px=tool size,\"hardness\":0..100=tool "
            "hardness,\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 "
            "%,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\""
            ",\"type\":\"proximityMatch|contentAware\"=\"proximityMatch\",\"sampleAllLayers\":bool=false}",
            has_retouch_target, cmd_spot_healing, true},
        {"paint.dodge", "Dodge", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px=tool size,\"hardness\":0..100=tool "
            "hardness,\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 "
            "%,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\""
            ",\"range\":\"shadows|midtones|highlights\"=\"midtones\",\"exposure\":1..100=50,\"protectTones\":bool="
            "true}",
            has_retouch_target, cmd_dodge, true},
        {"paint.burn", "Burn", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px=tool size,\"hardness\":0..100=tool "
            "hardness,\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 "
            "%,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\""
            ",\"range\":\"shadows|midtones|highlights\"=\"midtones\",\"exposure\":1..100=50,\"protectTones\":bool="
            "true}",
            has_retouch_target, cmd_burn, true},
        {"paint.sponge", "Sponge", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px=tool size,\"hardness\":0..100=tool "
            "hardness,\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 "
            "%,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\""
            ",\"mode\":\"desaturate|saturate\"=\"desaturate\",\"vibrance\":bool=true (flow = strength)}",
            has_retouch_target, cmd_sponge, true},
        {"paint.blur", "Blur", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px=tool size,\"hardness\":0..100=tool "
            "hardness,\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 "
            "%,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\""
            ",\"strength\":1..100=50,\"sampleAllLayers\":bool=false}",
            has_retouch_target, cmd_blur, true},
        {"paint.sharpen", "Sharpen", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px=tool size,\"hardness\":0..100=tool "
            "hardness,\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 "
            "%,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\""
            ",\"strength\":1..100=50,\"protectDetail\":bool=true,\"sampleAllLayers\":bool=false}",
            has_retouch_target, cmd_sharpen, true},
        {"paint.smudge", "Smudge", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px=tool size,\"hardness\":0..100=tool "
            "hardness,\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 "
            "%,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\""
            ",\"strength\":1..100=50,\"fingerPainting\":bool=false,\"sampleAllLayers\":bool=false}",
            has_retouch_target, cmd_smudge, true},
        {"paint.historyBrush", "History Brush", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px=tool size,\"hardness\":0..100=tool "
            "hardness,\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 "
            "%,\"layer\":id?,\"target\":\"pixels|mask\"=\"pixels\""
            ",\"state\":index (default 0, the oldest held state)}",
            has_retouch_target, cmd_history_brush, true},
    };

    void register_retouch_commands (std::vector<CommandSpec>* out)
    {
        out->insert (
            out->end (), RETOUCH_COMMANDS, RETOUCH_COMMANDS + sizeof RETOUCH_COMMANDS / sizeof RETOUCH_COMMANDS[0]);
    }

} // namespace engine
