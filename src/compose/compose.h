#pragma once

#include "color/convert.h"
#include "compose/psblend.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "raster/raster.h"
#include <stdint.h>
#include <vector>

namespace compose
{

    const int32_t RENDER_TILE = 256;

    struct Buffer
    {
        geom::Rect rect;
        std::vector<float> px;

        static Buffer transparent (geom::Rect rect);
        static Buffer filled (geom::Rect rect, const float c[4]);
        size_t pixel_count () const;
        float* at (int32_t x, int32_t y);
        const float* at (int32_t x, int32_t y) const;
        raster::Rgba8Image to_rgba8 () const;
        Buffer over_background (const float bg[3]) const;
    };

    class MaskCache;

    struct RenderSettings
    {
        const color::CmykSpace* cmyk;
        float text_gamma;
        MaskCache* masks;
        bool inks;
    };

    RenderSettings default_settings ();

    Buffer render (const doc::Document& d, geom::Rect rect, const RenderSettings& settings);
    Buffer render_tiled (const doc::Document& d, geom::Rect rect, int32_t tile, const RenderSettings& settings);
    Buffer flatten (const doc::Document& d, const RenderSettings& settings);
    Buffer fill_layer_pixels (
        const doc::Document& d, const doc::Layer& l, geom::Rect rect, const RenderSettings& settings);
    raster::Surface composite_to_surface (const doc::Document& d, geom::Rect rect, color::PixelFormat format,
        const float* background, const RenderSettings& settings);
    Buffer surface_to_buffer (const raster::Surface& s, geom::Rect rect, const color::CmykSpace* cmyk);
    geom::Rect layer_bounds (const doc::Layer& layer, geom::Rect canvas);
    bool composite_damage (const doc::Document& a, const doc::Document& b, geom::Rect* out);

} // namespace compose
