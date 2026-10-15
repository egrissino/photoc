#pragma once

#include <stdint.h>

namespace color
{

    struct CmykSpace
    {
        void* to_srgb;
        void* from_srgb;
        uint64_t id;
    };

    extern const float D50[3];
    extern const float SRGB_TO_XYZ_D50[3][3];
    extern const float XYZ_D50_TO_SRGB[3][3];

    void cmyk_to_rgb (const CmykSpace* space, const float cmyk[4], float out[3]);
    void rgb_to_cmyk (const CmykSpace* space, const float rgb[3], float out[4]);

    void cmyk_to_rgb_naive (const float cmyk[4], float out[3]);
    void rgb_to_cmyk_naive (const float rgb[3], float out[4]);

    float srgb_to_linear (float v);
    float linear_to_srgb (float v);

    void mat3_mul (const float m[3][3], const float v[3], float out[3]);

    void lab_to_srgb (const float lab[3], float out[3]);
    void srgb_to_lab (const float rgb[3], float out[3]);

    float rgb_to_gray (const float rgb[3]);

} // namespace color
