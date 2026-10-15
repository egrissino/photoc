#include "paint/paint.h"

#include <math.h>
#include <string.h>

namespace paint
{

    /* Brush engine core: the computed round tip, spacing, pressure, Flow building up within a stroke
       towards the Opacity ceiling, and the stroke composite (blend modes, Dissolve, erase,
       transparency lock, selection, 8-bit dither). Strokes are data, and every random value is a hash
       of the stroke seed and the dab index, so a journaled stroke replays exactly.

       Not ported yet: sampled tips, Shape Dynamics jitter, Scattering, Texture, Dual Brush, Color
       Dynamics, Transfer jitter, Brush Pose, Noise, Wet Edges, airbrush Build-up, speed spacing and
       Smoothing. */

    using geom::Rect;

    const uint64_t DITHER_SALT   = 0x0000646974686572ull;
    const float SMALL_TIP_RADIUS = 3.0f;
    const float SUBPIXEL[4]      = {-0.375f, -0.125f, 0.125f, 0.375f};
    const double MAX_GRID_STEPS  = 1e7;

    StrokePoint StrokePoint::make (double x, double y, float pressure)
    {
        StrokePoint p = {x, y, pressure, 0.0f, 0.0f, 0.0f, 1.0f, 0.0};
        return p;
    }

    BrushSettings BrushSettings::defaults ()
    {
        BrushSettings b;
        b.size             = 20.0f;
        b.hardness         = 1.0f;
        b.spacing          = 0.1f;
        b.opacity          = 1.0f;
        b.flow             = 1.0f;
        b.pressure_size    = true;
        b.pressure_opacity = false;
        b.color[0] = b.color[1] = b.color[2] = 0.0f;
        b.color[3]                           = 1.0f;
        b.background[0] = b.background[1] = b.background[2] = b.background[3] = 1.0f;
        b.erase                                                               = false;
        b.mode                                                                = color::BlendMode::Normal;
        b.angle                                                               = 0.0f;
        b.roundness                                                           = 1.0f;
        b.flip_x                                                              = false;
        b.flip_y                                                              = false;
        b.aliased                                                             = false;
        b.square                                                              = false;
        b.seed                                                                = 0;
        return b;
    }

    /* A final guard for direct users of the paint API: a malformed size must not become a giant
       allocation. Engine commands reject out-of-range values before this. */
    BrushSettings BrushSettings::bounded_for_render () const
    {
        BrushSettings b = *this;
        if (!isfinite (b.size))
        {
            b.size = 0.5f;
        }
        if (b.size < 0.5f)
        {
            b.size = 0.5f;
        }
        if (b.size > MAX_BRUSH_SIZE)
        {
            b.size = MAX_BRUSH_SIZE;
        }
        return b;
    }

    /* SplitMix64 finaliser. */
    uint64_t mix64 (uint64_t z)
    {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    /* Uniform in [0, 1). */
    float rand01 (uint64_t seed, uint64_t index, uint64_t stream)
    {
        uint64_t h = mix64 (seed ^ mix64 ((index * 0xD1B54A32D192ED03ull) ^ (stream * 0x8CB92BA72F3D8DD7ull)));
        return (float) (h >> 40) / (float) (1ull << 24);
    }

    /* Integer document coordinates to [0, 1) (noise, dissolve, dither). */
    float hash2 (int32_t x, int32_t y, uint64_t salt)
    {
        uint64_t k = ((uint64_t) (uint32_t) x << 32) | (uint64_t) (uint32_t) y;
        return (float) (mix64 (k ^ (salt * 0xA24BAED4963EE407ull)) >> 40) / (float) (1ull << 24);
    }

    /* FNV-1a then mixed: a seed for strokes that don't give one. */
    uint64_t seed_from_bytes (const uint8_t* bytes, size_t len)
    {
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < len; i++)
        {
            h ^= bytes[i];
            h *= 0x00000100000001B3ull;
        }
        return mix64 (h);
    }

    float clamp01 (float v)
    {
        return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }

    /* Opacity of a round tip at distance d: 1 in the hard core, then (1 - t^2)^4 to 0 at the radius
       (close to exp(-4.6 t^2) but exact and identical on every platform). */
    float tip_falloff (float d, float radius, float hardness)
    {
        float inner = radius * clamp01 (hardness);
        if (d <= inner || radius - inner < 1e-3f)
        {
            return 1.0f;
        }
        float t  = clamp01 ((d - inner) / (radius - inner));
        float s  = 1.0f - t * t;
        float s2 = s * s;
        return s2 * s2;
    }

    /* Where tip_falloff drops to 50%: the outline of the normal brush-tip cursor. */
    float half_coverage_radius (float radius, float hardness)
    {
        float h = isnan (hardness) ? 0.0f : clamp01 (hardness);
        float t = sqrtf (1.0f - sqrtf (sqrtf (0.5f)));
        return radius * (h + t * (1.0f - h));
    }

    /* Coverage of a round dab at distance d: anti-aliased edge times the hardness falloff. */
    float dab_coverage (float d, float radius, float hardness)
    {
        if (d >= radius + 0.5f)
        {
            return 0.0f;
        }
        float edge = clamp01 (radius + 0.5f - d);
        return edge * tip_falloff (d, radius, hardness);
    }

    /* Where an aliased (Pencil) dab lands on the pixel grid: the pixel centre for odd diameters, the
       nearest pixel corner for even ones. */
    void grid_center (double x, double y, float diameter, float* cx, float* cy)
    {
        long n = isfinite (diameter) ? (long) fmaxf (roundf (diameter), 1.0f) : 1;
        if (n % 2 == 1)
        {
            *cx = (float) (floor (x) + 0.5);
            *cy = (float) (floor (y) + 0.5);
        }
        else
        {
            *cx = (float) round (x);
            *cy = (float) round (y);
        }
    }

    StrokePoint lerp_pt (const StrokePoint& a, const StrokePoint& b, double f)
    {
        StrokePoint p;
        float ff   = (float) f;
        p.x        = a.x + (b.x - a.x) * f;
        p.y        = a.y + (b.y - a.y) * f;
        p.pressure = a.pressure + (b.pressure - a.pressure) * ff;
        p.tilt_x   = a.tilt_x + (b.tilt_x - a.tilt_x) * ff;
        p.tilt_y   = a.tilt_y + (b.tilt_y - a.tilt_y) * ff;
        p.rotation = a.rotation + (b.rotation - a.rotation) * ff;
        p.wheel    = a.wheel + (b.wheel - a.wheel) * ff;
        p.time     = a.time + (b.time - a.time) * f;
        return p;
    }

    /* Base (pre-jitter) diameter at a point: size times pen pressure when Pressure for Size is on.
       It drives spacing. */
    float base_diameter (const BrushSettings& b, const StrokePoint& p)
    {
        return b.pressure_size ? b.size * clamp01 (p.pressure) : b.size;
    }

    DabGenerator::DabGenerator (const BrushSettings& brush)
        : brush_ (brush.bounded_for_render ()), has_last_ (false), next_at_ (0.0), has_pending_ (false),
          has_initial_ (false), initial_dir_ (0.0f), dir_ (0.0f), step_ (0), grid_ (false), grid_offset_ (0.0),
          next_index_ (0)
    {
        memset (&last_, 0, sizeof last_);
        memset (&pending_, 0, sizeof pending_);
        /* A small aliased tip whose dabs are at most a pixel apart steps pixel by pixel: stepping by
           distance put two dabs in a row wherever the line crossed a pixel corner. */
        float spacing = brush_.spacing > 0.01f ? brush_.spacing : 0.01f;
        if (brush_.aliased && brush_.size * spacing <= 1.0f)
        {
            float cx, cy;
            grid_center (0.0, 0.0, brush_.size, &cx, &cy);
            grid_        = true;
            grid_offset_ = cx;
        }
    }

    double DabGenerator::step_len (const StrokePoint& p) const
    {
        float d    = fmaxf (base_diameter (brush_, p), 1.0f);
        double len = (double) (d * fmaxf (brush_.spacing, 0.01f));
        return len > 0.5 ? len : 0.5;
    }

    void DabGenerator::emit (const StrokePoint& p, std::vector<Dab>* out)
    {
        StepInput s = {p, dir_, has_initial_ ? initial_dir_ : dir_, step_};
        step_++;
        build (s, out);
    }

    void DabGenerator::build (const StepInput& s, std::vector<Dab>* out)
    {
        const BrushSettings& b = brush_;
        float diameter         = base_diameter (b, s.point);
        float roundness        = b.roundness < 0.01f ? 0.01f : b.roundness > 1.0f ? 1.0f : b.roundness;
        float opacity          = 1.0f;
        if (b.pressure_opacity)
        {
            opacity *= clamp01 (s.point.pressure);
        }
        Dab d;
        d.cx        = s.point.x;
        d.cy        = s.point.y;
        d.radius    = fmaxf (diameter / 2.0f, 0.5f);
        d.alpha     = clamp01 (b.flow);
        d.angle     = b.angle * (float) M_PI / 180.0f;
        d.roundness = roundness;
        d.flip_x    = b.flip_x;
        d.flip_y    = b.flip_y;
        d.opacity   = clamp01 (opacity);
        d.index     = next_index_++;
        out->push_back (d);
    }

    /* One step per pixel along the segment's major axis, on the line between the pixels holding its
       ends. False when the span is absurd: distance spacing takes over. */
    bool DabGenerator::grid_steps (const StrokePoint& a, const StrokePoint& p, std::vector<Dab>* out)
    {
        double x0 = grid_offset_ > 0.0 ? floor (a.x) : round (a.x);
        double y0 = grid_offset_ > 0.0 ? floor (a.y) : round (a.y);
        double dx = (grid_offset_ > 0.0 ? floor (p.x) : round (p.x)) - x0;
        double dy = (grid_offset_ > 0.0 ? floor (p.y) : round (p.y)) - y0;
        double n  = fmax (fabs (dx), fabs (dy));
        if (!isfinite (n) || n > MAX_GRID_STEPS)
        {
            return false;
        }
        uint64_t steps = (uint64_t) n;
        for (uint64_t i = 1; i <= steps; i++)
        {
            double t      = (double) i;
            StrokePoint q = lerp_pt (a, p, t / n);
            q.x           = x0 + round (dx * t / n) + grid_offset_;
            q.y           = y0 + round (dy * t / n) + grid_offset_;
            emit (q, out);
        }
        return true;
    }

    /* Walks the polyline and emits a dab every step_len pixels. */
    void DabGenerator::push (const StrokePoint* points, size_t count, std::vector<Dab>* out)
    {
        for (size_t k = 0; k < count; k++)
        {
            const StrokePoint& p = points[k];
            if (!has_last_)
            {
                has_last_    = true;
                last_        = p;
                has_pending_ = true;
                pending_     = p;
                next_at_     = step_len (p);
                continue;
            }
            StrokePoint a = last_;
            double dx = p.x - a.x, dy = p.y - a.y;
            double len = sqrt (dx * dx + dy * dy);
            if (len > 1e-9)
            {
                dir_ = (float) atan2 (-dy, dx);
                if (!has_initial_)
                {
                    has_initial_ = true;
                    initial_dir_ = dir_;
                }
            }
            if (has_pending_)
            {
                has_pending_ = false;
                emit (pending_, out);
            }
            if (len > 1e-9 && !(grid_ && grid_steps (a, p, out)))
            {
                while (next_at_ <= len + 1e-9)
                {
                    StrokePoint q = lerp_pt (a, p, next_at_ / len < 1.0 ? next_at_ / len : 1.0);
                    emit (q, out);
                    next_at_ += fmax (step_len (q), 0.25);
                }
                next_at_ -= len;
            }
            last_ = p;
        }
    }

    /* End of stroke: a lone first point still paints its dab. */
    void DabGenerator::finish (std::vector<Dab>* out)
    {
        if (has_pending_)
        {
            has_pending_ = false;
            emit (pending_, out);
        }
    }

    StrokeRenderer::StrokeRenderer (const BrushSettings& brush)
        : brush_ (brush.bounded_for_render ()), generator_ (brush)
    {
    }

    geom::Rect StrokeRenderer::bounds () const
    {
        return bounds_;
    }

    Rect StrokeRenderer::dab_rect (const Dab& d) const
    {
        int32_t rr = (int32_t) ceilf (d.radius) + 1;
        int32_t cx = (int32_t) floor (d.cx), cy = (int32_t) floor (d.cy);
        Rect r = {geom::sub_sat (cx, rr), geom::sub_sat (cy, rr), geom::add_sat (geom::add_sat (cx, rr), 1),
            geom::add_sat (geom::add_sat (cy, rr), 1)};
        return r;
    }

    /* One dab over `rect` into `out`: the tip shape times flow. Tips a few pixels across take each
       pixel's covered area (a 4x4 grid), so thin lines don't bead. */
    void StrokeRenderer::rasterize (const Dab& d, Rect rect, std::vector<float>* out) const
    {
        size_t w = rect.width (), h = rect.height ();
        out->assign (w * h, 0.0f);
        const BrushSettings& b = brush_;
        bool aliased           = b.aliased;
        float cx, cy;
        if (aliased)
        {
            grid_center (d.cx, d.cy, 2.0f * d.radius, &cx, &cy);
        }
        else
        {
            cx = (float) d.cx;
            cy = (float) d.cy;
        }
        float sn = sinf (d.angle), cs = cosf (d.angle);
        float fx = d.flip_x ? -1.0f : 1.0f, fy = d.flip_y ? -1.0f : 1.0f;
        float r            = d.radius;
        float ro           = fminf (fmaxf (d.roundness, 0.5f / r), 1.0f);
        float rm           = fmaxf (r * ro, 0.5f);
        float reach2       = (rm + 0.5f) * (rm + 0.5f);
        bool small         = !aliased && rm < SMALL_TIP_RADIUS;
        float small_reach2 = (rm + 0.75f) * (rm + 0.75f);
        for (size_t yy = 0; yy < h; yy++)
        {
            int32_t y = rect.y0 + (int32_t) yy;
            float dy  = (float) y + 0.5f - cy;
            for (size_t xx = 0; xx < w; xx++)
            {
                int32_t x = rect.x0 + (int32_t) xx;
                float dx  = (float) x + 0.5f - cx;
                /* The pixel offset in tip space: to y-up, rotate by -angle, flip. */
                float ux = dx, uy = -dy;
                float u = (ux * cs + uy * sn) * fx, v = (-ux * sn + uy * cs) * fy;
                float val;
                if (aliased && b.square)
                {
                    if (fabsf (dx) >= r || fabsf (dy) >= r)
                    {
                        continue;
                    }
                    val = 1.0f;
                }
                else if (small)
                {
                    float d2 = (u * ro) * (u * ro) + v * v;
                    if (d2 >= small_reach2)
                    {
                        continue;
                    }
                    float sum = 0.0f;
                    for (int sy = 0; sy < 4; sy++)
                    {
                        for (int sx = 0; sx < 4; sx++)
                        {
                            float sux = dx + SUBPIXEL[sx], suy = -(dy + SUBPIXEL[sy]);
                            float su = (sux * cs + suy * sn) * fx, sv = (-sux * sn + suy * cs) * fy;
                            float sd     = sqrtf ((su * ro) * (su * ro) + sv * sv);
                            float inside = clamp01 ((rm - sd) * 4.0f + 0.5f);
                            if (inside > 0.0f)
                            {
                                sum += inside * tip_falloff (fminf (sd, rm), rm, b.hardness);
                            }
                        }
                    }
                    val = sum / 16.0f;
                }
                else
                {
                    float d2 = (u * ro) * (u * ro) + v * v;
                    if (d2 >= reach2)
                    {
                        continue;
                    }
                    float dd = sqrtf (d2);
                    val      = aliased ? (dd <= rm ? 1.0f : 0.0f) : dab_coverage (dd, rm, b.hardness);
                }
                if (val <= 0.0f)
                {
                    continue;
                }
                (*out)[yy * w + xx] = val * d.alpha;
            }
        }
    }

    /* Flow builds up towards the dab's opacity ceiling: c <- c + v (ceil - c). */
    void StrokeRenderer::accumulate (Rect rect, const std::vector<float>& vals, float ceil)
    {
        if (rect.is_empty ())
        {
            return;
        }
        bounds_     = bounds_.unite (rect);
        size_t w    = rect.width ();
        int32_t t0x = geom::floor_div (rect.x0, COV_TILE), t0y = geom::floor_div (rect.y0, COV_TILE);
        int32_t t1x = geom::floor_div (rect.x1 - 1, COV_TILE), t1y = geom::floor_div (rect.y1 - 1, COV_TILE);
        for (int32_t ty = t0y; ty <= t1y; ty++)
        {
            for (int32_t tx = t0x; tx <= t1x; tx++)
            {
                Rect tile_rect = {tx * COV_TILE, ty * COV_TILE, (tx + 1) * COV_TILE, (ty + 1) * COV_TILE};
                Rect tr        = tile_rect.intersect (rect);
                if (tr.is_empty ())
                {
                    continue;
                }
                bool any = false;
                for (int32_t y = tr.y0; y < tr.y1 && !any; y++)
                {
                    for (int32_t x = tr.x0; x < tr.x1; x++)
                    {
                        if (vals[(size_t) (y - rect.y0) * w + (size_t) (x - rect.x0)] > 0.0f)
                        {
                            any = true;
                            break;
                        }
                    }
                }
                if (!any)
                {
                    continue;
                }
                std::pair<int32_t, int32_t> key (tx, ty);
                CovTile& tile = tiles_[key];
                if (tile.cov.empty ())
                {
                    tile.cov.assign ((size_t) COV_TILE * COV_TILE, 0.0f);
                }
                dirty_.insert (key);
                for (int32_t y = tr.y0; y < tr.y1; y++)
                {
                    for (int32_t x = tr.x0; x < tr.x1; x++)
                    {
                        float v = vals[(size_t) (y - rect.y0) * w + (size_t) (x - rect.x0)];
                        if (v <= 0.0f)
                        {
                            continue;
                        }
                        float& c = tile.cov[(size_t) (y - ty * COV_TILE) * COV_TILE + (size_t) (x - tx * COV_TILE)];
                        if (ceil > c)
                        {
                            c = c + v * (ceil - c);
                        }
                    }
                }
            }
        }
    }

    void StrokeRenderer::raster_pending ()
    {
        for (size_t i = 0; i < pending_.size (); i++)
        {
            Rect rect = dab_rect (pending_[i]);
            rasterize (pending_[i], rect, &scratch_);
            accumulate (rect, scratch_, pending_[i].opacity);
            bounds_ = bounds_.unite (rect);
        }
        pending_.clear ();
    }

    /* Feed input points (any chunking gives the same result). */
    void StrokeRenderer::push (const StrokePoint* points, size_t count)
    {
        generator_.push (points, count, &pending_);
        raster_pending ();
    }

    void StrokeRenderer::finish ()
    {
        generator_.finish (&pending_);
        raster_pending ();
    }

    float StrokeRenderer::coverage_at (int32_t x, int32_t y) const
    {
        std::pair<int32_t, int32_t> key (geom::floor_div (x, COV_TILE), geom::floor_div (y, COV_TILE));
        std::map<std::pair<int32_t, int32_t>, CovTile>::const_iterator it = tiles_.find (key);
        if (it == tiles_.end ())
        {
            return 0.0f;
        }
        return it->second
            .cov[(size_t) geom::floor_mod (y, COV_TILE) * COV_TILE + (size_t) geom::floor_mod (x, COV_TILE)];
    }

    /* The stroke's accumulated coverage over r (row-major), zero where nothing was painted. */
    void StrokeRenderer::coverage_in (Rect r, std::vector<float>* out) const
    {
        out->assign ((size_t) r.width () * r.height (), 0.0f);
        if (r.is_empty ())
        {
            return;
        }
        size_t w = r.width ();
        for (std::map<std::pair<int32_t, int32_t>, CovTile>::const_iterator it = tiles_.begin (); it != tiles_.end ();
             ++it)
        {
            Rect tile_rect = {it->first.first * COV_TILE, it->first.second * COV_TILE, (it->first.first + 1) * COV_TILE,
                (it->first.second + 1) * COV_TILE};
            Rect tr        = tile_rect.intersect (r);
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                const float* src =
                    &it->second.cov[(size_t) (y - tile_rect.y0) * COV_TILE + (size_t) (tr.x0 - tile_rect.x0)];
                memcpy (&(*out)[(size_t) (y - r.y0) * w + (size_t) (tr.x0 - r.x0)], src,
                    (size_t) tr.width () * sizeof (float));
            }
        }
    }

    /* The tiles changed since the last call (or composite), as one rectangle; they count as clean
       afterwards. */
    Rect StrokeRenderer::take_dirty_rect ()
    {
        Rect r;
        for (std::set<std::pair<int32_t, int32_t>>::const_iterator it = dirty_.begin (); it != dirty_.end (); ++it)
        {
            Rect t = {
                it->first * COV_TILE, it->second * COV_TILE, (it->first + 1) * COV_TILE, (it->second + 1) * COV_TILE};
            r = r.unite (t);
        }
        dirty_.clear ();
        return r.intersect (bounds_);
    }

    /* One dab's coverage (tip shape times flow) over its rectangle, as the stroke rasterizes it. */
    void StrokeRenderer::footprint (const Dab& d, Rect* rect, std::vector<float>* cov) const
    {
        *rect = dab_rect (d);
        rasterize (d, *rect, cov);
    }

    /* "Source over destination" at coverage k in native channels (works for every colour model). */
    void over_native (color::PixelFormat fmt, float* dst, const float* src, float k, bool lock_transparency)
    {
        size_t n = fmt.channels ();
        if (!fmt.alpha)
        {
            for (size_t c = 0; c < n; c++)
            {
                dst[c] += (src[c] - dst[c]) * k;
            }
            return;
        }
        size_t a = n - 1;
        float da = dst[a], sa = src[a] * k;
        float oa = sa + da * (1.0f - sa);
        if (oa > 0.0f)
        {
            for (size_t c = 0; c < a; c++)
            {
                dst[c] = (src[c] * sa + dst[c] * da * (1.0f - sa)) / oa;
            }
        }
        if (!lock_transparency)
        {
            dst[a] = oa;
        }
    }

    /* Before an 8-bit quantisation, every channel moves by the same position-hashed noise of under
       half a level, so soft, low-flow strokes keep their gradient as grain instead of bands. */
    void dither8 (float* px, size_t n, int32_t x, int32_t y)
    {
        float d = (hash2 (x, y, DITHER_SALT) - 0.5f) / 255.0f;
        for (size_t i = 0; i < n; i++)
        {
            px[i] += d;
        }
    }

    /* The stroke buffer onto `target`, reading the original pixels from `pre` (the surface before the
       stroke). With all = false only tiles touched since the last composite are redrawn (the live
       preview). Returns the damaged rectangle. */
    Rect StrokeRenderer::composite (const raster::Surface& pre, raster::Surface* target,
        const raster::Surface* selection, bool lock_transparency, bool all, const color::CmykSpace* cmyk)
    {
        std::vector<std::pair<int32_t, int32_t>> keys;
        if (all)
        {
            for (std::map<std::pair<int32_t, int32_t>, CovTile>::const_iterator it = tiles_.begin ();
                 it != tiles_.end (); ++it)
            {
                keys.push_back (it->first);
            }
        }
        else
        {
            keys.assign (dirty_.begin (), dirty_.end ());
        }
        dirty_.clear ();
        color::PixelFormat fmt = target->format ();
        size_t n               = fmt.channels ();
        const BrushSettings& b = brush_;
        float opacity          = clamp01 (b.opacity);
        float src[raster::MAX_CHANNELS];
        raster::from_rgba (fmt, b.color, cmyk, src);
        if (fmt.alpha)
        {
            src[n - 1] = b.color[3];
        }
        bool dither = fmt.sample == color::SampleType::U8;
        Rect dmg;
        std::vector<float> region;
        std::vector<float> sel;
        for (size_t k = 0; k < keys.size (); k++)
        {
            int32_t tx = keys[k].first, ty = keys[k].second;
            const CovTile& tile = tiles_[keys[k]];
            Rect tile_rect      = {tx * COV_TILE, ty * COV_TILE, (tx + 1) * COV_TILE, (ty + 1) * COV_TILE};
            Rect tr             = tile_rect.intersect (bounds_);
            if (tr.is_empty ())
            {
                continue;
            }
            pre.read_region (tr, &region);
            bool has_sel  = selection != NULL && selection->read_region (tr, &sel);
            size_t sel_ch = selection != NULL ? selection->channels () : 1;
            size_t w      = tr.width ();
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                for (int32_t x = tr.x0; x < tr.x1; x++)
                {
                    float c = tile.cov[(size_t) (y - ty * COV_TILE) * COV_TILE + (size_t) (x - tx * COV_TILE)];
                    if (c <= 0.0f)
                    {
                        continue;
                    }
                    size_t i = (size_t) (y - tr.y0) * w + (size_t) (x - tr.x0);
                    float s  = has_sel ? sel[i * sel_ch] : 1.0f;
                    float kk = fminf (c * opacity * s, 1.0f);
                    if (kk <= 0.0f)
                    {
                        continue;
                    }
                    if (b.mode == color::BlendMode::Dissolve)
                    {
                        kk = hash2 (x, y, b.seed) < kk ? 1.0f : 0.0f;
                        if (kk == 0.0f)
                        {
                            continue;
                        }
                    }
                    float* px = &region[i * n];
                    if (b.erase)
                    {
                        if (fmt.alpha && !lock_transparency)
                        {
                            px[n - 1] *= 1.0f - kk;
                        }
                        if (dither)
                        {
                            dither8 (px, n, x, y);
                        }
                        continue;
                    }
                    if (lock_transparency && fmt.alpha && px[n - 1] <= 0.0f)
                    {
                        continue;
                    }
                    if (b.mode == color::BlendMode::Normal || b.mode == color::BlendMode::Dissolve)
                    {
                        over_native (fmt, px, src, kk, lock_transparency);
                    }
                    else
                    {
                        float d[4], sr[4], o[4];
                        raster::to_rgba (fmt, px, cmyk, d);
                        raster::to_rgba (fmt, src, cmyk, sr);
                        color::composite (b.mode, d, sr, kk, o);
                        if (lock_transparency)
                        {
                            o[3] = d[3];
                        }
                        raster::from_rgba (fmt, o, cmyk, px);
                    }
                    if (dither)
                    {
                        dither8 (px, n, x, y);
                    }
                }
            }
            target->write_region (tr, region.data (), region.size ());
            dmg = dmg.unite (tr);
        }
        return all ? bounds_ : dmg;
    }

    /* A whole stroke onto `target` in one go. Returns the damaged rectangle. */
    Rect render_stroke (raster::Surface* target, const BrushSettings& brush, const std::vector<StrokePoint>& points,
        const raster::Surface* selection, bool lock_transparency, const color::CmykSpace* cmyk)
    {
        if (points.empty ())
        {
            return Rect ();
        }
        raster::Surface pre = *target;
        StrokeRenderer r (brush);
        r.push (points.data (), points.size ());
        r.finish ();
        return r.composite (pre, target, selection, lock_transparency, true, cmyk);
    }

} // namespace paint
