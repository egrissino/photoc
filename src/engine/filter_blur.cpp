#include "compose/masks.h"
#include "engine/filter_ops.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

namespace engine
{

    /* Blur > Average, Lens Blur, Radial Blur, Smart Blur, Surface Blur; Blur Gallery > Field,
       Iris, Tilt-Shift and Spin Blur (set by numbers, without the on-canvas pins); Sharpen > Smart
       Sharpen; Noise > Dust & Scratches and Reduce Noise. Positions are fractions of the area being
       filtered (the selection's bounds), 0.5 its centre. */

    /* The point (fx, fy) of the area, in plane coordinates. */
    void area_point (const FilterCtx& cx, float fx, float fy, float* x, float* y)
    {
        *x = (float) (cx.area.x0 - cx.src.x0) + fx * (float) cx.area.width ();
        *y = (float) (cx.area.y0 - cx.src.y0) + fy * (float) cx.area.height ();
    }

    float smoothstep (float a, float b, float x)
    {
        if (b <= a)
        {
            return x < a ? 0.0f : 1.0f;
        }
        float t = clamp01 ((x - a) / (b - a));
        return t * t * (3.0f - 2.0f * t);
    }

    /* Each pixel mixed toward `blurred` by mask m (0 keeps it). */
    void mix_planes (Plane* pl, const Plane& blurred, const std::vector<float>& m)
    {
        size_t count = pl->w * pl->h;
        for (size_t i = 0; i < count; i++)
        {
            for (size_t c = 0; c < pl->n; c++)
            {
                float& v = pl->px[i * pl->n + c];
                v += (blurred.px[i * pl->n + c] - v) * m[i];
            }
        }
    }

    bool run_average (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        double acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        size_t count  = pl->w * pl->h;
        for (size_t i = 0; i < count; i++)
        {
            for (size_t c = 0; c < pl->n && c < 8; c++)
            {
                acc[c] += pl->px[i * pl->n + c];
            }
        }
        for (size_t i = 0; i < count; i++)
        {
            for (size_t c = 0; c < pl->n && c < 8; c++)
            {
                pl->px[i * pl->n + c] = (float) (acc[c] / (double) count);
            }
        }
        return true;
    }

    /* A disc of radius r averaged, each row of the disc a span of prefix sums. */
    void disc_all (Plane* pl, float radius)
    {
        int r = (int) ceilf (radius);
        if (r < 1)
        {
            return;
        }
        int w = (int) pl->w, h = (int) pl->h;
        std::vector<int> half ((size_t) (2 * r + 1));
        double area = 0;
        for (int dy = -r; dy <= r; dy++)
        {
            half[(size_t) (dy + r)] = (int) floorf (sqrtf (fmaxf (radius * radius - (float) (dy * dy), 0.0f)));
            area += 2 * half[(size_t) (dy + r)] + 1;
        }
        for (size_t c = 0; c < pl->n; c++)
        {
            std::vector<double> pre ((size_t) (w + 1) * (size_t) h);
            for (int y = 0; y < h; y++)
            {
                double run                         = 0;
                pre[(size_t) y * (size_t) (w + 1)] = 0;
                for (int x = 0; x < w; x++)
                {
                    run += pl->px[((size_t) y * pl->w + (size_t) x) * pl->n + c];
                    pre[(size_t) y * (size_t) (w + 1) + (size_t) x + 1] = run;
                }
            }
#pragma omp parallel for schedule(dynamic, 8)
            for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
            {
                int y = (int) yi;
                for (int x = 0; x < w; x++)
                {
                    double sum = 0, n = 0;
                    for (int dy = -r; dy <= r; dy++)
                    {
                        int sy = std::min (std::max (y + dy, 0), h - 1);
                        int hw = half[(size_t) (dy + r)];
                        int a = std::max (x - hw, 0), b = std::min (x + hw, w - 1);
                        sum += pre[(size_t) sy * (size_t) (w + 1) + (size_t) b + 1]
                             - pre[(size_t) sy * (size_t) (w + 1) + (size_t) a];
                        n += b - a + 1;
                    }
                    pl->px[((size_t) y * pl->w + (size_t) x) * pl->n + c] = (float) (sum / std::max (n, 1.0));
                }
            }
        }
        (void) area;
    }

    /* Lens Blur: a disc ("iris") blur; pixels brighter than the threshold are boosted first, so
       highlights bloom into bright discs as through a lens. */
    bool run_lens_blur (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float radius, bright, thresh;
        if (!frange (p, "radius", 0.0f, 100.0f, 15.0f, &radius, cmd, err)
            || !frange (p, "brightness", 0.0f, 100.0f, 0.0f, &bright, cmd, err)
            || !frange (p, "threshold", 0.0f, 255.0f, 255.0f, &thresh, cmd, err))
        {
            return false;
        }
        size_t colors = colors_of (*pl), count = pl->w * pl->h;
        if (bright > 0.0f && colors > 0)
        {
            float t = thresh / 255.0f, gain = 1.0f + bright / 100.0f * 4.0f;
            for (size_t i = 0; i < count; i++)
            {
                float* q = &pl->px[i * pl->n];
                float m  = 0;
                for (size_t c = 0; c < colors; c++)
                {
                    m += q[c];
                }
                if (m / (float) colors >= t)
                {
                    for (size_t c = 0; c < colors; c++)
                    {
                        q[c] *= gain;
                    }
                }
            }
        }
        disc_all (pl, radius);
        return true;
    }

    /* Radial Blur: Spin (along circles around the centre, `amount` scaled to degrees of arc) or Zoom
       (along lines toward it). */
    bool run_radial_blur (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const METHODS[2] = {"spin", "zoom"};
        float amount, fx, fy;
        int method;
        if (!frange (p, "amount", 1.0f, 100.0f, 10.0f, &amount, cmd, err)
            || !choice (p, "method", METHODS, 2, 0, &method, cmd, err)
            || !frange (p, "centerX", 0.0f, 1.0f, 0.5f, &fx, cmd, err)
            || !frange (p, "centerY", 0.0f, 1.0f, 0.5f, &fy, cmd, err))
        {
            return false;
        }
        float ccx, ccy;
        area_point (cx, fx, fy, &ccx, &ccy);
        std::vector<float> src = pl->px;
        size_t n               = pl->n;
        float arc = amount * 0.7f * (float) M_PI / 180.0f, zoom = amount / 200.0f;
#pragma omp parallel for schedule(dynamic, 8)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            float q[raster::MAX_CHANNELS], acc[raster::MAX_CHANNELS];
            for (size_t x = 0; x < pl->w; x++)
            {
                float dx = (float) x - ccx, dy = (float) yi - ccy, d = sqrtf (dx * dx + dy * dy);
                float len = method == 0 ? d * arc : d * zoom;
                int steps = std::min (std::max ((int) ceilf (len), 1), 96);
                for (size_t c = 0; c < n; c++)
                {
                    acc[c] = 0.0f;
                }
                for (int s = 0; s <= steps; s++)
                {
                    float t = (float) s / (float) steps - 0.5f, sx, sy;
                    if (method == 0)
                    {
                        float a = t * arc, ca = cosf (a), sa = sinf (a);
                        sx = ccx + dx * ca - dy * sa;
                        sy = ccy + dx * sa + dy * ca;
                    }
                    else
                    {
                        float k = 1.0f - (t + 0.5f) * zoom;
                        sx      = ccx + dx * k;
                        sy      = ccy + dy * k;
                    }
                    sample_bilinear (*pl, src, sx, sy, false, q);
                    for (size_t c = 0; c < n; c++)
                    {
                        acc[c] += q[c];
                    }
                }
                for (size_t c = 0; c < n; c++)
                {
                    pl->px[((size_t) yi * pl->w + x) * n + c] = acc[c] / (float) (steps + 1);
                }
            }
        }
        return true;
    }

    /* The window offsets a large neighbourhood is sampled at (a sparse grid past a dozen steps). */
    int window_step (int r)
    {
        return std::max (1, r / 6);
    }

    /* Surface Blur: each channel the average of the neighbours within `radius` whose value differs
       by less than the threshold, weighted down as the difference grows. Edges stay sharp. */
    bool run_surface_blur (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float radius, thresh;
        if (!frange (p, "radius", 1.0f, 100.0f, 5.0f, &radius, cmd, err)
            || !frange (p, "threshold", 2.0f, 255.0f, 15.0f, &thresh, cmd, err))
        {
            return false;
        }
        int r = (int) lroundf (radius), step = window_step (r);
        float t                = thresh / 255.0f * 2.5f;
        std::vector<float> src = pl->px;
        size_t colors = colors_of (*pl), n = pl->n;
        int w = (int) pl->w, h = (int) pl->h;
#pragma omp parallel for schedule(dynamic, 8)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
        {
            int y = (int) yi;
            for (int x = 0; x < w; x++)
            {
                const float* o = &src[((size_t) y * pl->w + (size_t) x) * n];
                for (size_t c = 0; c < colors; c++)
                {
                    float sum = 0, wsum = 0;
                    for (int dy = -r; dy <= r; dy += step)
                    {
                        int sy = std::min (std::max (y + dy, 0), h - 1);
                        for (int dx = -r; dx <= r; dx += step)
                        {
                            int sx   = std::min (std::max (x + dx, 0), w - 1);
                            float v  = src[((size_t) sy * pl->w + (size_t) sx) * n + c];
                            float wt = 1.0f - fabsf (v - o[c]) / t;
                            if (wt <= 0.0f)
                            {
                                continue;
                            }
                            sum += v * wt;
                            wsum += wt;
                        }
                    }
                    if (wsum > 0.0f)
                    {
                        pl->px[((size_t) y * pl->w + (size_t) x) * n + c] = sum / wsum;
                    }
                }
            }
        }
        return true;
    }

    /* Smart Blur: neighbours count only when their brightness is within the threshold. Edge Only
       draws the edges white on black, Overlay Edge white over the blurred image. */
    bool run_smart_blur (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const MODES[3] = {"normal", "edgeOnly", "overlayEdge"};
        float radius, thresh;
        int mode;
        if (!frange (p, "radius", 0.1f, 100.0f, 3.0f, &radius, cmd, err)
            || !frange (p, "threshold", 0.1f, 100.0f, 25.0f, &thresh, cmd, err)
            || !choice (p, "mode", MODES, 3, 0, &mode, cmd, err))
        {
            return false;
        }
        std::vector<float> lum = plane_luma (*pl, cx);
        float t                = thresh / 255.0f;
        int r = std::max (1, (int) lroundf (radius)), step = window_step (r), w = (int) pl->w, h = (int) pl->h;
        size_t n = pl->n, colors = colors_of (*pl);
        std::vector<float> src = pl->px;
        if (mode == 0 || mode == 2)
        {
#pragma omp parallel for schedule(dynamic, 8)
            for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
            {
                int y = (int) yi;
                float acc[raster::MAX_CHANNELS];
                for (int x = 0; x < w; x++)
                {
                    float l0 = lum[(size_t) y * pl->w + (size_t) x], cnt = 0;
                    for (size_t c = 0; c < n; c++)
                    {
                        acc[c] = 0;
                    }
                    for (int dy = -r; dy <= r; dy += step)
                    {
                        int sy = std::min (std::max (y + dy, 0), h - 1);
                        for (int dx = -r; dx <= r; dx += step)
                        {
                            int sx = std::min (std::max (x + dx, 0), w - 1);
                            if (fabsf (lum[(size_t) sy * pl->w + (size_t) sx] - l0) > t)
                            {
                                continue;
                            }
                            const float* q = &src[((size_t) sy * pl->w + (size_t) sx) * n];
                            for (size_t c = 0; c < n; c++)
                            {
                                acc[c] += q[c];
                            }
                            cnt += 1;
                        }
                    }
                    for (size_t c = 0; c < n && cnt > 0; c++)
                    {
                        pl->px[((size_t) y * pl->w + (size_t) x) * n + c] = acc[c] / cnt;
                    }
                }
            }
        }
        if (mode == 0)
        {
            return true;
        }
        float white[raster::MAX_CHANNELS], black[raster::MAX_CHANNELS];
        float wc[4] = {1, 1, 1, 1}, bc[4] = {0, 0, 0, 1};
        color_in (cx, wc, white);
        color_in (cx, bc, black);
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                float l0  = lum[(size_t) y * pl->w + (size_t) x];
                bool edge = false;
                if (x + 1 < w && fabsf (lum[(size_t) y * pl->w + (size_t) x + 1] - l0) > t)
                {
                    edge = true;
                }
                if (y + 1 < h && fabsf (lum[(size_t) (y + 1) * pl->w + (size_t) x] - l0) > t)
                {
                    edge = true;
                }
                float* o = &pl->px[((size_t) y * pl->w + (size_t) x) * n];
                float a  = pl->alpha ? o[n - 1] : 1.0f;
                if (edge)
                {
                    for (size_t c = 0; c < colors; c++)
                    {
                        o[c] = white[c] * a;
                    }
                }
                else if (mode == 1)
                {
                    for (size_t c = 0; c < colors; c++)
                    {
                        o[c] = black[c] * a;
                    }
                }
            }
        }
        return true;
    }

    /* Field Blur with one pin: the whole area blurred evenly. */
    bool run_field_blur (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float blur;
        if (!frange (p, "blur", 0.0f, 500.0f, 15.0f, &blur, cmd, err))
        {
            return false;
        }
        if (blur > 0.0f)
        {
            gaussian_all (pl, blur * 0.5f);
        }
        return true;
    }

    /* Iris Blur: sharp inside the ellipse, easing into the full blur at its edge. */
    bool run_iris_blur (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float blur, fx, fy, rx, ry, feather;
        if (!frange (p, "blur", 0.0f, 500.0f, 15.0f, &blur, cmd, err)
            || !frange (p, "centerX", 0.0f, 1.0f, 0.5f, &fx, cmd, err)
            || !frange (p, "centerY", 0.0f, 1.0f, 0.5f, &fy, cmd, err)
            || !frange (p, "radiusX", 0.01f, 2.0f, 0.3f, &rx, cmd, err)
            || !frange (p, "radiusY", 0.01f, 2.0f, 0.3f, &ry, cmd, err)
            || !frange (p, "feather", 0.0f, 100.0f, 50.0f, &feather, cmd, err))
        {
            return false;
        }
        if (blur <= 0.0f)
        {
            return true;
        }
        float ccx, ccy;
        area_point (cx, fx, fy, &ccx, &ccy);
        float ax = rx * (float) cx.area.width (), ay = ry * (float) cx.area.height ();
        std::vector<float> m (pl->w * pl->h);
        for (size_t y = 0; y < pl->h; y++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                float dx = ((float) x - ccx) / ax, dy = ((float) y - ccy) / ay;
                m[y * pl->w + x] = smoothstep (1.0f - feather / 100.0f, 1.0f, sqrtf (dx * dx + dy * dy));
            }
        }
        Plane blurred = *pl;
        gaussian_all (&blurred, blur * 0.5f);
        mix_planes (pl, blurred, m);
        return true;
    }

    /* Tilt-Shift: a sharp band through the centre at `angle`, blurring past the transition. */
    bool run_tilt_shift (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float blur, fy, angle, focus, transition;
        if (!frange (p, "blur", 0.0f, 500.0f, 15.0f, &blur, cmd, err)
            || !frange (p, "centerY", 0.0f, 1.0f, 0.5f, &fy, cmd, err)
            || !frange (p, "angle", -90.0f, 90.0f, 0.0f, &angle, cmd, err)
            || !frange (p, "focus", 0.0f, 100.0f, 10.0f, &focus, cmd, err)
            || !frange (p, "transition", 0.0f, 100.0f, 15.0f, &transition, cmd, err))
        {
            return false;
        }
        if (blur <= 0.0f)
        {
            return true;
        }
        float ccx, ccy;
        area_point (cx, 0.5f, fy, &ccx, &ccy);
        float a = angle * (float) M_PI / 180.0f, nx = -sinf (a), ny = cosf (a), hgt = (float) cx.area.height ();
        float f0 = focus / 100.0f * hgt, f1 = f0 + transition / 100.0f * hgt;
        std::vector<float> m (pl->w * pl->h);
        for (size_t y = 0; y < pl->h; y++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                m[y * pl->w + x] = smoothstep (f0, f1, fabsf (((float) x - ccx) * nx + ((float) y - ccy) * ny));
            }
        }
        Plane blurred = *pl;
        gaussian_all (&blurred, blur * 0.5f);
        mix_planes (pl, blurred, m);
        return true;
    }

    /* Spin Blur: the ellipse spun by `angle` degrees, fading out at its edge. */
    bool run_spin_blur (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float angle, fx, fy, rx, ry;
        if (!frange (p, "angle", 0.0f, 360.0f, 15.0f, &angle, cmd, err)
            || !frange (p, "centerX", 0.0f, 1.0f, 0.5f, &fx, cmd, err)
            || !frange (p, "centerY", 0.0f, 1.0f, 0.5f, &fy, cmd, err)
            || !frange (p, "radiusX", 0.01f, 2.0f, 0.3f, &rx, cmd, err)
            || !frange (p, "radiusY", 0.01f, 2.0f, 0.3f, &ry, cmd, err))
        {
            return false;
        }
        float ccx, ccy;
        area_point (cx, fx, fy, &ccx, &ccy);
        float ax = rx * (float) cx.area.width (), ay = ry * (float) cx.area.height ();
        std::vector<float> src = pl->px;
        size_t n               = pl->n;
        float arc              = angle * (float) M_PI / 180.0f;
#pragma omp parallel for schedule(dynamic, 8)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            float q[raster::MAX_CHANNELS], acc[raster::MAX_CHANNELS];
            for (size_t x = 0; x < pl->w; x++)
            {
                float dx = (float) x - ccx, dy = (float) yi - ccy;
                float re = sqrtf ((dx / ax) * (dx / ax) + (dy / ay) * (dy / ay));
                float k  = 1.0f - smoothstep (0.85f, 1.0f, re);
                if (k <= 0.0f)
                {
                    continue;
                }
                float span = arc * k, len = sqrtf (dx * dx + dy * dy) * span;
                int steps = std::min (std::max ((int) ceilf (len), 1), 128);
                for (size_t c = 0; c < n; c++)
                {
                    acc[c] = 0.0f;
                }
                for (int s = 0; s <= steps; s++)
                {
                    float t = ((float) s / (float) steps - 0.5f) * span, ca = cosf (t), sa = sinf (t);
                    sample_bilinear (*pl, src, ccx + dx * ca - dy * sa, ccy + dx * sa + dy * ca, false, q);
                    for (size_t c = 0; c < n; c++)
                    {
                        acc[c] += q[c];
                    }
                }
                for (size_t c = 0; c < n; c++)
                {
                    pl->px[((size_t) yi * pl->w + x) * n + c] = acc[c] / (float) (steps + 1);
                }
            }
        }
        return true;
    }

    /* Smart Sharpen: the detail lost to a Gaussian, lens (disc) or motion blur added back, skipping
       differences below the noise reduction level. */
    bool run_smart_sharpen (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const REMOVE[3] = {"gaussian", "lens", "motion"};
        float amount, radius, noise, angle;
        int remove;
        if (!frange (p, "amount", 1.0f, 500.0f, 150.0f, &amount, cmd, err)
            || !frange (p, "radius", 0.1f, 64.0f, 1.0f, &radius, cmd, err)
            || !frange (p, "reduceNoise", 0.0f, 100.0f, 10.0f, &noise, cmd, err)
            || !frange (p, "angle", -360.0f, 360.0f, 0.0f, &angle, cmd, err)
            || !choice (p, "remove", REMOVE, 3, 1, &remove, cmd, err))
        {
            return false;
        }
        Plane blurred = *pl;
        if (remove == 0)
        {
            gaussian_all (&blurred, radius);
        }
        else if (remove == 1)
        {
            disc_all (&blurred, radius);
        }
        else
        {
            motion_all (&blurred, angle, radius * 2.0f);
        }
        size_t colors = colors_of (*pl), count = pl->w * pl->h;
        float t = noise / 100.0f * 0.1f, k = amount / 100.0f;
        for (size_t i = 0; i < count; i++)
        {
            for (size_t c = 0; c < colors; c++)
            {
                float o = pl->px[i * pl->n + c], d = o - blurred.px[i * pl->n + c];
                float g               = fabsf (d) <= t ? 0.0f : 1.0f;
                pl->px[i * pl->n + c] = clamp01 (o + k * d * g);
            }
        }
        return true;
    }

    /* Dust & Scratches: the median, where it differs from the pixel by more than the threshold. */
    bool run_dust (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float radius, thresh;
        if (!frange (p, "radius", 1.0f, 100.0f, 1.0f, &radius, cmd, err)
            || !frange (p, "threshold", 0.0f, 255.0f, 0.0f, &thresh, cmd, err))
        {
            return false;
        }
        std::vector<float> orig = pl->px;
        median_all (pl, (int) lroundf (radius));
        float t = thresh / 255.0f;
        for (size_t i = 0; i < pl->px.size (); i++)
        {
            if (fabsf (pl->px[i] - orig[i]) <= t)
            {
                pl->px[i] = orig[i];
            }
        }
        return true;
    }

    /* Reduce Noise: brightness smoothed edge-aware (strength, with the original detail kept by
       Preserve Details), colour noise blurred out of the chroma, then sharpened back. */
    bool run_reduce_noise (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float strength, details, color, sharpen;
        if (!frange (p, "strength", 0.0f, 10.0f, 6.0f, &strength, cmd, err)
            || !frange (p, "preserveDetails", 0.0f, 100.0f, 60.0f, &details, cmd, err)
            || !frange (p, "reduceColorNoise", 0.0f, 100.0f, 45.0f, &color, cmd, err)
            || !frange (p, "sharpenDetails", 0.0f, 100.0f, 25.0f, &sharpen, cmd, err))
        {
            return false;
        }
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        size_t count = pl->w * pl->h;
        Plane ycc;
        ycc.w     = pl->w;
        ycc.h     = pl->h;
        ycc.n     = 3;
        ycc.alpha = false;
        ycc.px.resize (count * 3);
        for (size_t i = 0; i < count; i++)
        {
            const float* c    = &rgba[i * 4];
            float y           = luma (c);
            ycc.px[i * 3]     = y;
            ycc.px[i * 3 + 1] = (c[2] - y) * 0.564f + 0.5f;
            ycc.px[i * 3 + 2] = (c[0] - y) * 0.713f + 0.5f;
        }
        /* Brightness: an edge-aware average, mixed back toward the original by Preserve Details. */
        if (strength > 0.0f)
        {
            Plane lum;
            lum.w     = ycc.w;
            lum.h     = ycc.h;
            lum.n     = 1;
            lum.alpha = false;
            lum.px    = channel_of (ycc, 0);
            Value sp  = Value{{"radius", 2.0f}, {"threshold", 2.0f + strength * 4.0f}};
            run_surface_blur (&lum, cx, sp, cmd, err);
            float keep = details / 100.0f * 0.7f;
            for (size_t i = 0; i < count; i++)
            {
                ycc.px[i * 3] = lum.px[i] + (ycc.px[i * 3] - lum.px[i]) * keep;
            }
        }
        if (color > 0.0f)
        {
            for (size_t c = 1; c < 3; c++)
            {
                std::vector<float> v = channel_of (ycc, c);
                compose::gaussian (v.data (), ycc.w, ycc.h, color / 100.0f * 4.0f);
                set_channel (&ycc, c, v);
            }
        }
        if (sharpen > 0.0f)
        {
            Plane lum;
            lum.w     = ycc.w;
            lum.h     = ycc.h;
            lum.n     = 1;
            lum.alpha = false;
            lum.px    = channel_of (ycc, 0);
            unsharp (&lum, sharpen / 100.0f, 1.0f, 0.0f);
            set_channel (&ycc, 0, lum.px);
        }
        for (size_t i = 0; i < count; i++)
        {
            float y = ycc.px[i * 3], cb = (ycc.px[i * 3 + 1] - 0.5f) / 0.564f, cr = (ycc.px[i * 3 + 2] - 0.5f) / 0.713f;
            float r = y + cr, b = y + cb, g = (y - 0.299f * r - 0.114f * b) / 0.587f;
            rgba[i * 4]     = r;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = b;
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    const FilterSpec BLUR_FILTERS[] = {
        {"filter.blur.average", "Average", "Filter/Blur", "{}", run_average, true, Reach::None},
        {"filter.blur.lensBlur", "Lens Blur…", "Filter/Blur",
            "{\"radius\":0..100=15,\"brightness\":0..100=0,\"threshold\":0..255=255}", run_lens_blur, true,
            Reach::Params},
        {"filter.blur.radialBlur", "Radial Blur…", "Filter/Blur",
            "{\"amount\":1..100=10,\"method\":\"spin|zoom\",\"centerX\":0..1=0.5,\"centerY\":0..1=0.5}",
            run_radial_blur, true, Reach::Canvas},
        {"filter.blur.smartBlur", "Smart Blur…", "Filter/Blur",
            "{\"radius\":0.1..100=3,\"threshold\":0.1..100=25,\"mode\":\"normal|edgeOnly|overlayEdge\"}",
            run_smart_blur, true, Reach::Params},
        {"filter.blur.surfaceBlur", "Surface Blur…", "Filter/Blur", "{\"radius\":1..100=5,\"threshold\":2..255=15}",
            run_surface_blur, true, Reach::Params},
        {"filter.blurGallery.fieldBlur", "Field Blur…", "Filter/Blur Gallery", "{\"blur\":0..500=15}", run_field_blur,
            true, Reach::Canvas},
        {"filter.blurGallery.irisBlur", "Iris Blur…", "Filter/Blur Gallery",
            "{\"blur\":0..500=15,\"centerX\":0..1=0.5,\"centerY\":0..1=0.5,\"radiusX\":0.01..2=0.3,\"radiusY\":0.01..2="
            "0.3,\"feather\":0..100=50}",
            run_iris_blur, true, Reach::Canvas},
        {"filter.blurGallery.tiltShift", "Tilt-Shift…", "Filter/Blur Gallery",
            "{\"blur\":0..500=15,\"centerY\":0..1=0.5,\"angle\":-90..90=0,\"focus\":0..100=10,\"transition\":0..100="
            "15}",
            run_tilt_shift, true, Reach::Canvas},
        {"filter.blurGallery.spinBlur", "Spin Blur…", "Filter/Blur Gallery",
            "{\"angle\":0..360=15,\"centerX\":0..1=0.5,\"centerY\":0..1=0.5,\"radiusX\":0.01..2=0.3,\"radiusY\":0.01.."
            "2=0.3}",
            run_spin_blur, true, Reach::Canvas},
        {"filter.noise.dustAndScratches", "Dust & Scratches…", "Filter/Noise",
            "{\"radius\":1..100=1,\"threshold\":0..255=0}", run_dust, true, Reach::Params},
        {"filter.noise.reduceNoise", "Reduce Noise…", "Filter/Noise",
            "{\"strength\":0..10=6,\"preserveDetails\":0..100=60,\"reduceColorNoise\":0..100=45,\"sharpenDetails\":0.."
            "100=25}",
            run_reduce_noise, false, Reach::Canvas},
        {"filter.sharpen.smartSharpen", "Smart Sharpen…", "Filter/Sharpen",
            "{\"amount\":1..500=150,\"radius\":0.1..64=1,\"remove\":\"gaussian|lens|motion\"=\"lens\",\"angle\":-360.."
            "360=0,\"reduceNoise\":0..100=10}",
            run_smart_sharpen, false, Reach::Params},
    };

    void blur_filters (std::vector<FilterSpec>* out)
    {
        out->insert (out->end (), BLUR_FILTERS, BLUR_FILTERS + sizeof BLUR_FILTERS / sizeof BLUR_FILTERS[0]);
    }

} // namespace engine
