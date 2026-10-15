#pragma once

#include "color/color.h"
#include "color/convert.h"
#include "geom/geom.h"
#include <map>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace raster
{

    struct Tile
    {
        std::vector<uint8_t> data;
    };

    struct TileEntry
    {
        geom::TileCoord coord;
        std::shared_ptr<Tile> tile;
    };

    typedef std::map<geom::TileCoord, std::shared_ptr<Tile>> TileMap;

    class Surface
    {
    public:
        Surface ();
        explicit Surface (color::PixelFormat format);
        static Surface with_default (color::PixelFormat format, const float* pixel);
        static bool from_interleaved (
            color::PixelFormat format, geom::Rect r, const uint8_t* bytes, size_t len, Surface* out);

        color::PixelFormat format () const;
        size_t channels () const;
        size_t tile_count () const;
        const TileMap& tiles () const;
        const Tile* tile (geom::TileCoord c) const;
        void default_pixel (float* out) const;
        const uint8_t* default_bytes () const;

        Tile* tile_mut (geom::TileCoord c);
        std::vector<TileEntry> take_tiles (geom::Rect r);
        void put_tiles (std::vector<TileEntry>& tiles);
        std::shared_ptr<Tile> solid_tile (const float* px) const;

        geom::Rect tile_bounds () const;
        geom::Rect content_bounds () const;
        bool has_tiles_in (geom::Rect r) const;
        void prune ();

        void read_pixel (int32_t x, int32_t y, float* out) const;
        void write_pixel (int32_t x, int32_t y, const float* px);
        float sample_channel (int32_t x, int32_t y, size_t c) const;
        void sample_row_strided (int32_t y, int32_t x0, int32_t step, size_t c, float* out, size_t count) const;
        void rgba (int32_t x, int32_t y, const color::CmykSpace* cmyk, float out[4]) const;

        bool read_region (geom::Rect r, std::vector<float>* out) const;
        bool write_region (geom::Rect r, const float* data, size_t len);
        void read_rgba (geom::Rect r, const color::CmykSpace* cmyk, float* out) const;
        void read_rgba8 (geom::Rect r, const color::CmykSpace* cmyk, uint8_t* out) const;
        void fill_rect (geom::Rect r, const float* px);

        bool to_interleaved (geom::Rect r, std::vector<uint8_t>* out) const;
        bool write_interleaved (geom::Rect r, const uint8_t* bytes, size_t len);

        Surface translated (int32_t dx, int32_t dy, geom::Rect content) const;
        void map_color (float (*fn) (float v, void* user), void* user);
        Surface convert (color::PixelFormat to, const color::CmykSpace* cmyk) const;

        bool operator== (const Surface& o) const;
        bool operator!= (const Surface& o) const;

    private:
        std::shared_ptr<Tile> new_tile (const uint8_t* pixel) const;
        size_t tile_offset (geom::TileCoord c, int32_t x, int32_t y) const;

        color::PixelFormat format_;
        std::vector<uint8_t> default_pixel_;
        TileMap tiles_;
    };

    struct Rgba8Image
    {
        uint32_t width;
        uint32_t height;
        std::vector<uint8_t> pixels;
    };

    Rgba8Image rgba8_image_new (uint32_t width, uint32_t height);
    bool is_hdr (color::SampleType sample);

    bool region_len (geom::Rect r, size_t per_pixel, size_t* out);

    void encode_pixel (color::PixelFormat format, const float* px, uint8_t* out);
    void decode_pixel (color::PixelFormat format, const uint8_t* bytes, float* out);
    size_t convert_pixel (
        color::PixelFormat from, color::PixelFormat to, const float* px, const color::CmykSpace* cmyk, float* out);
    void to_rgba (color::PixelFormat format, const float* px, const color::CmykSpace* cmyk, float out[4]);
    size_t from_rgba (color::PixelFormat format, const float rgba[4], const color::CmykSpace* cmyk, float* out);

    const size_t MAX_CHANNELS = 8;

} // namespace raster
