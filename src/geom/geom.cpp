#include "geom/geom.h"

#include <limits.h>
#include <math.h>

namespace geom
{

    int32_t add_sat (int32_t a, int32_t b)
    {
        int64_t r = (int64_t) a + (int64_t) b;
        if (r > INT32_MAX)
        {
            return INT32_MAX;
        }
        if (r < INT32_MIN)
        {
            return INT32_MIN;
        }
        return (int32_t) r;
    }

    int32_t sub_sat (int32_t a, int32_t b)
    {
        int64_t r = (int64_t) a - (int64_t) b;
        if (r > INT32_MAX)
        {
            return INT32_MAX;
        }
        if (r < INT32_MIN)
        {
            return INT32_MIN;
        }
        return (int32_t) r;
    }

    int32_t floor_div (int32_t a, int32_t b)
    {
        int32_t q = a / b;
        int32_t r = a % b;
        if (r != 0 && ((r < 0) != (b < 0)))
        {
            q -= 1;
        }
        return q;
    }

    int32_t floor_mod (int32_t a, int32_t b)
    {
        int32_t r = a % b;
        if (r != 0 && ((r < 0) != (b < 0)))
        {
            r += b;
        }
        return r;
    }

    uint64_t Size::area () const
    {
        return (uint64_t) width * (uint64_t) height;
    }

    bool Size::is_empty () const
    {
        return width == 0 || height == 0;
    }

    Rect Rect::from_xywh (int32_t x, int32_t y, uint32_t w, uint32_t h)
    {
        Rect r = {x, y, add_sat (x, (int32_t) w), add_sat (y, (int32_t) h)};
        return r;
    }

    Rect Rect::from_size (Size s)
    {
        return from_xywh (0, 0, s.width, s.height);
    }

    uint32_t Rect::width () const
    {
        return x1 > x0 ? (uint32_t) ((int64_t) x1 - (int64_t) x0) : 0;
    }

    uint32_t Rect::height () const
    {
        return y1 > y0 ? (uint32_t) ((int64_t) y1 - (int64_t) y0) : 0;
    }

    Size Rect::size () const
    {
        Size s = {width (), height ()};
        return s;
    }

    bool Rect::is_empty () const
    {
        return x1 <= x0 || y1 <= y0;
    }

    bool Rect::contains (int32_t x, int32_t y) const
    {
        return x >= x0 && x < x1 && y >= y0 && y < y1;
    }

    bool Rect::contains_rect (const Rect& o) const
    {
        if (o.is_empty ())
        {
            return true;
        }
        return o.x0 >= x0 && o.y0 >= y0 && o.x1 <= x1 && o.y1 <= y1;
    }

    Rect Rect::intersect (const Rect& o) const
    {
        Rect r = {x0 > o.x0 ? x0 : o.x0, y0 > o.y0 ? y0 : o.y0, x1 < o.x1 ? x1 : o.x1, y1 < o.y1 ? y1 : o.y1};
        if (r.is_empty ())
        {
            return Rect ();
        }
        return r;
    }

    Rect Rect::unite (const Rect& o) const
    {
        if (is_empty ())
        {
            return o;
        }
        if (o.is_empty ())
        {
            return *this;
        }
        Rect r = {x0 < o.x0 ? x0 : o.x0, y0 < o.y0 ? y0 : o.y0, x1 > o.x1 ? x1 : o.x1, y1 > o.y1 ? y1 : o.y1};
        return r;
    }

    Rect Rect::translate (int32_t dx, int32_t dy) const
    {
        Rect r = {add_sat (x0, dx), add_sat (y0, dy), add_sat (x1, dx), add_sat (y1, dy)};
        return r;
    }

    Rect Rect::inflate (int32_t d) const
    {
        Rect r = {sub_sat (x0, d), sub_sat (y0, d), add_sat (x1, d), add_sat (y1, d)};
        return r;
    }

    std::vector<TileCoord> Rect::tiles () const
    {
        std::vector<TileCoord> out;
        if (is_empty ())
        {
            return out;
        }
        TileCoord a = TileCoord::containing (x0, y0);
        TileCoord b = TileCoord::containing (x1 - 1, y1 - 1);
        out.reserve ((size_t) (b.tx - a.tx + 1) * (size_t) (b.ty - a.ty + 1));
        for (int32_t ty = a.ty; ty <= b.ty; ty++)
        {
            for (int32_t tx = a.tx; tx <= b.tx; tx++)
            {
                TileCoord c = {tx, ty};
                out.push_back (c);
            }
        }
        return out;
    }

    bool Rect::operator== (const Rect& o) const
    {
        return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1;
    }

    bool Rect::operator!= (const Rect& o) const
    {
        return !(*this == o);
    }

    TileCoord TileCoord::containing (int32_t x, int32_t y)
    {
        TileCoord c = {floor_div (x, TILE_SIZE), floor_div (y, TILE_SIZE)};
        return c;
    }

    Rect TileCoord::rect () const
    {
        return Rect::from_xywh (tx * TILE_SIZE, ty * TILE_SIZE, TILE_SIZE, TILE_SIZE);
    }

    bool TileCoord::operator== (const TileCoord& o) const
    {
        return tx == o.tx && ty == o.ty;
    }

    bool TileCoord::operator< (const TileCoord& o) const
    {
        if (tx != o.tx)
        {
            return tx < o.tx;
        }
        return ty < o.ty;
    }

    Affine Affine::identity ()
    {
        return Affine ();
    }

    Affine Affine::translate (double dx, double dy)
    {
        Affine a = {{1.0, 0.0, 0.0, 1.0, dx, dy}};
        return a;
    }

    Affine Affine::scale (double s)
    {
        Affine a = {{s, 0.0, 0.0, s, 0.0, 0.0}};
        return a;
    }

    Affine Affine::rotate (double radians)
    {
        double s = sin (radians);
        double c = cos (radians);
        Affine a = {{c, s, -s, c, 0.0, 0.0}};
        return a;
    }

    Affine Affine::mul (const Affine& o) const
    {
        const double* n = o.m;
        Affine r        = {{
            m[0] * n[0] + m[2] * n[1],
            m[1] * n[0] + m[3] * n[1],
            m[0] * n[2] + m[2] * n[3],
            m[1] * n[2] + m[3] * n[3],
            m[0] * n[4] + m[2] * n[5] + m[4],
            m[1] * n[4] + m[3] * n[5] + m[5],
        }};
        return r;
    }

    Affine Affine::then (const Affine& o) const
    {
        return o.mul (*this);
    }

    Point Affine::apply (Point p) const
    {
        Point r = {m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]};
        return r;
    }

    double Affine::determinant () const
    {
        return m[0] * m[3] - m[1] * m[2];
    }

    bool Affine::inverse (Affine* out) const
    {
        double det = determinant ();
        if (fabs (det) < 1e-12)
        {
            return false;
        }
        double inv = 1.0 / det;
        Affine r   = {{
            m[3] * inv,
            -m[1] * inv,
            -m[2] * inv,
            m[0] * inv,
            (m[2] * m[5] - m[3] * m[4]) * inv,
            (m[1] * m[4] - m[0] * m[5]) * inv,
        }};
        *out       = r;
        return true;
    }

} // namespace geom
