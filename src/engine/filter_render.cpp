#include "compose/masks.h"
#include "engine/filter_ops.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

namespace engine
{

    /* Filter > Render: Clouds and Difference Clouds (fractal noise between the foreground and
       background colours), Fibers, Lens Flare and Lighting Effects (one light, set by numbers). They
       work in RGB and convert back to the layer's mode; the noise is laid out in document
       coordinates. */

    /* Cloud value at document point (x, y): 0..1, stretched to use the whole range. */
    float cloud_at (float x, float y, uint64_t seed)
    {
        float c = fbm (x / 256.0f, y / 256.0f, 9, seed);
        return clamp01 ((c - 0.5f) * 2.4f + 0.5f);
    }

    void mix_colors (const float* a, const float* b, float t, float* out)
    {
        for (int k = 0; k < 3; k++)
        {
            out[k] = a[k] + (b[k] - a[k]) * t;
        }
    }

    bool run_clouds (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        uint64_t seed = seed_param (p);
        bool diff     = strcmp (cmd, "filter.render.differenceClouds") == 0;
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
#pragma omp parallel for schedule(dynamic, 16)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                float* o = &rgba[((size_t) yi * pl->w + x) * 4];
                float v  = cloud_at ((float) x + (float) cx.src.x0, (float) yi + (float) cx.src.y0, seed), c[3];
                mix_colors (cx.fg, cx.bg, v, c);
                if (diff)
                {
                    for (int k = 0; k < 3; k++)
                    {
                        o[k] = fabsf (o[k] - c[k]);
                    }
                }
                else
                {
                    for (int k = 0; k < 3; k++)
                    {
                        o[k] = c[k];
                    }
                    o[3] = 1.0f;
                }
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* Fibers: streaks running down the area; Variance sets how quickly the colour changes across,
       Strength how long the fibers run. */
    bool run_fibers (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float variance, strength;
        if (!frange (p, "variance", 1.0f, 64.0f, 16.0f, &variance, cmd, err)
            || !frange (p, "strength", 1.0f, 64.0f, 4.0f, &strength, cmd, err))
        {
            return false;
        }
        uint64_t seed = seed_param (p);
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        float fx = 0.02f + variance * 0.03f, fy = 1.0f / (strength * 6.0f);
#pragma omp parallel for schedule(dynamic, 16)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                float X = (float) x + (float) cx.src.x0, Y = (float) yi + (float) cx.src.y0;
                float v  = fbm (X * fx, Y * fy, 6, seed);
                v        = clamp01 ((v - 0.5f) * 2.6f + 0.5f);
                float* o = &rgba[((size_t) yi * pl->w + x) * 4];
                mix_colors (cx.fg, cx.bg, v, o);
                o[3] = 1.0f;
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    float gauss2 (float d, float r)
    {
        return expf (-(d * d) / (r * r));
    }

    /* Lens Flare: a bright core, its glow and ring, a streak for the movie lens, and ghost discs
       along the line through the image centre, screened over the image. */
    bool run_lens_flare (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const LENSES[4] = {"zoom50300", "prime35", "prime105", "moviePrime"};
        float bright, fx, fy;
        int lens;
        if (!frange (p, "brightness", 10.0f, 300.0f, 100.0f, &bright, cmd, err)
            || !frange (p, "centerX", 0.0f, 1.0f, 0.3f, &fx, cmd, err)
            || !frange (p, "centerY", 0.0f, 1.0f, 0.3f, &fy, cmd, err)
            || !choice (p, "lens", LENSES, 4, 0, &lens, cmd, err))
        {
            return false;
        }
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        float lx, ly, mx, my;
        area_point (cx, fx, fy, &lx, &ly);
        area_point (cx, 0.5f, 0.5f, &mx, &my);
        float diag                = sqrtf ((float) cx.area.width () * (float) cx.area.width ()
                                           + (float) cx.area.height () * (float) cx.area.height ());
        float k                   = bright / 100.0f;
        const float GHOST_T[5]    = {0.45f, 0.75f, 1.15f, 1.45f, 1.9f},
                    GHOST_R[5]    = {0.012f, 0.03f, 0.018f, 0.05f, 0.025f};
        const float GHOST_C[5][3] = {
            {0.3f, 0.5f, 1.0f}, {0.4f, 1.0f, 0.5f}, {1.0f, 0.6f, 0.3f}, {0.5f, 0.4f, 1.0f}, {1.0f, 0.4f, 0.6f}};
        float core = diag * (lens == 1 ? 0.025f : lens == 2 ? 0.018f : 0.02f);
#pragma omp parallel for schedule(dynamic, 16)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                float dx = (float) x - lx, dy = (float) yi - ly, d = sqrtf (dx * dx + dy * dy);
                float light[3] = {0, 0, 0};
                float c0       = gauss2 (d, core) + 0.35f * expf (-d / (diag * 0.12f));
                float warm[3]  = {1.0f, 0.92f, 0.8f};
                for (int c = 0; c < 3; c++)
                {
                    light[c] += c0 * warm[c];
                }
                float ring = 0.18f * gauss2 (d - diag * 0.11f, diag * 0.008f);
                light[0] += ring * 0.8f;
                light[1] += ring * 0.6f;
                light[2] += ring;
                if (lens == 3)
                {
                    float streak = 0.5f * expf (-fabsf (dy) / 1.5f) * expf (-fabsf (dx) / (diag * 0.35f));
                    light[0] += streak * 0.7f;
                    light[1] += streak * 0.8f;
                    light[2] += streak;
                }
                for (int g = 0; g < 5; g++)
                {
                    if (lens == 2 && g == 3)
                    {
                        continue;
                    }
                    float gx = lx + (mx - lx) * GHOST_T[g], gy = ly + (my - ly) * GHOST_T[g];
                    float gd = sqrtf (((float) x - gx) * ((float) x - gx) + ((float) yi - gy) * ((float) yi - gy)),
                          gr = diag * GHOST_R[g];
                    float a  = 0.12f * smoothstep (gr, gr * 0.8f, gd);
                    for (int c = 0; c < 3; c++)
                    {
                        light[c] += a * GHOST_C[g][c];
                    }
                }
                float* o = &rgba[((size_t) yi * pl->w + x) * 4];
                for (int c = 0; c < 3; c++)
                {
                    o[c] = 1.0f - (1.0f - o[c]) * (1.0f - clamp01 (light[c] * k));
                }
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* Lighting Effects: one Omni, Spot or Infinite light over the image's own brightness as a bump
       texture (Height), plus ambient light. */
    bool run_lighting (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const TYPES[3] = {"spot", "omni", "infinite"};
        float intensity, fx, fy, radius, angle, ambient, height;
        int type;
        if (!choice (p, "type", TYPES, 3, 0, &type, cmd, err)
            || !frange (p, "intensity", 0.0f, 100.0f, 35.0f, &intensity, cmd, err)
            || !frange (p, "centerX", 0.0f, 1.0f, 0.5f, &fx, cmd, err)
            || !frange (p, "centerY", 0.0f, 1.0f, 0.5f, &fy, cmd, err)
            || !frange (p, "radius", 0.01f, 2.0f, 0.5f, &radius, cmd, err)
            || !frange (p, "angle", -360.0f, 360.0f, -45.0f, &angle, cmd, err)
            || !frange (p, "ambience", -100.0f, 100.0f, 8.0f, &ambient, cmd, err)
            || !frange (p, "height", 0.0f, 100.0f, 0.0f, &height, cmd, err))
        {
            return false;
        }
        float lc[4] = {1, 1, 1, 1};
        if (param_present (p, "color"))
        {
            std::string hex;
            if (!param_str (p, "color", &hex) || !parse_hex (hex.c_str (), lc))
            {
                return bad_params (err, cmd, "`color` must be \"#rrggbb\"");
            }
        }
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        std::vector<float> lum = plane_luma (*pl, cx);
        compose::gaussian (lum.data (), pl->w, pl->h, 1.0f);
        float lx, ly;
        area_point (cx, fx, fy, &lx, &ly);
        float diag  = sqrtf ((float) cx.area.width () * (float) cx.area.width ()
                             + (float) cx.area.height () * (float) cx.area.height ());
        float range = radius * diag, lz = range * 0.4f;
        float a = angle * (float) M_PI / 180.0f, sdx = cosf (a), sdy = -sinf (a);
        float amb = (ambient + 100.0f) / 200.0f * 0.5f, gain = intensity / 35.0f;
        float bump = height / 100.0f * 12.0f;
        int w = (int) pl->w, h = (int) pl->h;
#pragma omp parallel for schedule(dynamic, 16)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) h; yi++)
        {
            int y = (int) yi;
            for (int x = 0; x < w; x++)
            {
                /* The surface normal from the brightness bumps. */
                float hx = lum[(size_t) y * pl->w + (size_t) std::min (x + 1, w - 1)]
                         - lum[(size_t) y * pl->w + (size_t) std::max (x - 1, 0)];
                float hy = lum[(size_t) std::min (y + 1, h - 1) * pl->w + (size_t) x]
                         - lum[(size_t) std::max (y - 1, 0) * pl->w + (size_t) x];
                float nx = -hx * bump, ny = -hy * bump, nz = 1.0f, nl = sqrtf (nx * nx + ny * ny + nz * nz);
                nx /= nl;
                ny /= nl;
                nz /= nl;
                float vx, vy, vz, fall = 1.0f;
                if (type == 2)
                {
                    vx = sdx * 0.7071f;
                    vy = sdy * 0.7071f;
                    vz = 0.7071f;
                }
                else
                {
                    vx       = lx - (float) x;
                    vy       = ly - (float) y;
                    vz       = lz;
                    float d2 = sqrtf (vx * vx + vy * vy);
                    fall     = 1.0f - smoothstep (range * 0.2f, range, d2);
                    if (type == 0)
                    {
                        /* A spot lights an elliptical pool, longer along `angle`. */
                        float along = (-vx) * sdx + (-vy) * sdy, across = (-vx) * sdy - (-vy) * sdx;
                        float e = sqrtf (
                            (along / range) * (along / range) + (across / (range * 0.6f)) * (across / (range * 0.6f)));
                        fall = 1.0f - smoothstep (0.5f, 1.0f, e);
                    }
                    float vl = sqrtf (vx * vx + vy * vy + vz * vz);
                    vx /= vl;
                    vy /= vl;
                    vz /= vl;
                }
                float diffuse = std::max (nx * vx + ny * vy + nz * vz, 0.0f) * fall * gain;
                float* o      = &rgba[((size_t) y * pl->w + (size_t) x) * 4];
                for (int c = 0; c < 3; c++)
                {
                    o[c] = clamp01 (o[c] * (amb + diffuse * lc[c]));
                }
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    const FilterSpec RENDER_FILTERS[] = {
        {"filter.render.clouds", "Clouds", "Filter/Render", "{\"seed\":int} (foreground to background colour)",
            run_clouds, false, Reach::None},
        {"filter.render.differenceClouds", "Difference Clouds", "Filter/Render", "{\"seed\":int}", run_clouds, false,
            Reach::None},
        {"filter.render.fibers", "Fibers…", "Filter/Render",
            "{\"variance\":1..64=16,\"strength\":1..64=4,\"seed\":int}", run_fibers, false, Reach::None},
        {"filter.render.lensFlare", "Lens Flare…", "Filter/Render",
            "{\"brightness\":10..300=100,\"centerX\":0..1=0.3,\"centerY\":0..1=0.3,\"lens\":\"zoom50300|prime35|"
            "prime105|moviePrime\"}",
            run_lens_flare, false, Reach::Canvas},
        {"filter.render.lightingEffects", "Lighting Effects…", "Filter/Render",
            "{\"type\":\"spot|omni|infinite\",\"intensity\":0..100=35,\"color\":\"#rrggbb\"=\"#ffffff\",\"centerX\":0.."
            "1=0.5,\"centerY\":0..1=0.5,\"radius\":0.01..2=0.5,"
            "\"angle\":deg=-45,\"ambience\":-100..100=8,\"height\":0..100=0}",
            run_lighting, false, Reach::Canvas},
    };

    void render_filters (std::vector<FilterSpec>* out)
    {
        out->insert (out->end (), RENDER_FILTERS, RENDER_FILTERS + sizeof RENDER_FILTERS / sizeof RENDER_FILTERS[0]);
    }

} // namespace engine
