#include "algo/inpaint.h"
#include "compose/compose.h"
#include "engine/brush.h"
#include "engine/layers.h"
#include "engine/retouch.h"
#include "engine/session.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace engine
{

    /* The rest of the brush tools: Background Eraser, Color Replacement and Mixer Brush (dab by dab,
       like Dodge and Smudge), Pattern Stamp with the pattern library (Edit > Define Pattern), Art
       History Brush, and the content-aware tools: Remove (and Spot Healing's Content-Aware type,
       Edit > Fill > Content-Aware), Patch, Content-Aware Move and Red Eye. Content-aware filling is
       patch-based inpainting (algo/inpaint.cpp), not a learned model. */

    using geom::Rect;

    float clampf (float v, float lo, float hi);

    const char* const SAMPLING_IDS[3] = {"continuous", "once", "backgroundSwatch"};

    bool sampling_param (const Value& p, const char* cmd, int* out, Error* err)
    {
        std::string v = "continuous";
        param_str (p, "sampling", &v);
        for (int i = 0; i < 3; i++)
        {
            if (v == SAMPLING_IDS[i])
            {
                *out = i;
                return true;
            }
        }
        return bad_params (err, cmd, "`sampling` must be continuous, once or backgroundSwatch");
    }

    bool limits_param (const Value& p, const char* cmd, bool* contiguous, Error* err)
    {
        std::string v = "contiguous";
        param_str (p, "limits", &v);
        if (v != "contiguous" && v != "discontiguous" && v != "findEdges")
        {
            return bad_params (err, cmd, "`limits` must be contiguous, discontiguous or findEdges");
        }
        *contiguous = v != "discontiguous";
        return true;
    }

    /* Background Eraser, Color Replacement and Mixer Brush as sequential-dab tools. */
    bool make_brush_tool (
        const Session& s, const char* cmd, const Value& p, color::PixelFormat fmt, DabTool* t, Error* err)
    {
        t->has_sample = false;
        memcpy (t->fg, s.tools.foreground, sizeof t->fg);
        t->fg[3] = 1.0f;
        if (strcmp (cmd, "paint.backgroundEraser") == 0 || strcmp (cmd, "paint.colorReplacement") == 0)
        {
            bool eraser = strcmp (cmd, "paint.backgroundEraser") == 0;
            t->kind     = eraser ? DabKind::BackgroundErase : DabKind::ColorReplace;
            t->label    = eraser ? "Background Eraser Tool" : "Color Replacement Tool";
            if (!sampling_param (p, cmd, &t->sampling, err) || !limits_param (p, cmd, &t->contiguous, err))
            {
                return false;
            }
            t->tolerance  = clampf (pnum (p, "tolerance", eraser ? 50.0f : 30.0f), 0.0f, 100.0f) / 100.0f;
            t->protect_fg = eraser && pflag (p, "protectForeground", false);
            if (t->sampling == 2)
            {
                memcpy (t->sample, s.tools.background, sizeof t->sample);
                t->has_sample = true;
            }
            t->replace_mode = color::BlendMode::Color;
            if (!eraser)
            {
                std::string m = "color";
                param_str (p, "mode", &m);
                if (m == "hue")
                {
                    t->replace_mode = color::BlendMode::Hue;
                }
                else if (m == "saturation")
                {
                    t->replace_mode = color::BlendMode::Saturation;
                }
                else if (m == "luminosity")
                {
                    t->replace_mode = color::BlendMode::Luminosity;
                }
                else if (m != "color")
                {
                    return bad_params (err, cmd, "`mode` must be hue, saturation, color or luminosity");
                }
            }
            return true;
        }
        if (strcmp (cmd, "paint.mixerBrush") == 0)
        {
            t->kind  = DabKind::Mixer;
            t->label = "Mixer Brush Tool";
            t->wet   = clampf (pnum (p, "wet", 50.0f), 0.0f, 100.0f) / 100.0f;
            t->load  = clampf (pnum (p, "load", 50.0f), 1.0f, 100.0f) / 100.0f;
            t->mix   = clampf (pnum (p, "mix", 50.0f), 0.0f, 100.0f) / 100.0f;
            t->level = 1.0f;
            memcpy (t->reservoir, t->fg, sizeof t->reservoir);
            /* Clean (no load): the brush only moves the colour already there. */
            if (!pflag (p, "loaded", true))
            {
                t->level = 0.0f;
                t->wet   = fmaxf (t->wet, 0.5f);
            }
            t->all_layers = pflag (p, "sampleAllLayers", false) && !is_mask_target (p);
            return true;
        }
        return bad_params (err, cmd, "not a sequential-dab tool");
    }

    float color_dist (const float* a, const float* b)
    {
        return fmaxf (fabsf (a[0] - b[0]), fmaxf (fabsf (a[1] - b[1]), fabsf (a[2] - b[2])));
    }

    /* How much each pixel under the dab matches the sampled colour (soft near the tolerance), only
       the part connected to the dab's centre with Contiguous. */
    void match_map (DabTool* t, const paint::Region& work, const paint::Footprint& fp, Rect r, std::vector<float>* m)
    {
        int32_t cx = std::min (std::max ((int32_t) floor (fp.dab.cx), work.rect.x0), work.rect.x1 - 1);
        int32_t cy = std::min (std::max ((int32_t) floor (fp.dab.cy), work.rect.y0), work.rect.y1 - 1);
        if (!t->has_sample || t->sampling == 0)
        {
            raster::to_rgba (t->fmt, work.px (cx, cy), t->cmyk, t->sample);
            t->has_sample = true;
        }
        size_t w = r.width (), h = r.height ();
        m->assign (w * h, 0.0f);
        float tol = fmaxf (t->tolerance, 1e-3f), soft = fmaxf (tol * 0.3f, 1.0f / 255.0f);
        std::vector<float> d (w * h);
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                float rgba[4];
                raster::to_rgba (t->fmt, work.px (x, y), t->cmyk, rgba);
                float dist = color_dist (rgba, t->sample);
                if (t->protect_fg && color_dist (rgba, t->fg) < 0.06f)
                {
                    dist = 1e9f;
                }
                d[(size_t) (y - r.y0) * w + (size_t) (x - r.x0)] = dist;
            }
        }
        if (!t->contiguous)
        {
            for (size_t i = 0; i < d.size (); i++)
            {
                (*m)[i] = clampf ((tol - d[i]) / soft, 0.0f, 1.0f);
            }
            return;
        }
        if (!r.contains (cx, cy))
        {
            return;
        }
        std::vector<int32_t> stack (1, (cy - r.y0) * (int32_t) w + (cx - r.x0));
        std::vector<uint8_t> seen (w * h, 0);
        seen[(size_t) stack[0]] = 1;
        while (!stack.empty ())
        {
            int32_t i = stack.back ();
            stack.pop_back ();
            if (d[(size_t) i] > tol)
            {
                continue;
            }
            (*m)[(size_t) i] = clampf ((tol - d[(size_t) i]) / soft, 0.0f, 1.0f);
            int32_t x = i % (int32_t) w, y = i / (int32_t) w;
            const int NB[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int k = 0; k < 4; k++)
            {
                int32_t nx = x + NB[k][0], ny = y + NB[k][1];
                if (nx < 0 || ny < 0 || nx >= (int32_t) w || ny >= (int32_t) h)
                {
                    continue;
                }
                int32_t ni = ny * (int32_t) w + nx;
                if (!seen[(size_t) ni] && fp.at (r.x0 + nx, r.y0 + ny) > 0.0f)
                {
                    seen[(size_t) ni] = 1;
                    stack.push_back (ni);
                }
            }
        }
    }

    void background_erase_dab (DabTool* t, paint::Region* work, const paint::Footprint& fp)
    {
        int a  = paint::alpha_index (t->fmt);
        Rect r = fp.rect.intersect (work->rect);
        if (a < 0 || r.is_empty ())
        {
            return;
        }
        std::vector<float> m;
        match_map (t, *work, fp, r, &m);
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                float k = clampf (fp.at (x, y), 0.0f, 1.0f) * m[(size_t) (y - r.y0) * r.width () + (size_t) (x - r.x0)];
                if (k > 0.0f)
                {
                    work->px_mut (x, y)[a] *= 1.0f - k;
                }
            }
        }
    }

    void color_replace_dab (DabTool* t, paint::Region* work, const paint::Footprint& fp)
    {
        Rect r = fp.rect.intersect (work->rect);
        if (r.is_empty ())
        {
            return;
        }
        std::vector<float> m;
        match_map (t, *work, fp, r, &m);
        int a    = paint::alpha_index (t->fmt);
        size_t n = t->fmt.channels ();
        float enc[raster::MAX_CHANNELS];
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                float k = clampf (fp.at (x, y), 0.0f, 1.0f) * m[(size_t) (y - r.y0) * r.width () + (size_t) (x - r.x0)];
                if (k <= 0.0f)
                {
                    continue;
                }
                float* px = work->px_mut (x, y);
                float rgba[4], o[4];
                raster::to_rgba (t->fmt, px, t->cmyk, rgba);
                float back[4] = {rgba[0], rgba[1], rgba[2], 1.0f};
                color::composite (t->replace_mode, back, t->fg, k, o);
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

    /* The Mixer Brush: a reservoir of paint that picks up the canvas colour under each dab (Mix,
       scaled by Wet) and lays down a blend of itself and the wet canvas, running dry by Load. */
    void mixer_dab (DabTool* t, paint::Region* work, const paint::Footprint& fp)
    {
        Rect r = fp.rect.intersect (work->rect);
        if (r.is_empty ())
        {
            return;
        }
        double acc[4] = {0, 0, 0, 0}, wsum = 0;
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                float c = fp.at (x, y);
                if (c <= 0.0f)
                {
                    continue;
                }
                float rgba[4];
                raster::to_rgba (t->fmt, work->px (x, y), t->cmyk, rgba);
                for (int k = 0; k < 3; k++)
                {
                    acc[k] += rgba[k] * rgba[3] * c;
                }
                acc[3] += rgba[3] * c;
                wsum += c;
            }
        }
        if (acc[3] > 1e-6)
        {
            float pick = t->mix * t->wet;
            for (int k = 0; k < 3; k++)
            {
                t->reservoir[k] += ((float) (acc[k] / acc[3]) - t->reservoir[k]) * pick;
            }
        }
        size_t n = t->fmt.channels ();
        int a    = paint::alpha_index (t->fmt);
        float enc[raster::MAX_CHANNELS];
        float strength = fmaxf (t->level, t->wet * 0.6f);
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                float k = clampf (fp.at (x, y), 0.0f, 1.0f) * strength;
                if (k <= 0.0f)
                {
                    continue;
                }
                float* px = work->px_mut (x, y);
                float rgba[4];
                raster::to_rgba (t->fmt, px, t->cmyk, rgba);
                float o[4];
                for (int c = 0; c < 3; c++)
                {
                    float paint = t->reservoir[c] + (rgba[c] - t->reservoir[c]) * t->wet * 0.5f;
                    o[c]        = rgba[c] + (paint - rgba[c]) * k;
                }
                o[3] = rgba[3] + (1.0f - rgba[3]) * k * t->level;
                raster::from_rgba (t->fmt, o, t->cmyk, enc);
                for (size_t c = 0; c < n; c++)
                {
                    px[c] = (int) c == a ? o[3] : enc[c];
                }
            }
        }
        t->level *= 1.0f - (1.0f - t->load) * 0.04f;
    }

    void brush_tool_dab (DabTool* t, paint::Region* work, const paint::Footprint& fp)
    {
        switch (t->kind)
        {
        case DabKind::BackgroundErase:
            background_erase_dab (t, work, fp);
            break;
        case DabKind::ColorReplace:
            color_replace_dab (t, work, fp);
            break;
        case DabKind::Mixer:
            mixer_dab (t, work, fp);
            break;
        default:
            break;
        }
    }

    /* ---- Patterns ---- */

    Pattern make_pattern (const char* name, uint32_t w, uint32_t h)
    {
        Pattern p;
        p.name   = name;
        p.width  = w;
        p.height = h;
        p.rgba.assign ((size_t) w * h * 4, 1.0f);
        return p;
    }

    void set_gray (Pattern* p, uint32_t x, uint32_t y, float v)
    {
        float* o = &p->rgba[((size_t) y * p->width + x) * 4];
        o[0] = o[1] = o[2] = v;
        o[3]               = 1.0f;
    }

    float hash01 (uint32_t x, uint32_t y, uint32_t seed)
    {
        uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return (float) (h & 0xffff) / 65535.0f;
    }

    /* A few built-in tiles, as Photoshop's default patterns. */
    void ensure_patterns (Session& s)
    {
        if (!s.patterns.empty ())
        {
            return;
        }
        Pattern p = make_pattern ("Checkerboard", 32, 32);
        for (uint32_t y = 0; y < 32; y++)
        {
            for (uint32_t x = 0; x < 32; x++)
            {
                set_gray (&p, x, y, ((x / 16) ^ (y / 16)) & 1 ? 0.75f : 0.95f);
            }
        }
        s.patterns.push_back (p);
        p = make_pattern ("Diagonal Lines", 16, 16);
        for (uint32_t y = 0; y < 16; y++)
        {
            for (uint32_t x = 0; x < 16; x++)
            {
                set_gray (&p, x, y, ((x + y) % 16) < 4 ? 0.2f : 0.95f);
            }
        }
        s.patterns.push_back (p);
        p = make_pattern ("Dots", 24, 24);
        for (uint32_t y = 0; y < 24; y++)
        {
            for (uint32_t x = 0; x < 24; x++)
            {
                float dx = (float) x + 0.5f - 12.0f, dy = (float) y + 0.5f - 12.0f;
                set_gray (&p, x, y, 0.95f - 0.75f * clampf (6.5f - sqrtf (dx * dx + dy * dy), 0.0f, 1.0f));
            }
        }
        s.patterns.push_back (p);
        p = make_pattern ("Bricks", 64, 32);
        for (uint32_t y = 0; y < 32; y++)
        {
            for (uint32_t x = 0; x < 64; x++)
            {
                uint32_t sx = (y / 16) & 1 ? (x + 16) % 64 : x;
                bool mortar = y % 16 < 2 || sx % 32 < 2;
                float n     = hash01 (x, y, 7) * 0.08f;
                float* o    = &p.rgba[((size_t) y * 64 + x) * 4];
                if (mortar)
                {
                    o[0] = o[1] = o[2] = 0.82f + n;
                }
                else
                {
                    o[0] = 0.62f + n;
                    o[1] = 0.28f + n * 0.5f;
                    o[2] = 0.2f + n * 0.5f;
                }
            }
        }
        s.patterns.push_back (p);
        p = make_pattern ("Noise", 128, 128);
        for (uint32_t y = 0; y < 128; y++)
        {
            for (uint32_t x = 0; x < 128; x++)
            {
                set_gray (&p, x, y, hash01 (x, y, 3));
            }
        }
        s.patterns.push_back (p);
        p = make_pattern ("Canvas", 64, 64);
        for (uint32_t y = 0; y < 64; y++)
        {
            for (uint32_t x = 0; x < 64; x++)
            {
                /* A woven weave: alternating raised threads. */
                bool over    = ((x / 4) + (y / 4)) & 1;
                float thread = over ? 0.5f + 0.5f * sinf ((float) (y % 4) / 4.0f * 3.1416f)
                                    : 0.5f + 0.5f * sinf ((float) (x % 4) / 4.0f * 3.1416f);
                float v      = 0.78f + 0.14f * thread + (hash01 (x, y, 11) - 0.5f) * 0.05f;
                float* o     = &p.rgba[((size_t) y * 64 + x) * 4];
                o[0]         = v;
                o[1]         = v * 0.97f;
                o[2]         = v * 0.9f;
            }
        }
        s.patterns.push_back (p);
    }

    /* The pattern ("pattern" index, default the first) tiled over `area` from origin (ox, oy), in
       `fmt`. */
    bool pattern_source (Session& s, const Value& p, const char* cmd, color::PixelFormat fmt, Rect area, double ox,
        double oy, raster::Surface* out, Error* err)
    {
        ensure_patterns (s);
        int64_t i = 0;
        param_i64 (p, "pattern", &i);
        if (i < 0 || (size_t) i >= s.patterns.size ())
        {
            return bad_params (err, cmd, "no pattern %lld", (long long) i);
        }
        const Pattern& pat = s.patterns[(size_t) i];
        *out               = raster::Surface (fmt);
        if (area.is_empty ())
        {
            return true;
        }
        size_t n = fmt.channels (), count = (size_t) area.width () * area.height ();
        std::vector<float> vals (count * n);
        int64_t pw = pat.width, ph = pat.height, x0 = (int64_t) floor (ox), y0 = (int64_t) floor (oy);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) area.height (); yi++)
        {
            int64_t ty = (((int64_t) area.y0 + yi - y0) % ph + ph) % ph;
            for (uint32_t x = 0; x < area.width (); x++)
            {
                int64_t tx = (((int64_t) area.x0 + x - x0) % pw + pw) % pw;
                raster::from_rgba (fmt, &pat.rgba[((size_t) ty * pat.width + (size_t) tx) * 4], s.cmyk,
                    &vals[((size_t) yi * area.width () + x) * n]);
            }
        }
        out->write_region (area, vals.data (), vals.size ());
        return true;
    }

    /* Edit > Define Pattern: the visible image inside the selection's bounds (or the canvas). */
    bool cmd_define_pattern (Session& s, const Value& p, Value* result, Error* err)
    {
        const doc::Document& d = *s.active ()->doc;
        Rect r = (d.has_selection ? d.selection.content_bounds () : d.bounds ()).intersect (d.bounds ());
        if (r.is_empty ())
        {
            return fail (err, ErrorKind::Other, "nothing is selected");
        }
        if (r.width () > 2048 || r.height () > 2048)
        {
            return fail (err, ErrorKind::Other, "patterns are at most 2048 px a side");
        }
        compose::RenderSettings rs = compose::default_settings ();
        rs.cmyk                    = s.cmyk;
        compose::Buffer b          = compose::render (d, r, rs);
        ensure_patterns (s);
        Pattern pat;
        pat.name = d.name;
        param_str (p, "name", &pat.name);
        pat.width  = r.width ();
        pat.height = r.height ();
        pat.rgba   = b.px;
        s.patterns.push_back (pat);
        *result = Value{{"pattern", s.patterns.size () - 1}};
        return true;
    }

    bool cmd_patterns_list (Session& s, const Value& p, Value* result, Error* err)
    {
        ensure_patterns (s);
        Value list = Value::array ();
        for (size_t i = 0; i < s.patterns.size (); i++)
        {
            list.push_back (
                Value{{"name", s.patterns[i].name}, {"width", s.patterns[i].width}, {"height", s.patterns[i].height}});
        }
        *result = Value{{"patterns", list}};
        return true;
    }

    bool cmd_patterns_delete (Session& s, const Value& p, Value* result, Error* err)
    {
        ensure_patterns (s);
        int64_t i;
        if (!param_i64 (p, "pattern", &i) || i < 0 || (size_t) i >= s.patterns.size ())
        {
            return bad_params (err, "patterns.delete", "no pattern at `pattern`");
        }
        s.patterns.erase (s.patterns.begin () + (ptrdiff_t) i);
        return true;
    }

    bool cmd_patterns_rename (Session& s, const Value& p, Value* result, Error* err)
    {
        ensure_patterns (s);
        int64_t i;
        std::string name;
        if (!param_i64 (p, "pattern", &i) || i < 0 || (size_t) i >= s.patterns.size ())
        {
            return bad_params (err, "patterns.rename", "no pattern at `pattern`");
        }
        if (!param_str (p, "name", &name) || name.empty ())
        {
            return bad_params (err, "patterns.rename", "`name` must be a non-empty string");
        }
        s.patterns[(size_t) i].name = name;
        return true;
    }

    /* Pattern Stamp: the pattern painted through the brush, aligned to the canvas or (unaligned)
       starting at each stroke. */
    bool cmd_pattern_stamp (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "paint.patternStamp";
        paint::BrushSettings brush;
        std::vector<paint::StrokePoint> pts;
        doc::LayerId id;
        if (!retouch_brush (s, p, cmd, &brush, &pts, &id, err))
        {
            return false;
        }
        color::BlendMode mode = color::BlendMode::Normal;
        std::string m;
        if (param_str (p, "mode", &m) && !blend_from_str (m.c_str (), &mode))
        {
            return bad_params (err, cmd, "unknown blend mode");
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
        raster::Surface src;
        if (!pattern_source (
                s, p, cmd, t.surf->format (), bounds, aligned ? 0.0 : pts[0].x, aligned ? 0.0 : pts[0].y, &src, err))
        {
            return false;
        }
        Rect dmg = paint::apply_coverage (
            t.surf, bounds, cov, brush.opacity, t.sel, t.lock, paint::Region::read (src, bounds), mode, s.cmyk);
        if (!e.commit ("Pattern Stamp", err))
        {
            return false;
        }
        *result = Value{{"damage", {dmg.x0, dmg.y0, dmg.width (), dmg.height ()}}};
        return true;
    }

    /* ---- Content-aware ---- */

    bool content_aware_region (
        const paint::Region& img, const std::vector<bool>& hole, uint64_t seed, paint::Region* out)
    {
        *out = img;
        return algo::inpaint (img.width (), img.height (), img.ch, &out->data, hole, NULL, seed);
    }

    /* `hole` (over `win`) filled from its surroundings on the target and laid on through `cov`
       (over `cr`). */
    Rect fill_hole (Session& s, StrokeTarget* t, doc::LayerId id, bool all, Rect win, const std::vector<bool>& hole,
        Rect cr, const std::vector<float>& cov, float opacity, uint64_t seed)
    {
        color::PixelFormat fmt = t->surf->format ();
        paint::Region img      = all ? composite_region (*t->pre, doc::NO_LAYER, SampleLayers::All, win, fmt, s.cmyk)
                                     : paint::Region::read (*t->surf, win);
        paint::Region filled;
        if (!content_aware_region (img, hole, seed, &filled))
        {
            return Rect ();
        }
        clamp_samples (fmt, &filled.data);
        (void) id;
        return paint::apply_coverage (
            t->surf, cr, cov, opacity, t->sel, t->lock, filled.crop (cr), color::BlendMode::Normal, s.cmyk);
    }

    /* Remove (and Spot Healing's Content-Aware type): what the stroke covers is filled from the
       image around it. */
    bool remove_stroke (Session& s, const Value& p, const char* cmd, const char* label, Value* result, Error* err)
    {
        paint::BrushSettings brush;
        std::vector<paint::StrokePoint> pts;
        doc::LayerId id;
        if (!retouch_brush (s, p, cmd, &brush, &pts, &id, err))
        {
            return false;
        }
        bool all = pflag (p, "sampleAllLayers", true) && !is_mask_target (p);
        Edit e (s);
        StrokeTarget t;
        if (!begin_target (&e, id, p, &t, err))
        {
            return false;
        }
        Rect bounds;
        std::vector<float> cov;
        paint::stroke_coverage (brush, pts, &bounds, &cov);
        Rect win = sampling_window (bounds.inflate (4), t.pre->bounds ());
        Rect dmg;
        if (!win.is_empty ())
        {
            size_t w = win.width (), h = win.height (), bw = bounds.width ();
            std::vector<bool> hole (w * h, false);
            for (size_t i = 0; i < cov.size (); i++)
            {
                int32_t x = bounds.x0 + (int32_t) (i % bw), y = bounds.y0 + (int32_t) (i / bw);
                if (cov[i] > 0.0f && win.contains (x, y))
                {
                    hole[(size_t) (y - win.y0) * w + (size_t) (x - win.x0)] = true;
                }
            }
            hole = dilate (w, h, hole, 2);
            /* Lay the fill on fully inside the stroke, softly at its rim. */
            Rect cr = bounds.intersect (win);
            std::vector<float> c2 ((size_t) cr.width () * cr.height ());
            for (int32_t y = cr.y0; y < cr.y1; y++)
            {
                for (int32_t x = cr.x0; x < cr.x1; x++)
                {
                    c2[(size_t) (y - cr.y0) * cr.width () + (size_t) (x - cr.x0)] =
                        clampf (cov[(size_t) (y - bounds.y0) * bw + (size_t) (x - bounds.x0)] * 2.0f, 0.0f, 1.0f);
                }
            }
            dmg = fill_hole (s, &t, id, all, win, hole, cr, c2, brush.opacity, (uint64_t) (pts[0].x * 7919 + pts[0].y));
        }
        if (!e.commit (label, err))
        {
            return false;
        }
        *result = Value{{"damage", {dmg.x0, dmg.y0, dmg.width (), dmg.height ()}}};
        return true;
    }

    bool cmd_remove (Session& s, const Value& p, Value* result, Error* err)
    {
        return remove_stroke (s, p, "paint.remove", "Remove Tool", result, err);
    }

    /* Edit > Fill > Content-Aware: the selection filled from around it. */
    bool fill_content_aware (Session& s, Edit* e, doc::LayerId id, float opacity, Error* err)
    {
        doc::Document* d = e->doc ();
        if (!d->has_selection)
        {
            return fail (err, ErrorKind::Other, "Content-Aware Fill needs a selection");
        }
        StrokeTarget t;
        if (!begin_target (e, id, Value::object (), &t, err))
        {
            return false;
        }
        Rect area = d->selection.content_bounds ().intersect (d->bounds ());
        if (area.is_empty ())
        {
            return true;
        }
        Rect win = sampling_window (area.inflate (8), d->bounds ());
        size_t w = win.width (), h = win.height ();
        std::vector<bool> hole (w * h, false);
        for (int32_t y = area.y0; y < area.y1; y++)
        {
            for (int32_t x = area.x0; x < area.x1; x++)
            {
                if (win.contains (x, y) && d->selection.sample_channel (x, y, 0) > 0.0f)
                {
                    hole[(size_t) (y - win.y0) * w + (size_t) (x - win.x0)] = true;
                }
            }
        }
        hole = dilate (w, h, hole, 2);
        std::vector<float> cov ((size_t) area.width () * area.height (), 1.0f);
        /* The selection shapes the result (feathered edges included) through apply_coverage. */
        if (fill_hole (s, &t, id, false, win, hole, area, cov, opacity, 17).is_empty () && !area.is_empty ())
        {
            return fail (err, ErrorKind::Other, "there is nothing around the selection to fill it from");
        }
        return true;
    }

    /* Edit > Fill > Pattern. */
    bool fill_pattern (
        Session& s, Edit* e, doc::LayerId id, const Value& p, float opacity, color::BlendMode mode, Error* err)
    {
        doc::Document* d = e->doc ();
        StrokeTarget t;
        if (!begin_target (e, id, p, &t, err))
        {
            return false;
        }
        Rect area = (d->has_selection ? d->selection.content_bounds () : d->bounds ()).intersect (d->bounds ());
        if (area.is_empty ())
        {
            return true;
        }
        raster::Surface src;
        if (!pattern_source (s, p, "edit.fill", t.surf->format (), area, 0, 0, &src, err))
        {
            return false;
        }
        std::vector<float> cov ((size_t) area.width () * area.height (), 1.0f);
        paint::apply_coverage (
            t.surf, area, cov, opacity, t.sel, t.lock, paint::Region::read (src, area), mode, s.cmyk);
        return true;
    }

    /* ---- Red Eye, Patch, Content-Aware Move ---- */

    float redness (const float* c)
    {
        return fmaxf (c[0] - fmaxf (c[1], c[2]), 0.0f);
    }

    /* Red Eye: the red pupil around the click found (connected pixels at least a share of the
       reddest one's redness, the share set by Pupil Size), its red taken down to the other channels
       and darkened. */
    bool cmd_red_eye (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "paint.redEye";
        double cx, cy;
        if (!param_f64 (p, "x", &cx) || !param_f64 (p, "y", &cy))
        {
            return bad_params (err, cmd, "missing `x`, `y`");
        }
        float pupil  = clampf (pnum (p, "pupilSize", 50.0f), 1.0f, 100.0f) / 100.0f,
              darken = clampf (pnum (p, "darken", 50.0f), 1.0f, 100.0f) / 100.0f;
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        StrokeTarget t;
        if (!begin_target (&e, id, p, &t, err))
        {
            return false;
        }
        int32_t px = (int32_t) floor (cx), py = (int32_t) floor (cy);
        Rect win = Rect::from_xywh (px - 80, py - 80, 161, 161).intersect (t.pre->bounds ());
        if (!win.contains (px, py))
        {
            return fail (err, ErrorKind::Other, "click inside the image");
        }
        paint::Region img      = paint::Region::read (*t.surf, win);
        color::PixelFormat fmt = t.surf->format ();
        size_t w = win.width (), h = win.height ();
        std::vector<float> red (w * h), rgba (w * h * 4);
        for (size_t i = 0; i < w * h; i++)
        {
            raster::to_rgba (fmt, &img.data[i * img.ch], s.cmyk, &rgba[i * 4]);
            red[i] = redness (&rgba[i * 4]);
        }
        /* The seed: the reddest pixel near the click. */
        float best  = 0.0f;
        size_t seed = 0;
        for (int32_t y = py - 10; y <= py + 10; y++)
        {
            for (int32_t x = px - 10; x <= px + 10; x++)
            {
                if (!win.contains (x, y))
                {
                    continue;
                }
                size_t i = (size_t) (y - win.y0) * w + (size_t) (x - win.x0);
                if (red[i] > best)
                {
                    best = red[i];
                    seed = i;
                }
            }
        }
        if (best < 0.1f)
        {
            *result = Value{{"found", false}};
            return e.commit ("Red Eye Tool", err);
        }
        float thr = best * (1.0f - pupil * 0.75f);
        std::vector<bool> mask (w * h, false);
        std::vector<size_t> stack (1, seed);
        mask[seed] = true;
        while (!stack.empty ())
        {
            size_t i = stack.back ();
            stack.pop_back ();
            int32_t x = (int32_t) (i % w), y = (int32_t) (i / w);
            for (int dy = -1; dy <= 1; dy++)
            {
                for (int dx = -1; dx <= 1; dx++)
                {
                    int32_t nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= (int32_t) w || ny >= (int32_t) h)
                    {
                        continue;
                    }
                    size_t ni = (size_t) ny * w + (size_t) nx;
                    if (!mask[ni] && red[ni] >= thr)
                    {
                        mask[ni] = true;
                        stack.push_back (ni);
                    }
                }
            }
        }
        mask = dilate (w, h, mask, 1);
        std::vector<float> cov (w * h, 0.0f);
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                float sum = 0;
                int n     = 0;
                for (int dy = -1; dy <= 1; dy++)
                {
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        int64_t nx = (int64_t) x + dx, ny = (int64_t) y + dy;
                        if (nx < 0 || ny < 0 || nx >= (int64_t) w || ny >= (int64_t) h)
                        {
                            continue;
                        }
                        sum += mask[(size_t) ny * w + (size_t) nx] ? 1.0f : 0.0f;
                        n++;
                    }
                }
                cov[y * w + x] = sum / (float) n;
            }
        }
        paint::Region fixed = img;
        for (size_t i = 0; i < w * h; i++)
        {
            if (cov[i] <= 0.0f)
            {
                continue;
            }
            float* c   = &rgba[i * 4];
            float o[4] = {(c[1] + c[2]) * 0.5f, c[1], c[2], c[3]};
            for (int k = 0; k < 3; k++)
            {
                o[k] *= 1.0f - darken * 0.6f;
            }
            raster::from_rgba (fmt, o, s.cmyk, &fixed.data[i * img.ch]);
        }
        paint::apply_coverage (t.surf, win, cov, 1.0f, t.sel, t.lock, fixed, color::BlendMode::Normal, s.cmyk);
        if (!e.commit ("Red Eye Tool", err))
        {
            return false;
        }
        *result = Value{{"found", true}};
        return true;
    }

    bool offset_param (const Value& p, const char* cmd, int32_t* dx, int32_t* dy, Error* err)
    {
        const Value* o = param (p, "offset");
        if (o == NULL || !o->is_array () || o->size () != 2 || !(*o)[0].is_number () || !(*o)[1].is_number ())
        {
            return bad_params (err, cmd, "`offset` must be [dx, dy]");
        }
        *dx = (int32_t) lround ((*o)[0].get<double> ());
        *dy = (int32_t) lround ((*o)[1].get<double> ());
        return true;
    }

    /* The selection's values over r, and as a hard mask. */
    void selection_over (
        const doc::Document& d, Rect r, int32_t dx, int32_t dy, std::vector<float>* cov, std::vector<bool>* mask)
    {
        size_t w = r.width ();
        cov->assign (w * r.height (), 0.0f);
        mask->assign (w * r.height (), false);
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                float v    = d.selection.sample_channel (x - dx, y - dy, 0);
                size_t i   = (size_t) (y - r.y0) * w + (size_t) (x - r.x0);
                (*cov)[i]  = v;
                (*mask)[i] = v > 0.0f;
            }
        }
    }

    /* The selection's content moved by (dx, dy) and healed into where it lands. */
    void heal_moved (Session& s, StrokeTarget* t, const paint::Region& source_img, Rect area, int32_t dx, int32_t dy)
    {
        const doc::Document& d = *t->pre;
        Rect dst_rect          = area.translate (dx, dy).inflate (2).intersect (d.bounds ());
        if (dst_rect.is_empty ())
        {
            return;
        }
        color::PixelFormat fmt = t->surf->format ();
        paint::Region dst      = paint::Region::read (*t->surf, dst_rect);
        paint::Region src      = paint::Region::make (dst_rect, dst.ch);
        for (int32_t y = dst_rect.y0; y < dst_rect.y1; y++)
        {
            for (int32_t x = dst_rect.x0; x < dst_rect.x1; x++)
            {
                int32_t sx = std::min (std::max (x - dx, source_img.rect.x0), source_img.rect.x1 - 1),
                        sy = std::min (std::max (y - dy, source_img.rect.y0), source_img.rect.y1 - 1);
                memcpy (src.px_mut (x, y), source_img.px (sx, sy), dst.ch * sizeof (float));
            }
        }
        std::vector<float> cov;
        std::vector<bool> mask;
        selection_over (d, dst_rect, dx, dy, &cov, &mask);
        paint::Region healed = heal_region (fmt, src, dst, mask);
        paint::apply_coverage (t->surf, dst_rect, cov, 1.0f, NULL, t->lock, healed, color::BlendMode::Normal, s.cmyk);
    }

    /* Patch: the selection healed with the pixels at the offset (Source), or its own pixels healed
       into the area at the offset (Destination). */
    bool cmd_patch (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "paint.patch";
        int32_t dx, dy;
        if (!offset_param (p, cmd, &dx, &dy, err))
        {
            return false;
        }
        std::string which = "source";
        param_str (p, "patch", &which);
        if (which != "source" && which != "destination")
        {
            return bad_params (err, cmd, "`patch` must be source or destination");
        }
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (!d->has_selection)
        {
            return fail (err, ErrorKind::Other, "make a selection to patch first");
        }
        StrokeTarget t;
        if (!begin_target (&e, id, p, &t, err))
        {
            return false;
        }
        Rect area = d->selection.content_bounds ().intersect (d->bounds ());
        if (area.is_empty ())
        {
            return true;
        }
        t.sel                  = NULL;
        color::PixelFormat fmt = t.surf->format ();
        if (which == "source")
        {
            Rect g                 = area.inflate (2).intersect (d->bounds ());
            paint::Region dst      = paint::Region::read (*t.surf, g);
            paint::Region src      = paint::Region::make (g, dst.ch);
            raster::Surface before = *t.surf;
            for (int32_t y = g.y0; y < g.y1; y++)
            {
                for (int32_t x = g.x0; x < g.x1; x++)
                {
                    float px[raster::MAX_CHANNELS];
                    int32_t sx = std::min (std::max (x + dx, d->bounds ().x0), d->bounds ().x1 - 1),
                            sy = std::min (std::max (y + dy, d->bounds ().y0), d->bounds ().y1 - 1);
                    before.read_pixel (sx, sy, px);
                    memcpy (src.px_mut (x, y), px, dst.ch * sizeof (float));
                }
            }
            std::vector<float> cov;
            std::vector<bool> mask;
            selection_over (*d, g, 0, 0, &cov, &mask);
            paint::Region healed = heal_region (fmt, src, dst, mask);
            paint::apply_coverage (t.surf, g, cov, 1.0f, NULL, t.lock, healed, color::BlendMode::Normal, s.cmyk);
        }
        else
        {
            heal_moved (s, &t, paint::Region::read (*t.surf, area), area, dx, dy);
        }
        return e.commit ("Patch Tool", err);
    }

    /* Content-Aware Move: the selection's content placed at the offset and healed in; Move fills
       where it was from its surroundings, Extend leaves it. The selection follows. */
    bool cmd_content_aware_move (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "paint.contentAwareMove";
        int32_t dx, dy;
        if (!offset_param (p, cmd, &dx, &dy, err))
        {
            return false;
        }
        std::string mode = "move";
        param_str (p, "mode", &mode);
        if (mode != "move" && mode != "extend")
        {
            return bad_params (err, cmd, "`mode` must be move or extend");
        }
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (!d->has_selection)
        {
            return fail (err, ErrorKind::Other, "select what to move first");
        }
        StrokeTarget t;
        if (!begin_target (&e, id, p, &t, err))
        {
            return false;
        }
        Rect area = d->selection.content_bounds ().intersect (d->bounds ());
        if (area.is_empty ())
        {
            return true;
        }
        t.sel                = NULL;
        paint::Region moving = paint::Region::read (*t.surf, area);
        if (mode == "move")
        {
            Rect win = sampling_window (area.inflate (8), d->bounds ());
            size_t w = win.width (), h = win.height ();
            std::vector<bool> hole (w * h, false);
            for (int32_t y = area.y0; y < area.y1; y++)
            {
                for (int32_t x = area.x0; x < area.x1; x++)
                {
                    if (win.contains (x, y) && d->selection.sample_channel (x, y, 0) > 0.0f)
                    {
                        hole[(size_t) (y - win.y0) * w + (size_t) (x - win.x0)] = true;
                    }
                }
            }
            hole = dilate (w, h, hole, 2);
            std::vector<float> cov;
            std::vector<bool> mask;
            Rect g = area.inflate (2).intersect (win);
            selection_over (*d, g, 0, 0, &cov, &mask);
            for (size_t i = 0; i < cov.size (); i++)
            {
                cov[i] = clampf (cov[i] * 2.0f, 0.0f, 1.0f);
            }
            fill_hole (s, &t, id, false, win, hole, g, cov, 1.0f, 23);
        }
        heal_moved (s, &t, moving, area, dx, dy);
        d->selection = d->selection.translated (dx, dy, d->selection.content_bounds ());
        return e.commit ("Content-Aware Move Tool", err);
    }

    /* ---- Art History Brush ---- */

    const char* const ART_STYLES[10] = {"tightShort", "tightMedium", "tightLong", "looseMedium", "looseLong", "dab",
        "tightCurl", "tightCurlLong", "looseCurl", "looseCurlLong"};

    /* Art History Brush: many short strokes in the history state's colours, scattered over Area
       around the pointer, shaped by the style; with Tolerance, only where the image differs from
       that state. */
    bool cmd_art_history (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "paint.artHistory";
        paint::BrushSettings brush;
        std::vector<paint::StrokePoint> pts;
        doc::LayerId id;
        if (!retouch_brush (s, p, cmd, &brush, &pts, &id, err))
        {
            return false;
        }
        std::string style = "tightShort";
        param_str (p, "style", &style);
        int si = -1;
        for (int i = 0; i < 10; i++)
        {
            if (style == ART_STYLES[i])
            {
                si = i;
            }
        }
        if (si < 0)
        {
            return bad_params (err, cmd, "unknown style `%s`", style.c_str ());
        }
        float area = clampf (pnum (p, "area", 50.0f), 0.0f, 500.0f),
              tol  = clampf (pnum (p, "tolerance", 0.0f), 0.0f, 100.0f) / 100.0f;
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
        color::PixelFormat fmt = t.surf->format ();
        bool loose = si == 3 || si == 4 || si == 8 || si == 9, curl = si >= 6;
        float len                  = si == 5 ? 0.0f
                                             : (si == 0 || si == 6 || si == 8 ? 3.0f
                                                   : si == 1 || si == 3       ? 6.0f
                                                                              : 11.0f)
                                  * brush.size;
        paint::BrushSettings small = brush;
        small.opacity              = 1.0f;
        uint64_t seed              = (uint64_t) (pts[0].x * 131 + pts[0].y * 17) + 1;
        Rect dmg;
        float step       = fmaxf (brush.size * 0.75f, 2.0f);
        double travelled = step;
        for (size_t i = 0; i < pts.size (); i++)
        {
            if (i > 0)
            {
                travelled += hypot (pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
            }
            if (travelled < step)
            {
                continue;
            }
            travelled = 0;
            for (int k = 0; k < 3; k++)
            {
                seed     = seed * 6364136223846793005ull + 1442695040888963407ull;
                float r1 = (float) ((seed >> 33) & 0xffff) / 65535.0f, r2 = (float) ((seed >> 17) & 0xffff) / 65535.0f,
                      r3  = (float) ((seed >> 1) & 0xffff) / 65535.0f;
                double sx = pts[i].x + (r1 - 0.5) * area, sy = pts[i].y + (r2 - 0.5) * area;
                float c[raster::MAX_CHANNELS], cur[raster::MAX_CHANNELS], a[4], b[4];
                src.read_pixel ((int32_t) floor (sx), (int32_t) floor (sy), c);
                if (tol > 0.0f)
                {
                    t.surf->read_pixel ((int32_t) floor (sx), (int32_t) floor (sy), cur);
                    raster::to_rgba (fmt, c, s.cmyk, a);
                    raster::to_rgba (fmt, cur, s.cmyk, b);
                    if (color_dist (a, b) < tol)
                    {
                        continue;
                    }
                }
                /* The stroke follows the image's local direction loosely (random for loose styles). */
                float ang = r3 * 6.2832f;
                std::vector<paint::StrokePoint> sp;
                int segs = len <= 0.0f ? 1 : 8;
                double x = sx, y = sy;
                for (int q = 0; q <= segs; q++)
                {
                    sp.push_back (paint::StrokePoint::make (x, y, 1.0f));
                    float turn = curl ? 0.45f : loose ? (r1 - 0.5f) * 0.6f : 0.05f;
                    ang += turn;
                    x += cos (ang) * len / segs;
                    y += sin (ang) * len / segs;
                }
                Rect bounds;
                std::vector<float> cov;
                paint::stroke_coverage (small, sp, &bounds, &cov);
                if (bounds.is_empty ())
                {
                    continue;
                }
                paint::Region paint_px = paint::Region::make (bounds, fmt.channels ());
                for (size_t q = 0; q < (size_t) bounds.width () * bounds.height (); q++)
                {
                    memcpy (&paint_px.data[q * fmt.channels ()], c, fmt.channels () * sizeof (float));
                }
                dmg = dmg.unite (paint::apply_coverage (
                    t.surf, bounds, cov, brush.opacity, t.sel, t.lock, paint_px, color::BlendMode::Normal, s.cmyk));
            }
        }
        if (!e.commit ("Art History Brush", err))
        {
            return false;
        }
        *result = Value{{"damage", {dmg.x0, dmg.y0, dmg.width (), dmg.height ()}}};
        return true;
    }

    /* ---- Dab tools as commands ---- */

    bool dab_cmd (Session& s, const Value& p, const char* cmd, Value* result, Error* err);

    bool cmd_background_eraser (Session& s, const Value& p, Value* result, Error* err)
    {
        /* A Background becomes a normal layer first, as in Photoshop. */
        if (s.active () != NULL && !is_mask_target (p))
        {
            doc::LayerId id;
            if (layer_param (s, p, &id, err) && s.active ()->doc->is_background (id))
            {
                Edit e (s);
                background_to_layer (e.doc (), id);
                if (!e.commit ("Layer 0", err))
                {
                    return false;
                }
            }
        }
        return dab_cmd (s, p, "paint.backgroundEraser", result, err);
    }

    bool cmd_color_replacement (Session& s, const Value& p, Value* result, Error* err)
    {
        return dab_cmd (s, p, "paint.colorReplacement", result, err);
    }

    bool cmd_mixer_brush (Session& s, const Value& p, Value* result, Error* err)
    {
        return dab_cmd (s, p, "paint.mixerBrush", result, err);
    }

    bool can_brush_tool (const Session& s, std::string* why)
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

    const CommandSpec BRUSH_TOOL_COMMANDS[] = {
        {"paint.backgroundEraser", "Background Eraser", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px,\"hardness\":0..100,\"opacity\":1..100=100,\"flow\":1..100=100,"
            "\"layer\":id?,\"target\":\"pixels|mask\""
            ",\"sampling\":\"continuous|once|backgroundSwatch\",\"limits\":\"contiguous|discontiguous|findEdges\","
            "\"tolerance\":0..100=50,\"protectForeground\":bool=false}",
            can_brush_tool, cmd_background_eraser, true},
        {"paint.colorReplacement", "Color Replacement", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px,\"hardness\":0..100,\"opacity\":1..100=100,\"flow\":1..100=100,"
            "\"layer\":id?,\"target\":\"pixels|mask\""
            ",\"mode\":\"hue|saturation|color|luminosity\"=\"color\",\"sampling\":\"continuous|once|backgroundSwatch\","
            "\"limits\":…,\"tolerance\":0..100=30}",
            can_brush_tool, cmd_color_replacement, true},
        {"paint.mixerBrush", "Mixer Brush", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px,\"hardness\":0..100,\"opacity\":1..100=100,\"flow\":1..100=100,"
            "\"layer\":id?,\"target\":\"pixels|mask\""
            ",\"wet\":0..100=50,\"load\":1..100=50,\"mix\":0..100=50,\"loaded\":bool=true,\"sampleAllLayers\":bool="
            "false}",
            can_brush_tool, cmd_mixer_brush, true},
        {"paint.patternStamp", "Pattern Stamp", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px,\"hardness\":0..100,\"opacity\":1..100=100,\"flow\":1..100=100,"
            "\"layer\":id?,\"target\":\"pixels|mask\""
            ",\"pattern\":index=0,\"aligned\":bool=true,\"mode\":blend}",
            can_brush_tool, cmd_pattern_stamp, true},
        {"paint.artHistory", "Art History Brush", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px,\"hardness\":0..100,\"opacity\":1..100=100,\"flow\":1..100=100,"
            "\"layer\":id?,\"target\":\"pixels|mask\""
            ",\"style\":\"tightShort|tightMedium|tightLong|looseMedium|looseLong|dab|tightCurl|tightCurlLong|looseCurl|"
            "looseCurlLong\",\"area\":px=50,"
            "\"tolerance\":0..100=0,\"state\":history index?}",
            can_brush_tool, cmd_art_history, true},
        {"paint.remove", "Remove", "", NULL,
            "{"
            "\"points\":[[x,y,pressure?],…],\"size\":px,\"hardness\":0..100,\"opacity\":1..100=100,\"flow\":1..100=100,"
            "\"layer\":id?,\"target\":\"pixels|mask\""
            ",\"sampleAllLayers\":bool=true}",
            can_brush_tool, cmd_remove, true},
        {"paint.redEye", "Red Eye", "", NULL,
            "{\"x\":px,\"y\":px,\"pupilSize\":1..100=50,\"darken\":1..100=50,\"layer\":id?}", can_brush_tool,
            cmd_red_eye, true},
        {"paint.patch", "Patch", "", NULL,
            "{\"offset\":[dx,dy],\"patch\":\"source|destination\"=\"source\",\"layer\":id?} (the selection)",
            can_brush_tool, cmd_patch, true},
        {"paint.contentAwareMove", "Content-Aware Move", "", NULL,
            "{\"offset\":[dx,dy],\"mode\":\"move|extend\"=\"move\",\"layer\":id?} (the selection)", can_brush_tool,
            cmd_content_aware_move, true},
        {"edit.definePattern", "Define Pattern…", "Edit", NULL,
            "{\"name\":str?} (the selection's bounds, or the canvas)", has_doc, cmd_define_pattern, true},
        {"patterns.list", "Patterns", "", NULL, "{} → {patterns:[{name, width, height}]}", always, cmd_patterns_list,
            false},
        {"patterns.delete", "Delete Pattern", "", NULL, "{\"pattern\":index}", always, cmd_patterns_delete, false},
        {"patterns.rename", "Rename Pattern", "", NULL, "{\"pattern\":index,\"name\":str}", always, cmd_patterns_rename,
            false},
    };

    void register_brush_tool_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), BRUSH_TOOL_COMMANDS,
            BRUSH_TOOL_COMMANDS + sizeof BRUSH_TOOL_COMMANDS / sizeof BRUSH_TOOL_COMMANDS[0]);
    }

} // namespace engine
