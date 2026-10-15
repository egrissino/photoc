#include "psd/type.h"

#include "psd/bytes.h"
#include "psd/descriptor.h"
#include "psd/engine_data.h"
#include "text/layout.h"
#include <algorithm>
#include <math.h>
#include <string.h>

namespace psd
{

    /* Type layers in PSD: the TySh block (the transform, then a text descriptor holding the text,
       anti-aliasing, bounds and EngineData, then a warp descriptor) <-> doc::TextLayer.

       EngineData sizes are text-space units that the transform maps to document pixels, shown by
       Photoshop as points at the document's resolution: size_pt = FontSize x 72 / dpi (likewise
       leading, baseline shift, indents and spacing). Runs are measured in UTF-16 units over the
       engine text, which uses \r for paragraph breaks and ends with one. Fonts are PostScript
       names. Writing patches the layer's original TySh when it has one, so settings this build
       doesn't model (warp, Photoshop's own keys) survive an edit. */

    struct TySh
    {
        double transform[6];
        DValue text;
        DValue warp;
        bool has_warp;
        float bounds[4];
    };

    bool parse_tysh (const uint8_t* data, size_t len, TySh* out)
    {
        Reader r = reader (data, len);
        rd_u16 (&r);
        for (int i = 0; i < 6; i++)
        {
            out->transform[i] = rd_f64 (&r);
        }
        rd_u16 (&r);
        rd_u32 (&r);
        if (!r.ok || !parse_descriptor (&r, &out->text))
        {
            return false;
        }
        out->has_warp = false;
        if (remaining (r) >= 6)
        {
            size_t at = r.pos;
            rd_u16 (&r);
            rd_u32 (&r);
            if (parse_descriptor (&r, &out->warp))
            {
                out->has_warp = true;
            }
            else
            {
                r.pos = at, r.ok = true;
            }
        }
        for (int i = 0; i < 4; i++)
        {
            uint32_t u = remaining (r) >= 4 ? rd_u32 (&r) : 0;
            memcpy (&out->bounds[i], &u, sizeof (float));
        }
        return true;
    }

    /* UTF-16 run lengths over `text` as UTF-8 byte lengths. */
    std::vector<size_t> utf16_to_bytes (const std::string& text, const std::vector<double>& lens)
    {
        std::vector<size_t> out;
        size_t at = 0;
        for (size_t i = 0; i < lens.size (); i++)
        {
            long units   = lens[i] > 0 ? (long) lens[i] : 0;
            size_t bytes = 0;
            while (units > 0 && at < text.size ())
            {
                uint8_t c = (uint8_t) text[at];
                size_t n  = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
                units -= n == 4 ? 2 : 1;
                bytes += n;
                at += n;
            }
            out.push_back (bytes);
        }
        return out;
    }

    size_t utf16_len (const std::string& s)
    {
        size_t n = 0;
        for (size_t i = 0; i < s.size ();)
        {
            uint8_t c = (uint8_t) s[i];
            size_t k  = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            n += k == 4 ? 2 : 1;
            i += k;
        }
        return n;
    }

    std::vector<double> num_array (const EValue* v)
    {
        std::vector<double> out;
        if (v == NULL || v->type != EType::Array)
        {
            return out;
        }
        for (size_t i = 0; i < v->items.size (); i++)
        {
            double d;
            if (e_num (&v->items[i], &d))
            {
                out.push_back (d);
            }
        }
        return out;
    }

    /* A key from the run's own data, else its base. */
    const EValue* lookup (const EValue& run, const EValue* base, const char* key)
    {
        const EValue* v = e_get (run, key);
        return v != NULL || base == NULL ? v : e_get (*base, key);
    }

    double num_of (const EValue& run, const EValue* base, const char* key, double fallback)
    {
        double d;
        return e_num (lookup (run, base, key), &d) ? d : fallback;
    }

    bool flag_of (const EValue& run, const EValue* base, const char* key, bool fallback)
    {
        bool b;
        return e_flag (lookup (run, base, key), &b) ? b : fallback;
    }

    doc::CharStyle char_style_of (const EValue& d, const EValue* base, const std::vector<std::string>& fontset, float k,
        text::FontDb* fonts, std::vector<std::string>* missing)
    {
        doc::CharStyle s = doc::CharStyle::make ();
        double fi        = num_of (d, base, "Font", -1);
        if (fi >= 0 && (size_t) fi < fontset.size ())
        {
            const std::string& ps = fontset[(size_t) fi];
            s.postscript          = ps;
            int face              = fonts != NULL ? fonts->find_postscript (ps) : -1;
            if (face >= 0)
            {
                s.family = fonts->face (face).family;
                s.style  = fonts->face (face).style;
            }
            else
            {
                size_t dash = ps.find ('-');
                s.family    = dash == std::string::npos ? ps : ps.substr (0, dash);
                s.style     = dash == std::string::npos ? "Regular" : ps.substr (dash + 1);
                if (missing != NULL && std::find (missing->begin (), missing->end (), ps) == missing->end ())
                {
                    missing->push_back (ps);
                }
            }
        }
        s.size_pt = (float) num_of (d, base, "FontSize", 12.0 / k) * k;
        if (!flag_of (d, base, "AutoLeading", true))
        {
            s.leading_pt = (float) num_of (d, base, "Leading", 0) * k;
        }
        s.tracking          = (float) num_of (d, base, "Tracking", 0);
        s.baseline_shift_pt = (float) num_of (d, base, "BaselineShift", 0) * k;
        s.hscale            = (float) num_of (d, base, "HorizontalScale", 1);
        s.vscale            = (float) num_of (d, base, "VerticalScale", 1);
        s.faux_bold         = flag_of (d, base, "FauxBold", false);
        s.faux_italic       = flag_of (d, base, "FauxItalic", false);
        s.underline         = flag_of (d, base, "Underline", false);
        s.strikethrough     = flag_of (d, base, "Strikethrough", false);
        s.ligatures         = flag_of (d, base, "Ligatures", true);
        s.kerning           = flag_of (d, base, "AutoKerning", true);
        s.all_caps          = num_of (d, base, "FontCaps", 0) == 2;
        const EValue* fill  = lookup (d, base, "FillColor");
        if (fill != NULL)
        {
            std::vector<double> v = num_array (e_get (*fill, "Values"));
            if (v.size () == 4)
            {
                for (int c = 0; c < 3; c++)
                {
                    s.color[c] = (float) v[(size_t) c + 1];
                }
            }
            else if (v.size () == 2)
            {
                s.color[0] = s.color[1] = s.color[2] = (float) v[1];
            }
            else if (v.size () == 5)
            {
                float kk = 1.0f - (float) v[4];
                for (int c = 0; c < 3; c++)
                {
                    s.color[c] = (1.0f - (float) v[(size_t) c + 1]) * kk;
                }
            }
        }
        return s;
    }

    doc::ParaStyle para_style_of (const EValue& d, const EValue* base, float k)
    {
        doc::ParaStyle p            = doc::ParaStyle::make ();
        int j                       = (int) num_of (d, base, "Justification", 0);
        const doc::TextAlign map[7] = {doc::TextAlign::Left, doc::TextAlign::Right, doc::TextAlign::Center,
            doc::TextAlign::JustifyLeft, doc::TextAlign::JustifyRight, doc::TextAlign::JustifyCenter,
            doc::TextAlign::JustifyAll};
        p.align                     = j >= 0 && j < 7 ? map[j] : doc::TextAlign::Left;
        p.first_indent_pt           = (float) num_of (d, base, "FirstLineIndent", 0) * k;
        p.start_indent_pt           = (float) num_of (d, base, "StartIndent", 0) * k;
        p.end_indent_pt             = (float) num_of (d, base, "EndIndent", 0) * k;
        p.space_before_pt           = (float) num_of (d, base, "SpaceBefore", 0) * k;
        p.space_after_pt            = (float) num_of (d, base, "SpaceAfter", 0) * k;
        p.auto_leading              = (float) num_of (d, base, "AutoLeading", 1.2);
        return p;
    }

    /* The sheet a ResourceDict index key points at, in set `set`, under `inner`. */
    const EValue* normal_sheet (const EValue* res, const char* set, const char* index_key, const char* inner)
    {
        if (res == NULL)
        {
            return NULL;
        }
        const EValue* s = e_get (*res, set);
        if (s == NULL || s->type != EType::Array || s->items.empty ())
        {
            return NULL;
        }
        double i = 0;
        e_num (e_get (*res, index_key), &i);
        size_t at = i >= 0 && (size_t) i < s->items.size () ? (size_t) i : 0;
        return e_get (s->items[at], inner);
    }

    bool tysh_to_text (const uint8_t* data, size_t len, float dpi, text::FontDb* fonts, doc::TextLayer* out,
        std::vector<std::string>* missing)
    {
        TySh t;
        if (!parse_tysh (data, len, &t))
        {
            return false;
        }
        doc::TextLayer layer = doc::TextLayer::make ();
        memcpy (layer.transform, t.transform, sizeof layer.transform);
        std::string aa;
        const DValue* ant = d_get (t.text, "AntA");
        if (ant != NULL && ant->type == DType::Enum)
        {
            aa = ant->str;
        }
        if (aa == "Anno" || aa == "antiAliasNone")
        {
            layer.antialias = doc::AntiAlias::None;
        }
        else if (aa == "AnSh" || aa == "antiAliasSharp")
        {
            layer.antialias = doc::AntiAlias::Sharp;
        }
        else if (aa == "AnCr" || aa == "antiAliasCrisp")
        {
            layer.antialias = doc::AntiAlias::Crisp;
        }
        else if (aa == "AnSt" || aa == "antiAliasStrong")
        {
            layer.antialias = doc::AntiAlias::Strong;
        }
        std::string txt;
        const DValue* tv = d_get (t.text, "Txt ");
        bool has_txt     = tv != NULL && tv->type == DType::Text;
        if (has_txt)
        {
            txt = tv->str;
        }
        const DValue* raw = d_get (t.text, "EngineData");
        EValue e;
        float k = 72.0f / (dpi > 0.0f ? dpi : 72.0f);
        if (raw == NULL || raw->type != DType::Raw
            || !parse_engine_data ((const uint8_t*) raw->str.data (), raw->str.size (), &e))
        {
            std::replace (txt.begin (), txt.end (), '\r', '\n');
            layer.text        = txt;
            layer.runs[0].len = txt.size ();
            layer.paras       = layer.para_runs ();
            *out              = layer;
            return true;
        }
        const char* text_path[3] = {"EngineDict", "Editor", "Text"};
        const EValue* edt        = e_path (e, text_path, 3);
        std::string ed_text      = edt != NULL && edt->type == EType::String ? edt->str : txt + "\r";
        std::string text         = has_txt ? txt : ed_text;
        if (!has_txt && !text.empty () && text.back () == '\r')
        {
            text.pop_back ();
        }
        std::replace (text.begin (), text.end (), '\r', '\n');
        layer.text        = text;
        const EValue* res = e_get (e, "ResourceDict");
        if (res == NULL)
        {
            res = e_get (e, "DocumentResources");
        }
        std::vector<std::string> fontset;
        const EValue* fs = res != NULL ? e_get (*res, "FontSet") : NULL;
        if (fs != NULL && fs->type == EType::Array)
        {
            for (size_t i = 0; i < fs->items.size (); i++)
            {
                const EValue* n = e_get (fs->items[i], "Name");
                fontset.push_back (n != NULL ? n->str : std::string ());
            }
        }
        const EValue* normal_style = normal_sheet (res, "StyleSheetSet", "TheNormalStyleSheet", "StyleSheetData");
        const EValue* normal_para  = normal_sheet (res, "ParagraphSheetSet", "TheNormalParagraphSheet", "Properties");
        EValue empty               = e_dict ();
        /* Character runs. */
        const char* srun_path[2] = {"EngineDict", "StyleRun"};
        const EValue* srun       = e_path (e, srun_path, 2);
        if (srun != NULL)
        {
            const EValue* ra         = e_get (*srun, "RunArray");
            std::vector<size_t> lens = utf16_to_bytes (ed_text, num_array (e_get (*srun, "RunLengthArray")));
            if (ra != NULL && ra->type == EType::Array && !ra->items.empty ())
            {
                layer.runs.clear ();
                for (size_t i = 0; i < ra->items.size (); i++)
                {
                    const EValue* sheet = e_get (ra->items[i], "StyleSheet");
                    const EValue* data  = sheet != NULL ? e_get (*sheet, "StyleSheetData") : NULL;
                    doc::TextRun r      = {i < lens.size () ? lens[i] : 0,
                        char_style_of (data != NULL ? *data : empty, normal_style, fontset, k, fonts, missing)};
                    layer.runs.push_back (r);
                }
            }
        }
        /* Paragraph runs. */
        const char* prun_path[2] = {"EngineDict", "ParagraphRun"};
        const EValue* prun       = e_path (e, prun_path, 2);
        if (prun != NULL)
        {
            const EValue* ra         = e_get (*prun, "RunArray");
            std::vector<size_t> lens = utf16_to_bytes (ed_text, num_array (e_get (*prun, "RunLengthArray")));
            if (ra != NULL && ra->type == EType::Array && !ra->items.empty ())
            {
                /* One style per paragraph, from the run each paragraph starts in. */
                std::vector<doc::ParaStyle> run_styles;
                for (size_t i = 0; i < ra->items.size (); i++)
                {
                    const EValue* sheet = e_get (ra->items[i], "ParagraphSheet");
                    const EValue* props = sheet != NULL ? e_get (*sheet, "Properties") : NULL;
                    run_styles.push_back (para_style_of (props != NULL ? *props : empty, normal_para, k));
                }
                layer.paras.clear ();
                size_t at = 0, ri = 0, run_end = lens.empty () ? text.size () : lens[0];
                for (;;)
                {
                    while (ri + 1 < run_styles.size () && at >= run_end)
                    {
                        run_end += ri + 1 < lens.size () ? lens[++ri] : 0;
                    }
                    doc::ParaRun pr = {0, run_styles[ri < run_styles.size () ? ri : run_styles.size () - 1]};
                    layer.paras.push_back (pr);
                    size_t nl = text.find ('\n', at);
                    if (nl == std::string::npos)
                    {
                        break;
                    }
                    at = nl + 1;
                }
                layer.paras = layer.para_runs ();
            }
        }
        /* Box text: the first shape's box bounds. */
        const char* shape_path[4] = {"EngineDict", "Rendered", "Shapes", "Children"};
        const EValue* children    = e_path (e, shape_path, 4);
        if (children != NULL && children->type == EType::Array && !children->items.empty ())
        {
            const EValue& sh = children->items[0];
            double type      = 0;
            e_num (e_get (sh, "ShapeType"), &type);
            const char* bb_path[3] = {"Cookie", "Photoshop", "BoxBounds"};
            std::vector<double> b  = num_array (e_path (sh, bb_path, 3));
            if (type == 1 && b.size () == 4 && b[2] > b[0] && b[3] > b[1])
            {
                layer.box         = true;
                layer.box_rect[0] = (float) b[0];
                layer.box_rect[1] = (float) b[1];
                layer.box_rect[2] = (float) (b[2] - b[0]);
                layer.box_rect[3] = (float) (b[3] - b[1]);
            }
        }
        *out = layer;
        return true;
    }

    /* ---- Writing ---- */

    EValue fill_color (const float c[3])
    {
        EValue f = e_dict ();
        e_set (&f, "Type", e_int (1));
        EValue v = e_array ();
        v.items.push_back (e_real (1.0));
        for (int i = 0; i < 3; i++)
        {
            v.items.push_back (e_real (c[i]));
        }
        e_set (&f, "Values", v);
        return f;
    }

    EValue style_sheet_data (const doc::CharStyle& s, size_t font, float k)
    {
        EValue d = e_dict ();
        e_set (&d, "Font", e_int ((int64_t) font));
        e_set (&d, "FontSize", e_real (s.size_pt / k));
        e_set (&d, "FauxBold", e_bool (s.faux_bold));
        e_set (&d, "FauxItalic", e_bool (s.faux_italic));
        e_set (&d, "AutoLeading", e_bool (s.leading_pt <= 0.0f));
        e_set (&d, "Leading", e_real (s.leading_pt > 0.0f ? s.leading_pt / k : 0.0));
        e_set (&d, "HorizontalScale", e_real (s.hscale));
        e_set (&d, "VerticalScale", e_real (s.vscale));
        e_set (&d, "Tracking", e_int ((int64_t) lroundf (s.tracking)));
        e_set (&d, "AutoKerning", e_bool (s.kerning));
        e_set (&d, "Kerning", e_int (0));
        e_set (&d, "BaselineShift", e_real (s.baseline_shift_pt / k));
        e_set (&d, "FontCaps", e_int (s.all_caps ? 2 : 0));
        e_set (&d, "Underline", e_bool (s.underline));
        e_set (&d, "Strikethrough", e_bool (s.strikethrough));
        e_set (&d, "Ligatures", e_bool (s.ligatures));
        e_set (&d, "DLigatures", e_bool (false));
        e_set (&d, "FillFlag", e_bool (true));
        e_set (&d, "FillColor", fill_color (s.color));
        return d;
    }

    EValue paragraph_properties (const doc::ParaStyle& p, float k)
    {
        const int just[7] = {0, 2, 1, 3, 5, 4, 6};
        EValue d          = e_dict ();
        e_set (&d, "Justification", e_int (just[(int) p.align]));
        e_set (&d, "FirstLineIndent", e_real (p.first_indent_pt / k));
        e_set (&d, "StartIndent", e_real (p.start_indent_pt / k));
        e_set (&d, "EndIndent", e_real (p.end_indent_pt / k));
        e_set (&d, "SpaceBefore", e_real (p.space_before_pt / k));
        e_set (&d, "SpaceAfter", e_real (p.space_after_pt / k));
        e_set (&d, "AutoHyphenate", e_bool (false));
        e_set (&d, "AutoLeading", e_real (p.auto_leading));
        e_set (&d, "LeadingType", e_int (0));
        return d;
    }

    EValue font_entry (const std::string& name)
    {
        EValue f = e_dict ();
        e_set (&f, "Name", e_string (name));
        e_set (&f, "Script", e_int (0));
        e_set (&f, "FontType", e_int (1));
        e_set (&f, "Synthetic", e_int (0));
        return f;
    }

    EValue real_pair (double a, double b)
    {
        EValue v = e_array ();
        v.items.push_back (e_real (a));
        v.items.push_back (e_real (b));
        return v;
    }

    /* EngineData for the layer, starting from the original's (unknown keys survive). */
    EValue build_engine_data (const doc::TextLayer& t, const EValue* tmpl, float dpi, text::FontDb* fonts)
    {
        float k  = 72.0f / (dpi > 0.0f ? dpi : 72.0f);
        EValue e = tmpl != NULL && tmpl->type == EType::Dict ? *tmpl : e_dict ();
        if (e_get (e, "EngineDict") == NULL || e_get (e, "EngineDict")->type != EType::Dict)
        {
            e_set (&e, "EngineDict", e_dict ());
        }
        if (e_get (e, "ResourceDict") == NULL)
        {
            e_set (&e, "ResourceDict", e_dict ());
        }
        std::vector<doc::TextRun> runs  = t.char_runs ();
        std::vector<doc::ParaRun> paras = t.para_runs ();
        std::string text                = t.text;
        std::replace (text.begin (), text.end (), '\n', '\r');
        std::string ed_text = text + "\r";

        /* Fonts: the original list, plus new PostScript names. */
        std::vector<std::string> fontset;
        const char* fs_path[2] = {"ResourceDict", "FontSet"};
        const EValue* fs       = e_path (e, fs_path, 2);
        if (fs != NULL && fs->type == EType::Array)
        {
            for (size_t i = 0; i < fs->items.size (); i++)
            {
                const EValue* n = e_get (fs->items[i], "Name");
                fontset.push_back (n != NULL ? n->str : std::string ());
            }
        }
        EValue sruns = e_array (), slens = e_array ();
        for (size_t i = 0; i < runs.size (); i++)
        {
            const doc::CharStyle& st = runs[i].style;
            std::string ps           = !st.postscript.empty () ? st.postscript
                                     : fonts != NULL           ? fonts->postscript_for (st.family, st.style)
                                                               : st.family + "-" + st.style;
            size_t fi                = std::find (fontset.begin (), fontset.end (), ps) - fontset.begin ();
            if (fi == fontset.size ())
            {
                fontset.push_back (ps);
            }
            EValue sheet = e_dict (), run = e_dict ();
            e_set (&sheet, "StyleSheetData", style_sheet_data (st, fi, k));
            e_set (&run, "StyleSheet", sheet);
            sruns.items.push_back (run);
        }
        /* Run lengths in UTF-16 units; the last run also covers the closing \r. */
        size_t at = 0;
        for (size_t i = 0; i < runs.size (); i++)
        {
            size_t len = utf16_len (t.text.substr (at, runs[i].len));
            at += runs[i].len;
            if (i + 1 == runs.size ())
            {
                len++;
            }
            slens.items.push_back (e_int ((int64_t) len));
        }
        EValue pruns = e_array (), plens = e_array ();
        at = 0;
        for (size_t i = 0; i < paras.size (); i++)
        {
            size_t len = utf16_len (t.text.substr (at, paras[i].len));
            at += paras[i].len;
            if (i + 1 == paras.size ())
            {
                len++;
            }
            EValue sheet = e_dict (), run = e_dict ();
            e_set (&sheet, "DefaultStyleSheet", e_int (0));
            e_set (&sheet, "Properties", paragraph_properties (paras[i].style, k));
            e_set (&run, "ParagraphSheet", sheet);
            pruns.items.push_back (run);
            plens.items.push_back (e_int ((int64_t) len));
        }
        EValue* dict  = e_get_mut (&e, "EngineDict");
        EValue editor = e_get (*dict, "Editor") != NULL ? *e_get (*dict, "Editor") : e_dict ();
        e_set (&editor, "Text", e_string (ed_text));
        e_set (dict, "Editor", editor);
        EValue pr = e_get (*dict, "ParagraphRun") != NULL ? *e_get (*dict, "ParagraphRun") : e_dict ();
        e_set (&pr, "RunArray", pruns);
        e_set (&pr, "RunLengthArray", plens);
        if (e_get (pr, "IsJoinable") == NULL)
        {
            e_set (&pr, "IsJoinable", e_int (1));
        }
        e_set (dict, "ParagraphRun", pr);
        EValue sr = e_get (*dict, "StyleRun") != NULL ? *e_get (*dict, "StyleRun") : e_dict ();
        e_set (&sr, "RunArray", sruns);
        e_set (&sr, "RunLengthArray", slens);
        if (e_get (sr, "IsJoinable") == NULL)
        {
            e_set (&sr, "IsJoinable", e_int (2));
        }
        e_set (dict, "StyleRun", sr);
        const int aa[5] = {0, 4, 1, 2, 3};
        e_set (dict, "AntiAlias", e_int (aa[(int) t.antialias]));
        if (e_get (*dict, "UseFractionalGlyphWidths") == NULL)
        {
            e_set (dict, "UseFractionalGlyphWidths", e_bool (true));
        }
        /* The shape: point or box. */
        int shape_type = t.box ? 1 : 0;
        EValue ps      = e_dict ();
        e_set (&ps, "ShapeType", e_int (shape_type));
        if (t.box)
        {
            EValue bb = e_array ();
            bb.items.push_back (e_real (t.box_rect[0]));
            bb.items.push_back (e_real (t.box_rect[1]));
            bb.items.push_back (e_real (t.box_rect[0] + t.box_rect[2]));
            bb.items.push_back (e_real (t.box_rect[1] + t.box_rect[3]));
            e_set (&ps, "BoxBounds", bb);
        }
        else
        {
            e_set (&ps, "PointBase", real_pair (0, 0));
        }
        EValue base = e_dict ();
        e_set (&base, "ShapeType", e_int (shape_type));
        e_set (&base, "TransformPoint0", real_pair (1, 0));
        e_set (&base, "TransformPoint1", real_pair (0, 1));
        e_set (&base, "TransformPoint2", real_pair (0, 0));
        e_set (&ps, "Base", base);
        EValue cookie = e_dict (), lines = e_dict (), child = e_dict (), shapes = e_dict (), rendered = e_dict (),
               kids = e_array ();
        e_set (&cookie, "Photoshop", ps);
        e_set (&lines, "WritingDirection", e_int (0));
        e_set (&lines, "Children", e_array ());
        e_set (&child, "ShapeType", e_int (shape_type));
        e_set (&child, "Procession", e_int (0));
        e_set (&child, "Lines", lines);
        e_set (&child, "Cookie", cookie);
        kids.items.push_back (child);
        e_set (&shapes, "WritingDirection", e_int (0));
        e_set (&shapes, "Children", kids);
        e_set (&rendered, "Version", e_int (1));
        e_set (&rendered, "Shapes", shapes);
        e_set (dict, "Rendered", rendered);
        /* Resources: the font list, and default sheets when the original had none. */
        EValue fontlist = e_array ();
        for (size_t i = 0; i < fontset.size (); i++)
        {
            fontlist.items.push_back (font_entry (fontset[i]));
        }
        const char* res_keys[2] = {"ResourceDict", "DocumentResources"};
        for (int r = 0; r < 2; r++)
        {
            if (r == 1 && e_get (e, res_keys[1]) == NULL)
            {
                e_set (&e, res_keys[1], *e_get (e, res_keys[0]));
                continue;
            }
            EValue* rd = e_get_mut (&e, res_keys[r]);
            e_set (rd, "FontSet", fontlist);
            if (e_get (*rd, "StyleSheetSet") == NULL)
            {
                EValue set = e_array (), sheet = e_dict ();
                e_set (&sheet, "Name", e_string ("Normal RGB"));
                e_set (&sheet, "StyleSheetData", style_sheet_data (runs[0].style, 0, k));
                set.items.push_back (sheet);
                e_set (rd, "StyleSheetSet", set);
            }
            if (e_get (*rd, "ParagraphSheetSet") == NULL)
            {
                EValue set = e_array (), sheet = e_dict ();
                e_set (&sheet, "Name", e_string ("Normal RGB"));
                e_set (&sheet, "DefaultStyleSheet", e_int (0));
                e_set (&sheet, "Properties", paragraph_properties (doc::ParaStyle::make (), k));
                set.items.push_back (sheet);
                e_set (rd, "ParagraphSheetSet", set);
            }
            if (e_get (*rd, "TheNormalStyleSheet") == NULL)
            {
                e_set (rd, "TheNormalStyleSheet", e_int (0));
            }
            if (e_get (*rd, "TheNormalParagraphSheet") == NULL)
            {
                e_set (rd, "TheNormalParagraphSheet", e_int (0));
            }
        }
        return e;
    }

    DValue rect_value (const char* cls, const float r[4])
    {
        DValue d = d_object (cls);
        d_set (&d, "Left", d_unit ("#Pnt", r[0]));
        d_set (&d, "Top ", d_unit ("#Pnt", r[1]));
        d_set (&d, "Rght", d_unit ("#Pnt", r[2]));
        d_set (&d, "Btom", d_unit ("#Pnt", r[3]));
        return d;
    }

    DValue default_warp ()
    {
        DValue w = d_object ("warp");
        d_set (&w, "warpStyle", d_enum ("warpStyle", "warpNone"));
        d_set (&w, "warpValue", d_double (0));
        d_set (&w, "warpPerspective", d_double (0));
        d_set (&w, "warpPerspectiveOther", d_double (0));
        d_set (&w, "warpRotate", d_enum ("Ornt", "Hrzn"));
        return w;
    }

    std::vector<uint8_t> text_to_tysh (
        const doc::TextLayer& t, const std::vector<uint8_t>* old, float dpi, text::FontDb* fonts)
    {
        TySh o;
        bool have_old = old != NULL && parse_tysh (old->data (), old->size (), &o);
        EValue tmpl;
        bool have_tmpl = false;
        if (have_old)
        {
            const DValue* raw = d_get (o.text, "EngineData");
            have_tmpl         = raw != NULL && raw->type == DType::Raw
                     && parse_engine_data ((const uint8_t*) raw->str.data (), raw->str.size (), &tmpl);
        }
        EValue e         = build_engine_data (t, have_tmpl ? &tmpl : NULL, dpi, fonts);
        DValue text      = have_old ? o.text : d_object ("TxLr");
        std::string body = t.text;
        std::replace (body.begin (), body.end (), '\n', '\r');
        d_set (&text, "Txt ", d_text (body));
        if (d_get (text, "textGridding") == NULL)
        {
            d_set (&text, "textGridding", d_enum ("textGridding", "None"));
        }
        d_set (&text, "Ornt", d_enum ("Ornt", "Hrzn"));
        const char* aa[5] = {"Anno", "antiAliasSharp", "AnCr", "AnSt", "AnSm"};
        d_set (&text, "AntA", d_enum ("Annt", aa[(int) t.antialias]));
        /* Bounds in text space: the box, or the laid-out lines. */
        float b[4] = {0, 0, 0, 0};
        if (t.box)
        {
            b[0] = t.box_rect[0];
            b[1] = t.box_rect[1];
            b[2] = t.box_rect[0] + t.box_rect[2];
            b[3] = t.box_rect[1] + t.box_rect[3];
        }
        else if (fonts != NULL)
        {
            text::Layout lay;
            text::layout_text (t, fonts, dpi, &lay);
            memcpy (b, lay.bounds, sizeof b);
        }
        d_set (&text, "bounds", rect_value ("bounds", b));
        d_set (&text, "boundingBox", rect_value ("boundingBox", b));
        if (d_get (text, "TextIndex") == NULL)
        {
            d_set (&text, "TextIndex", d_long (0));
        }
        std::vector<uint8_t> ed = write_engine_data (e);
        d_set (&text, "EngineData", d_raw ("tdta", std::string ((const char*) ed.data (), ed.size ())));
        std::vector<uint8_t> out;
        wr_u16 (&out, 1);
        for (int i = 0; i < 6; i++)
        {
            wr_f64 (&out, t.transform[i]);
        }
        wr_u16 (&out, 50);
        wr_u32 (&out, 16);
        write_descriptor (&out, text);
        wr_u16 (&out, 1);
        wr_u32 (&out, 16);
        write_descriptor (&out, have_old && o.has_warp ? o.warp : default_warp ());
        for (int i = 0; i < 4; i++)
        {
            float v = have_old ? o.bounds[i] : 0.0f;
            uint32_t u;
            memcpy (&u, &v, sizeof u);
            wr_u32 (&out, u);
        }
        return out;
    }

} // namespace psd
