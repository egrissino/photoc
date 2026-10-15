#include "doc/vector.h"

#include <math.h>
#include <string.h>

namespace doc
{

    /* Vector data: paths (the Pen tool and shapes), saved and work paths, vector masks and shape
       layers. Coordinates are document pixels (y down), cubic Bezier: segment i runs from knot i's
       anchor through its out handle and knot i+1's in handle to knot i+1's anchor (closed subpaths
       come back to the first knot). A knot whose handles sit on its anchor is a corner.

       As in Photoshop every subpath is filled on its own and folded into the result in order with
       its operation (the first always combines); Join subpaths are filled together with the one
       before them (holes). A shape layer is its path, a fill and a stroke; a live shape (rectangle,
       ellipse, polygon, line) remembers the parameters its path was made from. */

    Knot Knot::corner (double x, double y)
    {
        Knot k;
        k.anchor[0] = k.in_ctrl[0] = k.out_ctrl[0] = x;
        k.anchor[1] = k.in_ctrl[1] = k.out_ctrl[1] = y;
        k.smooth                                   = false;
        return k;
    }

    Path Path::make ()
    {
        Path p;
        p.even_odd = false;
        p.inverted = false;
        return p;
    }

    bool Path::empty () const
    {
        for (size_t i = 0; i < subpaths.size (); i++)
        {
            if (!subpaths[i].knots.empty ())
            {
                return false;
            }
        }
        return true;
    }

    bool Path::control_bounds (double out[4]) const
    {
        bool any = false;
        for (size_t i = 0; i < subpaths.size (); i++)
        {
            for (size_t k = 0; k < subpaths[i].knots.size (); k++)
            {
                const Knot& n        = subpaths[i].knots[k];
                const double* pts[3] = {n.anchor, n.in_ctrl, n.out_ctrl};
                for (int j = 0; j < 3; j++)
                {
                    if (!any)
                    {
                        out[0] = out[2] = pts[j][0];
                        out[1] = out[3] = pts[j][1];
                        any             = true;
                    }
                    out[0] = fmin (out[0], pts[j][0]);
                    out[1] = fmin (out[1], pts[j][1]);
                    out[2] = fmax (out[2], pts[j][0]);
                    out[3] = fmax (out[3], pts[j][1]);
                }
            }
        }
        return any;
    }

    void map_point (const double m[6], double p[2])
    {
        double x = p[0], y = p[1];
        p[0] = m[0] * x + m[2] * y + m[4];
        p[1] = m[1] * x + m[3] * y + m[5];
    }

    /* Every point mapped by m (x' = a x + c y + e, y' = b x + d y + f). */
    void Path::transform (const double m[6])
    {
        for (size_t i = 0; i < subpaths.size (); i++)
        {
            for (size_t k = 0; k < subpaths[i].knots.size (); k++)
            {
                Knot& n = subpaths[i].knots[k];
                map_point (m, n.anchor);
                map_point (m, n.in_ctrl);
                map_point (m, n.out_ctrl);
            }
        }
    }

    ShapeLayer ShapeLayer::make ()
    {
        ShapeLayer s;
        s.path     = Path::make ();
        s.has_fill = true;
        s.fill[0] = s.fill[1] = s.fill[2] = 0.0f;
        s.stroke.enabled                  = false;
        s.stroke.width                    = 3.0f;
        s.stroke.color[0] = s.stroke.color[1] = s.stroke.color[2] = 0.0f;
        s.stroke.opacity                                          = 1.0f;
        s.stroke.align                                            = StrokeAlign::Center;
        s.stroke.cap                                              = LineCap::Butt;
        s.stroke.join                                             = LineJoin::Miter;
        s.stroke.miter_limit                                      = 100.0f;
        memset (&s.live, 0, sizeof s.live);
        s.live.kind = LiveKind::None;
        return s;
    }

    bool same_knot (const Knot& a, const Knot& b)
    {
        return memcmp (a.anchor, b.anchor, sizeof a.anchor) == 0 && memcmp (a.in_ctrl, b.in_ctrl, sizeof a.in_ctrl) == 0
            && memcmp (a.out_ctrl, b.out_ctrl, sizeof a.out_ctrl) == 0 && a.smooth == b.smooth;
    }

    bool same_path (const Path& a, const Path& b)
    {
        if (a.even_odd != b.even_odd || a.inverted != b.inverted || a.subpaths.size () != b.subpaths.size ())
        {
            return false;
        }
        for (size_t i = 0; i < a.subpaths.size (); i++)
        {
            const Subpath &x = a.subpaths[i], &y = b.subpaths[i];
            if (x.closed != y.closed || x.op != y.op || x.knots.size () != y.knots.size ())
            {
                return false;
            }
            for (size_t k = 0; k < x.knots.size (); k++)
            {
                if (!same_knot (x.knots[k], y.knots[k]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool same_shape (const ShapeLayer& a, const ShapeLayer& b)
    {
        const ShapeStroke &s = a.stroke, &t = b.stroke;
        return same_path (a.path, b.path) && a.has_fill == b.has_fill && memcmp (a.fill, b.fill, sizeof a.fill) == 0
            && s.enabled == t.enabled && s.width == t.width && memcmp (s.color, t.color, sizeof s.color) == 0
            && s.opacity == t.opacity && s.align == t.align && s.cap == t.cap && s.join == t.join
            && s.miter_limit == t.miter_limit && s.dashes == t.dashes && memcmp (&a.live, &b.live, sizeof a.live) == 0;
    }

    /* The handle length that makes a cubic quarter circle. */
    const double KAPPA = 0.5522847498307936;

    Knot smooth_knot (double x, double y, double ix, double iy, double ox, double oy)
    {
        Knot k;
        k.anchor[0]   = x;
        k.anchor[1]   = y;
        k.in_ctrl[0]  = ix;
        k.in_ctrl[1]  = iy;
        k.out_ctrl[0] = ox;
        k.out_ctrl[1] = oy;
        k.smooth      = true;
        return k;
    }

    /* A rectangle (x0, y0, x1, y1), clockwise from the top left, with rounded corners of `radius`. */
    Subpath rect_subpath (const double r[4], double radius)
    {
        Subpath s;
        s.closed  = true;
        s.op      = PathOp::Combine;
        double x0 = fmin (r[0], r[2]), y0 = fmin (r[1], r[3]), x1 = fmax (r[0], r[2]), y1 = fmax (r[1], r[3]);
        double rad = fmin (fmax (radius, 0.0), fmin (x1 - x0, y1 - y0) / 2.0);
        if (rad <= 0.0)
        {
            s.knots.push_back (Knot::corner (x0, y0));
            s.knots.push_back (Knot::corner (x1, y0));
            s.knots.push_back (Knot::corner (x1, y1));
            s.knots.push_back (Knot::corner (x0, y1));
            return s;
        }
        double h = rad * KAPPA;
        /* Two knots per corner, where the arcs meet the straight edges. */
        s.knots.push_back (smooth_knot (x0 + rad, y0, x0 + rad - h, y0, x0 + rad, y0));
        s.knots.push_back (smooth_knot (x1 - rad, y0, x1 - rad, y0, x1 - rad + h, y0));
        s.knots.push_back (smooth_knot (x1, y0 + rad, x1, y0 + rad - h, x1, y0 + rad));
        s.knots.push_back (smooth_knot (x1, y1 - rad, x1, y1 - rad, x1, y1 - rad + h));
        s.knots.push_back (smooth_knot (x1 - rad, y1, x1 - rad + h, y1, x1 - rad, y1));
        s.knots.push_back (smooth_knot (x0 + rad, y1, x0 + rad, y1, x0 + rad - h, y1));
        s.knots.push_back (smooth_knot (x0, y1 - rad, x0, y1 - rad + h, x0, y1 - rad));
        s.knots.push_back (smooth_knot (x0, y0 + rad, x0, y0 + rad, x0, y0 + rad - h));
        return s;
    }

    /* An ellipse in (x0, y0, x1, y1): four smooth knots, top first, clockwise. */
    Subpath ellipse_subpath (const double r[4])
    {
        Subpath s;
        s.closed  = true;
        s.op      = PathOp::Combine;
        double cx = (r[0] + r[2]) / 2.0, cy = (r[1] + r[3]) / 2.0, rx = fabs (r[2] - r[0]) / 2.0,
               ry = fabs (r[3] - r[1]) / 2.0;
        double hx = rx * KAPPA, hy = ry * KAPPA;
        s.knots.push_back (smooth_knot (cx, cy - ry, cx - hx, cy - ry, cx + hx, cy - ry));
        s.knots.push_back (smooth_knot (cx + rx, cy, cx + rx, cy - hy, cx + rx, cy + hy));
        s.knots.push_back (smooth_knot (cx, cy + ry, cx + hx, cy + ry, cx - hx, cy + ry));
        s.knots.push_back (smooth_knot (cx - rx, cy, cx - rx, cy + hy, cx - rx, cy - hy));
        return s;
    }

    /* A regular polygon (or a star, inner radius `star` x outer when star > 0) in the box, the first
       point at the top. */
    Subpath polygon_subpath (const double r[4], uint32_t sides, double star)
    {
        Subpath s;
        s.closed = true;
        s.op     = PathOp::Combine;
        if (sides < 3)
        {
            sides = 3;
        }
        double cx = (r[0] + r[2]) / 2.0, cy = (r[1] + r[3]) / 2.0, rx = fabs (r[2] - r[0]) / 2.0,
               ry  = fabs (r[3] - r[1]) / 2.0;
        uint32_t n = star > 0.0 ? sides * 2 : sides;
        for (uint32_t i = 0; i < n; i++)
        {
            double a = -M_PI / 2.0 + 2.0 * M_PI * (double) i / (double) n;
            double k = star > 0.0 && (i & 1) ? fmin (fmax (star, 0.01), 1.0) : 1.0;
            s.knots.push_back (Knot::corner (cx + cos (a) * rx * k, cy + sin (a) * ry * k));
        }
        return s;
    }

    /* A line of `weight` px from `from` to `to`: a thin rectangle along it. */
    Subpath line_subpath (const double from[2], const double to[2], double weight)
    {
        Subpath s;
        s.closed  = true;
        s.op      = PathOp::Combine;
        double dx = to[0] - from[0], dy = to[1] - from[1], len = sqrt (dx * dx + dy * dy);
        double nx = len > 0 ? -dy / len * weight / 2.0 : 0.0, ny = len > 0 ? dx / len * weight / 2.0 : weight / 2.0;
        s.knots.push_back (Knot::corner (from[0] + nx, from[1] + ny));
        s.knots.push_back (Knot::corner (to[0] + nx, to[1] + ny));
        s.knots.push_back (Knot::corner (to[0] - nx, to[1] - ny));
        s.knots.push_back (Knot::corner (from[0] - nx, from[1] - ny));
        return s;
    }

    Path live_path (const LiveShape& s)
    {
        Path p = Path::make ();
        switch (s.kind)
        {
        case LiveKind::Rect:
            p.subpaths.push_back (rect_subpath (s.rect, s.radius));
            break;
        case LiveKind::Ellipse:
            p.subpaths.push_back (ellipse_subpath (s.rect));
            break;
        case LiveKind::Polygon:
            p.subpaths.push_back (polygon_subpath (s.rect, s.sides, s.star));
            break;
        case LiveKind::Line:
            {
                double a[2] = {s.rect[0], s.rect[1]}, b[2] = {s.rect[2], s.rect[3]};
                p.subpaths.push_back (line_subpath (a, b, s.weight));
                break;
            }
        default:
            break;
        }
        return p;
    }

} // namespace doc
