#include "codecs/codecs.h"
#include <math.h>
#include <png.h>
#include <setjmp.h>
#include <string.h>

namespace codecs
{

    /* PNG through libpng: 1 to 16 bits, every colour type (palettes and tRNS expand to RGB(A)),
       Adam7 interlacing, iCCP and pHYs.

       libpng reports errors with longjmp. Every function that calls libpng after setjmp holds only
       plain C locals, so the jump never skips a destructor; buffers are allocated by the caller
       before such a call. */

    const float METERS_PER_INCH = 0.0254f;

    struct PngSource
    {
        const uint8_t* data;
        size_t len;
        size_t pos;
    };

    struct PngMessage
    {
        char* buf;
        size_t len;
    };

    void png_mem_read (png_structp png, png_bytep out, png_size_t n)
    {
        PngSource* src = (PngSource*) png_get_io_ptr (png);
        if (n > src->len - src->pos)
        {
            png_error (png, "the file ends before the image data does");
        }
        memcpy (out, src->data + src->pos, n);
        src->pos += n;
    }

    void png_mem_write (png_structp png, png_bytep data, png_size_t n)
    {
        std::vector<uint8_t>* out = (std::vector<uint8_t>*) png_get_io_ptr (png);
        out->insert (out->end (), data, data + n);
    }

    void png_mem_flush (png_structp png) {}

    void png_on_error (png_structp png, png_const_charp msg)
    {
        PngMessage* m = (PngMessage*) png_get_error_ptr (png);
        set_error (m->buf, m->len, "PNG: %s", msg);
        png_longjmp (png, 1);
    }

    void png_on_warning (png_structp png, png_const_charp msg) {}

    bool host_little_endian ()
    {
        uint16_t v = 1;
        uint8_t b;
        memcpy (&b, &v, 1);
        return b == 1;
    }

    struct PngHeader
    {
        uint32_t width;
        uint32_t height;
        int channels;
        int bit_depth;
        uint32_t stored_bits;
        png_bytep icc;
        png_uint_32 icc_len;
        bool has_phys;
        png_uint_32 xppu;
        png_uint_32 yppu;
    };

    /* Reads the header and sets up the expansions; the output format is final afterwards. */
    bool png_read_header (png_structp png, png_infop info, PngHeader* h)
    {
        if (setjmp (png_jmpbuf (png)))
        {
            return false;
        }
        png_read_info (png, info);
        h->stored_bits = (uint32_t) png_get_bit_depth (png, info) * png_get_channels (png, info);
        png_set_expand (png);
        if (png_get_bit_depth (png, info) == 16 && host_little_endian ())
        {
            png_set_swap (png);
        }
        png_set_interlace_handling (png);
        png_read_update_info (png, info);
        h->width     = png_get_image_width (png, info);
        h->height    = png_get_image_height (png, info);
        h->channels  = png_get_channels (png, info);
        h->bit_depth = png_get_bit_depth (png, info);
        png_charp name;
        int compression;
        h->icc     = NULL;
        h->icc_len = 0;
        png_get_iCCP (png, info, &name, &compression, &h->icc, &h->icc_len);
        int unit    = 0;
        h->has_phys = png_get_pHYs (png, info, &h->xppu, &h->yppu, &unit) != 0 && unit == PNG_RESOLUTION_METER
                   && h->xppu > 0 && h->yppu > 0;
        return true;
    }

    bool png_read_pixels (png_structp png, png_infop info, png_bytepp rows)
    {
        if (setjmp (png_jmpbuf (png)))
        {
            return false;
        }
        png_read_image (png, rows);
        png_read_end (png, NULL);
        return true;
    }

    bool png_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        PngMessage msg  = {err, err_len};
        PngSource src   = {bytes, len, 0};
        png_structp png = png_create_read_struct (PNG_LIBPNG_VER_STRING, &msg, png_on_error, png_on_warning);
        if (png == NULL)
        {
            set_error (err, err_len, "PNG: cannot create a decoder");
            return false;
        }
        png_infop info = png_create_info_struct (png);
        if (info == NULL)
        {
            png_destroy_read_struct (&png, NULL, NULL);
            set_error (err, err_len, "PNG: cannot create a decoder");
            return false;
        }
        png_set_read_fn (png, &src, png_mem_read);

        PngHeader h;
        bool ok              = png_read_header (png, info, &h);
        ChannelLayout layout = ChannelLayout::Gray;
        Sample sample        = h.bit_depth == 16 ? Sample::U16 : Sample::U8;
        if (ok)
        {
            switch (h.channels)
            {
            case 1:
                layout = ChannelLayout::Gray;
                break;
            case 2:
                layout = ChannelLayout::GrayA;
                break;
            case 3:
                layout = ChannelLayout::Rgb;
                break;
            case 4:
                layout = ChannelLayout::Rgba;
                break;
            default:
                set_error (err, err_len, "PNG: unexpected channel count %d", h.channels);
                ok = false;
            }
        }
        if (ok && h.bit_depth != 8 && h.bit_depth != 16)
        {
            set_error (err, err_len, "PNG: unexpected output bit depth %d", h.bit_depth);
            ok = false;
        }
        if (ok)
        {
            ok = limits.check (h.width, h.height, layout, sample, err, err_len);
        }
        if (ok)
        {
            /* zlib never beats 1032:1: a file too small for the declared image is refused before
               the output buffer exists. */
            uint64_t stored = ((uint64_t) h.width * h.height * h.stored_bits + 7) / 8;
            if (stored / 1100 > (uint64_t) len)
            {
                set_error (err, err_len, "PNG: the file is too small for a %ux%u image", h.width, h.height);
                ok = false;
            }
        }

        std::vector<uint8_t> data;
        std::vector<png_bytep> rows;
        if (ok)
        {
            size_t row_bytes = (size_t) h.width * (size_t) h.channels * (h.bit_depth / 8);
            data.assign (row_bytes * h.height, 0);
            rows.resize (h.height);
            for (size_t y = 0; y < h.height; y++)
            {
                rows[y] = data.data () + y * row_bytes;
            }
            ok = png_read_pixels (png, info, rows.data ());
        }
        std::vector<uint8_t> icc;
        if (ok && h.icc != NULL && h.icc_len > 0)
        {
            icc.assign (h.icc, h.icc + h.icc_len);
        }
        png_destroy_read_struct (&png, &info, NULL);
        if (!ok)
        {
            return false;
        }

        if (!image_from_raw (h.width, h.height, layout, sample, data, out, err, err_len))
        {
            return false;
        }
        out->icc = icc;
        if (h.has_phys)
        {
            out->has_dpi = true;
            out->dpi_x   = (float) h.xppu * METERS_PER_INCH;
            out->dpi_y   = (float) h.yppu * METERS_PER_INCH;
        }
        return true;
    }

    bool png_write_all (png_structp png, png_infop info, const Image* img, const EncodeOptions* opts, png_bytepp rows)
    {
        if (setjmp (png_jmpbuf (png)))
        {
            return false;
        }
        int color;
        switch (img->layout)
        {
        case ChannelLayout::Gray:
            color = PNG_COLOR_TYPE_GRAY;
            break;
        case ChannelLayout::GrayA:
            color = PNG_COLOR_TYPE_GRAY_ALPHA;
            break;
        case ChannelLayout::Rgb:
            color = PNG_COLOR_TYPE_RGB;
            break;
        default:
            color = PNG_COLOR_TYPE_RGB_ALPHA;
            break;
        }
        int depth = img->sample == Sample::U16 ? 16 : 8;
        png_set_IHDR (png, info, img->width, img->height, depth, color, PNG_INTERLACE_NONE,
            PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_set_compression_level (png, opts->png_compression);
        if (opts->embed_icc && !img->icc.empty ())
        {
            /* Profiles libpng knows as slightly wrong sRGB variants (Photoshop wrote many) are kept as
               they are, not refused. */
            png_set_option (png, PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_ON);
            png_set_benign_errors (png, 1);
            png_set_iCCP (
                png, info, "ICC Profile", PNG_COMPRESSION_TYPE_BASE, img->icc.data (), (png_uint_32) img->icc.size ());
        }
        if (opts->embed_metadata && img->has_dpi && img->dpi_x > 0.0f && img->dpi_y > 0.0f)
        {
            png_set_pHYs (png, info, (png_uint_32) lroundf (img->dpi_x / METERS_PER_INCH),
                (png_uint_32) lroundf (img->dpi_y / METERS_PER_INCH), PNG_RESOLUTION_METER);
        }
        png_write_info (png, info);
        if (depth == 16 && host_little_endian ())
        {
            png_set_swap (png);
        }
        png_write_image (png, rows);
        png_write_end (png, NULL);
        return true;
    }

    bool png_encode (const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        if (layout_is_cmyk (img.layout) || (img.sample != Sample::U8 && img.sample != Sample::U16))
        {
            set_error (err, err_len, "PNG cannot store %s %s", layout_name (img.layout), sample_name (img.sample));
            return false;
        }
        PngMessage msg  = {err, err_len};
        png_structp png = png_create_write_struct (PNG_LIBPNG_VER_STRING, &msg, png_on_error, png_on_warning);
        if (png == NULL)
        {
            set_error (err, err_len, "PNG: cannot create an encoder");
            return false;
        }
        png_infop info = png_create_info_struct (png);
        if (info == NULL)
        {
            png_destroy_write_struct (&png, NULL);
            set_error (err, err_len, "PNG: cannot create an encoder");
            return false;
        }
        out->clear ();
        png_set_write_fn (png, out, png_mem_write, png_mem_flush);
        std::vector<png_bytep> rows (img.height);
        size_t row_bytes = img.row_bytes ();
        for (size_t y = 0; y < img.height; y++)
        {
            rows[y] = (png_bytep) (img.data.data () + y * row_bytes);
        }
        bool ok = png_write_all (png, info, &img, &opts, rows.data ());
        png_destroy_write_struct (&png, &info);
        return ok;
    }

} // namespace codecs
