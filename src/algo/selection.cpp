#include "algo/selection.h"

#include <memory>
#include <stdint.h>
#include <string.h>

namespace algo
{

    /* Selections are GRAY8 coverage surfaces: 0 = unselected, 1 = selected. */

    using color::PixelFormat;
    using geom::Rect;

    SelectionMode selection_mode_parse (const char* s)
    {
        if (strcmp (s, "add") == 0)
        {
            return SelectionMode::Add;
        }
        if (strcmp (s, "subtract") == 0)
        {
            return SelectionMode::Subtract;
        }
        if (strcmp (s, "intersect") == 0)
        {
            return SelectionMode::Intersect;
        }
        return SelectionMode::Replace;
    }

    /* The selection over `area` (no selection reads as all zero). */
    std::vector<float> mask_from_surface (const raster::Surface* s, Rect area)
    {
        size_t n = (size_t) area.width () * area.height ();
        if (s == NULL || area.is_empty ())
        {
            return std::vector<float> (n, 0.0f);
        }
        std::vector<float> region;
        if (!s->read_region (area, &region))
        {
            return std::vector<float> (n, 0.0f);
        }
        size_t ch = s->channels ();
        if (ch == 1)
        {
            return region;
        }
        std::vector<float> out (n);
        for (size_t i = 0; i < n; i++)
        {
            out[i] = region[i * ch];
        }
        return out;
    }

    struct Ellipse
    {
        float cx, cy, rx, ry;
    };

    bool ellipse_inside (const Ellipse& e, float x, float y)
    {
        float dx = (x - e.cx) / e.rx;
        float dy = (y - e.cy) / e.ry;
        return dx * dx + dy * dy <= 1.0f;
    }

    /* Coverage of pixel (x, y) in sixteenths: 16 when its four corners (anti-aliased) or its centre
       are inside, else the number of its 4x4 sub-samples inside (anti-aliased) or 0. */
    int ellipse_coverage (const Ellipse& e, int32_t x, int32_t y, bool aa)
    {
        float fx = (float) x, fy = (float) y;
        if (!aa)
        {
            return ellipse_inside (e, fx + 0.5f, fy + 0.5f) ? 16 : 0;
        }
        if (ellipse_inside (e, fx, fy) && ellipse_inside (e, fx + 1.0f, fy) && ellipse_inside (e, fx, fy + 1.0f)
            && ellipse_inside (e, fx + 1.0f, fy + 1.0f))
        {
            return 16;
        }
        int n = 0;
        for (int i = 0; i < 16; i++)
        {
            if (ellipse_inside (e, fx + ((float) (i % 4) + 0.5f) / 4.0f, fy + ((float) (i / 4) + 0.5f) / 4.0f))
            {
                n++;
            }
        }
        return n;
    }

    /* The Elliptical Marquee's shape: the ellipse inscribed in r, cut to clip. */
    raster::Surface ellipse_surface (Rect r, Rect clip, bool anti_alias)
    {
        raster::Surface s (PixelFormat::gray8 ());
        Rect cut = r.intersect (clip);
        if (cut.is_empty ())
        {
            return s;
        }
        Ellipse e;
        e.cx     = (float) ((int64_t) r.x0 + (int64_t) r.x1) / 2.0f;
        e.cy     = (float) ((int64_t) r.y0 + (int64_t) r.y1) / 2.0f;
        e.rx     = (float) r.width () / 2.0f;
        e.ry     = (float) r.height () / 2.0f;
        size_t w = cut.width ();
        std::vector<float> data (w * cut.height ());
#pragma omp parallel for schedule(dynamic, 16)
        for (int32_t y = cut.y0; y < cut.y1; y++)
        {
            for (int32_t x = cut.x0; x < cut.x1; x++)
            {
                data[(size_t) (y - cut.y0) * w + (size_t) (x - cut.x0)] =
                    (float) ellipse_coverage (e, x, y, anti_alias) / 16.0f;
            }
        }
        s.write_region (cut, data.data (), data.size ());
        s.prune ();
        return s;
    }

    /* fn(a, b) per pixel over `area` through float masks of the whole area: for selections that
       aren't GRAY8, or other combining functions. */
    raster::Surface combine_surfaces_floats (
        const raster::Surface& a, const raster::Surface& b, Rect area, float (*fn) (float a, float b))
    {
        raster::Surface out (PixelFormat::gray8 ());
        if (area.is_empty ())
        {
            return out;
        }
        std::vector<float> ma = mask_from_surface (&a, area);
        std::vector<float> mb = mask_from_surface (&b, area);
        for (size_t i = 0; i < ma.size (); i++)
        {
            ma[i] = fn (ma[i], mb[i]);
        }
        out.write_region (area, ma.data (), ma.size ());
        out.prune ();
        return out;
    }

    const int COMBINE_MAX      = 0;
    const int COMBINE_SUBTRACT = 1;
    const int COMBINE_MIN      = 2;

    uint8_t combine_byte (int op, uint8_t a, uint8_t b)
    {
        if (op == COMBINE_MAX)
        {
            return a > b ? a : b;
        }
        if (op == COMBINE_MIN)
        {
            return a < b ? a : b;
        }
        return (uint8_t) (((unsigned) a * (255u - b) + 127u) / 255u);
    }

    /* fn(a, b) per pixel over `area`, as a new GRAY8 selection. Two GRAY8 selections combine tile
       by tile on their bytes: only tiles either one has are visited, and a tile whose partner
       can't change it (an absent tile that is empty for Add and Subtract, full for Intersect) is
       shared rather than copied, so the cost follows the shapes, not the canvas. */
    raster::Surface combine_surfaces (
        const raster::Surface& a, const raster::Surface& b, Rect area, float (*fn) (float a, float b))
    {
        int op        = fn == select_max      ? COMBINE_MAX
                      : fn == select_subtract ? COMBINE_SUBTRACT
                      : fn == select_min      ? COMBINE_MIN
                                              : -1;
        PixelFormat g = PixelFormat::gray8 ();
        if (op < 0 || a.format () != g || b.format () != g)
        {
            return combine_surfaces_floats (a, b, area, fn);
        }
        raster::Surface out (g);
        if (area.is_empty ())
        {
            return out;
        }
        uint8_t da = a.default_bytes ()[0], db = b.default_bytes ()[0], dr = combine_byte (op, da, db);
        if (dr != 0)
        {
            float v = (float) dr / 255.0f;
            out     = raster::Surface::with_default (g, &v);
        }
        geom::TileCoord c0 = geom::TileCoord::containing (area.x0, area.y0),
                        c1 = geom::TileCoord::containing (area.x1 - 1, area.y1 - 1);
        std::vector<geom::TileCoord> coords;
        for (raster::TileMap::const_iterator it = a.tiles ().begin (); it != a.tiles ().end (); ++it)
        {
            const geom::TileCoord& c = it->first;
            if (c.tx >= c0.tx && c.tx <= c1.tx && c.ty >= c0.ty && c.ty <= c1.ty)
            {
                coords.push_back (c);
            }
        }
        for (raster::TileMap::const_iterator it = b.tiles ().begin (); it != b.tiles ().end (); ++it)
        {
            const geom::TileCoord& c = it->first;
            if (c.tx >= c0.tx && c.tx <= c1.tx && c.ty >= c0.ty && c.ty <= c1.ty && a.tile (c) == NULL)
            {
                coords.push_back (c);
            }
        }
        /* Whether an absent tile on one side leaves the other side's tile as it is. */
        bool b_absent_keeps_a = op == COMBINE_MIN ? db == 255 : db == 0;
        bool a_absent_keeps_b = op == COMBINE_MAX ? da == 0 : op == COMBINE_MIN ? da == 255 : false;
        std::vector<raster::TileEntry> entries (coords.size ());
#pragma omp parallel for schedule(dynamic, 4)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) coords.size (); ii++)
        {
            size_t i                           = (size_t) ii;
            geom::TileCoord c                  = coords[i];
            raster::TileMap::const_iterator ia = a.tiles ().find (c), ib = b.tiles ().find (c);
            bool has_a = ia != a.tiles ().end (), has_b = ib != b.tiles ().end ();
            entries[i].coord = c;
            if (has_a && !has_b && b_absent_keeps_a)
            {
                entries[i].tile = ia->second;
                continue;
            }
            if (has_b && !has_a && a_absent_keeps_b)
            {
                entries[i].tile = ib->second;
                continue;
            }
            std::shared_ptr<raster::Tile> t = std::make_shared<raster::Tile> ();
            size_t n                        = (size_t) geom::TILE_SIZE * (size_t) geom::TILE_SIZE;
            t->data.resize (n);
            const uint8_t *pa = has_a ? ia->second->data.data () : NULL, *pb = has_b ? ib->second->data.data () : NULL;
            for (size_t p = 0; p < n; p++)
            {
                t->data[p] = combine_byte (op, pa != NULL ? pa[p] : da, pb != NULL ? pb[p] : db);
            }
            entries[i].tile = t;
        }
        out.put_tiles (entries);
        out.prune ();
        return out;
    }

    float select_max (float a, float b)
    {
        return a > b ? a : b;
    }

    float select_subtract (float a, float b)
    {
        return a * (1.0f - b);
    }

    float select_min (float a, float b)
    {
        return a < b ? a : b;
    }

} // namespace algo
