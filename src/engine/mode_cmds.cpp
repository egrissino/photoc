#include "algo/quantize.h"
#include "compose/compose.h"
#include "engine/layers.h"
#include "engine/params.h"
#include "engine/vector.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace engine
{

    /* Image > Mode's palette and ink modes: Indexed Color, Color Table, Bitmap and Duotone.
       Indexed, Bitmap and Duotone documents keep expanded pixels (RGB 8-bit, Gray 8-bit, Gray; see
       Document::pixel_format): Indexed and Bitmap reduce the flattened image in place and record
       the palette (Document::color_table) or the 1-bit look; Duotone keeps the gray pixels and the
       inks (Document::duotone), which the canvas and flat exports print through. As in Photoshop
       the conversions flatten (Indexed, Bitmap) and Bitmap and Duotone need a Grayscale image. */

    using geom::Rect;

    float mode_num (const Value& p, const char* key, float fallback)
    {
        double v;
        return param_f64 (p, key, &v) ? (float) v : fallback;
    }

    /* The flattened image as straight RGBA (over white when `opaque`). */
    std::vector<float> mode_composite (Session& s, const doc::Document& d, bool opaque)
    {
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = s.cmyk;
        settings.inks                    = true;
        compose::Buffer b                = compose::flatten (d, settings);
        if (opaque)
        {
            float white[3] = {1.0f, 1.0f, 1.0f};
            b              = b.over_background (white);
        }
        return b.px;
    }

    /* The document's layers replaced by one 8-bit layer holding `px` in `mode`. */
    void single_layer (Session& s, doc::Document* d, doc::LayerId* active, color::ColorMode mode,
        const std::vector<float>& px, const char* name, bool background)
    {
        d->mode                = mode;
        d->depth               = color::SampleType::U8;
        color::PixelFormat fmt = d->pixel_format ();
        size_t n = fmt.channels (), count = px.size () / 4;
        std::vector<float> data (count * n);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) count; i++)
        {
            raster::from_rgba (fmt, &px[(size_t) i * 4], s.cmyk, &data[(size_t) i * n]);
        }
        doc::Layer l = doc::Layer::raster (s.ids, name, fmt);
        if (background)
        {
            l.locks.transparency = true;
            l.locks.position     = true;
        }
        l.pixels.write_region (d->bounds (), data.data (), data.size ());
        l.pixels.prune ();
        *active = l.id;
        d->layers.clear ();
        d->layers.push_back (l);
        color::PixelFormat gray = {color::ColorMode::Grayscale, color::SampleType::U8, false};
        for (size_t i = 0; i < d->channels.size (); i++)
        {
            if (d->channels[i].surface.format () != gray)
            {
                d->channels[i].surface = d->channels[i].surface.convert (gray, NULL);
            }
        }
        d->has_color_table = false;
        d->color_table     = doc::ColorTable ();
        d->has_duotone     = false;
        d->duotone         = doc::Duotone ();
    }

    bool mode_is (const Session& s, const color::ColorMode* modes, size_t n, const char* what, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        color::ColorMode m = s.active ()->doc->mode;
        for (size_t i = 0; i < n; i++)
        {
            if (modes[i] == m)
            {
                return true;
            }
        }
        *why = what;
        return false;
    }

    /* Every pixel surface in `layers` converted to `mode` and `depth`, keeping its alpha. */
    void convert_layers (
        std::vector<doc::Layer>* layers, color::ColorMode mode, color::SampleType depth, const color::CmykSpace* cmyk)
    {
        for (size_t i = 0; i < layers->size (); i++)
        {
            doc::Layer& l      = (*layers)[i];
            raster::Surface* s = l.surface_mut ();
            if (s != NULL)
            {
                color::PixelFormat to = {mode, depth, s->format ().alpha};
                if (to != s->format ())
                {
                    *s = s->convert (to, cmyk);
                }
            }
            convert_layers (&l.children, mode, depth, cmyk);
        }
    }

    /* Every gray surface in `layers` as RGB showing how the Duotone inks print it. */
    void bake_duotone (std::vector<doc::Layer>* layers, const doc::Duotone& duo, const color::CmykSpace* cmyk)
    {
        for (size_t i = 0; i < layers->size (); i++)
        {
            doc::Layer& l      = (*layers)[i];
            raster::Surface* s = l.surface_mut ();
            if (s != NULL)
            {
                color::PixelFormat from = s->format (), to = {color::ColorMode::Rgb, from.sample, from.alpha};
                Rect r = s->content_bounds ();
                std::vector<float> px;
                raster::Surface out (to);
                if (!r.is_empty () && s->read_region (r, &px))
                {
                    size_t n = from.channels (), count = px.size () / n, m = to.channels ();
                    std::vector<float> data (count * m);
                    for (size_t k = 0; k < count; k++)
                    {
                        float rgb[3];
                        duo.render (px[k * n], rgb);
                        memcpy (&data[k * m], rgb, sizeof rgb);
                        if (to.alpha)
                        {
                            data[k * m + 3] = px[k * n + n - 1];
                        }
                    }
                    out.write_region (r, data.data (), data.size ());
                    out.prune ();
                }
                *s = out;
            }
            bake_duotone (&l.children, duo, cmyk);
        }
    }

    /* Image > Mode: the document's colour mode or bit depth changed, every layer converted (through
       the session's CMYK profile for CMYK). Adjustment layers keep their settings. */
    bool convert_mode (Session& s, color::ColorMode mode, color::SampleType depth, const char* label, Error* err)
    {
        const doc::Document& cur = *s.active ()->doc;
        if (cur.mode == mode && cur.depth == depth)
        {
            return true;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        /* Duotone leaves for a colour mode as printed: each gray through its inks first. */
        bool to_color = mode != color::ColorMode::Grayscale && mode != color::ColorMode::Duotone
                     && mode != color::ColorMode::Bitmap;
        if (d->mode == color::ColorMode::Duotone && d->has_duotone && to_color)
        {
            bake_duotone (&d->layers, d->duotone, s.cmyk);
        }
        convert_layers (&d->layers, mode, depth, s.cmyk);
        if (d->mode != mode)
        {
            d->has_color_table = false;
            d->color_table     = doc::ColorTable ();
            d->has_duotone     = false;
            d->duotone         = doc::Duotone ();
        }
        d->mode  = mode;
        d->depth = depth;
        render_all_live (s, d);
        return e.commit (label, err);
    }

    bool cmd_mode_rgb (Session& s, const Value& p, Value* result, Error* err)
    {
        return convert_mode (s, color::ColorMode::Rgb, s.active ()->doc->depth, "RGB Color", err);
    }
    bool cmd_mode_gray (Session& s, const Value& p, Value* result, Error* err)
    {
        return convert_mode (s, color::ColorMode::Grayscale, s.active ()->doc->depth, "Grayscale", err);
    }
    bool cmd_mode_cmyk (Session& s, const Value& p, Value* result, Error* err)
    {
        return convert_mode (s, color::ColorMode::Cmyk, s.active ()->doc->depth, "CMYK Color", err);
    }
    bool cmd_mode_lab (Session& s, const Value& p, Value* result, Error* err)
    {
        return convert_mode (s, color::ColorMode::Lab, s.active ()->doc->depth, "Lab Color", err);
    }
    bool cmd_mode_8 (Session& s, const Value& p, Value* result, Error* err)
    {
        return convert_mode (s, s.active ()->doc->mode, color::SampleType::U8, "8 Bits/Channel", err);
    }
    bool cmd_mode_16 (Session& s, const Value& p, Value* result, Error* err)
    {
        return convert_mode (s, s.active ()->doc->mode, color::SampleType::U16, "16 Bits/Channel", err);
    }
    bool cmd_mode_32 (Session& s, const Value& p, Value* result, Error* err)
    {
        return convert_mode (s, s.active ()->doc->mode, color::SampleType::F32, "32 Bits/Channel", err);
    }

    /* ---- Indexed Color ---- */

    bool indexed_on (Session& s, doc::Document* d, doc::LayerId* active, const Value& p, Value* result, Error* err)
    {
        const char* cmd     = "image.mode.indexedColor";
        std::string palette = "selective", forced_s = "blackWhite", dither_s = "diffusion";
        param_str (p, "palette", &palette);
        param_str (p, "forced", &forced_s);
        param_str (p, "dither", &dither_s);
        algo::PaletteKind kind;
        if (!algo::palette_kind_from_id (palette.c_str (), &kind))
        {
            return bad_params (
                err, cmd, "palette: exact, systemMac, systemWindows, web, uniform, perceptual, selective or adaptive");
        }
        const char* const FORCED[4] = {"none", "blackWhite", "primaries", "web"};
        const char* const DITHER[4] = {"none", "diffusion", "pattern", "noise"};
        int fi = -1, di = -1;
        for (int i = 0; i < 4; i++)
        {
            if (forced_s == FORCED[i] || (i == 1 && forced_s == "blackAndWhite"))
            {
                fi = i;
            }
            if (dither_s == DITHER[i])
            {
                di = i;
            }
        }
        if (fi < 0)
        {
            return bad_params (err, cmd, "forced: none, blackWhite, primaries or web");
        }
        if (di < 0)
        {
            return bad_params (err, cmd, "dither: none, diffusion, pattern or noise");
        }
        double colors_f = 256.0;
        if (param_present (p, "colors") && (!param_f64 (p, "colors", &colors_f) || colors_f < 2.0 || colors_f > 256.0))
        {
            return bad_params (err, cmd, "colors must be from 2 to 256");
        }
        int64_t colors    = (int64_t) lround (colors_f);
        float amount      = fminf (fmaxf (mode_num (p, "amount", 75.0f), 0.0f), 100.0f) / 100.0f;
        bool transparency = true;
        param_bool (p, "transparency", &transparency);
        std::vector<float> px = mode_composite (s, *d, !transparency);
        bool has_clear        = false;
        for (size_t i = 3; transparency && i < px.size () && !has_clear; i += 4)
        {
            has_clear = px[i] < 0.5f;
        }
        std::vector<uint8_t> pal;
        std::string why;
        size_t want = has_clear ? std::max ((size_t) colors - 1, (size_t) 2) : (size_t) colors;
        if (!algo::build_palette (px, kind, want, (algo::Forced) fi, &pal, &why))
        {
            return bad_params (err, cmd, "%s", why.c_str ());
        }
        int transparent = -1;
        if (has_clear && pal.size () / 3 < 256)
        {
            pal.push_back (255);
            pal.push_back (255);
            pal.push_back (255);
            transparent = (int) (pal.size () / 3) - 1;
        }
        algo::quantize (&px, d->size.width, pal, (algo::Dither) di, amount, transparent);
        single_layer (
            s, d, active, color::ColorMode::Indexed, px, transparent >= 0 ? "Index" : "Background", transparent < 0);
        d->has_color_table         = true;
        d->color_table.rgb         = pal;
        d->color_table.transparent = transparent;
        *result =
            Value{{"colors", pal.size () / 3}, {"transparentIndex", transparent >= 0 ? Value (transparent) : Value ()}};
        return true;
    }

    bool cmd_indexed_color (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        if (e.doc () == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!indexed_on (s, e.doc (), e.active_layer (), p, result, err))
        {
            return false;
        }
        return e.commit ("Indexed Color", err);
    }

    bool can_indexed (const Session& s, std::string* why)
    {
        const color::ColorMode M[6] = {color::ColorMode::Rgb, color::ColorMode::Grayscale, color::ColorMode::Indexed,
            color::ColorMode::Duotone, color::ColorMode::Cmyk, color::ColorMode::Lab};
        return mode_is (s, M, 6, "Indexed Color needs an RGB or Grayscale image", why);
    }

    /* ---- Color Table ---- */

    std::string hex_rgb (const uint8_t* c)
    {
        char buf[8];
        snprintf (buf, sizeof buf, "#%02x%02x%02x", c[0], c[1], c[2]);
        return buf;
    }

    bool color_bytes (const Value& v, uint8_t out[3])
    {
        if (v.is_string ())
        {
            float c[4];
            if (!parse_hex (v.get<std::string> ().c_str (), c))
            {
                return false;
            }
            for (int k = 0; k < 3; k++)
            {
                out[k] = (uint8_t) lroundf (c[k] * 255.0f);
            }
            return true;
        }
        if (v.is_array () && v.size () >= 3)
        {
            for (size_t k = 0; k < 3; k++)
            {
                if (!v[k].is_number ())
                {
                    return false;
                }
                out[k] = (uint8_t) fminf (fmaxf (roundf (v[k].get<float> ()), 0.0f), 255.0f);
            }
            return true;
        }
        return false;
    }

    /* The built-in tables: 256-step ramps, and the system and web palettes. */
    bool color_table_preset (const std::string& name, std::vector<uint8_t>* out)
    {
        out->clear ();
        if (name == "systemMac" || name == "system")
        {
            *out = algo::mac_palette ();
        }
        else if (name == "systemWindows")
        {
            *out = algo::windows_palette ();
        }
        else if (name == "web")
        {
            *out = algo::web_palette ();
        }
        else if (name != "grayscale" && name != "blackBody" && name != "spectrum")
        {
            return false;
        }
        if (!out->empty ())
        {
            return true;
        }
        for (int i = 0; i < 256; i++)
        {
            float t = (float) i / 255.0f, c[3];
            if (name == "grayscale")
            {
                c[0] = c[1] = c[2] = t;
            }
            else if (name == "blackBody")
            {
                /* Black, red, yellow, white: the incandescence scale. */
                c[0] = fminf (t * 3.0f, 1.0f);
                c[1] = fminf (fmaxf (t * 3.0f - 1.0f, 0.0f), 1.0f);
                c[2] = fminf (fmaxf (t * 3.0f - 2.0f, 0.0f), 1.0f);
            }
            else
            {
                /* Hues from red round to magenta. */
                float h = t * 300.0f / 60.0f, x = 1.0f - fabsf (fmodf (h, 2.0f) - 1.0f);
                const float SEG[5][3] = {{1, -1, 0}, {-1, 1, 0}, {0, 1, -1}, {0, -1, 1}, {-1, 0, 1}};
                int seg               = (int) h > 4 ? 4 : (int) h;
                for (int k = 0; k < 3; k++)
                {
                    c[k] = SEG[seg][k] < 0.0f ? x : SEG[seg][k];
                }
            }
            for (int k = 0; k < 3; k++)
            {
                out->push_back ((uint8_t) lroundf (fminf (fmaxf (c[k], 0.0f), 1.0f) * 255.0f));
            }
        }
        return true;
    }

    Value table_report (const doc::ColorTable& t)
    {
        Value colors = Value::array ();
        for (size_t i = 0; i < t.count (); i++)
        {
            colors.push_back (hex_rgb (&t.rgb[i * 3]));
        }
        return Value{{"colors", colors}, {"transparent", t.transparent >= 0 ? Value (t.transparent) : Value ()}};
    }

    /* Image > Mode > Color Table: entries replaced (pixels keep their index, so their colour follows
       the table, as in Photoshop) and the transparent entry set. Without changes, the table. */
    bool cmd_color_table (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd    = "image.mode.colorTable";
        const DocState* ds = s.active ();
        if (ds == NULL || !ds->doc->has_color_table)
        {
            return fail (err, ErrorKind::Other, "the document has no color table");
        }
        doc::ColorTable cur = ds->doc->color_table, table = cur;
        std::string name = "custom";
        param_str (p, "table", &name);
        if (name != "custom" && !color_table_preset (name, &table.rgb))
        {
            return bad_params (err, cmd, "unknown table `%s`", name.c_str ());
        }
        const Value* colors = param (p, "colors");
        if (colors != NULL && colors->is_array ())
        {
            if (colors->empty () || colors->size () > 256)
            {
                return bad_params (err, cmd, "a color table has 1 to 256 entries");
            }
            table.rgb.clear ();
            for (size_t i = 0; i < colors->size (); i++)
            {
                uint8_t c[3];
                if (!color_bytes ((*colors)[i], c))
                {
                    return bad_params (err, cmd, "colors: [\"#rrggbb\" | [r, g, b], …]");
                }
                table.rgb.insert (table.rgb.end (), c, c + 3);
            }
        }
        const Value* entries = param (p, "entries");
        if (entries != NULL && entries->is_object ())
        {
            for (Value::const_iterator it = entries->begin (); it != entries->end (); ++it)
            {
                char* end = NULL;
                long i    = strtol (it.key ().c_str (), &end, 10);
                if (end == it.key ().c_str () || *end != 0 || i < 0)
                {
                    return bad_params (err, cmd, "entry index `%s`", it.key ().c_str ());
                }
                uint8_t c[3];
                if (!color_bytes (it.value (), c))
                {
                    return bad_params (err, cmd, "entry %ld: bad colour", i);
                }
                if ((size_t) i >= table.count ())
                {
                    return bad_params (err, cmd, "entry %ld is past the table (%zu entries)", i, table.count ());
                }
                memcpy (&table.rgb[(size_t) i * 3], c, 3);
            }
        }
        const Value* tr = param (p, "transparent");
        if (tr != NULL)
        {
            if (tr->is_null ())
            {
                table.transparent = -1;
            }
            else if (tr->is_number_integer () && tr->get<int64_t> () >= 0
                     && (size_t) tr->get<int64_t> () < table.count ())
            {
                table.transparent = (int) tr->get<int64_t> ();
            }
        }
        if (table.rgb == cur.rgb && table.transparent == cur.transparent)
        {
            *result = table_report (cur);
            return true;
        }
        Edit e (s);
        doc::Document* d                = e.doc ();
        std::vector<doc::WalkEntry> all = d->walk ();
        for (size_t li = 0; li < all.size (); li++)
        {
            doc::Layer* l         = d->layer_mut (all[li].layer->id);
            raster::Surface* surf = l != NULL ? l->surface_mut () : NULL;
            if (surf == NULL)
            {
                continue;
            }
            Rect r = surf->content_bounds ();
            if (r.is_empty ())
            {
                continue;
            }
            color::PixelFormat fmt = surf->format ();
            size_t n               = fmt.channels ();
            std::vector<float> data;
            if (!surf->read_region (r, &data))
            {
                continue;
            }
            size_t count = data.size () / n;
#pragma omp parallel for schedule(static)
            for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
            {
                float *q = &data[(size_t) ii * n], c[4];
                raster::to_rgba (fmt, q, s.cmyk, c);
                size_t i;
                if (c[3] <= 0.0f)
                {
                    /* A clear pixel is the old transparent entry only if its colour says so. */
                    if (cur.transparent < 0)
                    {
                        continue;
                    }
                    float tc[3];
                    cur.color ((size_t) cur.transparent, tc);
                    if (fabsf (tc[0] - c[0]) > 1e-3f || fabsf (tc[1] - c[1]) > 1e-3f || fabsf (tc[2] - c[2]) > 1e-3f)
                    {
                        continue;
                    }
                    i = (size_t) cur.transparent;
                }
                else
                {
                    i = cur.nearest (c);
                }
                float alpha = table.transparent == (int) i ? 0.0f : c[3] <= 0.0f ? 1.0f : c[3], nc[4];
                if (i < table.count ())
                {
                    table.color (i, nc);
                }
                else
                {
                    table.color (table.nearest (c), nc);
                }
                nc[3] = alpha;
                raster::from_rgba (fmt, nc, s.cmyk, q);
            }
            surf->write_region (r, data.data (), data.size ());
            surf->prune ();
        }
        d->color_table = table;
        if (!e.commit ("Color Table", err))
        {
            return false;
        }
        *result = table_report (table);
        return true;
    }

    bool can_color_table (const Session& s, std::string* why)
    {
        const color::ColorMode M[1] = {color::ColorMode::Indexed};
        return mode_is (s, M, 1, "Color Table needs an Indexed Color image", why);
    }

    /* ---- Bitmap ---- */

    bool bitmap_on (Session& s, doc::Document* d, doc::LayerId* active, const Value& p, Value* result, Error* err)
    {
        const char* cmd    = "image.mode.bitmap";
        float dpi          = fmaxf (d->resolution_dpi, 1.0f);
        std::string method = "diffusion", shape_s = "round";
        param_str (p, "method", &method);
        param_str (p, "shape", &shape_s);
        algo::BitmapMethod m;
        algo::HalftoneShape shape = algo::HalftoneShape::Round;
        float cell = 2.0f, angle = mode_num (p, "angle", 45.0f);
        if (method == "threshold" || method == "threshold50")
        {
            m = algo::BitmapMethod::Threshold;
        }
        else if (method == "pattern")
        {
            m = algo::BitmapMethod::Pattern;
        }
        else if (method == "diffusion")
        {
            m = algo::BitmapMethod::Diffusion;
        }
        else if (method == "halftone")
        {
            m          = algo::BitmapMethod::Halftone;
            float freq = fminf (fmaxf (mode_num (p, "frequency", 53.0f), 1.0f), 999.0f);
            if (!algo::halftone_shape_from_id (shape_s.c_str (), &shape))
            {
                return bad_params (err, cmd, "shape: round, ellipse, line, square, diamond or cross");
            }
            cell = fmaxf (dpi / freq, 2.0f);
        }
        else
        {
            return bad_params (err, cmd, "method `%s` (threshold, pattern, diffusion or halftone)", method.c_str ());
        }
        std::vector<float> px = mode_composite (s, *d, true);
        size_t count          = px.size () / 4;
        std::vector<float> gray (count);
        for (size_t i = 0; i < count; i++)
        {
            gray[i] = 0.299f * px[i * 4] + 0.587f * px[i * 4 + 1] + 0.114f * px[i * 4 + 2];
        }
        algo::to_bitmap (&gray, d->size.width, m, cell, angle, shape);
        for (size_t i = 0; i < count; i++)
        {
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = gray[i];
            px[i * 4 + 3]                             = 1.0f;
        }
        single_layer (s, d, active, color::ColorMode::Bitmap, px, "Background", true);
        return true;
    }

    bool cmd_bitmap (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        if (e.doc () == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!bitmap_on (s, e.doc (), e.active_layer (), p, result, err))
        {
            return false;
        }
        return e.commit ("Bitmap", err);
    }

    bool can_bitmap (const Session& s, std::string* why)
    {
        const color::ColorMode M[1] = {color::ColorMode::Grayscale};
        return mode_is (s, M, 1, "Bitmap needs a Grayscale image (Image > Mode > Grayscale first)", why);
    }

    /* ---- Duotone ---- */

    /* The default inks: black first, then a warm brown, gold and slate blue (Photoshop ships Pantone
       presets, which aren't reproduced). */
    std::vector<doc::DuotoneInk> default_inks (size_t n)
    {
        const char* const NAMES[4] = {"Black", "Warm Brown", "Gold", "Slate Blue"};
        const float COLORS[4][3]   = {
            {0.0f, 0.0f, 0.0f}, {0.62f, 0.38f, 0.18f}, {0.86f, 0.68f, 0.18f}, {0.27f, 0.38f, 0.58f}};
        std::vector<doc::DuotoneInk> v;
        for (size_t i = 0; i < n && i < 4; i++)
        {
            v.push_back (doc::DuotoneInk::make (NAMES[i], COLORS[i][0], COLORS[i][1], COLORS[i][2]));
        }
        return v;
    }

    bool duotone_on (Session& s, doc::Document* d, doc::LayerId* active, const Value& p, Value* result, Error* err)
    {
        const char* cmd  = "image.mode.duotone";
        std::string type = "duotone";
        param_str (p, "type", &type);
        const char* const TYPES[4] = {"monotone", "duotone", "tritone", "quadtone"};
        size_t n                   = 0;
        for (size_t i = 0; i < 4; i++)
        {
            if (type == TYPES[i])
            {
                n = i + 1;
            }
        }
        if (n == 0)
        {
            return bad_params (err, cmd, "type `%s` (monotone, duotone, tritone or quadtone)", type.c_str ());
        }
        std::vector<doc::DuotoneInk> inks = default_inks (n);
        const Value* list                 = param (p, "inks");
        if (list != NULL)
        {
            if (!list->is_array ())
            {
                return bad_params (
                    err, cmd, "inks: [{\"name\",\"color\":\"#rrggbb\",\"curve\":[[in,out],…] (0..100)}, …]");
            }
            for (size_t i = 0; i < list->size () && i < n; i++)
            {
                const Value& ink = (*list)[i];
                if (!ink.is_object ())
                {
                    continue;
                }
                std::string name;
                if (param_str (ink, "name", &name))
                {
                    inks[i].name = name;
                }
                const Value* c = param (ink, "color");
                uint8_t b[3];
                if (c != NULL && color_bytes (*c, b))
                {
                    for (int k = 0; k < 3; k++)
                    {
                        inks[i].color[k] = (float) b[k] / 255.0f;
                    }
                }
                const Value* curve = param (ink, "curve");
                if (curve != NULL && curve->is_array () && curve->size () >= 2)
                {
                    std::vector<doc::CurvePoint> pts;
                    for (size_t k = 0; k < curve->size (); k++)
                    {
                        const Value& q = (*curve)[k];
                        if (!q.is_array () || q.size () < 2 || !q[0].is_number () || !q[1].is_number ())
                        {
                            continue;
                        }
                        doc::CurvePoint cp = {q[0].get<float> () / 100.0f, q[1].get<float> () / 100.0f};
                        pts.push_back (cp);
                    }
                    /* Sorted by input. */
                    for (size_t a = 1; a < pts.size (); a++)
                    {
                        for (size_t b2 = a; b2 > 0 && pts[b2].input < pts[b2 - 1].input; b2--)
                        {
                            std::swap (pts[b2], pts[b2 - 1]);
                        }
                    }
                    if (pts.size () >= 2)
                    {
                        inks[i].curve = pts;
                    }
                }
            }
        }
        /* The dialog's flat form: "ink1" … "ink4" colours. */
        for (size_t i = 0; i < n; i++)
        {
            char key[24];
            snprintf (key, sizeof key, "ink%zu", i + 1);
            const Value* c = param (p, key);
            uint8_t b[3];
            if (c != NULL && color_bytes (*c, b))
            {
                for (int k = 0; k < 3; k++)
                {
                    inks[i].color[k] = (float) b[k] / 255.0f;
                }
            }
        }
        d->mode         = color::ColorMode::Duotone;
        d->has_duotone  = true;
        d->duotone.inks = inks;
        d->duotone.psd_raw.clear ();
        d->has_color_table = false;
        Value names        = Value::array ();
        for (size_t i = 0; i < inks.size (); i++)
        {
            names.push_back (inks[i].name);
        }
        *result = Value{{"inks", names}};
        return true;
    }

    bool cmd_duotone (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        if (e.doc () == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!duotone_on (s, e.doc (), e.active_layer (), p, result, err))
        {
            return false;
        }
        return e.commit ("Duotone", err);
    }

    /* Indexed Color, Bitmap or Duotone run on `d` in place (the dialogs' preview runs it on a
       copy). */
    bool apply_mode_command (
        Session& s, doc::Document* d, doc::LayerId* active, const char* cmd, const Value& p, Error* err)
    {
        Value ignored;
        if (strcmp (cmd, "image.mode.indexedColor") == 0)
        {
            return indexed_on (s, d, active, p, &ignored, err);
        }
        if (strcmp (cmd, "image.mode.bitmap") == 0)
        {
            return bitmap_on (s, d, active, p, &ignored, err);
        }
        if (strcmp (cmd, "image.mode.duotone") == 0)
        {
            return duotone_on (s, d, active, p, &ignored, err);
        }
        return fail (err, ErrorKind::Other, "no preview for %s", cmd);
    }

    bool can_duotone (const Session& s, std::string* why)
    {
        const color::ColorMode M[2] = {color::ColorMode::Grayscale, color::ColorMode::Duotone};
        return mode_is (s, M, 2, "Duotone needs a Grayscale image (Image > Mode > Grayscale first)", why);
    }

    /* In Photoshop's order: Bitmap, Grayscale, Duotone, Indexed Color, RGB, CMYK, Lab, the depths,
       then Color Table. */
    const CommandSpec MODE_COMMANDS[] = {
        {"image.mode.bitmap", "Bitmap…", "Image/Mode", NULL,
            "{\"method\":\"diffusion|threshold|pattern|halftone\"=\"diffusion\",\"frequency\":1..999=53,\"angle\":-180."
            ".180=45,\"shape\":\"round|ellipse|line|square|diamond|cross\"=\"round\"}"
            " (halftone frequency in lines per inch; flattens)",
            can_bitmap, cmd_bitmap, true},
        {"image.mode.grayscale", "Grayscale", "Image/Mode", NULL, "{}", has_doc, cmd_mode_gray, true},
        {"image.mode.duotone", "Duotone…", "Image/Mode", NULL,
            "{\"type\":\"duotone|monotone|tritone|quadtone\"=\"duotone\",\"inks\":[{\"name\",\"color\":\"#rrggbb\","
            "\"curve\":[[in,out],…] in 0..100}, …]?}",
            can_duotone, cmd_duotone, true},
        {"image.mode.indexedColor", "Indexed Color…", "Image/Mode", NULL,
            "{\"palette\":\"selective|perceptual|adaptive|exact|systemMac|systemWindows|web|uniform\"=\"selective\","
            "\"colors\":2..256=256,"
            "\"forced\":\"blackWhite|none|primaries|web\"=\"blackWhite\",\"transparency\":bool=true,\"dither\":"
            "\"diffusion|none|pattern|noise\"=\"diffusion\",\"amount\":0..100=75} (flattens)",
            can_indexed, cmd_indexed_color, true},
        {"image.mode.rgbColor", "RGB Color", "Image/Mode", NULL, "{}", has_doc, cmd_mode_rgb, true},
        {"image.mode.cmykColor", "CMYK Color", "Image/Mode", NULL, "{}", has_doc, cmd_mode_cmyk, true},
        {"image.mode.labColor", "Lab Color", "Image/Mode", NULL, "{}", has_doc, cmd_mode_lab, true},
        {"image.mode.8bits", "8 Bits/Channel", "Image/Mode", NULL, "{}", has_doc, cmd_mode_8, true},
        {"image.mode.16bits", "16 Bits/Channel", "Image/Mode", NULL, "{}", has_doc, cmd_mode_16, true},
        {"image.mode.32bits", "32 Bits/Channel", "Image/Mode", NULL, "{}", has_doc, cmd_mode_32, true},
        {"image.mode.colorTable", "Color Table…", "Image/Mode", NULL,
            "{\"table\":\"custom|blackBody|grayscale|spectrum|systemMac|systemWindows|web\"=\"custom\",\"colors\":[\"#"
            "rrggbb\",…]?,\"entries\":{\"index\":\"#rrggbb\"}?,"
            "\"transparent\":index|null?}",
            can_color_table, cmd_color_table, true},
    };

    void register_mode_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), MODE_COMMANDS, MODE_COMMANDS + sizeof MODE_COMMANDS / sizeof MODE_COMMANDS[0]);
    }

} // namespace engine
