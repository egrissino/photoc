#pragma once

#include "doc/doc.h"
#include "geom/geom.h"
#include "raster/raster.h"
#include <stdint.h>
#include <vector>

namespace compose
{

    float feather_sigma (float px);
    bool has_feather (const doc::LayerMask& m);
    void gaussian (float* v, size_t w, size_t h, float sigma);
    raster::Surface feathered_mask (const doc::LayerMask& m, geom::Rect canvas);
    bool same_tiles (const raster::Surface& a, const raster::Surface& b);

    class MaskCache
    {
    public:
        MaskCache ();
        raster::Surface feathered (const doc::LayerMask& m, geom::Rect canvas);

    private:
        struct Entry
        {
            raster::Surface source;
            float density;
            float feather;
            geom::Rect canvas;
            raster::Surface result;
            uint64_t used;
        };
        std::vector<Entry> entries_;
        uint64_t clock_;
    };

} // namespace compose
