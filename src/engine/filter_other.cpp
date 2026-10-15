#include "compose/masks.h"
#include "engine/filter_ops.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

namespace engine
{

    /* Filter > Other (Custom, HSB/HSL, Color to Alpha), Video (De-Interlace, NTSC Colors) and the
       Filter Gallery looks ported so far: Artistic > Cutout, Sketch > Graphic Pen and Stamp (in the
       foreground colour on the background colour, as in Photoshop) and Texture > Stained Glass. */

    /* Custom: a 5 x 5 convolution (row by row, the middle weight at index 12), divided by Scale
       with Offset (levels) added. */
    bool run_custom (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float k[25];
        for (int i = 0; i < 25; i++)
        {
            k[i] = i == 12 ? 1.0f : 0.0f;
        }
        const Value* kv = param (p, "kernel");
        if (kv != NULL)
        {
            if (!kv->is_array () || kv->size () != 25)
            {
                return bad_params (err, cmd, "`kernel` must be 25 numbers (5 x 5, row by row)");
            }
            for (size_t i = 0; i < 25; i++)
            {
                if (!(*kv)[i].is_number ())
                {
                    return bad_params (err, cmd, "`kernel` must be 25 numbers");
                }
                k[i] = (*kv)[i].get<float> ();
            }
        }
        float scale, offset;
        if (!frange (p, "scale", 1.0f, 9999.0f, 1.0f, &scale, cmd, err)
            || !frange (p, "offset", -9999.0f, 9999.0f, 0.0f, &offset, cmd, err))
        {
            return false;
        }
        std::vector<float> src = pl->px;
        size_t colors          = colors_of (*pl);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t yi = 0; yi < (ptrdiff_t) pl->h; yi++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                for (size_t c = 0; c < colors; c++)
                {
                    float acc = 0;
                    for (int j = 0; j < 5; j++)
                    {
                        for (int i = 0; i < 5; i++)
                        {
                            if (k[j * 5 + i] != 0.0f)
                            {
                                acc += k[j * 5 + i] * at_clamped (*pl, src, (int) x + i - 2, (int) yi + j - 2, c);
                            }
                        }
                    }
                    pl->px[((size_t) yi * pl->w + x) * pl->n + c] = clamp01 (acc / scale + offset / 255.0f);
                }
            }
        }
        return true;
    }

    void rgb_to_hsb (const float* c, float* out)
    {
        float mx = std::max (c[0], std::max (c[1], c[2])), mn = std::min (c[0], std::min (c[1], c[2])), d = mx - mn,
              h = 0;
        if (d > 0)
        {
            if (mx == c[0])
            {
                h = fmodf ((c[1] - c[2]) / d + 6.0f, 6.0f);
            }
            else if (mx == c[1])
            {
                h = (c[2] - c[0]) / d + 2.0f;
            }
            else
            {
                h = (c[0] - c[1]) / d + 4.0f;
            }
        }
        out[0] = h / 6.0f;
        out[1] = mx > 0 ? d / mx : 0;
        out[2] = mx;
    }

    void hsb_to_rgb (const float* h, float* out)
    {
        float hh = fmodf (h[0], 1.0f) * 6.0f, s = h[1], v = h[2];
        int i   = (int) floorf (hh);
        float f = hh - (float) i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
        float r[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
        memcpy (out, r[((i % 6) + 6) % 6], 3 * sizeof (float));
    }

    void rgb_to_hsl (const float* c, float* out)
    {
        float mx = std::max (c[0], std::max (c[1], c[2])), mn = std::min (c[0], std::min (c[1], c[2])),
              l = (mx + mn) * 0.5f, d = mx - mn;
        float hsb[3];
        rgb_to_hsb (c, hsb);
        out[0] = hsb[0];
        out[1] = d <= 0 ? 0 : d / (1.0f - fabsf (2 * l - 1));
        out[2] = l;
    }

    void hsl_to_rgb (const float* h, float* out)
    {
        float l = h[2], s = h[1];
        float v      = l + s * std::min (l, 1 - l);
        float hsb[3] = {h[0], v > 0 ? 2 * (1 - l / v) : 0, v};
        hsb_to_rgb (hsb, out);
    }

    /* HSB/HSL: the channels read as the input model and written in the output model's order (RGB,
       HSB or HSL in the red, green and blue channels). */
    bool run_hsb_hsl (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const MODELS[3] = {"rgb", "hsb", "hsl"};
        int in, out;
        if (!choice (p, "input", MODELS, 3, 0, &in, cmd, err) || !choice (p, "rowOrder", MODELS, 3, 1, &out, cmd, err))
        {
            return false;
        }
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        for (size_t i = 0; i < pl->w * pl->h; i++)
        {
            float *o = &rgba[i * 4], rgb[3];
            if (in == 0)
            {
                memcpy (rgb, o, sizeof rgb);
            }
            else if (in == 1)
            {
                hsb_to_rgb (o, rgb);
            }
            else
            {
                hsl_to_rgb (o, rgb);
            }
            if (out == 0)
            {
                memcpy (o, rgb, sizeof rgb);
            }
            else if (out == 1)
            {
                rgb_to_hsb (rgb, o);
            }
            else
            {
                rgb_to_hsl (rgb, o);
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* Color to Alpha: the colour taken out, each pixel made as transparent as it can be while
       still showing the same over that colour. */
    bool run_color_to_alpha (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        if (!pl->alpha)
        {
            return fail (err, ErrorKind::Other, "Color to Alpha needs a layer with transparency (not the Background)");
        }
        float ref[4] = {1, 1, 1, 1};
        if (param_present (p, "color"))
        {
            std::string hex;
            if (!param_str (p, "color", &hex) || !parse_hex (hex.c_str (), ref))
            {
                return bad_params (err, cmd, "`color` must be \"#rrggbb\"");
            }
        }
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        for (size_t i = 0; i < pl->w * pl->h; i++)
        {
            float *o = &rgba[i * 4], a = 0;
            for (int c = 0; c < 3; c++)
            {
                float d = o[c] > ref[c] ? (ref[c] < 1 ? (o[c] - ref[c]) / (1 - ref[c]) : 0)
                                        : (ref[c] > 0 ? (ref[c] - o[c]) / ref[c] : 0);
                a       = std::max (a, d);
            }
            if (a > 0)
            {
                for (int c = 0; c < 3; c++)
                {
                    o[c] = clamp01 ((o[c] - ref[c]) / a + ref[c]);
                }
            }
            o[3] *= a;
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* De-Interlace: the odd or even document rows rebuilt from their neighbours. */
    bool run_deinterlace (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const FIELDS[2] = {"odd", "even"};
        const char* const CREATE[2] = {"duplication", "interpolation"};
        int field, create;
        if (!choice (p, "eliminate", FIELDS, 2, 0, &field, cmd, err)
            || !choice (p, "create", CREATE, 2, 1, &create, cmd, err))
        {
            return false;
        }
        std::vector<float> src = pl->px;
        size_t row             = pl->w * pl->n;
        for (size_t y = 0; y < pl->h; y++)
        {
            int64_t Y = (int64_t) y + cx.src.y0;
            if ((Y & 1) != (field == 0 ? 1 : 0))
            {
                continue;
            }
            size_t above = y > 0 ? y - 1 : y + 1 < pl->h ? y + 1 : y, below = y + 1 < pl->h ? y + 1 : above;
            for (size_t i = 0; i < row; i++)
            {
                float a = src[above * row + i], b = src[below * row + i];
                pl->px[y * row + i] = create == 0 ? a : (a + b) * 0.5f;
            }
        }
        return true;
    }

    /* NTSC Colors: chroma cut back where the broadcast signal would overshoot. */
    bool run_ntsc (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        for (size_t i = 0; i < pl->w * pl->h; i++)
        {
            float* o = &rgba[i * 4];
            float y  = 0.299f * o[0] + 0.587f * o[1] + 0.114f * o[2];
            float ci = 0.596f * o[0] - 0.274f * o[1] - 0.322f * o[2],
                  cq = 0.211f * o[0] - 0.523f * o[1] + 0.312f * o[2];
            float c = sqrtf (ci * ci + cq * cq), lim = std::min (std::max (1.0f - y, 0.0f) + 0.1f, y + 0.2f) * 0.75f;
            if (c > lim && c > 0)
            {
                ci *= lim / c;
                cq *= lim / c;
            }
            o[0] = y + 0.956f * ci + 0.621f * cq;
            o[1] = y - 0.272f * ci - 0.647f * cq;
            o[2] = y - 1.106f * ci + 1.703f * cq;
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* Cutout: the image simplified and cut into a few flat tones, each the average colour of the
       pixels in its brightness band. */
    bool run_cutout (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float levels, simplicity, fidelity;
        if (!frange (p, "levels", 2.0f, 8.0f, 4.0f, &levels, cmd, err)
            || !frange (p, "edgeSimplicity", 0.0f, 10.0f, 4.0f, &simplicity, cmd, err)
            || !frange (p, "edgeFidelity", 1.0f, 3.0f, 2.0f, &fidelity, cmd, err))
        {
            return false;
        }
        if (simplicity > 0.0f)
        {
            median_all (pl, std::max (1, (int) lroundf (simplicity / fidelity)));
            gaussian_colors (pl, simplicity * 0.4f / fidelity);
        }
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        int n = (int) levels;
        std::vector<double> sums ((size_t) n * 3, 0.0), counts ((size_t) n, 0.0);
        std::vector<int> band (pl->w * pl->h);
        for (size_t i = 0; i < band.size (); i++)
        {
            int b   = std::min ((int) (luma (&rgba[i * 4]) * (float) n), n - 1);
            band[i] = b;
            for (int c = 0; c < 3; c++)
            {
                sums[(size_t) b * 3 + c] += rgba[i * 4 + c];
            }
            counts[(size_t) b] += 1;
        }
        for (size_t i = 0; i < band.size (); i++)
        {
            int b = band[i];
            for (int c = 0; c < 3; c++)
            {
                rgba[i * 4 + c] = (float) (sums[(size_t) b * 3 + c] / std::max (counts[(size_t) b], 1.0));
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    /* Foreground ink where `ink` is set, the background colour elsewhere (alpha kept). */
    void ink_plane (Plane* pl, const FilterCtx& cx, const std::vector<float>& ink)
    {
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        for (size_t i = 0; i < ink.size (); i++)
        {
            float* o = &rgba[i * 4];
            for (int c = 0; c < 3; c++)
            {
                o[c] = cx.bg[c] + (cx.fg[c] - cx.bg[c]) * ink[i];
            }
        }
        plane_from_rgba (pl, cx, rgba);
    }

    /* Graphic Pen: fine ink strokes in one direction, denser in the shadows. */
    bool run_graphic_pen (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        const char* const DIRS[4] = {"rightDiagonal", "horizontal", "leftDiagonal", "vertical"};
        float length, balance;
        int dir;
        if (!frange (p, "strokeLength", 1.0f, 15.0f, 15.0f, &length, cmd, err)
            || !frange (p, "balance", 0.0f, 100.0f, 50.0f, &balance, cmd, err)
            || !choice (p, "direction", DIRS, 4, 0, &dir, cmd, err))
        {
            return false;
        }
        uint64_t seed = seed_param (p);
        /* Streaky noise: random values smeared along the stroke direction, stretched back to a
           full range, offsets the brightness before it is cut into ink. */
        Plane strokes;
        strokes.w     = pl->w;
        strokes.h     = pl->h;
        strokes.n     = 1;
        strokes.alpha = false;
        strokes.px.resize (pl->w * pl->h);
        for (size_t i = 0; i < strokes.px.size (); i++)
        {
            strokes.px[i] = rand01 ((uint64_t) i, 3, seed);
        }
        const float ANG[4] = {45.0f, 0.0f, 135.0f, 90.0f};
        motion_all (&strokes, ANG[dir], length * 2.0f);
        double mean = 0, var = 0;
        for (size_t i = 0; i < strokes.px.size (); i++)
        {
            mean += strokes.px[i];
        }
        mean /= (double) std::max (strokes.px.size (), (size_t) 1);
        for (size_t i = 0; i < strokes.px.size (); i++)
        {
            var += (strokes.px[i] - mean) * (strokes.px[i] - mean);
        }
        float sd               = (float) sqrt (var / (double) std::max (strokes.px.size (), (size_t) 1));
        std::vector<float> lum = plane_luma (*pl, cx);
        std::vector<float> ink (lum.size ());
        float t = balance / 100.0f;
        for (size_t i = 0; i < ink.size (); i++)
        {
            float n = sd > 0 ? (strokes.px[i] - (float) mean) / sd * 0.22f : 0.0f;
            ink[i]  = lum[i] + n < t ? 1.0f : 0.0f;
        }
        ink_plane (pl, cx, ink);
        return true;
    }

    /* Stamp: the image as a rubber stamp, solid ink below the balance level. */
    bool run_stamp (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float balance, smooth;
        if (!frange (p, "balance", 0.0f, 50.0f, 25.0f, &balance, cmd, err)
            || !frange (p, "smoothness", 1.0f, 50.0f, 5.0f, &smooth, cmd, err))
        {
            return false;
        }
        std::vector<float> lum = plane_luma (*pl, cx);
        compose::gaussian (lum.data (), pl->w, pl->h, smooth * 0.3f);
        std::vector<float> ink (lum.size ());
        float t = balance / 50.0f;
        for (size_t i = 0; i < ink.size (); i++)
        {
            ink[i] = clamp01 ((t - lum[i]) * 20.0f + 0.5f);
        }
        ink_plane (pl, cx, ink);
        return true;
    }

    /* Stained Glass: cells of their average colour, leaded with the foreground colour, lit from
       the middle. */
    bool run_stained_glass (Plane* pl, const FilterCtx& cx, const Value& p, const char* cmd, Error* err)
    {
        float size, border, light;
        if (!frange (p, "cellSize", 2.0f, 50.0f, 10.0f, &size, cmd, err)
            || !frange (p, "borderThickness", 1.0f, 20.0f, 4.0f, &border, cmd, err)
            || !frange (p, "lightIntensity", 0.0f, 10.0f, 3.0f, &light, cmd, err))
        {
            return false;
        }
        uint64_t seed = seed_param (p);
        std::vector<float> rgba;
        plane_to_rgba (*pl, cx, &rgba);
        int64_t i0 = (int64_t) floorf ((float) cx.src.x0 / size) - 2,
                j0 = (int64_t) floorf ((float) cx.src.y0 / size) - 2;
        int64_t ni = (int64_t) (pl->w / size) + 5, nj = (int64_t) (pl->h / size) + 5;
        std::vector<double> sums ((size_t) (ni * nj) * 3, 0.0), counts ((size_t) (ni * nj), 0.0);
        std::vector<int64_t> cell (pl->w * pl->h);
        std::vector<float> edge (pl->w * pl->h);
        for (size_t y = 0; y < pl->h; y++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                int64_t ci, cj;
                float d1, d2;
                nearest_cell ((float) x + (float) cx.src.x0 + 0.5f, (float) y + (float) cx.src.y0 + 0.5f, size, seed,
                    &ci, &cj, &d1, &d2);
                int64_t k = std::min (std::max (cj - j0, (int64_t) 0), nj - 1) * ni
                          + std::min (std::max (ci - i0, (int64_t) 0), ni - 1);
                size_t i = y * pl->w + x;
                cell[i]  = k;
                edge[i]  = clamp01 (border * 0.25f - (d2 - d1) * 0.5f + 0.5f);
                for (int c = 0; c < 3; c++)
                {
                    sums[(size_t) k * 3 + c] += rgba[i * 4 + c];
                }
                counts[(size_t) k] += 1;
            }
        }
        float mx, my;
        area_point (cx, 0.5f, 0.5f, &mx, &my);
        float diag = sqrtf ((float) cx.area.width () * (float) cx.area.width ()
                            + (float) cx.area.height () * (float) cx.area.height ())
                   * 0.5f;
        for (size_t y = 0; y < pl->h; y++)
        {
            for (size_t x = 0; x < pl->w; x++)
            {
                size_t i  = y * pl->w + x;
                int64_t k = cell[i];
                float glow =
                    light * 0.06f
                    * (1.0f
                        - std::min (
                            sqrtf (((float) x - mx) * ((float) x - mx) + ((float) y - my) * ((float) y - my)) / diag,
                            1.0f));
                for (int c = 0; c < 3; c++)
                {
                    float v         = (float) (sums[(size_t) k * 3 + c] / std::max (counts[(size_t) k], 1.0)) + glow;
                    rgba[i * 4 + c] = v + (cx.fg[c] - v) * edge[i];
                }
            }
        }
        plane_from_rgba (pl, cx, rgba);
        return true;
    }

    const FilterSpec OTHER_FILTERS[] = {
        {"filter.video.deInterlace", "De-Interlace…", "Filter/Video",
            "{\"eliminate\":\"odd|even\",\"create\":\"duplication|interpolation\"=\"interpolation\"}", run_deinterlace,
            false, Reach::Params},
        {"filter.video.ntscColors", "NTSC Colors", "Filter/Video", "{}", run_ntsc, false, Reach::None},
        {"filter.other.colorToAlpha", "Color to Alpha…", "Filter/Other", "{\"color\":\"#rrggbb\"=\"#ffffff\"}",
            run_color_to_alpha, false, Reach::None},
        {"filter.other.custom", "Custom…", "Filter/Other",
            "{\"kernel\":[25 numbers, 5x5 row by row],\"scale\":1..9999=1,\"offset\":-9999..9999=0}", run_custom, false,
            Reach::Params},
        {"filter.other.hsbHsl", "HSB/HSL…", "Filter/Other",
            "{\"input\":\"rgb|hsb|hsl\"=\"rgb\",\"rowOrder\":\"rgb|hsb|hsl\"=\"hsb\"}", run_hsb_hsl, false,
            Reach::None},
        {"filter.gallery.cutout", "Cutout…", "Filter/Artistic",
            "{\"levels\":2..8=4,\"edgeSimplicity\":0..10=4,\"edgeFidelity\":1..3=2}", run_cutout, false, Reach::Canvas},
        {"filter.gallery.graphicPen", "Graphic Pen…", "Filter/Sketch",
            "{\"strokeLength\":1..15=15,\"balance\":0..100=50,\"direction\":\"rightDiagonal|horizontal|leftDiagonal|"
            "vertical\",\"seed\":int}",
            run_graphic_pen, false, Reach::Params},
        {"filter.gallery.stamp", "Stamp…", "Filter/Sketch", "{\"balance\":0..50=25,\"smoothness\":1..50=5}", run_stamp,
            false, Reach::Params},
        {"filter.gallery.stainedGlass", "Stained Glass…", "Filter/Texture",
            "{\"cellSize\":2..50=10,\"borderThickness\":1..20=4,\"lightIntensity\":0..10=3,\"seed\":int}",
            run_stained_glass, false, Reach::Params},
    };

    void other_filters (std::vector<FilterSpec>* out)
    {
        out->insert (out->end (), OTHER_FILTERS, OTHER_FILTERS + sizeof OTHER_FILTERS / sizeof OTHER_FILTERS[0]);
    }

} // namespace engine
