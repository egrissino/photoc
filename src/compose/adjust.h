#pragma once

#include "color/color.h"
#include "color/convert.h"
#include "compose/compose.h"
#include "doc/adjust.h"
#include <vector>

namespace compose
{

    struct Transfer
    {
        bool srgb;
        float gamma;

        static Transfer for_document (color::ColorMode mode, color::SampleType depth);
        Transfer for_exposure () const;
        float decode (float v) const;
        float encode (float v) const;
    };

    struct AdjustContext
    {
        Transfer transfer;
        bool has_depth;
        color::SampleType depth;
        color::ColorMode mode;
        const color::CmykSpace* cmyk;
    };

    const size_t LUT_SIZE    = 4096;
    const float LEGACY_PIVOT = 128.0f / 255.0f;

    void apply_adjustment (const doc::Adjustment& adj, Buffer* buf, const AdjustContext& cx);
    bool lab_channel_adjustment (const doc::Adjustment& adj);
    bool adjustment_quantum (color::SampleType depth, float* out);

    float posterize (float v, uint32_t levels);
    float levels_q (const doc::LevelsChannel& ch, float v, bool has_quantum, float quantum);
    float levels_float (const doc::LevelsChannel& ch, float v);
    std::vector<float> curve_lut (const std::vector<doc::CurvePoint>& points);
    std::vector<float> curve_map_lut (const std::vector<doc::CurvePoint>& points);
    float lut_sample (const std::vector<float>& table, float v);
    float modern_brightness (float v, float brightness);
    float modern_contrast (float v, float contrast);
    void rgb_to_hsl (const float c[3], float* h, float* s, float* l);
    void hsl_to_rgb (float h, float s, float l, float out[3]);
    void hue_saturation (const float c[3], float hue, float s, float l, bool colorize, float out[3]);
    float black_white_gray (const float c[3], const float weights[6]);
    float color_balance_channel (const float tones[3][3], bool preserve_luminosity, int ch, float v);
    void selective_color (const float c[3], bool relative, const float adj[9][4], float out[3]);
    void photo_filter_matrix (const float color[3], float density, bool normalize_y, float out[3][3]);
    void vibrance_px (const float c[3], float v, float s, Transfer transfer, float hi, float out[3]);
    float bayer4 (int32_t x, int32_t y);

} // namespace compose
