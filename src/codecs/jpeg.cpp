// clang-format off: jpeglib.h needs FILE from <stdio.h> declared first
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#include "codecs/codecs.h"
// clang-format on

namespace codecs
{

    /* JPEG through libjpeg-turbo: gray, YCbCr, CMYK and YCCK in; gray, RGB and CMYK out. ICC
       profiles span APP2 segments; resolution comes from the JFIF density. CMYK is stored
       Adobe-style, inverted (255 = no ink), and inverted back on read.

       Like libpng, libjpeg reports errors with longjmp: the functions that call it after setjmp
       hold only plain C locals, and buffers are allocated by their callers. */

    struct JpegError
    {
        struct jpeg_error_mgr mgr;
        jmp_buf jump;
        char* buf;
        size_t len;
    };

    void jpeg_on_error (j_common_ptr cinfo)
    {
        JpegError* e = (JpegError*) cinfo->err;
        char text[JMSG_LENGTH_MAX];
        (*cinfo->err->format_message) (cinfo, text);
        set_error (e->buf, e->len, "JPEG: %s", text);
        longjmp (e->jump, 1);
    }

    void jpeg_on_message (j_common_ptr cinfo, int level) {}

    struct JpegHeader
    {
        uint32_t width;
        uint32_t height;
        int components;
        bool cmyk_inverted;
        JOCTET* icc;
        unsigned int icc_len;
        int density_unit;
        uint16_t x_density;
        uint16_t y_density;
    };

    bool jpeg_read_head (j_decompress_ptr cinfo, JpegError* e, const uint8_t* bytes, size_t len, JpegHeader* h)
    {
        if (setjmp (e->jump))
        {
            return false;
        }
        jpeg_mem_src (cinfo, bytes, (unsigned long) len);
        jpeg_save_markers (cinfo, JPEG_APP0 + 2, 0xFFFF);
        jpeg_read_header (cinfo, TRUE);
        h->cmyk_inverted = false;
        switch (cinfo->jpeg_color_space)
        {
        case JCS_GRAYSCALE:
            cinfo->out_color_space = JCS_GRAYSCALE;
            break;
        case JCS_CMYK:
        case JCS_YCCK:
            cinfo->out_color_space = JCS_CMYK;
            h->cmyk_inverted       = true;
            break;
        default:
            cinfo->out_color_space = JCS_RGB;
            break;
        }
        h->icc     = NULL;
        h->icc_len = 0;
        jpeg_read_icc_profile (cinfo, &h->icc, &h->icc_len);
        h->density_unit = cinfo->density_unit;
        h->x_density    = cinfo->X_density;
        h->y_density    = cinfo->Y_density;
        jpeg_calc_output_dimensions (cinfo);
        h->width      = cinfo->output_width;
        h->height     = cinfo->output_height;
        h->components = cinfo->out_color_components;
        return true;
    }

    bool jpeg_read_pixels (j_decompress_ptr cinfo, JpegError* e, uint8_t* data, size_t row_bytes)
    {
        if (setjmp (e->jump))
        {
            return false;
        }
        jpeg_start_decompress (cinfo);
        while (cinfo->output_scanline < cinfo->output_height)
        {
            JSAMPROW row = data + (size_t) cinfo->output_scanline * row_bytes;
            jpeg_read_scanlines (cinfo, &row, 1);
        }
        jpeg_finish_decompress (cinfo);
        return true;
    }

    bool jpeg_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        struct jpeg_decompress_struct cinfo;
        JpegError e;
        cinfo.err          = jpeg_std_error (&e.mgr);
        e.mgr.error_exit   = jpeg_on_error;
        e.mgr.emit_message = jpeg_on_message;
        e.buf              = err;
        e.len              = err_len;
        jpeg_create_decompress (&cinfo);

        JpegHeader h;
        bool ok              = jpeg_read_head (&cinfo, &e, bytes, len, &h);
        ChannelLayout layout = ChannelLayout::Rgb;
        if (ok)
        {
            if (h.components == 1)
            {
                layout = ChannelLayout::Gray;
            }
            else if (h.components == 4)
            {
                layout = ChannelLayout::Cmyk;
            }
            else if (h.components != 3)
            {
                set_error (err, err_len, "JPEG: unexpected component count %d", h.components);
                ok = false;
            }
        }
        if (ok)
        {
            ok = limits.check (h.width, h.height, layout, Sample::U8, err, err_len);
        }
        std::vector<uint8_t> data;
        if (ok)
        {
            size_t row_bytes = (size_t) h.width * (size_t) h.components;
            data.assign (row_bytes * h.height, 0);
            ok = jpeg_read_pixels (&cinfo, &e, data.data (), row_bytes);
        }
        std::vector<uint8_t> icc;
        if (h.icc != NULL && ok)
        {
            icc.assign (h.icc, h.icc + h.icc_len);
        }
        if (h.icc != NULL)
        {
            free (h.icc);
        }
        jpeg_destroy_decompress (&cinfo);
        if (!ok)
        {
            return false;
        }

        if (layout == ChannelLayout::Cmyk && h.cmyk_inverted)
        {
            for (size_t i = 0; i < data.size (); i++)
            {
                data[i] = (uint8_t) (255 - data[i]);
            }
        }
        if (!image_from_raw (h.width, h.height, layout, Sample::U8, data, out, err, err_len))
        {
            return false;
        }
        out->icc = icc;
        /* Unit 0 is only an aspect ratio and never counts as a resolution. */
        if ((h.density_unit == 1 || h.density_unit == 2) && h.x_density > 0 && h.y_density > 0)
        {
            float scale  = h.density_unit == 2 ? 2.54f : 1.0f;
            out->has_dpi = true;
            out->dpi_x   = (float) h.x_density * scale;
            out->dpi_y   = (float) h.y_density * scale;
        }
        return true;
    }

    bool jpeg_write_all (
        j_compress_ptr cinfo, JpegError* e, const Image* img, const uint8_t* data, const EncodeOptions* opts)
    {
        if (setjmp (e->jump))
        {
            return false;
        }
        cinfo->image_width  = img->width;
        cinfo->image_height = img->height;
        if (img->layout == ChannelLayout::Gray)
        {
            cinfo->input_components = 1;
            cinfo->in_color_space   = JCS_GRAYSCALE;
        }
        else if (img->layout == ChannelLayout::Cmyk)
        {
            cinfo->input_components = 4;
            cinfo->in_color_space   = JCS_CMYK;
        }
        else
        {
            cinfo->input_components = 3;
            cinfo->in_color_space   = JCS_RGB;
        }
        jpeg_set_defaults (cinfo);
        int quality = opts->jpeg_quality < 1 ? 1 : opts->jpeg_quality > 100 ? 100 : opts->jpeg_quality;
        jpeg_set_quality (cinfo, quality, TRUE);
        /* CMYK is always 4:4:4: subsampled 4-component JPEGs decode inconsistently elsewhere. */
        bool subsample = opts->jpeg_chroma_subsampling && img->layout == ChannelLayout::Rgb;
        for (int c = 0; c < cinfo->num_components; c++)
        {
            cinfo->comp_info[c].h_samp_factor = (c == 0 && subsample) ? 2 : 1;
            cinfo->comp_info[c].v_samp_factor = (c == 0 && subsample) ? 2 : 1;
        }
        if (opts->embed_metadata && img->has_dpi && img->dpi_x >= 1.0f && img->dpi_y >= 1.0f)
        {
            cinfo->density_unit = 1;
            cinfo->X_density    = (UINT16) fminf (roundf (img->dpi_x), 65535.0f);
            cinfo->Y_density    = (UINT16) fminf (roundf (img->dpi_y), 65535.0f);
        }
        jpeg_start_compress (cinfo, TRUE);
        if (opts->embed_icc && !img->icc.empty ())
        {
            jpeg_write_icc_profile (cinfo, img->icc.data (), (unsigned int) img->icc.size ());
        }
        size_t row_bytes = img->row_bytes ();
        while (cinfo->next_scanline < cinfo->image_height)
        {
            JSAMPROW row = (JSAMPROW) (data + (size_t) cinfo->next_scanline * row_bytes);
            jpeg_write_scanlines (cinfo, &row, 1);
        }
        jpeg_finish_compress (cinfo);
        return true;
    }

    bool jpeg_encode (const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        if (img.sample != Sample::U8 || layout_has_alpha (img.layout))
        {
            set_error (err, err_len, "JPEG cannot store %s %s", layout_name (img.layout), sample_name (img.sample));
            return false;
        }
        if (img.width > 65535 || img.height > 65535)
        {
            set_error (err, err_len, "JPEG dimensions are limited to 65535");
            return false;
        }
        std::vector<uint8_t> inverted;
        const uint8_t* data = img.data.data ();
        if (img.layout == ChannelLayout::Cmyk)
        {
            inverted.resize (img.data.size ());
            for (size_t i = 0; i < inverted.size (); i++)
            {
                inverted[i] = (uint8_t) (255 - img.data[i]);
            }
            data = inverted.data ();
        }

        struct jpeg_compress_struct cinfo;
        JpegError e;
        cinfo.err          = jpeg_std_error (&e.mgr);
        e.mgr.error_exit   = jpeg_on_error;
        e.mgr.emit_message = jpeg_on_message;
        e.buf              = err;
        e.len              = err_len;
        jpeg_create_compress (&cinfo);
        unsigned char* buf    = NULL;
        unsigned long buf_len = 0;
        jpeg_mem_dest (&cinfo, &buf, &buf_len);
        bool ok = jpeg_write_all (&cinfo, &e, &img, data, &opts);
        jpeg_destroy_compress (&cinfo);
        if (ok)
        {
            out->assign (buf, buf + buf_len);
        }
        if (buf != NULL)
        {
            free (buf);
        }
        return ok;
    }

} // namespace codecs
