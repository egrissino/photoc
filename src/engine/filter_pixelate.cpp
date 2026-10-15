#include "engine/filter_ops.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

namespace engine
{

    /* Filter > Pixelate: Color Halftone, Crystallize, Facet, Fragment, Mezzotint, Pointillize.
       Cells are laid out on the canvas grid (document coordinates), so a selection filtered in two
       steps lines up. */

    /* The jittered cell grid: cell (i, j)'s point lies at a random spot inside it. */
    void cell_point (int64_t i, int64_t j, float size, uint64_t seed, float* x, float* y)
    {
        *x = ((float) i + rand01 ((uint64_t) i * 92821u, (uint64_t) j, seed)) * size;
        *y = ((float) j + rand01 ((uint64_t) j * 68917u, (uint64_t) i, seed + 1)) * size;
    }

    /* The cell point nearest (x, y), with the distances to it and to the second nearest. */
    void nearest_cell (float x, float y, float size, uint64_t seed, int64_t* ci, int64_t* cj, float* d1, float* d2)
    {
        int64_t i0 = (int64_t) floorf (x / size), j0 = (int64_t) floorf (y / size);
        *d1 = *d2 = 1e30f;
        *ci       = i0;
        *cj       = j0;
        for (int64_t j = j0 - 1; j <= j0 + 1; j++)
        {
            for (int64_t i = i0 - 1; i <= i0 + 1; i++)
            {
                float px, py;
                cell_point (i, j, size, seed, &px, &py);
                float d = sqrtf ((px - x) * (px - x) + (py - y) * (py - y));
                if (d < *d1)
                {
                    *d2 = *d1;
                    *d1 = d;
                    *ci = i;
                    *cj = j;
                }
                else if (d < *d2)
                {
                    *d2 = d;
                }
            }
        }
    }

    /* The plane's pixel at document point (x, y), clamped. */
    void doc_sample (const Plane& pl, const std::vector<float>& src, const FilterCtx& cx, float x, float y, float* out)
    {
        int px = std::min (std::max ((int) floorf (x) - cx.src.x0, 0), (int) pl.w - 1),
            py = std::min (std::max ((int) floorf (y) - cx.src.y0, 0), (int) pl.h - 1);
        memcpy (out, &src[((size_t) py * pl.w + (size_t) px) * pl.n], pl.n * sizeof (float));
    }

    bool run_crystallize (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float size;
        if (!frange (p, "cellSize", 3.0f, 300.0f, 10.0f, &size, cmd, err))
        {
            return false;
        }
        uint64_t seed          = seed_param (p);
        std::vector<float> src = pl->px;
#pragma omp parallel for schedule(dynamic, 8)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                int64_t ci, cj;
                float d1, d2, px, py;
                nearest_cell ((float) x + (float) cx.src.x0 + 0.5f, (float) yi + (float) cx.src.y0 + 0.5f, size, seed,
                    &ci, &cj, &d1, &d2);
                cell_point (ci, cj, size, seed, &px, &py);
                doc_sample (*pl, src, cx, px, py, &pl->px[((size_t) yi * pl->w + x) * pl->n]);
            }
        }
        return true;
    }

    /* Pointillize: dots of the image's colours, slightly varied, on the background colour. */
    bool run_pointillize (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float size;
        if (!frange (p, "cellSize", 3.0f, 300.0f, 5.0f, &size, cmd, err))
        {
            return false;
        }
        uint64_t seed          = seed_param (p);
        std::vector<float> src = pl->px;
        float bg[raster::MAX_CHANNELS];
        color_in (cx, cx.bg, bg);
        size_t colors = colors_of (*pl);
        float radius  = size * 0.62f;
#pragma omp parallel for schedule(dynamic, 8)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                float* o = &pl->px[((size_t) yi * pl->w + x) * pl->n];
                float dx = (float) x + (float) cx.src.x0 + 0.5f, dy = (float) yi + (float) cx.src.y0 + 0.5f;
                int64_t ci, cj;
                float d1, d2, px, py;
                nearest_cell (dx, dy, size, seed, &ci, &cj, &d1, &d2);
                float cover = clamp01 (radius - d1 + 0.5f);
                float dot[raster::MAX_CHANNELS];
                cell_point (ci, cj, size, seed, &px, &py);
                doc_sample (*pl, src, cx, px, py, dot);
                for (size_t c = 0; c < colors; c++)
                {
                    float jitter = (rand01 ((uint64_t) (ci * 31 + cj), c, seed + 9) - 0.5f) * 0.2f;
                    float v      = clamp01 (dot[c] + jitter);
                    o[c]         = bg[c] + (v - bg[c]) * cover;
                }
                if (pl->alpha)
                {
                    o[pl->n - 1] = 1.0f;
                }
            }
        }
        return true;
    }

    /* Kuwahara: each pixel the mean of whichever of its four (r+1)-square quadrants varies least
       (summed-area tables keep it fast at any radius). Colour channels only. */
    void kuwahara (Plane* pl, int r)
    {
        size_t w = pl->w, h = pl->h, colors = colors_of (*pl), W = w + 1;
        std::vector<double> sum (W * (h + 1) * colors, 0.0), sq (W * (h + 1), 0.0);
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                const float* q = &pl->px[(y * w + x) * pl->n];
                double l       = 0;
                for (size_t c = 0; c < colors; c++)
                {
                    size_t at = ((y + 1) * W + x + 1) * colors + c;
                    sum[at]   = q[c] + sum[(y * W + x + 1) * colors + c] + sum[((y + 1) * W + x) * colors + c]
                            - sum[(y * W + x) * colors + c];
                    l += q[c];
                }
                l /= (double) std::max (colors, (size_t) 1);
                sq[(y + 1) * W + x + 1] = l * l + sq[y * W + x + 1] + sq[(y + 1) * W + x] - sq[y * W + x];
            }
        }
        std::vector<float> out = pl->px;
#pragma omp parallel for schedule(dynamic, 8)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
        {
            int y = (int) yi;
            for (int x = 0; x < (int) w; x++)
            {
                double best = 1e30;
                int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
                for (int q = 0; q < 4; q++)
                {
                    int x0 = q & 1 ? x : x - r, y0 = q & 2 ? y : y - r;
                    int x1 = std::min (x0 + r, (int) w - 1) + 1, y1 = std::min (y0 + r, (int) h - 1) + 1;
                    x0       = std::max (x0, 0);
                    y0       = std::max (y0, 0);
                    double n = (double) (x1 - x0) * (y1 - y0);
                    if (n <= 0)
                    {
                        continue;
                    }
                    double s = 0;
                    for (size_t c = 0; c < colors; c++)
                    {
                        s += sum[((size_t) y1 * W + (size_t) x1) * colors + c]
                           - sum[((size_t) y0 * W + (size_t) x1) * colors + c]
                           - sum[((size_t) y1 * W + (size_t) x0) * colors + c]
                           + sum[((size_t) y0 * W + (size_t) x0) * colors + c];
                    }
                    double mean = s / n / (double) std::max (colors, (size_t) 1);
                    double s2   = sq[(size_t) y1 * W + (size_t) x1] - sq[(size_t) y0 * W + (size_t) x1]
                              - sq[(size_t) y1 * W + (size_t) x0] + sq[(size_t) y0 * W + (size_t) x0];
                    double var = s2 / n - mean * mean;
                    if (var < best)
                    {
                        best = var;
                        bx0  = x0;
                        by0  = y0;
                        bx1  = x1;
                        by1  = y1;
                    }
                }
                double n = (double) (bx1 - bx0) * (by1 - by0);
                if (n <= 0)
                {
                    continue;
                }
                for (size_t c = 0; c < colors; c++)
                {
                    double s = sum[((size_t) by1 * W + (size_t) bx1) * colors + c]
                             - sum[((size_t) by0 * W + (size_t) bx1) * colors + c]
                             - sum[((size_t) by1 * W + (size_t) bx0) * colors + c]
                             + sum[((size_t) by0 * W + (size_t) bx0) * colors + c];
                    out[((size_t) y * w + (size_t) x) * pl->n + c] = (float) (s / n);
                }
            }
        }
        pl->px = out;
    }

    bool run_facet (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        kuwahara (pl, 2);
        return true;
    }

    /* Fragment: four copies offset diagonally by 4 pixels, averaged. */
    bool run_fragment (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        std::vector<float> src = pl->px;
        const int OFF[4][2]    = {{-4, -4}, {4, -4}, {-4, 4}, {4, 4}};
        for (size_t y = 0; y < pl->h; y++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                for (size_t c = 0; c < pl->n; c++)
                {
                    float acc = 0;
                    for (int k = 0; k < 4; k++)
                    {
                        acc += at_clamped (*pl, src, (int) x + OFF[k][0], (int) y + OFF[k][1], c);
                    }
                    pl->px[(y * pl->w + x) * pl->n + c] = acc * 0.25f;
                }
            }
        }
        return true;
    }

    /* Color Halftone: every channel screened at its own angle into dots whose area follows the
       channel's value (ink in CMYK, light in RGB). */
    bool run_color_halftone (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float radius, ang[4];
        if (!frange (p, "maxRadius", 4.0f, 127.0f, 8.0f, &radius, cmd, err)
            || !frange (p, "channel1", -360.0f, 360.0f, 108.0f, &ang[0], cmd, err)
            || !frange (p, "channel2", -360.0f, 360.0f, 162.0f, &ang[1], cmd, err)
            || !frange (p, "channel3", -360.0f, 360.0f, 90.0f, &ang[2], cmd, err)
            || !frange (p, "channel4", -360.0f, 360.0f, 45.0f, &ang[3], cmd, err))
        {
            return false;
        }
        std::vector<float> src = pl->px;
        size_t colors          = colors_of (*pl);
        float cell             = radius * 2.0f / 1.4142f;
        for (size_t c = 0; c < colors; c++)
        {
            float a = ang[c % 4] * (float) M_PI / 180.0f, ca = cosf (a), sa = sinf (a);
#pragma omp parallel for schedule(dynamic, 8)
            for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
            {
                for (size_t x = 0; x < pl->w; x++)
                {
                    float dx = (float) x + (float) cx.src.x0 + 0.5f, dy = (float) yi + (float) cx.src.y0 + 0.5f;
                    /* Into the screen's rotated grid, to the nearest dot centre, and back. */
                    float u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
                    float cu = (floorf (u / cell) + 0.5f) * cell, cv = (floorf (v / cell) + 0.5f) * cell;
                    float px = cu * ca - cv * sa, py = cu * sa + cv * ca;
                    float q[raster::MAX_CHANNELS];
                    int sx = std::min (std::max ((int) floorf (px) - cx.src.x0, 0), (int) pl->w - 1),
                        sy = std::min (std::max ((int) floorf (py) - cx.src.y0, 0), (int) pl->h - 1);
                    memcpy (q, &src[((size_t) sy * pl->w + (size_t) sx) * pl->n], pl->n * sizeof (float));
                    float dot                                     = radius * sqrtf (clamp01 (q[c]));
                    float d                                       = sqrtf ((u - cu) * (u - cu) + (v - cv) * (v - cv));
                    pl->px[((size_t) yi * pl->w + x) * pl->n + c] = clamp01 (dot - d + 0.5f);
                }
            }
        }
        return true;
    }

    /* Mezzotint: each channel turned to pure dots, lines or strokes, more of them where it is
       brighter. */
    bool run_mezzotint (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const TYPES[10] = {"fineDots", "mediumDots", "grainyDots", "coarseDots", "shortLines",
            "mediumLines", "longLines", "shortStrokes", "mediumStrokes", "longStrokes"};
        int type;
        if (!choice (p, "type", TYPES, 10, 0, &type, cmd, err))
        {
            return false;
        }
        uint64_t seed    = seed_param (p);
        size_t colors    = colors_of (*pl);
        const int DOT[4] = {1, 2, 1, 3}, RUN[3] = {4, 10, 24};
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                int64_t X = (int64_t) x + cx.src.x0, Y = (int64_t) yi + cx.src.y0;
                for (size_t c = 0; c < colors; c++)
                {
                    uint64_t key;
                    if (type < 4)
                    {
                        int b = DOT[type];
                        key   = (uint64_t) ((Y / b) * 100003 + X / b);
                        if (type == 2)
                        {
                            key = (uint64_t) (Y * 100003 + X) / 2;
                        }
                    }
                    else if (type < 7)
                    {
                        int run = RUN[type - 4];
                        key = (uint64_t) (Y * 100003 + (X + (int64_t) (rand01 ((uint64_t) Y, 77, seed) * 64)) / run);
                    }
                    else
                    {
                        int run   = RUN[type - 7];
                        int64_t d = X - Y;
                        key = (uint64_t) (d * 100003 + (Y + (int64_t) (rand01 ((uint64_t) d, 78, seed) * 64)) / run);
                    }
                    float t  = rand01 (key, c, seed);
                    float* v = &pl->px[((size_t) yi * pl->w + x) * pl->n + c];
                    *v       = *v > t ? 1.0f : 0.0f;
                }
            }
        }
        return true;
    }

    const FilterSpec PIXELATE_FILTERS[] = {
        {"filter.pixelate.colorHalftone", "Color Halftone…", "Filter/Pixelate",
            "{\"maxRadius\":4..127=8,\"channel1\":deg=108,\"channel2\":deg=162,\"channel3\":deg=90,\"channel4\":deg="
            "45}",
            run_color_halftone, false, Reach::Params},
        {"filter.pixelate.crystallize", "Crystallize…", "Filter/Pixelate", "{\"cellSize\":3..300=10,\"seed\":int}",
            run_crystallize, false, Reach::Params},
        {"filter.pixelate.facet", "Facet", "Filter/Pixelate", "{}", run_facet, false, Reach::Params},
        {"filter.pixelate.fragment", "Fragment", "Filter/Pixelate", "{}", run_fragment, true, Reach::Params},
        {"filter.pixelate.mezzotint", "Mezzotint…", "Filter/Pixelate",
            "{\"type\":\"fineDots|mediumDots|grainyDots|coarseDots|shortLines|mediumLines|longLines|shortStrokes|"
            "mediumStrokes|longStrokes\",\"seed\":int}",
            run_mezzotint, false, Reach::None},
        {"filter.pixelate.pointillize", "Pointillize…", "Filter/Pixelate", "{\"cellSize\":3..300=5,\"seed\":int}",
            run_pointillize, false, Reach::Params},
    };

    void pixelate_filters (std::vector<FilterSpec>* out)
    {
        out->insert (
            out->end (), PIXELATE_FILTERS, PIXELATE_FILTERS + sizeof PIXELATE_FILTERS / sizeof PIXELATE_FILTERS[0]);
    }

} // namespace engine
