#include "algo/paint_fill.h"

#include <math.h>
#include <string.h>

namespace algo
{

    /* The Gradient tool and the Paint Bucket: a colour composited over an area of a surface with a
       per-pixel coverage, tile by tile in parallel. Tiles with no coverage are left untouched (their
       copy-on-write storage stays shared). */

    using geom::Rect;

    bool gradient_shape_from_str (const char* s, GradientShape* out)
    {
        const char* names[5] = {"linear", "radial", "angle", "reflected", "diamond"};
        for (int i = 0; i < 5; i++)
        {
            if (strcmp (s, names[i]) == 0)
            {
                *out = (GradientShape) i;
                return true;
            }
        }
        return false;
    }

    /* The gradient parameter at (x, y) for a drag from `from` to `to`. */
    float gradient_t (GradientShape shape, const float from[2], const float to[2], float x, float y)
    {
        float dx = to[0] - from[0], dy = to[1] - from[1];
        float len = fmaxf (sqrtf (dx * dx + dy * dy), 1e-6f);
        float ux = dx / len, uy = dy / len;
        float px = x - from[0], py = y - from[1];
        float along = px * ux + py * uy, across = -px * uy + py * ux;
        float t;
        switch (shape)
        {
        case GradientShape::Radial:
            t = sqrtf (px * px + py * py) / len;
            break;
        case GradientShape::Reflected:
            t = fabsf (along) / len;
            break;
        case GradientShape::Diamond:
            t = (fabsf (along) + fabsf (across)) / len;
            break;
        case GradientShape::Angle:
            {
                float a = atan2f (across, along) / (2.0f * (float) M_PI);
                t       = a - floorf (a);
                break;
            }
        default:
            t = along / len;
            break;
        }
        return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    }

    /* RGBA stops (sorted by position) sampled at t. */
    void sample_stops (const std::vector<GradientStop>& stops, float t, float out[4])
    {
        if (stops.empty ())
        {
            memset (out, 0, 4 * sizeof (float));
            return;
        }
        if (stops.size () == 1 || t <= stops[0].t)
        {
            memcpy (out, stops[0].color, 4 * sizeof (float));
            return;
        }
        for (size_t i = 0; i + 1 < stops.size (); i++)
        {
            const GradientStop &a = stops[i], &b = stops[i + 1];
            if (t <= b.t)
            {
                float k = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0.0f;
                for (int c = 0; c < 4; c++)
                {
                    out[c] = a.color[c] + (b.color[c] - a.color[c]) * k;
                }
                return;
            }
        }
        memcpy (out, stops.back ().color, 4 * sizeof (float));
    }

    struct FillSource
    {
        const Gradient* gradient;
        const float* color;
        const Region* region;
        float opacity;
        const raster::Surface* selection;
    };

    float fill_coverage (const FillSource& f, int32_t x, int32_t y)
    {
        float k = f.opacity;
        if (f.selection != NULL)
        {
            k *= f.selection->sample_channel (x, y, 0);
        }
        if (f.region != NULL)
        {
            const Region& r = *f.region;
            if (x < r.bbox.x0 || y < r.bbox.y0 || x >= r.bbox.x1 || y >= r.bbox.y1)
            {
                return 0.0f;
            }
            k *= r.mask[(size_t) (y - r.bbox.y0) * r.bbox.width () + (size_t) (x - r.bbox.x0)] / 255.0f;
        }
        return k;
    }

    void fill_color (const FillSource& f, int32_t x, int32_t y, float out[4])
    {
        if (f.gradient == NULL)
        {
            memcpy (out, f.color, 4 * sizeof (float));
            return;
        }
        const Gradient& g = *f.gradient;
        float t           = gradient_t (g.shape, g.from, g.to, (float) x + 0.5f, (float) y + 0.5f);
        sample_stops (g.stops, g.reverse ? 1.0f - t : t, out);
        if (g.dither)
        {
            /* One quantisation step of monochromatic noise breaks 8-bit banding without speckle. */
            float n = (color::dither_noise (x, y) - 0.5f) / 255.0f;
            for (int c = 0; c < 3; c++)
            {
                float v = out[c] + n;
                out[c]  = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            }
        }
    }

    void composite_area (raster::Surface* s, Rect area, color::BlendMode mode, const FillSource& f, bool lock,
        const color::CmykSpace* cmyk)
    {
        color::PixelFormat fmt              = s->format ();
        size_t n                            = fmt.channels ();
        std::vector<geom::TileCoord> coords = area.tiles ();
        std::vector<std::vector<float>> results (coords.size ());
        std::vector<Rect> rects (coords.size ());
#pragma omp parallel for schedule(dynamic)
        for (ptrdiff_t ti = 0; ti < (ptrdiff_t) coords.size (); ti++)
        {
            Rect r             = coords[(size_t) ti].rect ().intersect (area);
            rects[(size_t) ti] = r;
            if (r.is_empty ())
            {
                continue;
            }
            size_t w                   = r.width ();
            std::vector<float>& region = results[(size_t) ti];
            bool any                   = false;
            for (int32_t y = r.y0; y < r.y1; y++)
            {
                for (int32_t x = r.x0; x < r.x1; x++)
                {
                    float k = fill_coverage (f, x, y);
                    if (k <= 0.0f)
                    {
                        continue;
                    }
                    if (!any)
                    {
                        s->read_region (r, &region);
                        any = true;
                    }
                    float* px = &region[((size_t) (y - r.y0) * w + (size_t) (x - r.x0)) * n];
                    float d[4], src[4], o[4];
                    raster::to_rgba (fmt, px, cmyk, d);
                    fill_color (f, x, y, src);
                    color::composite (mode, d, src, k, o);
                    if (lock)
                    {
                        o[3] = d[3];
                    }
                    raster::from_rgba (fmt, o, cmyk, px);
                }
            }
        }
        for (size_t i = 0; i < coords.size (); i++)
        {
            if (!results[i].empty ())
            {
                s->write_region (rects[i], results[i].data (), results[i].size ());
            }
        }
    }

    /* The Gradient tool over `area` (the selection's bounds or the canvas). */
    void paint_gradient (raster::Surface* s, Rect area, const Gradient& g, float opacity, color::BlendMode mode,
        const raster::Surface* selection, bool lock, const color::CmykSpace* cmyk)
    {
        FillSource f = {&g, NULL, NULL, opacity, selection};
        composite_area (s, area, mode, f, lock, cmyk);
    }

    /* The Paint Bucket: `color` over a region found before the edit (on the layer, or on the
       composite with Sample All Layers), through the selection. */
    void bucket_fill_region (raster::Surface* s, const Region& region, const float color[4], float opacity,
        const raster::Surface* selection, bool lock, const color::CmykSpace* cmyk)
    {
        FillSource f = {NULL, color, &region, opacity, selection};
        composite_area (s, region.bbox, color::BlendMode::Normal, f, lock, cmyk);
    }

} // namespace algo
