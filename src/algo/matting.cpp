#include "algo/matting.h"

#include <algorithm>
#include <math.h>
#include <string.h>

namespace algo
{

    /* Select and Mask (Refine Edge) and colour decontamination.
       - Soft edges come from the guided filter (He, Sun and Tang, "Guided Image Filtering", ECCV
         2010): filtering the binary selection with the image as guide fits a local linear model
         alpha = a.I + b in every window, so the mask takes on the image's soft transitions (hair,
         blur) inside a band of `radius` pixels around the selection edge. Smart Radius adapts the
         band per pixel to the transition width seen in the image.
       - Then, in Photoshop's order: Smooth, Feather (Gaussian, sigma = feather / 2), Contrast and
         Shift Edge (grey dilation or erosion).
       - The Refine Edge Brush adds areas where coverage is estimated from the image (between the
         average nearby foreground and background colours), or restores the original edge.
       - Decontaminate Colors moves fringe colours toward the average of nearby fully selected
         pixels.
       Work is tiled: only tiles near the selection's edge are processed (in parallel), so the cost
       follows the edge's length rather than the selection's area. */

    using geom::Rect;

    const float MAT_EPS    = 1e-4f;
    const int32_t MAT_TILE = 256;

    /* The working image: RGB triplets in 0..1. */
    struct MatImage
    {
        size_t w;
        size_t h;
        std::vector<float> px;
    };

    RefineParams refine_defaults ()
    {
        RefineParams p = {0.0f, false, 0.0f, 0.0f, 0.0f, 0.0f};
        return p;
    }

    size_t gf_radius (const RefineParams& p)
    {
        return (size_t) fminf (fmaxf (ceilf (p.radius), 1.0f), 64.0f);
    }

    float smooth_sigma (const RefineParams& p)
    {
        return fminf (fmaxf (p.smooth, 0.0f), 100.0f) / 100.0f * 4.0f;
    }

    float feather_sigma (const RefineParams& p)
    {
        return fmaxf (p.feather, 0.0f) / 2.0f;
    }

    /* The signed shift in pixels: 100% moves the edge by max(radius, 4), at most 64. */
    float shift_amount (const RefineParams& p)
    {
        float px = fminf (fmaxf (p.shift_edge, -100.0f), 100.0f) / 100.0f * fmaxf (p.radius, 4.0f);
        return isfinite (px) ? fminf (fmaxf (px, -64.0f), 64.0f) : 0.0f;
    }

    int32_t shift_px (const RefineParams& p)
    {
        float px    = shift_amount (p);
        float reach = ceilf (fabsf (px));
        return (int32_t) (px < 0.0f ? -reach : reach);
    }

    bool uses_image (const RefineParams& p)
    {
        return p.radius > 0.0f;
    }

    /* Pixels read around an output pixel. */
    int32_t refine_halo (const RefineParams& p)
    {
        int32_t edge = 0;
        if (uses_image (p))
        {
            int32_t g = (int32_t) gf_radius (p);
            edge      = std::max ((int32_t) ceilf (p.radius), 2 * g + (p.smart_radius ? g + 1 : 0));
        }
        return edge + (int32_t) ceilf (3.0f * smooth_sigma (p)) + (int32_t) ceilf (3.0f * feather_sigma (p))
             + abs (shift_px (p)) + 2;
    }

    /* How far the refined mask can reach beyond the original. */
    int32_t refine_spread (const RefineParams& p)
    {
        return (uses_image (p) ? (int32_t) ceilf (p.radius) : 0) + (int32_t) ceilf (3.0f * smooth_sigma (p))
             + (int32_t) ceilf (3.0f * feather_sigma (p)) + std::max (shift_px (p), 0) + 1;
    }

    /* Box mean with radius r (the window clipped to the image, divided by its pixel count). */
    std::vector<float> box_mean (const std::vector<float>& src, size_t w, size_t h, size_t r)
    {
        std::vector<float> tmp (w * h), out (w * h);
        std::vector<double> pre (std::max (w, h) + 1);
        for (size_t y = 0; y < h; y++)
        {
            pre[0] = 0.0;
            for (size_t x = 0; x < w; x++)
            {
                pre[x + 1] = pre[x] + src[y * w + x];
            }
            for (size_t x = 0; x < w; x++)
            {
                size_t lo = x >= r ? x - r : 0, hi = std::min (x + r + 1, w);
                tmp[y * w + x] = (float) ((pre[hi] - pre[lo]) / (double) (hi - lo));
            }
        }
        for (size_t x = 0; x < w; x++)
        {
            pre[0] = 0.0;
            for (size_t y = 0; y < h; y++)
            {
                pre[y + 1] = pre[y] + tmp[y * w + x];
            }
            for (size_t y = 0; y < h; y++)
            {
                size_t lo = y >= r ? y - r : 0, hi = std::min (y + r + 1, h);
                out[y * w + x] = (float) ((pre[hi] - pre[lo]) / (double) (hi - lo));
            }
        }
        return out;
    }

    /* The guided filter with a colour guide (He et al., eqs. 19-21: a = (Sigma + eps U)^-1 cov(I, p)). */
    std::vector<float> guided_filter_color (const MatImage& img, const std::vector<float>& p, size_t r, float eps)
    {
        size_t w = img.w, h = img.h, n = w * h;
        std::vector<float> ch[3];
        for (int c = 0; c < 3; c++)
        {
            ch[c].resize (n);
            for (size_t i = 0; i < n; i++)
            {
                ch[c][i] = img.px[i * 3 + (size_t) c];
            }
        }
        std::vector<float> m[3], mip[3], mii[6];
        for (int c = 0; c < 3; c++)
        {
            m[c] = box_mean (ch[c], w, h, r);
        }
        std::vector<float> mp = box_mean (p, w, h, r);
        std::vector<float> tmp (n);
        for (int c = 0; c < 3; c++)
        {
            for (size_t i = 0; i < n; i++)
            {
                tmp[i] = ch[c][i] * p[i];
            }
            mip[c] = box_mean (tmp, w, h, r);
        }
        const int PAIRS[6][2] = {{0, 0}, {0, 1}, {0, 2}, {1, 1}, {1, 2}, {2, 2}};
        for (int k = 0; k < 6; k++)
        {
            for (size_t i = 0; i < n; i++)
            {
                tmp[i] = ch[PAIRS[k][0]][i] * ch[PAIRS[k][1]][i];
            }
            mii[k] = box_mean (tmp, w, h, r);
        }
        std::vector<float> av[3], bv (n);
        for (int c = 0; c < 3; c++)
        {
            av[c].resize (n);
        }
        for (size_t i = 0; i < n; i++)
        {
            double mu[3] = {m[0][i], m[1][i], m[2][i]}, e = eps;
            double rr = mii[0][i] - mu[0] * mu[0] + e, rg = mii[1][i] - mu[0] * mu[1], rb = mii[2][i] - mu[0] * mu[2];
            double gg = mii[3][i] - mu[1] * mu[1] + e, gb = mii[4][i] - mu[1] * mu[2],
                   bb     = mii[5][i] - mu[2] * mu[2] + e;
            double cov[3] = {mip[0][i] - mu[0] * mp[i], mip[1][i] - mu[1] * mp[i], mip[2][i] - mu[2] * mp[i]};
            double i00 = gg * bb - gb * gb, i01 = gb * rb - rg * bb, i02 = rg * gb - gg * rb;
            double i11 = rr * bb - rb * rb, i12 = rb * rg - rr * gb, i22 = rr * gg - rg * rg;
            double det = rr * i00 + rg * i01 + rb * i02;
            double a0 = 0, a1 = 0, a2 = 0;
            if (fabs (det) >= 1e-30)
            {
                a0 = (i00 * cov[0] + i01 * cov[1] + i02 * cov[2]) / det;
                a1 = (i01 * cov[0] + i11 * cov[1] + i12 * cov[2]) / det;
                a2 = (i02 * cov[0] + i12 * cov[1] + i22 * cov[2]) / det;
            }
            av[0][i] = (float) a0;
            av[1][i] = (float) a1;
            av[2][i] = (float) a2;
            bv[i]    = (float) (mp[i] - a0 * mu[0] - a1 * mu[1] - a2 * mu[2]);
        }
        std::vector<float> ma[3];
        for (int c = 0; c < 3; c++)
        {
            ma[c] = box_mean (av[c], w, h, r);
        }
        std::vector<float> mb = box_mean (bv, w, h, r);
        std::vector<float> out (n);
        for (size_t i = 0; i < n; i++)
        {
            out[i] = ma[0][i] * ch[0][i] + ma[1][i] * ch[1][i] + ma[2][i] * ch[2][i] + mb[i];
        }
        return out;
    }

    /* A separable Gaussian blur, edges clamped. */
    std::vector<float> mat_gauss (const std::vector<float>& src, size_t w, size_t h, float sigma)
    {
        if (sigma < 0.1f)
        {
            return src;
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
        std::vector<float> tmp (w * h), out (w * h);
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                float a = 0.0f;
                for (long i = -r; i <= r; i++)
                {
                    a += src[y * w + (size_t) std::min (std::max ((long) x + i, 0L), (long) w - 1)]
                       * k[(size_t) (i + r)];
                }
                tmp[y * w + x] = a;
            }
        }
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                float a = 0.0f;
                for (long i = -r; i <= r; i++)
                {
                    a += tmp[(size_t) std::min (std::max ((long) y + i, 0L), (long) h - 1) * w + x]
                       * k[(size_t) (i + r)];
                }
                out[y * w + x] = a;
            }
        }
        return out;
    }

    /* Grey dilation (grow) or erosion by r pixels: r steps alternating a 3 x 3 square and a cross,
       which approximates a disc (an octagon). */
    std::vector<float> mat_morph (const std::vector<float>& src, size_t w, size_t h, size_t r, bool grow)
    {
        std::vector<float> a = src, b (src.size ());
        for (size_t step = 0; step < r; step++)
        {
            bool square = step % 2 == 0;
            for (size_t y = 0; y < h; y++)
            {
                for (size_t x = 0; x < w; x++)
                {
                    float v = a[y * w + x];
                    for (int dy = -1; dy <= 1; dy++)
                    {
                        for (int dx = -1; dx <= 1; dx++)
                        {
                            if (!square && dx != 0 && dy != 0)
                            {
                                continue;
                            }
                            long nx = (long) x + dx, ny = (long) y + dy;
                            if (nx < 0 || ny < 0 || nx >= (long) w || ny >= (long) h)
                            {
                                continue;
                            }
                            float u = a[(size_t) ny * w + (size_t) nx];
                            v       = grow ? fmaxf (v, u) : fminf (v, u);
                        }
                    }
                    b[y * w + x] = v;
                }
            }
            a.swap (b);
        }
        return a;
    }

    /* Running max (or min) over a window of radius r, edge-clipped (van Herk / Gil-Werman: three
       comparisons per sample whatever the radius). */
    void running_extreme (const float* src, size_t n, float* out, size_t r, bool max)
    {
        size_t k  = 2 * r + 1;
        float pad = max ? -INFINITY : INFINITY;
        size_t m  = (n + 2 * r + k - 1) / k * k;
        std::vector<float> g (m), hb (m);
        for (size_t j = 0; j < m; j++)
        {
            float pj = j >= r && j < r + n ? src[j - r] : pad;
            g[j]     = j % k == 0 ? pj : max ? fmaxf (g[j - 1], pj) : fminf (g[j - 1], pj);
        }
        for (size_t jj = m; jj-- > 0;)
        {
            float pj = jj >= r && jj < r + n ? src[jj - r] : pad;
            hb[jj]   = jj % k == k - 1 || jj == m - 1 ? pj : max ? fmaxf (hb[jj + 1], pj) : fminf (hb[jj + 1], pj);
        }
        for (size_t i = 0; i < n; i++)
        {
            out[i] = max ? fmaxf (hb[i], g[i + 2 * r]) : fminf (hb[i], g[i + 2 * r]);
        }
    }

    std::vector<float> max_square (const std::vector<float>& src, size_t w, size_t h, size_t r, bool max)
    {
        std::vector<float> tmp (w * h), out (w * h), col (h), res (h);
        for (size_t y = 0; y < h; y++)
        {
            running_extreme (&src[y * w], w, &tmp[y * w], std::min (r, w - 1), max);
        }
        for (size_t x = 0; x < w; x++)
        {
            for (size_t y = 0; y < h; y++)
            {
                col[y] = tmp[y * w + x];
            }
            running_extreme (col.data (), h, res.data (), std::min (r, h - 1), max);
            for (size_t y = 0; y < h; y++)
            {
                out[y * w + x] = res[y];
            }
        }
        return out;
    }

    /* Each pixel's transition width (pixels) in the guide's luminance: the local range over the
       local steepest gradient within radius r (a sharp step gives about 2, a ramp L wide about L). */
    std::vector<float> edge_width (const MatImage& img, size_t r)
    {
        size_t w = img.w, h = img.h;
        std::vector<float> y (w * h), g (w * h);
        for (size_t i = 0; i < w * h; i++)
        {
            y[i] = 0.299f * img.px[i * 3] + 0.587f * img.px[i * 3 + 1] + 0.114f * img.px[i * 3 + 2];
        }
        for (size_t yy = 0; yy < h; yy++)
        {
            for (size_t xx = 0; xx < w; xx++)
            {
                float gx       = (y[yy * w + std::min (xx + 1, w - 1)] - y[yy * w + (xx > 0 ? xx - 1 : 0)]) / 2.0f;
                float gy       = (y[std::min (yy + 1, h - 1) * w + xx] - y[(yy > 0 ? yy - 1 : 0) * w + xx]) / 2.0f;
                g[yy * w + xx] = sqrtf (gx * gx + gy * gy);
            }
        }
        std::vector<float> gm = max_square (g, w, h, r, true), hi = max_square (y, w, h, r, true),
                           lo = max_square (y, w, h, r, false);
        std::vector<float> out (w * h);
        for (size_t i = 0; i < w * h; i++)
        {
            out[i] = (hi[i] - lo[i]) / fmaxf (gm[i], 1e-4f);
        }
        return out;
    }

    /* ---- The Refine Edge Brush ---- */

    /* Coverage of a capsule (segment a-b, radius r) at p, anti-aliased by a pixel. */
    float capsule (float px, float py, float ax, float ay, float bx, float by, float r)
    {
        float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        float t  = len2 > 0.0f ? fminf (fmaxf (((px - ax) * dx + (py - ay) * dy) / len2, 0.0f), 1.0f) : 0.0f;
        float cx = ax + t * dx, cy = ay + t * dy;
        float d = sqrtf ((px - cx) * (px - cx) + (py - cy) * (py - cy));
        return fminf (fmaxf (r + 0.5f - d, 0.0f), 1.0f);
    }

    Rect dab_bounds (float x, float y, float r)
    {
        Rect b = {(int32_t) floorf (x - r - 1.0f), (int32_t) floorf (y - r - 1.0f), (int32_t) ceilf (x + r + 1.0f),
            (int32_t) ceilf (y + r + 1.0f)};
        return b;
    }

    /* The brush strokes painted into signed coverage: +1 adds (coverage from the image), -1
       restores the original edge; later strokes paint over earlier ones. False when nothing is
       painted on the canvas. */
    bool edge_brush_make (const std::vector<EdgeStroke>& strokes, Rect canvas, EdgeBrush* out)
    {
        Rect bbox;
        for (size_t s = 0; s < strokes.size (); s++)
        {
            float r = fminf (fmaxf (strokes[s].size, EDGE_BRUSH_MIN), EDGE_BRUSH_MAX) / 2.0f;
            for (size_t i = 0; i + 1 < strokes[s].points.size (); i += 2)
            {
                bbox = bbox.unite (dab_bounds (strokes[s].points[i], strokes[s].points[i + 1], r));
            }
        }
        bbox = bbox.intersect (canvas);
        if (bbox.is_empty ())
        {
            return false;
        }
        size_t bw = bbox.width ();
        out->bbox = bbox;
        out->v.assign (bw * bbox.height (), 0);
        float max_add = 0.0f;
        for (size_t s = 0; s < strokes.size (); s++)
        {
            const EdgeStroke& st = strokes[s];
            float r              = fminf (fmaxf (st.size, EDGE_BRUSH_MIN), EDGE_BRUSH_MAX) / 2.0f;
            if (st.add)
            {
                max_add = fmaxf (max_add, 2.0f * r);
            }
            size_t np = st.points.size () / 2;
            if (np == 0)
            {
                continue;
            }
            Rect sb;
            for (size_t i = 0; i < np; i++)
            {
                sb = sb.unite (dab_bounds (st.points[i * 2], st.points[i * 2 + 1], r));
            }
            sb = sb.intersect (bbox);
            if (sb.is_empty ())
            {
                continue;
            }
            size_t sw = sb.width ();
            std::vector<float> cov (sw * sb.height (), 0.0f);
            for (size_t i = 0; i < np; i++)
            {
                float ax = st.points[i * 2], ay = st.points[i * 2 + 1];
                float bx = i + 1 < np ? st.points[(i + 1) * 2] : ax, by = i + 1 < np ? st.points[(i + 1) * 2 + 1] : ay;
                if (np > 1 && i + 1 == np)
                {
                    break;
                }
                Rect seg = dab_bounds (ax, ay, r).unite (dab_bounds (bx, by, r)).intersect (sb);
                for (int32_t y = seg.y0; y < seg.y1; y++)
                {
                    for (int32_t x = seg.x0; x < seg.x1; x++)
                    {
                        float c     = capsule ((float) x + 0.5f, (float) y + 0.5f, ax, ay, bx, by, r);
                        float& slot = cov[(size_t) (y - sb.y0) * sw + (size_t) (x - sb.x0)];
                        if (c > slot)
                        {
                            slot = c;
                        }
                    }
                }
            }
            float target = st.add ? 1.0f : -1.0f;
            for (int32_t y = sb.y0; y < sb.y1; y++)
            {
                for (int32_t x = sb.x0; x < sb.x1; x++)
                {
                    float c = cov[(size_t) (y - sb.y0) * sw + (size_t) (x - sb.x0)];
                    if (c <= 0.0f)
                    {
                        continue;
                    }
                    int8_t& slot = out->v[(size_t) (y - bbox.y0) * bw + (size_t) (x - bbox.x0)];
                    float cur    = (float) slot / 127.0f;
                    slot = (int8_t) fminf (fmaxf (roundf ((cur + (target - cur) * c) * 127.0f), -127.0f), 127.0f);
                }
            }
        }
        Rect added;
        bool any = false;
        for (size_t row = 0; row < bbox.height (); row++)
        {
            for (size_t x = 0; x < bw; x++)
            {
                int8_t c = out->v[row * bw + x];
                any      = any || c != 0;
                if (c > 0)
                {
                    added = added.unite (Rect{bbox.x0 + (int32_t) x, bbox.y0 + (int32_t) row, bbox.x0 + (int32_t) x + 1,
                        bbox.y0 + (int32_t) row + 1});
                }
            }
        }
        out->added  = added;
        out->window = (size_t) fminf (fmaxf (ceilf (max_add), 4.0f), 64.0f);
        return any;
    }

    float EdgeBrush::at (int32_t x, int32_t y) const
    {
        if (!bbox.contains (x, y))
        {
            return 0.0f;
        }
        return (float) v[(size_t) (y - bbox.y0) * bbox.width () + (size_t) (x - bbox.x0)] / 127.0f;
    }

    /* Where the brush adds: each pixel's coverage from its colour between the average nearby fully
       selected and fully unselected colours (those the brush didn't paint), within `window`
       pixels, or four times that where none is that near. */
    std::vector<float> brush_estimate (
        const MatImage& img, const std::vector<float>& m, const std::vector<float>& brush, size_t window)
    {
        size_t w = img.w, h = img.h, n = w * h;
        std::vector<float> known[2];
        for (int k = 0; k < 2; k++)
        {
            known[k].resize (n);
            for (size_t i = 0; i < n; i++)
            {
                known[k][i] = brush[i] <= 0.0f && (k == 0 ? m[i] >= 0.98f : m[i] <= 0.02f) ? 1.0f : 0.0f;
            }
        }
        /* [side][scale]: the weight's mean and each colour's weighted mean. */
        std::vector<float> den[2][2], sums[2][2][3];
        std::vector<float> tmp (n);
        for (int k = 0; k < 2; k++)
        {
            for (int sc = 0; sc < 2; sc++)
            {
                size_t r   = sc == 0 ? window : window * 4;
                den[k][sc] = box_mean (known[k], w, h, r);
                for (int c = 0; c < 3; c++)
                {
                    for (size_t i = 0; i < n; i++)
                    {
                        tmp[i] = img.px[i * 3 + (size_t) c] * known[k][i];
                    }
                    sums[k][sc][c] = box_mean (tmp, w, h, r);
                }
            }
        }
        std::vector<float> out = m;
        for (size_t i = 0; i < n; i++)
        {
            if (brush[i] <= 0.0f)
            {
                continue;
            }
            float mean[2][3];
            bool found[2] = {false, false};
            for (int k = 0; k < 2; k++)
            {
                for (int sc = 0; sc < 2 && !found[k]; sc++)
                {
                    float d = den[k][sc][i];
                    if (d < 1e-4f)
                    {
                        continue;
                    }
                    for (int c = 0; c < 3; c++)
                    {
                        mean[k][c] = sums[k][sc][c][i] / d;
                    }
                    found[k] = true;
                }
            }
            if (!found[0] || !found[1])
            {
                continue;
            }
            float num = 0.0f, dd = 0.0f;
            for (int c = 0; c < 3; c++)
            {
                float f = mean[0][c], b = mean[1][c], p = img.px[i * 3 + (size_t) c];
                num += (p - b) * (f - b);
                dd += (f - b) * (f - b);
            }
            if (dd > 1e-4f)
            {
                out[i] = fminf (fmaxf (num / dd, 0.0f), 1.0f);
            }
        }
        return out;
    }

    /* ---- Refinement ---- */

    /* Grows or shrinks by a fractional distance: whole pixels by morphology, the remainder blending
       with the next pixel's result. */
    std::vector<float> shift_mask (const std::vector<float>& src, size_t w, size_t h, float amount)
    {
        float mag = fabsf (amount);
        if (!isfinite (mag) || mag < 1e-4f)
        {
            return src;
        }
        bool grow    = amount > 0.0f;
        size_t whole = (size_t) floorf (mag);
        float frac   = mag - (float) whole;
        if (frac < 1e-4f)
        {
            return mat_morph (src, w, h, whole, grow);
        }
        std::vector<float> lo = whole == 0 ? src : mat_morph (src, w, h, whole, grow);
        std::vector<float> hi = mat_morph (src, w, h, whole + 1, grow);
        for (size_t i = 0; i < lo.size (); i++)
        {
            lo[i] += (hi[i] - lo[i]) * frac;
        }
        return lo;
    }

    /* One buffer refined: `img` is the guide (NULL when not needed), `brush` the signed brush
       coverage (empty for none). */
    std::vector<float> refine_buffer (const MatImage* img, const std::vector<float>& m, size_t w, size_t h,
        const RefineParams& p, const std::vector<float>& brush, size_t window)
    {
        std::vector<float> a = m;
        size_t n             = w * h;
        if (uses_image (p) && img != NULL)
        {
            std::vector<bool> boundary (n, false);
            bool any = false;
            for (size_t y = 0; y < h; y++)
            {
                for (size_t x = 0; x < w; x++)
                {
                    size_t i  = y * w + x;
                    bool v    = m[i] >= 0.5f;
                    bool diff = (x > 0 && (m[i - 1] >= 0.5f) != v) || (x + 1 < w && (m[i + 1] >= 0.5f) != v)
                             || (y > 0 && (m[i - w] >= 0.5f) != v) || (y + 1 < h && (m[i + w] >= 0.5f) != v);
                    boundary[i] = diff;
                    any         = any || diff;
                }
            }
            if (any)
            {
                std::vector<float> dist = edt (boundary, w, h);
                size_t r                = gf_radius (p);
                std::vector<float> q    = guided_filter_color (*img, m, r, MAT_EPS);
                std::vector<float> widths;
                if (p.smart_radius)
                {
                    widths = edge_width (*img, r);
                }
                for (size_t i = 0; i < n; i++)
                {
                    float reff = p.smart_radius ? fminf (fmaxf (widths[i], 1.5f), fmaxf (p.radius, 1.5f)) : p.radius;
                    float wgt  = fminf (fmaxf (reff + 1.0f - dist[i], 0.0f), 1.0f);
                    /* Restored by the brush: the original edge. */
                    if (!brush.empty () && brush[i] < 0.0f)
                    {
                        wgt *= 1.0f + brush[i];
                    }
                    if (wgt > 0.0f)
                    {
                        a[i] = m[i] + (fminf (fmaxf (q[i], 0.0f), 1.0f) - m[i]) * wgt;
                    }
                }
            }
        }
        /* Added by the brush: coverage estimated from the image. */
        bool adds = false;
        for (size_t i = 0; i < brush.size () && !adds; i++)
        {
            adds = brush[i] > 0.0f;
        }
        if (adds && img != NULL)
        {
            std::vector<float> est = brush_estimate (*img, m, brush, window);
            for (size_t i = 0; i < n; i++)
            {
                if (brush[i] > 0.0f)
                {
                    a[i] += (est[i] - a[i]) * brush[i];
                }
            }
        }
        float ss = smooth_sigma (p);
        if (ss >= 0.3f)
        {
            a          = mat_gauss (a, w, h, ss);
            float gain = 1.0f + ss;
            for (size_t i = 0; i < n; i++)
            {
                a[i] = fminf (fmaxf ((a[i] - 0.5f) * gain + 0.5f, 0.0f), 1.0f);
            }
        }
        a = mat_gauss (a, w, h, feather_sigma (p));
        if (p.contrast > 0.0f)
        {
            float gain = 1.0f / (1.0f - 0.99f * fminf (fmaxf (p.contrast, 0.0f), 100.0f) / 100.0f);
            for (size_t i = 0; i < n; i++)
            {
                a[i] = fminf (fmaxf ((a[i] - 0.5f) * gain + 0.5f, 0.0f), 1.0f);
            }
        }
        float amount = shift_amount (p);
        if (fabsf (amount) >= 1e-4f)
        {
            a = shift_mask (a, w, h, amount);
        }
        return a;
    }

    /* The selection `mask` refined over the canvas: tile by tile around its content (and wherever
       the brush adds), each tile read with a halo so its filters see past its sides. False when
       nothing stays selected. */
    bool refine_mask (const RgbaSource& src, const raster::Surface& mask, Rect canvas, const RefineParams& p,
        const EdgeBrush* brush, Region* out)
    {
        Rect content = mask.content_bounds ().intersect (canvas);
        if (content.is_empty ())
        {
            return false;
        }
        Rect bbox = content.inflate (refine_spread (p));
        if (brush != NULL)
        {
            bbox = bbox.unite (brush->added);
        }
        bbox         = bbox.intersect (canvas);
        int32_t halo = refine_halo (p) + (brush != NULL ? (int32_t) (brush->window * 4) : 0);
        std::vector<Rect> tiles;
        for (int32_t y = bbox.y0; y < bbox.y1; y += MAT_TILE)
        {
            for (int32_t x = bbox.x0; x < bbox.x1; x += MAT_TILE)
            {
                tiles.push_back (Rect{x, y, std::min (x + MAT_TILE, bbox.x1), std::min (y + MAT_TILE, bbox.y1)});
            }
        }
        size_t bw = bbox.width ();
        out->bbox = bbox;
        out->mask.assign (bw * bbox.height (), 0);
#pragma omp parallel for schedule(dynamic, 1)
        for (ptrdiff_t ti = 0; ti < (ptrdiff_t) tiles.size (); ti++)
        {
            Rect t    = tiles[(size_t) ti];
            Rect r    = t.inflate (halo).intersect (canvas);
            size_t rw = r.width (), rh = r.height ();
            std::vector<float> m = mask_from_surface (&mask, r);
            std::vector<float> painted;
            bool adds = false;
            if (brush != NULL && !brush->bbox.intersect (r).is_empty ())
            {
                painted.resize (rw * rh);
                for (size_t yy = 0; yy < rh; yy++)
                {
                    for (size_t xx = 0; xx < rw; xx++)
                    {
                        float b               = brush->at (r.x0 + (int32_t) xx, r.y0 + (int32_t) yy);
                        painted[yy * rw + xx] = b;
                        adds                  = adds || b > 0.0f;
                    }
                }
            }
            /* A uniformly empty or full tile with nothing added stays as it is. */
            float first  = m.empty () ? 0.0f : m[0];
            bool uniform = !adds && (first == 0.0f || first == 1.0f);
            for (size_t i = 1; uniform && i < m.size (); i++)
            {
                uniform = m[i] == first;
            }
            std::vector<float> res;
            if (uniform)
            {
                res = m;
            }
            else
            {
                MatImage img;
                bool need = uses_image (p) || adds;
                if (need)
                {
                    std::vector<float> rgba (rw * rh * 4);
                    src.read (src.user, r, rgba.data ());
                    img.w = rw;
                    img.h = rh;
                    img.px.resize (rw * rh * 3);
                    for (size_t i = 0; i < rw * rh; i++)
                    {
                        for (int c = 0; c < 3; c++)
                        {
                            img.px[i * 3 + (size_t) c] = fminf (fmaxf (rgba[i * 4 + (size_t) c], 0.0f), 1.0f);
                        }
                    }
                }
                res = refine_buffer (need ? &img : NULL, m, rw, rh, p, painted, brush != NULL ? brush->window : 4);
            }
            size_t cx = (size_t) (t.x0 - r.x0), cy = (size_t) (t.y0 - r.y0), cw = t.width ();
            for (size_t yy = 0; yy < t.height (); yy++)
            {
                uint8_t* row = &out->mask[((size_t) (t.y0 - bbox.y0) + yy) * bw + (size_t) (t.x0 - bbox.x0)];
                for (size_t xx = 0; xx < cw; xx++)
                {
                    row[xx] = (uint8_t) lroundf (fminf (fmaxf (res[(cy + yy) * rw + cx + xx], 0.0f), 1.0f) * 255.0f);
                }
            }
        }
        bool any = false;
        for (size_t i = 0; i < out->mask.size () && !any; i++)
        {
            any = out->mask[i] != 0;
        }
        return any;
    }

    /* ---- Output ---- */

    float region_at (const Region& r, int32_t x, int32_t y)
    {
        if (!r.bbox.contains (x, y))
        {
            return 0.0f;
        }
        return (float) r.mask[(size_t) (y - r.bbox.y0) * r.bbox.width () + (size_t) (x - r.bbox.x0)] / 255.0f;
    }

    /* Fringe colours (partly selected pixels of `alpha`) moved toward the average colour of nearby
       fully selected pixels (a box of radius max(radius, 3), four times wider where none is that
       near), by `amount` percent scaled by the fringe's transparency. In the surface's own
       channels. */
    raster::Surface decontaminate (const raster::Surface& src, const Region& alpha, float radius, float amount)
    {
        color::PixelFormat fmt = src.format ();
        size_t n = fmt.channels (), nc = n - (fmt.alpha ? 1 : 0);
        size_t rad     = (size_t) ceilf (fmaxf (radius, 3.0f));
        int32_t halo   = (int32_t) (rad * 4);
        float k_amount = fminf (fmaxf (amount, 0.0f), 100.0f) / 100.0f;
        std::vector<Rect> tiles;
        for (int32_t y = alpha.bbox.y0; y < alpha.bbox.y1; y += MAT_TILE)
        {
            for (int32_t x = alpha.bbox.x0; x < alpha.bbox.x1; x += MAT_TILE)
            {
                tiles.push_back (
                    Rect{x, y, std::min (x + MAT_TILE, alpha.bbox.x1), std::min (y + MAT_TILE, alpha.bbox.y1)});
            }
        }
        std::vector<std::vector<float>> results (tiles.size ());
#pragma omp parallel for schedule(dynamic, 1)
        for (ptrdiff_t ti = 0; ti < (ptrdiff_t) tiles.size (); ti++)
        {
            Rect t    = tiles[(size_t) ti];
            bool soft = false;
            for (int32_t y = t.y0; y < t.y1 && !soft; y++)
            {
                for (int32_t x = t.x0; x < t.x1 && !soft; x++)
                {
                    float a = region_at (alpha, x, y);
                    soft    = a > 0.02f && a < 0.98f;
                }
            }
            if (!soft)
            {
                continue;
            }
            Rect r    = t.inflate (halo);
            size_t rw = r.width (), rh = r.height ();
            std::vector<float> px;
            if (!src.read_region (r, &px))
            {
                continue;
            }
            std::vector<float> al (rw * rh), wts (rw * rh);
            for (size_t yy = 0; yy < rh; yy++)
            {
                for (size_t xx = 0; xx < rw; xx++)
                {
                    size_t i = yy * rw + xx;
                    al[i]    = region_at (alpha, r.x0 + (int32_t) xx, r.y0 + (int32_t) yy);
                    wts[i]   = al[i] >= 0.98f ? (fmt.alpha ? px[i * n + n - 1] : 1.0f) : 0.0f;
                }
            }
            std::vector<float> den = box_mean (wts, rw, rh, rad), den2 = box_mean (wts, rw, rh, rad * 4);
            std::vector<std::vector<float>> num (nc), num2 (nc);
            std::vector<float> v (rw * rh);
            for (size_t c = 0; c < nc; c++)
            {
                for (size_t i = 0; i < rw * rh; i++)
                {
                    v[i] = wts[i] * px[i * n + c];
                }
                num[c]  = box_mean (v, rw, rh, rad);
                num2[c] = box_mean (v, rw, rh, rad * 4);
            }
            size_t cx = (size_t) (t.x0 - r.x0), cy = (size_t) (t.y0 - r.y0);
            std::vector<float>& outp = results[(size_t) ti];
            outp.reserve (t.width () * t.height () * n);
            for (size_t yy = 0; yy < t.height (); yy++)
            {
                for (size_t xx = 0; xx < t.width (); xx++)
                {
                    size_t i = (cy + yy) * rw + cx + xx;
                    float a  = al[i];
                    float k  = a > 0.02f && a < 0.98f ? k_amount * fminf (fmaxf ((1.0f - a) * 4.0f, 0.0f), 1.0f) : 0.0f;
                    for (size_t c = 0; c < n; c++)
                    {
                        float val = px[i * n + c];
                        if (c >= nc || k == 0.0f)
                        {
                            outp.push_back (val);
                            continue;
                        }
                        float f = den[i] > 1e-4f ? num[c][i] / den[i] : den2[i] > 1e-4f ? num2[c][i] / den2[i] : val;
                        outp.push_back (val + (f - val) * k);
                    }
                }
            }
        }
        raster::Surface out = src;
        for (size_t ti = 0; ti < tiles.size (); ti++)
        {
            if (!results[ti].empty ())
            {
                out.write_region (tiles[ti], results[ti].data (), results[ti].size ());
            }
        }
        return out;
    }

    /* `src`'s pixels with their alpha multiplied by `alpha` (zero outside its box); the result has
       an alpha channel even if `src` has none. */
    raster::Surface masked_copy (const raster::Surface& src, const Region& alpha)
    {
        color::PixelFormat fmt = src.format ();
        color::PixelFormat of  = fmt;
        of.alpha               = true;
        size_t n = fmt.channels (), m = of.channels ();
        raster::Surface out (of);
        std::vector<Rect> tiles;
        for (int32_t y = alpha.bbox.y0; y < alpha.bbox.y1; y += MAT_TILE)
        {
            for (int32_t x = alpha.bbox.x0; x < alpha.bbox.x1; x += MAT_TILE)
            {
                tiles.push_back (
                    Rect{x, y, std::min (x + MAT_TILE, alpha.bbox.x1), std::min (y + MAT_TILE, alpha.bbox.y1)});
            }
        }
        for (size_t ti = 0; ti < tiles.size (); ti++)
        {
            Rect t = tiles[ti];
            std::vector<float> px;
            if (!src.read_region (t, &px))
            {
                continue;
            }
            std::vector<float> o;
            o.reserve (t.width () * t.height () * m);
            bool any = false;
            size_t i = 0;
            for (int32_t y = t.y0; y < t.y1; y++)
            {
                for (int32_t x = t.x0; x < t.x1; x++, i++)
                {
                    float a = region_at (alpha, x, y);
                    for (size_t c = 0; c + 1 < m; c++)
                    {
                        o.push_back (px[i * n + c]);
                    }
                    float sa = fmt.alpha ? px[i * n + n - 1] : 1.0f;
                    o.push_back (sa * a);
                    any = any || sa * a > 0.0f;
                }
            }
            if (any)
            {
                out.write_region (t, o.data (), o.size ());
            }
        }
        out.prune ();
        return out;
    }

} // namespace algo
