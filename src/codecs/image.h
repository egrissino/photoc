#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace codecs
{

    enum class ChannelLayout : uint8_t
    {
        Gray,
        GrayA,
        Rgb,
        Rgba,
        Cmyk,
        CmykA,
    };

    enum class Sample : uint8_t
    {
        U8,
        U16,
        F16,
        F32,
    };

    size_t layout_channels (ChannelLayout l);
    size_t layout_color_channels (ChannelLayout l);
    bool layout_has_alpha (ChannelLayout l);
    bool layout_is_gray (ChannelLayout l);
    bool layout_is_rgb (ChannelLayout l);
    bool layout_is_cmyk (ChannelLayout l);
    ChannelLayout layout_with_alpha (ChannelLayout l);
    ChannelLayout layout_without_alpha (ChannelLayout l);
    const char* layout_name (ChannelLayout l);

    size_t sample_size (Sample s);
    const char* sample_name (Sample s);

    struct Image
    {
        uint32_t width;
        uint32_t height;
        ChannelLayout layout;
        Sample sample;
        std::vector<uint8_t> data;
        std::vector<uint8_t> icc;
        bool has_dpi;
        float dpi_x;
        float dpi_y;

        size_t channels () const;
        size_t row_bytes () const;
        float sample_at (size_t index) const;
    };

    bool image_from_raw (uint32_t width, uint32_t height, ChannelLayout layout, Sample sample,
        std::vector<uint8_t>& data, Image* out, char* err, size_t err_len);
    bool image_convert (const Image& src, ChannelLayout layout, Sample sample, Image* out, char* err, size_t err_len);

    struct Limits
    {
        uint32_t max_width;
        uint32_t max_height;
        uint64_t max_pixels;
        uint64_t max_alloc;

        static Limits defaults ();
        bool check (
            uint32_t width, uint32_t height, ChannelLayout layout, Sample sample, char* err, size_t err_len) const;
    };

    void set_error (char* err, size_t err_len, const char* fmt, ...) __attribute__ ((format (printf, 3, 4)));

} // namespace codecs
