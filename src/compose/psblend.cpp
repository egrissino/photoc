#include "compose/psblend.h"

#include "color/convert.h"
#include <math.h>

namespace compose
{

    /* Photoshop-exact blend functions where they differ from the generic ones in color/blend, plus
       the compositing formula. Measured against Photoshop's own composites:
       - Vivid Light: the source extremes win (cs = 0 gives 0 even over white).
       - Hard Mix: the thresholded generic vivid light (backdrop extremes win).
       Backdrop values within EDGE of 0 / 1 count as exact where the result jumps there. */

    using color::BlendMode;

    float ps_vivid_light (float cb, float cs)
    {
        if (cs <= 0.5f)
        {
            if (cs <= 0.0f)
            {
                return 0.0f;
            }
            return 1.0f - fminf ((1.0f - cb) / (2.0f * cs), 1.0f);
        }
        if (cs >= 1.0f)
        {
            return 1.0f;
        }
        return fminf (cb / (2.0f * (1.0f - cs)), 1.0f);
    }

    float ps_color_burn (float cb, float cs)
    {
        if (cb >= 1.0f - EDGE)
        {
            return 1.0f;
        }
        if (cs <= 0.0f)
        {
            return 0.0f;
        }
        return 1.0f - fminf ((1.0f - cb) / cs, 1.0f);
    }

    float ps_color_dodge (float cb, float cs)
    {
        if (cb <= EDGE)
        {
            return 0.0f;
        }
        if (cs >= 1.0f)
        {
            return 1.0f;
        }
        return fminf (cb / (1.0f - cs), 1.0f);
    }

    float ps_vivid_light_generic (float cb, float cs)
    {
        if (cs <= 0.5f)
        {
            if (cb >= 1.0f - EDGE)
            {
                return 1.0f;
            }
            if (cs <= 0.0f)
            {
                return 0.0f;
            }
            return 1.0f - fminf ((1.0f - cb) / (2.0f * cs), 1.0f);
        }
        if (cb <= EDGE)
        {
            return 0.0f;
        }
        if (cs >= 1.0f)
        {
            return 1.0f;
        }
        return fminf (cb / (2.0f * (1.0f - cs)), 1.0f);
    }

    float ps_hard_mix (float cb, float cs)
    {
        return ps_vivid_light_generic (cb, cs) >= 0.5f - 1e-6f ? 1.0f : 0.0f;
    }

    float clamp01f (float v)
    {
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

    void ps_blend_rgb (BlendSpace space, BlendMode mode, const float cb[3], const float cs[3], float out[3])
    {
        float r[3];
        switch (mode)
        {
        case BlendMode::VividLight:
            for (int i = 0; i < 3; i++)
            {
                r[i] = ps_vivid_light (cb[i], cs[i]);
            }
            break;
        case BlendMode::HardMix:
            for (int i = 0; i < 3; i++)
            {
                r[i] = ps_hard_mix (cb[i], cs[i]);
            }
            break;
        case BlendMode::ColorBurn:
            for (int i = 0; i < 3; i++)
            {
                r[i] = ps_color_burn (cb[i], cs[i]);
            }
            break;
        case BlendMode::ColorDodge:
            for (int i = 0; i < 3; i++)
            {
                r[i] = ps_color_dodge (cb[i], cs[i]);
            }
            break;
        default:
            if (space.hdr)
            {
                color::blend_rgb_hdr (mode, cb, cs, r);
            }
            else
            {
                color::blend_rgb (mode, cb, cs, r);
            }
            break;
        }
        out[0] = r[0];
        out[1] = r[1];
        out[2] = r[2];
    }

    /* Photoshop's "special eight": below 100% Fill they blend a source pulled toward the mode's
       neutral colour at full strength, instead of using less coverage. */
    bool fill_is_special (BlendMode mode)
    {
        switch (mode)
        {
        case BlendMode::ColorBurn:
        case BlendMode::LinearBurn:
        case BlendMode::ColorDodge:
        case BlendMode::LinearDodge:
        case BlendMode::VividLight:
        case BlendMode::LinearLight:
        case BlendMode::HardMix:
        case BlendMode::Difference:
            return true;
        default:
            return false;
        }
    }

    void blend_rgb_fill (
        BlendSpace space, BlendMode mode, const float cb[3], const float cs[3], float fill, float out[3])
    {
        float f = clamp01f (fill);
        if (f >= 1.0f || !fill_is_special (mode))
        {
            ps_blend_rgb (space, mode, cb, cs, out);
            return;
        }
        if (mode == BlendMode::HardMix)
        {
            /* (cb - f(1 - cs)) / (1 - f): the hard threshold at f = 1, the backdrop at f = 0. */
            for (int i = 0; i < 3; i++)
            {
                out[i] = clamp01f ((cb[i] - f * (1.0f - cs[i])) / (1.0f - f));
            }
            return;
        }
        float neutral = 0.0f;
        if (mode == BlendMode::ColorBurn || mode == BlendMode::LinearBurn)
        {
            neutral = 1.0f;
        }
        else if (mode == BlendMode::VividLight || mode == BlendMode::LinearLight)
        {
            neutral = 0.5f;
        }
        float toward[3];
        for (int i = 0; i < 3; i++)
        {
            toward[i] = neutral + (cs[i] - neutral) * f;
        }
        ps_blend_rgb (space, mode, cb, toward, out);
    }

    /* A layer with separate opacity and fill. For the special eight below 100% Fill, coverage over
       the backdrop is `opacity` and the blend is blend_rgb_fill; over transparency Fill stays
       coverage. `out` may alias the inputs. */
    void composite_fill (BlendSpace space, BlendMode mode, const float backdrop[4], const float source[4],
        float opacity, float fill, float gamma, float out[4])
    {
        if (fill >= 1.0f || !fill_is_special (mode))
        {
            composite_gamma (space, mode, backdrop, source, opacity * clamp01f (fill), gamma, out);
            return;
        }
        float ab    = backdrop[3];
        float over  = source[3] * opacity;
        float alone = over * clamp01f (fill);
        if (over <= 0.0f)
        {
            for (int i = 0; i < 4; i++)
            {
                out[i] = backdrop[i];
            }
            return;
        }
        float ao = ab + (1.0f - ab) * alone;
        if (ao <= 0.0f)
        {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        float cb[3] = {backdrop[0], backdrop[1], backdrop[2]};
        float cs[3] = {source[0], source[1], source[2]};
        float b[3];
        blend_rgb_fill (space, mode, cb, cs, fill, b);
        float wb  = ab * (1.0f - over) / ao;
        float wbs = ab * over / ao;
        float ws  = (1.0f - ab) * alone / ao;
        for (int i = 0; i < 3; i++)
        {
            if (gamma == 1.0f)
            {
                out[i] = wb * cb[i] + wbs * b[i] + ws * cs[i];
            }
            else
            {
                out[i] = text_decode (
                    wb * text_encode (cb[i], gamma) + wbs * text_encode (b[i], gamma) + ws * text_encode (cs[i], gamma),
                    gamma);
            }
        }
        out[3] = ao;
    }

    void normal_rgb (const float backdrop[4], const float source[4], float as, float ao, float out[4])
    {
        if (ao <= 0.0f)
        {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        float kb  = backdrop[3] * (1.0f - as);
        float inv = 1.0f / ao;
        float r   = (kb * backdrop[0] + as * source[0]) * inv;
        float g   = (kb * backdrop[1] + as * source[1]) * inv;
        float b   = (kb * backdrop[2] + as * source[2]) * inv;
        out[0]    = r;
        out[1]    = g;
        out[2]    = b;
        out[3]    = ao;
    }

    /* Straight-alpha source over straight-alpha backdrop (W3C/PDF general formula). Lab documents
       mix Normal in CIELAB, as Photoshop does. `out` may alias the inputs. */
    void ps_composite (
        BlendSpace space, BlendMode mode, const float backdrop[4], const float source[4], float opacity, float out[4])
    {
        float ab = backdrop[3];
        float as = source[3] * opacity;
        if (as <= 0.0f)
        {
            for (int i = 0; i < 4; i++)
            {
                out[i] = backdrop[i];
            }
            return;
        }
        float ao = as + ab * (1.0f - as);
        if (mode == BlendMode::Normal && space.lab_mix && ab > 0.0f && ao > 0.0f)
        {
            float lb[3], ls[3], m[3];
            color::srgb_to_lab (backdrop, lb);
            color::srgb_to_lab (source, ls);
            float kb = ab * (1.0f - as) / ao;
            float k  = as / ao;
            for (int i = 0; i < 3; i++)
            {
                m[i] = lb[i] * kb + ls[i] * k;
            }
            color::lab_to_srgb (m, out);
            out[3] = ao;
            return;
        }
        if (mode == BlendMode::Normal)
        {
            normal_rgb (backdrop, source, as, ao, out);
            return;
        }
        float cb[3] = {backdrop[0], backdrop[1], backdrop[2]};
        float cs[3] = {source[0], source[1], source[2]};
        float b[3];
        ps_blend_rgb (space, mode, cb, cs, b);
        if (ao <= 0.0f)
        {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        for (int i = 0; i < 3; i++)
        {
            out[i] = ((1.0f - as) * ab * cb[i] + (1.0f - ab) * as * cs[i] + as * ab * b[i]) / ao;
        }
        out[3] = ao;
    }

    /* Normal outside Lab, for callers that pick the blend space once per buffer. */
    void composite_normal_rgb (const float backdrop[4], const float source[4], float opacity, float out[4])
    {
        float as = source[3] * opacity;
        if (as <= 0.0f)
        {
            for (int i = 0; i < 4; i++)
            {
                out[i] = backdrop[i];
            }
            return;
        }
        float ao = as + backdrop[3] * (1.0f - as);
        normal_rgb (backdrop, source, as, ao, out);
    }

    /* "Blend Text Colors Using Gamma": type layers mix in linear light raised to 1 / gamma. */
    float text_encode (float v, float gamma)
    {
        return powf (color::srgb_to_linear (fmaxf (v, 0.0f)), 1.0f / gamma);
    }

    float text_decode (float v, float gamma)
    {
        return color::linear_to_srgb (powf (fmaxf (v, 0.0f), gamma));
    }

    void composite_gamma (BlendSpace space, BlendMode mode, const float backdrop[4], const float source[4],
        float opacity, float gamma, float out[4])
    {
        if (gamma == 1.0f)
        {
            ps_composite (space, mode, backdrop, source, opacity, out);
            return;
        }
        float ab = backdrop[3];
        float as = source[3] * opacity;
        if (as <= 0.0f)
        {
            for (int i = 0; i < 4; i++)
            {
                out[i] = backdrop[i];
            }
            return;
        }
        float ao = as + ab * (1.0f - as);
        if (ao <= 0.0f)
        {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        float cb[3] = {backdrop[0], backdrop[1], backdrop[2]};
        float cs[3] = {source[0], source[1], source[2]};
        float b[3];
        ps_blend_rgb (space, mode, cb, cs, b);
        float wb  = (1.0f - as) * ab / ao;
        float ws  = (1.0f - ab) * as / ao;
        float wbs = as * ab / ao;
        for (int i = 0; i < 3; i++)
        {
            out[i] = text_decode (
                wb * text_encode (cb[i], gamma) + ws * text_encode (cs[i], gamma) + wbs * text_encode (b[i], gamma),
                gamma);
        }
        out[3] = ao;
    }

} // namespace compose
