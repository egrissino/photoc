#include "engine/layers.h"
#include "engine/session.h"
#include "io/io.h"
#include <algorithm>
#include <filesystem>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace engine
{

    /* Actions: recording commands into named actions (in sets), playing them back, the Actions
       panel's editing, saving them as JSON, and File > Automate > Batch. A step is a command and the
       settings it ran with. While recording, a layer picked by id becomes "the active layer" when it
       was the active one, else the layer's name, so actions play back on other documents. Brush
       strokes are not recorded (as in Photoshop). A step can ask for its dialog: playback then stops
       there and returns the step, for the window to show the dialog and carry on. */

    /* Commands an action doesn't record: strokes, brush settings, the action machinery, and edits
       of the work path made with the mouse. */
    bool action_recordable (const char* id)
    {
        const char* const SKIP[] = {"paint.stroke", "paint.pencil", "paint.blur", "paint.sharpen", "paint.smudge",
            "paint.dodge", "paint.burn", "paint.sponge", "paint.cloneStamp", "paint.healingBrush", "paint.spotHealing",
            "paint.historyBrush", "tools.setBrush", "edit.undo", "edit.redo", "history.", "actions.", "file.automate."};
        for (size_t i = 0; i < sizeof SKIP / sizeof SKIP[0]; i++)
        {
            size_t n = strlen (SKIP[i]);
            if (SKIP[i][n - 1] == '.' ? strncmp (id, SKIP[i], n) == 0 : strcmp (id, SKIP[i]) == 0)
            {
                return false;
            }
        }
        return true;
    }

    /* A step's settings as recorded: the active layer left implicit, other layers by name. */
    Value action_params (const Session& s, const Value& params)
    {
        Value v           = params.is_object () ? params : Value::object ();
        const DocState* d = s.active ();
        uint64_t id;
        if (d != NULL && param_u64 (v, "layer", &id))
        {
            v.erase ("layer");
            const doc::Layer* l = d->doc->layer (id);
            if (id != d->active_layer && l != NULL)
            {
                v["layerName"] = l->name;
            }
        }
        return v;
    }

    /* Back to ids for playback: a layer name finds the topmost layer of that name. */
    bool resolve_params (const Session& s, const Value& params, Value* out, Error* err)
    {
        *out = params.is_object () ? params : Value::object ();
        std::string name;
        if (!param_str (*out, "layerName", &name))
        {
            return true;
        }
        out->erase ("layerName");
        const DocState* d = s.active ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        std::vector<doc::WalkEntry> walk = d->doc->walk ();
        for (size_t i = walk.size (); i-- > 0;)
        {
            if (walk[i].layer->name == name)
            {
                (*out)["layer"] = walk[i].layer->id;
                return true;
            }
        }
        return fail (err, ErrorKind::Other, "there is no layer named \"%s\"", name.c_str ());
    }

    /* ---- JSON ---- */

    Value step_json (const Session& s, const ActionStep& st)
    {
        const CommandSpec* spec = s.find_command (st.command.c_str ());
        std::string label       = spec != NULL ? spec->label : st.command;
        size_t dots             = label.find ("…");
        if (dots != std::string::npos)
        {
            label = label.substr (0, dots);
        }
        return Value{{"command", st.command}, {"label", label}, {"params", st.params}, {"enabled", st.enabled},
            {"dialog", st.dialog}};
    }

    Value actions_json (const Session& s)
    {
        Value sets = Value::array ();
        for (size_t i = 0; i < s.actions.size (); i++)
        {
            Value acts = Value::array ();
            for (size_t a = 0; a < s.actions[i].actions.size (); a++)
            {
                const Action& ac = s.actions[i].actions[a];
                Value steps      = Value::array ();
                for (size_t k = 0; k < ac.steps.size (); k++)
                {
                    steps.push_back (step_json (s, ac.steps[k]));
                }
                acts.push_back (Value{{"name", ac.name}, {"steps", steps}});
            }
            sets.push_back (Value{{"name", s.actions[i].name}, {"actions", acts}});
        }
        Value v = {{"sets", sets}};
        if (s.recording.on)
        {
            v["recording"] = {{"set", s.recording.set}, {"action", s.recording.action}};
        }
        else
        {
            v["recording"] = nullptr;
        }
        return v;
    }

    bool sets_from_json (const Value& v, std::vector<ActionSet>* out, Error* err)
    {
        const Value* sets = param (v, "sets");
        if (sets == NULL || !sets->is_array ())
        {
            return fail (err, ErrorKind::Other, "not an actions file (no \"sets\")");
        }
        for (size_t i = 0; i < sets->size (); i++)
        {
            const Value& sv = (*sets)[i];
            ActionSet set;
            param_str (sv, "name", &set.name);
            const Value* acts = param (sv, "actions");
            for (size_t a = 0; acts != NULL && acts->is_array () && a < acts->size (); a++)
            {
                const Value& av = (*acts)[a];
                Action ac;
                param_str (av, "name", &ac.name);
                const Value* steps = param (av, "steps");
                for (size_t k = 0; steps != NULL && steps->is_array () && k < steps->size (); k++)
                {
                    const Value& stv = (*steps)[k];
                    ActionStep st    = {"", Value::object (), true, false};
                    if (!param_str (stv, "command", &st.command) || st.command.empty ())
                    {
                        continue;
                    }
                    const Value* pv = param (stv, "params");
                    if (pv != NULL && pv->is_object ())
                    {
                        st.params = *pv;
                    }
                    param_bool (stv, "enabled", &st.enabled);
                    param_bool (stv, "dialog", &st.dialog);
                    ac.steps.push_back (st);
                }
                set.actions.push_back (ac);
            }
            out->push_back (set);
        }
        return true;
    }

    /* Photoshop's Default Actions, as far as this build has the commands for them. */
    void default_actions (std::vector<ActionSet>* out)
    {
        ActionSet set;
        set.name = "Default Actions";
        Action a;
        a.name = "Sepia Toning (layer)";
        a.steps.push_back (ActionStep{"layer.duplicate", Value{{"name", "Sepia"}}, true, false});
        a.steps.push_back (ActionStep{"image.adjustments.desaturate", Value::object (), true, false});
        a.steps.push_back (ActionStep{"image.adjustments.photoFilter",
            Value{{"color", "#ac7a33"}, {"density", 60}, {"preserveLuminosity", true}}, true, false});
        set.actions.push_back (a);
        a      = Action ();
        a.name = "Soften (Gaussian Blur 2 px)";
        a.steps.push_back (ActionStep{"filter.blur.gaussianBlur", Value{{"radius", 2}}, true, true});
        set.actions.push_back (a);
        a      = Action ();
        a.name = "Sharpen for Web";
        a.steps.push_back (ActionStep{
            "filter.sharpen.unsharpMask", Value{{"amount", 80}, {"radius", 0.6}, {"threshold", 2}}, true, false});
        set.actions.push_back (a);
        a      = Action ();
        a.name = "White Border (50 px)";
        a.steps.push_back (ActionStep{"layer.flattenImage", Value::object (), true, false});
        a.steps.push_back (ActionStep{"image.canvasSize",
            Value{{"width", 100}, {"height", 100}, {"relative", true}, {"extensionColor", "white"}}, true, false});
        set.actions.push_back (a);
        out->push_back (set);
    }

    /* ---- Indices ---- */

    bool set_index (const Session& s, const Value& p, size_t* out, const char* cmd, Error* err)
    {
        int64_t i;
        if (!param_i64 (p, "set", &i) || i < 0 || (size_t) i >= s.actions.size ())
        {
            return bad_params (err, cmd, "no action set at `set`");
        }
        *out = (size_t) i;
        return true;
    }

    bool action_index (const Session& s, const Value& p, size_t* set, size_t* out, const char* cmd, Error* err)
    {
        if (!set_index (s, p, set, cmd, err))
        {
            return false;
        }
        int64_t i;
        if (!param_i64 (p, "action", &i) || i < 0 || (size_t) i >= s.actions[*set].actions.size ())
        {
            return bad_params (err, cmd, "no action at `action`");
        }
        *out = (size_t) i;
        return true;
    }

    bool step_index (
        const Session& s, const Value& p, size_t* set, size_t* action, size_t* out, const char* cmd, Error* err)
    {
        if (!action_index (s, p, set, action, cmd, err))
        {
            return false;
        }
        int64_t i;
        if (!param_i64 (p, "step", &i) || i < 0 || (size_t) i >= s.actions[*set].actions[*action].steps.size ())
        {
            return bad_params (err, cmd, "no step at `step`");
        }
        *out = (size_t) i;
        return true;
    }

    /* ---- Commands ---- */

    bool cmd_actions_list (Session& s, const Value& p, Value* result, Error* err)
    {
        *result = actions_json (s);
        return true;
    }

    bool cmd_actions_new_set (Session& s, const Value& p, Value* result, Error* err)
    {
        ActionSet set;
        set.name = "Set " + std::to_string (s.actions.size () + 1);
        param_str (p, "name", &set.name);
        s.actions.push_back (set);
        *result = Value{{"set", s.actions.size () - 1}};
        return true;
    }

    /* New Action: added to the set and, unless "record" is false, recording. */
    bool cmd_actions_new_action (Session& s, const Value& p, Value* result, Error* err)
    {
        size_t set;
        if (!set_index (s, p, &set, "actions.newAction", err))
        {
            return false;
        }
        Action a;
        a.name = "Action " + std::to_string (s.actions[set].actions.size () + 1);
        param_str (p, "name", &a.name);
        s.actions[set].actions.push_back (a);
        bool record = true;
        param_bool (p, "record", &record);
        s.recording.on     = record;
        s.recording.set    = set;
        s.recording.action = s.actions[set].actions.size () - 1;
        *result            = Value{{"set", set}, {"action", s.recording.action}};
        return true;
    }

    /* Record: carries on recording into an existing action. */
    bool cmd_actions_record (Session& s, const Value& p, Value* result, Error* err)
    {
        size_t set, action;
        if (!action_index (s, p, &set, &action, "actions.record", err))
        {
            return false;
        }
        s.recording.on     = true;
        s.recording.set    = set;
        s.recording.action = action;
        return true;
    }

    bool cmd_actions_stop (Session& s, const Value& p, Value* result, Error* err)
    {
        s.recording.on = false;
        return true;
    }

    /* Play: the action's enabled steps from "from" (or only "step"), each its own history step.
       A step that asks for its dialog stops playback (with "dialogs" on) and is returned. */
    bool cmd_actions_play (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "actions.play";
        size_t set, action;
        if (!action_index (s, p, &set, &action, cmd, err))
        {
            return false;
        }
        if (s.recording.on && s.recording.set == set && s.recording.action == action)
        {
            return fail (err, ErrorKind::Other, "stop recording the action before playing it");
        }
        std::vector<ActionStep> steps = s.actions[set].actions[action].steps;
        int64_t from = 0, only = -1;
        param_i64 (p, "from", &from);
        param_i64 (p, "step", &only);
        bool dialogs = true;
        param_bool (p, "dialogs", &dialogs);
        size_t begin = only >= 0 ? (size_t) only : (size_t) std::max<int64_t> (from, 0);
        size_t end   = only >= 0 ? std::min (begin + 1, steps.size ()) : steps.size ();
        size_t ran   = 0;
        for (size_t i = begin; i < end; i++)
        {
            const ActionStep& st = steps[i];
            if (!st.enabled)
            {
                continue;
            }
            if (dialogs && st.dialog && only < 0)
            {
                *result = Value{{"paused", i}, {"command", st.command}, {"params", st.params}, {"ran", ran}};
                return true;
            }
            Value params, r;
            Error e;
            if (!resolve_params (s, st.params, &params, &e) || !s.execute (st.command.c_str (), params, &r, &e))
            {
                const CommandSpec* spec = s.find_command (st.command.c_str ());
                return fail (err, ErrorKind::Other, "step %zu (%s): %s", i + 1,
                    spec != NULL ? spec->label : st.command.c_str (), e.message.c_str ());
            }
            ran++;
        }
        *result = Value{{"done", true}, {"ran", ran}};
        return true;
    }

    /* Delete a step, an action ("action" without "step") or a set. */
    bool cmd_actions_delete (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "actions.delete";
        size_t set, action, step;
        if (param_present (p, "step"))
        {
            if (!step_index (s, p, &set, &action, &step, cmd, err))
            {
                return false;
            }
            std::vector<ActionStep>& v = s.actions[set].actions[action].steps;
            v.erase (v.begin () + (ptrdiff_t) step);
            return true;
        }
        if (param_present (p, "action"))
        {
            if (!action_index (s, p, &set, &action, cmd, err))
            {
                return false;
            }
            std::vector<Action>& v = s.actions[set].actions;
            v.erase (v.begin () + (ptrdiff_t) action);
            if (s.recording.on && s.recording.set == set)
            {
                if (s.recording.action == action)
                {
                    s.recording.on = false;
                }
                else if (s.recording.action > action)
                {
                    s.recording.action--;
                }
            }
            return true;
        }
        if (!set_index (s, p, &set, cmd, err))
        {
            return false;
        }
        s.actions.erase (s.actions.begin () + (ptrdiff_t) set);
        if (s.recording.on && s.recording.set == set)
        {
            s.recording.on = false;
        }
        else if (s.recording.on && s.recording.set > set)
        {
            s.recording.set--;
        }
        return true;
    }

    bool cmd_actions_rename (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "actions.rename";
        std::string name;
        if (!param_str (p, "name", &name) || name.empty ())
        {
            return bad_params (err, cmd, "`name` must be a non-empty string");
        }
        size_t set, action;
        if (param_present (p, "action"))
        {
            if (!action_index (s, p, &set, &action, cmd, err))
            {
                return false;
            }
            s.actions[set].actions[action].name = name;
        }
        else
        {
            if (!set_index (s, p, &set, cmd, err))
            {
                return false;
            }
            s.actions[set].name = name;
        }
        return true;
    }

    bool cmd_actions_duplicate (Session& s, const Value& p, Value* result, Error* err)
    {
        size_t set, action;
        if (!action_index (s, p, &set, &action, "actions.duplicate", err))
        {
            return false;
        }
        Action copy = s.actions[set].actions[action];
        copy.name += " copy";
        s.actions[set].actions.insert (s.actions[set].actions.begin () + (ptrdiff_t) action + 1, copy);
        *result = Value{{"action", action + 1}};
        return true;
    }

    /* A step's checkbox ("enabled"), dialog toggle ("dialog") or settings ("params"). */
    bool cmd_actions_set_step (Session& s, const Value& p, Value* result, Error* err)
    {
        size_t set, action, step;
        if (!step_index (s, p, &set, &action, &step, "actions.setStep", err))
        {
            return false;
        }
        ActionStep& st = s.actions[set].actions[action].steps[step];
        param_bool (p, "enabled", &st.enabled);
        param_bool (p, "dialog", &st.dialog);
        const Value* pv = param (p, "params");
        if (pv != NULL && pv->is_object ())
        {
            st.params = *pv;
        }
        return true;
    }

    bool cmd_actions_move_step (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "actions.moveStep";
        size_t set, action, step;
        if (!step_index (s, p, &set, &action, &step, cmd, err))
        {
            return false;
        }
        std::vector<ActionStep>& v = s.actions[set].actions[action].steps;
        int64_t to;
        if (!param_i64 (p, "to", &to) || to < 0 || (size_t) to >= v.size ())
        {
            return bad_params (err, cmd, "`to` must be a step index");
        }
        ActionStep st = v[step];
        v.erase (v.begin () + (ptrdiff_t) step);
        v.insert (v.begin () + (ptrdiff_t) to, st);
        return true;
    }

    bool cmd_actions_export (Session& s, const Value& p, Value* result, Error* err)
    {
        std::string path;
        if (!param_str (p, "path", &path) || path.empty ())
        {
            return bad_params (err, "actions.export", "missing `path`");
        }
        Value v = actions_json (s);
        v.erase ("recording");
        for (size_t i = 0; i < v["sets"].size (); i++)
        {
            for (size_t a = 0; a < v["sets"][i]["actions"].size (); a++)
            {
                for (size_t k = 0; k < v["sets"][i]["actions"][a]["steps"].size (); k++)
                {
                    v["sets"][i]["actions"][a]["steps"][k].erase ("label");
                }
            }
        }
        v["format"]      = "photoc-actions";
        v["version"]     = 1;
        std::string text = v.dump (2);
        char msg[512]    = "";
        if (!io::write_file (path.c_str (), std::vector<uint8_t> (text.begin (), text.end ()), msg, sizeof msg))
        {
            return fail (err, ErrorKind::Other, "%s", msg);
        }
        return true;
    }

    /* Load Actions: sets from a file, added (or, with "replace", instead of the current ones). An
       empty or missing file with "defaults" gives the Default Actions. */
    bool cmd_actions_import (Session& s, const Value& p, Value* result, Error* err)
    {
        std::string path;
        if (!param_str (p, "path", &path) || path.empty ())
        {
            return bad_params (err, "actions.import", "missing `path`");
        }
        bool replace = false, defaults = false;
        param_bool (p, "replace", &replace);
        param_bool (p, "defaults", &defaults);
        std::vector<uint8_t> bytes;
        char msg[512] = "";
        std::vector<ActionSet> sets;
        if (!io::read_file (path.c_str (), &bytes, msg, sizeof msg))
        {
            if (!defaults)
            {
                return fail (err, ErrorKind::Other, "%s", msg);
            }
            default_actions (&sets);
        }
        else
        {
            Value v = Value::parse (bytes.begin (), bytes.end (), nullptr, false);
            if (v.is_discarded ())
            {
                return fail (err, ErrorKind::Other, "%s is not an actions file", path.c_str ());
            }
            if (!sets_from_json (v, &sets, err))
            {
                return false;
            }
        }
        if (replace)
        {
            s.actions      = sets;
            s.recording.on = false;
        }
        else
        {
            s.actions.insert (s.actions.end (), sets.begin (), sets.end ());
        }
        *result = Value{{"sets", sets.size ()}};
        return true;
    }

    /* File > Automate > Batch: the action played on every image in a folder (no dialogs), each
       saved into the destination folder in "format" (default: Photoshop) or, without a destination,
       over the original when it is a PSD. */
    bool cmd_batch (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "file.automate.batch";
        size_t set, action;
        if (!action_index (s, p, &set, &action, cmd, err))
        {
            return false;
        }
        std::string source, dest, format = "psd";
        if (!param_str (p, "source", &source) || source.empty ())
        {
            return bad_params (err, cmd, "missing `source` (a folder)");
        }
        param_str (p, "destination", &dest);
        param_str (p, "format", &format);
        if (!dest.empty () && format != "psd")
        {
            codecs::Format f;
            if (!codecs::from_extension (format.c_str (), &f) || !codecs::format_can_encode (f))
            {
                return bad_params (err, cmd, "can't save as `%s`", format.c_str ());
            }
        }
        // Paths are UTF-8 everywhere (on Windows through the app manifest), so go through u8 strings
        std::error_code ec;
        std::filesystem::directory_iterator dir (
            std::u8string_view ((const char8_t*) source.data (), source.size ()), ec);
        if (ec)
        {
            return fail (err, ErrorKind::Other, "can't read the folder %s", source.c_str ());
        }
        std::vector<std::string> files;
        for (const std::filesystem::directory_entry& e : dir)
        {
            std::u8string u8   = e.path ().filename ().u8string ();
            std::string name ((const char*) u8.data (), u8.size ());
            codecs::Format f;
            size_t n = name.size ();
            bool psd =
                n > 4
                && (strcasecmp (name.c_str () + n - 4, ".psd") == 0 || strcasecmp (name.c_str () + n - 4, ".psb") == 0);
            if (name[0] != '.' && (psd || codecs::from_extension (name.c_str (), &f)))
            {
                files.push_back (name);
            }
        }
        std::sort (files.begin (), files.end ());
        Value errors = Value::array ();
        size_t done = 0, start_docs = s.doc_count ();
        compose::RenderSettings rs = compose::default_settings ();
        rs.cmyk                    = s.cmyk;
        for (size_t i = 0; i < files.size (); i++)
        {
            std::string in = source + "/" + files[i];
            io::ImportResult r;
            char msg[512] = "";
            if (!io::open_file (s.ids, s.fonts (), in.c_str (), &r, msg, sizeof msg))
            {
                errors.push_back (files[i] + ": " + msg);
                continue;
            }
            size_t index = s.add_document (r.document, in);
            Value pr;
            Error e;
            bool ok = s.execute ("actions.play", Value{{"set", set}, {"action", action}, {"dialogs", false}}, &pr, &e);
            if (!ok)
            {
                errors.push_back (files[i] + ": " + e.message);
            }
            std::string out;
            if (ok)
            {
                if (dest.empty ())
                {
                    size_t n = in.size ();
                    bool psd = n > 4 && strcasecmp (in.c_str () + n - 4, ".psd") == 0;
                    out      = psd ? in : in.substr (0, in.find_last_of ('.')) + ".psd";
                }
                else
                {
                    out = dest + "/" + files[i].substr (0, files[i].find_last_of ('.')) + "." + format;
                }
                std::vector<std::string> warnings;
                if (!io::save_file (*s.doc_at (index)->doc, s.fonts (), out.c_str (), NULL,
                        codecs::EncodeOptions::defaults (), rs, &warnings, msg, sizeof msg))
                {
                    errors.push_back (files[i] + ": " + msg);
                    ok = false;
                }
            }
            if (ok)
            {
                done++;
            }
            s.close (index);
        }
        if (start_docs > 0)
        {
            s.set_active (std::min (s.doc_count () - 1, start_docs - 1));
        }
        *result = Value{{"processed", done}, {"files", files.size ()}, {"errors", errors}};
        return true;
    }

    bool has_actions (const Session& s, std::string* why)
    {
        if (!s.actions.empty ())
        {
            return true;
        }
        *why = "there are no actions";
        return false;
    }

    bool is_recording (const Session& s, std::string* why)
    {
        if (s.recording.on)
        {
            return true;
        }
        *why = "no action is recording";
        return false;
    }

    const CommandSpec ACTION_COMMANDS[] = {
        {"actions.list", "Actions", "", NULL,
            "{} → {sets:[{name, actions:[{name, steps:[{command, label, params, enabled, dialog}]}]}], recording}",
            always, cmd_actions_list, false},
        {"actions.newSet", "New Set…", "", NULL, "{\"name\":str?}", always, cmd_actions_new_set, false},
        {"actions.newAction", "New Action…", "", NULL, "{\"set\":n,\"name\":str?,\"record\":bool=true}", has_actions,
            cmd_actions_new_action, false},
        {"actions.record", "Start Recording", "", NULL, "{\"set\":n,\"action\":n}", has_actions, cmd_actions_record,
            false},
        {"actions.stop", "Stop Recording", "", NULL, "{}", is_recording, cmd_actions_stop, false},
        {"actions.play", "Play", "", NULL,
            "{\"set\":n,\"action\":n,\"from\":step?,\"step\":n? (only that step),\"dialogs\":bool=true} → {done} | "
            "{paused, command, params}",
            has_actions, cmd_actions_play, false},
        {"actions.delete", "Delete", "", NULL, "{\"set\":n,\"action\":n?,\"step\":n?}", has_actions, cmd_actions_delete,
            false},
        {"actions.rename", "Rename", "", NULL, "{\"set\":n,\"action\":n?,\"name\":str}", has_actions,
            cmd_actions_rename, false},
        {"actions.duplicate", "Duplicate", "", NULL, "{\"set\":n,\"action\":n}", has_actions, cmd_actions_duplicate,
            false},
        {"actions.setStep", "Step Options", "", NULL,
            "{\"set\":n,\"action\":n,\"step\":n,\"enabled\":bool?,\"dialog\":bool?,\"params\":{…}?}", has_actions,
            cmd_actions_set_step, false},
        {"actions.moveStep", "Move Step", "", NULL, "{\"set\":n,\"action\":n,\"step\":n,\"to\":n}", has_actions,
            cmd_actions_move_step, false},
        {"actions.export", "Save Actions…", "", NULL, "{\"path\":str} (JSON)", always, cmd_actions_export, false},
        {"actions.import", "Load Actions…", "", NULL, "{\"path\":str,\"replace\":bool=false,\"defaults\":bool=false}",
            always, cmd_actions_import, false},
        {"file.automate.batch", "Batch…", "File/Automate", NULL,
            "{\"set\":n,\"action\":n,\"source\":folder,\"destination\":folder?,\"format\":\"psd|png|jpg|tif|…\"="
            "\"psd\"}",
            has_actions, cmd_batch, false},
    };

    void register_action_commands (std::vector<CommandSpec>* out)
    {
        out->insert (
            out->end (), ACTION_COMMANDS, ACTION_COMMANDS + sizeof ACTION_COMMANDS / sizeof ACTION_COMMANDS[0]);
    }

} // namespace engine
