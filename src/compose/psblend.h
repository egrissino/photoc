#pragma once

#include "color/blend.h"

namespace compose
{

    const float EDGE       = 1e-4f;
    const float TEXT_GAMMA = 1.45f;

    struct BlendSpace
    {
        bool lab_mix;
        bool hdr;
    };

    void ps_blend_rgb (BlendSpace space, color::BlendMode mode, const float cb[3], const float cs[3], float out[3]);
    bool fill_is_special (color::BlendMode mode);
    void blend_rgb_fill (
        BlendSpace space, color::BlendMode mode, const float cb[3], const float cs[3], float fill, float out[3]);
    void composite_fill (BlendSpace space, color::BlendMode mode, const float backdrop[4], const float source[4],
        float opacity, float fill, float gamma, float out[4]);
    void ps_composite (BlendSpace space, color::BlendMode mode, const float backdrop[4], const float source[4],
        float opacity, float out[4]);
    void composite_normal_rgb (const float backdrop[4], const float source[4], float opacity, float out[4]);
    float text_encode (float v, float gamma);
    float text_decode (float v, float gamma);
    void composite_gamma (BlendSpace space, color::BlendMode mode, const float backdrop[4], const float source[4],
        float opacity, float gamma, float out[4]);

} // namespace compose
