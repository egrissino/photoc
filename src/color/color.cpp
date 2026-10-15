#include "color/color.h"

#include "color/convert.h"
#include <string.h>

namespace color
{

    size_t sample_bytes (SampleType s)
    {
        switch (s)
        {
        case SampleType::U8:
            return 1;
        case SampleType::U16:
            return 2;
        case SampleType::F32:
            return 4;
        }
        return 1;
    }

    uint32_t sample_bits (SampleType s)
    {
        return (uint32_t) sample_bytes (s) * 8;
    }

    size_t color_channels (ColorMode m)
    {
        switch (m)
        {
        case ColorMode::Bitmap:
        case ColorMode::Grayscale:
        case ColorMode::Duotone:
            return 1;
        case ColorMode::Indexed:
        case ColorMode::Rgb:
        case ColorMode::Lab:
        case ColorMode::Multichannel:
            return 3;
        case ColorMode::Cmyk:
            return 4;
        }
        return 3;
    }

    PixelFormat PixelFormat::rgba8 ()
    {
        return PixelFormat{ColorMode::Rgb, SampleType::U8, true};
    }
    PixelFormat PixelFormat::rgba16 ()
    {
        return PixelFormat{ColorMode::Rgb, SampleType::U16, true};
    }
    PixelFormat PixelFormat::rgba32f ()
    {
        return PixelFormat{ColorMode::Rgb, SampleType::F32, true};
    }
    PixelFormat PixelFormat::gray8 ()
    {
        return PixelFormat{ColorMode::Grayscale, SampleType::U8, false};
    }
    PixelFormat PixelFormat::graya8 ()
    {
        return PixelFormat{ColorMode::Grayscale, SampleType::U8, true};
    }
    PixelFormat PixelFormat::cmyka8 ()
    {
        return PixelFormat{ColorMode::Cmyk, SampleType::U8, true};
    }

    size_t PixelFormat::channels () const
    {
        return color_channels (mode) + (alpha ? 1 : 0);
    }

    size_t PixelFormat::bytes_per_pixel () const
    {
        return channels () * sample_bytes (sample);
    }

    PixelFormat PixelFormat::with_sample (SampleType s) const
    {
        PixelFormat f = *this;
        f.sample      = s;
        return f;
    }

    bool PixelFormat::operator== (const PixelFormat& o) const
    {
        return mode == o.mode && sample == o.sample && alpha == o.alpha;
    }

    bool PixelFormat::operator!= (const PixelFormat& o) const
    {
        return !(*this == o);
    }

    float dither_noise (int32_t x, int32_t y)
    {
        uint32_t h = ((uint32_t) x * 0x9E3779B1u) ^ ((uint32_t) y * 0x85EBCA77u);
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        return (float) (uint16_t) (h & 0xFFFF) / 65535.0f;
    }

    float read_sample (const uint8_t* bytes, SampleType sample, size_t index)
    {
        switch (sample)
        {
        case SampleType::U8:
            return (float) bytes[index] / 255.0f;
        case SampleType::U16:
            {
                uint16_t v;
                memcpy (&v, bytes + index * 2, 2);
                return (float) v / 65535.0f;
            }
        case SampleType::F32:
            {
                float v;
                memcpy (&v, bytes + index * 4, 4);
                return v;
            }
        }
        return 0.0f;
    }

    void write_sample (uint8_t* bytes, SampleType sample, size_t index, float v)
    {
        switch (sample)
        {
        case SampleType::U8:
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
                bytes[index] = q;
                break;
            }
        case SampleType::U16:
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
                memcpy (bytes + index * 2, &q, 2);
                break;
            }
        case SampleType::F32:
            memcpy (bytes + index * 4, &v, 4);
            break;
        }
    }

    Color Color::rgb (float r, float g, float b)
    {
        return Color{ColorMode::Rgb, {r, g, b, 0.0f}, 1.0f};
    }
    Color Color::rgba (float r, float g, float b, float a)
    {
        return Color{ColorMode::Rgb, {r, g, b, 0.0f}, a};
    }
    Color Color::gray (float v)
    {
        return Color{ColorMode::Grayscale, {v, 0.0f, 0.0f, 0.0f}, 1.0f};
    }
    Color Color::black ()
    {
        return rgb (0.0f, 0.0f, 0.0f);
    }
    Color Color::white ()
    {
        return rgb (1.0f, 1.0f, 1.0f);
    }
    Color Color::transparent ()
    {
        return rgba (0.0f, 0.0f, 0.0f, 0.0f);
    }

    void Color::to_rgb (const CmykSpace* cmyk, float out[3]) const
    {
        switch (mode)
        {
        case ColorMode::Rgb:
        case ColorMode::Indexed:
        case ColorMode::Multichannel:
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            return;
        case ColorMode::Grayscale:
        case ColorMode::Bitmap:
        case ColorMode::Duotone:
            out[0] = out[1] = out[2] = c[0];
            return;
        case ColorMode::Cmyk:
            cmyk_to_rgb (cmyk, c, out);
            return;
        case ColorMode::Lab:
            {
                float lab[3] = {c[0] * 100.0f, c[1] * 255.0f - 128.0f, c[2] * 255.0f - 128.0f};
                lab_to_srgb (lab, out);
                return;
            }
        }
    }

    bool is_rgb_like (ColorMode m)
    {
        return m == ColorMode::Rgb || m == ColorMode::Indexed || m == ColorMode::Multichannel;
    }

    Color Color::in_mode (ColorMode target, const CmykSpace* cmyk) const
    {
        if (mode == target || (is_rgb_like (mode) && is_rgb_like (target)))
        {
            return *this;
        }
        float rgb[3];
        to_rgb (cmyk, rgb);
        Color out = {target, {0.0f, 0.0f, 0.0f, 0.0f}, alpha};
        switch (target)
        {
        case ColorMode::Rgb:
        case ColorMode::Indexed:
        case ColorMode::Multichannel:
            out.mode = ColorMode::Rgb;
            out.c[0] = rgb[0];
            out.c[1] = rgb[1];
            out.c[2] = rgb[2];
            break;
        case ColorMode::Grayscale:
        case ColorMode::Bitmap:
        case ColorMode::Duotone:
            out.c[0] = rgb_to_gray (rgb);
            break;
        case ColorMode::Cmyk:
            rgb_to_cmyk (cmyk, rgb, out.c);
            break;
        case ColorMode::Lab:
            {
                float lab[3];
                srgb_to_lab (rgb, lab);
                out.c[0] = lab[0] / 100.0f;
                out.c[1] = (lab[1] + 128.0f) / 255.0f;
                out.c[2] = (lab[2] + 128.0f) / 255.0f;
                break;
            }
        }
        return out;
    }

    void Color::to_rgba8 (const CmykSpace* cmyk, uint8_t out[4]) const
    {
        float rgb[3];
        to_rgb (cmyk, rgb);
        write_sample (out, SampleType::U8, 0, rgb[0]);
        write_sample (out, SampleType::U8, 1, rgb[1]);
        write_sample (out, SampleType::U8, 2, rgb[2]);
        write_sample (out, SampleType::U8, 3, alpha);
    }

} // namespace color
