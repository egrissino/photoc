#include "algo/inpaint.h"

#include "algo/poisson.h"
#include <algorithm>
#include <math.h>
#include <string.h>

namespace algo
{

    /* Content-aware filling by patch search (no learned model): a hole is filled with patches
       copied from the rest of the image so that every patch of the result looks like some patch
       outside the hole. Multi-scale, after Wexler, Shechtman and Irani: the hole is filled smoothly
       at the coarsest scale, then at each finer scale the nearest-neighbour field (for each patch
       touching the hole, the most similar fully known patch) is found by PatchMatch (random
       guesses, propagation from neighbours, random search around the best so far) and every hole
       pixel becomes the weighted vote of the patches covering it. */

    const int PATCH_R    = 3;
    const int MAX_LEVELS = 9;

    struct Level
    {
        int w;
        int h;
        std::vector<float> img;
        std::vector<uint8_t> hole;
        std::vector<uint8_t> valid;
        std::vector<int32_t> valid_list;
    };

    struct Rng
    {
        uint64_t s;
    };

    uint32_t next (Rng* r)
    {
        r->s ^= r->s << 13;
        r->s ^= r->s >> 7;
        r->s ^= r->s << 17;
        return (uint32_t) (r->s >> 11);
    }

    /* Patch centres whose whole patch is inside the image and outside the hole (and the allowed
       source area). */
    void mark_valid (Level* L, const std::vector<uint8_t>* source)
    {
        int w = L->w, h = L->h;
        std::vector<uint8_t> grown (L->hole);
        /* Grow the hole by the patch radius: a centre is valid only if no hole pixel is within it. */
        std::vector<uint8_t> tmp ((size_t) w * h, 0);
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                bool any = false;
                for (int d = -PATCH_R; d <= PATCH_R && !any; d++)
                {
                    int xx = x + d;
                    if (xx >= 0 && xx < w
                        && (L->hole[(size_t) y * w + xx] || (source != NULL && !(*source)[(size_t) y * w + xx])))
                    {
                        any = true;
                    }
                }
                tmp[(size_t) y * w + x] = any;
            }
        }
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                bool any = false;
                for (int d = -PATCH_R; d <= PATCH_R && !any; d++)
                {
                    int yy = y + d;
                    if (yy >= 0 && yy < h && tmp[(size_t) yy * w + x])
                    {
                        any = true;
                    }
                }
                grown[(size_t) y * w + x] = any;
            }
        }
        L->valid.assign ((size_t) w * h, 0);
        L->valid_list.clear ();
        for (int y = PATCH_R; y < h - PATCH_R; y++)
        {
            for (int x = PATCH_R; x < w - PATCH_R; x++)
            {
                if (!grown[(size_t) y * w + x])
                {
                    L->valid[(size_t) y * w + x] = 1;
                    L->valid_list.push_back (y * w + x);
                }
            }
        }
    }

    /* Half size: known pixels averaged, a coarse pixel is hole if any of its pixels is. */
    Level downsample (const Level& f, size_t ch)
    {
        Level c;
        c.w = (f.w + 1) / 2;
        c.h = (f.h + 1) / 2;
        c.img.assign ((size_t) c.w * c.h * ch, 0.0f);
        c.hole.assign ((size_t) c.w * c.h, 0);
        for (int y = 0; y < c.h; y++)
        {
            for (int x = 0; x < c.w; x++)
            {
                float acc[16] = {0};
                int n         = 0;
                bool hole     = false;
                for (int dy = 0; dy < 2; dy++)
                {
                    for (int dx = 0; dx < 2; dx++)
                    {
                        int fx = 2 * x + dx, fy = 2 * y + dy;
                        if (fx >= f.w || fy >= f.h)
                        {
                            continue;
                        }
                        size_t fi = (size_t) fy * f.w + fx;
                        if (f.hole[fi])
                        {
                            hole = true;
                            continue;
                        }
                        for (size_t k = 0; k < ch && k < 16; k++)
                        {
                            acc[k] += f.img[fi * ch + k];
                        }
                        n++;
                    }
                }
                size_t ci  = (size_t) y * c.w + x;
                c.hole[ci] = hole;
                for (size_t k = 0; k < ch && k < 16; k++)
                {
                    c.img[ci * ch + k] = n > 0 ? acc[k] / (float) n : 0.0f;
                }
            }
        }
        return c;
    }

    /* Sum of squared differences between the patches at p and q, stopping past `cap`. */
    float patch_dist (const Level& L, size_t ch, int px, int py, int qx, int qy, float cap)
    {
        float d = 0.0f;
        for (int dy = -PATCH_R; dy <= PATCH_R; dy++)
        {
            int ay = py + dy;
            if (ay < 0 || ay >= L.h)
            {
                continue;
            }
            const float *a = &L.img[((size_t) ay * L.w) * ch], *b = &L.img[((size_t) (qy + dy) * L.w) * ch];
            for (int dx = -PATCH_R; dx <= PATCH_R; dx++)
            {
                int ax = px + dx;
                if (ax < 0 || ax >= L.w)
                {
                    continue;
                }
                const float *pa = a + (size_t) ax * ch, *pb = b + (size_t) (qx + dx) * ch;
                for (size_t k = 0; k < ch; k++)
                {
                    float t = pa[k] - pb[k];
                    d += t * t;
                }
            }
            if (d > cap)
            {
                return d;
            }
        }
        return d;
    }

    struct Field
    {
        std::vector<int32_t> targets;
        std::vector<int32_t> nnf;
        std::vector<float> dist;
        std::vector<int32_t> slot;
    };

    /* The patch centres that touch the hole. */
    void field_targets (const Level& L, Field* f)
    {
        int w = L.w, h = L.h;
        f->slot.assign ((size_t) w * h, -1);
        f->targets.clear ();
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                bool touch = false;
                for (int dy = -PATCH_R; dy <= PATCH_R && !touch; dy++)
                {
                    int yy = y + dy;
                    if (yy < 0 || yy >= h)
                    {
                        continue;
                    }
                    for (int dx = -PATCH_R; dx <= PATCH_R; dx++)
                    {
                        int xx = x + dx;
                        if (xx >= 0 && xx < w && L.hole[(size_t) yy * w + xx])
                        {
                            touch = true;
                            break;
                        }
                    }
                }
                if (touch)
                {
                    f->slot[(size_t) y * w + x] = (int32_t) f->targets.size ();
                    f->targets.push_back (y * w + x);
                }
            }
        }
        f->nnf.assign (f->targets.size (), -1);
        f->dist.assign (f->targets.size (), INFINITY);
    }

    void try_match (const Level& L, size_t ch, Field* f, size_t t, int qx, int qy)
    {
        if (qx < 0 || qy < 0 || qx >= L.w || qy >= L.h || !L.valid[(size_t) qy * L.w + qx])
        {
            return;
        }
        int q = qy * L.w + qx;
        if (q == f->nnf[t])
        {
            return;
        }
        int p   = f->targets[t];
        float d = patch_dist (L, ch, p % L.w, p / L.w, qx, qy, f->dist[t]);
        if (d < f->dist[t])
        {
            f->dist[t] = d;
            f->nnf[t]  = q;
        }
    }

    /* PatchMatch: alternating scan order, propagation then random search. */
    void search (const Level& L, size_t ch, Field* f, Rng* rng, int iterations)
    {
        for (size_t t = 0; t < f->targets.size (); t++)
        {
            if (f->nnf[t] >= 0)
            {
                int q = f->nnf[t];
                if (L.valid[(size_t) q])
                {
                    int p      = f->targets[t];
                    f->dist[t] = patch_dist (L, ch, p % L.w, p / L.w, q % L.w, q / L.w, INFINITY);
                    continue;
                }
            }
            int q      = L.valid_list[next (rng) % L.valid_list.size ()];
            f->nnf[t]  = q;
            int p      = f->targets[t];
            f->dist[t] = patch_dist (L, ch, p % L.w, p / L.w, q % L.w, q / L.w, INFINITY);
        }
        for (int it = 0; it < iterations; it++)
        {
            bool rev = it & 1;
            size_t n = f->targets.size ();
            for (size_t k = 0; k < n; k++)
            {
                size_t t = rev ? n - 1 - k : k;
                int p = f->targets[t], px = p % L.w, py = p / L.w;
                int step = rev ? 1 : -1;
                /* From the neighbours already visited this pass, shifted by one. */
                int nb[2] = {(px + step >= 0 && px + step < L.w) ? f->slot[(size_t) py * L.w + px + step] : -1,
                    (py + step >= 0 && py + step < L.h) ? f->slot[(size_t) (py + step) * L.w + px] : -1};
                for (int i = 0; i < 2; i++)
                {
                    if (nb[i] < 0 || f->nnf[(size_t) nb[i]] < 0)
                    {
                        continue;
                    }
                    int q = f->nnf[(size_t) nb[i]];
                    try_match (L, ch, f, t, q % L.w + (i == 0 ? -step : 0), q / L.w + (i == 1 ? -step : 0));
                }
                int radius = std::max (L.w, L.h);
                while (radius >= 1)
                {
                    int q  = f->nnf[t];
                    int qx = q % L.w + (int) (next (rng) % (uint32_t) (2 * radius + 1)) - radius;
                    int qy = q / L.w + (int) (next (rng) % (uint32_t) (2 * radius + 1)) - radius;
                    try_match (L, ch, f, t, qx, qy);
                    radius /= 2;
                }
            }
        }
    }

    /* Every hole pixel: the vote of the patches covering it, weighted by how well they matched. */
    void vote (Level* L, size_t ch, const Field& f)
    {
        int w = L->w;
        std::vector<float> acc ((size_t) w * L->h * ch, 0.0f), wsum ((size_t) w * L->h, 0.0f);
        std::vector<float> ds (f.dist);
        std::sort (ds.begin (), ds.end ());
        float sigma2 = ds.empty () ? 1.0f : std::max (ds[ds.size () * 3 / 4], 1e-6f);
        for (size_t t = 0; t < f.targets.size (); t++)
        {
            int q = f.nnf[t];
            if (q < 0)
            {
                continue;
            }
            int p = f.targets[t], px = p % w, py = p / w, qx = q % w, qy = q / w;
            float wt = expf (-f.dist[t] / (2.0f * sigma2)) + 1e-6f;
            for (int dy = -PATCH_R; dy <= PATCH_R; dy++)
            {
                int y = py + dy;
                if (y < 0 || y >= L->h)
                {
                    continue;
                }
                for (int dx = -PATCH_R; dx <= PATCH_R; dx++)
                {
                    int x = px + dx;
                    if (x < 0 || x >= w)
                    {
                        continue;
                    }
                    size_t i = (size_t) y * w + x;
                    if (!L->hole[i])
                    {
                        continue;
                    }
                    const float* src = &L->img[((size_t) (qy + dy) * w + (size_t) (qx + dx)) * ch];
                    for (size_t k = 0; k < ch; k++)
                    {
                        acc[i * ch + k] += src[k] * wt;
                    }
                    wsum[i] += wt;
                }
            }
        }
        for (size_t i = 0; i < wsum.size (); i++)
        {
            if (!L->hole[i] || wsum[i] <= 0.0f)
            {
                continue;
            }
            for (size_t k = 0; k < ch; k++)
            {
                L->img[i * ch + k] = acc[i * ch + k] / wsum[i];
            }
        }
    }

    bool inpaint (size_t w, size_t h, size_t ch, std::vector<float>* img, const std::vector<bool>& hole,
        const std::vector<bool>* source, uint64_t seed)
    {
        if (w == 0 || h == 0 || ch == 0 || ch > 16)
        {
            return false;
        }
        std::vector<Level> levels (1);
        levels[0].w   = (int) w;
        levels[0].h   = (int) h;
        levels[0].img = *img;
        levels[0].hole.assign (w * h, 0);
        bool any = false;
        for (size_t i = 0; i < w * h; i++)
        {
            levels[0].hole[i] = hole[i];
            any               = any || hole[i];
        }
        if (!any)
        {
            return true;
        }
        /* Down to where the hole is a few patches across. */
        while ((int) levels.size () < MAX_LEVELS && levels.back ().w > 8 * PATCH_R && levels.back ().h > 8 * PATCH_R)
        {
            int count = 0;
            for (size_t i = 0; i < levels.back ().hole.size (); i++)
            {
                count += levels.back ().hole[i];
            }
            if (count < 4 * (2 * PATCH_R + 1) * (2 * PATCH_R + 1))
            {
                break;
            }
            levels.push_back (downsample (levels.back (), ch));
        }
        /* The allowed source area at every scale. */
        std::vector<std::vector<uint8_t>> src (levels.size ());
        if (source != NULL)
        {
            src[0].assign (w * h, 0);
            for (size_t i = 0; i < w * h; i++)
            {
                src[0][i] = (*source)[i];
            }
            for (size_t l = 1; l < levels.size (); l++)
            {
                const Level& fine = levels[l - 1];
                src[l].assign ((size_t) levels[l].w * levels[l].h, 0);
                for (int y = 0; y < levels[l].h; y++)
                {
                    for (int x = 0; x < levels[l].w; x++)
                    {
                        bool ok = true;
                        for (int dy = 0; dy < 2 && ok; dy++)
                        {
                            for (int dx = 0; dx < 2; dx++)
                            {
                                int fx = 2 * x + dx, fy = 2 * y + dy;
                                if (fx < fine.w && fy < fine.h && !src[l - 1][(size_t) fy * fine.w + fx])
                                {
                                    ok = false;
                                }
                            }
                        }
                        src[l][(size_t) y * levels[l].w + x] = ok;
                    }
                }
            }
        }
        bool usable = true;
        for (size_t l = 0; l < levels.size (); l++)
        {
            mark_valid (&levels[l], source != NULL ? &src[l] : NULL);
            if (levels[l].valid_list.empty ())
            {
                usable = l > 0 ? usable : false;
            }
        }
        if (levels[0].valid_list.empty ())
        {
            return false;
        }
        /* Drop coarse levels with nothing to copy from. */
        while (levels.size () > 1 && levels.back ().valid_list.empty ())
        {
            levels.pop_back ();
        }
        Rng rng = {seed * 0x9E3779B97F4A7C15ull + 1};
        /* The coarsest hole starts smooth. */
        Level& top = levels.back ();
        std::vector<bool> th (top.hole.size ());
        for (size_t i = 0; i < th.size (); i++)
        {
            th[i] = top.hole[i];
        }
        top.img = membrane_fill ((size_t) top.w, (size_t) top.h, ch, top.img.data (), th);
        Field prev;
        for (size_t li = levels.size (); li-- > 0;)
        {
            Level& L = levels[li];
            Field f;
            field_targets (L, &f);
            if (li + 1 < levels.size ())
            {
                /* From the coarser field: each pixel's match scaled up, its image voted from it. */
                const Level& C = levels[li + 1];
                for (size_t t = 0; t < f.targets.size (); t++)
                {
                    int p = f.targets[t], px = p % L.w, py = p / L.w;
                    int cx = std::min (px / 2, C.w - 1), cy = std::min (py / 2, C.h - 1);
                    int cs = prev.slot[(size_t) cy * C.w + cx];
                    if (cs < 0 || prev.nnf[(size_t) cs] < 0)
                    {
                        continue;
                    }
                    int q  = prev.nnf[(size_t) cs];
                    int qx = 2 * (q % C.w) + (px - 2 * cx), qy = 2 * (q / C.w) + (py - 2 * cy);
                    if (qx >= 0 && qy >= 0 && qx < L.w && qy < L.h && L.valid[(size_t) qy * L.w + qx])
                    {
                        f.nnf[t] = qy * L.w + qx;
                    }
                }
                search (L, ch, &f, &rng, 0);
                vote (&L, ch, f);
            }
            int em = li == 0 ? 4 : 6;
            for (int e = 0; e < em; e++)
            {
                search (L, ch, &f, &rng, e == 0 ? 4 : 2);
                vote (&L, ch, f);
            }
            prev = f;
        }
        *img = levels[0].img;
        (void) usable;
        return true;
    }

} // namespace algo
