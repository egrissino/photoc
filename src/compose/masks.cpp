#include "compose/masks.h"

#include <math.h>
#include <string.h>
#include <vector>

namespace compose
{

    /* Layer mask feather (Properties > Feather): the mask blurred by a Gaussian of sigma = feather,
       up to Photoshop's 1000 px. */

    using geom::Rect;

    const float MAX_FEATHER_SIGMA = 1000.0f;

    float feather_sigma (float px)
    {
        if (!isfinite (px))
        {
            return 0.0f;
        }
        return px < 0.0f ? 0.0f : px > MAX_FEATHER_SIGMA ? MAX_FEATHER_SIGMA : px;
    }

    bool has_feather (const doc::LayerMask& m)
    {
        return m.enabled && feather_sigma (m.feather) > 0.0f;
    }

    /* One box pass of radius r over a line; edges clamp. `scratch` holds the input. */
    void blur_line (float* line, float* scratch, size_t len, size_t r)
    {
        if (len == 0)
        {
            return;
        }
        memcpy (scratch, line, len * sizeof (float));
        long n      = (long) len;
        long radius = (long) r;
        float acc   = 0.0f;
        for (long i = -radius; i <= radius; i++)
        {
            acc += scratch[i < 0 ? 0 : i >= n ? n - 1 : i];
        }
        float k = (float) (2 * r + 1);
        for (long i = 0; i < n; i++)
        {
            line[i]  = acc / k;
            long add = i + radius + 1, sub = i - radius;
            acc += scratch[add >= n ? n - 1 : add] - scratch[sub < 0 ? 0 : sub];
        }
    }

    /* Three box passes over each row of a w x h plane, rows in parallel. */
    void blur_rows (float* v, size_t w, size_t h, size_t r)
    {
#pragma omp parallel
        {
            std::vector<float> scratch (w);
#pragma omp for schedule(static)
            for (ptrdiff_t y = 0; y < (ptrdiff_t) h; y++)
            {
                for (int pass = 0; pass < 3; pass++)
                {
                    blur_line (v + (size_t) y * w, scratch.data (), w, r);
                }
            }
        }
    }

    void transpose (const float* v, float* out, size_t w, size_t h)
    {
        const size_t BLOCK = 32;
        for (size_t by = 0; by < h; by += BLOCK)
        {
            for (size_t bx = 0; bx < w; bx += BLOCK)
            {
                for (size_t y = by; y < by + BLOCK && y < h; y++)
                {
                    for (size_t x = bx; x < bx + BLOCK && x < w; x++)
                    {
                        out[x * h + y] = v[y * w + x];
                    }
                }
            }
        }
    }

    /* Approximate Gaussian blur: three box passes per axis whose variance matches sigma² (Wells
       1986). Edges clamp, which is exact here: the planes extend into constant mask. */
    void gaussian (float* v, size_t w, size_t h, float sigma)
    {
        if (sigma <= 0.0f || w == 0 || h == 0)
        {
            return;
        }
        float ideal  = sqrtf (12.0f * sigma * sigma / 3.0f + 1.0f);
        size_t width = (size_t) floorf (ideal) | 1;
        size_t r     = (width - 1) / 2;
        if (r == 0)
        {
            return;
        }
        std::vector<float> t (w * h);
        blur_rows (v, w, h, r);
        transpose (v, t.data (), w, h);
        blur_rows (t.data (), h, w, r);
        transpose (t.data (), v, h, w);
    }

    /* The mask's values (with density) blurred by its feather, as a single-channel float surface
       over the canvas. Outside the blurred area it holds the mask's constant value. */
    raster::Surface feathered_mask (const doc::LayerMask& m, Rect canvas)
    {
        float d[raster::MAX_CHANNELS];
        m.surface.default_pixel (d);
        float outside          = 1.0f - m.density * (1.0f - d[0]);
        color::PixelFormat fmt = {color::ColorMode::Grayscale, color::SampleType::F32, false};
        raster::Surface out    = raster::Surface::with_default (fmt, &outside);
        float sigma            = feather_sigma (m.feather);
        Rect area              = m.surface.content_bounds ();
        if (area.is_empty ())
        {
            return out;
        }
        int32_t grow = (int32_t) ceilf (sigma * 3.0f) + 2;
        area         = area.inflate (grow).intersect (canvas);
        if (area.is_empty ())
        {
            return out;
        }
        std::vector<float> v;
        if (!m.values (area, &v))
        {
            return out;
        }
        gaussian (v.data (), area.width (), area.height (), sigma);
        out.write_region (area, v.data (), v.size ());
        return out;
    }

    /* The same tiles (and format and default): the surfaces are one version of the pixels. */
    bool same_tiles (const raster::Surface& a, const raster::Surface& b)
    {
        if (a.format () != b.format ()
            || memcmp (a.default_bytes (), b.default_bytes (), a.format ().bytes_per_pixel ()) != 0)
        {
            return false;
        }
        const raster::TileMap &ta = a.tiles (), &tb = b.tiles ();
        if (ta.size () != tb.size ())
        {
            return false;
        }
        raster::TileMap::const_iterator i = ta.begin (), k = tb.begin ();
        for (; i != ta.end (); ++i, ++k)
        {
            if (!(i->first == k->first) || i->second != k->second)
            {
                return false;
            }
        }
        return true;
    }

    const size_t MASK_CACHE_ENTRIES = 16;

    MaskCache::MaskCache () : clock_ (0) {}

    /* feathered_mask(), reused while the mask's pixels, density, feather and the canvas are
       unchanged (the blur is the slow part of drawing a feathered layer). */
    raster::Surface MaskCache::feathered (const doc::LayerMask& m, Rect canvas)
    {
        clock_++;
        for (size_t i = 0; i < entries_.size (); i++)
        {
            Entry& e = entries_[i];
            if (e.density == m.density && e.feather == m.feather && e.canvas == canvas
                && same_tiles (e.source, m.surface))
            {
                e.used = clock_;
                return e.result;
            }
        }
        Entry e;
        e.source  = m.surface;
        e.density = m.density;
        e.feather = m.feather;
        e.canvas  = canvas;
        e.result  = feathered_mask (m, canvas);
        e.used    = clock_;
        if (entries_.size () < MASK_CACHE_ENTRIES)
        {
            entries_.push_back (e);
        }
        else
        {
            size_t oldest = 0;
            for (size_t i = 1; i < entries_.size (); i++)
            {
                if (entries_[i].used < entries_[oldest].used)
                {
                    oldest = i;
                }
            }
            entries_[oldest] = e;
        }
        return e.result;
    }

} // namespace compose
