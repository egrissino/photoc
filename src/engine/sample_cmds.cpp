#include "compose/compose.h"
#include "engine/session.h"
#include <math.h>
#include <string.h>
#include <string>
#include <vector>

namespace engine
{

    /* Eyedropper sampling: Photoshop's Sample Size (Point Sample, or the average of a 3x3 ...
       101x101 square centred on the pixel) and Sample (which layers the colour comes from). The
       Eyedropper tool, the painting tools' Alt-click and the Color Picker read colours through it.
       The average is weighted by alpha and returned as straight RGBA. */

    using geom::Rect;

    /* The top-level layers up to and including the one holding the active layer. */
    std::vector<doc::Layer> layers_to_active (const doc::Document& d, doc::LayerId active)
    {
        std::vector<doc::Layer> out;
        doc::LayerPath path;
        if (!d.path_of (active, &path) || path.empty ())
        {
            return d.layers;
        }
        for (size_t i = 0; i <= path[0] && i < d.layers.size (); i++)
        {
            out.push_back (d.layers[i]);
        }
        return out;
    }

    /* The colour of the `size` x `size` square centred on pixel (x, y), from "current" (the active
       layer), "currentAndBelow" or "all" (the composite). Transparent off the canvas. */
    void sample_color (const doc::Document& d, doc::LayerId active, double x, double y, int size,
        const std::string& which, const color::CmykSpace* cmyk, float out[4])
    {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        int32_t px = (int32_t) floor (x), py = (int32_t) floor (y), half = size / 2;
        Rect r = Rect{px - half, py - half, px - half + size, py - half + size}.intersect (d.bounds ());
        if (r.is_empty () || !d.bounds ().contains (px, py))
        {
            return;
        }
        std::vector<float> rgba;
        const doc::Layer* l = d.layer (active);
        if (which == "current" && l != NULL && l->surface () != NULL)
        {
            rgba.resize ((size_t) r.width () * r.height () * 4);
            l->surface ()->read_rgba (r, cmyk, rgba.data ());
        }
        else
        {
            compose::RenderSettings settings = compose::default_settings ();
            settings.cmyk                    = cmyk;
            if (which == "currentAndBelow")
            {
                doc::Document below = d;
                below.layers        = layers_to_active (d, active);
                rgba                = compose::render (below, r, settings).px;
            }
            else
            {
                rgba = compose::render (d, r, settings).px;
            }
        }
        double acc[3] = {0, 0, 0}, alpha = 0;
        size_t n = rgba.size () / 4;
        for (size_t i = 0; i < n; i++)
        {
            double a = rgba[i * 4 + 3];
            for (int c = 0; c < 3; c++)
            {
                acc[c] += rgba[i * 4 + c] * a;
            }
            alpha += a;
        }
        if (n == 0 || alpha <= 0.0)
        {
            return;
        }
        for (int c = 0; c < 3; c++)
        {
            out[c] = (float) (acc[c] / alpha);
        }
        out[3] = (float) (alpha / (double) n);
    }

    const int SAMPLE_SIZES[] = {1, 3, 5, 11, 31, 51, 101};

    bool cmd_sample_color (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "document.sampleColor";
        double x, y;
        if (!param_f64 (p, "x", &x) || !param_f64 (p, "y", &y) || !isfinite (x) || !isfinite (y))
        {
            return bad_params (err, cmd, "`x` and `y` must be finite numbers");
        }
        int size = 1;
        int64_t sz;
        std::string point;
        if (param_i64 (p, "size", &sz))
        {
            bool ok = false;
            for (size_t i = 0; i < sizeof SAMPLE_SIZES / sizeof SAMPLE_SIZES[0]; i++)
            {
                ok = ok || SAMPLE_SIZES[i] == sz;
            }
            if (!ok)
            {
                return bad_params (err, cmd, "`size` must be point, 1, 3, 5, 11, 31, 51 or 101");
            }
            size = (int) sz;
        }
        else if (param_str (p, "size", &point) && point != "point")
        {
            return bad_params (err, cmd, "`size` must be point, 1, 3, 5, 11, 31, 51 or 101");
        }
        std::string which = "all";
        param_str (p, "sampleLayer", &which);
        if (which != "current" && which != "currentAndBelow" && which != "all")
        {
            return bad_params (err, cmd, "`sampleLayer` must be current, currentAndBelow or all");
        }
        const DocState* d = s.active ();
        float c[4];
        sample_color (*d->doc, d->active_layer, x, y, size, which, s.cmyk, c);
        *result = Value::array ();
        for (int i = 0; i < 4; i++)
        {
            result->push_back (c[i]);
        }
        return true;
    }

    const CommandSpec SAMPLE_COMMANDS[] = {
        {"document.sampleColor", "Sample Color", "", NULL,
            "{\"x\":f64,\"y\":f64,\"size\":\"point\"|1|3|5|11|31|51|101=1,\"sampleLayer\":\"current|currentAndBelow|"
            "all\"=\"all\"} → [r,g,b,a]",
            has_doc, cmd_sample_color, false},
    };

    void register_sample_commands (std::vector<CommandSpec>* out)
    {
        out->insert (
            out->end (), SAMPLE_COMMANDS, SAMPLE_COMMANDS + sizeof SAMPLE_COMMANDS / sizeof SAMPLE_COMMANDS[0]);
    }

} // namespace engine
