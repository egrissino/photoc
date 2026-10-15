#include "compose/masks.h"
#include "engine/filter_ops.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

namespace engine
{

    /* Filter > Distort: each pixel of the area takes the colour found at a mapped position
       (bilinear, premultiplied). Pinch, Spherize, Twirl and ZigZag act inside the ellipse that fits
       the area; Polar Coordinates, Ripple, Wave, Shear and Displace across the area, their
       undefined parts wrapping around it or repeating its edge pixels. */

    struct Warp
    {
        float x0;
        float y0;
        float x1;
        float y1;
        float cx;
        float cy;
        float ax;
        float ay;
        float a;
        float b;
        int mode;
        bool wrap;
        std::vector<float> row_dx;
        std::vector<float> col_dy;
        std::vector<float> map_dx;
        std::vector<float> map_dy;
        size_t w;
    };

    typedef void (*WarpFn) (const Warp& wp, float x, float y, float* sx, float* sy);

    Warp make_warp (const Plane& pl, const FilterCtx& cx)
    {
        Warp wp;
        wp.x0 = (float) (cx.area.x0 - cx.src.x0);
        wp.y0 = (float) (cx.area.y0 - cx.src.y0);
        wp.x1 = wp.x0 + (float) cx.area.width ();
        wp.y1 = wp.y0 + (float) cx.area.height ();
        wp.cx = (wp.x0 + wp.x1) * 0.5f;
        wp.cy = (wp.y0 + wp.y1) * 0.5f;
        wp.ax = (wp.x1 - wp.x0) * 0.5f;
        wp.ay = (wp.y1 - wp.y0) * 0.5f;
        wp.a = wp.b = 0.0f;
        wp.mode     = 0;
        wp.wrap     = false;
        wp.w        = pl.w;
        return wp;
    }

    /* Every area pixel through fn (dest centre -> source point). */
    void warp_plane (Plane* pl, const Warp& wp, WarpFn fn)
    {
        std::vector<float> src = pl->px;
        size_t n               = pl->n;
        int ix0 = (int) wp.x0, iy0 = (int) wp.y0, ix1 = (int) wp.x1, iy1 = (int) wp.y1;
        float aw = wp.x1 - wp.x0, ah = wp.y1 - wp.y0;
#pragma omp parallel for schedule(dynamic, 8)
        for (ptrdiff_t yi = iy0; yi < iy1; yi++)
        {
            float q[raster::MAX_CHANNELS];
            for (int x = ix0; x < ix1; x++)
            {
                float sx, sy;
                fn (wp, (float) x + 0.5f, (float) yi + 0.5f, &sx, &sy);
                sx -= 0.5f;
                sy -= 0.5f;
                if (wp.wrap)
                {
                    sx = wp.x0 + fmodf (fmodf (sx - wp.x0, aw) + aw, aw);
                    sy = wp.y0 + fmodf (fmodf (sy - wp.y0, ah) + ah, ah);
                }
                else
                {
                    sx = std::min (std::max (sx, wp.x0), wp.x1 - 1.0f);
                    sy = std::min (std::max (sy, wp.y0), wp.y1 - 1.0f);
                }
                sample_bilinear (*pl, src, sx, sy, false, q);
                for (size_t c = 0; c < n; c++)
                {
                    pl->px[((size_t) yi * pl->w + (size_t) x) * n + c] = q[c];
                }
            }
        }
    }

    /* Position in the inscribed ellipse: (u, v) scaled to its radii, r the distance (1 at the
       edge). */
    void to_unit (const Warp& wp, float x, float y, float* u, float* v, float* r)
    {
        *u = (x - wp.cx) / wp.ax;
        *v = (y - wp.cy) / wp.ay;
        *r = sqrtf (*u * *u + *v * *v);
    }

    void radial_scale (const Warp& wp, float u, float v, float k, float* sx, float* sy)
    {
        *sx = wp.cx + u * k * wp.ax;
        *sy = wp.cy + v * k * wp.ay;
    }

    void map_pinch (const Warp& wp, float x, float y, float* sx, float* sy)
    {
        float u, v, r;
        to_unit (wp, x, y, &u, &v, &r);
        *sx = x;
        *sy = y;
        if (r >= 1.0f || r <= 0.0f)
        {
            return;
        }
        /* Pinch samples farther out (the middle is squeezed), a negative amount nearer. */
        float rs = powf (r, 1.0f - wp.a * 0.5f);
        radial_scale (wp, u, v, rs / r, sx, sy);
    }

    void map_spherize (const Warp& wp, float x, float y, float* sx, float* sy)
    {
        float u, v, r;
        *sx = x;
        *sy = y;
        if (wp.mode == 1)
        {
            u = (x - wp.cx) / wp.ax;
            if (fabsf (u) >= 1.0f)
            {
                return;
            }
            float us = u * (1.0f - wp.a * 0.5f * (1.0f - u * u));
            *sx      = wp.cx + us * wp.ax;
            return;
        }
        if (wp.mode == 2)
        {
            v = (y - wp.cy) / wp.ay;
            if (fabsf (v) >= 1.0f)
            {
                return;
            }
            float vs = v * (1.0f - wp.a * 0.5f * (1.0f - v * v));
            *sy      = wp.cy + vs * wp.ay;
            return;
        }
        to_unit (wp, x, y, &u, &v, &r);
        if (r >= 1.0f || r <= 0.0f)
        {
            return;
        }
        /* A positive amount magnifies the middle (up to 2x) like a sphere, a negative one shrinks
           it; the edge stays put. */
        float rs = r * (1.0f - wp.a * 0.5f * (1.0f - r * r));
        radial_scale (wp, u, v, rs / r, sx, sy);
    }

    void map_twirl (const Warp& wp, float x, float y, float* sx, float* sy)
    {
        float u, v, r;
        to_unit (wp, x, y, &u, &v, &r);
        *sx = x;
        *sy = y;
        if (r >= 1.0f)
        {
            return;
        }
        float t = wp.a * (1.0f - r) * (1.0f - r), ct = cosf (t), st = sinf (t);
        *sx = wp.cx + (u * ct - v * st) * wp.ax;
        *sy = wp.cy + (u * st + v * ct) * wp.ay;
    }

    void map_zigzag (const Warp& wp, float x, float y, float* sx, float* sy)
    {
        float u, v, r;
        to_unit (wp, x, y, &u, &v, &r);
        *sx = x;
        *sy = y;
        if (r >= 1.0f)
        {
            return;
        }
        float wave = sinf (r * wp.b * 2.0f * (float) M_PI), fade = 1.0f - r;
        float rs = r, t = 0.0f;
        if (wp.mode != 0)
        {
            rs = r + wp.a * 0.08f * wave * fade / std::max (wp.b * 0.25f, 1.0f);
        }
        if (wp.mode != 1)
        {
            t = wp.a * 0.35f * wave * fade;
        }
        float ct = cosf (t), st = sinf (t), k = r > 0.0f ? rs / r : 1.0f;
        *sx = wp.cx + (u * ct - v * st) * k * wp.ax;
        *sy = wp.cy + (u * st + v * ct) * k * wp.ay;
    }

    void map_polar (const Warp& wp, float x, float y, float* sx, float* sy)
    {
        float w = wp.x1 - wp.x0, h = wp.y1 - wp.y0;
        if (wp.mode == 0)
        {
            /* Rectangular to Polar: the top row gathers in the middle, columns go round clockwise
               from the top. */
            float dx = (x - wp.cx) / wp.ax, dy = (y - wp.cy) / wp.ay;
            float a = atan2f (dx, -dy), r = sqrtf (dx * dx + dy * dy);
            *sx = wp.x0 + (a + (float) M_PI) / (2.0f * (float) M_PI) * w;
            *sy = wp.y0 + r * h;
        }
        else
        {
            float t = (x - wp.x0) / w, r = (y - wp.y0) / h;
            float a = t * 2.0f * (float) M_PI - (float) M_PI;
            *sx     = wp.cx + sinf (a) * r * wp.ax;
            *sy     = wp.cy - cosf (a) * r * wp.ay;
        }
    }

    /* Offsets that depend on the row (row_dx) and the column (col_dy): Ripple, Wave, Shear. */
    void map_offsets (const Warp& wp, float x, float y, float* sx, float* sy)
    {
        int ix = std::min (std::max ((int) x, 0), (int) wp.col_dy.size () - 1),
            iy = std::min (std::max ((int) y, 0), (int) wp.row_dx.size () - 1);
        *sx    = x + (wp.row_dx.empty () ? 0.0f : wp.row_dx[(size_t) iy]);
        *sy    = y + (wp.col_dy.empty () ? 0.0f : wp.col_dy[(size_t) ix]);
    }

    void map_displace (const Warp& wp, float x, float y, float* sx, float* sy)
    {
        size_t i = (size_t) y * wp.w + (size_t) x;
        *sx      = x + wp.map_dx[i];
        *sy      = y + wp.map_dy[i];
    }

    bool run_pinch (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float amount;
        if (!frange (p, "amount", -100.0f, 100.0f, 50.0f, &amount, cmd, err))
        {
            return false;
        }
        Warp wp = make_warp (*pl, cx);
        wp.a    = amount / 100.0f;
        warp_plane (pl, wp, map_pinch);
        return true;
    }

    bool run_spherize (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const MODES[3] = {"normal", "horizontal", "vertical"};
        float amount;
        int mode;
        if (!frange (p, "amount", -100.0f, 100.0f, 100.0f, &amount, cmd, err)
            || !choice (p, "mode", MODES, 3, 0, &mode, cmd, err))
        {
            return false;
        }
        Warp wp = make_warp (*pl, cx);
        wp.a    = amount / 100.0f;
        wp.mode = mode;
        warp_plane (pl, wp, map_spherize);
        return true;
    }

    bool run_twirl (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float angle;
        if (!frange (p, "angle", -999.0f, 999.0f, 50.0f, &angle, cmd, err))
        {
            return false;
        }
        Warp wp = make_warp (*pl, cx);
        wp.a    = angle * (float) M_PI / 180.0f;
        warp_plane (pl, wp, map_twirl);
        return true;
    }

    bool run_zigzag (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const STYLES[3] = {"aroundCenter", "outFromCenter", "pondRipples"};
        float amount, ridges;
        int style;
        if (!frange (p, "amount", -100.0f, 100.0f, 10.0f, &amount, cmd, err)
            || !frange (p, "ridges", 1.0f, 20.0f, 5.0f, &ridges, cmd, err)
            || !choice (p, "style", STYLES, 3, 2, &style, cmd, err))
        {
            return false;
        }
        Warp wp = make_warp (*pl, cx);
        wp.a    = amount / 100.0f;
        wp.b    = ridges;
        wp.mode = style;
        warp_plane (pl, wp, map_zigzag);
        return true;
    }

    bool run_polar (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const MODES[2] = {"rectToPolar", "polarToRect"};
        int mode;
        if (!choice (p, "mode", MODES, 2, 0, &mode, cmd, err))
        {
            return false;
        }
        Warp wp = make_warp (*pl, cx);
        wp.mode = mode;
        warp_plane (pl, wp, map_polar);
        return true;
    }

    bool undefined_param (const Value& p, Warp* wp, const char* cmd, Error* err)
    {
        const char* const UNDEF[2] = {"wrap", "repeat"};
        int u;
        if (!choice (p, "undefined", UNDEF, 2, 1, &u, cmd, err))
        {
            return false;
        }
        wp->wrap = u == 0;
        return true;
    }

    bool run_ripple (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const SIZES[3] = {"small", "medium", "large"};
        float amount;
        int size;
        if (!frange (p, "amount", -999.0f, 999.0f, 100.0f, &amount, cmd, err)
            || !choice (p, "size", SIZES, 3, 1, &size, cmd, err))
        {
            return false;
        }
        Warp wp      = make_warp (*pl, cx);
        float lambda = size == 0 ? 6.0f : size == 1 ? 12.0f : 24.0f, amp = amount / 100.0f * lambda / 3.0f;
        wp.row_dx.resize (pl->h);
        wp.col_dy.resize (pl->w);
        for (size_t y = 0; y < pl->h; y++)
        {
            wp.row_dx[y] = amp * sinf (2.0f * (float) M_PI * ((float) y + (float) cx.src.y0) / lambda);
        }
        for (size_t x = 0; x < pl->w; x++)
        {
            wp.col_dy[x] = amp * sinf (2.0f * (float) M_PI * ((float) x + (float) cx.src.x0) / lambda);
        }
        warp_plane (pl, wp, map_offsets);
        return true;
    }

    float wave_shape (int type, float t)
    {
        float f = t / (2.0f * (float) M_PI);
        f -= floorf (f);
        if (type == 1)
        {
            return f < 0.5f ? f * 4.0f - 1.0f : 3.0f - f * 4.0f;
        }
        if (type == 2)
        {
            return f < 0.5f ? 1.0f : -1.0f;
        }
        return sinf (t);
    }

    /* Wave: the sum of random generators, each a wave with its own length, height and phase. */
    bool run_wave (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const TYPES[3] = {"sine", "triangle", "square"};
        float gens, wl0, wl1, am0, am1, sxs, sys;
        int type;
        if (!frange (p, "generators", 1.0f, 999.0f, 5.0f, &gens, cmd, err)
            || !frange (p, "minWavelength", 1.0f, 998.0f, 10.0f, &wl0, cmd, err)
            || !frange (p, "maxWavelength", 2.0f, 999.0f, 120.0f, &wl1, cmd, err)
            || !frange (p, "minAmplitude", 1.0f, 998.0f, 5.0f, &am0, cmd, err)
            || !frange (p, "maxAmplitude", 1.0f, 999.0f, 35.0f, &am1, cmd, err)
            || !frange (p, "scaleX", 1.0f, 100.0f, 100.0f, &sxs, cmd, err)
            || !frange (p, "scaleY", 1.0f, 100.0f, 100.0f, &sys, cmd, err)
            || !choice (p, "type", TYPES, 3, 0, &type, cmd, err))
        {
            return false;
        }
        Warp wp = make_warp (*pl, cx);
        if (!undefined_param (p, &wp, cmd, err))
        {
            return false;
        }
        uint64_t seed = seed_param (p);
        int n         = (int) gens;
        wp.row_dx.assign (pl->h, 0.0f);
        wp.col_dy.assign (pl->w, 0.0f);
        for (int g = 0; g < n; g++)
        {
            float lx = wl0 + (wl1 - wl0) * rand01 ((uint64_t) g, 1, seed),
                  ly = wl0 + (wl1 - wl0) * rand01 ((uint64_t) g, 2, seed);
            float ax = am0 + (am1 - am0) * rand01 ((uint64_t) g, 3, seed),
                  ay = am0 + (am1 - am0) * rand01 ((uint64_t) g, 4, seed);
            float px = rand01 ((uint64_t) g, 5, seed) * 6.2832f, py = rand01 ((uint64_t) g, 6, seed) * 6.2832f;
            for (size_t y = 0; y < pl->h; y++)
            {
                wp.row_dx[y] +=
                    ax * wave_shape (type, 2.0f * (float) M_PI * (float) y / std::max (lx, 1.0f) + px) / (float) n;
            }
            for (size_t x = 0; x < pl->w; x++)
            {
                wp.col_dy[x] +=
                    ay * wave_shape (type, 2.0f * (float) M_PI * (float) x / std::max (ly, 1.0f) + py) / (float) n;
            }
        }
        for (size_t y = 0; y < pl->h; y++)
        {
            wp.row_dx[y] *= sxs / 100.0f;
        }
        for (size_t x = 0; x < pl->w; x++)
        {
            wp.col_dy[x] *= sys / 100.0f;
        }
        warp_plane (pl, wp, map_offsets);
        return true;
    }

    /* Shear: rows shifted along a bow from the top of the area to its bottom. */
    bool run_shear (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float bend;
        if (!frange (p, "bend", -100.0f, 100.0f, 30.0f, &bend, cmd, err))
        {
            return false;
        }
        Warp wp = make_warp (*pl, cx);
        if (!undefined_param (p, &wp, cmd, err))
        {
            return false;
        }
        wp.row_dx.assign (pl->h, 0.0f);
        float h = wp.y1 - wp.y0, w = wp.x1 - wp.x0;
        for (size_t y = 0; y < pl->h; y++)
        {
            float t      = std::min (std::max (((float) y - wp.y0) / h, 0.0f), 1.0f);
            wp.row_dx[y] = -bend / 100.0f * w * 0.25f * sinf ((float) M_PI * t);
        }
        warp_plane (pl, wp, map_offsets);
        return true;
    }

    /* Displace: the layer's own brightness as the displacement map (mid gray stays put); a scale of
       100% moves up to 128 pixels, as in Photoshop. */
    bool run_displace (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float hs, vs;
        if (!frange (p, "horizontalScale", -999.0f, 999.0f, 10.0f, &hs, cmd, err)
            || !frange (p, "verticalScale", -999.0f, 999.0f, 10.0f, &vs, cmd, err))
        {
            return false;
        }
        Warp wp = make_warp (*pl, cx);
        if (!undefined_param (p, &wp, cmd, err))
        {
            return false;
        }
        Plane straight = *pl;
        premultiply (&straight, false);
        std::vector<float> lum = plane_luma (straight, cx);
        compose::gaussian (lum.data (), pl->w, pl->h, 1.0f);
        wp.map_dx.resize (lum.size ());
        wp.map_dy.resize (lum.size ());
        for (size_t i = 0; i < lum.size (); i++)
        {
            float d      = (lum[i] - 0.5f) * 2.0f * 128.0f;
            wp.map_dx[i] = d * hs / 100.0f;
            wp.map_dy[i] = d * vs / 100.0f;
        }
        warp_plane (pl, wp, map_displace);
        return true;
    }

    const FilterSpec DISTORT_FILTERS[] = {
        {"filter.distort.displace", "Displace…", "Filter/Distort",
            "{\"horizontalScale\":-999..999=10,\"verticalScale\":-999..999=10,\"undefined\":\"wrap|repeat\"} (the "
            "layer's brightness is the map)",
            run_displace, true, Reach::Canvas},
        {"filter.distort.pinch", "Pinch…", "Filter/Distort", "{\"amount\":-100..100=50}", run_pinch, true,
            Reach::Canvas},
        {"filter.distort.polarCoordinates", "Polar Coordinates…", "Filter/Distort",
            "{\"mode\":\"rectToPolar|polarToRect\"}", run_polar, true, Reach::Canvas},
        {"filter.distort.ripple", "Ripple…", "Filter/Distort",
            "{\"amount\":-999..999=100,\"size\":\"small|medium|large\"=\"medium\"}", run_ripple, true, Reach::Canvas},
        {"filter.distort.shear", "Shear…", "Filter/Distort", "{\"bend\":-100..100=30,\"undefined\":\"wrap|repeat\"}",
            run_shear, true, Reach::Canvas},
        {"filter.distort.spherize", "Spherize…", "Filter/Distort",
            "{\"amount\":-100..100=100,\"mode\":\"normal|horizontal|vertical\"}", run_spherize, true, Reach::Canvas},
        {"filter.distort.twirl", "Twirl…", "Filter/Distort", "{\"angle\":-999..999=50}", run_twirl, true,
            Reach::Canvas},
        {"filter.distort.wave", "Wave…", "Filter/Distort",
            "{\"generators\":1..999=5,\"minWavelength\":1..998=10,\"maxWavelength\":2..999=120,\"minAmplitude\":1..998="
            "5,\"maxAmplitude\":1..999=35,\"scaleX\":1..100=100,"
            "\"scaleY\":1..100=100,\"type\":\"sine|triangle|square\",\"undefined\":\"wrap|repeat\",\"seed\":int}",
            run_wave, true, Reach::Canvas},
        {"filter.distort.zigZag", "ZigZag…", "Filter/Distort",
            "{\"amount\":-100..100=10,\"ridges\":1..20=5,\"style\":\"aroundCenter|outFromCenter|pondRipples\"="
            "\"pondRipples\"}",
            run_zigzag, true, Reach::Canvas},
    };

    void distort_filters (std::vector<FilterSpec>* out)
    {
        out->insert (
            out->end (), DISTORT_FILTERS, DISTORT_FILTERS + sizeof DISTORT_FILTERS / sizeof DISTORT_FILTERS[0]);
    }

} // namespace engine
