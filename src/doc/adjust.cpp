#include "doc/adjust.h"

#include <math.h>
#include <string.h>

namespace doc
{

    /* Adjustment parameters (data only). Evaluation lives in compose/adjust. */

    LevelsChannel LevelsChannel::identity ()
    {
        LevelsChannel c = {0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        return c;
    }

    bool LevelsChannel::operator== (const LevelsChannel& o) const
    {
        return in_black == o.in_black && in_white == o.in_white && gamma == o.gamma && out_black == o.out_black
            && out_white == o.out_white;
    }

    bool LevelsChannel::is_identity () const
    {
        return *this == identity ();
    }

    float rem_euclid_f (float a, float b)
    {
        float r = fmodf (a, b);
        return r < 0.0f ? r + b : r;
    }

    /* Range i (0 = reds ... 5 = magentas) at Photoshop's default extent: fully on for 30 degrees
       around its centre, fading over 30 degrees each side. */
    HueRange HueRange::neutral (int index)
    {
        float c    = 60.0f * (float) index;
        HueRange r = {0.0f, 0.0f, 0.0f, {c - 45.0f, c - 15.0f, c + 15.0f, c + 45.0f}};
        return r;
    }

    bool HueRange::is_neutral () const
    {
        return hue == 0.0f && saturation == 0.0f && lightness == 0.0f;
    }

    /* How much a pixel of hue `deg` belongs to this range, 0..1. Measured from the range's start, so
       ranges that wrap past 0/360 (reds) work. */
    float HueRange::weight (float deg) const
    {
        float a = bounds[0];
        float b = isfinite (bounds[1]) ? rem_euclid_f (bounds[1] - a, 360.0f) : 0.0f;
        float c = isfinite (bounds[2]) ? rem_euclid_f (bounds[2] - a, 360.0f) : 0.0f;
        float d = isfinite (bounds[3]) ? rem_euclid_f (bounds[3] - a, 360.0f) : 0.0f;
        if (c < b)
        {
            c = b;
        }
        if (d < c)
        {
            d = c;
        }
        float h = isfinite (deg) ? rem_euclid_f (deg - a, 360.0f) : 0.0f;
        if (h <= b)
        {
            return b <= 0.0f ? 1.0f : h / b;
        }
        if (h <= c)
        {
            return 1.0f;
        }
        if (h <= d)
        {
            return d <= c ? 1.0f : (d - h) / (d - c);
        }
        return 0.0f;
    }

    /* Four range sliders made increasing, within one turn, with their centre in -30..330. */
    void canonical_hue_bounds (const float in[4], float out[4])
    {
        for (int i = 0; i < 4; i++)
        {
            if (!isfinite (in[i]))
            {
                out[0] = -45.0f;
                out[1] = -15.0f;
                out[2] = 15.0f;
                out[3] = 45.0f;
                return;
            }
        }
        out[0] = in[0];
        for (int k = 1; k < 4; k++)
        {
            float v = out[k - 1] + rem_euclid_f (in[k] - out[k - 1], 360.0f);
            out[k]  = v < out[0] + 359.0f ? v : out[0] + 359.0f;
        }
        float centre = (out[1] + out[2]) / 2.0f;
        float shift  = rem_euclid_f (centre + 30.0f, 360.0f) - 30.0f - centre;
        for (int k = 0; k < 4; k++)
        {
            out[k] += shift;
        }
    }

    bool is_identity_curve (const std::vector<CurvePoint>& p)
    {
        if (p.size () < 2)
        {
            return true;
        }
        return p.size () == 2 && p[0].input == 0.0f && p[0].output == 0.0f && p[1].input == 1.0f && p[1].output == 1.0f;
    }

    std::vector<CurvePoint> identity_curve ()
    {
        std::vector<CurvePoint> c (2);
        c[0].input  = 0.0f;
        c[0].output = 0.0f;
        c[1].input  = 1.0f;
        c[1].output = 1.0f;
        return c;
    }

    /* The neutral adjustment of `kind` (what Photoshop's dialog opens with). */
    Adjustment Adjustment::make (AdjustmentKind kind)
    {
        Adjustment a;
        a.kind                           = kind;
        a.brightness_contrast.brightness = 0.0f;
        a.brightness_contrast.contrast   = 0.0f;
        a.brightness_contrast.legacy     = false;
        a.levels.master                  = LevelsChannel::identity ();
        for (int i = 0; i < 3; i++)
        {
            a.levels.channels[i] = LevelsChannel::identity ();
        }
        a.levels.space  = ToneSpace::Rgb;
        a.levels.black  = LevelsChannel::identity ();
        a.curves.master = identity_curve ();
        for (int i = 0; i < 3; i++)
        {
            a.curves.channels[i] = identity_curve ();
        }
        a.curves.space              = ToneSpace::Rgb;
        a.curves.pencil             = false;
        a.exposure.exposure         = 0.0f;
        a.exposure.offset           = 0.0f;
        a.exposure.gamma            = 1.0f;
        a.vibrance.vibrance         = 0.0f;
        a.vibrance.saturation       = 0.0f;
        a.hue_saturation.hue        = 0.0f;
        a.hue_saturation.saturation = 0.0f;
        a.hue_saturation.lightness  = 0.0f;
        a.hue_saturation.colorize   = false;
        for (int i = 0; i < 6; i++)
        {
            a.hue_saturation.ranges[i] = HueRange::neutral (i);
        }
        memset (&a.color_balance, 0, sizeof a.color_balance);
        a.color_balance.preserve_luminosity = true;
        const float bw[6]                   = {40.0f, 60.0f, 40.0f, 60.0f, 20.0f, 80.0f};
        memcpy (a.black_white.weights, bw, sizeof bw);
        a.black_white.has_tint             = false;
        a.black_white.tint[0]              = 0.882f;
        a.black_white.tint[1]              = 0.827f;
        a.black_white.tint[2]              = 0.702f;
        a.photo_filter.color[0]            = 236.0f / 255.0f;
        a.photo_filter.color[1]            = 138.0f / 255.0f;
        a.photo_filter.color[2]            = 0.0f;
        a.photo_filter.density             = 0.25f;
        a.photo_filter.preserve_luminosity = true;
        memset (&a.channel_mixer, 0, sizeof a.channel_mixer);
        a.channel_mixer.matrix[0][0] = a.channel_mixer.matrix[1][1] = a.channel_mixer.matrix[2][2] = 1.0f;
        a.color_lookup.size                                                                        = 0;
        a.color_lookup.tetrahedral                                                                 = false;
        a.color_lookup.dither                                                                      = false;
        a.posterize_levels                                                                         = 4;
        a.threshold_level                                                                          = 128.0f / 255.0f;
        GradientStop black = {0.0f, {0.0f, 0.0f, 0.0f}};
        GradientStop white = {1.0f, {1.0f, 1.0f, 1.0f}};
        a.gradient_map.stops.push_back (black);
        a.gradient_map.stops.push_back (white);
        a.gradient_map.reverse = false;
        a.gradient_map.dither  = false;
        memset (&a.selective_color, 0, sizeof a.selective_color);
        a.selective_color.relative = true;
        return a;
    }

    const char* Adjustment::label () const
    {
        switch (kind)
        {
        case AdjustmentKind::BrightnessContrast:
            return "Brightness/Contrast";
        case AdjustmentKind::Levels:
            return "Levels";
        case AdjustmentKind::Curves:
            return "Curves";
        case AdjustmentKind::Exposure:
            return "Exposure";
        case AdjustmentKind::Vibrance:
            return "Vibrance";
        case AdjustmentKind::HueSaturation:
            return "Hue/Saturation";
        case AdjustmentKind::ColorBalance:
            return "Color Balance";
        case AdjustmentKind::BlackWhite:
            return "Black & White";
        case AdjustmentKind::PhotoFilter:
            return "Photo Filter";
        case AdjustmentKind::ChannelMixer:
            return "Channel Mixer";
        case AdjustmentKind::ColorLookup:
            return "Color Lookup";
        case AdjustmentKind::Invert:
            return "Invert";
        case AdjustmentKind::Posterize:
            return "Posterize";
        case AdjustmentKind::Threshold:
            return "Threshold";
        case AdjustmentKind::GradientMap:
            return "Gradient Map";
        case AdjustmentKind::SelectiveColor:
            return "Selective Color";
        case AdjustmentKind::Unsupported:
            return "Adjustment";
        }
        return "Adjustment";
    }

    /* The command-id form: image.adjustments.<id>, layer.newAdjustmentLayer.<id>. */
    const char* adjustment_kind_id (AdjustmentKind kind)
    {
        switch (kind)
        {
        case AdjustmentKind::BrightnessContrast:
            return "brightnessContrast";
        case AdjustmentKind::Levels:
            return "levels";
        case AdjustmentKind::Curves:
            return "curves";
        case AdjustmentKind::Exposure:
            return "exposure";
        case AdjustmentKind::Vibrance:
            return "vibrance";
        case AdjustmentKind::HueSaturation:
            return "hueSaturation";
        case AdjustmentKind::ColorBalance:
            return "colorBalance";
        case AdjustmentKind::BlackWhite:
            return "blackWhite";
        case AdjustmentKind::PhotoFilter:
            return "photoFilter";
        case AdjustmentKind::ChannelMixer:
            return "channelMixer";
        case AdjustmentKind::ColorLookup:
            return "colorLookup";
        case AdjustmentKind::Invert:
            return "invert";
        case AdjustmentKind::Posterize:
            return "posterize";
        case AdjustmentKind::Threshold:
            return "threshold";
        case AdjustmentKind::GradientMap:
            return "gradientMap";
        case AdjustmentKind::SelectiveColor:
            return "selectiveColor";
        case AdjustmentKind::Unsupported:
            return "unsupported";
        }
        return "unsupported";
    }

    bool adjustment_kind_from_id (const char* id, AdjustmentKind* out)
    {
        for (int k = 0; k <= (int) AdjustmentKind::Unsupported; k++)
        {
            if (strcmp (id, adjustment_kind_id ((AdjustmentKind) k)) == 0)
            {
                *out = (AdjustmentKind) k;
                return true;
            }
        }
        return false;
    }

    /* Values in the table of a size^3 Color Lookup (size^3 x 3); 0 for a size below 2 or one too big
       to address (a stored document's size is untrusted). */
    size_t lut3d_len (uint32_t size)
    {
        if (size < 2 || size > 1024)
        {
            return 0;
        }
        return (size_t) size * size * size * 3;
    }

    /* Equality of the active kind's parameters (the other blocks are ignored). */
    bool same_floats (const float* a, const float* b, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            if (a[i] != b[i])
            {
                return false;
            }
        }
        return true;
    }

    bool same_curve (const std::vector<CurvePoint>& a, const std::vector<CurvePoint>& b)
    {
        if (a.size () != b.size ())
        {
            return false;
        }
        for (size_t i = 0; i < a.size (); i++)
        {
            if (a[i].input != b[i].input || a[i].output != b[i].output)
            {
                return false;
            }
        }
        return true;
    }

    bool same_range (const HueRange& a, const HueRange& b)
    {
        return a.hue == b.hue && a.saturation == b.saturation && a.lightness == b.lightness
            && same_floats (a.bounds, b.bounds, 4);
    }

    bool Adjustment::operator== (const Adjustment& o) const
    {
        if (kind != o.kind)
        {
            return false;
        }
        switch (kind)
        {
        case AdjustmentKind::BrightnessContrast:
            {
                const BrightnessContrastParams &a = brightness_contrast, &b = o.brightness_contrast;
                return a.brightness == b.brightness && a.contrast == b.contrast && a.legacy == b.legacy;
            }
        case AdjustmentKind::Levels:
            {
                const LevelsParams &a = levels, &b = o.levels;
                return a.master == b.master && a.channels[0] == b.channels[0] && a.channels[1] == b.channels[1]
                    && a.channels[2] == b.channels[2] && a.space == b.space && a.black == b.black;
            }
        case AdjustmentKind::Curves:
            {
                const CurvesParams &a = curves, &b = o.curves;
                return same_curve (a.master, b.master) && same_curve (a.channels[0], b.channels[0])
                    && same_curve (a.channels[1], b.channels[1]) && same_curve (a.channels[2], b.channels[2])
                    && a.space == b.space && same_curve (a.black, b.black) && a.pencil == b.pencil;
            }
        case AdjustmentKind::Exposure:
            return exposure.exposure == o.exposure.exposure && exposure.offset == o.exposure.offset
                && exposure.gamma == o.exposure.gamma;
        case AdjustmentKind::Vibrance:
            return vibrance.vibrance == o.vibrance.vibrance && vibrance.saturation == o.vibrance.saturation;
        case AdjustmentKind::HueSaturation:
            {
                const HueSaturationParams &a = hue_saturation, &b = o.hue_saturation;
                if (a.hue != b.hue || a.saturation != b.saturation || a.lightness != b.lightness
                    || a.colorize != b.colorize)
                {
                    return false;
                }
                for (int i = 0; i < 6; i++)
                {
                    if (!same_range (a.ranges[i], b.ranges[i]))
                    {
                        return false;
                    }
                }
                return true;
            }
        case AdjustmentKind::ColorBalance:
            {
                const ColorBalanceParams &a = color_balance, &b = o.color_balance;
                return same_floats (a.shadows, b.shadows, 3) && same_floats (a.midtones, b.midtones, 3)
                    && same_floats (a.highlights, b.highlights, 3) && a.preserve_luminosity == b.preserve_luminosity;
            }
        case AdjustmentKind::BlackWhite:
            {
                const BlackWhiteParams &a = black_white, &b = o.black_white;
                return same_floats (a.weights, b.weights, 6) && a.has_tint == b.has_tint
                    && same_floats (a.tint, b.tint, 3);
            }
        case AdjustmentKind::PhotoFilter:
            {
                const PhotoFilterParams &a = photo_filter, &b = o.photo_filter;
                return same_floats (a.color, b.color, 3) && a.density == b.density
                    && a.preserve_luminosity == b.preserve_luminosity;
            }
        case AdjustmentKind::ChannelMixer:
            return same_floats (&channel_mixer.matrix[0][0], &o.channel_mixer.matrix[0][0], 12)
                && channel_mixer.monochrome == o.channel_mixer.monochrome;
        case AdjustmentKind::ColorLookup:
            {
                const ColorLookupParams &a = color_lookup, &b = o.color_lookup;
                return a.name == b.name && a.lut == b.lut && a.size == b.size && a.tetrahedral == b.tetrahedral
                    && a.dither == b.dither;
            }
        case AdjustmentKind::Invert:
            return true;
        case AdjustmentKind::Posterize:
            return posterize_levels == o.posterize_levels;
        case AdjustmentKind::Threshold:
            return threshold_level == o.threshold_level;
        case AdjustmentKind::GradientMap:
            {
                const GradientMapParams &a = gradient_map, &b = o.gradient_map;
                if (a.reverse != b.reverse || a.dither != b.dither || a.stops.size () != b.stops.size ())
                {
                    return false;
                }
                for (size_t i = 0; i < a.stops.size (); i++)
                {
                    if (a.stops[i].location != b.stops[i].location
                        || !same_floats (a.stops[i].color, b.stops[i].color, 3))
                    {
                        return false;
                    }
                }
                return true;
            }
        case AdjustmentKind::SelectiveColor:
            return selective_color.relative == o.selective_color.relative
                && same_floats (&selective_color.adjustments[0][0], &o.selective_color.adjustments[0][0], 36);
        case AdjustmentKind::Unsupported:
            return unsupported.psd_key == o.unsupported.psd_key && unsupported.raw == o.unsupported.raw;
        }
        return false;
    }

    bool Adjustment::operator!= (const Adjustment& o) const
    {
        return !(*this == o);
    }

} // namespace doc
