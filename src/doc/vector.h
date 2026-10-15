#pragma once

#include "raster/raster.h"
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace doc
{

    struct Knot
    {
        double anchor[2];
        double in_ctrl[2];
        double out_ctrl[2];
        bool smooth;

        static Knot corner (double x, double y);
    };

    enum class PathOp : uint8_t
    {
        Combine,
        Subtract,
        Intersect,
        Exclude,
        Join,
    };

    struct Subpath
    {
        bool closed;
        PathOp op;
        std::vector<Knot> knots;
    };

    struct Path
    {
        std::vector<Subpath> subpaths;
        bool even_odd;
        bool inverted;

        static Path make ();
        bool empty () const;
        bool control_bounds (double out[4]) const;
        void transform (const double m[6]);
    };

    struct NamedPath
    {
        std::string name;
        Path path;
    };

    struct VectorMask
    {
        Path path;
        bool enabled;
        bool linked;
        float density;
        float feather;
        raster::Surface surface;
    };

    enum class StrokeAlign : uint8_t
    {
        Inside,
        Center,
        Outside,
    };

    enum class LineCap : uint8_t
    {
        Butt,
        Round,
        Square,
    };

    enum class LineJoin : uint8_t
    {
        Miter,
        Round,
        Bevel,
    };

    enum class LiveKind : uint8_t
    {
        None,
        Rect,
        Ellipse,
        Polygon,
        Line,
    };

    struct ShapeStroke
    {
        bool enabled;
        float width;
        float color[3];
        float opacity;
        StrokeAlign align;
        LineCap cap;
        LineJoin join;
        float miter_limit;
        std::vector<float> dashes;
    };

    struct LiveShape
    {
        LiveKind kind;
        double rect[4];
        double radius;
        uint32_t sides;
        double star;
        double weight;
    };

    struct ShapeLayer
    {
        Path path;
        bool has_fill;
        float fill[3];
        ShapeStroke stroke;
        LiveShape live;

        static ShapeLayer make ();
    };

    bool same_path (const Path& a, const Path& b);
    bool same_shape (const ShapeLayer& a, const ShapeLayer& b);
    Subpath rect_subpath (const double r[4], double radius);
    Subpath ellipse_subpath (const double r[4]);
    Subpath polygon_subpath (const double r[4], uint32_t sides, double star);
    Subpath line_subpath (const double from[2], const double to[2], double weight);
    Path live_path (const LiveShape& s);

} // namespace doc
