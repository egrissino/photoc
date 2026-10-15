#include "algo/resample.h"
#include "algo/transform.h"
#include "compose/compose.h"
#include "engine/layers.h"
#include "engine/session.h"
#include "engine/text.h"
#include <math.h>
#include <string.h>

namespace engine
{

    /* Image > Image Size, Canvas Size, Crop and Trim. Rotated crops and the Crop tool's target
       size are not ported yet. */

    using geom::Rect;

    void color_param (const Value& p, const char* key, const float fallback[4], float out[4]);

    const uint32_t MAX_SIDE           = 300000;
    const uint64_t MAX_RESAMPLE_BYTES = (uint64_t) 8 << 30;

    /* Every pixel surface of a layer tree: content, and masks when `masks`. */
    void for_each_surface (std::vector<doc::Layer>* layers, bool masks,
        void (*fn) (raster::Surface* s, bool is_mask, void* user), void* user)
    {
        for (size_t i = 0; i < layers->size (); i++)
        {
            doc::Layer& l = (*layers)[i];
            if (masks && l.has_mask)
            {
                fn (&l.mask.surface, true, user);
            }
            raster::Surface* s = l.surface_mut ();
            if (s != NULL)
            {
                fn (s, false, user);
            }
            for_each_surface (&l.children, masks, fn, user);
        }
    }

    struct ResizeJob
    {
        double sx;
        double sy;
        algo::Resample filter;
        Rect canvas;
    };

    void resize_one (raster::Surface* s, bool is_mask, void* user)
    {
        const ResizeJob* j = (const ResizeJob*) user;
        *s                 = algo::resize_surface_in_canvas (
            *s, j->sx, j->sy, is_mask ? algo::Resample::Bilinear : j->filter, j->canvas);
    }

    /* The whole document (pixels, masks, selection) resampled to nw x nh. Content reaching the
       canvas edge keeps it: a Background stays opaque to the border. */
    void resample_doc (doc::Document* d, uint32_t nw, uint32_t nh, algo::Resample filter)
    {
        uint32_t ow = d->size.width, oh = d->size.height;
        double sx = (double) nw / (ow > 0 ? ow : 1), sy = (double) nh / (oh > 0 ? oh : 1);
        if (fabs (sx - 1.0) < 1e-12 && fabs (sy - 1.0) < 1e-12)
        {
            return;
        }
        ResizeJob job = {sx, sy, filter, d->bounds ()};
        for_each_surface (&d->layers, true, resize_one, &job);
        if (d->has_selection)
        {
            d->selection = algo::resize_surface_in_canvas (d->selection, sx, sy, algo::Resample::Bilinear, job.canvas);
        }
        for (size_t i = 0; i < d->channels.size (); i++)
        {
            d->channels[i].surface =
                algo::resize_surface_in_canvas (d->channels[i].surface, sx, sy, algo::Resample::Bilinear, job.canvas);
        }
        geom::Size size = {nw, nh};
        d->size         = size;
    }

    bool check_resample_budget (const char* cmd, const doc::Document& d, uint32_t nw, uint32_t nh, Error* err)
    {
        uint64_t bpp = d.pixel_format ().bytes_per_pixel ();
        if (bpp == 0)
        {
            bpp = 1;
        }
        uint64_t bytes = (uint64_t) nw * nh * bpp;
        if (bytes <= MAX_RESAMPLE_BYTES)
        {
            return true;
        }
        return bad_params (err, cmd,
            "%u x %u px is %llu megapixels; resampling at this bit depth is limited to %llu megapixels. Choose a "
            "smaller size",
            nw, nh, (unsigned long long) ((uint64_t) nw * nh / 1000000),
            (unsigned long long) (MAX_RESAMPLE_BYTES / bpp / 1000000));
    }

    uint32_t side (double v)
    {
        double r = round (v);
        return (uint32_t) (r < 1.0 ? 1.0 : r > MAX_SIDE ? MAX_SIDE : r);
    }

    /* Image > Image Size. A missing dimension keeps the aspect ratio; "none" changes the resolution
       only; "automatic" is Preserve Details growing, Bicubic Sharper shrinking. */
    bool cmd_image_size (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd          = "image.imageSize";
        const doc::Document& cur = *s.active ()->doc;
        uint32_t ow = cur.size.width, oh = cur.size.height;
        std::string method = "bicubic";
        param_str (p, "resample", &method);
        bool none = method == "none", automatic = method == "automatic";
        algo::Resample filter = algo::Resample::Bicubic;
        if (!none && !automatic && !algo::resample_from_str (method.c_str (), &filter))
        {
            filter = algo::Resample::Bicubic;
        }
        double pw, ph;
        bool hw = param_f64 (p, "width", &pw), hh = param_f64 (p, "height", &ph);
        double nwf = ow, nhf = oh;
        if (hw && hh)
        {
            nwf = pw;
            nhf = ph;
        }
        else if (hw)
        {
            nwf = pw;
            nhf = pw * oh / (ow > 0 ? ow : 1);
        }
        else if (hh)
        {
            nhf = ph;
            nwf = ph * ow / (oh > 0 ? oh : 1);
        }
        uint32_t nw = side (nwf), nh = side (nhf);
        bool resize = !none && (nw != ow || nh != oh);
        if (resize && !check_resample_budget (cmd, cur, nw, nh, err))
        {
            return false;
        }
        double dpi;
        bool has_dpi = param_f64 (p, "resolution", &dpi);
        if (automatic)
        {
            filter = nw > ow || nh > oh ? algo::Resample::PreserveDetails : algo::Resample::BicubicSharper;
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        double old_dpi   = d->resolution_dpi;
        if (has_dpi)
        {
            d->resolution_dpi = (float) (dpi < 1.0 ? 1.0 : dpi > 10000.0 ? 10000.0 : dpi);
        }
        if (resize)
        {
            resample_doc (d, nw, nh, filter);
        }
        /* Type keeps its look: scaled with the pixels, and against the new resolution's points. */
        double k  = old_dpi / d->resolution_dpi;
        double sx = resize ? (double) nw / ow * k : k, sy = resize ? (double) nh / oh * k : k;
        double m[6] = {sx, 0, 0, sy, 0, 0};
        follow_document_transform (s, d, m);
        if (!e.commit ("Image Size", err))
        {
            return false;
        }
        *result = Value{{"width", s.active ()->doc->size.width}, {"height", s.active ()->doc->size.height}};
        return true;
    }

    struct Shift
    {
        int32_t dx;
        int32_t dy;
    };

    void translate_one (raster::Surface* s, bool is_mask, void* user)
    {
        const Shift* m = (const Shift*) user;
        *s             = s->translated (m->dx, m->dy, s->content_bounds ());
    }

    void crop_one (raster::Surface* s, bool is_mask, void* user)
    {
        const Rect* keep = (const Rect*) user;
        float def[raster::MAX_CHANNELS];
        s->default_pixel (def);
        raster::Surface out = raster::Surface::with_default (s->format (), def);
        Rect r              = s->content_bounds ().intersect (*keep);
        if (!r.is_empty ())
        {
            std::vector<uint8_t> bytes;
            s->to_interleaved (r, &bytes);
            out.write_interleaved (r, bytes.data (), bytes.size ());
        }
        *s = out;
    }

    /* Every surface and the selection moved by (dx, dy). */
    void translate_doc (doc::Document* d, int32_t dx, int32_t dy)
    {
        if (dx == 0 && dy == 0)
        {
            return;
        }
        Shift m = {dx, dy};
        for_each_surface (&d->layers, true, translate_one, &m);
        if (d->has_selection)
        {
            d->selection = d->selection.translated (dx, dy, d->selection.content_bounds ());
        }
        for (size_t i = 0; i < d->channels.size (); i++)
        {
            raster::Surface& c = d->channels[i].surface;
            c                  = c.translated (dx, dy, c.content_bounds ());
        }
    }

    /* The Background keeps no transparency: area a canvas change adds is filled with `ext`, behind
       what it held (`old`, in new coordinates). */
    void extend_background (doc::Document* d, Rect old, const float ext[4], const color::CmykSpace* cmyk)
    {
        const doc::Layer* b = d->background ();
        if (b == NULL)
        {
            return;
        }
        raster::Surface* surf = d->layer_mut (b->id)->surface_mut ();
        float rgba[4]         = {ext[0], ext[1], ext[2], 1.0f};
        float px[raster::MAX_CHANNELS];
        raster::from_rgba (surf->format (), rgba, cmyk, px);
        Rect keep = surf->content_bounds ().intersect (old);
        std::vector<uint8_t> saved;
        if (!keep.is_empty ())
        {
            surf->to_interleaved (keep, &saved);
        }
        surf->fill_rect (d->bounds (), px);
        if (!keep.is_empty ())
        {
            surf->write_interleaved (keep, saved.data (), saved.size ());
        }
    }

    /* The document cropped to r (current coordinates); a frame past the canvas fills the
       Background's new area with `ext`. */
    void crop_doc (doc::Document* d, Rect r, bool delete_pixels, const float ext[4], const color::CmykSpace* cmyk)
    {
        Rect old_canvas = d->bounds ();
        bool extends    = r.x0 < old_canvas.x0 || r.y0 < old_canvas.y0 || r.x1 > old_canvas.x1 || r.y1 > old_canvas.y1;
        if (delete_pixels)
        {
            for_each_surface (&d->layers, false, crop_one, &r);
        }
        translate_doc (d, -r.x0, -r.y0);
        geom::Size size = {r.width (), r.height ()};
        d->size         = size;
        if (extends)
        {
            extend_background (d, old_canvas.translate (-r.x0, -r.y0), ext, cmyk);
        }
    }

    const char* const ANCHOR_NAMES[9] = {
        "topLeft", "top", "topRight", "left", "center", "right", "bottomLeft", "bottom", "bottomRight"};

    bool extension_color (const Session& s, const Value& p, float out[4])
    {
        std::string e = "background";
        param_str (p, "extensionColor", &e);
        if (e == "background")
        {
            memcpy (out, s.tools.background, 4 * sizeof (float));
        }
        else if (e == "foreground")
        {
            memcpy (out, s.tools.foreground, 4 * sizeof (float));
        }
        else if (e == "white")
        {
            out[0] = out[1] = out[2] = out[3] = 1.0f;
        }
        else if (e == "black")
        {
            out[0] = out[1] = out[2] = 0.0f;
            out[3]                   = 1.0f;
        }
        else if (e == "transparent")
        {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
        }
        else if (!parse_hex (e.c_str (), out))
        {
            return false;
        }
        return true;
    }

    /* Image > Canvas Size: the canvas grows or shrinks around the anchor; the Background is
       extended with the extension colour (a transparent extension makes it a normal layer). */
    bool cmd_canvas_size (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd          = "image.canvasSize";
        const doc::Document& cur = *s.active ()->doc;
        double ow = cur.size.width, oh = cur.size.height;
        bool relative = false;
        param_bool (p, "relative", &relative);
        double w = relative ? 0.0 : ow, h = relative ? 0.0 : oh;
        param_f64 (p, "width", &w);
        param_f64 (p, "height", &h);
        uint32_t nw = side (relative ? ow + w : w), nh = side (relative ? oh + h : h);
        std::string anchor = "center";
        param_str (p, "anchor", &anchor);
        int ai = -1;
        for (int i = 0; i < 9; i++)
        {
            if (anchor == ANCHOR_NAMES[i])
            {
                ai = i;
            }
        }
        if (ai < 0)
        {
            return bad_params (err, cmd,
                "`anchor` must be one of topLeft|top|topRight|left|center|right|bottomLeft|bottom|bottomRight");
        }
        double ax = (ai % 3) * 0.5, ay = (ai / 3) * 0.5;
        int32_t dx = (int32_t) round ((nw - ow) * ax), dy = (int32_t) round ((nh - oh) * ay);
        float ext[4];
        if (!extension_color (s, p, ext))
        {
            return bad_params (err, cmd, "unknown extensionColor");
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        translate_doc (d, dx, dy);
        double shift[6] = {1, 0, 0, 1, (double) dx, (double) dy};
        follow_document_transform (s, d, shift);
        geom::Size size = {nw, nh};
        d->size         = size;
        Rect old        = Rect::from_xywh (dx, dy, (uint32_t) ow, (uint32_t) oh);
        if (ext[3] == 0.0f && (nw > ow || nh > oh))
        {
            background_to_layer (d, d->background () != NULL ? d->background ()->id : doc::NO_LAYER);
        }
        if (ext[3] > 0.0f)
        {
            extend_background (d, old, ext, s.cmyk);
        }
        if (!e.commit ("Canvas Size", err))
        {
            return false;
        }
        *result = Value{{"width", nw}, {"height", nh}, {"offset", {dx, dy}}};
        return true;
    }

    /* Image > Crop: to x/y/width/height (the Crop tool; may reach past the canvas) or to the
       selection's bounds. */
    bool cmd_crop (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "image.crop";
        bool del        = true;
        param_bool (p, "deleteCroppedPixels", &del);
        int64_t x, y, w, h;
        bool explicit_rect = param_i64 (p, "x", &x) && param_i64 (p, "y", &y) && param_i64 (p, "width", &w)
                          && param_i64 (p, "height", &h) && w > 0 && h > 0;
        if (param_present (p, "angle"))
        {
            double a;
            if (param_f64 (p, "angle", &a) && a != 0.0)
            {
                return bad_params (err, cmd, "rotated crops are not ported yet");
            }
        }
        Rect r;
        const doc::Document& cur = *s.active ()->doc;
        if (explicit_rect)
        {
            if (w > MAX_SIDE || h > MAX_SIDE)
            {
                return bad_params (
                    err, cmd, "%lld x %lld exceeds the %u px limit per side", (long long) w, (long long) h, MAX_SIDE);
            }
            if (x < INT32_MIN || y < INT32_MIN || x + w > INT32_MAX || y + h > INT32_MAX)
            {
                return bad_params (err, cmd, "the crop is outside the 32-bit coordinate range");
            }
            r = Rect::from_xywh ((int32_t) x, (int32_t) y, (uint32_t) w, (uint32_t) h);
        }
        else if (cur.has_selection)
        {
            r = cur.selection.content_bounds ().intersect (cur.bounds ());
        }
        if (r.is_empty ())
        {
            return fail (err, ErrorKind::Other, "nothing to crop: pass x/y/width/height or make a selection");
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        crop_doc (d, r, del, s.tools.background, s.cmyk);
        double shift[6] = {1, 0, 0, 1, (double) -r.x0, (double) -r.y0};
        follow_document_transform (s, d, shift);
        d->has_selection = false;
        d->selection     = raster::Surface (color::PixelFormat::gray8 ());
        if (!e.commit ("Crop", err))
        {
            return false;
        }
        *result = Value{{"x", r.x0}, {"y", r.y0}, {"width", r.width ()}, {"height", r.height ()}};
        return true;
    }

    /* Image > Trim: the canvas cut to what differs from transparency or a corner's colour. */
    bool cmd_trim (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd                  = "image.trim";
        const doc::Document& cur         = *s.active ()->doc;
        Rect canvas                      = cur.bounds ();
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = s.cmyk;
        compose::Buffer buf              = compose::flatten (cur, settings);
        std::string based                = "transparent";
        param_str (p, "basedOn", &based);
        if (based != "transparent" && based != "topLeft" && based != "bottomRight")
        {
            return bad_params (err, cmd, "basedOn must be transparent|topLeft|bottomRight");
        }
        size_t w = canvas.width (), n = buf.pixel_count ();
        float ref[4] = {0, 0, 0, 0};
        bool has_ref = based != "transparent";
        if (based == "topLeft")
        {
            memcpy (ref, &buf.px[0], sizeof ref);
        }
        if (based == "bottomRight")
        {
            memcpy (ref, &buf.px[(n - 1) * 4], sizeof ref);
        }
        int32_t bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = INT32_MIN, by1 = INT32_MIN;
        for (size_t i = 0; i < n; i++)
        {
            const float* q = &buf.px[i * 4];
            bool differs;
            if (!has_ref)
            {
                differs = q[3] > 0.0f;
            }
            else
            {
                differs = fabsf (q[3] - ref[3]) > 0.5f / 255.0f;
                for (int c = 0; c < 3 && !differs; c++)
                {
                    differs = fabsf (q[c] * q[3] - ref[c] * ref[3]) > 0.5f / 255.0f;
                }
            }
            if (!differs)
            {
                continue;
            }
            int32_t x = (int32_t) (i % w), y = (int32_t) (i / w);
            if (x < bx0)
            {
                bx0 = x;
            }
            if (y < by0)
            {
                by0 = y;
            }
            if (x + 1 > bx1)
            {
                bx1 = x + 1;
            }
            if (y + 1 > by1)
            {
                by1 = y + 1;
            }
        }
        if (bx0 == INT32_MAX)
        {
            return fail (err, ErrorKind::Other, "nothing to trim to");
        }
        bool top = true, bottom = true, left = true, right = true;
        param_bool (p, "top", &top);
        param_bool (p, "bottom", &bottom);
        param_bool (p, "left", &left);
        param_bool (p, "right", &right);
        Rect r = {left ? bx0 : canvas.x0, top ? by0 : canvas.y0, right ? bx1 : canvas.x1, bottom ? by1 : canvas.y1};
        Edit e (s);
        crop_doc (e.doc (), r, true, s.tools.background, s.cmyk);
        double shift[6] = {1, 0, 0, 1, (double) -r.x0, (double) -r.y0};
        follow_document_transform (s, e.doc (), shift);
        if (!e.commit ("Trim", err))
        {
            return false;
        }
        *result = Value{{"x", r.x0}, {"y", r.y0}, {"width", r.width ()}, {"height", r.height ()}};
        return true;
    }

    /* Image > Reveal All: the canvas grows to every layer's pixels. */
    bool cmd_reveal_all (Session& s, const Value& p, Value* result, Error* err)
    {
        const doc::Document& cur         = *s.active ()->doc;
        Rect all                         = cur.bounds ();
        std::vector<doc::WalkEntry> walk = cur.walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            const raster::Surface* surf = walk[i].layer->surface ();
            if (surf != NULL)
            {
                all = all.unite (surf->content_bounds ());
            }
        }
        if (all == cur.bounds ())
        {
            *result = Value{{"changed", false}};
            return true;
        }
        Value params = {{"x", all.x0}, {"y", all.y0}, {"width", all.width ()}, {"height", all.height ()},
            {"deleteCroppedPixels", false}};
        if (!s.execute ("image.crop", params, result, err))
        {
            return false;
        }
        *result = Value{{"changed", true}};
        return true;
    }

    /* Image > Image Rotation > Arbitrary: every surface turned `angle` degrees clockwise about the
       canvas centre, and the canvas grown to hold the turned image. The Background's new corners take
       the background colour. */
    struct Rotation
    {
        algo::Homography h;
    };

    void rotate_one (raster::Surface* s, bool is_mask, void* user)
    {
        const Rotation* r = (const Rotation*) user;
        if (is_mask)
        {
            *s = algo::warp_gray (*s, r->h, algo::Interp::Bilinear);
            return;
        }
        /* A surface without alpha is the Background, which cmd_rotate_arbitrary turns itself. */
        Rect content = s->content_bounds ();
        if (content.is_empty () || !s->format ().alpha)
        {
            return;
        }
        *s = algo::warp_surface (*s, content, r->h, algo::Interp::Bicubic);
    }

    bool cmd_rotate_arbitrary (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "image.imageRotation.arbitrary";
        double angle;
        if (!param_f64 (p, "angle", &angle) || !(angle >= -359.99 && angle <= 359.99))
        {
            return bad_params (err, cmd, "`angle` must be -359.99..359.99 degrees");
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        double W = d->size.width, H = d->size.height;
        double t = angle * M_PI / 180.0, c = cos (t), sn = sin (t);
        uint32_t nw = (uint32_t) ceil (fabs (W * c) + fabs (H * sn) - 1e-6),
                 nh = (uint32_t) ceil (fabs (W * sn) + fabs (H * c) - 1e-6);
        if (nw > MAX_SIDE || nh > MAX_SIDE)
        {
            return bad_params (err, cmd, "the turned canvas would exceed the %u px limit per side", MAX_SIDE);
        }
        /* new = T(nw/2, nh/2) * R(angle) * T(-W/2, -H/2); clockwise on screen (y down). */
        double tx = nw / 2.0 - (c * W / 2.0 - sn * H / 2.0), ty = nh / 2.0 - (sn * W / 2.0 + c * H / 2.0);
        Rotation r = {algo::Homography::affine (c, sn, -sn, c, tx, ty)};
        /* The Background: filled first with the background colour over the new canvas. */
        const doc::Layer* bg = d->background ();
        raster::Surface bg_old;
        doc::LayerId bg_id = bg != NULL ? bg->id : doc::NO_LAYER;
        if (bg != NULL)
        {
            bg_old = *bg->surface ();
        }
        for_each_surface (&d->layers, true, rotate_one, &r);
        double turn[6] = {c, sn, -sn, c, tx, ty};
        follow_document_transform (s, d, turn);
        if (bg_id != doc::NO_LAYER)
        {
            raster::Surface* surf = d->layer_mut (bg_id)->surface_mut ();
            float rgba[4]         = {s.tools.background[0], s.tools.background[1], s.tools.background[2], 1.0f};
            float px[raster::MAX_CHANNELS];
            raster::Surface warped = algo::warp_surface (bg_old, bg_old.content_bounds (), r.h, algo::Interp::Bicubic);
            raster::from_rgba (warped.format (), rgba, s.cmyk, px);
            raster::Surface fresh (warped.format ());
            fresh.fill_rect (Rect::from_xywh (0, 0, nw, nh), px);
            algo::composite_over (&fresh, warped);
            *surf = fresh.convert (surf->format (), s.cmyk);
        }
        if (d->has_selection)
        {
            d->selection     = algo::warp_gray (d->selection, r.h, algo::Interp::Bilinear);
            d->has_selection = !d->selection.content_bounds ().is_empty ();
        }
        for (size_t i = 0; i < d->channels.size (); i++)
        {
            d->channels[i].surface = algo::warp_gray (d->channels[i].surface, r.h, algo::Interp::Bilinear);
        }
        geom::Size size = {nw, nh};
        d->size         = size;
        if (!e.commit ("Rotate Canvas", err))
        {
            return false;
        }
        *result = Value{{"width", nw}, {"height", nh}};
        return true;
    }

    const CommandSpec IMAGE_COMMANDS[] = {
        {"image.imageRotation.arbitrary", "Arbitrary…", "Image/Image Rotation", NULL,
            "{\"angle\":-359.99..359.99 (degrees, clockwise)}", has_doc, cmd_rotate_arbitrary, true},
        {"image.imageSize", "Image Size…", "Image", "Cmd+Alt+I",
            "{\"width\":px,\"height\":px,\"resolution\":ppi,\"resample\":\"automatic|preserveDetails|bicubicSmoother|"
            "bicubicSharper|bicubicAutomatic|bicubic|nearest|"
            "bilinear|lanczos|none\"=\"bicubic\"} → {width,height}",
            has_doc, cmd_image_size, true},
        {"image.canvasSize", "Canvas Size…", "Image", "Cmd+Alt+C",
            "{\"width\":px,\"height\":px,\"relative\":bool=false,\"anchor\":\"topLeft|top|topRight|left|center|right|"
            "bottomLeft|bottom|bottomRight\"=\"center\","
            "\"extensionColor\":\"background|foreground|white|black|transparent|#rrggbb\"=\"background\"}",
            has_doc, cmd_canvas_size, true},
        {"image.crop", "Crop", "Image", NULL,
            "{\"x\":px,\"y\":px,\"width\":px,\"height\":px,\"deleteCroppedPixels\":bool=true} (no rect: the "
            "selection's bounds)",
            has_doc, cmd_crop, true},
        {"image.trim", "Trim…", "Image", NULL,
            "{\"basedOn\":\"transparent|topLeft|bottomRight\"=\"transparent\",\"top\":bool=true,\"bottom\":bool=true,"
            "\"left\":bool=true,\"right\":bool=true}",
            has_doc, cmd_trim, true},
        {"image.revealAll", "Reveal All", "Image", NULL, "{}", has_doc, cmd_reveal_all, true},
    };

    void register_image_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), IMAGE_COMMANDS, IMAGE_COMMANDS + sizeof IMAGE_COMMANDS / sizeof IMAGE_COMMANDS[0]);
    }

} // namespace engine
