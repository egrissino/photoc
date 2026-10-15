#include "algo/matting.h"
#include "algo/quick_select.h"
#include "algo/select_ops.h"
#include "algo/selection.h"
#include "compose/compose.h"
#include "engine/layers.h"
#include "engine/session.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace engine
{

    /* The Select menu and the selection tools: Lasso and Polygonal Lasso (select.lasso), Magic Wand,
       Color Range, Select > Modify, Grow, Similar and Reselect. Color Range's Sampled Colors judge
       RGB distance (the reference's Lab model, fitted to Photoshop, is not ported yet). */

    using geom::Rect;

    float num_or (const Value& p, const char* key, float fallback)
    {
        double v;
        return param_f64 (p, key, &v) ? (float) v : fallback;
    }

    bool flag_or (const Value& p, const char* key, bool fallback)
    {
        bool v;
        return param_bool (p, key, &v) ? v : fallback;
    }

    std::string mode_param (const Value& p)
    {
        std::string m = "replace";
        param_str (p, "mode", &m);
        return m;
    }

    /* RGBA floats the tools sample over the canvas: the active layer's pixels, or the composite. */
    bool sample_pixels (const Session& s, bool all_layers, std::vector<float>* px, Error* err)
    {
        const DocState* d = s.active ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        Rect area           = d->doc->bounds ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (!all_layers && l != NULL && l->surface () != NULL)
        {
            px->resize ((size_t) area.width () * area.height () * 4);
            l->surface ()->read_rgba (area, s.cmyk, px->data ());
            return true;
        }
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = s.cmyk;
        *px                              = compose::render (*d->doc, area, settings).px;
        return true;
    }

    /* The same as 8-bit RGBA: a pixel layer reads its tiles straight to bytes, without a float
       copy of the canvas. */
    bool sample_bytes (const Session& s, bool all_layers, std::vector<uint8_t>* img, Error* err)
    {
        const DocState* d = s.active ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        Rect area           = d->doc->bounds ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        img->resize ((size_t) area.width () * area.height () * 4);
        if (!all_layers && l != NULL && l->surface () != NULL)
        {
            l->surface ()->read_rgba8 (area, s.cmyk, img->data ());
            return true;
        }
        std::vector<float> px;
        if (!sample_pixels (s, true, &px, err))
        {
            return false;
        }
#pragma omp parallel for schedule(static)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) px.size (); i++)
        {
            float v            = px[(size_t) i] < 0.0f ? 0.0f : px[(size_t) i] > 1.0f ? 1.0f : px[(size_t) i];
            (*img)[(size_t) i] = (uint8_t) (v * 255.0f + 0.5f);
        }
        return true;
    }

    bool set_mask_selection (Session& s, const char* label, const std::vector<float>& mask, const std::string& mode,
        Value* result, Error* err)
    {
        Edit e (s);
        doc::Document* d = e.doc ();
        apply_selection (d, algo::mask_to_surface (mask, d->bounds ()), mode);
        bool selected = d->has_selection;
        if (!e.commit (label, err))
        {
            return false;
        }
        *result = Value{{"selected", selected}};
        return true;
    }

    bool cmd_magic_wand (Session& s, const Value& p, Value* result, Error* err)
    {
        int32_t x = (int32_t) floorf (num_or (p, "x", 0.0f)), y = (int32_t) floorf (num_or (p, "y", 0.0f));
        std::vector<uint8_t> img;
        if (!sample_bytes (s, flag_or (p, "sampleAllLayers", false), &img, err))
        {
            return false;
        }
        Rect area = s.active ()->doc->bounds ();
        algo::Region region;
        bool found = algo::wand_region (img, area, x, y, num_or (p, "tolerance", 32.0f),
            flag_or (p, "contiguous", true), flag_or (p, "antiAlias", true), &region);
        Edit e (s);
        doc::Document* d      = e.doc ();
        raster::Surface shape = found ? algo::region_surface (region) : raster::Surface (color::PixelFormat::gray8 ());
        apply_selection (d, shape, mode_param (p));
        bool selected = d->has_selection;
        if (!e.commit ("Magic Wand", err))
        {
            return false;
        }
        *result = Value{{"selected", selected}};
        return true;
    }

    /* Lasso and Polygonal Lasso: the polygon through "points", anti-aliased and feathered. */
    bool cmd_lasso (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd  = "select.lasso";
        const Value* pts = param (p, "points");
        std::vector<algo::PolyPoint> poly;
        if (pts != NULL && pts->is_array ())
        {
            for (size_t i = 0; i < pts->size (); i++)
            {
                const Value& q = (*pts)[i];
                if (!q.is_array () || q.size () < 2 || !q[0].is_number () || !q[1].is_number ())
                {
                    continue;
                }
                algo::PolyPoint pt = {q[0].get<float> (), q[1].get<float> ()};
                poly.push_back (pt);
            }
        }
        if (poly.size () < 3)
        {
            return bad_params (err, cmd, "needs at least 3 points");
        }
        float feather = num_or (p, "feather", 0.0f);
        if (!(feather >= 0.0f && feather <= 1000.0f))
        {
            return bad_params (err, cmd, "feather must be a number in 0..1000");
        }
        /* Only the polygon's bounds (with room for the feather) are drawn, not the whole canvas. */
        float x0 = poly[0].x, y0 = poly[0].y, x1 = poly[0].x, y1 = poly[0].y;
        for (size_t i = 1; i < poly.size (); i++)
        {
            x0 = fminf (x0, poly[i].x);
            y0 = fminf (y0, poly[i].y);
            x1 = fmaxf (x1, poly[i].x);
            y1 = fmaxf (y1, poly[i].y);
        }
        int32_t margin = 1 + (int32_t) ceilf (feather * 3.0f);
        Rect box       = {(int32_t) floorf (x0) - margin, (int32_t) floorf (y0) - margin, (int32_t) ceilf (x1) + margin,
                  (int32_t) ceilf (y1) + margin};
        Rect area      = box.intersect (s.active ()->doc->bounds ());
        raster::Surface shape (color::PixelFormat::gray8 ());
        if (!area.is_empty ())
        {
            std::vector<float> mask = algo::polygon_mask (poly, area, flag_or (p, "antiAlias", true));
            if (feather > 0.0f)
            {
                mask = algo::feather_mask (mask, area.width (), area.height (), feather);
            }
            shape = algo::mask_to_surface (mask, area);
        }
        Edit e (s);
        doc::Document* d = e.doc ();
        apply_selection (d, shape, mode_param (p));
        bool selected = d->has_selection;
        if (!e.commit ("Lasso", err))
        {
            return false;
        }
        *result = Value{{"selected", selected}};
        return true;
    }

    /* Hue (degrees) and chroma of an RGB colour. */
    void hue_chroma (const float rgb[3], float* hue, float* chroma)
    {
        float r = rgb[0], g = rgb[1], b = rgb[2];
        float mx = fmaxf (r, fmaxf (g, b)), mn = fminf (r, fminf (g, b)), d = mx - mn;
        *chroma = d;
        if (d <= 0.0f)
        {
            *hue = 0.0f;
            return;
        }
        float h;
        if (mx == r)
        {
            h = fmodf ((g - b) / d + 6.0f, 6.0f);
        }
        else if (mx == g)
        {
            h = (b - r) / d + 2.0f;
        }
        else
        {
            h = (r - g) / d + 4.0f;
        }
        *hue = h * 60.0f;
    }

    float clamp01 (float v)
    {
        return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }

    /* Select > Color Range: sampled colours (with fuzziness, several samples), a hue family, or
       highlights / midtones / shadows. Replacing stays inside the selection the dialog opened on. */
    bool cmd_color_range (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd    = "select.colorRange";
        std::string preset = "sampledColors";
        if (param_present (p, "select") && !param_str (p, "select", &preset))
        {
            return bad_params (err, cmd, "`select` must be a string");
        }
        std::string mode = mode_param (p);
        std::vector<float> px;
        if (!sample_pixels (s, flag_or (p, "sampleAllLayers", true), &px, err))
        {
            return false;
        }
        const doc::Document& d = *s.active ()->doc;
        Rect area              = d.bounds ();
        size_t count = px.size () / 4, w = area.width ();
        std::vector<float> mask (count, 0.0f);
        const char* hues[6] = {"reds", "yellows", "greens", "cyans", "blues", "magentas"};
        int hue_index       = -1;
        for (int i = 0; i < 6; i++)
        {
            if (preset == hues[i])
            {
                hue_index = i;
            }
        }
        if (preset == "sampledColors")
        {
            /* Each pixel takes its best match over the samples: the colour param, colours, and the
               pixels under "points"; "subtractPoints" remove theirs. */
            std::vector<float> plus, minus;
            float fg[4];
            const Value* c      = param (p, "color");
            const Value* colors = param (p, "colors");
            const Value* points = param (p, "points");
            const Value* sub    = param (p, "subtractPoints");
            if ((c != NULL && c->is_string ()) || (colors == NULL && points == NULL))
            {
                float hex[4];
                std::string cs;
                if (param_str (p, "color", &cs) && parse_hex (cs.c_str (), hex))
                {
                    memcpy (fg, hex, sizeof fg);
                }
                else
                {
                    memcpy (fg, s.tools.foreground, sizeof fg);
                }
                plus.insert (plus.end (), fg, fg + 3);
            }
            if (colors != NULL && colors->is_array ())
            {
                for (size_t i = 0; i < colors->size (); i++)
                {
                    float hex[4];
                    if ((*colors)[i].is_string ()
                        && parse_hex ((*colors)[i].get_ref<const std::string&> ().c_str (), hex))
                    {
                        plus.insert (plus.end (), hex, hex + 3);
                    }
                }
            }
            for (int list = 0; list < 2; list++)
            {
                const Value* ps = list == 0 ? points : sub;
                if (ps == NULL || !ps->is_array ())
                {
                    continue;
                }
                for (size_t i = 0; i < ps->size (); i++)
                {
                    const Value& q = (*ps)[i];
                    if (!q.is_array () || q.size () < 2 || !q[0].is_number () || !q[1].is_number ())
                    {
                        return bad_params (err, cmd, "points must be [x, y] pairs");
                    }
                    int32_t x = (int32_t) floor (q[0].get<double> ()), y = (int32_t) floor (q[1].get<double> ());
                    if (x < area.x0 || y < area.y0 || x >= area.x1 || y >= area.y1)
                    {
                        return bad_params (err, cmd, "point (%d, %d) is outside the canvas", x, y);
                    }
                    const float* pp = &px[((size_t) (y - area.y0) * w + (size_t) (x - area.x0)) * 4];
                    (list == 0 ? plus : minus).insert (list == 0 ? plus.end () : minus.end (), pp, pp + 3);
                }
            }
            float f = fmaxf (num_or (p, "fuzziness", 40.0f), 0.0f) + 1.0f;
#pragma omp parallel for schedule(static)
            for (ptrdiff_t ii = 0; ii < (ptrdiff_t) count; ii++)
            {
                size_t i       = (size_t) ii;
                const float* q = &px[i * 4];
                if (q[3] <= 0.0f)
                {
                    continue;
                }
                float best = 0.0f, worst = 0.0f;
                for (size_t k = 0; k < plus.size (); k += 3)
                {
                    float dd =
                        fmaxf (fabsf (q[0] - plus[k]), fmaxf (fabsf (q[1] - plus[k + 1]), fabsf (q[2] - plus[k + 2])))
                        * 255.0f;
                    best = fmaxf (best, clamp01 (1.0f - dd / f));
                }
                for (size_t k = 0; k < minus.size (); k += 3)
                {
                    float dd = fmaxf (fabsf (q[0] - minus[k]),
                                   fmaxf (fabsf (q[1] - minus[k + 1]), fabsf (q[2] - minus[k + 2])))
                             * 255.0f;
                    worst = fmaxf (worst, clamp01 (1.0f - dd / f));
                }
                mask[i] = clamp01 (best - worst);
            }
        }
        else if (hue_index >= 0)
        {
            /* A hue family: full at its hue, none 60° away, times chroma (neutrals never). */
            float center = 60.0f * (float) hue_index;
            for (size_t i = 0; i < count; i++)
            {
                const float* q = &px[i * 4];
                if (q[3] <= 0.0f)
                {
                    continue;
                }
                float rgb[3] = {clamp01 (q[0]), clamp01 (q[1]), clamp01 (q[2])}, h, chroma;
                hue_chroma (rgb, &h, &chroma);
                float dh = fmodf (h - center + 360.0f, 360.0f);
                dh       = fminf (dh, 360.0f - dh);
                mask[i]  = clamp01 (clamp01 (1.0f - dh / 60.0f) * chroma);
            }
        }
        else if (preset == "highlights" || preset == "midtones" || preset == "shadows")
        {
            float lo        = preset == "shadows" ? 0.0f : preset == "highlights" ? 190.0f : 105.0f;
            float hi        = preset == "shadows" ? 65.0f : preset == "highlights" ? 255.0f : 150.0f;
            const Value* tr = param (p, "tonalRange");
            if (tr != NULL)
            {
                if (preset == "midtones")
                {
                    if (!tr->is_array () || tr->size () != 2 || !(*tr)[0].is_number () || !(*tr)[1].is_number ())
                    {
                        return bad_params (err, cmd, "tonalRange: want [low, high] levels 0..255");
                    }
                    lo = (*tr)[0].get<float> ();
                    hi = (*tr)[1].get<float> ();
                }
                else
                {
                    if (!tr->is_number ())
                    {
                        return bad_params (err, cmd, "tonalRange: want a level 0..255");
                    }
                    (preset == "shadows" ? hi : lo) = tr->get<float> ();
                }
                if (lo < 0.0f || hi > 255.0f || lo > hi)
                {
                    return bad_params (err, cmd, "tonalRange: want levels 0..255, low <= high");
                }
            }
            float ramp = fmaxf (fminf (fmaxf (num_or (p, "fuzziness", 20.0f), 0.0f), 100.0f) / 100.0f * 255.0f, 1e-3f);
            for (size_t i = 0; i < count; i++)
            {
                const float* q = &px[i * 4];
                if (q[3] <= 0.0f)
                {
                    continue;
                }
                float l   = clamp01 (0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2]) * 255.0f;
                float out = l < lo ? lo - l : l > hi ? l - hi : 0.0f;
                mask[i]   = clamp01 (1.0f - out / ramp);
            }
        }
        else
        {
            return bad_params (err, cmd,
                "unknown select `%s` (sampledColors, reds, yellows, greens, cyans, blues, magentas, highlights, "
                "midtones, shadows)",
                preset.c_str ());
        }
        if (flag_or (p, "invert", false))
        {
            for (size_t i = 0; i < count; i++)
            {
                mask[i] = 1.0f - mask[i];
            }
        }
        if (mode == "replace" && d.has_selection)
        {
            std::vector<float> prev = algo::mask_from_surface (&d.selection, area);
            for (size_t i = 0; i < count; i++)
            {
                mask[i] = fminf (mask[i], prev[i]);
            }
        }
        return set_mask_selection (s, "Color Range", mask, mode, result, err);
    }

    /* Whether any pixel on the outermost rows or columns is selected. */
    bool touches_edge (const std::vector<float>& m, size_t w, size_t h)
    {
        for (size_t x = 0; x < w; x++)
        {
            if (m[x] > 0.0f || m[(h - 1) * w + x] > 0.0f)
            {
                return true;
            }
        }
        for (size_t y = 0; y < h; y++)
        {
            if (m[y * w] > 0.0f || m[y * w + w - 1] > 0.0f)
            {
                return true;
            }
        }
        return false;
    }

    /* Select > Modify. "Apply effect at canvas bounds" off: the selection continues past the canvas
       (edge pixels repeat outwards); on, the canvas edge is a selection edge. Border always follows
       the canvas edge. */
    bool modify (Session& s, const Value& p, const char* op, Value* result, Error* err)
    {
        float mn = 1.0f, mx = 500.0f;
        if (strcmp (op, "border") == 0)
        {
            mx = 200.0f;
        }
        if (strcmp (op, "feather") == 0)
        {
            mn = 0.1f;
            mx = 1000.0f;
        }
        float r         = num_or (p, "radius", 1.0f);
        std::string cmd = std::string ("select.modify.") + op;
        if (!(r >= mn && r <= mx))
        {
            return bad_params (err, cmd.c_str (), "radius must be a number in %g..%g", mn, mx);
        }
        bool at_bounds         = strcmp (op, "border") == 0 || flag_or (p, "applyAtCanvasBounds", false);
        const doc::Document& d = *s.active ()->doc;
        Rect area              = d.bounds ();
        size_t w = area.width (), h = area.height ();
        std::vector<float> m = algo::mask_from_surface (&d.selection, area);
        float reach          = strcmp (op, "feather") == 0 ? r * 3.0f : r;
        size_t pad           = touches_edge (m, w, h) ? (size_t) ceilf (reach) + 2 : 0;
        size_t pw = w + 2 * pad, ph = h + 2 * pad;
        if ((double) pw * (double) ph > (double) (1u << 30))
        {
            return bad_params (err, cmd.c_str (), "the selection is too large to modify");
        }
        std::vector<float> padded;
        if (pad == 0)
        {
            padded = m;
        }
        else
        {
            padded.assign (pw * ph, 0.0f);
            for (size_t py = 0; py < ph; py++)
            {
                if (at_bounds && (py < pad || py >= pad + h))
                {
                    continue;
                }
                size_t y   = py < pad ? 0 : py - pad >= h ? h - 1 : py - pad;
                float* row = &padded[py * pw];
                memcpy (row + pad, &m[y * w], w * sizeof (float));
                if (!at_bounds)
                {
                    for (size_t x = 0; x < pad; x++)
                    {
                        row[x]           = m[y * w];
                        row[pad + w + x] = m[y * w + w - 1];
                    }
                }
            }
        }
        std::vector<float> out;
        const char* label;
        if (strcmp (op, "expand") == 0)
        {
            out   = algo::expand_mask (padded, pw, ph, r);
            label = "Expand Selection";
        }
        else if (strcmp (op, "contract") == 0)
        {
            out   = algo::contract_mask (padded, pw, ph, r);
            label = "Contract Selection";
        }
        else if (strcmp (op, "border") == 0)
        {
            out   = algo::border_mask (padded, pw, ph, r);
            label = "Border Selection";
        }
        else if (strcmp (op, "smooth") == 0)
        {
            out   = algo::smooth_mask (padded, pw, ph, r);
            label = "Smooth Selection";
        }
        else
        {
            out   = algo::feather_mask (padded, pw, ph, r);
            label = "Feather Selection";
        }
        if (pad != 0)
        {
            std::vector<float> cropped (w * h);
            for (size_t y = 0; y < h; y++)
            {
                memcpy (&cropped[y * w], &out[(y + pad) * pw + pad], w * sizeof (float));
            }
            out = cropped;
        }
        return set_mask_selection (s, label, out, "replace", result, err);
    }

    bool cmd_modify_border (Session& s, const Value& p, Value* result, Error* err)
    {
        return modify (s, p, "border", result, err);
    }

    bool cmd_modify_smooth (Session& s, const Value& p, Value* result, Error* err)
    {
        return modify (s, p, "smooth", result, err);
    }

    bool cmd_modify_expand (Session& s, const Value& p, Value* result, Error* err)
    {
        return modify (s, p, "expand", result, err);
    }

    bool cmd_modify_contract (Session& s, const Value& p, Value* result, Error* err)
    {
        return modify (s, p, "contract", result, err);
    }

    bool cmd_modify_feather (Session& s, const Value& p, Value* result, Error* err)
    {
        return modify (s, p, "feather", result, err);
    }

    /* Grow (contiguous) and Similar (anywhere): pixels within the selected pixels' colour range
       widened by the tolerance. */
    bool grow_similar (Session& s, const Value& p, bool contiguous, Value* result, Error* err)
    {
        float tol = num_or (p, "tolerance", 32.0f) / 255.0f;
        std::vector<float> px;
        if (!sample_pixels (s, flag_or (p, "sampleAllLayers", false), &px, err))
        {
            return false;
        }
        const doc::Document& d = *s.active ()->doc;
        Rect area              = d.bounds ();
        size_t w = area.width (), h = area.height (), n = w * h;
        std::vector<float> m = algo::mask_from_surface (&d.selection, area);
        float lo[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, hi[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
        for (size_t i = 0; i < n; i++)
        {
            if (m[i] < 0.5f)
            {
                continue;
            }
            for (int c = 0; c < 4; c++)
            {
                lo[c] = fminf (lo[c], px[i * 4 + c]);
                hi[c] = fmaxf (hi[c], px[i * 4 + c]);
            }
        }
        if (lo[0] > hi[0])
        {
            return fail (err, ErrorKind::Other, "selection is empty");
        }
        std::vector<float> out (n);
        for (size_t i = 0; i < n; i++)
        {
            out[i] = m[i] >= 0.5f ? 1.0f : 0.0f;
        }
        std::vector<uint8_t> inside (n);
        for (size_t i = 0; i < n; i++)
        {
            bool ok = true;
            for (int c = 0; c < 4 && ok; c++)
            {
                ok = px[i * 4 + c] >= lo[c] - tol - 1e-6f && px[i * 4 + c] <= hi[c] + tol + 1e-6f;
            }
            inside[i] = ok;
        }
        if (contiguous)
        {
            std::vector<size_t> stack;
            for (size_t i = 0; i < n; i++)
            {
                if (out[i] > 0.0f)
                {
                    stack.push_back (i);
                }
            }
            while (!stack.empty ())
            {
                size_t i = stack.back ();
                stack.pop_back ();
                size_t x = i % w, y = i / w;
                size_t nb[4];
                int k = 0;
                if (x > 0)
                {
                    nb[k++] = i - 1;
                }
                if (x + 1 < w)
                {
                    nb[k++] = i + 1;
                }
                if (y > 0)
                {
                    nb[k++] = i - w;
                }
                if (y + 1 < h)
                {
                    nb[k++] = i + w;
                }
                for (int j = 0; j < k; j++)
                {
                    if (out[nb[j]] == 0.0f && inside[nb[j]])
                    {
                        out[nb[j]] = 1.0f;
                        stack.push_back (nb[j]);
                    }
                }
            }
        }
        else
        {
            for (size_t i = 0; i < n; i++)
            {
                if (inside[i])
                {
                    out[i] = 1.0f;
                }
            }
        }
        return set_mask_selection (s, contiguous ? "Grow" : "Similar", out, "replace", result, err);
    }

    bool cmd_grow (Session& s, const Value& p, Value* result, Error* err)
    {
        return grow_similar (s, p, true, result, err);
    }

    bool cmd_similar (Session& s, const Value& p, Value* result, Error* err)
    {
        return grow_similar (s, p, false, result, err);
    }

    /* The most recent selection in the undo history, while there is none now. */
    bool reselect_target (const Session& s, raster::Surface* out)
    {
        const DocState* d = s.active ();
        if (d == NULL || d->doc->has_selection)
        {
            return false;
        }
        for (size_t i = d->history.past_len (); i-- > 0;)
        {
            ops::DocRef st = d->history.state (i);
            if (st && st->has_selection)
            {
                *out = st->selection;
                return true;
            }
        }
        return false;
    }

    bool can_reselect (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        raster::Surface sel;
        if (reselect_target (s, &sel))
        {
            return true;
        }
        *why = "there is no selection to restore";
        return false;
    }

    bool cmd_reselect (Session& s, const Value& p, Value* result, Error* err)
    {
        raster::Surface sel;
        if (!reselect_target (s, &sel))
        {
            return fail (err, ErrorKind::Other, "there is no selection to restore");
        }
        Edit e (s);
        e.doc ()->has_selection = true;
        e.doc ()->selection     = sel;
        if (!e.commit ("Reselect", err))
        {
            return false;
        }
        *result = Value{{"selected", true}};
        return true;
    }

    /* Where Quick Selection reads colours: the active layer, or the visible composite. */
    struct QuickSource
    {
        const doc::Document* doc;
        const raster::Surface* layer;
        const color::CmykSpace* cmyk;
    };

    void quick_read (void* user, Rect r, float* out)
    {
        const QuickSource* q = (const QuickSource*) user;
        if (q->layer != NULL)
        {
            q->layer->read_rgba (r, q->cmyk, out);
            return;
        }
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = q->cmyk;
        compose::Buffer b                = compose::render (*q->doc, r, settings);
        memcpy (out, b.px.data (), b.px.size () * sizeof (float));
    }

    /* Quick Selection as it's dragged, as in Photoshop: the selection grows while the stroke is
       painted. Each update cuts only the newest part of the stroke, in a small window, with what
       the stroke already selected held selected; the parts are combined. On release select.quick
       with "progressive" commits the stroke as one history step. */
    bool quick_stroke_begin (Session& s, float size, const char* mode, bool all_layers, Error* err)
    {
        const DocState* d = s.active ();
        if (d == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        QuickStroke& q = s.quick;
        q.active       = true;
        q.doc          = d->doc->id;
        q.revision     = d->revision;
        q.size         = fminf (fmaxf (size, 1.0f), 5000.0f);
        q.all_layers   = all_layers;
        q.mode         = mode;
        q.points.clear ();
        q.done    = 0;
        q.region  = raster::Surface (color::PixelFormat::gray8 ());
        q.preview = d->doc->has_selection && q.mode != "replace" ? d->doc->selection
                                                                 : raster::Surface (color::PixelFormat::gray8 ());
        return true;
    }

    void quick_stroke_cancel (Session& s)
    {
        s.quick.active = false;
        s.quick.points.clear ();
        s.quick.region  = raster::Surface (color::PixelFormat::gray8 ());
        s.quick.preview = raster::Surface (color::PixelFormat::gray8 ());
    }

    /* More of the stroke: the new points (with the last one already cut, so the segments join)
       are cut and added to what the stroke selected; `preview` follows. */
    bool quick_stroke_extend (Session& s, const std::vector<float>& points, Error* err)
    {
        QuickStroke& q    = s.quick;
        const DocState* d = s.active ();
        if (!q.active)
        {
            return fail (err, ErrorKind::Other, "no Quick Selection stroke");
        }
        if (d == NULL || d->doc->id != q.doc || d->revision != q.revision)
        {
            quick_stroke_cancel (s);
            return fail (err, ErrorKind::Other, "the document changed during the stroke");
        }
        q.points.insert (q.points.end (), points.begin (), points.end ());
        size_t total = q.points.size () / 2;
        if (total == 0 || q.done >= total)
        {
            return true;
        }
        std::vector<algo::QuickPoint> seg, all;
        for (size_t i = 0; i < total; i++)
        {
            algo::QuickPoint pt = {q.points[i * 2], q.points[i * 2 + 1]};
            all.push_back (pt);
            if (i + 1 >= q.done)
            {
                seg.push_back (pt);
            }
        }
        const doc::Layer* l = d->doc->layer (d->active_layer);
        QuickSource src     = {d->doc.get (), NULL, s.cmyk};
        if (!q.all_layers && l != NULL && l->surface () != NULL)
        {
            src.layer = l->surface ();
        }
        algo::RgbaSource source = {quick_read, &src};
        algo::Region region;
        algo::QuickStats stats;
        double t0 = timing_now ();
        /* Three windows at most (four times the first one's reach): what the stroke selected
           reaches across the window's sides, so new pixels beside it would otherwise keep growing
           the window to cover the whole stroke, and updates would slow as the stroke gets longer. */
        bool found = algo::quick_select (source, d->doc->bounds (), seg, q.size, &all, &q.region, 3, &region, &stats);
        q.done     = total;
        if (!found)
        {
            return true;
        }
        Rect area = d->doc->bounds ();
        q.region  = algo::combine_surfaces (q.region, algo::region_surface (region), area, algo::select_max);
        if (q.mode == "subtract")
        {
            const raster::Surface& base = d->doc->has_selection ? d->doc->selection : q.region;
            q.preview = d->doc->has_selection ? algo::combine_surfaces (base, q.region, area, algo::select_subtract)
                                              : raster::Surface (color::PixelFormat::gray8 ());
        }
        else if (q.mode == "add" && d->doc->has_selection)
        {
            q.preview = algo::combine_surfaces (d->doc->selection, q.region, area, algo::select_max);
        }
        else
        {
            q.preview = q.region;
        }
        if (s.timing)
        {
            char what[96];
            snprintf (what, sizeof what, "quick stroke update (%zu pts, %d window(s))", seg.size (), stats.windows);
            timing_log (s, what, timing_now () - t0);
        }
        return true;
    }

    /* ---- Select and Mask ---- */

    /* The refinement settings in Photoshop's units. */
    algo::RefineParams refine_params (const Value& p)
    {
        algo::RefineParams r = algo::refine_defaults ();
        r.radius             = fminf (fmaxf (num_or (p, "radius", 0.0f), 0.0f), 250.0f);
        r.smart_radius       = flag_or (p, "smartRadius", false);
        r.smooth             = fminf (fmaxf (num_or (p, "smooth", 0.0f), 0.0f), 100.0f);
        r.feather            = fminf (fmaxf (num_or (p, "feather", 0.0f), 0.0f), 1000.0f);
        r.contrast           = fminf (fmaxf (num_or (p, "contrast", 0.0f), 0.0f), 100.0f);
        r.shift_edge         = fminf (fmaxf (num_or (p, "shiftEdge", 0.0f), -100.0f), 100.0f);
        return r;
    }

    /* The Refine Edge Brush strokes: [{"points":[[x,y],…],"size":px,"mode":"add|subtract"}], in
       painting order. */
    bool refine_strokes (const Value& p, std::vector<algo::EdgeStroke>* out, Error* err)
    {
        const char* cmd = "select.selectAndMask";
        const Value* v  = param (p, "refineStrokes");
        if (v == NULL || v->is_null ())
        {
            return true;
        }
        if (!v->is_array ())
        {
            return bad_params (err, cmd, "refineStrokes must be an array of strokes");
        }
        if (v->size () > 4096)
        {
            return bad_params (err, cmd, "refineStrokes holds more than 4096 strokes");
        }
        size_t total = 0;
        for (size_t i = 0; i < v->size (); i++)
        {
            const Value& st  = (*v)[i];
            const Value* pts = st.is_object () ? param (st, "points") : NULL;
            if (pts == NULL || !pts->is_array () || pts->empty ())
            {
                return bad_params (err, cmd, "each stroke needs a non-empty points array");
            }
            total += pts->size ();
            if (total > 100000)
            {
                return bad_params (err, cmd, "refineStrokes holds more than 100000 points");
            }
            algo::EdgeStroke e;
            for (size_t k = 0; k < pts->size (); k++)
            {
                const Value& q = (*pts)[k];
                if (!q.is_array () || q.size () != 2 || !q[0].is_number () || !q[1].is_number ())
                {
                    return bad_params (err, cmd, "stroke points must be [x, y] pairs");
                }
                float x = q[0].get<float> (), y = q[1].get<float> ();
                if (!isfinite (x) || !isfinite (y) || fabsf (x) > 1e7f || fabsf (y) > 1e7f)
                {
                    return bad_params (err, cmd, "stroke points must be finite");
                }
                e.points.push_back (x);
                e.points.push_back (y);
            }
            e.size = num_or (st, "size", 30.0f);
            if (!(e.size >= algo::EDGE_BRUSH_MIN && e.size <= algo::EDGE_BRUSH_MAX))
            {
                return bad_params (err, cmd, "stroke size must be 1..5000 px");
            }
            std::string mode = "add";
            param_str (st, "mode", &mode);
            if (mode != "add" && mode != "subtract")
            {
                return bad_params (err, cmd, "stroke mode must be add or subtract");
            }
            e.add = mode == "add";
            out->push_back (e);
        }
        return true;
    }

    /* The refined coverage of `d`'s selection (or the active layer's mask without one). False with
       `*empty` when nothing stays selected. */
    bool refine_document (Session& s, const doc::Document& d, doc::LayerId active, const Value& p, algo::Region* out,
        bool* empty, Error* err)
    {
        algo::RefineParams rp = refine_params (p);
        std::vector<algo::EdgeStroke> strokes;
        if (!refine_strokes (p, &strokes, err))
        {
            return false;
        }
        const doc::Layer* l             = d.layer (active);
        const raster::Surface* src_mask = d.has_selection          ? &d.selection
                                        : l != NULL && l->has_mask ? &l->mask.surface
                                                                   : NULL;
        if (src_mask == NULL)
        {
            return fail (err, ErrorKind::Other, "Select and Mask needs a selection or a layer mask");
        }
        algo::EdgeBrush brush;
        bool brushed    = algo::edge_brush_make (strokes, d.bounds (), &brush);
        QuickSource src = {&d, NULL, s.cmyk};
        if (!flag_or (p, "sampleAllLayers", false) && l != NULL && l->surface () != NULL)
        {
            src.layer = l->surface ();
        }
        algo::RgbaSource source = {quick_read, &src};
        *empty                  = !algo::refine_mask (source, *src_mask, d.bounds (), rp, brushed ? &brush : NULL, out);
        return true;
    }

    /* Select and Mask: refines the selection's edge, then outputs to the selection, a layer mask on
       the active layer, a new layer or a new document (with or without a mask). Decontaminating
       colours needs new pixels, so it turns Selection / Layer Mask output into New Layer with Layer
       Mask, as in Photoshop. New layers hide the layer they came from. */
    bool cmd_select_and_mask (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd    = "select.selectAndMask";
        const DocState* ds = s.active ();
        if (ds == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        doc::LayerId active  = ds->active_layer;
        const doc::Layer* al = ds->doc->layer (active);
        std::string output   = !ds->doc->has_selection && al != NULL && al->has_mask ? "layerMask" : "selection";
        param_str (p, "output", &output);
        const char* const OUTPUTS[6] = {
            "selection", "layerMask", "newLayer", "newLayerWithMask", "newDocument", "newDocumentWithMask"};
        bool known = false;
        for (int i = 0; i < 6; i++)
        {
            known = known || output == OUTPUTS[i];
        }
        if (!known)
        {
            return bad_params (err, cmd,
                "output must be selection, layerMask, newLayer, newLayerWithMask, newDocument or newDocumentWithMask");
        }
        bool decontam = flag_or (p, "decontaminate", false);
        float amount  = fminf (fmaxf (num_or (p, "amount", 100.0f), 0.0f), 100.0f);
        if (decontam && (output == "selection" || output == "layerMask"))
        {
            output = "newLayerWithMask";
        }
        algo::Region region;
        bool empty = false;
        if (!refine_document (s, *ds->doc, active, p, &region, &empty, err))
        {
            return false;
        }
        raster::Surface coverage =
            empty ? raster::Surface (color::PixelFormat::gray8 ()) : algo::region_surface (region);
        if (output == "selection")
        {
            Edit e (s);
            e.doc ()->has_selection = !empty;
            e.doc ()->selection     = coverage;
            if (!e.commit ("Select and Mask", err))
            {
                return false;
            }
            *result = Value{{"output", output}, {"selected", !empty}};
            return true;
        }
        if (al == NULL)
        {
            return fail (err, ErrorKind::Other, "no active layer");
        }
        if (output == "layerMask")
        {
            Edit e (s);
            doc::Layer* l = e.doc ()->layer_mut (active);
            if (l == NULL)
            {
                return no_layer (err, active);
            }
            l->has_mask             = true;
            l->mask                 = doc::LayerMask::reveal_all ();
            l->mask.surface         = coverage;
            e.doc ()->has_selection = false;
            e.doc ()->selection     = raster::Surface (color::PixelFormat::gray8 ());
            if (!e.commit ("Select and Mask", err))
            {
                return false;
            }
            *result = Value{{"output", output}, {"layer", active}, {"selected", false}};
            return true;
        }
        const raster::Surface* pixels = al->surface ();
        if (pixels == NULL)
        {
            return fail (err, ErrorKind::Other, "the active layer has no pixels to copy");
        }
        algo::Region none;
        const algo::Region& reg = empty ? none : region;
        raster::Surface content =
            decontam ? algo::decontaminate (*pixels, reg, refine_params (p).radius, amount) : *pixels;
        bool with_mask = output == "newLayerWithMask" || output == "newDocumentWithMask";
        if (!with_mask)
        {
            content = algo::masked_copy (content, reg);
        }
        doc::Layer layer = doc::Layer::raster (s.ids, ds->doc->copy_name (al->name), content.format ());
        layer.pixels     = content;
        if (with_mask)
        {
            layer.has_mask     = true;
            layer.mask         = doc::LayerMask::reveal_all ();
            layer.mask.surface = coverage;
        }
        if (output == "newDocument" || output == "newDocumentWithMask")
        {
            /* Same canvas, mode and depth, so the pixels need no conversion. */
            const doc::Document& like = *ds->doc;
            doc::Document nd          = doc::Document::make (s.ids, "Untitled", like.size, like.mode, like.depth);
            nd.resolution_dpi         = like.resolution_dpi;
            nd.icc_profile            = like.icc_profile;
            layer.name                = al->name;
            nd.layers.push_back (layer);
            size_t index = s.add_document (nd, "");
            *result      = Value{{"output", output}, {"document", index}, {"selected", false}};
            return true;
        }
        Edit e (s);
        doc::LayerId nid      = e.doc ()->insert_above (active, layer);
        doc::Layer* src_layer = e.doc ()->layer_mut (active);
        if (src_layer != NULL)
        {
            src_layer->visible = false;
        }
        e.doc ()->has_selection = false;
        e.doc ()->selection     = raster::Surface (color::PixelFormat::gray8 ());
        *e.active_layer ()      = nid;
        if (!e.commit ("Select and Mask", err))
        {
            return false;
        }
        *result = Value{{"output", output}, {"layer", nid}, {"selected", false}};
        return true;
    }

    /* The refined coverage for the Select and Mask preview: a selection surface (empty when nothing
       stays selected), from `d` as it is. */
    bool select_and_mask_coverage (
        Session& s, const doc::Document& d, doc::LayerId active, const Value& p, raster::Surface* out, Error* err)
    {
        algo::Region region;
        bool empty = false;
        if (!refine_document (s, d, active, p, &region, &empty, err))
        {
            return false;
        }
        *out = empty ? raster::Surface (color::PixelFormat::gray8 ()) : algo::region_surface (region);
        return true;
    }

    /* Select and Mask's Brush: a hard round stroke added to or taken from the selection. */
    bool cmd_brush_stroke (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "select.brushStroke";
        Value strokes   = Value::array ();
        Value one       = p;
        if (!param (one, "mode"))
        {
            one["mode"] = "add";
        }
        strokes.push_back (one);
        std::vector<algo::EdgeStroke> list;
        if (!refine_strokes (Value{{"refineStrokes", strokes}}, &list, err))
        {
            return false;
        }
        if (list.empty () || list[0].points.empty ())
        {
            return bad_params (err, cmd, "needs points");
        }
        bool subtract = !list[0].add;
        list[0].add   = true;
        algo::EdgeBrush b;
        const DocState* ds = s.active ();
        if (ds == NULL)
        {
            return fail (err, ErrorKind::NoDocument, "no active document");
        }
        if (!algo::edge_brush_make (list, ds->doc->bounds (), &b))
        {
            return true;
        }
        std::vector<float> cov (b.v.size ());
        for (size_t i = 0; i < cov.size (); i++)
        {
            cov[i] = (float) b.v[i] / 127.0f;
        }
        raster::Surface shape = algo::mask_to_surface (cov, b.bbox);
        Edit e (s);
        apply_selection (e.doc (), shape, subtract ? "subtract" : "add");
        bool selected = e.doc ()->has_selection;
        if (!e.commit ("Brush", err))
        {
            return false;
        }
        *result = Value{{"selected", selected}};
        return true;
    }

    /* Select and Mask needs a selection, or a layer mask on the active layer. */
    bool can_select_and_mask (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        const DocState* d   = s.active ();
        const doc::Layer* l = d->doc->layer (d->active_layer);
        if (d->doc->has_selection || (l != NULL && l->has_mask))
        {
            return true;
        }
        *why = "make a selection first (or target a layer mask)";
        return false;
    }

    /* A dragged Quick Selection stroke step by step: "begin" (size, mode, sampleAllLayers),
       "extend" (points), "cancel"; select.quick with "progressive" commits it. */
    bool cmd_quick_stroke (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "select.quickStroke";
        std::string phase;
        if (!param_str (p, "phase", &phase))
        {
            return bad_params (err, cmd, "missing `phase` (begin, extend or cancel)");
        }
        if (phase == "begin")
        {
            std::string mode = mode_param (p);
            if (mode != "replace" && mode != "add" && mode != "subtract")
            {
                return bad_params (err, cmd, "mode must be replace, add or subtract");
            }
            return quick_stroke_begin (
                s, num_or (p, "size", 30.0f), mode.c_str (), flag_or (p, "sampleAllLayers", false), err);
        }
        if (phase == "cancel")
        {
            quick_stroke_cancel (s);
            return true;
        }
        if (phase != "extend")
        {
            return bad_params (err, cmd, "unknown phase `%s`", phase.c_str ());
        }
        const Value* pts = param (p, "points");
        std::vector<float> flat;
        for (size_t i = 0; pts != NULL && pts->is_array () && i < pts->size (); i++)
        {
            const Value& q = (*pts)[i];
            if (!q.is_array () || q.size () < 2 || !q[0].is_number () || !q[1].is_number ())
            {
                return bad_params (err, cmd, "points must be [x, y] pairs");
            }
            flat.push_back (q[0].get<float> ());
            flat.push_back (q[1].get<float> ());
        }
        return quick_stroke_extend (s, flat, err);
    }

    /* The Quick Selection tool: a brush stroke grows a selection to the image edges near it (see
       algo/quick_select.cpp), added to the selection (or subtracting with mode "subtract"). */
    bool cmd_quick_select (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd  = "select.quick";
        const Value* pts = param (p, "points");
        std::vector<algo::QuickPoint> points;
        if (pts != NULL && pts->is_array ())
        {
            for (size_t i = 0; i < pts->size (); i++)
            {
                const Value& q = (*pts)[i];
                if (!q.is_array () || q.size () < 2 || !q[0].is_number () || !q[1].is_number ())
                {
                    return bad_params (err, cmd, "points must be [x, y] pairs");
                }
                algo::QuickPoint pt = {q[0].get<float> (), q[1].get<float> ()};
                points.push_back (pt);
            }
        }
        if (points.empty ())
        {
            return bad_params (err, cmd, "needs at least one point");
        }
        float size = num_or (p, "size", 30.0f);
        if (!(size >= 1.0f && size <= 5000.0f))
        {
            return bad_params (err, cmd, "size must be 1..5000");
        }
        std::string mode = mode_param (p);
        if (mode != "replace" && mode != "add" && mode != "subtract")
        {
            return bad_params (err, cmd, "mode must be replace, add or subtract");
        }
        const DocState* ds = s.active ();
        /* A dragged stroke is already cut: commit it. */
        bool progressive = false;
        param_bool (p, "progressive", &progressive);
        QuickStroke& q = s.quick;
        if (progressive && q.active && ds != NULL && ds->doc->id == q.doc && ds->revision == q.revision)
        {
            raster::Surface stroke = q.region;
            quick_stroke_cancel (s);
            Edit e (s);
            doc::Document* d = e.doc ();
            apply_selection (d, stroke, mode);
            bool selected = d->has_selection;
            if (!e.commit ("Quick Selection", err))
            {
                return false;
            }
            *result = Value{{"selected", selected}};
            return true;
        }
        quick_stroke_cancel (s);
        const doc::Layer* l = ds->doc->layer (ds->active_layer);
        QuickSource src     = {ds->doc.get (), NULL, s.cmyk};
        if (!flag_or (p, "sampleAllLayers", false) && l != NULL && l->surface () != NULL)
        {
            src.layer = l->surface ();
        }
        algo::RgbaSource source = {quick_read, &src};
        algo::Region region;
        algo::QuickStats stats;
        double t0  = timing_now ();
        bool found = algo::quick_select (source, ds->doc->bounds (), points, size, NULL, NULL, 0, &region, &stats);
        double t1  = timing_now ();
        Edit e (s);
        doc::Document* d      = e.doc ();
        raster::Surface shape = found ? algo::region_surface (region) : raster::Surface (color::PixelFormat::gray8 ());
        double t2             = timing_now ();
        apply_selection (d, shape, mode);
        double t3     = timing_now ();
        bool selected = d->has_selection;
        if (!e.commit ("Quick Selection", err))
        {
            return false;
        }
        double t4 = timing_now ();
        if (s.timing)
        {
            char what[96];
            snprintf (what, sizeof what, "  quick: %d window(s), last %zu px", stats.windows, stats.pixels);
            timing_log (s, what, t1 - t0);
            timing_log (s, "    read pixels", stats.read);
            timing_log (s, "    stroke seeds", stats.seeds);
            timing_log (s, "    min cut (all windows)", stats.cut);
            timing_log (s, "    upsample + trim", stats.upsample);
            timing_log (s, "  region to surface", t2 - t1);
            timing_log (s, "  combine with selection", t3 - t2);
            timing_log (s, "  commit (history)", t4 - t3);
        }
        *result = Value{{"selected", selected}};
        return true;
    }

    const CommandSpec SELECT_COMMANDS[] = {
        {"select.selectAndMask", "Select and Mask…", "Select", "Cmd+Alt+R",
            "{\"radius\":0..250=0,\"smartRadius\":bool=false,\"smooth\":0..100=0,\"feather\":0..1000=0,\"contrast\":0.."
            "100=0,\"shiftEdge\":-100..100=0,"
            "\"refineStrokes\":[{\"points\":[[x,y],…],\"size\":px,\"mode\":\"add|subtract\"}]?,\"decontaminate\":bool="
            "false,\"amount\":0..100=100,"
            "\"output\":\"selection|layerMask|newLayer|newLayerWithMask|newDocument|newDocumentWithMask\","
            "\"sampleAllLayers\":bool=false}",
            can_select_and_mask, cmd_select_and_mask, true},
        {"select.brushStroke", "Select and Mask Brush", "", NULL,
            "{\"points\":[[x,y],…],\"size\":px=30,\"mode\":\"add|subtract\"=\"add\"}", has_doc, cmd_brush_stroke, true},
        {"select.quickStroke", "Quick Selection Stroke", "", NULL,
            "{\"phase\":\"begin|extend|cancel\",\"points\":[[x,y],…]?,\"size\":px=30,\"mode\":\"replace|add|"
            "subtract\"}",
            has_doc, cmd_quick_stroke, false},
        {"select.quick", "Quick Selection", "", NULL,
            "{\"points\":[[x,y],…],\"size\":px=30,\"mode\":\"replace|add|subtract\"=\"replace\",\"sampleAllLayers\":"
            "bool=false}",
            has_doc, cmd_quick_select, true},
        {"select.reselect", "Reselect", "Select", "Cmd+Shift+D", "{}", can_reselect, cmd_reselect, true},
        {"select.colorRange", "Color Range…", "Select", NULL,
            "{\"select\":\"sampledColors|reds|yellows|greens|cyans|blues|magentas|highlights|midtones|shadows\"="
            "\"sampledColors\",\"color\":\"#rrggbb\"=foreground,"
            "\"colors\":[\"#rrggbb\",…]?,\"points\":[[x,y],…]?,\"subtractPoints\":[[x,y],…]?,\"fuzziness\":0..200=40 "
            "(tones: 0..100 %=20),\"tonalRange\":level|[lo,hi]?,"
            "\"invert\":bool=false,\"sampleAllLayers\":bool=true,\"mode\":\"replace|add|subtract|intersect\"="
            "\"replace\"}",
            has_doc, cmd_color_range, true},
        {"select.modify.border", "Border…", "Select/Modify", NULL, "{\"radius\":1..200=1}", has_selection,
            cmd_modify_border, true},
        {"select.modify.smooth", "Smooth…", "Select/Modify", NULL,
            "{\"radius\":1..500=1,\"applyAtCanvasBounds\":bool=false}", has_selection, cmd_modify_smooth, true},
        {"select.modify.expand", "Expand…", "Select/Modify", NULL,
            "{\"radius\":1..500=1,\"applyAtCanvasBounds\":bool=false}", has_selection, cmd_modify_expand, true},
        {"select.modify.contract", "Contract…", "Select/Modify", NULL,
            "{\"radius\":1..500=1,\"applyAtCanvasBounds\":bool=false}", has_selection, cmd_modify_contract, true},
        {"select.modify.feather", "Feather…", "Select/Modify", "Shift+F6",
            "{\"radius\":0.1..1000=1,\"applyAtCanvasBounds\":bool=false}", has_selection, cmd_modify_feather, true},
        {"select.grow", "Grow", "Select", NULL, "{\"tolerance\":0..255=32,\"sampleAllLayers\":bool=false}",
            has_selection, cmd_grow, true},
        {"select.similar", "Similar", "Select", NULL, "{\"tolerance\":0..255=32,\"sampleAllLayers\":bool=false}",
            has_selection, cmd_similar, true},
        {"select.magicWand", "Magic Wand", "", NULL,
            "{\"x\":px,\"y\":px,\"tolerance\":0..255=32,\"contiguous\":bool=true,\"antiAlias\":bool=true,"
            "\"sampleAllLayers\":bool=false,"
            "\"mode\":\"replace|add|subtract|intersect\"=\"replace\"}",
            has_doc, cmd_magic_wand, true},
        {"select.lasso", "Lasso", "", NULL,
            "{\"points\":[[x,y],…],\"mode\":\"replace|add|subtract|intersect\"=\"replace\",\"antiAlias\":bool=true,"
            "\"feather\":px=0}",
            has_doc, cmd_lasso, true},
    };

    void register_select_commands (std::vector<CommandSpec>* out)
    {
        out->insert (
            out->end (), SELECT_COMMANDS, SELECT_COMMANDS + sizeof SELECT_COMMANDS / sizeof SELECT_COMMANDS[0]);
    }

} // namespace engine
