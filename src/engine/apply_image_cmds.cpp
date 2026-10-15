#include "color/blend.h"
#include "compose/compose.h"
#include "engine/apply_image.h"
#include "engine/layers.h"
#include "engine/params.h"
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <string>
#include <vector>

namespace engine
{

    /* Image > Apply Image and Image > Calculations: a channel of an open document of the same size
       (a layer's or the merged image's composite or colour channel, an alpha channel, the
       selection, a layer's transparency or mask) blended onto the active layer, or two of them
       blended into a new channel, document or selection. */

    using geom::Rect;

    /* The colour channels' names in `mode`, and the composite's. */
    size_t color_channel_names (color::ColorMode mode, const char* names[4], const char** composite)
    {
        if (mode == color::ColorMode::Cmyk)
        {
            names[0]   = "Cyan";
            names[1]   = "Magenta";
            names[2]   = "Yellow";
            names[3]   = "Black";
            *composite = "CMYK";
            return 4;
        }
        if (mode == color::ColorMode::Lab)
        {
            names[0]   = "Lightness";
            names[1]   = "a";
            names[2]   = "b";
            *composite = "Lab";
            return 3;
        }
        if (mode == color::ColorMode::Grayscale)
        {
            names[0]   = "Gray";
            *composite = "Gray";
            return 1;
        }
        names[0]   = "Red";
        names[1]   = "Green";
        names[2]   = "Blue";
        *composite = "RGB";
        return 3;
    }

    bool same_name (const std::string& a, const char* b)
    {
        if (a.size () != strlen (b))
        {
            return false;
        }
        for (size_t i = 0; i < a.size (); i++)
        {
            if (tolower ((unsigned char) a[i]) != tolower ((unsigned char) b[i]))
            {
                return false;
            }
        }
        return true;
    }

    /* A channel named in params: "composite", a colour channel's name, an alpha channel (index or
       name), "selection", "transparency" or "mask". */
    bool parse_chan_ref (const Value& v, const doc::Document& d, ChanRef* out)
    {
        if (v.is_number_integer ())
        {
            out->kind  = ChanKind::Alpha;
            out->index = (size_t) v.get<int64_t> ();
            return true;
        }
        if (!v.is_string ())
        {
            return false;
        }
        std::string s = v.get<std::string> ();
        const char *names[4], *composite;
        size_t n   = color_channel_names (d.mode, names, &composite);
        out->index = 0;
        if (same_name (s, "composite") || same_name (s, "merged") || same_name (s, composite))
        {
            out->kind = ChanKind::Composite;
            return true;
        }
        if (same_name (s, "selection"))
        {
            out->kind = ChanKind::Selection;
            return true;
        }
        if (same_name (s, "transparency"))
        {
            out->kind = ChanKind::Transparency;
            return true;
        }
        if (same_name (s, "mask") || same_name (s, "layerMask"))
        {
            out->kind = ChanKind::LayerMask;
            return true;
        }
        for (size_t i = 0; i < n; i++)
        {
            if (same_name (s, names[i]))
            {
                out->kind  = n == 1 ? ChanKind::Composite : ChanKind::Color;
                out->index = i;
                return true;
            }
        }
        for (size_t i = 0; i < d.channels.size (); i++)
        {
            if (d.channels[i].name == s)
            {
                out->kind  = ChanKind::Alpha;
                out->index = i;
                return true;
            }
        }
        return false;
    }

    /* One full-canvas plane of a grey surface. */
    std::vector<float> surface_plane (const raster::Surface& s, Rect area)
    {
        std::vector<float> data;
        std::vector<float> out ((size_t) area.width () * (size_t) area.height (), 0.0f);
        if (!s.read_region (area, &data))
        {
            return out;
        }
        size_t n = s.channels ();
        for (size_t i = 0; i < out.size (); i++)
        {
            out[i] = data[i * n];
        }
        return out;
    }

    /* A layer's (or, with layer 0, the merged image's) pixels in the document's own channels. */
    bool native_pixels (Session& s, const doc::Document& d, doc::LayerId layer, std::vector<float>* out, Error* err)
    {
        Rect area              = d.bounds ();
        color::PixelFormat fmt = d.pixel_format ();
        if (layer == 0)
        {
            compose::RenderSettings settings = compose::default_settings ();
            settings.cmyk                    = s.cmyk;
            raster::Surface flat             = compose::composite_to_surface (d, area, fmt, NULL, settings);
            if (!flat.read_region (area, out))
            {
                return fail (err, ErrorKind::Other, "the image is too large");
            }
            return true;
        }
        const doc::Layer* l = d.layer (layer);
        if (l == NULL)
        {
            return no_layer (err, layer);
        }
        const raster::Surface* surf = l->surface ();
        if (surf == NULL)
        {
            return fail (err, ErrorKind::Other, "layer \"%s\" has no pixels", l->name.c_str ());
        }
        if (surf->format () == fmt)
        {
            if (!surf->read_region (area, out))
            {
                return fail (err, ErrorKind::Other, "the image is too large");
            }
            return true;
        }
        raster::Surface conv = surf->convert (fmt, s.cmyk);
        if (!conv.read_region (area, out))
        {
            return fail (err, ErrorKind::Other, "the image is too large");
        }
        return true;
    }

    /* The planes of channel `r` of `d` (layer 0: merged; `active` for transparency and masks): one
       per colour channel for a composite, else one. */
    bool ref_planes (Session& s, const doc::Document& d, doc::LayerId layer, doc::LayerId active, const ChanRef& r,
        std::vector<std::vector<float>>* out, Error* err)
    {
        Rect area    = d.bounds ();
        size_t count = (size_t) area.width () * (size_t) area.height ();
        out->clear ();
        if (r.kind == ChanKind::Composite || r.kind == ChanKind::Color)
        {
            std::vector<float> px;
            if (!native_pixels (s, d, layer, &px, err))
            {
                return false;
            }
            size_t n = d.pixel_format ().channels (), colors = color::color_channels (d.mode);
            for (size_t c = 0; c < colors; c++)
            {
                if (r.kind == ChanKind::Color && c != r.index)
                {
                    continue;
                }
                std::vector<float> pl (count);
                for (size_t i = 0; i < count; i++)
                {
                    pl[i] = px[i * n + c];
                }
                out->push_back (pl);
            }
            if (out->empty ())
            {
                return fail (err, ErrorKind::Other, "no colour channel %zu", r.index);
            }
            return true;
        }
        if (r.kind == ChanKind::Alpha)
        {
            if (r.index >= d.channels.size ())
            {
                return fail (err, ErrorKind::Other, "no alpha channel %zu", r.index);
            }
            out->push_back (surface_plane (d.channels[r.index].surface, area));
            return true;
        }
        if (r.kind == ChanKind::Selection)
        {
            out->push_back (d.has_selection ? surface_plane (d.selection, area) : std::vector<float> (count, 0.0f));
            return true;
        }
        doc::LayerId id     = layer != 0 ? layer : active;
        const doc::Layer* l = d.layer (id);
        if (l == NULL)
        {
            return fail (err, ErrorKind::Other,
                r.kind == ChanKind::Transparency ? "transparency needs a layer" : "a layer mask needs a layer");
        }
        if (r.kind == ChanKind::LayerMask)
        {
            if (!l->has_mask)
            {
                return fail (err, ErrorKind::Other, "layer \"%s\" has no mask", l->name.c_str ());
            }
            out->push_back (surface_plane (l->mask.surface, area));
            return true;
        }
        const raster::Surface* surf = l->surface ();
        std::vector<float> pl (count, 1.0f);
        std::vector<float> px;
        if (surf != NULL && surf->format ().alpha && surf->read_region (area, &px))
        {
            size_t n = surf->channels ();
            for (size_t i = 0; i < count; i++)
            {
                pl[i] = px[i * n + n - 1];
            }
        }
        out->push_back (pl);
        return true;
    }

    /* A composite reduced to one plane: its luminosity, as Calculations does. */
    std::vector<float> planes_luminosity (
        Session& s, const doc::Document& d, const std::vector<std::vector<float>>& planes)
    {
        if (planes.size () == 1)
        {
            return planes[0];
        }
        color::PixelFormat fmt = d.pixel_format ();
        size_t count = planes[0].size (), n = fmt.channels ();
        std::vector<float> out (count);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
        {
            size_t i = (size_t) ii;
            float px[raster::MAX_CHANNELS], rgba[4];
            for (size_t c = 0; c < planes.size (); c++)
            {
                px[c] = planes[c][i];
            }
            for (size_t c = planes.size (); c < n; c++)
            {
                px[c] = 1.0f;
            }
            raster::to_rgba (fmt, px, s.cmyk, rgba);
            out[i] = 0.3f * rgba[0] + 0.59f * rgba[1] + 0.11f * rgba[2];
        }
        return out;
    }

    /* A source spec {document?, layer?, channel?, invert?}, or the same as flat <prefix>Document,
       <prefix>Layer, <prefix>Channel and <prefix>Invert keys. False when there's neither. */
    bool source_spec (const Value& p, const char* key, Value* out)
    {
        const Value* v = param (p, key);
        if (v != NULL && v->is_object ())
        {
            *out = *v;
            return true;
        }
        const char* const FIELDS[4][2] = {
            {"document", "Document"}, {"layer", "Layer"}, {"channel", "Channel"}, {"invert", "Invert"}};
        *out = Value::object ();
        for (int i = 0; i < 4; i++)
        {
            const Value* f = param (p, (std::string (key) + FIELDS[i][1]).c_str ());
            if (f == NULL || f->is_null ())
            {
                continue;
            }
            if (f->is_string () && (f->get<std::string> ().empty () || (i == 1 && f->get<std::string> () == "merged")))
            {
                continue;
            }
            (*out)[FIELDS[i][0]] = *f;
        }
        return !out->empty ();
    }

    /* A source's planes over the active document's canvas (sizes must match). */
    bool source_planes (
        Session& s, const Value& spec, const char* cmd, bool gray, std::vector<std::vector<float>>* out, Error* err)
    {
        const DocState* target = s.active ();
        if (target == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        const DocState* st = target;
        int64_t di;
        if (param_i64 (spec, "document", &di))
        {
            st = di >= 0 ? s.doc_at ((size_t) di) : NULL;
            if (st == NULL)
            {
                return bad_params (err, cmd, "no open document %lld", (long long) di);
            }
        }
        const doc::Document& d = *st->doc;
        if (d.size.width != target->doc->size.width || d.size.height != target->doc->size.height)
        {
            return bad_params (err, cmd, "source document \"%s\" is %ux%u; it must match the target's size",
                d.name.c_str (), (unsigned) d.size.width, (unsigned) d.size.height);
        }
        ChanRef r       = {ChanKind::Composite, 0};
        const Value* ch = param (spec, "channel");
        if (ch != NULL && !parse_chan_ref (*ch, d, &r))
        {
            return bad_params (err, cmd, "unknown channel %s", ch->dump ().c_str ());
        }
        int64_t layer = 0;
        param_i64 (spec, "layer", &layer);
        if (!ref_planes (s, d, (doc::LayerId) layer, st->active_layer, r, out, err))
        {
            return false;
        }
        if (gray && out->size () > 1)
        {
            std::vector<float> l = planes_luminosity (s, d, *out);
            out->clear ();
            out->push_back (l);
        }
        bool invert = false;
        param_bool (spec, "invert", &invert);
        for (size_t k = 0; invert && k < out->size (); k++)
        {
            for (size_t i = 0; i < (*out)[k].size (); i++)
            {
                (*out)[k][i] = 1.0f - (*out)[k][i];
            }
        }
        return true;
    }

    /* The optional mask: a grey plane, or empty. */
    bool mask_plane (Session& s, const Value& p, const char* cmd, std::vector<float>* out, Error* err)
    {
        out->clear ();
        Value spec;
        if (!source_spec (p, "mask", &spec))
        {
            return true;
        }
        const Value* ch = param (spec, "channel");
        if (ch == NULL || (ch->is_string () && ch->get<std::string> () == "none"))
        {
            return true;
        }
        std::vector<std::vector<float>> planes;
        if (!source_planes (s, spec, cmd, true, &planes, err))
        {
            return false;
        }
        *out = planes[0];
        return true;
    }

    /* Blending: a separable layer mode, or Add / Subtract with scale and offset. */
    bool blending_parse (const Value& p, const char* cmd, Blending* out, Error* err)
    {
        std::string name = "multiply";
        param_str (p, "blending", &name);
        double scale = 1.0, offset = 0.0;
        param_f64 (p, "scale", &scale);
        param_f64 (p, "offset", &offset);
        out->scale  = (float) fmin (fmax (scale, 1.0), 2.0);
        out->offset = (float) fmin (fmax (offset, -255.0), 255.0) / 255.0f;
        out->kind   = 0;
        if (same_name (name, "add"))
        {
            out->kind = 1;
            return true;
        }
        if (same_name (name, "subtract"))
        {
            out->kind = 2;
            return true;
        }
        if (!blend_from_str (name.c_str (), &out->mode) || !color::blend_is_separable (out->mode)
            || out->mode == color::BlendMode::Dissolve || out->mode == color::BlendMode::PassThrough)
        {
            return bad_params (err, cmd, "unsupported blending `%s`", name.c_str ());
        }
        return true;
    }

    float blending_apply (const Blending& b, float base, float top)
    {
        float v = b.kind == 1 ? (base + top) / b.scale + b.offset
                : b.kind == 2 ? (base - top) / b.scale + b.offset
                              : color::blend_channel (b.mode, base, top);
        return fminf (fmaxf (v, 0.0f), 1.0f);
    }

    /* Apply Image onto layer `id` of `d`; the sources come from the session's documents (the
       preview runs this on a copy). */
    bool apply_image_to (Session& s, doc::Document* d, doc::LayerId id, const Value& p, Error* err)
    {
        const char* cmd = "image.applyImage";
        Blending blending;
        if (!blending_parse (p, cmd, &blending, err))
        {
            return false;
        }
        double opacity_pct = 100.0;
        param_f64 (p, "opacity", &opacity_pct);
        float opacity = (float) fmin (fmax (opacity_pct, 0.0), 100.0) / 100.0f;
        bool preserve = false;
        param_bool (p, "preserveTransparency", &preserve);
        Value spec;
        if (!source_spec (p, "source", &spec))
        {
            spec = Value::object ();
        }
        std::vector<std::vector<float>> src;
        if (!source_planes (s, spec, cmd, false, &src, err))
        {
            return false;
        }
        std::vector<float> mask;
        if (!mask_plane (s, p, cmd, &mask, err))
        {
            return false;
        }
        doc::Layer* l = d->layer_mut (id);
        if (l == NULL)
        {
            return no_layer (err, id);
        }
        if (!check_pixels_unlocked (*d, id, err))
        {
            return false;
        }
        raster::Surface* surf = l->surface_mut ();
        if (surf == NULL)
        {
            return fail (err, ErrorKind::Other, "Apply Image needs a pixel layer");
        }
        color::PixelFormat sf = surf->format ();
        if (sf != d->pixel_format ())
        {
            return fail (err, ErrorKind::Other, "the target layer's format doesn't match the document");
        }
        Rect area = d->bounds ();
        std::vector<float> v;
        if (!surf->read_region (area, &v))
        {
            return fail (err, ErrorKind::Other, "the image is too large");
        }
        size_t n = sf.channels (), colors = color::color_channels (sf.mode),
               count                     = (size_t) area.width () * (size_t) area.height ();
        size_t w                         = (size_t) area.width ();
        bool sel                         = d->has_selection;
        const raster::Surface& selection = d->selection;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
        {
            size_t i     = (size_t) ii;
            float* px    = &v[i * n];
            float base_a = sf.alpha ? fminf (fmaxf (px[colors], 0.0f), 1.0f) : 1.0f;
            float wgt    = opacity * (mask.empty () ? 1.0f : mask[i]);
            if (sel)
            {
                wgt *= selection.sample_channel (area.x0 + (int32_t) (i % w), area.y0 + (int32_t) (i / w), 0);
            }
            if (preserve)
            {
                wgt *= base_a;
            }
            if (wgt <= 0.0f)
            {
                continue;
            }
            /* Painted over the layer like a brush: the blend acts where the layer has pixels, and
               alpha grows by source-over, so an empty layer takes the source as is. */
            float out_a = preserve ? base_a : wgt + base_a * (1.0f - wgt);
            for (size_t c = 0; c < colors; c++)
            {
                float top     = src[c < src.size () ? c : 0][i];
                float blended = top + (blending_apply (blending, px[c], top) - top) * base_a;
                px[c]         = preserve ? px[c] + (blended - px[c]) * wgt
                                         : (wgt * blended + base_a * (1.0f - wgt) * px[c]) / out_a;
            }
            if (sf.alpha)
            {
                px[colors] = out_a;
            }
        }
        surf->write_region (area, v.data (), v.size ());
        surf->prune ();
        return true;
    }

    bool cmd_apply_image (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!layer_param (s, Value (), &id, err))
        {
            return false;
        }
        Edit e (s);
        if (e.doc () == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!apply_image_to (s, e.doc (), id, p, err))
        {
            return false;
        }
        return e.commit ("Apply Image", err);
    }

    /* Calculations: Source 1 blended onto Source 2 (each reduced to grey). */
    bool calculations_plane (Session& s, const Value& p, std::vector<float>* out, Error* err)
    {
        const char* cmd = "image.calculations";
        Blending blending;
        if (!blending_parse (p, cmd, &blending, err))
        {
            return false;
        }
        double opacity_pct = 100.0;
        param_f64 (p, "opacity", &opacity_pct);
        float opacity = (float) fmin (fmax (opacity_pct, 0.0), 100.0) / 100.0f;
        Value spec1, spec2;
        if (!source_spec (p, "source1", &spec1))
        {
            spec1 = Value::object ();
        }
        if (!source_spec (p, "source2", &spec2))
        {
            spec2 = Value::object ();
        }
        std::vector<std::vector<float>> s1, s2;
        std::vector<float> mask;
        if (!source_planes (s, spec1, cmd, true, &s1, err) || !source_planes (s, spec2, cmd, true, &s2, err)
            || !mask_plane (s, p, cmd, &mask, err))
        {
            return false;
        }
        out->resize (s2[0].size ());
        for (size_t i = 0; i < out->size (); i++)
        {
            float b = s2[0][i], t = s1[0][i];
            (*out)[i] = b + (blending_apply (blending, b, t) - b) * opacity * (mask.empty () ? 1.0f : mask[i]);
        }
        return true;
    }

    raster::Surface plane_to_surface (const std::vector<float>& plane, Rect area, color::PixelFormat fmt)
    {
        raster::Surface out (fmt);
        out.write_region (area, plane.data (), plane.size ());
        out.prune ();
        return out;
    }

    bool cmd_calculations (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "image.calculations";
        std::vector<float> plane;
        if (!calculations_plane (s, p, &plane, err))
        {
            return false;
        }
        std::string target = "newChannel", name;
        param_str (p, "result", &target);
        bool named = param_str (p, "name", &name);
        if (target == "newChannel")
        {
            Edit e (s);
            doc::Document* d = e.doc ();
            if (d == NULL)
            {
                return fail (err, ErrorKind::NoDocument, "no active document");
            }
            doc::AlphaChannel ch;
            ch.name    = named ? name : next_channel_name (*d);
            ch.surface = plane_to_surface (
                plane, d->bounds (), color::PixelFormat{color::ColorMode::Grayscale, d->depth, false});
            d->channels.push_back (ch);
            if (!e.commit ("Calculations", err))
            {
                return false;
            }
            *result = Value{{"channel", d->channels.size () - 1}};
            return true;
        }
        if (target == "newDocument")
        {
            const doc::Document& like = *s.active ()->doc;
            doc::Document nd          = doc::Document::make (
                s.ids, named ? name : "Untitled", like.size, color::ColorMode::Grayscale, like.depth);
            nd.resolution_dpi      = like.resolution_dpi;
            color::PixelFormat fmt = nd.pixel_format ();
            doc::Layer bg          = doc::Layer::raster (s.ids, "Background", fmt);
            bg.locks.transparency  = true;
            bg.locks.position      = true;
            std::vector<float> data (plane.size () * fmt.channels (), 1.0f);
            for (size_t i = 0; i < plane.size (); i++)
            {
                data[i * fmt.channels ()] = fminf (fmaxf (plane[i], 0.0f), 1.0f);
            }
            bg.pixels.write_region (nd.bounds (), data.data (), data.size ());
            bg.pixels.prune ();
            nd.layers.push_back (bg);
            size_t index = s.add_document (nd, "");
            *result      = Value{{"document", index}};
            return true;
        }
        if (target == "selection")
        {
            Edit e (s);
            doc::Document* d = e.doc ();
            if (d == NULL)
            {
                return fail (err, ErrorKind::NoDocument, "no active document");
            }
            bool any = false;
            for (size_t i = 0; i < plane.size () && !any; i++)
            {
                any = plane[i] > 0.0f;
            }
            d->has_selection = any;
            d->selection     = any ? plane_to_surface (plane, d->bounds (), color::PixelFormat::gray8 ())
                                   : raster::Surface (color::PixelFormat::gray8 ());
            if (!e.commit ("Calculations", err))
            {
                return false;
            }
            *result = Value{{"selected", any}};
            return true;
        }
        return bad_params (err, cmd, "unknown result `%s` (newChannel, newDocument or selection)", target.c_str ());
    }

    bool has_apply_target (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l != NULL && l->kind == doc::LayerKind::Raster)
        {
            return true;
        }
        *why = "Apply Image needs a pixel layer";
        return false;
    }

    const CommandSpec APPLY_IMAGE_COMMANDS[] = {
        {"image.applyImage", "Apply Image…", "Image", NULL,
            "{\"source\":{\"document\":index?,\"layer\":id? (default "
            "merged),\"channel\":\"composite|red|…|selection|transparency|mask\"|alpha "
            "index=\"composite\",\"invert\":bool=false},"
            "\"blending\":\"multiply|screen|overlay|…|add|subtract\"=\"multiply\",\"opacity\":0..100=100,\"scale\":1.."
            "2=1,\"offset\":-255..255=0,"
            "\"preserveTransparency\":bool=false,\"mask\":{source}?}",
            has_apply_target, cmd_apply_image, true},
        {"image.calculations", "Calculations…", "Image", NULL,
            "{\"source1\":{source},\"source2\":{source},\"blending\":\"multiply|…\"=\"multiply\",\"opacity\":0..100="
            "100,\"scale\":1..2=1,\"offset\":-255..255=0,\"mask\":{source}?,"
            "\"result\":\"newChannel|newDocument|selection\"=\"newChannel\",\"name\":str?}",
            has_doc, cmd_calculations, true},
    };

    void register_apply_image_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), APPLY_IMAGE_COMMANDS,
            APPLY_IMAGE_COMMANDS + sizeof APPLY_IMAGE_COMMANDS / sizeof APPLY_IMAGE_COMMANDS[0]);
    }

} // namespace engine
