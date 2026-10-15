#pragma once

#include "color/blend.h"
#include "paint/paint.h"
#include "raster/raster.h"
#include <stddef.h>
#include <vector>

namespace paint
{

    struct Region
    {
        geom::Rect rect;
        size_t ch;
        std::vector<float> data;

        static Region make (geom::Rect rect, size_t ch);
        static Region read (const raster::Surface& s, geom::Rect rect);
        size_t width () const;
        size_t height () const;
        size_t index (int32_t x, int32_t y) const;
        const float* px (int32_t x, int32_t y) const;
        float* px_mut (int32_t x, int32_t y);
        Region crop (geom::Rect r) const;
        void write (raster::Surface* s) const;
    };

    struct Footprint
    {
        Dab dab;
        size_t index;
        geom::Rect rect;
        std::vector<float> cov;

        float at (int32_t x, int32_t y) const;
    };

    typedef void (*DabEffectFn) (Region* work, const Footprint& fp, void* user);

    int alpha_index (color::PixelFormat fmt);
    void over_native (color::PixelFormat fmt, float* dst, const float* src, float k, bool lock_transparency);
    void stroke_coverage (const BrushSettings& brush, const std::vector<StrokePoint>& points, geom::Rect* bounds,
        std::vector<float>* cov);
    geom::Rect apply_coverage (raster::Surface* target, geom::Rect rect, const std::vector<float>& cov, float opacity,
        const raster::Surface* selection, bool lock, const Region& paint, color::BlendMode blend,
        const color::CmykSpace* cmyk);
    geom::Rect apply_dab_stroke (raster::Surface* target, const BrushSettings& brush,
        const std::vector<StrokePoint>& points, const raster::Surface* selection, bool lock, int32_t halo,
        DabEffectFn effect, void* user);
    void mix_back (raster::Surface* target, const Region& orig, const Region& work, geom::Rect rect, float opacity,
        const raster::Surface* selection, bool lock);
    void mix_premultiplied (float* dst, const float* src, size_t n, size_t a, float k);

    struct Smudge
    {
        float strength;
        bool has_finger;
        std::vector<float> finger;
        int32_t radius;
        int alpha;
        bool has_carry;
        std::vector<float> carry;

        static Smudge make (float strength, const float* finger, size_t n, float max_brush_size, int alpha);
        void dab (Region* work, const Footprint& fp);
    };

} // namespace paint
