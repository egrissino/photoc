#pragma once

#include "algo/quick_select.h"
#include "algo/select_ops.h"
#include "geom/geom.h"
#include "raster/raster.h"
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace algo
{

    struct RefineParams
    {
        float radius;
        bool smart_radius;
        float smooth;
        float feather;
        float contrast;
        float shift_edge;
    };

    struct EdgeStroke
    {
        std::vector<float> points;
        float size;
        bool add;
    };

    struct EdgeBrush
    {
        geom::Rect bbox;
        std::vector<int8_t> v;
        geom::Rect added;
        size_t window;

        float at (int32_t x, int32_t y) const;
    };

    const float EDGE_BRUSH_MIN = 1.0f;
    const float EDGE_BRUSH_MAX = 5000.0f;

    RefineParams refine_defaults ();
    bool edge_brush_make (const std::vector<EdgeStroke>& strokes, geom::Rect canvas, EdgeBrush* out);
    std::vector<float> mat_morph (const std::vector<float>& src, size_t w, size_t h, size_t r, bool grow);
    bool refine_mask (const RgbaSource& src, const raster::Surface& mask, geom::Rect canvas, const RefineParams& p,
        const EdgeBrush* brush, Region* out);
    raster::Surface decontaminate (const raster::Surface& src, const Region& alpha, float radius, float amount);
    raster::Surface masked_copy (const raster::Surface& src, const Region& alpha);

} // namespace algo
