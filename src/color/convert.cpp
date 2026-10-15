#include "color/convert.h"

#include <lcms2.h>
#include <math.h>

namespace color
{

    const float D50[3] = {0.96422f, 1.0f, 0.82521f};

    const float SRGB_TO_XYZ_D50[3][3] = {
        {0.436074f, 0.385064f, 0.143080f},
        {0.222504f, 0.716878f, 0.060618f},
        {0.013932f, 0.097104f, 0.714173f},
    };

    const float XYZ_D50_TO_SRGB[3][3] = {
        {3.133856f, -1.616867f, -0.490615f},
        {-0.978768f, 1.916142f, 0.033454f},
        {0.071945f, -0.228991f, 1.405243f},
    };

    float clamp01 (float v)
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

    /* Called from parallel tile loops: the cms module must create these transforms with
       cmsFLAGS_NOCACHE, because the one-pixel cache lcms2 keeps by default is not thread-safe. */
    void cmyk_to_rgb (const CmykSpace* space, const float cmyk[4], float out[3])
    {
        if (space == NULL || space->to_srgb == NULL)
        {
            cmyk_to_rgb_naive (cmyk, out);
            return;
        }
        float in[4] = {clamp01 (cmyk[0]) * 100.0f, clamp01 (cmyk[1]) * 100.0f, clamp01 (cmyk[2]) * 100.0f,
            clamp01 (cmyk[3]) * 100.0f};
        cmsDoTransform ((cmsHTRANSFORM) space->to_srgb, in, out, 1);
    }

    void rgb_to_cmyk (const CmykSpace* space, const float rgb[3], float out[4])
    {
        if (space == NULL || space->from_srgb == NULL)
        {
            rgb_to_cmyk_naive (rgb, out);
            return;
        }
        float in[3] = {clamp01 (rgb[0]), clamp01 (rgb[1]), clamp01 (rgb[2])};
        float ink[4];
        cmsDoTransform ((cmsHTRANSFORM) space->from_srgb, in, ink, 1);
        for (int i = 0; i < 4; i++)
        {
            out[i] = ink[i] / 100.0f;
        }
    }

    void cmyk_to_rgb_naive (const float c[4], float out[3])
    {
        float k = 1.0f - c[3];
        out[0]  = (1.0f - c[0]) * k;
        out[1]  = (1.0f - c[1]) * k;
        out[2]  = (1.0f - c[2]) * k;
    }

    void rgb_to_cmyk_naive (const float rgb[3], float out[4])
    {
        float k = 1.0f - fmaxf (fmaxf (rgb[0], rgb[1]), rgb[2]);
        if (k >= 1.0f - 1e-6f)
        {
            out[0] = out[1] = out[2] = 0.0f;
            out[3]                   = 1.0f;
            return;
        }
        float d = 1.0f - k;
        out[0]  = (1.0f - rgb[0] - k) / d;
        out[1]  = (1.0f - rgb[1] - k) / d;
        out[2]  = (1.0f - rgb[2] - k) / d;
        out[3]  = k;
    }

    float srgb_to_linear (float v)
    {
        if (v <= 0.04045f)
        {
            return v / 12.92f;
        }
        return powf ((v + 0.055f) / 1.055f, 2.4f);
    }

    float linear_to_srgb (float v)
    {
        if (v <= 0.0031308f)
        {
            return v * 12.92f;
        }
        return 1.055f * powf (v, 1.0f / 2.4f) - 0.055f;
    }

    void mat3_mul (const float m[3][3], const float v[3], float out[3])
    {
        for (int r = 0; r < 3; r++)
        {
            out[r] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2];
        }
    }

    float lab_finv (float t)
    {
        const float e = 6.0f / 29.0f;
        if (t > e)
        {
            return t * t * t;
        }
        return 3.0f * (e * e) * (t - 4.0f / 29.0f);
    }

    void lab_to_srgb (const float lab[3], float out[3])
    {
        float fy     = (lab[0] + 16.0f) / 116.0f;
        float fx     = fy + lab[1] / 500.0f;
        float fz     = fy - lab[2] / 200.0f;
        float xyz[3] = {lab_finv (fx) * D50[0], lab_finv (fy) * D50[1], lab_finv (fz) * D50[2]};
        float lin[3];
        mat3_mul (XYZ_D50_TO_SRGB, xyz, lin);
        for (int i = 0; i < 3; i++)
        {
            out[i] = linear_to_srgb (clamp01 (lin[i]));
        }
    }

    float lab_f (float t)
    {
        const float e = 6.0f / 29.0f;
        if (t > e * e * e)
        {
            return cbrtf (t);
        }
        return t / (3.0f * (e * e)) + 4.0f / 29.0f;
    }

    void srgb_to_lab (const float rgb[3], float out[3])
    {
        float lin[3] = {srgb_to_linear (rgb[0]), srgb_to_linear (rgb[1]), srgb_to_linear (rgb[2])};
        float xyz[3];
        mat3_mul (SRGB_TO_XYZ_D50, lin, xyz);
        float fx = lab_f (xyz[0] / D50[0]);
        float fy = lab_f (xyz[1] / D50[1]);
        float fz = lab_f (xyz[2] / D50[2]);
        out[0]   = 116.0f * fy - 16.0f;
        out[1]   = 500.0f * (fx - fy);
        out[2]   = 200.0f * (fy - fz);
    }

    float rgb_to_gray (const float rgb[3])
    {
        return 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    }

} // namespace color
