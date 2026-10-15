#include "compose/compose.h"
#include "engine/brush.h"
#include "engine/layers.h"
#include "engine/session.h"
#include <math.h>
#include <string.h>

namespace engine
{

    /* Edit > Cut, Copy, Copy Merged, Paste and Paste Special, on the session's clipboard: pixels with
       alpha and where they came from (for Paste in Place). Whole-layer copies of groups, type and
       smart objects are not ported (Copy takes what they show). */

    using geom::Rect;

    /* The visible document as one surface with alpha, in the document's colour. */
    raster::Surface merged_surface (const doc::Document& d, const color::CmykSpace* cmyk)
    {
        color::PixelFormat fmt           = d.pixel_format ();
        fmt.alpha                        = true;
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = cmyk;
        return compose::composite_to_surface (d, d.bounds (), fmt, NULL, settings);
    }

    bool set_clipboard (Session& s, raster::Surface surface, Value* result, Error* err)
    {
        Rect b = surface.content_bounds ();
        if (b.is_empty ())
        {
            return fail (err, ErrorKind::Other, "Could not copy: the selected area is empty");
        }
        s.clipboard.full    = true;
        s.clipboard.surface = surface;
        s.clipboard.bounds  = b;
        *result             = Value{{"bounds", {b.x0, b.y0, b.width (), b.height ()}}};
        return true;
    }

    /* Copy with the mask targeted: the mask's values as grayscale with the selection as alpha, so it
       pastes into another mask. */
    bool copy_mask (Session& s, Value* result, Error* err)
    {
        const DocState* st     = s.active ();
        const doc::Document& d = *st->doc;
        const doc::Layer* l    = d.layer (st->active_layer);
        if (l == NULL || !l->has_mask)
        {
            return fail (err, ErrorKind::Other, "the active layer has no layer mask");
        }
        Rect area = d.has_selection ? d.selection.content_bounds ().intersect (d.bounds ()) : d.bounds ();
        if (area.is_empty ())
        {
            return fail (err, ErrorKind::Other, "Could not copy: the selected area is empty");
        }
        std::vector<float> v, m;
        if (!l->mask.surface.read_region (area, &v))
        {
            return fail (err, ErrorKind::Other, "the area is too large to copy");
        }
        if (d.has_selection)
        {
            d.selection.read_region (area, &m);
        }
        size_t n = l->mask.surface.channels (), mk = d.selection.channels ();
        size_t count = v.size () / n;
        std::vector<float> px (count * 2);
        for (size_t i = 0; i < count; i++)
        {
            px[i * 2]     = v[i * n];
            px[i * 2 + 1] = d.has_selection ? m[i * mk] : 1.0f;
        }
        color::PixelFormat fmt = {color::ColorMode::Grayscale, l->mask.surface.format ().sample, true};
        raster::Surface out (fmt);
        out.write_region (area, px.data (), px.size ());
        out.prune ();
        if (!set_clipboard (s, out, result, err))
        {
            return false;
        }
        (*result)["target"] = "mask";
        return true;
    }

    bool copy_layer (Session& s, bool merged, Value* result, Error* err)
    {
        const DocState* st = s.active ();
        if (st == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        const doc::Document& d     = *st->doc;
        const raster::Surface* sel = d.has_selection ? &d.selection : NULL;
        if (merged)
        {
            return set_clipboard (s, lift (merged_surface (d, s.cmyk), sel, d.bounds ()), result, err);
        }
        const doc::Layer* l = d.layer (st->active_layer);
        if (l == NULL)
        {
            return fail (err, ErrorKind::Other, "no active layer");
        }
        if (l->surface () != NULL)
        {
            return set_clipboard (s, lift (*l->surface (), sel, d.bounds ()), result, err);
        }
        if (!l->is_group ())
        {
            return fail (err, ErrorKind::Other, "the active layer has no pixels");
        }
        doc::Document shown = d;
        shown.layers.assign (1, *l);
        shown.layers[0].visible = true;
        return set_clipboard (s, lift (merged_surface (shown, s.cmyk), sel, d.bounds ()), result, err);
    }

    bool cmd_edit_copy (Session& s, const Value& p, Value* result, Error* err)
    {
        if (is_mask_target (p))
        {
            return copy_mask (s, result, err);
        }
        return copy_layer (s, false, result, err);
    }

    bool cmd_edit_copy_merged (Session& s, const Value& p, Value* result, Error* err)
    {
        return copy_layer (s, true, result, err);
    }

    /* A locked layer refuses before anything is copied, so the clipboard stays as it was. */
    bool cmd_edit_cut (Session& s, const Value& p, Value* result, Error* err)
    {
        const DocState* st = s.active ();
        doc::LayerId id    = st->active_layer;
        if (!check_pixels_unlocked (*st->doc, id, err))
        {
            return false;
        }
        if (!copy_layer (s, false, result, err))
        {
            return false;
        }
        Edit e (s);
        if (!clear_selected (e.doc (), id, s.tools.background, s.cmyk, err))
        {
            return false;
        }
        return e.commit ("Cut Pixels", err);
    }

    /* How far a paste moves the clipboard: not at all in place, or when the pixels lie on the canvas
       and no view centre is given; else onto "center" (the view centre) or the canvas centre. */
    void paste_offset (const Clipboard& clip, Rect canvas, const Value& p, bool in_place, int32_t* dx, int32_t* dy)
    {
        *dx = *dy           = 0;
        const Value* center = param (p, "center");
        bool on_canvas      = clip.bounds.intersect (canvas) == clip.bounds;
        if (in_place || (on_canvas && center == NULL))
        {
            return;
        }
        double cx = (canvas.x0 + canvas.x1) / 2.0, cy = (canvas.y0 + canvas.y1) / 2.0;
        if (center != NULL && center->is_array () && center->size () >= 2 && (*center)[0].is_number ()
            && (*center)[1].is_number ())
        {
            cx = (*center)[0].get<double> ();
            cy = (*center)[1].get<double> ();
        }
        Rect b = clip.bounds;
        *dx    = (int32_t) lround (cx - (b.x0 + b.x1) / 2.0);
        *dy    = (int32_t) lround (cy - (b.y0 + b.y1) / 2.0);
    }

    /* The clipboard moved by the paste offset, in the document's colour with alpha. */
    raster::Surface placed_clip (const Session& s, const doc::Document& d, int32_t dx, int32_t dy)
    {
        const Clipboard& clip = s.clipboard;
        raster::Surface moved =
            dx == 0 && dy == 0 ? clip.surface : clip.surface.translated (dx, dy, clip.surface.content_bounds ());
        color::PixelFormat fmt = d.pixel_format ();
        fmt.alpha              = true;
        if (moved.format () != fmt)
        {
            moved = moved.convert (fmt, s.cmyk);
        }
        return moved;
    }

    /* Paste into the targeted mask: the pasted pixels' gray over what was there, through their alpha
       and `limit`. */
    bool paste_to_mask (Session& s, bool in_place, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id = s.active ()->active_layer;
        Edit e (s);
        doc::Document* d = e.doc ();
        doc::Layer* l    = d->layer_mut (id);
        if (l == NULL || !l->has_mask)
        {
            return fail (err, ErrorKind::Other, "the active layer has no layer mask");
        }
        if (d->effective_locks (id).all)
        {
            return fail (err, ErrorKind::Other, "layer \"%s\" is locked", l->name.c_str ());
        }
        int32_t dx, dy;
        paste_offset (s.clipboard, d->bounds (), p, in_place, &dx, &dy);
        raster::Surface moved = s.clipboard.surface;
        if (dx != 0 || dy != 0)
        {
            moved = moved.translated (dx, dy, moved.content_bounds ());
        }
        Rect area = moved.content_bounds ();
        std::vector<float> src, dst;
        if (!moved.read_region (area, &src) || !l->mask.surface.read_region (area, &dst))
        {
            return fail (err, ErrorKind::Other, "the paste is too large");
        }
        color::PixelFormat sf = moved.format ();
        size_t n = sf.channels (), mn = l->mask.surface.channels ();
        size_t count = dst.size () / mn;
        for (size_t i = 0; i < count; i++)
        {
            float rgba[4];
            raster::to_rgba (sf, &src[i * n], s.cmyk, rgba);
            float gray  = 0.299f * rgba[0] + 0.587f * rgba[1] + 0.114f * rgba[2];
            float a     = rgba[3];
            dst[i * mn] = dst[i * mn] * (1.0f - a) + gray * a;
        }
        l->mask.surface.write_region (area, dst.data (), dst.size ());
        l->mask.surface.prune ();
        if (!e.commit ("Paste", err))
        {
            return false;
        }
        *result = Value{{"layer", id}, {"offset", {dx, dy}}, {"target", "mask"}};
        return true;
    }

    /* Paste as a new layer above the active one (Photoshop deselects). */
    bool paste (Session& s, const Value& p, bool in_place, Value* result, Error* err)
    {
        if (s.active () == NULL)
        {
            return s.execute ("file.newFromClipboard", Value::object (), result, err);
        }
        if (is_mask_target (p))
        {
            return paste_to_mask (s, in_place, p, result, err);
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        int32_t dx, dy;
        paste_offset (s.clipboard, d->bounds (), p, in_place, &dx, &dy);
        raster::Surface surf = placed_clip (s, *d, dx, dy);
        doc::Layer l         = doc::Layer::raster (s.ids, d->next_layer_name ("Layer"), surf.format ());
        l.pixels             = surf;
        doc::LayerId id      = d->insert_above (*e.active_layer (), l);
        *e.active_layer ()   = id;
        d->has_selection     = false;
        d->selection         = raster::Surface (color::PixelFormat::gray8 ());
        if (!e.commit ("Paste", err))
        {
            return false;
        }
        *result = Value{{"layer", id}, {"offset", {dx, dy}}};
        return true;
    }

    bool cmd_edit_paste (Session& s, const Value& p, Value* result, Error* err)
    {
        return paste (s, p, false, result, err);
    }

    bool cmd_edit_paste_in_place (Session& s, const Value& p, Value* result, Error* err)
    {
        return paste (s, p, true, result, err);
    }

    /* The selection (or its inverse) as a mask surface at the document's depth. */
    raster::Surface selection_limit (const doc::Document& d, bool invert)
    {
        float fill             = invert ? 1.0f : 0.0f;
        color::PixelFormat fmt = {color::ColorMode::Grayscale, d.depth, false};
        raster::Surface out    = raster::Surface::with_default (fmt, &fill);
        Rect bounds            = d.bounds ();
        std::vector<float> v;
        if (!d.selection.read_region (bounds, &v))
        {
            return out;
        }
        size_t ch = d.selection.channels ();
        std::vector<float> g (v.size () / ch);
        for (size_t i = 0; i < g.size (); i++)
        {
            g[i] = invert ? 1.0f - v[i * ch] : v[i * ch];
        }
        out.write_region (bounds, g.data (), g.size ());
        out.prune ();
        return out;
    }

    /* Paste Into / Outside: the clipboard as a new layer centred on the selection, shown only inside
       (or outside) it through an unlinked mask, so the pixels can still be moved around. */
    bool paste_limited (Session& s, const Value& p, bool outside, Value* result, Error* err)
    {
        const doc::Document& d = *s.active ()->doc;
        raster::Surface limit  = selection_limit (d, outside);
        Value params           = Value::object ();
        Rect b                 = d.selection.content_bounds ();
        params["center"]       = {(b.x0 + b.x1) / 2.0, (b.y0 + b.y1) / 2.0};
        if (param_present (p, "center"))
        {
            params["center"] = *param (p, "center");
        }
        Edit e (s);
        doc::Document* nd = e.doc ();
        int32_t dx, dy;
        paste_offset (s.clipboard, nd->bounds (), params, false, &dx, &dy);
        raster::Surface surf = placed_clip (s, *nd, dx, dy);
        doc::Layer l         = doc::Layer::raster (s.ids, nd->next_layer_name ("Layer"), surf.format ());
        l.pixels             = surf;
        l.has_mask           = true;
        l.mask               = doc::LayerMask::reveal_all ();
        l.mask.surface       = limit;
        l.mask.linked        = false;
        doc::LayerId id      = nd->insert_above (*e.active_layer (), l);
        *e.active_layer ()   = id;
        nd->has_selection    = false;
        nd->selection        = raster::Surface (color::PixelFormat::gray8 ());
        if (!e.commit (outside ? "Paste Outside" : "Paste Into", err))
        {
            return false;
        }
        *result = Value{{"layer", id}, {"offset", {dx, dy}}};
        return true;
    }

    bool cmd_edit_paste_into (Session& s, const Value& p, Value* result, Error* err)
    {
        return paste_limited (s, p, false, result, err);
    }

    bool cmd_edit_paste_outside (Session& s, const Value& p, Value* result, Error* err)
    {
        return paste_limited (s, p, true, result, err);
    }

    /* A new document the size of the clipboard, holding it as one layer. */
    bool cmd_file_new_from_clipboard (Session& s, const Value& p, Value* result, Error* err)
    {
        const Clipboard& clip  = s.clipboard;
        Rect b                 = clip.bounds;
        color::PixelFormat fmt = clip.surface.format ();
        color::ColorMode mode  = fmt.mode;
        geom::Size size        = {(uint32_t) b.width (), (uint32_t) b.height ()};
        doc::Document d        = doc::Document::make (s.ids, "Untitled", size, mode, fmt.sample);
        doc::Layer l           = doc::Layer::raster (s.ids, d.next_layer_name ("Layer"), fmt);
        l.pixels               = clip.surface.translated (-b.x0, -b.y0, b);
        d.layers.push_back (l);
        size_t index = s.add_document (d, "");
        *result      = Value{{"document", index}, {"width", b.width ()}, {"height", b.height ()}};
        return true;
    }

    bool has_clip (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (s.clipboard.full)
        {
            return true;
        }
        *why = "the clipboard is empty";
        return false;
    }

    bool has_clip_only (const Session& s, std::string* why)
    {
        if (s.clipboard.full)
        {
            return true;
        }
        *why = "the clipboard is empty";
        return false;
    }

    bool has_clip_and_selection (const Session& s, std::string* why)
    {
        if (!has_clip (s, why))
        {
            return false;
        }
        if (s.active ()->doc->has_selection)
        {
            return true;
        }
        *why = "Paste Into needs a selection";
        return false;
    }

    bool can_copy (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l->surface () != NULL || l->is_group () || l->has_mask)
        {
            return true;
        }
        *why = "the active layer has no pixels";
        return false;
    }

    bool has_pixels (const Session& s, std::string* why)
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

    const CommandSpec CLIPBOARD_COMMANDS[] = {
        {"edit.cut", "Cut", "Edit", "Cmd+X", "{}", has_pixels, cmd_edit_cut, true},
        {"edit.copy", "Copy", "Edit", "Cmd+C", "{\"target\":\"mask\"? (the targeted mask, copied as grayscale)}",
            can_copy, cmd_edit_copy, true},
        {"edit.copyMerged", "Copy Merged", "Edit", "Cmd+Shift+C", "{}", has_doc, cmd_edit_copy_merged, true},
        {"edit.paste", "Paste", "Edit", "Cmd+V",
            "{\"center\":[x,y]? (view centre; default keeps the position when it overlaps the "
            "canvas),\"target\":\"pixels|mask\"=\"pixels\"} (no document: a new "
            "document from the clipboard)",
            has_clip_only, cmd_edit_paste, true},
        {"edit.pasteSpecial.pasteInPlace", "Paste in Place", "Edit/Paste Special", "Cmd+Shift+V",
            "{\"target\":\"pixels|mask\"=\"pixels\"}", has_clip, cmd_edit_paste_in_place, true},
        {"edit.pasteSpecial.pasteInto", "Paste Into", "Edit/Paste Special", "Cmd+Alt+Shift+V", "{\"center\":[x,y]?}",
            has_clip_and_selection, cmd_edit_paste_into, true},
        {"edit.pasteSpecial.pasteOutside", "Paste Outside", "Edit/Paste Special", NULL, "{\"center\":[x,y]?}",
            has_clip_and_selection, cmd_edit_paste_outside, true},
        {"file.newFromClipboard", "New from Clipboard", "File", NULL, "{}", has_clip_only, cmd_file_new_from_clipboard,
            true},
    };

    void register_clipboard_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), CLIPBOARD_COMMANDS,
            CLIPBOARD_COMMANDS + sizeof CLIPBOARD_COMMANDS / sizeof CLIPBOARD_COMMANDS[0]);
    }

} // namespace engine
