#include "compose/masks.h"
#include "engine/filter.h"
#include "engine/filter_ops.h"
#include "engine/layers.h"
#include "io/io.h"
#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace engine
{

    /* The Filter menu's pipeline and its first filters (Blur, Sharpen, Noise, Other, Mosaic,
       Emboss, Find Edges, Solarize), plus Image > Auto Tone, Auto Contrast and Adjustments >
       Equalize, which share it. Every filter is a FilterSpec (filter_ops.h; the other groups live
       in filter_*.cpp): apply_filter reads the active layer's pixels around the selection into a
       Plane in the layer's own channels (any colour mode), premultiplied by alpha when the filter
       averages neighbours so transparent pixels don't bleed their colour, runs the filter, and
       blends the result in through the selection. */

    using geom::Rect;

    /* One channel of `pl` copied out as its own array. */
    std::vector<float> channel_of (const Plane& pl, size_t c)
    {
        std::vector<float> out (pl.w * pl.h);
        for (size_t i = 0; i < out.size (); i++)
        {
            out[i] = pl.px[i * pl.n + c];
        }
        return out;
    }

    void set_channel (Plane* pl, size_t c, const std::vector<float>& v)
    {
        for (size_t i = 0; i < v.size (); i++)
        {
            pl->px[i * pl->n + c] = v[i];
        }
    }

    void premultiply (Plane* pl, bool on)
    {
        if (!pl->alpha)
        {
            return;
        }
        size_t count = pl->w * pl->h;
        for (size_t i = 0; i < count; i++)
        {
            float* p = &pl->px[i * pl->n];
            float a  = p[pl->n - 1];
            for (size_t c = 0; c + 1 < pl->n; c++)
            {
                if (on)
                {
                    p[c] *= a;
                }
                else
                {
                    p[c] = a > 1e-6f ? p[c] / a : 0.0f;
                }
            }
        }
    }

    /* Every channel blurred with a Gaussian of `sigma`. */
    void gaussian_all (Plane* pl, float sigma)
    {
        for (size_t c = 0; c < pl->n; c++)
        {
            std::vector<float> v = channel_of (*pl, c);
            compose::gaussian (v.data (), pl->w, pl->h, sigma);
            set_channel (pl, c, v);
        }
    }

    /* A box average of radius r along rows (dx = 1) or columns, clamped at the edges. */
    void box_pass (std::vector<float>* v, size_t w, size_t h, int r, bool rows)
    {
        size_t len = rows ? w : h, lines = rows ? h : w;
        std::vector<float> line (len), out (len);
#pragma omp parallel for schedule(static) firstprivate(line, out)
        for (ptrdiff_t li = 0; li < (ptrdiff_t) lines; li++)
        {
            size_t l = (size_t) li;
            for (size_t i = 0; i < len; i++)
            {
                line[i] = (*v)[rows ? l * w + i : i * w + l];
            }
            double sum = 0;
            for (int k = -r; k <= r; k++)
            {
                sum += line[(size_t) std::min (std::max (k, 0), (int) len - 1)];
            }
            for (size_t i = 0; i < len; i++)
            {
                out[i]  = (float) (sum / (2 * r + 1));
                int add = std::min ((int) i + r + 1, (int) len - 1), sub = std::max ((int) i - r, 0);
                sum += line[(size_t) add] - line[(size_t) sub];
            }
            for (size_t i = 0; i < len; i++)
            {
                (*v)[rows ? l * w + i : i * w + l] = out[i];
            }
        }
    }

    void box_all (Plane* pl, int r)
    {
        for (size_t c = 0; c < pl->n; c++)
        {
            std::vector<float> v = channel_of (*pl, c);
            box_pass (&v, pl->w, pl->h, r, true);
            box_pass (&v, pl->w, pl->h, r, false);
            set_channel (pl, c, v);
        }
    }

    /* Motion Blur: the average along a line `distance` long at `angle` degrees, centred. */
    void motion_all (Plane* pl, float angle, float distance)
    {
        int steps = std::max (1, (int) ceilf (distance));
        float dx = cosf (angle * (float) M_PI / 180.0f), dy = -sinf (angle * (float) M_PI / 180.0f);
        std::vector<float> src = pl->px;
        size_t w = pl->w, h = pl->h, n = pl->n;
#pragma omp parallel for schedule(dynamic, 16)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
        {
            for (size_t x = 0; x < w; x++)
            {
                float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                for (int s = 0; s <= steps; s++)
                {
                    float t = ((float) s / (float) steps - 0.5f) * distance;
                    int sx = (int) lroundf ((float) x + dx * t), sy = (int) lroundf ((float) yi + dy * t);
                    sx             = std::min (std::max (sx, 0), (int) w - 1);
                    sy             = std::min (std::max (sy, 0), (int) h - 1);
                    const float* q = &src[((size_t) sy * w + (size_t) sx) * n];
                    for (size_t c = 0; c < n && c < 8; c++)
                    {
                        acc[c] += q[c];
                    }
                }
                float* o = &pl->px[((size_t) yi * w + x) * n];
                for (size_t c = 0; c < n && c < 8; c++)
                {
                    o[c] = acc[c] / (float) (steps + 1);
                }
            }
        }
    }

    /* Unsharp Mask: original + amount x (original - blurred) where the difference passes the
       threshold (levels 0..255). Alpha is left alone. */
    void unsharp (Plane* pl, float amount, float sigma, float threshold)
    {
        Plane blurred = *pl;
        gaussian_all (&blurred, sigma);
        size_t colors = pl->alpha ? pl->n - 1 : pl->n;
        size_t count  = pl->w * pl->h;
        float t       = threshold / 255.0f;
        for (size_t i = 0; i < count; i++)
        {
            for (size_t c = 0; c < colors; c++)
            {
                float o = pl->px[i * pl->n + c], d = o - blurred.px[i * pl->n + c];
                if (fabsf (d) * 2.0f < t)
                {
                    continue;
                }
                pl->px[i * pl->n + c] = std::min (std::max (o + amount * d, 0.0f), 1.0f);
            }
        }
    }

    /* High Pass: 50% gray plus the detail finer than the radius. */
    void high_pass (Plane* pl, float sigma)
    {
        Plane blurred = *pl;
        gaussian_all (&blurred, sigma);
        size_t colors = pl->alpha ? pl->n - 1 : pl->n;
        size_t count  = pl->w * pl->h;
        for (size_t i = 0; i < count; i++)
        {
            for (size_t c = 0; c < colors; c++)
            {
                float v               = pl->px[i * pl->n + c] - blurred.px[i * pl->n + c] + 0.5f;
                pl->px[i * pl->n + c] = std::min (std::max (v, 0.0f), 1.0f);
            }
        }
    }

    /* A repeatable hash noise in -1..1 for pixel i, channel c. */
    float noise_at (uint64_t i, uint64_t c, uint64_t seed)
    {
        uint64_t z = i * 0x9E3779B97F4A7C15ull + c * 0xBF58476D1CE4E5B9ull + seed;
        z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z          = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return (float) ((double) (z >> 11) / (double) (1ull << 53)) * 2.0f - 1.0f;
    }

    /* Add Noise: uniform or Gaussian noise of `amount` percent, per channel or the same for all. */
    void add_noise (Plane* pl, float amount, bool gaussian, bool mono, uint64_t seed)
    {
        size_t colors = pl->alpha ? pl->n - 1 : pl->n;
        size_t count  = pl->w * pl->h;
        float a       = amount / 100.0f;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
        {
            size_t i = (size_t) ii;
            for (size_t c = 0; c < colors; c++)
            {
                uint64_t ch = mono ? 0 : c;
                float v;
                if (gaussian)
                {
                    float u1 = (noise_at (i, ch * 2, seed) + 1.0f) * 0.5f,
                          u2 = (noise_at (i, ch * 2 + 1, seed) + 1.0f) * 0.5f;
                    v        = sqrtf (-2.0f * logf (fmaxf (u1, 1e-7f))) * cosf (2.0f * (float) M_PI * u2) * 0.5f;
                }
                else
                {
                    v = noise_at (i, ch, seed);
                }
                float* p = &pl->px[i * pl->n + c];
                *p       = std::min (std::max (*p + v * a * 0.5f, 0.0f), 1.0f);
            }
        }
    }

    /* Median: each channel replaced by the median of the square of radius r around it (on a
       256-level histogram that slides along the row). */
    void median_all (Plane* pl, int r)
    {
        size_t w = pl->w, h = pl->h, n = pl->n;
        std::vector<float> src = pl->px;
#pragma omp parallel for schedule(dynamic, 8)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
        {
            int y = (int) yi;
            for (size_t c = 0; c < n; c++)
            {
                int hist[256] = {0};
                int count     = 0;
                for (int x = -r - 1; x < (int) w; x++)
                {
                    /* Slide: add the column entering on the right, drop the one leaving on the left. */
                    int add = x + r, drop = x - r - 1;
                    for (int dy = -r; dy <= r; dy++)
                    {
                        int sy = y + dy;
                        if (sy < 0 || sy >= (int) h)
                        {
                            continue;
                        }
                        if (add >= 0 && add < (int) w)
                        {
                            hist[(int) lroundf (
                                std::min (std::max (src[((size_t) sy * w + (size_t) add) * n + c], 0.0f), 1.0f)
                                * 255.0f)]++;
                            count++;
                        }
                        if (drop >= 0 && drop < (int) w)
                        {
                            hist[(int) lroundf (
                                std::min (std::max (src[((size_t) sy * w + (size_t) drop) * n + c], 0.0f), 1.0f)
                                * 255.0f)]--;
                            count--;
                        }
                    }
                    if (x < 0 || count <= 0)
                    {
                        continue;
                    }
                    int half = count / 2, acc = 0, m = 0;
                    while (m < 255 && acc + hist[m] <= half)
                    {
                        acc += hist[m++];
                    }
                    pl->px[((size_t) y * w + (size_t) x) * n + c] = (float) m / 255.0f;
                }
            }
        }
    }

    /* The level below which `clip` of the histogram lies, and the one above which it lies. */
    void clip_levels (const std::vector<double>& hist, double clip, int* lo, int* hi)
    {
        double total = 0;
        for (size_t i = 0; i < hist.size (); i++)
        {
            total += hist[i];
        }
        double acc = 0;
        *lo        = 0;
        while (*lo < 255 && acc + hist[(size_t) *lo] <= total * clip)
        {
            acc += hist[(size_t) (*lo)++];
        }
        acc = 0;
        *hi = 255;
        while (*hi > 0 && acc + hist[(size_t) *hi] <= total * clip)
        {
            acc += hist[(size_t) (*hi)--];
        }
    }

    /* Auto Tone (each colour channel stretched on its own), Auto Contrast (all channels by the same
       stretch) and Equalize (all channels through the combined histogram's cumulative curve), from
       the pixels inside `area` of the plane at `origin`. Alpha is left alone. */
    void auto_levels (Plane* pl, Rect origin, Rect area, const char* which)
    {
        size_t colors = pl->alpha ? pl->n - 1 : pl->n;
        std::vector<std::vector<double>> hist (colors + 1, std::vector<double> (256, 0.0));
        for (size_t y = 0; y < pl->h; y++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                int32_t cx = origin.x0 + (int32_t) x, cy = origin.y0 + (int32_t) y;
                if (cx < area.x0 || cx >= area.x1 || cy < area.y0 || cy >= area.y1)
                {
                    continue;
                }
                const float* p = &pl->px[(y * pl->w + x) * pl->n];
                double wgt     = pl->alpha ? p[pl->n - 1] : 1.0;
                for (size_t c = 0; c < colors; c++)
                {
                    int v = (int) lroundf (std::min (std::max (p[c], 0.0f), 1.0f) * 255.0f);
                    hist[c][(size_t) v] += wgt;
                    hist[colors][(size_t) v] += wgt;
                }
            }
        }
        float lut[8][256];
        for (size_t c = 0; c < colors && c < 8; c++)
        {
            bool equalize                 = strcmp (which, "equalize") == 0;
            bool own                      = strcmp (which, "autoTone") == 0 || strcmp (which, "autoColor") == 0;
            const std::vector<double>& hh = own ? hist[c] : hist[colors];
            if (equalize)
            {
                double total = 0, acc = 0;
                for (int i = 0; i < 256; i++)
                {
                    total += hh[(size_t) i];
                }
                for (int i = 0; i < 256; i++)
                {
                    acc += hh[(size_t) i];
                    lut[c][i] = total > 0 ? (float) (acc / total) : (float) i / 255.0f;
                }
                continue;
            }
            int lo, hi;
            clip_levels (hh, 0.001, &lo, &hi);
            for (int i = 0; i < 256; i++)
            {
                lut[c][i] = hi > lo ? std::min (std::max ((float) (i - lo) / (float) (hi - lo), 0.0f), 1.0f)
                                    : (float) i / 255.0f;
            }
        }
        /* Auto Color also neutralises the midtones: a gamma per channel moves its median onto the
           channels' average median. */
        if (strcmp (which, "autoColor") == 0 && colors == 3)
        {
            float med[3], target = 0.0f;
            for (size_t c = 0; c < 3; c++)
            {
                double total = 0, acc = 0;
                for (int i = 0; i < 256; i++)
                {
                    total += hist[c][(size_t) i];
                }
                int m = 0;
                while (m < 255 && acc + hist[c][(size_t) m] < total / 2)
                {
                    acc += hist[c][(size_t) m++];
                }
                med[c] = lut[c][m];
                target += med[c] / 3.0f;
            }
            for (size_t c = 0; c < 3; c++)
            {
                float t = std::min (std::max (med[c], 0.02f), 0.98f), want = std::min (std::max (target, 0.02f), 0.98f);
                float inv = logf (want) / logf (t);
                for (int i = 0; i < 256; i++)
                {
                    lut[c][i] = powf (lut[c][i], inv);
                }
            }
        }
        size_t count = pl->w * pl->h;
        for (size_t i = 0; i < count; i++)
        {
            for (size_t c = 0; c < colors && c < 8; c++)
            {
                float* p = &pl->px[i * pl->n + c];
                float f  = std::min (std::max (*p, 0.0f), 1.0f) * 255.0f;
                int a = (int) floorf (f), b = std::min (a + 1, 255);
                float t = f - (float) a;
                *p      = lut[c][a] * (1.0f - t) + lut[c][b] * t;
            }
        }
    }

    /* Maximum (grow = true) or Minimum: each channel the extreme of the square of radius r, done
       as a row pass then a column pass (the square's extreme is separable). */
    void extreme_pass (std::vector<float>* v, size_t w, size_t h, int r, bool rows, bool grow)
    {
        size_t len = rows ? w : h, lines = rows ? h : w;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t li = 0; li < (ptrdiff_t) lines; li++)
        {
            size_t l = (size_t) li;
            std::vector<float> line (len);
            for (size_t i = 0; i < len; i++)
            {
                line[i] = (*v)[rows ? l * w + i : i * w + l];
            }
            for (size_t i = 0; i < len; i++)
            {
                int a = std::max ((int) i - r, 0), b = std::min ((int) i + r, (int) len - 1);
                float m = line[(size_t) a];
                for (int j = a + 1; j <= b; j++)
                {
                    m = grow ? std::max (m, line[(size_t) j]) : std::min (m, line[(size_t) j]);
                }
                (*v)[rows ? l * w + i : i * w + l] = m;
            }
        }
    }

    void extreme_all (Plane* pl, int r, bool grow)
    {
        for (size_t c = 0; c < pl->n; c++)
        {
            std::vector<float> v = channel_of (*pl, c);
            extreme_pass (&v, pl->w, pl->h, r, true, grow);
            extreme_pass (&v, pl->w, pl->h, r, false, grow);
            set_channel (pl, c, v);
        }
    }

    /* Mosaic: square cells of `cell` pixels (aligned to the canvas origin) filled with their average. */
    void mosaic (Plane* pl, Rect origin, int cell)
    {
        size_t w = pl->w, h = pl->h, n = pl->n;
        int x0 = origin.x0, y0 = origin.y0;
        int cy0 = (int) floorf ((float) y0 / (float) cell) * cell;
        for (int cy = cy0; cy < y0 + (int) h; cy += cell)
        {
            int cx0 = (int) floorf ((float) x0 / (float) cell) * cell;
            for (int cx = cx0; cx < x0 + (int) w; cx += cell)
            {
                int ax = std::max (cx - x0, 0), bx = std::min (cx + cell - x0, (int) w);
                int ay = std::max (cy - y0, 0), by = std::min (cy + cell - y0, (int) h);
                double acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                int cnt       = 0;
                for (int y = ay; y < by; y++)
                {
                    for (int x = ax; x < bx; x++)
                    {
                        for (size_t c = 0; c < n && c < 8; c++)
                        {
                            acc[c] += pl->px[((size_t) y * w + (size_t) x) * n + c];
                        }
                        cnt++;
                    }
                }
                if (cnt == 0)
                {
                    continue;
                }
                for (int y = ay; y < by; y++)
                {
                    for (int x = ax; x < bx; x++)
                    {
                        for (size_t c = 0; c < n && c < 8; c++)
                        {
                            pl->px[((size_t) y * w + (size_t) x) * n + c] = (float) (acc[c] / cnt);
                        }
                    }
                }
            }
        }
    }

    /* Offset: the pixels inside `area` moved by (dx, dy), wrapping around its edges. */
    void offset_wrap (Plane* pl, Rect origin, Rect area, int dx, int dy)
    {
        std::vector<float> src = pl->px;
        int aw = (int) area.width (), ah = (int) area.height ();
        size_t n = pl->n;
        for (int y = area.y0; y < area.y1; y++)
        {
            for (int x = area.x0; x < area.x1; x++)
            {
                int sx         = area.x0 + (((x - area.x0 - dx) % aw) + aw) % aw;
                int sy         = area.y0 + (((y - area.y0 - dy) % ah) + ah) % ah;
                const float* q = &src[((size_t) (sy - origin.y0) * pl->w + (size_t) (sx - origin.x0)) * n];
                float* o       = &pl->px[((size_t) (y - origin.y0) * pl->w + (size_t) (x - origin.x0)) * n];
                for (size_t c = 0; c < n; c++)
                {
                    o[c] = q[c];
                }
            }
        }
    }

    /* The value of channel c at (x, y), clamped to the plane. */
    float at_clamped (const Plane& pl, const std::vector<float>& src, int x, int y, size_t c)
    {
        x = std::min (std::max (x, 0), (int) pl.w - 1);
        y = std::min (std::max (y, 0), (int) pl.h - 1);
        return src[((size_t) y * pl.w + (size_t) x) * pl.n + c];
    }

    /* Find Edges: per colour channel, white where flat and dark along edges (an inverted Sobel). */
    void find_edges (Plane* pl)
    {
        std::vector<float> src = pl->px;
        size_t colors          = pl->alpha ? pl->n - 1 : pl->n;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            int y = (int) yi;
            for (int x = 0; x < (int) pl->w; x++)
            {
                for (size_t c = 0; c < colors; c++)
                {
                    float gx = at_clamped (*pl, src, x + 1, y - 1, c) + 2 * at_clamped (*pl, src, x + 1, y, c)
                             + at_clamped (*pl, src, x + 1, y + 1, c) - at_clamped (*pl, src, x - 1, y - 1, c)
                             - 2 * at_clamped (*pl, src, x - 1, y, c) - at_clamped (*pl, src, x - 1, y + 1, c);
                    float gy = at_clamped (*pl, src, x - 1, y + 1, c) + 2 * at_clamped (*pl, src, x, y + 1, c)
                             + at_clamped (*pl, src, x + 1, y + 1, c) - at_clamped (*pl, src, x - 1, y - 1, c)
                             - 2 * at_clamped (*pl, src, x, y - 1, c) - at_clamped (*pl, src, x + 1, y - 1, c);
                    pl->px[((size_t) y * pl->w + (size_t) x) * pl->n + c] =
                        1.0f - std::min (sqrtf (gx * gx + gy * gy) * 0.5f, 1.0f);
                }
            }
        }
    }

    /* Emboss: 50% gray plus the brightness difference across `height` pixels at `angle`, times
       `amount`; every colour channel gets the same gray. */
    void emboss (Plane* pl, float angle, float height, float amount)
    {
        std::vector<float> src = pl->px;
        size_t colors          = pl->alpha ? pl->n - 1 : pl->n;
        float dx = cosf (angle * (float) M_PI / 180.0f) * height, dy = -sinf (angle * (float) M_PI / 180.0f) * height;
        int ox = (int) lroundf (dx), oy = (int) lroundf (dy);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            int y = (int) yi;
            for (int x = 0; x < (int) pl->w; x++)
            {
                float a = 0, b = 0;
                for (size_t c = 0; c < colors; c++)
                {
                    a += at_clamped (*pl, src, x + ox, y + oy, c);
                    b += at_clamped (*pl, src, x - ox, y - oy, c);
                }
                float v = std::min (std::max (0.5f + (a - b) / (float) colors * amount, 0.0f), 1.0f);
                for (size_t c = 0; c < colors; c++)
                {
                    pl->px[((size_t) y * pl->w + (size_t) x) * pl->n + c] = v;
                }
            }
        }
    }

    /* Solarize: tones above the middle are inverted. */
    void solarize (Plane* pl)
    {
        size_t colors = pl->alpha ? pl->n - 1 : pl->n;
        for (size_t i = 0; i < pl->w * pl->h; i++)
        {
            for (size_t c = 0; c < colors; c++)
            {
                float* p = &pl->px[i * pl->n + c];
                if (*p > 0.5f)
                {
                    *p = 1.0f - *p;
                }
            }
        }
    }

    float fparam (const Value& p, const char* key, float fallback)
    {
        double v;
        return param_f64 (p, key, &v) && isfinite (v) ? (float) v : fallback;
    }

    /* A number in lo..hi (fallback when absent). */
    bool frange (
        const Value& p, const char* key, float lo, float hi, float fallback, float* out, const char* cmd, Error* err)
    {
        *out = fallback;
        if (!param_present (p, key))
        {
            return true;
        }
        double v;
        if (!param_f64 (p, key, &v) || !isfinite (v) || v < lo || v > hi)
        {
            return bad_params (err, cmd, "`%s` must be a number in %g..%g", key, lo, hi);
        }
        *out = (float) v;
        return true;
    }

    /* One of `ids` (fallback when absent). */
    bool choice (const Value& p, const char* key, const char* const* ids, int n, int fallback, int* out,
        const char* cmd, Error* err)
    {
        *out = fallback;
        std::string v;
        if (!param_present (p, key))
        {
            return true;
        }
        if (param_str (p, key, &v))
        {
            for (int i = 0; i < n; i++)
            {
                if (v == ids[i])
                {
                    *out = i;
                    return true;
                }
            }
        }
        std::string list;
        for (int i = 0; i < n; i++)
        {
            list += std::string (i ? ", " : "") + ids[i];
        }
        return bad_params (err, cmd, "`%s` must be one of %s", key, list.c_str ());
    }

    uint64_t seed_param (const Value& p)
    {
        int64_t seed = 1;
        param_i64 (p, "seed", &seed);
        return (uint64_t) seed;
    }

    size_t colors_of (const Plane& pl)
    {
        return pl.alpha ? pl.n - 1 : pl.n;
    }

    float luma (const float* rgb)
    {
        return 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    }

    /* The colour channels only (alpha kept). */
    void gaussian_colors (Plane* pl, float sigma)
    {
        for (size_t c = 0; c < colors_of (*pl); c++)
        {
            std::vector<float> v = channel_of (*pl, c);
            compose::gaussian (v.data (), pl->w, pl->h, sigma);
            set_channel (pl, c, v);
        }
    }

    /* Plane value at fractional (x, y): bilinear, clamped to the plane or wrapped around it. */
    void sample_bilinear (const Plane& pl, const std::vector<float>& src, float x, float y, bool wrap, float* out)
    {
        int w = (int) pl.w, h = (int) pl.h;
        float fx = floorf (x), fy = floorf (y);
        float tx = x - fx, ty = y - fy;
        int x0 = (int) fx, y0 = (int) fy;
        int xs[2] = {x0, x0 + 1}, ys[2] = {y0, y0 + 1};
        for (int i = 0; i < 2; i++)
        {
            if (wrap)
            {
                xs[i] = ((xs[i] % w) + w) % w;
                ys[i] = ((ys[i] % h) + h) % h;
            }
            else
            {
                xs[i] = std::min (std::max (xs[i], 0), w - 1);
                ys[i] = std::min (std::max (ys[i], 0), h - 1);
            }
        }
        const float *a = &src[((size_t) ys[0] * pl.w + (size_t) xs[0]) * pl.n],
                    *b = &src[((size_t) ys[0] * pl.w + (size_t) xs[1]) * pl.n];
        const float *c = &src[((size_t) ys[1] * pl.w + (size_t) xs[0]) * pl.n],
                    *d = &src[((size_t) ys[1] * pl.w + (size_t) xs[1]) * pl.n];
        for (size_t k = 0; k < pl.n; k++)
        {
            float top = a[k] + (b[k] - a[k]) * tx, bottom = c[k] + (d[k] - c[k]) * tx;
            out[k] = top + (bottom - top) * ty;
        }
    }

    float rand01 (uint64_t a, uint64_t b, uint64_t seed)
    {
        return (noise_at (a, b, seed) + 1.0f) * 0.5f;
    }

    float smooth (float t)
    {
        return t * t * (3.0f - 2.0f * t);
    }

    /* Smoothly interpolated lattice noise in 0..1. */
    float value_noise (float x, float y, uint64_t seed)
    {
        float fx = floorf (x), fy = floorf (y);
        int64_t ix = (int64_t) fx, iy = (int64_t) fy;
        float tx = smooth (x - fx), ty = smooth (y - fy);
        float a = rand01 ((uint64_t) ix, (uint64_t) iy, seed), b = rand01 ((uint64_t) (ix + 1), (uint64_t) iy, seed);
        float c   = rand01 ((uint64_t) ix, (uint64_t) (iy + 1), seed),
              d   = rand01 ((uint64_t) (ix + 1), (uint64_t) (iy + 1), seed);
        float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
        return top + (bottom - top) * ty;
    }

    /* Fractal sum of `octaves` noise layers, each half the size and weight of the last (0..1). */
    float fbm (float x, float y, int octaves, uint64_t seed)
    {
        float sum = 0.0f, amp = 0.5f, norm = 0.0f;
        for (int o = 0; o < octaves; o++)
        {
            sum += value_noise (x, y, seed + (uint64_t) o * 7919u) * amp;
            norm += amp;
            x *= 2.0f;
            y *= 2.0f;
            amp *= 0.5f;
        }
        return norm > 0.0f ? sum / norm : 0.5f;
    }

    void plane_to_rgba (const Plane& pl, const FilterCtx& cx, std::vector<float>* out)
    {
        size_t count = pl.w * pl.h;
        out->resize (count * 4);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) count; i++)
        {
            raster::to_rgba (cx.fmt, &pl.px[(size_t) i * pl.n], cx.cmyk, &(*out)[(size_t) i * 4]);
        }
    }

    /* Back from RGBA; the layer keeps its own alpha unless it has one to take. */
    void plane_from_rgba (Plane* pl, const FilterCtx& cx, const std::vector<float>& rgba)
    {
        size_t count = pl->w * pl->h;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) count; i++)
        {
            float px[raster::MAX_CHANNELS];
            float c[4] = {clamp01 (rgba[(size_t) i * 4]), clamp01 (rgba[(size_t) i * 4 + 1]),
                clamp01 (rgba[(size_t) i * 4 + 2]), clamp01 (rgba[(size_t) i * 4 + 3])};
            raster::from_rgba (cx.fmt, c, cx.cmyk, px);
            for (size_t k = 0; k < pl->n; k++)
            {
                pl->px[(size_t) i * pl->n + k] = px[k];
            }
        }
    }

    std::vector<float> plane_luma (const Plane& pl, const FilterCtx& cx)
    {
        size_t count = pl.w * pl.h;
        std::vector<float> out (count);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) count; i++)
        {
            float rgba[4];
            raster::to_rgba (cx.fmt, &pl.px[(size_t) i * pl.n], cx.cmyk, rgba);
            out[(size_t) i] = luma (rgba);
        }
        return out;
    }

    void color_in (const FilterCtx& cx, const float rgba[4], float* out)
    {
        raster::from_rgba (cx.fmt, rgba, cx.cmyk, out);
    }

    /* ---- The first filters as specs ---- */

    bool run_gaussian (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float r;
        if (!frange (p, "radius", 0.1f, 1000.0f, 1.0f, &r, cmd, err))
        {
            return false;
        }
        gaussian_all (pl, r);
        return true;
    }

    bool run_box (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float r;
        if (!frange (p, "radius", 1.0f, 2000.0f, 1.0f, &r, cmd, err))
        {
            return false;
        }
        box_all (pl, (int) lroundf (r));
        return true;
    }

    bool run_motion (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float dist, angle;
        if (!frange (p, "distance", 1.0f, 2000.0f, 10.0f, &dist, cmd, err)
            || !frange (p, "angle", -360.0f, 360.0f, 0.0f, &angle, cmd, err))
        {
            return false;
        }
        motion_all (pl, angle, dist);
        return true;
    }

    bool run_blur (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        gaussian_all (pl, strcmp (cmd, "filter.blur.blur") == 0 ? 0.7f : 1.6f);
        return true;
    }

    bool run_unsharp (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float amount, r, t;
        if (!frange (p, "amount", 1.0f, 500.0f, 100.0f, &amount, cmd, err)
            || !frange (p, "radius", 0.1f, 1000.0f, 1.0f, &r, cmd, err)
            || !frange (p, "threshold", 0.0f, 255.0f, 0.0f, &t, cmd, err))
        {
            return false;
        }
        unsharp (pl, amount / 100.0f, r, t);
        return true;
    }

    bool run_sharpen (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        if (strcmp (cmd, "filter.sharpen.sharpenEdges") == 0)
        {
            unsharp (pl, 0.8f, 0.8f, 12.0f);
        }
        else
        {
            unsharp (pl, strcmp (cmd, "filter.sharpen.sharpen") == 0 ? 0.6f : 1.4f, 0.8f, 0.0f);
        }
        return true;
    }

    bool run_add_noise (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float amount;
        if (!frange (p, "amount", 0.1f, 400.0f, 12.5f, &amount, cmd, err))
        {
            return false;
        }
        const char* const DIST[2] = {"uniform", "gaussian"};
        int dist;
        if (!choice (p, "distribution", DIST, 2, 0, &dist, cmd, err))
        {
            return false;
        }
        bool mono = false;
        param_bool (p, "monochromatic", &mono);
        add_noise (pl, amount, dist == 1, mono, seed_param (p));
        return true;
    }

    bool run_high_pass (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float r;
        if (!frange (p, "radius", 0.1f, 1000.0f, 10.0f, &r, cmd, err))
        {
            return false;
        }
        high_pass (pl, r);
        return true;
    }

    bool run_median (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float r = 1.0f;
        if (strcmp (cmd, "filter.noise.median") == 0 && !frange (p, "radius", 1.0f, 100.0f, 1.0f, &r, cmd, err))
        {
            return false;
        }
        median_all (pl, (int) lroundf (r));
        return true;
    }

    bool run_auto_levels (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* which = strcmp (cmd, "image.autoTone") == 0     ? "autoTone"
                          : strcmp (cmd, "image.autoContrast") == 0 ? "autoContrast"
                          : strcmp (cmd, "image.autoColor") == 0    ? "autoColor"
                                                                    : "equalize";
        auto_levels (pl, cx.src, cx.area, which);
        return true;
    }

    bool run_extreme (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float r;
        if (!frange (p, "radius", 1.0f, 500.0f, 1.0f, &r, cmd, err))
        {
            return false;
        }
        extreme_all (pl, (int) lroundf (r), strcmp (cmd, "filter.other.maximum") == 0);
        return true;
    }

    bool run_mosaic (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float cell;
        if (!frange (p, "cellSize", 2.0f, 200.0f, 10.0f, &cell, cmd, err))
        {
            return false;
        }
        mosaic (pl, cx.src, (int) lroundf (cell));
        return true;
    }

    bool run_offset (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        offset_wrap (pl, cx.src, cx.area, (int) lroundf (fparam (p, "horizontal", 0.0f)),
            (int) lroundf (fparam (p, "vertical", 0.0f)));
        return true;
    }

    bool run_find_edges (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        find_edges (pl);
        return true;
    }

    bool run_emboss (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float height, amount, angle;
        if (!frange (p, "height", 1.0f, 100.0f, 3.0f, &height, cmd, err)
            || !frange (p, "amount", 1.0f, 500.0f, 100.0f, &amount, cmd, err)
            || !frange (p, "angle", -360.0f, 360.0f, 135.0f, &angle, cmd, err))
        {
            return false;
        }
        emboss (pl, angle, height, amount / 100.0f);
        return true;
    }

    bool run_solarize (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        solarize (pl);
        return true;
    }

    /* In Photoshop's menu order within each submenu; submenus appear in the order first seen. */
    const FilterSpec BASIC_FILTERS[] = {
        {"filter.blur.blur", "Blur", "Filter/Blur", "{}", run_blur, true, Reach::Params},
        {"filter.blur.blurMore", "Blur More", "Filter/Blur", "{}", run_blur, true, Reach::Params},
        {"filter.blur.boxBlur", "Box Blur…", "Filter/Blur", "{\"radius\":1..2000=1}", run_box, true, Reach::Params},
        {"filter.blur.gaussianBlur", "Gaussian Blur…", "Filter/Blur", "{\"radius\":0.1..1000=1}", run_gaussian, true,
            Reach::Params},
        {"filter.blur.motionBlur", "Motion Blur…", "Filter/Blur", "{\"angle\":-360..360=0,\"distance\":1..2000=10}",
            run_motion, true, Reach::Params},
        {"filter.noise.addNoise", "Add Noise…", "Filter/Noise",
            "{\"amount\":0.1..400=12.5,\"distribution\":\"uniform|gaussian\",\"monochromatic\":bool=false,\"seed\":int="
            "1}",
            run_add_noise, false, Reach::None},
        {"filter.noise.despeckle", "Despeckle", "Filter/Noise", "{}", run_median, true, Reach::Params},
        {"filter.noise.median", "Median…", "Filter/Noise", "{\"radius\":1..100=1}", run_median, true, Reach::Params},
        {"filter.pixelate.mosaic", "Mosaic…", "Filter/Pixelate", "{\"cellSize\":2..200=10}", run_mosaic, true,
            Reach::Params},
        {"filter.sharpen.sharpen", "Sharpen", "Filter/Sharpen", "{}", run_sharpen, false, Reach::Params},
        {"filter.sharpen.sharpenEdges", "Sharpen Edges", "Filter/Sharpen", "{}", run_sharpen, false, Reach::Params},
        {"filter.sharpen.sharpenMore", "Sharpen More", "Filter/Sharpen", "{}", run_sharpen, false, Reach::Params},
        {"filter.sharpen.unsharpMask", "Unsharp Mask…", "Filter/Sharpen",
            "{\"amount\":1..500=100,\"radius\":0.1..1000=1,\"threshold\":0..255=0}", run_unsharp, false, Reach::Params},
        {"filter.stylize.emboss", "Emboss…", "Filter/Stylize",
            "{\"angle\":-360..360=135,\"height\":1..100=3,\"amount\":1..500=100}", run_emboss, false, Reach::Params},
        {"filter.stylize.findEdges", "Find Edges", "Filter/Stylize", "{}", run_find_edges, false, Reach::Params},
        {"filter.stylize.solarize", "Solarize", "Filter/Stylize", "{}", run_solarize, false, Reach::None},
        {"filter.other.highPass", "High Pass…", "Filter/Other", "{\"radius\":0.1..1000=10}", run_high_pass, false,
            Reach::Params},
        {"filter.other.maximum", "Maximum…", "Filter/Other", "{\"radius\":1..500=1}", run_extreme, true, Reach::Params},
        {"filter.other.minimum", "Minimum…", "Filter/Other", "{\"radius\":1..500=1}", run_extreme, true, Reach::Params},
        {"filter.other.offset", "Offset…", "Filter/Other", "{\"horizontal\":px=0,\"vertical\":px=0} (wraps around)",
            run_offset, false, Reach::None},
        {"image.autoTone", "Auto Tone", "Image", "{}", run_auto_levels, false, Reach::None},
        {"image.autoContrast", "Auto Contrast", "Image", "{}", run_auto_levels, false, Reach::None},
        {"image.autoColor", "Auto Color", "Image", "{}", run_auto_levels, false, Reach::None},
        {"image.adjustments.equalize", "Equalize", "Image/Adjustments", "{}", run_auto_levels, false, Reach::None},
    };

    void basic_filters (std::vector<FilterSpec>* out)
    {
        out->insert (out->end (), BASIC_FILTERS, BASIC_FILTERS + sizeof BASIC_FILTERS / sizeof BASIC_FILTERS[0]);
    }

    /* True when `menu` is `top` itself (or, for the Image menu, under it). */
    bool in_menu (const char* menu, const char* top)
    {
        if (strcmp (menu, top) == 0)
        {
            return true;
        }
        return strcmp (top, "Image") == 0 && strncmp (menu, "Image/", 6) == 0;
    }

    /* Every filter, the submenus in Photoshop's order. */
    void all_filters (std::vector<FilterSpec>* out)
    {
        out->clear ();
        std::vector<FilterSpec> groups[8];
        basic_filters (&groups[0]);
        blur_filters (&groups[1]);
        distort_filters (&groups[2]);
        pixelate_filters (&groups[3]);
        render_filters (&groups[4]);
        stylize_filters (&groups[5]);
        other_filters (&groups[6]);
        tone_filters (&groups[7]);
        const char* const ORDER[] = {"Filter/Blur", "Filter/Blur Gallery", "Filter/Distort", "Filter/Noise",
            "Filter/Pixelate", "Filter/Render", "Filter/Sharpen", "Filter/Stylize", "Filter/Video", "Filter/Other",
            "Filter/Artistic", "Filter/Sketch", "Filter/Texture", "Image"};
        for (size_t o = 0; o < sizeof ORDER / sizeof ORDER[0]; o++)
        {
            for (int g = 0; g < 8; g++)
            {
                for (size_t i = 0; i < groups[g].size (); i++)
                {
                    if (in_menu (groups[g][i].menu, ORDER[o]))
                    {
                        out->push_back (groups[g][i]);
                    }
                }
            }
        }
    }

    bool find_filter (const char* id, FilterSpec* out)
    {
        std::vector<FilterSpec> all;
        all_filters (&all);
        for (size_t i = 0; i < all.size (); i++)
        {
            if (strcmp (all[i].id, id) == 0)
            {
                *out = all[i];
                return true;
            }
        }
        return false;
    }

    /* How far around the area a filter reads, from its size settings. */
    int params_reach (const Value& p)
    {
        float reach = std::max (std::max (fparam (p, "radius", 0.0f) * 3.0f, fparam (p, "distance", 0.0f)),
            std::max (fparam (p, "height", 0.0f), fparam (p, "cellSize", 0.0f)));
        return (int) ceilf (std::min (reach, 4000.0f)) + 4;
    }

    /* ---- Smart filters ---- */

    /* `after` laid over `before` with the filter's blend mode and opacity (the result in `after`). */
    void blend_filter (const raster::Surface& before, raster::Surface* after, color::BlendMode mode, float opacity,
        const color::CmykSpace* cmyk)
    {
        Rect r = before.content_bounds ().unite (after->content_bounds ());
        if (r.is_empty ())
        {
            return;
        }
        color::PixelFormat fmt = after->format ();
        size_t n = fmt.channels (), count = (size_t) r.width () * r.height ();
        std::vector<float> a (count * 4), b (count * 4), out (count * n);
        before.read_rgba (r, cmyk, a.data ());
        after->read_rgba (r, cmyk, b.data ());
#pragma omp parallel for schedule(static)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) count; i++)
        {
            float o[4];
            color::composite (mode, &a[(size_t) i * 4], &b[(size_t) i * 4], opacity, o);
            raster::from_rgba (fmt, o, cmyk, &out[(size_t) i * n]);
        }
        after->write_region (r, out.data (), out.size ());
        after->prune ();
    }

    /* The smart layer's rendered contents through its enabled filters, bottom to top. */
    void apply_smart_filters (
        const doc::Document& d, doc::Layer* l, const color::CmykSpace* cmyk, const float fg[4], const float bg[4])
    {
        if (!l->smart.filters_enabled)
        {
            return;
        }
        doc::IdSource ids;
        doc::Document t = doc::Document::make (ids, "filters", d.size, d.mode, d.depth);
        doc::Layer work = doc::Layer::make (ids, "work", doc::LayerKind::Raster);
        work.pixels     = l->pixels;
        t.layers.push_back (work);
        doc::LayerId wid = t.layers[0].id;
        for (size_t i = 0; i < l->smart.filters.size (); i++)
        {
            const doc::SmartFilter& f = l->smart.filters[i];
            if (!f.enabled || f.id.empty ())
            {
                continue;
            }
            Value p = Value::parse (f.params, nullptr, false);
            if (p.is_discarded () || !p.is_object ())
            {
                p = Value::object ();
            }
            raster::Surface before = t.layers[0].pixels;
            Error e;
            if (!apply_filter (&t, wid, f.id.c_str (), p, cmyk, fg, bg, &e))
            {
                continue;
            }
            if (f.blend != color::BlendMode::Normal || f.opacity < 1.0f)
            {
                blend_filter (before, &t.layers[0].pixels, f.blend, f.opacity, cmyk);
            }
        }
        l->pixels = t.layers[0].pixels;
    }

    /* The smart layer drawn again: contents, then filters. */
    void render_smart_filtered (
        const doc::Document& d, doc::Layer* l, const color::CmykSpace* cmyk, const float fg[4], const float bg[4])
    {
        if (!io::render_smart_layer (d, l, cmyk))
        {
            return;
        }
        memcpy (l->smart.rendered, l->smart.quad, sizeof l->smart.quad);
        apply_smart_filters (d, l, cmyk, fg, bg);
    }

    /* The settings kept with a smart filter: the params without the target layer. */
    std::string filter_params_text (const Value& p)
    {
        Value v = p.is_object () ? p : Value::object ();
        v.erase ("layer");
        return v.dump ();
    }

    /* A filter run on a smart object joins its smart filters (on top). */
    bool add_smart_filter (doc::Document* d, doc::LayerId id, const FilterSpec& spec, const Value& p,
        const color::CmykSpace* cmyk, const float fg[4], const float bg[4], Error* err)
    {
        doc::Layer* l               = d->layer_mut (id);
        const doc::SmartContents* c = d->smart_contents (l->smart.uuid);
        if (c == NULL || !c->doc)
        {
            return fail (err, ErrorKind::Other,
                "the smart object's contents can't be rendered in this build, so filters can't be added");
        }
        doc::SmartFilter f;
        f.id              = spec.id;
        std::string label = spec.label;
        size_t dots       = label.find ("…");
        f.name            = dots != std::string::npos ? label.substr (0, dots) : label;
        f.params          = filter_params_text (p);
        f.enabled         = true;
        f.blend           = color::BlendMode::Normal;
        f.opacity         = 1.0f;
        l->smart.filters.push_back (f);
        render_smart_filtered (*d, l, cmyk, fg, bg);
        return true;
    }

    /* New settings for smart filter `index` (the filter dialog's OK, and its preview). */
    bool set_smart_filter (doc::Document* d, doc::LayerId id, int index, const Value& p, const color::CmykSpace* cmyk,
        const float fg[4], const float bg[4], Error* err)
    {
        doc::Layer* l = d->layer_mut (id);
        if (l == NULL || l->kind != doc::LayerKind::Smart)
        {
            return fail (err, ErrorKind::Other, "not a smart object");
        }
        if (index < 0 || (size_t) index >= l->smart.filters.size ())
        {
            return fail (err, ErrorKind::Other, "no smart filter %d", index);
        }
        l->smart.filters[(size_t) index].params = filter_params_text (p);
        render_smart_filtered (*d, l, cmyk, fg, bg);
        return true;
    }

    /* Runs filter `cmd` on layer `id` of `d` (the document is changed in place; the UI uses this
       on a copy for its preview). */
    bool apply_filter (doc::Document* d, doc::LayerId id, const char* cmd, const Value& p, const color::CmykSpace* cmyk,
        const float fg[4], const float bg[4], Error* err)
    {
        FilterSpec spec;
        if (!find_filter (cmd, &spec))
        {
            return bad_params (err, cmd, "unknown filter");
        }
        doc::Layer* l = d->layer_mut (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        if (l->kind == doc::LayerKind::Smart)
        {
            return add_smart_filter (d, id, spec, p, cmyk, fg, bg, err);
        }
        if (!check_pixels_unlocked (*d, id, err))
        {
            return false;
        }
        raster::Surface* surf = l->surface_mut ();
        if (surf == NULL)
        {
            return fail (err, ErrorKind::Other, "filters apply to pixel layers; the active layer is %s %s layer",
                doc::layer_kind_article (l->kind), doc::layer_kind_name (l->kind));
        }
        Rect area = d->bounds ();
        if (d->has_selection)
        {
            area = d->selection.content_bounds ().intersect (area);
        }
        if (area.is_empty ())
        {
            return true;
        }
        /* Read a margin around the area so neighbourhood filters see what lies just outside it. */
        Rect src = area;
        if (spec.reach == Reach::Canvas)
        {
            src = d->bounds ();
        }
        else if (spec.reach == Reach::Params)
        {
            src = area.inflate (params_reach (p)).intersect (d->bounds ());
        }
        color::PixelFormat fmt = surf->format ();
        Plane pl;
        pl.w     = src.width ();
        pl.h     = src.height ();
        pl.n     = fmt.channels ();
        pl.alpha = fmt.alpha;
        if (!surf->read_region (src, &pl.px))
        {
            return fail (err, ErrorKind::Other, "the layer is too large to filter");
        }
        std::vector<float> before = pl.px;
        FilterCtx cx;
        cx.src    = src;
        cx.area   = area;
        cx.canvas = d->bounds ();
        cx.fmt    = fmt;
        cx.cmyk   = cmyk;
        memcpy (cx.fg, fg, sizeof cx.fg);
        memcpy (cx.bg, bg, sizeof cx.bg);
        if (spec.premultiplied)
        {
            premultiply (&pl, true);
        }
        if (!spec.run (&pl, cx, p, cmd, err))
        {
            return false;
        }
        if (spec.premultiplied)
        {
            premultiply (&pl, false);
        }
        /* Only the selected pixels take the result, weighted by the selection; a locked
           transparency keeps the old alpha. */
        bool keep_alpha = pl.alpha && d->effective_locks (id).transparency;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl.h; yi++)
        {
            size_t y = (size_t) yi;
            for (size_t x = 0; x < pl.w; x++)
            {
                int32_t px = src.x0 + (int32_t) x, py = src.y0 + (int32_t) y;
                float k = px >= area.x0 && px < area.x1 && py >= area.y0 && py < area.y1 ? 1.0f : 0.0f;
                if (k > 0.0f && d->has_selection)
                {
                    k = d->selection.sample_channel (px, py, 0);
                }
                size_t at = (y * pl.w + x) * pl.n;
                for (size_t c = 0; c < pl.n; c++)
                {
                    float v = std::min (std::max (pl.px[at + c], 0.0f), 1.0f);
                    if (keep_alpha && c == pl.n - 1)
                    {
                        v = before[at + c];
                    }
                    pl.px[at + c] = before[at + c] + (v - before[at + c]) * k;
                }
            }
        }
        surf->write_region (src, pl.px.data (), pl.px.size ());
        surf->prune ();
        return true;
    }

    /* Every filter command: the running command names the filter. Random filters run without a
       seed get a fresh one. */
    bool cmd_run_filter (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = s.running ();
        FilterSpec spec;
        if (cmd == NULL || !find_filter (cmd, &spec))
        {
            return fail (err, ErrorKind::Other, "unknown filter");
        }
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Value params = p.is_object () ? p : Value::object ();
        if (!param_present (params, "seed"))
        {
            params["seed"] = (int64_t) (s.ids.fresh () & 0x7fffffff);
        }
        if (!prepare_filter_params (s, cmd, &params, err))
        {
            return false;
        }
        Edit e (s);
        if (e.doc () == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!apply_filter (e.doc (), id, cmd, params, s.cmyk, s.tools.foreground, s.tools.background, err))
        {
            return false;
        }
        std::string label = spec.label;
        size_t dots       = label.find ("…");
        if (dots != std::string::npos)
        {
            label = label.substr (0, dots);
        }
        return e.commit (label.c_str (), err);
    }

    void register_filter_commands (std::vector<CommandSpec>* out)
    {
        std::vector<FilterSpec> all;
        all_filters (&all);
        for (size_t i = 0; i < all.size (); i++)
        {
            const char* shortcut = NULL;
            if (strcmp (all[i].id, "image.autoTone") == 0)
            {
                shortcut = "Cmd+Shift+L";
            }
            if (strcmp (all[i].id, "image.autoContrast") == 0)
            {
                shortcut = "Cmd+Alt+Shift+L";
            }
            if (strcmp (all[i].id, "image.autoColor") == 0)
            {
                shortcut = "Cmd+Shift+B";
            }
            CommandSpec c = {
                all[i].id, all[i].label, all[i].menu, shortcut, all[i].params, has_layer, cmd_run_filter, true};
            out->push_back (c);
        }
    }

} // namespace engine
