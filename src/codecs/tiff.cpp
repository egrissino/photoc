#include "codecs/codecs.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <tiffio.h>

namespace codecs
{

    /* TIFF through libtiff, from and to memory. Reading takes the first image: gray, RGB and CMYK
       with or without alpha at 8, 16 and 32-bit float, chunky or planar, any compression libtiff
       has; other kinds (palettes, 1-bit, YCbCr, LAB...) through libtiff's RGBA reader at 8 bits.
       Unassociated alpha is kept, associated alpha is divided out. Writing stores the same kinds,
       Deflate-compressed with prediction, with the ICC profile and resolution. */

    struct TiffMem
    {
        const uint8_t* data;
        std::vector<uint8_t>* out;
        size_t len;
        size_t pos;
    };

    tmsize_t tiff_read (thandle_t h, void* buf, tmsize_t n)
    {
        TiffMem* m   = (TiffMem*) h;
        size_t avail = m->pos < m->len ? m->len - m->pos : 0, k = (size_t) n < avail ? (size_t) n : avail;
        const uint8_t* src = m->out != NULL ? m->out->data () : m->data;
        memcpy (buf, src + m->pos, k);
        m->pos += k;
        return (tmsize_t) k;
    }

    tmsize_t tiff_write (thandle_t h, void* buf, tmsize_t n)
    {
        TiffMem* m = (TiffMem*) h;
        if (m->out == NULL)
        {
            return 0;
        }
        if (m->pos + (size_t) n > m->out->size ())
        {
            m->out->resize (m->pos + (size_t) n);
        }
        memcpy (m->out->data () + m->pos, buf, (size_t) n);
        m->pos += (size_t) n;
        if (m->pos > m->len)
        {
            m->len = m->pos;
        }
        return n;
    }

    toff_t tiff_seek (thandle_t h, toff_t off, int whence)
    {
        TiffMem* m   = (TiffMem*) h;
        int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t) m->pos : (int64_t) m->len;
        int64_t at   = base + (int64_t) off;
        if (at < 0)
        {
            return (toff_t) -1;
        }
        m->pos = (size_t) at;
        return (toff_t) at;
    }

    int tiff_close (thandle_t h)
    {
        return 0;
    }

    toff_t tiff_size (thandle_t h)
    {
        return (toff_t) ((TiffMem*) h)->len;
    }

    int tiff_map (thandle_t h, void** base, toff_t* size)
    {
        return 0;
    }

    void tiff_unmap (thandle_t h, void* base, toff_t size) {}

    /* libtiff's messages go to its handler; errors come back as return codes and are reported by
       the caller, warnings are dropped. */
    void tiff_quiet (const char* module, const char* fmt, va_list ap) {}

    TIFF* tiff_open (TiffMem* m, const char* mode)
    {
        TIFFSetErrorHandler (tiff_quiet);
        TIFFSetWarningHandler (tiff_quiet);
        return TIFFClientOpen ("memory", mode, (thandle_t) m, tiff_read, tiff_write, tiff_seek, tiff_close, tiff_size,
            tiff_map, tiff_unmap);
    }

    /* Through the RGBA reader: any TIFF libtiff understands, as 8-bit RGBA. */
    bool tiff_decode_rgba (TIFF* t, uint32_t w, uint32_t h, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        if (!limits.check (w, h, ChannelLayout::Rgba, Sample::U8, err, err_len))
        {
            return false;
        }
        std::vector<uint32_t> px ((size_t) w * h);
        if (!TIFFReadRGBAImageOriented (t, w, h, px.data (), ORIENTATION_TOPLEFT, 0))
        {
            set_error (err, err_len, "TIFF: this kind of TIFF image can't be read");
            return false;
        }
        std::vector<uint8_t> data ((size_t) w * h * 4);
        for (size_t i = 0; i < px.size (); i++)
        {
            data[i * 4]     = (uint8_t) TIFFGetR (px[i]);
            data[i * 4 + 1] = (uint8_t) TIFFGetG (px[i]);
            data[i * 4 + 2] = (uint8_t) TIFFGetB (px[i]);
            data[i * 4 + 3] = (uint8_t) TIFFGetA (px[i]);
        }
        return image_from_raw (w, h, ChannelLayout::Rgba, Sample::U8, data, out, err, err_len);
    }

    bool tiff_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        TiffMem m = {bytes, NULL, len, 0};
        TIFF* t   = tiff_open (&m, "rm");
        if (t == NULL)
        {
            set_error (err, err_len, "TIFF: the file can't be read");
            return false;
        }
        uint32_t w = 0, h = 0;
        uint16_t spp = 1, bps = 8, fmt = SAMPLEFORMAT_UINT, photo = PHOTOMETRIC_RGB, planar = PLANARCONFIG_CONTIG,
                 extra_n = 0, inkset = INKSET_CMYK;
        uint16_t* extra = NULL;
        TIFFGetField (t, TIFFTAG_IMAGEWIDTH, &w);
        TIFFGetField (t, TIFFTAG_IMAGELENGTH, &h);
        TIFFGetFieldDefaulted (t, TIFFTAG_SAMPLESPERPIXEL, &spp);
        TIFFGetFieldDefaulted (t, TIFFTAG_BITSPERSAMPLE, &bps);
        TIFFGetFieldDefaulted (t, TIFFTAG_SAMPLEFORMAT, &fmt);
        TIFFGetFieldDefaulted (t, TIFFTAG_PLANARCONFIG, &planar);
        TIFFGetFieldDefaulted (t, TIFFTAG_EXTRASAMPLES, &extra_n, &extra);
        TIFFGetFieldDefaulted (t, TIFFTAG_INKSET, &inkset);
        if (!TIFFGetField (t, TIFFTAG_PHOTOMETRIC, &photo))
        {
            photo = spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK;
        }
        bool ok    = false;
        int colors = photo == PHOTOMETRIC_RGB                                           ? 3
                   : photo == PHOTOMETRIC_SEPARATED && inkset == INKSET_CMYK            ? 4
                   : photo == PHOTOMETRIC_MINISBLACK || photo == PHOTOMETRIC_MINISWHITE ? 1
                                                                                        : 0;
        bool direct =
            colors > 0 && spp >= colors
            && ((bps == 8 || bps == 16) && fmt == SAMPLEFORMAT_UINT ? true : bps == 32 && fmt == SAMPLEFORMAT_IEEEFP);
        if (!direct)
        {
            ok = tiff_decode_rgba (t, w, h, limits, out, err, err_len);
        }
        else
        {
            bool alpha           = spp > colors;
            bool associated      = alpha && extra_n > 0 && extra != NULL && extra[0] == EXTRASAMPLE_ASSOCALPHA;
            ChannelLayout layout = colors == 1 ? (alpha ? ChannelLayout::GrayA : ChannelLayout::Gray)
                                 : colors == 3 ? (alpha ? ChannelLayout::Rgba : ChannelLayout::Rgb)
                                               : (alpha ? ChannelLayout::CmykA : ChannelLayout::Cmyk);
            Sample sample        = bps == 8 ? Sample::U8 : bps == 16 ? Sample::U16 : Sample::F32;
            if (limits.check (w, h, layout, sample, err, err_len))
            {
                size_t n = layout_channels (layout), bs = sample_size (sample);
                std::vector<uint8_t> data ((size_t) w * h * n * bs);
                tmsize_t sl = TIFFScanlineSize (t);
                std::vector<uint8_t> line ((size_t) sl);
                ok         = true;
                int planes = planar == PLANARCONFIG_SEPARATE ? (int) n : 1;
                for (int pl = 0; pl < planes && ok; pl++)
                {
                    for (uint32_t y = 0; y < h && ok; y++)
                    {
                        if (TIFFReadScanline (t, line.data (), y, (uint16_t) pl) < 0)
                        {
                            ok = false;
                            break;
                        }
                        for (uint32_t x = 0; x < w; x++)
                        {
                            if (planes == 1)
                            {
                                /* Chunky: the first n of spp samples. */
                                memcpy (&data[((size_t) y * w + x) * n * bs], &line[(size_t) x * spp * bs], n * bs);
                            }
                            else
                            {
                                memcpy (
                                    &data[(((size_t) y * w + x) * n + (size_t) pl) * bs], &line[(size_t) x * bs], bs);
                            }
                        }
                    }
                }
                if (!ok)
                {
                    set_error (err, err_len, "TIFF: the image data is damaged");
                }
                if (ok && photo == PHOTOMETRIC_MINISWHITE)
                {
                    for (size_t i = 0; i < (size_t) w * h; i++)
                    {
                        uint8_t* p = &data[i * n * bs];
                        if (bs == 1)
                        {
                            p[0] = (uint8_t) (255 - p[0]);
                        }
                        else if (bs == 2)
                        {
                            uint16_t v;
                            memcpy (&v, p, 2);
                            v = (uint16_t) (65535 - v);
                            memcpy (p, &v, 2);
                        }
                        else
                        {
                            float v;
                            memcpy (&v, p, 4);
                            v = 1.0f - v;
                            memcpy (p, &v, 4);
                        }
                    }
                }
                if (ok)
                {
                    ok = image_from_raw (w, h, layout, sample, data, out, err, err_len);
                }
                if (ok && associated)
                {
                    /* Premultiplied: back to straight colour. */
                    size_t count = (size_t) w * h, cc = n - 1;
                    Image f;
                    if (image_convert (*out, layout, Sample::F32, &f, err, err_len))
                    {
                        float* v = (float*) f.data.data ();
                        for (size_t i = 0; i < count; i++)
                        {
                            float a = v[i * n + cc];
                            for (size_t c = 0; c < cc; c++)
                            {
                                v[i * n + c] = a > 0 ? v[i * n + c] / a : 0;
                            }
                        }
                        image_convert (f, layout, sample, out, err, err_len);
                    }
                }
            }
        }
        if (ok)
        {
            uint32_t icc_len = 0;
            void* icc        = NULL;
            if (TIFFGetField (t, TIFFTAG_ICCPROFILE, &icc_len, &icc) && icc != NULL && icc_len > 0)
            {
                out->icc.assign ((uint8_t*) icc, (uint8_t*) icc + icc_len);
            }
            float xr = 0, yr = 0;
            uint16_t unit = RESUNIT_INCH;
            TIFFGetFieldDefaulted (t, TIFFTAG_RESOLUTIONUNIT, &unit);
            if (TIFFGetField (t, TIFFTAG_XRESOLUTION, &xr) && TIFFGetField (t, TIFFTAG_YRESOLUTION, &yr) && xr > 0
                && yr > 0 && unit != RESUNIT_NONE)
            {
                float k      = unit == RESUNIT_CENTIMETER ? 2.54f : 1.0f;
                out->has_dpi = true;
                out->dpi_x   = xr * k;
                out->dpi_y   = yr * k;
            }
        }
        TIFFClose (t);
        return ok;
    }

    bool tiff_encode (const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        out->clear ();
        TiffMem m = {NULL, out, 0, 0};
        TIFF* t   = tiff_open (&m, "w");
        if (t == NULL)
        {
            set_error (err, err_len, "TIFF: can't start writing");
            return false;
        }
        size_t n = img.channels (), bs = sample_size (img.sample);
        bool alpha = layout_has_alpha (img.layout), fl = img.sample == Sample::F32;
        uint16_t photo = layout_is_gray (img.layout) ? PHOTOMETRIC_MINISBLACK
                       : layout_is_cmyk (img.layout) ? PHOTOMETRIC_SEPARATED
                                                     : PHOTOMETRIC_RGB;
        TIFFSetField (t, TIFFTAG_IMAGEWIDTH, img.width);
        TIFFSetField (t, TIFFTAG_IMAGELENGTH, img.height);
        TIFFSetField (t, TIFFTAG_SAMPLESPERPIXEL, (uint16_t) n);
        TIFFSetField (t, TIFFTAG_BITSPERSAMPLE, (uint16_t) (bs * 8));
        TIFFSetField (t, TIFFTAG_SAMPLEFORMAT, fl ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
        TIFFSetField (t, TIFFTAG_PHOTOMETRIC, photo);
        if (photo == PHOTOMETRIC_SEPARATED)
        {
            TIFFSetField (t, TIFFTAG_INKSET, INKSET_CMYK);
        }
        TIFFSetField (t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField (t, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
        TIFFSetField (t, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
        TIFFSetField (t, TIFFTAG_PREDICTOR, fl ? PREDICTOR_FLOATINGPOINT : PREDICTOR_HORIZONTAL);
        if (alpha)
        {
            uint16_t ex = EXTRASAMPLE_UNASSALPHA;
            TIFFSetField (t, TIFFTAG_EXTRASAMPLES, (uint16_t) 1, &ex);
        }
        TIFFSetField (t, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize (t, 0));
        if (img.has_dpi)
        {
            TIFFSetField (t, TIFFTAG_XRESOLUTION, img.dpi_x);
            TIFFSetField (t, TIFFTAG_YRESOLUTION, img.dpi_y);
            TIFFSetField (t, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        }
        if (opts.embed_icc && !img.icc.empty ())
        {
            TIFFSetField (t, TIFFTAG_ICCPROFILE, (uint32_t) img.icc.size (), img.icc.data ());
        }
        TIFFSetField (t, TIFFTAG_SOFTWARE, "PhotoC");
        std::vector<uint8_t> line (img.width * n * bs);
        bool ok = true;
        for (uint32_t y = 0; y < img.height && ok; y++)
        {
            memcpy (line.data (), &img.data[(size_t) y * line.size ()], line.size ());
            if (TIFFWriteScanline (t, line.data (), y, 0) < 0)
            {
                ok = false;
            }
        }
        if (ok && !TIFFWriteDirectory (t))
        {
            ok = false;
        }
        TIFFClose (t);
        if (!ok)
        {
            set_error (err, err_len, "TIFF: writing failed");
        }
        out->resize (m.len);
        return ok;
    }

} // namespace codecs
