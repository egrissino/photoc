#include "paint/retouch.h"

#include <math.h>
#include <string.h>

namespace paint
{

    /* The dab machinery of the retouching tools. Two stroke shapes cover them all:

       - Accumulate, then composite (stroke_coverage + apply_coverage): the dabs build up one
         coverage map, and a paint image computed once from the pre-stroke state (a clone source, a
         healed patch, a history state) is composited through it. Pixels painted earlier in the same
         stroke are never re-sampled.
       - Sequential dabs (apply_dab_stroke): each dab edits a working copy in place (dab n sees dab
         n - 1, as Dodge, Blur and Smudge need), and the working copy is mixed back at the stroke's
         opacity.

       Pixels are the surface's native channels as normalised floats (any depth and colour model). */

    using geom::Rect;

    Region Region::make (Rect rect, size_t ch)
    {
        Region r;
        r.rect = rect;
        r.ch   = ch;
        r.data.assign ((size_t) rect.width () * rect.height () * ch, 0.0f);
        return r;
    }

    Region Region::read (const raster::Surface& s, Rect rect)
    {
        Region r;
        r.rect = rect;
        r.ch   = s.channels ();
        if (!s.read_region (rect, &r.data))
        {
            r.data.assign ((size_t) rect.width () * rect.height () * r.ch, 0.0f);
        }
        return r;
    }

    size_t Region::width () const
    {
        return rect.width ();
    }

    size_t Region::height () const
    {
        return rect.height ();
    }

    size_t Region::index (int32_t x, int32_t y) const
    {
        return (size_t) (y - rect.y0) * width () + (size_t) (x - rect.x0);
    }

    const float* Region::px (int32_t x, int32_t y) const
    {
        return &data[index (x, y) * ch];
    }

    float* Region::px_mut (int32_t x, int32_t y)
    {
        return &data[index (x, y) * ch];
    }

    /* A copy of the sub-rectangle r (clipped to this region). */
    Region Region::crop (Rect r) const
    {
        r          = r.intersect (rect);
        Region out = make (r, ch);
        size_t w   = r.width ();
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            memcpy (&out.data[(size_t) (y - r.y0) * w * ch], px (r.x0, y), w * ch * sizeof (float));
        }
        return out;
    }

    void Region::write (raster::Surface* s) const
    {
        s->write_region (rect, data.data (), data.size ());
    }

    float Footprint::at (int32_t x, int32_t y) const
    {
        return cov[(size_t) (y - rect.y0) * rect.width () + (size_t) (x - rect.x0)];
    }

    int alpha_index (color::PixelFormat fmt)
    {
        return fmt.alpha ? (int) fmt.channels () - 1 : -1;
    }

    /* The accumulated coverage of a whole stroke (dabs build up with flow, capped at the opacity
       ceiling) over its bounds. */
    void stroke_coverage (
        const BrushSettings& brush, const std::vector<StrokePoint>& points, Rect* bounds, std::vector<float>* cov)
    {
        StrokeRenderer r (brush);
        r.push (points.data (), points.size ());
        r.finish ();
        *bounds = r.bounds ();
        r.coverage_in (*bounds, cov);
    }

    /* `paint` composited through a coverage map onto the target, weighted by coverage x opacity x
       selection. Blend modes other than Normal composite in straight RGBA. */
    Rect apply_coverage (raster::Surface* target, Rect rect, const std::vector<float>& cov, float opacity,
        const raster::Surface* selection, bool lock, const Region& paint, color::BlendMode blend,
        const color::CmykSpace* cmyk)
    {
        if (rect.is_empty ())
        {
            return Rect ();
        }
        color::PixelFormat fmt = target->format ();
        size_t n               = fmt.channels ();
        int a                  = alpha_index (fmt);
        std::vector<float> region;
        target->read_region (rect, &region);
        size_t w = rect.width ();
        for (size_t i = 0; i < cov.size (); i++)
        {
            int32_t x = rect.x0 + (int32_t) (i % w), y = rect.y0 + (int32_t) (i / w);
            float sel = selection != NULL ? selection->sample_channel (x, y, 0) : 1.0f;
            float k   = fminf (cov[i] * opacity * sel, 1.0f);
            if (k <= 0.0f)
            {
                continue;
            }
            float* p = &region[i * n];
            if (lock && a >= 0 && p[a] <= 0.0f)
            {
                continue;
            }
            const float* src = paint.px (x, y);
            if (blend == color::BlendMode::Normal)
            {
                over_native (fmt, p, src, k, lock);
            }
            else
            {
                float d[4], s[4], o[4];
                raster::to_rgba (fmt, p, cmyk, d);
                raster::to_rgba (fmt, src, cmyk, s);
                color::composite (blend, d, s, k, o);
                if (lock)
                {
                    o[3] = d[3];
                }
                raster::from_rgba (fmt, o, cmyk, p);
            }
        }
        target->write_region (rect, region.data (), region.size ());
        return rect;
    }

    /* A sequential-dab stroke: `effect` edits a working copy dab by dab (the copy reaches `halo`
       pixels past the stroke so neighbourhood effects can read around it), then the stroke bounds
       are mixed back at the stroke's opacity. Returns the damaged rectangle. */
    Rect apply_dab_stroke (raster::Surface* target, const BrushSettings& brush, const std::vector<StrokePoint>& points,
        const raster::Surface* selection, bool lock, int32_t halo, DabEffectFn effect, void* user)
    {
        std::vector<Dab> dabs;
        DabGenerator gen (brush);
        gen.push (points.data (), points.size (), &dabs);
        gen.finish (&dabs);
        if (dabs.empty ())
        {
            return Rect ();
        }
        StrokeRenderer ctx (brush);
        std::vector<Footprint> fps (dabs.size ());
        Rect bounds;
        for (size_t i = 0; i < dabs.size (); i++)
        {
            fps[i].dab   = dabs[i];
            fps[i].index = i;
            ctx.footprint (dabs[i], &fps[i].rect, &fps[i].cov);
            bounds = bounds.unite (fps[i].rect);
        }
        Region orig = Region::read (*target, bounds.inflate (halo));
        Region work = orig;
        for (size_t i = 0; i < fps.size (); i++)
        {
            effect (&work, fps[i], user);
        }
        mix_back (target, orig, work, bounds, brush.opacity, selection, lock);
        return bounds;
    }

    /* dst <- lerp(dst, src, k) premultiplied (straight in, straight out; a = alpha index). Where the
       result is fully transparent the colour mixes straight, so it stays defined. */
    void mix_premultiplied (float* dst, const float* src, size_t n, size_t a, float k)
    {
        float da = dst[a], sa = src[a];
        float oa = da + (sa - da) * k;
        for (size_t c = 0; c < n; c++)
        {
            if (c == a)
            {
                continue;
            }
            dst[c] = oa > 1e-6f ? (dst[c] * da + (src[c] * sa - dst[c] * da) * k) / oa : dst[c] + (src[c] - dst[c]) * k;
        }
        dst[a] = oa;
    }

    /* The working pixels mixed back over the original at opacity x selection inside rect, written
       to the target. */
    void mix_back (raster::Surface* target, const Region& orig, const Region& work, Rect rect, float opacity,
        const raster::Surface* selection, bool lock)
    {
        int a      = alpha_index (target->format ());
        Region out = orig.crop (rect);
        size_t n   = out.ch;
        opacity    = opacity < 0.0f ? 0.0f : opacity > 1.0f ? 1.0f : opacity;
        for (int32_t y = rect.y0; y < rect.y1; y++)
        {
            for (int32_t x = rect.x0; x < rect.x1; x++)
            {
                float k = opacity * (selection != NULL ? selection->sample_channel (x, y, 0) : 1.0f);
                if (k <= 0.0f)
                {
                    continue;
                }
                const float* wv = work.px (x, y);
                float* o        = out.px_mut (x, y);
                if (a >= 0 && !lock)
                {
                    mix_premultiplied (o, wv, n, (size_t) a, k);
                }
                else
                {
                    for (size_t c = 0; c < n; c++)
                    {
                        if (lock && (int) c == a)
                        {
                            continue;
                        }
                        o[c] += (wv[c] - o[c]) * k;
                    }
                }
            }
        }
        out.write (target);
    }

    void premultiply_px (float* px, size_t n, int alpha)
    {
        if (alpha < 0)
        {
            return;
        }
        for (size_t c = 0; c < n; c++)
        {
            if ((int) c != alpha)
            {
                px[c] *= px[alpha];
            }
        }
    }

    /* Smudge: a colour buffer carried from dab to dab (centred on the dab), laid down at `strength`
       and then re-picked up from the pixels under the dab. With alpha the carried colour is
       premultiplied, so smudging into transparency drags no colour in. */
    Smudge Smudge::make (float strength, const float* finger, size_t n, float max_brush_size, int alpha)
    {
        Smudge s;
        s.strength   = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
        s.has_finger = finger != NULL;
        if (finger != NULL)
        {
            s.finger.assign (finger, finger + n);
        }
        s.radius    = (int32_t) ceilf (max_brush_size / 2.0f) + 2;
        s.alpha     = alpha;
        s.has_carry = false;
        return s;
    }

    void Smudge::dab (Region* work, const Footprint& fp)
    {
        int32_t r = radius;
        size_t n = work->ch, side = (size_t) (2 * r + 1);
        int32_t cx = (int32_t) floor (fp.dab.cx), cy = (int32_t) floor (fp.dab.cy);
        Rect wr = work->rect;
        int a   = alpha >= 0 && (size_t) alpha < n ? alpha : -1;
        if (!has_carry)
        {
            has_carry = true;
            carry.assign (side * side * n, 0.0f);
            std::vector<float> v (n);
            for (size_t qy = 0; qy < side; qy++)
            {
                for (size_t qx = 0; qx < side; qx++)
                {
                    if (has_finger)
                    {
                        for (size_t c = 0; c < n; c++)
                        {
                            v[c] = c < finger.size () ? finger[c] : 1.0f;
                        }
                    }
                    else
                    {
                        int32_t x = cx + (int32_t) qx - r, y = cy + (int32_t) qy - r;
                        x = x < wr.x0 ? wr.x0 : x >= wr.x1 ? wr.x1 - 1 : x;
                        y = y < wr.y0 ? wr.y0 : y >= wr.y1 ? wr.y1 - 1 : y;
                        memcpy (v.data (), work->px (x, y), n * sizeof (float));
                    }
                    premultiply_px (v.data (), n, a);
                    memcpy (&carry[(qy * side + qx) * n], v.data (), n * sizeof (float));
                }
            }
        }
        /* Lay down the carried colour. */
        Rect fr = fp.rect.intersect (wr);
        std::vector<float> src (n);
        for (int32_t y = fr.y0; y < fr.y1; y++)
        {
            for (int32_t x = fr.x0; x < fr.x1; x++)
            {
                float k    = fp.at (x, y) * strength;
                int32_t qx = x - cx + r, qy = y - cy + r;
                if (k <= 0.0f || qx < 0 || qy < 0 || qx >= (int32_t) side || qy >= (int32_t) side)
                {
                    continue;
                }
                const float* q = &carry[((size_t) qy * side + (size_t) qx) * n];
                float* p       = work->px_mut (x, y);
                if (a >= 0)
                {
                    float ca = q[a];
                    for (size_t c = 0; c < n; c++)
                    {
                        src[c] = (int) c != a && ca > 1e-6f ? q[c] / ca : q[c];
                    }
                    mix_premultiplied (p, src.data (), n, (size_t) a, k);
                }
                else
                {
                    for (size_t c = 0; c < n; c++)
                    {
                        p[c] += (q[c] - p[c]) * k;
                    }
                }
            }
        }
        /* Pick up: the carried colour keeps `strength` of itself. */
        for (size_t qy = 0; qy < side; qy++)
        {
            for (size_t qx = 0; qx < side; qx++)
            {
                int32_t x = cx + (int32_t) qx - r, y = cy + (int32_t) qy - r;
                if (x < wr.x0 || y < wr.y0 || x >= wr.x1 || y >= wr.y1)
                {
                    continue;
                }
                float* q       = &carry[(qy * side + qx) * n];
                const float* p = work->px (x, y);
                float pa       = a >= 0 ? p[a] : 1.0f;
                for (size_t c = 0; c < n; c++)
                {
                    float v = a >= 0 && (int) c != a ? p[c] * pa : p[c];
                    q[c]    = q[c] * strength + v * (1.0f - strength);
                }
            }
        }
    }

} // namespace paint
