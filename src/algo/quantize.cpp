#include "algo/quantize.h"

#include "color/convert.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <unordered_map>

namespace algo
{

    /* Colour reduction for Image > Mode > Indexed Color and Bitmap: palettes (exact, system, web,
       uniform, perceptual, selective, adaptive), forced colours, and dithering (none, Floyd-
       Steinberg diffusion, ordered pattern, noise), plus the 1-bit methods. Buffers are straight
       RGBA in 0..1, row by row. Median cut follows Heckbert (1982), refined by Lloyd's k-means;
       error diffusion is Floyd and Steinberg (1976). Palettes are RGB byte triplets. */

    bool palette_kind_from_id (const char* s, PaletteKind* out)
    {
        struct Name
        {
            const char* id;
            PaletteKind kind;
        };
        const Name NAMES[] = {{"exact", PaletteKind::Exact}, {"system", PaletteKind::SystemMac},
            {"systemMac", PaletteKind::SystemMac}, {"systemWindows", PaletteKind::SystemWindows},
            {"web", PaletteKind::Web}, {"uniform", PaletteKind::Uniform}, {"perceptual", PaletteKind::Perceptual},
            {"localPerceptual", PaletteKind::Perceptual}, {"selective", PaletteKind::Selective},
            {"localSelective", PaletteKind::Selective}, {"adaptive", PaletteKind::Adaptive},
            {"localAdaptive", PaletteKind::Adaptive}};
        for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        {
            if (strcmp (s, NAMES[i].id) == 0)
            {
                *out = NAMES[i].kind;
                return true;
            }
        }
        return false;
    }

    bool halftone_shape_from_id (const char* s, HalftoneShape* out)
    {
        const char* const IDS[6] = {"round", "ellipse", "line", "square", "diamond", "cross"};
        for (int i = 0; i < 6; i++)
        {
            if (strcmp (s, IDS[i]) == 0)
            {
                *out = (HalftoneShape) i;
                return true;
            }
        }
        return false;
    }

    uint8_t to8 (float v)
    {
        return (uint8_t) lroundf (fminf (fmaxf (v, 0.0f), 1.0f) * 255.0f);
    }

    void push_rgb (std::vector<uint8_t>* v, uint8_t r, uint8_t g, uint8_t b)
    {
        v->push_back (r);
        v->push_back (g);
        v->push_back (b);
    }

    bool has_rgb (const std::vector<uint8_t>& v, const uint8_t* c)
    {
        for (size_t i = 0; i + 2 < v.size (); i += 3)
        {
            if (v[i] == c[0] && v[i + 1] == c[1] && v[i + 2] == c[2])
            {
                return true;
            }
        }
        return false;
    }

    /* The 216-colour web-safe cube (0, 51, …, 255 per channel). */
    std::vector<uint8_t> web_palette ()
    {
        std::vector<uint8_t> v;
        for (int r = 0; r < 6; r++)
        {
            for (int g = 0; g < 6; g++)
            {
                for (int b = 0; b < 6; b++)
                {
                    push_rgb (&v, (uint8_t) (r * 51), (uint8_t) (g * 51), (uint8_t) (b * 51));
                }
            }
        }
        return v;
    }

    /* The classic Mac OS system palette: the web cube, then 10-step red, green, blue and gray
       ramps. */
    std::vector<uint8_t> mac_palette ()
    {
        std::vector<uint8_t> v = web_palette ();
        std::vector<uint8_t> steps;
        for (int k = 15; k >= 1; k--)
        {
            if ((k * 17) % 51 != 0)
            {
                steps.push_back ((uint8_t) (k * 17));
            }
        }
        for (int ch = 0; ch < 4; ch++)
        {
            for (size_t i = 0; i < steps.size (); i++)
            {
                uint8_t s = steps[i];
                push_rgb (&v, ch == 0 || ch == 3 ? s : 0, ch == 1 || ch == 3 ? s : 0, ch == 2 || ch == 3 ? s : 0);
            }
        }
        if (v.size () > 256 * 3)
        {
            v.resize (256 * 3);
        }
        return v;
    }

    /* The Windows default palette: the 20 static system colours, the web cube, then a gray ramp. */
    std::vector<uint8_t> windows_palette ()
    {
        const uint8_t STATIC[20][3] = {{0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0}, {0, 0, 128}, {128, 0, 128},
            {0, 128, 128}, {192, 192, 192}, {192, 220, 192}, {166, 202, 240}, {255, 251, 240}, {160, 160, 164},
            {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {0, 0, 255}, {255, 0, 255}, {0, 255, 255},
            {255, 255, 255}};
        std::vector<uint8_t> v;
        for (int i = 0; i < 20; i++)
        {
            push_rgb (&v, STATIC[i][0], STATIC[i][1], STATIC[i][2]);
        }
        std::vector<uint8_t> web = web_palette ();
        for (size_t i = 0; i < web.size (); i += 3)
        {
            if (!has_rgb (v, &web[i]))
            {
                push_rgb (&v, web[i], web[i + 1], web[i + 2]);
            }
        }
        uint8_t g = 8;
        for (int guard = 0; v.size () < 256 * 3 && guard < 1024; guard++)
        {
            uint8_t c[3] = {g, g, g};
            if (!has_rgb (v, c))
            {
                push_rgb (&v, g, g, g);
            }
            g = (uint8_t) (g + 12);
        }
        return v;
    }

    /* Up to n colours on an equal-step RGB grid. */
    std::vector<uint8_t> uniform_palette (size_t n)
    {
        size_t k = std::max ((size_t) floorf (cbrtf ((float) n)), (size_t) 2);
        std::vector<uint8_t> v;
        for (size_t r = 0; r < k; r++)
        {
            for (size_t g = 0; g < k; g++)
            {
                for (size_t b = 0; b < k; b++)
                {
                    push_rgb (&v, (uint8_t) lroundf ((float) r / (k - 1) * 255.0f),
                        (uint8_t) lroundf ((float) g / (k - 1) * 255.0f),
                        (uint8_t) lroundf ((float) b / (k - 1) * 255.0f));
                }
            }
        }
        return v;
    }

    std::vector<uint8_t> forced_colors (Forced f)
    {
        std::vector<uint8_t> v;
        if (f == Forced::BlackWhite || f == Forced::Primaries)
        {
            push_rgb (&v, 0, 0, 0);
            push_rgb (&v, 255, 255, 255);
        }
        if (f == Forced::Primaries)
        {
            const uint8_t P[6][3] = {
                {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 255, 255}, {255, 0, 255}, {255, 255, 0}};
            for (int i = 0; i < 6; i++)
            {
                push_rgb (&v, P[i][0], P[i][1], P[i][2]);
            }
        }
        if (f == Forced::Web)
        {
            v = web_palette ();
        }
        return v;
    }

    /* A weighted point for median cut and k-means: a colour (RGB or Lab) and how many pixels have
       it. */
    struct ColorPoint
    {
        float c[3];
        uint32_t n;
    };

    /* The opaque pixels' distinct 8-bit colours with their counts, in colour order. */
    std::vector<ColorPoint> color_histogram (const std::vector<float>& rgba)
    {
        std::unordered_map<uint32_t, uint32_t> m;
        for (size_t i = 0; i + 3 < rgba.size (); i += 4)
        {
            if (rgba[i + 3] < 0.5f)
            {
                continue;
            }
            uint32_t key = (uint32_t) to8 (rgba[i]) << 16 | (uint32_t) to8 (rgba[i + 1]) << 8 | to8 (rgba[i + 2]);
            m[key]++;
        }
        std::vector<uint32_t> keys;
        keys.reserve (m.size ());
        for (std::unordered_map<uint32_t, uint32_t>::const_iterator it = m.begin (); it != m.end (); ++it)
        {
            keys.push_back (it->first);
        }
        std::sort (keys.begin (), keys.end ());
        std::vector<ColorPoint> v (keys.size ());
        for (size_t i = 0; i < keys.size (); i++)
        {
            v[i].c[0] = (float) (keys[i] >> 16 & 255);
            v[i].c[1] = (float) (keys[i] >> 8 & 255);
            v[i].c[2] = (float) (keys[i] & 255);
            v[i].n    = m[keys[i]];
        }
        return v;
    }

    /* Median cut into at most n boxes: the box with the widest spread (weighted by the square root
       of its pixel count) splits at its weighted median along that axis. Returns the box means. */
    std::vector<ColorPoint> median_cut (const std::vector<ColorPoint>& pts, size_t n)
    {
        std::vector<std::vector<ColorPoint>> boxes;
        std::vector<ColorPoint> means;
        if (pts.empty () || n == 0)
        {
            return means;
        }
        boxes.push_back (pts);
        while (boxes.size () < n)
        {
            size_t bi   = SIZE_MAX;
            int axis    = 0;
            float score = -1.0f;
            for (size_t i = 0; i < boxes.size (); i++)
            {
                const std::vector<ColorPoint>& b = boxes[i];
                if (b.size () < 2)
                {
                    continue;
                }
                int best_axis = 0;
                float best = -1.0f, wsum = 0.0f;
                for (int a = 0; a < 3; a++)
                {
                    float lo = 1e30f, hi = -1e30f;
                    for (size_t k = 0; k < b.size (); k++)
                    {
                        lo = fminf (lo, b[k].c[a]);
                        hi = fmaxf (hi, b[k].c[a]);
                    }
                    if (hi - lo > best)
                    {
                        best      = hi - lo;
                        best_axis = a;
                    }
                }
                for (size_t k = 0; k < b.size (); k++)
                {
                    wsum += (float) b[k].n;
                }
                float sc = best * sqrtf (wsum);
                if (sc > score)
                {
                    score = sc;
                    bi    = i;
                    axis  = best_axis;
                }
            }
            if (bi == SIZE_MAX || score <= 0.0f)
            {
                break;
            }
            std::vector<ColorPoint> unsorted = boxes[bi];
            boxes.erase (boxes.begin () + (ptrdiff_t) bi);
            /* Sorted along the axis (ties keep their order). */
            std::vector<std::pair<float, size_t>> order (unsorted.size ());
            for (size_t k = 0; k < unsorted.size (); k++)
            {
                order[k] = std::make_pair (unsorted[k].c[axis], k);
            }
            std::sort (order.begin (), order.end ());
            std::vector<ColorPoint> b (unsorted.size ());
            for (size_t k = 0; k < order.size (); k++)
            {
                b[k] = unsorted[order[k].second];
            }
            uint64_t total = 0, acc = 0;
            for (size_t k = 0; k < b.size (); k++)
            {
                total += b[k].n;
            }
            size_t cut = 1;
            for (size_t k = 0; k < b.size (); k++)
            {
                acc += b[k].n;
                if (acc * 2 >= total)
                {
                    cut = std::min (std::max (k + 1, (size_t) 1), b.size () - 1);
                    break;
                }
            }
            boxes.push_back (std::vector<ColorPoint> (b.begin (), b.begin () + (ptrdiff_t) cut));
            boxes.push_back (std::vector<ColorPoint> (b.begin () + (ptrdiff_t) cut, b.end ()));
        }
        for (size_t i = 0; i < boxes.size (); i++)
        {
            double s[3] = {0, 0, 0}, w = 0;
            for (size_t k = 0; k < boxes[i].size (); k++)
            {
                for (int a = 0; a < 3; a++)
                {
                    s[a] += (double) boxes[i][k].c[a] * boxes[i][k].n;
                }
                w += boxes[i][k].n;
            }
            ColorPoint m;
            for (int a = 0; a < 3; a++)
            {
                m.c[a] = (float) (s[a] / w);
            }
            m.n = 0;
            means.push_back (m);
        }
        return means;
    }

    /* Lloyd refinement of the centres over the weighted points. */
    void kmeans (const std::vector<ColorPoint>& pts, std::vector<ColorPoint>* centers, int iters)
    {
        size_t k = centers->size ();
        if (k == 0)
        {
            return;
        }
        for (int it = 0; it < iters; it++)
        {
            std::vector<double> sum (k * 3, 0.0), cnt (k, 0.0);
            for (size_t p = 0; p < pts.size (); p++)
            {
                size_t best = 0;
                float bd    = 1e30f;
                for (size_t c = 0; c < k; c++)
                {
                    float d = 0.0f;
                    for (int a = 0; a < 3; a++)
                    {
                        float e = pts[p].c[a] - (*centers)[c].c[a];
                        d += e * e;
                    }
                    if (d < bd)
                    {
                        bd   = d;
                        best = c;
                    }
                }
                for (int a = 0; a < 3; a++)
                {
                    sum[best * 3 + (size_t) a] += (double) pts[p].c[a] * pts[p].n;
                }
                cnt[best] += pts[p].n;
            }
            for (size_t c = 0; c < k; c++)
            {
                if (cnt[c] <= 0.0)
                {
                    continue;
                }
                for (int a = 0; a < 3; a++)
                {
                    (*centers)[c].c[a] = (float) (sum[c * 3 + (size_t) a] / cnt[c]);
                }
            }
        }
    }

    /* The palette for the image: `colors` (2..256) for the computed kinds, forced colours first
       (they count toward it). False with a message when Exact finds more than 256 colours. */
    bool build_palette (const std::vector<float>& rgba, PaletteKind kind, size_t colors, Forced forced,
        std::vector<uint8_t>* out, std::string* err)
    {
        colors                   = std::min (std::max (colors, (size_t) 2), (size_t) 256);
        std::vector<uint8_t> pal = forced_colors (forced);
        size_t forced_n          = pal.size () / 3;
        size_t room              = colors > forced_n ? colors - forced_n : 0;
        bool computed_kind =
            kind == PaletteKind::Adaptive || kind == PaletteKind::Perceptual || kind == PaletteKind::Selective;
        std::vector<ColorPoint> hist;
        if (kind == PaletteKind::Exact || (room > 0 && computed_kind))
        {
            hist = color_histogram (rgba);
        }
        std::vector<uint8_t> computed;
        if (kind == PaletteKind::Exact)
        {
            if (hist.size () > 256)
            {
                *err = "the image has " + std::to_string (hist.size ()) + " colours; Exact needs 256 or fewer";
                return false;
            }
            for (size_t i = 0; i < hist.size (); i++)
            {
                push_rgb (&computed, (uint8_t) hist[i].c[0], (uint8_t) hist[i].c[1], (uint8_t) hist[i].c[2]);
            }
        }
        else if (kind == PaletteKind::SystemMac)
        {
            computed = mac_palette ();
        }
        else if (kind == PaletteKind::SystemWindows)
        {
            computed = windows_palette ();
        }
        else if (kind == PaletteKind::Web)
        {
            computed = web_palette ();
        }
        else if (kind == PaletteKind::Uniform)
        {
            computed = uniform_palette (std::max (room, (size_t) 8));
        }
        else if (hist.size () <= room)
        {
            for (size_t i = 0; i < hist.size (); i++)
            {
                push_rgb (&computed, (uint8_t) hist[i].c[0], (uint8_t) hist[i].c[1], (uint8_t) hist[i].c[2]);
            }
        }
        else if (kind == PaletteKind::Adaptive)
        {
            std::vector<ColorPoint> centers = median_cut (hist, room);
            kmeans (hist, &centers, 2);
            for (size_t i = 0; i < centers.size (); i++)
            {
                push_rgb (&computed, to8 (centers[i].c[0] / 255.0f), to8 (centers[i].c[1] / 255.0f),
                    to8 (centers[i].c[2] / 255.0f));
            }
        }
        else
        {
            /* Perceptual and Selective cut in Lab; Selective then snaps near-web colours onto the
               web cube. */
            std::vector<ColorPoint> lab = hist;
            for (size_t i = 0; i < lab.size (); i++)
            {
                float rgb[3] = {hist[i].c[0] / 255.0f, hist[i].c[1] / 255.0f, hist[i].c[2] / 255.0f};
                color::srgb_to_lab (rgb, lab[i].c);
            }
            std::vector<ColorPoint> centers = median_cut (lab, room);
            kmeans (lab, &centers, 4);
            for (size_t i = 0; i < centers.size (); i++)
            {
                float rgb[3];
                color::lab_to_srgb (centers[i].c, rgb);
                uint8_t c[3] = {to8 (rgb[0]), to8 (rgb[1]), to8 (rgb[2])};
                if (kind == PaletteKind::Selective)
                {
                    uint8_t snap[3];
                    bool near = true;
                    for (int a = 0; a < 3; a++)
                    {
                        snap[a] = (uint8_t) (lroundf ((float) c[a] / 51.0f) * 51);
                        near    = near && abs ((int) c[a] - (int) snap[a]) <= 10;
                    }
                    if (near)
                    {
                        memcpy (c, snap, 3);
                    }
                }
                push_rgb (&computed, c[0], c[1], c[2]);
            }
        }
        for (size_t i = 0; i + 2 < computed.size () && pal.size () < 256 * 3; i += 3)
        {
            if (!has_rgb (pal, &computed[i]))
            {
                push_rgb (&pal, computed[i], computed[i + 1], computed[i + 2]);
            }
        }
        if (computed_kind || kind == PaletteKind::Uniform)
        {
            size_t keep = std::max (colors, forced_n);
            if (pal.size () > keep * 3)
            {
                pal.resize (keep * 3);
            }
        }
        if (pal.empty ())
        {
            push_rgb (&pal, 0, 0, 0);
        }
        *out = pal;
        return true;
    }

    /* The 8 x 8 Bayer threshold in -0.5..0.5. */
    float bayer8 (size_t x, size_t y)
    {
        const uint8_t M[64] = {0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6,
            38, 60, 28, 52, 20, 62, 30, 54, 22, 3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7,
            39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21};
        return ((float) M[(y % 8) * 8 + x % 8] + 0.5f) / 64.0f - 0.5f;
    }

    float hash01 (size_t x, size_t y, uint32_t seed)
    {
        uint32_t h = (uint32_t) x * 0x8da6b343u ^ (uint32_t) y * 0xd8163841u ^ seed * 0xcb1ab31fu;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return (float) (h & 0xffff) / 65535.0f;
    }

    /* The nearest palette entry for every colour on a 64-level grid, so each pixel's lookup is one
       table read. */
    std::vector<uint8_t> nearest_grid (const std::vector<uint8_t>& pal)
    {
        const size_t G = 64;
        std::vector<uint8_t> grid (G * G * G);
        size_t n = pal.size () / 3;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ri = 0; ri < (ptrdiff_t) G; ri++)
        {
            size_t r = (size_t) ri;
            for (size_t g = 0; g < G; g++)
            {
                for (size_t b = 0; b < G; b++)
                {
                    float c[3]  = {((float) r + 0.5f) * 4.0f, ((float) g + 0.5f) * 4.0f, ((float) b + 0.5f) * 4.0f};
                    size_t best = 0;
                    float bd    = 1e30f;
                    for (size_t i = 0; i < n; i++)
                    {
                        float d = 0.0f;
                        for (int a = 0; a < 3; a++)
                        {
                            float e = c[a] - (float) pal[i * 3 + (size_t) a];
                            d += e * e;
                        }
                        if (d < bd)
                        {
                            bd   = d;
                            best = i;
                        }
                    }
                    grid[(r * G + g) * G + b] = (uint8_t) best;
                }
            }
        }
        return grid;
    }

    size_t grid_index (const std::vector<uint8_t>& grid, const float c[3])
    {
        size_t r = (size_t) (fminf (fmaxf (c[0], 0.0f), 1.0f) * 63.999f),
               g = (size_t) (fminf (fmaxf (c[1], 0.0f), 1.0f) * 63.999f),
               b = (size_t) (fminf (fmaxf (c[2], 0.0f), 1.0f) * 63.999f);
        return grid[(r * 64 + g) * 64 + b];
    }

    /* Every opaque pixel mapped to its palette entry in place, with the chosen dither (`amount`
       0..1 scales it). Pixels under half opaque become entry `transparent` with alpha 0 when there
       is one (-1: none); otherwise alpha is kept. */
    void quantize (std::vector<float>* rgba, size_t w, const std::vector<uint8_t>& pal, Dither dither, float amount,
        int transparent)
    {
        std::vector<float>& px = *rgba;
        size_t count = px.size () / 4, h = w > 0 ? count / w : 0;
        std::vector<uint8_t> grid = nearest_grid (pal);
        float a                   = fminf (fmaxf (amount, 0.0f), 1.0f);
        float spread              = fminf (1.0f / fmaxf (cbrtf ((float) (pal.size () / 3)), 1.0f), 0.5f) * a;
        std::vector<float> err (dither == Dither::Diffusion ? 2 * (w + 2) * 3 : 0, 0.0f);
        for (size_t y = 0; y < h; y++)
        {
            if (dither == Dither::Diffusion)
            {
                memcpy (err.data (), err.data () + (w + 2) * 3, (w + 2) * 3 * sizeof (float));
                std::fill (err.begin () + (ptrdiff_t) ((w + 2) * 3), err.end (), 0.0f);
            }
            for (size_t x = 0; x < w; x++)
            {
                float* p = &px[(y * w + x) * 4];
                if (p[3] < 0.5f)
                {
                    if (transparent >= 0)
                    {
                        p[0] = p[1] = p[2] = p[3] = 0.0f;
                        continue;
                    }
                    if (p[3] <= 0.0f)
                    {
                        continue;
                    }
                }
                float c[3] = {p[0], p[1], p[2]};
                if (dither == Dither::Diffusion)
                {
                    for (int k = 0; k < 3; k++)
                    {
                        c[k] += err[(x + 1) * 3 + (size_t) k];
                    }
                }
                else if (dither == Dither::Pattern)
                {
                    float t = bayer8 (x, y) * spread;
                    for (int k = 0; k < 3; k++)
                    {
                        c[k] += t;
                    }
                }
                else if (dither == Dither::Noise)
                {
                    float t = (hash01 (x, y, 7) - 0.5f) * spread;
                    for (int k = 0; k < 3; k++)
                    {
                        c[k] += t;
                    }
                }
                for (int k = 0; k < 3; k++)
                {
                    c[k] = fminf (fmaxf (c[k], 0.0f), 1.0f);
                }
                size_t i   = grid_index (grid, c);
                float e[3] = {pal[i * 3] / 255.0f, pal[i * 3 + 1] / 255.0f, pal[i * 3 + 2] / 255.0f};
                if (dither == Dither::Diffusion)
                {
                    float *cur = err.data (), *next = err.data () + (w + 2) * 3;
                    for (int q = 0; q < 3; q++)
                    {
                        float d = (c[q] - e[q]) * a;
                        cur[(x + 2) * 3 + (size_t) q] += d * 7.0f / 16.0f;
                        next[x * 3 + (size_t) q] += d * 3.0f / 16.0f;
                        next[(x + 1) * 3 + (size_t) q] += d * 5.0f / 16.0f;
                        next[(x + 2) * 3 + (size_t) q] += d / 16.0f;
                    }
                }
                p[0] = e[0];
                p[1] = e[1];
                p[2] = e[2];
                if (transparent >= 0)
                {
                    p[3] = 1.0f;
                }
            }
        }
    }

    /* A halftone spot's value at (u, v) in -1..1 within its cell. */
    float spot (HalftoneShape shape, float u, float v)
    {
        float s;
        switch (shape)
        {
        case HalftoneShape::Ellipse:
            s = 1.0f - (u * u + 0.6f * v * v) / 1.6f;
            break;
        case HalftoneShape::Line:
            s = 1.0f - fabsf (v);
            break;
        case HalftoneShape::Square:
            s = 1.0f - fmaxf (fabsf (u), fabsf (v));
            break;
        case HalftoneShape::Diamond:
            s = 1.0f - (fabsf (u) + fabsf (v)) / 2.0f;
            break;
        case HalftoneShape::Cross:
            s = 1.0f - fminf (fabsf (u), fabsf (v));
            break;
        default:
            s = 1.0f - (u * u + v * v) / 2.0f;
            break;
        }
        return fminf (fmaxf (s, 0.0f), 1.0f);
    }

    /* Gray values (0..1, row by row, w wide) to 0 or 1 in place: 50% threshold, ordered pattern,
       error diffusion, or a halftone screen (`cell` pixels, `angle` degrees, `shape`). */
    void to_bitmap (
        std::vector<float>* gray, size_t w, BitmapMethod method, float cell, float angle, HalftoneShape shape)
    {
        std::vector<float>& g = *gray;
        size_t h              = w > 0 ? g.size () / w : 0;
        if (method == BitmapMethod::Threshold)
        {
            for (size_t i = 0; i < g.size (); i++)
            {
                g[i] = g[i] >= 0.5f ? 1.0f : 0.0f;
            }
        }
        else if (method == BitmapMethod::Pattern)
        {
            for (size_t i = 0; i < g.size (); i++)
            {
                g[i] = g[i] > bayer8 (i % w, i / w) + 0.5f ? 1.0f : 0.0f;
            }
        }
        else if (method == BitmapMethod::Diffusion)
        {
            std::vector<float> buf = g;
            for (size_t y = 0; y < h; y++)
            {
                for (size_t x = 0; x < w; x++)
                {
                    size_t i  = y * w + x;
                    float old = buf[i], nv = old >= 0.5f ? 1.0f : 0.0f, e = old - nv;
                    g[i] = nv;
                    if (x + 1 < w)
                    {
                        buf[i + 1] += e * 7.0f / 16.0f;
                    }
                    if (y + 1 < h)
                    {
                        if (x > 0)
                        {
                            buf[i + w - 1] += e * 3.0f / 16.0f;
                        }
                        buf[i + w] += e * 5.0f / 16.0f;
                        if (x + 1 < w)
                        {
                            buf[i + w + 1] += e / 16.0f;
                        }
                    }
                }
            }
        }
        else
        {
            /* A pixel turns black when the spot function's rank at its place in the cell is at least
               its gray value, so the dots grow with darkness. */
            cell    = fmaxf (cell, 2.0f);
            float s = sinf (angle * (float) M_PI / 180.0f), c = cosf (angle * (float) M_PI / 180.0f);
            std::vector<float> table (32 * 32);
            for (size_t k = 0; k < table.size (); k++)
            {
                table[k] =
                    spot (shape, ((float) (k % 32) + 0.5f) / 16.0f - 1.0f, ((float) (k / 32) + 0.5f) / 16.0f - 1.0f);
            }
            std::sort (table.begin (), table.end ());
#pragma omp parallel for schedule(static)
            for (ptrdiff_t ii = 0; ii < (ptrdiff_t) g.size (); ii++)
            {
                size_t i = (size_t) ii;
                float x = (float) (i % w) + 0.5f, y = (float) (i / w) + 0.5f;
                float rx = x * c + y * s, ry = -x * s + y * c;
                float u    = (rx / cell - floorf (rx / cell)) * 2.0f - 1.0f,
                      v    = (ry / cell - floorf (ry / cell)) * 2.0f - 1.0f;
                float sv   = spot (shape, u, v);
                float rank = (float) (std::lower_bound (table.begin (), table.end (), sv) - table.begin ())
                           / (float) table.size ();
                g[i] = rank >= g[i] ? 0.0f : 1.0f;
            }
        }
    }

} // namespace algo
