#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace doc
{

    struct CurvePoint
    {
        float input;
        float output;
    };

    struct LevelsChannel
    {
        float in_black;
        float in_white;
        float gamma;
        float out_black;
        float out_white;

        static LevelsChannel identity ();
        bool is_identity () const;
        bool operator== (const LevelsChannel& o) const;
    };

    enum class ToneSpace : uint8_t
    {
        Rgb,
        Cmyk,
        Lab,
    };

    struct HueRange
    {
        float hue;
        float saturation;
        float lightness;
        float bounds[4];

        static HueRange neutral (int index);
        bool is_neutral () const;
        float weight (float deg) const;
    };

    void canonical_hue_bounds (const float in[4], float out[4]);
    bool is_identity_curve (const std::vector<CurvePoint>& points);
    std::vector<CurvePoint> identity_curve ();

    enum class AdjustmentKind : uint8_t
    {
        BrightnessContrast,
        Levels,
        Curves,
        Exposure,
        Vibrance,
        HueSaturation,
        ColorBalance,
        BlackWhite,
        PhotoFilter,
        ChannelMixer,
        ColorLookup,
        Invert,
        Posterize,
        Threshold,
        GradientMap,
        SelectiveColor,
        Unsupported,
    };

    struct BrightnessContrastParams
    {
        float brightness;
        float contrast;
        bool legacy;
    };

    struct LevelsParams
    {
        LevelsChannel master;
        LevelsChannel channels[3];
        ToneSpace space;
        LevelsChannel black;
    };

    struct CurvesParams
    {
        std::vector<CurvePoint> master;
        std::vector<CurvePoint> channels[3];
        ToneSpace space;
        std::vector<CurvePoint> black;
        bool pencil;
    };

    struct ExposureParams
    {
        float exposure;
        float offset;
        float gamma;
    };

    struct VibranceParams
    {
        float vibrance;
        float saturation;
    };

    struct HueSaturationParams
    {
        float hue;
        float saturation;
        float lightness;
        bool colorize;
        HueRange ranges[6];
    };

    struct ColorBalanceParams
    {
        float shadows[3];
        float midtones[3];
        float highlights[3];
        bool preserve_luminosity;
    };

    struct BlackWhiteParams
    {
        float weights[6];
        bool has_tint;
        float tint[3];
    };

    struct PhotoFilterParams
    {
        float color[3];
        float density;
        bool preserve_luminosity;
    };

    struct ChannelMixerParams
    {
        float matrix[3][4];
        bool monochrome;
    };

    struct ColorLookupParams
    {
        std::string name;
        std::shared_ptr<const std::vector<float>> lut;
        uint32_t size;
        bool tetrahedral;
        bool dither;
    };

    struct GradientStop
    {
        float location;
        float color[3];
    };

    struct GradientMapParams
    {
        std::vector<GradientStop> stops;
        bool reverse;
        bool dither;
    };

    struct SelectiveColorParams
    {
        bool relative;
        float adjustments[9][4];
    };

    struct UnsupportedParams
    {
        std::string psd_key;
        std::vector<uint8_t> raw;
    };

    struct Adjustment
    {
        AdjustmentKind kind;
        BrightnessContrastParams brightness_contrast;
        LevelsParams levels;
        CurvesParams curves;
        ExposureParams exposure;
        VibranceParams vibrance;
        HueSaturationParams hue_saturation;
        ColorBalanceParams color_balance;
        BlackWhiteParams black_white;
        PhotoFilterParams photo_filter;
        ChannelMixerParams channel_mixer;
        ColorLookupParams color_lookup;
        uint32_t posterize_levels;
        float threshold_level;
        GradientMapParams gradient_map;
        SelectiveColorParams selective_color;
        UnsupportedParams unsupported;

        static Adjustment make (AdjustmentKind kind);
        const char* label () const;
        bool operator== (const Adjustment& o) const;
        bool operator!= (const Adjustment& o) const;
    };

    const char* adjustment_kind_id (AdjustmentKind kind);
    bool adjustment_kind_from_id (const char* id, AdjustmentKind* out);
    size_t lut3d_len (uint32_t size);

} // namespace doc
