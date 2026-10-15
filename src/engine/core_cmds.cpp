#include "engine/layers.h"
#include "engine/session.h"
#include "raster/raster.h"
#include <math.h>
#include <string.h>

namespace engine
{

    using color::BlendMode;
    using color::ColorMode;
    using color::PixelFormat;
    using color::SampleType;
    using geom::Rect;

    const uint32_t MAX_DIMENSION = 300000;
    const double MAX_RESOLUTION  = 30000.0;

    bool color_mode_from_str (const std::string& s, ColorMode* out)
    {
        if (s == "rgb")
        {
            *out = ColorMode::Rgb;
        }
        else if (s == "gray" || s == "grayscale")
        {
            *out = ColorMode::Grayscale;
        }
        else if (s == "cmyk")
        {
            *out = ColorMode::Cmyk;
        }
        else if (s == "lab")
        {
            *out = ColorMode::Lab;
        }
        else
        {
            return false;
        }
        return true;
    }

    bool sample_type_from_depth (uint64_t depth, SampleType* out)
    {
        if (depth == 8)
        {
            *out = SampleType::U8;
        }
        else if (depth == 16)
        {
            *out = SampleType::U16;
        }
        else if (depth == 32)
        {
            *out = SampleType::F32;
        }
        else
        {
            return false;
        }
        return true;
    }

    /* "backgroundColor": [r, g, b] in 0..1, or the toolbox background colour. */
    bool new_background_color (const Session& s, const Value& p, float out[4], Error* err)
    {
        const Value* v = param (p, "backgroundColor");
        if (v == NULL)
        {
            memcpy (out, s.tools.background, sizeof (float) * 4);
            return true;
        }
        if (!v->is_array () || v->size () != 3)
        {
            return bad_params (err, "file.new", "`backgroundColor` must be [r,g,b] in 0..1");
        }
        for (size_t i = 0; i < 3; i++)
        {
            const Value& c = (*v)[i];
            if (!c.is_number ())
            {
                return bad_params (err, "file.new", "`backgroundColor` must be [r,g,b] in 0..1");
            }
            double d = c.get<double> ();
            if (!isfinite (d) || d < 0.0 || d > 1.0)
            {
                return bad_params (err, "file.new", "`backgroundColor` must be [r,g,b] in 0..1");
            }
            out[i] = (float) d;
        }
        out[3] = 1.0f;
        return true;
    }

    bool validate_new_params (const Session& s, const Value& p, Error* err)
    {
        if (p.is_null ())
        {
            return true;
        }
        if (!p.is_object ())
        {
            return bad_params (err, "file.new", "params must be a JSON object");
        }
        for (Value::const_iterator it = p.begin (); it != p.end (); ++it)
        {
            const std::string& key = it.key ();
            const Value& v         = it.value ();
            if (key == "width" || key == "height" || key == "resolution")
            {
                double max = key == "resolution" ? MAX_RESOLUTION : (double) MAX_DIMENSION;
                if (!v.is_number () || !isfinite (v.get<double> ()) || v.get<double> () < 1.0 || v.get<double> () > max)
                {
                    return bad_params (err, "file.new", "`%s` must be a number within 1..=%g", key.c_str (), max);
                }
            }
            else if (key == "mode")
            {
                ColorMode m;
                if (!v.is_string () || !color_mode_from_str (v.get<std::string> (), &m))
                {
                    return bad_params (err, "file.new", "`mode` must be rgb, gray, grayscale, cmyk or lab");
                }
            }
            else if (key == "depth")
            {
                SampleType t;
                if (!v.is_number_integer () || v.get<int64_t> () < 0
                    || !sample_type_from_depth ((uint64_t) v.get<int64_t> (), &t))
                {
                    return bad_params (err, "file.new", "`depth` must be 8, 16 or 32");
                }
            }
            else if (key == "background")
            {
                float c[4];
                std::string b = v.is_string () ? v.get<std::string> () : "";
                bool named    = b == "white" || b == "black" || b == "transparent" || b == "backgroundColor";
                if (!v.is_string () || (!named && !parse_hex (b.c_str (), c)))
                {
                    return bad_params (err, "file.new",
                        "`background` must be white, black, transparent, backgroundColor or a valid hex colour");
                }
            }
            else if (key == "backgroundColor")
            {
                float c[4];
                if (!new_background_color (s, p, c, err))
                {
                    return false;
                }
            }
            else if (key == "name")
            {
                if (!v.is_string ())
                {
                    return bad_params (err, "file.new", "`name` must be a string");
                }
            }
            else
            {
                return bad_params (err, "file.new", "unknown parameter `%s`", key.c_str ());
            }
        }
        return true;
    }

    /* A size sent as a float (512.0 from a UI field) is still that size. */
    uint32_t new_dimension (const Value& p, const char* key, uint32_t fallback)
    {
        uint64_t u;
        if (param_u64 (p, key, &u))
        {
            if (u < 1)
            {
                u = 1;
            }
            if (u > MAX_DIMENSION)
            {
                u = MAX_DIMENSION;
            }
            return (uint32_t) u;
        }
        double d;
        if (param_f64 (p, key, &d) && isfinite (d))
        {
            d = round (d);
            if (d < 1.0)
            {
                d = 1.0;
            }
            if (d > MAX_DIMENSION)
            {
                d = MAX_DIMENSION;
            }
            return (uint32_t) d;
        }
        return fallback;
    }

    bool cmd_file_new (Session& s, const Value& p, Value* result, Error* err)
    {
        if (!validate_new_params (s, p, err))
        {
            return false;
        }
        geom::Size size = {new_dimension (p, "width", 1920), new_dimension (p, "height", 1080)};
        std::string str;
        ColorMode mode = ColorMode::Rgb;
        if (param_str (p, "mode", &str))
        {
            color_mode_from_str (str, &mode);
        }
        SampleType depth = SampleType::U8;
        uint64_t bits;
        if (param_u64 (p, "depth", &bits))
        {
            sample_type_from_depth (bits, &depth);
        }
        std::string name = "Untitled";
        param_str (p, "name", &name);
        double res = 72.0;
        param_f64 (p, "resolution", &res);
        if (res < 1.0)
        {
            res = 1.0;
        }
        if (res > MAX_RESOLUTION)
        {
            res = MAX_RESOLUTION;
        }
        std::string background = "white";
        param_str (p, "background", &background);

        doc::Document d;
        if (background == "backgroundColor")
        {
            float c[4];
            if (!new_background_color (s, p, c, err))
            {
                return false;
            }
            d = doc::Document::with_background (
                s.ids, name, size, mode, depth, color::Color::rgba (c[0], c[1], c[2], 1.0f), s.cmyk);
        }
        else if (background == "transparent")
        {
            d = doc::Document::make (s.ids, name, size, mode, depth);
            d.layers.push_back (doc::Layer::raster (s.ids, "Layer 1", d.pixel_format ()));
        }
        else if (background == "black")
        {
            d = doc::Document::with_background (s.ids, name, size, mode, depth, color::Color::black (), s.cmyk);
        }
        else
        {
            float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            parse_hex (background.c_str (), c);
            d = doc::Document::with_background (
                s.ids, name, size, mode, depth, color::Color::rgba (c[0], c[1], c[2], c[3]), s.cmyk);
        }
        d.resolution_dpi = (float) res;
        size_t index     = s.add_document (d, "");
        *result          = Value{{"document", index}};
        return true;
    }

    bool cmd_file_close (Session& s, const Value& p, Value* result, Error* err)
    {
        uint64_t index;
        if (!param_u64 (p, "document", &index))
        {
            index = s.active_index ();
        }
        if (!s.close ((size_t) index))
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        return true;
    }

    bool cmd_edit_undo (Session& s, const Value& p, Value* result, Error* err)
    {
        *result = s.undo ();
        return true;
    }

    bool cmd_edit_redo (Session& s, const Value& p, Value* result, Error* err)
    {
        *result = s.redo ();
        return true;
    }

    bool cmd_select_all (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        float one    = 1.0f;
        d->selection = raster::Surface (PixelFormat::gray8 ());
        d->selection.fill_rect (d->bounds (), &one);
        d->has_selection = true;
        return e.commit ("Select All", err);
    }

    bool cmd_select_deselect (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        d->has_selection = false;
        d->selection     = raster::Surface (PixelFormat::gray8 ());
        return e.commit ("Deselect", err);
    }

    bool cmd_select_inverse (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        raster::Surface old = d->has_selection ? d->selection : raster::Surface (PixelFormat::gray8 ());
        Rect b              = d->bounds ();
        std::vector<float> data;
        if (!old.read_region (b, &data))
        {
            return fail (err, ErrorKind::Other, "the canvas is too large to invert the selection");
        }
        for (size_t i = 0; i < data.size (); i++)
        {
            data[i] = 1.0f - data[i];
        }
        raster::Surface m (PixelFormat::gray8 ());
        m.write_region (b, data.data (), data.size ());
        m.prune ();
        d->selection     = m;
        d->has_selection = true;
        return e.commit ("Inverse", err);
    }

    /* The colour that leaves everything below unchanged in `mode` (New Layer > Fill with neutral
       color). False for modes that have none. */
    bool neutral_color (BlendMode mode, float rgb[3])
    {
        float v;
        switch (mode)
        {
        case BlendMode::Darken:
        case BlendMode::Multiply:
        case BlendMode::ColorBurn:
        case BlendMode::LinearBurn:
        case BlendMode::DarkerColor:
        case BlendMode::Divide:
            v = 1.0f;
            break;
        case BlendMode::Lighten:
        case BlendMode::Screen:
        case BlendMode::ColorDodge:
        case BlendMode::LinearDodge:
        case BlendMode::LighterColor:
        case BlendMode::Difference:
        case BlendMode::Exclusion:
        case BlendMode::Subtract:
            v = 0.0f;
            break;
        case BlendMode::Overlay:
        case BlendMode::SoftLight:
        case BlendMode::HardLight:
        case BlendMode::VividLight:
        case BlendMode::LinearLight:
        case BlendMode::PinLight:
            v = 0.5f;
            break;
        default:
            return false;
        }
        rgb[0] = rgb[1] = rgb[2] = v;
        return true;
    }

    bool flag_param (const Value& p, const char* key, bool* out, Error* err)
    {
        *out = false;
        if (!param_present (p, key))
        {
            return true;
        }
        if (!param_bool (p, key, out))
        {
            return bad_params (err, "layer.new.layer", "`%s` must be true or false", key);
        }
        return true;
    }

    /* A pixel layer above the active one. It joins the clip group it lands in; clipToPrevious clips
       it anyway. */
    bool cmd_layer_new_layer (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.new.layer";
        bool clip, fill_neutral;
        if (!flag_param (p, "clipToPrevious", &clip, err) || !flag_param (p, "fillNeutral", &fill_neutral, err))
        {
            return false;
        }

        BlendMode mode = BlendMode::Normal;
        if (param_present (p, "mode"))
        {
            std::string name;
            if (!param_str (p, "mode", &name))
            {
                return bad_params (err, cmd, "`mode` must be a blend mode name");
            }
            if (!blend_from_str (name.c_str (), &mode) || mode == BlendMode::PassThrough)
            {
                return bad_params (err, cmd, "unknown layer blend mode `%s`", name.c_str ());
            }
        }
        float opacity = 1.0f;
        if (param_present (p, "opacity"))
        {
            double o;
            if (!param_f64 (p, "opacity", &o) || !isfinite (o))
            {
                return bad_params (err, cmd, "`opacity` must be a number from 0 to 1");
            }
            opacity = (float) (o < 0.0 ? 0.0 : o > 1.0 ? 1.0 : o);
        }
        doc::LabelColor label = doc::LabelColor::None;
        if (param_present (p, "color"))
        {
            std::string id;
            if (!param_str (p, "color", &id) || !doc::label_color_from_id (id.c_str (), &label))
            {
                return bad_params (err, cmd, "`color` must be a layer color id");
            }
        }
        float neutral[3];
        if (fill_neutral && !neutral_color (mode, neutral))
        {
            return bad_params (err, cmd, "%s has no neutral color to fill with", color::blend_label (mode));
        }
        std::string name;
        bool has_name = param_present (p, "name");
        if (has_name && !param_str (p, "name", &name))
        {
            return bad_params (err, cmd, "`name` must be a string");
        }

        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::Layer layer =
            doc::Layer::raster (s.ids, has_name ? name : d->next_layer_name ("Layer"), d->pixel_format ());
        layer.blend   = mode;
        layer.opacity = opacity;
        layer.label   = label;
        if (fill_neutral)
        {
            float rgba[4] = {neutral[0], neutral[1], neutral[2], 1.0f};
            float px[raster::MAX_CHANNELS];
            raster::from_rgba (layer.pixels.format (), rgba, s.cmyk, px);
            layer.pixels.fill_rect (d->bounds (), px);
        }
        doc::LayerId id = d->insert_above (*e.active_layer (), layer);
        clip_settle (d, std::vector<doc::LayerId> (1, id), ClipBefore (), clip);
        *e.active_layer () = id;
        if (!e.commit ("New Layer", err))
        {
            return false;
        }
        *result = Value{{"layer", id}};
        return true;
    }

    const CommandSpec CORE_COMMANDS[] = {
        {"file.new", "New…", "File", "Cmd+N",
            "{\"width\":1..300000=1920,\"height\":1..300000=1080,\"mode\":\"rgb|gray|grayscale|cmyk|lab\"=\"rgb\","
            "\"depth\":8|16|32=8,"
            "\"background\":\"white|black|backgroundColor|transparent|#rrggbb\"=\"white\",\"backgroundColor\":[r,g,b]?,"
            "\"resolution\":1..30000=72,\"name\":str}",
            always, cmd_file_new, true},
        {"file.close", "Close", "File", "Cmd+W", "{\"document\":index?}", has_doc, cmd_file_close, true},
        {"edit.undo", "Undo", "Edit", "Cmd+Z", "{}", can_undo, cmd_edit_undo, true},
        {"edit.redo", "Redo", "Edit", "Cmd+Shift+Z", "{}", can_redo, cmd_edit_redo, true},
        {"select.all", "All", "Select", "Cmd+A", "{}", has_doc, cmd_select_all, true},
        {"select.deselect", "Deselect", "Select", "Cmd+D", "{}", has_selection, cmd_select_deselect, true},
        {"select.inverse", "Inverse", "Select", "Cmd+Shift+I", "{}", has_selection, cmd_select_inverse, true},
        {"layer.new.layer", "Layer…", "Layer/New", "Cmd+Shift+N",
            "{\"name\":str?,\"clipToPrevious\":bool=false,\"color\":\"none|red|orange|yellow|green|seafoam|blue|indigo|"
            "magenta|fuchsia|violet|gray\"=\"none\","
            "\"mode\":\"Normal|Multiply|…\"=\"Normal\",\"opacity\":0..1=1,\"fillNeutral\":bool=false}",
            has_doc, cmd_layer_new_layer, true},
    };

    void register_core_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), CORE_COMMANDS, CORE_COMMANDS + sizeof CORE_COMMANDS / sizeof CORE_COMMANDS[0]);
    }

} // namespace engine
