#pragma once

#include "geom/geom.h"
#include "raster/raster.h"
#include <stddef.h>

namespace algo
{

    enum class Resample
    {
        Nearest,
        Bilinear,
        Bicubic,
        BicubicSmoother,
        BicubicSharper,
        BicubicAutomatic,
        Lanczos,
        PreserveDetails,
    };

    bool resample_from_str (const char* s, Resample* out);
    void premultiply (float* data, size_t count, size_t ch, bool alpha);
    void unpremultiply (float* data, size_t count, size_t ch, bool alpha);
    geom::Rect scaled_rect (geom::Rect r, double sx, double sy);
    raster::Surface resize_surface (const raster::Surface& s, double sx, double sy, Resample filter);
    raster::Surface resize_surface_in_canvas (
        const raster::Surface& s, double sx, double sy, Resample filter, geom::Rect canvas);

} // namespace algo
