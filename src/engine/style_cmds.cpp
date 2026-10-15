#include "engine/layers.h"
#include "engine/session.h"
#include "engine/style.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace engine
{

    /* Layer > Layer Style: the effects of a layer. layer.layerStyle.<kind> adds an effect or edits
       one (by "index", or the only instance of a kind Photoshop allows once); replace sets the whole
       list (the Layer Style dialog's OK); clear, copy/paste, show/hide all, the global light and
       Rasterize Layer Style. Percentages are 0..100, angles degrees, sizes pixels. */

    const char* const POSITIONS[3]       = {"outside", "inside", "center"};
    const char* const BEVEL_STYLES[4]    = {"outerBevel", "innerBevel", "emboss", "pillowEmboss"};
    const char* const GRADIENT_STYLES[5] = {"linear", "radial", "angle", "reflected", "diamond"};

    /* Kinds Photoshop allows several of (the rest have one instance). */
    bool multi_instance (doc::FxKind k)
    {
        return k == doc::FxKind::DropShadow || k == doc::FxKind::InnerShadow || k == doc::FxKind::ColorOverlay
            || k == doc::FxKind::GradientOverlay || k == doc::FxKind::Stroke;
    }

    void hex_of (const float c[3], char out[8])
    {
        snprintf (out, 8, "#%02x%02x%02x", (int) lroundf (fminf (fmaxf (c[0], 0.0f), 1.0f) * 255),
            (int) lroundf (fminf (fmaxf (c[1], 0.0f), 1.0f) * 255),
            (int) lroundf (fminf (fmaxf (c[2], 0.0f), 1.0f) * 255));
    }

    bool read_color (const Value& p, const char* key, float out[3], const char* cmd, Error* err)
    {
        if (!param_present (p, key))
        {
            return true;
        }
        float c[4]    = {out[0], out[1], out[2], 1.0f};
        float none[4] = {-1, -1, -1, -1};
        color_param (p, key, none, c);
        if (c[0] < 0.0f)
        {
            return bad_params (err, cmd, "`%s` must be \"#rrggbb\" or [r, g, b]", key);
        }
        memcpy (out, c, 3 * sizeof (float));
        return true;
    }

    bool read_number (
        const Value& p, const char* key, float lo, float hi, float scale, float* out, const char* cmd, Error* err)
    {
        if (!param_present (p, key))
        {
            return true;
        }
        double v;
        if (!param_f64 (p, key, &v) || !isfinite (v) || v < lo || v > hi)
        {
            return bad_params (err, cmd, "`%s` must be a number in %g..%g", key, lo, hi);
        }
        *out = (float) v * scale;
        return true;
    }

    bool read_flag (const Value& p, const char* key, bool* out, const char* cmd, Error* err)
    {
        if (!param_present (p, key))
        {
            return true;
        }
        if (!param_bool (p, key, out))
        {
            return bad_params (err, cmd, "`%s` must be true or false", key);
        }
        return true;
    }

    bool read_blend (const Value& p, const char* key, color::BlendMode* out, const char* cmd, Error* err)
    {
        if (!param_present (p, key))
        {
            return true;
        }
        std::string m;
        if (!param_str (p, key, &m) || !blend_from_str (m.c_str (), out) || *out == color::BlendMode::PassThrough)
        {
            return bad_params (err, cmd, "unknown blend mode in `%s`", key);
        }
        return true;
    }

    /* index of `s` in names[n], or -1. */
    int name_index (const std::string& s, const char* const* names, int n)
    {
        for (int i = 0; i < n; i++)
        {
            if (s == names[i])
            {
                return i;
            }
        }
        return -1;
    }

    /* The effect settings present in p applied to e. */
    bool effect_from_params (const Value& p_in, doc::Effect* e, const char* cmd, Error* err)
    {
        std::string s;
        /* Pattern Overlay: its pattern (by name), Link with Layer, phase, and a scale of 1..1000%. */
        Value p = p_in;
        if (e->kind == doc::FxKind::PatternOverlay)
        {
            double sc;
            if (param_f64 (p, "scale", &sc))
            {
                e->scale = (float) fmin (fmax (sc, 1.0), 1000.0) / 100.0f;
                p.erase ("scale");
            }
            std::string name;
            if (param_str (p, "pattern", &name))
            {
                if (name.size () >= sizeof e->pattern)
                {
                    return bad_params (err, cmd, "pattern names are under %zu characters", sizeof e->pattern);
                }
                snprintf (e->pattern, sizeof e->pattern, "%s", name.c_str ());
            }
            param_bool (p, "link", &e->link);
            const Value* ph = param (p, "phase");
            if (ph != NULL && ph->is_array () && ph->size () == 2 && (*ph)[0].is_number () && (*ph)[1].is_number ())
            {
                e->phase[0] = (*ph)[0].get<float> ();
                e->phase[1] = (*ph)[1].get<float> ();
            }
        }
        if (!read_flag (p, "enabled", &e->enabled, cmd, err) || !read_blend (p, "mode", &e->blend, cmd, err)
            || !read_number (p, "opacity", 0, 100, 0.01f, &e->opacity, cmd, err)
            || !read_color (p, "color", e->color, cmd, err)
            || !read_number (p, "angle", -360, 360, 1.0f, &e->angle, cmd, err)
            || !read_flag (p, "useGlobalLight", &e->global_light, cmd, err)
            || !read_number (p, "distance", 0, 30000, 1.0f, &e->distance, cmd, err)
            || !read_number (p, "spread", 0, 100, 0.01f, &e->spread, cmd, err)
            || !read_number (p, "choke", 0, 100, 0.01f, &e->spread, cmd, err)
            || !read_number (p, "size", 0, 250, 1.0f, &e->size, cmd, err)
            || !read_number (p, "range", 1, 100, 0.01f, &e->range, cmd, err)
            || !read_flag (p, "knocksOut", &e->knocks_out, cmd, err) || !read_flag (p, "invert", &e->invert, cmd, err)
            || !read_color (p, "color2", e->color2, cmd, err) || !read_flag (p, "reverse", &e->reverse, cmd, err)
            || !read_number (p, "scale", 10, 150, 0.01f, &e->scale, cmd, err)
            || !read_number (p, "depth", 1, 1000, 0.01f, &e->depth, cmd, err)
            || !read_number (p, "soften", 0, 16, 1.0f, &e->soften, cmd, err)
            || !read_number (p, "altitude", 0, 90, 1.0f, &e->altitude, cmd, err)
            || !read_blend (p, "shadowMode", &e->blend2, cmd, err)
            || !read_number (p, "shadowOpacity", 0, 100, 0.01f, &e->opacity2, cmd, err)
            || !read_color (p, "shadowColor", e->color2, cmd, err))
        {
            return false;
        }
        if (param_str (p, "technique", &s))
        {
            if (s != "softer" && s != "precise" && s != "smooth")
            {
                return bad_params (err, cmd, "`technique` must be softer, precise or smooth");
            }
            e->precise = s == "precise";
        }
        if (param_str (p, "source", &s))
        {
            if (s != "edge" && s != "center")
            {
                return bad_params (err, cmd, "`source` must be edge or center");
            }
            e->center = s == "center";
        }
        if (param_str (p, "position", &s))
        {
            int i = name_index (s, POSITIONS, 3);
            if (i < 0)
            {
                return bad_params (err, cmd, "`position` must be outside, inside or center");
            }
            e->position = (doc::StrokePosition) i;
        }
        if (param_str (p, "style", &s))
        {
            int i = e->kind == doc::FxKind::BevelEmboss ? name_index (s, BEVEL_STYLES, 4)
                                                        : name_index (s, GRADIENT_STYLES, 5);
            if (i < 0)
            {
                return bad_params (err, cmd, "unknown `style` %s", s.c_str ());
            }
            if (e->kind == doc::FxKind::BevelEmboss)
            {
                e->bevel = (doc::BevelStyle) i;
            }
            else
            {
                e->gradient = (doc::GradientStyle) i;
            }
        }
        if (param_str (p, "direction", &s))
        {
            if (s != "up" && s != "down")
            {
                return bad_params (err, cmd, "`direction` must be up or down");
            }
            e->up = s == "up";
        }
        return true;
    }

    /* The effect as the params that rebuild it. */
    Value effect_json (const doc::Effect& e)
    {
        char c1[8], c2[8];
        hex_of (e.color, c1);
        hex_of (e.color2, c2);
        Value v = {{"kind", doc::fx_kind_id (e.kind)}, {"enabled", e.enabled}, {"mode", color::blend_label (e.blend)},
            {"opacity", lroundf (e.opacity * 100.0f)}};
        switch (e.kind)
        {
        case doc::FxKind::DropShadow:
        case doc::FxKind::InnerShadow:
            v["color"]                                                = c1;
            v["angle"]                                                = e.angle;
            v["useGlobalLight"]                                       = e.global_light;
            v["distance"]                                             = e.distance;
            v[e.kind == doc::FxKind::DropShadow ? "spread" : "choke"] = e.spread * 100.0f;
            v["size"]                                                 = e.size;
            if (e.kind == doc::FxKind::DropShadow)
            {
                v["knocksOut"] = e.knocks_out;
            }
            break;
        case doc::FxKind::OuterGlow:
        case doc::FxKind::InnerGlow:
            v["color"]                                               = c1;
            v["technique"]                                           = e.precise ? "precise" : "softer";
            v[e.kind == doc::FxKind::OuterGlow ? "spread" : "choke"] = e.spread * 100.0f;
            v["size"]                                                = e.size;
            v["range"]                                               = e.range * 100.0f;
            if (e.kind == doc::FxKind::InnerGlow)
            {
                v["source"] = e.center ? "center" : "edge";
            }
            break;
        case doc::FxKind::Satin:
            v["color"]    = c1;
            v["angle"]    = e.angle;
            v["distance"] = e.distance;
            v["size"]     = e.size;
            v["invert"]   = e.invert;
            break;
        case doc::FxKind::ColorOverlay:
            v["color"] = c1;
            break;
        case doc::FxKind::GradientOverlay:
            v["color"]   = c1;
            v["color2"]  = c2;
            v["style"]   = GRADIENT_STYLES[(int) e.gradient];
            v["angle"]   = e.angle;
            v["scale"]   = e.scale * 100.0f;
            v["reverse"] = e.reverse;
            break;
        case doc::FxKind::Stroke:
            v["color"]    = c1;
            v["size"]     = e.size;
            v["position"] = POSITIONS[(int) e.position];
            break;
        case doc::FxKind::PatternOverlay:
            v["pattern"] = e.pattern;
            v["scale"]   = e.scale * 100.0f;
            v["link"]    = e.link;
            v["phase"]   = {e.phase[0], e.phase[1]};
            break;
        case doc::FxKind::BevelEmboss:
            v["style"]          = BEVEL_STYLES[(int) e.bevel];
            v["depth"]          = e.depth * 100.0f;
            v["direction"]      = e.up ? "up" : "down";
            v["size"]           = e.size;
            v["soften"]         = e.soften;
            v["angle"]          = e.angle;
            v["altitude"]       = e.altitude;
            v["useGlobalLight"] = e.global_light;
            v["color"]          = c1;
            v["shadowMode"]     = color::blend_label (e.blend2);
            v["shadowOpacity"]  = lroundf (e.opacity2 * 100.0f);
            v["shadowColor"]    = c2;
            break;
        }
        return v;
    }

    Value style_json (const doc::LayerStyle& st)
    {
        Value list = Value::array ();
        for (size_t i = 0; i < st.items.size (); i++)
        {
            list.push_back (effect_json (st.items[i]));
        }
        return Value{{"enabled", st.enabled}, {"effects", list}};
    }

    /* An effect built from {"kind": ..., settings}. */
    bool effect_from_json (const Value& v, doc::Effect* out, const char* cmd, Error* err)
    {
        std::string k;
        doc::FxKind kind;
        if (!v.is_object () || !param_str (v, "kind", &k) || !doc::fx_kind_from_id (k.c_str (), &kind))
        {
            return bad_params (err, cmd, "each effect needs a known `kind`");
        }
        *out = doc::Effect::make (kind);
        return effect_from_params (v, out, cmd, err);
    }

    bool styled_layer (Session& s, const Value& p, doc::LayerId* id, Error* err)
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
        if (l->kind == doc::LayerKind::Adjustment)
        {
            return fail (err, ErrorKind::Other, "adjustment layers can't have a layer style");
        }
        if (s.active ()->doc->is_background (*id))
        {
            return fail (err, ErrorKind::Other, "the Background can't have a layer style; convert it to a layer first");
        }
        return true;
    }

    /* layer.layerStyle.<kind>: add or edit one effect. */
    /* A Pattern Overlay's pattern in the document: copied from the library when it isn't there
       yet (an unnamed one takes the library's first). */
    bool ensure_doc_pattern (Session& s, doc::Document* d, doc::Effect* fx, const char* cmd, Error* err)
    {
        if (fx->kind != doc::FxKind::PatternOverlay)
        {
            return true;
        }
        ensure_patterns (s);
        if (fx->pattern[0] == 0 && !s.patterns.empty ())
        {
            snprintf (fx->pattern, sizeof fx->pattern, "%s", s.patterns[0].name.c_str ());
        }
        if (d->pattern (fx->pattern) != NULL)
        {
            return true;
        }
        for (size_t i = 0; i < s.patterns.size (); i++)
        {
            if (s.patterns[i].name != fx->pattern)
            {
                continue;
            }
            doc::DocPattern dp;
            dp.name   = s.patterns[i].name;
            dp.width  = s.patterns[i].width;
            dp.height = s.patterns[i].height;
            dp.rgba   = s.patterns[i].rgba;
            d->patterns.push_back (dp);
            return true;
        }
        return bad_params (err, cmd, "no pattern \"%s\" (patterns.list names them)", fx->pattern);
    }

    bool set_effect (Session& s, doc::FxKind kind, const Value& p, Value* result, Error* err)
    {
        std::string cmd = std::string ("layer.layerStyle.") + doc::fx_kind_id (kind);
        doc::LayerId id;
        if (!styled_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l                   = e.doc ()->layer_mut (id);
        std::vector<doc::Effect>& items = l->style.items;
        int64_t index                   = -1;
        if (param_i64 (p, "index", &index))
        {
            if (index < 0 || (size_t) index >= items.size () || items[(size_t) index].kind != kind)
            {
                return bad_params (
                    err, cmd.c_str (), "no %s at index %lld", doc::fx_kind_label (kind), (long long) index);
            }
        }
        else if (!multi_instance (kind))
        {
            for (size_t i = 0; i < items.size (); i++)
            {
                if (items[i].kind == kind)
                {
                    index = (int64_t) i;
                }
            }
        }
        doc::Effect fx = index >= 0 ? items[(size_t) index] : doc::Effect::make (kind);
        if (!effect_from_params (p, &fx, cmd.c_str (), err))
        {
            return false;
        }
        if (!ensure_doc_pattern (s, e.doc (), &fx, cmd.c_str (), err))
        {
            return false;
        }
        if (index >= 0)
        {
            items[(size_t) index] = fx;
        }
        else
        {
            /* A new instance goes on top of its kind, in the panel's order. */
            size_t at = doc::fx_insert_at (items, kind);
            items.insert (items.begin () + (ptrdiff_t) at, fx);
            index = (int64_t) at;
        }
        l->style.enabled = true;
        if (!e.commit (doc::fx_kind_label (kind), err))
        {
            return false;
        }
        *result = Value{{"index", index}, {"effect", effect_json (fx)}};
        return true;
    }

    bool cmd_drop_shadow (Session& s, const Value& p, Value* r, Error* err)
    {
        return set_effect (s, doc::FxKind::DropShadow, p, r, err);
    }
    bool cmd_inner_shadow (Session& s, const Value& p, Value* r, Error* err)
    {
        return set_effect (s, doc::FxKind::InnerShadow, p, r, err);
    }
    bool cmd_outer_glow (Session& s, const Value& p, Value* r, Error* err)
    {
        return set_effect (s, doc::FxKind::OuterGlow, p, r, err);
    }
    bool cmd_inner_glow (Session& s, const Value& p, Value* r, Error* err)
    {
        return set_effect (s, doc::FxKind::InnerGlow, p, r, err);
    }
    bool cmd_satin (Session& s, const Value& p, Value* r, Error* err)
    {
        return set_effect (s, doc::FxKind::Satin, p, r, err);
    }
    bool cmd_color_overlay (Session& s, const Value& p, Value* r, Error* err)
    {
        return set_effect (s, doc::FxKind::ColorOverlay, p, r, err);
    }
    bool cmd_gradient_overlay (Session& s, const Value& p, Value* r, Error* err)
    {
        return set_effect (s, doc::FxKind::GradientOverlay, p, r, err);
    }
    bool cmd_stroke_fx (Session& s, const Value& p, Value* r, Error* err)
    {
        return set_effect (s, doc::FxKind::Stroke, p, r, err);
    }
    bool cmd_bevel (Session& s, const Value& p, Value* r, Error* err)
    {
        return set_effect (s, doc::FxKind::BevelEmboss, p, r, err);
    }
    bool cmd_pattern_overlay (Session& s, const Value& p, Value* r, Error* err)
    {
        return set_effect (s, doc::FxKind::PatternOverlay, p, r, err);
    }

    /* Layer > Layer Style > Scale Effects: every effect's sizes and distances (and the gradient and
       pattern scales) times "scale" percent. */
    bool cmd_scale_effects (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.layerStyle.scaleEffects";
        double pct;
        if (!param_f64 (p, "scale", &pct) || pct < 1.0 || pct > 1000.0)
        {
            return bad_params (err, cmd, "`scale` is 1..1000 (percent)");
        }
        float k = (float) pct / 100.0f;
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
        if (l->style.items.empty ())
        {
            return fail (err, ErrorKind::Other, "the layer has no layer style");
        }
        for (size_t i = 0; i < l->style.items.size (); i++)
        {
            doc::Effect& fx = l->style.items[i];
            fx.distance     = fminf (fx.distance * k, 30000.0f);
            fx.size         = fminf (fx.size * k, 250.0f);
            fx.soften       = fminf (fx.soften * k, 16.0f);
            if (fx.kind == doc::FxKind::GradientOverlay)
            {
                fx.scale = fminf (fmaxf (fx.scale * k, 0.1f), 1.5f);
            }
            if (fx.kind == doc::FxKind::PatternOverlay)
            {
                fx.scale = fminf (fmaxf (fx.scale * k, 0.01f), 10.0f);
            }
        }
        return e.commit ("Scale Effects", err);
    }

    /* The whole effect list at once (the Layer Style dialog's OK). */
    bool cmd_style_replace (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.layerStyle.replace";
        doc::LayerId id;
        if (!styled_layer (s, p, &id, err))
        {
            return false;
        }
        const Value* list = param (p, "effects");
        if (list == NULL || !list->is_array ())
        {
            return bad_params (err, cmd, "missing `effects` (an array)");
        }
        doc::LayerStyle st;
        st.enabled = true;
        param_bool (p, "enabled", &st.enabled);
        Edit e (s);
        for (size_t i = 0; i < list->size (); i++)
        {
            doc::Effect fx;
            if (!effect_from_json ((*list)[i], &fx, cmd, err))
            {
                return false;
            }
            if (!ensure_doc_pattern (s, e.doc (), &fx, cmd, err))
            {
                return false;
            }
            st.items.push_back (fx);
        }
        e.doc ()->layer_mut (id)->style = st;
        if (!e.commit ("Layer Style", err))
        {
            return false;
        }
        *result = style_json (st);
        return true;
    }

    bool has_style (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l != NULL && !l->style.items.empty ())
        {
            return true;
        }
        *why = "the layer has no layer style";
        return false;
    }

    /* Clear Layer Style, or one effect ("effect": index). */
    bool cmd_style_clear (Session& s, const Value& p, Value* result, Error* err)
    {
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
        int64_t index;
        if (param_i64 (p, "effect", &index))
        {
            if (index < 0 || (size_t) index >= l->style.items.size ())
            {
                return bad_params (err, "layer.layerStyle.clear", "no effect %lld", (long long) index);
            }
            l->style.items.erase (l->style.items.begin () + (ptrdiff_t) index);
            return e.commit ("Delete Effect", err);
        }
        l->style.items.clear ();
        l->style.enabled = true;
        return e.commit ("Clear Layer Style", err);
    }

    /* Shows or hides one effect ("effect": index) or the whole style. */
    bool cmd_effects_visible (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.setEffectsVisible";
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        bool visible;
        if (!param_bool (p, "visible", &visible))
        {
            return bad_params (err, cmd, "missing `visible`");
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        int64_t index;
        if (param_i64 (p, "effect", &index))
        {
            if (index < 0 || (size_t) index >= l->style.items.size ())
            {
                return bad_params (err, cmd, "no effect %lld", (long long) index);
            }
            l->style.items[(size_t) index].enabled = visible;
        }
        else
        {
            l->style.enabled = visible;
        }
        return e.commit (visible ? "Show Effect" : "Hide Effect", err);
    }

    bool show_hide_all (Session& s, bool visible, Error* err)
    {
        Edit e (s);
        std::vector<doc::LayerId> ids = s.active ()->selected ();
        for (size_t i = 0; i < ids.size (); i++)
        {
            doc::Layer* l = e.doc ()->layer_mut (ids[i]);
            if (l != NULL)
            {
                l->style.enabled = visible;
            }
        }
        return e.commit (visible ? "Show All Effects" : "Hide All Effects", err);
    }

    bool cmd_show_all (Session& s, const Value& p, Value* result, Error* err)
    {
        return show_hide_all (s, true, err);
    }

    bool cmd_hide_all (Session& s, const Value& p, Value* result, Error* err)
    {
        return show_hide_all (s, false, err);
    }

    bool cmd_copy_style (Session& s, const Value& p, Value* result, Error* err)
    {
        const DocState* d     = s.active ();
        const doc::Layer* l   = d->doc->layer (d->active_layer);
        s.style_clipboard     = l->style;
        s.has_style_clipboard = true;
        *result               = style_json (l->style);
        return true;
    }

    bool can_paste_style (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        if (s.has_style_clipboard)
        {
            return true;
        }
        *why = "no layer style has been copied";
        return false;
    }

    /* Paste Layer Style: onto every selected layer that can take one. */
    bool cmd_paste_style (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        std::vector<doc::LayerId> ids = s.active ()->selected ();
        for (size_t i = 0; i < ids.size (); i++)
        {
            doc::Layer* l = e.doc ()->layer_mut (ids[i]);
            if (l != NULL && l->kind != doc::LayerKind::Adjustment && !e.doc ()->is_background (ids[i]))
            {
                l->style = s.style_clipboard;
            }
        }
        return e.commit ("Paste Layer Style", err);
    }

    /* Global Light: the angle and altitude effects using it share. */
    bool cmd_global_light (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.layerStyle.globalLight";
        Edit e (s);
        doc::Document* d = e.doc ();
        if (!read_number (p, "angle", -360, 360, 1.0f, &d->light_angle, cmd, err)
            || !read_number (p, "altitude", 0, 90, 1.0f, &d->light_altitude, cmd, err))
        {
            return false;
        }
        if (!e.commit ("Global Light", err))
        {
            return false;
        }
        *result = Value{{"angle", d->light_angle}, {"altitude", d->light_altitude}};
        return true;
    }

    /* Rasterize Layer Style: the effects baked into the layer's pixels. */
    bool cmd_rasterize_style (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        doc::Layer* l    = d->layer_mut (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        if (l->kind != doc::LayerKind::Raster)
        {
            return fail (err, ErrorKind::Other, "only pixel layers can be rasterized with their style here");
        }
        doc::Document solo = *d;
        doc::Layer one     = *l;
        one.visible        = true;
        one.opacity        = 1.0f;
        one.blend          = color::BlendMode::Normal;
        one.clipped        = false;
        solo.layers.assign (1, one);
        color::PixelFormat fmt = l->pixels.format ();
        fmt.alpha              = true;
        l->pixels              = composite_layers (solo, fmt, s.cmyk);
        l->style.items.clear ();
        l->fill_opacity = 1.0f;
        return e.commit ("Rasterize Layer Style", err);
    }

    bool cmd_style_get (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        const doc::Layer* l = s.active ()->doc->layer (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        *result = style_json (l->style);
        return true;
    }

    const char FX_PARAMS_SHADOW[] = "{\"layer\":id?,\"index\":n?,\"enabled\":bool,\"mode\":blend,\"opacity\":0..100,"
                                    "\"color\":\"#rrggbb\",\"angle\":deg,\"useGlobalLight\":bool,\"distance\":px,"
                                    "\"spread|choke\":0..100,\"size\":0..250,\"knocksOut\":bool}";

    const CommandSpec STYLE_COMMANDS[] = {
        {"layer.layerStyle.dropShadow", "Drop Shadow…", "Layer/Layer Style", NULL, FX_PARAMS_SHADOW, has_layer,
            cmd_drop_shadow, true},
        {"layer.layerStyle.innerShadow", "Inner Shadow…", "Layer/Layer Style", NULL, FX_PARAMS_SHADOW, has_layer,
            cmd_inner_shadow, true},
        {"layer.layerStyle.outerGlow", "Outer Glow…", "Layer/Layer Style", NULL,
            "{\"layer\":id?,\"index\":n?,\"mode\":blend,\"opacity\":0..100,\"color\":\"#rrggbb\",\"technique\":"
            "\"softer|precise\",\"spread\":0..100,\"size\":0..250,\"range\":1..100}",
            has_layer, cmd_outer_glow, true},
        {"layer.layerStyle.innerGlow", "Inner Glow…", "Layer/Layer Style", NULL,
            "{\"layer\":id?,\"mode\":blend,\"opacity\":0..100,\"color\":\"#rrggbb\",\"technique\":\"softer|precise\","
            "\"source\":\"edge|center\",\"choke\":0..100,\"size\":0..250,"
            "\"range\":1..100}",
            has_layer, cmd_inner_glow, true},
        {"layer.layerStyle.satin", "Satin…", "Layer/Layer Style", NULL,
            "{\"layer\":id?,\"mode\":blend,\"opacity\":0..100,\"color\":\"#rrggbb\",\"angle\":deg,\"distance\":px,"
            "\"size\":0..250,\"invert\":bool}",
            has_layer, cmd_satin, true},
        {"layer.layerStyle.colorOverlay", "Color Overlay…", "Layer/Layer Style", NULL,
            "{\"layer\":id?,\"index\":n?,\"mode\":blend,\"opacity\":0..100,\"color\":\"#rrggbb\"}", has_layer,
            cmd_color_overlay, true},
        {"layer.layerStyle.gradientOverlay", "Gradient Overlay…", "Layer/Layer Style", NULL,
            "{\"layer\":id?,\"index\":n?,\"mode\":blend,\"opacity\":0..100,\"color\":\"#rrggbb\" "
            "(start),\"color2\":\"#rrggbb\" (end),"
            "\"style\":\"linear|radial|angle|reflected|diamond\",\"angle\":deg,\"scale\":10..150,\"reverse\":bool}",
            has_layer, cmd_gradient_overlay, true},
        {"layer.layerStyle.stroke", "Stroke…", "Layer/Layer Style", NULL,
            "{\"layer\":id?,\"index\":n?,\"mode\":blend,\"opacity\":0..100,\"color\":\"#rrggbb\",\"size\":1..250,"
            "\"position\":\"outside|inside|center\"}",
            has_layer, cmd_stroke_fx, true},
        {"layer.layerStyle.bevelEmboss", "Bevel & Emboss…", "Layer/Layer Style", NULL,
            "{\"layer\":id?,\"style\":\"outerBevel|innerBevel|emboss|pillowEmboss\",\"depth\":1..1000,\"direction\":"
            "\"up|down\",\"size\":0..250,\"soften\":0..16,"
            "\"angle\":deg,\"altitude\":0..90,\"useGlobalLight\":bool,\"mode\":blend "
            "(highlight),\"opacity\":0..100,\"color\":\"#rrggbb\",\"shadowMode\":blend,"
            "\"shadowOpacity\":0..100,\"shadowColor\":\"#rrggbb\"}",
            has_layer, cmd_bevel, true},
        {"layer.layerStyle.patternOverlay", "Pattern Overlay…", "Layer/Layer Style", NULL,
            "{\"layer\":id?,\"mode\":blend,\"opacity\":0..100,\"pattern\":name,\"scale\":1..1000,\"link\":bool=true,"
            "\"phase\":[x,y]}",
            has_layer, cmd_pattern_overlay, true},
        {"layer.layerStyle.copyLayerStyle", "Copy Layer Style", "Layer/Layer Style", NULL, "{}", has_layer,
            cmd_copy_style, false},
        {"layer.layerStyle.pasteLayerStyle", "Paste Layer Style", "Layer/Layer Style", NULL, "{}", can_paste_style,
            cmd_paste_style, true},
        {"layer.layerStyle.clear", "Clear Layer Style", "Layer/Layer Style", NULL, "{\"layer\":id?,\"effect\":index?}",
            has_style, cmd_style_clear, true},
        {"layer.layerStyle.globalLight", "Global Light…", "Layer/Layer Style", NULL,
            "{\"angle\":deg,\"altitude\":0..90}", has_doc, cmd_global_light, true},
        {"layer.layerStyle.scaleEffects", "Scale Effects…", "Layer/Layer Style", NULL,
            "{\"layer\":id?,\"scale\":1..1000 (percent)}", has_style, cmd_scale_effects, true},
        {"layer.layerStyle.hideAllEffects", "Hide All Effects", "Layer/Layer Style", NULL, "{}", has_layer,
            cmd_hide_all, true},
        {"layer.layerStyle.showAllEffects", "Show All Effects", "Layer/Layer Style", NULL, "{}", has_layer,
            cmd_show_all, true},
        {"layer.rasterize.layerStyle", "Layer Style", "Layer/Rasterize", NULL, "{\"layer\":id?}", has_style,
            cmd_rasterize_style, true},
        {"layer.layerStyle.replace", "Layer Style", "", NULL,
            "{\"layer\":id?,\"enabled\":bool=true,\"effects\":[{\"kind\":…,…settings}]}", has_layer, cmd_style_replace,
            true},
        {"layer.layerStyle.get", "Layer Style", "", NULL, "{\"layer\":id?} → {enabled, effects}", has_layer,
            cmd_style_get, false},
        {"layer.setEffectsVisible", "Effect Visibility", "", NULL,
            "{\"layer\":id?,\"effect\":index? (none: the whole style),\"visible\":bool}", has_layer,
            cmd_effects_visible, true},
    };

    void register_style_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), STYLE_COMMANDS, STYLE_COMMANDS + sizeof STYLE_COMMANDS / sizeof STYLE_COMMANDS[0]);
    }

} // namespace engine
