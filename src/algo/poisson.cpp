#include "algo/poisson.h"

#include <math.h>

namespace algo
{

    /* Gradient-domain ("Poisson") blending, the core of the healing tools (Pérez, Gangnet and Blake,
       Poisson Image Editing, 2003). Seamless cloning keeps the source's gradients and solves the
       membrane (Laplace) interpolant of the boundary mismatch, so healed pixels keep the source's
       texture and take the destination's colour and lighting. The Laplace equation is solved by SOR,
       cascadically: restricted to half resolution recursively, solved there, prolonged as the
       initial guess, then a few sweeps per level. */

    const float TOL         = 2e-5f;
    const size_t FINE_ITERS = 30;
    const float FINE_OMEGA  = 1.7f;

    /* SOR sweeps over the unknown pixels until the largest update is below TOL. */
    void sor (size_t w, size_t h, const std::vector<bool>& unknown, float* v, size_t max_iter, float omega)
    {
        std::vector<uint32_t> cells;
        std::vector<uint8_t> masks;
        for (size_t i = 0; i < w * h; i++)
        {
            if (!unknown[i])
            {
                continue;
            }
            size_t x = i % w, y = i / w;
            uint8_t m = (uint8_t) ((x > 0) | ((x + 1 < w) << 1) | ((y > 0) << 2) | ((y + 1 < h) << 3));
            cells.push_back ((uint32_t) i);
            masks.push_back (m);
        }
        for (size_t it = 0; it < max_iter; it++)
        {
            float max_d = 0.0f;
            for (size_t k = 0; k < cells.size (); k++)
            {
                size_t i  = cells[k];
                uint8_t m = masks[k];
                float s = 0.0f, n = 0.0f;
                if (m & 1)
                {
                    s += v[i - 1];
                    n += 1.0f;
                }
                if (m & 2)
                {
                    s += v[i + 1];
                    n += 1.0f;
                }
                if (m & 4)
                {
                    s += v[i - w];
                    n += 1.0f;
                }
                if (m & 8)
                {
                    s += v[i + w];
                    n += 1.0f;
                }
                if (n == 0.0f)
                {
                    continue;
                }
                float d = omega * (s / n - v[i]);
                v[i] += d;
                if (fabsf (d) > max_d)
                {
                    max_d = fabsf (d);
                }
            }
            if (max_d < TOL)
            {
                break;
            }
        }
    }

    void solve_rec (size_t w, size_t h, const std::vector<bool>& unknown, float* v, int depth)
    {
        size_t n_unknown = 0, n_known = 0;
        double known_sum = 0.0;
        for (size_t i = 0; i < w * h; i++)
        {
            if (unknown[i])
            {
                n_unknown++;
            }
            else
            {
                n_known++;
                known_sum += v[i];
            }
        }
        if (n_unknown == 0 || n_known == 0)
        {
            return;
        }
        if (w >= 8 && h >= 8 && n_unknown > 64 && depth < 16)
        {
            /* Restrict: a coarse pixel is known when any child is (the mean of the known ones). */
            size_t cw = (w + 1) / 2, chh = (h + 1) / 2;
            std::vector<bool> cu (cw * chh, true);
            std::vector<float> cv (cw * chh, 0.0f);
            for (size_t cy = 0; cy < chh; cy++)
            {
                for (size_t cx = 0; cx < cw; cx++)
                {
                    float s = 0.0f;
                    int n   = 0;
                    for (int k = 0; k < 4; k++)
                    {
                        size_t x = cx * 2 + (size_t) (k & 1), y = cy * 2 + (size_t) (k >> 1);
                        if (x < w && y < h && !unknown[y * w + x])
                        {
                            s += v[y * w + x];
                            n++;
                        }
                    }
                    if (n > 0)
                    {
                        cu[cy * cw + cx] = false;
                        cv[cy * cw + cx] = s / (float) n;
                    }
                }
            }
            solve_rec (cw, chh, cu, cv.data (), depth + 1);
            /* Prolong: bilinear from the coarse solution as the initial guess. */
            for (size_t y = 0; y < h; y++)
            {
                float fy  = fminf (fmaxf (((float) y + 0.5f) / 2.0f - 0.5f, 0.0f), (float) (chh - 1));
                size_t y0 = (size_t) fy, y1 = y0 + 1 < chh ? y0 + 1 : chh - 1;
                float ty = fy - (float) y0;
                for (size_t x = 0; x < w; x++)
                {
                    if (!unknown[y * w + x])
                    {
                        continue;
                    }
                    float fx  = fminf (fmaxf (((float) x + 0.5f) / 2.0f - 0.5f, 0.0f), (float) (cw - 1));
                    size_t x0 = (size_t) fx, x1 = x0 + 1 < cw ? x0 + 1 : cw - 1;
                    float tx     = fx - (float) x0;
                    float a      = cv[y0 * cw + x0] + (cv[y0 * cw + x1] - cv[y0 * cw + x0]) * tx;
                    float b      = cv[y1 * cw + x0] + (cv[y1 * cw + x1] - cv[y1 * cw + x0]) * tx;
                    v[y * w + x] = a + (b - a) * ty;
                }
            }
            sor (w, h, unknown, v, FINE_ITERS, FINE_OMEGA);
        }
        else
        {
            float mean = (float) (known_sum / (double) n_known);
            for (size_t i = 0; i < w * h; i++)
            {
                if (unknown[i])
                {
                    v[i] = mean;
                }
            }
            sor (w, h, unknown, v, 2000, 1.85f);
        }
    }

    /* Laplace on the unknown pixels with Dirichlet values from the known ones (a Neumann boundary
       at the grid edge). */
    void solve_membrane (size_t w, size_t h, const std::vector<bool>& unknown, float* v)
    {
        solve_rec (w, h, unknown, v, 0);
    }

    /* f = dst outside the mask; inside, src plus the membrane that removes the boundary mismatch. */
    std::vector<float> seamless_clone (
        size_t w, size_t h, size_t ch, const float* src, const float* dst, const std::vector<bool>& mask)
    {
        std::vector<float> out (w * h * ch);
#pragma omp parallel for schedule(static) if (w * h >= 64 * 64)
        for (ptrdiff_t ci = 0; ci < (ptrdiff_t) ch; ci++)
        {
            size_t c = (size_t) ci;
            std::vector<float> hv (w * h);
            for (size_t i = 0; i < w * h; i++)
            {
                hv[i] = mask[i] ? 0.0f : dst[i * ch + c] - src[i * ch + c];
            }
            solve_membrane (w, h, mask, hv.data ());
            for (size_t i = 0; i < w * h; i++)
            {
                out[i * ch + c] = mask[i] ? src[i * ch + c] + hv[i] : dst[i * ch + c];
            }
        }
        return out;
    }

    /* Harmonic interpolation of the known pixels into the hole (no source texture). */
    std::vector<float> membrane_fill (size_t w, size_t h, size_t ch, const float* img, const std::vector<bool>& hole)
    {
        std::vector<float> out (w * h * ch);
#pragma omp parallel for schedule(static) if (w * h >= 64 * 64)
        for (ptrdiff_t ci = 0; ci < (ptrdiff_t) ch; ci++)
        {
            size_t c = (size_t) ci;
            std::vector<float> v (w * h);
            for (size_t i = 0; i < w * h; i++)
            {
                v[i] = hole[i] ? 0.0f : img[i * ch + c];
            }
            solve_membrane (w, h, hole, v.data ());
            for (size_t i = 0; i < w * h; i++)
            {
                out[i * ch + c] = v[i];
            }
        }
        return out;
    }

    /* The search state of best_offset(). */
    struct OffsetSearch
    {
        size_t w;
        size_t h;
        size_t ch;
        const float* img;
        long x0, y0, x1, y1;
        long hole_x0, hole_y0;
        size_t hole_w, hole_h;
        std::vector<uint32_t> sat;
        std::vector<int32_t> pts;
        int32_t max_radius;
        bool found;
        float best;
        int32_t bdx, bdy;
    };

    /* Hole pixels in [x0, x1) x [y0, y1), from the summed-area table over the hole's bounds. */
    uint32_t hole_count (const OffsetSearch& s, long x0, long y0, long x1, long y1)
    {
        x0 -= s.hole_x0;
        x1 -= s.hole_x0;
        y0 -= s.hole_y0;
        y1 -= s.hole_y0;
        if (x0 < 0)
        {
            x0 = 0;
        }
        if (y0 < 0)
        {
            y0 = 0;
        }
        if (x1 > (long) s.hole_w)
        {
            x1 = (long) s.hole_w;
        }
        if (y1 > (long) s.hole_h)
        {
            y1 = (long) s.hole_h;
        }
        if (x1 <= x0 || y1 <= y0)
        {
            return 0;
        }
        size_t w1 = s.hole_w + 1;
        return s.sat[(size_t) y1 * w1 + (size_t) x1] + s.sat[(size_t) y0 * w1 + (size_t) x0]
             - s.sat[(size_t) y0 * w1 + (size_t) x1] - s.sat[(size_t) y1 * w1 + (size_t) x0];
    }

    /* Scores displacement (dx, dy): the ring's squared difference, with a mild preference for
       nearer sources. Keeps the best. */
    void consider_offset (OffsetSearch* s, int32_t dx, int32_t dy)
    {
        long hx0 = s->x0 + dx, hy0 = s->y0 + dy, hx1 = s->x1 + dx, hy1 = s->y1 + dy;
        if ((dx == 0 && dy == 0) || hx0 < 0 || hy0 < 0 || hx1 > (long) s->w || hy1 > (long) s->h
            || hole_count (*s, hx0, hy0, hx1, hy1) != 0)
        {
            return;
        }
        size_t np     = s->pts.size () / 2;
        float n       = (float) (np > 0 ? np : 1);
        float penalty = 1.0f + 1e-3f * (hypotf ((float) dx, (float) dy) / (float) s->max_radius);
        float e       = 0.0f;
        for (size_t i = 0; i < np; i++)
        {
            int32_t px = s->pts[i * 2], py = s->pts[i * 2 + 1];
            size_t a = ((size_t) py * s->w + (size_t) px) * s->ch,
                   b = ((size_t) (py + dy) * s->w + (size_t) (px + dx)) * s->ch;
            for (size_t c = 0; c < s->ch; c++)
            {
                float d = s->img[a + c] - s->img[b + c];
                e += d * d;
            }
            if (i % 32 == 31 && s->found && e / n * penalty >= s->best)
            {
                return;
            }
        }
        float score = e / n * penalty;
        if (!s->found || score < s->best)
        {
            s->found = true;
            s->best  = score;
            s->bdx   = dx;
            s->bdy   = dy;
        }
    }

    /* Proximity Match: the displacement (within max_radius) whose surroundings best match the ring
       of `ring` pixels around the hole, with the displaced hole fully inside the grid and clear of
       the hole. A coarse grid search, then a full-resolution refinement around the winner. */
    bool best_offset (size_t w, size_t h, size_t ch, const float* img, const std::vector<bool>& hole, size_t ring,
        int32_t max_radius, int32_t* out_dx, int32_t* out_dy)
    {
        long bx0 = (long) w, by0 = (long) h, bx1 = -1, by1 = -1;
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                if (!hole[y * w + x])
                {
                    continue;
                }
                if ((long) x < bx0)
                {
                    bx0 = (long) x;
                }
                if ((long) y < by0)
                {
                    by0 = (long) y;
                }
                if ((long) x + 1 > bx1)
                {
                    bx1 = (long) x + 1;
                }
                if ((long) y + 1 > by1)
                {
                    by1 = (long) y + 1;
                }
            }
        }
        if (bx1 < 0)
        {
            return false;
        }
        int32_t extent = (int32_t) (w > h ? w : h);
        if (max_radius > extent)
        {
            max_radius = extent;
        }
        if (max_radius <= 0)
        {
            return false;
        }
        int32_t r = (int32_t) (ring < 1 ? 1 : ring);
        OffsetSearch s;
        s.w       = w;
        s.h       = h;
        s.ch      = ch;
        s.img     = img;
        s.hole_x0 = bx0;
        s.hole_y0 = by0;
        s.hole_w  = (size_t) (bx1 - bx0);
        s.hole_h  = (size_t) (by1 - by0);
        s.sat.assign ((s.hole_w + 1) * (s.hole_h + 1), 0);
        for (size_t y = 0; y < s.hole_h; y++)
        {
            uint32_t row = 0;
            for (size_t x = 0; x < s.hole_w; x++)
            {
                row += hole[(y + (size_t) by0) * w + x + (size_t) bx0] ? 1u : 0u;
                s.sat[(y + 1) * (s.hole_w + 1) + x + 1] = s.sat[y * (s.hole_w + 1) + x + 1] + row;
            }
        }
        s.x0 = bx0 - r;
        s.y0 = by0 - r;
        s.x1 = bx1 + r;
        s.y1 = by1 + r;
        /* Sample points: the ring around the hole, subsampled to about 2000. */
        std::vector<int32_t> ringpts;
        for (long y = s.y0 > 0 ? s.y0 : 0; y < (s.y1 < (long) h ? s.y1 : (long) h); y++)
        {
            for (long x = s.x0 > 0 ? s.x0 : 0; x < (s.x1 < (long) w ? s.x1 : (long) w); x++)
            {
                if (hole[(size_t) y * w + (size_t) x])
                {
                    continue;
                }
                if (hole_count (s, x - r, y - r, x + r + 1, y + r + 1) > 0)
                {
                    ringpts.push_back ((int32_t) x);
                    ringpts.push_back ((int32_t) y);
                }
            }
        }
        size_t np = ringpts.size () / 2, stride = np / 2000 > 1 ? np / 2000 : 1;
        for (size_t i = 0; i < np; i += stride)
        {
            s.pts.push_back (ringpts[i * 2]);
            s.pts.push_back (ringpts[i * 2 + 1]);
        }
        s.max_radius = max_radius;
        s.found      = false;
        s.best       = INFINITY;
        s.bdx = s.bdy = 0;
        int32_t step  = max_radius / 24 > 1 ? max_radius / 24 : 1;
        for (int32_t dy = -max_radius; dy <= max_radius; dy += step)
        {
            for (int32_t dx = -max_radius; dx <= max_radius; dx += step)
            {
                consider_offset (&s, dx, dy);
            }
        }
        if (step > 1 && s.found)
        {
            int32_t cx = s.bdx, cy = s.bdy;
            for (int32_t dy = cy - step; dy <= cy + step; dy++)
            {
                for (int32_t dx = cx - step; dx <= cx + step; dx++)
                {
                    consider_offset (&s, dx, dy);
                }
            }
        }
        *out_dx = s.bdx;
        *out_dy = s.bdy;
        return s.found;
    }

} // namespace algo
