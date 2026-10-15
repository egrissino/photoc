#pragma once

#include <stdint.h>
#include <vector>

namespace geom
{

    const int32_t TILE_SIZE = 256;

    int32_t add_sat (int32_t a, int32_t b);
    int32_t sub_sat (int32_t a, int32_t b);
    int32_t floor_div (int32_t a, int32_t b);
    int32_t floor_mod (int32_t a, int32_t b);

    struct Size
    {
        uint32_t width  = 0;
        uint32_t height = 0;

        uint64_t area () const;
        bool is_empty () const;
    };

    struct TileCoord;

    struct Rect
    {
        int32_t x0 = 0;
        int32_t y0 = 0;
        int32_t x1 = 0;
        int32_t y1 = 0;

        static Rect from_xywh (int32_t x, int32_t y, uint32_t w, uint32_t h);
        static Rect from_size (Size s);

        uint32_t width () const;
        uint32_t height () const;
        Size size () const;
        bool is_empty () const;
        bool contains (int32_t x, int32_t y) const;
        bool contains_rect (const Rect& other) const;
        Rect intersect (const Rect& o) const;
        Rect unite (const Rect& o) const;
        Rect translate (int32_t dx, int32_t dy) const;
        Rect inflate (int32_t d) const;
        std::vector<TileCoord> tiles () const;

        bool operator== (const Rect& o) const;
        bool operator!= (const Rect& o) const;
    };

    struct TileCoord
    {
        int32_t tx = 0;
        int32_t ty = 0;

        static TileCoord containing (int32_t x, int32_t y);
        Rect rect () const;

        bool operator== (const TileCoord& o) const;
        bool operator< (const TileCoord& o) const;
    };

    struct Point
    {
        double x = 0.0;
        double y = 0.0;
    };

    struct Affine
    {
        double m[6] = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};

        static Affine identity ();
        static Affine translate (double dx, double dy);
        static Affine scale (double s);
        static Affine rotate (double radians);

        Affine mul (const Affine& o) const;
        Affine then (const Affine& o) const;
        Point apply (Point p) const;
        double determinant () const;
        bool inverse (Affine* out) const;
    };

} // namespace geom
