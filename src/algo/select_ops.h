#pragma once

#include "algo/selection.h"
#include "geom/geom.h"
#include "raster/raster.h"
#include <stdint.h>
#include <vector>

namespace algo
{

    struct Region
    {
        geom::Rect bbox;
        std::vector<uint8_t> mask;
    };

    struct PolyPoint
    {
        float x;
        float y;
    };

    raster::Surface mask_to_surface (const std::vector<float>& m, geom::Rect area);
    std::vector<float> edt (const std::vector<bool>& inside, size_t w, size_t h);
    std::vector<float> expand_mask (const std::vector<float>& m, size_t w, size_t h, float r);
    std::vector<float> contract_mask (const std::vector<float>& m, size_t w, size_t h, float r);
    std::vector<float> border_mask (const std::vector<float>& m, size_t w, size_t h, float r);
    std::vector<float> smooth_mask (const std::vector<float>& m, size_t w, size_t h, float r);
    std::vector<float> feather_mask (const std::vector<float>& m, size_t w, size_t h, float radius);
    std::vector<float> polygon_mask (const std::vector<PolyPoint>& points, geom::Rect area, bool anti_alias);
    bool wand_region (const std::vector<uint8_t>& img, geom::Rect area, int32_t sx, int32_t sy, float tolerance,
        bool contiguous, bool anti_alias, Region* out);
    raster::Surface region_surface (const Region& r);

} // namespace algo
