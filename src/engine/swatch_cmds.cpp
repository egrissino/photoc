#include "engine/session.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace engine
{

    /* The Swatches panel's library: named colours in groups, held by the session (the UI keeps it
       between runs). Colours are RGB in the working space; the reference's CMYK, Lab and spot
       swatches and .aco/.ase files are not ported yet. */

    void color_param (const Value& p, const char* key, const float fallback[4], float out[4]);

    const size_t MAX_SWATCHES = 10000;
    const size_t MAX_GROUPS   = 1000;

    Swatch make_swatch (const std::string& name, int r, int g, int b)
    {
        Swatch s;
        s.name   = name;
        s.rgb[0] = (float) r / 255.0f;
        s.rgb[1] = (float) g / 255.0f;
        s.rgb[2] = (float) b / 255.0f;
        return s;
    }

    /* The default library: grays, then pastel, pure and dark colours in ten hues. */
    std::vector<SwatchGroup> builtin_swatches ()
    {
        const char* gray_names[10] = {"Black", "90% Gray", "80% Gray", "70% Gray", "60% Gray", "50% Gray", "40% Gray",
            "30% Gray", "20% Gray", "White"};
        const int gray_levels[10]  = {0, 26, 51, 77, 102, 128, 153, 179, 204, 255};
        const char* hues[10]       = {
            "Red", "Orange", "Yellow", "Yellow Green", "Green", "Cyan Green", "Cyan", "Blue", "Violet", "Magenta"};
        const int pastel[10][3] = {{236, 128, 128}, {244, 176, 132}, {250, 224, 128}, {214, 240, 128}, {150, 232, 150},
            {128, 232, 200}, {128, 220, 240}, {128, 176, 244}, {168, 144, 244}, {232, 144, 232}};
        const int pure[10][3]   = {{230, 40, 40}, {245, 120, 30}, {250, 210, 30}, {160, 220, 40}, {40, 200, 80},
              {30, 200, 170}, {30, 170, 230}, {40, 100, 230}, {120, 70, 220}, {210, 50, 180}};
        const int dark[10][3]   = {{120, 20, 20}, {130, 60, 10}, {130, 110, 10}, {80, 120, 20}, {20, 100, 40},
              {10, 100, 90}, {10, 80, 120}, {20, 50, 120}, {60, 30, 110}, {110, 20, 90}};
        std::vector<SwatchGroup> out (4);
        out[0].name = "Grays";
        out[1].name = "Pastel";
        out[2].name = "Pure";
        out[3].name = "Dark";
        for (int i = 0; i < 10; i++)
        {
            out[0].items.push_back (make_swatch (gray_names[i], gray_levels[i], gray_levels[i], gray_levels[i]));
            out[1].items.push_back (
                make_swatch (std::string ("Pastel ") + hues[i], pastel[i][0], pastel[i][1], pastel[i][2]));
            out[2].items.push_back (make_swatch (hues[i], pure[i][0], pure[i][1], pure[i][2]));
            out[3].items.push_back (make_swatch (std::string ("Dark ") + hues[i], dark[i][0], dark[i][1], dark[i][2]));
        }
        return out;
    }

    size_t swatch_total (const std::vector<SwatchGroup>& groups)
    {
        size_t n = 0;
        for (size_t i = 0; i < groups.size (); i++)
        {
            n += groups[i].items.size ();
        }
        return n;
    }

    bool find_swatch (const std::vector<SwatchGroup>& groups, const std::string& name, size_t* gi, size_t* ii)
    {
        for (size_t g = 0; g < groups.size (); g++)
        {
            for (size_t i = 0; i < groups[g].items.size (); i++)
            {
                if (groups[g].items[i].name == name)
                {
                    *gi = g;
                    *ii = i;
                    return true;
                }
            }
        }
        return false;
    }

    std::string swatch_hex (const float rgb[3])
    {
        char buf[8];
        snprintf (buf, sizeof buf, "#%02x%02x%02x", (int) lroundf (rgb[0] * 255.0f), (int) lroundf (rgb[1] * 255.0f),
            (int) lroundf (rgb[2] * 255.0f));
        return buf;
    }

    /* The swatch a command names with "swatch" (and optionally "group"). */
    bool locate_swatch (const Session& s, const Value& p, const char* cmd, size_t* gi, size_t* ii, Error* err)
    {
        std::string name, group;
        if (!param_str (p, "swatch", &name))
        {
            return bad_params (err, cmd, "missing `swatch` (a swatch name)");
        }
        if (param_str (p, "group", &group))
        {
            for (size_t g = 0; g < s.swatches.size (); g++)
            {
                if (s.swatches[g].name != group)
                {
                    continue;
                }
                for (size_t i = 0; i < s.swatches[g].items.size (); i++)
                {
                    if (s.swatches[g].items[i].name == name)
                    {
                        *gi = g;
                        *ii = i;
                        return true;
                    }
                }
            }
        }
        else if (find_swatch (s.swatches, name, gi, ii))
        {
            return true;
        }
        return bad_params (err, cmd, "no swatch \"%s\" (see swatches.list)", name.c_str ());
    }

    bool cmd_swatches_list (Session& s, const Value& p, Value* result, Error* err)
    {
        Value groups = Value::array ();
        for (size_t g = 0; g < s.swatches.size (); g++)
        {
            Value items = Value::array ();
            for (size_t i = 0; i < s.swatches[g].items.size (); i++)
            {
                const Swatch& sw = s.swatches[g].items[i];
                items.push_back (Value{{"name", sw.name}, {"hex", swatch_hex (sw.rgb)}});
            }
            groups.push_back (Value{{"name", s.swatches[g].name}, {"swatches", items}});
        }
        *result = Value{{"groups", groups}, {"count", swatch_total (s.swatches)}};
        return true;
    }

    /* New Swatch: the colour (default the foreground) named "Color Swatch N" unless named. */
    bool cmd_swatches_add (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "swatches.add";
        if (swatch_total (s.swatches) >= MAX_SWATCHES)
        {
            return fail (err, ErrorKind::Other, "the Swatches panel is full (%zu swatches)", MAX_SWATCHES);
        }
        float c[4];
        color_param (p, "color", s.tools.foreground, c);
        std::string name, group;
        bool named = param_str (p, "name", &name) && !name.empty ();
        if (!named)
        {
            for (int n = 1;; n++)
            {
                char buf[64];
                snprintf (buf, sizeof buf, "Color Swatch %d", n);
                size_t a, b;
                if (!find_swatch (s.swatches, buf, &a, &b))
                {
                    name = buf;
                    break;
                }
            }
        }
        else
        {
            size_t a, b;
            std::string base = name;
            for (int n = 2; find_swatch (s.swatches, name, &a, &b); n++)
            {
                name = base + " " + std::to_string (n);
            }
        }
        size_t gi = 0;
        if (param_str (p, "group", &group))
        {
            gi = s.swatches.size ();
            for (size_t g = 0; g < s.swatches.size (); g++)
            {
                if (s.swatches[g].name == group)
                {
                    gi = g;
                }
            }
            if (gi == s.swatches.size ())
            {
                if (s.swatches.size () >= MAX_GROUPS)
                {
                    return bad_params (err, cmd, "too many swatch groups");
                }
                SwatchGroup ng;
                ng.name = group;
                s.swatches.push_back (ng);
            }
        }
        else if (s.swatches.empty ())
        {
            SwatchGroup ng;
            ng.name = "Swatches";
            s.swatches.push_back (ng);
        }
        Swatch sw;
        sw.name = name;
        for (int i = 0; i < 3; i++)
        {
            sw.rgb[i] = c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];
        }
        std::vector<Swatch>& items = s.swatches[gi].items;
        uint64_t at                = items.size ();
        if (param_present (p, "index") && !param_u64 (p, "index", &at))
        {
            return bad_params (err, cmd, "`index` must be a non-negative integer");
        }
        if (at > items.size ())
        {
            at = items.size ();
        }
        items.insert (items.begin () + (ptrdiff_t) at, sw);
        *result = Value{{"name", name}, {"group", s.swatches[gi].name}, {"index", at}};
        return true;
    }

    bool cmd_swatches_delete (Session& s, const Value& p, Value* result, Error* err)
    {
        size_t gi, ii;
        if (!locate_swatch (s, p, "swatches.delete", &gi, &ii, err))
        {
            return false;
        }
        std::string name = s.swatches[gi].items[ii].name;
        s.swatches[gi].items.erase (s.swatches[gi].items.begin () + (ptrdiff_t) ii);
        *result = Value{{"deleted", {name}}};
        return true;
    }

    bool cmd_swatches_rename (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "swatches.rename";
        size_t gi, ii;
        if (!locate_swatch (s, p, cmd, &gi, &ii, err))
        {
            return false;
        }
        std::string to;
        if (!param_str (p, "name", &to) || to.empty ())
        {
            return bad_params (err, cmd, "missing `name`");
        }
        size_t a, b;
        if (s.swatches[gi].items[ii].name != to && find_swatch (s.swatches, to, &a, &b))
        {
            return bad_params (err, cmd, "a swatch named \"%s\" already exists", to.c_str ());
        }
        s.swatches[gi].items[ii].name = to;
        *result                       = Value{{"name", to}};
        return true;
    }

    /* A swatch as the foreground (or background) colour. */
    bool cmd_swatches_use (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "swatches.use";
        size_t gi, ii;
        if (!locate_swatch (s, p, cmd, &gi, &ii, err))
        {
            return false;
        }
        std::string target = "foreground";
        param_str (p, "target", &target);
        if (target != "foreground" && target != "background")
        {
            return bad_params (err, cmd, "target must be foreground or background");
        }
        const Swatch& sw = s.swatches[gi].items[ii];
        float* dst       = target == "foreground" ? s.tools.foreground : s.tools.background;
        for (int i = 0; i < 3; i++)
        {
            dst[i] = sw.rgb[i];
        }
        dst[3]  = 1.0f;
        *result = Value{{"swatch", sw.name}, {"target", target}, {"hex", swatch_hex (sw.rgb)}};
        return true;
    }

    bool cmd_swatches_reset (Session& s, const Value& p, Value* result, Error* err)
    {
        bool append = false;
        param_bool (p, "append", &append);
        std::vector<SwatchGroup> defaults = builtin_swatches ();
        if (append)
        {
            s.swatches.insert (s.swatches.end (), defaults.begin (), defaults.end ());
        }
        else
        {
            s.swatches = defaults;
        }
        *result = Value{{"groups", s.swatches.size ()}, {"count", swatch_total (s.swatches)}};
        return true;
    }

    bool cmd_swatches_new_group (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "swatches.newGroup";
        if (s.swatches.size () >= MAX_GROUPS)
        {
            return bad_params (err, cmd, "too many swatch groups");
        }
        std::string base = "Group";
        param_str (p, "name", &base);
        std::string name = base;
        for (int n = 2;; n++)
        {
            bool taken = false;
            for (size_t g = 0; g < s.swatches.size (); g++)
            {
                taken = taken || s.swatches[g].name == name;
            }
            if (!taken)
            {
                break;
            }
            name = base + " " + std::to_string (n);
        }
        SwatchGroup ng;
        ng.name = name;
        s.swatches.push_back (ng);
        *result = Value{{"group", name}};
        return true;
    }

    bool cmd_swatches_delete_group (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "swatches.deleteGroup";
        std::string group;
        if (!param_str (p, "group", &group))
        {
            return bad_params (err, cmd, "missing `group`");
        }
        for (size_t g = 0; g < s.swatches.size (); g++)
        {
            if (s.swatches[g].name != group)
            {
                continue;
            }
            bool keep = false;
            param_bool (p, "keepSwatches", &keep);
            if (keep && g > 0)
            {
                s.swatches[g - 1].items.insert (
                    s.swatches[g - 1].items.end (), s.swatches[g].items.begin (), s.swatches[g].items.end ());
            }
            s.swatches.erase (s.swatches.begin () + (ptrdiff_t) g);
            *result = Value{{"group", group}};
            return true;
        }
        return bad_params (err, cmd, "no swatch group \"%s\"", group.c_str ());
    }

    const CommandSpec SWATCH_COMMANDS[] = {
        {"swatches.list", "List Swatches", "", NULL, "{} → {groups:[{name,swatches:[{name,hex}]}],count}", always,
            cmd_swatches_list, false},
        {"swatches.add", "New Swatch…", "", NULL,
            "{\"color\":\"#rrggbb\"?=foreground,\"name\":str?=\"Color Swatch "
            "N\",\"group\":name?=first,\"index\":n?=end}",
            always, cmd_swatches_add, true},
        {"swatches.delete", "Delete Swatch", "", NULL, "{\"swatch\":name,\"group\":name?}", always, cmd_swatches_delete,
            true},
        {"swatches.rename", "Rename Swatch…", "", NULL, "{\"swatch\":name,\"group\":name?,\"name\":str}", always,
            cmd_swatches_rename, true},
        {"swatches.use", "Use Swatch", "", NULL,
            "{\"swatch\":name,\"group\":name?,\"target\":\"foreground|background\"=\"foreground\"}", always,
            cmd_swatches_use, true},
        {"swatches.reset", "Reset Swatches", "", NULL, "{\"append\":bool=false}", always, cmd_swatches_reset, true},
        {"swatches.newGroup", "New Swatch Group…", "", NULL, "{\"name\":str=\"Group\"}", always, cmd_swatches_new_group,
            true},
        {"swatches.deleteGroup", "Delete Swatch Group", "", NULL, "{\"group\":name,\"keepSwatches\":bool=false}",
            always, cmd_swatches_delete_group, true},
    };

    void register_swatch_commands (std::vector<CommandSpec>* out)
    {
        out->insert (
            out->end (), SWATCH_COMMANDS, SWATCH_COMMANDS + sizeof SWATCH_COMMANDS / sizeof SWATCH_COMMANDS[0]);
    }

} // namespace engine
