#pragma once

#include "color/blend.h"
#include "color/color.h"
#include "color/convert.h"
#include "geom/geom.h"
#include "raster/raster.h"
#include <map>
#include <set>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace paint
{

    const float MAX_BRUSH_SIZE = 5000.0f;
    const double MAX_COORD     = 1000000.0;
    const int32_t COV_TILE     = 64;

    struct StrokePoint
    {
        double x;
        double y;
        float pressure;
        float tilt_x;
        float tilt_y;
        float rotation;
        float wheel;
        double time;

        static StrokePoint make (double x, double y, float pressure);
    };

    struct BrushSettings
    {
        float size;
        float hardness;
        float spacing;
        float opacity;
        float flow;
        bool pressure_size;
        bool pressure_opacity;
        float color[4];
        float background[4];
        bool erase;
        color::BlendMode mode;
        float angle;
        float roundness;
        bool flip_x;
        bool flip_y;
        bool aliased;
        bool square;
        uint64_t seed;

        static BrushSettings defaults ();
        BrushSettings bounded_for_render () const;
    };

    struct Dab
    {
        double cx;
        double cy;
        float radius;
        float alpha;
        float angle;
        float roundness;
        bool flip_x;
        bool flip_y;
        float opacity;
        uint64_t index;
    };

    uint64_t mix64 (uint64_t z);
    float rand01 (uint64_t seed, uint64_t index, uint64_t stream);
    float hash2 (int32_t x, int32_t y, uint64_t salt);
    uint64_t seed_from_bytes (const uint8_t* bytes, size_t len);

    float tip_falloff (float d, float radius, float hardness);
    float half_coverage_radius (float radius, float hardness);
    float dab_coverage (float d, float radius, float hardness);
    void grid_center (double x, double y, float diameter, float* cx, float* cy);

    struct StepInput
    {
        StrokePoint point;
        float direction;
        float initial_direction;
        uint64_t step;
    };

    class DabGenerator
    {
    public:
        explicit DabGenerator (const BrushSettings& brush);
        void push (const StrokePoint* points, size_t count, std::vector<Dab>* out);
        void finish (std::vector<Dab>* out);

    private:
        double step_len (const StrokePoint& p) const;
        void emit (const StrokePoint& p, std::vector<Dab>* out);
        void build (const StepInput& s, std::vector<Dab>* out);
        bool grid_steps (const StrokePoint& a, const StrokePoint& p, std::vector<Dab>* out);

        BrushSettings brush_;
        bool has_last_;
        StrokePoint last_;
        double next_at_;
        bool has_pending_;
        StrokePoint pending_;
        bool has_initial_;
        float initial_dir_;
        float dir_;
        uint64_t step_;
        bool grid_;
        double grid_offset_;
        uint64_t next_index_;
    };

    struct CovTile
    {
        std::vector<float> cov;
    };

    class StrokeRenderer
    {
    public:
        StrokeRenderer (const BrushSettings& brush);
        void push (const StrokePoint* points, size_t count);
        void finish ();
        geom::Rect bounds () const;
        float coverage_at (int32_t x, int32_t y) const;
        void coverage_in (geom::Rect r, std::vector<float>* out) const;
        geom::Rect take_dirty_rect ();
        void footprint (const Dab& d, geom::Rect* rect, std::vector<float>* cov) const;
        geom::Rect composite (const raster::Surface& pre, raster::Surface* target, const raster::Surface* selection,
            bool lock_transparency, bool all, const color::CmykSpace* cmyk);

    private:
        void raster_pending ();
        geom::Rect dab_rect (const Dab& d) const;
        void rasterize (const Dab& d, geom::Rect rect, std::vector<float>* out) const;
        void accumulate (geom::Rect rect, const std::vector<float>& vals, float ceil);

        BrushSettings brush_;
        DabGenerator generator_;
        std::map<std::pair<int32_t, int32_t>, CovTile> tiles_;
        std::set<std::pair<int32_t, int32_t>> dirty_;
        geom::Rect bounds_;
        std::vector<Dab> pending_;
        std::vector<float> scratch_;
    };

    geom::Rect render_stroke (raster::Surface* target, const BrushSettings& brush,
        const std::vector<StrokePoint>& points, const raster::Surface* selection, bool lock_transparency,
        const color::CmykSpace* cmyk);

} // namespace paint
