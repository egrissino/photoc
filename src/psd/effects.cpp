#include "psd/effects.h"

#include "psd/descriptor.h"
#include <math.h>
#include <string.h>

namespace psd
{

    /* Layer effects <-> the PSD `lfx2` block (an object-based effects descriptor). Key names follow
       what Photoshop writes, as documented by psd-tools / ag-psd and used by the reference. Contours
       are written Linear; a gradient overlay keeps its first and last colour stops. */

    using color::BlendMode;

    struct BlendName
    {
        BlendMode mode;
        const char* key;
        const char* desc;
    };

    /* The layer record's 4-character key and the descriptor's BlnM value for each mode. */
    const BlendName BLEND_NAMES[] = {
        {BlendMode::PassThrough, "pass", "passThrough"},
        {BlendMode::Normal, "norm", "Nrml"},
        {BlendMode::Dissolve, "diss", "Dslv"},
        {BlendMode::Darken, "dark", "Drkn"},
        {BlendMode::Multiply, "mul ", "Mltp"},
        {BlendMode::ColorBurn, "idiv", "CBrn"},
        {BlendMode::LinearBurn, "lbrn", "linearBurn"},
        {BlendMode::DarkerColor, "dkCl", "darkerColor"},
        {BlendMode::Lighten, "lite", "Lghn"},
        {BlendMode::Screen, "scrn", "Scrn"},
        {BlendMode::ColorDodge, "div ", "CDdg"},
        {BlendMode::LinearDodge, "lddg", "linearDodge"},
        {BlendMode::LighterColor, "lgCl", "lighterColor"},
        {BlendMode::Overlay, "over", "Ovrl"},
        {BlendMode::SoftLight, "sLit", "SftL"},
        {BlendMode::HardLight, "hLit", "HrdL"},
        {BlendMode::VividLight, "vLit", "vividLight"},
        {BlendMode::LinearLight, "lLit", "linearLight"},
        {BlendMode::PinLight, "pLit", "pinLight"},
        {BlendMode::HardMix, "hMix", "hardMix"},
        {BlendMode::Difference, "diff", "Dfrn"},
        {BlendMode::Exclusion, "smud", "Xclu"},
        {BlendMode::Subtract, "fsub", "blendSubtraction"},
        {BlendMode::Divide, "fdiv", "blendDivide"},
        {BlendMode::Hue, "hue ", "H   "},
        {BlendMode::Saturation, "sat ", "Strt"},
        {BlendMode::Color, "colr", "Clr "},
        {BlendMode::Luminosity, "lum ", "Lmns"},
    };
    const size_t BLEND_NAME_COUNT = sizeof BLEND_NAMES / sizeof BLEND_NAMES[0];

    /* Long names some writers use in descriptors instead of the short codes. */
    const BlendName BLEND_LONG[] = {
        {BlendMode::Normal, "", "normal"},
        {BlendMode::Dissolve, "", "dissolve"},
        {BlendMode::Darken, "", "darken"},
        {BlendMode::Multiply, "", "multiply"},
        {BlendMode::ColorBurn, "", "colorBurn"},
        {BlendMode::Lighten, "", "lighten"},
        {BlendMode::Screen, "", "screen"},
        {BlendMode::ColorDodge, "", "colorDodge"},
        {BlendMode::Overlay, "", "overlay"},
        {BlendMode::SoftLight, "", "softLight"},
        {BlendMode::HardLight, "", "hardLight"},
        {BlendMode::Difference, "", "difference"},
        {BlendMode::Exclusion, "", "exclusion"},
        {BlendMode::Hue, "", "hue"},
        {BlendMode::Saturation, "", "saturation"},
        {BlendMode::Color, "", "color"},
        {BlendMode::Luminosity, "", "luminosity"},
    };

    const char* blend_key (BlendMode m)
    {
        for (size_t i = 0; i < BLEND_NAME_COUNT; i++)
        {
            if (BLEND_NAMES[i].mode == m)
            {
                return BLEND_NAMES[i].key;
            }
        }
        return "norm";
    }

    bool blend_from_key (const char* key, BlendMode* out)
    {
        for (size_t i = 0; i < BLEND_NAME_COUNT; i++)
        {
            if (memcmp (BLEND_NAMES[i].key, key, 4) == 0)
            {
                *out = BLEND_NAMES[i].mode;
                return true;
            }
        }
        return false;
    }

    BlendMode blend_of (const DValue& d, const char* key, BlendMode fallback)
    {
        std::string v = d_enum_of (d, key);
        for (size_t i = 0; i < BLEND_NAME_COUNT; i++)
        {
            if (v == BLEND_NAMES[i].desc)
            {
                return BLEND_NAMES[i].mode;
            }
        }
        for (size_t i = 0; i < sizeof BLEND_LONG / sizeof BLEND_LONG[0]; i++)
        {
            if (v == BLEND_LONG[i].desc)
            {
                return BLEND_LONG[i].mode;
            }
        }
        return fallback;
    }

    DValue blend_value (BlendMode m)
    {
        for (size_t i = 0; i < BLEND_NAME_COUNT; i++)
        {
            if (BLEND_NAMES[i].mode == m)
            {
                return d_enum ("BlnM", BLEND_NAMES[i].desc);
            }
        }
        return d_enum ("BlnM", "Nrml");
    }

    /* A colour descriptor (RGB 0..255, newer 0..1 floats, gray, CMYK or Lab approximated). */
    void color_from (const DValue* c, float out[3])
    {
        if (c == NULL || c->type != DType::Object)
        {
            return;
        }
        if (d_get (*c, "redFloat") != NULL)
        {
            out[0] = (float) d_num (*c, "redFloat", 0);
            out[1] = (float) d_num (*c, "greenFloat", 0);
            out[2] = (float) d_num (*c, "blueFloat", 0);
        }
        else if (c->cls == "Grsc")
        {
            out[0] = out[1] = out[2] = 1.0f - (float) d_num (*c, "Gry ", 0) / 100.0f;
        }
        else if (c->cls == "CMYC")
        {
            float k = 1.0f - (float) d_num (*c, "Blck", 0) / 100.0f;
            out[0]  = (1.0f - (float) d_num (*c, "Cyn ", 0) / 100.0f) * k;
            out[1]  = (1.0f - (float) d_num (*c, "Mgnt", 0) / 100.0f) * k;
            out[2]  = (1.0f - (float) d_num (*c, "Ylw ", 0) / 100.0f) * k;
        }
        else
        {
            out[0] = (float) d_num (*c, "Rd  ", 0) / 255.0f;
            out[1] = (float) d_num (*c, "Grn ", 0) / 255.0f;
            out[2] = (float) d_num (*c, "Bl  ", 0) / 255.0f;
        }
        for (int i = 0; i < 3; i++)
        {
            out[i] = fminf (fmaxf (out[i], 0.0f), 1.0f);
        }
    }

    DValue color_value (const float c[3])
    {
        DValue d = d_object ("RGBC");
        d_set (&d, "Rd  ", d_double (c[0] * 255.0));
        d_set (&d, "Grn ", d_double (c[1] * 255.0));
        d_set (&d, "Bl  ", d_double (c[2] * 255.0));
        return d;
    }

    DValue linear_contour ()
    {
        DValue c = d_object ("ShpC");
        d_set (&c, "Nm  ", d_text ("Linear"));
        DValue pts = d_list ();
        for (int i = 0; i < 2; i++)
        {
            DValue p = d_object ("CrPt");
            d_set (&p, "Hrzn", d_double (i * 255.0));
            d_set (&p, "Vrtc", d_double (i * 255.0));
            pts.items.push_back (p);
        }
        d_set (&c, "Crv ", pts);
        return c;
    }

    float pct (const DValue& d, const char* key, float fallback)
    {
        return (float) d_num (d, key, fallback * 100.0) / 100.0f;
    }

    /* One effect from its descriptor (kind given by the key it was stored under). */
    bool parse_one (const std::string& key, const DValue& d, doc::Effect* out)
    {
        if (!d_flag (d, "present", true))
        {
            return false;
        }
        doc::FxKind kind;
        if (key == "DrSh")
        {
            kind = doc::FxKind::DropShadow;
        }
        else if (key == "IrSh")
        {
            kind = doc::FxKind::InnerShadow;
        }
        else if (key == "OrGl")
        {
            kind = doc::FxKind::OuterGlow;
        }
        else if (key == "IrGl")
        {
            kind = doc::FxKind::InnerGlow;
        }
        else if (key == "FrFX")
        {
            kind = doc::FxKind::Stroke;
        }
        else if (key == "SoFi")
        {
            kind = doc::FxKind::ColorOverlay;
        }
        else if (key == "GrFl")
        {
            kind = doc::FxKind::GradientOverlay;
        }
        else if (key == "ChFX")
        {
            kind = doc::FxKind::Satin;
        }
        else if (key == "ebbl")
        {
            kind = doc::FxKind::BevelEmboss;
        }
        else
        {
            return false;
        }
        doc::Effect e = doc::Effect::make (kind);
        e.enabled     = d_flag (d, "enab", true);
        e.blend       = blend_of (d, "Md  ", e.blend);
        e.opacity     = pct (d, "Opct", e.opacity);
        color_from (d_get (d, "Clr "), e.color);
        e.angle        = (float) d_num (d, "lagl", e.angle);
        e.global_light = d_flag (d, "uglg", e.global_light);
        e.distance     = (float) d_num (d, "Dstn", e.distance);
        e.spread       = pct (d, "Ckmt", e.spread);
        e.size         = (float) d_num (d, "blur", e.size);
        switch (kind)
        {
        case doc::FxKind::DropShadow:
            e.knocks_out = d_flag (d, "layerConceals", true);
            break;
        case doc::FxKind::OuterGlow:
        case doc::FxKind::InnerGlow:
            e.precise = d_enum_of (d, "GlwT") == "PrBL";
            e.range   = pct (d, "Inpr", e.range);
            e.center  = d_enum_of (d, "glwS") == "SrcC";
            break;
        case doc::FxKind::Stroke:
            {
                e.size         = (float) d_num (d, "Sz  ", e.size);
                std::string st = d_enum_of (d, "Styl");
                e.position     = st == "InsF" ? doc::StrokePosition::Inside
                               : st == "CtrF" ? doc::StrokePosition::Center
                                              : doc::StrokePosition::Outside;
                break;
            }
        case doc::FxKind::Satin:
            e.invert = d_flag (d, "Invr", e.invert);
            break;
        case doc::FxKind::GradientOverlay:
            {
                e.angle            = (float) d_num (d, "Angl", 90.0);
                e.scale            = pct (d, "Scl ", 1.0f);
                e.reverse          = d_flag (d, "Rvrs", false);
                std::string t      = d_enum_of (d, "Type");
                e.gradient         = t == "Rdl " ? doc::GradientStyle::Radial
                                   : t == "Angl" ? doc::GradientStyle::Angle
                                   : t == "Rflc" ? doc::GradientStyle::Reflected
                                   : t == "Dmnd" ? doc::GradientStyle::Diamond
                                                 : doc::GradientStyle::Linear;
                const DValue* g    = d_get (d, "Grad");
                const DValue* clrs = g != NULL ? d_get (*g, "Clrs") : NULL;
                if (clrs != NULL && clrs->type == DType::List && !clrs->items.empty ())
                {
                    /* The stops nearest the two ends. */
                    size_t lo = 0, hi = 0;
                    for (size_t i = 0; i < clrs->items.size (); i++)
                    {
                        if (d_num (clrs->items[i], "Lctn", 0) < d_num (clrs->items[lo], "Lctn", 0))
                        {
                            lo = i;
                        }
                        if (d_num (clrs->items[i], "Lctn", 0) > d_num (clrs->items[hi], "Lctn", 0))
                        {
                            hi = i;
                        }
                    }
                    color_from (d_get (clrs->items[lo], "Clr "), e.color);
                    color_from (d_get (clrs->items[hi], "Clr "), e.color2);
                }
                break;
            }
        case doc::FxKind::BevelEmboss:
            {
                std::string st = d_enum_of (d, "bvlS");
                e.bevel        = st == "OtrB" ? doc::BevelStyle::OuterBevel
                               : st == "Embs" ? doc::BevelStyle::Emboss
                               : st == "PlEb" ? doc::BevelStyle::PillowEmboss
                                              : doc::BevelStyle::InnerBevel;
                e.depth        = pct (d, "srgR", 1.0f);
                e.up           = d_enum_of (d, "bvlD") != "Out ";
                e.soften       = (float) d_num (d, "Sftn", 0);
                e.altitude     = (float) d_num (d, "Lald", 30);
                e.blend        = blend_of (d, "hglM", BlendMode::Screen);
                e.opacity      = pct (d, "hglO", 0.75f);
                e.color[0] = e.color[1] = e.color[2] = 1.0f;
                color_from (d_get (d, "hglC"), e.color);
                e.blend2    = blend_of (d, "sdwM", BlendMode::Multiply);
                e.opacity2  = pct (d, "sdwO", 0.75f);
                e.color2[0] = e.color2[1] = e.color2[2] = 0.0f;
                color_from (d_get (d, "sdwC"), e.color2);
                break;
            }
        default:
            break;
        }
        *out = e;
        return true;
    }

    struct KindKey
    {
        const char* single;
        const char* multi;
        doc::FxKind kind;
    };

    /* In the Layers panel's order, which is also the order effects are read in. */
    const KindKey KIND_KEYS[] = {
        {"ebbl", "", doc::FxKind::BevelEmboss},
        {"FrFX", "frameFXMulti", doc::FxKind::Stroke},
        {"IrSh", "innerShadowMulti", doc::FxKind::InnerShadow},
        {"IrGl", "", doc::FxKind::InnerGlow},
        {"ChFX", "", doc::FxKind::Satin},
        {"SoFi", "solidFillMulti", doc::FxKind::ColorOverlay},
        {"GrFl", "gradientFillMulti", doc::FxKind::GradientOverlay},
        {"OrGl", "", doc::FxKind::OuterGlow},
        {"DrSh", "dropShadowMulti", doc::FxKind::DropShadow},
    };
    const size_t KIND_KEY_COUNT = sizeof KIND_KEYS / sizeof KIND_KEYS[0];

    /* `lfx2` block data: a version (0), the descriptor version (16) and the descriptor. */
    bool parse_lfx2 (const uint8_t* data, size_t len, doc::LayerStyle* out)
    {
        Reader r = reader (data, len);
        rd_u32 (&r);
        rd_u32 (&r);
        DValue d;
        if (!parse_descriptor (&r, &d))
        {
            return false;
        }
        out->enabled = d_flag (d, "masterFXSwitch", true);
        out->items.clear ();
        for (size_t k = 0; k < KIND_KEY_COUNT; k++)
        {
            const DValue* multi = KIND_KEYS[k].multi[0] != 0 ? d_get (d, KIND_KEYS[k].multi) : NULL;
            if (multi != NULL && multi->type == DType::List)
            {
                for (size_t i = 0; i < multi->items.size (); i++)
                {
                    doc::Effect e;
                    if (multi->items[i].type == DType::Object && parse_one (KIND_KEYS[k].single, multi->items[i], &e))
                    {
                        out->items.push_back (e);
                    }
                }
                continue;
            }
            const DValue* one = d_get (d, KIND_KEYS[k].single);
            doc::Effect e;
            if (one != NULL && one->type == DType::Object && parse_one (KIND_KEYS[k].single, *one, &e))
            {
                out->items.push_back (e);
            }
        }
        return true;
    }

    DValue with_common (const char* cls, const doc::Effect& e)
    {
        DValue d = d_object (cls);
        d_set (&d, "enab", d_bool (e.enabled));
        d_set (&d, "present", d_bool (true));
        d_set (&d, "showInDialog", d_bool (true));
        d_set (&d, "Md  ", blend_value (e.blend));
        d_set (&d, "Opct", d_unit ("#Prc", e.opacity * 100.0));
        return d;
    }

    DValue write_one (const doc::Effect& e)
    {
        switch (e.kind)
        {
        case doc::FxKind::DropShadow:
        case doc::FxKind::InnerShadow:
            {
                bool inner = e.kind == doc::FxKind::InnerShadow;
                DValue d   = with_common (inner ? "IrSh" : "DrSh", e);
                d_set (&d, "Clr ", color_value (e.color));
                d_set (&d, "uglg", d_bool (e.global_light));
                d_set (&d, "lagl", d_unit ("#Ang", e.angle));
                d_set (&d, "Dstn", d_unit ("#Pxl", e.distance));
                d_set (&d, "Ckmt", d_unit ("#Pxl", e.spread * 100.0));
                d_set (&d, "blur", d_unit ("#Pxl", e.size));
                d_set (&d, "Nose", d_unit ("#Prc", 0));
                d_set (&d, "AntA", d_bool (false));
                d_set (&d, "TrnS", linear_contour ());
                if (!inner)
                {
                    d_set (&d, "layerConceals", d_bool (e.knocks_out));
                }
                return d;
            }
        case doc::FxKind::OuterGlow:
        case doc::FxKind::InnerGlow:
            {
                bool inner = e.kind == doc::FxKind::InnerGlow;
                DValue d   = with_common (inner ? "IrGl" : "OrGl", e);
                d_set (&d, "Clr ", color_value (e.color));
                d_set (&d, "GlwT", d_enum ("BETE", e.precise ? "PrBL" : "SfBL"));
                d_set (&d, "Ckmt", d_unit ("#Pxl", e.spread * 100.0));
                d_set (&d, "blur", d_unit ("#Pxl", e.size));
                d_set (&d, "Nose", d_unit ("#Prc", 0));
                d_set (&d, "ShdN", d_unit ("#Prc", 0));
                d_set (&d, "AntA", d_bool (false));
                d_set (&d, "TrnS", linear_contour ());
                d_set (&d, "Inpr", d_unit ("#Prc", e.range * 100.0));
                if (inner)
                {
                    d_set (&d, "glwS", d_enum ("IGSr", e.center ? "SrcC" : "SrcE"));
                }
                return d;
            }
        case doc::FxKind::Stroke:
            {
                DValue d         = with_common ("FrFX", e);
                const char* styl = e.position == doc::StrokePosition::Inside ? "InsF"
                                 : e.position == doc::StrokePosition::Center ? "CtrF"
                                                                             : "OutF";
                d_set (&d, "Styl", d_enum ("FStl", styl));
                d_set (&d, "PntT", d_enum ("FrFl", "SClr"));
                d_set (&d, "Sz  ", d_unit ("#Pxl", e.size));
                d_set (&d, "Clr ", color_value (e.color));
                return d;
            }
        case doc::FxKind::ColorOverlay:
            {
                DValue d = with_common ("SoFi", e);
                d_set (&d, "Clr ", color_value (e.color));
                return d;
            }
        case doc::FxKind::GradientOverlay:
            {
                DValue d = with_common ("GrFl", e);
                DValue g = d_object ("Grdn");
                d_set (&g, "Nm  ", d_text ("Custom"));
                d_set (&g, "GrdF", d_enum ("GrdF", "CstS"));
                d_set (&g, "Intr", d_double (0.0));
                DValue clrs = d_list (), trns = d_list ();
                for (int i = 0; i < 2; i++)
                {
                    DValue s = d_object ("Clrt");
                    d_set (&s, "Clr ", color_value (i == 0 ? e.color : e.color2));
                    d_set (&s, "Type", d_enum ("Clry", "UsrS"));
                    d_set (&s, "Lctn", d_long (i * 4096));
                    d_set (&s, "Mdpn", d_long (50));
                    clrs.items.push_back (s);
                    DValue t = d_object ("TrnS");
                    d_set (&t, "Opct", d_unit ("#Prc", 100.0));
                    d_set (&t, "Lctn", d_long (i * 4096));
                    d_set (&t, "Mdpn", d_long (50));
                    trns.items.push_back (t);
                }
                d_set (&g, "Clrs", clrs);
                d_set (&g, "Trns", trns);
                d_set (&d, "Grad", g);
                d_set (&d, "Angl", d_unit ("#Ang", e.angle));
                const char* types[5] = {"Lnr ", "Rdl ", "Angl", "Rflc", "Dmnd"};
                d_set (&d, "Type", d_enum ("GrdT", types[(int) e.gradient]));
                d_set (&d, "Rvrs", d_bool (e.reverse));
                d_set (&d, "Algn", d_bool (true));
                d_set (&d, "Scl ", d_unit ("#Prc", e.scale * 100.0));
                d_set (&d, "Dthr", d_bool (false));
                DValue off = d_object ("Pnt ");
                d_set (&off, "Hrzn", d_unit ("#Prc", 0));
                d_set (&off, "Vrtc", d_unit ("#Prc", 0));
                d_set (&d, "Ofst", off);
                return d;
            }
        case doc::FxKind::Satin:
            {
                DValue d = with_common ("ChFX", e);
                d_set (&d, "Clr ", color_value (e.color));
                d_set (&d, "AntA", d_bool (true));
                d_set (&d, "Invr", d_bool (e.invert));
                d_set (&d, "lagl", d_unit ("#Ang", e.angle));
                d_set (&d, "Dstn", d_unit ("#Pxl", e.distance));
                d_set (&d, "blur", d_unit ("#Pxl", e.size));
                d_set (&d, "MpgS", linear_contour ());
                return d;
            }
        case doc::FxKind::BevelEmboss:
            {
                DValue d = d_object ("ebbl");
                d_set (&d, "enab", d_bool (e.enabled));
                d_set (&d, "present", d_bool (true));
                d_set (&d, "showInDialog", d_bool (true));
                d_set (&d, "hglM", blend_value (e.blend));
                d_set (&d, "hglC", color_value (e.color));
                d_set (&d, "hglO", d_unit ("#Prc", e.opacity * 100.0));
                d_set (&d, "sdwM", blend_value (e.blend2));
                d_set (&d, "sdwC", color_value (e.color2));
                d_set (&d, "sdwO", d_unit ("#Prc", e.opacity2 * 100.0));
                d_set (&d, "bvlT", d_enum ("bvlT", "SfBL"));
                const char* styles[4] = {"OtrB", "InrB", "Embs", "PlEb"};
                d_set (&d, "bvlS", d_enum ("BESl", styles[(int) e.bevel]));
                d_set (&d, "uglg", d_bool (e.global_light));
                d_set (&d, "lagl", d_unit ("#Ang", e.angle));
                d_set (&d, "Lald", d_unit ("#Ang", e.altitude));
                d_set (&d, "srgR", d_unit ("#Prc", e.depth * 100.0));
                d_set (&d, "blur", d_unit ("#Pxl", e.size));
                d_set (&d, "bvlD", d_enum ("BESs", e.up ? "In  " : "Out "));
                d_set (&d, "TrnS", linear_contour ());
                d_set (&d, "Sftn", d_unit ("#Pxl", e.soften));
                d_set (&d, "useShape", d_bool (false));
                d_set (&d, "useTexture", d_bool (false));
                return d;
            }
        }
        return d_object ("null");
    }

    std::vector<uint8_t> write_lfx2 (const doc::LayerStyle& st)
    {
        DValue d = d_object ("null");
        d_set (&d, "Scl ", d_unit ("#Prc", 100.0));
        d_set (&d, "masterFXSwitch", d_bool (st.enabled));
        for (size_t k = 0; k < KIND_KEY_COUNT; k++)
        {
            std::vector<DValue> of_kind;
            for (size_t i = 0; i < st.items.size (); i++)
            {
                if (st.items[i].kind == KIND_KEYS[k].kind)
                {
                    of_kind.push_back (write_one (st.items[i]));
                }
            }
            if (of_kind.empty ())
            {
                continue;
            }
            if (of_kind.size () == 1 || KIND_KEYS[k].multi[0] == 0)
            {
                d_set (&d, KIND_KEYS[k].single, of_kind[0]);
            }
            else
            {
                DValue list = d_list ();
                list.items  = of_kind;
                d_set (&d, KIND_KEYS[k].multi, list);
            }
        }
        std::vector<uint8_t> out;
        wr_u32 (&out, 0);
        wr_u32 (&out, 16);
        write_descriptor (&out, d);
        return out;
    }

} // namespace psd
