#include "codecs/codecs.h"
#include "compose/compose.h"
#include "engine/session.h"
#include "io/io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

const char* USAGE =
    "photoc-cli: headless Photoc\n"
    "\n"
    "USAGE:\n"
    "  photoc-cli convert <in> <out> [--format <ext>] [--quality <1-100>]\n"
    "      Convert between formats (.png, .jpg, .ppm, .pgm, .pam, .pfm). --quality sets the JPEG quality.\n"
    "  photoc-cli info <file> [--compact]\n"
    "      Print the document as JSON (size, mode, depth, layer tree).\n"
    "  photoc-cli run (<file> | --new <json>) --cmd <id> [--params <json>] [--cmd ...] [--out <file>] [--format "
    "<ext>]\n"
    "      [--quality <1-100>]\n"
    "      Open a file (or make a new document), run engine commands in order, save the result.\n"
    "      Each --params applies to the preceding --cmd. Prints each command's JSON result.\n"
    "  photoc-cli commands [--json] [--filter <text>]\n"
    "      List the engine command registry.\n"
    "\n"
    "  photoc-cli <subcommand> --help (or -h) prints this text. A flag the subcommand doesn't take is\n"
    "  an error.\n";

struct Flag
{
    std::string name;
    std::string value;
    bool has_value;
};

struct Args
{
    std::vector<std::string> positional;
    std::vector<Flag> flags;
};

typedef int (*SubcommandFn) (const Args& a);

struct Subcommand
{
    const char* name;
    const char* const* values;
    const char* const* bare;
    SubcommandFn run;
};

/* PHOTOC_TIMING=1 logs how long commands and file operations take, on stderr. */
bool timing_enabled ()
{
    const char* t = getenv ("PHOTOC_TIMING");
    return t != NULL && t[0] != 0 && strcmp (t, "0") != 0;
}

bool in_list (const char* const* list, const std::string& s)
{
    for (size_t i = 0; list[i] != NULL; i++)
    {
        if (s == list[i])
        {
            return true;
        }
    }
    return false;
}

/* 0 = parsed, 1 = help asked, -1 = usage error (message printed). */
int parse_args (const Subcommand& sub, int argc, char** argv, Args* a)
{
    for (int i = 0; i < argc; i++)
    {
        std::string s = argv[i];
        if (s == "--help" || s == "-h")
        {
            return 1;
        }
        size_t eq = s.find ('=');
        if (s.compare (0, 2, "--") == 0 && eq != std::string::npos)
        {
            std::string k = s.substr (0, eq);
            if (in_list (sub.bare, k))
            {
                fprintf (stderr, "error: %s takes no value\n", k.c_str ());
                return -1;
            }
            if (!in_list (sub.values, k))
            {
                fprintf (stderr, "error: unknown flag %s for %s\n", k.c_str (), sub.name);
                return -1;
            }
            Flag f = {k, s.substr (eq + 1), true};
            a->flags.push_back (f);
        }
        else if (in_list (sub.values, s))
        {
            if (i + 1 >= argc)
            {
                fprintf (stderr, "error: %s needs a value\n", s.c_str ());
                return -1;
            }
            Flag f = {s, argv[++i], true};
            a->flags.push_back (f);
        }
        else if (in_list (sub.bare, s))
        {
            Flag f = {s, "", false};
            a->flags.push_back (f);
        }
        else if (s.compare (0, 2, "--") == 0)
        {
            fprintf (stderr, "error: unknown flag %s for %s\n", s.c_str (), sub.name);
            return -1;
        }
        else
        {
            a->positional.push_back (s);
        }
    }
    return 0;
}

const char* flag_value (const Args& a, const char* name)
{
    for (size_t i = a.flags.size (); i-- > 0;)
    {
        if (a.flags[i].name == name && a.flags[i].has_value)
        {
            return a.flags[i].value.c_str ();
        }
    }
    return NULL;
}

bool flag_set (const Args& a, const char* name)
{
    for (size_t i = 0; i < a.flags.size (); i++)
    {
        if (a.flags[i].name == name)
        {
            return true;
        }
    }
    return false;
}

/* --quality as JPEG quality; false (message printed) when it isn't 1..100. */
bool encode_options (const Args& a, codecs::EncodeOptions* out)
{
    *out          = codecs::EncodeOptions::defaults ();
    const char* q = flag_value (a, "--quality");
    if (q == NULL)
    {
        return true;
    }
    char* end = NULL;
    long v    = strtol (q, &end, 10);
    if (end == q || *end != 0 || v < 1 || v > 100)
    {
        fprintf (stderr, "error: --quality must be a whole number from 1 to 100\n");
        return false;
    }
    out->jpeg_quality = (int) v;
    out->webp_quality = (int) v;
    return true;
}

void print_warnings (const std::vector<std::string>& warnings)
{
    for (size_t i = 0; i < warnings.size (); i++)
    {
        fprintf (stderr, "warning: %s\n", warnings[i].c_str ());
    }
}

const char* mode_name (color::ColorMode m)
{
    switch (m)
    {
    case color::ColorMode::Bitmap:
        return "bitmap";
    case color::ColorMode::Grayscale:
        return "grayscale";
    case color::ColorMode::Indexed:
        return "indexed";
    case color::ColorMode::Rgb:
        return "rgb";
    case color::ColorMode::Cmyk:
        return "cmyk";
    case color::ColorMode::Lab:
        return "lab";
    case color::ColorMode::Multichannel:
        return "multichannel";
    case color::ColorMode::Duotone:
        return "duotone";
    }
    return "rgb";
}

engine::Value layer_json (const doc::Layer& l)
{
    engine::Value v = {
        {"id", l.id},
        {"name", l.name},
        {"kind", doc::layer_kind_name (l.kind)},
        {"visible", l.visible},
        {"opacity", l.opacity},
        {"fillOpacity", l.fill_opacity},
        {"blend", color::blend_label (l.blend)},
        {"clipped", l.clipped},
        {"mask", l.has_mask},
    };
    if (l.kind == doc::LayerKind::Raster)
    {
        geom::Rect b = l.pixels.content_bounds ();
        v["bounds"]  = {b.x0, b.y0, b.x1, b.y1};
    }
    if (l.kind == doc::LayerKind::Group)
    {
        engine::Value children = engine::Value::array ();
        for (size_t i = l.children.size (); i-- > 0;)
        {
            children.push_back (layer_json (l.children[i]));
        }
        v["children"] = children;
    }
    return v;
}

/* Layers are listed top to bottom, as in the Layers panel. */
engine::Value document_json (const doc::Document& d)
{
    engine::Value layers = engine::Value::array ();
    for (size_t i = d.layers.size (); i-- > 0;)
    {
        layers.push_back (layer_json (d.layers[i]));
    }
    return engine::Value{
        {"name", d.name},
        {"width", d.size.width},
        {"height", d.size.height},
        {"mode", mode_name (d.mode)},
        {"depth", color::sample_bits (d.depth)},
        {"resolution", d.resolution_dpi},
        {"selection", d.has_selection},
        {"layers", layers},
    };
}

int cmd_convert (const Args& a)
{
    if (a.positional.size () != 2)
    {
        fprintf (stderr, "error: convert needs <in> <out>\n");
        return 2;
    }
    codecs::EncodeOptions opts;
    if (!encode_options (a, &opts))
    {
        return 2;
    }
    doc::IdSource ids;
    io::ImportResult r;
    char err[512];
    if (!io::open_file (ids, NULL, a.positional[0].c_str (), &r, err, sizeof err))
    {
        fprintf (stderr, "error: %s\n", err);
        return 1;
    }
    print_warnings (r.warnings);
    std::vector<std::string> warnings;
    if (!io::save_file (r.document, NULL, a.positional[1].c_str (), flag_value (a, "--format"), opts,
            compose::default_settings (), &warnings, err, sizeof err))
    {
        fprintf (stderr, "error: %s\n", err);
        return 1;
    }
    print_warnings (warnings);
    return 0;
}

int cmd_info (const Args& a)
{
    if (a.positional.size () != 1)
    {
        fprintf (stderr, "error: info needs <file>\n");
        return 2;
    }
    doc::IdSource ids;
    io::ImportResult r;
    char err[512];
    text::FontDb fonts;
    if (!io::open_file (ids, &fonts, a.positional[0].c_str (), &r, err, sizeof err))
    {
        fprintf (stderr, "error: %s\n", err);
        return 1;
    }
    print_warnings (r.warnings);
    printf ("%s\n", document_json (r.document).dump (flag_set (a, "--compact") ? -1 : 2).c_str ());
    return 0;
}

int cmd_run (const Args& a)
{
    codecs::EncodeOptions opts;
    if (!encode_options (a, &opts))
    {
        return 2;
    }
    engine::Session s;
    s.timing = timing_enabled ();
    engine::Error e;
    engine::Value result;
    const char* new_params = flag_value (a, "--new");
    if (new_params != NULL)
    {
        engine::Value p = engine::Value::parse (new_params, NULL, false);
        if (p.is_discarded ())
        {
            fprintf (stderr, "error: --new is not valid JSON\n");
            return 2;
        }
        if (!s.execute ("file.new", p, &result, &e))
        {
            fprintf (stderr, "error: %s\n", e.message.c_str ());
            return 1;
        }
    }
    else if (a.positional.size () == 1)
    {
        io::ImportResult r;
        char err[512];
        double t0 = engine::timing_now ();
        if (!io::open_file (s.ids, s.fonts (), a.positional[0].c_str (), &r, err, sizeof err))
        {
            fprintf (stderr, "error: %s\n", err);
            return 1;
        }
        engine::timing_log (s, "open file", engine::timing_now () - t0);
        engine::timing_log (s, "  read from disk", r.timing.read);
        engine::timing_log (s, "  decode", r.timing.decode);
        engine::timing_log (s, "  build document", r.timing.build);
        engine::timing_log (s, "  smart object contents", r.timing.smart);
        print_warnings (r.warnings);
        s.add_document (r.document, a.positional[0]);
    }
    else
    {
        fprintf (stderr, "error: run needs <file> or --new <json>\n");
        return 2;
    }

    for (size_t i = 0; i < a.flags.size (); i++)
    {
        if (a.flags[i].name != "--cmd")
        {
            continue;
        }
        engine::Value params;
        if (i + 1 < a.flags.size () && a.flags[i + 1].name == "--params")
        {
            params = engine::Value::parse (a.flags[i + 1].value, NULL, false);
            if (params.is_discarded ())
            {
                fprintf (stderr, "error: --params for %s is not valid JSON\n", a.flags[i].value.c_str ());
                return 2;
            }
        }
        engine::Value out;
        if (!s.execute (a.flags[i].value.c_str (), params, &out, &e))
        {
            fprintf (stderr, "error: %s\n", e.message.c_str ());
            return 1;
        }
        printf ("%s\n", engine::Value{{"command", a.flags[i].value}, {"result", out}}.dump ().c_str ());
    }

    const char* out_path = flag_value (a, "--out");
    if (out_path != NULL)
    {
        const engine::DocState* d = s.active ();
        if (d == NULL)
        {
            fprintf (stderr, "error: no document left to save\n");
            return 1;
        }
        std::vector<std::string> warnings;
        char err[512];
        io::IoTiming t;
        double t0 = engine::timing_now ();
        if (!io::save_file_timed (*d->doc, s.fonts (), out_path, flag_value (a, "--format"), opts,
                compose::default_settings (), &warnings, err, sizeof err, &t))
        {
            fprintf (stderr, "error: %s\n", err);
            return 1;
        }
        engine::timing_log (s, "save file", engine::timing_now () - t0);
        engine::timing_log (s, "  flatten", t.flatten);
        engine::timing_log (s, "  encode", t.encode);
        engine::timing_log (s, "  write to disk", t.write);
        print_warnings (warnings);
    }
    return 0;
}

int cmd_commands (const Args& a)
{
    engine::Session s;
    s.timing                                    = timing_enabled ();
    const char* filter                          = flag_value (a, "--filter");
    const std::vector<engine::CommandSpec>& all = s.commands ();
    engine::Value list                          = engine::Value::array ();
    for (size_t i = 0; i < all.size (); i++)
    {
        const engine::CommandSpec& c = all[i];
        if (filter != NULL && strstr (c.id, filter) == NULL && strstr (c.label, filter) == NULL)
        {
            continue;
        }
        if (flag_set (a, "--json"))
        {
            list.push_back (engine::Value{{"id", c.id}, {"label", c.label}, {"menu", c.menu},
                {"shortcut", c.shortcut ? c.shortcut : ""}, {"params", c.params}});
        }
        else
        {
            printf ("%-24s %-14s %-14s %s\n", c.id, c.label, c.menu, c.shortcut ? c.shortcut : "");
        }
    }
    if (flag_set (a, "--json"))
    {
        printf ("%s\n", list.dump (2).c_str ());
    }
    return 0;
}

const char* const NONE[]            = {NULL};
const char* const CONVERT_VALUES[]  = {"--format", "--quality", NULL};
const char* const INFO_BARE[]       = {"--compact", NULL};
const char* const RUN_VALUES[]      = {"--new", "--cmd", "--params", "--out", "--format", "--quality", NULL};
const char* const COMMANDS_VALUES[] = {"--filter", NULL};
const char* const COMMANDS_BARE[]   = {"--json", NULL};

const Subcommand SUBCOMMANDS[] = {
    {"convert", CONVERT_VALUES, NONE, cmd_convert},
    {"info", NONE, INFO_BARE, cmd_info},
    {"run", RUN_VALUES, NONE, cmd_run},
    {"commands", COMMANDS_VALUES, COMMANDS_BARE, cmd_commands},
};

int main (int argc, char** argv)
{
    if (argc < 2 || strcmp (argv[1], "--help") == 0 || strcmp (argv[1], "-h") == 0)
    {
        fputs (USAGE, argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    for (size_t i = 0; i < sizeof SUBCOMMANDS / sizeof SUBCOMMANDS[0]; i++)
    {
        const Subcommand& sub = SUBCOMMANDS[i];
        if (strcmp (argv[1], sub.name) != 0)
        {
            continue;
        }
        Args a;
        int parsed = parse_args (sub, argc - 2, argv + 2, &a);
        if (parsed == 1)
        {
            fputs (USAGE, stdout);
            return 0;
        }
        if (parsed < 0)
        {
            return 2;
        }
        return sub.run (a);
    }
    fprintf (stderr, "error: unknown subcommand %s\n\n%s", argv[1], USAGE);
    return 2;
}
