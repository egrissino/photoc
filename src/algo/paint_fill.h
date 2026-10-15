#pragma once

#include "algo/select_ops.h"
#include "color/blend.h"
#include "color/convert.h"
#include "geom/geom.h"
#include "raster/raster.h"
#include <vector>

namespace algo
{

    enum class GradientShape
    {
        Linear,
        Radial,
        Angle,
        Reflected,
        Diamond,
    };

    struct GradientStop
    {
        float t;
        float color[4];
    };

    struct Gradient
    {
        GradientShape shape;
        float from[2];
        float to[2];
        std::vector<GradientStop> stops;
        bool reverse;
        bool dither;
    };

    bool gradient_shape_from_str (const char* s, GradientShape* out);
    float gradient_t (GradientShape shape, const float from[2], const float to[2], float x, float y);
    void sample_stops (const std::vector<GradientStop>& stops, float t, float out[4]);
    void paint_gradient (raster::Surface* s, geom::Rect area, const Gradient& g, float opacity, color::BlendMode mode,
        const raster::Surface* selection, bool lock, const color::CmykSpace* cmyk);
    void bucket_fill_region (raster::Surface* s, const Region& region, const float color[4], float opacity,
        const raster::Surface* selection, bool lock, const color::CmykSpace* cmyk);

} // namespace algo
