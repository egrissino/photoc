#include "engine/session.h"

#include "io/io.h"
#include "text/fonts.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>

namespace engine
{

    const size_t NO_DOC = (size_t) -1;

    ops::LayerTarget DocState::layer_target () const
    {
        ops::LayerTarget t;
        t.active   = active_layer;
        t.selected = selected_layers;
        return t;
    }

    bool DocState::dirty () const
    {
        return revision != saved_revision;
    }

    bool contains (const std::vector<doc::LayerId>& ids, doc::LayerId id)
    {
        return std::find (ids.begin (), ids.end (), id) != ids.end ();
    }

    /* The targeted layers in document order (bottom to top): the selection when it includes the
       active layer, else the active layer alone. */
    std::vector<doc::LayerId> DocState::selected () const
    {
        std::vector<doc::LayerId> out;
        if (active_layer == doc::NO_LAYER)
        {
            return out;
        }
        bool use_set                     = contains (selected_layers, active_layer);
        std::vector<doc::WalkEntry> walk = doc->walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            doc::LayerId id = walk[i].layer->id;
            if (use_set ? contains (selected_layers, id) : id == active_layer)
            {
                out.push_back (id);
            }
        }
        return out;
    }

    bool DocState::is_selected (doc::LayerId id) const
    {
        if (active_layer == doc::NO_LAYER)
        {
            return false;
        }
        if (id == active_layer)
        {
            return true;
        }
        return contains (selected_layers, active_layer) && contains (selected_layers, id);
    }

    /* The selection always holds the active layer, only live layers, and no duplicates. */
    void DocState::fix_selection ()
    {
        if (active_layer == doc::NO_LAYER || doc->layer (active_layer) == NULL)
        {
            active_layer = doc::NO_LAYER;
            selected_layers.clear ();
            layer_anchor = doc::NO_LAYER;
            return;
        }
        if (!contains (selected_layers, active_layer))
        {
            selected_layers.assign (1, active_layer);
        }
        else
        {
            std::vector<doc::LayerId> kept;
            for (size_t i = 0; i < selected_layers.size (); i++)
            {
                doc::LayerId id = selected_layers[i];
                if (doc->layer (id) != NULL && !contains (kept, id))
                {
                    kept.push_back (id);
                }
            }
            selected_layers = kept;
        }
        if (!contains (selected_layers, layer_anchor))
        {
            layer_anchor = active_layer;
        }
    }

    Session::~Session () {}

    /* The fonts type uses, scanned on first use (Fontconfig takes a moment). */
    text::FontDb* Session::fonts ()
    {
        if (!fonts_)
        {
            fonts_.reset (new text::FontDb ());
        }
        return fonts_.get ();
    }

    Session::Session () : timing (false), cmyk (NULL), running_ (NULL), active_ (NO_DOC)
    {
        recording.on  = false;
        recording.set = recording.action = 0;
        tools.foreground[0] = tools.foreground[1] = tools.foreground[2] = 0.0f;
        tools.foreground[3]                                             = 1.0f;
        tools.background[0] = tools.background[1] = tools.background[2] = 1.0f;
        tools.background[3]                                             = 1.0f;
        tools.brush                                                     = paint::BrushSettings::defaults ();
        live_stroke.valid                                               = false;
        quick.active                                                    = false;
        quick.doc                                                       = 0;
        quick.revision                                                  = 0;
        quick.size                                                      = 30.0f;
        quick.all_layers                                                = false;
        quick.done                                                      = 0;
        clipboard.full                                                  = false;
        swatches                                                        = builtin_swatches ();
        has_style_clipboard                                             = false;
        live_stroke.doc                                                 = 0;
        live_stroke.revision                                            = 0;
        register_core_commands (&commands_);
        register_layer_commands (&commands_);
        register_edit_commands (&commands_);
        register_adjustment_commands (&commands_);
        register_brush_commands (&commands_);
        register_layer_tree_commands (&commands_);
        register_mask_commands (&commands_);
        register_clipboard_commands (&commands_);
        register_transform_commands (&commands_);
        register_select_commands (&commands_);
        register_fill_tool_commands (&commands_);
        register_image_commands (&commands_);
        register_retouch_commands (&commands_);
        register_swatch_commands (&commands_);
        register_channel_commands (&commands_);
        register_filter_commands (&commands_);
        register_mode_commands (&commands_);
        register_fill_layer_commands (&commands_);
        register_comps_commands (&commands_);
        register_misc_commands (&commands_);
        register_sample_commands (&commands_);
        register_style_commands (&commands_);
        register_text_commands (&commands_);
        register_view_commands (&commands_);
        register_vector_commands (&commands_);
        register_align_commands (&commands_);
        register_smart_commands (&commands_);
        register_action_commands (&commands_);
        register_brush_tool_commands (&commands_);
        register_warp_commands (&commands_);
        register_tone_commands (&commands_);
        register_apply_image_commands (&commands_);
    }

    const std::vector<CommandSpec>& Session::commands () const
    {
        return commands_;
    }

    const CommandSpec* Session::find_command (const char* id) const
    {
        for (size_t i = 0; i < commands_.size (); i++)
        {
            if (strcmp (commands_[i].id, id) == 0)
            {
                return &commands_[i];
            }
        }
        return NULL;
    }

    bool Session::command_enabled (const CommandSpec& spec, std::string* why) const
    {
        return spec.enabled == NULL || spec.enabled (*this, why);
    }

    /* Every action from the UI, scripts and keyboard shortcuts goes through here. */
    bool Session::execute (const char* id, const Value& params, Value* result, Error* err)
    {
        const CommandSpec* spec = find_command (id);
        if (spec == NULL)
        {
            return fail (err, ErrorKind::UnknownCommand, "unknown command `%s`", id);
        }
        std::string why;
        if (!command_enabled (*spec, &why))
        {
            return fail (err, ErrorKind::Disabled, "command `%s` is not available right now: %s", id, why.c_str ());
        }
        Value ignored;
        /* While an action records, top-level edits become its steps (their settings taken before
           the command runs, while layer ids still name what the user picked). */
        bool record    = recording.on && running_ == NULL && spec->journal && action_recordable (id);
        Value recorded = record ? action_params (*this, params) : Value ();
        /* The command being run, for run functions shared by several commands (filters). */
        const char* outer = running_;
        running_          = spec->id;
        double t0         = timing_now ();
        bool ok           = spec->run (*this, params, result != NULL ? result : &ignored, err);
        if (outer == NULL)
        {
            timing_log (*this, spec->id, timing_now () - t0);
        }
        running_ = outer;
        if (!ok)
        {
            return false;
        }
        if (record && recording.set < actions.size () && recording.action < actions[recording.set].actions.size ())
        {
            ActionStep st = {id, recorded, true, false};
            actions[recording.set].actions[recording.action].steps.push_back (st);
        }
        if (spec->journal)
        {
            JournalEntry e;
            e.id     = id;
            e.params = params;
            journal.push_back (e);
        }
        return true;
    }

    const char* Session::running () const
    {
        return running_;
    }

    size_t Session::doc_count () const
    {
        return docs_.size ();
    }

    DocState* Session::doc_at (size_t index)
    {
        return index < docs_.size () ? &docs_[index] : NULL;
    }

    const DocState* Session::doc_at (size_t index) const
    {
        return index < docs_.size () ? &docs_[index] : NULL;
    }

    DocState* Session::active ()
    {
        return doc_at (active_);
    }

    const DocState* Session::active () const
    {
        return doc_at (active_);
    }

    bool Session::has_active () const
    {
        return active () != NULL;
    }

    size_t Session::active_index () const
    {
        return active_;
    }

    void Session::set_active (size_t index)
    {
        if (index < docs_.size ())
        {
            active_ = index;
        }
    }

    bool Session::doc_id_in_use (doc::DocId id) const
    {
        for (size_t i = 0; i < docs_.size (); i++)
        {
            if (docs_[i].doc->id == id)
            {
                return true;
            }
        }
        return false;
    }

    size_t Session::add_document (const doc::Document& d, const std::string& path)
    {
        doc::Document copy = d;
        while (doc_id_in_use (copy.id))
        {
            copy.id = ids.fresh ();
        }
        DocState st;
        st.active_layer = copy.layers.empty () ? doc::NO_LAYER : copy.layers.back ().id;
        if (st.active_layer != doc::NO_LAYER)
        {
            st.selected_layers.push_back (st.active_layer);
        }
        st.layer_anchor   = st.active_layer;
        st.solo_layer     = doc::NO_LAYER;
        st.doc            = std::make_shared<const doc::Document> (copy);
        st.path           = path;
        st.revision       = 0;
        st.saved_revision = 0;
        st.has_damage     = false;
        st.smart_parent   = 0;
        st.history.set_current_layers (st.layer_target ());
        docs_.push_back (st);
        active_ = docs_.size () - 1;
        return active_;
    }

    bool Session::close (size_t index)
    {
        if (index >= docs_.size ())
        {
            return false;
        }
        docs_.erase (docs_.begin () + (ptrdiff_t) index);
        if (docs_.empty ())
        {
            active_ = NO_DOC;
        }
        else if (active_ != NO_DOC && active_ > index)
        {
            active_ -= 1;
        }
        else if (active_ == index || active_ == NO_DOC)
        {
            active_ = std::min (index, docs_.size () - 1);
        }
        return true;
    }

    /* After the active document was written to `path`: it is clean, saves there next time, and takes
       the file's name. File identity is not a history step, so undo keeps the new name. */
    void Session::mark_saved (const std::string& path, const std::string& name)
    {
        DocState* st = active ();
        if (st == NULL)
        {
            return;
        }
        if (st->doc->name != name)
        {
            std::shared_ptr<doc::Document> renamed = std::make_shared<doc::Document> (*st->doc);
            renamed->name                          = name;
            st->doc                                = renamed;
        }
        st->path           = path;
        st->saved_revision = st->revision;
    }

    void restore_target (DocState* st, const ops::LayerTarget& t)
    {
        st->active_layer = st->doc->layer (t.active) != NULL ? t.active : doc::NO_LAYER;
        st->selected_layers.clear ();
        for (size_t i = 0; i < t.selected.size (); i++)
        {
            if (st->doc->layer (t.selected[i]) != NULL)
            {
                st->selected_layers.push_back (t.selected[i]);
            }
        }
        st->fix_selection ();
    }

    /* Moving through history keeps the document's current name: Save As changes file identity
       outside history. */
    ops::DocRef keep_name (ops::DocRef d, const std::string& name)
    {
        if (d->name == name)
        {
            return d;
        }
        std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d);
        copy->name                          = name;
        return copy;
    }

    bool Session::undo ()
    {
        DocState* st = active ();
        if (st == NULL)
        {
            return false;
        }
        ops::DocRef d;
        ops::LayerTarget layers;
        if (!st->history.undo (st->doc, &d, &layers))
        {
            return false;
        }
        st->doc = keep_name (d, st->doc->name);
        restore_target (st, layers);
        st->revision++;
        st->has_damage = false;
        return true;
    }

    bool Session::redo ()
    {
        DocState* st = active ();
        if (st == NULL)
        {
            return false;
        }
        ops::DocRef d;
        ops::LayerTarget layers;
        if (!st->history.redo (st->doc, &d, &layers))
        {
            return false;
        }
        st->doc = keep_name (d, st->doc->name);
        restore_target (st, layers);
        st->revision++;
        st->has_damage = false;
        return true;
    }

    /* One history step. The constructor takes a working copy of the active document (cheap: tiles
       are shared); commit() makes it current and records the step. Destroying an Edit without
       committing discards every change, so error paths just return. */
    Edit::Edit (Session& s) : session_ (s), state_ (s.active ()), active_ (doc::NO_LAYER)
    {
        if (state_ != NULL)
        {
            working_  = *state_->doc;
            active_   = state_->active_layer;
            selected_ = state_->selected_layers;
        }
    }

    doc::Document* Edit::doc ()
    {
        return state_ != NULL ? &working_ : NULL;
    }

    doc::LayerId* Edit::active_layer ()
    {
        return &active_;
    }

    /* The layer selection the step leaves behind (structural edits select what they made). */
    void Edit::select (const std::vector<doc::LayerId>& ids, doc::LayerId active)
    {
        selected_ = ids;
        active_   = active;
        if (active != doc::NO_LAYER && !contains (selected_, active))
        {
            selected_.push_back (active);
        }
    }

    bool Edit::commit (const char* label, Error* err)
    {
        if (state_ == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        ops::LayerTarget prior  = state_->layer_target ();
        ops::DocRef before      = state_->doc;
        state_->doc             = std::make_shared<const doc::Document> (working_);
        state_->active_layer    = active_;
        state_->selected_layers = selected_;
        state_->fix_selection ();

        state_->history.set_current_layers (prior);
        state_->history.record (label, before, state_->layer_target ());
        state_->history.trim (*state_->doc);
        state_->revision++;
        state_->has_damage = false;
        return true;
    }

    bool always (const Session& s, std::string* why)
    {
        return true;
    }

    /* With timing on (PHOTOC_TIMING=1), how long a step took, on stderr. */
    double timing_now ()
    {
        return io::monotonic_seconds ();
    }

    void timing_log (const Session& s, const char* what, double seconds)
    {
        if (s.timing)
        {
            fprintf (stderr, "[timing] %-34s %9.1f ms\n", what, seconds * 1000.0);
        }
    }

    bool has_doc (const Session& s, std::string* why)
    {
        if (s.has_active ())
        {
            return true;
        }
        *why = "no document open";
        return false;
    }

    bool has_layer (const Session& s, std::string* why)
    {
        const DocState* d = s.active ();
        if (d == NULL)
        {
            *why = "no document open";
            return false;
        }
        if (d->active_layer == doc::NO_LAYER || d->doc->layer (d->active_layer) == NULL)
        {
            *why = "no active layer";
            return false;
        }
        return true;
    }

    bool has_selection (const Session& s, std::string* why)
    {
        const DocState* d = s.active ();
        if (d == NULL)
        {
            *why = "no document open";
            return false;
        }
        if (!d->doc->has_selection)
        {
            *why = "no selection";
            return false;
        }
        return true;
    }

    bool can_undo (const Session& s, std::string* why)
    {
        const DocState* d = s.active ();
        if (d != NULL && d->history.can_undo ())
        {
            return true;
        }
        *why = "nothing to undo";
        return false;
    }

    bool can_redo (const Session& s, std::string* why)
    {
        const DocState* d = s.active ();
        if (d != NULL && d->history.can_redo ())
        {
            return true;
        }
        *why = "nothing to redo";
        return false;
    }

} // namespace engine
