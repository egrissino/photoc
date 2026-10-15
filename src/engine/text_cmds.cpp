#include "engine/layers.h"
#include "engine/session.h"
#include "engine/text.h"
#include "engine/vector.h"
#include "text/fonts.h"
#include "text/render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace engine
{

    /* Type: creating and editing type layers, their character and paragraph styles, the font
       list, and Rasterize Type. A type layer keeps its model (doc::TextLayer) and its rendering as
       pixels, redrawn after every change. */

    const char* const ALIGN_IDS[7] = {
        "left", "center", "right", "justifyLeft", "justifyCenter", "justifyRight", "justifyAll"};
    const char* const AA_IDS[5] = {"none", "sharp", "crisp", "strong", "smooth"};

    int id_index (const std::string& s, const char* const* ids, int n)
    {
        for (int i = 0; i < n; i++)
        {
            if (s == ids[i])
            {
                return i;
            }
        }
        return -1;
    }

    void hex3 (const float c[3], char out[8])
    {
        snprintf (out, 8, "#%02x%02x%02x", (int) lroundf (fminf (fmaxf (c[0], 0.0f), 1.0f) * 255),
            (int) lroundf (fminf (fmaxf (c[1], 0.0f), 1.0f) * 255),
            (int) lroundf (fminf (fmaxf (c[2], 0.0f), 1.0f) * 255));
    }

    Value char_style_json (const doc::CharStyle& s)
    {
        char c[8];
        hex3 (s.color, c);
        return Value{{"font", s.family}, {"style", s.style}, {"postscript", s.postscript}, {"size", s.size_pt},
            {"color", c}, {"tracking", s.tracking}, {"leading", s.leading_pt}, {"baselineShift", s.baseline_shift_pt},
            {"horizontalScale", s.hscale * 100.0f}, {"verticalScale", s.vscale * 100.0f}, {"underline", s.underline},
            {"strikethrough", s.strikethrough}, {"fauxBold", s.faux_bold}, {"fauxItalic", s.faux_italic},
            {"allCaps", s.all_caps}, {"kerning", s.kerning}, {"ligatures", s.ligatures}};
    }

    bool num_in (
        const Value& v, const char* key, float lo, float hi, float scale, float* out, const char* cmd, Error* err)
    {
        if (!param_present (v, key))
        {
            return true;
        }
        double d;
        if (!param_f64 (v, key, &d) || !isfinite (d) || d < lo || d > hi)
        {
            return bad_params (err, cmd, "`%s` must be a number in %g..%g", key, lo, hi);
        }
        *out = (float) d * scale;
        return true;
    }

    bool flag_in (const Value& v, const char* key, bool* out, const char* cmd, Error* err)
    {
        if (!param_present (v, key))
        {
            return true;
        }
        if (!param_bool (v, key, out))
        {
            return bad_params (err, cmd, "`%s` must be true or false", key);
        }
        return true;
    }

    /* The character style settings present in v applied to s. */
    bool char_style_from (const Value& v, doc::CharStyle* s, const char* cmd, Error* err)
    {
        /* Picking a font drops the PostScript name a PSD gave the run. */
        if (param_str (v, "font", &s->family) | param_str (v, "style", &s->style))
        {
            s->postscript.clear ();
        }
        param_str (v, "postscript", &s->postscript);
        if (param_present (v, "color"))
        {
            float c[4] = {s->color[0], s->color[1], s->color[2], 1.0f}, none[4] = {-1, -1, -1, -1};
            color_param (v, "color", none, c);
            if (c[0] < 0.0f)
            {
                return bad_params (err, cmd, "`color` must be \"#rrggbb\" or [r, g, b]");
            }
            memcpy (s->color, c, sizeof s->color);
        }
        return num_in (v, "size", 0.01f, 1296.0f, 1.0f, &s->size_pt, cmd, err)
            && num_in (v, "tracking", -1000, 10000, 1.0f, &s->tracking, cmd, err)
            && num_in (v, "leading", 0, 5000, 1.0f, &s->leading_pt, cmd, err)
            && num_in (v, "baselineShift", -5000, 5000, 1.0f, &s->baseline_shift_pt, cmd, err)
            && num_in (v, "horizontalScale", 1, 1000, 0.01f, &s->hscale, cmd, err)
            && num_in (v, "verticalScale", 1, 1000, 0.01f, &s->vscale, cmd, err)
            && flag_in (v, "underline", &s->underline, cmd, err)
            && flag_in (v, "strikethrough", &s->strikethrough, cmd, err)
            && flag_in (v, "fauxBold", &s->faux_bold, cmd, err) && flag_in (v, "fauxItalic", &s->faux_italic, cmd, err)
            && flag_in (v, "allCaps", &s->all_caps, cmd, err) && flag_in (v, "kerning", &s->kerning, cmd, err)
            && flag_in (v, "ligatures", &s->ligatures, cmd, err);
    }

    Value para_style_json (const doc::ParaStyle& p)
    {
        return Value{{"align", ALIGN_IDS[(int) p.align]}, {"firstIndent", p.first_indent_pt},
            {"startIndent", p.start_indent_pt}, {"endIndent", p.end_indent_pt}, {"spaceBefore", p.space_before_pt},
            {"spaceAfter", p.space_after_pt}, {"autoLeading", p.auto_leading * 100.0f}};
    }

    bool para_style_from (const Value& v, doc::ParaStyle* p, const char* cmd, Error* err)
    {
        std::string a;
        if (param_str (v, "align", &a))
        {
            int i = id_index (a, ALIGN_IDS, 7);
            if (i < 0)
            {
                return bad_params (err, cmd,
                    "`align` must be left, center, right, justifyLeft, justifyCenter, justifyRight or justifyAll");
            }
            p->align = (doc::TextAlign) i;
        }
        return num_in (v, "firstIndent", -1296, 1296, 1.0f, &p->first_indent_pt, cmd, err)
            && num_in (v, "startIndent", 0, 1296, 1.0f, &p->start_indent_pt, cmd, err)
            && num_in (v, "endIndent", 0, 1296, 1.0f, &p->end_indent_pt, cmd, err)
            && num_in (v, "spaceBefore", 0, 1296, 1.0f, &p->space_before_pt, cmd, err)
            && num_in (v, "spaceAfter", 0, 1296, 1.0f, &p->space_after_pt, cmd, err)
            && num_in (v, "autoLeading", 0, 500, 0.01f, &p->auto_leading, cmd, err);
    }

    Value text_json (const doc::TextLayer& t)
    {
        Value runs = Value::array (), paras = Value::array ();
        std::vector<doc::TextRun> cr = t.char_runs ();
        for (size_t i = 0; i < cr.size (); i++)
        {
            Value r     = char_style_json (cr[i].style);
            r["length"] = cr[i].len;
            runs.push_back (r);
        }
        std::vector<doc::ParaRun> pr = t.para_runs ();
        for (size_t i = 0; i < pr.size (); i++)
        {
            paras.push_back (para_style_json (pr[i].style));
        }
        Value v = {{"text", t.text}, {"runs", runs}, {"paragraphs", paras}, {"antialias", AA_IDS[(int) t.antialias]}};
        v["transform"] = Value::array ();
        for (int i = 0; i < 6; i++)
        {
            v["transform"].push_back (t.transform[i]);
        }
        if (t.box)
        {
            v["box"] = {t.box_rect[0], t.box_rect[1], t.box_rect[2], t.box_rect[3]};
        }
        return v;
    }

    /* A text model from JSON: "text", "runs" ([{length, …char style}]), "paragraphs" ([…para
       style], one per paragraph), "box" ([x, y, w, h], absent for point text), "transform"
       ([a, b, c, d, e, f]) and "antialias". Missing parts take defaults. */
    bool text_from_json (const Value& v, doc::TextLayer* t, const char* cmd, Error* err)
    {
        *t = doc::TextLayer::make ();
        if (!v.is_object ())
        {
            return bad_params (err, cmd, "the text must be an object");
        }
        param_str (v, "text", &t->text);
        const Value* runs = param (v, "runs");
        if (runs != NULL && runs->is_array () && !runs->empty ())
        {
            t->runs.clear ();
            for (size_t i = 0; i < runs->size (); i++)
            {
                doc::TextRun r = {0, doc::CharStyle::make ()};
                int64_t len    = 0;
                param_i64 ((*runs)[i], "length", &len);
                r.len = len < 0 ? 0 : (size_t) len;
                if (!char_style_from ((*runs)[i], &r.style, cmd, err))
                {
                    return false;
                }
                t->runs.push_back (r);
            }
        }
        const Value* paras = param (v, "paragraphs");
        if (paras != NULL && paras->is_array () && !paras->empty ())
        {
            t->paras.clear ();
            for (size_t i = 0; i < paras->size (); i++)
            {
                doc::ParaRun p = {0, doc::ParaStyle::make ()};
                if (!para_style_from ((*paras)[i], &p.style, cmd, err))
                {
                    return false;
                }
                t->paras.push_back (p);
            }
        }
        t->paras         = t->para_runs ();
        const Value* box = param (v, "box");
        if (box != NULL && box->is_array () && box->size () == 4)
        {
            t->box = true;
            for (int i = 0; i < 4; i++)
            {
                t->box_rect[i] = (*box)[(size_t) i].is_number () ? (*box)[(size_t) i].get<float> () : 0.0f;
            }
            if (t->box_rect[2] < 1.0f || t->box_rect[3] < 1.0f)
            {
                return bad_params (err, cmd, "a text box needs a width and height of at least 1 px");
            }
        }
        const Value* m = param (v, "transform");
        if (m != NULL && m->is_array () && m->size () == 6)
        {
            for (int i = 0; i < 6; i++)
            {
                t->transform[i] = (*m)[(size_t) i].is_number () ? (*m)[(size_t) i].get<double> () : 0.0;
            }
        }
        std::string aa;
        if (param_str (v, "antialias", &aa))
        {
            int i = id_index (aa, AA_IDS, 5);
            if (i < 0)
            {
                return bad_params (err, cmd, "`antialias` must be none, sharp, crisp, strong or smooth");
            }
            t->antialias = (doc::AntiAlias) i;
        }
        return true;
    }

    /* Photoshop names a type layer after its text (the first line, cut). */
    std::string type_layer_name (const std::string& text)
    {
        std::string name = text.substr (0, text.find ('\n'));
        if (name.size () > 60)
        {
            size_t cut = 60;
            while (cut > 0 && ((uint8_t) name[cut] & 0xC0) == 0x80)
            {
                cut--;
            }
            name = name.substr (0, cut);
        }
        return name.empty () ? std::string ("Layer") : name;
    }

    void render_type (Session& s, const doc::Document& d, doc::Layer* l)
    {
        text::render_layer_text (l->text, s.fonts (), d.resolution_dpi, d.pixel_format (), s.cmyk, &l->pixels);
    }

    void render_all_type (Session& s, doc::Document* d)
    {
        std::vector<doc::WalkEntry> walk = d->walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (walk[i].layer->kind != doc::LayerKind::Text)
            {
                continue;
            }
            doc::Layer* l = d->layer_mut (walk[i].layer->id);
            if (l != NULL)
            {
                render_type (s, *d, l);
            }
        }
    }

    /* After a change of the whole document mapped by m (x' = a x + c y + e, y' = b x + d y + f):
       guides follow when the map keeps them straight (scales, shifts, quarter turns; other angles
       drop them), and every type layer's placement is mapped and its rendering redone (in the
       document's current format). */
    void follow_document_transform (Session& s, doc::Document* d, const double m[6])
    {
        bool straight = m[1] == 0.0 && m[2] == 0.0, turned = m[0] == 0.0 && m[3] == 0.0;
        std::vector<doc::Guide> guides;
        for (size_t i = 0; i < d->guides.size () && (straight || turned); i++)
        {
            doc::Guide g = d->guides[i];
            double p     = g.position;
            if (straight)
            {
                g.position = g.vertical ? m[0] * p + m[4] : m[3] * p + m[5];
            }
            else if (g.vertical)
            {
                g = doc::Guide{false, m[1] * p + m[5]};
            }
            else
            {
                g = doc::Guide{true, m[2] * p + m[4]};
            }
            guides.push_back (g);
        }
        d->guides                        = guides;
        std::vector<doc::WalkEntry> walk = d->walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (walk[i].layer->kind != doc::LayerKind::Text)
            {
                continue;
            }
            doc::Layer* l = d->layer_mut (walk[i].layer->id);
            double t[6];
            memcpy (t, l->text.transform, sizeof t);
            double r[6] = {m[0] * t[0] + m[2] * t[1], m[1] * t[0] + m[3] * t[1], m[0] * t[2] + m[2] * t[3],
                m[1] * t[2] + m[3] * t[3], m[0] * t[4] + m[2] * t[5] + m[4], m[1] * t[4] + m[3] * t[5] + m[5]};
            memcpy (l->text.transform, r, sizeof r);
            render_type (s, *d, l);
        }
        follow_vectors (s, d, m);
    }

    bool type_layer (Session& s, const Value& p, doc::LayerId* id, Error* err)
    {
        if (!layer_param (s, p, id, err))
        {
            return false;
        }
        const doc::Layer* l = s.active ()->doc->layer (*id);
        if (l == NULL)
        {
            return no_layer (err, *id);
        }
        if (l->kind != doc::LayerKind::Text)
        {
            return fail (err, ErrorKind::Other, "\"%s\" is not a type layer", l->name.c_str ());
        }
        return true;
    }

    /* A new type layer: point text with its first baseline at (x, y), or box text in "box"
       ([x, y, w, h] in document pixels). Style from the params, else the session's colour and the
       default font. */
    bool cmd_type_create (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd  = "type.create";
        doc::TextLayer t = doc::TextLayer::make ();
        param_str (p, "text", &t.text);
        doc::CharStyle st = doc::CharStyle::make ();
        memcpy (st.color, s.tools.foreground, sizeof st.color);
        st.family = s.fonts ()->default_family ();
        if (!char_style_from (p, &st, cmd, err))
        {
            return false;
        }
        doc::ParaStyle ps = doc::ParaStyle::make ();
        if (!para_style_from (p, &ps, cmd, err))
        {
            return false;
        }
        t.runs[0].style  = st;
        t.runs[0].len    = t.text.size ();
        t.paras[0].style = ps;
        t.paras          = t.para_runs ();
        std::string aa;
        if (param_str (p, "antialias", &aa))
        {
            int i = id_index (aa, AA_IDS, 5);
            if (i < 0)
            {
                return bad_params (err, cmd, "`antialias` must be none, sharp, crisp, strong or smooth");
            }
            t.antialias = (doc::AntiAlias) i;
        }
        const Value* box = param (p, "box");
        if (box != NULL)
        {
            if (!box->is_array () || box->size () != 4)
            {
                return bad_params (err, cmd, "`box` must be [x, y, width, height]");
            }
            float b[4];
            for (int i = 0; i < 4; i++)
            {
                b[i] = (*box)[(size_t) i].is_number () ? (*box)[(size_t) i].get<float> () : 0.0f;
            }
            if (b[2] < 1.0f || b[3] < 1.0f)
            {
                return bad_params (err, cmd, "a text box needs a width and height of at least 1 px");
            }
            /* The box lives in text space, the transform places it. */
            t.box          = true;
            t.box_rect[0]  = 0.0f;
            t.box_rect[1]  = 0.0f;
            t.box_rect[2]  = b[2];
            t.box_rect[3]  = b[3];
            t.transform[4] = b[0];
            t.transform[5] = b[1];
        }
        else
        {
            double x = 0, y = 0;
            param_f64 (p, "x", &x);
            param_f64 (p, "y", &y);
            t.transform[4] = x;
            t.transform[5] = y;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::Layer l = doc::Layer::make (s.ids, type_layer_name (t.text), doc::LayerKind::Text);
        l.text       = t;
        render_type (s, *d, &l);
        doc::LayerId at = *e.active_layer () != doc::NO_LAYER ? *e.active_layer () : d->top_layer ();
        doc::LayerId id = d->insert_above (at, l);
        e.select (std::vector<doc::LayerId> (1, id), id);
        if (!e.commit ("Type Layer", err))
        {
            return false;
        }
        *result = Value{{"layer", id}};
        return true;
    }

    /* The whole text model at once (the Type tool's commit). The layer keeps following its text in
       its name unless it was renamed. */
    bool cmd_type_edit (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "type.edit";
        doc::LayerId id;
        if (!type_layer (s, p, &id, err))
        {
            return false;
        }
        const Value* v = param (p, "text");
        if (v == NULL)
        {
            return bad_params (err, cmd, "missing `text` (the text model)");
        }
        doc::TextLayer t;
        if (!text_from_json (*v, &t, cmd, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        if (l->name == type_layer_name (l->text.text))
        {
            l->name = type_layer_name (t.text);
        }
        l->text = t;
        render_type (s, *e.doc (), l);
        return e.commit ("Edit Type Layer", err);
    }

    /* Character style settings over [start, end) (default: the whole text). */
    bool cmd_type_set_style (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "type.setStyle";
        doc::LayerId id;
        if (!type_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l     = e.doc ()->layer_mut (id);
        doc::TextLayer& t = l->text;
        int64_t a = 0, b = (int64_t) t.text.size ();
        param_i64 (p, "start", &a);
        param_i64 (p, "end", &b);
        if (a < 0 || b < a || (size_t) b > t.text.size ())
        {
            return bad_params (err, cmd, "`start` / `end` must be a byte range of the text");
        }
        /* Each run in the range keeps what the params don't set. */
        std::vector<doc::TextRun> runs = t.char_runs ();
        size_t at                      = 0;
        std::vector<std::pair<std::pair<size_t, size_t>, doc::CharStyle>> changes;
        for (size_t i = 0; i < runs.size (); i++)
        {
            size_t ra = at, rb = at + runs[i].len;
            at        = rb;
            size_t x0 = ra > (size_t) a ? ra : (size_t) a, x1 = rb < (size_t) b ? rb : (size_t) b;
            if (x0 >= x1 && !(t.text.empty () || a == b))
            {
                continue;
            }
            doc::CharStyle st = runs[i].style;
            if (!char_style_from (p, &st, cmd, err))
            {
                return false;
            }
            changes.push_back (std::make_pair (std::make_pair (x0, x1), st));
            if (t.text.empty () || a == b)
            {
                break;
            }
        }
        for (size_t i = 0; i < changes.size (); i++)
        {
            doc::text_set_style (&t, changes[i].first.first, changes[i].first.second, changes[i].second);
        }
        std::string aa;
        if (param_str (p, "antialias", &aa))
        {
            int i = id_index (aa, AA_IDS, 5);
            if (i < 0)
            {
                return bad_params (err, cmd, "`antialias` must be none, sharp, crisp, strong or smooth");
            }
            t.antialias = (doc::AntiAlias) i;
        }
        render_type (s, *e.doc (), l);
        return e.commit ("Character", err);
    }

    /* Paragraph settings for the paragraphs touching [start, end] (default: all). */
    bool cmd_type_set_paragraph (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "type.setParagraph";
        doc::LayerId id;
        if (!type_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l     = e.doc ()->layer_mut (id);
        doc::TextLayer& t = l->text;
        int64_t a = 0, b = (int64_t) t.text.size ();
        param_i64 (p, "start", &a);
        param_i64 (p, "end", &b);
        if (a < 0 || b < a || (size_t) b > t.text.size ())
        {
            return bad_params (err, cmd, "`start` / `end` must be a byte range of the text");
        }
        std::vector<doc::ParaRun> paras = t.para_runs ();
        size_t pstart                   = 0;
        for (size_t i = 0; i < paras.size (); i++)
        {
            size_t pend = pstart + paras[i].len;
            bool touches =
                (pstart <= (size_t) b && (pend > (size_t) a || (i + 1 == paras.size () && pend >= (size_t) a)));
            if (touches && !para_style_from (p, &paras[i].style, cmd, err))
            {
                return false;
            }
            pstart = pend;
        }
        t.paras = paras;
        render_type (s, *e.doc (), l);
        return e.commit ("Paragraph", err);
    }

    bool cmd_type_get (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!type_layer (s, p, &id, err))
        {
            return false;
        }
        *result = text_json (s.active ()->doc->layer (id)->text);
        return true;
    }

    /* Font families (and with "family", that family's styles). */
    bool cmd_type_fonts (Session& s, const Value& p, Value* result, Error* err)
    {
        std::string fam;
        text::FontDb* db               = s.fonts ();
        Value list                     = Value::array ();
        std::vector<std::string> names = param_str (p, "family", &fam) ? db->styles (fam) : db->families ();
        for (size_t i = 0; i < names.size (); i++)
        {
            list.push_back (names[i]);
        }
        *result =
            Value{{param_present (p, "family") ? "styles" : "families", list}, {"default", db->default_family ()}};
        return true;
    }

    bool cmd_rasterize_type (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!type_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        l->kind       = doc::LayerKind::Raster;
        l->text       = doc::TextLayer::make ();
        return e.commit ("Rasterize Type", err);
    }

    bool has_type_layer (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l != NULL && l->kind == doc::LayerKind::Text)
        {
            return true;
        }
        *why = "the active layer is not a type layer";
        return false;
    }

    const CommandSpec TEXT_COMMANDS[] = {
        {"type.create", "Type Layer", "", NULL,
            "{\"x\":px,\"y\":px (first baseline) | "
            "\"box\":[x,y,w,h],\"text\":str=\"\",\"font\":family,\"style\":str,\"size\":pt=12,\"color\":\"#rrggbb\"="
            "foreground,"
            "\"align\":\"left|center|right|justify…\",\"antialias\":\"none|sharp|crisp|strong|smooth\"=\"smooth\","
            "…character style}",
            has_doc, cmd_type_create, true},
        {"type.edit", "Edit Type Layer", "", NULL,
            "{\"layer\":id?,\"text\":{text, runs:[{length,…}], paragraphs:[…], box?, transform, antialias}}", has_layer,
            cmd_type_edit, true},
        {"type.setStyle", "Character", "", NULL,
            "{\"layer\":id?,\"start\":byte?,\"end\":byte?,\"font\":str,\"style\":str,\"size\":pt,\"color\":\"#rrggbb\","
            "\"tracking\":1/1000 em,\"leading\":pt (0 auto),"
            "\"baselineShift\":pt,\"horizontalScale\":%,\"verticalScale\":%,\"underline\":bool,\"strikethrough\":bool,"
            "\"fauxBold\":bool,\"fauxItalic\":bool,"
            "\"allCaps\":bool,\"kerning\":bool,\"ligatures\":bool,\"antialias\":str}",
            has_type_layer, cmd_type_set_style, true},
        {"type.setParagraph", "Paragraph", "", NULL,
            "{\"layer\":id?,\"start\":byte?,\"end\":byte?,\"align\":\"left|center|right|justifyLeft|justifyCenter|"
            "justifyRight|justifyAll\",\"firstIndent\":pt,"
            "\"startIndent\":pt,\"endIndent\":pt,\"spaceBefore\":pt,\"spaceAfter\":pt,\"autoLeading\":%}",
            has_type_layer, cmd_type_set_paragraph, true},
        {"type.get", "Type", "", NULL, "{\"layer\":id?} → the text model", has_type_layer, cmd_type_get, false},
        {"type.fonts", "Fonts", "", NULL, "{\"family\":str?} → {families | styles, default}", always, cmd_type_fonts,
            false},
        {"layer.rasterize.type", "Type", "Layer/Rasterize", NULL, "{\"layer\":id?}", has_type_layer, cmd_rasterize_type,
            true},
    };

    void register_text_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), TEXT_COMMANDS, TEXT_COMMANDS + sizeof TEXT_COMMANDS / sizeof TEXT_COMMANDS[0]);
    }

} // namespace engine
