#include "color/convert.h"
#include "compose/adjust.h"
#include "compose/compose.h"
#include "engine/filter.h"
#include "engine/filter_ops.h"
#include "engine/params.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

namespace engine
{

    using geom::Rect;

    /* Image > Adjustments that need more than a per-pixel formula: Shadows/Highlights, Replace
       Color and Match Color run as filters (so they follow the selection, Fade and smart objects);
       HDR Toning flattens the image first, as in Photoshop. All work on straight RGBA in 0..1. The
       formulas are approximations of Photoshop's behaviour; Shadows/Highlights' constants were
       measured on Photoshop. */

    float tone_luma (const float* c)
    {
        return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    }

    /* ---- Shadows/Highlights ---- */

    const float SH_SPATIAL = 1.1424f;
    const float SH_GUIDE   = 1.3559f;
    const float SH_RANGE   = 1.0152f;
    const int SH_LEVELS    = 17;

    void blur_plane (std::vector<float>* v, size_t w, size_t h, size_t n, float sigma)
    {
        Plane pl;
        pl.w     = w;
        pl.h     = h;
        pl.n     = n;
        pl.alpha = false;
        pl.px.swap (*v);
        gaussian_all (&pl, sigma);
        v->swap (pl.px);
    }

    /* Each pixel's neighbourhood brightness: a bilateral average of radius `radius` (a bilateral
       grid of 17 tonal levels, blurred on a grid shrunk by a quarter of the radius). */
    std::vector<float> sh_neighbourhood (const std::vector<float>& lum, size_t w, size_t h, float radius)
    {
        std::vector<float> guide = lum;
        blur_plane (&guide, w, h, 1, SH_GUIDE);
        float sigma = SH_SPATIAL * radius;
        if (sigma < 0.3f)
        {
            return guide;
        }
        size_t d  = std::max ((size_t) floorf (sigma / 4.0f), (size_t) 1);
        size_t rw = (w + d - 1) / d, rh = (h + d - 1) / d, cells = rw * rh;
        float step = 1.0f / (float) (SH_LEVELS - 1);
        std::vector<float> avg ((size_t) SH_LEVELS * cells);
#pragma omp parallel for schedule(dynamic)
        for (int k = 0; k < SH_LEVELS; k++)
        {
            float g = (float) k * step;
            std::vector<float> buf (2 * cells, 0.0f);
            for (size_t i = 0; i < lum.size (); i++)
            {
                float l   = lum[i];
                float wgt = fmaxf (1.0f - fabsf (l - g) / SH_RANGE, 0.0f);
                size_t c  = 2 * ((i / w) / d * rw + (i % w) / d);
                buf[c] += wgt * l;
                buf[c + 1] += wgt;
            }
            blur_plane (&buf, rw, rh, 2, sigma / (float) d);
            for (size_t j = 0; j < cells; j++)
            {
                avg[(size_t) k * cells + j] = buf[2 * j + 1] > 1e-6f ? buf[2 * j] / buf[2 * j + 1] : g;
            }
        }
        std::vector<float> out (lum.size ());
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
        {
            size_t y  = (size_t) yi;
            float fy  = fminf (fmaxf (((float) y + 0.5f) / (float) d - 0.5f, 0.0f), (float) (rh - 1));
            size_t y0 = (size_t) floorf (fy), y1 = std::min (y0 + 1, rh - 1);
            float ty = fy - floorf (fy);
            for (size_t x = 0; x < w; x++)
            {
                float fx  = fminf (fmaxf (((float) x + 0.5f) / (float) d - 0.5f, 0.0f), (float) (rw - 1));
                size_t x0 = (size_t) floorf (fx), x1 = std::min (x0 + 1, rw - 1);
                float tx = fx - floorf (fx);
                float t  = fminf (fminf (fmaxf (guide[y * w + x], 0.0f), 1.0f) / step, (float) (SH_LEVELS - 1) - 1e-4f);
                size_t k = (size_t) floorf (t);
                float f  = t - (float) k;
                float v[2];
                for (int s = 0; s < 2; s++)
                {
                    const float* a = &avg[(k + (size_t) s) * cells];
                    float top      = a[y0 * rw + x0] + (a[y0 * rw + x1] - a[y0 * rw + x0]) * tx;
                    float bot      = a[y1 * rw + x0] + (a[y1 * rw + x1] - a[y1 * rw + x0]) * tx;
                    v[s]           = top + (bot - top) * ty;
                }
                out[y * w + x] = v[0] * (1.0f - f) + v[1] * f;
            }
        }
        return out;
    }

    /* The tonal width's falloff: one minus the cumulative of a beta density whose shape follows
       Tonal Width (measured), indexed by level. */
    const float SH_MASK_BETA[20][2] = {{1.9468f, 22.9366f}, {1.7770f, 18.3471f}, {1.6403f, 14.7559f},
        {1.5270f, 11.9346f}, {1.4269f, 9.6467f}, {1.3470f, 7.8430f}, {1.2807f, 6.4038f}, {1.2240f, 5.2384f},
        {1.1772f, 4.2967f}, {1.1373f, 3.5350f}, {1.1021f, 2.9109f}, {1.0755f, 2.4186f}, {1.0508f, 2.0189f},
        {1.0307f, 1.7034f}, {1.0140f, 1.4583f}, {1.0015f, 1.2758f}, {1.0f, 1.1403f}, {1.0f, 1.0510f}, {1.0f, 1.0002f},
        {1.0f, 1.0f}};

    void sh_tonal_mask (float tone, float out[256])
    {
        float t       = fminf (fmaxf (tone, 0.0f), 95.0f) / 5.0f;
        int i         = std::min ((int) floorf (t), 18);
        float f       = t - (float) i;
        double a      = SH_MASK_BETA[i][0] + (SH_MASK_BETA[i + 1][0] - SH_MASK_BETA[i][0]) * f;
        double b      = SH_MASK_BETA[i][1] + (SH_MASK_BETA[i + 1][1] - SH_MASK_BETA[i][1]) * f;
        const int SUB = 16, n = 255 * SUB;
        double cum[256];
        cum[0]     = 0.0;
        double acc = 0.0;
        for (int j = 0; j < n; j++)
        {
            double x = ((double) j + 0.5) / n;
            acc += exp ((a - 1.0) * log (x) + (b - 1.0) * log (1.0 - x));
            if ((j + 1) % SUB == 0)
            {
                cum[(j + 1) / SUB] = acc;
            }
        }
        double total = fmax (acc, 1e-300);
        for (int k = 0; k < 256; k++)
        {
            out[k] = (float) (1.0 - cum[k] / total);
        }
    }

    float sh_strength (float a)
    {
        float u = powf (fminf (fmaxf (a, 0.0f), 100.0f) / 100.0f, 0.6216f);
        return u <= 0.3f ? u / 0.3f : (1.0f + 20.0f * u) / 7.0f;
    }

    /* The lift curve: 1 - (1 - v)^(2^n), blended between whole n by the strength's fraction. */
    float sh_lift (float s, float v)
    {
        int n    = std::min (std::max ((int) floorf (s), 0), 2);
        float t  = fminf (fmaxf (s - (float) n, 0.0f), 1.0f);
        float c  = fminf (fmaxf (1.0f - v, 0.0f), 1.0f);
        float lo = powf (c, (float) (1 << n)), hi = lo * lo;
        return 1.0f - ((1.0f - t) * lo + t * hi);
    }

    float sh_interp (const float (*table)[2], int n, float x)
    {
        if (x <= table[0][0])
        {
            return table[0][1];
        }
        for (int i = 0; i + 1 < n; i++)
        {
            if (x <= table[i + 1][0])
            {
                return table[i][1]
                     + (table[i + 1][1] - table[i][1]) * (x - table[i][0]) / (table[i + 1][0] - table[i][0]);
            }
        }
        return table[n - 1][1];
    }

    const float SH_COLOR_UP[12][2] = {{0.0f, 0.0f}, {1.0f, 0.1566f}, {2.0f, 0.2271f}, {5.0f, 0.3776f}, {10.0f, 0.5653f},
        {20.0f, 0.8669f}, {30.0f, 1.1299f}, {40.0f, 1.3748f}, {50.0f, 1.6093f}, {60.0f, 1.8375f}, {75.0f, 2.1721f},
        {100.0f, 2.7193f}};
    const float SH_COLOR_DOWN[9][2] = {{0.0f, 0.0f}, {1.0f, -0.0145f}, {5.0f, -0.0709f}, {10.0f, -0.1373f},
        {20.0f, -0.2570f}, {30.0f, -0.3612f}, {50.0f, -0.5312f}, {75.0f, -0.6866f}, {100.0f, -0.7968f}};
    const float SH_MID_K[11][2]     = {{-100.0f, -0.6955f}, {-75.0f, -0.5873f}, {-50.0f, -0.4429f}, {-25.0f, -0.2514f},
            {-10.0f, -0.1088f}, {0.0f, 0.0f}, {10.0f, 0.1206f}, {25.0f, 0.3262f}, {50.0f, 0.7423f}, {75.0f, 1.2635f},
            {100.0f, 1.9032f}};
    const float SH_MID_P[11][2]     = {{-100.0f, 2.267f}, {-75.0f, 2.362f}, {-50.0f, 2.492f}, {-25.0f, 2.667f},
            {-10.0f, 2.803f}, {0.0f, 2.9f}, {10.0f, 3.016f}, {25.0f, 3.216f}, {50.0f, 3.625f}, {75.0f, 4.160f},
            {100.0f, 4.850f}};

    float sh_midtone (float m, float y)
    {
        if (m == 0.0f)
        {
            return y;
        }
        float k = sh_interp (SH_MID_K, 11, m), p = sh_interp (SH_MID_P, 11, m);
        return fminf (fmaxf (y + k * (y - 0.5f) * powf (fmaxf (4.0f * y * (1.0f - y), 0.0f), p), 0.0f), 1.0f);
    }

    size_t sh_level (float v)
    {
        return (size_t) lroundf (fminf (fmaxf (v, 0.0f), 1.0f) * 255.0f);
    }

    /* Black and white clip: the new luminance stretched so that the given percentages of the
       opaque pixels clip. */
    void sh_clip (std::vector<float>* y2, const std::vector<float>& px, float black, float white)
    {
        if (black <= 0.0f && white <= 0.0f)
        {
            return;
        }
        std::vector<uint32_t> hist (4096, 0);
        uint32_t n = 0;
        for (size_t i = 0; i < y2->size (); i++)
        {
            if (px[i * 4 + 3] <= 0.0f)
            {
                continue;
            }
            hist[(size_t) lroundf (fminf (fmaxf ((*y2)[i], 0.0f), 1.0f) * 4095.0f)]++;
            n++;
        }
        if (n == 0)
        {
            return;
        }
        float lo = 0.0f, hi = 1.0f;
        for (int pass = 0; pass < 2; pass++)
        {
            float pct = pass == 0 ? black : white;
            if (pct <= 0.0f)
            {
                continue;
            }
            uint32_t target = (uint32_t) (fminf (pct, 50.0f) / 100.0f * (float) n), acc = 0;
            float found = pass == 0 ? 1.0f : 0.0f;
            for (int k = 0; k < 4096; k++)
            {
                int i = pass == 0 ? k : 4095 - k;
                acc += hist[(size_t) i];
                if (acc > target)
                {
                    found = (float) i / 4095.0f;
                    break;
                }
            }
            if (pass == 0)
            {
                lo = found;
            }
            else
            {
                hi = found;
            }
        }
        if (hi - lo < 1e-3f)
        {
            return;
        }
        for (size_t i = 0; i < y2->size (); i++)
        {
            (*y2)[i] = fminf (fmaxf (((*y2)[i] - lo) / (hi - lo), 0.0f), 1.0f);
        }
    }

    struct ShadowsHighlights
    {
        float shadow_amount;
        float shadow_tone;
        float shadow_radius;
        float highlight_amount;
        float highlight_tone;
        float highlight_radius;
        float color;
        float midtone;
        float black_clip;
        float white_clip;
    };

    /* Lift the shadows and pull down the highlights by each pixel's neighbourhood brightness, then
       midtone contrast, clipping, and the colour correction (saturation where tones moved). */
    void shadows_highlights (std::vector<float>* rgba, size_t w, size_t h, const ShadowsHighlights& p)
    {
        std::vector<float>& px = *rgba;
        size_t count           = w * h;
        if (count == 0)
        {
            return;
        }
        std::vector<float> lum (count);
        for (size_t i = 0; i < count; i++)
        {
            const float* q = &px[i * 4];
            lum[i]         = (fmaxf (fmaxf (q[0], q[1]), q[2]) + tone_luma (q)) / 2.0f;
        }
        std::vector<float> ls, lh;
        if (p.shadow_amount > 0.0f)
        {
            ls = sh_neighbourhood (lum, w, h, p.shadow_radius);
        }
        if (p.highlight_amount > 0.0f)
        {
            lh = !ls.empty () && fabsf (p.highlight_radius - p.shadow_radius) < 1e-3f
                   ? ls
                   : sh_neighbourhood (lum, w, h, p.highlight_radius);
        }
        float ms[256], mh[256];
        sh_tonal_mask (p.shadow_tone, ms);
        sh_tonal_mask (p.highlight_tone, mh);
        float ss = sh_strength (p.shadow_amount), hs = sh_strength (p.highlight_amount);
        std::vector<float> y2 (count);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
        {
            size_t i       = (size_t) ii;
            const float* q = &px[i * 4];
            float adj[3]   = {q[0], q[1], q[2]};
            if (!ls.empty ())
            {
                float m = ms[sh_level (ls[i])];
                for (int c = 0; c < 3; c++)
                {
                    adj[c] += m * (sh_lift (ss, adj[c]) - adj[c]);
                }
            }
            if (!lh.empty ())
            {
                float m = mh[255 - sh_level (lh[i])];
                for (int c = 0; c < 3; c++)
                {
                    adj[c] -= m * (sh_lift (hs, 1.0f - q[c]) - (1.0f - q[c]));
                }
            }
            y2[i] = sh_midtone (p.midtone, fminf (fmaxf (tone_luma (adj), 0.0f), 1.0f));
        }
        sh_clip (&y2, px, p.black_clip, p.white_clip);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
        {
            size_t i = (size_t) ii;
            float* q = &px[i * 4];
            if (q[3] <= 0.0f)
            {
                continue;
            }
            float y1 = tone_luma (q), yn = y2[i];
            float f = fabsf (yn - y1) / fmaxf (fmaxf (yn, y1), 1e-6f);
            float g = p.color >= 0.0f ? expf (sh_interp (SH_COLOR_UP, 12, p.color) * f)
                                      : 1.0f + sh_interp (SH_COLOR_DOWN, 9, -p.color) * f;
            float d[3], k = 1.0f;
            for (int c = 0; c < 3; c++)
            {
                d[c] = (q[c] - y1) * g;
                if (d[c] > 1e-9f)
                {
                    k = fminf (k, (1.0f - yn) / d[c]);
                }
                else if (d[c] < -1e-9f)
                {
                    k = fminf (k, yn / -d[c]);
                }
            }
            k = fminf (fmaxf (k, 0.0f), 1.0f);
            for (int c = 0; c < 3; c++)
            {
                q[c] = fminf (fmaxf (yn + k * d[c], 0.0f), 1.0f);
            }
        }
    }

    bool run_shadows_highlights (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        ShadowsHighlights sh;
        if (!frange (p, "shadowAmount", 0.0f, 100.0f, 35.0f, &sh.shadow_amount, cmd, err)
            || !frange (p, "shadowTone", 0.0f, 100.0f, 50.0f, &sh.shadow_tone, cmd, err)
            || !frange (p, "shadowRadius", 0.0f, 2500.0f, 30.0f, &sh.shadow_radius, cmd, err)
            || !frange (p, "highlightAmount", 0.0f, 100.0f, 0.0f, &sh.highlight_amount, cmd, err)
            || !frange (p, "highlightTone", 0.0f, 100.0f, 50.0f, &sh.highlight_tone, cmd, err)
            || !frange (p, "highlightRadius", 0.0f, 2500.0f, 30.0f, &sh.highlight_radius, cmd, err)
            || !frange (p, "color", -100.0f, 100.0f, 20.0f, &sh.color, cmd, err)
            || !frange (p, "midtone", -100.0f, 100.0f, 0.0f, &sh.midtone, cmd, err)
            || !frange (p, "blackClip", 0.0f, 50.0f, 0.01f, &sh.black_clip, cmd, err)
            || !frange (p, "whiteClip", 0.0f, 50.0f, 0.01f, &sh.white_clip, cmd, err))
        {
            return false;
        }
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        shadows_highlights (&rgba, pl->w, pl->h, sh);
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* ---- Replace Color ---- */

    /* A Color Range-style selection around `color` (fuzziness in levels), then a hue, saturation
       and lightness shift weighted by it. */
    bool run_replace_color (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float fuzz, hue, sat, light;
        if (!frange (p, "fuzziness", 0.0f, 200.0f, 40.0f, &fuzz, cmd, err)
            || !frange (p, "hue", -180.0f, 180.0f, 0.0f, &hue, cmd, err)
            || !frange (p, "saturation", -100.0f, 100.0f, 0.0f, &sat, cmd, err)
            || !frange (p, "lightness", -100.0f, 100.0f, 0.0f, &light, cmd, err))
        {
            return false;
        }
        float target[4] = {cx.fg[0], cx.fg[1], cx.fg[2], 1.0f};
        std::string hex;
        if (param_str (p, "color", &hex) && !parse_hex (hex.c_str (), target))
        {
            return bad_params (err, cmd, "`color` must be \"#rrggbb\"");
        }
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        size_t count = pl->w * pl->h;
        float f      = fuzz + 1.0f;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
        {
            float* q = &rgba[(size_t) ii * 4];
            if (q[3] <= 0.0f)
            {
                continue;
            }
            float dd =
                fmaxf (fabsf (q[0] - target[0]), fmaxf (fabsf (q[1] - target[1]), fabsf (q[2] - target[2]))) * 255.0f;
            float k = clamp01 (1.0f - dd / f);
            if (k <= 0.0f)
            {
                continue;
            }
            float c[3] = {q[0], q[1], q[2]}, hh, ss, ll, o[3];
            compose::rgb_to_hsl (c, &hh, &ss, &ll);
            float s  = sat >= 0.0f ? ss + (1.0f - ss) * sat / 100.0f : ss * (1.0f + sat / 100.0f);
            float l  = light >= 0.0f ? ll + (1.0f - ll) * light / 100.0f : ll * (1.0f + light / 100.0f);
            float nh = fmodf (hh + hue / 360.0f + 1.0f, 1.0f);
            compose::hsl_to_rgb (nh, clamp01 (s), clamp01 (l), o);
            for (int i = 0; i < 3; i++)
            {
                q[i] = clamp01 (c[i] + (o[i] - c[i]) * k);
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* ---- Match Color ---- */

    /* Lab mean and standard deviation of straight RGBA pixels, weighted by alpha and `mask` (one
       value per pixel, or NULL). False when nothing is opaque. */
    bool lab_stats (const float* px, size_t count, const float* mask, float mean[3], float sd[3])
    {
        double sum[3] = {0, 0, 0}, sq[3] = {0, 0, 0}, wsum = 0;
        for (size_t i = 0; i < count; i++)
        {
            double w = px[i * 4 + 3] * (mask != NULL ? mask[i] : 1.0f);
            if (w <= 0.0)
            {
                continue;
            }
            float lab[3];
            color::srgb_to_lab (&px[i * 4], lab);
            for (int k = 0; k < 3; k++)
            {
                sum[k] += w * lab[k];
                sq[k] += w * lab[k] * lab[k];
            }
            wsum += w;
        }
        if (wsum <= 0.0)
        {
            return false;
        }
        for (int k = 0; k < 3; k++)
        {
            double m = sum[k] / wsum;
            mean[k]  = (float) m;
            sd[k]    = (float) sqrt (fmax (sq[k] / wsum - m * m, 0.0));
        }
        return true;
    }

    /* Reinhard-style colour transfer in Lab toward the source's statistics ("sourceStats", put in
       by the command: [meanL, meanA, meanB, sdL, sdA, sdB]); without a source only Neutralize,
       Luminance and Color Intensity act. The target's statistics are the layer's (inside the
       selection's bounds with useSelectionInTarget). */
    bool run_match_color (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float lum, inten, fade;
        if (!frange (p, "luminance", 1.0f, 200.0f, 100.0f, &lum, cmd, err)
            || !frange (p, "intensity", 1.0f, 200.0f, 100.0f, &inten, cmd, err)
            || !frange (p, "fade", 0.0f, 100.0f, 0.0f, &fade, cmd, err))
        {
            return false;
        }
        bool neutralize = false, in_sel = false;
        param_bool (p, "neutralize", &neutralize);
        param_bool (p, "useSelectionInTarget", &in_sel);
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        size_t count = pl->w * pl->h;
        std::vector<float> mask;
        if (in_sel)
        {
            mask.assign (count, 0.0f);
            for (size_t y = 0; y < pl->h; y++)
            {
                for (size_t x = 0; x < pl->w; x++)
                {
                    int32_t px = cx.src.x0 + (int32_t) x, py = cx.src.y0 + (int32_t) y;
                    if (px >= cx.area.x0 && px < cx.area.x1 && py >= cx.area.y0 && py < cx.area.y1)
                    {
                        mask[y * pl->w + x] = 1.0f;
                    }
                }
            }
        }
        float tm[3], ts[3];
        if (!lab_stats (rgba.data (), count, mask.empty () ? NULL : mask.data (), tm, ts))
        {
            return true;
        }
        float sm[3], sd[3];
        memcpy (sm, tm, sizeof sm);
        memcpy (sd, ts, sizeof sd);
        const Value* st = param (p, "sourceStats");
        if (st != NULL && st->is_array () && st->size () == 6)
        {
            for (size_t k = 0; k < 3; k++)
            {
                sm[k] = (*st)[k].is_number () ? (*st)[k].get<float> () : tm[k];
                sd[k] = (*st)[k + 3].is_number () ? (*st)[k + 3].get<float> () : ts[k];
            }
        }
        if (neutralize)
        {
            sm[1] = sm[2] = 0.0f;
        }
        float scale[3];
        for (int k = 0; k < 3; k++)
        {
            scale[k] = ts[k] > 1e-4f ? sd[k] / ts[k] : 1.0f;
        }
        float lk = lum / 100.0f, ik = inten / 100.0f, fk = fade / 100.0f;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
        {
            float* q = &rgba[(size_t) ii * 4];
            if (q[3] <= 0.0f)
            {
                continue;
            }
            float lab[3], o[3];
            color::srgb_to_lab (q, lab);
            float l     = ((lab[0] - tm[0]) * scale[0] + sm[0]) * lk;
            float nl[3] = {fminf (fmaxf (l, 0.0f), 100.0f), ((lab[1] - tm[1]) * scale[1] + sm[1]) * ik,
                ((lab[2] - tm[2]) * scale[2] + sm[2]) * ik};
            color::lab_to_srgb (nl, o);
            for (int c = 0; c < 3; c++)
            {
                q[c] = clamp01 (o[c] + (q[c] - o[c]) * fk);
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* Match Color's source statistics, from open document "source" (an index; its layer
       "sourceLayer", or the merged image), inside its selection with useSelectionInSource. */
    bool match_color_source (Session& s, Value* p, Error* err)
    {
        const char* cmd = "image.adjustments.matchColor";
        int64_t index;
        if (!param_i64 (*p, "source", &index) || index < 0)
        {
            return true;
        }
        const DocState* src = s.doc_at ((size_t) index);
        if (src == NULL)
        {
            return bad_params (err, cmd, "no open document %lld", (long long) index);
        }
        const doc::Document& d           = *src->doc;
        Rect area                        = d.bounds ();
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = s.cmyk;
        compose::Buffer buf;
        int64_t layer;
        if (param_i64 (*p, "sourceLayer", &layer))
        {
            const doc::Layer* l         = d.layer ((doc::LayerId) layer);
            const raster::Surface* surf = l != NULL ? l->surface () : NULL;
            if (surf == NULL)
            {
                return bad_params (err, cmd, "source layer %lld has no pixels", (long long) layer);
            }
            buf = compose::surface_to_buffer (*surf, area, s.cmyk);
        }
        else
        {
            buf = compose::flatten (d, settings);
        }
        std::vector<float> mask;
        bool in_sel = false;
        param_bool (*p, "useSelectionInSource", &in_sel);
        if (in_sel && d.has_selection)
        {
            mask.resize (buf.pixel_count ());
            size_t w = (size_t) buf.rect.width ();
            for (size_t i = 0; i < mask.size (); i++)
            {
                mask[i] =
                    d.selection.sample_channel (buf.rect.x0 + (int32_t) (i % w), buf.rect.y0 + (int32_t) (i / w), 0);
            }
        }
        float m[3], sd[3];
        if (!lab_stats (buf.px.data (), buf.pixel_count (), mask.empty () ? NULL : mask.data (), m, sd))
        {
            return fail (err, ErrorKind::Other, "the source has no opaque pixels");
        }
        (*p)["sourceStats"] = {m[0], m[1], m[2], sd[0], sd[1], sd[2]};
        return true;
    }

    /* Settings a filter takes from the session rather than its layer, filled in before it runs. */
    bool prepare_filter_params (Session& s, const char* cmd, Value* p, Error* err)
    {
        if (strcmp (cmd, "image.adjustments.matchColor") == 0)
        {
            return match_color_source (s, p, err);
        }
        return true;
    }

    /* ---- HDR Toning ---- */

    struct HdrToning
    {
        float radius;
        float strength;
        float gamma;
        float exposure;
        float detail;
        float shadow;
        float highlight;
        float vibrance;
        float saturation;
        std::vector<float> curve;
    };

    float hdr_curve_eval (const std::vector<float>& pts, float x)
    {
        size_t n = pts.size () / 2;
        if (n < 2)
        {
            return x;
        }
        if (x <= pts[0])
        {
            return pts[1];
        }
        for (size_t i = 0; i + 1 < n; i++)
        {
            if (x <= pts[(i + 1) * 2])
            {
                float span = fmaxf (pts[(i + 1) * 2] - pts[i * 2], 1e-6f);
                return pts[i * 2 + 1] + (pts[(i + 1) * 2 + 1] - pts[i * 2 + 1]) * (x - pts[i * 2]) / span;
            }
        }
        return pts[(n - 1) * 2 + 1];
    }

    /* Local adaptation (a base/detail split of log luminance): the blurred log luminance is
       compressed toward its bright end by Strength, detail boosted; then exposure, gamma, shadow and
       highlight, the toning curve, and saturation / vibrance around the new luminance. */
    void hdr_toning (std::vector<float>* rgba, size_t w, size_t h, const HdrToning& p)
    {
        std::vector<float>& px = *rgba;
        size_t count           = w * h;
        if (count == 0)
        {
            return;
        }
        /* The floor keeps near-black pixels from dominating the log range (and ringing as halos). */
        const float EPS = 0.01f;
        std::vector<float> logl (count);
        for (size_t i = 0; i < count; i++)
        {
            logl[i] = logf (fmaxf (tone_luma (&px[i * 4]), 0.0f) + EPS);
        }
        std::vector<float> base = logl;
        blur_plane (&base, w, h, 1, fmaxf (p.radius, 1.0f) / 2.0f);
        /* The base compresses toward its bright end (the 99th percentile), so whites stay white
           and the shadows come up. */
        std::vector<float> opaque;
        for (size_t i = 0; i < count; i++)
        {
            if (px[i * 4 + 3] > 0.0f)
            {
                opaque.push_back (base[i]);
            }
        }
        if (opaque.empty ())
        {
            return;
        }
        size_t at = (size_t) ((double) (opaque.size () - 1) * 0.99);
        std::nth_element (opaque.begin (), opaque.begin () + (ptrdiff_t) at, opaque.end ());
        float mean     = opaque[at];
        float compress = 1.0f / (1.0f + fmaxf (p.strength, 0.0f)), detail = 1.0f + p.detail / 100.0f,
              exposure = powf (2.0f, p.exposure);
        float gamma    = fminf (fmaxf (p.gamma, 0.1f), 4.0f);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
        {
            size_t i = (size_t) ii;
            float* q = &px[i * 4];
            if (q[3] <= 0.0f)
            {
                continue;
            }
            float l0 = fmaxf (tone_luma (q), 0.0f) + EPS;
            float nl = mean + (base[i] - mean) * compress + (logl[i] - base[i]) * detail;
            float l1 = powf (fmaxf (expf (nl) * exposure, 0.0f), 1.0f / gamma);
            l1 += p.shadow / 100.0f * powf (1.0f - l1, 3.0f) * 0.5f - p.highlight / 100.0f * powf (l1, 3.0f) * 0.5f;
            l1          = hdr_curve_eval (p.curve, fminf (fmaxf (l1, 0.0f), 1.0f));
            float ratio = l1 / l0, out[3];
            for (int c = 0; c < 3; c++)
            {
                out[c] = q[c] * ratio;
            }
            float ll = tone_luma (out);
            float mx = fmaxf (fmaxf (out[0], out[1]), out[2]), mn = fminf (fminf (out[0], out[1]), out[2]);
            float sat_now = mx > 0.0f ? (mx - mn) / mx : 0.0f;
            float s       = fmaxf (1.0f + p.saturation / 100.0f + p.vibrance / 100.0f * (1.0f - sat_now), 0.0f);
            for (int c = 0; c < 3; c++)
            {
                q[c] = clamp01 (ll + (out[c] - ll) * s);
            }
        }
    }

    /* HDR Toning on `d` in place: the image flattened over white, toned, as a new Background. */
    bool apply_hdr_toning (Session& s, doc::Document* d, const Value& p, doc::LayerId* active, Error* err)
    {
        const char* cmd = "image.adjustments.hdrToning";
        HdrToning h;
        if (!frange (p, "radius", 1.0f, 500.0f, 30.0f, &h.radius, cmd, err)
            || !frange (p, "strength", 0.1f, 4.0f, 0.5f, &h.strength, cmd, err)
            || !frange (p, "gamma", 0.1f, 2.0f, 1.0f, &h.gamma, cmd, err)
            || !frange (p, "exposure", -5.0f, 5.0f, 0.0f, &h.exposure, cmd, err)
            || !frange (p, "detail", -100.0f, 300.0f, 30.0f, &h.detail, cmd, err)
            || !frange (p, "shadow", -100.0f, 100.0f, 0.0f, &h.shadow, cmd, err)
            || !frange (p, "highlight", -100.0f, 100.0f, 0.0f, &h.highlight, cmd, err)
            || !frange (p, "vibrance", -100.0f, 100.0f, 0.0f, &h.vibrance, cmd, err)
            || !frange (p, "saturation", -100.0f, 100.0f, 20.0f, &h.saturation, cmd, err))
        {
            return false;
        }
        const Value* curve = param (p, "curve");
        if (curve != NULL && curve->is_array ())
        {
            for (size_t i = 0; i < curve->size (); i++)
            {
                const Value& pt = (*curve)[i];
                if (!pt.is_array () || pt.size () != 2 || !pt[0].is_number () || !pt[1].is_number ())
                {
                    return bad_params (err, cmd, "`curve` must be [[in, out], …] in 0..255");
                }
                h.curve.push_back (pt[0].get<float> () / 255.0f);
                h.curve.push_back (pt[1].get<float> () / 255.0f);
            }
            /* Sorted by input. */
            for (size_t i = 1; i < h.curve.size () / 2; i++)
            {
                for (size_t j = i; j > 0 && h.curve[j * 2] < h.curve[(j - 1) * 2]; j--)
                {
                    std::swap (h.curve[j * 2], h.curve[(j - 1) * 2]);
                    std::swap (h.curve[j * 2 + 1], h.curve[(j - 1) * 2 + 1]);
                }
            }
        }
        color::PixelFormat format        = d->pixel_format ();
        float white[3]                   = {1.0f, 1.0f, 1.0f};
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = s.cmyk;
        Rect area                        = d->bounds ();
        raster::Surface flat             = compose::composite_to_surface (*d, area, format, white, settings);
        std::vector<float> native;
        if (!flat.read_region (area, &native))
        {
            return fail (err, ErrorKind::Other, "the image is too large");
        }
        size_t count = (size_t) area.width () * (size_t) area.height (), n = format.channels ();
        std::vector<float> rgba (count * 4);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) count; i++)
        {
            raster::to_rgba (format, &native[(size_t) i * n], s.cmyk, &rgba[(size_t) i * 4]);
        }
        hdr_toning (&rgba, (size_t) area.width (), (size_t) area.height (), h);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) count; i++)
        {
            float enc[raster::MAX_CHANNELS];
            raster::from_rgba (format, &rgba[(size_t) i * 4], s.cmyk, enc);
            for (size_t k = 0; k < n; k++)
            {
                native[(size_t) i * n + k] = enc[k];
            }
        }
        doc::Layer bg         = doc::Layer::raster (s.ids, "Background", format);
        bg.locks.transparency = true;
        bg.locks.position     = true;
        bg.pixels.write_region (area, native.data (), native.size ());
        bg.pixels.prune ();
        *active = bg.id;
        d->layers.clear ();
        d->layers.push_back (bg);
        d->has_selection = false;
        return true;
    }

    bool cmd_hdr_toning (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        if (e.doc () == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!apply_hdr_toning (s, e.doc (), p, e.active_layer (), err))
        {
            return false;
        }
        return e.commit ("HDR Toning", err);
    }

    const FilterSpec TONE_FILTERS[] = {
        {"image.adjustments.shadowsHighlights", "Shadows/Highlights…", "Image/Adjustments",
            "{\"shadowAmount\":0..100=35,\"shadowTone\":0..100=50,\"shadowRadius\":0..2500=30,\"highlightAmount\":0.."
            "100=0,\"highlightTone\":0..100=50,"
            "\"highlightRadius\":0..2500=30,\"color\":-100..100=20,\"midtone\":-100..100=0,\"blackClip\":0..50=0.01,"
            "\"whiteClip\":0..50=0.01}",
            run_shadows_highlights, false, Reach::Canvas},
        {"image.adjustments.matchColor", "Match Color…", "Image/Adjustments",
            "{\"source\":document index?,\"sourceLayer\":id? (default the merged "
            "image),\"luminance\":1..200=100,\"intensity\":1..200=100,\"fade\":0..100=0,"
            "\"neutralize\":bool=false,\"useSelectionInSource\":bool=false,\"useSelectionInTarget\":bool=false}",
            run_match_color, false, Reach::Canvas},
        {"image.adjustments.replaceColor", "Replace Color…", "Image/Adjustments",
            "{\"color\":\"#rrggbb\" (default the "
            "foreground),\"fuzziness\":0..200=40,\"hue\":-180..180=0,\"saturation\":-100..100=0,\"lightness\":-100.."
            "100=0}",
            run_replace_color, false, Reach::None},
    };

    void tone_filters (std::vector<FilterSpec>* out)
    {
        out->insert (out->end (), TONE_FILTERS, TONE_FILTERS + sizeof TONE_FILTERS / sizeof TONE_FILTERS[0]);
    }

    const CommandSpec TONE_COMMANDS[] = {
        {"image.adjustments.hdrToning", "HDR Toning…", "Image/Adjustments", NULL,
            "{\"radius\":1..500=30,\"strength\":0.1..4=0.5,\"gamma\":0.1..2=1,\"exposure\":-5..5=0,\"detail\":-100.."
            "300=30,\"shadow\":-100..100=0,\"highlight\":-100..100=0,"
            "\"vibrance\":-100..100=0,\"saturation\":-100..100=20,\"curve\":[[in,out],…] 0..255?} (flattens the image)",
            has_doc, cmd_hdr_toning, true},
    };

    void register_tone_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), TONE_COMMANDS, TONE_COMMANDS + sizeof TONE_COMMANDS / sizeof TONE_COMMANDS[0]);
    }

} // namespace engine
