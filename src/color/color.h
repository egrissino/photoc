#pragma once

#include <stddef.h>
#include <stdint.h>

namespace color
{

    struct CmykSpace;

    enum class SampleType : uint8_t
    {
        U8,
        U16,
        F32,
    };

    enum class ColorMode : uint8_t
    {
        Bitmap,
        Grayscale,
        Indexed,
        Rgb,
        Cmyk,
        Lab,
        Multichannel,
        Duotone,
    };

    size_t sample_bytes (SampleType s);
    uint32_t sample_bits (SampleType s);

    size_t color_channels (ColorMode m);

    struct PixelFormat
    {
        ColorMode mode;
        SampleType sample;
        bool alpha;

        static PixelFormat rgba8 ();
        static PixelFormat rgba16 ();
        static PixelFormat rgba32f ();
        static PixelFormat gray8 ();
        static PixelFormat graya8 ();
        static PixelFormat cmyka8 ();

        size_t channels () const;
        size_t bytes_per_pixel () const;
        PixelFormat with_sample (SampleType s) const;

        bool operator== (const PixelFormat& o) const;
        bool operator!= (const PixelFormat& o) const;
    };

    float dither_noise (int32_t x, int32_t y);

    float read_sample (const uint8_t* bytes, SampleType sample, size_t index);

    void write_sample (uint8_t* bytes, SampleType sample, size_t index, float v);

    struct Color
    {
        ColorMode mode;
        float c[4];
        float alpha;

        static Color rgb (float r, float g, float b);
        static Color rgba (float r, float g, float b, float a);
        static Color gray (float v);
        static Color black ();
        static Color white ();
        static Color transparent ();

        void to_rgb (const CmykSpace* cmyk, float out[3]) const;
        Color in_mode (ColorMode mode, const CmykSpace* cmyk) const;
        void to_rgba8 (const CmykSpace* cmyk, uint8_t out[4]) const;
    };

} // namespace color
