#pragma once

#include <stddef.h>
#include <stdint.h>

namespace color
{

    enum class BlendMode : uint8_t
    {
        PassThrough,
        Normal,
        Dissolve,
        Darken,
        Multiply,
        ColorBurn,
        LinearBurn,
        DarkerColor,
        Lighten,
        Screen,
        ColorDodge,
        LinearDodge,
        LighterColor,
        Overlay,
        SoftLight,
        HardLight,
        VividLight,
        LinearLight,
        PinLight,
        HardMix,
        Difference,
        Exclusion,
        Subtract,
        Divide,
        Hue,
        Saturation,
        Color,
        Luminosity,
        Reflect,
        Glow,
        Negation,
        Xor,
        PaintNetColorBurn,
        PaintNetColorDodge,
    };

    const size_t LAYER_MODE_COUNT     = 27;
    const size_t PAINT_NET_MODE_COUNT = 6;
    extern const BlendMode LAYER_MODES[LAYER_MODE_COUNT];
    extern const BlendMode PAINT_NET_MODES[PAINT_NET_MODE_COUNT];

    const char* blend_label (BlendMode mode);
    const char* blend_psd_key (BlendMode mode);
    bool blend_from_psd_key (const char key[4], BlendMode* out);
    bool blend_has_psd_equivalent (BlendMode mode);
    bool blend_is_separable (BlendMode mode);

    float blend_channel (BlendMode mode, float cb, float cs);
    float blend_channel_hdr (BlendMode mode, float cb, float cs);

    float lum (const float c[3]);
    void set_lum (const float c[3], float l, float out[3]);

    void blend_rgb (BlendMode mode, const float cb[3], const float cs[3], float out[3]);
    void blend_rgb_within (BlendMode mode, const float cb[3], const float cs[3], float top, float out[3]);
    void blend_rgb_hdr (BlendMode mode, const float cb[3], const float cs[3], float out[3]);

    void composite (BlendMode mode, const float backdrop[4], const float source[4], float opacity, float out[4]);

} // namespace color
