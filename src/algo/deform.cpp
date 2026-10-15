#include "algo/deform.h"

#include <algorithm>
#include <float.h>
#include <math.h>
#include <string.h>

namespace algo
{

    /* Free-form deformations: Warp's Bezier patch and preset styles, Puppet Warp's pins (moving
       least squares, after Schaefer, McPhail and Warren: rigid, similarity or affine), and
       homographies, all as forward maps of the plane. mesh_warp renders any of them: the source
       rectangle is cut into a fine grid, every grid point mapped, and each grid triangle filled by
       sampling the source at the inverse of its affine map. seam_carve is Content-Aware Scale's
       seam carving (Avidan and Shamir): the least noticeable paths are removed, or duplicated. */

    const char* const WARP_STYLE_IDS[WARP_STYLE_COUNT] = {"arc", "arcLower", "arcUpper", "arch", "bulge", "shellLower",
        "shellUpper", "flag", "wave", "fish", "rise", "fisheye", "inflate", "squeeze", "twist"};
    const char* const WARP_STYLE_LABELS[WARP_STYLE_COUNT] = {"Arc", "Arc Lower", "Arc Upper", "Arch", "Bulge",
        "Shell Lower", "Shell Upper", "Flag", "Wave", "Fish", "Rise", "Fisheye", "Inflate", "Squeeze", "Twist"};

    bool warp_style_from_id (const char* id, WarpStyle* out)
    {
        for (int i = 0; i < WARP_STYLE_COUNT; i++)
        {
            if (strcmp (id, WARP_STYLE_IDS[i]) == 0)
            {
                *out = (WarpStyle) i;
                return true;
            }
        }
        return false;
    }

    Deform Deform::patch_identity (const double rect[4])
    {
        Deform d;
        d.kind = DeformKind::Patch;
        memcpy (d.rect, rect, sizeof d.rect);
        for (int j = 0; j < 4; j++)
        {
            for (int i = 0; i < 4; i++)
            {
                d.pts[j * 4 + i][0] = rect[0] + (rect[2] - rect[0]) * i / 3.0;
                d.pts[j * 4 + i][1] = rect[1] + (rect[3] - rect[1]) * j / 3.0;
            }
        }
        d.style    = WarpStyle::Arc;
        d.vertical = false;
        d.bend     = 0.5;
        d.hdist = d.vdist = 0.0;
        d.mode            = PinMode::Normal;
        d.h               = Homography::identity ();
        return d;
    }

    void bernstein (double t, double out[4])
    {
        double u = 1.0 - t;
        out[0]   = u * u * u;
        out[1]   = 3 * u * u * t;
        out[2]   = 3 * u * t * t;
        out[3]   = t * t * t;
    }

    /* A preset style's displacement at normalized (x, y) in -1..1, in half-sizes. */
    void style_offset (WarpStyle s, double b, double x, double y, double* dx, double* dy)
    {
        *dx = *dy = 0.0;
        switch (s)
        {
        case WarpStyle::Arc:
            *dy = -b * (1 - x * x);
            break;
        case WarpStyle::ArcLower:
            *dy = -b * (1 - x * x) * (y + 1) / 2;
            break;
        case WarpStyle::ArcUpper:
            *dy = -b * (1 - x * x) * (1 - y) / 2;
            break;
        case WarpStyle::Arch:
            *dy = -b * (1 - x * x) * (1 - y) / 2;
            *dx = b * x * 0.25 * (1 - y) / 2;
            break;
        case WarpStyle::Bulge:
            *dy = b * (1 - x * x) * y * 0.5;
            break;
        case WarpStyle::ShellLower:
            *dy = b * (1 - x * x) * (y + 1) / 2 * 0.5;
            break;
        case WarpStyle::ShellUpper:
            *dy = -b * (1 - x * x) * (1 - y) / 2 * 0.5;
            break;
        case WarpStyle::Flag:
            *dy = b * sin (x * M_PI) * 0.25;
            break;
        case WarpStyle::Wave:
            *dy = b * sin (x * M_PI + y * M_PI * 0.5) * 0.25;
            break;
        case WarpStyle::Fish:
            *dy = b * (y * (1 - x * x) * 0.25 + sin (x * M_PI) * 0.15);
            break;
        case WarpStyle::Rise:
            *dy = -b * sin (x * M_PI / 2) * 0.5;
            break;
        case WarpStyle::Fisheye:
            {
                double r2 = x * x + y * y;
                if (r2 < 1.0)
                {
                    double k = b * (1 - r2) * 0.5;
                    *dx      = x * k;
                    *dy      = y * k;
                }
                break;
            }
        case WarpStyle::Inflate:
            *dx = b * x * (1 - y * y) * 0.25;
            *dy = b * y * (1 - x * x) * 0.25;
            break;
        case WarpStyle::Squeeze:
            *dx = -b * x * (1 - y * y) * 0.3;
            *dy = b * y * (1 - x * x) * 0.15;
            break;
        case WarpStyle::Twist:
            {
                double r = sqrt (x * x + y * y), a = b * M_PI * 0.5 * fmax (0.0, 1.0 - r);
                *dx = x * cos (a) - y * sin (a) - x;
                *dy = x * sin (a) + y * cos (a) - y;
                break;
            }
        }
    }

    /* Moving least squares: the deformation that best keeps the neighbourhood of (x, y) rigid
       (similar, affine) while taking each pin to its new place, pins weighted by inverse squared
       distance. */
    void pins_point (const Deform& d, double x, double y, double* ox, double* oy)
    {
        size_t n = d.from.size () / 2;
        *ox      = x;
        *oy      = y;
        if (n == 0)
        {
            return;
        }
        double sw = 0, px = 0, py = 0, qx = 0, qy = 0;
        std::vector<double> w (n);
        for (size_t i = 0; i < n; i++)
        {
            double ddx = d.from[i * 2] - x, ddy = d.from[i * 2 + 1] - y, d2 = ddx * ddx + ddy * ddy;
            if (d2 < 1e-12)
            {
                *ox = d.to[i * 2];
                *oy = d.to[i * 2 + 1];
                return;
            }
            w[i] = 1.0 / d2;
            sw += w[i];
            px += w[i] * d.from[i * 2];
            py += w[i] * d.from[i * 2 + 1];
            qx += w[i] * d.to[i * 2];
            qy += w[i] * d.to[i * 2 + 1];
        }
        px /= sw;
        py /= sw;
        qx /= sw;
        qy /= sw;
        if (n == 1)
        {
            *ox = x + qx - px;
            *oy = y + qy - py;
            return;
        }
        double vx = x - px, vy = y - py;
        if (d.mode == PinMode::Distort)
        {
            /* Affine: (v - p*) M + q*, M = (sum w p^T p)^-1 sum w p^T q. */
            double a11 = 0, a12 = 0, a22 = 0, b11 = 0, b12 = 0, b21 = 0, b22 = 0;
            for (size_t i = 0; i < n; i++)
            {
                double hx = d.from[i * 2] - px, hy = d.from[i * 2 + 1] - py, gx = d.to[i * 2] - qx,
                       gy = d.to[i * 2 + 1] - qy;
                a11 += w[i] * hx * hx;
                a12 += w[i] * hx * hy;
                a22 += w[i] * hy * hy;
                b11 += w[i] * hx * gx;
                b12 += w[i] * hx * gy;
                b21 += w[i] * hy * gx;
                b22 += w[i] * hy * gy;
            }
            double det = a11 * a22 - a12 * a12;
            if (fabs (det) < 1e-12)
            {
                *ox = x + qx - px;
                *oy = y + qy - py;
                return;
            }
            double i11 = a22 / det, i12 = -a12 / det, i22 = a11 / det;
            double m11 = i11 * b11 + i12 * b21, m12 = i11 * b12 + i12 * b22, m21 = i12 * b11 + i22 * b21,
                   m22 = i12 * b12 + i22 * b22;
            *ox        = vx * m11 + vy * m21 + qx;
            *oy        = vx * m12 + vy * m22 + qy;
            return;
        }
        /* Similarity / rigid: f(v) = sum q^_i A_i (/ mu), A_i = w_i [p^; -p^perp][v; -v perp]^T. */
        double fx = 0, fy = 0, mu = 0;
        for (size_t i = 0; i < n; i++)
        {
            double hx = d.from[i * 2] - px, hy = d.from[i * 2 + 1] - py, gx = d.to[i * 2] - qx,
                   gy = d.to[i * 2 + 1] - qy;
            /* A = w [[hx, hy], [hy, -hx]] [[vx, vy], [vy, -vx]]^T  (perp(x, y) = (-y, x)). */
            double a11 = w[i] * (hx * vx + hy * vy), a12 = w[i] * (hx * vy - hy * vx);
            double a21 = w[i] * (hy * vx - hx * vy), a22 = w[i] * (hy * vy + hx * vx);
            fx += gx * a11 + gy * a21;
            fy += gx * a12 + gy * a22;
            mu += w[i] * (hx * hx + hy * hy);
        }
        if (d.mode == PinMode::Rigid)
        {
            double len = sqrt (fx * fx + fy * fy), vl = sqrt (vx * vx + vy * vy);
            if (len > 1e-12)
            {
                *ox = fx / len * vl + qx;
                *oy = fy / len * vl + qy;
            }
            else
            {
                *ox = x + qx - px;
                *oy = y + qy - py;
            }
            return;
        }
        *ox = fx / mu + qx;
        *oy = fy / mu + qy;
    }

    void deform_point (const Deform& d, double x, double y, double* ox, double* oy)
    {
        double w = d.rect[2] - d.rect[0], h = d.rect[3] - d.rect[1];
        switch (d.kind)
        {
        case DeformKind::Patch:
            {
                double u = w != 0 ? (x - d.rect[0]) / w : 0, v = h != 0 ? (y - d.rect[1]) / h : 0, bu[4], bv[4];
                bernstein (u, bu);
                bernstein (v, bv);
                *ox = *oy = 0.0;
                for (int j = 0; j < 4; j++)
                {
                    for (int i = 0; i < 4; i++)
                    {
                        *ox += bu[i] * bv[j] * d.pts[j * 4 + i][0];
                        *oy += bu[i] * bv[j] * d.pts[j * 4 + i][1];
                    }
                }
                return;
            }
        case DeformKind::Style:
            {
                double nx = w != 0 ? 2 * (x - d.rect[0]) / w - 1 : 0, ny = h != 0 ? 2 * (y - d.rect[1]) / h - 1 : 0, dx,
                       dy;
                if (d.vertical)
                {
                    style_offset (d.style, d.bend, ny, nx, &dy, &dx);
                }
                else
                {
                    style_offset (d.style, d.bend, nx, ny, &dx, &dy);
                }
                dy += ny * d.hdist * nx * 0.5;
                dx += nx * d.vdist * ny * 0.5;
                *ox = x + dx * w / 2;
                *oy = y + dy * h / 2;
                return;
            }
        case DeformKind::Pins:
            pins_point (d, x, y, ox, oy);
            return;
        default:
            d.h.apply (x, y, ox, oy);
            return;
        }
    }

    /* Gaussian elimination with partial pivoting; false when singular. */
    bool solve_linear (std::vector<double>* a, std::vector<double>* b, size_t n)
    {
        for (size_t c = 0; c < n; c++)
        {
            size_t piv = c;
            for (size_t r = c + 1; r < n; r++)
            {
                if (fabs ((*a)[r * n + c]) > fabs ((*a)[piv * n + c]))
                {
                    piv = r;
                }
            }
            if (fabs ((*a)[piv * n + c]) < 1e-14)
            {
                return false;
            }
            if (piv != c)
            {
                for (size_t k = 0; k < n; k++)
                {
                    std::swap ((*a)[c * n + k], (*a)[piv * n + k]);
                }
                std::swap ((*b)[c], (*b)[piv]);
            }
            for (size_t r = 0; r < n; r++)
            {
                if (r == c)
                {
                    continue;
                }
                double f = (*a)[r * n + c] / (*a)[c * n + c];
                if (f == 0)
                {
                    continue;
                }
                for (size_t k = c; k < n; k++)
                {
                    (*a)[r * n + k] -= f * (*a)[c * n + k];
                }
                (*b)[r] -= f * (*b)[c];
            }
        }
        for (size_t c = 0; c < n; c++)
        {
            (*b)[c] /= (*a)[c * n + c];
        }
        return true;
    }

    /* The Bezier patch over rect that best fits target points (least squares), given at a
       samples x samples grid over the rect, row by row. */
    void fit_patch (const double rect[4], const std::vector<double>& targets, int samples, double out[16][2])
    {
        std::vector<double> ata (256, 0.0), bx (16, 0.0), by (16, 0.0);
        for (int sj = 0; sj < samples; sj++)
        {
            for (int si = 0; si < samples; si++)
            {
                double u = (double) si / (samples - 1), v = (double) sj / (samples - 1), bu[4], bv[4];
                bernstein (u, bu);
                bernstein (v, bv);
                double tx = targets[((size_t) sj * samples + si) * 2],
                       ty = targets[((size_t) sj * samples + si) * 2 + 1];
                double basis[16];
                for (int j = 0; j < 4; j++)
                {
                    for (int i = 0; i < 4; i++)
                    {
                        basis[j * 4 + i] = bu[i] * bv[j];
                    }
                }
                for (int r = 0; r < 16; r++)
                {
                    for (int c = 0; c < 16; c++)
                    {
                        ata[(size_t) (r * 16 + c)] += basis[r] * basis[c];
                    }
                    bx[(size_t) r] += basis[r] * tx;
                    by[(size_t) r] += basis[r] * ty;
                }
            }
        }
        std::vector<double> a2 = ata;
        if (!solve_linear (&ata, &bx, 16) || !solve_linear (&a2, &by, 16))
        {
            return;
        }
        for (int k = 0; k < 16; k++)
        {
            out[k][0] = bx[(size_t) k];
            out[k][1] = by[(size_t) k];
        }
        (void) rect;
    }

    /* The Bezier patch that best fits a style, so a preset can be refined by hand. */
    void style_to_patch (const Deform& style, Deform* out)
    {
        *out        = Deform::patch_identity (style.rect);
        const int S = 9;
        std::vector<double> t ((size_t) S * S * 2);
        double w = style.rect[2] - style.rect[0], h = style.rect[3] - style.rect[1];
        for (int sj = 0; sj < S; sj++)
        {
            for (int si = 0; si < S; si++)
            {
                deform_point (style, style.rect[0] + w * si / (S - 1), style.rect[1] + h * sj / (S - 1),
                    &t[((size_t) sj * S + si) * 2], &t[((size_t) sj * S + si) * 2 + 1]);
            }
        }
        fit_patch (style.rect, t, S, out->pts);
    }

    struct MeshSrc
    {
        const float* px;
        geom::Rect rect;
        size_t n;
        bool clamp;
    };

    /* The source at continuous (u, v), pixel centres at +0.5; transparent (or clamped) outside. */
    void mesh_sample (const MeshSrc& s, double u, double v, Interp interp, float* out)
    {
        u -= 0.5;
        v -= 0.5;
        for (size_t c = 0; c < s.n; c++)
        {
            out[c] = 0.0f;
        }
        int32_t w = (int32_t) s.rect.width (), h = (int32_t) s.rect.height ();
        double lu = u - s.rect.x0, lv = v - s.rect.y0;
        if (interp == Interp::Nearest)
        {
            int32_t x = (int32_t) floor (lu + 0.5), y = (int32_t) floor (lv + 0.5);
            if (s.clamp)
            {
                x = std::min (std::max (x, 0), w - 1);
                y = std::min (std::max (y, 0), h - 1);
            }
            if (x < 0 || y < 0 || x >= w || y >= h)
            {
                return;
            }
            memcpy (out, &s.px[((size_t) y * (size_t) w + (size_t) x) * s.n], s.n * sizeof (float));
            return;
        }
        int32_t ix = (int32_t) floor (lu), iy = (int32_t) floor (lv);
        double fx = lu - ix, fy = lv - iy;
        for (int dy = 0; dy < 2; dy++)
        {
            for (int dx = 0; dx < 2; dx++)
            {
                int32_t x = ix + dx, y = iy + dy;
                if (s.clamp)
                {
                    x = std::min (std::max (x, 0), w - 1);
                    y = std::min (std::max (y, 0), h - 1);
                }
                if (x < 0 || y < 0 || x >= w || y >= h)
                {
                    continue;
                }
                double wt      = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                const float* p = &s.px[((size_t) y * (size_t) w + (size_t) x) * s.n];
                for (size_t c = 0; c < s.n; c++)
                {
                    out[c] += (float) (p[c] * wt);
                }
            }
        }
    }

    struct MeshDst
    {
        std::vector<float>* px;
        geom::Rect rect;
        size_t n;
    };

    /* One triangle: destination corners d (x, y), their source points s. */
    void fill_triangle (const MeshSrc& src, MeshDst* dst, const double d[3][2], const double s[3][2], Interp interp)
    {
        double area = (d[1][0] - d[0][0]) * (d[2][1] - d[0][1]) - (d[2][0] - d[0][0]) * (d[1][1] - d[0][1]);
        if (fabs (area) < 1e-12)
        {
            return;
        }
        double x0 = fmin (d[0][0], fmin (d[1][0], d[2][0])), x1 = fmax (d[0][0], fmax (d[1][0], d[2][0]));
        double y0 = fmin (d[0][1], fmin (d[1][1], d[2][1])), y1 = fmax (d[0][1], fmax (d[1][1], d[2][1]));
        int32_t px0 = std::max ((int32_t) floor (x0), dst->rect.x0), px1 = std::min ((int32_t) ceil (x1), dst->rect.x1);
        int32_t py0 = std::max ((int32_t) floor (y0), dst->rect.y0), py1 = std::min ((int32_t) ceil (y1), dst->rect.y1);
        float tmp[raster::MAX_CHANNELS];
        for (int32_t y = py0; y < py1; y++)
        {
            for (int32_t x = px0; x < px1; x++)
            {
                double cx = x + 0.5, cy = y + 0.5;
                double l1 = ((d[1][0] - cx) * (d[2][1] - cy) - (d[2][0] - cx) * (d[1][1] - cy)) / area;
                double l2 = ((d[2][0] - cx) * (d[0][1] - cy) - (d[0][0] - cx) * (d[2][1] - cy)) / area;
                double l3 = 1.0 - l1 - l2;
                if (l1 < -1e-7 || l2 < -1e-7 || l3 < -1e-7)
                {
                    continue;
                }
                double u = l1 * s[0][0] + l2 * s[1][0] + l3 * s[2][0], v = l1 * s[0][1] + l2 * s[1][1] + l3 * s[2][1];
                mesh_sample (src, u, v, interp, tmp);
                memcpy (&(*dst->px)[((size_t) (y - dst->rect.y0) * dst->rect.width () + (size_t) (x - dst->rect.x0))
                                    * dst->n],
                    tmp, dst->n * sizeof (float));
            }
        }
    }

    raster::Surface mesh_warp (const raster::Surface& source, geom::Rect area, const Deform& d, Interp interp)
    {
        color::PixelFormat fmt = source.format ();
        float dflt[raster::MAX_CHANNELS];
        source.default_pixel (dflt);
        raster::Surface out = raster::Surface::with_default (fmt, dflt);
        if (area.is_empty ())
        {
            return out;
        }
        size_t n   = fmt.channels ();
        bool alpha = fmt.alpha;
        int N      = std::min (std::max ((int) (std::max (area.width (), area.height ()) / 6), 8), 128);
        std::vector<double> g ((size_t) (N + 1) * (N + 1) * 2);
        double mnx = DBL_MAX, mny = DBL_MAX, mxx = -DBL_MAX, mxy = -DBL_MAX;
        for (int j = 0; j <= N; j++)
        {
            for (int i = 0; i <= N; i++)
            {
                double sx = area.x0 + (double) area.width () * i / N, sy = area.y0 + (double) area.height () * j / N,
                       dx, dy;
                deform_point (d, sx, sy, &dx, &dy);
                if (!isfinite (dx) || !isfinite (dy))
                {
                    return out;
                }
                g[((size_t) j * (N + 1) + i) * 2]     = dx;
                g[((size_t) j * (N + 1) + i) * 2 + 1] = dy;
                mnx                                   = fmin (mnx, dx);
                mny                                   = fmin (mny, dy);
                mxx                                   = fmax (mxx, dx);
                mxy                                   = fmax (mxy, dy);
            }
        }
        const double LIM = 1 << 20;
        geom::Rect dr    = {(int32_t) fmax (floor (mnx), -LIM) - 1, (int32_t) fmax (floor (mny), -LIM) - 1,
               (int32_t) fmin (ceil (mxx), LIM) + 1, (int32_t) fmin (ceil (mxy), LIM) + 1};
        if (dr.is_empty () || (uint64_t) dr.width () * dr.height () > ((uint64_t) 1 << 28))
        {
            return out;
        }
        geom::Rect sr = area.inflate (2);
        std::vector<float> spx;
        if (!source.read_region (sr, &spx))
        {
            return out;
        }
        size_t scount = spx.size () / n;
        if (alpha)
        {
            for (size_t i = 0; i < scount; i++)
            {
                float a = spx[i * n + n - 1];
                for (size_t c = 0; c + 1 < n; c++)
                {
                    spx[i * n + c] *= a;
                }
            }
        }
        std::vector<float> dpx ((size_t) dr.width () * dr.height () * n, 0.0f);
        if (!alpha)
        {
            for (size_t i = 0; i < dpx.size () / n; i++)
            {
                memcpy (&dpx[i * n], dflt, n * sizeof (float));
            }
        }
        MeshSrc ms = {spx.data (), sr, n, !alpha};
        MeshDst md = {&dpx, dr, n};
        /* Two passes over alternate rows of cells, so threads never share an edge. */
        for (int pass = 0; pass < 2; pass++)
        {
#pragma omp parallel for schedule(dynamic)
            for (int j = pass; j < N; j += 2)
            {
                for (int i = 0; i < N; i++)
                {
                    double s00[2] = {
                        area.x0 + (double) area.width () * i / N, area.y0 + (double) area.height () * j / N};
                    double s11[2]     = {area.x0 + (double) area.width () * (i + 1) / N,
                            area.y0 + (double) area.height () * (j + 1) / N};
                    const double *p00 = &g[((size_t) j * (N + 1) + i) * 2],
                                 *p10 = &g[((size_t) j * (N + 1) + i + 1) * 2];
                    const double *p01 = &g[((size_t) (j + 1) * (N + 1) + i) * 2],
                                 *p11 = &g[((size_t) (j + 1) * (N + 1) + i + 1) * 2];
                    double d1[3][2]   = {{p00[0], p00[1]}, {p10[0], p10[1]}, {p11[0], p11[1]}};
                    double s1[3][2]   = {{s00[0], s00[1]}, {s11[0], s00[1]}, {s11[0], s11[1]}};
                    double d2[3][2]   = {{p00[0], p00[1]}, {p11[0], p11[1]}, {p01[0], p01[1]}};
                    double s2[3][2]   = {{s00[0], s00[1]}, {s11[0], s11[1]}, {s00[0], s11[1]}};
                    fill_triangle (ms, &md, d1, s1, interp);
                    fill_triangle (ms, &md, d2, s2, interp);
                }
            }
        }
        if (alpha)
        {
            for (size_t i = 0; i < dpx.size () / n; i++)
            {
                float a = dpx[i * n + n - 1];
                for (size_t c = 0; c + 1 < n; c++)
                {
                    dpx[i * n + c] = a > 1e-6f ? dpx[i * n + c] / a : 0.0f;
                }
            }
        }
        out.write_region (dr, dpx.data (), dpx.size ());
        out.prune ();
        return out;
    }

    /* ---- Seam carving ---- */

    /* Gradient energy of one pixel, protected pixels made very costly. */
    float energy_at (const std::vector<float>& img, size_t w, size_t h, size_t ch, const std::vector<float>& protect,
        size_t x, size_t y)
    {
        size_t xl = x > 0 ? x - 1 : x, xr = x + 1 < w ? x + 1 : x, yu = y > 0 ? y - 1 : y, yd = y + 1 < h ? y + 1 : y;
        float s = 0;
        for (size_t c = 0; c < ch; c++)
        {
            s += fabsf (img[(y * w + xr) * ch + c] - img[(y * w + xl) * ch + c]);
            s += fabsf (img[(yd * w + x) * ch + c] - img[(yu * w + x) * ch + c]);
        }
        return s + (protect.empty () ? 0.0f : protect[y * w + x] * 1000.0f);
    }

    void seam_energy (const std::vector<float>& img, size_t w, size_t h, size_t ch, const std::vector<float>& protect,
        std::vector<float>* e)
    {
        e->assign (w * h, 0.0f);
#pragma omp parallel for schedule(static)
        for (ptrdiff_t y = 0; y < (ptrdiff_t) h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                (*e)[(size_t) y * w + x] = energy_at (img, w, h, ch, protect, x, (size_t) y);
            }
        }
    }

    /* The cheapest top-to-bottom 8-connected path: one x per row. */
    void find_seam (const std::vector<float>& e, size_t w, size_t h, std::vector<float>* m, std::vector<size_t>* seam)
    {
        m->assign (e.begin (), e.end ());
        for (size_t y = 1; y < h; y++)
        {
            const float* up = &(*m)[(y - 1) * w];
            float* row      = &(*m)[y * w];
            for (size_t x = 0; x < w; x++)
            {
                float best = up[x];
                if (x > 0 && up[x - 1] < best)
                {
                    best = up[x - 1];
                }
                if (x + 1 < w && up[x + 1] < best)
                {
                    best = up[x + 1];
                }
                row[x] += best;
            }
        }
        seam->assign (h, 0);
        size_t bx = 0;
        for (size_t x = 1; x < w; x++)
        {
            if ((*m)[(h - 1) * w + x] < (*m)[(h - 1) * w + bx])
            {
                bx = x;
            }
        }
        (*seam)[h - 1] = bx;
        for (size_t y = h - 1; y-- > 0;)
        {
            size_t x = (*seam)[y + 1], best = x;
            if (x > 0 && (*m)[y * w + x - 1] < (*m)[y * w + best])
            {
                best = x - 1;
            }
            if (x + 1 < w && (*m)[y * w + x + 1] < (*m)[y * w + best])
            {
                best = x + 1;
            }
            (*seam)[y] = best;
        }
    }

    /* One seam taken out of the image, its protection map, its energy (refreshed beside the seam)
       and, when given, its map back to original columns. */
    void remove_seam (std::vector<float>* img, std::vector<float>* protect, std::vector<float>* e,
        std::vector<size_t>* idx, size_t w, size_t h, size_t ch, const std::vector<size_t>& seam)
    {
        size_t nw = w - 1;
        for (size_t y = 0; y < h; y++)
        {
            size_t sx  = seam[y];
            float* row = &(*img)[y * w * ch];
            float* dst = &(*img)[y * nw * ch];
            memmove (dst, row, sx * ch * sizeof (float));
            memmove (dst + sx * ch, row + (sx + 1) * ch, (w - sx - 1) * ch * sizeof (float));
            memmove (&(*e)[y * nw], &(*e)[y * w], sx * sizeof (float));
            memmove (&(*e)[y * nw + sx], &(*e)[y * w + sx + 1], (w - sx - 1) * sizeof (float));
            if (!protect->empty ())
            {
                memmove (&(*protect)[y * nw], &(*protect)[y * w], sx * sizeof (float));
                memmove (&(*protect)[y * nw + sx], &(*protect)[y * w + sx + 1], (w - sx - 1) * sizeof (float));
            }
            if (idx != NULL)
            {
                memmove (&(*idx)[y * nw], &(*idx)[y * w], sx * sizeof (size_t));
                memmove (&(*idx)[y * nw + sx], &(*idx)[y * w + sx + 1], (w - sx - 1) * sizeof (size_t));
            }
        }
        img->resize (nw * h * ch);
        e->resize (nw * h);
        if (!protect->empty ())
        {
            protect->resize (nw * h);
        }
        if (idx != NULL)
        {
            idx->resize (nw * h);
        }
        /* Only pixels next to the seam (in this row or the rows beside it) see new neighbours. */
        for (size_t y = 0; y < h; y++)
        {
            size_t lo = seam[y], hi = seam[y];
            if (y > 0)
            {
                lo = std::min (lo, seam[y - 1]);
                hi = std::max (hi, seam[y - 1]);
            }
            if (y + 1 < h)
            {
                lo = std::min (lo, seam[y + 1]);
                hi = std::max (hi, seam[y + 1]);
            }
            size_t x0 = lo >= 2 ? lo - 2 : 0, x1 = std::min (hi + 2, nw);
            for (size_t x = x0; x < x1; x++)
            {
                (*e)[y * nw + x] = energy_at (*img, nw, h, ch, *protect, x, y);
            }
        }
    }

    /* Columns of `img` (and the protection map alongside) from w to nw. */
    void carve_width (std::vector<float>* img, std::vector<float>* protect, size_t* w, size_t h, size_t ch, size_t nw)
    {
        std::vector<float> e, m;
        std::vector<size_t> seam;
        if (*w > nw)
        {
            seam_energy (*img, *w, h, ch, *protect, &e);
            while (*w > nw)
            {
                find_seam (e, *w, h, &m, &seam);
                remove_seam (img, protect, &e, NULL, *w, h, ch, seam);
                (*w)--;
            }
        }
        while (*w < nw)
        {
            /* Seams found by removing them from a copy, then each doubled in the original. */
            size_t k              = std::min (nw - *w, std::max ((size_t) 1, *w / 2));
            std::vector<float> ci = *img, cp = *protect;
            std::vector<size_t> idx (*w * h);
            for (size_t y = 0; y < h; y++)
            {
                for (size_t x = 0; x < *w; x++)
                {
                    idx[y * *w + x] = x;
                }
            }
            size_t cw = *w;
            std::vector<std::vector<size_t>> chosen (h);
            seam_energy (ci, cw, h, ch, cp, &e);
            for (size_t s = 0; s < k; s++)
            {
                find_seam (e, cw, h, &m, &seam);
                for (size_t y = 0; y < h; y++)
                {
                    chosen[y].push_back (idx[y * cw + seam[y]]);
                }
                remove_seam (&ci, &cp, &e, &idx, cw, h, ch, seam);
                cw--;
            }
            size_t ow = *w + k;
            std::vector<float> ni (ow * h * ch), np (protect->empty () ? 0 : ow * h);
            for (size_t y = 0; y < h; y++)
            {
                std::vector<size_t>& c = chosen[y];
                std::sort (c.begin (), c.end ());
                size_t o = 0, ci2 = 0;
                for (size_t x = 0; x < *w; x++)
                {
                    memcpy (&ni[(y * ow + o) * ch], &(*img)[(y * *w + x) * ch], ch * sizeof (float));
                    if (!np.empty ())
                    {
                        np[y * ow + o] = (*protect)[y * *w + x];
                    }
                    o++;
                    while (ci2 < c.size () && c[ci2] == x)
                    {
                        size_t xr = x + 1 < *w ? x + 1 : x;
                        for (size_t q = 0; q < ch; q++)
                        {
                            ni[(y * ow + o) * ch + q] =
                                0.5f * ((*img)[(y * *w + x) * ch + q] + (*img)[(y * *w + xr) * ch + q]);
                        }
                        if (!np.empty ())
                        {
                            np[y * ow + o] = (*protect)[y * *w + x];
                        }
                        o++;
                        ci2++;
                    }
                }
            }
            *img     = ni;
            *protect = np;
            *w       = ow;
        }
    }

    void transpose (const std::vector<float>& in, size_t w, size_t h, size_t ch, std::vector<float>* out)
    {
        out->resize (in.size ());
        for (size_t y = 0; y < h; y++)
        {
            for (size_t x = 0; x < w; x++)
            {
                memcpy (&(*out)[(x * h + y) * ch], &in[(y * w + x) * ch], ch * sizeof (float));
            }
        }
    }

    void seam_carve (const std::vector<float>& px, size_t w, size_t h, size_t ch, size_t nw, size_t nh,
        const std::vector<float>* protect, std::vector<float>* out)
    {
        std::vector<float> img = px, prot = protect != NULL ? *protect : std::vector<float> ();
        size_t cw = w;
        carve_width (&img, &prot, &cw, h, ch, nw);
        if (nh != h)
        {
            std::vector<float> t, tp;
            transpose (img, nw, h, ch, &t);
            if (!prot.empty ())
            {
                transpose (prot, nw, h, 1, &tp);
            }
            size_t ch2 = h;
            carve_width (&t, &tp, &ch2, nw, ch, nh);
            transpose (t, nh, nw, ch, &img);
        }
        *out = img;
    }

} // namespace algo
