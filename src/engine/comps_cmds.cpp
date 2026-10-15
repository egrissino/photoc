#include "engine/brush.h"
#include "engine/layers.h"
#include "engine/params.h"
#include "io/io.h"
#include <set>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace engine
{

    /* Window > Layer Comps: capture, apply and manage layer comps (layerComp.*), and File > Export
       > Layer Comps to Files. A comp records every layer's visibility, position and appearance; its
       three options choose which of them applying restores. Comps live in the document, so every
       change is one history step. Commands acting on "the" comp take "comp": id | name and default
       to the comp applied (or made) last. */

    /* Where a layer's content starts (top left of its pixels), or false for layers without pixels
       of their own. */
    bool layer_position (const doc::Layer& l, int32_t* x, int32_t* y)
    {
        if (l.is_group () || l.kind == doc::LayerKind::Adjustment || l.kind == doc::LayerKind::Fill)
        {
            return false;
        }
        const raster::Surface* s = l.surface ();
        if (s == NULL)
        {
            return false;
        }
        geom::Rect r = s->content_bounds ();
        if (r.is_empty ())
        {
            return false;
        }
        *x = r.x0;
        *y = r.y0;
        return true;
    }

    std::vector<doc::CompLayerState> capture_states (const doc::Document& d)
    {
        std::vector<doc::CompLayerState> out;
        std::vector<doc::WalkEntry> all = d.walk ();
        for (size_t i = 0; i < all.size (); i++)
        {
            const doc::Layer& l = *all[i].layer;
            doc::CompLayerState st;
            st.layer        = l.id;
            st.has_visible  = true;
            st.visible      = l.visible;
            st.has_position = layer_position (l, &st.x, &st.y);
            if (!st.has_position)
            {
                st.x = st.y = 0;
            }
            st.has_appearance = true;
            st.blend          = l.blend;
            st.opacity        = l.opacity;
            st.fill           = l.fill_opacity;
            st.style          = l.style;
            out.push_back (st);
        }
        return out;
    }

    /* The comp's recorded states put back (only the options it applies, or all of them). Layers it
       doesn't know keep their state. */
    void apply_comp (doc::Document* d, const doc::LayerComp& c, bool all)
    {
        for (size_t i = 0; i < c.states.size (); i++)
        {
            const doc::CompLayerState& st = c.states[i];
            doc::Layer* l                 = d->layer_mut (st.layer);
            if (l == NULL)
            {
                continue;
            }
            if ((all || c.apply_visibility) && st.has_visible)
            {
                l->visible = st.visible;
            }
            if ((all || c.apply_appearance) && st.has_appearance)
            {
                l->blend        = st.blend;
                l->opacity      = st.opacity;
                l->fill_opacity = st.fill;
                l->style        = st.style;
            }
            int32_t x, y;
            if ((all || c.apply_position) && st.has_position && layer_position (*l, &x, &y) && (x != st.x || y != st.y))
            {
                Error ignored;
                move_layers (d, std::vector<doc::LayerId> (1, st.layer), st.x - x, st.y - y, false, &ignored);
            }
        }
    }

    uint32_t next_comp_id (const doc::Document& d)
    {
        uint32_t id = 1;
        for (size_t i = 0; i < d.layer_comps.size (); i++)
        {
            id = std::max (id, d.layer_comps[i].id + 1);
        }
        return id;
    }

    doc::LayerComp* find_comp (doc::Document* d, uint32_t id)
    {
        for (size_t i = 0; i < d->layer_comps.size (); i++)
        {
            if (d->layer_comps[i].id == id)
            {
                return &d->layer_comps[i];
            }
        }
        return NULL;
    }

    /* The comp a command acts on: "comp" as an id or a name, else the last applied comp, else the
       first. */
    bool comp_param (const doc::Document& d, const Value& p, const char* cmd, uint32_t* out, Error* err)
    {
        const Value* v = param (p, "comp");
        if (v != NULL && v->is_number_integer ())
        {
            for (size_t i = 0; i < d.layer_comps.size (); i++)
            {
                if ((int64_t) d.layer_comps[i].id == v->get<int64_t> ())
                {
                    *out = d.layer_comps[i].id;
                    return true;
                }
            }
            return bad_params (err, cmd, "no layer comp with id %lld", (long long) v->get<int64_t> ());
        }
        if (v != NULL && v->is_string ())
        {
            for (size_t i = 0; i < d.layer_comps.size (); i++)
            {
                if (d.layer_comps[i].name == v->get<std::string> ())
                {
                    *out = d.layer_comps[i].id;
                    return true;
                }
            }
            return bad_params (err, cmd, "no layer comp named \"%s\"", v->get<std::string> ().c_str ());
        }
        if (v != NULL)
        {
            return bad_params (err, cmd, "\"comp\" is a comp id or name");
        }
        for (size_t i = 0; i < d.layer_comps.size (); i++)
        {
            if (d.layer_comps[i].id == d.last_applied_comp)
            {
                *out = d.last_applied_comp;
                return true;
            }
        }
        if (d.layer_comps.empty ())
        {
            return fail (err, ErrorKind::Other, "the document has no layer comps");
        }
        *out = d.layer_comps[0].id;
        return true;
    }

    bool comp_flag (const Value& p, const char* key, bool fallback)
    {
        bool b = fallback;
        param_bool (p, key, &b);
        return b;
    }

    size_t missing_layers (const doc::Document& d, const doc::LayerComp& c)
    {
        size_t n = 0;
        for (size_t i = 0; i < c.states.size (); i++)
        {
            n += d.layer (c.states[i].layer) == NULL ? 1 : 0;
        }
        return n;
    }

    bool cmd_comp_new (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::LayerComp c;
        c.id = next_comp_id (*d);
        if (!param_str (p, "name", &c.name))
        {
            for (size_t n = 1;; n++)
            {
                char name[32];
                snprintf (name, sizeof name, "Layer Comp %zu", n);
                bool taken = false;
                for (size_t i = 0; i < d->layer_comps.size (); i++)
                {
                    taken = taken || d->layer_comps[i].name == name;
                }
                if (!taken)
                {
                    c.name = name;
                    break;
                }
            }
        }
        param_str (p, "comment", &c.comment);
        c.apply_visibility = comp_flag (p, "visibility", true);
        c.apply_position   = comp_flag (p, "position", true);
        c.apply_appearance = comp_flag (p, "appearance", true);
        c.states           = capture_states (*d);
        d->layer_comps.push_back (c);
        d->last_applied_comp = c.id;
        if (!e.commit ("New Layer Comp", err))
        {
            return false;
        }
        *result = Value{{"comp", c.id}};
        return true;
    }

    /* Update: the comp records the layers as they are now ("what": all, or one property). */
    bool cmd_comp_update (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd  = "layerComp.update";
        std::string what = "all";
        param_str (p, "what", &what);
        if (what != "all" && what != "visibility" && what != "position" && what != "appearance")
        {
            return bad_params (err, cmd, "\"what\" is all, visibility, position or appearance");
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        uint32_t id;
        if (!comp_param (*d, p, cmd, &id, err))
        {
            return false;
        }
        doc::LayerComp* c                    = find_comp (d, id);
        std::vector<doc::CompLayerState> now = capture_states (*d);
        if (what == "all")
        {
            c->states = now;
        }
        else
        {
            for (size_t i = 0; i < now.size (); i++)
            {
                doc::CompLayerState* old = NULL;
                for (size_t k = 0; k < c->states.size (); k++)
                {
                    if (c->states[k].layer == now[i].layer)
                    {
                        old = &c->states[k];
                    }
                }
                doc::CompLayerState st = old != NULL ? *old : now[i];
                if (what == "visibility")
                {
                    st.has_visible = now[i].has_visible;
                    st.visible     = now[i].visible;
                }
                else if (what == "position")
                {
                    st.has_position = now[i].has_position;
                    st.x            = now[i].x;
                    st.y            = now[i].y;
                }
                else
                {
                    st.has_appearance = now[i].has_appearance;
                    st.blend          = now[i].blend;
                    st.opacity        = now[i].opacity;
                    st.fill           = now[i].fill;
                    st.style          = now[i].style;
                }
                now[i] = st;
            }
            c->states = now;
        }
        if (!e.commit ("Update Layer Comp", err))
        {
            return false;
        }
        *result = Value{{"comp", id}};
        return true;
    }

    /* Whether the layers still match what the last applied comp restores (then its Last Document
       State stays as it was while comps are stepped through). */
    bool matches_last_comp (const doc::Document& d, const std::vector<doc::CompLayerState>& now)
    {
        const doc::LayerComp* c = NULL;
        for (size_t i = 0; i < d.layer_comps.size (); i++)
        {
            if (d.layer_comps[i].id == d.last_applied_comp)
            {
                c = &d.layer_comps[i];
            }
        }
        if (c == NULL || c->states.size () != now.size ())
        {
            return false;
        }
        for (size_t i = 0; i < now.size (); i++)
        {
            const doc::CompLayerState* saved = NULL;
            for (size_t k = 0; k < c->states.size (); k++)
            {
                if (c->states[k].layer == now[i].layer)
                {
                    saved = &c->states[k];
                }
            }
            if (saved == NULL)
            {
                return false;
            }
            if (c->apply_visibility && saved->visible != now[i].visible)
            {
                return false;
            }
            if (c->apply_position && saved->has_position && (saved->x != now[i].x || saved->y != now[i].y))
            {
                return false;
            }
            if (c->apply_appearance
                && (saved->blend != now[i].blend || saved->opacity != now[i].opacity || saved->fill != now[i].fill
                    || !doc::same_style (saved->style, now[i].style)))
            {
                return false;
            }
        }
        return true;
    }

    bool apply_comp_step (Session& s, uint32_t id, const char* label, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d  = e.doc ();
        doc::LayerComp* c = find_comp (d, id);
        if (c == NULL)
        {
            return fail (err, ErrorKind::Other, "no layer comp with id %u", id);
        }
        doc::LayerComp comp                  = *c;
        std::vector<doc::CompLayerState> now = capture_states (*d);
        if (!matches_last_comp (*d, now))
        {
            d->has_last_state                           = true;
            d->last_document_state.id                   = 0;
            d->last_document_state.name                 = "Last Document State";
            d->last_document_state.apply_visibility     = d->last_document_state.apply_position =
                d->last_document_state.apply_appearance = true;
            d->last_document_state.states               = now;
        }
        apply_comp (d, comp, false);
        d->last_applied_comp = id;
        size_t missing       = missing_layers (*d, comp);
        if (!e.commit (label, err))
        {
            return false;
        }
        *result = Value{{"comp", id}, {"missingLayers", missing}};
        return true;
    }

    bool cmd_comp_apply (Session& s, const Value& p, Value* result, Error* err)
    {
        uint32_t id;
        if (!comp_param (*s.active ()->doc, p, "layerComp.apply", &id, err))
        {
            return false;
        }
        return apply_comp_step (s, id, "Apply Layer Comp", result, err);
    }

    bool comp_step (Session& s, int delta, Value* result, Error* err)
    {
        const doc::Document& d = *s.active ()->doc;
        size_t n = d.layer_comps.size (), cur = n;
        for (size_t i = 0; i < n; i++)
        {
            if (d.layer_comps[i].id == d.last_applied_comp)
            {
                cur = i;
            }
        }
        size_t next = cur == n ? (delta > 0 ? 0 : n - 1) : (cur + n + (size_t) (delta > 0 ? 1 : n - 1)) % n;
        return apply_comp_step (
            s, d.layer_comps[next].id, delta > 0 ? "Apply Next Layer Comp" : "Apply Previous Layer Comp", result, err);
    }

    bool cmd_comp_next (Session& s, const Value& p, Value* r, Error* e)
    {
        return comp_step (s, 1, r, e);
    }
    bool cmd_comp_previous (Session& s, const Value& p, Value* r, Error* e)
    {
        return comp_step (s, -1, r, e);
    }

    bool cmd_comp_delete (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        uint32_t id;
        if (!comp_param (*d, p, "layerComp.delete", &id, err))
        {
            return false;
        }
        for (size_t i = 0; i < d->layer_comps.size (); i++)
        {
            if (d->layer_comps[i].id == id)
            {
                d->layer_comps.erase (d->layer_comps.begin () + (ptrdiff_t) i);
            }
        }
        if (d->last_applied_comp == id)
        {
            d->last_applied_comp = 0;
        }
        return e.commit ("Delete Layer Comp", err);
    }

    bool cmd_comp_duplicate (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        uint32_t id;
        if (!comp_param (*d, p, "layerComp.duplicate", &id, err))
        {
            return false;
        }
        doc::LayerComp c = *find_comp (d, id);
        c.id             = next_comp_id (*d);
        c.name += " copy";
        size_t at = 0;
        while (at < d->layer_comps.size () && d->layer_comps[at].id != id)
        {
            at++;
        }
        d->layer_comps.insert (d->layer_comps.begin () + (ptrdiff_t) (at + 1), c);
        if (!e.commit ("Duplicate Layer Comp", err))
        {
            return false;
        }
        *result = Value{{"comp", c.id}};
        return true;
    }

    /* Layer Comp Options: the name, comment and the three options. */
    bool cmd_comp_options (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        uint32_t id;
        if (!comp_param (*d, p, "layerComp.setOptions", &id, err))
        {
            return false;
        }
        doc::LayerComp* c = find_comp (d, id);
        param_str (p, "name", &c->name);
        param_str (p, "comment", &c->comment);
        param_bool (p, "visibility", &c->apply_visibility);
        param_bool (p, "position", &c->apply_position);
        param_bool (p, "appearance", &c->apply_appearance);
        return e.commit ("Layer Comp Options", err);
    }

    bool cmd_comp_restore_last (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (!d->has_last_state)
        {
            return fail (err, ErrorKind::Other, "no Last Document State");
        }
        doc::LayerComp last = d->last_document_state;
        apply_comp (d, last, true);
        d->last_applied_comp = 0;
        return e.commit ("Restore Last Document State", err);
    }

    /* Comps that refer to deleted layers; "clear": true drops those states. */
    bool cmd_comp_warnings (Session& s, const Value& p, Value* result, Error* err)
    {
        const doc::Document& cur = *s.active ()->doc;
        Value list               = Value::array ();
        for (size_t i = 0; i < cur.layer_comps.size (); i++)
        {
            size_t n = missing_layers (cur, cur.layer_comps[i]);
            if (n > 0)
            {
                list.push_back (
                    Value{{"comp", cur.layer_comps[i].id}, {"name", cur.layer_comps[i].name}, {"missingLayers", n}});
            }
        }
        if (comp_flag (p, "clear", false) && !list.empty ())
        {
            Edit e (s);
            doc::Document* d = e.doc ();
            for (size_t i = 0; i < d->layer_comps.size (); i++)
            {
                std::vector<doc::CompLayerState> kept;
                for (size_t k = 0; k < d->layer_comps[i].states.size (); k++)
                {
                    if (d->layer (d->layer_comps[i].states[k].layer) != NULL)
                    {
                        kept.push_back (d->layer_comps[i].states[k]);
                    }
                }
                d->layer_comps[i].states = kept;
            }
            if (!e.commit ("Clear Layer Comp Warnings", err))
            {
                return false;
            }
        }
        *result = Value{{"warnings", list}, {"count", list.size ()}};
        return true;
    }

    bool cmd_comp_list (Session& s, const Value& p, Value* result, Error* err)
    {
        const doc::Document& d = *s.active ()->doc;
        Value comps            = Value::array ();
        for (size_t i = 0; i < d.layer_comps.size (); i++)
        {
            const doc::LayerComp& c = d.layer_comps[i];
            comps.push_back (Value{{"id", c.id}, {"name", c.name}, {"comment", c.comment},
                {"visibility", c.apply_visibility}, {"position", c.apply_position}, {"appearance", c.apply_appearance},
                {"layers", c.states.size ()}, {"missingLayers", missing_layers (d, c)}});
        }
        *result =
            Value{{"comps", comps}, {"lastApplied", d.last_applied_comp}, {"hasLastDocumentState", d.has_last_state}};
        return true;
    }

    /* File > Export > Layer Comps to Files: one file per comp in "dir", named
       <prefix>_<number>_<comp name>.<format>. */
    bool cmd_comps_to_files (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "file.export.layerCompsToFiles";
        std::string dir, format = "png", prefix;
        if (!param_str (p, "dir", &dir))
        {
            return bad_params (err, cmd, "missing \"dir\" (the folder to write to)");
        }
        param_str (p, "format", &format);
        if (!format.empty () && format[0] == '.')
        {
            format = format.substr (1);
        }
        const DocState* ds = s.active ();
        if (!param_str (p, "prefix", &prefix))
        {
            prefix = io::file_stem (ds->doc->name.c_str ());
        }
        Value files = Value::array ();
        for (size_t i = 0; i < ds->doc->layer_comps.size (); i++)
        {
            doc::Document copy = *ds->doc;
            apply_comp (&copy, ds->doc->layer_comps[i], false);
            std::string name = ds->doc->layer_comps[i].name;
            for (size_t k = 0; k < name.size (); k++)
            {
                if (name[k] == '/' || name[k] == '\\' || name[k] == ':')
                {
                    name[k] = '_';
                }
            }
            char num[16];
            snprintf (num, sizeof num, "_%04zu_", i + 1);
            std::string path           = dir + "/" + prefix + num + name + "." + format;
            compose::RenderSettings rs = compose::default_settings ();
            rs.cmyk                    = s.cmyk;
            std::vector<std::string> warnings;
            char msg[512];
            if (!io::save_file (copy, s.fonts (), path.c_str (), NULL, codecs::EncodeOptions::defaults (), rs,
                    &warnings, msg, sizeof msg))
            {
                return fail (err, ErrorKind::Other, "%s: %s", path.c_str (), msg);
            }
            files.push_back (path);
        }
        *result = Value{{"files", files}};
        return true;
    }

    bool has_comps (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (!s.active ()->doc->layer_comps.empty ())
        {
            return true;
        }
        *why = "the document has no layer comps";
        return false;
    }

    bool has_last_state (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (s.active ()->doc->has_last_state)
        {
            return true;
        }
        *why = "no comp has been applied over the document";
        return false;
    }

    const CommandSpec COMPS_COMMANDS[] = {
        {"layerComp.new", "New Layer Comp…", "", NULL,
            "{\"name\":str?,\"comment\":str?,\"visibility\":bool=true,\"position\":bool=true,\"appearance\":bool=true}",
            has_doc, cmd_comp_new, true},
        {"layerComp.update", "Update Layer Comp", "", NULL,
            "{\"comp\":id|name?,\"what\":\"all|visibility|position|appearance\"=\"all\"}", has_comps, cmd_comp_update,
            true},
        {"layerComp.apply", "Apply Layer Comp", "", NULL, "{\"comp\":id|name?}", has_comps, cmd_comp_apply, true},
        {"layerComp.previous", "Apply Previous Layer Comp", "", NULL, "{}", has_comps, cmd_comp_previous, true},
        {"layerComp.next", "Apply Next Layer Comp", "", NULL, "{}", has_comps, cmd_comp_next, true},
        {"layerComp.delete", "Delete Layer Comp", "", NULL, "{\"comp\":id|name?}", has_comps, cmd_comp_delete, true},
        {"layerComp.duplicate", "Duplicate Layer Comp", "", NULL, "{\"comp\":id|name?}", has_comps, cmd_comp_duplicate,
            true},
        {"layerComp.setOptions", "Layer Comp Options", "", NULL,
            "{\"comp\":id|name?,\"name\":str?,\"comment\":str?,\"visibility\":bool?,\"position\":bool?,\"appearance\":"
            "bool?}",
            has_comps, cmd_comp_options, true},
        {"layerComp.restoreLastDocumentState", "Restore Last Document State", "", NULL, "{}", has_last_state,
            cmd_comp_restore_last, true},
        {"layerComp.warnings", "Layer Comp Warnings", "", NULL, "{\"clear\":bool=false}", has_comps, cmd_comp_warnings,
            true},
        {"layerComp.list", "List Layer Comps", "", NULL, "{}", has_doc, cmd_comp_list, false},
        {"file.export.layerCompsToFiles", "Layer Comps to Files…", "File/Export", NULL,
            "{\"dir\":path,\"format\":\"png|jpg|psd|…\"=\"png\",\"prefix\":str?}", has_comps, cmd_comps_to_files,
            false},
    };

    void register_comps_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), COMPS_COMMANDS, COMPS_COMMANDS + sizeof COMPS_COMMANDS / sizeof COMPS_COMMANDS[0]);
    }

} // namespace engine
