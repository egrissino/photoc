#include "algo/deform.h"
#include "algo/transform.h"
#include "codecs/codecs.h"
#include "compose/compose.h"
#include "io/io.h"
#include "psd/psd.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace io
{

    /* Smart object contents: decoding the embedded files into documents (Photoshop documents and
       any image this build reads; nested smart objects too) and rendering a smart layer: the
       contents flattened at their own size and mapped onto the layer's four corners. */

    const int MAX_SMART_DEPTH = 8;

    /* A file's bytes as contents; `name` is its file name. */
    bool contents_from_bytes (doc::IdSource& ids, text::FontDb* fonts, const std::string& name,
        std::shared_ptr<const std::vector<uint8_t>> bytes, doc::SmartContents* out, std::vector<std::string>* warnings,
        char* err, size_t err_len)
    {
        out->filename    = name;
        out->bytes       = bytes;
        std::string stem = file_stem (name.c_str ());
        ImportResult r;
        bool psd = psd::is_psd (bytes->data (), bytes->size ());
        memcpy (out->filetype, psd ? (bytes->size () > 5 && (*bytes)[5] == 2 ? "8BPB" : "8BPS") : "    ", 4);
        if (!psd)
        {
            codecs::Format f;
            if (codecs::detect (bytes->data (), bytes->size (), &f))
            {
                /* Photoshop's file type codes are the extension padded to four characters. */
                const char* ext = codecs::format_extensions (f)[0];
                char code[5]    = "    ";
                for (int i = 0; i < 4 && ext[i]; i++)
                {
                    code[i] = ext[i];
                }
                memcpy (out->filetype, code, 4);
            }
        }
        bool ok = psd ? psd::read_document (ids, stem, bytes->data (), bytes->size (), NULL, fonts, &r.document,
                            &r.warnings, err, err_len)
                      : import_bytes (ids, stem, bytes->data (), bytes->size (), &r, err, err_len);
        if (!ok)
        {
            return false;
        }
        if (warnings != NULL)
        {
            warnings->insert (warnings->end (), r.warnings.begin (), r.warnings.end ());
        }
        out->width  = r.document.size.width;
        out->height = r.document.size.height;
        out->doc    = std::make_shared<const doc::Document> (r.document);
        return true;
    }

    /* Every embedded file of `d` (and theirs) opened as a document. Contents that can't be read keep
       their bytes; their layers show the pixels the file stored. */
    void decode_smart_contents (
        doc::IdSource& ids, text::FontDb* fonts, doc::Document* d, std::vector<std::string>* warnings, int depth)
    {
        if (depth > MAX_SMART_DEPTH)
        {
            return;
        }
        for (size_t i = 0; i < d->smart.size (); i++)
        {
            doc::SmartContents& c = d->smart[i];
            if (c.doc || !c.bytes || c.bytes->empty ())
            {
                continue;
            }
            doc::SmartContents decoded = c;
            char err[256]              = "";
            if (!contents_from_bytes (ids, fonts, c.filename, c.bytes, &decoded, NULL, err, sizeof err))
            {
                warnings->push_back (
                    "smart object \"" + c.filename + "\" can't be opened (" + err + "); it shows its stored pixels");
                continue;
            }
            doc::Document inner = *decoded.doc;
            decode_smart_contents (ids, fonts, &inner, warnings, depth + 1);
            decoded.doc = std::make_shared<const doc::Document> (inner);
            /* The size the layer was placed with wins over the file's own (they agree normally). */
            if (c.width == 0 || c.height == 0)
            {
                c.width  = decoded.width;
                c.height = decoded.height;
            }
            c.doc = decoded.doc;
        }
    }

    /* The layer's pixels from its contents: flattened, then mapped onto its corners. False when the
       contents can't be rendered (the stored pixels stay). */
    bool render_smart_layer (const doc::Document& d, doc::Layer* l, const color::CmykSpace* cmyk)
    {
        const doc::SmartContents* c = d.smart_contents (l->smart.uuid);
        if (c == NULL || !c->doc)
        {
            return false;
        }
        const doc::Document& inner       = *c->doc;
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = cmyk;
        color::PixelFormat fmt           = d.pixel_format ();
        fmt.alpha                        = true;
        raster::Surface flat             = compose::composite_to_surface (inner, inner.bounds (), fmt, NULL, settings);
        double r[4]                      = {0, 0, (double) inner.size.width, (double) inner.size.height};
        if (l->smart.warped)
        {
            /* A warped smart object: the contents through its Bezier patch. */
            algo::Deform d = algo::Deform::patch_identity (r);
            memcpy (d.pts, l->smart.warp, sizeof d.pts);
            l->pixels = algo::mesh_warp (flat, inner.bounds (), d, algo::Interp::Bilinear);
            return true;
        }
        algo::Homography h;
        if (!algo::Homography::rect_to_quad (r, l->smart.quad, &h))
        {
            return false;
        }
        l->pixels = algo::warp_surface (flat, inner.bounds (), h, algo::Interp::Bicubic);
        l->pixels.prune ();
        return true;
    }

} // namespace io
