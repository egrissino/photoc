#pragma once

#include "algo/select_ops.h"
#include "geom/geom.h"
#include "raster/raster.h"
#include <stddef.h>
#include <vector>

namespace algo
{

    typedef void (*ReadRgba) (void* user, geom::Rect r, float* out);

    struct RgbaSource
    {
        ReadRgba read;
        void* user;
    };

    struct QuickPoint
    {
        float x;
        float y;
    };

    struct QuickStats
    {
        int windows;
        size_t pixels;
        double read;
        double seeds;
        double cut;
        double upsample;
    };

    bool quick_select (const RgbaSource& src, geom::Rect canvas, const std::vector<QuickPoint>& points, float size,
        const std::vector<QuickPoint>* stroke, const raster::Surface* prior, int max_windows, Region* out,
        QuickStats* stats);

} // namespace algo
