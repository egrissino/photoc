#pragma once

#include "geom/geom.h"
#include "raster/raster.h"
#include <vector>

namespace algo
{

    enum class SelectionMode
    {
        Replace,
        Add,
        Subtract,
        Intersect,
    };

    SelectionMode selection_mode_parse (const char* s);

    std::vector<float> mask_from_surface (const raster::Surface* s, geom::Rect area);
    raster::Surface ellipse_surface (geom::Rect r, geom::Rect clip, bool anti_alias);
    raster::Surface combine_surfaces (
        const raster::Surface& a, const raster::Surface& b, geom::Rect area, float (*fn) (float a, float b));

    float select_max (float a, float b);
    float select_subtract (float a, float b);
    float select_min (float a, float b);

} // namespace algo
