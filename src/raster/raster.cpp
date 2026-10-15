#include "raster/raster.h"

#include <algorithm>
#include <set>
#include <stdint.h>
#include <string.h>

namespace raster
{

    using color::ColorMode;
    using color::PixelFormat;
    using color::SampleType;
    using geom::Rect;
    using geom::TILE_SIZE;
    using geom::TileCoord;

    const size_t TILE_PIXELS = (size_t) TILE_SIZE * (size_t) TILE_SIZE;

    Surface::Surface () : format_ (PixelFormat::rgba8 ()), default_pixel_ (PixelFormat::rgba8 ().bytes_per_pixel (), 0)
    {
    }

    Surface::Surface (PixelFormat format) : format_ (format), default_pixel_ (format.bytes_per_pixel (), 0) {}

    Surface Surface::with_default (PixelFormat format, const float* pixel)
    {
        Surface s (format);
        encode_pixel (format, pixel, s.default_pixel_.data ());
        return s;
    }

    bool Surface::from_interleaved (PixelFormat format, Rect r, const uint8_t* bytes, size_t len, Surface* out)
    {
        Surface s (format);
        if (!s.write_interleaved (r, bytes, len))
        {
            return false;
        }
        *out = s;
        return true;
    }

    PixelFormat Surface::format () const
    {
        return format_;
    }

    size_t Surface::channels () const
    {
        return format_.channels ();
    }

    size_t Surface::tile_count () const
    {
        return tiles_.size ();
    }

    const TileMap& Surface::tiles () const
    {
        return tiles_;
    }

    const Tile* Surface::tile (TileCoord c) const
    {
        TileMap::const_iterator it = tiles_.find (c);
        return it == tiles_.end () ? NULL : it->second.get ();
    }

    void Surface::default_pixel (float* out) const
    {
        decode_pixel (format_, default_pixel_.data (), out);
    }

    const uint8_t* Surface::default_bytes () const
    {
        return default_pixel_.data ();
    }

    std::shared_ptr<Tile> Surface::new_tile (const uint8_t* pixel) const
    {
        size_t bpp              = format_.bytes_per_pixel ();
        std::shared_ptr<Tile> t = std::make_shared<Tile> ();
        t->data.assign (TILE_PIXELS * bpp, 0);
        bool zero = true;
        for (size_t i = 0; i < bpp; i++)
        {
            if (pixel[i] != 0)
            {
                zero = false;
            }
        }
        if (!zero)
        {
            for (size_t p = 0; p < TILE_PIXELS; p++)
            {
                memcpy (&t->data[p * bpp], pixel, bpp);
            }
        }
        return t;
    }

    /* Sample index of (x, y) inside tile c. */
    size_t Surface::tile_offset (TileCoord c, int32_t x, int32_t y) const
    {
        size_t lx = (size_t) (x - c.tx * TILE_SIZE);
        size_t ly = (size_t) (y - c.ty * TILE_SIZE);
        return (ly * (size_t) TILE_SIZE + lx) * format_.channels ();
    }

    /* Allocates a missing tile (filled with the default pixel) or un-shares a shared one. */
    Tile* Surface::tile_mut (TileCoord c)
    {
        TileMap::iterator it = tiles_.find (c);
        if (it == tiles_.end ())
        {
            it = tiles_.insert (std::make_pair (c, new_tile (default_pixel_.data ()))).first;
        }
        else if (it->second.use_count () > 1)
        {
            it->second = std::make_shared<Tile> (*it->second);
        }
        return it->second.get ();
    }

    std::vector<TileEntry> Surface::take_tiles (Rect r)
    {
        std::vector<TileEntry> out;
        std::shared_ptr<Tile> blank;
        std::vector<TileCoord> coords = r.tiles ();
        for (size_t i = 0; i < coords.size (); i++)
        {
            TileEntry e;
            e.coord              = coords[i];
            TileMap::iterator it = tiles_.find (coords[i]);
            if (it != tiles_.end ())
            {
                e.tile = it->second;
                tiles_.erase (it);
            }
            else
            {
                if (!blank)
                {
                    blank = new_tile (default_pixel_.data ());
                }
                e.tile = blank;
            }
            out.push_back (e);
        }
        return out;
    }

    void Surface::put_tiles (std::vector<TileEntry>& entries)
    {
        for (size_t i = 0; i < entries.size (); i++)
        {
            tiles_[entries[i].coord] = entries[i].tile;
        }
        entries.clear ();
    }

    std::shared_ptr<Tile> Surface::solid_tile (const float* px) const
    {
        uint8_t enc[MAX_CHANNELS * 4];
        encode_pixel (format_, px, enc);
        return new_tile (enc);
    }

    Rect Surface::tile_bounds () const
    {
        Rect r;
        for (TileMap::const_iterator it = tiles_.begin (); it != tiles_.end (); ++it)
        {
            r = r.unite (it->first.rect ());
        }
        return r;
    }

    int32_t ring_distance (TileCoord c, int32_t gx0, int32_t gy0, int32_t gx1, int32_t gy1)
    {
        int32_t d = c.tx - gx0;
        if (gx1 - c.tx < d)
        {
            d = gx1 - c.tx;
        }
        if (c.ty - gy0 < d)
        {
            d = c.ty - gy0;
        }
        if (gy1 - c.ty < d)
        {
            d = gy1 - c.ty;
        }
        return d;
    }

    /* Exact bounds of pixels that differ from the default pixel. Tiles are scanned from the outside
       of the tile grid inwards; a tile entirely inside the bounds found so far is skipped. */
    Rect Surface::content_bounds () const
    {
        if (tiles_.empty ())
        {
            return Rect ();
        }
        size_t bpp  = format_.bytes_per_pixel ();
        int32_t gx0 = INT32_MAX, gy0 = INT32_MAX, gx1 = INT32_MIN, gy1 = INT32_MIN;
        for (TileMap::const_iterator it = tiles_.begin (); it != tiles_.end (); ++it)
        {
            gx0 = std::min (gx0, it->first.tx);
            gy0 = std::min (gy0, it->first.ty);
            gx1 = std::max (gx1, it->first.tx);
            gy1 = std::max (gy1, it->first.ty);
        }
        std::vector<std::pair<int32_t, TileMap::const_iterator>> order;
        order.reserve (tiles_.size ());
        for (TileMap::const_iterator it = tiles_.begin (); it != tiles_.end (); ++it)
        {
            order.push_back (std::make_pair (ring_distance (it->first, gx0, gy0, gx1, gy1), it));
        }
        std::stable_sort (order.begin (), order.end (),
            [] (const std::pair<int32_t, TileMap::const_iterator>& a,
                const std::pair<int32_t, TileMap::const_iterator>& b) { return a.first < b.first; });

        const uint8_t* dp = default_pixel_.data ();
        size_t ts         = (size_t) TILE_SIZE;
        Rect out;
        for (size_t k = 0; k < order.size (); k++)
        {
            TileCoord c                      = order[k].second->first;
            const std::vector<uint8_t>& data = order[k].second->second->data;
            Rect origin                      = c.rect ();
            if (!out.is_empty () && out.contains_rect (origin))
            {
                continue;
            }
            size_t x0 = SIZE_MAX, y0 = SIZE_MAX, x1 = 0, y1 = 0;
            for (size_t y = 0; y < ts; y++)
            {
                const uint8_t* row = &data[y * ts * bpp];
                size_t first       = ts;
                for (size_t x = 0; x < ts; x++)
                {
                    if (memcmp (row + x * bpp, dp, bpp) != 0)
                    {
                        first = x;
                        break;
                    }
                }
                if (first == ts)
                {
                    continue;
                }
                size_t last = first;
                for (size_t x = ts; x-- > first;)
                {
                    if (memcmp (row + x * bpp, dp, bpp) != 0)
                    {
                        last = x;
                        break;
                    }
                }
                x0 = std::min (x0, first);
                x1 = std::max (x1, last + 1);
                y0 = std::min (y0, y);
                y1 = y + 1;
            }
            if (x0 != SIZE_MAX)
            {
                Rect found = {(int32_t) x0, (int32_t) y0, (int32_t) x1, (int32_t) y1};
                out        = out.unite (found.translate (origin.x0, origin.y0));
            }
        }
        return out;
    }

    bool Surface::has_tiles_in (Rect r) const
    {
        std::vector<TileCoord> coords = r.tiles ();
        for (size_t i = 0; i < coords.size (); i++)
        {
            if (tiles_.count (coords[i]))
            {
                return true;
            }
        }
        return false;
    }

    void Surface::prune ()
    {
        size_t bpp = default_pixel_.size ();
        for (TileMap::iterator it = tiles_.begin (); it != tiles_.end ();)
        {
            const std::vector<uint8_t>& d = it->second->data;
            bool all_default              = true;
            for (size_t p = 0; p < TILE_PIXELS; p++)
            {
                if (memcmp (&d[p * bpp], default_pixel_.data (), bpp) != 0)
                {
                    all_default = false;
                    break;
                }
            }
            if (all_default)
            {
                it = tiles_.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    void Surface::read_pixel (int32_t x, int32_t y, float* out) const
    {
        TileCoord c   = TileCoord::containing (x, y);
        size_t n      = channels ();
        const Tile* t = tile (c);
        if (t == NULL)
        {
            for (size_t i = 0; i < n; i++)
            {
                out[i] = color::read_sample (default_pixel_.data (), format_.sample, i);
            }
            return;
        }
        size_t base = tile_offset (c, x, y);
        for (size_t i = 0; i < n; i++)
        {
            out[i] = color::read_sample (t->data.data (), format_.sample, base + i);
        }
    }

    void Surface::write_pixel (int32_t x, int32_t y, const float* px)
    {
        TileCoord c = TileCoord::containing (x, y);
        size_t n    = channels ();
        size_t base = tile_offset (c, x, y);
        Tile* t     = tile_mut (c);
        for (size_t i = 0; i < n; i++)
        {
            color::write_sample (t->data.data (), format_.sample, base + i, px[i]);
        }
    }

    float Surface::sample_channel (int32_t x, int32_t y, size_t c) const
    {
        TileCoord tc  = TileCoord::containing (x, y);
        const Tile* t = tile (tc);
        if (t == NULL)
        {
            return color::read_sample (default_pixel_.data (), format_.sample, c);
        }
        return color::read_sample (t->data.data (), format_.sample, tile_offset (tc, x, y) + c);
    }

    /* Channel c at (x0 + i*step, y) for i in 0..count, looking each tile up once per run. */
    void Surface::sample_row_strided (int32_t y, int32_t x0, int32_t step, size_t c, float* out, size_t count) const
    {
        if (step < 1)
        {
            step = 1;
        }
        float dp   = color::read_sample (default_pixel_.data (), format_.sample, c);
        size_t n   = channels ();
        int32_t ty = geom::floor_div (y, TILE_SIZE);
        size_t ly  = (size_t) geom::floor_mod (y, TILE_SIZE);
        size_t i   = 0;
        while (i < count)
        {
            int32_t x        = x0 + (int32_t) i * step;
            int32_t tx       = geom::floor_div (x, TILE_SIZE);
            int32_t tile_end = (tx + 1) * TILE_SIZE;
            int32_t in_tile  = ((tile_end - x) + step - 1) / step;
            size_t end       = i + (size_t) (in_tile < 1 ? 1 : in_tile);
            if (end > count)
            {
                end = count;
            }
            TileCoord tc  = {tx, ty};
            const Tile* t = tile (tc);
            if (t == NULL)
            {
                for (size_t k = i; k < end; k++)
                {
                    out[k] = dp;
                }
            }
            else
            {
                for (size_t k = i; k < end; k++)
                {
                    size_t lx = (size_t) (x + (int32_t) (k - i) * step - tx * TILE_SIZE);
                    out[k] =
                        color::read_sample (t->data.data (), format_.sample, (ly * (size_t) TILE_SIZE + lx) * n + c);
                }
            }
            i = end;
        }
    }

    void Surface::rgba (int32_t x, int32_t y, const color::CmykSpace* cmyk, float out[4]) const
    {
        float px[MAX_CHANNELS];
        read_pixel (x, y, px);
        to_rgba (format_, px, cmyk, out);
    }

    bool region_len (Rect r, size_t per_pixel, size_t* out)
    {
        size_t w = r.width ();
        size_t h = r.height ();
        if (h != 0 && w > SIZE_MAX / h)
        {
            return false;
        }
        size_t wh = w * h;
        if (per_pixel != 0 && wh > SIZE_MAX / 8 / per_pixel)
        {
            return false;
        }
        *out = wh * per_pixel;
        return true;
    }

    bool Surface::read_region (Rect r, std::vector<float>* out) const
    {
        size_t n = channels ();
        size_t len;
        if (!region_len (r, n, &len))
        {
            return false;
        }
        out->assign (len, 0.0f);
        size_t w = r.width ();
        float dp[MAX_CHANNELS];
        default_pixel (dp);
        std::vector<TileCoord> coords = r.tiles ();
        for (size_t k = 0; k < coords.size (); k++)
        {
            TileCoord tc  = coords[k];
            Rect tr       = tc.rect ().intersect (r);
            const Tile* t = tile (tc);
            size_t span   = (size_t) tr.width () * n;
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                float* dst = out->data () + (((size_t) (y - r.y0)) * w + (size_t) (tr.x0 - r.x0)) * n;
                if (t != NULL)
                {
                    size_t base = tile_offset (tc, tr.x0, y);
                    for (size_t i = 0; i < span; i++)
                    {
                        dst[i] = color::read_sample (t->data.data (), format_.sample, base + i);
                    }
                }
                else
                {
                    for (size_t i = 0; i < span; i += n)
                    {
                        memcpy (dst + i, dp, n * sizeof (float));
                    }
                }
            }
        }
        return true;
    }

    bool Surface::write_region (Rect r, const float* data, size_t len)
    {
        size_t n = channels ();
        size_t expected;
        if (!region_len (r, n, &expected) || len != expected)
        {
            return false;
        }
        size_t w                      = r.width ();
        SampleType sample             = format_.sample;
        std::vector<TileCoord> coords = r.tiles ();
        for (size_t k = 0; k < coords.size (); k++)
        {
            TileCoord tc = coords[k];
            Rect tr      = tc.rect ().intersect (r);
            Tile* t      = tile_mut (tc);
            size_t span  = (size_t) tr.width () * n;
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                const float* src = data + (((size_t) (y - r.y0)) * w + (size_t) (tr.x0 - r.x0)) * n;
                size_t base      = tile_offset (tc, tr.x0, y);
                for (size_t i = 0; i < span; i++)
                {
                    color::write_sample (t->data.data (), sample, base + i, src[i]);
                }
            }
        }
        return true;
    }

    /* r as straight RGBA floats (4 per pixel, w*h pixels), with fast paths for RGBA8, GrayA8 and
       CMYK8. Missing tiles read as the default pixel. */
    void Surface::read_rgba (Rect r, const color::CmykSpace* cmyk, float* out) const
    {
        size_t w = r.width ();
        size_t n = channels ();
        float dpx[MAX_CHANNELS];
        default_pixel (dpx);
        float dp[4];
        to_rgba (format_, dpx, cmyk, dp);
        std::vector<TileCoord> coords = r.tiles ();
        for (size_t k = 0; k < coords.size (); k++)
        {
            TileCoord tc  = coords[k];
            Rect tr       = tc.rect ().intersect (r);
            const Tile* t = tile (tc);
            size_t count  = tr.width ();
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                float* dst = out + (((size_t) (y - r.y0)) * w + (size_t) (tr.x0 - r.x0)) * 4;
                if (t == NULL)
                {
                    for (size_t i = 0; i < count; i++)
                    {
                        memcpy (dst + i * 4, dp, sizeof dp);
                    }
                    continue;
                }
                size_t base         = tile_offset (tc, tr.x0, y);
                const uint8_t* data = t->data.data ();
                if (format_.mode == ColorMode::Rgb && format_.sample == SampleType::U8 && format_.alpha)
                {
                    const uint8_t* s = data + base;
                    for (size_t i = 0; i < count; i++)
                    {
                        for (int c = 0; c < 4; c++)
                        {
                            dst[i * 4 + c] = (float) s[i * 4 + c] / 255.0f;
                        }
                    }
                }
                else if (format_.mode == ColorMode::Grayscale && format_.sample == SampleType::U8 && format_.alpha)
                {
                    const uint8_t* s = data + base;
                    for (size_t i = 0; i < count; i++)
                    {
                        float g        = (float) s[i * 2] / 255.0f;
                        dst[i * 4 + 0] = g;
                        dst[i * 4 + 1] = g;
                        dst[i * 4 + 2] = g;
                        dst[i * 4 + 3] = (float) s[i * 2 + 1] / 255.0f;
                    }
                }
                else if (format_.mode == ColorMode::Cmyk && format_.sample == SampleType::U8)
                {
                    /* Identical ink bytes share the ICC result; alpha is independent. */
                    const uint8_t* s    = data + base;
                    bool have_prev      = false;
                    uint8_t prev_ink[4] = {0, 0, 0, 0};
                    float prev_rgb[3]   = {0.0f, 0.0f, 0.0f};
                    for (size_t i = 0; i < count; i++)
                    {
                        const uint8_t* ink = s + i * n;
                        if (!have_prev || memcmp (ink, prev_ink, 4) != 0)
                        {
                            float c4[4] = {ink[0] / 255.0f, ink[1] / 255.0f, ink[2] / 255.0f, ink[3] / 255.0f};
                            color::cmyk_to_rgb (cmyk, c4, prev_rgb);
                            memcpy (prev_ink, ink, 4);
                            have_prev = true;
                        }
                        dst[i * 4 + 0] = prev_rgb[0];
                        dst[i * 4 + 1] = prev_rgb[1];
                        dst[i * 4 + 2] = prev_rgb[2];
                        dst[i * 4 + 3] = n > 4 ? (float) ink[4] / 255.0f : 1.0f;
                    }
                }
                else
                {
                    float px[MAX_CHANNELS];
                    for (size_t i = 0; i < count; i++)
                    {
                        for (size_t c = 0; c < n; c++)
                        {
                            px[c] = color::read_sample (data, format_.sample, base + i * n + c);
                        }
                        to_rgba (format_, px, cmyk, dst + i * 4);
                    }
                }
            }
        }
    }

    /* r as straight 8-bit RGBA (4 bytes per pixel). RGBA8 surfaces copy tile bytes directly. */
    void Surface::read_rgba8 (Rect r, const color::CmykSpace* cmyk, uint8_t* out) const
    {
        size_t w   = r.width ();
        bool rgba8 = format_.mode == ColorMode::Rgb && format_.sample == SampleType::U8 && format_.alpha;
        float dpx[MAX_CHANNELS];
        default_pixel (dpx);
        float dp[4];
        to_rgba (format_, dpx, cmyk, dp);
        uint8_t dp8[4];
        for (int c = 0; c < 4; c++)
        {
            color::write_sample (dp8, SampleType::U8, (size_t) c, dp[c]);
        }
        std::vector<float> row;
        std::vector<TileCoord> coords = r.tiles ();
        for (size_t k = 0; k < coords.size (); k++)
        {
            TileCoord tc  = coords[k];
            Rect tr       = tc.rect ().intersect (r);
            const Tile* t = tile (tc);
            size_t count  = tr.width ();
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                uint8_t* dst = out + (((size_t) (y - r.y0)) * w + (size_t) (tr.x0 - r.x0)) * 4;
                if (t == NULL)
                {
                    for (size_t i = 0; i < count; i++)
                    {
                        memcpy (dst + i * 4, dp8, 4);
                    }
                }
                else if (rgba8)
                {
                    memcpy (dst, t->data.data () + tile_offset (tc, tr.x0, y), count * 4);
                }
                else
                {
                    row.resize (count * 4);
                    Rect line = {tr.x0, y, tr.x1, y + 1};
                    read_rgba (line, cmyk, row.data ());
                    for (size_t i = 0; i < count * 4; i++)
                    {
                        color::write_sample (dst, SampleType::U8, i, row[i]);
                    }
                }
            }
        }
    }

    /* Fully covered tiles share one tile, so a solid fill costs one tile however large r is. */
    void Surface::fill_rect (Rect r, const float* px)
    {
        size_t bpp = format_.bytes_per_pixel ();
        uint8_t enc[MAX_CHANNELS * 4];
        encode_pixel (format_, px, enc);
        std::shared_ptr<Tile> full;
        std::vector<TileCoord> coords = r.tiles ();
        for (size_t k = 0; k < coords.size (); k++)
        {
            TileCoord tc = coords[k];
            Rect tr      = tc.rect ().intersect (r);
            if (tr == tc.rect ())
            {
                if (!full)
                {
                    full = new_tile (enc);
                }
                tiles_[tc] = full;
                continue;
            }
            Tile* t = tile_mut (tc);
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                size_t first = tile_offset (tc, tr.x0, y) / channels ();
                for (size_t p = 0; p < (size_t) tr.width (); p++)
                {
                    memcpy (&t->data[(first + p) * bpp], enc, bpp);
                }
            }
        }
    }

    bool Surface::to_interleaved (Rect r, std::vector<uint8_t>* out) const
    {
        size_t bpp = format_.bytes_per_pixel ();
        size_t len;
        if (!region_len (r, bpp, &len))
        {
            return false;
        }
        out->assign (len, 0);
        size_t w                      = r.width ();
        std::vector<TileCoord> coords = r.tiles ();
        for (size_t k = 0; k < coords.size (); k++)
        {
            TileCoord tc  = coords[k];
            Rect tr       = tc.rect ().intersect (r);
            const Tile* t = tile (tc);
            size_t span   = (size_t) tr.width () * bpp;
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                uint8_t* dst = out->data () + (((size_t) (y - r.y0)) * w + (size_t) (tr.x0 - r.x0)) * bpp;
                if (t != NULL)
                {
                    size_t src = tile_offset (tc, tr.x0, y) / channels () * bpp;
                    memcpy (dst, t->data.data () + src, span);
                }
                else
                {
                    for (size_t i = 0; i < span; i += bpp)
                    {
                        memcpy (dst + i, default_pixel_.data (), bpp);
                    }
                }
            }
        }
        return true;
    }

    bool Surface::write_interleaved (Rect r, const uint8_t* bytes, size_t len)
    {
        size_t bpp = format_.bytes_per_pixel ();
        size_t expected;
        if (!region_len (r, bpp, &expected) || len != expected)
        {
            return false;
        }
        size_t w                      = r.width ();
        std::vector<TileCoord> coords = r.tiles ();
        for (size_t k = 0; k < coords.size (); k++)
        {
            TileCoord tc = coords[k];
            Rect tr      = tc.rect ().intersect (r);
            Tile* t      = tile_mut (tc);
            size_t span  = (size_t) tr.width () * bpp;
            for (int32_t y = tr.y0; y < tr.y1; y++)
            {
                const uint8_t* src = bytes + (((size_t) (y - r.y0)) * w + (size_t) (tr.x0 - r.x0)) * bpp;
                size_t dst         = tile_offset (tc, tr.x0, y) / channels () * bpp;
                memcpy (t->data.data () + dst, src, span);
            }
        }
        return true;
    }

    /* The surface moved by whole pixels. `content` must hold every non-default pixel; only tiles it
       reaches are kept. Moves by whole tiles share the tiles themselves. */
    Surface Surface::translated (int32_t dx, int32_t dy, Rect content) const
    {
        Surface out (format_);
        out.default_pixel_ = default_pixel_;
        if (dx % TILE_SIZE == 0 && dy % TILE_SIZE == 0)
        {
            int32_t tx = dx / TILE_SIZE, ty = dy / TILE_SIZE;
            for (TileMap::const_iterator it = tiles_.begin (); it != tiles_.end (); ++it)
            {
                if (it->first.rect ().intersect (content).is_empty ())
                {
                    continue;
                }
                TileCoord moved   = {geom::add_sat (it->first.tx, tx), geom::add_sat (it->first.ty, ty)};
                out.tiles_[moved] = it->second;
            }
            return out;
        }
        size_t bpp = format_.bytes_per_pixel ();
        Rect moved = content.translate (dx, dy);
        std::set<TileCoord> targets;
        for (TileMap::const_iterator it = tiles_.begin (); it != tiles_.end (); ++it)
        {
            if (it->first.rect ().intersect (content).is_empty ())
            {
                continue;
            }
            std::vector<TileCoord> hit = it->first.rect ().translate (dx, dy).intersect (moved).tiles ();
            targets.insert (hit.begin (), hit.end ());
        }
        int64_t ts         = TILE_SIZE;
        int64_t tile_bytes = (int64_t) (TILE_PIXELS * bpp);
        for (std::set<TileCoord>::const_iterator dc = targets.begin (); dc != targets.end (); ++dc)
        {
            std::shared_ptr<Tile> tile     = new_tile (default_pixel_.data ());
            Rect dr                        = dc->rect ();
            Rect sr                        = dr.translate (geom::sub_sat (0, dx), geom::sub_sat (0, dy));
            std::vector<TileCoord> sources = sr.intersect (content).tiles ();
            for (size_t k = 0; k < sources.size (); k++)
            {
                TileCoord sc   = sources[k];
                const Tile* st = this->tile (sc);
                if (st == NULL)
                {
                    continue;
                }
                Rect part    = sc.rect ().intersect (sr);
                int64_t span = (int64_t) part.width () * (int64_t) bpp;
                for (int32_t y = part.y0; y < part.y1; y++)
                {
                    int64_t src =
                        (((int64_t) y - (int64_t) sc.ty * ts) * ts + ((int64_t) part.x0 - (int64_t) sc.tx * ts))
                        * (int64_t) bpp;
                    int64_t dst = (((int64_t) y + dy - dr.y0) * ts + ((int64_t) part.x0 + dx - dr.x0)) * (int64_t) bpp;
                    if (src < 0 || dst < 0 || src + span > tile_bytes || dst + span > tile_bytes)
                    {
                        continue;
                    }
                    memcpy (tile->data.data () + dst, st->data.data () + src, (size_t) span);
                }
            }
            out.tiles_[*dc] = tile;
        }
        return out;
    }

    /* Applies fn to every colour sample (not alpha), including the default pixel. `fn` runs on
       several threads at once, so it must not change shared state. */
    void Surface::map_color (float (*fn) (float v, void* user), void* user)
    {
        size_t colors = color::color_channels (format_.mode);
        size_t n      = channels ();
        float dp[MAX_CHANNELS];
        default_pixel (dp);
        for (size_t k = 0; k < colors; k++)
        {
            dp[k] = fn (dp[k], user);
        }
        encode_pixel (format_, dp, default_pixel_.data ());

        /* Parallel pattern used throughout: claim the tiles serially (tile_mut changes the map),
           then process each tile on its own thread. */
        std::vector<Tile*> work;
        for (TileMap::iterator it = tiles_.begin (); it != tiles_.end (); ++it)
        {
            work.push_back (tile_mut (it->first));
        }
        SampleType sample = format_.sample;

#pragma omp parallel for schedule(dynamic)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) work.size (); i++)
        {
            uint8_t* data = work[(size_t) i]->data.data ();
            for (size_t p = 0; p < TILE_PIXELS; p++)
            {
                for (size_t k = 0; k < colors; k++)
                {
                    size_t at = p * n + k;
                    color::write_sample (data, sample, at, fn (color::read_sample (data, sample, at), user));
                }
            }
        }
    }

    Surface Surface::convert (PixelFormat to, const color::CmykSpace* cmyk) const
    {
        float dp[MAX_CHANNELS];
        float dp_to[MAX_CHANNELS];
        default_pixel (dp);
        convert_pixel (format_, to, dp, cmyk, dp_to);
        Surface out      = with_default (to, dp_to);
        size_t from_n    = channels ();
        size_t to_n      = to.channels ();
        PixelFormat from = format_;

        std::vector<const Tile*> src;
        std::vector<Tile*> dst;
        for (TileMap::const_iterator it = tiles_.begin (); it != tiles_.end (); ++it)
        {
            src.push_back (it->second.get ());
            dst.push_back (out.tile_mut (it->first));
        }

#pragma omp parallel for schedule(dynamic)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) src.size (); i++)
        {
            const uint8_t* in = src[(size_t) i]->data.data ();
            uint8_t* outp     = dst[(size_t) i]->data.data ();
            float px_in[MAX_CHANNELS];
            float px_out[MAX_CHANNELS];
            for (size_t p = 0; p < TILE_PIXELS; p++)
            {
                for (size_t k = 0; k < from_n; k++)
                {
                    px_in[k] = color::read_sample (in, from.sample, p * from_n + k);
                }
                convert_pixel (from, to, px_in, cmyk, px_out);
                for (size_t k = 0; k < to_n; k++)
                {
                    color::write_sample (outp, to.sample, p * to_n + k, px_out[k]);
                }
            }
        }
        return out;
    }

    bool Surface::operator== (const Surface& o) const
    {
        if (format_ != o.format_ || default_pixel_ != o.default_pixel_ || tiles_.size () != o.tiles_.size ())
        {
            return false;
        }
        TileMap::const_iterator a = tiles_.begin ();
        TileMap::const_iterator b = o.tiles_.begin ();
        for (; a != tiles_.end (); ++a, ++b)
        {
            if (!(a->first == b->first))
            {
                return false;
            }
            if (a->second != b->second && a->second->data != b->second->data)
            {
                return false;
            }
        }
        return true;
    }

    bool Surface::operator!= (const Surface& o) const
    {
        return !(*this == o);
    }

    Rgba8Image rgba8_image_new (uint32_t width, uint32_t height)
    {
        Rgba8Image img;
        img.width  = width;
        img.height = height;
        img.pixels.assign ((size_t) width * (size_t) height * 4, 0);
        return img;
    }

    bool is_hdr (SampleType sample)
    {
        return sample == SampleType::F32;
    }

    void encode_pixel (PixelFormat format, const float* px, uint8_t* out)
    {
        size_t n = format.channels ();
        for (size_t i = 0; i < n; i++)
        {
            color::write_sample (out, format.sample, i, px[i]);
        }
    }

    void decode_pixel (PixelFormat format, const uint8_t* bytes, float* out)
    {
        size_t n = format.channels ();
        for (size_t i = 0; i < n; i++)
        {
            out[i] = color::read_sample (bytes, format.sample, i);
        }
    }

    /* One pixel between formats via sRGB RGBA. Returns the number of values written. */
    size_t convert_pixel (PixelFormat from, PixelFormat to, const float* px, const color::CmykSpace* cmyk, float* out)
    {
        if (from.mode == to.mode)
        {
            size_t n = color::color_channels (from.mode);
            float a  = from.alpha ? px[n] : 1.0f;
            for (size_t i = 0; i < n; i++)
            {
                out[i] = px[i];
            }
            if (!to.alpha)
            {
                return n;
            }
            out[n] = a;
            return n + 1;
        }
        float rgba[4];
        to_rgba (from, px, cmyk, rgba);
        return from_rgba (to, rgba, cmyk, out);
    }

    void to_rgba (PixelFormat format, const float* px, const color::CmykSpace* cmyk, float out[4])
    {
        size_t n = color::color_channels (format.mode);
        float a  = format.alpha ? px[n] : 1.0f;
        switch (format.mode)
        {
        case ColorMode::Grayscale:
        case ColorMode::Bitmap:
        case ColorMode::Duotone:
            out[0] = out[1] = out[2] = px[0];
            break;
        case ColorMode::Cmyk:
            color::cmyk_to_rgb (cmyk, px, out);
            break;
        case ColorMode::Lab:
            {
                float lab[3] = {px[0] * 100.0f, px[1] * 255.0f - 128.0f, px[2] * 255.0f - 128.0f};
                color::lab_to_srgb (lab, out);
                break;
            }
        default:
            out[0] = px[0];
            out[1] = px[1];
            out[2] = px[2];
            break;
        }
        out[3] = a;
    }

    size_t from_rgba (PixelFormat format, const float rgba[4], const color::CmykSpace* cmyk, float* out)
    {
        float rgb[3] = {rgba[0], rgba[1], rgba[2]};
        size_t n;
        switch (format.mode)
        {
        case ColorMode::Grayscale:
        case ColorMode::Bitmap:
        case ColorMode::Duotone:
            out[0] = color::rgb_to_gray (rgb);
            n      = 1;
            break;
        case ColorMode::Cmyk:
            color::rgb_to_cmyk (cmyk, rgb, out);
            n = 4;
            break;
        case ColorMode::Lab:
            {
                float lab[3];
                color::srgb_to_lab (rgb, lab);
                out[0] = lab[0] / 100.0f;
                out[1] = (lab[1] + 128.0f) / 255.0f;
                out[2] = (lab[2] + 128.0f) / 255.0f;
                n      = 3;
                break;
            }
        default:
            out[0] = rgb[0];
            out[1] = rgb[1];
            out[2] = rgb[2];
            n      = 3;
            break;
        }
        if (!format.alpha)
        {
            return n;
        }
        out[n] = rgba[3];
        return n + 1;
    }

} // namespace raster
