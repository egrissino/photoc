#include "codecs/image.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace codecs
{

    /* The codecs' own flat image: samples interleaved (RGBARGBA...), row-major, top to bottom, in
       native byte order. Integer samples span [0, max]; float samples are nominally [0, 1] but may
       exceed it (HDR). Alpha, when present, is the last channel and is straight. CMYK holds ink
       amounts (0 = no ink). */

    void set_error (char* err, size_t err_len, const char* fmt, ...)
    {
        if (err == NULL || err_len == 0)
        {
            return;
        }
        va_list ap;
        va_start (ap, fmt);
        vsnprintf (err, err_len, fmt, ap);
        va_end (ap);
    }

    size_t layout_channels (ChannelLayout l)
    {
        switch (l)
        {
        case ChannelLayout::Gray:
            return 1;
        case ChannelLayout::GrayA:
            return 2;
        case ChannelLayout::Rgb:
            return 3;
        case ChannelLayout::Rgba:
            return 4;
        case ChannelLayout::Cmyk:
            return 4;
        case ChannelLayout::CmykA:
            return 5;
        }
        return 1;
    }

    bool layout_has_alpha (ChannelLayout l)
    {
        return l == ChannelLayout::GrayA || l == ChannelLayout::Rgba || l == ChannelLayout::CmykA;
    }

    size_t layout_color_channels (ChannelLayout l)
    {
        return layout_channels (l) - (layout_has_alpha (l) ? 1 : 0);
    }

    bool layout_is_gray (ChannelLayout l)
    {
        return l == ChannelLayout::Gray || l == ChannelLayout::GrayA;
    }

    bool layout_is_rgb (ChannelLayout l)
    {
        return l == ChannelLayout::Rgb || l == ChannelLayout::Rgba;
    }

    bool layout_is_cmyk (ChannelLayout l)
    {
        return l == ChannelLayout::Cmyk || l == ChannelLayout::CmykA;
    }

    ChannelLayout layout_with_alpha (ChannelLayout l)
    {
        if (layout_is_gray (l))
        {
            return ChannelLayout::GrayA;
        }
        if (layout_is_rgb (l))
        {
            return ChannelLayout::Rgba;
        }
        return ChannelLayout::CmykA;
    }

    ChannelLayout layout_without_alpha (ChannelLayout l)
    {
        if (layout_is_gray (l))
        {
            return ChannelLayout::Gray;
        }
        if (layout_is_rgb (l))
        {
            return ChannelLayout::Rgb;
        }
        return ChannelLayout::Cmyk;
    }

    const char* layout_name (ChannelLayout l)
    {
        switch (l)
        {
        case ChannelLayout::Gray:
            return "Gray";
        case ChannelLayout::GrayA:
            return "GrayA";
        case ChannelLayout::Rgb:
            return "Rgb";
        case ChannelLayout::Rgba:
            return "Rgba";
        case ChannelLayout::Cmyk:
            return "Cmyk";
        case ChannelLayout::CmykA:
            return "CmykA";
        }
        return "Gray";
    }

    size_t sample_size (Sample s)
    {
        switch (s)
        {
        case Sample::U8:
            return 1;
        case Sample::U16:
            return 2;
        case Sample::F16:
            return 2;
        case Sample::F32:
            return 4;
        }
        return 1;
    }

    const char* sample_name (Sample s)
    {
        switch (s)
        {
        case Sample::U8:
            return "U8";
        case Sample::U16:
            return "U16";
        case Sample::F16:
            return "F16";
        case Sample::F32:
            return "F32";
        }
        return "U8";
    }

    size_t Image::channels () const
    {
        return layout_channels (layout);
    }

    size_t Image::row_bytes () const
    {
        return (size_t) width * channels () * sample_size (sample);
    }

    float half_to_float (uint16_t h)
    {
        uint32_t sign = (uint32_t) (h >> 15) << 31;
        uint32_t exp  = (h >> 10) & 0x1f;
        uint32_t mant = h & 0x3ff;
        uint32_t bits;
        if (exp == 0)
        {
            if (mant == 0)
            {
                bits = sign;
            }
            else
            {
                exp = 127 - 15 + 1;
                while ((mant & 0x400) == 0)
                {
                    mant <<= 1;
                    exp--;
                }
                mant &= 0x3ff;
                bits = sign | (exp << 23) | (mant << 13);
            }
        }
        else if (exp == 31)
        {
            bits = sign | 0x7f800000u | (mant << 13);
        }
        else
        {
            bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
        }
        float f;
        memcpy (&f, &bits, 4);
        return f;
    }

    /* Sample `index` as a normalised float. */
    float Image::sample_at (size_t index) const
    {
        switch (sample)
        {
        case Sample::U8:
            return (float) data[index] / 255.0f;
        case Sample::U16:
            {
                uint16_t v;
                memcpy (&v, &data[index * 2], 2);
                return (float) v / 65535.0f;
            }
        case Sample::F16:
            {
                uint16_t v;
                memcpy (&v, &data[index * 2], 2);
                return half_to_float (v);
            }
        case Sample::F32:
            {
                float v;
                memcpy (&v, &data[index * 4], 4);
                return v;
            }
        }
        return 0.0f;
    }

    bool image_byte_len (uint32_t width, uint32_t height, ChannelLayout layout, Sample sample, size_t* out)
    {
        size_t w = width, h = height, per = layout_channels (layout) * sample_size (sample);
        if (h != 0 && w > SIZE_MAX / h)
        {
            return false;
        }
        size_t px = w * h;
        if (per != 0 && px > SIZE_MAX / per)
        {
            return false;
        }
        *out = px * per;
        return true;
    }

    /* Takes ownership of `data` (swapped in) after checking its length matches the header. */
    bool image_from_raw (uint32_t width, uint32_t height, ChannelLayout layout, Sample sample,
        std::vector<uint8_t>& data, Image* out, char* err, size_t err_len)
    {
        size_t len;
        if (!image_byte_len (width, height, layout, sample, &len))
        {
            set_error (err, err_len, "%ux%u image does not fit in memory", width, height);
            return false;
        }
        if (data.size () != len)
        {
            set_error (err, err_len, "image data is %zu bytes, expected %zu", data.size (), len);
            return false;
        }
        Image img;
        img.width   = width;
        img.height  = height;
        img.layout  = layout;
        img.sample  = sample;
        img.has_dpi = false;
        img.dpi_x = img.dpi_y = 0.0f;
        img.data.swap (data);
        *out = img;
        return true;
    }

    void write_norm (uint8_t* dst, Sample sample, size_t index, float v)
    {
        switch (sample)
        {
        case Sample::U8:
            {
                uint8_t q = 0;
                if (v >= 1.0f)
                {
                    q = 255;
                }
                else if (v > 0.0f)
                {
                    q = (uint8_t) (v * 255.0f + 0.5f);
                }
                dst[index] = q;
                break;
            }
        case Sample::U16:
        case Sample::F16:
            {
                uint16_t q = 0;
                if (v >= 1.0f)
                {
                    q = 65535;
                }
                else if (v > 0.0f)
                {
                    q = (uint16_t) (v * 65535.0f + 0.5f);
                }
                memcpy (&dst[index * 2], &q, 2);
                break;
            }
        case Sample::F32:
            memcpy (&dst[index * 4], &v, 4);
            break;
        }
    }

    /* Converts layout and sample type through normalised floats: gray <-> RGB by Rec.601 luma,
       CMYK <-> RGB by the naive formulas (colour-managed conversion belongs to the caller), alpha
       added as opaque or dropped. F16 output is not supported (written as U16). */
    bool image_convert (const Image& src, ChannelLayout layout, Sample sample, Image* out, char* err, size_t err_len)
    {
        if (sample == Sample::F16)
        {
            sample = Sample::U16;
        }
        size_t len;
        if (!image_byte_len (src.width, src.height, layout, sample, &len))
        {
            set_error (err, err_len, "%ux%u image does not fit in memory", src.width, src.height);
            return false;
        }
        Image img;
        img.width   = src.width;
        img.height  = src.height;
        img.layout  = layout;
        img.sample  = sample;
        img.icc     = src.icc;
        img.has_dpi = src.has_dpi;
        img.dpi_x   = src.dpi_x;
        img.dpi_y   = src.dpi_y;
        img.data.assign (len, 0);

        size_t in_n   = src.channels ();
        size_t out_n  = layout_channels (layout);
        size_t pixels = (size_t) src.width * (size_t) src.height;
        for (size_t p = 0; p < pixels; p++)
        {
            float v[5];
            for (size_t c = 0; c < in_n; c++)
            {
                v[c] = src.sample_at (p * in_n + c);
            }
            float rgb[3];
            float alpha = layout_has_alpha (src.layout) ? v[in_n - 1] : 1.0f;
            if (layout_is_gray (src.layout))
            {
                rgb[0] = rgb[1] = rgb[2] = v[0];
            }
            else if (layout_is_rgb (src.layout))
            {
                rgb[0] = v[0];
                rgb[1] = v[1];
                rgb[2] = v[2];
            }
            else
            {
                float k = 1.0f - v[3];
                for (int c = 0; c < 3; c++)
                {
                    rgb[c] = (1.0f - v[c]) * k;
                }
            }
            float o[5];
            if (layout_is_gray (layout))
            {
                o[0] = layout_is_gray (src.layout) ? v[0] : 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
            }
            else if (layout_is_rgb (layout))
            {
                o[0] = rgb[0];
                o[1] = rgb[1];
                o[2] = rgb[2];
            }
            else if (layout_is_cmyk (src.layout))
            {
                for (int c = 0; c < 4; c++)
                {
                    o[c] = v[c];
                }
            }
            else
            {
                float mx = fmaxf (fmaxf (rgb[0], rgb[1]), rgb[2]);
                float k  = 1.0f - mx;
                if (k >= 1.0f - 1e-6f)
                {
                    o[0] = o[1] = o[2] = 0.0f;
                    o[3]               = 1.0f;
                }
                else
                {
                    for (int c = 0; c < 3; c++)
                    {
                        o[c] = (1.0f - rgb[c] - k) / (1.0f - k);
                    }
                    o[3] = k;
                }
            }
            if (layout_has_alpha (layout))
            {
                o[out_n - 1] = alpha;
            }
            for (size_t c = 0; c < out_n; c++)
            {
                write_norm (img.data.data (), sample, p * out_n + c, o[c]);
            }
        }
        *out = img;
        return true;
    }

    Limits Limits::defaults ()
    {
        Limits l;
        l.max_width  = 1u << 18;
        l.max_height = 1u << 18;
        l.max_pixels = 1ull << 30;
        l.max_alloc  = 8ull << 30;
        return l;
    }

    bool Limits::check (
        uint32_t width, uint32_t height, ChannelLayout layout, Sample sample, char* err, size_t err_len) const
    {
        if (width == 0 || height == 0)
        {
            set_error (err, err_len, "image has no pixels (%ux%u)", width, height);
            return false;
        }
        if (width > max_width || height > max_height || (uint64_t) width * height > max_pixels)
        {
            set_error (err, err_len, "%ux%u is larger than the %ux%u (%llu pixel) limit", width, height, max_width,
                max_height, (unsigned long long) max_pixels);
            return false;
        }
        uint64_t bytes = (uint64_t) width * height * layout_channels (layout) * sample_size (sample);
        if (bytes > max_alloc)
        {
            set_error (err, err_len, "%ux%u image needs %llu bytes, over the %llu byte limit", width, height,
                (unsigned long long) bytes, (unsigned long long) max_alloc);
            return false;
        }
        return true;
    }

} // namespace codecs
