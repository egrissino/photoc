#include "algo/select_ops.h"

#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace algo
{

    /* Selection operations on float masks (row-major over an area): Select > Modify, the lasso's
       polygons and the Magic Wand's flood fill. */

    using geom::Rect;

    /* A GRAY8 selection surface from a mask over `area`. */
    raster::Surface mask_to_surface (const std::vector<float>& m, Rect area)
    {
        raster::Surface out (color::PixelFormat::gray8 ());
        if (area.is_empty () || m.empty ())
        {
            return out;
        }
        out.write_region (area, m.data (), m.size ());
        out.prune ();
        return out;
    }

    const float EMPTY_DISTANCE = 1e10f;

    /* Euclidean distance to the nearest `true` pixel: nearest seeds down the columns, then the lower
       envelope of parabolas along the rows (Felzenszwalb and Huttenlocher). */
    std::vector<float> edt (const std::vector<bool>& inside, size_t w, size_t h)
    {
        size_t n = w * h;
        if (n == 0 || inside.size () != n)
        {
            return std::vector<float> ();
        }
        bool any = false, all = true;
        for (size_t i = 0; i < n; i++)
        {
            any = any || inside[i];
            all = all && inside[i];
        }
        if (!any)
        {
            return std::vector<float> (n, EMPTY_DISTANCE);
        }
        if (all)
        {
            return std::vector<float> (n, 0.0f);
        }
        const uint32_t NONE = UINT32_MAX;
        std::vector<uint32_t> vert (n, NONE);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t xi = 0; xi < (ptrdiff_t) w; xi++)
        {
            size_t x     = (size_t) xi;
            long nearest = -1;
            for (size_t y = 0; y < h; y++)
            {
                if (inside[y * w + x])
                {
                    nearest = (long) y;
                }
                if (nearest >= 0)
                {
                    vert[y * w + x] = (uint32_t) (y - (size_t) nearest);
                }
            }
            nearest = -1;
            for (size_t y = h; y-- > 0;)
            {
                if (vert[y * w + x] == 0)
                {
                    nearest = (long) y;
                }
                if (nearest >= 0)
                {
                    uint32_t d = (uint32_t) ((size_t) nearest - y);
                    if (d < vert[y * w + x])
                    {
                        vert[y * w + x] = d;
                    }
                }
            }
        }
        std::vector<float> out (n);
#pragma omp parallel
        {
            std::vector<size_t> sites (w);
            std::vector<double> starts (w);
#pragma omp for schedule(static)
            for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
            {
                const uint32_t* costs = &vert[(size_t) yi * w];
                float* dst            = &out[(size_t) yi * w];
                size_t count          = 0;
                for (size_t q = 0; q < w; q++)
                {
                    if (costs[q] == NONE)
                    {
                        continue;
                    }
                    double fq    = (double) costs[q] * costs[q];
                    double start = -INFINITY;
                    while (count > 0)
                    {
                        size_t p  = sites[count - 1];
                        double dp = costs[p];
                        start     = (fq - dp * dp) / (2.0 * (double) (q - p)) + ((double) q + (double) p) * 0.5;
                        if (start > starts[count - 1])
                        {
                            break;
                        }
                        count--;
                    }
                    if (count == 0)
                    {
                        start = -INFINITY;
                    }
                    sites[count]  = q;
                    starts[count] = start;
                    count++;
                }
                if (count == 0)
                {
                    for (size_t x = 0; x < w; x++)
                    {
                        dst[x] = EMPTY_DISTANCE;
                    }
                    continue;
                }
                size_t k = 0;
                for (size_t x = 0; x < w; x++)
                {
                    while (k + 1 < count && starts[k + 1] < (double) x)
                    {
                        k++;
                    }
                    double dx = (double) x - (double) sites[k];
                    double dv = costs[sites[k]];
                    dst[x]    = (float) sqrt (dx * dx + dv * dv);
                }
            }
        }
        return out;
    }

    /* Grow by r pixels: a pixel d from the nearest selected centre is covered when d <= r
       (fractional radii give partial coverage). */
    std::vector<float> expand_mask (const std::vector<float>& m, size_t w, size_t h, float r)
    {
        std::vector<bool> inside (m.size ());
        for (size_t i = 0; i < m.size (); i++)
        {
            inside[i] = m[i] >= 0.5f;
        }
        std::vector<float> d = edt (inside, w, h);
        std::vector<float> out (m.size ());
        for (size_t i = 0; i < m.size (); i++)
        {
            float c = r + 1.0f - d[i];
            c       = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
            out[i]  = m[i] > c ? m[i] : c;
        }
        return out;
    }

    std::vector<float> contract_mask (const std::vector<float>& m, size_t w, size_t h, float r)
    {
        std::vector<float> inv (m.size ());
        for (size_t i = 0; i < m.size (); i++)
        {
            inv[i] = 1.0f - m[i];
        }
        std::vector<float> e = expand_mask (inv, w, h, r);
        for (size_t i = 0; i < e.size (); i++)
        {
            e[i] = 1.0f - e[i];
        }
        return e;
    }

    /* Border: a soft band 2r wide centred on the edge, full at the edge and fading to nothing r
       pixels out on either side (as measured in Photoshop). */
    std::vector<float> border_mask (const std::vector<float>& m, size_t w, size_t h, float r)
    {
        std::vector<bool> inside (m.size ()), outside (m.size ());
        for (size_t i = 0; i < m.size (); i++)
        {
            inside[i]  = m[i] >= 0.5f;
            outside[i] = !inside[i];
        }
        std::vector<float> to_in = edt (inside, w, h), to_out = edt (outside, w, h);
        std::vector<float> out (m.size (), 0.0f);
        if (r <= 0.0f || to_in.size () != m.size () || to_out.size () != m.size ())
        {
            return out;
        }
        for (size_t i = 0; i < m.size (); i++)
        {
            float d = inside[i] ? to_out[i] : to_in[i];
            float v = 1.0f - (d - 1.0f) / r;
            out[i]  = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        }
        return out;
    }

    /* Box average of radius r along each row (edges replicated), rows in parallel. */
    void box_rows (const float* src, float* dst, size_t w, size_t h, size_t r, size_t stride_in, size_t step_in,
        size_t stride_out, size_t step_out)
    {
        float win = (float) (2 * r + 1);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
        {
            const float* row = src + (size_t) yi * stride_in;
            float* o         = dst + (size_t) yi * stride_out;
            long n = (long) w, rr = (long) r;
            double acc = 0.0;
            for (long k = -rr; k <= rr; k++)
            {
                acc += row[(size_t) (k < 0 ? 0 : k >= n ? n - 1 : k) * step_in];
            }
            for (long x = 0; x < n; x++)
            {
                o[(size_t) x * step_out] = (float) (acc / win);
                long add = x + rr + 1, sub = x - rr;
                acc += row[(size_t) (add >= n ? n - 1 : add) * step_in] - row[(size_t) (sub < 0 ? 0 : sub) * step_in];
            }
        }
    }

    /* Smooth: removes specks and rounds corners (box average of radius round(r), edges replicated,
       re-thresholded at one half). */
    std::vector<float> smooth_mask (const std::vector<float>& m, size_t w, size_t h, float r)
    {
        size_t rad = (size_t) roundf (r < 0.0f ? 0.0f : r);
        if (rad == 0 || w == 0 || h == 0)
        {
            return m;
        }
        std::vector<float> tmp (m.size ()), out (m.size ());
        box_rows (m.data (), tmp.data (), w, h, rad, w, 1, w, 1);
        box_rows (tmp.data (), out.data (), h, w, rad, 1, w, 1, w);
        for (size_t i = 0; i < out.size (); i++)
        {
            out[i] = out[i] >= 0.5f ? 1.0f : 0.0f;
        }
        return out;
    }

    /* Feather: Gaussian blur with sigma = radius (truncated at 3 sigma, zero beyond the canvas), as
       in Photoshop. */
    std::vector<float> feather_mask (const std::vector<float>& m, size_t w, size_t h, float radius)
    {
        float sigma = radius < 0.0f ? 0.0f : radius;
        if (sigma < 0.1f || w == 0 || h == 0)
        {
            return m;
        }
        long r = (long) ceilf (sigma * 3.0f);
        std::vector<float> k ((size_t) (2 * r + 1));
        float sum = 0.0f;
        for (long i = -r; i <= r; i++)
        {
            k[(size_t) (i + r)] = expf (-(float) (i * i) / (2.0f * sigma * sigma));
            sum += k[(size_t) (i + r)];
        }
        for (size_t i = 0; i < k.size (); i++)
        {
            k[i] /= sum;
        }
        std::vector<float> tmp (m.size ()), out (m.size ());
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
        {
            size_t y = (size_t) yi;
            for (long x = 0; x < (long) w; x++)
            {
                float a = 0.0f;
                long lo = x - r < 0 ? 0 : x - r, hi = x + r >= (long) w ? (long) w - 1 : x + r;
                for (long xx = lo; xx <= hi; xx++)
                {
                    a += m[y * w + (size_t) xx] * k[(size_t) (xx - x + r)];
                }
                tmp[y * w + (size_t) x] = a;
            }
        }
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
        {
            long y  = (long) yi;
            long lo = y - r < 0 ? 0 : y - r, hi = y + r >= (long) h ? (long) h - 1 : y + r;
            float* o = &out[(size_t) y * w];
            for (size_t x = 0; x < w; x++)
            {
                o[x] = 0.0f;
            }
            for (long yy = lo; yy <= hi; yy++)
            {
                float kv         = k[(size_t) (yy - y + r)];
                const float* src = &tmp[(size_t) yy * w];
                for (size_t x = 0; x < w; x++)
                {
                    o[x] += src[x] * kv;
                }
            }
        }
        return out;
    }

    /* Even-odd polygon coverage over `area`: exact horizontal coverage on 4 sub-rows per pixel row
       (1 without anti-aliasing). */
    std::vector<float> polygon_mask (const std::vector<PolyPoint>& points, Rect area, bool anti_alias)
    {
        size_t w = area.width (), h = area.height ();
        std::vector<float> m (w * h, 0.0f);
        if (points.size () < 3)
        {
            return m;
        }
        int sub = anti_alias ? 4 : 1;
#pragma omp parallel for schedule(dynamic, 16)
        for (ptrdiff_t pyi = 0; pyi < (ptrdiff_t) h; pyi++)
        {
            size_t py = (size_t) pyi;
            std::vector<float> xs;
            for (int s = 0; s < sub; s++)
            {
                float y = (float) area.y0 + (float) py + ((float) s + 0.5f) / (float) sub;
                xs.clear ();
                for (size_t i = 0; i < points.size (); i++)
                {
                    PolyPoint a = points[i], b = points[(i + 1) % points.size ()];
                    if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y))
                    {
                        xs.push_back (a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x));
                    }
                }
                std::sort (xs.begin (), xs.end ());
                for (size_t j = 0; j + 1 < xs.size (); j += 2)
                {
                    float x0 = xs[j] - (float) area.x0, x1 = xs[j + 1] - (float) area.x0;
                    if (x0 < 0.0f)
                    {
                        x0 = 0.0f;
                    }
                    if (x1 > (float) w)
                    {
                        x1 = (float) w;
                    }
                    if (x1 <= x0)
                    {
                        continue;
                    }
                    size_t c0 = (size_t) floorf (x0), c1 = (size_t) ceilf (x1);
                    if (c1 > w)
                    {
                        c1 = w;
                    }
                    for (size_t cx = c0; cx < c1; cx++)
                    {
                        float cov;
                        if (anti_alias)
                        {
                            cov = fminf ((float) (cx + 1), x1) - fmaxf ((float) cx, x0);
                        }
                        else
                        {
                            float c = (float) cx + 0.5f;
                            cov     = c >= x0 && c < x1 ? 1.0f : 0.0f;
                        }
                        if (cov > 0.0f)
                        {
                            m[py * w + cx] += cov / (float) sub;
                        }
                    }
                }
            }
            for (size_t x = 0; x < w; x++)
            {
                m[py * w + x] = fminf (m[py * w + x], 1.0f);
            }
        }
        return m;
    }

    bool similar (const uint8_t t[4], int tol, const uint8_t* p)
    {
        if (t[3] == 0 && p[3] == 0)
        {
            return true;
        }
        for (int c = 0; c < 4; c++)
        {
            if (abs ((int) p[c] - (int) t[c]) > tol)
            {
                return false;
            }
        }
        return true;
    }

    /* Magic Wand on 8-bit RGBA: a scanline flood fill (or a global match), anti-aliased only at the
       edges inside the touched box. Fully transparent pixels match each other whatever RGB they
       hide. False when nothing is selected or the seed is outside `area`. */
    bool wand_region (const std::vector<uint8_t>& img, Rect area, int32_t sx, int32_t sy, float tolerance,
        bool contiguous, bool anti_alias, Region* out)
    {
        if (sx < area.x0 || sy < area.y0 || sx >= area.x1 || sy >= area.y1)
        {
            return false;
        }
        size_t w = area.width (), h = area.height ();
        const uint8_t* target = &img[((size_t) (sy - area.y0) * w + (size_t) (sx - area.x0)) * 4];
        int tol               = (int) floorf (tolerance + 1e-3f);
        if (tol < 0)
        {
            tol = 0;
        }
        uint8_t t[4] = {target[0], target[1], target[2], target[3]};
        std::vector<uint8_t> marks (w * h, 0);
        size_t bx0 = SIZE_MAX, by0 = SIZE_MAX, bx1 = 0, by1 = 0;
        if (contiguous)
        {
            std::vector<std::pair<size_t, size_t>> stack;
            stack.push_back (std::make_pair ((size_t) (sx - area.x0), (size_t) (sy - area.y0)));
            while (!stack.empty ())
            {
                size_t x = stack.back ().first, y = stack.back ().second;
                stack.pop_back ();
                size_t row = y * w;
                if (marks[row + x] != 0)
                {
                    continue;
                }
                size_t l = x, r = x;
                while (l > 0 && marks[row + l - 1] == 0 && similar (t, tol, &img[(row + l - 1) * 4]))
                {
                    l--;
                }
                while (r + 1 < w && marks[row + r + 1] == 0 && similar (t, tol, &img[(row + r + 1) * 4]))
                {
                    r++;
                }
                memset (&marks[row + l], 255, r - l + 1);
                bx0 = std::min (bx0, l);
                bx1 = std::max (bx1, r);
                by0 = std::min (by0, y);
                by1 = std::max (by1, y);
                for (int side = 0; side < 2; side++)
                {
                    if (side == 0 && y == 0)
                    {
                        continue;
                    }
                    size_t ny = side == 0 ? y - 1 : y + 1;
                    if (ny >= h)
                    {
                        continue;
                    }
                    size_t nrow = ny * w;
                    bool in_run = false;
                    for (size_t nx = l; nx <= r; nx++)
                    {
                        bool ok = marks[nrow + nx] == 0 && similar (t, tol, &img[(nrow + nx) * 4]);
                        if (ok && !in_run)
                        {
                            stack.push_back (std::make_pair (nx, ny));
                        }
                        in_run = ok;
                    }
                }
            }
        }
        else
        {
            for (size_t y = 0; y < h; y++)
            {
                for (size_t x = 0; x < w; x++)
                {
                    if (!similar (t, tol, &img[(y * w + x) * 4]))
                    {
                        continue;
                    }
                    marks[y * w + x] = 255;
                    bx0              = std::min (bx0, x);
                    bx1              = std::max (bx1, x);
                    by0              = std::min (by0, y);
                    by1              = std::max (by1, y);
                }
            }
        }
        if (bx0 == SIZE_MAX)
        {
            return false;
        }
        size_t pad = anti_alias ? 1 : 0;
        size_t x0 = bx0 >= pad ? bx0 - pad : 0, y0 = by0 >= pad ? by0 - pad : 0;
        size_t x1 = std::min (bx1 + 1 + pad, w), y1 = std::min (by1 + 1 + pad, h);
        size_t bw = x1 - x0;
        out->mask.assign (bw * (y1 - y0), 0);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yy = 0; yy < (ptrdiff_t) (y1 - y0); yy++)
        {
            size_t y = y0 + (size_t) yy;
            for (size_t xx = 0; xx < bw; xx++)
            {
                size_t x  = x0 + xx;
                uint8_t v = marks[y * w + x];
                uint8_t o = v;
                if (anti_alias)
                {
                    /* Edge pixels blend with their 3x3 neighbourhood (outside the area counts as
                       the pixel itself). */
                    unsigned sum = 0;
                    bool edge    = false;
                    for (int dy = -1; dy <= 1; dy++)
                    {
                        for (int dx = -1; dx <= 1; dx++)
                        {
                            long nx = (long) x + dx, ny = (long) y + dy;
                            uint8_t nv = nx < 0 || ny < 0 || nx >= (long) w || ny >= (long) h
                                           ? v
                                           : marks[(size_t) ny * w + (size_t) nx];
                            edge       = edge || nv != v;
                            sum += nv;
                        }
                    }
                    if (edge)
                    {
                        float avg = (float) sum / (9.0f * 255.0f);
                        float f   = v > 127 ? 0.5f + 0.5f * avg : 0.5f * avg;
                        o         = (uint8_t) roundf (f * 255.0f);
                    }
                }
                out->mask[(size_t) yy * bw + xx] = o;
            }
        }
        Rect b    = {area.x0 + (int32_t) x0, area.y0 + (int32_t) y0, area.x0 + (int32_t) x1, area.y0 + (int32_t) y1};
        out->bbox = b;
        return true;
    }

    raster::Surface region_surface (const Region& r)
    {
        raster::Surface out (color::PixelFormat::gray8 ());
        if (r.bbox.is_empty ())
        {
            return out;
        }
        out.write_interleaved (r.bbox, r.mask.data (), r.mask.size ());
        out.prune ();
        return out;
    }

} // namespace algo
