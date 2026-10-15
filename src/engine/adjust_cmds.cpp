#include "compose/adjust.h"
#include "engine/adjust.h"
#include "engine/layers.h"
#include "engine/session.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace engine
{

    /* Image > Adjustments (destructive, through the selection) and Layer > New Adjustment Layer.
       Parameters follow the reference's units: Levels and Curves in 0-255 levels, percentages for
       the rest, colours as "#rrggbb" or [r, g, b] in 0..1. Out-of-range values are clamped like
       Photoshop's fields; a key of the wrong type is an error. */

    using doc::Adjustment;
    using doc::AdjustmentKind;
    using doc::LevelsChannel;
    using doc::ToneSpace;
    using geom::Rect;

    const char* const HUE_RANGES[6]       = {"reds", "yellows", "greens", "cyans", "blues", "magentas"};
    const char* const SELECTIVE_RANGES[9] = {
        "reds", "yellows", "greens", "cyans", "blues", "magentas", "whites", "neutrals", "blacks"};

    const PhotoFilterPreset PHOTO_FILTERS[] = {
        {"warming85", "Warming Filter (85)", {236, 138, 0}},
        {"warmingLBA", "Warming Filter (LBA)", {250, 150, 0}},
        {"warming81", "Warming Filter (81)", {235, 177, 19}},
        {"cooling80", "Cooling Filter (80)", {0, 109, 255}},
        {"coolingLBB", "Cooling Filter (LBB)", {0, 93, 255}},
        {"cooling82", "Cooling Filter (82)", {0, 181, 255}},
        {"red", "Red", {234, 26, 26}},
        {"orange", "Orange", {243, 132, 23}},
        {"yellow", "Yellow", {249, 227, 28}},
        {"green", "Green", {25, 201, 25}},
        {"cyan", "Cyan", {29, 203, 234}},
        {"blue", "Blue", {29, 53, 234}},
        {"violet", "Violet", {155, 29, 234}},
        {"magenta", "Magenta", {227, 24, 227}},
        {"sepia", "Sepia", {172, 122, 51}},
        {"deepRed", "Deep Red", {255, 0, 0}},
        {"deepBlue", "Deep Blue", {0, 34, 205}},
        {"deepEmerald", "Deep Emerald", {0, 140, 0}},
        {"deepYellow", "Deep Yellow", {255, 213, 0}},
        {"underwater", "Underwater", {0, 194, 177}},
    };
    const size_t PHOTO_FILTER_COUNT = sizeof PHOTO_FILTERS / sizeof PHOTO_FILTERS[0];

    float clampf (float v, float lo, float hi)
    {
        return v < lo ? lo : v > hi ? hi : v;
    }

    /* A number param clamped to lo..hi; `def` when absent. */
    bool adj_num (
        const Value& p, const char* cmd, const char* key, float def, float lo, float hi, float* out, Error* err)
    {
        const Value* v = param (p, key);
        if (v == NULL)
        {
            *out = clampf (def, lo, hi);
            return true;
        }
        if (!v->is_number () || !isfinite (v->get<double> ()))
        {
            return bad_params (err, cmd, "`%s` must be a number", key);
        }
        *out = clampf ((float) v->get<double> (), lo, hi);
        return true;
    }

    bool adj_bool (const Value& p, const char* cmd, const char* key, bool def, bool* out, Error* err)
    {
        const Value* v = param (p, key);
        if (v == NULL)
        {
            *out = def;
            return true;
        }
        if (!v->is_boolean ())
        {
            return bad_params (err, cmd, "`%s` must be true or false", key);
        }
        *out = v->get<bool> ();
        return true;
    }

    /* An array of exactly n numbers; false with *present unset when the key is absent. */
    bool adj_numbers (const Value& p, const char* cmd, const char* key, size_t n, float* out, bool* present, Error* err)
    {
        *present       = false;
        const Value* v = param (p, key);
        if (v == NULL)
        {
            return true;
        }
        if (!v->is_array () || v->size () != n)
        {
            return bad_params (err, cmd, "`%s` must be an array of %zu numbers", key, n);
        }
        for (size_t i = 0; i < n; i++)
        {
            const Value& x = (*v)[i];
            if (!x.is_number () || !isfinite (x.get<double> ()))
            {
                return bad_params (err, cmd, "`%s` must be an array of %zu numbers", key, n);
            }
            out[i] = (float) x.get<double> ();
        }
        *present = true;
        return true;
    }

    bool color_value (const Value& v, float out[3])
    {
        if (v.is_string ())
        {
            float c[4];
            if (!parse_hex (v.get_ref<const std::string&> ().c_str (), c))
            {
                return false;
            }
            memcpy (out, c, sizeof (float) * 3);
            return true;
        }
        if (v.is_array () && v.size () >= 3)
        {
            for (size_t i = 0; i < 3; i++)
            {
                if (!v[i].is_number () || !isfinite (v[i].get<double> ()))
                {
                    return false;
                }
                out[i] = clampf ((float) v[i].get<double> (), 0.0f, 1.0f);
            }
            return true;
        }
        return false;
    }

    bool adj_color (const Value& p, const char* cmd, const char* key, float out[3], Error* err)
    {
        const Value* v = param (p, key);
        if (v == NULL)
        {
            return true;
        }
        if (!color_value (*v, out))
        {
            return bad_params (err, cmd, "`%s` must be \"#rrggbb\" or [r, g, b] in 0..1", key);
        }
        return true;
    }

    /* A channel object ({"inBlack": ...}) or NULL; an error when the key holds something else. */
    bool adj_object (const Value& p, const char* cmd, const char* key, const Value** out, Error* err)
    {
        *out = param (p, key);
        if (*out != NULL && !(*out)->is_object ())
        {
            return bad_params (err, cmd, "`%s` must be an object", key);
        }
        return true;
    }

    ToneSpace default_space (color::ColorMode mode)
    {
        if (mode == color::ColorMode::Cmyk)
        {
            return ToneSpace::Cmyk;
        }
        if (mode == color::ColorMode::Lab)
        {
            return ToneSpace::Lab;
        }
        return ToneSpace::Rgb;
    }

    /* The tone space the params address: any channel key of a space picks it; otherwise the base's
       space, or the document mode's. */
    ToneSpace space_of (const Value& p, bool has_base, ToneSpace base, color::ColorMode mode)
    {
        bool cmyk = param_present (p, "cyan") || param_present (p, "magenta") || param_present (p, "yellow")
                 || param_present (p, "black");
        bool rgb = param_present (p, "red") || param_present (p, "green") || param_present (p, "blue");
        if (cmyk)
        {
            return ToneSpace::Cmyk;
        }
        if (param_present (p, "a") || param_present (p, "b") || (param_present (p, "lightness") && !rgb))
        {
            return ToneSpace::Lab;
        }
        if (rgb)
        {
            return ToneSpace::Rgb;
        }
        return has_base ? base : default_space (mode);
    }

    void channel_keys (ToneSpace space, const char* keys[4], size_t* n)
    {
        if (space == ToneSpace::Cmyk)
        {
            keys[0] = "cyan";
            keys[1] = "magenta";
            keys[2] = "yellow";
            keys[3] = "black";
            *n      = 4;
        }
        else if (space == ToneSpace::Lab)
        {
            keys[0] = "lightness";
            keys[1] = "a";
            keys[2] = "b";
            *n      = 3;
        }
        else
        {
            keys[0] = "red";
            keys[1] = "green";
            keys[2] = "blue";
            *n      = 3;
        }
    }

    bool read_levels_channel (const Value& o, const char* cmd, const LevelsChannel& c, LevelsChannel* out, Error* err)
    {
        float in_black, in_white, gamma, out_black, out_white;
        if (!adj_num (o, cmd, "inBlack", c.in_black * 255.0f, 0.0f, 253.0f, &in_black, err))
        {
            return false;
        }
        if (!adj_num (o, cmd, "inWhite", c.in_white * 255.0f, 2.0f, 255.0f, &in_white, err))
        {
            return false;
        }
        if (in_white < in_black + 2.0f)
        {
            in_white = in_black + 2.0f;
        }
        if (!adj_num (o, cmd, "gamma", c.gamma, 0.01f, 9.99f, &gamma, err))
        {
            return false;
        }
        if (!adj_num (o, cmd, "outBlack", c.out_black * 255.0f, 0.0f, 255.0f, &out_black, err))
        {
            return false;
        }
        if (!adj_num (o, cmd, "outWhite", c.out_white * 255.0f, 0.0f, 255.0f, &out_white, err))
        {
            return false;
        }
        LevelsChannel r = {in_black / 255.0f, in_white / 255.0f, gamma, out_black / 255.0f, out_white / 255.0f};
        *out            = r;
        return true;
    }

    bool levels_from_params (
        const Value& p, const char* cmd, color::ColorMode mode, bool has_base, Adjustment* a, Error* err)
    {
        doc::LevelsParams& L = a->levels;
        ToneSpace space      = space_of (p, has_base, L.space, mode);
        if (has_base && space != L.space)
        {
            for (int i = 0; i < 3; i++)
            {
                L.channels[i] = LevelsChannel::identity ();
            }
            L.black = LevelsChannel::identity ();
        }
        L.space                = space;
        LevelsChannel chans[4] = {L.channels[0], L.channels[1], L.channels[2], L.black};
        if (L.space == ToneSpace::Lab)
        {
            if (!read_levels_channel (p, cmd, chans[0], &chans[0], err))
            {
                return false;
            }
        }
        else
        {
            if (!read_levels_channel (p, cmd, L.master, &L.master, err))
            {
                return false;
            }
            const Value* gray;
            if (!adj_object (p, cmd, "gray", &gray, err))
            {
                return false;
            }
            if (gray != NULL && !read_levels_channel (*gray, cmd, L.master, &L.master, err))
            {
                return false;
            }
        }
        const char* keys[4];
        size_t n;
        channel_keys (L.space, keys, &n);
        for (size_t i = 0; i < n; i++)
        {
            const Value* o;
            if (!adj_object (p, cmd, keys[i], &o, err))
            {
                return false;
            }
            if (o != NULL && !read_levels_channel (*o, cmd, chans[i], &chans[i], err))
            {
                return false;
            }
        }
        for (int i = 0; i < 3; i++)
        {
            L.channels[i] = chans[i];
        }
        L.black = L.space == ToneSpace::Cmyk ? chans[3] : LevelsChannel::identity ();
        return true;
    }

    /* [[input, output], ...] in 0..255, 2..64 points; inputs closer than half a level merge. */
    bool parse_curve (const Value& v, const char* cmd, const char* key, std::vector<doc::CurvePoint>* out, Error* err)
    {
        if (v.is_array () && v.size () > 0 && v[0].is_number ())
        {
            return fail (err, ErrorKind::Other, "pencil-drawn Curves maps are not ported yet");
        }
        if (!v.is_array () || v.size () < 2 || v.size () > 64)
        {
            return bad_params (err, cmd, "`%s` must be an array of 2..=64 [input, output] pairs in 0..255", key);
        }
        std::vector<doc::CurvePoint> pts;
        for (size_t i = 0; i < v.size (); i++)
        {
            const Value& q = v[i];
            if (!q.is_array () || q.size () != 2 || !q[0].is_number () || !q[1].is_number ()
                || !isfinite (q[0].get<double> ()) || !isfinite (q[1].get<double> ()))
            {
                return bad_params (err, cmd, "`%s` must be an array of 2..=64 [input, output] pairs in 0..255", key);
            }
            doc::CurvePoint cp = {clampf ((float) q[0].get<double> (), 0.0f, 255.0f) / 255.0f,
                clampf ((float) q[1].get<double> (), 0.0f, 255.0f) / 255.0f};
            pts.push_back (cp);
        }
        for (size_t i = 1; i < pts.size (); i++)
        {
            for (size_t j = i; j > 0 && pts[j].input < pts[j - 1].input; j--)
            {
                doc::CurvePoint t = pts[j];
                pts[j]            = pts[j - 1];
                pts[j - 1]        = t;
            }
        }
        out->clear ();
        for (size_t i = 0; i < pts.size (); i++)
        {
            if (!out->empty () && fabsf (pts[i].input - out->back ().input) < 0.5f / 255.0f)
            {
                continue;
            }
            out->push_back (pts[i]);
        }
        if (out->size () < 2)
        {
            return bad_params (err, cmd, "`%s` must have at least 2 distinct points", key);
        }
        return true;
    }

    bool curves_from_params (
        const Value& p, const char* cmd, color::ColorMode mode, bool has_base, Adjustment* a, Error* err)
    {
        doc::CurvesParams& C = a->curves;
        ToneSpace space      = space_of (p, has_base, C.space, mode);
        if (has_base && space != C.space)
        {
            for (int i = 0; i < 3; i++)
            {
                C.channels[i] = doc::identity_curve ();
            }
            C.black.clear ();
        }
        C.space     = space;
        bool pencil = false;
        if (!adj_bool (p, cmd, "pencil", false, &pencil, err))
        {
            return false;
        }
        if (pencil)
        {
            return fail (err, ErrorKind::Other, "pencil-drawn Curves are not ported yet");
        }
        const Value* composite = param (p, "points");
        if (composite == NULL)
        {
            composite = param (p, "gray");
        }
        std::vector<doc::CurvePoint> chans[4] = {
            C.channels[0], C.channels[1], C.channels[2], C.black.empty () ? doc::identity_curve () : C.black};
        if (composite != NULL)
        {
            std::vector<doc::CurvePoint> c;
            if (!parse_curve (*composite, cmd, "points", &c, err))
            {
                return false;
            }
            if (C.space == ToneSpace::Lab)
            {
                chans[0] = c;
            }
            else
            {
                C.master = c;
            }
        }
        const char* keys[4];
        size_t n;
        channel_keys (C.space, keys, &n);
        for (size_t i = 0; i < n; i++)
        {
            const Value* v = param (p, keys[i]);
            if (v != NULL && !parse_curve (*v, cmd, keys[i], &chans[i], err))
            {
                return false;
            }
        }
        for (int i = 0; i < 3; i++)
        {
            C.channels[i] = chans[i];
        }
        C.black.clear ();
        if (C.space == ToneSpace::Cmyk && !doc::is_identity_curve (chans[3]))
        {
            C.black = chans[3];
        }
        return true;
    }

    bool hue_saturation_from_params (const Value& p, const char* cmd, Adjustment* a, Error* err)
    {
        doc::HueSaturationParams& H = a->hue_saturation;
        bool was_colorize           = H.colorize;
        if (!adj_bool (p, cmd, "colorize", was_colorize, &H.colorize, err))
        {
            return false;
        }
        if (H.colorize)
        {
            float hue0 = fmodf (H.hue, 360.0f);
            if (hue0 < 0.0f)
            {
                hue0 += 360.0f;
            }
            if (!adj_num (p, cmd, "hue", hue0, 0.0f, 360.0f, &H.hue, err))
            {
                return false;
            }
            /* Turning Colorize on starts at 25% saturation unless one is given; an explicit 0 stays
               0, which colorizes to grey. */
            float s0 = was_colorize ? H.saturation : fmaxf (H.saturation, 25.0f);
            if (!adj_num (p, cmd, "saturation", s0, 0.0f, 100.0f, &H.saturation, err))
            {
                return false;
            }
        }
        else
        {
            if (!adj_num (p, cmd, "hue", H.hue, -180.0f, 180.0f, &H.hue, err))
            {
                return false;
            }
            if (!adj_num (p, cmd, "saturation", H.saturation, -100.0f, 100.0f, &H.saturation, err))
            {
                return false;
            }
        }
        if (!adj_num (p, cmd, "lightness", H.lightness, -100.0f, 100.0f, &H.lightness, err))
        {
            return false;
        }
        for (int i = 0; i < 6; i++)
        {
            const Value* o;
            if (!adj_object (p, cmd, HUE_RANGES[i], &o, err))
            {
                return false;
            }
            if (o == NULL)
            {
                continue;
            }
            doc::HueRange& r = H.ranges[i];
            if (!adj_num (*o, cmd, "hue", r.hue, -180.0f, 180.0f, &r.hue, err))
            {
                return false;
            }
            if (!adj_num (*o, cmd, "saturation", r.saturation, -100.0f, 100.0f, &r.saturation, err))
            {
                return false;
            }
            if (!adj_num (*o, cmd, "lightness", r.lightness, -100.0f, 100.0f, &r.lightness, err))
            {
                return false;
            }
            float b[4];
            bool present;
            if (!adj_numbers (*o, cmd, "range", 4, b, &present, err))
            {
                return false;
            }
            if (present)
            {
                doc::canonical_hue_bounds (b, r.bounds);
            }
        }
        return true;
    }

    bool gradient_stops (const Value& p, const char* cmd, std::vector<doc::GradientStop>* out, Error* err)
    {
        const Value* v = param (p, "stops");
        if (v == NULL)
        {
            return true;
        }
        if (!v->is_array () || v->size () < 2 || v->size () > 64)
        {
            return bad_params (err, cmd, "`stops` must be an array of 2..=64 [location 0..1, colour] pairs");
        }
        std::vector<doc::GradientStop> stops;
        for (size_t i = 0; i < v->size (); i++)
        {
            const Value& s = (*v)[i];
            const Value *t = NULL, *c = NULL;
            if (s.is_array () && s.size () == 2)
            {
                t = &s[0];
                c = &s[1];
            }
            else if (s.is_object ())
            {
                t = param (s, "location");
                c = param (s, "color");
            }
            doc::GradientStop g;
            if (t == NULL || c == NULL || !t->is_number () || !isfinite (t->get<double> ())
                || !color_value (*c, g.color))
            {
                return bad_params (err, cmd, "`stops` must be an array of 2..=64 [location 0..1, colour] pairs");
            }
            g.location = clampf ((float) t->get<double> (), 0.0f, 1.0f);
            size_t k   = stops.size ();
            stops.push_back (g);
            while (k > 0 && stops[k].location < stops[k - 1].location)
            {
                doc::GradientStop tmp = stops[k];
                stops[k]              = stops[k - 1];
                stops[k - 1]          = tmp;
                k--;
            }
        }
        *out = stops;
        return true;
    }

    bool selective_from_params (const Value& p, const char* cmd, Adjustment* a, Error* err)
    {
        doc::SelectiveColorParams& S = a->selective_color;
        std::string method;
        if (param_present (p, "method"))
        {
            if (!param_str (p, "method", &method) || (method != "relative" && method != "absolute"))
            {
                return bad_params (err, cmd, "`method` must be \"relative\" or \"absolute\"");
            }
            S.relative = method == "relative";
        }
        if (!adj_bool (p, cmd, "relative", S.relative, &S.relative, err))
        {
            return false;
        }
        for (int r = 0; r < 9; r++)
        {
            float v[4];
            bool present;
            if (!adj_numbers (p, cmd, SELECTIVE_RANGES[r], 4, v, &present, err))
            {
                return false;
            }
            if (present)
            {
                for (int k = 0; k < 4; k++)
                {
                    S.adjustments[r][k] = clampf (v[k], -100.0f, 100.0f);
                }
            }
        }
        std::string colors;
        if (param_present (p, "colors"))
        {
            if (!param_str (p, "colors", &colors))
            {
                return bad_params (err, cmd, "`colors` must be a range name");
            }
            int row = -1;
            for (int r = 0; r < 9; r++)
            {
                if (colors == SELECTIVE_RANGES[r])
                {
                    row = r;
                }
            }
            if (row < 0)
            {
                return bad_params (err, cmd, "unknown range `%s`", colors.c_str ());
            }
            const char* inks[4] = {"cyan", "magenta", "yellow", "black"};
            for (int k = 0; k < 4; k++)
            {
                if (!adj_num (p, cmd, inks[k], S.adjustments[row][k], -100.0f, 100.0f, &S.adjustments[row][k], err))
                {
                    return false;
                }
            }
        }
        return true;
    }

    /* The adjustment of `kind` read from `p` over `base` (or the kind's defaults when `base` is NULL
       or of another kind): missing keys keep the base's values, so a partial update only changes
       what it names. */
    bool adjustment_from_params (
        AdjustmentKind kind, const Value& p, color::ColorMode mode, const Adjustment* base, Adjustment* out, Error* err)
    {
        char cmd[64];
        snprintf (cmd, sizeof cmd, "image.adjustments.%s", doc::adjustment_kind_id (kind));
        if (!p.is_object () && !p.is_null ())
        {
            return bad_params (err, cmd, "params must be an object");
        }
        bool has_base = base != NULL && base->kind == kind;
        Adjustment a  = has_base ? *base : Adjustment::make (kind);
        bool present;
        switch (kind)
        {
        case AdjustmentKind::Invert:
            break;
        case AdjustmentKind::BrightnessContrast:
            {
                doc::BrightnessContrastParams& B = a.brightness_contrast;
                if (!adj_bool (p, cmd, "legacy", B.legacy, &B.legacy, err))
                {
                    return false;
                }
                if (!adj_num (p, cmd, "brightness", B.brightness, -150.0f, 150.0f, &B.brightness, err))
                {
                    return false;
                }
                if (!adj_num (p, cmd, "contrast", B.contrast, B.legacy ? -100.0f : -50.0f, 100.0f, &B.contrast, err))
                {
                    return false;
                }
                break;
            }
        case AdjustmentKind::Threshold:
            {
                float level;
                if (!adj_num (p, cmd, "level", a.threshold_level * 255.0f, 1.0f, 255.0f, &level, err))
                {
                    return false;
                }
                a.threshold_level = roundf (level) / 255.0f;
                break;
            }
        case AdjustmentKind::Posterize:
            {
                float levels;
                if (!adj_num (p, cmd, "levels", (float) a.posterize_levels, 2.0f, 255.0f, &levels, err))
                {
                    return false;
                }
                a.posterize_levels = (uint32_t) roundf (levels);
                break;
            }
        case AdjustmentKind::Exposure:
            if (!adj_num (p, cmd, "exposure", a.exposure.exposure, -20.0f, 20.0f, &a.exposure.exposure, err))
            {
                return false;
            }
            if (!adj_num (p, cmd, "offset", a.exposure.offset, -0.5f, 0.5f, &a.exposure.offset, err))
            {
                return false;
            }
            if (!adj_num (p, cmd, "gamma", a.exposure.gamma, 0.01f, 9.99f, &a.exposure.gamma, err))
            {
                return false;
            }
            break;
        case AdjustmentKind::Vibrance:
            if (!adj_num (p, cmd, "vibrance", a.vibrance.vibrance, -100.0f, 100.0f, &a.vibrance.vibrance, err))
            {
                return false;
            }
            if (!adj_num (p, cmd, "saturation", a.vibrance.saturation, -100.0f, 100.0f, &a.vibrance.saturation, err))
            {
                return false;
            }
            break;
        case AdjustmentKind::HueSaturation:
            if (!hue_saturation_from_params (p, cmd, &a, err))
            {
                return false;
            }
            break;
        case AdjustmentKind::Levels:
            if (!levels_from_params (p, cmd, mode, has_base, &a, err))
            {
                return false;
            }
            break;
        case AdjustmentKind::Curves:
            if (!curves_from_params (p, cmd, mode, has_base, &a, err))
            {
                return false;
            }
            break;
        case AdjustmentKind::ColorBalance:
            {
                doc::ColorBalanceParams& B = a.color_balance;
                float* tones[3]            = {B.shadows, B.midtones, B.highlights};
                const char* keys[3]        = {"shadows", "midtones", "highlights"};
                for (int i = 0; i < 3; i++)
                {
                    float v[3];
                    if (!adj_numbers (p, cmd, keys[i], 3, v, &present, err))
                    {
                        return false;
                    }
                    if (present)
                    {
                        for (int k = 0; k < 3; k++)
                        {
                            tones[i][k] = clampf (v[k], -100.0f, 100.0f);
                        }
                    }
                }
                if (!adj_bool (p, cmd, "preserveLuminosity", B.preserve_luminosity, &B.preserve_luminosity, err))
                {
                    return false;
                }
                break;
            }
        case AdjustmentKind::BlackWhite:
            {
                doc::BlackWhiteParams& W = a.black_white;
                for (int i = 0; i < 6; i++)
                {
                    if (!adj_num (p, cmd, HUE_RANGES[i], W.weights[i], -200.0f, 300.0f, &W.weights[i], err))
                    {
                        return false;
                    }
                }
                if (!adj_color (p, cmd, "tintColor", W.tint, err))
                {
                    return false;
                }
                if (!adj_bool (p, cmd, "tint", W.has_tint, &W.has_tint, err))
                {
                    return false;
                }
                break;
            }
        case AdjustmentKind::PhotoFilter:
            {
                doc::PhotoFilterParams& F = a.photo_filter;
                if (param_present (p, "filter"))
                {
                    std::string id;
                    if (!param_str (p, "filter", &id))
                    {
                        return bad_params (err, cmd, "`filter` must be a preset id");
                    }
                    bool found = false;
                    for (size_t i = 0; i < PHOTO_FILTER_COUNT; i++)
                    {
                        if (id != PHOTO_FILTERS[i].id)
                        {
                            continue;
                        }
                        for (int k = 0; k < 3; k++)
                        {
                            F.color[k] = (float) PHOTO_FILTERS[i].color[k] / 255.0f;
                        }
                        found = true;
                    }
                    if (!found)
                    {
                        return bad_params (err, cmd, "unknown filter `%s`", id.c_str ());
                    }
                }
                if (!adj_color (p, cmd, "color", F.color, err))
                {
                    return false;
                }
                float density;
                if (!adj_num (p, cmd, "density", F.density * 100.0f, 0.0f, 100.0f, &density, err))
                {
                    return false;
                }
                F.density = density / 100.0f;
                if (!adj_bool (p, cmd, "preserveLuminosity", F.preserve_luminosity, &F.preserve_luminosity, err))
                {
                    return false;
                }
                break;
            }
        case AdjustmentKind::ChannelMixer:
            {
                doc::ChannelMixerParams& M = a.channel_mixer;
                bool was_mono              = M.monochrome;
                if (!adj_bool (p, cmd, "monochrome", was_mono, &M.monochrome, err))
                {
                    return false;
                }
                /* Turning Monochrome on starts from Photoshop's grey mix. */
                if (M.monochrome && !was_mono && !param_present (p, "gray"))
                {
                    const float gray[4] = {0.4f, 0.4f, 0.2f, 0.0f};
                    memcpy (M.matrix[0], gray, sizeof gray);
                }
                const char* rows[4] = {"red", "green", "blue", "gray"};
                for (int r = 0; r < 4; r++)
                {
                    float v[4];
                    if (!adj_numbers (p, cmd, rows[r], 4, v, &present, err))
                    {
                        return false;
                    }
                    if (!present)
                    {
                        continue;
                    }
                    for (int k = 0; k < 4; k++)
                    {
                        M.matrix[r == 3 ? 0 : r][k] = clampf (v[k], -200.0f, 200.0f) / 100.0f;
                    }
                }
                break;
            }
        case AdjustmentKind::GradientMap:
            if (!gradient_stops (p, cmd, &a.gradient_map.stops, err))
            {
                return false;
            }
            if (!adj_bool (p, cmd, "reverse", a.gradient_map.reverse, &a.gradient_map.reverse, err))
            {
                return false;
            }
            if (!adj_bool (p, cmd, "dither", a.gradient_map.dither, &a.gradient_map.dither, err))
            {
                return false;
            }
            break;
        case AdjustmentKind::SelectiveColor:
            if (!selective_from_params (p, cmd, &a, err))
            {
                return false;
            }
            break;
        case AdjustmentKind::ColorLookup:
            return fail (err, ErrorKind::Other, "Color Lookup is not ported yet");
        case AdjustmentKind::Unsupported:
            return fail (err, ErrorKind::Other, "unsupported adjustment");
        }
        *out = a;
        return true;
    }

    /* `adj` applied to the non-default pixels of `s` through the selection, tile by tile in parallel.
       32-bit surfaces get the float behaviour; integer depths the unrounded curves. */
    void adjust_surface (raster::Surface* s, const Adjustment& adj, const raster::Surface* sel, color::ColorMode mode,
        const color::CmykSpace* cmyk)
    {
        Rect r = s->content_bounds ();
        if (r.is_empty ())
        {
            return;
        }
        color::PixelFormat fmt              = s->format ();
        compose::AdjustContext cx           = {compose::Transfer::for_document (mode, fmt.sample),
                      fmt.sample == color::SampleType::F32, fmt.sample, mode, cmyk};
        std::vector<geom::TileCoord> coords = r.tiles ();
        std::vector<raster::Tile*> tiles;
        for (size_t i = 0; i < coords.size (); i++)
        {
            tiles.push_back (s->tile_mut (coords[i]));
        }
        size_t n = fmt.channels ();

#pragma omp parallel for schedule(dynamic)
        for (ptrdiff_t t = 0; t < (ptrdiff_t) coords.size (); t++)
        {
            Rect origin         = coords[(size_t) t].rect ();
            Rect tr             = origin.intersect (r);
            uint8_t* bytes      = tiles[(size_t) t]->data.data ();
            compose::Buffer buf = compose::Buffer::transparent (tr);
            float px[raster::MAX_CHANNELS];
            size_t i = 0;
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                for (int32_t x = tr.x0; x < tr.x1; x++, i++)
                {
                    size_t at = ((size_t) (y - origin.y0) * (size_t) geom::TILE_SIZE + (size_t) (x - origin.x0)) * n;
                    for (size_t c = 0; c < n; c++)
                    {
                        px[c] = color::read_sample (bytes, fmt.sample, at + c);
                    }
                    raster::to_rgba (fmt, px, cmyk, &buf.px[i * 4]);
                }
            }
            compose::Buffer orig = buf;
            compose::apply_adjustment (adj, &buf, cx);
            i = 0;
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                for (int32_t x = tr.x0; x < tr.x1; x++, i++)
                {
                    float k = sel != NULL ? sel->sample_channel (x, y, 0) : 1.0f;
                    float mixed[4];
                    for (int c = 0; c < 4; c++)
                    {
                        mixed[c] = orig.px[i * 4 + c] + (buf.px[i * 4 + c] - orig.px[i * 4 + c]) * k;
                    }
                    raster::from_rgba (fmt, mixed, cmyk, px);
                    size_t at = ((size_t) (y - origin.y0) * (size_t) geom::TILE_SIZE + (size_t) (x - origin.x0)) * n;
                    for (size_t c = 0; c < n; c++)
                    {
                        color::write_sample (bytes, fmt.sample, at + c, px[c]);
                    }
                }
            }
        }
    }

    float q100 (float v)
    {
        return roundf (v * 255.0f * 100.0f) / 100.0f;
    }

    Value levels_obj (const LevelsChannel& c)
    {
        return Value{{"inBlack", q100 (c.in_black)}, {"gamma", roundf (c.gamma * 1000.0f) / 1000.0f},
            {"inWhite", q100 (c.in_white)}, {"outBlack", q100 (c.out_black)}, {"outWhite", q100 (c.out_white)}};
    }

    Value curve_arr (const std::vector<doc::CurvePoint>& c)
    {
        std::vector<doc::CurvePoint> pts = c.size () >= 2 ? c : doc::identity_curve ();
        Value out                        = Value::array ();
        for (size_t i = 0; i < pts.size (); i++)
        {
            out.push_back (Value{q100 (pts[i].input), q100 (pts[i].output)});
        }
        return out;
    }

    Value rgb_arr (const float c[3])
    {
        return Value{c[0], c[1], c[2]};
    }

    /* The complete parameter set of `a`: adjustment_from_params(kind, to_params(a)) == a. */
    Value adjustment_to_params (const Adjustment& a)
    {
        Value p = Value::object ();
        switch (a.kind)
        {
        case AdjustmentKind::BrightnessContrast:
            p["brightness"] = a.brightness_contrast.brightness;
            p["contrast"]   = a.brightness_contrast.contrast;
            p["legacy"]     = a.brightness_contrast.legacy;
            break;
        case AdjustmentKind::Threshold:
            p["level"] = roundf (a.threshold_level * 255.0f);
            break;
        case AdjustmentKind::Posterize:
            p["levels"] = a.posterize_levels;
            break;
        case AdjustmentKind::Exposure:
            p["exposure"] = a.exposure.exposure;
            p["offset"]   = a.exposure.offset;
            p["gamma"]    = a.exposure.gamma;
            break;
        case AdjustmentKind::Vibrance:
            p["vibrance"]   = a.vibrance.vibrance;
            p["saturation"] = a.vibrance.saturation;
            break;
        case AdjustmentKind::HueSaturation:
            {
                const doc::HueSaturationParams& H = a.hue_saturation;
                p["hue"]                          = H.hue;
                p["saturation"]                   = H.saturation;
                p["lightness"]                    = H.lightness;
                p["colorize"]                     = H.colorize;
                for (int i = 0; i < 6; i++)
                {
                    const doc::HueRange& r = H.ranges[i];
                    p[HUE_RANGES[i]] = Value{{"hue", r.hue}, {"saturation", r.saturation}, {"lightness", r.lightness},
                        {"range", {r.bounds[0], r.bounds[1], r.bounds[2], r.bounds[3]}}};
                }
                break;
            }
        case AdjustmentKind::Levels:
            {
                const doc::LevelsParams& L = a.levels;
                const char* keys[4];
                size_t n;
                channel_keys (L.space, keys, &n);
                if (L.space != ToneSpace::Lab)
                {
                    p = levels_obj (L.master);
                }
                const LevelsChannel* chans[4] = {&L.channels[0], &L.channels[1], &L.channels[2], &L.black};
                for (size_t i = 0; i < n; i++)
                {
                    p[keys[i]] = levels_obj (*chans[i]);
                }
                break;
            }
        case AdjustmentKind::Curves:
            {
                const doc::CurvesParams& C = a.curves;
                const char* keys[4];
                size_t n;
                channel_keys (C.space, keys, &n);
                if (C.space != ToneSpace::Lab)
                {
                    p["points"] = curve_arr (C.master);
                }
                const std::vector<doc::CurvePoint>* chans[4] = {
                    &C.channels[0], &C.channels[1], &C.channels[2], &C.black};
                for (size_t i = 0; i < n; i++)
                {
                    p[keys[i]] = curve_arr (*chans[i]);
                }
                break;
            }
        case AdjustmentKind::ColorBalance:
            p["shadows"]            = rgb_arr (a.color_balance.shadows);
            p["midtones"]           = rgb_arr (a.color_balance.midtones);
            p["highlights"]         = rgb_arr (a.color_balance.highlights);
            p["preserveLuminosity"] = a.color_balance.preserve_luminosity;
            break;
        case AdjustmentKind::BlackWhite:
            for (int i = 0; i < 6; i++)
            {
                p[HUE_RANGES[i]] = a.black_white.weights[i];
            }
            p["tint"]      = a.black_white.has_tint;
            p["tintColor"] = rgb_arr (a.black_white.tint);
            break;
        case AdjustmentKind::PhotoFilter:
            p["color"]              = rgb_arr (a.photo_filter.color);
            p["density"]            = a.photo_filter.density * 100.0f;
            p["preserveLuminosity"] = a.photo_filter.preserve_luminosity;
            break;
        case AdjustmentKind::ChannelMixer:
            {
                const doc::ChannelMixerParams& M = a.channel_mixer;
                const char* rows[3]              = {"red", "green", "blue"};
                for (int r = 0; r < 3; r++)
                {
                    p[rows[r]] = Value{M.matrix[r][0] * 100.0f, M.matrix[r][1] * 100.0f, M.matrix[r][2] * 100.0f,
                        M.matrix[r][3] * 100.0f};
                }
                if (M.monochrome)
                {
                    p["gray"] = p["red"];
                }
                p["monochrome"] = M.monochrome;
                break;
            }
        case AdjustmentKind::GradientMap:
            {
                Value stops = Value::array ();
                for (size_t i = 0; i < a.gradient_map.stops.size (); i++)
                {
                    const doc::GradientStop& g = a.gradient_map.stops[i];
                    stops.push_back (Value{g.location, rgb_arr (g.color)});
                }
                p["stops"]   = stops;
                p["reverse"] = a.gradient_map.reverse;
                p["dither"]  = a.gradient_map.dither;
                break;
            }
        case AdjustmentKind::SelectiveColor:
            p["method"] = a.selective_color.relative ? "relative" : "absolute";
            for (int r = 0; r < 9; r++)
            {
                const float* v         = a.selective_color.adjustments[r];
                p[SELECTIVE_RANGES[r]] = Value{v[0], v[1], v[2], v[3]};
            }
            break;
        default:
            break;
        }
        return p;
    }

    /* The Properties panel's edits to an adjustment layer: params merge over the current values;
       no params resets the adjustment to its defaults. */
    bool cmd_layer_set_adjustment (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        bool has_user_keys = false;
        if (p.is_object ())
        {
            for (Value::const_iterator it = p.begin (); it != p.end (); ++it)
            {
                if (it.key () != "layer" && it.key () != "coalesce" && it.key ().compare (0, 2, "__") != 0)
                {
                    has_user_keys = true;
                }
            }
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::Layer* l = d->layer_mut (id);
        if (l == NULL)
        {
            return fail (err, ErrorKind::NoLayer, "no such layer %llu", (unsigned long long) id);
        }
        if (l->kind != doc::LayerKind::Adjustment)
        {
            return fail (err, ErrorKind::Other, "not an adjustment layer");
        }
        if (l->adjustment.kind == AdjustmentKind::Unsupported)
        {
            return fail (err, ErrorKind::Other, "this adjustment can't be edited (kept as imported)");
        }
        Adjustment next;
        if (!adjustment_from_params (l->adjustment.kind, has_user_keys ? p : Value::object (), d->mode,
                has_user_keys ? &l->adjustment : NULL, &next, err))
        {
            return false;
        }
        l->adjustment = next;
        return e.commit ("Modify Adjustment", err);
    }

    /* Image > Adjustments works on a pixel layer. */
    bool has_adjustable (const Session& s, std::string* why)
    {
        const DocState* d = s.active ();
        if (d == NULL)
        {
            *why = "no document open";
            return false;
        }
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l == NULL)
        {
            *why = "no active layer";
            return false;
        }
        if (l->kind != doc::LayerKind::Raster)
        {
            *why = std::string ("active layer is ") + doc::layer_kind_article (l->kind) + " "
                 + doc::layer_kind_name (l->kind) + " layer, not a pixel layer";
            return false;
        }
        return true;
    }

    bool destructive_adjust (Session& s, const Adjustment& adj, const char* label, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, Value (), &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::Layer* l = d->layer_mut (id);
        if (l == NULL)
        {
            return fail (err, ErrorKind::NoLayer, "no such layer %llu", (unsigned long long) id);
        }
        if (!check_pixels_unlocked (*d, id, err))
        {
            return false;
        }
        raster::Surface* surf = l->surface_mut ();
        if (surf == NULL)
        {
            return fail (err, ErrorKind::Other, "the active layer has no pixels");
        }
        adjust_surface (surf, adj, d->has_selection ? &d->selection : NULL, d->mode, s.cmyk);
        surf->prune ();
        return e.commit (label, err);
    }

    bool run_image_adjustment (Session& s, const Value& p, AdjustmentKind kind, Error* err)
    {
        const DocState* d     = s.active ();
        color::ColorMode mode = d != NULL ? d->doc->mode : color::ColorMode::Rgb;
        Adjustment adj;
        if (!adjustment_from_params (kind, p, mode, NULL, &adj, err))
        {
            return false;
        }
        return destructive_adjust (s, adj, adj.label (), err);
    }

    /* A new adjustment layer above the active one, masked by the selection (a white mask reveals
       everything without one). New pixel masks use the document's depth. */
    bool run_new_adjustment_layer (Session& s, const Value& p, AdjustmentKind kind, Value* result, Error* err)
    {
        const DocState* ds    = s.active ();
        color::ColorMode mode = ds != NULL ? ds->doc->mode : color::ColorMode::Rgb;
        Adjustment adj;
        if (!adjustment_from_params (kind, p, mode, NULL, &adj, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::Layer l = doc::Layer::make (s.ids, d->next_layer_name (adj.label ()), doc::LayerKind::Adjustment);
        l.adjustment = adj;
        l.has_mask   = true;
        l.mask       = doc::LayerMask::reveal_all ();
        if (d->has_selection)
        {
            l.mask.surface = d->selection;
        }
        color::PixelFormat mask_format = l.mask.surface.format ().with_sample (d->depth);
        if (l.mask.surface.format () != mask_format)
        {
            l.mask.surface = l.mask.surface.convert (mask_format, NULL);
        }
        doc::LayerId id    = d->insert_above (*e.active_layer (), l);
        *e.active_layer () = id;
        std::string label  = std::string ("New ") + adj.label () + " Layer";
        if (!e.commit (label.c_str (), err))
        {
            return false;
        }
        *result = Value{{"layer", id}};
        return true;
    }

    bool cmd_desaturate (Session& s, const Value& p, Value* result, Error* err)
    {
        Adjustment adj                = Adjustment::make (AdjustmentKind::HueSaturation);
        adj.hue_saturation.saturation = -100.0f;
        return destructive_adjust (s, adj, "Desaturate", err);
    }

    /* One small wrapper per kind: a command's run function carries no state of its own. */
    bool img_brightness_contrast (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::BrightnessContrast, e);
    }
    bool img_levels (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::Levels, e);
    }
    bool img_curves (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::Curves, e);
    }
    bool img_exposure (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::Exposure, e);
    }
    bool img_vibrance (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::Vibrance, e);
    }
    bool img_hue_saturation (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::HueSaturation, e);
    }
    bool img_color_balance (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::ColorBalance, e);
    }
    bool img_black_white (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::BlackWhite, e);
    }
    bool img_photo_filter (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::PhotoFilter, e);
    }
    bool img_channel_mixer (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::ChannelMixer, e);
    }
    bool img_invert (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::Invert, e);
    }
    bool img_posterize (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::Posterize, e);
    }
    bool img_threshold (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::Threshold, e);
    }
    bool img_gradient_map (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::GradientMap, e);
    }
    bool img_selective_color (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::SelectiveColor, e);
    }
    bool img_color_lookup (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_image_adjustment (s, p, AdjustmentKind::ColorLookup, e);
    }

    bool lay_brightness_contrast (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::BrightnessContrast, r, e);
    }
    bool lay_levels (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::Levels, r, e);
    }
    bool lay_curves (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::Curves, r, e);
    }
    bool lay_exposure (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::Exposure, r, e);
    }
    bool lay_vibrance (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::Vibrance, r, e);
    }
    bool lay_hue_saturation (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::HueSaturation, r, e);
    }
    bool lay_color_balance (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::ColorBalance, r, e);
    }
    bool lay_black_white (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::BlackWhite, r, e);
    }
    bool lay_photo_filter (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::PhotoFilter, r, e);
    }
    bool lay_channel_mixer (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::ChannelMixer, r, e);
    }
    bool lay_invert (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::Invert, r, e);
    }
    bool lay_posterize (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::Posterize, r, e);
    }
    bool lay_threshold (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::Threshold, r, e);
    }
    bool lay_gradient_map (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::GradientMap, r, e);
    }
    bool lay_selective_color (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::SelectiveColor, r, e);
    }
    bool lay_color_lookup (Session& s, const Value& p, Value* r, Error* e)
    {
        return run_new_adjustment_layer (s, p, AdjustmentKind::ColorLookup, r, e);
    }

    struct AdjustmentCommand
    {
        const char* image_id;
        const char* layer_id;
        const char* label;
        const char* shortcut;
        const char* params;
        RunFn image_run;
        RunFn layer_run;
    };

    /* Photoshop's Image > Adjustments order. */
    const AdjustmentCommand ADJUSTMENT_COMMANDS[] = {
        {"image.adjustments.brightnessContrast", "layer.newAdjustmentLayer.brightnessContrast", "Brightness/Contrast…",
            NULL, "{\"brightness\":-150..150=0,\"contrast\":-50..100=0 (legacy: -100..100),\"legacy\":bool=false}",
            img_brightness_contrast, lay_brightness_contrast},
        {"image.adjustments.levels", "layer.newAdjustmentLayer.levels", "Levels…", "Cmd+L",
            "{\"inBlack\":0..253=0,\"inWhite\":2..255=255,\"gamma\":0.01..9.99=1,\"outBlack\":0..255=0,\"outWhite\":0.."
            "255=255,\"red\"|\"green\"|\"blue\":{…}?}",
            img_levels, lay_levels},
        {"image.adjustments.curves", "layer.newAdjustmentLayer.curves", "Curves…", "Cmd+M",
            "{\"points\":[[in,out],…] (0..255),\"red\"|\"green\"|\"blue\":[[in,out],…]?}", img_curves, lay_curves},
        {"image.adjustments.exposure", "layer.newAdjustmentLayer.exposure", "Exposure…", NULL,
            "{\"exposure\":-20..20=0,\"offset\":-0.5..0.5=0,\"gamma\":0.01..9.99=1}", img_exposure, lay_exposure},
        {"image.adjustments.vibrance", "layer.newAdjustmentLayer.vibrance", "Vibrance…", NULL,
            "{\"vibrance\":-100..100=0,\"saturation\":-100..100=0}", img_vibrance, lay_vibrance},
        {"image.adjustments.hueSaturation", "layer.newAdjustmentLayer.hueSaturation", "Hue/Saturation…", "Cmd+U",
            "{\"hue\":-180..180=0,\"saturation\":-100..100=0,\"lightness\":-100..100=0,\"colorize\":bool=false,"
            "\"reds\"…\"magentas\":{\"hue\",\"saturation\",\"lightness\",\"range\"}?}",
            img_hue_saturation, lay_hue_saturation},
        {"image.adjustments.colorBalance", "layer.newAdjustmentLayer.colorBalance", "Color Balance…", "Cmd+B",
            "{\"shadows\"|\"midtones\"|\"highlights\":[c-r,m-g,y-b] (-100..100),\"preserveLuminosity\":bool=true}",
            img_color_balance, lay_color_balance},
        {"image.adjustments.blackWhite", "layer.newAdjustmentLayer.blackWhite", "Black & White…", "Cmd+Alt+Shift+B",
            "{\"reds\"…\"magentas\":-200..300,\"tint\":bool=false,\"tintColor\":\"#rrggbb\"}", img_black_white,
            lay_black_white},
        {"image.adjustments.photoFilter", "layer.newAdjustmentLayer.photoFilter", "Photo Filter…", NULL,
            "{\"filter\":preset id,\"color\":\"#rrggbb\",\"density\":0..100=25,\"preserveLuminosity\":bool=true}",
            img_photo_filter, lay_photo_filter},
        {"image.adjustments.channelMixer", "layer.newAdjustmentLayer.channelMixer", "Channel Mixer…", NULL,
            "{\"red\"|\"green\"|\"blue\"|\"gray\":[r %,g %,b %,constant %] (-200..200),\"monochrome\":bool=false}",
            img_channel_mixer, lay_channel_mixer},
        {"image.adjustments.colorLookup", "layer.newAdjustmentLayer.colorLookup", "Color Lookup…", NULL,
            "{} (not ported yet)", img_color_lookup, lay_color_lookup},
        {"image.adjustments.invert", "layer.newAdjustmentLayer.invert", "Invert", "Cmd+I", "{}", img_invert,
            lay_invert},
        {"image.adjustments.posterize", "layer.newAdjustmentLayer.posterize", "Posterize…", NULL,
            "{\"levels\":2..255=4}", img_posterize, lay_posterize},
        {"image.adjustments.threshold", "layer.newAdjustmentLayer.threshold", "Threshold…", NULL,
            "{\"level\":1..255=128}", img_threshold, lay_threshold},
        {"image.adjustments.gradientMap", "layer.newAdjustmentLayer.gradientMap", "Gradient Map…", NULL,
            "{\"stops\":[[location 0..1,\"#rrggbb\"],…],\"reverse\":bool=false,\"dither\":bool=false}",
            img_gradient_map, lay_gradient_map},
        {"image.adjustments.selectiveColor", "layer.newAdjustmentLayer.selectiveColor", "Selective Color…", NULL,
            "{\"method\":\"relative|absolute\"=\"relative\",\"reds\"…\"blacks\":[c,m,y,k] (-100..100)}",
            img_selective_color, lay_selective_color},
    };

    void register_adjustment_commands (std::vector<CommandSpec>* out)
    {
        size_t n = sizeof ADJUSTMENT_COMMANDS / sizeof ADJUSTMENT_COMMANDS[0];
        for (size_t i = 0; i < n; i++)
        {
            const AdjustmentCommand& a = ADJUSTMENT_COMMANDS[i];
            CommandSpec image          = {
                a.image_id, a.label, "Image/Adjustments", a.shortcut, a.params, has_adjustable, a.image_run, true};
            out->push_back (image);
        }
        CommandSpec set_adjustment = {"layer.setAdjustment", "Adjustment Properties", "", NULL,
            "{\"layer\":id?, …params of that adjustment kind}", has_layer, cmd_layer_set_adjustment, true};
        out->push_back (set_adjustment);
        CommandSpec desaturate = {"image.adjustments.desaturate", "Desaturate", "Image/Adjustments", "Cmd+Shift+U",
            "{}", has_adjustable, cmd_desaturate, true};
        out->push_back (desaturate);
        for (size_t i = 0; i < n; i++)
        {
            const AdjustmentCommand& a = ADJUSTMENT_COMMANDS[i];
            CommandSpec layer          = {
                a.layer_id, a.label, "Layer/New Adjustment Layer", NULL, a.params, has_doc, a.layer_run, true};
            out->push_back (layer);
        }
    }

} // namespace engine
