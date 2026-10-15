#include "algo/retouch.h"

#include <math.h>

namespace algo
{

    /* Per-pixel and per-dab kernels for the toning and focus tools (Dodge, Burn, Sponge, Blur,
       Sharpen). The tone curves are our own, chosen to match the observed behaviour of Photoshop's
       tools: the selected range moves most, black and white stay put for Midtones, and Protect Tones
       works on luminance (scaling RGB, with gamut compression instead of per-channel clipping). */

    float clampf (float v, float lo, float hi)
    {
        return v < lo ? lo : v > hi ? hi : v;
    }

    /* Rec. 601 luma. */
    float luma (const float c[3])
    {
        return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    }

    /* The dodge / burn transfer of one value v with strength e. */
    float tone_curve (float v, float e, ToneRange range, bool burn)
    {
        v        = fmaxf (v, 0.0f);
        e        = clampf (e, 0.0f, 1.0f);
        float vm = fminf (v, 1.0f);
        switch (range)
        {
        case ToneRange::Midtones:
            {
                /* A gamma bend weighted to peak at mid-grey: 0 and 1 are fixed points. */
                float w = clampf (4.0f * v * (1.0f - v), 0.0f, 1.0f);
                return v + w * (powf (vm, burn ? 1.0f + e : 1.0f / (1.0f + e)) - v);
            }
        case ToneRange::Shadows:
            return burn ? v - e * (1.0f - vm) * (1.0f - vm) * v : v + e * (1.0f - vm) * (1.0f - vm) * (1.0f - vm);
        case ToneRange::Highlights:
            return burn ? v - e * vm * vm * v : v + e * vm * vm * (1.0f - vm);
        }
        return v;
    }

    /* Back into 0..1 keeping the luma (mix toward grey) instead of clipping each channel. */
    void compress_gamut (const float c[3], float l, float out[3])
    {
        l        = clampf (l, 0.0f, 1.0f);
        float lo = fminf (c[0], fminf (c[1], c[2])), hi = fmaxf (c[0], fmaxf (c[1], c[2]));
        float t = 1.0f;
        if (hi > 1.0f && hi - l > 1e-9f)
        {
            t = fminf (t, (1.0f - l) / (hi - l));
        }
        if (lo < 0.0f && l - lo > 1e-9f)
        {
            t = fminf (t, l / (l - lo));
        }
        for (int i = 0; i < 3; i++)
        {
            out[i] = l + (c[i] - l) * t;
        }
    }

    /* Dodge or Burn one straight RGB colour with strength e. Protect Tones applies the curve to luma
       and scales the colour to it (hue and saturation kept). */
    void dodge_burn (const float c[3], float e, ToneRange range, bool burn, bool protect_tones, float out[3])
    {
        if (e <= 0.0f)
        {
            for (int i = 0; i < 3; i++)
            {
                out[i] = c[i];
            }
            return;
        }
        if (protect_tones)
        {
            float l = luma (c), nl = tone_curve (l, e, range, burn);
            float s[3];
            for (int i = 0; i < 3; i++)
            {
                s[i] = l > 1e-6f ? c[i] * nl / l : nl;
            }
            compress_gamut (s, nl, out);
        }
        else
        {
            for (int i = 0; i < 3; i++)
            {
                out[i] = tone_curve (c[i], e, range, burn);
            }
        }
    }

    /* HSV-style saturation (chroma / max). */
    float saturation (const float c[3])
    {
        float lo = fminf (c[0], fminf (c[1], c[2])), hi = fmaxf (c[0], fmaxf (c[1], c[2]));
        return hi <= 1e-6f ? 0.0f : clampf ((hi - lo) / hi, 0.0f, 1.0f);
    }

    /* Sponge: away from (saturate) or toward the luma grey by `amount`. Vibrance damps the change
       where it would clip. */
    void sponge (const float c[3], float amount, bool saturate, bool vibrance, float out[3])
    {
        float a = clampf (amount, 0.0f, 1.0f);
        if (a <= 0.0f)
        {
            for (int i = 0; i < 3; i++)
            {
                out[i] = c[i];
            }
            return;
        }
        float s = saturation (c);
        if (vibrance)
        {
            a *= saturate ? 1.0f - s : 0.25f + 0.75f * s;
        }
        float l = luma (c), k = saturate ? 1.0f + a : 1.0f - a;
        float m[3];
        for (int i = 0; i < 3; i++)
        {
            m[i] = l + (c[i] - l) * k;
        }
        compress_gamut (m, l, out);
    }

    /* Gaussian blur of the window [x0, x1) x [y0, y1) of an interleaved w x h x ch buffer, edges
       clamped; with alpha (index >= 0) colour is blurred premultiplied. */
    void local_blur (const float* data, size_t w, size_t h, size_t ch, int alpha, size_t x0, size_t y0, size_t x1,
        size_t y1, float sigma, std::vector<float>* out)
    {
        size_t ow = x1 - x0, oh = y1 - y0;
        int r = (int) fmaxf (ceilf (sigma * 3.0f), 1.0f);
        std::vector<float> k ((size_t) (2 * r + 1));
        float ks = 0.0f;
        for (int i = -r; i <= r; i++)
        {
            k[(size_t) (i + r)] = expf (-(float) (i * i) / fmaxf (2.0f * sigma * sigma, 1e-6f));
            ks += k[(size_t) (i + r)];
        }
        for (size_t i = 0; i < k.size (); i++)
        {
            k[i] /= ks;
        }
        size_t th = oh + 2 * (size_t) r;
        std::vector<float> tmp (ow * th * ch, 0.0f), px (ch);
        for (size_t ty = 0; ty < th; ty++)
        {
            long y    = (long) y0 + (long) ty - r;
            size_t yy = (size_t) (y < 0 ? 0 : y >= (long) h ? (long) h - 1 : y);
            for (size_t ox = 0; ox < ow; ox++)
            {
                float* o = &tmp[(ty * ow + ox) * ch];
                for (int i = 0; i <= 2 * r; i++)
                {
                    long x         = (long) x0 + (long) ox + i - r;
                    size_t xx      = (size_t) (x < 0 ? 0 : x >= (long) w ? (long) w - 1 : x);
                    const float* s = &data[(yy * w + xx) * ch];
                    float av       = alpha >= 0 ? s[alpha] : 1.0f;
                    for (size_t c = 0; c < ch; c++)
                    {
                        o[c] += (alpha >= 0 && (int) c != alpha ? s[c] * av : s[c]) * k[(size_t) i];
                    }
                }
            }
        }
        out->assign (ow * oh * ch, 0.0f);
        for (size_t oy = 0; oy < oh; oy++)
        {
            for (size_t ox = 0; ox < ow; ox++)
            {
                float* o = &(*out)[(oy * ow + ox) * ch];
                for (int i = 0; i <= 2 * r; i++)
                {
                    const float* s = &tmp[((oy + (size_t) i) * ow + ox) * ch];
                    for (size_t c = 0; c < ch; c++)
                    {
                        o[c] += s[c] * k[(size_t) i];
                    }
                }
                if (alpha >= 0)
                {
                    float av = o[alpha];
                    for (size_t c = 0; c < ch; c++)
                    {
                        if ((int) c != alpha)
                        {
                            o[c] = av > 1e-6f ? o[c] / av : data[((y0 + oy) * w + x0 + ox) * ch + c];
                        }
                    }
                }
            }
        }
    }

    /* Unsharp mask of a window: v + amount (v - blur(v)); Protect Detail clamps each result to its
       3x3 neighbourhood's range. Alpha is left as it is. */
    void local_sharpen (const float* data, size_t w, size_t h, size_t ch, int alpha, size_t x0, size_t y0, size_t x1,
        size_t y1, float amount, bool protect_detail, std::vector<float>* out)
    {
        size_t ow = x1 - x0;
        std::vector<float> blur;
        local_blur (data, w, h, ch, alpha, x0, y0, x1, y1, 1.0f, &blur);
        out->assign (blur.size (), 0.0f);
        for (size_t y = y0; y < y1; y++)
        {
            for (size_t x = x0; x < x1; x++)
            {
                size_t o = ((y - y0) * ow + (x - x0)) * ch, i = (y * w + x) * ch;
                for (size_t c = 0; c < ch; c++)
                {
                    float v = data[i + c];
                    if ((int) c == alpha)
                    {
                        (*out)[o + c] = v;
                        continue;
                    }
                    float s = v + amount * (v - blur[o + c]);
                    if (protect_detail)
                    {
                        float lo = v, hi = v;
                        for (int dy = -1; dy <= 1; dy++)
                        {
                            for (int dx = -1; dx <= 1; dx++)
                            {
                                long nx = (long) x + dx, ny = (long) y + dy;
                                nx       = nx < 0 ? 0 : nx >= (long) w ? (long) w - 1 : nx;
                                ny       = ny < 0 ? 0 : ny >= (long) h ? (long) h - 1 : ny;
                                float nv = data[((size_t) ny * w + (size_t) nx) * ch + c];
                                lo       = fminf (lo, nv);
                                hi       = fmaxf (hi, nv);
                            }
                        }
                        s = clampf (s, lo, hi);
                    }
                    (*out)[o + c] = s;
                }
            }
        }
    }

} // namespace algo
