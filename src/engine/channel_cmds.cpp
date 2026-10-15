#include "algo/selection.h"
#include "engine/layers.h"
#include "engine/session.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace engine
{

    /* Alpha channels: Select > Save Selection / Load Selection and the Channels panel's alpha rows
       (new, duplicate, rename, delete). A channel is a full-canvas grey surface where white is
       selected, as in Photoshop's default "Color Indicates: Masked Areas" display. */

    using color::PixelFormat;
    using geom::Rect;

    bool has_channels (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (!s.active ()->doc->channels.empty ())
        {
            return true;
        }
        *why = "the document has no alpha channels";
        return false;
    }

    /* The "channel" param as an index into the document's alpha channels. */
    bool channel_param (const doc::Document& d, const Value& p, const char* cmd, size_t* out, Error* err)
    {
        int64_t i;
        if (!param_i64 (p, "channel", &i))
        {
            return bad_params (err, cmd, "missing `channel` (an alpha channel index)");
        }
        if (i < 0 || (size_t) i >= d.channels.size ())
        {
            return bad_params (err, cmd, "no alpha channel %lld", (long long) i);
        }
        *out = (size_t) i;
        return true;
    }

    std::string next_channel_name (const doc::Document& d)
    {
        for (int n = 1;; n++)
        {
            char name[32];
            snprintf (name, sizeof name, "Alpha %d", n);
            bool taken = false;
            for (size_t i = 0; i < d.channels.size (); i++)
            {
                taken = taken || d.channels[i].name == name;
            }
            if (!taken)
            {
                return name;
            }
        }
    }

    /* `src` with every value inside `area` replaced by 1 - value. */
    raster::Surface inverted (const raster::Surface& src, Rect area)
    {
        std::vector<float> data;
        raster::Surface out (PixelFormat::gray8 ());
        if (!src.read_region (area, &data))
        {
            return out;
        }
        for (size_t i = 0; i < data.size (); i++)
        {
            data[i] = 1.0f - data[i];
        }
        out.write_region (area, data.data (), data.size ());
        out.prune ();
        return out;
    }

    /* Select > Save Selection: into a new channel, or combined into an existing one. */
    bool cmd_save_selection (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "select.save";
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        Rect area      = d->bounds ();
        std::string op = "replace";
        param_str (p, "operation", &op);
        int64_t index = -1;
        param_i64 (p, "channel", &index);
        size_t at;
        if (index < 0)
        {
            doc::AlphaChannel c;
            c.name = next_channel_name (*d);
            param_str (p, "name", &c.name);
            c.surface = d->selection;
            d->channels.push_back (c);
            at = d->channels.size () - 1;
        }
        else
        {
            if ((size_t) index >= d->channels.size ())
            {
                return bad_params (err, cmd, "no alpha channel %lld", (long long) index);
            }
            at                   = (size_t) index;
            raster::Surface& dst = d->channels[at].surface;
            if (op == "add")
            {
                dst = algo::combine_surfaces (dst, d->selection, area, algo::select_max);
            }
            else if (op == "subtract")
            {
                dst = algo::combine_surfaces (dst, d->selection, area, algo::select_subtract);
            }
            else if (op == "intersect")
            {
                dst = algo::combine_surfaces (dst, d->selection, area, algo::select_min);
            }
            else if (op == "replace")
            {
                dst = d->selection;
            }
            else
            {
                return bad_params (err, cmd, "`operation` must be replace, add, subtract or intersect");
            }
        }
        if (!e.commit ("Save Selection", err))
        {
            return false;
        }
        *result = Value{{"channel", at}, {"name", s.active ()->doc->channels[at].name}};
        return true;
    }

    /* Select > Load Selection (and Ctrl-click on a channel): the channel as a selection, inverted
       if asked, combined with the current one by `mode`. */
    bool cmd_load_selection (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "select.load";
        size_t at;
        if (!channel_param (*s.active ()->doc, p, cmd, &at, err))
        {
            return false;
        }
        bool invert = false;
        param_bool (p, "invert", &invert);
        std::string mode = "replace";
        param_str (p, "mode", &mode);
        Edit e (s);
        doc::Document* d      = e.doc ();
        raster::Surface shape = invert ? inverted (d->channels[at].surface, d->bounds ()) : d->channels[at].surface;
        apply_selection (d, shape, mode);
        bool selected = d->has_selection;
        if (!e.commit ("Load Selection", err))
        {
            return false;
        }
        *result = Value{{"selected", selected}};
        return true;
    }

    /* A new alpha channel, black (nothing selected) unless `fill` is "white". */
    bool cmd_channel_new (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::AlphaChannel c;
        c.name = next_channel_name (*d);
        param_str (p, "name", &c.name);
        c.surface        = raster::Surface (PixelFormat::gray8 ());
        std::string fill = "black";
        param_str (p, "fill", &fill);
        if (fill == "white")
        {
            float one = 1.0f;
            c.surface.fill_rect (d->bounds (), &one);
        }
        d->channels.push_back (c);
        if (!e.commit ("New Channel", err))
        {
            return false;
        }
        *result = Value{{"channel", d->channels.size () - 1}};
        return true;
    }

    bool cmd_channel_duplicate (Session& s, const Value& p, Value* result, Error* err)
    {
        size_t at;
        if (!channel_param (*s.active ()->doc, p, "channel.duplicate", &at, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d    = e.doc ();
        doc::AlphaChannel c = d->channels[at];
        c.name += " copy";
        param_str (p, "name", &c.name);
        d->channels.insert (d->channels.begin () + (ptrdiff_t) at + 1, c);
        if (!e.commit ("Duplicate Channel", err))
        {
            return false;
        }
        *result = Value{{"channel", at + 1}};
        return true;
    }

    bool cmd_channel_rename (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "channel.rename";
        size_t at;
        if (!channel_param (*s.active ()->doc, p, cmd, &at, err))
        {
            return false;
        }
        std::string name;
        if (!param_str (p, "name", &name) || name.empty ())
        {
            return bad_params (err, cmd, "missing `name`");
        }
        Edit e (s);
        e.doc ()->channels[at].name = name;
        return e.commit ("Channel Name", err);
    }

    bool cmd_channel_delete (Session& s, const Value& p, Value* result, Error* err)
    {
        size_t at;
        if (!channel_param (*s.active ()->doc, p, "channel.delete", &at, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        d->channels.erase (d->channels.begin () + (ptrdiff_t) at);
        return e.commit ("Delete Channel", err);
    }

    /* Channels the panel lists: name, plus whether anything in it is selected. */
    bool cmd_channel_list (Session& s, const Value& p, Value* result, Error* err)
    {
        const doc::Document& d = *s.active ()->doc;
        Value list             = Value::array ();
        for (size_t i = 0; i < d.channels.size (); i++)
        {
            list.push_back (Value{{"index", i}, {"name", d.channels[i].name},
                {"empty", d.channels[i].surface.content_bounds ().intersect (d.bounds ()).is_empty ()}});
        }
        *result = Value{{"channels", list}};
        return true;
    }

    const CommandSpec CHANNEL_COMMANDS[] = {
        {"select.load", "Load Selection…", "Select", NULL,
            "{\"channel\":index,\"invert\":bool=false,\"mode\":\"replace|add|subtract|intersect\"=\"replace\"}",
            has_channels, cmd_load_selection, true},
        {"select.save", "Save Selection…", "Select", NULL,
            "{\"channel\":index|-1=-1 "
            "(new),\"name\":string?,\"operation\":\"replace|add|subtract|intersect\"=\"replace\"}",
            has_selection, cmd_save_selection, true},
        {"channel.new", "New Channel", "", NULL, "{\"name\":string?,\"fill\":\"black|white\"=\"black\"}", has_doc,
            cmd_channel_new, true},
        {"channel.duplicate", "Duplicate Channel", "", NULL, "{\"channel\":index,\"name\":string?}", has_channels,
            cmd_channel_duplicate, true},
        {"channel.rename", "Rename Channel", "", NULL, "{\"channel\":index,\"name\":string}", has_channels,
            cmd_channel_rename, true},
        {"channel.delete", "Delete Channel", "", NULL, "{\"channel\":index}", has_channels, cmd_channel_delete, true},
        {"channel.list", "List Channels", "", NULL, "{}", has_doc, cmd_channel_list, false},
    };

    void register_channel_commands (std::vector<CommandSpec>* out)
    {
        out->insert (
            out->end (), CHANNEL_COMMANDS, CHANNEL_COMMANDS + sizeof CHANNEL_COMMANDS / sizeof CHANNEL_COMMANDS[0]);
    }

} // namespace engine
