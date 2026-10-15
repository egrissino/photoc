#pragma once

#include "color/convert.h"
#include "engine/session.h"
#include "geom/geom.h"
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace engine
{

    /* A layer's pixels as floats in its own channels (colour then alpha), w x h. */
    struct Plane
    {
        size_t w;
        size_t h;
        size_t n;
        bool alpha;
        std::vector<float> px;
    };

    /* What a filter sees: where the plane lies (`src`), the area it changes (the selection's
       bounds), the canvas, the layer's format and the foreground / background colours (RGBA). */
    struct FilterCtx
    {
        geom::Rect src;
        geom::Rect area;
        geom::Rect canvas;
        color::PixelFormat fmt;
        const color::CmykSpace* cmyk;
        float fg[4];
        float bg[4];
    };

    typedef bool (*FilterFn) (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err);

    enum class Reach : uint8_t
    {
        Params,
        Canvas,
        None,
    };

    /* A filter: its command, the pass that runs it, whether it works on premultiplied colour
       (neighbourhood averages) or straight colour (per-pixel looks), and how much around the
       area it reads. */
    struct FilterSpec
    {
        const char* id;
        const char* label;
        const char* menu;
        const char* params;
        FilterFn run;
        bool premultiplied;
        Reach reach;
    };

    void basic_filters (std::vector<FilterSpec>* out);
    void blur_filters (std::vector<FilterSpec>* out);
    void distort_filters (std::vector<FilterSpec>* out);
    void pixelate_filters (std::vector<FilterSpec>* out);
    void render_filters (std::vector<FilterSpec>* out);
    void stylize_filters (std::vector<FilterSpec>* out);
    void other_filters (std::vector<FilterSpec>* out);
    void tone_filters (std::vector<FilterSpec>* out);
    void all_filters (std::vector<FilterSpec>* out);

    /* Parameters. */
    float fparam (const Value& p, const char* key, float fallback);
    bool frange (
        const Value& p, const char* key, float lo, float hi, float fallback, float* out, const char* cmd, Error* err);
    bool choice (const Value& p, const char* key, const char* const* ids, int n, int fallback, int* out,
        const char* cmd, Error* err);
    uint64_t seed_param (const Value& p);

    /* Planes. */
    size_t colors_of (const Plane& pl);
    std::vector<float> channel_of (const Plane& pl, size_t c);
    void set_channel (Plane* pl, size_t c, const std::vector<float>& v);
    void premultiply (Plane* pl, bool on);
    void gaussian_all (Plane* pl, float sigma);
    void gaussian_colors (Plane* pl, float sigma);
    void box_pass (std::vector<float>* v, size_t w, size_t h, int r, bool rows);
    void box_all (Plane* pl, int r);
    void median_all (Plane* pl, int r);
    void motion_all (Plane* pl, float angle, float distance);
    void disc_all (Plane* pl, float radius);
    void kuwahara (Plane* pl, int r);
    void cell_point (int64_t i, int64_t j, float size, uint64_t seed, float* x, float* y);
    void nearest_cell (float x, float y, float size, uint64_t seed, int64_t* ci, int64_t* cj, float* d1, float* d2);
    void unsharp (Plane* pl, float amount, float sigma, float threshold);
    float at_clamped (const Plane& pl, const std::vector<float>& src, int x, int y, size_t c);
    void sample_bilinear (const Plane& pl, const std::vector<float>& src, float x, float y, bool wrap, float* out);
    float noise_at (uint64_t i, uint64_t c, uint64_t seed);
    float rand01 (uint64_t a, uint64_t b, uint64_t seed);
    float value_noise (float x, float y, uint64_t seed);
    float fbm (float x, float y, int octaves, uint64_t seed);

    /* Colour: the plane as straight RGBA and back, a pixel's luminance, a colour in the layer's
       channels. */
    void plane_to_rgba (const Plane& pl, const FilterCtx& cx, std::vector<float>* out);
    void plane_from_rgba (Plane* pl, const FilterCtx& cx, const std::vector<float>& rgba);
    std::vector<float> plane_luma (const Plane& pl, const FilterCtx& cx);
    void color_in (const FilterCtx& cx, const float rgba[4], float* out);
    float clamp01 (float v);
    float smoothstep (float a, float b, float x);
    void area_point (const FilterCtx& cx, float fx, float fy, float* x, float* y);
    float luma (const float* rgb);

} // namespace engine
