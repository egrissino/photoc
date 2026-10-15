#include "io/io.h"

#include "psd/psd.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

namespace io
{

    using codecs::ChannelLayout;
    using codecs::Sample;
    using color::ColorMode;
    using color::SampleType;
    using geom::Rect;

    bool read_file (const char* path, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        FILE* f = fopen (path, "rb");
        if (f == NULL)
        {
            codecs::set_error (err, err_len, "cannot open %s: %s", path, strerror (errno));
            return false;
        }
        out->clear ();
        uint8_t buf[1 << 16];
        size_t n;
        while ((n = fread (buf, 1, sizeof buf, f)) > 0)
        {
            out->insert (out->end (), buf, buf + n);
        }
        bool failed = ferror (f) != 0;
        fclose (f);
        if (failed)
        {
            codecs::set_error (err, err_len, "cannot read %s", path);
            return false;
        }
        return true;
    }

    /* Writes to "<path>.tmp-<pid>", then renames over `path`, so a failed save never leaves a
       half-written file in place of the old one. */
    bool write_file (const char* path, const std::vector<uint8_t>& data, char* err, size_t err_len)
    {
        std::string tmp = std::string (path) + ".tmp-" + std::to_string ((long) getpid ());
        FILE* f         = fopen (tmp.c_str (), "wb");
        if (f == NULL)
        {
            codecs::set_error (err, err_len, "cannot create %s: %s", tmp.c_str (), strerror (errno));
            return false;
        }
        bool ok = data.empty () || fwrite (data.data (), 1, data.size (), f) == data.size ();
        ok      = fflush (f) == 0 && ok;
        ok      = fsync (fileno (f)) == 0 && ok;
        ok      = fclose (f) == 0 && ok;
        if (!ok)
        {
            codecs::set_error (err, err_len, "cannot write %s: %s", path, strerror (errno));
            unlink (tmp.c_str ());
            return false;
        }
        if (rename (tmp.c_str (), path) != 0)
        {
            codecs::set_error (err, err_len, "cannot replace %s: %s", path, strerror (errno));
            unlink (tmp.c_str ());
            return false;
        }
        return true;
    }

    /* "dir/photo.ppm" -> "photo". */
    std::string file_stem (const char* path)
    {
        const char* base = strrchr (path, '/');
        base             = base == NULL ? path : base + 1;
        const char* dot  = strrchr (base, '.');
        if (dot == NULL || dot == base)
        {
            return base;
        }
        return std::string (base, (size_t) (dot - base));
    }

    /* A decoded image as a single-layer document. As in Photoshop, an opaque image opens as the
       locked Background, one with transparency as a normal layer named "Layer 0". */
    bool image_to_document (doc::IdSource& ids, const std::string& name, const codecs::Image& img, ImportResult* out,
        char* err, size_t err_len)
    {
        ImportResult r;
        ColorMode mode;
        ChannelLayout target;
        if (codecs::layout_is_gray (img.layout))
        {
            mode   = ColorMode::Grayscale;
            target = ChannelLayout::GrayA;
        }
        else if (codecs::layout_is_rgb (img.layout))
        {
            mode   = ColorMode::Rgb;
            target = ChannelLayout::Rgba;
        }
        else
        {
            mode   = ColorMode::Cmyk;
            target = ChannelLayout::CmykA;
        }
        SampleType depth;
        Sample sample;
        switch (img.sample)
        {
        case Sample::U8:
            depth  = SampleType::U8;
            sample = Sample::U8;
            break;
        case Sample::U16:
            depth  = SampleType::U16;
            sample = Sample::U16;
            break;
        case Sample::F16:
            r.warnings.push_back ("16-bit float samples are stored as 32-bit float");
            depth  = SampleType::F32;
            sample = Sample::F32;
            break;
        default:
            depth  = SampleType::F32;
            sample = Sample::F32;
            break;
        }

        codecs::Image converted;
        const codecs::Image* src = &img;
        if (img.layout != target || img.sample != sample)
        {
            if (!codecs::image_convert (img, target, sample, &converted, err, err_len))
            {
                return false;
            }
            src = &converted;
        }

        geom::Size size  = {img.width, img.height};
        r.document       = doc::Document::make (ids, name, size, mode, depth);
        bool alpha       = codecs::layout_has_alpha (img.layout);
        doc::Layer layer = doc::Layer::raster (ids, alpha ? "Layer 0" : "Background", r.document.pixel_format ());
        if (!alpha)
        {
            layer.locks.transparency = true;
            layer.locks.position     = true;
        }
        if (!layer.pixels.write_interleaved (r.document.bounds (), src->data.data (), src->data.size ()))
        {
            codecs::set_error (err, err_len, "image data does not match its %ux%u size", img.width, img.height);
            return false;
        }
        layer.pixels.prune ();
        r.document.layers.push_back (layer);
        if (!img.icc.empty ())
        {
            r.document.icc_profile = std::make_shared<const std::vector<uint8_t>> (img.icc);
        }
        if (img.has_dpi)
        {
            r.document.resolution_dpi = img.dpi_x;
        }
        *out = r;
        return true;
    }

    Sample codec_sample (SampleType s)
    {
        switch (s)
        {
        case SampleType::U8:
            return Sample::U8;
        case SampleType::U16:
            return Sample::U16;
        case SampleType::F32:
            return Sample::F32;
        }
        return Sample::U8;
    }

    ChannelLayout layout_for (ColorMode mode, bool alpha)
    {
        ChannelLayout l;
        if (mode == ColorMode::Grayscale)
        {
            l = ChannelLayout::Gray;
        }
        else if (mode == ColorMode::Cmyk)
        {
            l = ChannelLayout::Cmyk;
        }
        else
        {
            l = ChannelLayout::Rgb;
        }
        return alpha ? codecs::layout_with_alpha (l) : l;
    }

    /* The one layer a document can be written from without compositing: a single visible, fully
       opaque, Normal pixel layer without a mask. */
    const raster::Surface* single_layer (const doc::Document& d)
    {
        if (d.layers.size () != 1 || d.mode == ColorMode::Lab)
        {
            return NULL;
        }
        const doc::Layer& l = d.layers[0];
        if (l.kind != doc::LayerKind::Raster || !l.visible || l.has_mask || l.opacity < 1.0f || l.fill_opacity < 1.0f)
        {
            return NULL;
        }
        if (l.blend != color::BlendMode::Normal)
        {
            return NULL;
        }
        return &l.pixels;
    }

    /* Drops the last (alpha) channel in place when every pixel is fully opaque. */
    void drop_alpha_if_opaque (codecs::Image* img)
    {
        if (!codecs::layout_has_alpha (img->layout))
        {
            return;
        }
        size_t n      = img->channels ();
        size_t bps    = codecs::sample_size (img->sample);
        size_t pixels = (size_t) img->width * img->height;
        for (size_t p = 0; p < pixels; p++)
        {
            if (img->sample_at (p * n + n - 1) < 1.0f)
            {
                return;
            }
        }
        size_t keep = (n - 1) * bps;
        for (size_t p = 0; p < pixels; p++)
        {
            memmove (&img->data[p * keep], &img->data[p * n * bps], keep);
        }
        img->data.resize (pixels * keep);
        img->layout = codecs::layout_without_alpha (img->layout);
    }

    /* The document as a flat image. A single plain layer keeps the document's model and depth;
       anything else is composited (in RGB or gray) and the layers are flattened. */
    bool document_to_image (const doc::Document& d, const compose::RenderSettings& settings, codecs::Image* out,
        std::vector<std::string>* warnings, char* err, size_t err_len)
    {
        Rect canvas            = d.bounds ();
        color::PixelFormat fmt = d.pixel_format ();
        std::vector<uint8_t> data;
        codecs::Image img;
        const raster::Surface* s = single_layer (d);
        /* A Duotone image exports as printed: its inks rendered to RGB. */
        bool duo = d.mode == ColorMode::Duotone && d.has_duotone;
        if (s != NULL && s->format () == fmt && !duo)
        {
            if (!s->to_interleaved (canvas, &data))
            {
                codecs::set_error (err, err_len, "the canvas is too large to export");
                return false;
            }
            if (!codecs::image_from_raw (d.size.width, d.size.height, layout_for (fmt.mode, true),
                    codec_sample (fmt.sample), data, &img, err, err_len))
            {
                return false;
            }
        }
        else
        {
            if (duo)
            {
                warnings->push_back ("Duotone written as RGB, its inks rendered");
            }
            else
            {
                warnings->push_back (std::to_string (d.walk ().size ())
                                     + " layer(s) flattened; layers, masks and blend modes are not kept");
            }
            bool gray                  = fmt.mode == ColorMode::Grayscale && !duo;
            compose::RenderSettings rs = settings;
            rs.inks                    = true;
            if (fmt.mode == ColorMode::Cmyk || fmt.mode == ColorMode::Lab)
            {
                warnings->push_back (
                    std::string (fmt.mode == ColorMode::Cmyk ? "Cmyk" : "Lab") + " composite written as sRGB RGB");
            }
            color::PixelFormat out_fmt = {gray ? ColorMode::Grayscale : ColorMode::Rgb, fmt.sample, true};
            raster::Surface flat       = compose::composite_to_surface (d, canvas, out_fmt, NULL, rs);
            if (!flat.to_interleaved (canvas, &data))
            {
                codecs::set_error (err, err_len, "the canvas is too large to export");
                return false;
            }
            if (!codecs::image_from_raw (d.size.width, d.size.height, layout_for (out_fmt.mode, true),
                    codec_sample (fmt.sample), data, &img, err, err_len))
            {
                return false;
            }
        }
        drop_alpha_if_opaque (&img);
        if (d.icc_profile)
        {
            img.icc = *d.icc_profile;
        }
        img.has_dpi = true;
        img.dpi_x = img.dpi_y = d.resolution_dpi;
        *out                  = img;
        return true;
    }

    /* Seconds on a steady clock, for timing file operations. */
    double monotonic_seconds ()
    {
        struct timespec ts;
        clock_gettime (CLOCK_MONOTONIC, &ts);
        return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
    }

    bool import_bytes (doc::IdSource& ids, const std::string& name, const uint8_t* bytes, size_t len, ImportResult* out,
        char* err, size_t err_len)
    {
        codecs::Image img;
        double t0 = monotonic_seconds ();
        if (!codecs::decode (bytes, len, codecs::Limits::defaults (), &img, err, err_len))
        {
            return false;
        }
        double t1          = monotonic_seconds ();
        bool ok            = image_to_document (ids, name, img, out, err, err_len);
        out->timing.decode = t1 - t0;
        out->timing.build  = monotonic_seconds () - t1;
        return ok;
    }

    bool export_bytes (const doc::Document& d, codecs::Format format, const codecs::EncodeOptions& opts,
        const compose::RenderSettings& settings, std::vector<uint8_t>* out, std::vector<std::string>* warnings,
        char* err, size_t err_len)
    {
        if (!codecs::format_can_encode (format))
        {
            codecs::set_error (err, err_len, "writing %s files is not ported yet", codecs::format_name (format));
            return false;
        }
        return export_bytes_timed (d, format, opts, settings, out, warnings, err, err_len, NULL);
    }

    bool export_bytes_timed (const doc::Document& d, codecs::Format format, const codecs::EncodeOptions& opts,
        const compose::RenderSettings& settings, std::vector<uint8_t>* out, std::vector<std::string>* warnings,
        char* err, size_t err_len, IoTiming* timing)
    {
        if (!codecs::format_can_encode (format))
        {
            codecs::set_error (err, err_len, "writing %s files is not ported yet", codecs::format_name (format));
            return false;
        }
        codecs::Image img;
        double t0 = monotonic_seconds ();
        if (!document_to_image (d, settings, &img, warnings, err, err_len))
        {
            return false;
        }
        double t1 = monotonic_seconds ();
        bool ok   = codecs::encode (img, format, opts, out, err, err_len);
        if (timing != NULL)
        {
            timing->flatten = t1 - t0;
            timing->encode  = monotonic_seconds () - t1;
        }
        return ok;
    }

    /* `fonts` lets PSD type open as editable type (NULL: it opens as the pixels Photoshop stored). */
    bool open_file (
        doc::IdSource& ids, text::FontDb* fonts, const char* path, ImportResult* out, char* err, size_t err_len)
    {
        std::vector<uint8_t> bytes;
        memset (&out->timing, 0, sizeof out->timing);
        double t0 = monotonic_seconds ();
        if (!read_file (path, &bytes, err, err_len))
        {
            return false;
        }
        out->timing.read = monotonic_seconds () - t0;
        if (psd::is_psd (bytes.data (), bytes.size ()))
        {
            double t1 = monotonic_seconds ();
            if (!psd::read_document (ids, file_stem (path), bytes.data (), bytes.size (), NULL, fonts, &out->document,
                    &out->warnings, err, err_len))
            {
                return false;
            }
            double t2 = monotonic_seconds ();
            decode_smart_contents (ids, fonts, &out->document, &out->warnings, 0);
            out->timing.decode = t2 - t1;
            out->timing.smart  = monotonic_seconds () - t2;
            return true;
        }
        return import_bytes (ids, file_stem (path), bytes.data (), bytes.size (), out, err, err_len);
    }

    /* True when the path (or format name) names a Photoshop document. */
    bool is_psd_name (const char* name)
    {
        size_t n = strlen (name);
        return n >= 3 && strcasecmp (name + n - 3, "psd") == 0;
    }

    /* `format_ext` overrides the format the path's extension names (NULL to use the path). */
    bool save_file (const doc::Document& d, text::FontDb* fonts, const char* path, const char* format_ext,
        const codecs::EncodeOptions& opts, const compose::RenderSettings& settings, std::vector<std::string>* warnings,
        char* err, size_t err_len)
    {
        return save_file_timed (d, fonts, path, format_ext, opts, settings, warnings, err, err_len, NULL);
    }

    /* The same, recording how long each step took when `timing` isn't NULL (a PSD's flatten and
       encode count together as encode). */
    bool save_file_timed (const doc::Document& d, text::FontDb* fonts, const char* path, const char* format_ext,
        const codecs::EncodeOptions& opts, const compose::RenderSettings& settings, std::vector<std::string>* warnings,
        char* err, size_t err_len, IoTiming* timing)
    {
        IoTiming local;
        IoTiming* t = timing != NULL ? timing : &local;
        memset (t, 0, sizeof *t);
        codecs::Format format;
        const char* name = format_ext != NULL ? format_ext : path;
        std::vector<uint8_t> bytes;
        if (is_psd_name (name))
        {
            double t0 = monotonic_seconds ();
            if (!psd::write_document (d, settings.cmyk, fonts, &bytes, warnings, err, err_len))
            {
                return false;
            }
            t->encode = monotonic_seconds () - t0;
        }
        else
        {
            if (!codecs::from_extension (name, &format))
            {
                codecs::set_error (err, err_len, "unknown file format for %s", name);
                return false;
            }
            if (!export_bytes_timed (d, format, opts, settings, &bytes, warnings, err, err_len, t))
            {
                return false;
            }
        }
        double t0 = monotonic_seconds ();
        bool ok   = write_file (path, bytes, err, err_len);
        t->write  = monotonic_seconds () - t0;
        return ok;
    }

} // namespace io
