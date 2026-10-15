#include "algo/transform.h"

#include "algo/resample.h"
#include <float.h>
#include <math.h>
#include <string.h>
#include <vector>

namespace algo
{

    /* Free Transform: projective warps of surfaces. A transform is where the four corners of a
       source rectangle land; scale, rotate, skew and flips are affine special cases, Distort and
       Perspective use the full homography. Warping inverse-maps each destination pixel and samples
       premultiplied (nearest, bilinear or Catmull-Rom bicubic), destination tile by tile in parallel.
       Large reductions are pre-filtered with a proper resize so shrinking doesn't alias. */

    using geom::Rect;

    Homography Homography::identity ()
    {
        Homography h = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
        return h;
    }

    /* Affine [a b c d e f] maps (x, y) to (a x + c y + e, b x + d y + f). */
    Homography Homography::affine (double a, double b, double c, double d, double e, double f)
    {
        Homography h = {{a, c, e, b, d, f, 0, 0, 1}};
        return h;
    }

    bool Homography::is_identity () const
    {
        if (fabs (m[8]) < 1e-12)
        {
            return false;
        }
        double w = m[8];
        return fabs (m[0] / w - 1.0) < 1e-9 && fabs (m[4] / w - 1.0) < 1e-9 && fabs (m[1] / w) < 1e-9
            && fabs (m[2] / w) < 1e-9 && fabs (m[3] / w) < 1e-9 && fabs (m[5] / w) < 1e-9 && fabs (m[6] / w) < 1e-9
            && fabs (m[7] / w) < 1e-9;
    }

    bool Homography::is_affine () const
    {
        return fabs (m[6]) < 1e-12 && fabs (m[7]) < 1e-12;
    }

    void Homography::apply (double x, double y, double* ox, double* oy) const
    {
        double w = m[6] * x + m[7] * y + m[8];
        *ox      = (m[0] * x + m[1] * y + m[2]) / w;
        *oy      = (m[3] * x + m[4] * y + m[5]) / w;
    }

    Homography Homography::mul (const Homography& o) const
    {
        Homography r;
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
            {
                double s = 0.0;
                for (int k = 0; k < 3; k++)
                {
                    s += m[i * 3 + k] * o.m[k * 3 + j];
                }
                r.m[i * 3 + j] = s;
            }
        }
        return r;
    }

    bool Homography::inverse (Homography* out) const
    {
        double c[9] = {
            m[4] * m[8] - m[5] * m[7],
            m[2] * m[7] - m[1] * m[8],
            m[1] * m[5] - m[2] * m[4],
            m[5] * m[6] - m[3] * m[8],
            m[0] * m[8] - m[2] * m[6],
            m[2] * m[3] - m[0] * m[5],
            m[3] * m[7] - m[4] * m[6],
            m[1] * m[6] - m[0] * m[7],
            m[0] * m[4] - m[1] * m[3],
        };
        double det = m[0] * c[0] + m[1] * c[3] + m[2] * c[6];
        if (fabs (det) < 1e-12)
        {
            return false;
        }
        for (int i = 0; i < 9; i++)
        {
            out->m[i] = c[i] / det;
        }
        return true;
    }

    /* Unit square to quad (corners (0,0), (1,0), (1,1), (0,1)). */
    bool square_to_quad (const double q[4][2], Homography* out)
    {
        double x0 = q[0][0], y0 = q[0][1], x1 = q[1][0], y1 = q[1][1], x2 = q[2][0], y2 = q[2][1], x3 = q[3][0],
               y3 = q[3][1];
        double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
        if (fabs (sx) < 1e-12 && fabs (sy) < 1e-12)
        {
            Homography h = {{x1 - x0, x3 - x0, x0, y1 - y0, y3 - y0, y0, 0, 0, 1}};
            *out         = h;
            return true;
        }
        double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
        double den = dx1 * dy2 - dx2 * dy1;
        if (fabs (den) < 1e-12)
        {
            return false;
        }
        double g     = (sx * dy2 - dx2 * sy) / den;
        double h     = (dx1 * sy - sx * dy1) / den;
        Homography r = {{x1 - x0 + g * x1, x3 - x0 + h * x3, x0, y1 - y0 + g * y1, y3 - y0 + h * y3, y0, g, h, 1}};
        *out         = r;
        return true;
    }

    /* The transform taking rectangle r (x0, y0, x1, y1) onto `quad` (corners clockwise from
       top-left); false for degenerate quads. */
    bool Homography::rect_to_quad (const double r[4], const double quad[4][2], Homography* out)
    {
        double w = r[2] - r[0], h = r[3] - r[1];
        if (w <= 0.0 || h <= 0.0)
        {
            return false;
        }
        Homography to_unit = {{1.0 / w, 0, -r[0] / w, 0, 1.0 / h, -r[1] / h, 0, 0, 1}};
        Homography sq;
        if (!square_to_quad (quad, &sq))
        {
            return false;
        }
        *out = sq.mul (to_unit);
        return true;
    }

    /* Approximate linear scale of the map near (x, y): the square root of its Jacobian. */
    double Homography::local_scale (double x, double y) const
    {
        double e = 0.5, ax, ay, bx, by, cx, cy, dx, dy;
        apply (x - e, y, &ax, &ay);
        apply (x + e, y, &bx, &by);
        apply (x, y - e, &cx, &cy);
        apply (x, y + e, &dx, &dy);
        double jx0 = bx - ax, jx1 = by - ay, jy0 = dx - cx, jy1 = dy - cy;
        return sqrt (fabs (jx0 * jy1 - jx1 * jy0));
    }

    Interp interp_from_str (const char* s)
    {
        if (strcmp (s, "nearest") == 0 || strcmp (s, "nearestNeighbor") == 0)
        {
            return Interp::Nearest;
        }
        if (strcmp (s, "bilinear") == 0)
        {
            return Interp::Bilinear;
        }
        return Interp::Bicubic;
    }

    void catmull_rom (double t, double out[4])
    {
        double t2 = t * t, t3 = t2 * t;
        out[0] = -0.5 * t3 + t2 - 0.5 * t;
        out[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
        out[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
        out[3] = 0.5 * t3 - 0.5 * t2;
    }

    /* Warp `src` (its content inside src_rect) by h (source to destination). The result has alpha
       (added when src had none); pixels outside the warped quad are transparent. */
    raster::Surface warp_surface (const raster::Surface& source, Rect src_rect, const Homography& map, Interp interp)
    {
        color::PixelFormat fmt = source.format ();
        raster::Surface converted;
        const raster::Surface* src = &source;
        if (!fmt.alpha)
        {
            fmt.alpha = true;
            converted = source.convert (fmt, NULL);
            src       = &converted;
        }
        raster::Surface out (fmt);
        if (src_rect.is_empty ())
        {
            return out;
        }
        Homography h = map;
        double scale = h.local_scale ((src_rect.x0 + src_rect.x1) / 2.0, (src_rect.y0 + src_rect.y1) / 2.0);
        raster::Surface reduced;
        if (interp != Interp::Nearest && scale < 0.5 && scale > 0.0)
        {
            double f = pow (2.0, ceil (log2 (scale)));
            if (f > 1.0)
            {
                f = 1.0;
            }
            reduced  = resize_surface (*src, f, f, Resample::Bicubic);
            src      = &reduced;
            h        = h.mul (Homography::affine (1.0 / f, 0, 0, 1.0 / f, 0, 0));
            src_rect = scaled_rect (src_rect, f, f);
        }
        Homography inv;
        if (!h.inverse (&inv))
        {
            return out;
        }
        double cx[4], cy[4];
        double sxs[4] = {(double) src_rect.x0, (double) src_rect.x1, (double) src_rect.x1, (double) src_rect.x0};
        double sys[4] = {(double) src_rect.y0, (double) src_rect.y0, (double) src_rect.y1, (double) src_rect.y1};
        for (int i = 0; i < 4; i++)
        {
            h.apply (sxs[i], sys[i], &cx[i], &cy[i]);
        }
        /* The forward w must keep one sign over the source (it is in front of the horizon), else the
           warped corners don't bound it. */
        double fw[4];
        double ex[4] = {src_rect.x0 - 1.0, src_rect.x1 + 1.0, src_rect.x1 + 1.0, src_rect.x0 - 1.0};
        double ey[4] = {src_rect.y0 - 1.0, src_rect.y0 - 1.0, src_rect.y1 + 1.0, src_rect.y1 + 1.0};
        bool pos = true, neg = true;
        for (int i = 0; i < 4; i++)
        {
            fw[i] = h.m[6] * ex[i] + h.m[7] * ey[i] + h.m[8];
            pos   = pos && fw[i] > 0.0;
            neg   = neg && fw[i] < 0.0;
        }
        bool finite = true;
        for (int i = 0; i < 4; i++)
        {
            finite = finite && isfinite (cx[i]) && isfinite (cy[i]);
        }
        if (!finite || (!pos && !neg))
        {
            return out;
        }
        const double LIM = 1 << 20;
        double mnx = DBL_MAX, mny = DBL_MAX, mxx = -DBL_MAX, mxy = -DBL_MAX;
        for (int i = 0; i < 4; i++)
        {
            mnx = fmin (mnx, cx[i]);
            mny = fmin (mny, cy[i]);
            mxx = fmax (mxx, cx[i]);
            mxy = fmax (mxy, cy[i]);
        }
        Rect dst = {(int32_t) fmax (floor (mnx), -LIM) - 1, (int32_t) fmax (floor (mny), -LIM) - 1,
            (int32_t) fmin (ceil (mxx), LIM) + 1, (int32_t) fmin (ceil (mxy), LIM) + 1};
        if (dst.is_empty ())
        {
            return out;
        }
        size_t n = fmt.channels (), a = n - 1;
        std::vector<geom::TileCoord> coords = dst.tiles ();
        std::vector<std::vector<float>> results (coords.size ());
        std::vector<Rect> rects (coords.size ());
#pragma omp parallel for schedule(dynamic)
        for (ptrdiff_t ti = 0; ti < (ptrdiff_t) coords.size (); ti++)
        {
            Rect t             = coords[(size_t) ti].rect ().intersect (dst);
            rects[(size_t) ti] = t;
            if (t.is_empty ())
            {
                continue;
            }
            /* Source footprint of the tile, padded for the filter. */
            double fx0 = DBL_MAX, fy0 = DBL_MAX, fx1 = -DBL_MAX, fy1 = -DBL_MAX;
            double tx[4] = {(double) t.x0, (double) t.x1, (double) t.x1, (double) t.x0},
                   ty[4] = {(double) t.y0, (double) t.y0, (double) t.y1, (double) t.y1};
            for (int i = 0; i < 4; i++)
            {
                double u, v;
                inv.apply (tx[i], ty[i], &u, &v);
                fx0 = fmin (fx0, u);
                fy0 = fmin (fy0, v);
                fx1 = fmax (fx1, u);
                fy1 = fmax (fy1, v);
            }
            Rect foot = {(int32_t) floor (fmax (fx0, -LIM)) - 3, (int32_t) floor (fmax (fy0, -LIM)) - 3,
                (int32_t) ceil (fmin (fx1, LIM)) + 3, (int32_t) ceil (fmin (fy1, LIM)) + 3};
            foot      = foot.intersect (src_rect);
            if (foot.is_empty () || !src->has_tiles_in (foot))
            {
                continue;
            }
            std::vector<float> px;
            if (!src->read_region (foot, &px))
            {
                continue;
            }
            size_t fwid = foot.width ();
            premultiply (px.data (), px.size () / n, n, true);
            std::vector<float>& outp = results[(size_t) ti];
            outp.assign ((size_t) t.width () * (size_t) t.height () * n, 0.0f);
            bool any = false;
            double acc[raster::MAX_CHANNELS];
            for (int32_t y = t.y0; y < t.y1; y++)
            {
                for (int32_t x = t.x0; x < t.x1; x++)
                {
                    double u, v;
                    inv.apply (x + 0.5, y + 0.5, &u, &v);
                    u -= 0.5;
                    v -= 0.5;
                    if (u < src_rect.x0 - 1.0 || v < src_rect.y0 - 1.0 || u > src_rect.x1 || v > src_rect.y1)
                    {
                        continue;
                    }
                    for (size_t c = 0; c < n; c++)
                    {
                        acc[c] = 0.0;
                    }
                    int32_t ix = (int32_t) floor (u), iy = (int32_t) floor (v);
                    if (interp == Interp::Nearest)
                    {
                        int32_t nx = (int32_t) floor (u + 0.5), ny = (int32_t) floor (v + 0.5);
                        if (nx >= foot.x0 && ny >= foot.y0 && nx < foot.x1 && ny < foot.y1)
                        {
                            const float* p = &px[((size_t) (ny - foot.y0) * fwid + (size_t) (nx - foot.x0)) * n];
                            for (size_t c = 0; c < n; c++)
                            {
                                acc[c] = p[c];
                            }
                        }
                    }
                    else if (interp == Interp::Bilinear)
                    {
                        double fx = u - ix, fy = v - iy;
                        double wts[2][2] = {{(1 - fx) * (1 - fy), fx * (1 - fy)}, {(1 - fx) * fy, fx * fy}};
                        for (int dy = 0; dy < 2; dy++)
                        {
                            for (int dx = 0; dx < 2; dx++)
                            {
                                int32_t sx = ix + dx, sy = iy + dy;
                                if (sx < foot.x0 || sy < foot.y0 || sx >= foot.x1 || sy >= foot.y1)
                                {
                                    continue;
                                }
                                const float* p = &px[((size_t) (sy - foot.y0) * fwid + (size_t) (sx - foot.x0)) * n];
                                for (size_t c = 0; c < n; c++)
                                {
                                    acc[c] += p[c] * wts[dy][dx];
                                }
                            }
                        }
                    }
                    else
                    {
                        double wx[4], wy[4];
                        catmull_rom (u - ix, wx);
                        catmull_rom (v - iy, wy);
                        for (int j = 0; j < 4; j++)
                        {
                            int32_t sy = iy - 1 + j;
                            if (sy < foot.y0 || sy >= foot.y1)
                            {
                                continue;
                            }
                            for (int i = 0; i < 4; i++)
                            {
                                int32_t sx = ix - 1 + i;
                                if (sx < foot.x0 || sx >= foot.x1)
                                {
                                    continue;
                                }
                                double k       = wx[i] * wy[j];
                                const float* p = &px[((size_t) (sy - foot.y0) * fwid + (size_t) (sx - foot.x0)) * n];
                                for (size_t c = 0; c < n; c++)
                                {
                                    acc[c] += p[c] * k;
                                }
                            }
                        }
                    }
                    double al = acc[a] < 0.0 ? 0.0 : acc[a] > 1.0 ? 1.0 : acc[a];
                    if (al <= 0.0)
                    {
                        continue;
                    }
                    any      = true;
                    float* o = &outp[((size_t) (y - t.y0) * (size_t) t.width () + (size_t) (x - t.x0)) * n];
                    for (size_t c = 0; c < a; c++)
                    {
                        o[c] = (float) (acc[c] / al);
                    }
                    o[a] = (float) al;
                }
            }
            if (!any)
            {
                outp.clear ();
            }
        }
        for (size_t i = 0; i < coords.size (); i++)
        {
            if (!results[i].empty ())
            {
                out.write_region (rects[i], results[i].data (), results[i].size ());
            }
        }
        out.prune ();
        return out;
    }

    /* Warp a single-channel surface whose outside reads as its default (masks, selections): warp
       the content with alpha, then flatten back onto the default. */
    raster::Surface warp_gray (const raster::Surface& s, const Homography& h, Interp interp)
    {
        float def[raster::MAX_CHANNELS];
        s.default_pixel (def);
        color::PixelFormat fmt = s.format ();
        raster::Surface out    = raster::Surface::with_default (fmt, def);
        Rect src               = s.content_bounds ();
        if (src.is_empty ())
        {
            return out;
        }
        color::PixelFormat with_alpha = fmt;
        with_alpha.alpha              = true;
        std::vector<float> v;
        if (!s.read_region (src, &v))
        {
            return out;
        }
        size_t ch    = s.channels ();
        size_t count = v.size () / ch;
        std::vector<float> lifted (count * 2);
        for (size_t i = 0; i < count; i++)
        {
            lifted[i * 2]     = v[i * ch];
            lifted[i * 2 + 1] = 1.0f;
        }
        raster::Surface tmp (with_alpha);
        tmp.write_region (src, lifted.data (), lifted.size ());
        raster::Surface w = warp_surface (tmp, src, h, interp);
        std::vector<float> old (count * ch);
        for (size_t i = 0; i < count; i++)
        {
            memcpy (&old[i * ch], def, ch * sizeof (float));
        }
        out.write_region (src, old.data (), old.size ());
        Rect b = w.content_bounds ();
        if (!b.is_empty ())
        {
            std::vector<float> px;
            w.read_region (b, &px);
            size_t m = px.size () / 2;
            std::vector<float> flat (m * ch);
            for (size_t i = 0; i < m; i++)
            {
                float val = px[i * 2] * px[i * 2 + 1] + def[0] * (1.0f - px[i * 2 + 1]);
                for (size_t c = 0; c < ch; c++)
                {
                    flat[i * ch + c] = val;
                }
            }
            out.write_region (b, flat.data (), flat.size ());
        }
        out.prune ();
        return out;
    }

    /* warp_gray() limited to a selection: only the selected values move; the vacated area reads as
       the default. */
    raster::Surface warp_gray_selected (
        const raster::Surface& s, const raster::Surface& sel, const Homography& h, Interp interp)
    {
        color::PixelFormat fmt = s.format ();
        if (fmt.alpha || fmt.channels () != 1)
        {
            return warp_gray (s, h, interp);
        }
        float def[raster::MAX_CHANNELS];
        s.default_pixel (def);
        raster::Surface rest  = s;
        color::PixelFormat la = fmt;
        la.alpha              = true;
        raster::Surface lifted (la);
        Rect area = sel.content_bounds ();
        if (area.is_empty ())
        {
            return rest;
        }
        std::vector<float> v;
        s.read_region (area, &v);
        size_t w = area.width ();
        std::vector<float> lp (v.size () * 2), rp (v.size ());
        for (size_t i = 0; i < v.size (); i++)
        {
            int32_t x = area.x0 + (int32_t) (i % w), y = area.y0 + (int32_t) (i / w);
            float k       = sel.sample_channel (x, y, 0);
            lp[i * 2]     = v[i];
            lp[i * 2 + 1] = k;
            rp[i]         = v[i] * (1.0f - k) + def[0] * k;
        }
        lifted.write_region (area, lp.data (), lp.size ());
        lifted.prune ();
        rest.write_region (area, rp.data (), rp.size ());
        Rect src = lifted.content_bounds ();
        if (src.is_empty ())
        {
            rest.prune ();
            return rest;
        }
        raster::Surface moved = warp_surface (lifted, src, h, interp);
        Rect b                = moved.content_bounds ();
        if (!b.is_empty ())
        {
            std::vector<float> mv, under;
            moved.read_region (b, &mv);
            rest.read_region (b, &under);
            for (size_t i = 0; i < under.size (); i++)
            {
                under[i] = mv[i * 2] * mv[i * 2 + 1] + under[i] * (1.0f - mv[i * 2 + 1]);
            }
            rest.write_region (b, under.data (), under.size ());
        }
        rest.prune ();
        return rest;
    }

    /* A layer surface split by a selection: the selected pixels, and everything else (both with
       alpha). Fully lifted pixels in the rest go transparent so they no longer count as content. */
    void split_selected (
        const raster::Surface& surf, const raster::Surface& sel, raster::Surface* lifted, raster::Surface* rest)
    {
        color::PixelFormat with_alpha = surf.format ();
        with_alpha.alpha              = true;
        *lifted                       = raster::Surface (with_alpha);
        *rest                         = surf.format () == with_alpha ? surf : surf.convert (with_alpha, NULL);
        Rect src                      = surf.content_bounds ();
        if (src.is_empty ())
        {
            return;
        }
        size_t n = with_alpha.channels ();
        std::vector<float> rp;
        rest->read_region (src, &rp);
        std::vector<float> lp = rp;
        size_t w              = src.width ();
        for (size_t i = 0; i < rp.size () / n; i++)
        {
            int32_t x = src.x0 + (int32_t) (i % w), y = src.y0 + (int32_t) (i / w);
            float k = sel.sample_channel (x, y, 0);
            lp[i * n + n - 1] *= k;
            rp[i * n + n - 1] *= 1.0f - k;
            if (rp[i * n + n - 1] <= 0.0f)
            {
                for (size_t c = 0; c < n; c++)
                {
                    rp[i * n + c] = 0.0f;
                }
            }
        }
        lifted->write_region (src, lp.data (), lp.size ());
        lifted->prune ();
        rest->write_region (src, rp.data (), rp.size ());
    }

    /* Normal-blend `top` over `dst` (straight alpha, same format). */
    void composite_over (raster::Surface* dst, const raster::Surface& top)
    {
        Rect b = top.content_bounds ();
        if (b.is_empty ())
        {
            return;
        }
        size_t n = dst->format ().channels (), a = n - 1;
        std::vector<float> t, d;
        top.read_region (b, &t);
        dst->read_region (b, &d);
        for (size_t i = 0; i < d.size () / n; i++)
        {
            float* dp       = &d[i * n];
            const float* tp = &t[i * n];
            float ta = tp[a], da = dp[a];
            float oa = ta + da * (1.0f - ta);
            if (oa > 0.0f)
            {
                for (size_t c = 0; c < a; c++)
                {
                    dp[c] = (tp[c] * ta + dp[c] * da * (1.0f - ta)) / oa;
                }
            }
            dp[a] = oa;
        }
        dst->write_region (b, d.data (), d.size ());
    }

} // namespace algo
