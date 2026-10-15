#include "compose/adjust.h"

#include "color/blend.h"
#include <algorithm>
#include <float.h>
#include <math.h>
#include <string.h>

namespace compose
{

    /* Evaluation of adjustments on a straight-RGBA buffer: Image > Adjustments (destructive) and
       adjustment layers both come here. Formulas are approximations of Photoshop behaviour measured
       against Photoshop-rendered composites; the comments keep the measurements they rest on. */

    using doc::Adjustment;
    using doc::AdjustmentKind;
    using doc::CurvePoint;
    using doc::LevelsChannel;
    using doc::ToneSpace;

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

    float clampf (float v, float lo, float hi)
    {
        if (v < lo)
        {
            return lo;
        }
        if (v > hi)
        {
            return hi;
        }
        return v;
    }

    float rem1 (float v)
    {
        float r = fmodf (v, 1.0f);
        return r < 0.0f ? r + 1.0f : r;
    }

    float signf (float v)
    {
        return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
    }

    /* The document tone curve that adjustments working in linear light (Exposure) go through: sRGB
       for colour documents, the "Dot Gain 20%" grey curve (about gamma 1.73) for grayscale ones.
       32-bit documents store linear light already. */
    Transfer Transfer::for_document (color::ColorMode mode, color::SampleType depth)
    {
        Transfer t = {false, 1.0f};
        if (depth == color::SampleType::F32)
        {
            return t;
        }
        if (mode == color::ColorMode::Grayscale || mode == color::ColorMode::Duotone
            || mode == color::ColorMode::Bitmap)
        {
            t.gamma = 1.732f;
            return t;
        }
        t.srgb = true;
        return t;
    }

    /* Exposure linearises RGB documents through a pure 2.2 power, not the sRGB curve. */
    Transfer Transfer::for_exposure () const
    {
        if (!srgb)
        {
            return *this;
        }
        Transfer t = {false, 2.2f};
        return t;
    }

    float Transfer::decode (float v) const
    {
        if (srgb)
        {
            return color::srgb_to_linear (fmaxf (v, 0.0f));
        }
        if (gamma == 1.0f)
        {
            return fmaxf (v, 0.0f);
        }
        return powf (fmaxf (v, 0.0f), gamma);
    }

    float Transfer::encode (float v) const
    {
        if (srgb)
        {
            return color::linear_to_srgb (fmaxf (v, 0.0f));
        }
        if (gamma == 1.0f)
        {
            return fmaxf (v, 0.0f);
        }
        return powf (fmaxf (v, 0.0f), 1.0f / gamma);
    }

    /* Steps per unit of an integer document's samples: adjustment layers round there (8-bit: 255
       levels, 16-bit: 32768), as Photoshop does. False for 32-bit float. */
    bool adjustment_quantum (color::SampleType depth, float* out)
    {
        if (depth == color::SampleType::U8)
        {
            *out = 255.0f;
        }
        else if (depth == color::SampleType::U16)
        {
            *out = 32768.0f;
        }
        else
        {
            return false;
        }
        return true;
    }

    float lut_sample (const std::vector<float>& table, float v)
    {
        float x  = clamp01 (v) * (float) (table.size () - 1);
        size_t i = (size_t) floorf (x);
        if (i > table.size () - 1)
        {
            i = table.size () - 1;
        }
        size_t j = i + 1 < table.size () ? i + 1 : table.size () - 1;
        float f  = x - (float) i;
        return table[i] * (1.0f - f) + table[j] * f;
    }

    float lut_x (size_t k)
    {
        return (float) k / (float) (LUT_SIZE - 1);
    }

    /* Photoshop posterize: n equal input bins over 0..255, output levels floor(k * 255 / (n - 1)). */
    float posterize (float v, uint32_t levels)
    {
        float n   = (float) (levels < 2 ? 2 : levels > 255 ? 255 : levels);
        float x   = roundf (clamp01 (v) * 255.0f);
        float bin = fminf (floorf (x * n / 256.0f), n - 1.0f);
        return floorf (bin * 255.0f / (n - 1.0f)) / 255.0f;
    }

    /* Levels on whole levels when `quantum` is given: the input-range stretch is rounded to a whole
       level before the midtone gamma, as Photoshop does. A gamma above 1 gets Photoshop's bounded
       shadow toe (a p-norm soft-min of the power curve with a line of slope about 2^gamma). */
    float levels_q (const LevelsChannel& ch, float v, bool has_quantum, float quantum)
    {
        float range = fmaxf (ch.in_white - ch.in_black, 1e-6f);
        float u;
        if (has_quantum && isfinite (quantum) && quantum >= 1.0f)
        {
            float vq = roundf (v * quantum) / quantum;
            u        = floorf (clamp01 ((vq - ch.in_black) / range) * quantum + 0.5f + 1e-3f) / quantum;
        }
        else
        {
            u = clamp01 ((v - ch.in_black) / range);
        }
        float g = fmaxf (ch.gamma, 0.01f);
        float t;
        if (g > 1.0f)
        {
            float power = powf (u, 1.0f / g);
            float line  = 0.93f * powf (2.0f, g) * u;
            if (power <= 1e-6f || line <= 1e-6f)
            {
                t = fminf (power, line);
            }
            else
            {
                const float p = 10.0f;
                t             = powf (powf (power, -p) + powf (line, -p), -1.0f / p);
            }
        }
        else
        {
            t = powf (u, 1.0f / g);
        }
        return ch.out_black + t * (ch.out_white - ch.out_black);
    }

    /* Levels in a 32-bit document: nothing clipped, a plain power gamma mirrored below black. */
    float levels_float (const LevelsChannel& ch, float v)
    {
        float t = (v - ch.in_black) / fmaxf (ch.in_white - ch.in_black, 1e-6f);
        if (ch.gamma != 1.0f)
        {
            t = signf (t) * powf (fabsf (t), 1.0f / fmaxf (ch.gamma, 0.01f));
        }
        return ch.out_black + t * (ch.out_white - ch.out_black);
    }

    bool point_less (const CurvePoint& a, const CurvePoint& b)
    {
        return a.input < b.input;
    }

    std::vector<CurvePoint> sorted_unique (const std::vector<CurvePoint>& points)
    {
        std::vector<CurvePoint> pts;
        for (size_t i = 0; i < points.size (); i++)
        {
            if (isfinite (points[i].input) && isfinite (points[i].output))
            {
                pts.push_back (points[i]);
            }
        }
        std::stable_sort (pts.begin (), pts.end (), point_less);
        std::vector<CurvePoint> out;
        for (size_t i = 0; i < pts.size (); i++)
        {
            if (!out.empty () && fabsf (pts[i].input - out.back ().input) < 1e-6f)
            {
                continue;
            }
            out.push_back (pts[i]);
        }
        return out;
    }

    std::vector<float> identity_lut ()
    {
        std::vector<float> t (LUT_SIZE);
        for (size_t k = 0; k < LUT_SIZE; k++)
        {
            t[k] = lut_x (k);
        }
        return t;
    }

    /* Photoshop's curve: a natural cubic spline through the points (it may overshoot between them,
       as Photoshop's does), constant outside the first and last point, clamped to 0..1. */
    std::vector<float> curve_lut (const std::vector<CurvePoint>& points)
    {
        std::vector<CurvePoint> pts = sorted_unique (points);
        if (pts.size () < 2)
        {
            return identity_lut ();
        }
        size_t n = pts.size ();
        std::vector<double> m2 (n, 0.0);
        if (n > 2)
        {
            std::vector<double> c (n, 0.0), d (n, 0.0);
            for (size_t i = 1; i + 1 < n; i++)
            {
                double h0 = (double) pts[i].input - pts[i - 1].input;
                double h1 = (double) pts[i + 1].input - pts[i].input;
                double a = h0 / 6.0, b = (h0 + h1) / 3.0, cc = h1 / 6.0;
                double r = ((double) pts[i + 1].output - pts[i].output) / h1
                         - ((double) pts[i].output - pts[i - 1].output) / h0;
                double denom = b - a * c[i - 1];
                c[i]         = cc / denom;
                d[i]         = (r - a * d[i - 1]) / denom;
            }
            for (size_t i = n - 1; i-- > 1;)
            {
                m2[i] = d[i] - c[i] * m2[i + 1];
            }
        }
        std::vector<float> out (LUT_SIZE);
        for (size_t k = 0; k < LUT_SIZE; k++)
        {
            float xv = lut_x (k);
            if (xv <= pts[0].input)
            {
                out[k] = clamp01 (pts[0].output);
                continue;
            }
            if (xv >= pts[n - 1].input)
            {
                out[k] = clamp01 (pts[n - 1].output);
                continue;
            }
            size_t i = n - 2;
            for (size_t s = 0; s + 1 < n; s++)
            {
                if (xv <= pts[s + 1].input)
                {
                    i = s;
                    break;
                }
            }
            double x0 = pts[i].input, y0 = pts[i].output, x1 = pts[i + 1].input, y1 = pts[i + 1].output;
            double h = x1 - x0, t = xv;
            double a = (x1 - t) / h, b = (t - x0) / h;
            double y = a * y0 + b * y1 + ((a * a * a - a) * m2[i] + (b * b * b - b) * m2[i + 1]) * h * h / 6.0;
            out[k]   = clamp01 ((float) y);
        }
        return out;
    }

    /* A pencil-drawn Curves map: straight segments between its entries, constant outside. */
    std::vector<float> curve_map_lut (const std::vector<CurvePoint>& points)
    {
        std::vector<CurvePoint> pts = sorted_unique (points);
        if (pts.empty ())
        {
            return identity_lut ();
        }
        std::vector<float> out (LUT_SIZE);
        size_t seg = 0;
        for (size_t k = 0; k < LUT_SIZE; k++)
        {
            float xv = lut_x (k);
            if (xv <= pts.front ().input)
            {
                out[k] = clamp01 (pts.front ().output);
                continue;
            }
            if (xv >= pts.back ().input)
            {
                out[k] = clamp01 (pts.back ().output);
                continue;
            }
            while (seg + 1 < pts.size () && pts[seg + 1].input < xv)
            {
                seg++;
            }
            if (seg + 1 >= pts.size ())
            {
                out[k] = clamp01 (pts.back ().output);
                continue;
            }
            float x0 = pts[seg].input, y0 = pts[seg].output, x1 = pts[seg + 1].input, y1 = pts[seg + 1].output;
            float t = x1 > x0 ? (xv - x0) / (x1 - x0) : 0.0f;
            out[k]  = clamp01 (y0 + (y1 - y0) * t);
        }
        return out;
    }

    std::vector<float> curve_lut_mode (const std::vector<CurvePoint>& points, bool pencil)
    {
        return pencil ? curve_map_lut (points) : curve_lut (points);
    }

    /* Per-channel LUTs of Levels or Curves: each channel's record first, then the master record, as
       Photoshop does. Rows: the three channels, then black (identity unless CMYK). Lab has no
       composite record, so the master is ignored there. */
    void tone_luts (const Adjustment& adj, bool has_quantum, float quantum, std::vector<float> luts[4])
    {
        if (adj.kind == AdjustmentKind::Levels)
        {
            const doc::LevelsParams& L   = adj.levels;
            LevelsChannel ident          = LevelsChannel::identity ();
            const LevelsChannel& m       = L.space == ToneSpace::Lab ? ident : L.master;
            const LevelsChannel* rows[4] = {
                &L.channels[0], &L.channels[1], &L.channels[2], L.space == ToneSpace::Cmyk ? &L.black : &ident};
            for (int r = 0; r < 4; r++)
            {
                luts[r].resize (LUT_SIZE);
                for (size_t k = 0; k < LUT_SIZE; k++)
                {
                    luts[r][k] =
                        levels_q (m, levels_q (*rows[r], lut_x (k), has_quantum, quantum), has_quantum, quantum);
                }
            }
            return;
        }
        if (adj.kind == AdjustmentKind::Curves)
        {
            const doc::CurvesParams& C = adj.curves;
            std::vector<float> m = C.space == ToneSpace::Lab ? identity_lut () : curve_lut_mode (C.master, C.pencil);
            std::vector<CurvePoint> none;
            const std::vector<CurvePoint>* rows[4] = {
                &C.channels[0], &C.channels[1], &C.channels[2], C.space == ToneSpace::Cmyk ? &C.black : &none};
            for (int r = 0; r < 4; r++)
            {
                std::vector<float> own = curve_lut_mode (*rows[r], C.pencil);
                luts[r].resize (LUT_SIZE);
                for (size_t k = 0; k < LUT_SIZE; k++)
                {
                    luts[r][k] = lut_sample (m, own[k]);
                }
            }
            return;
        }
        for (int r = 0; r < 4; r++)
        {
            luts[r] = identity_lut ();
        }
    }

    /* Channel LUTs on a display-RGB colour in CMYK (ink brightness) or Lab space. The change is
       added as the difference of two round trips, so untouched channels and out-of-gamut colours
       stay exact. */
    void tone_in_space (
        ToneSpace space, const std::vector<float> luts[4], const float c[3], const color::CmykSpace* cmyk, float out[3])
    {
        float before[3], after[3];
        if (space == ToneSpace::Cmyk)
        {
            float ink[4], ink_out[4];
            color::rgb_to_cmyk (cmyk, c, ink);
            bool same = true;
            for (int i = 0; i < 4; i++)
            {
                ink_out[i] = 1.0f - lut_sample (luts[i], 1.0f - ink[i]);
                if (ink_out[i] != ink[i])
                {
                    same = false;
                }
            }
            if (same)
            {
                memcpy (out, c, sizeof (float) * 3);
                return;
            }
            color::cmyk_to_rgb (cmyk, ink, before);
            color::cmyk_to_rgb (cmyk, ink_out, after);
        }
        else if (space == ToneSpace::Lab)
        {
            float l[3];
            color::srgb_to_lab (c, l);
            float n[3] = {l[0] / 100.0f, (l[1] + 128.0f) / 255.0f, (l[2] + 128.0f) / 255.0f};
            float o[3];
            bool same = true;
            for (int i = 0; i < 3; i++)
            {
                o[i] = lut_sample (luts[i], n[i]);
                if (o[i] != n[i])
                {
                    same = false;
                }
            }
            if (same)
            {
                memcpy (out, c, sizeof (float) * 3);
                return;
            }
            float lb[3] = {n[0] * 100.0f, n[1] * 255.0f - 128.0f, n[2] * 255.0f - 128.0f};
            float la[3] = {o[0] * 100.0f, o[1] * 255.0f - 128.0f, o[2] * 255.0f - 128.0f};
            color::lab_to_srgb (lb, before);
            color::lab_to_srgb (la, after);
        }
        else
        {
            for (int i = 0; i < 3; i++)
            {
                out[i] = lut_sample (luts[i], c[i]);
            }
            return;
        }
        for (int i = 0; i < 3; i++)
        {
            out[i] = clamp01 (c[i] + after[i] - before[i]);
        }
    }

    void rgb_to_hsl (const float c[3], float* h, float* s, float* l)
    {
        float mx = fmaxf (fmaxf (c[0], c[1]), c[2]);
        float mn = fminf (fminf (c[0], c[1]), c[2]);
        *l       = (mx + mn) / 2.0f;
        if (fabsf (mx - mn) < 1e-7f)
        {
            *h = 0.0f;
            *s = 0.0f;
            return;
        }
        float d = mx - mn;
        *s      = *l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
        float hh;
        if (mx == c[0])
        {
            hh = fmodf ((c[1] - c[2]) / d, 6.0f);
            if (hh < 0.0f)
            {
                hh += 6.0f;
            }
        }
        else if (mx == c[1])
        {
            hh = (c[2] - c[0]) / d + 2.0f;
        }
        else
        {
            hh = (c[0] - c[1]) / d + 4.0f;
        }
        *h = hh / 6.0f;
    }

    float hsl_channel (float p, float q, float t)
    {
        t = rem1 (t);
        if (t < 1.0f / 6.0f)
        {
            return p + (q - p) * 6.0f * t;
        }
        if (t < 0.5f)
        {
            return q;
        }
        if (t < 2.0f / 3.0f)
        {
            return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        }
        return p;
    }

    void hsl_to_rgb (float h, float s, float l, float out[3])
    {
        if (s <= 0.0f)
        {
            out[0] = out[1] = out[2] = l;
            return;
        }
        float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        float p = 2.0f * l - q;
        out[0]  = hsl_channel (p, q, h + 1.0f / 3.0f);
        out[1]  = hsl_channel (p, q, h);
        out[2]  = hsl_channel (p, q, h - 1.0f / 3.0f);
    }

    /* Hue/Saturation's range edits as three hue-indexed tables (hue shift in degrees, saturation and
       lightness as fractions). Ranges add up where they overlap. */
    void hue_range_tables (const doc::HueRange ranges[6], std::vector<float> t[3])
    {
        for (int i = 0; i < 3; i++)
        {
            t[i].assign (LUT_SIZE, 0.0f);
        }
        for (size_t k = 0; k < LUT_SIZE; k++)
        {
            float deg = lut_x (k) * 360.0f;
            for (int r = 0; r < 6; r++)
            {
                if (ranges[r].is_neutral ())
                {
                    continue;
                }
                float w = ranges[r].weight (deg);
                t[0][k] += w * ranges[r].hue;
                t[1][k] += w * ranges[r].saturation / 100.0f;
                t[2][k] += w * ranges[r].lightness / 100.0f;
            }
        }
    }

    float lighten_by (float v, float l)
    {
        return l > 0.0f ? v + (1.0f - v) * l : v * (1.0f + l);
    }

    /* One Hue/Saturation evaluation: hue shift in degrees, saturation and lightness in -1..1.
       Photoshop applies Lightness first, then rotates the hue and scales the saturation. Colorize sets
       hue and saturation instead, building the colour at the lightened lightness. */
    void hue_saturation (const float c[3], float hue, float s, float l, bool colorize, float out[3])
    {
        if (colorize)
        {
            float hh, ss, ll;
            rgb_to_hsl (c, &hh, &ss, &ll);
            float h = fmodf (hue, 360.0f);
            if (h < 0.0f)
            {
                h += 360.0f;
            }
            hsl_to_rgb (h / 360.0f, fminf (fabsf (s), 1.0f), lighten_by (ll, l), out);
            return;
        }
        float lit[3] = {lighten_by (c[0], l), lighten_by (c[1], l), lighten_by (c[2], l)};
        float hh, ss, ll;
        rgb_to_hsl (lit, &hh, &ss, &ll);
        hh = rem1 (hh + hue / 360.0f);
        ss = s > 0.0f ? ss / fmaxf (1.0f - s, 1.0f / 512.0f) : ss * (1.0f + s);
        hsl_to_rgb (hh, clamp01 (ss), ll, out);
    }

    /* A range's Lightness: each channel moves towards the pixel's brightest channel when raised and
       its darkest when lowered (not towards white or black). */
    void range_lightness (float c[3], float l)
    {
        l            = clampf (l, -1.0f, 1.0f);
        float target = l > 0.0f ? fmaxf (fmaxf (c[0], c[1]), c[2]) : fminf (fminf (c[0], c[1]), c[2]);
        for (int i = 0; i < 3; i++)
        {
            c[i] += (target - c[i]) * fabsf (l);
        }
    }

    /* Black & White grey: the smallest channel, plus the secondary part (middle - smallest) and the
       primary part (largest - middle), each weighted by its slider (percent). */
    float black_white_gray (const float c[3], const float weights[6])
    {
        float r = c[0], g = c[1], b = c[2];
        float mx = fmaxf (fmaxf (r, g), b), mn = fminf (fminf (r, g), b);
        float mid     = r + g + b - mx - mn;
        int primary   = (r >= g && r >= b) ? 0 : (g >= b ? 2 : 4);
        int secondary = (b <= r && b <= g) ? 1 : (r <= g ? 3 : 5);
        return clamp01 (mn + (mid - mn) * weights[secondary] / 100.0f + (mx - mid) * weights[primary] / 100.0f);
    }

    /* Ordered-dither offset in -0.5..0.5 (4x4 Bayer matrix) for document pixel (x, y). */
    float bayer4 (int32_t x, int32_t y)
    {
        const float M[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        return (M[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.0f - 0.5f;
    }

    const float BALANCE_LEVELS = 0.39f;

    float balance_black (float v, float b)
    {
        return fmaxf ((v - b) / (1.0f - b), 0.0f);
    }

    float balance_white (float v, float w)
    {
        return fminf (v / w, 1.0f);
    }

    /* Color Balance on channel ch (0 red, 1 green, 2 blue). Every slider is a levels-and-gamma curve
       per channel, reverse-engineered from Photoshop 27.11 (within 1 level for a single slider).
       Tones apply highlights, shadows, then midtones. */
    float color_balance_channel (const float tones[3][3], bool preserve_luminosity, int ch, float v)
    {
        v                  = clamp01 (v);
        const int order[3] = {2, 0, 1};
        for (int o = 0; o < 3; o++)
        {
            int tone             = order[o];
            const float* sliders = tones[tone];
            float a[3];
            for (int i = 0; i < 3; i++)
            {
                a[i] = clampf (sliders[i] / 100.0f, -1.0f, 1.0f);
            }
            if (preserve_luminosity && tone != 1)
            {
                float own = a[ch];
                float lo = fminf (fminf (a[0], a[1]), a[2]), hi = fmaxf (fmaxf (a[0], a[1]), a[2]);
                v = tone == 0 ? balance_black (v, BALANCE_LEVELS * (hi - own))
                              : balance_white (v, 1.0f - BALANCE_LEVELS * (own - lo));
                continue;
            }
            for (int src = 0; src < 3; src++)
            {
                float s  = a[src];
                bool own = src == ch;
                if (s == 0.0f || (!preserve_luminosity && !own))
                {
                    continue;
                }
                if (tone == 1 && !preserve_luminosity)
                {
                    v = powf (v, powf (2.0f, -s));
                }
                else if (tone == 1)
                {
                    v = powf (v, powf (2.0f, (own ? -s : s) / 2.0f));
                }
                else if (tone == 0 && s < 0.0f)
                {
                    v = powf (balance_black (v, BALANCE_LEVELS * -s), powf (2.0f, -s / 2.0f));
                }
                else if (tone == 0)
                {
                    v = powf (v, powf (2.0f, -s / 2.0f));
                }
                else if (tone == 2 && s > 0.0f)
                {
                    v = powf (balance_white (v, 1.0f - BALANCE_LEVELS * s), powf (2.0f, -s / 2.0f));
                }
                else
                {
                    v = powf (v, powf (2.0f, -s / 2.0f));
                }
            }
        }
        return v;
    }

    /* How much a colour belongs to each Selective Color range: reds, yellows, greens, cyans, blues,
       magentas (chroma in the hue sector), whites, neutrals and blacks (lightness). */
    void selective_color_weights (const float c[3], float w[9])
    {
        float mx = fmaxf (fmaxf (c[0], c[1]), c[2]), mn = fminf (fminf (c[0], c[1]), c[2]);
        float mid = c[0] + c[1] + c[2] - mx - mn;
        float top[3], bottom[3];
        for (int i = 0; i < 3; i++)
        {
            top[i]    = c[i] >= mx ? mx - mid : 0.0f;
            bottom[i] = c[i] <= mn ? mid - mn : 0.0f;
        }
        w[0] = top[0];
        w[1] = bottom[2];
        w[2] = top[1];
        w[3] = bottom[0];
        w[4] = top[2];
        w[5] = bottom[1];
        w[6] = fmaxf ((mn - 0.5f) * 2.0f, 0.0f);
        w[7] = clamp01 (1.0f - fabsf (mx - 0.5f) - fabsf (mn - 0.5f));
        w[8] = fmaxf ((0.5f - mx) * 2.0f, 0.0f);
    }

    /* Selective Color: each range shifts the cyan, magenta and yellow ink (1 - R, G, B) by its
       percentage plus its black on every ink, weighted by membership. Relative scales by the ink
       present; absolute by the room the ink has (within 1 level of Photoshop 27.11). */
    void selective_color (const float c[3], bool relative, const float adj[9][4], float out[3])
    {
        float w[9];
        selective_color_weights (c, w);
        float delta[3] = {0.0f, 0.0f, 0.0f};
        for (int r = 0; r < 9; r++)
        {
            if (w[r] <= 0.0f)
            {
                continue;
            }
            for (int i = 0; i < 3; i++)
            {
                float ink = 1.0f - c[i];
                float d   = adj[r][i] / 100.0f + adj[r][3] / 100.0f;
                if (relative)
                {
                    d *= ink;
                }
                else if (d > 0.0f)
                {
                    d *= fminf (2.0f * (1.0f - ink), 1.0f);
                }
                else
                {
                    d *= fminf (2.0f * ink, 1.0f);
                }
                delta[i] += d * w[r];
            }
        }
        for (int i = 0; i < 3; i++)
        {
            out[i] = clamp01 (c[i] - delta[i]);
        }
    }

    /* Photo Filter's linear-light matrix: the pixel's D50 XYZ relative to white multiplied by the
       filter colour's, mixed in by density, folded into one matrix. */
    void photo_filter_matrix (const float color[3], float density, bool normalize_y, float out[3][3])
    {
        float d      = isfinite (density) ? clamp01 (density) : 0.0f;
        float lin[3] = {color::srgb_to_linear (clamp01 (color[0])), color::srgb_to_linear (clamp01 (color[1])),
            color::srgb_to_linear (clamp01 (color[2]))};
        float xyz[3];
        color::mat3_mul (color::SRGB_TO_XYZ_D50, lin, xyz);
        float s[3];
        for (int i = 0; i < 3; i++)
        {
            s[i] = 1.0f - d + d * xyz[i] / color::D50[i];
        }
        if (normalize_y && s[1] > 1e-6f)
        {
            float y = s[1];
            for (int i = 0; i < 3; i++)
            {
                s[i] /= y;
            }
        }
        for (int r = 0; r < 3; r++)
        {
            for (int c = 0; c < 3; c++)
            {
                float sum = 0.0f;
                for (int k = 0; k < 3; k++)
                {
                    sum += color::XYZ_D50_TO_SRGB[r][k] * s[k] * color::SRGB_TO_XYZ_D50[k][c];
                }
                out[r][c] = sum;
            }
        }
    }

    const float VIBRANCE_GREY[2]  = {0.288f, 0.712f};
    const float VIBRANCE_BOOST[7] = {0.5001f, 0.0887f, 0.4374f, 0.7573f, -0.8113f, 0.5154f, 2.8026f};

    float vibrance_skin (float h)
    {
        if (h <= 30.0f)
        {
            return 1.0f;
        }
        if (h < 45.0f)
        {
            return (45.0f - h) / 15.0f;
        }
        if (h < 300.0f)
        {
            return 0.0f;
        }
        return (h - 300.0f) / 60.0f;
    }

    float hsv_hue (const float c[3], float mx, float mn)
    {
        float d = mx - mn;
        float h;
        if (mx == c[0])
        {
            h = fmodf ((c[1] - c[2]) / d, 6.0f);
            if (h < 0.0f)
            {
                h += 6.0f;
            }
        }
        else if (mx == c[1])
        {
            h = (c[2] - c[0]) / d + 2.0f;
        }
        else
        {
            h = (c[0] - c[1]) / d + 4.0f;
        }
        return h * 60.0f;
    }

    /* The Vibrance slider (v in -1..1) on a linear-light pixel: an HSV change that keeps the hue,
       fitted against Photoshop 25.4 on 7,400-colour charts (see the reference for the measurements). */
    void vibrance_lin (float c[3], float v)
    {
        float mx = fmaxf (fmaxf (c[0], c[1]), c[2]), mn = fminf (fminf (c[0], c[1]), c[2]);
        if (mx <= 0.0f || mx - mn <= 1e-9f)
        {
            return;
        }
        float t     = 1.0f - mn / mx;
        float shape = t * (1.0f - t) * (2.0f - t + t * t) * mx * fmaxf (1.0f - mx, 0.0f);
        float t2, v2;
        if (v < 0.0f)
        {
            float a = -v;
            t2      = t * (1.0f - a / 4.0f) * (1.0f - a + a * t * (1.0f + t) / 2.0f);
            v2      = mx - a * shape;
        }
        else
        {
            const float* B = VIBRANCE_BOOST;
            float w        = vibrance_skin (hsv_hue (c, mx, mn));
            float a        = v * (1.0f - w) + 0.64f * powf (v, 1.6f) * w;
            float boost    = (B[0] * a + B[1] * a * a) * powf (t, B[2]) * powf (1.0f - t, B[3])
                        * expf ((B[4] + B[5] * a + B[6] * (1.0f - t)) * fmaxf (1.0f - mx, 0.0f));
            t2 = fminf (t * (1.0f + boost), 1.0f);
            v2 = mx + v / 4.0f * shape;
        }
        float m2 = v2 * (1.0f - t2);
        float k  = (v2 - m2) / (mx - mn);
        for (int i = 0; i < 3; i++)
        {
            c[i] = m2 + (c[i] - mn) * k;
        }
    }

    /* Vibrance, then Saturation, both in linear light through the document's tone curve. Saturation
       moves colours away from the grey 0.288 R + 0.712 G by 1 + s. */
    void vibrance_px (const float c[3], float v, float s, Transfer transfer, float hi, float out[3])
    {
        float l[3] = {transfer.decode (c[0]), transfer.decode (c[1]), transfer.decode (c[2])};
        if (v != 0.0f)
        {
            vibrance_lin (l, v);
        }
        if (s != 0.0f)
        {
            float g = VIBRANCE_GREY[0] * l[0] + VIBRANCE_GREY[1] * l[1];
            for (int i = 0; i < 3; i++)
            {
                l[i] = clampf (g + (1.0f + s) * (l[i] - g), 0.0f, hi);
            }
        }
        for (int i = 0; i < 3; i++)
        {
            out[i] = transfer.encode (clampf (l[i], 0.0f, hi));
        }
    }

    /* Modern Brightness: a line of slope 1.375^(b/50) from the origin, rolled off to (1, 1). */
    float modern_brightness (float v, float brightness)
    {
        if (brightness == 0.0f)
        {
            return v;
        }
        float s = powf (1.375f, brightness / 50.0f);
        float p = brightness >= 0.0f ? fmaxf (4.5f - 0.013f * brightness, 2.0f) : 5.0f - 0.072f * brightness;
        v       = clamp01 (v);
        return clamp01 (s * v + (1.0f - s) * powf (v, p));
    }

    float contrast_half (float t, float end, float mid)
    {
        return end * 0.5f * (t * t * t - 2.0f * t * t + t) + (-2.0f * t * t * t + 3.0f * t * t) * 0.5f
             + mid * 0.5f * (t * t * t - t * t);
    }

    /* Modern Contrast: a symmetric cubic-Hermite S-curve pivoting at 0.5 (within 0.5/255). */
    float modern_contrast (float v, float contrast)
    {
        if (contrast == 0.0f)
        {
            return v;
        }
        float k   = contrast / 128.0f;
        float end = 1.0f - k, mid = 1.0f + k;
        v = clamp01 (v);
        if (v <= 0.5f)
        {
            return contrast_half (v / 0.5f, end, mid);
        }
        return 1.0f - contrast_half ((1.0f - v) / 0.5f, end, mid);
    }

    /* Legacy Brightness/Contrast: contrast scales around 128/255, brightness added before it when
       contrast is raised and after it when lowered (measured on Photoshop 27.11). */
    float legacy_brightness_contrast (float brightness, float contrast, float v)
    {
        float b   = brightness / 255.0f;
        float c   = clampf (contrast, -100.0f, 99.0f);
        float k   = c >= 0.0f ? 1.0f / (1.0f - c / 100.0f) : 1.0f + c / 100.0f;
        float pre = c >= 0.0f ? b : 0.0f, post = c >= 0.0f ? 0.0f : b;
        return clamp01 ((v + pre - LEGACY_PIVOT) * k + LEGACY_PIVOT + post);
    }

    float brightness_contrast (const doc::BrightnessContrastParams& p, float v)
    {
        if (p.legacy)
        {
            return legacy_brightness_contrast (p.brightness, p.contrast, v);
        }
        return clamp01 (modern_contrast (modern_brightness (v, p.brightness), p.contrast));
    }

    void gradient_at (const std::vector<doc::GradientStop>& stops, float t, float out[3])
    {
        if (stops.empty ())
        {
            out[0] = out[1] = out[2] = t;
            return;
        }
        if (stops.size () == 1 || t <= stops[0].location)
        {
            memcpy (out, stops[0].color, sizeof (float) * 3);
            return;
        }
        for (size_t i = 0; i + 1 < stops.size (); i++)
        {
            const doc::GradientStop &a = stops[i], &b = stops[i + 1];
            if (t <= b.location)
            {
                float k = b.location > a.location ? (t - a.location) / (b.location - a.location) : 0.0f;
                for (int c = 0; c < 3; c++)
                {
                    out[c] = a.color[c] + (b.color[c] - a.color[c]) * k;
                }
                return;
            }
        }
        memcpy (out, stops.back ().color, sizeof (float) * 3);
    }

    void lab_norm (const float c[3], float out[3])
    {
        float l[3];
        color::srgb_to_lab (c, l);
        out[0] = l[0] / 100.0f;
        out[1] = (l[1] + 128.0f) / 255.0f;
        out[2] = (l[2] + 128.0f) / 255.0f;
    }

    void lab_denorm (const float n[3], float out[3])
    {
        float l[3] = {n[0] * 100.0f, n[1] * 255.0f - 128.0f, n[2] * 255.0f - 128.0f};
        color::lab_to_srgb (l, out);
    }

    /* The adjustments Photoshop evaluates on L, a and b in Lab documents. */
    bool lab_channel_adjustment (const Adjustment& adj)
    {
        switch (adj.kind)
        {
        case AdjustmentKind::Invert:
        case AdjustmentKind::Posterize:
        case AdjustmentKind::Threshold:
        case AdjustmentKind::BrightnessContrast:
        case AdjustmentKind::GradientMap:
            return true;
        default:
            return false;
        }
    }

    void apply_gradient_map (const doc::GradientMapParams& g, Buffer* buf, bool lab)
    {
        size_t w = buf->rect.width () > 0 ? buf->rect.width () : 1;
        for (size_t i = 0; i < buf->pixel_count (); i++)
        {
            float* p = &buf->px[i * 4];
            if (p[3] <= 0.0f)
            {
                continue;
            }
            float t;
            if (lab)
            {
                float n[3];
                lab_norm (p, n);
                t = clamp01 (n[0]);
            }
            else
            {
                t = color::rgb_to_gray (p);
            }
            if (g.reverse)
            {
                t = 1.0f - t;
            }
            float o[3];
            gradient_at (g.stops, t, o);
            if (g.dither)
            {
                float d = bayer4 (buf->rect.x0 + (int32_t) (i % w), buf->rect.y0 + (int32_t) (i / w)) / 255.0f;
                for (int c = 0; c < 3; c++)
                {
                    o[c] = clamp01 (o[c] + d);
                }
            }
            memcpy (p, o, sizeof o);
        }
    }

    void apply_lab (const Adjustment& adj, Buffer* buf)
    {
        if (adj.kind == AdjustmentKind::GradientMap)
        {
            apply_gradient_map (adj.gradient_map, buf, true);
            return;
        }
        float t = roundf (adj.threshold_level * 255.0f);
        for (size_t i = 0; i < buf->pixel_count (); i++)
        {
            float* p = &buf->px[i * 4];
            if (p[3] <= 0.0f)
            {
                continue;
            }
            float n[3];
            lab_norm (p, n);
            switch (adj.kind)
            {
            case AdjustmentKind::Invert:
                {
                    float o[3] = {1.0f - clamp01 (n[0]), 1.0f - clamp01 (n[1]), 1.0f - clamp01 (n[2])};
                    lab_denorm (o, p);
                    break;
                }
            case AdjustmentKind::Posterize:
                {
                    float o[3] = {posterize (n[0], adj.posterize_levels), posterize (n[1], adj.posterize_levels),
                        posterize (n[2], adj.posterize_levels)};
                    lab_denorm (o, p);
                    break;
                }
            case AdjustmentKind::Threshold:
                p[0] = p[1] = p[2] = roundf (clamp01 (n[0]) * 255.0f) >= t ? 1.0f : 0.0f;
                break;
            case AdjustmentKind::BrightnessContrast:
                {
                    float l = brightness_contrast (adj.brightness_contrast, n[0]);
                    if (l == n[0])
                    {
                        break;
                    }
                    float moved[3] = {l, n[1], n[2]};
                    float before[3], after[3];
                    lab_denorm (n, before);
                    lab_denorm (moved, after);
                    for (int c = 0; c < 3; c++)
                    {
                        p[c] = clamp01 (p[c] + after[c] - before[c]);
                    }
                    break;
                }
            default:
                break;
            }
        }
    }

    /* Applies `adj` to every visible pixel of `buf` (straight RGBA). Integer depths work on whole
       levels like Photoshop; 32-bit documents get its float Levels and Exposure. */
    void apply_adjustment (const Adjustment& adj, Buffer* buf, const AdjustContext& cx)
    {
        if (cx.mode == color::ColorMode::Lab && lab_channel_adjustment (adj))
        {
            apply_lab (adj, buf);
            return;
        }
        bool f32         = cx.has_depth && cx.depth == color::SampleType::F32;
        float quantum    = 0.0f;
        bool has_quantum = cx.has_depth && adjustment_quantum (cx.depth, &quantum);
        size_t n         = buf->pixel_count ();

        switch (adj.kind)
        {
        case AdjustmentKind::GradientMap:
            apply_gradient_map (adj.gradient_map, buf, false);
            return;
        case AdjustmentKind::ColorLookup:
        case AdjustmentKind::Unsupported:
            return;
        default:
            break;
        }

        std::vector<float> luts[4];
        bool levels_float_path = adj.kind == AdjustmentKind::Levels && adj.levels.space == ToneSpace::Rgb && f32;
        bool curve_identity[3] = {false, false, false};
        if ((adj.kind == AdjustmentKind::Levels && !levels_float_path) || adj.kind == AdjustmentKind::Curves)
        {
            tone_luts (adj, has_quantum, quantum, luts);
            if (adj.kind == AdjustmentKind::Curves && adj.curves.space == ToneSpace::Rgb && f32)
            {
                for (int i = 0; i < 3; i++)
                {
                    curve_identity[i] =
                        doc::is_identity_curve (adj.curves.master) && doc::is_identity_curve (adj.curves.channels[i]);
                }
            }
        }
        std::vector<float> hue_tables[3];
        bool has_ranges = false;
        if (adj.kind == AdjustmentKind::HueSaturation && !adj.hue_saturation.colorize)
        {
            for (int r = 0; r < 6; r++)
            {
                if (!adj.hue_saturation.ranges[r].is_neutral ())
                {
                    has_ranges = true;
                }
            }
            if (has_ranges)
            {
                hue_range_tables (adj.hue_saturation.ranges, hue_tables);
            }
        }
        float filter[3][3];
        if (adj.kind == AdjustmentKind::PhotoFilter)
        {
            photo_filter_matrix (
                adj.photo_filter.color, adj.photo_filter.density, adj.photo_filter.preserve_luminosity && f32, filter);
        }
        Transfer exposure_transfer = cx.transfer.for_exposure ();
        float threshold            = roundf (adj.threshold_level * 255.0f);

        for (size_t i = 0; i < n; i++)
        {
            float* p = &buf->px[i * 4];
            if (p[3] <= 0.0f)
            {
                continue;
            }
            float c[3] = {p[0], p[1], p[2]};
            float o[3] = {c[0], c[1], c[2]};
            switch (adj.kind)
            {
            case AdjustmentKind::Invert:
                for (int k = 0; k < 3; k++)
                {
                    o[k] = 1.0f - c[k];
                }
                break;
            case AdjustmentKind::Threshold:
                {
                    float v = roundf (color::rgb_to_gray (c) * 255.0f) >= threshold ? 1.0f : 0.0f;
                    o[0] = o[1] = o[2] = v;
                    break;
                }
            case AdjustmentKind::Posterize:
                for (int k = 0; k < 3; k++)
                {
                    o[k] = posterize (c[k], adj.posterize_levels);
                }
                break;
            case AdjustmentKind::BrightnessContrast:
                for (int k = 0; k < 3; k++)
                {
                    o[k] = brightness_contrast (adj.brightness_contrast, c[k]);
                }
                break;
            case AdjustmentKind::Exposure:
                {
                    float m = powf (2.0f, adj.exposure.exposure);
                    float g = fmaxf (adj.exposure.gamma, 0.01f);
                    for (int k = 0; k < 3; k++)
                    {
                        if (f32)
                        {
                            float lin = c[k] * m + adj.exposure.offset;
                            o[k]      = g == 1.0f ? lin : signf (lin) * powf (fabsf (lin), 1.0f / g);
                            continue;
                        }
                        float lin = fmaxf (exposure_transfer.decode (c[k]) * m + adj.exposure.offset, 0.0f);
                        if (g != 1.0f)
                        {
                            lin = powf (lin, 1.0f / g);
                        }
                        o[k] = clamp01 (exposure_transfer.encode (lin));
                    }
                    break;
                }
            case AdjustmentKind::Levels:
                if (levels_float_path)
                {
                    const doc::LevelsParams& L = adj.levels;
                    bool master_identity       = L.master.is_identity ();
                    for (int k = 0; k < 3; k++)
                    {
                        float v = L.channels[k].is_identity () ? c[k] : levels_float (L.channels[k], c[k]);
                        o[k]    = master_identity ? v : levels_float (L.master, v);
                    }
                }
                else if (adj.levels.space == ToneSpace::Rgb)
                {
                    for (int k = 0; k < 3; k++)
                    {
                        o[k] = lut_sample (luts[k], c[k]);
                    }
                }
                else
                {
                    tone_in_space (adj.levels.space, luts, c, cx.cmyk, o);
                }
                break;
            case AdjustmentKind::Curves:
                if (adj.curves.space == ToneSpace::Rgb)
                {
                    for (int k = 0; k < 3; k++)
                    {
                        o[k] = curve_identity[k] ? c[k] : lut_sample (luts[k], c[k]);
                    }
                }
                else
                {
                    tone_in_space (adj.curves.space, luts, c, cx.cmyk, o);
                }
                break;
            case AdjustmentKind::HueSaturation:
                {
                    const doc::HueSaturationParams& H = adj.hue_saturation;
                    float dh = 0.0f, ds = 0.0f, dl = 0.0f;
                    if (has_ranges)
                    {
                        float h0, s0, l0;
                        rgb_to_hsl (c, &h0, &s0, &l0);
                        float chroma =
                            fminf ((fmaxf (fmaxf (c[0], c[1]), c[2]) - fminf (fminf (c[0], c[1]), c[2])) * 4.0f, 1.0f);
                        dh = lut_sample (hue_tables[0], h0) * chroma;
                        ds = lut_sample (hue_tables[1], h0) * chroma;
                        dl = lut_sample (hue_tables[2], h0) * chroma;
                    }
                    hue_saturation (c, H.hue + dh, clampf (H.saturation / 100.0f + ds, -1.0f, 1.0f),
                        clampf (H.lightness / 100.0f, -1.0f, 1.0f), H.colorize, o);
                    range_lightness (o, dl);
                    break;
                }
            case AdjustmentKind::Vibrance:
                vibrance_px (c, adj.vibrance.vibrance / 100.0f, adj.vibrance.saturation / 100.0f, cx.transfer,
                    f32 ? INFINITY : 1.0f, o);
                break;
            case AdjustmentKind::ChannelMixer:
                {
                    const doc::ChannelMixerParams& M = adj.channel_mixer;
                    for (int r = 0; r < 3; r++)
                    {
                        const float* row = M.monochrome ? M.matrix[0] : M.matrix[r];
                        o[r]             = clamp01 (row[0] * c[0] + row[1] * c[1] + row[2] * c[2] + row[3]);
                    }
                    break;
                }
            case AdjustmentKind::PhotoFilter:
                {
                    float lin[3] = {cx.transfer.decode (c[0]), cx.transfer.decode (c[1]), cx.transfer.decode (c[2])};
                    float f[3];
                    color::mat3_mul (filter, lin, f);
                    for (int k = 0; k < 3; k++)
                    {
                        f[k] = cx.transfer.encode (f[k]);
                    }
                    if (adj.photo_filter.preserve_luminosity && !f32)
                    {
                        float kept[3];
                        color::set_lum (f, color::lum (c), kept);
                        memcpy (f, kept, sizeof f);
                    }
                    for (int k = 0; k < 3; k++)
                    {
                        o[k] = clamp01 (f[k]);
                    }
                    break;
                }
            case AdjustmentKind::BlackWhite:
                {
                    float g = black_white_gray (c, adj.black_white.weights);
                    if (adj.black_white.has_tint)
                    {
                        color::set_lum (adj.black_white.tint, g, o);
                        for (int k = 0; k < 3; k++)
                        {
                            o[k] = clamp01 (o[k]);
                        }
                    }
                    else
                    {
                        o[0] = o[1] = o[2] = g;
                    }
                    break;
                }
            case AdjustmentKind::ColorBalance:
                {
                    const doc::ColorBalanceParams& B = adj.color_balance;
                    float tones[3][3];
                    memcpy (tones[0], B.shadows, sizeof B.shadows);
                    memcpy (tones[1], B.midtones, sizeof B.midtones);
                    memcpy (tones[2], B.highlights, sizeof B.highlights);
                    for (int k = 0; k < 3; k++)
                    {
                        o[k] = color_balance_channel (tones, B.preserve_luminosity, k, c[k]);
                    }
                    break;
                }
            case AdjustmentKind::SelectiveColor:
                selective_color (c, adj.selective_color.relative, adj.selective_color.adjustments, o);
                break;
            default:
                break;
            }
            p[0] = o[0];
            p[1] = o[1];
            p[2] = o[2];
        }
    }

} // namespace compose
