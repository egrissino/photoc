#include "color/blend.h"

#include <float.h>
#include <math.h>
#include <string.h>

namespace color
{

    const BlendMode LAYER_MODES[LAYER_MODE_COUNT] = {
        BlendMode::Normal,
        BlendMode::Dissolve,
        BlendMode::Darken,
        BlendMode::Multiply,
        BlendMode::ColorBurn,
        BlendMode::LinearBurn,
        BlendMode::DarkerColor,
        BlendMode::Lighten,
        BlendMode::Screen,
        BlendMode::ColorDodge,
        BlendMode::LinearDodge,
        BlendMode::LighterColor,
        BlendMode::Overlay,
        BlendMode::SoftLight,
        BlendMode::HardLight,
        BlendMode::VividLight,
        BlendMode::LinearLight,
        BlendMode::PinLight,
        BlendMode::HardMix,
        BlendMode::Difference,
        BlendMode::Exclusion,
        BlendMode::Subtract,
        BlendMode::Divide,
        BlendMode::Hue,
        BlendMode::Saturation,
        BlendMode::Color,
        BlendMode::Luminosity,
    };

    const BlendMode PAINT_NET_MODES[PAINT_NET_MODE_COUNT] = {
        BlendMode::Reflect,
        BlendMode::Glow,
        BlendMode::Negation,
        BlendMode::Xor,
        BlendMode::PaintNetColorBurn,
        BlendMode::PaintNetColorDodge,
    };

    const char* blend_label (BlendMode mode)
    {
        switch (mode)
        {
        case BlendMode::PassThrough:
            return "Pass Through";
        case BlendMode::Normal:
            return "Normal";
        case BlendMode::Dissolve:
            return "Dissolve";
        case BlendMode::Darken:
            return "Darken";
        case BlendMode::Multiply:
            return "Multiply";
        case BlendMode::ColorBurn:
            return "Color Burn";
        case BlendMode::LinearBurn:
            return "Linear Burn";
        case BlendMode::DarkerColor:
            return "Darker Color";
        case BlendMode::Lighten:
            return "Lighten";
        case BlendMode::Screen:
            return "Screen";
        case BlendMode::ColorDodge:
            return "Color Dodge";
        case BlendMode::LinearDodge:
            return "Linear Dodge (Add)";
        case BlendMode::LighterColor:
            return "Lighter Color";
        case BlendMode::Overlay:
            return "Overlay";
        case BlendMode::SoftLight:
            return "Soft Light";
        case BlendMode::HardLight:
            return "Hard Light";
        case BlendMode::VividLight:
            return "Vivid Light";
        case BlendMode::LinearLight:
            return "Linear Light";
        case BlendMode::PinLight:
            return "Pin Light";
        case BlendMode::HardMix:
            return "Hard Mix";
        case BlendMode::Difference:
            return "Difference";
        case BlendMode::Exclusion:
            return "Exclusion";
        case BlendMode::Subtract:
            return "Subtract";
        case BlendMode::Divide:
            return "Divide";
        case BlendMode::Hue:
            return "Hue";
        case BlendMode::Saturation:
            return "Saturation";
        case BlendMode::Color:
            return "Color";
        case BlendMode::Luminosity:
            return "Luminosity";
        case BlendMode::Reflect:
            return "Reflect";
        case BlendMode::Glow:
            return "Glow";
        case BlendMode::Negation:
            return "Negation";
        case BlendMode::Xor:
            return "XOR";
        case BlendMode::PaintNetColorBurn:
            return "Color Burn (Paint.NET)";
        case BlendMode::PaintNetColorDodge:
            return "Color Dodge (Paint.NET)";
        }
        return "Normal";
    }

    /* Four bytes, not NUL-terminated in the file format. Paint.NET modes have no PSD equivalent and
       fall back to Normal; exporters must report that loss (blend_has_psd_equivalent). */
    const char* blend_psd_key (BlendMode mode)
    {
        switch (mode)
        {
        case BlendMode::PassThrough:
            return "pass";
        case BlendMode::Normal:
            return "norm";
        case BlendMode::Dissolve:
            return "diss";
        case BlendMode::Darken:
            return "dark";
        case BlendMode::Multiply:
            return "mul ";
        case BlendMode::ColorBurn:
            return "idiv";
        case BlendMode::LinearBurn:
            return "lbrn";
        case BlendMode::DarkerColor:
            return "dkCl";
        case BlendMode::Lighten:
            return "lite";
        case BlendMode::Screen:
            return "scrn";
        case BlendMode::ColorDodge:
            return "div ";
        case BlendMode::LinearDodge:
            return "lddg";
        case BlendMode::LighterColor:
            return "lgCl";
        case BlendMode::Overlay:
            return "over";
        case BlendMode::SoftLight:
            return "sLit";
        case BlendMode::HardLight:
            return "hLit";
        case BlendMode::VividLight:
            return "vLit";
        case BlendMode::LinearLight:
            return "lLit";
        case BlendMode::PinLight:
            return "pLit";
        case BlendMode::HardMix:
            return "hMix";
        case BlendMode::Difference:
            return "diff";
        case BlendMode::Exclusion:
            return "smud";
        case BlendMode::Subtract:
            return "fsub";
        case BlendMode::Divide:
            return "fdiv";
        case BlendMode::Hue:
            return "hue ";
        case BlendMode::Saturation:
            return "sat ";
        case BlendMode::Color:
            return "colr";
        case BlendMode::Luminosity:
            return "lum ";
        case BlendMode::Reflect:
        case BlendMode::Glow:
        case BlendMode::Negation:
        case BlendMode::Xor:
        case BlendMode::PaintNetColorBurn:
        case BlendMode::PaintNetColorDodge:
            return "norm";
        }
        return "norm";
    }

    bool blend_from_psd_key (const char key[4], BlendMode* out)
    {
        if (memcmp (key, blend_psd_key (BlendMode::PassThrough), 4) == 0)
        {
            *out = BlendMode::PassThrough;
            return true;
        }
        for (size_t i = 0; i < LAYER_MODE_COUNT; i++)
        {
            if (memcmp (key, blend_psd_key (LAYER_MODES[i]), 4) == 0)
            {
                *out = LAYER_MODES[i];
                return true;
            }
        }
        return false;
    }

    bool blend_has_psd_equivalent (BlendMode mode)
    {
        for (size_t i = 0; i < PAINT_NET_MODE_COUNT; i++)
        {
            if (PAINT_NET_MODES[i] == mode)
            {
                return false;
            }
        }
        return true;
    }

    bool blend_is_separable (BlendMode mode)
    {
        switch (mode)
        {
        case BlendMode::Hue:
        case BlendMode::Saturation:
        case BlendMode::Color:
        case BlendMode::Luminosity:
        case BlendMode::DarkerColor:
        case BlendMode::LighterColor:
            return false;
        default:
            return true;
        }
    }

    float blend_reflect (float cb, float cs)
    {
        if (cs >= 1.0f)
        {
            return 1.0f;
        }
        return fminf (cb * cb / (1.0f - cs), 1.0f);
    }

    float blend_vivid_light_hard_mix (float cb, float cs)
    {
        const float edge = 1e-4f;
        if (cs <= 0.5f)
        {
            if (cb >= 1.0f - edge)
            {
                return 1.0f;
            }
            if (cs <= 0.0f)
            {
                return 0.0f;
            }
            return 1.0f - fminf ((1.0f - cb) / (2.0f * cs), 1.0f);
        }
        if (cb <= edge)
        {
            return 0.0f;
        }
        if (cs >= 1.0f)
        {
            return 1.0f;
        }
        return fminf (cb / (2.0f * (1.0f - cs)), 1.0f);
    }

    float blend_color_burn (float cb, float cs)
    {
        if (cb >= 1.0f)
        {
            return 1.0f;
        }
        if (cs <= 0.0f)
        {
            return 0.0f;
        }
        return 1.0f - fminf ((1.0f - cb) / cs, 1.0f);
    }

    float blend_color_dodge (float cb, float cs)
    {
        if (cb <= 0.0f)
        {
            return 0.0f;
        }
        if (cs >= 1.0f)
        {
            return 1.0f;
        }
        return fminf (cb / (1.0f - cs), 1.0f);
    }

    float blend_hard_light (float cb, float cs)
    {
        if (cs <= 0.5f)
        {
            return cb * 2.0f * cs;
        }
        float s = 2.0f * cs - 1.0f;
        return cb + s - cb * s;
    }

    /* Photoshop's Soft Light (differs from the W3C/PDF definition in the upper half). */
    float blend_soft_light_ps (float cb, float cs)
    {
        if (cs <= 0.5f)
        {
            return 2.0f * cb * cs + cb * cb * (1.0f - 2.0f * cs);
        }
        return 2.0f * cb * (1.0f - cs) + sqrtf (fmaxf (cb, 0.0f)) * (2.0f * cs - 1.0f);
    }

    uint8_t blend_quantize8 (float v)
    {
        if (!(v > 0.0f))
        {
            return 0;
        }
        if (v >= 1.0f)
        {
            return 255;
        }
        return (uint8_t) roundf (v * 255.0f);
    }

    /* B(cb, cs) with Linear Dodge and Divide clipped at `hi` (1 at integer depths). */
    float blend_channel_clipped (BlendMode mode, float cb, float cs, float hi)
    {
        switch (mode)
        {
        case BlendMode::Normal:
        case BlendMode::Dissolve:
        case BlendMode::PassThrough:
            return cs;
        case BlendMode::Darken:
            return fminf (cb, cs);
        case BlendMode::Multiply:
            return cb * cs;
        case BlendMode::ColorBurn:
            return blend_color_burn (cb, cs);
        case BlendMode::LinearBurn:
            return fmaxf (cb + cs - 1.0f, 0.0f);
        case BlendMode::Lighten:
            return fmaxf (cb, cs);
        case BlendMode::Screen:
            return cb + cs - cb * cs;
        case BlendMode::ColorDodge:
            return blend_color_dodge (cb, cs);
        case BlendMode::LinearDodge:
            return fminf (cb + cs, hi);
        case BlendMode::Overlay:
            return blend_hard_light (cs, cb);
        case BlendMode::SoftLight:
            return blend_soft_light_ps (cb, cs);
        case BlendMode::HardLight:
            return blend_hard_light (cb, cs);
        case BlendMode::VividLight:
            if (cs <= 0.0f)
            {
                return 0.0f;
            }
            if (cs >= 1.0f)
            {
                return 1.0f;
            }
            if (cs <= 0.5f)
            {
                return blend_color_burn (cb, 2.0f * cs);
            }
            return blend_color_dodge (cb, 2.0f * (cs - 0.5f));
        case BlendMode::LinearLight:
            {
                float v = cb + 2.0f * cs - 1.0f;
                if (v < 0.0f)
                {
                    return 0.0f;
                }
                if (v > 1.0f)
                {
                    return 1.0f;
                }
                return v;
            }
        case BlendMode::PinLight:
            if (cs <= 0.5f)
            {
                return fminf (cb, 2.0f * cs);
            }
            return fmaxf (cb, 2.0f * cs - 1.0f);
        case BlendMode::HardMix:
            return blend_vivid_light_hard_mix (cb, cs) >= 0.5f - 1e-6f ? 1.0f : 0.0f;
        case BlendMode::Difference:
            return fabsf (cb - cs);
        case BlendMode::Exclusion:
            return cb + cs - 2.0f * cb * cs;
        case BlendMode::Subtract:
            return fmaxf (cb - cs, 0.0f);
        case BlendMode::Divide:
            if (cs <= 0.0f)
            {
                return cb <= 0.0f ? 0.0f : 1.0f;
            }
            return fminf (cb / cs, hi);
        case BlendMode::Reflect:
            return blend_reflect (cb, cs);
        case BlendMode::Glow:
            return blend_reflect (cs, cb);
        case BlendMode::Negation:
            return 1.0f - fabsf (1.0f - cb - cs);
        case BlendMode::Xor:
            /* Paint.NET's XOR works on 8-bit channels even in a float compositor. */
            return (float) (blend_quantize8 (cb) ^ blend_quantize8 (cs)) / 255.0f;
        case BlendMode::PaintNetColorBurn:
            if (cs <= 0.0f)
            {
                return 0.0f;
            }
            return fmaxf (1.0f - (1.0f - cb) / cs, 0.0f);
        case BlendMode::PaintNetColorDodge:
            if (cs >= 1.0f)
            {
                return 1.0f;
            }
            return fminf (cb / (1.0f - cs), 1.0f);
        case BlendMode::DarkerColor:
        case BlendMode::LighterColor:
        case BlendMode::Hue:
        case BlendMode::Saturation:
        case BlendMode::Color:
        case BlendMode::Luminosity:
            return cs;
        }
        return cs;
    }

    float blend_channel (BlendMode mode, float cb, float cs)
    {
        return blend_channel_clipped (mode, cb, cs, 1.0f);
    }

    float blend_channel_hdr (BlendMode mode, float cb, float cs)
    {
        return blend_channel_clipped (mode, cb, cs, FLT_MAX);
    }

    float lum (const float c[3])
    {
        return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
    }

    /* Moves c toward its grey until every channel is within 0..top, keeping the luminosity. When the
       luminosity itself is out of range no colour of it is in gamut, so the result is the grey. */
    void blend_clip_color (const float c[3], float top, float out[3])
    {
        float l = lum (c);
        float n = fminf (fminf (c[0], c[1]), c[2]);
        float x = fmaxf (fmaxf (c[0], c[1]), c[2]);
        out[0]  = c[0];
        out[1]  = c[1];
        out[2]  = c[2];
        if (n < 0.0f)
        {
            float d = l - n;
            for (int i = 0; i < 3; i++)
            {
                out[i] = (l > 0.0f && d > 0.0f) ? l + (out[i] - l) * l / d : l;
            }
        }
        if (x > top)
        {
            float d = x - l;
            for (int i = 0; i < 3; i++)
            {
                out[i] = (l < top && d > 0.0f) ? l + (out[i] - l) * (top - l) / d : l;
            }
        }
    }

    void blend_set_lum_within (const float c[3], float l, float top, float out[3])
    {
        float d        = l - lum (c);
        float moved[3] = {c[0] + d, c[1] + d, c[2] + d};
        blend_clip_color (moved, top, out);
    }

    void set_lum (const float c[3], float l, float out[3])
    {
        blend_set_lum_within (c, l, 1.0f, out);
    }

    float blend_sat (const float c[3])
    {
        return fmaxf (fmaxf (c[0], c[1]), c[2]) - fminf (fminf (c[0], c[1]), c[2]);
    }

    void blend_set_sat (const float c[3], float s, float out[3])
    {
        int idx[3] = {0, 1, 2};
        for (int i = 1; i < 3; i++)
        {
            for (int j = i; j > 0 && c[idx[j]] < c[idx[j - 1]]; j--)
            {
                int t      = idx[j];
                idx[j]     = idx[j - 1];
                idx[j - 1] = t;
            }
        }
        int imin = idx[0], imid = idx[1], imax = idx[2];
        out[0] = out[1] = out[2] = 0.0f;
        if (c[imax] > c[imin])
        {
            out[imid] = (c[imid] - c[imin]) * s / (c[imax] - c[imin]);
            out[imax] = s;
        }
        out[imin] = 0.0f;
    }

    void blend_rgb (BlendMode mode, const float cb[3], const float cs[3], float out[3])
    {
        blend_rgb_within (mode, cb, cs, 1.0f, out);
    }

    void blend_rgb_within (BlendMode mode, const float cb_in[3], const float cs_in[3], float top, float out[3])
    {
        float cb[3] = {cb_in[0], cb_in[1], cb_in[2]};
        float cs[3] = {cs_in[0], cs_in[1], cs_in[2]};
        float t[3];
        switch (mode)
        {
        case BlendMode::Hue:
            blend_set_sat (cs, blend_sat (cb), t);
            blend_set_lum_within (t, lum (cb), top, out);
            return;
        case BlendMode::Saturation:
            blend_set_sat (cb, blend_sat (cs), t);
            blend_set_lum_within (t, lum (cb), top, out);
            return;
        case BlendMode::Color:
            blend_set_lum_within (cs, lum (cb), top, out);
            return;
        case BlendMode::Luminosity:
            blend_set_lum_within (cb, lum (cs), top, out);
            return;
        case BlendMode::DarkerColor:
            {
                const float* pick = lum (cs) < lum (cb) ? cs : cb;
                out[0]            = pick[0];
                out[1]            = pick[1];
                out[2]            = pick[2];
                return;
            }
        case BlendMode::LighterColor:
            {
                const float* pick = lum (cs) > lum (cb) ? cs : cb;
                out[0]            = pick[0];
                out[1]            = pick[1];
                out[2]            = pick[2];
                return;
            }
        default:
            for (int i = 0; i < 3; i++)
            {
                out[i] = blend_channel (mode, cb[i], cs[i]);
            }
            return;
        }
    }

    void blend_rgb_hdr (BlendMode mode, const float cb[3], const float cs[3], float out[3])
    {
        if (blend_is_separable (mode))
        {
            float r[3];
            for (int i = 0; i < 3; i++)
            {
                r[i] = blend_channel_hdr (mode, cb[i], cs[i]);
            }
            out[0] = r[0];
            out[1] = r[1];
            out[2] = r[2];
            return;
        }
        blend_rgb_within (mode, cb, cs, FLT_MAX, out);
    }

    /* Straight-alpha source over straight-alpha backdrop (W3C/PDF general formula). Dissolve must be
       resolved by the caller before calling. `out` may alias either input. */
    void composite (BlendMode mode, const float backdrop[4], const float source[4], float opacity, float out[4])
    {
        float bd[4] = {backdrop[0], backdrop[1], backdrop[2], backdrop[3]};
        float ab    = bd[3];
        float as    = source[3] * opacity;
        if (as <= 0.0f)
        {
            for (int i = 0; i < 4; i++)
            {
                out[i] = bd[i];
            }
            return;
        }
        float cb[3] = {bd[0], bd[1], bd[2]};
        float cs[3] = {source[0], source[1], source[2]};
        float b[3];
        blend_rgb (mode, cb, cs, b);
        float ao = as + ab * (1.0f - as);
        if (ao <= 0.0f)
        {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        for (int i = 0; i < 3; i++)
        {
            float premul = (1.0f - as) * ab * cb[i] + (1.0f - ab) * as * cs[i] + as * ab * b[i];
            out[i]       = premul / ao;
        }
        out[3] = ao;
    }

} // namespace color
