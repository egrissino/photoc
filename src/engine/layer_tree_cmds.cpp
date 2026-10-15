#include "compose/compose.h"
#include "engine/layers.h"
#include "engine/session.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

namespace engine
{

    /* The layer tree: selecting layers, groups, stacking order, visibility, locks, links, clipping,
       merging and Layer via Copy / Cut. */

    using geom::Rect;

    bool has_id (const std::vector<doc::LayerId>& ids, doc::LayerId id)
    {
        return std::find (ids.begin (), ids.end (), id) != ids.end ();
    }

    bool path_starts_with (const doc::LayerPath& path, const doc::LayerPath& prefix)
    {
        return prefix.size () <= path.size () && std::equal (prefix.begin (), prefix.end (), path.begin ());
    }

    /* No "layer" param and more than one layer selected: the command acts on the whole selection. */
    bool multi_target (const Session& s, const Value& p)
    {
        const DocState* d = s.active ();
        return d != NULL && !param_present (p, "layer") && d->selected ().size () > 1;
    }

    /* The "layer" param, else every selected layer (bottom to top), else the active layer. */
    bool layer_targets (const Session& s, const Value& p, std::vector<doc::LayerId>* out, Error* err)
    {
        out->clear ();
        if (multi_target (s, p))
        {
            *out = s.active ()->selected ();
            return true;
        }
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        out->push_back (id);
        return true;
    }

    /* The ids with no ancestor also in ids (a selected group already carries its children), in
       document order. */
    std::vector<doc::LayerId> top_level (const doc::Document& d, const std::vector<doc::LayerId>& ids)
    {
        std::vector<doc::WalkEntry> walk = d.walk ();
        std::vector<const doc::WalkEntry*> picked;
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (has_id (ids, walk[i].layer->id))
            {
                picked.push_back (&walk[i]);
            }
        }
        std::vector<doc::LayerId> out;
        for (size_t i = 0; i < picked.size (); i++)
        {
            bool nested = false;
            for (size_t k = 0; k < picked.size () && !nested; k++)
            {
                nested = picked[k]->path.size () < picked[i]->path.size ()
                      && path_starts_with (picked[i]->path, picked[k]->path);
            }
            if (!nested)
            {
                out.push_back (picked[i]->layer->id);
            }
        }
        return out;
    }

    size_t group_depth (const std::vector<doc::Layer>& layers)
    {
        size_t deepest = 0;
        for (size_t i = 0; i < layers.size (); i++)
        {
            if (!layers[i].is_group ())
            {
                continue;
            }
            size_t depth = 1 + group_depth (layers[i].children);
            if (depth > deepest)
            {
                deepest = depth;
            }
        }
        return deepest;
    }

    bool check_group_depth (const doc::Document& d, const char* what, Error* err)
    {
        if (group_depth (d.layers) <= doc::MAX_GROUP_DEPTH)
        {
            return true;
        }
        return fail (err, ErrorKind::Other, "%s would nest layers deeper than %zu groups", what, doc::MAX_GROUP_DEPTH);
    }

    /* The Background (taken before the edit) stays at the bottom of the stack. */
    bool keep_background_at_bottom (doc::LayerId bg, const doc::Document& d, Error* err)
    {
        if (bg == doc::NO_LAYER || (!d.layers.empty () && d.layers[0].id == bg))
        {
            return true;
        }
        return fail (err, ErrorKind::Other,
            "the Background layer stays at the bottom of the stack (Layer › New › Layer from Background makes it a "
            "normal layer)");
    }

    doc::LayerId background_id (const doc::Document& d)
    {
        const doc::Layer* b = d.background ();
        return b != NULL ? b->id : doc::NO_LAYER;
    }

    /* Refuses a layer whose pixels are locked, directly or through a group, with Photoshop's
       message. */
    bool check_pixels_unlocked (const doc::Document& d, doc::LayerId id, Error* err)
    {
        const doc::Layer* l = d.layer (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        doc::Locks locks = d.effective_locks (id);
        if (!locks.pixels && !locks.all)
        {
            return true;
        }
        return fail (err, ErrorKind::Other, "Could not complete your request because the layer \"%s\" is locked",
            l->name.c_str ());
    }

    /* Selecting layers is not an edit: it does not dirty a clean document. */
    bool set_layer_selection (
        Session& s, const std::vector<doc::LayerId>& ids, doc::LayerId active, doc::LayerId anchor, Error* err)
    {
        DocState* st = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        for (size_t i = 0; i < ids.size (); i++)
        {
            if (st->doc->layer (ids[i]) == NULL)
            {
                return no_layer (err, ids[i]);
            }
        }
        if (active != doc::NO_LAYER && st->doc->layer (active) == NULL)
        {
            return no_layer (err, active);
        }
        if (active == doc::NO_LAYER && !ids.empty ())
        {
            active = ids.back ();
        }
        st->selected_layers = ids;
        if (active != doc::NO_LAYER && !has_id (st->selected_layers, active))
        {
            st->selected_layers.push_back (active);
        }
        st->active_layer = active;
        st->layer_anchor = anchor != doc::NO_LAYER ? anchor : active;
        st->fix_selection ();
        bool clean = st->saved_revision == st->revision;
        st->revision++;
        if (clean)
        {
            st->saved_revision = st->revision;
        }
        return true;
    }

    Value id_array (const std::vector<doc::LayerId>& ids)
    {
        Value out = Value::array ();
        for (size_t i = 0; i < ids.size (); i++)
        {
            out.push_back (ids[i]);
        }
        return out;
    }

    /* Layers panel rows top to bottom; closed groups hide their children unless `all`. */
    void range_order (const std::vector<doc::Layer>& layers, bool all, std::vector<doc::LayerId>* out)
    {
        for (size_t i = layers.size (); i-- > 0;)
        {
            out->push_back (layers[i].id);
            if (layers[i].is_group () && (layers[i].expanded || all))
            {
                range_order (layers[i].children, all, out);
            }
        }
    }

    /* Click (replace), ⌘-click (toggle), ⇧-click (range from the anchor) or add. */
    bool cmd_layer_select (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.select";
        uint64_t id;
        if (!param_u64 (p, "layer", &id))
        {
            return bad_params (err, cmd, "missing `layer`");
        }
        std::string mode = "replace";
        if (param_present (p, "mode") && !param_str (p, "mode", &mode))
        {
            return bad_params (err, cmd, "`mode` must be a string");
        }
        DocState* st = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (st->doc->layer (id) == NULL)
        {
            return no_layer (err, id);
        }
        std::vector<doc::LayerId> sel = st->selected ();
        if (mode == "replace")
        {
            if (!set_layer_selection (s, std::vector<doc::LayerId> (1, id), id, id, err))
            {
                return false;
            }
        }
        else if (mode == "add")
        {
            if (!has_id (sel, id))
            {
                sel.push_back (id);
            }
            if (!set_layer_selection (s, sel, id, id, err))
            {
                return false;
            }
        }
        else if (mode == "toggle")
        {
            if (has_id (sel, id))
            {
                if (sel.size () > 1)
                {
                    sel.erase (std::find (sel.begin (), sel.end (), id));
                    doc::LayerId active = st->active_layer == id ? sel.back () : st->active_layer;
                    if (!set_layer_selection (s, sel, active, active, err))
                    {
                        return false;
                    }
                }
            }
            else
            {
                sel.push_back (id);
                if (!set_layer_selection (s, sel, id, id, err))
                {
                    return false;
                }
            }
        }
        else if (mode == "range")
        {
            doc::LayerId anchor = st->layer_anchor;
            if (anchor == doc::NO_LAYER || st->doc->layer (anchor) == NULL)
            {
                anchor = st->active_layer;
            }
            if (anchor == doc::NO_LAYER)
            {
                anchor = id;
            }
            std::vector<doc::LayerId> order;
            range_order (st->doc->layers, false, &order);
            if (!has_id (order, anchor) || !has_id (order, id))
            {
                order.clear ();
                range_order (st->doc->layers, true, &order);
            }
            size_t a = std::find (order.begin (), order.end (), anchor) - order.begin ();
            size_t b = std::find (order.begin (), order.end (), (doc::LayerId) id) - order.begin ();
            if (a >= order.size ())
            {
                a = 0;
            }
            if (b >= order.size ())
            {
                b = 0;
            }
            std::vector<doc::LayerId> range (
                order.begin () + (ptrdiff_t) std::min (a, b), order.begin () + (ptrdiff_t) std::max (a, b) + 1);
            if (!set_layer_selection (s, range, id, anchor, err))
            {
                return false;
            }
        }
        else
        {
            return bad_params (err, cmd, "unknown mode `%s`", mode.c_str ());
        }
        *result = Value{{"selected", id_array (s.active ()->selected ())}};
        return true;
    }

    bool cmd_select_all_layers (Session& s, const Value& p, Value* result, Error* err)
    {
        const DocState* st = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        std::vector<doc::WalkEntry> walk = st->doc->walk ();
        std::vector<doc::LayerId> ids;
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (!st->doc->is_background (walk[i].layer->id))
            {
                ids.push_back (walk[i].layer->id);
            }
        }
        if (ids.empty ())
        {
            return fail (err, ErrorKind::Other, "there are no layers to select");
        }
        doc::LayerId active = has_id (ids, st->active_layer) ? st->active_layer : ids.back ();
        if (!set_layer_selection (s, ids, active, doc::NO_LAYER, err))
        {
            return false;
        }
        *result = Value{{"selected", ids.size ()}};
        return true;
    }

    bool cmd_select_deselect_layers (Session& s, const Value& p, Value* result, Error* err)
    {
        DocState* st = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        st->active_layer = doc::NO_LAYER;
        st->fix_selection ();
        bool clean = st->saved_revision == st->revision;
        st->revision++;
        if (clean)
        {
            st->saved_revision = st->revision;
        }
        return true;
    }

    bool cmd_layer_new_group (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        std::string name;
        if (!param_str (p, "name", &name))
        {
            name = d->next_layer_name ("Group");
        }
        doc::LayerId id =
            d->insert_above (*e.active_layer (), doc::Layer::group (s.ids, name, std::vector<doc::Layer> ()));
        *e.active_layer () = id;
        if (!check_group_depth (*d, "New Group", err))
        {
            return false;
        }
        if (!e.commit ("New Group", err))
        {
            return false;
        }
        *result = Value{{"layer", id}};
        return true;
    }

    /* Group Layers (⌘G): the layer, or every selected layer, moves into a new group placed where the
       top-most of them was. */
    bool cmd_layer_group_layers (Session& s, const Value& p, Value* result, Error* err)
    {
        std::vector<doc::LayerId> targets;
        uint64_t one;
        if (param_u64 (p, "layer", &one))
        {
            targets.push_back (one);
        }
        else if (s.active () != NULL)
        {
            targets = s.active ()->selected ();
        }
        if (targets.empty ())
        {
            return fail (err, ErrorKind::Other, "no layer selected");
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        std::vector<doc::LayerId> ids = top_level (*d, targets);
        if (ids.empty ())
        {
            return no_layer (err, targets[0]);
        }
        if (d->is_background (ids[0]))
        {
            return fail (err, ErrorKind::Other,
                "the Background layer can't be grouped (Layer › New › Layer from Background makes it a normal layer)");
        }
        std::string name;
        if (!param_str (p, "name", &name))
        {
            name = d->next_layer_name ("Group");
        }
        doc::LayerId gid = d->insert_above (ids.back (), doc::Layer::group (s.ids, name, std::vector<doc::Layer> ()));
        std::vector<doc::Layer> children;
        for (size_t i = 0; i < ids.size (); i++)
        {
            doc::Layer l;
            if (!d->remove (ids[i], &l))
            {
                return no_layer (err, ids[i]);
            }
            children.push_back (l);
        }
        d->layer_mut (gid)->children = children;
        if (!check_group_depth (*d, "Group Layers", err))
        {
            return false;
        }
        e.select (std::vector<doc::LayerId> (1, gid), gid);
        if (!e.commit ("Group Layers", err))
        {
            return false;
        }
        *result = Value{{"layer", gid}};
        return true;
    }

    bool cmd_layer_ungroup (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::LayerPath path;
        if (!d->path_of (id, &path))
        {
            return no_layer (err, id);
        }
        size_t idx;
        std::vector<doc::Layer>* sib = d->siblings_mut (path, &idx);
        if (sib == NULL)
        {
            return no_layer (err, id);
        }
        if (!(*sib)[idx].is_group ())
        {
            return fail (err, ErrorKind::Other, "the active layer is not a group");
        }
        std::vector<doc::Layer> children = (*sib)[idx].children;
        sib->erase (sib->begin () + (ptrdiff_t) idx);
        sib->insert (sib->begin () + (ptrdiff_t) idx, children.begin (), children.end ());
        std::vector<doc::LayerId> ids;
        for (size_t i = 0; i < children.size (); i++)
        {
            ids.push_back (children[i].id);
        }
        doc::LayerId active = ids.empty () ? d->top_layer () : ids.back ();
        e.select (ids, active);
        return e.commit ("Ungroup Layers", err);
    }

    bool is_group_active (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d = s.active ();
        if (d->doc->layer (d->active_layer)->is_group ())
        {
            return true;
        }
        *why = "the active layer is not a group";
        return false;
    }

    bool any_group (const std::vector<doc::Layer>& layers)
    {
        for (size_t i = 0; i < layers.size (); i++)
        {
            if (layers[i].is_group ())
            {
                return true;
            }
        }
        return false;
    }

    void set_all_expanded (std::vector<doc::Layer>* layers, bool expanded, size_t* count)
    {
        for (size_t i = 0; i < layers->size (); i++)
        {
            doc::Layer& l = (*layers)[i];
            if (!l.is_group ())
            {
                continue;
            }
            l.expanded = expanded;
            *count += 1;
            set_all_expanded (&l.children, expanded, count);
        }
    }

    /* Open or close a group in the Layers panel (no "expanded": toggle; "all": every group). View
       state, not a history step. */
    bool cmd_layer_set_expanded (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.setExpanded";
        DocState* st    = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        bool want = false, all = false;
        bool has_want = param_present (p, "expanded");
        if (has_want && !param_bool (p, "expanded", &want))
        {
            return bad_params (err, cmd, "`expanded` must be true or false");
        }
        if (param_present (p, "all") && !param_bool (p, "all", &all))
        {
            return bad_params (err, cmd, "`all` must be true or false");
        }
        std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*st->doc);
        size_t count                        = 0;
        if (all && !param_present (p, "layer"))
        {
            bool current                     = false;
            std::vector<doc::WalkEntry> walk = copy->walk ();
            for (size_t i = 0; i < walk.size (); i++)
            {
                current = current || (walk[i].layer->is_group () && walk[i].layer->expanded);
            }
            if (!has_want)
            {
                want = !current;
            }
            set_all_expanded (&copy->layers, want, &count);
        }
        else
        {
            doc::LayerId id;
            if (!layer_param (s, p, &id, err))
            {
                return false;
            }
            doc::Layer* l = copy->layer_mut (id);
            if (l == NULL)
            {
                return no_layer (err, id);
            }
            if (!l->is_group ())
            {
                return bad_params (err, cmd, "layer %llu is not a group", (unsigned long long) id);
            }
            if (!has_want)
            {
                want = !l->expanded;
            }
            if (all)
            {
                set_all_expanded (&copy->layers, want, &count);
            }
            else
            {
                l->expanded = want;
                count       = 1;
            }
        }
        st->doc = copy;
        *result = Value{{"expanded", want}, {"groups", count}};
        return true;
    }

    /* Drag in the Layers panel: the layer (or "layers", in document order) goes above, below or
       into the target; "copy" places duplicates there instead (⌥-drag). */
    bool cmd_layer_move_to (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.moveTo";
        uint64_t target;
        if (!param_u64 (p, "target", &target))
        {
            return bad_params (err, cmd, "missing `target`");
        }
        std::string pos = "above";
        if (param_present (p, "position") && !param_str (p, "position", &pos))
        {
            return bad_params (err, cmd, "`position` must be a string");
        }
        if (pos != "above" && pos != "below" && pos != "into")
        {
            return bad_params (err, cmd, "position must be above, below, or into");
        }
        bool copy = false;
        param_bool (p, "copy", &copy);
        const Value* batch = param (p, "layers");
        if (batch != NULL && param_present (p, "layer"))
        {
            return bad_params (err, cmd, "give either `layer` or `layers`, not both");
        }
        std::vector<doc::LayerId> requested;
        if (batch != NULL)
        {
            if (!batch->is_array () || batch->empty ())
            {
                return bad_params (err, cmd, "`layers` must be a nonempty array of ids");
            }
            for (size_t i = 0; i < batch->size (); i++)
            {
                if (!(*batch)[i].is_number_unsigned ()
                    && !((*batch)[i].is_number_integer () && (*batch)[i].get<int64_t> () >= 0))
                {
                    return bad_params (err, cmd, "every layer id must be an unsigned integer");
                }
                doc::LayerId id = (*batch)[i].get<uint64_t> ();
                if (has_id (requested, id))
                {
                    return bad_params (err, cmd, "duplicate layer id");
                }
                requested.push_back (id);
            }
        }
        else
        {
            doc::LayerId id;
            if (!layer_param (s, p, &id, err))
            {
                return false;
            }
            requested.push_back (id);
        }
        const DocState* st = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        const doc::Document& cur = *st->doc;
        doc::LayerPath target_path;
        if (!cur.path_of (target, &target_path))
        {
            return no_layer (err, target);
        }
        for (size_t i = 0; i < requested.size (); i++)
        {
            if (cur.layer (requested[i]) == NULL)
            {
                return no_layer (err, requested[i]);
            }
        }
        std::vector<doc::LayerId> moved_ids = top_level (cur, requested);
        if (moved_ids.size () == 1 && moved_ids[0] == target && batch == NULL && !copy)
        {
            return true;
        }
        for (size_t i = 0; i < moved_ids.size (); i++)
        {
            doc::LayerPath path;
            cur.path_of (moved_ids[i], &path);
            bool beside_itself = copy && moved_ids[i] == target && pos != "into";
            if (path_starts_with (target_path, path) && !beside_itself)
            {
                return bad_params (err, cmd, "target cannot be one of the moved layers or its descendant");
            }
        }
        if (pos == "into" && !cur.layer (target)->is_group ())
        {
            return bad_params (err, cmd, "target is not a group");
        }

        doc::LayerId original_active = st->active_layer;
        doc::LayerId bg              = background_id (cur);
        Edit e (s);
        doc::Document* d = e.doc ();
        std::vector<doc::Layer> moved;
        std::vector<doc::LayerId> copies;
        ClipBefore before;
        if (!copy)
        {
            before = clip_before (*d, moved_ids);
        }
        for (size_t i = 0; i < moved_ids.size (); i++)
        {
            if (copy)
            {
                doc::Layer dup = layer_copy (s.ids, *d, *d->layer (moved_ids[i]));
                copies.push_back (dup.id);
                moved.push_back (dup);
            }
            else
            {
                doc::Layer l;
                d->remove (moved_ids[i], &l);
                moved.push_back (l);
            }
        }
        doc::LayerPath path;
        if (!d->path_of (target, &path))
        {
            return no_layer (err, target);
        }
        if (pos == "into")
        {
            doc::Layer* g = d->layer_at_mut (path);
            g->children.insert (g->children.end (), moved.begin (), moved.end ());
        }
        else
        {
            size_t idx;
            std::vector<doc::Layer>* sib = d->siblings_mut (path, &idx);
            if (sib == NULL)
            {
                return no_layer (err, target);
            }
            size_t at = pos == "below" ? idx : idx + 1;
            sib->insert (sib->begin () + (ptrdiff_t) at, moved.begin (), moved.end ());
        }
        if (!check_group_depth (*d, "Reorder Layer", err))
        {
            return false;
        }
        if (!keep_background_at_bottom (bg, *d, err))
        {
            return false;
        }
        clip_settle (d, copy ? copies : moved_ids, before, false);
        doc::LayerId active;
        if (copy)
        {
            active = copies.back ();
        }
        else if (batch != NULL)
        {
            active = has_id (requested, original_active) ? original_active : requested.back ();
        }
        else
        {
            active = moved_ids[0];
        }
        if (copy && batch != NULL)
        {
            e.select (copies, active);
        }
        else if (batch != NULL)
        {
            e.select (requested, active);
        }
        else
        {
            *e.active_layer () = active;
        }
        if (!e.commit (copy ? "Duplicate Layer" : "Reorder Layers", err))
        {
            return false;
        }
        if (copy && batch != NULL)
        {
            *result = Value{{"moved", moved_ids.size ()}, {"layers", id_array (copies)}};
        }
        else if (copy)
        {
            *result = Value{{"layer", copies[0]}};
        }
        else if (batch != NULL)
        {
            *result = Value{{"moved", moved_ids.size ()}};
        }
        return true;
    }

    /* Arrange: steps up (positive) or down within the layer's group; the Background stays put and
       nothing goes below it. */
    bool arrange (Session& s, const Value& p, int steps, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::LayerId bg = background_id (*d);
        if (bg == id)
        {
            return fail (err, ErrorKind::Other, "the Background layer can't be moved");
        }
        int dir           = steps > 0 ? 1 : -1;
        int count         = steps > 0 ? steps : -steps;
        bool moved        = false;
        ClipBefore before = clip_before (*d, std::vector<doc::LayerId> (1, id));
        for (int i = 0; i < count; i++)
        {
            doc::LayerPath path;
            if (!d->path_of (id, &path))
            {
                return no_layer (err, id);
            }
            size_t idx;
            std::vector<doc::Layer>* sib = d->siblings_mut (path, &idx);
            long to                      = (long) idx + dir;
            if (to < 0 || to >= (long) sib->size ())
            {
                break;
            }
            std::swap ((*sib)[idx], (*sib)[(size_t) to]);
            if (bg != doc::NO_LAYER && d->layers[0].id != bg)
            {
                std::swap ((*sib)[idx], (*sib)[(size_t) to]);
                break;
            }
            moved = true;
        }
        if (!moved)
        {
            return fail (err, ErrorKind::Other, "layer can't move further");
        }
        clip_settle (d, std::vector<doc::LayerId> (1, id), before, false);
        return e.commit ("Arrange", err);
    }

    bool cmd_arrange_forward (Session& s, const Value& p, Value* result, Error* err)
    {
        return arrange (s, p, 1, err);
    }

    bool cmd_arrange_backward (Session& s, const Value& p, Value* result, Error* err)
    {
        return arrange (s, p, -1, err);
    }

    bool cmd_arrange_front (Session& s, const Value& p, Value* result, Error* err)
    {
        return arrange (s, p, 10000, err);
    }

    bool cmd_arrange_back (Session& s, const Value& p, Value* result, Error* err)
    {
        return arrange (s, p, -10000, err);
    }

    /* Reverse: the selected layers trade places, top for bottom. */
    bool cmd_arrange_reverse (Session& s, const Value& p, Value* result, Error* err)
    {
        const DocState* st = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        std::vector<doc::LayerId> sel = st->selected ();
        Edit e (s);
        doc::Document* d              = e.doc ();
        doc::LayerId bg               = background_id (*d);
        std::vector<doc::LayerId> ids = top_level (*d, sel);
        if (ids.size () < 2)
        {
            return fail (err, ErrorKind::Other, "select two or more layers to reverse");
        }
        std::vector<doc::LayerPath> paths (ids.size ());
        std::vector<doc::Layer> layers (ids.size ());
        for (size_t i = 0; i < ids.size (); i++)
        {
            d->path_of (ids[i], &paths[i]);
            layers[i] = *d->layer (ids[i]);
        }
        for (size_t i = 0; i < ids.size (); i++)
        {
            *d->layer_at_mut (paths[i]) = layers[ids.size () - 1 - i];
        }
        if (!keep_background_at_bottom (bg, *d, err))
        {
            return false;
        }
        return e.commit ("Reverse", err);
    }

    bool two_plus (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (s.active ()->selected ().size () >= 2)
        {
            return true;
        }
        *why = "select two or more layers";
        return false;
    }

    bool has_background_layer (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (s.active ()->doc->background () != NULL)
        {
            return true;
        }
        *why = "the document has no Background layer";
        return false;
    }

    void unlock_background (doc::Layer* l)
    {
        l->name               = "Layer 0";
        l->locks.transparency = false;
        l->locks.position     = false;
    }

    bool cmd_layer_from_background (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        const doc::Layer* b = d->background ();
        if (b == NULL)
        {
            return fail (err, ErrorKind::Other, "the document has no Background layer");
        }
        doc::LayerId id = b->id;
        unlock_background (d->layer_mut (id));
        *e.active_layer () = id;
        if (!e.commit ("Layer From Background", err))
        {
            return false;
        }
        *result = Value{{"layer", id}};
        return true;
    }

    bool set_visible (Session& s, const Value& p, bool visible, Error* err)
    {
        std::vector<doc::LayerId> ids;
        if (!layer_targets (s, p, &ids, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        for (size_t i = 0; i < ids.size (); i++)
        {
            doc::Layer* l = d->layer_mut (ids[i]);
            if (l == NULL)
            {
                return no_layer (err, ids[i]);
            }
            l->visible = visible;
        }
        const char* label =
            ids.size () > 1 ? (visible ? "Show Layers" : "Hide Layers") : (visible ? "Show Layer" : "Hide Layer");
        return e.commit (label, err);
    }

    /* ⌘, hides the targeted layers, or shows them when every one is already hidden. */
    bool cmd_layer_hide_layers (Session& s, const Value& p, Value* result, Error* err)
    {
        std::vector<doc::LayerId> ids;
        if (!layer_targets (s, p, &ids, err))
        {
            return false;
        }
        const doc::Document& cur = *s.active ()->doc;
        bool all_hidden          = true;
        for (size_t i = 0; i < ids.size (); i++)
        {
            const doc::Layer* l = cur.layer (ids[i]);
            if (l == NULL || l->visible)
            {
                all_hidden = false;
            }
        }
        return set_visible (s, p, all_hidden, err);
    }

    bool cmd_layer_show_layers (Session& s, const Value& p, Value* result, Error* err)
    {
        return set_visible (s, p, true, err);
    }

    /* Is the layer at `path` shown alone: it and its enclosing groups visible, everything outside it
       hidden (layers inside it keep their own visibility)? */
    bool shown_alone (const doc::Document& d, const doc::LayerPath& path)
    {
        std::vector<doc::WalkEntry> walk = d.walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            const doc::LayerPath& p = walk[i].path;
            if (path_starts_with (path, p))
            {
                if (!walk[i].layer->visible)
                {
                    return false;
                }
            }
            else if (!path_starts_with (p, path) && walk[i].layer->visible)
            {
                return false;
            }
        }
        return true;
    }

    /* ⌥-click on an eye: show only that layer, or restore the others when it already is. */
    bool cmd_layer_show_only (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, p, &id, err))
        {
            return false;
        }
        DocState* st = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        const doc::Document& cur = *st->doc;
        doc::LayerPath path;
        if (!cur.path_of (id, &path))
        {
            return no_layer (err, id);
        }
        doc::LayerPath solo_path;
        bool saved =
            st->solo_layer != doc::NO_LAYER && cur.path_of (st->solo_layer, &solo_path) && shown_alone (cur, solo_path);
        std::vector<doc::WalkEntry> walk = cur.walk ();
        bool solo                        = !shown_alone (cur, path);
        std::vector<std::pair<doc::LayerPath, bool>> rows;
        std::vector<std::pair<doc::LayerId, bool>> snapshot;
        if (solo)
        {
            if (saved)
            {
                snapshot = st->solo_snapshot;
            }
            else
            {
                for (size_t i = 0; i < walk.size (); i++)
                {
                    snapshot.push_back (std::make_pair (walk[i].layer->id, walk[i].layer->visible));
                }
            }
            for (size_t i = 0; i < walk.size (); i++)
            {
                const doc::LayerPath& wp = walk[i].path;
                if (wp.size () <= path.size () || !path_starts_with (wp, path))
                {
                    rows.push_back (std::make_pair (wp, path_starts_with (path, wp)));
                }
            }
        }
        else
        {
            std::unordered_map<doc::LayerId, bool> before;
            if (saved)
            {
                for (size_t i = 0; i < st->solo_snapshot.size (); i++)
                {
                    before[st->solo_snapshot[i].first] = st->solo_snapshot[i].second;
                }
            }
            for (size_t i = 0; i < walk.size (); i++)
            {
                bool v = true;
                if (saved)
                {
                    std::unordered_map<doc::LayerId, bool>::const_iterator it = before.find (walk[i].layer->id);
                    v = it != before.end () ? it->second : walk[i].layer->visible;
                }
                rows.push_back (std::make_pair (walk[i].path, v));
            }
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        for (size_t i = 0; i < rows.size (); i++)
        {
            doc::Layer* l = d->layer_at_mut (rows[i].first);
            if (l != NULL)
            {
                l->visible = rows[i].second;
            }
        }
        if (!e.commit (solo ? "Show Only This Layer" : "Show Layers", err))
        {
            return false;
        }
        st                = s.active ();
        st->solo_layer    = solo ? id : doc::NO_LAYER;
        st->solo_snapshot = snapshot;
        *result           = Value{{"shownAlone", solo}};
        return true;
    }

    /* "/": Lock Transparent Pixels on or off (on unless every targeted layer has it). */
    bool cmd_layer_toggle_lock_transparency (Session& s, const Value& p, Value* result, Error* err)
    {
        std::vector<doc::LayerId> all;
        if (!layer_targets (s, p, &all, err))
        {
            return false;
        }
        const doc::Document& cur = *s.active ()->doc;
        std::vector<doc::LayerId> ids;
        bool lock = false;
        for (size_t i = 0; i < all.size (); i++)
        {
            const doc::Layer* l = cur.layer (all[i]);
            if (l == NULL || cur.is_background (all[i]))
            {
                continue;
            }
            ids.push_back (all[i]);
            if (!l->locks.transparency)
            {
                lock = true;
            }
        }
        if (ids.empty ())
        {
            return fail (err, ErrorKind::Other, "the Background layer can't lock transparent pixels");
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        for (size_t i = 0; i < ids.size (); i++)
        {
            d->layer_mut (ids[i])->locks.transparency = lock;
        }
        if (!e.commit (lock ? "Lock Transparent Pixels" : "Unlock Transparent Pixels", err))
        {
            return false;
        }
        *result = Value{{"locked", lock}};
        return true;
    }

    /* Lock Layers…: the given locks on every selected layer; none given toggles Lock All. */
    bool cmd_layer_lock_layers (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* keys[5] = {"transparency", "pixels", "position", "artboard", "all"};
        bool given[5], values[5];
        bool any = false;
        for (int i = 0; i < 5; i++)
        {
            given[i] = param_bool (p, keys[i], &values[i]);
            any      = any || given[i];
        }
        std::vector<doc::LayerId> sel = s.active () != NULL ? s.active ()->selected () : std::vector<doc::LayerId> ();
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!any)
        {
            bool all_locked = true;
            for (size_t i = 0; i < sel.size (); i++)
            {
                const doc::Layer* l = d->layer (sel[i]);
                if (l == NULL || !l->locks.all)
                {
                    all_locked = false;
                }
            }
            given[4]  = true;
            values[4] = !all_locked;
        }
        for (size_t i = 0; i < sel.size (); i++)
        {
            doc::Layer* l = d->layer_mut (sel[i]);
            if (l == NULL)
            {
                return no_layer (err, sel[i]);
            }
            bool* slots[5] = {
                &l->locks.transparency, &l->locks.pixels, &l->locks.position, &l->locks.artboard, &l->locks.all};
            for (int k = 0; k < 5; k++)
            {
                if (given[k])
                {
                    *slots[k] = values[k];
                }
            }
        }
        if (!e.commit ("Lock Layers", err))
        {
            return false;
        }
        *result = Value{{"layers", sel.size ()}};
        return true;
    }

    uint64_t unused_link_group (const doc::Document& d)
    {
        std::vector<doc::WalkEntry> walk = d.walk ();
        std::vector<uint64_t> used;
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (walk[i].layer->link_group != 0)
            {
                used.push_back (walk[i].layer->link_group);
            }
        }
        std::sort (used.begin (), used.end ());
        if (used.empty ())
        {
            return 1;
        }
        if (used.back () != UINT64_MAX)
        {
            return used.back () + 1;
        }
        uint64_t free_id = 1;
        for (size_t i = 0; i < used.size (); i++)
        {
            if (used[i] > free_id)
            {
                break;
            }
            if (used[i] == free_id)
            {
                free_id++;
            }
        }
        return free_id;
    }

    /* Link Layers toggles: the selection already one link group unlinks. A group left with one
       member is no longer a link. */
    bool cmd_layer_link_layers (Session& s, const Value& p, Value* result, Error* err)
    {
        std::vector<doc::LayerId> sel = s.active ()->selected ();
        Edit e (s);
        doc::Document* d = e.doc ();
        uint64_t first   = d->layer (sel[0])->link_group;
        bool unlink      = first != 0;
        for (size_t i = 0; i < sel.size (); i++)
        {
            if (d->layer (sel[i])->link_group != first)
            {
                unlink = false;
            }
        }
        uint64_t next = unlink ? 0 : unused_link_group (*d);
        for (size_t i = 0; i < sel.size (); i++)
        {
            d->layer_mut (sel[i])->link_group = next;
        }
        std::vector<doc::WalkEntry> walk = d->walk ();
        std::unordered_map<uint64_t, size_t> counts;
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (walk[i].layer->link_group != 0)
            {
                counts[walk[i].layer->link_group]++;
            }
        }
        std::vector<doc::LayerId> lonely;
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (walk[i].layer->link_group != 0 && counts[walk[i].layer->link_group] < 2)
            {
                lonely.push_back (walk[i].layer->id);
            }
        }
        for (size_t i = 0; i < lonely.size (); i++)
        {
            d->layer_mut (lonely[i])->link_group = 0;
        }
        if (!e.commit ("Link Layers", err))
        {
            return false;
        }
        *result = Value{{"linked", !unlink}};
        return true;
    }

    bool can_link (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d             = s.active ();
        std::vector<doc::LayerId> sel = d->selected ();
        if (sel.size () >= 2 || d->doc->layer (sel[0])->link_group != 0)
        {
            return true;
        }
        *why = "select two or more layers to link";
        return false;
    }

    std::vector<uint64_t> link_groups (const doc::Document& d, const std::vector<doc::LayerId>& ids)
    {
        std::vector<uint64_t> out;
        for (size_t i = 0; i < ids.size (); i++)
        {
            const doc::Layer* l = d.layer (ids[i]);
            if (l != NULL && l->link_group != 0 && std::find (out.begin (), out.end (), l->link_group) == out.end ())
            {
                out.push_back (l->link_group);
            }
        }
        return out;
    }

    bool has_linked (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        const DocState* d = s.active ();
        if (!link_groups (*d->doc, d->selected ()).empty ())
        {
            return true;
        }
        *why = "the selected layers are not linked";
        return false;
    }

    bool cmd_layer_select_linked (Session& s, const Value& p, Value* result, Error* err)
    {
        const DocState* d                = s.active ();
        std::vector<uint64_t> groups     = link_groups (*d->doc, d->selected ());
        std::vector<doc::WalkEntry> walk = d->doc->walk ();
        std::vector<doc::LayerId> ids;
        for (size_t i = 0; i < walk.size (); i++)
        {
            uint64_t g = walk[i].layer->link_group;
            if (g != 0 && std::find (groups.begin (), groups.end (), g) != groups.end ())
            {
                ids.push_back (walk[i].layer->id);
            }
        }
        doc::LayerId active = has_id (ids, d->active_layer) ? d->active_layer : ids.back ();
        if (!set_layer_selection (s, ids, active, active, err))
        {
            return false;
        }
        *result = Value{{"selected", ids.size ()}};
        return true;
    }

    bool cmd_layer_rename (Session& s, const Value& p, Value* result, Error* err)
    {
        std::string name;
        if (!param_str (p, "name", &name) || name.find_first_not_of (" \t\r\n") == std::string::npos)
        {
            return bad_params (err, "layer.renameLayer", "missing `name`");
        }
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
        l->name = name;
        return e.commit ("Rename Layer", err);
    }

    /* With a "layer" param, that layer. Otherwise the selection: Create clips every selected layer
       but the lowest (the base); Release releases them all. */
    bool set_clipped (Session& s, const Value& p, bool clip, Value* result, Error* err)
    {
        std::vector<doc::LayerId> ids;
        if (param_present (p, "layer"))
        {
            doc::LayerId id;
            if (!layer_param (s, p, &id, err))
            {
                return false;
            }
            ids.push_back (id);
        }
        else
        {
            std::vector<doc::LayerId> sel = s.active ()->selected ();
            if (sel.empty ())
            {
                doc::LayerId id;
                if (!layer_param (s, p, &id, err))
                {
                    return false;
                }
                ids.push_back (id);
            }
            else if (sel.size () == 1 || !clip)
            {
                ids = sel;
            }
            else
            {
                ids.assign (sel.begin () + 1, sel.end ());
            }
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        for (size_t i = 0; i < ids.size (); i++)
        {
            doc::Layer* l = d->layer_mut (ids[i]);
            if (l == NULL)
            {
                return no_layer (err, ids[i]);
            }
            l->clipped = clip;
        }
        if (!e.commit (clip ? "Create Clipping Mask" : "Release Clipping Mask", err))
        {
            return false;
        }
        *result = Value{{"layers", id_array (ids)}};
        return true;
    }

    bool cmd_layer_create_clipping_mask (Session& s, const Value& p, Value* result, Error* err)
    {
        return set_clipped (s, p, true, result, err);
    }

    bool cmd_layer_release_clipping_mask (Session& s, const Value& p, Value* result, Error* err)
    {
        return set_clipped (s, p, false, result, err);
    }

    /* ⌘E: the selected layers become one pixel layer in place of the top one (hidden ones are
       discarded, as in Photoshop). One layer selected: Merge Down. */
    bool cmd_layer_merge_layers (Session& s, const Value& p, Value* result, Error* err)
    {
        std::vector<doc::LayerId> sel = s.active ()->selected ();
        if (sel.size () < 2)
        {
            return s.execute ("layer.mergeDown", Value::object (), result, err);
        }
        Edit e (s);
        doc::Document* d              = e.doc ();
        std::vector<doc::LayerId> ids = top_level (*d, sel);
        doc::Layer top                = *d->layer (ids.back ());
        doc::Document solo            = *d;
        solo.layers.clear ();
        for (size_t i = 0; i < ids.size (); i++)
        {
            const doc::Layer* l = d->layer (ids[i]);
            if (l->visible)
            {
                solo.layers.push_back (*l);
            }
        }
        if (solo.layers.empty ())
        {
            return fail (err, ErrorKind::Other, "the selected layers are all hidden");
        }
        color::PixelFormat fmt = d->pixel_format ();
        fmt.alpha              = true;
        doc::Layer merged      = doc::Layer::raster (s.ids, top.name, fmt);
        merged.locks           = top.locks;
        merged.pixels          = composite_layers (solo, fmt, s.cmyk);
        for (size_t i = 0; i + 1 < ids.size (); i++)
        {
            d->remove (ids[i], NULL);
        }
        *d->layer_mut (top.id) = merged;
        e.select (std::vector<doc::LayerId> (1, merged.id), merged.id);
        if (!e.commit ("Merge Layers", err))
        {
            return false;
        }
        *result = Value{{"layer", merged.id}};
        return true;
    }

    /* ⇧⌘E: the visible top-level layers merge into the lowest of them; hidden ones stay. */
    bool cmd_layer_merge_visible (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        std::vector<size_t> visible;
        bool any_group_visible = false;
        for (size_t i = 0; i < d->layers.size (); i++)
        {
            if (!d->layers[i].visible)
            {
                continue;
            }
            visible.push_back (i);
            any_group_visible = any_group_visible || d->layers[i].is_group ();
        }
        if (visible.size () < 2 && !any_group_visible)
        {
            return fail (err, ErrorKind::Other, "there are not enough visible layers to merge");
        }
        size_t lowest      = visible[0];
        doc::Layer base    = d->layers[lowest];
        bool is_bg         = lowest == 0 && d->is_background (base.id);
        doc::Document solo = *d;
        solo.layers.clear ();
        for (size_t i = 0; i < visible.size (); i++)
        {
            solo.layers.push_back (d->layers[visible[i]]);
        }
        color::PixelFormat fmt = d->pixel_format ();
        if (!is_bg)
        {
            fmt.alpha = true;
        }
        doc::Layer merged = doc::Layer::raster (s.ids, base.name, fmt);
        merged.locks      = base.locks;
        if (is_bg)
        {
            float white[3]                   = {1.0f, 1.0f, 1.0f};
            compose::RenderSettings settings = compose::default_settings ();
            settings.cmyk                    = s.cmyk;
            merged.pixels                    = compose::composite_to_surface (solo, d->bounds (), fmt, white, settings);
        }
        else
        {
            merged.pixels = composite_layers (solo, fmt, s.cmyk);
        }
        std::vector<doc::Layer> out;
        for (size_t i = 0; i < d->layers.size (); i++)
        {
            if (i == lowest)
            {
                out.push_back (merged);
            }
            else if (!d->layers[i].visible)
            {
                out.push_back (d->layers[i]);
            }
        }
        d->layers          = out;
        *e.active_layer () = merged.id;
        return e.commit ("Merge Visible", err);
    }

    /* The layer's pixels inside the selection (weighted by it), as a surface with alpha. */
    raster::Surface lift (const raster::Surface& src, const raster::Surface* sel, Rect canvas)
    {
        color::PixelFormat fmt        = src.format ();
        color::PixelFormat with_alpha = fmt;
        with_alpha.alpha              = true;
        raster::Surface out (with_alpha);
        Rect area = (sel != NULL ? sel->content_bounds () : src.content_bounds ()).intersect (canvas);
        if (area.is_empty ())
        {
            return out;
        }
        std::vector<float> px;
        if (!src.read_region (area, &px))
        {
            return out;
        }
        size_t k     = fmt.channels ();
        size_t n     = with_alpha.channels ();
        size_t count = px.size () / k;
        std::vector<float> data (count * n);
        for (size_t i = 0; i < count; i++)
        {
            memcpy (&data[i * n], &px[i * k], k * sizeof (float));
            if (!fmt.alpha)
            {
                data[i * n + n - 1] = 1.0f;
            }
        }
        if (sel != NULL)
        {
            std::vector<float> m;
            if (sel->read_region (area, &m))
            {
                size_t mk = sel->channels ();
                for (size_t i = 0; i < count; i++)
                {
                    data[i * n + n - 1] *= m[i * mk];
                }
            }
        }
        out.write_region (area, data.data (), data.size ());
        out.prune ();
        return out;
    }

    /* ⌘J / ⇧⌘J: the selected pixels to a new layer above (Cut also clears them). With no selection,
       Layer via Copy duplicates the layer (or every selected layer). */
    bool layer_via (Session& s, bool cut, Value* result, Error* err)
    {
        const DocState* st = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::LayerId id = st->active_layer;
        if (id == doc::NO_LAYER)
        {
            return fail (err, ErrorKind::Other, "no active layer");
        }
        bool has_sel = st->doc->has_selection;
        if (!has_sel && !cut)
        {
            return s.execute ("layer.duplicate", Value::object (), result, err);
        }
        const doc::Layer* src = st->doc->layer (id);
        if (src == NULL || src->surface () == NULL)
        {
            return fail (err, ErrorKind::Other, "the active layer has no pixels");
        }
        raster::Surface clip = lift (*src->surface (), has_sel ? &st->doc->selection : NULL, st->doc->bounds ());
        if (clip.content_bounds ().is_empty ())
        {
            return fail (err, ErrorKind::Other, "Could not complete the command: the selected area is empty");
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        if (cut)
        {
            if (!check_pixels_unlocked (*d, id, err))
            {
                return false;
            }
            if (!clear_selected (d, id, s.tools.background, s.cmyk, err))
            {
                return false;
            }
        }
        doc::Layer l       = doc::Layer::raster (s.ids, d->next_layer_name ("Layer"), clip.format ());
        l.pixels           = clip;
        doc::LayerId nid   = d->insert_above (id, l);
        *e.active_layer () = nid;
        d->has_selection   = false;
        d->selection       = raster::Surface (color::PixelFormat::gray8 ());
        if (!e.commit (cut ? "Layer Via Cut" : "Layer Via Copy", err))
        {
            return false;
        }
        *result = Value{{"layer", nid}};
        return true;
    }

    bool cmd_layer_via_copy (Session& s, const Value& p, Value* result, Error* err)
    {
        return layer_via (s, false, result, err);
    }

    bool cmd_layer_via_cut (Session& s, const Value& p, Value* result, Error* err)
    {
        return layer_via (s, true, result, err);
    }

    bool has_pixel_layer (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d = s.active ();
        if (d->doc->layer (d->active_layer)->surface () != NULL)
        {
            return true;
        }
        *why = "the active layer has no pixels";
        return false;
    }

    bool has_any_group (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        std::vector<doc::WalkEntry> walk = s.active ()->doc->walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (walk[i].layer->is_group ())
            {
                return true;
            }
        }
        *why = "the document has no layer groups";
        return false;
    }

    const CommandSpec LAYER_TREE_COMMANDS[] = {
        {"layer.select", "Select Layer", "", NULL,
            "{\"layer\":id,\"mode\":\"replace|toggle|range|add\"=\"replace\"} (toggle = ⌘-click, range = ⇧-click)",
            has_doc, cmd_layer_select, true},
        {"select.allLayers", "All Layers", "Select", "Cmd+Alt+A", "{}", has_doc, cmd_select_all_layers, true},
        {"select.deselectLayers", "Deselect Layers", "Select", NULL, "{}", has_doc, cmd_select_deselect_layers, true},
        {"layer.new.group", "Group…", "Layer/New", NULL, "{\"name\":str?}", has_doc, cmd_layer_new_group, true},
        {"layer.new.groupFromLayers", "Group from Layers…", "Layer/New", NULL, "{\"name\":str?}", has_layer,
            cmd_layer_group_layers, true},
        {"layer.new.layerFromBackground", "Layer From Background…", "Layer/New", NULL, "{}", has_background_layer,
            cmd_layer_from_background, true},
        {"layer.new.layerViaCopy", "Layer via Copy", "Layer/New", "Cmd+J", "{}", has_layer, cmd_layer_via_copy, true},
        {"layer.new.layerViaCut", "Layer via Cut", "Layer/New", "Cmd+Shift+J", "{}", has_pixel_layer, cmd_layer_via_cut,
            true},
        {"layer.groupLayers", "Group Layers", "Layer", "Cmd+G",
            "{\"layer\":id?,\"name\":str?} (no layer: every selected layer)", has_layer, cmd_layer_group_layers, true},
        {"layer.ungroupLayers", "Ungroup Layers", "Layer", "Cmd+Shift+G", "{\"layer\":id?}", is_group_active,
            cmd_layer_ungroup, true},
        {"layer.setExpanded", "Expand/Collapse Group", "", NULL, "{\"layer\":id?,\"expanded\":bool?,\"all\":bool?}",
            has_any_group, cmd_layer_set_expanded, false},
        {"layer.moveTo", "Reorder Layer", "", NULL,
            "{\"layer\":id?|\"layers\":[id,…]?,\"target\":id,\"position\":\"above|below|into\"=\"above\",\"copy\":bool="
            "false}",
            has_layer, cmd_layer_move_to, true},
        {"layer.arrange.bringToFront", "Bring to Front", "Layer/Arrange", "Cmd+Shift+]", "{\"layer\":id?}", has_layer,
            cmd_arrange_front, true},
        {"layer.arrange.bringForward", "Bring Forward", "Layer/Arrange", "Cmd+]", "{\"layer\":id?}", has_layer,
            cmd_arrange_forward, true},
        {"layer.arrange.sendBackward", "Send Backward", "Layer/Arrange", "Cmd+[", "{\"layer\":id?}", has_layer,
            cmd_arrange_backward, true},
        {"layer.arrange.sendToBack", "Send to Back", "Layer/Arrange", "Cmd+Shift+[", "{\"layer\":id?}", has_layer,
            cmd_arrange_back, true},
        {"layer.arrange.reverse", "Reverse", "Layer/Arrange", NULL, "{}", two_plus, cmd_arrange_reverse, true},
        {"layer.createClippingMask", "Create Clipping Mask", "Layer", "Cmd+Alt+G", "{\"layer\":id?}", has_layer,
            cmd_layer_create_clipping_mask, true},
        {"layer.releaseClippingMask", "Release Clipping Mask", "Layer", NULL, "{\"layer\":id?}", has_layer,
            cmd_layer_release_clipping_mask, true},
        {"layer.hideLayers", "Hide Layers", "Layer", "Cmd+,", "{\"layer\":id?} (shows them when every one is hidden)",
            has_layer, cmd_layer_hide_layers, true},
        {"layer.showLayers", "Show Layers", "", NULL, "{\"layer\":id?}", has_layer, cmd_layer_show_layers, true},
        {"layer.showOnly", "Show Only This Layer", "", NULL, "{\"layer\":id?} → {shownAlone}", has_layer,
            cmd_layer_show_only, true},
        {"layer.toggleLockTransparency", "Lock Transparent Pixels", "", "/", "{\"layer\":id?}", has_layer,
            cmd_layer_toggle_lock_transparency, true},
        {"layer.lockLayers", "Lock Layers…", "Layer", NULL,
            "{\"transparency\":bool?,\"pixels\":bool?,\"position\":bool?,\"artboard\":bool?,\"all\":bool?} (none "
            "given: toggle lock all)",
            has_layer, cmd_layer_lock_layers, true},
        {"layer.linkLayers", "Link Layers", "Layer", NULL, "{} (toggles)", can_link, cmd_layer_link_layers, true},
        {"layer.selectLinkedLayers", "Select Linked Layers", "Layer", NULL, "{}", has_linked, cmd_layer_select_linked,
            true},
        {"layer.renameLayer", "Rename Layer", "Layer", NULL, "{\"layer\":id?,\"name\":str}", has_layer,
            cmd_layer_rename, true},
        {"layer.mergeLayers", "Merge Layers", "Layer", "Cmd+E", "{} (one layer selected: Merge Down)", has_layer,
            cmd_layer_merge_layers, true},
        {"layer.mergeVisible", "Merge Visible", "Layer", "Cmd+Shift+E", "{}", has_doc, cmd_layer_merge_visible, true},
    };

    void register_layer_tree_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), LAYER_TREE_COMMANDS,
            LAYER_TREE_COMMANDS + sizeof LAYER_TREE_COMMANDS / sizeof LAYER_TREE_COMMANDS[0]);
    }

} // namespace engine
