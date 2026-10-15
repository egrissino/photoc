#include "engine/layers.h"
#include "engine/params.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace engine
{

    /* Layer > New Fill Layer (Solid Color, Gradient, Pattern), editing a fill layer, Blend If, and
       Layer > New > Background from Layer. */

    /* A colour param: "#rrggbb", "foreground" or "background". */
    bool fill_color (const Session& s, const Value& v, float out[3])
    {
        if (!v.is_string ())
        {
            return false;
        }
        std::string t = v.get<std::string> ();
        if (t == "foreground" || t == "background")
        {
            memcpy (out, t == "foreground" ? s.tools.foreground : s.tools.background, 3 * sizeof (float));
            return true;
        }
        float c[4];
        if (!parse_hex (t.c_str (), c))
        {
            return false;
        }
        memcpy (out, c, 3 * sizeof (float));
        return true;
    }

    bool fill_flag (const Value& p, const char* key, bool* out)
    {
        return param_bool (p, key, out);
    }

    bool fill_number (const Value& p, const char* key, float* out)
    {
        double v;
        if (!param_f64 (p, key, &v))
        {
            return false;
        }
        *out = (float) v;
        return true;
    }

    /* The fill settings in `p` laid over `f` (keys left out keep their value). `kind` is the fill's
       kind ("type" in p may change it). */
    bool parse_fill (const Session& s, const Value& p, const char* cmd, doc::FillLayer* f, Error* err)
    {
        std::string type;
        if (param_str (p, "type", &type))
        {
            if (type == "solid" || type == "solidColor")
            {
                f->kind = doc::FillKind::Solid;
            }
            else if (type == "gradient")
            {
                f->kind = doc::FillKind::Gradient;
            }
            else if (type == "pattern")
            {
                f->kind = doc::FillKind::Pattern;
            }
            else
            {
                return bad_params (err, cmd, "type must be solid, gradient or pattern");
            }
        }
        const Value* c = param (p, "color");
        if (c != NULL && !fill_color (s, *c, f->color))
        {
            return bad_params (err, cmd, "`color` must be \"#rrggbb\", \"foreground\" or \"background\"");
        }
        const Value* stops = param (p, "stops");
        if (stops != NULL)
        {
            if (!stops->is_array () || stops->empty ())
            {
                return bad_params (err, cmd, "`stops` is [[location 0..1, \"#rrggbb\"], …]");
            }
            std::vector<doc::GradientStop> sorted;
            for (size_t i = 0; i < stops->size (); i++)
            {
                const Value& st = (*stops)[i];
                doc::GradientStop g;
                if (!st.is_array () || st.size () != 2 || !st[0].is_number () || !fill_color (s, st[1], g.color))
                {
                    return bad_params (
                        err, cmd, "a stop is [location 0..1, \"#rrggbb\"|\"foreground\"|\"background\"]");
                }
                g.location = fminf (fmaxf (st[0].get<float> (), 0.0f), 1.0f);
                sorted.push_back (g);
            }
            /* By location; equal locations keep their order. */
            for (size_t a = 1; a < sorted.size (); a++)
            {
                for (size_t b = a; b > 0 && sorted[b].location < sorted[b - 1].location; b--)
                {
                    std::swap (sorted[b], sorted[b - 1]);
                }
            }
            f->stops = sorted;
        }
        const Value* tr = param (p, "transparency");
        if (tr != NULL)
        {
            if (!tr->is_array ())
            {
                return bad_params (err, cmd, "`transparency` is [[location 0..1, opacity 0..100], …]");
            }
            f->opacity_stops.clear ();
            for (size_t i = 0; i < tr->size (); i++)
            {
                const Value& o = (*tr)[i];
                if (!o.is_array () || o.size () != 2 || !o[0].is_number () || !o[1].is_number ())
                {
                    return bad_params (err, cmd, "an opacity stop is [location, opacity %%]");
                }
                doc::CurvePoint cp = {fminf (fmaxf (o[0].get<float> (), 0.0f), 1.0f),
                    fminf (fmaxf (o[1].get<float> () / 100.0f, 0.0f), 1.0f)};
                f->opacity_stops.push_back (cp);
            }
            for (size_t a = 1; a < f->opacity_stops.size (); a++)
            {
                for (size_t b = a; b > 0 && f->opacity_stops[b].input < f->opacity_stops[b - 1].input; b--)
                {
                    std::swap (f->opacity_stops[b], f->opacity_stops[b - 1]);
                }
            }
        }
        std::string style;
        if (param_str (p, "style", &style))
        {
            const char* const STYLES[5] = {"linear", "radial", "angle", "reflected", "diamond"};
            int k                       = -1;
            for (int i = 0; i < 5; i++)
            {
                if (style == STYLES[i])
                {
                    k = i;
                }
            }
            if (k < 0)
            {
                return bad_params (err, cmd, "style must be linear, radial, angle, reflected or diamond");
            }
            f->style = (doc::GradientStyle) k;
        }
        float v;
        if (fill_number (p, "angle", &v))
        {
            if (f->kind == doc::FillKind::Pattern)
            {
                f->pattern_angle = fmodf (v, 360.0f);
            }
            else
            {
                f->angle = fmodf (v, 360.0f);
            }
        }
        if (fill_number (p, "scale", &v))
        {
            v = fminf (fmaxf (v, 1.0f), 1000.0f) / 100.0f;
            if (f->kind == doc::FillKind::Pattern)
            {
                f->pattern_scale = v;
            }
            else
            {
                f->scale = v;
            }
        }
        fill_flag (p, "reverse", &f->reverse);
        fill_flag (p, "dither", &f->dither);
        fill_flag (p, "align", &f->align);
        fill_flag (p, "link", &f->link);
        const Value* phase = param (p, "phase");
        if (phase != NULL && phase->is_array () && phase->size () == 2 && (*phase)[0].is_number ()
            && (*phase)[1].is_number ())
        {
            f->phase[0] = (*phase)[0].get<float> ();
            f->phase[1] = (*phase)[1].get<float> ();
        }
        const Value* pat = param (p, "pattern");
        if (pat != NULL)
        {
            const Pattern* found = NULL;
            if (pat->is_number_integer () && pat->get<int64_t> () >= 0
                && (size_t) pat->get<int64_t> () < s.patterns.size ())
            {
                found = &s.patterns[(size_t) pat->get<int64_t> ()];
            }
            for (size_t i = 0; found == NULL && pat->is_string () && i < s.patterns.size (); i++)
            {
                if (s.patterns[i].name == pat->get<std::string> ())
                {
                    found = &s.patterns[i];
                }
            }
            if (found == NULL)
            {
                return bad_params (err, cmd, "no pattern %s (patterns.list names them)", pat->dump ().c_str ());
            }
            f->pattern_name = found->name;
            f->pattern_w    = found->width;
            f->pattern_h    = found->height;
            f->pattern_rgba = found->rgba;
        }
        if (f->kind == doc::FillKind::Pattern && f->pattern_w == 0)
        {
            if (s.patterns.empty ())
            {
                return fail (err, ErrorKind::Other, "there are no patterns (Edit > Define Pattern makes one)");
            }
            f->pattern_name = s.patterns[0].name;
            f->pattern_w    = s.patterns[0].width;
            f->pattern_h    = s.patterns[0].height;
            f->pattern_rgba = s.patterns[0].rgba;
        }
        return true;
    }

    /* A new fill layer above the active one, masked by the selection (Photoshop's Color Fill,
       Gradient Fill or Pattern Fill). */
    bool new_fill_layer (Session& s, const Value& p, doc::FillKind kind, Value* result, Error* err)
    {
        const char* const CMDS[3] = {
            "layer.newFillLayer.solidColor", "layer.newFillLayer.gradient", "layer.newFillLayer.pattern"};
        const char* const NAMES[3] = {"Color Fill", "Gradient Fill", "Pattern Fill"};
        const char* cmd            = CMDS[(int) kind];
        ensure_patterns (s);
        doc::FillLayer f = doc::FillLayer::make ();
        f.kind           = kind;
        memcpy (f.color, s.tools.foreground, 3 * sizeof (float));
        doc::GradientStop a = {0.0f, {0, 0, 0}}, b = {1.0f, {0, 0, 0}};
        memcpy (a.color, s.tools.foreground, sizeof a.color);
        memcpy (b.color, s.tools.background, sizeof b.color);
        f.stops.clear ();
        f.stops.push_back (a);
        f.stops.push_back (b);
        if (!parse_fill (s, p, cmd, &f, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        std::string name;
        if (!param_str (p, "name", &name))
        {
            name = d->next_layer_name (NAMES[(int) kind]);
        }
        doc::Layer l = doc::Layer::make (s.ids, name, doc::LayerKind::Fill);
        l.fill       = f;
        double opacity;
        if (param_f64 (p, "opacity", &opacity))
        {
            l.opacity = (float) fminf (fmaxf (opacity, 0.0), 100.0) / 100.0f;
        }
        std::string mode;
        if (param_str (p, "mode", &mode) && !blend_from_str (mode.c_str (), &l.blend))
        {
            return bad_params (err, cmd, "unknown blend mode `%s`", mode.c_str ());
        }
        l.has_mask = true;
        l.mask     = doc::LayerMask::reveal_all ();
        if (d->has_selection)
        {
            l.mask.surface = d->selection;
        }
        doc::LayerId id = d->insert_above (*e.active_layer (), l);
        e.select (std::vector<doc::LayerId> (1, id), id);
        if (!e.commit ((std::string ("New ") + NAMES[(int) kind] + " Layer").c_str (), err))
        {
            return false;
        }
        *result = Value{{"layer", id}};
        return true;
    }

    bool cmd_new_solid_fill (Session& s, const Value& p, Value* r, Error* e)
    {
        return new_fill_layer (s, p, doc::FillKind::Solid, r, e);
    }
    bool cmd_new_gradient_fill (Session& s, const Value& p, Value* r, Error* e)
    {
        return new_fill_layer (s, p, doc::FillKind::Gradient, r, e);
    }
    bool cmd_new_pattern_fill (Session& s, const Value& p, Value* r, Error* e)
    {
        return new_fill_layer (s, p, doc::FillKind::Pattern, r, e);
    }

    /* A fill layer's settings changed (double-clicking its thumbnail). */
    bool cmd_set_fill (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.setFill";
        ensure_patterns (s);
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        if (l->kind != doc::LayerKind::Fill)
        {
            return fail (err, ErrorKind::Other, "not a fill layer");
        }
        doc::FillLayer f = l->fill;
        if (!parse_fill (s, p, cmd, &f, err))
        {
            return false;
        }
        l->fill = f;
        return e.commit ("Modify Fill Layer", err);
    }

    /* A Blend If range from {"this":[black, black split, white split, white], "underlying":[…]}. */
    bool parse_range (const Value& v, const char* cmd, doc::BlendRange* r, Error* err)
    {
        const char* const KEYS[2] = {"this", "underlying"};
        for (int k = 0; k < 2; k++)
        {
            const Value* a = param (v, KEYS[k]);
            if (a == NULL)
            {
                continue;
            }
            if (!a->is_array () || a->size () != 4)
            {
                return bad_params (err, cmd, "`%s` is [black, black split, white split, white] in 0..255", KEYS[k]);
            }
            float q[4];
            for (size_t i = 0; i < 4; i++)
            {
                if (!(*a)[i].is_number ())
                {
                    return bad_params (err, cmd, "Blend If levels are numbers");
                }
                q[i] = fminf (fmaxf ((*a)[i].get<float> (), 0.0f), 255.0f);
            }
            q[1]         = fmaxf (q[1], q[0]);
            q[3]         = fmaxf (q[3], q[2]);
            float *black = k == 0 ? r->this_black : r->under_black, *white = k == 0 ? r->this_white : r->under_white;
            black[0] = q[0];
            black[1] = q[1];
            white[0] = q[2];
            white[1] = q[3];
        }
        return true;
    }

    /* Layer > Layer Style > Blending Options' Blend If: per channel the levels of this layer and of
       the layers below that let it show. "reset": true restores the defaults. */
    bool cmd_blend_if (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.blendIf";
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        doc::BlendIf bi = l->blend_if;
        bool reset      = false;
        if (param_bool (p, "reset", &reset) && reset)
        {
            bi = doc::BlendIf::make ();
        }
        const char* const CH[4] = {"gray", "red", "green", "blue"};
        for (int c = 0; c < 4; c++)
        {
            const Value* v = param (p, CH[c]);
            if (v == NULL)
            {
                continue;
            }
            if (!v->is_object ())
            {
                return bad_params (err, cmd, "`%s` is {\"this\":[…], \"underlying\":[…]}", CH[c]);
            }
            if (!parse_range (*v, cmd, c == 0 ? &bi.gray : &bi.channels[c - 1], err))
            {
                return false;
            }
        }
        l->blend_if = bi;
        return e.commit ("Blending Options", err);
    }

    /* Layer > New > Background from Layer: the layer becomes the opaque, locked Background at the
       bottom, its transparent areas filled with the background colour. */
    bool cmd_background_from_layer (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d->background () != NULL)
        {
            return fail (err, ErrorKind::Other, "the document already has a Background");
        }
        doc::Layer moved;
        const doc::Layer* src = d->layer (id);
        if (src == NULL)
        {
            return no_layer (err, id);
        }
        if (src->kind != doc::LayerKind::Raster)
        {
            return fail (err, ErrorKind::Other, "Background from Layer works on pixel layers");
        }
        if (!d->remove (id, &moved))
        {
            return no_layer (err, id);
        }
        color::PixelFormat fmt = d->pixel_format ();
        geom::Rect canvas      = d->bounds ();
        float bgc[4]           = {s.tools.background[0], s.tools.background[1], s.tools.background[2], 1.0f};
        std::vector<float> under ((size_t) canvas.width () * canvas.height () * 4), over (under.size ());
        moved.pixels.read_rgba (canvas, s.cmyk, over.data ());
        size_t n = fmt.channels (), count = under.size () / 4;
        std::vector<float> data (count * n);
        for (size_t i = 0; i < count; i++)
        {
            float a = over[i * 4 + 3], c[4];
            for (int k = 0; k < 3; k++)
            {
                c[k] = over[i * 4 + (size_t) k] * a + bgc[k] * (1.0f - a);
            }
            c[3] = 1.0f;
            raster::from_rgba (fmt, c, s.cmyk, &data[i * n]);
        }
        doc::Layer bg = doc::Layer::raster (s.ids, "Background", fmt);
        bg.id         = moved.id;
        bg.pixels.write_region (canvas, data.data (), data.size ());
        bg.pixels.prune ();
        bg.locks.transparency = true;
        bg.locks.position     = true;
        bg.has_mask           = false;
        d->layers.insert (d->layers.begin (), bg);
        e.select (std::vector<doc::LayerId> (1, bg.id), bg.id);
        return e.commit ("Background From Layer", err);
    }

    bool can_background_from_layer (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d = s.active ();
        if (d->doc->background () != NULL)
        {
            *why = "the document already has a Background";
            return false;
        }
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l != NULL && l->kind == doc::LayerKind::Raster)
        {
            return true;
        }
        *why = "Background from Layer works on pixel layers";
        return false;
    }

    const char* const FILL_PARAMS =
        "{\"color\":\"#rrggbb\"|\"foreground\" "
        "(solid),\"stops\":[[location,\"#rrggbb\"],…],\"transparency\":[[location,opacity "
        "%],…],\"style\":\"linear|radial|angle|reflected|diamond\","
        "\"angle\":deg=90,\"scale\":1..1000 %=100,\"reverse\":bool,\"dither\":bool,\"align\":bool=true "
        "(gradient),\"pattern\":name|index,\"scale\":%,\"angle\":deg,\"link\":bool=true,"
        "\"phase\":[x,y] (pattern),\"name\":str?,\"opacity\":0..100?,\"mode\":blend?}";

    const CommandSpec FILL_LAYER_COMMANDS[] = {
        {"layer.newFillLayer.solidColor", "Solid Color…", "Layer/New Fill Layer", NULL, FILL_PARAMS, has_doc,
            cmd_new_solid_fill, true},
        {"layer.newFillLayer.gradient", "Gradient…", "Layer/New Fill Layer", NULL, FILL_PARAMS, has_doc,
            cmd_new_gradient_fill, true},
        {"layer.newFillLayer.pattern", "Pattern…", "Layer/New Fill Layer", NULL, FILL_PARAMS, has_doc,
            cmd_new_pattern_fill, true},
        {"layer.setFill", "Fill Layer Options", "", NULL,
            "{\"layer\":id?,\"type\":\"solid|gradient|pattern\"?, …the new fill layer settings}", has_layer,
            cmd_set_fill, true},
        {"layer.blendIf", "Blend If", "", NULL,
            "{\"layer\":id?,\"gray\"|\"red\"|\"green\"|\"blue\":{\"this\":[black,black split,white "
            "split,white],\"underlying\":[…]} (0..255),\"reset\":bool?}",
            has_layer, cmd_blend_if, true},
        {"layer.new.backgroundFromLayer", "Background from Layer", "Layer/New", NULL, "{\"layer\":id?}",
            can_background_from_layer, cmd_background_from_layer, true},
    };

    void register_fill_layer_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), FILL_LAYER_COMMANDS,
            FILL_LAYER_COMMANDS + sizeof FILL_LAYER_COMMANDS / sizeof FILL_LAYER_COMMANDS[0]);
    }

} // namespace engine
