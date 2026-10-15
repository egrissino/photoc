#include "algo/quick_select.h"

#include "algo/maxflow.h"
#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

namespace algo
{

    /* The Quick Selection brush, as the reference fitted it to Photoshop 25.4 (measured as a black
       box):
       - The brush footprint is always selected, whatever colours it covers.
       - Beyond it the selection grows to image edges, but every selected pixel costs a little, so a
         click selects a whole object only when it is small enough for that brush; a click in a flat
         area selects about the brush disc.
       - A stroke is one set of seeds. Colours elsewhere in the image make no difference.
       - It works on the whole document up to 512 x 512 pixels and at half resolution above, aligned
         to the document's corner; the mask is then scaled up bilinearly.
       The model is one min cut (Boykov and Jolly, ICCV 2001) over the working image minimising
         sum over selected pixels p beyond the footprint of  AREA * (1 + DIST * d(p)^POW)
         + sum over cut neighbour pairs p~q of  w(|Ip - Iq|) / |p - q|,   w(D) = FLOOR + 1 / (1 + (D / SIGMA)^K)
       with the (slightly grown) footprint hard foreground and d the distance from it. */

    const float Q_AREA      = 0.004079f;
    const float Q_DIST      = 0.002651f;
    const float Q_POW       = 0.98885f;
    const float Q_FLOOR     = 0.018847f;
    const float Q_SIGMA     = 0.048450f;
    const float Q_K         = 1.98549f;
    const float Q_GROW      = 1.69371f;
    const float Q_GROW_FRAC = 0.103003f;
    /* Working windows above 4096 pixels are cut coarse to fine: at half size first, then again
       only in a band around that boundary (Lombaert, Sun, Grady and Xu, ICCV 2005). Each pixel
       carries its own small area cost, so a full-resolution cut does an augmentation for nearly
       every pixel the selection could reach; the pyramid keeps a click to tens of milliseconds
       (it was seconds at 300000) while matching the full cut along real edges. */
    const size_t Q_DIRECT_PX = 4096;
    const float Q_BAND       = 3.0f;

    const uint8_t FREE    = 0;
    const uint8_t HARD_FG = 1;
    const uint8_t HARD_BG = 2;

    struct RgbImage
    {
        size_t w;
        size_t h;
        std::vector<float> px;
    };

    /* Box-average downsampling by an integer step (output ceil(w/step) x ceil(h/step)). */
    RgbImage downsample (const RgbImage& img, size_t step)
    {
        RgbImage out;
        out.w = (img.w + step - 1) / step;
        out.h = (img.h + step - 1) / step;
        out.px.assign (out.w * out.h * 3, 0.0f);
        for (size_t oy = 0; oy < out.h; oy++)
        {
            for (size_t ox = 0; ox < out.w; ox++)
            {
                float acc[3] = {0, 0, 0};
                float n      = 0;
                for (size_t y = oy * step; y < (oy + 1) * step && y < img.h; y++)
                {
                    for (size_t x = ox * step; x < (ox + 1) * step && x < img.w; x++)
                    {
                        for (int c = 0; c < 3; c++)
                        {
                            acc[c] += img.px[(y * img.w + x) * 3 + c];
                        }
                        n += 1.0f;
                    }
                }
                for (int c = 0; c < 3; c++)
                {
                    out.px[(oy * out.w + ox) * 3 + c] = acc[c] / n;
                }
            }
        }
        return out;
    }

    /* RGB over r at 1/step resolution; transparent pixels count as mid gray, so transparency is a
       colour the model can separate. */
    RgbImage read_rgb (const RgbaSource& src, geom::Rect r, size_t step)
    {
        RgbImage full;
        full.w = r.width ();
        full.h = r.height ();
        std::vector<float> rgba (full.w * full.h * 4);
        src.read (src.user, r, rgba.data ());
        full.px.resize (full.w * full.h * 3);
        for (size_t i = 0; i < full.w * full.h; i++)
        {
            float a = fminf (fmaxf (rgba[i * 4 + 3], 0.0f), 1.0f);
            for (int c = 0; c < 3; c++)
            {
                full.px[i * 3 + c] = fminf (fmaxf (rgba[i * 4 + c], 0.0f), 1.0f) * a + 0.5f * (1.0f - a);
            }
        }
        return step <= 1 ? full : downsample (full, step);
    }

    float cut_weight (float d2, float scale)
    {
        return scale * (Q_FLOOR + 1.0f / (1.0f + powf (sqrtf (d2) / Q_SIGMA, Q_K)));
    }

    void add_fixed_link (MaxflowGraph* g, size_t node, uint8_t fixed, float wt)
    {
        /* Labelling `node` background cuts a link to a foreground pixel, and the other way round. */
        if (fixed == HARD_FG)
        {
            g->add_tweights (node, wt, 0.0f);
        }
        else
        {
            g->add_tweights (node, 0.0f, wt);
        }
    }

    /* One min cut over the grid (8-connected): free pixels pay cost_fg when selected; fixed ones
       keep their label. */
    std::vector<uint8_t> grid_cut (
        const RgbImage& img, const std::vector<float>& cost_fg, const std::vector<uint8_t>& fixed, float scale)
    {
        size_t w = img.w, h = img.h, n = w * h;
        std::vector<uint8_t> out (n);
        std::vector<uint32_t> idx (n, UINT32_MAX);
        uint32_t count = 0;
        for (size_t i = 0; i < n; i++)
        {
            out[i] = fixed[i] == HARD_FG;
            if (fixed[i] == FREE)
            {
                idx[i] = count++;
            }
        }
        if (count == 0)
        {
            return out;
        }
        MaxflowGraph g (count, (size_t) count * 4);
        for (size_t i = 0; i < n; i++)
        {
            if (idx[i] != UINT32_MAX)
            {
                g.add_tweights (idx[i], 0.0f, cost_fg[i]);
            }
        }
        const int nb[4][2] = {{1, 0}, {0, 1}, {1, 1}, {-1, 1}};
        const float len[4] = {1.0f, 1.0f, (float) M_SQRT2, (float) M_SQRT2};
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                size_t i       = y * w + x;
                const float* a = &img.px[i * 3];
                for (int k = 0; k < 4; k++)
                {
                    long nx = (long) x + nb[k][0], ny = (long) y + nb[k][1];
                    if (nx < 0 || nx >= (long) w || ny >= (long) h)
                    {
                        continue;
                    }
                    size_t j = (size_t) ny * w + (size_t) nx;
                    bool fi = idx[i] != UINT32_MAX, fj = idx[j] != UINT32_MAX;
                    if (!fi && !fj)
                    {
                        continue;
                    }
                    const float* b = &img.px[j * 3];
                    float d2 =
                        (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
                    float wt = cut_weight (d2, scale) / len[k];
                    if (fi && fj)
                    {
                        g.add_edge (idx[i], idx[j], wt, wt);
                    }
                    else if (fi)
                    {
                        add_fixed_link (&g, idx[i], fixed[j], wt);
                    }
                    else
                    {
                        add_fixed_link (&g, idx[j], fixed[i], wt);
                    }
                }
            }
        }
        g.maxflow ();
        for (size_t i = 0; i < n; i++)
        {
            if (idx[i] != UINT32_MAX)
            {
                out[i] = g.in_source (idx[i]);
            }
        }
        return out;
    }

    /* The cut, coarse to fine above Q_DIRECT_PX pixels: the half-size problem (colours averaged,
       pixel costs summed, cut weights doubled since a coarse side spans two fine ones) gives a
       boundary, and the full-size cut is redone only within Q_BAND of it. */
    std::vector<uint8_t> cut_levels (
        const RgbImage& img, const std::vector<uint8_t>& fixed, const std::vector<float>& cost_fg, float scale)
    {
        size_t w = img.w, h = img.h, n = w * h;
        if (n <= Q_DIRECT_PX || w < 8 || h < 8)
        {
            return grid_cut (img, cost_fg, fixed, scale);
        }
        size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
        RgbImage coarse_img = downsample (img, 2);
        /* A coarse cell is fixed when at least half its pixels are (fixed either way), so fixed
           areas neither swell nor vanish level by level; a footprint smaller than that still fixes
           the cells it touches when nothing else would. */
        std::vector<uint8_t> cfix (cw * ch, FREE), nfg (cw * ch, 0), nbg (cw * ch, 0), ncount (cw * ch, 0);
        std::vector<float> ccost (cw * ch, 0.0f);
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                size_t c = (y / 2) * cw + x / 2;
                ccost[c] += cost_fg[y * w + x];
                ncount[c]++;
                if (fixed[y * w + x] == HARD_FG)
                {
                    nfg[c]++;
                }
                if (fixed[y * w + x] == HARD_BG)
                {
                    nbg[c]++;
                }
            }
        }
        bool any_fg = false;
        for (size_t c = 0; c < cw * ch; c++)
        {
            if (nfg[c] * 2 >= ncount[c] && nfg[c] > 0)
            {
                cfix[c] = HARD_FG;
            }
            else if (nbg[c] * 2 >= ncount[c] && nbg[c] > 0)
            {
                cfix[c] = HARD_BG;
            }
            any_fg = any_fg || cfix[c] == HARD_FG;
        }
        for (size_t c = 0; !any_fg && c < cw * ch; c++)
        {
            if (nfg[c] > 0)
            {
                cfix[c] = HARD_FG;
            }
        }
        std::vector<uint8_t> coarse = cut_levels (coarse_img, cfix, ccost, scale * 2.0f);
        std::vector<uint8_t> proj (n);
        for (size_t i = 0; i < n; i++)
        {
            proj[i] = coarse[(i / w / 2) * cw + (i % w) / 2];
        }
        std::vector<bool> edge (n);
        bool any = false;
        for (size_t i = 0; i < n; i++)
        {
            size_t x = i % w, y = i / w;
            uint8_t v = proj[i];
            /* The coarse boundary, and fixed pixels the coarse cut labelled otherwise (a cell can
               be mostly free around them), are refined at full size. */
            bool disagrees = (fixed[i] == HARD_FG && !v) || (fixed[i] == HARD_BG && v);
            edge[i]        = disagrees || (x > 0 && proj[i - 1] != v) || (x + 1 < w && proj[i + 1] != v)
                   || (y > 0 && proj[i - w] != v) || (y + 1 < h && proj[i + w] != v);
            any = any || edge[i];
        }
        if (!any)
        {
            for (size_t i = 0; i < n; i++)
            {
                proj[i] = fixed[i] == HARD_FG ? 1 : fixed[i] == HARD_BG ? 0 : proj[i];
            }
            return proj;
        }
        std::vector<float> near = edt (edge, w, h);
        std::vector<uint8_t> banded (n);
        for (size_t i = 0; i < n; i++)
        {
            if (fixed[i] != FREE)
            {
                banded[i] = fixed[i];
            }
            else if (near[i] <= Q_BAND)
            {
                banded[i] = FREE;
            }
            else
            {
                banded[i] = proj[i] ? HARD_FG : HARD_BG;
            }
        }
        return grid_cut (img, cost_fg, banded, scale);
    }

    /* The model at working resolution: seeds (the brush footprint, `radius` working pixels) are
       selected, and so is `prior` (what the stroke selected so far, or empty), though only the
       seeds' distance prices growth. `closed` sides (left, top, right, bottom) border on
       background; the others are free. Returns the selection. */
    std::vector<uint8_t> select_working (const RgbImage& img, const std::vector<bool>& seeds, float radius,
        const std::vector<bool>& prior, const bool closed[4])
    {
        std::vector<float> d = edt (seeds, img.w, img.h);
        float grow           = fmaxf (Q_GROW + Q_GROW_FRAC * radius, 0.0f);
        std::vector<float> cost (d.size ());
        std::vector<uint8_t> fixed (d.size ());
        for (size_t i = 0; i < d.size (); i++)
        {
            cost[i]  = Q_AREA * (1.0f + Q_DIST * powf (fmaxf (d[i] - grow, 0.0f), Q_POW));
            fixed[i] = d[i] <= grow || (!prior.empty () && prior[i]) ? HARD_FG : FREE;
        }
        /* A closed side's pixels stay unselected (unless already selected), as if what lies beyond
           were background. */
        size_t w = img.w, h = img.h;
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                bool side = (closed[0] && x == 0) || (closed[1] && y == 0) || (closed[2] && x + 1 == w)
                         || (closed[3] && y + 1 == h);
                if (side && fixed[y * w + x] != HARD_FG)
                {
                    fixed[y * w + x] = HARD_BG;
                }
            }
        }
        return cut_levels (img, fixed, cost, 1.0f);
    }

    /* Whether a selected pixel that isn't in `prior` lies on an inner side of the window, more than
       a few pixels from the prior selection (a stroke's earlier selection crosses the sides, and
       pixels just beside it there were decided before). */
    bool side_reached (
        const std::vector<uint8_t>& low, const std::vector<bool>& prior, size_t w, size_t h, const bool inner[4])
    {
        const long NEAR = 3;
        for (int side = 0; side < 4; side++)
        {
            if (!inner[side])
            {
                continue;
            }
            bool vertical = side == 0 || side == 2;
            size_t len    = vertical ? h : w;
            for (size_t k = 0; k < len; k++)
            {
                size_t x = side == 0 ? 0 : side == 2 ? w - 1 : k, y = side == 1 ? 0 : side == 3 ? h - 1 : k;
                size_t i = y * w + x;
                if (!low[i] || (!prior.empty () && prior[i]))
                {
                    continue;
                }
                if (prior.empty ())
                {
                    return true;
                }
                bool near = false;
                for (long dy = -NEAR; dy <= NEAR && !near; dy++)
                {
                    for (long dx = -NEAR; dx <= NEAR && !near; dx++)
                    {
                        long nx = (long) x + dx, ny = (long) y + dy;
                        if (nx >= 0 && ny >= 0 && nx < (long) w && ny < (long) h)
                        {
                            near = prior[(size_t) ny * w + (size_t) nx];
                        }
                    }
                }
                if (!near)
                {
                    return true;
                }
            }
        }
        return false;
    }

    /* Pixels within `radius` (working pixels) of the polyline, plus each point's own pixel. */
    std::vector<bool> stroke_mask (const std::vector<QuickPoint>& pts, float radius, size_t w, size_t h)
    {
        std::vector<bool> m (w * h, false);
        size_t segs = pts.size () == 1 ? 1 : pts.size () - 1;
        for (size_t s = 0; s < segs; s++)
        {
            QuickPoint a = pts[s], b = pts.size () == 1 ? pts[0] : pts[s + 1];
            long x0  = (long) fmaxf (floorf (fminf (a.x, b.x) - radius), 0.0f),
                 y0  = (long) fmaxf (floorf (fminf (a.y, b.y) - radius), 0.0f);
            long x1  = (long) fminf (ceilf (fmaxf (a.x, b.x) + radius), (float) w),
                 y1  = (long) fminf (ceilf (fmaxf (a.y, b.y) + radius), (float) h);
            float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
            for (long y = y0; y < y1; y++)
            {
                for (long x = x0; x < x1; x++)
                {
                    float px = (float) x + 0.5f, py = (float) y + 0.5f;
                    float t =
                        len2 > 0.0f ? fminf (fmaxf (((px - a.x) * dx + (py - a.y) * dy) / len2, 0.0f), 1.0f) : 0.0f;
                    float qx = a.x + t * dx - px, qy = a.y + t * dy - py;
                    if (qx * qx + qy * qy <= radius * radius)
                    {
                        m[(size_t) y * w + (size_t) x] = true;
                    }
                }
            }
        }
        for (size_t i = 0; i < pts.size (); i++)
        {
            if (pts[i].x >= 0.0f && pts[i].y >= 0.0f && (size_t) pts[i].x < w && (size_t) pts[i].y < h)
            {
                m[(size_t) pts[i].y * w + (size_t) pts[i].x] = true;
            }
        }
        return m;
    }

    /* Bilinear upsampling (pixel centres aligned, edges clamped) to 8-bit coverage. */
    std::vector<uint8_t> upsample (
        const std::vector<uint8_t>& low, size_t lw, size_t lh, size_t step, size_t w, size_t h)
    {
        std::vector<uint8_t> out (w * h);
        if (step == 1)
        {
            for (size_t i = 0; i < out.size (); i++)
            {
                out[i] = low[i] ? 255 : 0;
            }
            return out;
        }
        float s = (float) step;
        for (size_t y = 0; y < h; y++)
        {
            float fy = ((float) y + 0.5f) / s - 0.5f;
            long iy  = (long) floorf (fy);
            float ty = fy - floorf (fy);
            for (size_t x = 0; x < w; x++)
            {
                float fx = ((float) x + 0.5f) / s - 0.5f;
                long ix  = (long) floorf (fx);
                float tx = fx - floorf (fx);
                float v[2][2];
                for (int j = 0; j < 2; j++)
                {
                    for (int i = 0; i < 2; i++)
                    {
                        long cx = ix + i < 0 ? 0 : ix + i >= (long) lw ? (long) lw - 1 : ix + i;
                        long cy = iy + j < 0 ? 0 : iy + j >= (long) lh ? (long) lh - 1 : iy + j;
                        v[j][i] = low[(size_t) cy * lw + (size_t) cx] ? 1.0f : 0.0f;
                    }
                }
                float val = (v[0][0] * (1 - tx) + v[0][1] * tx) * (1 - ty) + (v[1][0] * (1 - tx) + v[1][1] * tx) * ty;
                out[y * w + x] = (uint8_t) lroundf (val * 255.0f);
            }
        }
        return out;
    }

    /* The region cut to its non-zero bounds; false when empty. */
    bool trim_region (Region* r)
    {
        size_t w = r->bbox.width ();
        if (w == 0)
        {
            return false;
        }
        size_t h  = r->mask.size () / w;
        size_t x0 = SIZE_MAX, y0 = SIZE_MAX, x1 = 0, y1 = 0;
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                if (r->mask[y * w + x] == 0)
                {
                    continue;
                }
                if (x < x0)
                {
                    x0 = x;
                }
                if (x + 1 > x1)
                {
                    x1 = x + 1;
                }
                if (y < y0)
                {
                    y0 = y;
                }
                y1 = y + 1;
            }
        }
        if (x0 == SIZE_MAX)
        {
            return false;
        }
        if (x0 == 0 && y0 == 0 && x1 == w && y1 == h)
        {
            return true;
        }
        std::vector<uint8_t> mask;
        mask.reserve ((x1 - x0) * (y1 - y0));
        for (size_t y = y0; y < y1; y++)
        {
            mask.insert (
                mask.end (), r->mask.begin () + (ptrdiff_t) (y * w + x0), r->mask.begin () + (ptrdiff_t) (y * w + x1));
        }
        geom::Rect b = r->bbox;
        r->bbox      = geom::Rect{b.x0 + (int32_t) x0, b.y0 + (int32_t) y0, b.x0 + (int32_t) x1, b.y0 + (int32_t) y1};
        r->mask      = mask;
        return true;
    }

    /* Seconds on a steady clock, for QuickStats. */
    double quick_clock ()
    {
        struct timespec ts;
        clock_gettime (CLOCK_MONOTONIC, &ts);
        return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
    }

    int32_t snap_down (int32_t v, int32_t origin, int32_t step)
    {
        int32_t d = v - origin;
        int32_t q = d >= 0 ? d / step : -((-d + step - 1) / step);
        return origin + q * step;
    }

    /* Grows a selection from a brush stroke (points in document pixels, brush diameter `size`) on
       the document `canvas`. The result is the region to add to or subtract from the selection;
       false when the stroke misses the canvas. A dragged stroke is cut a segment at a time:
       `points` is the newest segment (the window is around it), `stroke` (or NULL for `points`)
       the whole stroke so far, whose footprint seeds the cut and prices growth by distance as one
       stroke would, and `prior` (or NULL) what earlier parts selected, which stays selected. At most
       `max_windows` windows are tried (0: as many as it takes). `stats` (or NULL) gets how long
       each step took, summed over the windows tried. */
    bool quick_select (const RgbaSource& src, geom::Rect canvas, const std::vector<QuickPoint>& points, float size,
        const std::vector<QuickPoint>* stroke, const raster::Surface* prior, int max_windows, Region* out,
        QuickStats* stats)
    {
        QuickStats local;
        QuickStats* st = stats != NULL ? stats : &local;
        memset (st, 0, sizeof *st);
        if (points.empty () || canvas.is_empty ())
        {
            return false;
        }
        float r   = fmaxf (size, 1.0f) / 2.0f;
        float bx0 = INFINITY, by0 = INFINITY, bx1 = -INFINITY, by1 = -INFINITY;
        for (size_t i = 0; i < points.size (); i++)
        {
            bx0 = fminf (bx0, points[i].x - r);
            by0 = fminf (by0, points[i].y - r);
            bx1 = fmaxf (bx1, points[i].x + r);
            by1 = fmaxf (by1, points[i].y + r);
        }
        float lim       = 1e9f;
        geom::Rect bbox = geom::Rect{(int32_t) fmaxf (floorf (bx0), -lim), (int32_t) fmaxf (floorf (by0), -lim),
            (int32_t) fminf (ceilf (bx1), lim) + 1, (int32_t) fminf (ceilf (by1), lim) + 1}
                              .intersect (canvas);
        if (bbox.is_empty ())
        {
            return false;
        }
        int32_t step = (uint64_t) canvas.width () * canvas.height () <= 512u * 512u ? 1 : 2;
        float rw     = r / (float) step;
        /* Work in a window around the stroke; grow it while the selection runs into its border. */
        int32_t margin = (int32_t) fmaxf (48.0f, 2.0f * rw) * step;
        for (;;)
        {
            geom::Rect grown  = bbox.inflate (margin).intersect (canvas);
            geom::Rect window = {
                snap_down (grown.x0, canvas.x0, step), snap_down (grown.y0, canvas.y0, step), grown.x1, grown.y1};
            double t0    = quick_clock ();
            RgbImage img = read_rgb (src, window, (size_t) step);
            double t1    = quick_clock ();
            st->windows++;
            st->pixels = img.w * img.h;
            st->read += t1 - t0;
            size_t lw = img.w, lh = img.h;
            float s                                 = (float) step;
            const std::vector<QuickPoint>& seed_pts = stroke != NULL ? *stroke : points;
            std::vector<QuickPoint> pts (seed_pts.size ());
            for (size_t i = 0; i < seed_pts.size (); i++)
            {
                pts[i].x = (seed_pts[i].x - (float) window.x0) / s;
                pts[i].y = (seed_pts[i].y - (float) window.y0) / s;
            }
            std::vector<bool> seeds = stroke_mask (pts, fmaxf (rw, 0.5f), lw, lh);
            bool any                = false;
            for (size_t i = 0; i < seeds.size () && !any; i++)
            {
                any = seeds[i];
            }
            if (!any)
            {
                return false;
            }
            /* The prior selection at working resolution (each working pixel's centre). */
            std::vector<bool> fixed_fg;
            if (prior != NULL && prior->tile_count () > 0 && prior->has_tiles_in (window))
            {
                fixed_fg.assign (lw * lh, false);
                std::vector<float> pm;
                if (prior->read_region (window, &pm))
                {
                    size_t ww = window.width (), pc = prior->channels ();
                    for (size_t y = 0; y < lh; y++)
                    {
                        for (size_t x = 0; x < lw; x++)
                        {
                            size_t px = std::min (x * (size_t) step + (size_t) step / 2, ww - 1),
                                   py = std::min (y * (size_t) step + (size_t) step / 2, (size_t) window.height () - 1);
                            fixed_fg[y * lw + x] = pm[(py * ww + px) * pc] >= 0.5f;
                        }
                    }
                }
            }
            double t2 = quick_clock ();
            st->seeds += t2 - t1;
            /* The window's sides are free like the canvas edges, so a selection clear of them is the
               best over the whole canvas too; one reaching a side inside the canvas is redone in a
               larger window. For part of a stroke, the selection so far crosses the sides, so only
               new pixels well away from it count as reaching one. */
            bool inner[4] = {
                window.x0 > canvas.x0, window.y0 > canvas.y0, window.x1 < canvas.x1, window.y1 < canvas.y1};
            bool no_sides[4]         = {false, false, false, false};
            std::vector<uint8_t> low = select_working (img, seeds, rw, fixed_fg, no_sides);
            st->cut += quick_clock () - t2;
            bool touches = side_reached (low, fixed_fg, lw, lh, inner);
            bool more    = margin < (int32_t) (canvas.width () + canvas.height ());
            if (touches && more && (max_windows <= 0 || st->windows < max_windows))
            {
                margin *= 2;
                continue;
            }
            /* Out of windows with the selection still running to a side: cut this window again
               with its inner sides closed, so the selection ends at an edge rather than the side. */
            if (touches && more)
            {
                double t4 = quick_clock ();
                low       = select_working (img, seeds, rw, fixed_fg, inner);
                st->cut += quick_clock () - t4;
            }
            double t3    = quick_clock ();
            out->bbox    = window;
            out->mask    = upsample (low, lw, lh, (size_t) step, window.width (), window.height ());
            bool found   = trim_region (out);
            st->upsample = quick_clock () - t3;
            return found;
        }
    }

} // namespace algo
