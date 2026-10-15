#include "codecs/codecs.h"

#include <ctype.h>
#include <string.h>
#include <string>

namespace codecs
{

    const Format ALL_FORMATS[FORMAT_COUNT] = {
        Format::Png,
        Format::Jpeg,
        Format::Tiff,
        Format::WebP,
        Format::Gif,
        Format::Bmp,
        Format::Tga,
        Format::Ico,
        Format::Pnm,
        Format::Pcx,
        Format::Qoi,
        Format::OpenExr,
        Format::Hdr,
        Format::Avif,
        Format::Heif,
        Format::Fits,
    };

    const char* format_name (Format f)
    {
        switch (f)
        {
        case Format::Png:
            return "PNG";
        case Format::Jpeg:
            return "JPEG";
        case Format::Tiff:
            return "TIFF";
        case Format::WebP:
            return "WebP";
        case Format::Gif:
            return "GIF";
        case Format::Bmp:
            return "BMP";
        case Format::Tga:
            return "TGA";
        case Format::Ico:
            return "ICO";
        case Format::Pnm:
            return "Netpbm";
        case Format::Pcx:
            return "PCX";
        case Format::Qoi:
            return "QOI";
        case Format::OpenExr:
            return "OpenEXR";
        case Format::Hdr:
            return "Radiance HDR";
        case Format::Avif:
            return "AVIF";
        case Format::Heif:
            return "HEIF";
        case Format::Fits:
            return "FITS";
        }
        return "unknown";
    }

    const char* const PNG_EXT[]  = {"png", "apng", NULL};
    const char* const JPEG_EXT[] = {"jpg", "jpeg", "jpe", "jfif", NULL};
    const char* const TIFF_EXT[] = {"tif", "tiff", NULL};
    const char* const WEBP_EXT[] = {"webp", NULL};
    const char* const GIF_EXT[]  = {"gif", NULL};
    const char* const BMP_EXT[]  = {"bmp", "dib", NULL};
    const char* const TGA_EXT[]  = {"tga", "icb", "vda", "vst", NULL};
    const char* const ICO_EXT[]  = {"ico", NULL};
    const char* const PNM_EXT[]  = {"pnm", "pbm", "pgm", "ppm", "pam", "pfm", NULL};
    const char* const PCX_EXT[]  = {"pcx", NULL};
    const char* const QOI_EXT[]  = {"qoi", NULL};
    const char* const EXR_EXT[]  = {"exr", NULL};
    const char* const HDR_EXT[]  = {"hdr", NULL};
    const char* const AVIF_EXT[] = {"avif", NULL};
    const char* const HEIF_EXT[] = {"heic", "heif", "hif", NULL};
    const char* const FITS_EXT[] = {"fits", "fit", "fts", NULL};

    /* NULL-terminated, lower case, without the dot. */
    const char* const* format_extensions (Format f)
    {
        switch (f)
        {
        case Format::Png:
            return PNG_EXT;
        case Format::Jpeg:
            return JPEG_EXT;
        case Format::Tiff:
            return TIFF_EXT;
        case Format::WebP:
            return WEBP_EXT;
        case Format::Gif:
            return GIF_EXT;
        case Format::Bmp:
            return BMP_EXT;
        case Format::Tga:
            return TGA_EXT;
        case Format::Ico:
            return ICO_EXT;
        case Format::Pnm:
            return PNM_EXT;
        case Format::Pcx:
            return PCX_EXT;
        case Format::Qoi:
            return QOI_EXT;
        case Format::OpenExr:
            return EXR_EXT;
        case Format::Hdr:
            return HDR_EXT;
        case Format::Avif:
            return AVIF_EXT;
        case Format::Heif:
            return HEIF_EXT;
        case Format::Fits:
            return FITS_EXT;
        }
        return PNM_EXT;
    }

    /* Formats whose codec has been ported (OpenEXR, AVIF, HEIF and FITS not yet). */
    bool format_can_decode (Format f)
    {
        return f != Format::OpenExr && f != Format::Avif && f != Format::Heif && f != Format::Fits;
    }

    bool format_can_encode (Format f)
    {
        return format_can_decode (f);
    }

    EncodeOptions EncodeOptions::defaults ()
    {
        EncodeOptions o;
        o.jpeg_quality            = 90;
        o.jpeg_chroma_subsampling = true;
        o.png_compression         = 6;
        o.embed_icc               = true;
        o.embed_metadata          = true;
        o.webp_quality            = 85;
        o.webp_lossless           = false;
        return o;
    }

    /* The layout and sample type `format` stores an image in: PNG keeps gray/RGB with alpha at 8 or
       16 bits (CMYK becomes RGB, float becomes 16-bit); JPEG keeps gray, RGB or CMYK at 8 bits
       without alpha; Netpbm stores everything except float with alpha or CMYK (16-bit then). */
    void encode_plan (Format format, ChannelLayout layout, Sample sample, ChannelLayout* out_layout, Sample* out_sample)
    {
        *out_layout = layout;
        *out_sample = sample;
        switch (format)
        {
        case Format::Png:
            if (layout_is_cmyk (layout))
            {
                *out_layout = layout_has_alpha (layout) ? ChannelLayout::Rgba : ChannelLayout::Rgb;
            }
            if (sample != Sample::U8)
            {
                *out_sample = Sample::U16;
            }
            break;
        case Format::Jpeg:
            *out_layout = layout_without_alpha (layout);
            *out_sample = Sample::U8;
            break;
        case Format::Tiff:
            if (sample == Sample::F16)
            {
                *out_sample = Sample::F32;
            }
            break;
        case Format::WebP:
        case Format::Qoi:
            *out_layout = layout_has_alpha (layout) ? ChannelLayout::Rgba : ChannelLayout::Rgb;
            *out_sample = Sample::U8;
            break;
        case Format::Gif:
        case Format::Ico:
            *out_layout = ChannelLayout::Rgba;
            *out_sample = Sample::U8;
            break;
        case Format::Bmp:
            *out_layout = layout_has_alpha (layout) ? ChannelLayout::Rgba : ChannelLayout::Rgb;
            *out_sample = Sample::U8;
            break;
        case Format::Tga:
            if (layout_is_cmyk (layout))
            {
                *out_layout = layout_has_alpha (layout) ? ChannelLayout::Rgba : ChannelLayout::Rgb;
            }
            *out_sample = Sample::U8;
            break;
        case Format::Pcx:
            *out_layout = ChannelLayout::Rgb;
            *out_sample = Sample::U8;
            break;
        case Format::Hdr:
            *out_layout = ChannelLayout::Rgb;
            *out_sample = Sample::F32;
            break;
        case Format::Pnm:
            {
                bool pfm = layout == ChannelLayout::Gray || layout == ChannelLayout::Rgb;
                if (sample == Sample::F16)
                {
                    *out_sample = pfm ? Sample::F32 : Sample::U16;
                }
                if (sample == Sample::F32 && !pfm)
                {
                    *out_sample = Sample::U16;
                }
                break;
            }
        default:
            break;
        }
    }

    bool starts_with (const uint8_t* b, size_t len, const char* magic, size_t n)
    {
        return len >= n && memcmp (b, magic, n) == 0;
    }

    bool looks_like_tga (const uint8_t* b, size_t len)
    {
        if (len < 18)
        {
            return false;
        }
        if (len >= 26 && memcmp (b + len - 18, "TRUEVISION-XFILE.\0", 18) == 0)
        {
            return true;
        }
        uint8_t cmap_type = b[1], img_type = b[2], cmap_depth = b[7], depth = b[16], desc = b[17];
        uint16_t cmap_len = (uint16_t) (b[5] | (b[6] << 8));
        uint16_t w        = (uint16_t) (b[12] | (b[13] << 8));
        uint16_t h        = (uint16_t) (b[14] | (b[15] << 8));
        bool indexed      = img_type == 1 || img_type == 9;
        bool type_ok =
            img_type == 1 || img_type == 2 || img_type == 3 || img_type == 9 || img_type == 10 || img_type == 11;
        bool cmap_ok;
        if (cmap_type == 0)
        {
            cmap_ok = cmap_len == 0 && cmap_depth == 0 && !indexed;
        }
        else if (cmap_type == 1)
        {
            cmap_ok = indexed && cmap_len > 0
                   && (cmap_depth == 15 || cmap_depth == 16 || cmap_depth == 24 || cmap_depth == 32);
        }
        else
        {
            cmap_ok = false;
        }
        bool depth_ok;
        if (indexed)
        {
            depth_ok = depth == 8;
        }
        else if (img_type == 3 || img_type == 11)
        {
            depth_ok = depth == 8 || depth == 16;
        }
        else
        {
            depth_ok = depth == 15 || depth == 16 || depth == 24 || depth == 32;
        }
        return type_ok && cmap_ok && depth_ok && w > 0 && h > 0 && (desc & 0xC0) == 0;
    }

    bool ftyp_has_brand (const uint8_t* b, size_t len, const char* const* brands)
    {
        if (len < 12 || memcmp (b + 4, "ftyp", 4) != 0)
        {
            return false;
        }
        size_t size = ((size_t) b[0] << 24) | ((size_t) b[1] << 16) | ((size_t) b[2] << 8) | b[3];
        size_t end  = len < 64 ? len : 64;
        if (size < 12)
        {
            size = 12;
        }
        if (size < end)
        {
            end = size;
        }
        for (size_t off = 8, i = 0; off + 4 <= end; off += 4, i++)
        {
            if (i == 1)
            {
                continue;
            }
            for (size_t k = 0; brands[k] != NULL; k++)
            {
                if (memcmp (b + off, brands[k], 4) == 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    const char* const AVIF_BRANDS[] = {"avif", "avis", NULL};
    const char* const HEIF_BRANDS[] = {"heic", "heix", "heim", "heis", "hevc", "hevx", "mif1", "msf1", "miaf", NULL};

    /* The format from the file's magic bytes. */
    bool detect (const uint8_t* b, size_t len, Format* out)
    {
        Format f;
        if (starts_with (b, len, "\x89PNG\r\n\x1a\n", 8))
        {
            f = Format::Png;
        }
        else if (starts_with (b, len, "\xFF\xD8\xFF", 3))
        {
            f = Format::Jpeg;
        }
        else if (starts_with (b, len, "II*\0", 4) || starts_with (b, len, "MM\0*", 4)
                 || starts_with (b, len, "II+\0", 4) || starts_with (b, len, "MM\0+", 4))
        {
            f = Format::Tiff;
        }
        else if (len >= 12 && memcmp (b, "RIFF", 4) == 0 && memcmp (b + 8, "WEBP", 4) == 0)
        {
            f = Format::WebP;
        }
        else if (starts_with (b, len, "GIF87a", 6) || starts_with (b, len, "GIF89a", 6))
        {
            f = Format::Gif;
        }
        else if (starts_with (b, len, "qoif", 4))
        {
            f = Format::Qoi;
        }
        else if (starts_with (b, len, "\x76\x2F\x31\x01", 4))
        {
            f = Format::OpenExr;
        }
        else if (starts_with (b, len, "#?RADIANCE", 10) || starts_with (b, len, "#?RGBE", 6))
        {
            f = Format::Hdr;
        }
        else if (ftyp_has_brand (b, len, AVIF_BRANDS))
        {
            f = Format::Avif;
        }
        else if (ftyp_has_brand (b, len, HEIF_BRANDS))
        {
            f = Format::Heif;
        }
        else if (len >= 14 && b[0] == 'B' && b[1] == 'M')
        {
            f = Format::Bmp;
        }
        else if (len >= 6 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0 && (b[4] != 0 || b[5] != 0))
        {
            f = Format::Ico;
        }
        else if (len >= 3 && b[0] == 'P' && ((b[1] >= '1' && b[1] <= '7') || b[1] == 'F' || b[1] == 'f')
                 && isspace (b[2]))
        {
            f = Format::Pnm;
        }
        else if (starts_with (b, len, "SIMPLE  =", 9))
        {
            f = Format::Fits;
        }
        else if (looks_like_tga (b, len))
        {
            f = Format::Tga;
        }
        else if (len >= 128 && b[0] == 0x0A && b[1] <= 5 && b[2] == 1 && (b[3] == 1 || b[3] == 4 || b[3] == 8))
        {
            f = Format::Pcx;
        }
        else
        {
            return false;
        }
        *out = f;
        return true;
    }

    /* By extension: case-insensitive, with or without the dot, or a whole file name or path. */
    bool from_extension (const char* name_or_ext, Format* out)
    {
        const char* start = name_or_ext;
        for (const char* p = name_or_ext; *p; p++)
        {
            if (*p == '.' || *p == '/' || *p == '\\')
            {
                start = p + 1;
            }
        }
        std::string ext;
        for (const char* p = start; *p; p++)
        {
            ext += (char) tolower ((unsigned char) *p);
        }
        for (size_t i = 0; i < FORMAT_COUNT; i++)
        {
            const char* const* exts = format_extensions (ALL_FORMATS[i]);
            for (size_t k = 0; exts[k] != NULL; k++)
            {
                if (ext == exts[k])
                {
                    *out = ALL_FORMATS[i];
                    return true;
                }
            }
        }
        return false;
    }

    bool decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        Format f;
        if (!detect (bytes, len, &f))
        {
            set_error (err, err_len, "unrecognized image format");
            return false;
        }
        switch (f)
        {
        case Format::Pnm:
            return pnm_decode (bytes, len, limits, out, err, err_len);
        case Format::Png:
            return png_decode (bytes, len, limits, out, err, err_len);
        case Format::Jpeg:
            return jpeg_decode (bytes, len, limits, out, err, err_len);
        case Format::Tiff:
            return tiff_decode (bytes, len, limits, out, err, err_len);
        case Format::WebP:
            return webp_decode (bytes, len, limits, out, err, err_len);
        case Format::Gif:
            return gif_decode (bytes, len, limits, out, err, err_len);
        case Format::Bmp:
            return bmp_decode (bytes, len, limits, out, err, err_len);
        case Format::Ico:
            return ico_decode (bytes, len, limits, out, err, err_len);
        case Format::Tga:
            return tga_decode (bytes, len, limits, out, err, err_len);
        case Format::Qoi:
            return qoi_decode (bytes, len, limits, out, err, err_len);
        case Format::Hdr:
            return hdr_decode (bytes, len, limits, out, err, err_len);
        case Format::Pcx:
            return pcx_decode (bytes, len, limits, out, err, err_len);
        default:
            set_error (err, err_len, "reading %s files is not ported yet", format_name (f));
            return false;
        }
    }

    bool encode (const Image& img, Format format, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err,
        size_t err_len)
    {
        if (!format_can_encode (format))
        {
            set_error (err, err_len, "writing %s files is not ported yet", format_name (format));
            return false;
        }
        ChannelLayout layout;
        Sample sample;
        encode_plan (format, img.layout, img.sample, &layout, &sample);
        Image converted;
        const Image* src = &img;
        if (layout != img.layout || sample != img.sample)
        {
            if (!image_convert (img, layout, sample, &converted, err, err_len))
            {
                return false;
            }
            src = &converted;
        }
        switch (format)
        {
        case Format::Png:
            return png_encode (*src, opts, out, err, err_len);
        case Format::Jpeg:
            return jpeg_encode (*src, opts, out, err, err_len);
        case Format::Tiff:
            return tiff_encode (*src, opts, out, err, err_len);
        case Format::WebP:
            return webp_encode (*src, opts, out, err, err_len);
        case Format::Gif:
            return gif_encode (*src, opts, out, err, err_len);
        case Format::Bmp:
            return bmp_encode (*src, out, err, err_len);
        case Format::Ico:
            return ico_encode (*src, opts, out, err, err_len);
        case Format::Tga:
            return tga_encode (*src, out, err, err_len);
        case Format::Qoi:
            return qoi_encode (*src, out, err, err_len);
        case Format::Hdr:
            return hdr_encode (*src, out, err, err_len);
        case Format::Pcx:
            return pcx_encode (*src, out, err, err_len);
        default:
            return pnm_encode (*src, out, err, err_len);
        }
    }

} // namespace codecs
