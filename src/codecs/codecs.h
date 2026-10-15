#pragma once

#include "codecs/image.h"
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace codecs
{

    enum class Format : uint8_t
    {
        Png,
        Jpeg,
        Tiff,
        WebP,
        Gif,
        Bmp,
        Tga,
        Ico,
        Pnm,
        Pcx,
        Qoi,
        OpenExr,
        Hdr,
        Avif,
        Heif,
        Fits,
    };

    const size_t FORMAT_COUNT = 16;
    extern const Format ALL_FORMATS[FORMAT_COUNT];

    const char* format_name (Format f);
    const char* const* format_extensions (Format f);
    bool format_can_decode (Format f);
    bool format_can_encode (Format f);
    bool detect (const uint8_t* bytes, size_t len, Format* out);
    bool from_extension (const char* name_or_ext, Format* out);

    struct EncodeOptions
    {
        int jpeg_quality;
        bool jpeg_chroma_subsampling;
        int png_compression;
        bool embed_icc;
        bool embed_metadata;
        int webp_quality;
        bool webp_lossless;

        static EncodeOptions defaults ();
    };

    void encode_plan (
        Format format, ChannelLayout layout, Sample sample, ChannelLayout* out_layout, Sample* out_sample);

    bool decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool encode (const Image& img, Format format, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err,
        size_t err_len);

    bool pnm_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool pnm_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool png_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool png_encode (const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool jpeg_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool jpeg_encode (
        const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool tiff_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool tiff_encode (
        const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool webp_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool webp_encode (
        const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool gif_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool gif_encode (const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool bmp_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool bmp_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool ico_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool ico_encode (const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool tga_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool tga_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool qoi_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool qoi_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool hdr_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool hdr_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool pcx_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len);
    bool pcx_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len);

    uint16_t rd_le16 (const uint8_t* p);
    uint32_t rd_le32 (const uint8_t* p);
    void wr_le16 (std::vector<uint8_t>* w, uint16_t v);
    void wr_le32 (std::vector<uint8_t>* w, uint32_t v);

} // namespace codecs
