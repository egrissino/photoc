#pragma once

#include "color/convert.h"
#include "doc/vector.h"
#include "geom/geom.h"
#include "raster/raster.h"
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace vec
{

    struct Coverage
    {
        geom::Rect rect;
        std::vector<float> a;
    };

    struct StrokeParams
    {
        float width;
        doc::LineCap cap;
        doc::LineJoin join;
        float miter_limit;
        doc::StrokeAlign align;
        std::vector<float> dashes;

        static StrokeParams make (float width);
    };

    void flatten (const doc::Subpath& s, double tol, std::vector<double>* xy);
    bool path_bounds (const doc::Path& p, double out[4]);
    bool fill_coverage (const doc::Path& p, geom::Rect clip, Coverage* out);
    bool stroke_coverage (const doc::Path& p, const StrokeParams& sp, geom::Rect clip, Coverage* out);
    raster::Surface path_mask (const doc::Path& p, geom::Rect clip);
    raster::Surface vector_mask_surface (const doc::Path& p, geom::Rect clip);
    raster::Surface render_shape (
        const doc::ShapeLayer& s, color::PixelFormat format, const color::CmykSpace* cmyk, geom::Rect clip);
    double distance_to_subpath (const doc::Subpath& s, double x, double y);

} // namespace vec
