#include "algo/transform.h"
#include "engine/brush.h"
#include "engine/filter.h"
#include "engine/layers.h"
#include "engine/session.h"
#include "engine/smart.h"
#include "io/io.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace engine
{

    /* Layer > Smart Objects and File > Place Embedded. A smart object layer shows embedded
       contents (a document of its own) mapped onto four corners; transforming it moves the corners
       and renders the contents again, so nothing is lost. Edit Contents opens the contents as a
       document; saving that document (Ctrl+S) puts it back into every layer using it. */

    /* A fresh id in the 8-4-4-4-12 form Photoshop uses. */
    std::string new_uuid (Session& s)
    {
        uint64_t a = s.ids.fresh () * 0x9E3779B97F4A7C15ull ^ 0xD1B54A32D192ED03ull,
                 b = (a ^ (a >> 29)) * 0xBF58476D1CE4E5B9ull;
        a          = (a ^ (a >> 31)) * 0x94D049BB133111EBull;
        char buf[40];
        snprintf (buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", (unsigned) (a >> 32), (unsigned) (a >> 16) & 0xffff,
            (unsigned) a & 0xffff, (unsigned) (b >> 48), (unsigned long long) (b & 0xffffffffffffull));
        return buf;
    }

    /* Renders a smart layer from its contents and remembers the corners it was rendered at. */
    void render_smart (Session& s, const doc::Document& d, doc::Layer* l)
    {
        render_smart_filtered (d, l, s.cmyk, s.tools.foreground, s.tools.background);
    }

    /* Smart layers whose corners moved since they were rendered (all of them with `all`, after a
       change of the document's format). */
    void render_smart_changed (Session& s, doc::Document* d, bool all)
    {
        std::vector<doc::WalkEntry> walk = d->walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (walk[i].layer->kind != doc::LayerKind::Smart)
            {
                continue;
            }
            doc::Layer* l          = d->layer_mut (walk[i].layer->id);
            color::PixelFormat fmt = d->pixel_format ();
            fmt.alpha              = true;
            bool moved             = memcmp (l->smart.quad, l->smart.rendered, sizeof l->smart.quad) != 0;
            if (all || moved || l->pixels.format () != fmt)
            {
                render_smart (s, *d, l);
            }
        }
    }

    void rect_quad (double x0, double y0, double x1, double y1, double quad[4][2])
    {
        quad[0][0] = x0;
        quad[0][1] = y0;
        quad[1][0] = x1;
        quad[1][1] = y0;
        quad[2][0] = x1;
        quad[2][1] = y1;
        quad[3][0] = x0;
        quad[3][1] = y1;
    }

    void map_quad (double quad[4][2], const algo::Homography& h)
    {
        for (int i = 0; i < 4; i++)
        {
            h.apply (quad[i][0], quad[i][1], &quad[i][0], &quad[i][1]);
        }
    }

    /* A smart layer's placement through h: its corners, and its warp's control points. */
    void map_smart (doc::SmartLayer* s, const algo::Homography& h)
    {
        map_quad (s->quad, h);
        if (s->warped)
        {
            for (int i = 0; i < 16; i++)
            {
                h.apply (s->warp[i][0], s->warp[i][1], &s->warp[i][0], &s->warp[i][1]);
            }
        }
    }

    /* The corners for contents resized from ow x oh to nw x nh: the old placement extended, so the
       contents keep their scale. */
    void resize_quad (double quad[4][2], uint32_t ow, uint32_t oh, uint32_t nw, uint32_t nh)
    {
        double r[4] = {0, 0, (double) ow, (double) oh};
        algo::Homography h;
        if (ow == 0 || oh == 0 || !algo::Homography::rect_to_quad (r, quad, &h))
        {
            return;
        }
        double q[4][2];
        rect_quad (0, 0, nw, nh, q);
        map_quad (q, h);
        memcpy (quad, q, sizeof q);
    }

    bool smart_layer (const Session& s, const Value& p, doc::LayerId* id, Error* err)
    {
        if (!layer_param (s, p, id, err))
        {
            return false;
        }
        const doc::Layer* l = s.active ()->doc->layer (*id);
        if (l == NULL)
        {
            return no_layer (err, *id);
        }
        if (l->kind != doc::LayerKind::Smart)
        {
            return fail (err, ErrorKind::Other, "\"%s\" is not a smart object", l->name.c_str ());
        }
        return true;
    }

    /* Convert to Smart Object: the selected layers move into contents of their own size (their
       bounds), replaced by one smart object where the top one was. */
    bool cmd_convert_smart (Session& s, const Value& p, Value* result, Error* err)
    {
        std::vector<doc::LayerId> sel = s.active ()->selected ();
        Edit e (s);
        doc::Document* d              = e.doc ();
        std::vector<doc::LayerId> top = top_level (*d, sel);
        if (top.empty ())
        {
            return fail (err, ErrorKind::Other, "no layer selected");
        }
        /* In stacking order, bottom first. */
        std::vector<doc::WalkEntry> walk = d->walk ();
        std::vector<doc::LayerId> order;
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (std::find (top.begin (), top.end (), walk[i].layer->id) != top.end ())
            {
                order.push_back (walk[i].layer->id);
            }
        }
        geom::Rect bounds;
        for (size_t i = 0; i < order.size (); i++)
        {
            const doc::Layer* l = d->layer (order[i]);
            if (l->is_group ())
            {
                std::vector<doc::WalkEntry> inner = d->walk ();
                for (size_t k = 0; k < inner.size (); k++)
                {
                    const raster::Surface* sf = inner[k].layer->surface ();
                    doc::LayerPath gp;
                    d->path_of (l->id, &gp);
                    bool under = inner[k].path.size () > gp.size ()
                              && std::equal (gp.begin (), gp.end (), inner[k].path.begin ());
                    if (under && sf != NULL)
                    {
                        bounds = bounds.unite (sf->content_bounds ());
                    }
                }
            }
            else if (l->surface () != NULL)
            {
                bounds = bounds.unite (l->surface ()->content_bounds ());
            }
        }
        bounds = bounds.intersect (d->bounds ().inflate (100000));
        if (bounds.is_empty ())
        {
            bounds = d->bounds ();
        }
        std::string name = d->layer (order.back ())->name;
        doc::Document inner =
            doc::Document::make (s.ids, name, geom::Size{bounds.width (), bounds.height ()}, d->mode, d->depth);
        inner.resolution_dpi = d->resolution_dpi;
        inner.icc_profile    = d->icc_profile;
        for (size_t i = 0; i < order.size (); i++)
        {
            doc::Layer l = *d->layer (order[i]);
            translate_layer (&l, -bounds.x0, -bounds.y0);
            inner.layers.push_back (l);
        }
        doc::SmartContents c;
        c.uuid     = new_uuid (s);
        c.filename = name + ".psb";
        memcpy (c.filetype, "8BPB", 4);
        c.width  = bounds.width ();
        c.height = bounds.height ();
        c.doc    = std::make_shared<const doc::Document> (inner);
        d->smart.push_back (c);
        doc::Layer sl = doc::Layer::make (s.ids, name, doc::LayerKind::Smart);
        sl.smart.uuid = c.uuid;
        rect_quad (bounds.x0, bounds.y0, bounds.x1, bounds.y1, sl.smart.quad);
        render_smart (s, *d, &sl);
        doc::LayerId id = d->insert_above (order.back (), sl);
        for (size_t i = 0; i < order.size (); i++)
        {
            doc::Layer gone;
            d->remove (order[i], &gone);
        }
        e.select (std::vector<doc::LayerId> (1, id), id);
        if (!e.commit ("Convert to Smart Object", err))
        {
            return false;
        }
        *result = Value{{"layer", id}};
        return true;
    }

    /* Contents placed from a file: centred on the canvas, scaled down to fit when larger. */
    bool place_contents (Session& s, Edit* e, doc::SmartContents c, const std::string& name, const Value& p,
        doc::LayerId* out, Error* err)
    {
        doc::Document* d = e->doc ();
        double w = c.width, h = c.height, scale = 1.0;
        if (w > d->size.width || h > d->size.height)
        {
            scale = std::min ((double) d->size.width / w, (double) d->size.height / h);
        }
        double cx = d->size.width / 2.0, cy = d->size.height / 2.0;
        param_f64 (p, "x", &cx);
        param_f64 (p, "y", &cy);
        d->smart.push_back (c);
        doc::Layer sl = doc::Layer::make (s.ids, name, doc::LayerKind::Smart);
        sl.smart.uuid = c.uuid;
        /* On whole pixels, so contents placed at their own size stay sharp. */
        double x0 = round (cx - w * scale / 2), y0 = round (cy - h * scale / 2);
        rect_quad (x0, y0, x0 + w * scale, y0 + h * scale, sl.smart.quad);
        render_smart (s, *d, &sl);
        doc::LayerId at = *e->active_layer () != doc::NO_LAYER ? *e->active_layer () : d->top_layer ();
        *out            = d->insert_above (at, sl);
        e->select (std::vector<doc::LayerId> (1, *out), *out);
        (void) err;
        return true;
    }

    bool load_contents (
        Session& s, const char* cmd, const Value& p, doc::SmartContents* c, std::string* stem, Error* err)
    {
        std::string path;
        if (!param_str (p, "path", &path) || path.empty ())
        {
            return bad_params (err, cmd, "missing `path`");
        }
        std::vector<uint8_t> bytes;
        char msg[512] = "";
        if (!io::read_file (path.c_str (), &bytes, msg, sizeof msg))
        {
            return fail (err, ErrorKind::Other, "%s", msg);
        }
        std::string name = path.substr (path.find_last_of ('/') == std::string::npos ? 0 : path.find_last_of ('/') + 1);
        std::vector<std::string> warnings;
        if (!io::contents_from_bytes (s.ids, s.fonts (), name, std::make_shared<const std::vector<uint8_t>> (bytes), c,
                &warnings, msg, sizeof msg))
        {
            return fail (err, ErrorKind::Other, "%s", msg);
        }
        doc::Document inner = *c->doc;
        io::decode_smart_contents (s.ids, s.fonts (), &inner, &warnings, 1);
        c->doc  = std::make_shared<const doc::Document> (inner);
        c->uuid = new_uuid (s);
        *stem   = io::file_stem (path.c_str ());
        return true;
    }

    /* File > Place Embedded. */
    bool cmd_place_embedded (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::SmartContents c;
        std::string stem;
        if (!load_contents (s, "file.placeEmbedded", p, &c, &stem, err))
        {
            return false;
        }
        Edit e (s);
        doc::LayerId id;
        if (!place_contents (s, &e, c, stem, p, &id, err))
        {
            return false;
        }
        if (!e.commit ("Place Embedded", err))
        {
            return false;
        }
        *result = Value{{"layer", id}};
        return true;
    }

    /* Replace Contents: a file in place of the contents, at the same scale and placement. */
    bool cmd_replace_contents (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!smart_layer (s, p, &id, err))
        {
            return false;
        }
        doc::SmartContents c;
        std::string stem;
        if (!load_contents (s, "layer.smartObject.replaceContents", p, &c, &stem, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d               = e.doc ();
        std::string old                = d->layer (id)->smart.uuid;
        const doc::SmartContents* prev = d->smart_contents (old);
        uint32_t ow = prev != NULL ? prev->width : c.width, oh = prev != NULL ? prev->height : c.height;
        /* Every layer sharing the contents takes the new ones. */
        c.uuid                   = old;
        doc::SmartContents* slot = d->smart_contents_mut (old);
        if (slot != NULL)
        {
            *slot = c;
        }
        else
        {
            d->smart.push_back (c);
        }
        std::vector<doc::WalkEntry> walk = d->walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (walk[i].layer->kind != doc::LayerKind::Smart || walk[i].layer->smart.uuid != old)
            {
                continue;
            }
            doc::Layer* l = d->layer_mut (walk[i].layer->id);
            resize_quad (l->smart.quad, ow, oh, c.width, c.height);
            render_smart (s, *d, l);
        }
        return e.commit ("Replace Contents", err);
    }

    /* Export Contents: the embedded file as it is, or edited contents as a Photoshop document. */
    bool cmd_export_contents (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.smartObject.exportContents";
        doc::LayerId id;
        if (!smart_layer (s, p, &id, err))
        {
            return false;
        }
        std::string path;
        if (!param_str (p, "path", &path) || path.empty ())
        {
            return bad_params (err, cmd, "missing `path`");
        }
        const doc::Document& d      = *s.active ()->doc;
        const doc::SmartContents* c = d.smart_contents (d.layer (id)->smart.uuid);
        if (c == NULL)
        {
            return fail (err, ErrorKind::Other, "the smart object's contents are missing");
        }
        char msg[512] = "";
        if (c->bytes)
        {
            if (!io::write_file (path.c_str (), *c->bytes, msg, sizeof msg))
            {
                return fail (err, ErrorKind::Other, "%s", msg);
            }
        }
        else
        {
            std::vector<std::string> warnings;
            compose::RenderSettings rs = compose::default_settings ();
            rs.cmyk                    = s.cmyk;
            if (!c->doc
                || !io::save_file (*c->doc, s.fonts (), path.c_str (), "psd", codecs::EncodeOptions::defaults (), rs,
                    &warnings, msg, sizeof msg))
            {
                return fail (err, ErrorKind::Other, "%s", msg[0] ? msg : "the contents can't be written");
            }
        }
        *result = Value{{"path", path}};
        return true;
    }

    /* Edit Contents: the contents open as a document linked back to this one. */
    bool cmd_edit_contents (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!smart_layer (s, p, &id, err))
        {
            return false;
        }
        const DocState* parent      = s.active ();
        const doc::SmartContents* c = parent->doc->smart_contents (parent->doc->layer (id)->smart.uuid);
        if (c == NULL || !c->doc)
        {
            return fail (err, ErrorKind::Other, "the smart object's contents can't be opened in this build");
        }
        doc::DocId parent_id = parent->doc->id;
        std::string uuid     = c->uuid;
        /* Already open: show it. */
        for (size_t i = 0; i < s.doc_count (); i++)
        {
            if (s.doc_at (i)->smart_parent == parent_id && s.doc_at (i)->smart_uuid == uuid)
            {
                s.set_active (i);
                *result = Value{{"document", i}};
                return true;
            }
        }
        doc::Document inner = *c->doc;
        inner.id            = s.ids.fresh ();
        inner.name          = c->filename;
        size_t index        = s.add_document (inner, "");
        DocState* st        = s.doc_at (index);
        st->smart_parent    = parent_id;
        st->smart_uuid      = uuid;
        *result             = Value{{"document", index}};
        return true;
    }

    /* The active document is a smart object's contents: put it back into its document. */
    bool cmd_commit_contents (Session& s, const Value& p, Value* result, Error* err)
    {
        DocState* child = s.active ();
        if (child == NULL || child->smart_parent == 0)
        {
            return fail (err, ErrorKind::Other, "this document is not a smart object's contents");
        }
        size_t child_index = s.active_index (), parent_index = s.doc_count ();
        for (size_t i = 0; i < s.doc_count (); i++)
        {
            if (s.doc_at (i)->doc->id == child->smart_parent)
            {
                parent_index = i;
            }
        }
        if (parent_index == s.doc_count ())
        {
            return fail (err, ErrorKind::Other, "the document this smart object belongs to was closed");
        }
        std::shared_ptr<const doc::Document> contents = child->doc;
        std::string uuid                              = child->smart_uuid;
        s.set_active (parent_index);
        bool ok;
        {
            Edit e (s);
            doc::Document* d      = e.doc ();
            doc::SmartContents* c = d->smart_contents_mut (uuid);
            if (c == NULL)
            {
                s.set_active (child_index);
                return fail (err, ErrorKind::Other, "the smart object is no longer in its document");
            }
            uint32_t ow = c->width, oh = c->height;
            c->doc = contents;
            c->bytes.reset ();
            c->width                         = contents->size.width;
            c->height                        = contents->size.height;
            std::vector<doc::WalkEntry> walk = d->walk ();
            for (size_t i = 0; i < walk.size (); i++)
            {
                if (walk[i].layer->kind != doc::LayerKind::Smart || walk[i].layer->smart.uuid != uuid)
                {
                    continue;
                }
                doc::Layer* l = d->layer_mut (walk[i].layer->id);
                if (ow != c->width || oh != c->height)
                {
                    resize_quad (l->smart.quad, ow, oh, c->width, c->height);
                }
                render_smart (s, *d, l);
            }
            ok = e.commit ("Edit Contents", err);
        }
        s.set_active (child_index);
        if (ok)
        {
            s.doc_at (child_index)->saved_revision = s.doc_at (child_index)->revision;
        }
        return ok;
    }

    /* New Smart Object via Copy: a duplicate with contents of its own. */
    bool cmd_smart_via_copy (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!smart_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d            = e.doc ();
        doc::Layer copy             = d->layer (id)->duplicate (s.ids);
        copy.name                   = d->copy_name (copy.name);
        const doc::SmartContents* c = d->smart_contents (copy.smart.uuid);
        if (c != NULL)
        {
            doc::SmartContents nc = *c;
            nc.uuid               = new_uuid (s);
            d->smart.push_back (nc);
            copy.smart.uuid = nc.uuid;
        }
        doc::LayerId nid = d->insert_above (id, copy);
        e.select (std::vector<doc::LayerId> (1, nid), nid);
        if (!e.commit ("New Smart Object via Copy", err))
        {
            return false;
        }
        *result = Value{{"layer", nid}};
        return true;
    }

    bool cmd_rasterize_smart (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!smart_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        l->kind       = doc::LayerKind::Raster;
        l->smart.uuid.clear ();
        return e.commit ("Rasterize Smart Object", err);
    }

    /* Reset Transform: the contents at their own size, centred where the layer is. */
    bool cmd_reset_smart_transform (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!smart_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Document* d            = e.doc ();
        doc::Layer* l               = d->layer_mut (id);
        const doc::SmartContents* c = d->smart_contents (l->smart.uuid);
        if (c == NULL || c->width == 0)
        {
            return fail (err, ErrorKind::Other, "the smart object's contents are missing");
        }
        double cx = 0, cy = 0;
        for (int i = 0; i < 4; i++)
        {
            cx += l->smart.quad[i][0] / 4;
            cy += l->smart.quad[i][1] / 4;
        }
        rect_quad (round (cx - c->width / 2.0), round (cy - c->height / 2.0), round (cx - c->width / 2.0) + c->width,
            round (cy - c->height / 2.0) + c->height, l->smart.quad);
        l->smart.warped = false;
        render_smart (s, *d, l);
        return e.commit ("Reset Transform", err);
    }

    bool cmd_smart_info (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!smart_layer (s, p, &id, err))
        {
            return false;
        }
        const doc::Document& d      = *s.active ()->doc;
        const doc::Layer* l         = d.layer (id);
        const doc::SmartContents* c = d.smart_contents (l->smart.uuid);
        Value q                     = Value::array ();
        for (int i = 0; i < 4; i++)
        {
            q.push_back ({l->smart.quad[i][0], l->smart.quad[i][1]});
        }
        Value filters = Value::array ();
        for (size_t i = 0; i < l->smart.filters.size (); i++)
        {
            const doc::SmartFilter& f = l->smart.filters[i];
            Value fp                  = Value::parse (f.params, nullptr, false);
            filters.push_back (
                Value{{"id", f.id}, {"name", f.name}, {"enabled", f.enabled}, {"mode", blend_variant_name (f.blend)},
                    {"opacity", f.opacity * 100.0f}, {"params", fp.is_discarded () ? Value::object () : fp}});
        }
        Value v = {
            {"uuid", l->smart.uuid}, {"quad", q}, {"filters", filters}, {"filtersEnabled", l->smart.filters_enabled}};
        if (c != NULL)
        {
            v["filename"] = c->filename;
            v["width"]    = c->width;
            v["height"]   = c->height;
            v["editable"] = (bool) c->doc;
        }
        *result = v;
        return true;
    }

    /* ---- Smart filters ---- */

    bool filter_index (const Session& s, const Value& p, doc::LayerId* id, size_t* index, const char* cmd, Error* err)
    {
        if (!smart_layer (s, p, id, err))
        {
            return false;
        }
        int64_t i;
        const doc::Layer* l = s.active ()->doc->layer (*id);
        if (!param_i64 (p, "index", &i) || i < 0 || (size_t) i >= l->smart.filters.size ())
        {
            return bad_params (err, cmd, "no smart filter at `index`");
        }
        *index = (size_t) i;
        return true;
    }

    /* One filter on or off ("index"), or the whole stack. */
    bool cmd_smart_filter_enabled (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!smart_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        int64_t i;
        bool on;
        if (param_i64 (p, "index", &i))
        {
            if (i < 0 || (size_t) i >= l->smart.filters.size ())
            {
                return bad_params (err, "layer.smartFilter.enabled", "no smart filter at `index`");
            }
            on = !l->smart.filters[(size_t) i].enabled;
            param_bool (p, "enabled", &on);
            l->smart.filters[(size_t) i].enabled = on;
        }
        else
        {
            on = !l->smart.filters_enabled;
            param_bool (p, "enabled", &on);
            l->smart.filters_enabled = on;
        }
        render_smart (s, *e.doc (), l);
        return e.commit (on ? "Enable Smart Filter" : "Disable Smart Filter", err);
    }

    bool cmd_smart_filter_delete (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        size_t i;
        if (!filter_index (s, p, &id, &i, "layer.smartFilter.delete", err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        l->smart.filters.erase (l->smart.filters.begin () + (ptrdiff_t) i);
        render_smart (s, *e.doc (), l);
        return e.commit ("Delete Smart Filter", err);
    }

    bool cmd_smart_filter_clear (Session& s, const Value& p, Value* result, Error* err)
    {
        doc::LayerId id;
        if (!smart_layer (s, p, &id, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l = e.doc ()->layer_mut (id);
        l->smart.filters.clear ();
        render_smart (s, *e.doc (), l);
        return e.commit ("Clear Smart Filters", err);
    }

    bool cmd_smart_filter_edit (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.smartFilter.edit";
        doc::LayerId id;
        size_t i;
        if (!filter_index (s, p, &id, &i, cmd, err))
        {
            return false;
        }
        const Value* params = param (p, "params");
        if (params == NULL || !params->is_object ())
        {
            return bad_params (err, cmd, "`params` must be the filter's settings");
        }
        Edit e (s);
        if (!set_smart_filter (e.doc (), id, (int) i, *params, s.cmyk, s.tools.foreground, s.tools.background, err))
        {
            return false;
        }
        return e.commit ("Edit Smart Filter", err);
    }

    /* Blending Options: the filter's mode and opacity (percent) over what is under it. */
    bool cmd_smart_filter_blending (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.smartFilter.blending";
        doc::LayerId id;
        size_t i;
        if (!filter_index (s, p, &id, &i, cmd, err))
        {
            return false;
        }
        Edit e (s);
        doc::Layer* l       = e.doc ()->layer_mut (id);
        doc::SmartFilter& f = l->smart.filters[i];
        std::string m;
        if (param_str (p, "mode", &m)
            && (!blend_from_str (m.c_str (), &f.blend) || f.blend == color::BlendMode::PassThrough))
        {
            return bad_params (err, cmd, "unknown blend mode");
        }
        double o;
        if (param_f64 (p, "opacity", &o))
        {
            if (!(o >= 0.0 && o <= 100.0))
            {
                return bad_params (err, cmd, "`opacity` must be 0..100");
            }
            f.opacity = (float) (o / 100.0);
        }
        render_smart (s, *e.doc (), l);
        return e.commit ("Smart Filter Blending Options", err);
    }

    bool cmd_smart_filter_move (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "layer.smartFilter.move";
        doc::LayerId id;
        size_t i;
        if (!filter_index (s, p, &id, &i, cmd, err))
        {
            return false;
        }
        int64_t to;
        const doc::Layer* cur = s.active ()->doc->layer (id);
        if (!param_i64 (p, "to", &to) || to < 0 || (size_t) to >= cur->smart.filters.size ())
        {
            return bad_params (err, cmd, "`to` must be a filter index");
        }
        Edit e (s);
        doc::Layer* l      = e.doc ()->layer_mut (id);
        doc::SmartFilter f = l->smart.filters[i];
        l->smart.filters.erase (l->smart.filters.begin () + (ptrdiff_t) i);
        l->smart.filters.insert (l->smart.filters.begin () + (ptrdiff_t) to, f);
        render_smart (s, *e.doc (), l);
        return e.commit ("Move Smart Filter", err);
    }

    bool has_smart_filters (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l != NULL && l->kind == doc::LayerKind::Smart && !l->smart.filters.empty ())
        {
            return true;
        }
        *why = "the active layer has no smart filters";
        return false;
    }

    bool has_smart_layer (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (l != NULL && l->kind == doc::LayerKind::Smart)
        {
            return true;
        }
        *why = "the active layer is not a smart object";
        return false;
    }

    bool is_smart_contents (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (s.active ()->smart_parent != 0)
        {
            return true;
        }
        *why = "this document is not a smart object's contents";
        return false;
    }

    const CommandSpec SMART_COMMANDS[] = {
        {"file.placeEmbedded", "Place Embedded…", "File", NULL,
            "{\"path\":str,\"x\":px?,\"y\":px? (centre; default the canvas centre)}", has_doc, cmd_place_embedded,
            true},
        {"layer.smartObject.convert", "Convert to Smart Object", "Layer/Smart Objects", NULL,
            "{} (the selected layers)", has_layer, cmd_convert_smart, true},
        {"layer.smartObject.newViaCopy", "New Smart Object via Copy", "Layer/Smart Objects", NULL, "{\"layer\":id?}",
            has_smart_layer, cmd_smart_via_copy, true},
        {"layer.smartObject.editContents", "Edit Contents", "Layer/Smart Objects", NULL, "{\"layer\":id?} → {document}",
            has_smart_layer, cmd_edit_contents, false},
        {"layer.smartObject.resetTransform", "Reset Transform", "Layer/Smart Objects", NULL, "{\"layer\":id?}",
            has_smart_layer, cmd_reset_smart_transform, true},
        {"layer.smartObject.replaceContents", "Replace Contents…", "Layer/Smart Objects", NULL,
            "{\"layer\":id?,\"path\":str}", has_smart_layer, cmd_replace_contents, true},
        {"layer.smartObject.exportContents", "Export Contents…", "Layer/Smart Objects", NULL,
            "{\"layer\":id?,\"path\":str}", has_smart_layer, cmd_export_contents, false},
        {"layer.smartObject.rasterize", "Rasterize", "Layer/Smart Objects", NULL, "{\"layer\":id?}", has_smart_layer,
            cmd_rasterize_smart, true},
        {"layer.rasterize.smartObject", "Smart Object", "Layer/Rasterize", NULL, "{\"layer\":id?}", has_smart_layer,
            cmd_rasterize_smart, true},
        {"layer.smartObject.commitContents", "Save Contents", "", NULL, "{} (from the contents document)",
            is_smart_contents, cmd_commit_contents, false},
        {"filter.convertForSmartFilters", "Convert for Smart Filters", "Filter", NULL, "{} (the selected layers)",
            has_layer, cmd_convert_smart, true},
        {"layer.smartFilter.enabled", "Disable Smart Filters", "Layer/Smart Filter", NULL,
            "{\"layer\":id?,\"index\":n? (none: the whole stack),\"enabled\":bool? (toggle)}", has_smart_filters,
            cmd_smart_filter_enabled, true},
        {"layer.smartFilter.clear", "Clear Smart Filters", "Layer/Smart Filter", NULL, "{\"layer\":id?}",
            has_smart_filters, cmd_smart_filter_clear, true},
        {"layer.smartFilter.delete", "Delete Smart Filter", "", NULL, "{\"layer\":id?,\"index\":n}", has_smart_filters,
            cmd_smart_filter_delete, true},
        {"layer.smartFilter.edit", "Edit Smart Filter", "", NULL,
            "{\"layer\":id?,\"index\":n,\"params\":{…the filter's settings}}", has_smart_filters, cmd_smart_filter_edit,
            true},
        {"layer.smartFilter.blending", "Smart Filter Blending Options", "", NULL,
            "{\"layer\":id?,\"index\":n,\"mode\":blend,\"opacity\":0..100}", has_smart_filters,
            cmd_smart_filter_blending, true},
        {"layer.smartFilter.move", "Move Smart Filter", "", NULL, "{\"layer\":id?,\"index\":n,\"to\":n}",
            has_smart_filters, cmd_smart_filter_move, true},
        {"layer.smartObject.info", "Smart Object", "", NULL,
            "{\"layer\":id?} → {uuid, quad, filename, width, height, editable}", has_smart_layer, cmd_smart_info,
            false},
    };

    void register_smart_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), SMART_COMMANDS, SMART_COMMANDS + sizeof SMART_COMMANDS / sizeof SMART_COMMANDS[0]);
    }

} // namespace engine
