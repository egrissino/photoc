#include "compose/effects.h"

#include "compose/psblend.h"
#include <algorithm>
#include <math.h>
#include <string.h>

namespace compose
{

    /* Layer styles, rendered as the reference renderer does (it was fitted to Photoshop):
       bottom to top, drop shadows, outer glows and an outer bevel blend straight into the backdrop
       with their own modes; then the layer itself (its content at fill opacity) with gradient and
       colour overlays, satin, inner glows, inner shadows, strokes and an inner bevel on top, blended
       with the layer's mode; emboss styles shade the result. Layer opacity applies to the whole
       stack, fill opacity only to the layer's own pixels. Interior effects are painted relative to
       the layer's shape and then take its alpha. The outside parts of strokes blend onto the
       backdrop with their own modes, an upper stroke covering the lower ones.

       Every map depends only on the shape within the effects' reach, so a render rectangle is
       computed over the rectangle grown by twice the margin and cropped (the same pixels whatever
       the tiling). Simplifications against the reference: contours are linear, distances are exact
       Euclidean (not Photoshop's chamfer), no noise or jitter, colour strokes and glows only,
       bevels are Smooth only. */

    using color::BlendMode;
    using geom::Rect;

    /* A pixel counts as inside the shape above this alpha. */
    const float INSIDE_EPS = 0.5f / 255.0f;
    /* Effects reach at most this far (px) from the shape. */
    const float MAX_REACH = 512.0f;

    struct Map
    {
        size_t w;
        size_t h;
        std::vector<float> v;
    };

    Map map_new (size_t w, size_t h, float fill)
    {
        Map m;
        m.w = w;
        m.h = h;
        m.v.assign (w * h, fill);
        return m;
    }

    float map_get (const Map& m, long x, long y, float outside)
    {
        if (x < 0 || y < 0 || x >= (long) m.w || y >= (long) m.h)
        {
            return outside;
        }
        return m.v[(size_t) y * m.w + (size_t) x];
    }

    bool has_effects (const doc::Layer& layer)
    {
        if (!layer.style.enabled)
        {
            return false;
        }
        for (size_t i = 0; i < layer.style.items.size (); i++)
        {
            if (layer.style.items[i].enabled)
            {
                return true;
            }
        }
        return false;
    }

    float reach_of (const doc::Effect& e)
    {
        switch (e.kind)
        {
        case doc::FxKind::DropShadow:
        case doc::FxKind::InnerShadow:
            return e.distance + e.size;
        case doc::FxKind::OuterGlow:
        case doc::FxKind::InnerGlow:
        case doc::FxKind::Stroke:
            return e.size;
        case doc::FxKind::Satin:
            return fmaxf (e.distance, 1.0f) + e.size;
        case doc::FxKind::BevelEmboss:
            return e.size + e.soften;
        default:
            return 0.0f;
        }
    }

    /* Pixels the effects can reach beyond the layer's shape. */
    int32_t effects_margin (const doc::Layer& layer)
    {
        float m = 0.0f;
        for (size_t i = 0; i < layer.style.items.size (); i++)
        {
            const doc::Effect& e = layer.style.items[i];
            if (!e.enabled)
            {
                continue;
            }
            float r = reach_of (e);
            if (isfinite (r))
            {
                m = fmaxf (m, r);
            }
        }
        return (int32_t) ceilf (fminf (m, MAX_REACH)) + 2;
    }

    /* Squared distance transform along one line (Felzenszwalb and Huttenlocher); FAR marks no
       seed. */
    const float FAR = 1e20f;

    void dt_line (const float* f, size_t n, float* out, std::vector<size_t>* v, std::vector<float>* z)
    {
        size_t k = 0;
        (*v)[0]  = 0;
        (*z)[0]  = -FAR;
        (*z)[1]  = FAR;
        for (size_t q = 1; q < n; q++)
        {
            float s;
            for (;;)
            {
                size_t p = (*v)[k];
                s        = ((f[q] + (float) q * (float) q) - (f[p] + (float) p * (float) p))
                  / (2.0f * (float) q - 2.0f * (float) p);
                if (s <= (*z)[k] && k > 0)
                {
                    k--;
                    continue;
                }
                break;
            }
            if (s <= (*z)[k])
            {
                (*v)[0] = q;
                (*z)[0] = -FAR;
                (*z)[1] = FAR;
                continue;
            }
            k++;
            (*v)[k]     = q;
            (*z)[k]     = s;
            (*z)[k + 1] = FAR;
        }
        k = 0;
        for (size_t q = 0; q < n; q++)
        {
            while ((*z)[k + 1] < (float) q)
            {
                k++;
            }
            float d = (float) q - (float) (*v)[k];
            out[q]  = d * d + f[(*v)[k]];
        }
    }

    /* Euclidean distance from every pixel to the nearest `inside` one (0 on them). */
    std::vector<float> edt (const std::vector<uint8_t>& inside, size_t w, size_t h)
    {
        std::vector<float> g (w * h);
        for (size_t i = 0; i < g.size (); i++)
        {
            g[i] = inside[i] ? 0.0f : FAR;
        }
        size_t n = w > h ? w : h;
        std::vector<float> col (n), out (n);
        std::vector<size_t> v (n + 1);
        std::vector<float> z (n + 2);
        for (size_t x = 0; x < w; x++)
        {
            for (size_t y = 0; y < h; y++)
            {
                col[y] = g[y * w + x];
            }
            dt_line (col.data (), h, out.data (), &v, &z);
            for (size_t y = 0; y < h; y++)
            {
                g[y * w + x] = out[y];
            }
        }
        for (size_t y = 0; y < h; y++)
        {
            dt_line (&g[y * w], w, out.data (), &v, &z);
            for (size_t x = 0; x < w; x++)
            {
                g[y * w + x] = out[x] >= FAR * 0.5f ? FAR : sqrtf (out[x]);
            }
        }
        return g;
    }

    /* Distance outside the shape, measured from its edge: 0.5 for the first pixel beyond it. */
    std::vector<float> dist_outside (const Map& s)
    {
        std::vector<uint8_t> in (s.v.size ());
        for (size_t i = 0; i < in.size (); i++)
        {
            in[i] = s.v[i] >= 0.5f;
        }
        std::vector<float> d = edt (in, s.w, s.h);
        for (size_t i = 0; i < d.size (); i++)
        {
            d[i] = in[i] ? 0.0f : d[i] - 0.5f;
        }
        return d;
    }

    /* Distance inside the shape from its edge: 0.5 for the edge pixel. */
    std::vector<float> dist_inside (const Map& s)
    {
        std::vector<uint8_t> out (s.v.size ());
        for (size_t i = 0; i < out.size (); i++)
        {
            out[i] = s.v[i] < 0.5f;
        }
        std::vector<float> d = edt (out, s.w, s.h);
        for (size_t i = 0; i < d.size (); i++)
        {
            d[i] = out[i] ? 0.0f : d[i] - 0.5f;
        }
        return d;
    }

    /* One box pass of (fractional) width bw along rows or columns, zero outside: the full taps
       within the box plus the two partly covered end taps, as a running sum. */
    void box_pass (Map* m, float bw, bool rows)
    {
        bw         = fminf (fmaxf (isnan (bw) ? 1.0f : bw, 1.0f), MAX_REACH);
        float half = bw / 2.0f;
        long r     = (long) fmaxf (ceilf (half - 0.5f), 0.0f);
        double f = fmin (fmax ((double) half - ((double) r - 0.5), 0.0), 1.0), norm = 1.0 / bw;
        size_t len = rows ? m->w : m->h, lines = rows ? m->h : m->w;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t li = 0; li < (ptrdiff_t) lines; li++)
        {
            std::vector<double> pre (len + 1, 0.0);
            std::vector<float> line (len);
            for (size_t i = 0; i < len; i++)
            {
                line[i]    = rows ? m->v[(size_t) li * m->w + i] : m->v[i * m->w + (size_t) li];
                pre[i + 1] = pre[i] + line[i];
            }
            for (long i = 0; i < (long) len; i++)
            {
                long a = std::max (i - r + 1, 0L), b = std::min (i + r - 1, (long) len - 1);
                double sum = b >= a ? pre[(size_t) b + 1] - pre[(size_t) a] : 0.0;
                if (r > 0)
                {
                    if (i - r >= 0)
                    {
                        sum += f * line[(size_t) (i - r)];
                    }
                    if (i + r < (long) len)
                    {
                        sum += f * line[(size_t) (i + r)];
                    }
                }
                else
                {
                    sum = line[(size_t) i] * f;
                }
                float val = (float) (sum * norm);
                if (rows)
                {
                    m->v[(size_t) li * m->w + (size_t) i] = val;
                }
                else
                {
                    m->v[(size_t) i * m->w + (size_t) li] = val;
                }
            }
        }
    }

    /* Two box passes of width w each way: a tent of half-width about w, Photoshop's soft falloff. */
    void tent (Map* m, float w)
    {
        if (!isfinite (w) || w <= 1.0f)
        {
            return;
        }
        for (int pass = 0; pass < 2; pass++)
        {
            box_pass (m, w, true);
            box_pass (m, w, false);
        }
    }

    /* A copy moved by whole pixels (dx, dy); pixels from outside read `outside`. */
    Map shifted (const Map& m, long dx, long dy, float outside)
    {
        Map out = map_new (m.w, m.h, 0.0f);
        for (long y = 0; y < (long) m.h; y++)
        {
            for (long x = 0; x < (long) m.w; x++)
            {
                out.v[(size_t) y * m.w + (size_t) x] = map_get (m, x - dx, y - dy, outside);
            }
        }
        return out;
    }

    /* Grown by r whole pixels, keeping the soft edge. */
    Map dilate (const Map& s, float r)
    {
        if (r <= 0.0f)
        {
            return s;
        }
        std::vector<float> d = dist_outside (s);
        Map out              = s;
        for (size_t i = 0; i < out.v.size (); i++)
        {
            if (s.v[i] > 0.0f && s.v[i] < 0.5f)
            {
                out.v[i] = fminf (s.v[i] + r, 1.0f);
            }
            else
            {
                out.v[i] = fmaxf (out.v[i], fminf (fmaxf (r + 0.5f - d[i], 0.0f), 1.0f));
            }
        }
        return out;
    }

    /* Spread or choke of a soft effect: (whole-pixel dilation, blur width). */
    void spread_split (float size, float spread, float* r, float* bw)
    {
        *r  = roundf (size * spread);
        *bw = size * (1.0f - spread);
    }

    /* The offset of an effect lit from `angle` at `distance` (away from the light), whole pixels. */
    void light_offset (float angle, float distance, long* dx, long* dy)
    {
        float a = angle * (float) M_PI / 180.0f;
        *dx     = (long) lroundf (-cosf (a) * distance);
        *dy     = (long) lroundf (sinf (a) * distance);
    }

    Map shadow_map (const Map& shape, const doc::Effect& e, float light, bool inner)
    {
        long dx, dy;
        light_offset (e.global_light ? light : e.angle, e.distance, &dx, &dy);
        Map src = shape;
        if (inner)
        {
            for (size_t i = 0; i < src.v.size (); i++)
            {
                src.v[i] = 1.0f - src.v[i];
            }
        }
        Map m = shifted (src, dx, dy, inner ? 1.0f : 0.0f);
        float r, bw;
        spread_split (e.size, e.spread, &r, &bw);
        m = dilate (m, r);
        tent (&m, bw);
        if (inner)
        {
            for (size_t i = 0; i < m.v.size (); i++)
            {
                m.v[i] *= shape.v[i];
            }
        }
        return m;
    }

    Map glow_map (const Map& shape, const doc::Effect& e, bool inner)
    {
        Map src = shape;
        if (inner)
        {
            for (size_t i = 0; i < src.v.size (); i++)
            {
                src.v[i] = 1.0f - src.v[i];
            }
        }
        Map m;
        if (e.precise)
        {
            /* Coverage falls linearly from 0.5 at the edge to 0 at size + 1 px, solid within the
               spread or choke. */
            std::vector<float> d = inner ? dist_inside (shape) : dist_outside (shape);
            float solid = e.size * e.spread, soft = fmaxf (e.size + 1.0f - solid, 1e-3f);
            m = map_new (shape.w, shape.h, 0.0f);
            for (size_t i = 0; i < m.v.size (); i++)
            {
                bool on = inner ? shape.v[i] >= 0.5f : shape.v[i] < 0.5f;
                if (!on)
                {
                    m.v[i] = 1.0f;
                    continue;
                }
                float dd = d[i] + 0.5f;
                m.v[i]   = dd <= solid ? 1.0f : 0.5f * fminf (fmaxf (1.0f - (dd - solid) / soft, 0.0f), 1.0f);
            }
        }
        else
        {
            float r, bw;
            spread_split (e.size, e.spread, &r, &bw);
            m = dilate (src, r);
            tent (&m, bw);
        }
        /* Range stretches the falloff: v / range, capped at 1. */
        float range = isfinite (e.range) ? fminf (fmaxf (e.range, 0.01f), 1.0f) : 1.0f;
        if (range < 0.999f)
        {
            for (size_t i = 0; i < m.v.size (); i++)
            {
                m.v[i] = fminf (m.v[i] / range, 1.0f);
            }
        }
        if (inner && e.center)
        {
            for (size_t i = 0; i < m.v.size (); i++)
            {
                m.v[i] = 1.0f - m.v[i];
            }
        }
        if (inner)
        {
            for (size_t i = 0; i < m.v.size (); i++)
            {
                m.v[i] *= shape.v[i];
            }
        }
        return m;
    }

    Map satin_map (const Map& shape, const doc::Effect& e)
    {
        long dx, dy;
        light_offset (e.angle, fmaxf (e.distance, 1.0f), &dx, &dy);
        Map a = shifted (shape, dx, dy, 0.0f), b = shifted (shape, -dx, -dy, 0.0f);
        tent (&a, e.size);
        tent (&b, e.size);
        Map m = map_new (shape.w, shape.h, 0.0f);
        for (size_t i = 0; i < m.v.size (); i++)
        {
            float v = fabsf (a.v[i] - b.v[i]);
            if (e.invert)
            {
                v = 1.0f - v;
            }
            m.v[i] = v * shape.v[i];
        }
        return m;
    }

    enum class BevelPaint
    {
        Inner,
        Outer,
        Both,
    };

    BevelPaint bevel_paint (const doc::Effect& e)
    {
        if (e.bevel == doc::BevelStyle::OuterBevel)
        {
            return BevelPaint::Outer;
        }
        if (e.bevel == doc::BevelStyle::Emboss || e.bevel == doc::BevelStyle::PillowEmboss)
        {
            return BevelPaint::Both;
        }
        return BevelPaint::Inner;
    }

    /* Smooth bevel: the height is the shape blurred by a tent of the bevel width (half the size,
       rounded up, for emboss styles), shaded by the light; `region` 0 shades the inside (times the
       shape's alpha), 1 the edge and the outside where the height is raised. */
    void bevel_shade (
        const Map& shape, const Map& hmap, float depth, float light_angle, float altitude, int region, Map* hi, Map* sh)
    {
        float sa = sinf (light_angle * (float) M_PI / 180.0f), ca = cosf (light_angle * (float) M_PI / 180.0f);
        float se = sinf (altitude * (float) M_PI / 180.0f), ce = cosf (altitude * (float) M_PI / 180.0f);
        float lv[3] = {ca * ce, -sa * ce, se};
        *hi         = map_new (shape.w, shape.h, 0.0f);
        *sh         = map_new (shape.w, shape.h, 0.0f);
        for (long y = 0; y < (long) shape.h; y++)
        {
            for (long x = 0; x < (long) shape.w; x++)
            {
                size_t i    = (size_t) y * shape.w + (size_t) x;
                float gx    = (map_get (hmap, x + 1, y, 0.0f) - map_get (hmap, x - 1, y, 0.0f)) * 0.5f * depth;
                float gy    = (map_get (hmap, x, y + 1, 0.0f) - map_get (hmap, x, y - 1, 0.0f)) * 0.5f * depth;
                float len   = sqrtf (gx * gx + gy * gy + 1.0f);
                float shade = (-gx * lv[0] - gy * lv[1] + lv[2]) / len;
                float reg =
                    region == 0 ? shape.v[i] : (shape.v[i] < 1.0f - INSIDE_EPS && hmap.v[i] > 1e-5f ? 1.0f : 0.0f);
                float k = shade - se;
                if (k > 0.0f)
                {
                    hi->v[i] = fminf (fmaxf (k / fmaxf (1.0f - se, 1e-3f), 0.0f), 1.0f) * reg;
                }
                else
                {
                    sh->v[i] = fminf (fmaxf (-k / fmaxf (se, 1e-3f), 0.0f), 1.0f) * reg;
                }
            }
        }
    }

    void bevel_maps (const Map& shape, const doc::Effect& e, float light_angle, float light_alt, Map* hi, Map* sh)
    {
        float size       = fmaxf (e.size, 1.0f);
        BevelPaint paint = bevel_paint (e);
        float width      = paint == BevelPaint::Both ? ceilf (size / 2.0f) : size;
        float depth      = e.depth * width * (e.up ? 1.0f : -1.0f);
        Map hmap         = shape;
        tent (&hmap, width);
        if (e.soften >= 1.0f)
        {
            tent (&hmap, e.soften);
        }
        float angle = e.global_light ? light_angle : e.angle, alt = e.global_light ? light_alt : e.altitude;
        if (paint != BevelPaint::Both)
        {
            bevel_shade (shape, hmap, depth, angle, alt, paint == BevelPaint::Outer ? 1 : 0, hi, sh);
            return;
        }
        /* Emboss: the inside half where the shape is, the outside half (pillow: facing the other
           way) for the rest of each pixel. */
        Map hin, sin_, hout, sout;
        bevel_shade (shape, hmap, depth, angle, alt, 0, &hin, &sin_);
        bevel_shade (
            shape, hmap, e.bevel == doc::BevelStyle::PillowEmboss ? -depth : depth, angle, alt, 1, &hout, &sout);
        *hi = hin;
        *sh = sin_;
        for (size_t i = 0; i < shape.v.size (); i++)
        {
            float s = fminf (shape.v[i], 1.0f), in = shape.v[i] > INSIDE_EPS ? 1.0f : 0.0f;
            hi->v[i] = hin.v[i] * in + hout.v[i] * (1.0f - s);
            sh->v[i] = sin_.v[i] * in + sout.v[i] * (1.0f - s);
        }
    }

    /* Paints colour c through coverage m (times opacity) onto the w x h pixels of dst. */
    void paint_color (
        std::vector<float>* dst, const Map& m, const float c[3], BlendMode blend, float opacity, BlendSpace space)
    {
        for (size_t i = 0; i < m.v.size (); i++)
        {
            float a = m.v[i] * opacity;
            if (a <= 0.0f)
            {
                continue;
            }
            float* p     = &(*dst)[i * 4];
            float src[4] = {c[0], c[1], c[2], fminf (a, 1.0f)};
            ps_composite (space, blend, p, src, 1.0f, p);
        }
    }

    /* Gradient parameter t (0..1) at pixel centre (x, y) in `bounds`, for the overlay's style,
       angle (degrees, counter-clockwise), scale and Reverse. */
    float gradient_t (const doc::Effect& e, Rect bounds, float x, float y)
    {
        float w = (float) std::max (bounds.width (), 1u), h = (float) std::max (bounds.height (), 1u);
        float cx = (float) bounds.x0 + w / 2.0f, cy = (float) bounds.y0 + h / 2.0f;
        float a = e.angle * (float) M_PI / 180.0f, s = sinf (a), c = cosf (a);
        float dx = x - cx, dy = y - cy;
        float along = dx * c - dy * s, across = dx * s + dy * c;
        float scale  = fmaxf (e.scale, 1e-3f);
        float len    = fmaxf (sqrtf ((c * w) * (c * w) + (s * h) * (s * h)), 1.0f) * scale;
        float chord  = fmaxf (fminf (w / fmaxf (fabsf (c), 1e-6f), h / fmaxf (fabsf (s), 1e-6f)), 1.0f) * scale;
        float corner = 0.5f * (c - s), t;
        switch (e.gradient)
        {
        case doc::GradientStyle::Reflected:
            t = fabsf ((along - corner) / (chord / 2.0f));
            break;
        case doc::GradientStyle::Radial:
            t = sqrtf (dx * dx + dy * dy) / (len / 2.0f);
            break;
        case doc::GradientStyle::Diamond:
            t = (fabsf (along) + fabsf (across)) / (len / 2.0f);
            break;
        case doc::GradientStyle::Angle:
            {
                float v = (a - atan2f (-dy, dx)) / (2.0f * (float) M_PI);
                t       = v - floorf (v);
                break;
            }
        default:
            t = (along - corner) / chord + 0.5f;
            break;
        }
        t = fminf (fmaxf (t, 0.0f), 1.0f);
        return e.reverse ? 1.0f - t : t;
    }

    /* Premultiplied interpolation between two straight-alpha pixels. */
    void mix_premul (const float a[4], const float b[4], float k, float out[4])
    {
        float alpha = a[3] + (b[3] - a[3]) * k;
        if (alpha <= 0.0f)
        {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        for (int c = 0; c < 3; c++)
        {
            out[c] = (a[c] * a[3] + (b[c] * b[3] - a[c] * a[3]) * k) / alpha;
        }
        out[3] = fminf (alpha, 1.0f);
    }

    /* Composites `content` (the layer's pixels over `big`, masked, clipped layers applied) with its
       effects into `backdrop` (any rect inside `big`). `bounds` is the layer's own extent, which
       aligned gradients span. */
    void composite_with_effects (const doc::Layer& layer, const Buffer& content, Buffer* backdrop, Rect bounds,
        float light_angle, float light_alt, BlendSpace space, const doc::Document* d)
    {
        Rect big = content.rect, rect = backdrop->rect;
        size_t w = big.width (), h = big.height (), n = w * h;
        Map shape = map_new (w, h, 0.0f);
        for (size_t i = 0; i < n; i++)
        {
            shape.v[i] = content.px[i * 4 + 3];
        }
        std::vector<const doc::Effect*> items;
        for (size_t i = 0; i < layer.style.items.size (); i++)
        {
            if (layer.style.items[i].enabled)
            {
                items.push_back (&layer.style.items[i]);
            }
        }
        /* The backdrop extended to big (outside rect it is never written back). */
        std::vector<float> work (n * 4, 0.0f);
        Rect inter = rect.intersect (big);
        for (int32_t y = inter.y0; y < inter.y1; y++)
        {
            for (int32_t x = inter.x0; x < inter.x1; x++)
            {
                memcpy (&work[((size_t) (y - big.y0) * w + (size_t) (x - big.x0)) * 4], backdrop->at (x, y),
                    4 * sizeof (float));
            }
        }
        /* Several instances of a kind: the first listed is on top, so paint from the last. */
        float fill = layer.fill_opacity;
        for (size_t k = items.size (); k-- > 0;)
        {
            const doc::Effect& e = *items[k];
            if (e.kind == doc::FxKind::DropShadow)
            {
                Map m = shadow_map (shape, e, light_angle, false);
                if (e.knocks_out)
                {
                    for (size_t i = 0; i < n; i++)
                    {
                        m.v[i] *= 1.0f - shape.v[i] * (1.0f - fill);
                    }
                }
                paint_color (&work, m, e.color, e.blend, e.opacity, space);
            }
        }
        for (size_t k = items.size (); k-- > 0;)
        {
            const doc::Effect& e = *items[k];
            if (e.kind == doc::FxKind::OuterGlow)
            {
                Map m = glow_map (shape, e, false);
                for (size_t i = 0; i < n; i++)
                {
                    m.v[i] *= 1.0f - shape.v[i] * (1.0f - fill);
                }
                paint_color (&work, m, e.color, e.blend, e.opacity, space);
            }
        }

        /* The layer: content at fill opacity inside the shape, then the interior effects relative
           to the shape. */
        std::vector<float> lay (n * 4);
        Map full = map_new (w, h, 0.0f);
        for (size_t i = 0; i < n; i++)
        {
            bool in = shape.v[i] > INSIDE_EPS;
            memcpy (&lay[i * 4], &content.px[i * 4], 3 * sizeof (float));
            lay[i * 4 + 3] = in ? fill : 0.0f;
            full.v[i]      = in ? 1.0f : 0.0f;
        }
        /* Pattern Overlay (under the gradient and colour overlays): the document's pattern tiled
           from the layer's corner (or the canvas's without Link), scaled. */
        for (size_t k = items.size (); k-- > 0 && d != NULL;)
        {
            const doc::Effect& e = *items[k];
            if (e.kind != doc::FxKind::PatternOverlay)
            {
                continue;
            }
            const doc::DocPattern* pat = d->pattern (e.pattern);
            if (pat == NULL || pat->width == 0 || pat->height == 0)
            {
                continue;
            }
            float ox = (e.link ? (float) bounds.x0 : 0.0f) + e.phase[0],
                  oy = (e.link ? (float) bounds.y0 : 0.0f) + e.phase[1], sc = fmaxf (e.scale, 0.01f);
            int32_t pw = (int32_t) pat->width, ph = (int32_t) pat->height;
            for (size_t i = 0; i < n; i++)
            {
                if (full.v[i] <= 0.0f)
                {
                    continue;
                }
                float x    = ((float) big.x0 + (float) (i % w) + 0.5f - ox) / sc,
                      y    = ((float) big.y0 + (float) (i / w) + 0.5f - oy) / sc;
                int32_t px = ((int32_t) floorf (x) % pw + pw) % pw, py = ((int32_t) floorf (y) % ph + ph) % ph;
                const float* c = &pat->rgba[((size_t) py * pat->width + (size_t) px) * 4];
                float src[4]   = {c[0], c[1], c[2], c[3] * e.opacity};
                ps_composite (space, e.blend, &lay[i * 4], src, 1.0f, &lay[i * 4]);
            }
        }
        for (size_t k = items.size (); k-- > 0;)
        {
            const doc::Effect& e = *items[k];
            if (e.kind != doc::FxKind::GradientOverlay)
            {
                continue;
            }
            for (size_t i = 0; i < n; i++)
            {
                if (full.v[i] <= 0.0f)
                {
                    continue;
                }
                float t = gradient_t (
                    e, bounds, (float) big.x0 + (float) (i % w) + 0.5f, (float) big.y0 + (float) (i / w) + 0.5f);
                float src[4];
                for (int c = 0; c < 3; c++)
                {
                    src[c] = e.color[c] + (e.color2[c] - e.color[c]) * t;
                }
                src[3] = e.opacity;
                ps_composite (space, e.blend, &lay[i * 4], src, 1.0f, &lay[i * 4]);
            }
        }
        for (size_t k = items.size (); k-- > 0;)
        {
            if (items[k]->kind == doc::FxKind::ColorOverlay)
            {
                paint_color (&lay, full, items[k]->color, items[k]->blend, items[k]->opacity, space);
            }
        }
        /* Satin, inner glows and inner shadows, relative to the shape's coverage. */
        doc::FxKind interior[3] = {doc::FxKind::Satin, doc::FxKind::InnerGlow, doc::FxKind::InnerShadow};
        for (int pass = 0; pass < 3; pass++)
        {
            for (size_t k = items.size (); k-- > 0;)
            {
                const doc::Effect& e = *items[k];
                if (e.kind != interior[pass])
                {
                    continue;
                }
                Map m = pass == 0 ? satin_map (shape, e)
                      : pass == 1 ? glow_map (shape, e, true)
                                  : shadow_map (shape, e, light_angle, true);
                for (size_t i = 0; i < n; i++)
                {
                    m.v[i] = shape.v[i] > INSIDE_EPS ? fminf (m.v[i] / shape.v[i], 1.0f) : 0.0f;
                }
                paint_color (&lay, m, e.color, e.blend, e.opacity, space);
            }
        }
        /* Strokes: inside parts over the layer (bottom instance first). */
        bool any_stroke = false;
        for (size_t k = 0; k < items.size (); k++)
        {
            any_stroke = any_stroke || items[k]->kind == doc::FxKind::Stroke;
        }
        std::vector<float> din, dout;
        if (any_stroke)
        {
            din  = dist_inside (shape);
            dout = dist_outside (shape);
        }
        for (size_t k = items.size (); k-- > 0;)
        {
            const doc::Effect& e = *items[k];
            if (e.kind != doc::FxKind::Stroke)
            {
                continue;
            }
            float in_w = e.position == doc::StrokePosition::Inside ? e.size
                       : e.position == doc::StrokePosition::Center ? e.size / 2.0f
                                                                   : 0.0f;
            if (in_w <= 0.0f)
            {
                continue;
            }
            Map m = map_new (w, h, 0.0f);
            for (size_t i = 0; i < n; i++)
            {
                m.v[i] = full.v[i] > 0.0f ? fminf (fmaxf (in_w + 0.5f - din[i], 0.0f), 1.0f) : 0.0f;
            }
            paint_color (&lay, m, e.color, e.blend, e.opacity, space);
        }
        for (size_t k = items.size (); k-- > 0;)
        {
            const doc::Effect& e = *items[k];
            if (e.kind != doc::FxKind::BevelEmboss || bevel_paint (e) != BevelPaint::Inner)
            {
                continue;
            }
            Map hi, sh;
            bevel_maps (shape, e, light_angle, light_alt, &hi, &sh);
            for (size_t i = 0; i < n; i++)
            {
                float a = shape.v[i] > INSIDE_EPS ? 1.0f / shape.v[i] : 0.0f;
                hi.v[i] = fminf (hi.v[i] * a, 1.0f);
                sh.v[i] = fminf (sh.v[i] * a, 1.0f);
            }
            paint_color (&lay, hi, e.color, e.blend, e.opacity, space);
            paint_color (&lay, sh, e.color2, e.blend2, e.opacity2, space);
        }
        for (size_t i = 0; i < n; i++)
        {
            lay[i * 4 + 3] *= fminf (shape.v[i], 1.0f);
        }

        /* Outside parts of strokes beneath the layer, onto the backdrop with their own modes; each
           takes the share of the pixel the strokes above it left. */
        if (any_stroke)
        {
            std::vector<float> acc (n * 4, 0.0f), cover (n, 0.0f), base = work;
            for (size_t k = 0; k < items.size (); k++)
            {
                const doc::Effect& e = *items[k];
                if (e.kind != doc::FxKind::Stroke)
                {
                    continue;
                }
                float out_w = e.position == doc::StrokePosition::Outside ? e.size
                            : e.position == doc::StrokePosition::Center  ? e.size / 2.0f
                                                                         : 0.0f;
                if (out_w <= 0.0f)
                {
                    continue;
                }
                for (size_t i = 0; i < n; i++)
                {
                    float band = fminf (fmaxf (out_w + 0.5f - dout[i], 0.0f), 1.0f), kk;
                    if (shape.v[i] > INSIDE_EPS)
                    {
                        float c = fminf (fmaxf (lay[i * 4 + 3], 0.0f), 1.0f);
                        kk      = c >= 1.0f ? 1.0f
                                            : fminf (fmaxf ((1.0f - fminf (shape.v[i], 1.0f)) / (1.0f - c), 0.0f), 1.0f);
                    }
                    else
                    {
                        kk = band;
                    }
                    float share = kk * e.opacity * (1.0f - cover[i]);
                    if (share <= 0.0f)
                    {
                        continue;
                    }
                    cover[i] += share;
                    float b[4], src[4] = {e.color[0], e.color[1], e.color[2], 1.0f};
                    ps_composite (space, e.blend, &base[i * 4], src, 1.0f, b);
                    for (int c = 0; c < 3; c++)
                    {
                        acc[i * 4 + c] += share * b[c] * b[3];
                    }
                    acc[i * 4 + 3] += share * b[3];
                }
            }
            for (size_t i = 0; i < n; i++)
            {
                if (cover[i] <= 0.0f)
                {
                    continue;
                }
                float* p = &work[i * 4];
                float kk = 1.0f - fminf (cover[i], 1.0f), alpha = p[3] * kk + acc[i * 4 + 3];
                if (alpha <= 0.0f)
                {
                    continue;
                }
                for (int c = 0; c < 3; c++)
                {
                    p[c] = (p[c] * p[3] * kk + acc[i * 4 + c]) / alpha;
                }
                p[3] = fminf (alpha, 1.0f);
            }
        }
        for (size_t k = items.size (); k-- > 0;)
        {
            const doc::Effect& e = *items[k];
            if (e.kind != doc::FxKind::BevelEmboss || bevel_paint (e) != BevelPaint::Outer)
            {
                continue;
            }
            Map hi, sh;
            bevel_maps (shape, e, light_angle, light_alt, &hi, &sh);
            paint_color (&work, hi, e.color, e.blend, e.opacity, space);
            paint_color (&work, sh, e.color2, e.blend2, e.opacity2, space);
        }

        /* The layer with its interior effects onto that, in the layer's mode. */
        BlendMode mode = layer.blend == BlendMode::PassThrough ? BlendMode::Normal : layer.blend;
        for (size_t i = 0; i < n; i++)
        {
            if (lay[i * 4 + 3] > 0.0f)
            {
                ps_composite (space, mode, &work[i * 4], &lay[i * 4], 1.0f, &work[i * 4]);
            }
        }
        for (size_t k = items.size (); k-- > 0;)
        {
            const doc::Effect& e = *items[k];
            if (e.kind != doc::FxKind::BevelEmboss || bevel_paint (e) != BevelPaint::Both)
            {
                continue;
            }
            Map hi, sh;
            bevel_maps (shape, e, light_angle, light_alt, &hi, &sh);
            paint_color (&work, hi, e.color, e.blend, e.opacity, space);
            paint_color (&work, sh, e.color2, e.blend2, e.opacity2, space);
        }
        /* Layer opacity over the whole stack. */
        for (int32_t y = inter.y0; y < inter.y1; y++)
        {
            for (int32_t x = inter.x0; x < inter.x1; x++)
            {
                float* b = backdrop->at (x, y);
                float before[4];
                memcpy (before, b, sizeof before);
                mix_premul (before, &work[((size_t) (y - big.y0) * w + (size_t) (x - big.x0)) * 4], layer.opacity, b);
            }
        }
    }

} // namespace compose
