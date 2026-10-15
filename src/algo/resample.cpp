#include "algo/resample.h"

#include <math.h>
#include <string.h>
#include <vector>

namespace algo
{

    /* High-quality separable resampling of surfaces (Image Size, and transforms that shrink a lot).
       Photoshop's bicubic filters are the Keys cubic with a fitted parameter and stretch; Lanczos-3 is
       widened by 1/scale when reducing. Work goes in bands of output rows, in parallel. */

    using geom::Rect;

    /* Sub-pixel positions Photoshop's filters resolve: offsets are rounded to 1/128 pixel. */
    const double PHASES = 128.0;

    enum class KernelKind
    {
        Nearest,
        Bilinear,
        Keys,
        Lanczos,
    };

    struct Kernel
    {
        KernelKind kind;
        double a;
        double stretch;
    };

    struct Pass
    {
        double sx;
        double sy;
        Kernel kx;
        Kernel ky;
    };

    struct Taps
    {
        int32_t start;
        std::vector<float> weights;
        float outside;
    };

    const char* const RESAMPLE_NAMES[] = {"nearest", "bilinear", "bicubic", "bicubicSmoother", "bicubicSharper",
        "bicubicAutomatic", "lanczos", "preserveDetails"};

    bool resample_from_str (const char* s, Resample* out)
    {
        for (int i = 0; i < 8; i++)
        {
            if (strcmp (s, RESAMPLE_NAMES[i]) == 0)
            {
                *out = (Resample) i;
                return true;
            }
        }
        if (strcmp (s, "nearestNeighbor") == 0)
        {
            *out = Resample::Nearest;
            return true;
        }
        return false;
    }

    void premultiply (float* data, size_t count, size_t ch, bool alpha)
    {
        if (!alpha)
        {
            return;
        }
        for (size_t i = 0; i < count; i++)
        {
            float* px = data + i * ch;
            for (size_t c = 0; c + 1 < ch; c++)
            {
                px[c] *= px[ch - 1];
            }
        }
    }

    void unpremultiply (float* data, size_t count, size_t ch, bool alpha)
    {
        if (!alpha)
        {
            return;
        }
        for (size_t i = 0; i < count; i++)
        {
            float* px = data + i * ch;
            float a   = px[ch - 1];
            for (size_t c = 0; c + 1 < ch; c++)
            {
                px[c] = a > 1e-7f ? px[c] / a : 0.0f;
            }
        }
    }

    Rect scaled_rect (Rect r, double sx, double sy)
    {
        if (r.is_empty ())
        {
            return Rect ();
        }
        Rect out = {(int32_t) floor (r.x0 * sx), (int32_t) floor (r.y0 * sy), (int32_t) ceil (r.x1 * sx),
            (int32_t) ceil (r.y1 * sy)};
        return out;
    }

    Kernel simple (KernelKind k)
    {
        Kernel out = {k, 0.0, 1.0};
        return out;
    }

    /* Photoshop's bicubic filters (fitted on Photoshop 25.4): Bicubic a = -0.75; Smoother stretched
       by 1.15 with a = -0.625 (- 0.5 (1 - s) reducing); Sharper stretched by 1.05 with a = -1
       (- 1.6 (1 - s) reducing). Reducing widens the filter by 1/s, down to 1/4. */
    Kernel keys (Resample method, double s)
    {
        s        = s < 0.25 ? 0.25 : s > 1.0 ? 1.0 : s;
        double a = -0.75, stretch = 1.0;
        if (method == Resample::BicubicSmoother)
        {
            a       = -0.625 - 0.5 * (1.0 - s);
            stretch = 1.15;
        }
        else if (method == Resample::BicubicSharper)
        {
            a       = -1.0 - 1.6 * (1.0 - s);
            stretch = 1.05;
        }
        Kernel k = {KernelKind::Keys, a, stretch / s};
        return k;
    }

    double keys_weight (double x, double a)
    {
        x = fabs (x);
        if (x < 1.0)
        {
            return (a + 2.0) * x * x * x - (a + 3.0) * x * x + 1.0;
        }
        if (x < 2.0)
        {
            return a * (x * x * x - 5.0 * x * x + 8.0 * x - 4.0);
        }
        return 0.0;
    }

    double lanczos_weight (double x)
    {
        x = fabs (x);
        if (x < 1e-9)
        {
            return 1.0;
        }
        if (x < 3.0)
        {
            double px = M_PI * x;
            return 3.0 * sin (px) * sin (px / 3.0) / (px * px);
        }
        return 0.0;
    }

    /* Photoshop's passes: a bicubic filter is designed for the height's scale (or the width's when
       the height stays) and the smaller scale goes first; Bicubic Automatic growing one axis and
       shrinking the other is Sharper then Smoother. The others run both axes at once. */
    void plan (Resample filter, double sx, double sy, std::vector<Pass>* out)
    {
        Pass both;
        both.sx = sx;
        both.sy = sy;
        switch (filter)
        {
        case Resample::Nearest:
            both.kx = both.ky = simple (KernelKind::Nearest);
            out->push_back (both);
            return;
        case Resample::Bilinear:
            both.kx = both.ky = simple (KernelKind::Bilinear);
            out->push_back (both);
            return;
        case Resample::Lanczos:
            both.kx = both.ky = simple (KernelKind::Lanczos);
            out->push_back (both);
            return;
        case Resample::PreserveDetails:
            if (sx > 1.0 || sy > 1.0)
            {
                both.kx = both.ky = simple (KernelKind::Lanczos);
                out->push_back (both);
                return;
            }
            plan (Resample::BicubicSharper, sx, sy, out);
            return;
        case Resample::BicubicAutomatic:
            if (sx <= 1.0 && sy <= 1.0)
            {
                return plan (Resample::BicubicSharper, sx, sy, out);
            }
            if (sx >= 1.0 && sy >= 1.0)
            {
                return plan (Resample::BicubicSmoother, sx, sy, out);
            }
            if (sx < 1.0)
            {
                Pass a = {sx, 1.0, keys (Resample::BicubicSharper, sx), simple (KernelKind::Nearest)};
                Pass b = {1.0, sy, simple (KernelKind::Nearest), keys (Resample::BicubicSmoother, sy)};
                out->push_back (a);
                out->push_back (b);
            }
            else
            {
                Pass a = {1.0, sy, simple (KernelKind::Nearest), keys (Resample::BicubicSharper, sy)};
                Pass b = {sx, 1.0, keys (Resample::BicubicSmoother, sx), simple (KernelKind::Nearest)};
                out->push_back (a);
                out->push_back (b);
            }
            return;
        default:
            {
                Kernel k = keys (filter, sy != 1.0 ? sy : sx);
                if (sx == 1.0 || sy == 1.0)
                {
                    both.kx = both.ky = k;
                    out->push_back (both);
                }
                else if (sy < sx)
                {
                    Pass a = {1.0, sy, k, k}, b = {sx, 1.0, k, k};
                    out->push_back (a);
                    out->push_back (b);
                }
                else
                {
                    Pass a = {sx, 1.0, k, k}, b = {1.0, sy, k, k};
                    out->push_back (a);
                    out->push_back (b);
                }
            }
        }
    }

    /* The source position of output sample o, kept on the edge samples when there is an edge and
       rounded to 1/128 pixel. */
    double position (int32_t o, double scale, bool has_edge, int32_t e0, int32_t e1)
    {
        double u = (o + 0.5) / scale - 0.5;
        if (has_edge)
        {
            double hi = e1 > e0 ? e1 : e0;
            u         = u < e0 ? e0 : u > hi ? hi : u;
        }
        double j = floor (u);
        return j + round ((u - j) * PHASES) / PHASES;
    }

    /* The source pixels [a, b) output pixel o covers. */
    void cover (int32_t o, double scale, int32_t* a, int32_t* b)
    {
        *a        = (int32_t) floor (o / scale + 1e-9);
        double hi = floor ((o + 1) / scale + 1e-9);
        *b        = (int32_t) (hi > *a + 1.0 ? hi : *a + 1.0);
    }

    /* Unnormalized weights of output sample o, starting at source sample *lo. */
    void weights (
        int32_t o, double scale, Kernel k, bool has_edge, int32_t e0, int32_t e1, int32_t* lo, std::vector<double>* w)
    {
        w->clear ();
        switch (k.kind)
        {
        case KernelKind::Nearest:
            {
                double u = (o + 0.5) / scale - 0.5;
                *lo      = (int32_t) floor (u + 0.5);
                w->push_back (1.0);
                return;
            }
        case KernelKind::Bilinear:
            {
                if (scale < 0.5)
                {
                    int32_t a, b;
                    cover (o, scale, &a, &b);
                    *lo = a;
                    w->assign ((size_t) (b - a), 1.0 / (b - a));
                    return;
                }
                double u = position (o, scale, has_edge, e0, e1);
                double j = floor (u);
                *lo      = (int32_t) j;
                w->push_back (1.0 - (u - j));
                w->push_back (u - j);
                return;
            }
        case KernelKind::Keys:
            {
                if (scale < 0.25)
                {
                    /* Averaged down to four times the target first, then filtered. */
                    double inner = 4.0 * scale;
                    int32_t ie0 = 0, ie1 = 0;
                    if (has_edge)
                    {
                        ie0 = (int32_t) ceil (e0 * inner - 1e-9);
                        ie1 = (int32_t) ceil ((e1 + 1) * inner - 1e-9) - 1;
                    }
                    int32_t k0;
                    std::vector<double> kw;
                    weights (o, 0.25, k, has_edge, ie0, ie1, &k0, &kw);
                    std::vector<int32_t> ba (kw.size ()), bb (kw.size ());
                    int32_t mn = INT32_MAX, mx = INT32_MIN;
                    for (size_t i = 0; i < kw.size (); i++)
                    {
                        int32_t kk = k0 + (int32_t) i;
                        if (has_edge)
                        {
                            kk = kk < ie0 ? ie0 : kk > (ie1 > ie0 ? ie1 : ie0) ? (ie1 > ie0 ? ie1 : ie0) : kk;
                        }
                        cover (kk, inner, &ba[i], &bb[i]);
                        if (ba[i] < mn)
                        {
                            mn = ba[i];
                        }
                        if (bb[i] > mx)
                        {
                            mx = bb[i];
                        }
                    }
                    if (kw.empty ())
                    {
                        mn = mx = 0;
                    }
                    *lo = mn;
                    w->assign ((size_t) (mx - mn > 0 ? mx - mn : 0), 0.0);
                    for (size_t i = 0; i < kw.size (); i++)
                    {
                        for (int32_t s = ba[i]; s < bb[i]; s++)
                        {
                            (*w)[(size_t) (s - mn)] += kw[i] / (bb[i] - ba[i]);
                        }
                    }
                    return;
                }
                double u  = position (o, scale, has_edge, e0, e1);
                int32_t a = (int32_t) floor (u - 2.0 * k.stretch), b = (int32_t) ceil (u + 2.0 * k.stretch);
                double sum = 0.0;
                for (int32_t i = a; i <= b; i++)
                {
                    double v = keys_weight ((u - i) / k.stretch, k.a);
                    w->push_back (v);
                    sum += v;
                }
                for (size_t i = 0; i < w->size (); i++)
                {
                    (*w)[i] /= sum;
                }
                *lo = a;
                return;
            }
        case KernelKind::Lanczos:
            {
                double stretch = scale < 1.0 ? 1.0 / scale : 1.0;
                double u       = (o + 0.5) / scale - 0.5;
                if (has_edge)
                {
                    double hi = e1 > e0 ? e1 : e0;
                    u         = u < e0 ? e0 : u > hi ? hi : u;
                }
                int32_t a = (int32_t) ceil (u - 3.0 * stretch), b = (int32_t) floor (u + 3.0 * stretch);
                double sum = 0.0;
                for (int32_t i = a; i <= b; i++)
                {
                    double v = lanczos_weight ((i - u) / stretch);
                    w->push_back (v);
                    sum += v;
                }
                double inv = fabs (sum) > 1e-12 ? 1.0 / sum : 1.0;
                for (size_t i = 0; i < w->size (); i++)
                {
                    (*w)[i] *= inv;
                }
                *lo = a;
                return;
            }
        }
    }

    /* Taps for output [dst0, dst1). With an edge, taps beyond it read the edge sample; taps outside
       the content [c0, c1] read the default pixel and are folded into `outside`. */
    void make_taps (int32_t dst0, int32_t dst1, double scale, Kernel kernel, bool has_edge, int32_t e0, int32_t e1,
        int32_t c0, int32_t c1, std::vector<Taps>* out)
    {
        if (scale == 1.0)
        {
            kernel = simple (KernelKind::Nearest);
        }
        out->clear ();
        std::vector<double> w;
        for (int32_t o = dst0; o < dst1; o++)
        {
            int32_t lo;
            weights (o, scale, kernel, has_edge, e0, e1, &lo, &w);
            int32_t hi    = lo + (int32_t) w.size () - 1;
            int32_t start = has_edge ? (lo < e0 ? e0 : lo > e1 ? e1 : lo) : lo;
            int32_t end   = has_edge ? (hi < e0 ? e0 : hi > e1 ? e1 : hi) : hi;
            std::vector<float> folded ((size_t) (end - start + 1 > 1 ? end - start + 1 : 1), 0.0f);
            for (int32_t i = lo; i <= hi; i++)
            {
                int32_t ci = has_edge ? (i < e0 ? e0 : i > e1 ? e1 : i) : i;
                size_t at  = (size_t) (ci - start);
                if (at < folded.size ())
                {
                    folded[at] += (float) w[(size_t) (i - lo)];
                }
            }
            int32_t a = start > c0 ? start : c0;
            int32_t b = start + (int32_t) folded.size () - 1 < c1 ? start + (int32_t) folded.size () - 1 : c1;
            Taps t;
            if (a > b)
            {
                t.start   = c0;
                t.outside = 0.0f;
                for (size_t i = 0; i < folded.size (); i++)
                {
                    t.outside += folded[i];
                }
            }
            else
            {
                size_t i0 = (size_t) (a - start), i1 = (size_t) (b - start) + 1;
                t.start   = a;
                t.outside = 0.0f;
                for (size_t i = 0; i < folded.size (); i++)
                {
                    if (i < i0 || i >= i1)
                    {
                        t.outside += folded[i];
                    }
                }
                t.weights.assign (folded.begin () + (ptrdiff_t) i0, folded.begin () + (ptrdiff_t) i1);
            }
            out->push_back (t);
        }
    }

    void horizontal (const float* row, float* out, const std::vector<Taps>& hx, int32_t x_lo, int32_t x_hi,
        const float* def, size_t n)
    {
        for (size_t o = 0; o < hx.size (); o++)
        {
            const Taps& t = hx[o];
            float* d      = out + o * n;
            for (size_t c = 0; c < n; c++)
            {
                d[c] = 0.0f;
            }
            for (size_t k = 0; k < t.weights.size (); k++)
            {
                int32_t sx = t.start + (int32_t) k;
                if (sx < x_lo || sx >= x_hi)
                {
                    continue;
                }
                const float* sp = row + (size_t) (sx - x_lo) * n;
                float w         = t.weights[k];
                for (size_t c = 0; c < n; c++)
                {
                    d[c] += sp[c] * w;
                }
            }
            for (size_t c = 0; c < n; c++)
            {
                d[c] += def[c] * t.outside;
            }
        }
    }

    /* One pass. With `canvas`, the content and the canvas repeat their edge outwards. */
    raster::Surface resize_pass (const raster::Surface& s, const Pass& pass, bool has_canvas, Rect canvas)
    {
        color::PixelFormat fmt = s.format ();
        float defp[raster::MAX_CHANNELS];
        s.default_pixel (defp);
        raster::Surface out = raster::Surface::with_default (fmt, defp);
        Rect src            = s.content_bounds ();
        Rect dst            = scaled_rect (src, pass.sx, pass.sy);
        if (dst.is_empty ())
        {
            return out;
        }
        size_t n      = fmt.channels ();
        bool alpha    = fmt.alpha;
        Rect edge     = has_canvas ? src.unite (canvas) : Rect ();
        bool has_edge = has_canvas && !edge.is_empty ();
        Rect area     = has_edge ? edge : src;
        std::vector<Taps> hx, vy;
        make_taps (dst.x0, dst.x1, pass.sx, pass.kx, has_edge, edge.x0, edge.x1 - 1, area.x0, area.x1 - 1, &hx);
        make_taps (dst.y0, dst.y1, pass.sy, pass.ky, has_edge, edge.y0, edge.y1 - 1, area.y0, area.y1 - 1, &vy);
        int32_t x_lo = INT32_MAX, x_hi = INT32_MIN;
        for (size_t i = 0; i < hx.size (); i++)
        {
            if (!hx[i].weights.empty () && hx[i].start < x_lo)
            {
                x_lo = hx[i].start;
            }
            int32_t e = hx[i].start + (int32_t) hx[i].weights.size ();
            if (e > x_hi)
            {
                x_hi = e;
            }
        }
        if (x_lo == INT32_MAX)
        {
            x_lo = area.x0;
        }
        if (x_hi < x_lo)
        {
            x_hi = x_lo;
        }
        float def[raster::MAX_CHANNELS];
        memcpy (def, defp, sizeof def);
        premultiply (def, 1, n, alpha);
        size_t dw = dst.width ();
        std::vector<float> def_row (dw * n);
        {
            std::vector<float> row ((size_t) (x_hi - x_lo) * n);
            for (size_t i = 0; i < (size_t) (x_hi - x_lo); i++)
            {
                memcpy (&row[i * n], def, n * sizeof (float));
            }
            horizontal (row.data (), def_row.data (), hx, x_lo, x_hi, def, n);
        }
        const size_t BAND        = 64;
        const size_t READ_BUDGET = (size_t) 1 << 21;
        size_t row_floats        = (size_t) (x_hi - x_lo) * n;
        int32_t rows_per_read    = (int32_t) (row_floats > 0 ? READ_BUDGET / row_floats : 1);
        if (rows_per_read < 1)
        {
            rows_per_read = 1;
        }
        if (rows_per_read > geom::TILE_SIZE)
        {
            rows_per_read = geom::TILE_SIZE;
        }
        size_t band_count = (vy.size () + BAND - 1) / BAND;
        std::vector<std::vector<float>> results (band_count);
#pragma omp parallel for schedule(dynamic)
        for (ptrdiff_t bi = 0; bi < (ptrdiff_t) band_count; bi++)
        {
            size_t b0 = (size_t) bi * BAND, b1 = b0 + BAND < vy.size () ? b0 + BAND : vy.size ();
            int32_t y_lo = INT32_MAX, y_hi = INT32_MIN;
            for (size_t i = b0; i < b1; i++)
            {
                if (!vy[i].weights.empty () && vy[i].start < y_lo)
                {
                    y_lo = vy[i].start;
                }
                int32_t e = vy[i].start + (int32_t) vy[i].weights.size ();
                if (e > y_hi)
                {
                    y_hi = e;
                }
            }
            if (y_lo == INT32_MAX)
            {
                y_lo = area.y0;
            }
            if (y_hi < y_lo)
            {
                y_hi = y_lo;
            }
            std::vector<float>& res = results[(size_t) bi];
            res.assign ((b1 - b0) * dw * n, 0.0f);
            std::vector<float> rows, hrow (dw * n);
            /* Source rows stream through a tile row at a time; rows with nothing stored are the
               default row. */
            for (int32_t y = y_lo; y < y_hi;)
            {
                int32_t y1 = (geom::floor_div (y, geom::TILE_SIZE) + 1) * geom::TILE_SIZE;
                if (y1 > y_hi)
                {
                    y1 = y_hi;
                }
                Rect tile_row  = {x_lo, y, x_hi, y1};
                bool stored    = x_hi > x_lo && s.has_tiles_in (tile_row);
                size_t rw      = (size_t) (x_hi - x_lo) * n;
                int32_t read_y = y, read_y1 = y;
                for (int32_t r = y; r < y1; r++)
                {
                    const float* h;
                    if (stored)
                    {
                        if (r >= read_y1)
                        {
                            read_y    = r;
                            read_y1   = r + rows_per_read < y1 ? r + rows_per_read : y1;
                            Rect read = {x_lo, read_y, x_hi, read_y1};
                            s.read_region (read, &rows);
                        }
                        float* row = &rows[(size_t) (r - read_y) * rw];
                        premultiply (row, (size_t) (x_hi - x_lo), n, alpha);
                        horizontal (row, hrow.data (), hx, x_lo, x_hi, def, n);
                        h = hrow.data ();
                    }
                    else
                    {
                        h = def_row.data ();
                    }
                    for (size_t i = b0; i < b1; i++)
                    {
                        const Taps& t = vy[i];
                        if (r < t.start || r >= t.start + (int32_t) t.weights.size ())
                        {
                            continue;
                        }
                        float w  = t.weights[(size_t) (r - t.start)];
                        float* d = &res[(i - b0) * dw * n];
                        for (size_t k = 0; k < dw * n; k++)
                        {
                            d[k] += h[k] * w;
                        }
                    }
                }
                y = y1;
            }
            for (size_t i = b0; i < b1; i++)
            {
                float* d = &res[(i - b0) * dw * n];
                for (size_t k = 0; k < dw * n; k++)
                {
                    d[k] += def_row[k] * vy[i].outside;
                }
            }
            if (alpha)
            {
                for (size_t k = 0; k < res.size () / n; k++)
                {
                    float& a = res[k * n + n - 1];
                    a        = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
                }
            }
            unpremultiply (res.data (), res.size () / n, n, alpha);
        }
        for (size_t bi = 0; bi < band_count; bi++)
        {
            size_t b0 = bi * BAND, b1 = b0 + BAND < vy.size () ? b0 + BAND : vy.size ();
            Rect r = {dst.x0, dst.y0 + (int32_t) b0, dst.x1, dst.y0 + (int32_t) b1};
            out.write_region (r, results[bi].data (), results[bi].size ());
        }
        return out;
    }

    /* The filter's passes; between passes the image is stored in its own format, which clips (and
       rounds) it as Photoshop does. Preserve Details' light sharpening when enlarging is not ported
       yet (it enlarges with Lanczos alone). */
    raster::Surface resize (
        const raster::Surface& s, double sx, double sy, Resample filter, bool has_canvas, Rect canvas)
    {
        std::vector<Pass> passes;
        plan (filter, sx, sy, &passes);
        raster::Surface cur = s;
        for (size_t i = 0; i < passes.size (); i++)
        {
            cur    = resize_pass (cur, passes[i], has_canvas, canvas);
            canvas = scaled_rect (canvas, passes[i].sx, passes[i].sy);
        }
        cur.prune ();
        return cur;
    }

    /* The surface scaled by (sx, sy) about the document origin; its default pixel is kept. */
    raster::Surface resize_surface (const raster::Surface& s, double sx, double sy, Resample filter)
    {
        return resize (s, sx, sy, filter, false, Rect ());
    }

    /* resize_surface() for a layer, mask or channel of a document whose canvas is `canvas` (Image
       Size): at the canvas edges the pixels repeat outwards, so content reaching an edge keeps it. */
    raster::Surface resize_surface_in_canvas (
        const raster::Surface& s, double sx, double sy, Resample filter, Rect canvas)
    {
        return resize (s, sx, sy, filter, true, canvas);
    }

} // namespace algo
