#include "vec/vec.h"

#include <ft2build.h>
#include <math.h>
#include <string.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_STROKER_H

namespace vec
{

    /* Rasterizing paths with FreeType's anti-aliased scanline renderer. Paths become FT outlines in
       26.6 fixed point with y flipped (FreeType's y counts up) and cubic tags, so curves are exact;
       dashed strokes go through flattened polylines. Fill coverage follows Photoshop's path model:
       each subpath (with any Join subpaths after it) is filled on its own and folded into the result
       by its operation — combine a+b-ab, subtract a(1-b), intersect ab, exclude a+b-2ab. Strokes
       are FT_Stroker outlines filled nonzero; inside / outside alignment strokes twice the width
       and keeps the part inside / outside the fill. */

    /* FT_Outline counts are 16-bit. */
    const size_t MAX_OUTLINE_POINTS = 32000;

    struct Outline
    {
        std::vector<FT_Vector> pts;
        std::vector<char> tags;
        std::vector<short> contours;
    };

    struct Canvas
    {
        int32_t x0;
        int32_t y0;
        int32_t w;
        int32_t h;
        float* px;
    };

    StrokeParams StrokeParams::make (float width)
    {
        StrokeParams s;
        s.width       = width;
        s.cap         = doc::LineCap::Butt;
        s.join        = doc::LineJoin::Miter;
        s.miter_limit = 4.0f;
        s.align       = doc::StrokeAlign::Center;
        return s;
    }

    FT_Vector ft_point (const double p[2])
    {
        FT_Vector v;
        v.x = (FT_Pos) llround (p[0] * 64.0);
        v.y = (FT_Pos) llround (-p[1] * 64.0);
        return v;
    }

    void push_point (Outline* o, const double p[2], char tag)
    {
        o->pts.push_back (ft_point (p));
        o->tags.push_back (tag);
    }

    bool is_straight (const double p0[2], const double c1[2], const double c2[2], const double p1[2])
    {
        return c1[0] == p0[0] && c1[1] == p0[1] && c2[0] == p1[0] && c2[1] == p1[1];
    }

    /* The subpath as one contour; `close` adds the segment back to the first knot. */
    void add_contour (Outline* o, const doc::Subpath& s, bool close)
    {
        size_t n = s.knots.size ();
        if (n < 2)
        {
            return;
        }
        push_point (o, s.knots[0].anchor, FT_CURVE_TAG_ON);
        size_t segs = close ? n : n - 1;
        for (size_t i = 0; i < segs; i++)
        {
            const doc::Knot &a = s.knots[i], &b = s.knots[(i + 1) % n];
            bool last = i + 1 == n;
            if (!is_straight (a.anchor, a.out_ctrl, b.in_ctrl, b.anchor))
            {
                push_point (o, a.out_ctrl, FT_CURVE_TAG_CUBIC);
                push_point (o, b.in_ctrl, FT_CURVE_TAG_CUBIC);
            }
            /* The closing segment ends on the contour's first point, which FreeType joins
               implicitly. */
            if (!last)
            {
                push_point (o, b.anchor, FT_CURVE_TAG_ON);
            }
        }
        o->contours.push_back ((short) (o->pts.size () - 1));
    }

    void add_polyline (Outline* o, const double* xy, size_t count)
    {
        if (count < 2)
        {
            return;
        }
        for (size_t i = 0; i < count; i++)
        {
            push_point (o, &xy[i * 2], FT_CURVE_TAG_ON);
        }
        o->contours.push_back ((short) (o->pts.size () - 1));
    }

    void view_outline (Outline* o, bool even_odd, FT_Outline* out)
    {
        memset (out, 0, sizeof *out);
        out->n_points   = (short) o->pts.size ();
        out->n_contours = (short) o->contours.size ();
        out->points     = o->pts.data ();
        out->tags       = o->tags.data ();
        out->contours   = o->contours.data ();
        out->flags      = even_odd ? FT_OUTLINE_EVEN_ODD_FILL : FT_OUTLINE_NONE;
    }

    /* Spans union into the canvas; FreeType rows count up, so row = -y - 1. */
    void union_spans (int y, int count, const FT_Span* spans, void* user)
    {
        Canvas* cv  = (Canvas*) user;
        int32_t row = -y - 1 - cv->y0;
        if (row < 0 || row >= cv->h)
        {
            return;
        }
        float* line = cv->px + (size_t) row * (size_t) cv->w;
        for (int i = 0; i < count; i++)
        {
            float c = spans[i].coverage / 255.0f;
            for (int x = spans[i].x; x < spans[i].x + spans[i].len; x++)
            {
                int32_t col = x - cv->x0;
                if (col >= 0 && col < cv->w && line[col] < c)
                {
                    line[col] = c;
                }
            }
        }
    }

    void render_outline (FT_Library lib, FT_Outline* o, geom::Rect r, float* px)
    {
        if (o->n_points == 0 || r.is_empty ())
        {
            return;
        }
        Canvas cv = {r.x0, r.y0, (int32_t) r.width (), (int32_t) r.height (), px};
        FT_Raster_Params params;
        memset (&params, 0, sizeof params);
        params.flags         = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
        params.gray_spans    = union_spans;
        params.user          = &cv;
        params.clip_box.xMin = r.x0;
        params.clip_box.xMax = r.x1;
        params.clip_box.yMin = -r.y1;
        params.clip_box.yMax = -r.y0;
        FT_Outline_Render (lib, o, &params);
    }

    /* Pixel bounds of an outline's control box. */
    geom::Rect outline_rect (const FT_Outline* o)
    {
        geom::Rect r;
        if (o->n_points == 0)
        {
            return r;
        }
        FT_BBox b;
        FT_Outline_Get_CBox (o, &b);
        r.x0 = (int32_t) floor (b.xMin / 64.0) - 1;
        r.x1 = (int32_t) ceil (b.xMax / 64.0) + 1;
        r.y0 = (int32_t) floor (-b.yMax / 64.0) - 1;
        r.y1 = (int32_t) ceil (-b.yMin / 64.0) + 1;
        return r;
    }

    void bezier_point (
        const double p0[2], const double c1[2], const double c2[2], const double p1[2], double t, double out[2])
    {
        double u = 1.0 - t;
        double a = u * u * u, b = 3.0 * u * u * t, c = 3.0 * u * t * t, d = t * t * t;
        out[0] = a * p0[0] + b * c1[0] + c * c2[0] + d * p1[0];
        out[1] = a * p0[1] + b * c1[1] + c * c2[1] + d * p1[1];
    }

    /* The subpath as a polyline (closed subpaths end back on the first point), within `tol` px.
       The step count bounds the curve's distance from its chords by its second differences. */
    void flatten (const doc::Subpath& s, double tol, std::vector<double>* xy)
    {
        xy->clear ();
        size_t n = s.knots.size ();
        if (n == 0)
        {
            return;
        }
        xy->push_back (s.knots[0].anchor[0]);
        xy->push_back (s.knots[0].anchor[1]);
        size_t segs = s.closed ? n : n - 1;
        if (n < 2)
        {
            segs = 0;
        }
        for (size_t i = 0; i < segs; i++)
        {
            const doc::Knot &a = s.knots[i], &b = s.knots[(i + 1) % n];
            int steps = 1;
            if (!is_straight (a.anchor, a.out_ctrl, b.in_ctrl, b.anchor))
            {
                double d0x = a.anchor[0] - 2 * a.out_ctrl[0] + b.in_ctrl[0],
                       d0y = a.anchor[1] - 2 * a.out_ctrl[1] + b.in_ctrl[1];
                double d1x = a.out_ctrl[0] - 2 * b.in_ctrl[0] + b.anchor[0],
                       d1y = a.out_ctrl[1] - 2 * b.in_ctrl[1] + b.anchor[1];
                double dd  = fmax (sqrt (d0x * d0x + d0y * d0y), sqrt (d1x * d1x + d1y * d1y));
                steps      = (int) ceil (sqrt (0.75 * dd / fmax (tol, 1e-3)));
                if (steps < 1)
                {
                    steps = 1;
                }
                if (steps > 1000)
                {
                    steps = 1000;
                }
            }
            for (int k = 1; k <= steps; k++)
            {
                double p[2];
                bezier_point (a.anchor, a.out_ctrl, b.in_ctrl, b.anchor, (double) k / steps, p);
                xy->push_back (p[0]);
                xy->push_back (p[1]);
            }
        }
    }

    bool path_bounds (const doc::Path& p, double out[4])
    {
        return p.control_bounds (out);
    }

    geom::Rect bounds_rect (const double b[4], double pad)
    {
        geom::Rect r;
        r.x0 = (int32_t) fmax (floor (b[0] - pad) - 1.0, -1e9);
        r.y0 = (int32_t) fmax (floor (b[1] - pad) - 1.0, -1e9);
        r.x1 = (int32_t) fmin (ceil (b[2] + pad) + 1.0, 1e9);
        r.y1 = (int32_t) fmin (ceil (b[3] + pad) + 1.0, 1e9);
        return r;
    }

    bool alloc_coverage (geom::Rect r, Coverage* out)
    {
        out->rect = r;
        out->a.clear ();
        if (r.is_empty ())
        {
            return true;
        }
        if ((uint64_t) r.width () * r.height () > ((uint64_t) 1 << 30))
        {
            return false;
        }
        out->a.assign ((size_t) r.width () * r.height (), 0.0f);
        return true;
    }

    float fold (doc::PathOp op, float a, float b)
    {
        switch (op)
        {
        case doc::PathOp::Subtract:
            return a * (1.0f - b);
        case doc::PathOp::Intersect:
            return a * b;
        case doc::PathOp::Exclude:
            return a + b - 2.0f * a * b;
        default:
            return a + b - a * b;
        }
    }

    /* Renders the subpaths [first, last) as one fill, chunked to FreeType's point limit. */
    void fill_group (FT_Library lib, const doc::Path& p, size_t first, size_t last, geom::Rect r, float* px)
    {
        Outline o;
        for (size_t i = first; i < last; i++)
        {
            if (o.pts.size () + p.subpaths[i].knots.size () * 3 + 1 > MAX_OUTLINE_POINTS && !o.pts.empty ())
            {
                FT_Outline ft;
                view_outline (&o, p.even_odd, &ft);
                render_outline (lib, &ft, r, px);
                o = Outline ();
            }
            if (p.subpaths[i].knots.size () * 3 + 1 > MAX_OUTLINE_POINTS)
            {
                continue;
            }
            add_contour (&o, p.subpaths[i], true);
        }
        FT_Outline ft;
        view_outline (&o, p.even_odd, &ft);
        render_outline (lib, &ft, r, px);
    }

    /* Coverage of the path's fill over its bounds within clip, ignoring `inverted`. */
    bool fill_coverage (const doc::Path& p, geom::Rect clip, Coverage* out)
    {
        double b[4];
        if (!p.control_bounds (b))
        {
            return alloc_coverage (geom::Rect (), out);
        }
        if (!alloc_coverage (bounds_rect (b, 0.0).intersect (clip), out))
        {
            return false;
        }
        if (out->a.empty ())
        {
            return true;
        }
        FT_Library lib;
        if (FT_Init_FreeType (&lib) != 0)
        {
            return false;
        }
        std::vector<float> tmp;
        bool first = true;
        size_t i   = 0;
        while (i < p.subpaths.size ())
        {
            size_t j = i + 1;
            while (j < p.subpaths.size () && p.subpaths[j].op == doc::PathOp::Join)
            {
                j++;
            }
            doc::PathOp op = first ? doc::PathOp::Combine : p.subpaths[i].op;
            if (op == doc::PathOp::Join)
            {
                op = doc::PathOp::Combine;
            }
            /* The group's own box; outside it the group covers nothing. */
            doc::Path g = doc::Path::make ();
            for (size_t k = i; k < j; k++)
            {
                g.subpaths.push_back (p.subpaths[k]);
            }
            double gb[4];
            geom::Rect gr = g.control_bounds (gb) ? bounds_rect (gb, 0.0).intersect (out->rect) : geom::Rect ();
            if (op == doc::PathOp::Intersect)
            {
                /* Everything outside the group's box goes. */
                for (int32_t y = out->rect.y0; y < out->rect.y1; y++)
                {
                    for (int32_t x = out->rect.x0; x < out->rect.x1; x++)
                    {
                        if (!gr.contains (x, y))
                        {
                            out->a[(size_t) (y - out->rect.y0) * out->rect.width () + (size_t) (x - out->rect.x0)] =
                                0.0f;
                        }
                    }
                }
            }
            if (!gr.is_empty ())
            {
                tmp.assign ((size_t) gr.width () * gr.height (), 0.0f);
                fill_group (lib, p, i, j, gr, tmp.data ());
                for (int32_t y = gr.y0; y < gr.y1; y++)
                {
                    float* dst =
                        &out->a[(size_t) (y - out->rect.y0) * out->rect.width () + (size_t) (gr.x0 - out->rect.x0)];
                    const float* src = &tmp[(size_t) (y - gr.y0) * gr.width ()];
                    for (uint32_t x = 0; x < gr.width (); x++)
                    {
                        dst[x] = fold (op, dst[x], src[x]);
                    }
                }
            }
            first = false;
            i     = j;
        }
        FT_Done_FreeType (lib);
        return true;
    }

    FT_Stroker_LineCap ft_cap (doc::LineCap c)
    {
        switch (c)
        {
        case doc::LineCap::Round:
            return FT_STROKER_LINECAP_ROUND;
        case doc::LineCap::Square:
            return FT_STROKER_LINECAP_SQUARE;
        default:
            return FT_STROKER_LINECAP_BUTT;
        }
    }

    FT_Stroker_LineJoin ft_join (doc::LineJoin j)
    {
        switch (j)
        {
        case doc::LineJoin::Round:
            return FT_STROKER_LINEJOIN_ROUND;
        case doc::LineJoin::Bevel:
            return FT_STROKER_LINEJOIN_BEVEL;
        default:
            return FT_STROKER_LINEJOIN_MITER_FIXED;
        }
    }

    /* Cuts a polyline into dashes: `pattern` alternates on / off lengths (px). */
    void dash_polyline (const std::vector<double>& xy, const std::vector<double>& pattern, Outline* o)
    {
        size_t n = xy.size () / 2;
        if (n < 2 || pattern.empty ())
        {
            return;
        }
        double total = 0.0;
        for (size_t i = 0; i < pattern.size (); i++)
        {
            total += pattern[i];
        }
        if (total <= 0.0)
        {
            return;
        }
        size_t pi   = 0;
        double left = pattern[0];
        bool on     = true;
        std::vector<double> cur;
        cur.push_back (xy[0]);
        cur.push_back (xy[1]);
        for (size_t i = 1; i < n; i++)
        {
            double ax = xy[(i - 1) * 2], ay = xy[(i - 1) * 2 + 1], bx = xy[i * 2], by = xy[i * 2 + 1];
            double len = hypot (bx - ax, by - ay), at = 0.0;
            while (len - at > left)
            {
                at += left;
                double t = at / len, px = ax + (bx - ax) * t, py = ay + (by - ay) * t;
                if (on)
                {
                    cur.push_back (px);
                    cur.push_back (py);
                    if (o->pts.size () + cur.size () / 2 < MAX_OUTLINE_POINTS)
                    {
                        add_polyline (o, cur.data (), cur.size () / 2);
                    }
                }
                cur.clear ();
                cur.push_back (px);
                cur.push_back (py);
                on   = !on;
                pi   = (pi + 1) % pattern.size ();
                left = pattern[pi];
            }
            left -= len - at;
            cur.push_back (bx);
            cur.push_back (by);
        }
        if (on && o->pts.size () + cur.size () / 2 < MAX_OUTLINE_POINTS)
        {
            add_polyline (o, cur.data (), cur.size () / 2);
        }
    }

    /* Strokes the outline into an exported FT outline appended to `outs`. */
    void stroke_outline (FT_Library lib, FT_Stroker st, Outline* o, bool opened, std::vector<FT_Outline>* outs)
    {
        if (o->pts.empty ())
        {
            return;
        }
        FT_Outline src;
        view_outline (o, false, &src);
        if (FT_Stroker_ParseOutline (st, &src, opened) != 0)
        {
            return;
        }
        FT_UInt np, nc;
        if (FT_Stroker_GetCounts (st, &np, &nc) != 0 || np == 0)
        {
            return;
        }
        FT_Outline out;
        if (FT_Outline_New (lib, np, (FT_Int) nc, &out) != 0)
        {
            return;
        }
        out.n_points   = 0;
        out.n_contours = 0;
        FT_Stroker_Export (st, &out);
        outs->push_back (out);
    }

    /* The stroke at `width` px (center aligned). */
    bool center_stroke (const doc::Path& p, const StrokeParams& sp, float width, geom::Rect clip, Coverage* out)
    {
        alloc_coverage (geom::Rect (), out);
        if (width <= 0.0f)
        {
            return true;
        }
        FT_Library lib;
        if (FT_Init_FreeType (&lib) != 0)
        {
            return false;
        }
        FT_Stroker st;
        if (FT_Stroker_New (lib, &st) != 0)
        {
            FT_Done_FreeType (lib);
            return false;
        }
        FT_Stroker_Set (st, (FT_Fixed) lround (width * 32.0), ft_cap (sp.cap), ft_join (sp.join),
            (FT_Fixed) lround (fmax (sp.miter_limit, 1.0f) * 65536.0));
        std::vector<FT_Outline> outs;
        bool dashed = false;
        for (size_t i = 0; i < sp.dashes.size (); i++)
        {
            if (sp.dashes[i] > 0.0f)
            {
                dashed = true;
            }
        }
        if (dashed)
        {
            /* Dash lengths are in stroke widths, as in Photoshop; an odd list repeats. */
            std::vector<double> pattern;
            for (size_t r = 0; r < (sp.dashes.size () % 2 ? 2u : 1u); r++)
            {
                for (size_t i = 0; i < sp.dashes.size (); i++)
                {
                    pattern.push_back (fmax (sp.dashes[i], 0.0f) * width);
                }
            }
            Outline o;
            std::vector<double> xy;
            for (size_t i = 0; i < p.subpaths.size (); i++)
            {
                flatten (p.subpaths[i], 0.1, &xy);
                dash_polyline (xy, pattern, &o);
            }
            stroke_outline (lib, st, &o, true, &outs);
        }
        else
        {
            Outline closed, open;
            for (size_t i = 0; i < p.subpaths.size (); i++)
            {
                const doc::Subpath& s = p.subpaths[i];
                Outline* o            = s.closed ? &closed : &open;
                if (o->pts.size () + s.knots.size () * 3 + 1 > MAX_OUTLINE_POINTS)
                {
                    stroke_outline (lib, st, o, !s.closed, &outs);
                    *o = Outline ();
                }
                add_contour (o, s, s.closed);
            }
            stroke_outline (lib, st, &closed, false, &outs);
            stroke_outline (lib, st, &open, true, &outs);
        }
        geom::Rect r;
        for (size_t i = 0; i < outs.size (); i++)
        {
            r = r.unite (outline_rect (&outs[i]));
        }
        bool ok = alloc_coverage (r.intersect (clip), out);
        if (ok)
        {
            for (size_t i = 0; i < outs.size (); i++)
            {
                render_outline (lib, &outs[i], out->rect, out->a.data ());
            }
        }
        for (size_t i = 0; i < outs.size (); i++)
        {
            FT_Outline_Done (lib, &outs[i]);
        }
        FT_Stroker_Done (st);
        FT_Done_FreeType (lib);
        return ok;
    }

    float coverage_at (const Coverage& c, int32_t x, int32_t y)
    {
        if (!c.rect.contains (x, y))
        {
            return 0.0f;
        }
        return c.a[(size_t) (y - c.rect.y0) * c.rect.width () + (size_t) (x - c.rect.x0)];
    }

    bool stroke_coverage (const doc::Path& p, const StrokeParams& sp, geom::Rect clip, Coverage* out)
    {
        if (sp.align == doc::StrokeAlign::Center)
        {
            return center_stroke (p, sp, sp.width, clip, out);
        }
        if (!center_stroke (p, sp, sp.width * 2.0f, clip, out))
        {
            return false;
        }
        Coverage fill;
        if (!fill_coverage (p, clip, &fill))
        {
            return false;
        }
        bool inside = sp.align == doc::StrokeAlign::Inside;
        for (int32_t y = out->rect.y0; y < out->rect.y1; y++)
        {
            for (int32_t x = out->rect.x0; x < out->rect.x1; x++)
            {
                float f  = coverage_at (fill, x, y);
                float& a = out->a[(size_t) (y - out->rect.y0) * out->rect.width () + (size_t) (x - out->rect.x0)];
                a *= inside ? f : 1.0f - f;
            }
        }
        return true;
    }

    /* A one-channel mask of the path over clip (everything outside the path for inverted paths). */
    raster::Surface path_mask (const doc::Path& p, geom::Rect clip)
    {
        float bg          = p.inverted ? 1.0f : 0.0f;
        raster::Surface s = raster::Surface::with_default (color::PixelFormat::gray8 (), &bg);
        Coverage c;
        if (!fill_coverage (p, clip, &c) || c.a.empty ())
        {
            return s;
        }
        if (p.inverted)
        {
            for (size_t i = 0; i < c.a.size (); i++)
            {
                c.a[i] = 1.0f - c.a[i];
            }
        }
        s.write_region (c.rect, c.a.data (), c.a.size ());
        s.prune ();
        return s;
    }

    /* A vector mask's gray rendering. An empty path reveals everything (hides with `inverted`),
       as Photoshop's Reveal All / Hide All masks. */
    raster::Surface vector_mask_surface (const doc::Path& p, geom::Rect clip)
    {
        if (p.empty ())
        {
            float v = p.inverted ? 0.0f : 1.0f;
            return raster::Surface::with_default (color::PixelFormat::gray8 (), &v);
        }
        return path_mask (p, clip);
    }

    /* A shape layer's pixels: the fill, then the stroke over it, within clip. */
    raster::Surface render_shape (
        const doc::ShapeLayer& s, color::PixelFormat format, const color::CmykSpace* cmyk, geom::Rect clip)
    {
        format.alpha = true;
        raster::Surface out (format);
        Coverage fill, stroke;
        if (s.has_fill)
        {
            if (!fill_coverage (s.path, clip, &fill))
            {
                return out;
            }
            if (s.path.inverted)
            {
                Coverage inv;
                if (!alloc_coverage (clip, &inv))
                {
                    return out;
                }
                for (int32_t y = clip.y0; y < clip.y1; y++)
                {
                    for (int32_t x = clip.x0; x < clip.x1; x++)
                    {
                        inv.a[(size_t) (y - clip.y0) * clip.width () + (size_t) (x - clip.x0)] =
                            1.0f - coverage_at (fill, x, y);
                    }
                }
                fill = inv;
            }
        }
        if (s.stroke.enabled && s.stroke.opacity > 0.0f)
        {
            StrokeParams sp = StrokeParams::make (s.stroke.width);
            sp.cap          = s.stroke.cap;
            sp.join         = s.stroke.join;
            sp.miter_limit  = s.stroke.miter_limit;
            sp.align        = s.stroke.align;
            sp.dashes       = s.stroke.dashes;
            if (!stroke_coverage (s.path, sp, clip, &stroke))
            {
                return out;
            }
        }
        geom::Rect r = fill.rect.unite (stroke.rect);
        if (r.is_empty ())
        {
            return out;
        }
        size_t n = format.channels (), count = (size_t) r.width () * r.height ();
        std::vector<float> vals (count * n, 0.0f);
        float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        std::vector<float> clear (n);
        raster::from_rgba (format, zero, cmyk, clear.data ());
        for (int32_t y = r.y0; y < r.y1; y++)
        {
            for (int32_t x = r.x0; x < r.x1; x++)
            {
                size_t i = (size_t) (y - r.y0) * r.width () + (size_t) (x - r.x0);
                float fa = coverage_at (fill, x, y), sa = coverage_at (stroke, x, y) * s.stroke.opacity;
                float a = sa + fa * (1.0f - sa);
                if (a <= 0.0f)
                {
                    memcpy (&vals[i * n], clear.data (), n * sizeof (float));
                    continue;
                }
                float rgba[4];
                for (int k = 0; k < 3; k++)
                {
                    rgba[k] = (s.stroke.color[k] * sa + s.fill[k] * fa * (1.0f - sa)) / a;
                }
                rgba[3] = a;
                raster::from_rgba (format, rgba, cmyk, &vals[i * n]);
            }
        }
        out.write_region (r, vals.data (), vals.size ());
        out.prune ();
        return out;
    }

    double distance_to_subpath (const doc::Subpath& s, double x, double y)
    {
        std::vector<double> xy;
        flatten (s, 0.25, &xy);
        size_t n = xy.size () / 2;
        if (n == 0)
        {
            return INFINITY;
        }
        double best = hypot (xy[0] - x, xy[1] - y);
        for (size_t i = 1; i < n; i++)
        {
            double ax = xy[(i - 1) * 2], ay = xy[(i - 1) * 2 + 1], bx = xy[i * 2], by = xy[i * 2 + 1];
            double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
            double t = l2 > 0.0 ? fmin (fmax (((x - ax) * dx + (y - ay) * dy) / l2, 0.0), 1.0) : 0.0;
            double d = hypot (ax + dx * t - x, ay + dy * t - y);
            if (d < best)
            {
                best = d;
            }
        }
        return best;
    }

} // namespace vec
