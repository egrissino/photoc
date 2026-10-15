#include "compose/compose.h"
#include "compose/masks.h"
#include "engine/layers.h"
#include "engine/session.h"
#include "engine/text.h"
#include "engine/vector.h"
#include <math.h>
#include <string.h>
#include <vector>

namespace engine
{

    /* Edit > Fade, Image > Duplicate, Image > Mode and Layer > Stamp Visible. */

    using geom::Rect;

    /* The active layer's pixels before the last step, when that step changed them. */
    const raster::Surface* fade_source (const Session& s)
    {
        const DocState* d = s.active ();
        if (d == NULL || d->history.past_len () == 0)
        {
            return NULL;
        }
        ops::DocRef before    = d->history.state (d->history.past_len () - 1);
        const doc::Layer* was = before ? before->layer (d->active_layer) : NULL;
        const doc::Layer* now = d->doc->layer (d->active_layer);
        if (was == NULL || now == NULL || was->surface () == NULL || now->surface () == NULL)
        {
            return NULL;
        }
        if (was->surface ()->format () != now->surface ()->format () || before->size.width != d->doc->size.width
            || before->size.height != d->doc->size.height)
        {
            return NULL;
        }
        if (compose::same_tiles (*was->surface (), *now->surface ()))
        {
            return NULL;
        }
        return was->surface ();
    }

    bool can_fade (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (fade_source (s) != NULL)
        {
            return true;
        }
        *why = "the last step didn't change the active layer's pixels";
        return false;
    }

    /* Fade: the last step's result on the active layer blended over what it replaced, with `mode`
       at `opacity` percent. A new history step, as in Photoshop. */
    bool cmd_fade (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd               = "edit.fade";
        const raster::Surface* before = fade_source (s);
        if (before == NULL)
        {
            return fail (err, ErrorKind::Other, "the last step didn't change the active layer's pixels");
        }
        double opacity = 100.0;
        param_f64 (p, "opacity", &opacity);
        if (!(opacity >= 0.0 && opacity <= 100.0))
        {
            return bad_params (err, cmd, "opacity must be 0..100");
        }
        color::BlendMode mode = color::BlendMode::Normal;
        std::string m;
        if (param_str (p, "mode", &m) && (!blend_from_str (m.c_str (), &mode) || mode == color::BlendMode::PassThrough))
        {
            return bad_params (err, cmd, "unknown blend mode");
        }
        std::string label = std::string ("Fade ")
                          + (s.active ()->history.undo_label () != NULL ? s.active ()->history.undo_label () : "");
        raster::Surface old = *before;
        Edit e (s);
        doc::Document* d      = e.doc ();
        raster::Surface* surf = d->layer_mut (*e.active_layer ())->surface_mut ();
        Rect area             = old.content_bounds ().unite (surf->content_bounds ()).intersect (d->bounds ());
        if (area.is_empty ())
        {
            return e.commit (label.c_str (), err);
        }
        color::PixelFormat fmt = surf->format ();
        size_t n = fmt.channels (), count = (size_t) area.width () * area.height ();
        std::vector<float> a (count * 4), b (count * 4), out (count * n);
        old.read_rgba (area, s.cmyk, a.data ());
        surf->read_rgba (area, s.cmyk, b.data ());
        float k = (float) (opacity / 100.0);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
        {
            size_t i = (size_t) ii;
            float o[4];
            color::composite (mode, &a[i * 4], &b[i * 4], k, o);
            /* Without alpha (the Background) the result stays opaque; Normal fades alpha too. */
            if (!fmt.alpha)
            {
                o[3] = 1.0f;
            }
            raster::from_rgba (fmt, o, s.cmyk, &out[i * n]);
        }
        surf->write_region (area, out.data (), out.size ());
        surf->prune ();
        return e.commit (label.c_str (), err);
    }

    /* Image > Duplicate: the document (layers, selection, channels) as a new untitled one. */
    bool cmd_image_duplicate (Session& s, const Value& p, Value* result, Error* err)
    {
        const DocState* cur = s.active ();
        doc::Document copy  = *cur->doc;
        doc::LayerId active = cur->active_layer;
        copy.id             = s.ids.fresh ();
        copy.name           = cur->doc->name + " copy";
        param_str (p, "name", &copy.name);
        s.add_document (copy, "");
        DocState* d = s.active ();
        if (d->doc->layer (active) != NULL)
        {
            d->active_layer = active;
            d->selected_layers.assign (1, active);
            d->layer_anchor = active;
        }
        *result = Value{{"name", copy.name}};
        return true;
    }

    /* Layer > Stamp Visible (Cmd+Alt+Shift+E): everything visible merged into a new layer above the
       active one; the layers themselves stay. */
    bool cmd_stamp_visible (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::Document solo = *d;
        solo.layers.clear ();
        for (size_t i = 0; i < d->layers.size (); i++)
        {
            if (d->layers[i].visible)
            {
                solo.layers.push_back (d->layers[i]);
            }
        }
        if (solo.layers.empty ())
        {
            return fail (err, ErrorKind::Other, "there are no visible layers to stamp");
        }
        color::PixelFormat fmt = d->pixel_format ();
        fmt.alpha              = true;
        doc::Layer stamp       = doc::Layer::raster (s.ids, d->next_layer_name ("Layer"), fmt);
        stamp.pixels           = composite_layers (solo, fmt, s.cmyk);
        doc::LayerId at        = *e.active_layer () != doc::NO_LAYER ? *e.active_layer () : d->top_layer ();
        doc::LayerId id        = d->insert_above (at, stamp);
        e.select (std::vector<doc::LayerId> (1, id), id);
        if (!e.commit ("Stamp Visible", err))
        {
            return false;
        }
        *result = Value{{"layer", id}};
        return true;
    }

    const CommandSpec MISC_COMMANDS[] = {
        {"edit.fade", "Fade…", "Edit", "Cmd+Shift+F",
            "{\"opacity\":0..100=100,\"mode\":\"normal|multiply|…\"=\"normal\"}", can_fade, cmd_fade, true},
        {"image.duplicate", "Duplicate", "Image", NULL, "{\"name\":string?}", has_doc, cmd_image_duplicate, true},
        {"layer.stampVisible", "Stamp Visible", "Layer", "Cmd+Alt+Shift+E", "{}", has_doc, cmd_stamp_visible, true},
    };

    void register_misc_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), MISC_COMMANDS, MISC_COMMANDS + sizeof MISC_COMMANDS / sizeof MISC_COMMANDS[0]);
    }

} // namespace engine
