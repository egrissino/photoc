#include "compose/masks.h"
#include "engine/filter_ops.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

namespace engine
{

    /* Filter > Stylize: Diffuse, Extrude, Oil Paint, Tiles, Trace Contour, Wind. */

    /* Diffuse: pixels swapped with random neighbours; Darken / Lighten Only take a neighbour only
       when it is darker / lighter, Anisotropic the most similar one. */
    bool run_diffuse (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const MODES[4] = {"normal", "darkenOnly", "lightenOnly", "anisotropic"};
        int mode;
        if (!choice (p, "mode", MODES, 4, 0, &mode, cmd, err))
        {
            return false;
        }
        uint64_t seed          = seed_param (p);
        std::vector<float> src = pl->px;
        size_t colors = colors_of (*pl), n = pl->n;
        int w = (int) pl->w, h = (int) pl->h;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
        {
            int y = (int) yi;
            for (int x = 0; x < w; x++)
            {
                uint64_t key   = (uint64_t) (y + cx.src.y0) * 1000003u + (uint64_t) (x + cx.src.x0);
                const float* o = &src[((size_t) y * pl->w + (size_t) x) * n];
                int bx = x, by = y;
                if (mode == 3)
                {
                    float best = 1e30f;
                    for (int k = 0; k < 8; k++)
                    {
                        int dx = (int) (rand01 (key, (uint64_t) k * 2, seed) * 5.0f) - 2,
                            dy = (int) (rand01 (key, (uint64_t) k * 2 + 1, seed) * 5.0f) - 2;
                        if (dx == 0 && dy == 0)
                        {
                            continue;
                        }
                        int sx = std::min (std::max (x + dx, 0), w - 1), sy = std::min (std::max (y + dy, 0), h - 1);
                        const float* q = &src[((size_t) sy * pl->w + (size_t) sx) * n];
                        float d        = 0;
                        for (size_t c = 0; c < colors; c++)
                        {
                            d += fabsf (q[c] - o[c]);
                        }
                        if (d < best)
                        {
                            best = d;
                            bx   = sx;
                            by   = sy;
                        }
                    }
                }
                else
                {
                    int dx = (int) (rand01 (key, 0, seed) * 5.0f) - 2, dy = (int) (rand01 (key, 1, seed) * 5.0f) - 2;
                    bx = std::min (std::max (x + dx, 0), w - 1);
                    by = std::min (std::max (y + dy, 0), h - 1);
                    if (mode != 0)
                    {
                        const float* q = &src[((size_t) by * pl->w + (size_t) bx) * n];
                        float lo = 0, lq = 0;
                        for (size_t c = 0; c < colors; c++)
                        {
                            lo += o[c];
                            lq += q[c];
                        }
                        if ((mode == 1 && lq >= lo) || (mode == 2 && lq <= lo))
                        {
                            bx = x;
                            by = y;
                        }
                    }
                }
                memcpy (&pl->px[((size_t) y * pl->w + (size_t) x) * n], &src[((size_t) by * pl->w + (size_t) bx) * n],
                    n * sizeof (float));
            }
        }
        return true;
    }

    struct ExtrudeBlock
    {
        float depth;
        int x0;
        int y0;
        int x1;
        int y1;
        float color[3];
    };

    bool block_before (const ExtrudeBlock& a, const ExtrudeBlock& b)
    {
        return a.depth < b.depth;
    }

    void fill_square (std::vector<float>* rgba, size_t w, size_t h, float cx, float cy, float half, const float* color)
    {
        int x0 = std::max ((int) floorf (cx - half), 0), x1 = std::min ((int) ceilf (cx + half), (int) w);
        int y0 = std::max ((int) floorf (cy - half), 0), y1 = std::min ((int) ceilf (cy + half), (int) h);
        for (int y = y0; y < y1; y++)
        {
            for (int x = x0; x < x1; x++)
            {
                float* o = &(*rgba)[((size_t) y * w + (size_t) x) * 4];
                o[0]     = color[0];
                o[1]     = color[1];
                o[2]     = color[2];
                o[3]     = 1.0f;
            }
        }
    }

    /* Extrude: the area cut into cells raised toward the viewer as blocks (sides shaded, nearest
       drawn last) or pyramids (four lit faces). Depth is random or follows brightness. */
    bool run_extrude (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const TYPES[2]  = {"blocks", "pyramids"};
        const char* const DEPTHS[2] = {"random", "levelBased"};
        float size, depth;
        int type, dmode;
        if (!choice (p, "type", TYPES, 2, 0, &type, cmd, err)
            || !frange (p, "size", 2.0f, 255.0f, 30.0f, &size, cmd, err)
            || !frange (p, "depth", 1.0f, 255.0f, 30.0f, &depth, cmd, err)
            || !choice (p, "depthMode", DEPTHS, 2, 0, &dmode, cmd, err))
        {
            return false;
        }
        bool solid = false, mask_incomplete = false;
        param_bool (p, "solidFronts", &solid);
        param_bool (p, "maskIncomplete", &mask_incomplete);
        uint64_t seed = seed_param (p);
        std::vector<float> rgba, orig;
        plane_to_rgba (*pl, cx, &rgba);
        orig  = rgba;
        int s = (int) size, ax0 = cx.area.x0 - cx.src.x0, ay0 = cx.area.y0 - cx.src.y0,
            ax1 = ax0 + (int) cx.area.width (), ay1 = ay0 + (int) cx.area.height ();
        float mx = (ax0 + ax1) * 0.5f, my = (ay0 + ay1) * 0.5f;
        std::vector<ExtrudeBlock> blocks;
        for (int y = ay0; y < ay1; y += s)
        {
            for (int x = ax0; x < ax1; x += s)
            {
                ExtrudeBlock b = {0, x, y, std::min (x + s, ax1), std::min (y + s, ay1), {0, 0, 0}};
                bool partial   = b.x1 - b.x0 < s || b.y1 - b.y0 < s;
                if (mask_incomplete && partial)
                {
                    continue;
                }
                double acc[3] = {0, 0, 0};
                int cnt       = 0;
                for (int yy = b.y0; yy < b.y1; yy++)
                {
                    for (int xx = b.x0; xx < b.x1; xx++)
                    {
                        for (int c = 0; c < 3; c++)
                        {
                            acc[c] += orig[((size_t) yy * pl->w + (size_t) xx) * 4 + c];
                        }
                        cnt++;
                    }
                }
                for (int c = 0; c < 3; c++)
                {
                    b.color[c] = (float) (acc[c] / std::max (cnt, 1));
                }
                b.depth = dmode == 0 ? rand01 ((uint64_t) x, (uint64_t) y, seed) : luma (b.color);
                blocks.push_back (b);
            }
        }
        /* Everything the blocks cover starts black, as the gaps between them show. */
        for (int y = ay0; y < ay1; y++)
        {
            for (int x = ax0; x < ax1; x++)
            {
                float* o     = &rgba[((size_t) y * pl->w + (size_t) x) * 4];
                bool skipped = false;
                if (mask_incomplete)
                {
                    int bx = (x - ax0) / s, by = (y - ay0) / s;
                    skipped = ax0 + (bx + 1) * s > ax1 || ay0 + (by + 1) * s > ay1;
                }
                if (skipped)
                {
                    continue;
                }
                o[0] = o[1] = o[2] = 0.0f;
                o[3]               = 1.0f;
            }
        }
        if (type == 1)
        {
            for (size_t i = 0; i < blocks.size (); i++)
            {
                const ExtrudeBlock& b = blocks[i];
                float bcx = (b.x0 + b.x1) * 0.5f, bcy = (b.y0 + b.y1) * 0.5f;
                for (int y = b.y0; y < b.y1; y++)
                {
                    for (int x = b.x0; x < b.x1; x++)
                    {
                        float dx = (float) x + 0.5f - bcx, dy = (float) y + 0.5f - bcy;
                        float shade = fabsf (dx) > fabsf (dy) ? (dx > 0 ? 0.65f : 0.9f) : (dy > 0 ? 0.45f : 1.0f);
                        float k     = 1.0f - (1.0f - shade) * std::min (b.depth * depth / 128.0f, 1.0f);
                        float* o    = &rgba[((size_t) y * pl->w + (size_t) x) * 4];
                        for (int c = 0; c < 3; c++)
                        {
                            o[c] = b.color[c] * k;
                        }
                    }
                }
            }
        }
        else
        {
            std::sort (blocks.begin (), blocks.end (), block_before);
            const int STEPS = 8;
            for (size_t i = 0; i < blocks.size (); i++)
            {
                const ExtrudeBlock& b = blocks[i];
                float bcx = (b.x0 + b.x1) * 0.5f, bcy = (b.y0 + b.y1) * 0.5f, half = (float) s * 0.5f;
                float lift = b.depth * depth / 255.0f;
                float fx = bcx + (bcx - mx) * lift * 0.6f, fy = bcy + (bcy - my) * lift * 0.6f,
                      fh = half * (1.0f + lift * 0.5f);
                for (int k = 0; k <= STEPS; k++)
                {
                    float t = (float) k / STEPS, side[3];
                    for (int c = 0; c < 3; c++)
                    {
                        side[c] = b.color[c] * (0.35f + 0.35f * t);
                    }
                    fill_square (
                        &rgba, pl->w, pl->h, bcx + (fx - bcx) * t, bcy + (fy - bcy) * t, half + (fh - half) * t, side);
                }
                if (solid)
                {
                    fill_square (&rgba, pl->w, pl->h, fx, fy, fh, b.color);
                }
                else
                {
                    /* The cell's own pixels on the front, scaled to it. */
                    int x0 = std::max ((int) floorf (fx - fh), 0), x1 = std::min ((int) ceilf (fx + fh), (int) pl->w);
                    int y0 = std::max ((int) floorf (fy - fh), 0), y1 = std::min ((int) ceilf (fy + fh), (int) pl->h);
                    for (int y = y0; y < y1; y++)
                    {
                        for (int x = x0; x < x1; x++)
                        {
                            int sx = std::min (
                                std::max (
                                    b.x0 + (int) (((float) x + 0.5f - (fx - fh)) / (2 * fh) * (float) (b.x1 - b.x0)),
                                    b.x0),
                                b.x1 - 1);
                            int sy = std::min (
                                std::max (
                                    b.y0 + (int) (((float) y + 0.5f - (fy - fh)) / (2 * fh) * (float) (b.y1 - b.y0)),
                                    b.y0),
                                b.y1 - 1);
                            memcpy (&rgba[((size_t) y * pl->w + (size_t) x) * 4],
                                &orig[((size_t) sy * pl->w + (size_t) sx) * 4], 3 * sizeof (float));
                            rgba[((size_t) y * pl->w + (size_t) x) * 4 + 3] = 1.0f;
                        }
                    }
                }
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* Oil Paint: flattened into strokes (Kuwahara, Stylization the stroke size, Cleanliness a
       smoothing), then lit as raised paint with brush-bristle grooves. */
    bool run_oil_paint (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float style, clean, scale, bristle, angle, shine;
        if (!frange (p, "stylization", 0.1f, 10.0f, 2.5f, &style, cmd, err)
            || !frange (p, "cleanliness", 0.0f, 10.0f, 5.0f, &clean, cmd, err)
            || !frange (p, "scale", 0.1f, 10.0f, 1.0f, &scale, cmd, err)
            || !frange (p, "bristleDetail", 0.0f, 10.0f, 2.0f, &bristle, cmd, err)
            || !frange (p, "angle", -180.0f, 180.0f, -60.0f, &angle, cmd, err)
            || !frange (p, "shine", 0.0f, 10.0f, 1.0f, &shine, cmd, err))
        {
            return false;
        }
        bool lighting = true;
        param_bool (p, "lighting", &lighting);
        kuwahara (pl, std::max (1, (int) lroundf (1.0f + style * 1.2f)));
        if (clean > 0.0f)
        {
            Plane soft = *pl;
            gaussian_colors (&soft, clean * 0.25f);
            kuwahara (&soft, 2);
            pl->px = soft.px;
        }
        if (!lighting || shine <= 0.0f)
        {
            return true;
        }
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        std::vector<float> height = plane_luma (*pl, cx);
        compose::gaussian (height.data (), pl->w, pl->h, scale);
        uint64_t seed = 99;
        for (size_t y = 0; y < pl->h; y++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                float X = (float) x + (float) cx.src.x0, Y = (float) y + (float) cx.src.y0;
                height[y * pl->w + x] += (fbm (X * 0.6f / scale, Y * 0.08f / scale, 3, seed) - 0.5f) * bristle * 0.03f;
            }
        }
        float a = angle * (float) M_PI / 180.0f, lx = cosf (a), ly = -sinf (a), k = shine * 1.5f;
        int w = (int) pl->w, h = (int) pl->h;
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                float gx = height[(size_t) y * pl->w + (size_t) std::min (x + 1, w - 1)]
                         - height[(size_t) y * pl->w + (size_t) std::max (x - 1, 0)];
                float gy = height[(size_t) std::min (y + 1, h - 1) * pl->w + (size_t) x]
                         - height[(size_t) std::max (y - 1, 0) * pl->w + (size_t) x];
                float s  = -(gx * lx + gy * ly) * k;
                float* o = &rgba[((size_t) y * pl->w + (size_t) x) * 4];
                for (int c = 0; c < 3; c++)
                {
                    o[c] = clamp01 (o[c] + s);
                }
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* Tiles: the area cut into squares (Number across its short side) shifted at random by up to
       Maximum Offset percent, the gaps filled with a colour, the inverse image or the image. */
    bool run_tiles (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const FILLS[4] = {"background", "foreground", "inverse", "unaltered"};
        float number, offset;
        int fill;
        if (!frange (p, "number", 1.0f, 99.0f, 10.0f, &number, cmd, err)
            || !frange (p, "maxOffset", 1.0f, 99.0f, 10.0f, &offset, cmd, err)
            || !choice (p, "fill", FILLS, 4, 0, &fill, cmd, err))
        {
            return false;
        }
        uint64_t seed = seed_param (p);
        std::vector<float> rgba, orig;
        plane_to_rgba (*pl, cx, &rgba);
        orig    = rgba;
        int ax0 = cx.area.x0 - cx.src.x0, ay0 = cx.area.y0 - cx.src.y0, ax1 = ax0 + (int) cx.area.width (),
            ay1  = ay0 + (int) cx.area.height ();
        int tile = std::max (1, (int) ((float) std::min (ax1 - ax0, ay1 - ay0) / number));
        for (int y = ay0; y < ay1; y++)
        {
            for (int x = ax0; x < ax1; x++)
            {
                float* o = &rgba[((size_t) y * pl->w + (size_t) x) * 4];
                if (fill == 0)
                {
                    memcpy (o, cx.bg, 3 * sizeof (float));
                }
                else if (fill == 1)
                {
                    memcpy (o, cx.fg, 3 * sizeof (float));
                }
                else if (fill == 2)
                {
                    for (int c = 0; c < 3; c++)
                    {
                        o[c] = 1.0f - o[c];
                    }
                }
                o[3] = 1.0f;
            }
        }
        float maxoff = (float) tile * offset / 100.0f;
        for (int ty = ay0; ty < ay1; ty += tile)
        {
            for (int tx = ax0; tx < ax1; tx += tile)
            {
                int dx = (int) lroundf ((rand01 ((uint64_t) tx, (uint64_t) ty, seed) * 2.0f - 1.0f) * maxoff);
                int dy = (int) lroundf ((rand01 ((uint64_t) ty, (uint64_t) tx, seed + 3) * 2.0f - 1.0f) * maxoff);
                for (int y = ty; y < std::min (ty + tile, ay1); y++)
                {
                    for (int x = tx; x < std::min (tx + tile, ax1); x++)
                    {
                        int X = x + dx, Y = y + dy;
                        if (X < ax0 || X >= ax1 || Y < ay0 || Y >= ay1)
                        {
                            continue;
                        }
                        memcpy (&rgba[((size_t) Y * pl->w + (size_t) X) * 4],
                            &orig[((size_t) y * pl->w + (size_t) x) * 4], 4 * sizeof (float));
                    }
                }
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* Trace Contour: per colour channel, a thin line where it crosses Level (on the side Edge
       names), white elsewhere. */
    bool run_trace_contour (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const EDGES[2] = {"lower", "upper"};
        float level;
        int edge;
        if (!frange (p, "level", 0.0f, 255.0f, 128.0f, &level, cmd, err)
            || !choice (p, "edge", EDGES, 2, 1, &edge, cmd, err))
        {
            return false;
        }
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        std::vector<float> src = rgba;
        float t                = level / 255.0f;
        int w = (int) pl->w, h = (int) pl->h;
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                for (int c = 0; c < 3; c++)
                {
                    float v    = src[((size_t) y * pl->w + (size_t) x) * 4 + c];
                    bool above = v > t, line = false;
                    const int NB[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (int k = 0; k < 4 && !line; k++)
                    {
                        int sx = x + NB[k][0], sy = y + NB[k][1];
                        if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                        {
                            continue;
                        }
                        bool nb = src[((size_t) sy * pl->w + (size_t) sx) * 4 + c] > t;
                        line    = nb != above && (edge == 1 ? above : !above);
                    }
                    rgba[((size_t) y * pl->w + (size_t) x) * 4 + c] = line ? 0.0f : 1.0f;
                }
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* Wind: bright pixels streaked downwind along their row; Blast streaks further, Stagger in
       broken runs. */
    bool run_wind (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const METHODS[3] = {"wind", "blast", "stagger"};
        const char* const DIRS[2]    = {"fromRight", "fromLeft"};
        int method, dir;
        if (!choice (p, "method", METHODS, 3, 0, &method, cmd, err)
            || !choice (p, "direction", DIRS, 2, 0, &dir, cmd, err))
        {
            return false;
        }
        uint64_t seed = seed_param (p);
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        int w      = (int) pl->w;
        float base = method == 1 ? 60.0f : 20.0f;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            float trail[3] = {0, 0, 0};
            float len      = base * (0.5f + rand01 ((uint64_t) yi + (uint64_t) cx.src.y0, 5, seed));
            float decay    = 1.0f - 1.0f / len;
            for (int i = 0; i < w; i++)
            {
                int x    = dir == 0 ? w - 1 - i : i;
                float* o = &rgba[((size_t) yi * pl->w + (size_t) x) * 4];
                if (method == 2 && rand01 ((uint64_t) yi * 7919u + (uint64_t) x, 6, seed) < 0.03f)
                {
                    trail[0] = trail[1] = trail[2] = 0.0f;
                }
                for (int c = 0; c < 3; c++)
                {
                    trail[c] = std::max (trail[c] * decay, o[c]);
                    o[c]     = trail[c];
                }
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    const FilterSpec STYLIZE_FILTERS[] = {
        {"filter.stylize.diffuse", "Diffuse…", "Filter/Stylize",
            "{\"mode\":\"normal|darkenOnly|lightenOnly|anisotropic\",\"seed\":int}", run_diffuse, true, Reach::Params},
        {"filter.stylize.extrude", "Extrude…", "Filter/Stylize",
            "{\"type\":\"blocks|pyramids\",\"size\":2..255=30,\"depth\":1..255=30,\"depthMode\":\"random|levelBased\","
            "\"solidFronts\":bool=false,\"maskIncomplete\":bool=false,\"seed\":int}",
            run_extrude, false, Reach::None},
        {"filter.stylize.oilPaint", "Oil Paint…", "Filter/Stylize",
            "{\"stylization\":0.1..10=2.5,\"cleanliness\":0..10=5,\"scale\":0.1..10=1,\"bristleDetail\":0..10=2,"
            "\"lighting\":bool=true,\"angle\":-180..180=-60,\"shine\":0..10=1}",
            run_oil_paint, false, Reach::Canvas},
        {"filter.stylize.tiles", "Tiles…", "Filter/Stylize",
            "{\"number\":1..99=10,\"maxOffset\":1..99=10,\"fill\":\"background|foreground|inverse|unaltered\",\"seed\":"
            "int}",
            run_tiles, false, Reach::None},
        {"filter.stylize.traceContour", "Trace Contour…", "Filter/Stylize",
            "{\"level\":0..255=128,\"edge\":\"lower|upper\"=\"upper\"}", run_trace_contour, false, Reach::Params},
        {"filter.stylize.wind", "Wind…", "Filter/Stylize",
            "{\"method\":\"wind|blast|stagger\",\"direction\":\"fromRight|fromLeft\",\"seed\":int}", run_wind, false,
            Reach::None},
    };

    void stylize_filters (std::vector<FilterSpec>* out)
    {
        out->insert (
            out->end (), STYLIZE_FILTERS, STYLIZE_FILTERS + sizeof STYLIZE_FILTERS / sizeof STYLIZE_FILTERS[0]);
    }

} // namespace engine
