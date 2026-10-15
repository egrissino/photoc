#include "psd/adjust_map.h"

#include "color/convert.h"
#include "psd/bytes.h"
#include "psd/descriptor.h"
#include <algorithm>
#include <math.h>
#include <string.h>

namespace psd
{

    /* Adjustment layers in PSD: the adjustment blocks (Adobe's "Adjustment layer" layouts, as the
       reference maps them) <-> doc::Adjustment. Levels and Curves keep the composite and per-channel
       records of RGB, Gray (one record for all three), CMYK and Lab documents. Blocks this build
       can't read (pencil curves, colour lookups, gradients in other colour spaces, CMYK channel
       mixers, unknown versions) stay Unsupported and are written back as they were. */

    const char* const ADJ_KEYS[] = {"levl", "curv", "hue2", "brit", "nvrt", "thrs", "post", "expA", "vibA", "blnc",
        "mixr", "grdm", "phfl", "selc", "blwh", "clrL"};
    const size_t ADJ_KEY_COUNT   = sizeof ADJ_KEYS / sizeof ADJ_KEYS[0];

    bool is_adjustment_key (const char* key)
    {
        for (size_t i = 0; i < ADJ_KEY_COUNT; i++)
        {
            if (memcmp (ADJ_KEYS[i], key, 4) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool be16 (const std::vector<uint8_t>& d, size_t at, uint16_t* out)
    {
        if (at + 2 > d.size ())
        {
            return false;
        }
        *out = (uint16_t) (d[at] << 8 | d[at + 1]);
        return true;
    }

    bool bei16 (const std::vector<uint8_t>& d, size_t at, float* out)
    {
        uint16_t v;
        if (!be16 (d, at, &v))
        {
            return false;
        }
        *out = (float) (int16_t) v;
        return true;
    }

    bool be32 (const std::vector<uint8_t>& d, size_t at, uint32_t* out)
    {
        if (at + 4 > d.size ())
        {
            return false;
        }
        *out = (uint32_t) d[at] << 24 | (uint32_t) d[at + 1] << 16 | (uint32_t) d[at + 2] << 8 | d[at + 3];
        return true;
    }

    bool bef32 (const std::vector<uint8_t>& d, size_t at, float* out)
    {
        uint32_t u;
        if (!be32 (d, at, &u))
        {
            return false;
        }
        memcpy (out, &u, sizeof (float));
        return true;
    }

    bool levels_rec (const std::vector<uint8_t>& d, size_t at, doc::LevelsChannel* c)
    {
        uint16_t v[5];
        for (int i = 0; i < 5; i++)
        {
            if (!be16 (d, at + (size_t) i * 2, &v[i]))
            {
                return false;
            }
        }
        c->in_black  = v[0] / 255.0f;
        c->in_white  = v[1] / 255.0f;
        c->out_black = v[2] / 255.0f;
        c->out_white = v[3] / 255.0f;
        c->gamma     = v[4] / 100.0f;
        return true;
    }

    /* A versioned descriptor (4-byte version, then the descriptor) at `at`. */
    bool versioned (const std::vector<uint8_t>& d, size_t at, DValue* out)
    {
        if (at >= d.size ())
        {
            return false;
        }
        Reader r = reader (d.data () + at, d.size () - at);
        rd_u32 (&r);
        return parse_descriptor (&r, out);
    }

    bool parse_curves (const std::vector<uint8_t>& d, doc::Adjustment* a)
    {
        if (d.empty () || d[0] != 0)
        {
            return false;
        }
        uint16_t version;
        uint32_t bits;
        if (!be16 (d, 1, &version) || (version != 1 && version != 4) || !be32 (d, 3, &bits))
        {
            return false;
        }
        std::vector<doc::CurvePoint> curves[5];
        for (int i = 0; i < 5; i++)
        {
            curves[i] = doc::identity_curve ();
        }
        size_t at = 7;
        for (int bit = 0; bit < 32; bit++)
        {
            if ((bits & (1u << bit)) == 0)
            {
                continue;
            }
            uint16_t n;
            if (!be16 (d, at, &n))
            {
                return false;
            }
            at += 2;
            std::vector<doc::CurvePoint> pts;
            for (uint16_t i = 0; i < n; i++)
            {
                uint16_t out, in;
                if (!be16 (d, at, &out) || !be16 (d, at + 2, &in))
                {
                    return false;
                }
                at += 4;
                doc::CurvePoint p = {in / 255.0f, out / 255.0f};
                pts.push_back (p);
            }
            if (bit < 5)
            {
                curves[bit] = pts;
            }
        }
        a->curves.master = curves[0];
        for (int i = 0; i < 3; i++)
        {
            a->curves.channels[i] = curves[i + 1];
        }
        a->curves.black  = curves[4];
        a->curves.pencil = false;
        return true;
    }

    bool stop_before (const doc::GradientStop& x, const doc::GradientStop& y)
    {
        return x.location < y.location;
    }

    bool parse_gradient_map (const std::vector<uint8_t>& d, doc::Adjustment* a)
    {
        uint16_t version;
        if (!be16 (d, 0, &version) || d.size () < 4)
        {
            return false;
        }
        a->gradient_map.reverse = d[2] != 0;
        a->gradient_map.dither  = d[3] != 0;
        size_t at;
        if (version == 1)
        {
            at = 4;
        }
        else if (version == 3)
        {
            at = 8;
        }
        else
        {
            return false;
        }
        uint32_t name_len;
        if (!be32 (d, at, &name_len) || name_len > 4096)
        {
            return false;
        }
        at += 4 + (size_t) name_len * 2;
        uint16_t n;
        if (!be16 (d, at, &n))
        {
            return false;
        }
        at += 2;
        std::vector<doc::GradientStop> stops;
        for (uint16_t i = 0; i < n; i++)
        {
            uint32_t loc;
            uint16_t space, c[4];
            if (!be32 (d, at, &loc) || !be16 (d, at + 8, &space))
            {
                return false;
            }
            for (int k = 0; k < 4; k++)
            {
                if (!be16 (d, at + 10 + (size_t) k * 2, &c[k]))
                {
                    return false;
                }
            }
            doc::GradientStop s;
            s.location = fminf (fmaxf (loc / 4096.0f, 0.0f), 1.0f);
            if (space == 0)
            {
                for (int k = 0; k < 3; k++)
                {
                    s.color[k] = c[k] / 65535.0f;
                }
            }
            else if (space == 8)
            {
                s.color[0] = s.color[1] = s.color[2] = 1.0f - c[0] / 10000.0f;
            }
            else
            {
                return false;
            }
            stops.push_back (s);
            at += 20;
        }
        if (stops.size () < 2)
        {
            return false;
        }
        std::sort (stops.begin (), stops.end (), stop_before);
        a->gradient_map.stops = stops;
        return true;
    }

    bool parse_photo_filter (const std::vector<uint8_t>& d, doc::Adjustment* a)
    {
        uint16_t version;
        if (!be16 (d, 0, &version))
        {
            return false;
        }
        size_t at;
        if (version == 2)
        {
            uint16_t space, c[3];
            if (!be16 (d, 2, &space) || space != 0)
            {
                return false;
            }
            for (int k = 0; k < 3; k++)
            {
                if (!be16 (d, 4 + (size_t) k * 2, &c[k]))
                {
                    return false;
                }
                a->photo_filter.color[k] = c[k] / 65535.0f;
            }
            at = 12;
        }
        else if (version == 3)
        {
            /* D50 XYZ with 1.0 = 32768 (as the reference infers it). */
            uint32_t x[3];
            float xyz[3], lin[3];
            for (int k = 0; k < 3; k++)
            {
                if (!be32 (d, 2 + (size_t) k * 4, &x[k]))
                {
                    return false;
                }
                xyz[k] = x[k] / 32768.0f;
                if (xyz[k] > 2.0f)
                {
                    return false;
                }
            }
            color::mat3_mul (color::XYZ_D50_TO_SRGB, xyz, lin);
            for (int k = 0; k < 3; k++)
            {
                a->photo_filter.color[k] =
                    fminf (fmaxf (color::linear_to_srgb (fminf (fmaxf (lin[k], 0.0f), 1.0f)), 0.0f), 1.0f);
            }
            at = 14;
        }
        else
        {
            return false;
        }
        uint32_t density;
        if (!be32 (d, at, &density) || at + 4 >= d.size ())
        {
            return false;
        }
        a->photo_filter.density             = fminf (density / 100.0f, 1.0f);
        a->photo_filter.preserve_luminosity = d[at + 4] != 0;
        return true;
    }

    bool parse_any (
        const std::string& key, const std::vector<uint8_t>& d, const std::vector<uint8_t>* cged, doc::Adjustment* a)
    {
        uint16_t v;
        float f;
        if (key == "nvrt")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::Invert);
            return true;
        }
        if (key == "thrs")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::Threshold);
            if (!be16 (d, 0, &v))
            {
                return false;
            }
            a->threshold_level = v / 255.0f;
            return true;
        }
        if (key == "post")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::Posterize);
            if (!be16 (d, 0, &v))
            {
                return false;
            }
            a->posterize_levels = v;
            return true;
        }
        if (key == "brit")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::BrightnessContrast);
            DValue desc;
            if (cged != NULL && versioned (*cged, 0, &desc) && d_get (desc, "Brgh") != NULL)
            {
                a->brightness_contrast.brightness = (float) d_num (desc, "Brgh", 0);
                a->brightness_contrast.contrast   = (float) d_num (desc, "Cntr", 0);
                a->brightness_contrast.legacy     = d_flag (desc, "useLegacy", false);
                return true;
            }
            float b, c;
            if (!bei16 (d, 0, &b) || !bei16 (d, 2, &c))
            {
                return false;
            }
            a->brightness_contrast.brightness = b;
            a->brightness_contrast.contrast   = c;
            a->brightness_contrast.legacy     = true;
            return true;
        }
        if (key == "hue2")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::HueSaturation);
            if (d.size () < 16)
            {
                return false;
            }
            bool colorize = d[2] != 0;
            size_t base   = colorize ? 4 : 10;
            float h, s, l;
            if (!bei16 (d, base, &h) || !bei16 (d, base + 2, &s) || !bei16 (d, base + 4, &l))
            {
                return false;
            }
            a->hue_saturation.hue        = h;
            a->hue_saturation.saturation = s;
            a->hue_saturation.lightness  = l;
            a->hue_saturation.colorize   = colorize;
            for (int i = 0; i < 6; i++)
            {
                size_t at = 16 + (size_t) i * 14;
                float r[7];
                bool ok = true;
                for (int k = 0; k < 7 && ok; k++)
                {
                    ok = bei16 (d, at + (size_t) k * 2, &r[k]);
                }
                if (!ok)
                {
                    continue;
                }
                doc::HueRange& hr = a->hue_saturation.ranges[i];
                hr.hue            = r[4];
                hr.saturation     = r[5];
                hr.lightness      = r[6];
                doc::canonical_hue_bounds (r, hr.bounds);
            }
            return true;
        }
        if (key == "expA")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::Exposure);
            return bef32 (d, 2, &a->exposure.exposure) && bef32 (d, 6, &a->exposure.offset)
                && bef32 (d, 10, &a->exposure.gamma);
        }
        if (key == "levl")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::Levels);
            if (!levels_rec (d, 2, &a->levels.master))
            {
                return false;
            }
            for (int i = 0; i < 3; i++)
            {
                if (!levels_rec (d, 12 + (size_t) i * 10, &a->levels.channels[i]))
                {
                    return false;
                }
            }
            levels_rec (d, 42, &a->levels.black);
            return true;
        }
        if (key == "curv")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::Curves);
            return parse_curves (d, a);
        }
        if (key == "selc")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::SelectiveColor);
            if (!be16 (d, 0, &v) || v != 1 || d.size () < 84)
            {
                return false;
            }
            be16 (d, 2, &v);
            a->selective_color.relative = v == 0;
            for (int r = 0; r < 9; r++)
            {
                for (int k = 0; k < 4; k++)
                {
                    if (!bei16 (d, 4 + (size_t) (r + 1) * 8 + (size_t) k * 2, &f))
                    {
                        return false;
                    }
                    a->selective_color.adjustments[r][k] = f;
                }
            }
            return true;
        }
        if (key == "grdm")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::GradientMap);
            return parse_gradient_map (d, a);
        }
        if (key == "vibA")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::Vibrance);
            DValue desc;
            if (!versioned (d, 0, &desc))
            {
                return false;
            }
            a->vibrance.vibrance   = (float) fmin (fmax (d_num (desc, "vibrance", 0), -100), 100);
            a->vibrance.saturation = (float) fmin (fmax (d_num (desc, "Strt", 0), -100), 100);
            return true;
        }
        if (key == "blnc")
        {
            *a              = doc::Adjustment::make (doc::AdjustmentKind::ColorBalance);
            float* tones[3] = {a->color_balance.shadows, a->color_balance.midtones, a->color_balance.highlights};
            for (int t = 0; t < 3; t++)
            {
                for (int i = 0; i < 3; i++)
                {
                    if (!bei16 (d, (size_t) (t * 3 + i) * 2, &f))
                    {
                        return false;
                    }
                    tones[t][i] = fminf (fmaxf (f, -100.0f), 100.0f);
                }
            }
            if (d.size () < 19)
            {
                return false;
            }
            a->color_balance.preserve_luminosity = d[18] != 0;
            return true;
        }
        if (key == "blwh")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::BlackWhite);
            DValue desc;
            if (!versioned (d, 0, &desc))
            {
                return false;
            }
            const char* keys[6] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
            for (int i = 0; i < 6; i++)
            {
                a->black_white.weights[i] =
                    (float) fmin (fmax (d_num (desc, keys[i], a->black_white.weights[i]), -200), 300);
            }
            a->black_white.has_tint = d_flag (desc, "useTint", false);
            const DValue* tc        = d_get (desc, "tintColor");
            if (tc != NULL && tc->type == DType::Object && d_get (*tc, "Rd  ") != NULL)
            {
                a->black_white.tint[0] = (float) d_num (*tc, "Rd  ", 0) / 255.0f;
                a->black_white.tint[1] = (float) d_num (*tc, "Grn ", 0) / 255.0f;
                a->black_white.tint[2] = (float) d_num (*tc, "Bl  ", 0) / 255.0f;
            }
            return true;
        }
        if (key == "phfl")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::PhotoFilter);
            return parse_photo_filter (d, a);
        }
        if (key == "mixr")
        {
            *a = doc::Adjustment::make (doc::AdjustmentKind::ChannelMixer);
            if (!be16 (d, 0, &v) || v != 1)
            {
                return false;
            }
            be16 (d, 2, &v);
            a->channel_mixer.monochrome = v != 0;
            for (int r = 0; r < 3; r++)
            {
                size_t at = 4 + (size_t) r * 10;
                float x[5];
                bool ok = true;
                for (int k = 0; k < 5 && ok; k++)
                {
                    ok = bei16 (d, at + (size_t) k * 2, &x[k]);
                }
                if (!ok)
                {
                    if (r > 0 && a->channel_mixer.monochrome)
                    {
                        break;
                    }
                    return false;
                }
                for (int k = 0; k < 3; k++)
                {
                    a->channel_mixer.matrix[r][k] = fminf (fmaxf (x[k], -200.0f), 200.0f) / 100.0f;
                }
                a->channel_mixer.matrix[r][3] = fminf (fmaxf (x[4], -200.0f), 200.0f) / 100.0f;
            }
            return true;
        }
        return false;
    }

    doc::Adjustment unsupported (const std::string& key, const std::vector<uint8_t>& data)
    {
        doc::Adjustment a     = doc::Adjustment::make (doc::AdjustmentKind::Unsupported);
        a.unsupported.psd_key = key;
        a.unsupported.raw     = data;
        return a;
    }

    doc::Adjustment parse_adjustment (const std::string& key, const std::vector<uint8_t>& data,
        const std::vector<uint8_t>* cged, color::ColorMode mode)
    {
        /* A CMYK channel mixer with a black source has no RGB meaning. */
        if (key == "mixr" && mode == color::ColorMode::Cmyk)
        {
            for (size_t r = 0; 4 + r * 10 + 8 <= data.size (); r++)
            {
                if (data[4 + r * 10 + 6] != 0 || data[4 + r * 10 + 7] != 0)
                {
                    return unsupported (key, data);
                }
            }
        }
        doc::Adjustment a;
        if (!parse_any (key, data, cged, &a))
        {
            return unsupported (key, data);
        }
        if (a.kind == doc::AdjustmentKind::Levels)
        {
            if (mode == color::ColorMode::Cmyk)
            {
                a.levels.space = doc::ToneSpace::Cmyk;
            }
            else if (mode == color::ColorMode::Lab)
            {
                a.levels.space = doc::ToneSpace::Lab;
            }
            if (mode != color::ColorMode::Cmyk)
            {
                a.levels.black = doc::LevelsChannel::identity ();
            }
            if (mode == color::ColorMode::Grayscale)
            {
                a.levels.channels[1] = a.levels.channels[2] = a.levels.channels[0];
            }
        }
        else if (a.kind == doc::AdjustmentKind::Curves)
        {
            if (mode == color::ColorMode::Cmyk)
            {
                a.curves.space = doc::ToneSpace::Cmyk;
            }
            else if (mode == color::ColorMode::Lab)
            {
                a.curves.space = doc::ToneSpace::Lab;
            }
            if (mode != color::ColorMode::Cmyk || doc::is_identity_curve (a.curves.black))
            {
                a.curves.black.clear ();
            }
            if (mode == color::ColorMode::Grayscale)
            {
                a.curves.channels[1] = a.curves.channels[2] = a.curves.channels[0];
            }
        }
        return a;
    }

    void put16 (std::vector<uint8_t>* v, uint16_t x)
    {
        wr_u16 (v, x);
    }

    void puti16 (std::vector<uint8_t>* v, float x)
    {
        float r = roundf (x);
        wr_u16 (v, (uint16_t) (int16_t) (r < -32768.0f ? -32768.0f : r > 32767.0f ? 32767.0f : r));
    }

    uint16_t q255 (float v)
    {
        return (uint16_t) lroundf (fminf (fmaxf (v, 0.0f), 1.0f) * 255.0f);
    }

    void levels_write (std::vector<uint8_t>* v, const doc::LevelsChannel& c)
    {
        put16 (v, q255 (c.in_black));
        put16 (v, q255 (c.in_white));
        put16 (v, q255 (c.out_black));
        put16 (v, q255 (c.out_white));
        put16 (v, (uint16_t) fminf (fmaxf (roundf (c.gamma * 100.0f), 1.0f), 999.0f));
    }

    std::vector<uint8_t> versioned_bytes (const DValue& desc)
    {
        std::vector<uint8_t> out;
        wr_u32 (&out, 16);
        write_descriptor (&out, desc);
        return out;
    }

    std::vector<AdjBlock> write_adjustment (const doc::Adjustment& a)
    {
        std::vector<AdjBlock> out;
        std::vector<uint8_t> v;
        AdjBlock b;
        switch (a.kind)
        {
        case doc::AdjustmentKind::Invert:
            b.key = "nvrt";
            break;
        case doc::AdjustmentKind::Threshold:
            b.key = "thrs";
            put16 (&v, std::max<uint16_t> (q255 (a.threshold_level), 1));
            put16 (&v, 0);
            break;
        case doc::AdjustmentKind::Posterize:
            b.key = "post";
            put16 (&v, (uint16_t) std::min<uint32_t> (std::max<uint32_t> (a.posterize_levels, 2), 255));
            put16 (&v, 0);
            break;
        case doc::AdjustmentKind::BrightnessContrast:
            {
                b.key = "brit";
                puti16 (&v, a.brightness_contrast.brightness);
                puti16 (&v, a.brightness_contrast.contrast);
                put16 (&v, 127);
                v.push_back (0);
                DValue d = d_object ("null");
                d_set (&d, "Vrsn", d_long (1));
                d_set (&d, "Brgh", d_long ((int32_t) lroundf (a.brightness_contrast.brightness)));
                d_set (&d, "Cntr", d_long ((int32_t) lroundf (a.brightness_contrast.contrast)));
                d_set (&d, "means", d_long (127));
                d_set (&d, "Lab ", d_bool (false));
                d_set (&d, "useLegacy", d_bool (a.brightness_contrast.legacy));
                d_set (&d, "Auto", d_bool (false));
                AdjBlock cg;
                cg.key  = "CgEd";
                cg.data = versioned_bytes (d);
                b.data  = v;
                out.push_back (b);
                out.push_back (cg);
                return out;
            }
        case doc::AdjustmentKind::HueSaturation:
            {
                b.key = "hue2";
                put16 (&v, 2);
                v.push_back (a.hue_saturation.colorize ? 1 : 0);
                v.push_back (0);
                for (int rep = 0; rep < 2; rep++)
                {
                    puti16 (&v, a.hue_saturation.hue);
                    puti16 (&v, a.hue_saturation.saturation);
                    puti16 (&v, a.hue_saturation.lightness);
                }
                for (int i = 0; i < 6; i++)
                {
                    const doc::HueRange& r = a.hue_saturation.ranges[i];
                    for (int k = 0; k < 4; k++)
                    {
                        float x = fmodf (r.bounds[k], 360.0f);
                        if (x < 0.0f)
                        {
                            x += 360.0f;
                        }
                        put16 (&v, (uint16_t) ((uint16_t) lroundf (x) % 360));
                    }
                    puti16 (&v, r.hue);
                    puti16 (&v, r.saturation);
                    puti16 (&v, r.lightness);
                }
                break;
            }
        case doc::AdjustmentKind::Exposure:
            {
                b.key = "expA";
                put16 (&v, 1);
                float xs[3] = {a.exposure.exposure, a.exposure.offset, a.exposure.gamma};
                for (int i = 0; i < 3; i++)
                {
                    uint32_t u;
                    memcpy (&u, &xs[i], sizeof u);
                    wr_u32 (&v, u);
                }
                v.push_back (1);
                break;
            }
        case doc::AdjustmentKind::Levels:
            {
                b.key = "levl";
                put16 (&v, 2);
                levels_write (&v, a.levels.master);
                for (int i = 0; i < 3; i++)
                {
                    levels_write (&v, a.levels.channels[i]);
                }
                levels_write (
                    &v, a.levels.space == doc::ToneSpace::Cmyk ? a.levels.black : doc::LevelsChannel::identity ());
                for (int i = 5; i < 29; i++)
                {
                    levels_write (&v, doc::LevelsChannel::identity ());
                }
                break;
            }
        case doc::AdjustmentKind::Curves:
            {
                b.key = "curv";
                v.push_back (0);
                put16 (&v, 1);
                bool ink = a.curves.space == doc::ToneSpace::Cmyk && a.curves.black.size () >= 2;
                wr_u32 (&v, ink ? 0x1Fu : 0x0Fu);
                const std::vector<doc::CurvePoint>* cs[5] = {&a.curves.master, &a.curves.channels[0],
                    &a.curves.channels[1], &a.curves.channels[2], &a.curves.black};
                for (int i = 0; i < (ink ? 5 : 4); i++)
                {
                    std::vector<doc::CurvePoint> pts = cs[i]->size () >= 2 ? *cs[i] : doc::identity_curve ();
                    size_t n                         = std::min<size_t> (pts.size (), 19);
                    put16 (&v, (uint16_t) n);
                    for (size_t k = 0; k < n; k++)
                    {
                        put16 (&v, q255 (pts[k].output));
                        put16 (&v, q255 (pts[k].input));
                    }
                }
                break;
            }
        case doc::AdjustmentKind::SelectiveColor:
            b.key = "selc";
            put16 (&v, 1);
            put16 (&v, a.selective_color.relative ? 0 : 1);
            for (int i = 0; i < 8; i++)
            {
                v.push_back (0);
            }
            for (int r = 0; r < 9; r++)
            {
                for (int k = 0; k < 4; k++)
                {
                    puti16 (&v, fminf (fmaxf (a.selective_color.adjustments[r][k], -100.0f), 100.0f));
                }
            }
            break;
        case doc::AdjustmentKind::GradientMap:
            {
                b.key                                = "grdm";
                std::vector<doc::GradientStop> stops = a.gradient_map.stops;
                if (stops.empty ())
                {
                    doc::GradientStop s0 = {0.0f, {0, 0, 0}}, s1 = {1.0f, {1, 1, 1}};
                    stops.push_back (s0);
                    stops.push_back (s1);
                }
                else if (stops.size () == 1)
                {
                    doc::GradientStop s = stops[0];
                    s.location          = 0.0f;
                    stops[0]            = s;
                    s.location          = 1.0f;
                    stops.push_back (s);
                }
                put16 (&v, 1);
                v.push_back (a.gradient_map.reverse ? 1 : 0);
                v.push_back (a.gradient_map.dither ? 1 : 0);
                const char* name = "Custom";
                wr_u32 (&v, (uint32_t) strlen (name) + 1);
                for (const char* c = name; *c != 0; c++)
                {
                    put16 (&v, (uint16_t) *c);
                }
                put16 (&v, 0);
                put16 (&v, (uint16_t) stops.size ());
                for (size_t i = 0; i < stops.size (); i++)
                {
                    wr_u32 (&v, (uint32_t) lroundf (fminf (fmaxf (stops[i].location, 0.0f), 1.0f) * 4096.0f));
                    wr_u32 (&v, 50);
                    put16 (&v, 0);
                    for (int k = 0; k < 3; k++)
                    {
                        put16 (&v, (uint16_t) lroundf (fminf (fmaxf (stops[i].color[k], 0.0f), 1.0f) * 65535.0f));
                    }
                    put16 (&v, 0);
                    put16 (&v, 0);
                }
                put16 (&v, 2);
                for (uint32_t t = 0; t <= 4096; t += 4096)
                {
                    wr_u32 (&v, t);
                    wr_u32 (&v, 50);
                    put16 (&v, 255);
                }
                put16 (&v, 2);
                put16 (&v, 0);
                put16 (&v, 32);
                put16 (&v, 0);
                wr_u32 (&v, 0);
                put16 (&v, 0);
                put16 (&v, 0);
                wr_u32 (&v, 2048);
                put16 (&v, 3);
                const uint16_t tail[8] = {0, 0, 0, 0, 0x8000, 0x8000, 0x8000, 0x8000};
                for (int i = 0; i < 8; i++)
                {
                    put16 (&v, tail[i]);
                }
                put16 (&v, 0);
                break;
            }
        case doc::AdjustmentKind::Vibrance:
            {
                b.key    = "vibA";
                DValue d = d_object ("null");
                d_set (
                    &d, "vibrance", d_long ((int32_t) lroundf (fminf (fmaxf (a.vibrance.vibrance, -100.0f), 100.0f))));
                d_set (&d, "Strt", d_long ((int32_t) lroundf (fminf (fmaxf (a.vibrance.saturation, -100.0f), 100.0f))));
                v = versioned_bytes (d);
                break;
            }
        case doc::AdjustmentKind::ColorBalance:
            {
                b.key                 = "blnc";
                const float* tones[3] = {a.color_balance.shadows, a.color_balance.midtones, a.color_balance.highlights};
                for (int t = 0; t < 3; t++)
                {
                    for (int i = 0; i < 3; i++)
                    {
                        puti16 (&v, fminf (fmaxf (tones[t][i], -100.0f), 100.0f));
                    }
                }
                v.push_back (a.color_balance.preserve_luminosity ? 1 : 0);
                v.push_back (0);
                break;
            }
        case doc::AdjustmentKind::BlackWhite:
            {
                b.key               = "blwh";
                DValue d            = d_object ("null");
                const char* keys[6] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
                for (int i = 0; i < 6; i++)
                {
                    d_set (&d, keys[i],
                        d_long ((int32_t) lroundf (fminf (fmaxf (a.black_white.weights[i], -200.0f), 300.0f))));
                }
                d_set (&d, "useTint", d_bool (a.black_white.has_tint));
                DValue tint = d_object ("RGBC");
                d_set (&tint, "Rd  ", d_double (a.black_white.tint[0] * 255.0));
                d_set (&tint, "Grn ", d_double (a.black_white.tint[1] * 255.0));
                d_set (&tint, "Bl  ", d_double (a.black_white.tint[2] * 255.0));
                d_set (&d, "tintColor", tint);
                d_set (&d, "bwPresetKind", d_long (1));
                d_set (&d, "blackAndWhitePresetFileName", d_text (""));
                v = versioned_bytes (d);
                break;
            }
        case doc::AdjustmentKind::PhotoFilter:
            b.key = "phfl";
            put16 (&v, 2);
            put16 (&v, 0);
            for (int k = 0; k < 3; k++)
            {
                put16 (&v, (uint16_t) lroundf (fminf (fmaxf (a.photo_filter.color[k], 0.0f), 1.0f) * 65535.0f));
            }
            put16 (&v, 0);
            wr_u32 (&v, (uint32_t) lroundf (fminf (fmaxf (a.photo_filter.density, 0.0f), 1.0f) * 100.0f));
            v.push_back (a.photo_filter.preserve_luminosity ? 1 : 0);
            /* Photoshop wants the payload padded to 4 bytes inside the block. */
            v.push_back (0);
            v.push_back (0);
            v.push_back (0);
            break;
        case doc::AdjustmentKind::ChannelMixer:
            b.key = "mixr";
            put16 (&v, 1);
            put16 (&v, a.channel_mixer.monochrome ? 1 : 0);
            for (int r = 0; r < 4; r++)
            {
                const float* row = a.channel_mixer.matrix[r < 3 ? r : 0];
                for (int k = 0; k < 3; k++)
                {
                    puti16 (&v, fminf (fmaxf (row[k] * 100.0f, -200.0f), 200.0f));
                }
                put16 (&v, 0);
                puti16 (&v, fminf (fmaxf (row[3] * 100.0f, -200.0f), 200.0f));
            }
            break;
        case doc::AdjustmentKind::Unsupported:
            if (a.unsupported.psd_key.size () != 4)
            {
                return out;
            }
            b.key = a.unsupported.psd_key;
            v     = a.unsupported.raw;
            break;
        default:
            /* Color Lookup tables aren't written yet. */
            return out;
        }
        b.data = v;
        out.push_back (b);
        return out;
    }

} // namespace psd
