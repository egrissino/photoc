#include "psd/smart.h"

#include "psd/bytes.h"
#include "psd/descriptor.h"
#include "psd/effects.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace psd
{

    /* Smart objects in Photoshop files. A smart object layer carries SoLd (a descriptor: the
       contents' id, its corners after the transform and its size), or the older PlLd (binary, the
       same transform). The embedded files live once per document in a global lnk2 (or lnkD / lnk3)
       block: per file a kind ('liFD' embedded data; linked and alias files are kept but can't be
       opened), version, id, file name, type and creator codes, and the file's bytes. */

    /* Eight numbers: the corners' x, y from the top left clockwise. */
    bool quad_from_list (const DValue* v, double quad[4][2])
    {
        if (v == NULL || v->type != DType::List || v->items.size () != 8)
        {
            return false;
        }
        for (int i = 0; i < 8; i++)
        {
            quad[i / 2][i % 2] = v->items[(size_t) i].num;
        }
        return true;
    }

    /* Photoshop's filters as this build's: the filter class, the command, and its settings
       (descriptor key -> parameter; "%" keys take percent units as they are). */
    struct FilterMap
    {
        const char* cls;
        const char* id;
        const char* keys[3];
        const char* params[3];
    };

    const FilterMap FILTER_MAP[] = {
        {"GsnB", "filter.blur.gaussianBlur", {"Rds ", NULL, NULL}, {"radius", NULL, NULL}},
        {"BxBr", "filter.blur.boxBlur", {"Rds ", NULL, NULL}, {"radius", NULL, NULL}},
        {"MtnB", "filter.blur.motionBlur", {"Angl", "Dstn", NULL}, {"angle", "distance", NULL}},
        {"Blr ", "filter.blur.blur", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
        {"BlrM", "filter.blur.blurMore", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
        {"Avrg", "filter.blur.average", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
        {"SrfB", "filter.blur.surfaceBlur", {"Rds ", "Thsh", NULL}, {"radius", "threshold", NULL}},
        {"UnsM", "filter.sharpen.unsharpMask", {"Amnt", "Rds ", "Thsh"}, {"amount", "radius", "threshold"}},
        {"Shrp", "filter.sharpen.sharpen", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
        {"ShrM", "filter.sharpen.sharpenMore", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
        {"ShrE", "filter.sharpen.sharpenEdges", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
        {"AdNs", "filter.noise.addNoise", {"Nose", NULL, NULL}, {"amount", NULL, NULL}},
        {"Mdn ", "filter.noise.median", {"Rds ", NULL, NULL}, {"radius", NULL, NULL}},
        {"Dspc", "filter.noise.despeckle", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
        {"DstS", "filter.noise.dustAndScratches", {"Rds ", "Thsh", NULL}, {"radius", "threshold", NULL}},
        {"HghP", "filter.other.highPass", {"Rds ", NULL, NULL}, {"radius", NULL, NULL}},
        {"Mxm ", "filter.other.maximum", {"Rds ", NULL, NULL}, {"radius", NULL, NULL}},
        {"Mnm ", "filter.other.minimum", {"Rds ", NULL, NULL}, {"radius", NULL, NULL}},
        {"Msc ", "filter.pixelate.mosaic", {"ClSz", NULL, NULL}, {"cellSize", NULL, NULL}},
        {"Crst", "filter.pixelate.crystallize", {"ClSz", NULL, NULL}, {"cellSize", NULL, NULL}},
        {"Pntl", "filter.pixelate.pointillize", {"ClSz", NULL, NULL}, {"cellSize", NULL, NULL}},
        {"Embs", "filter.stylize.emboss", {"Angl", "Hght", "Amnt"}, {"angle", "height", "amount"}},
        {"FndE", "filter.stylize.findEdges", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
        {"Slrz", "filter.stylize.solarize", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
        {"Twrl", "filter.distort.twirl", {"Angl", NULL, NULL}, {"angle", NULL, NULL}},
        {"Pnch", "filter.distort.pinch", {"Amnt", NULL, NULL}, {"amount", NULL, NULL}},
        {"Clds", "filter.render.clouds", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
        {"DfrC", "filter.render.differenceClouds", {NULL, NULL, NULL}, {NULL, NULL, NULL}},
    };

    /* One of Photoshop's smart filters; the id stays empty for filters this build doesn't have. */
    doc::SmartFilter filter_from_ps (const DValue& fx)
    {
        doc::SmartFilter f;
        const DValue* nm = d_get (fx, "Nm  ");
        f.name           = nm != NULL && nm->type == DType::Text ? nm->str : "Filter";
        f.enabled        = d_flag (fx, "enab", true);
        f.blend          = color::BlendMode::Normal;
        f.opacity        = 1.0f;
        const DValue* bo = d_get (fx, "blendOptions");
        if (bo != NULL && bo->type == DType::Object)
        {
            f.opacity        = (float) (d_num (*bo, "Opct", 100.0) / 100.0);
            const DValue* md = d_get (*bo, "Md  ");
            color::BlendMode m;
            if (md != NULL && md->type == DType::Enum && blend_from_key (md->str.c_str (), &m))
            {
                f.blend = m;
            }
        }
        f.params         = "{}";
        const DValue* fl = d_get (fx, "Fltr");
        if (fl == NULL || fl->type != DType::Object)
        {
            return f;
        }
        for (size_t i = 0; i < sizeof FILTER_MAP / sizeof FILTER_MAP[0]; i++)
        {
            const FilterMap& m = FILTER_MAP[i];
            if (fl->cls != m.cls)
            {
                continue;
            }
            f.id               = m.id;
            std::string params = "{";
            for (int k = 0; k < 3 && m.keys[k] != NULL; k++)
            {
                if (d_get (*fl, m.keys[k]) == NULL)
                {
                    continue;
                }
                char buf[96];
                snprintf (buf, sizeof buf, "%s\"%s\":%g", params.size () > 1 ? "," : "", m.params[k],
                    d_num (*fl, m.keys[k], 0));
                params += buf;
            }
            if (strcmp (m.cls, "AdNs") == 0)
            {
                if (d_enum_of (*fl, "Dstr") == "Gsn ")
                {
                    params += params.size () > 1 ? ",\"distribution\":\"gaussian\"" : "\"distribution\":\"gaussian\"";
                }
                if (d_flag (*fl, "Mnch", false))
                {
                    params += params.size () > 1 ? ",\"monochromatic\":true" : "\"monochromatic\":true";
                }
            }
            f.params = params + "}";
            break;
        }
        return f;
    }

    /* This build's own smart filters (a private key), else Photoshop's filterFX. */
    void parse_filters (const DValue& d, doc::SmartLayer* out)
    {
        out->filters.clear ();
        out->filters_enabled = true;
        const DValue* own    = d_get (d, "photocSmartFilters");
        if (own != NULL && own->type == DType::List)
        {
            out->filters_enabled = d_flag (d, "photocFiltersOn", true);
            for (size_t i = 0; i < own->items.size (); i++)
            {
                const DValue& it = own->items[i];
                if (it.type != DType::Object)
                {
                    continue;
                }
                doc::SmartFilter f;
                const DValue* v = d_get (it, "Idnt");
                f.id            = v != NULL && v->type == DType::Text ? v->str : "";
                v               = d_get (it, "Nm  ");
                f.name          = v != NULL && v->type == DType::Text ? v->str : f.id;
                v               = d_get (it, "prms");
                f.params        = v != NULL && v->type == DType::Text ? v->str : "{}";
                f.enabled       = d_flag (it, "enab", true);
                f.opacity       = (float) d_num (it, "Opct", 1.0);
                f.blend         = color::BlendMode::Normal;
                v               = d_get (it, "Md  ");
                color::BlendMode m;
                if (v != NULL && v->type == DType::Text && blend_from_key (v->str.c_str (), &m))
                {
                    f.blend = m;
                }
                out->filters.push_back (f);
            }
            return;
        }
        const DValue* fx = d_get (d, "filterFX");
        if (fx == NULL || fx->type != DType::Object)
        {
            return;
        }
        out->filters_enabled = d_flag (*fx, "enab", true);
        const DValue* list   = d_get (*fx, "filterFXList");
        if (list == NULL || list->type != DType::List)
        {
            return;
        }
        for (size_t i = 0; i < list->items.size (); i++)
        {
            if (list->items[i].type == DType::Object)
            {
                out->filters.push_back (filter_from_ps (list->items[i]));
            }
        }
    }

    bool parse_sold (const uint8_t* data, size_t len, doc::SmartLayer* out, uint32_t* w, uint32_t* h)
    {
        Reader r = reader (data, len);
        if (rd_key4 (&r) != "soLD")
        {
            return false;
        }
        rd_u32 (&r);
        rd_u32 (&r);
        DValue d;
        if (!parse_descriptor (&r, &d))
        {
            return false;
        }
        const DValue* id = d_get (d, "Idnt");
        if (id == NULL)
        {
            id = d_get (d, "placed");
        }
        if (id == NULL || id->type != DType::Text)
        {
            return false;
        }
        out->uuid = id->str;
        if (!quad_from_list (d_get (d, "nonAffineTransform"), out->quad)
            && !quad_from_list (d_get (d, "Trnf"), out->quad))
        {
            return false;
        }
        parse_filters (d, out);
        *w = *h          = 0;
        const DValue* sz = d_get (d, "Sz  ");
        if (sz != NULL && sz->type == DType::Object)
        {
            *w = (uint32_t) lround (d_num (*sz, "Wdth", 0));
            *h = (uint32_t) lround (d_num (*sz, "Hght", 0));
        }
        return true;
    }

    bool parse_plld (const uint8_t* data, size_t len, doc::SmartLayer* out)
    {
        Reader r = reader (data, len);
        if (rd_key4 (&r) != "plcL")
        {
            return false;
        }
        rd_u32 (&r);
        out->uuid = rd_pascal (&r, 1);
        rd_u32 (&r);
        rd_u32 (&r);
        rd_u32 (&r);
        rd_u32 (&r);
        for (int i = 0; i < 8; i++)
        {
            out->quad[i / 2][i % 2] = rd_f64 (&r);
        }
        return r.ok && !out->uuid.empty ();
    }

    bool parse_smart_layer (const uint8_t* sold, size_t sold_len, const uint8_t* plld, size_t plld_len,
        doc::SmartLayer* out, uint32_t* w, uint32_t* h)
    {
        *w = *h = 0;
        if (sold != NULL && parse_sold (sold, sold_len, out, w, h))
        {
            return true;
        }
        return plld != NULL && parse_plld (plld, plld_len, out);
    }

    /* The files of an lnk2 / lnkD / lnk3 block. */
    void parse_linked_files (const uint8_t* data, size_t len, std::vector<doc::SmartContents>* out)
    {
        Reader r = reader (data, len);
        while (r.ok && remaining (r) >= 8)
        {
            uint64_t n = rd_u64 (&r);
            if (n == 0 || n > remaining (r))
            {
                break;
            }
            size_t start     = r.pos;
            Reader e         = reader (r.data + r.pos, (size_t) n);
            std::string kind = rd_key4 (&e);
            uint32_t version = rd_u32 (&e);
            doc::SmartContents c;
            c.uuid           = rd_pascal (&e, 1);
            c.filename       = rd_unicode (&e);
            std::string type = rd_key4 (&e);
            memcpy (c.filetype, type.size () == 4 ? type.data () : "    ", 4);
            rd_key4 (&e);
            uint64_t size = rd_u64 (&e);
            uint8_t open  = rd_u8 (&e);
            if (open)
            {
                rd_u32 (&e);
                DValue skip;
                parse_descriptor (&e, &skip);
            }
            c.width = c.height = 0;
            if (kind == "liFD" && e.ok && size <= remaining (e))
            {
                const uint8_t* p = rd_bytes (&e, (size_t) size);
                c.bytes          = std::make_shared<const std::vector<uint8_t>> (p, p + size);
            }
            (void) version;
            if (e.ok && !c.uuid.empty ())
            {
                out->push_back (c);
            }
            r.pos = start + (size_t) n;
            while ((r.pos & 3) != 0 && r.pos < r.len)
            {
                r.pos++;
            }
        }
    }

    DValue list8 (const double quad[4][2])
    {
        DValue l = d_list ();
        for (int i = 0; i < 8; i++)
        {
            l.items.push_back (d_double (quad[i / 2][i % 2]));
        }
        return l;
    }

    DValue frame_rate (int32_t num, int32_t den)
    {
        DValue f = d_object ("null");
        d_set (&f, "numerator", d_long (num));
        d_set (&f, "denominator", d_long (den));
        return f;
    }

    /* SoLd for a smart layer of contents w x h at `dpi`. */
    std::vector<uint8_t> write_sold (const doc::SmartLayer& s, uint32_t w, uint32_t h, float dpi)
    {
        DValue d = d_object ("null");
        d_set (&d, "Idnt", d_text (s.uuid));
        d_set (&d, "placed", d_text (s.uuid));
        d_set (&d, "PgNm", d_long (1));
        d_set (&d, "totalPages", d_long (1));
        d_set (&d, "Crop", d_long (1));
        d_set (&d, "frameStep", frame_rate (0, 600));
        d_set (&d, "duration", frame_rate (0, 600));
        d_set (&d, "frameCount", d_long (1));
        d_set (&d, "Annt", d_long (16));
        d_set (&d, "Type", d_long (2));
        d_set (&d, "Trnf", list8 (s.quad));
        d_set (&d, "nonAffineTransform", list8 (s.quad));
        DValue warp = d_object ("warp");
        d_set (&warp, "warpStyle", d_enum ("warpStyle", "warpNone"));
        d_set (&warp, "warpValue", d_double (0));
        d_set (&warp, "warpPerspective", d_double (0));
        d_set (&warp, "warpPerspectiveOther", d_double (0));
        d_set (&warp, "warpRotate", d_enum ("Ornt", "Hrzn"));
        DValue b = d_object ("classFloatRect");
        d_set (&b, "Top ", d_unit ("#Pxl", 0));
        d_set (&b, "Left", d_unit ("#Pxl", 0));
        d_set (&b, "Btom", d_unit ("#Pxl", h));
        d_set (&b, "Rght", d_unit ("#Pxl", w));
        d_set (&warp, "bounds", b);
        d_set (&warp, "uOrder", d_long (4));
        d_set (&warp, "vOrder", d_long (4));
        d_set (&d, "warp", warp);
        DValue sz = d_object ("Pnt ");
        d_set (&sz, "Wdth", d_double (w));
        d_set (&sz, "Hght", d_double (h));
        d_set (&d, "Sz  ", sz);
        d_set (&d, "Rslt", d_unit ("#Rsl", dpi));
        d_set (&d, "comp", d_long (-1));
        DValue comp = d_object ("null");
        d_set (&comp, "compID", d_long (-1));
        d_set (&comp, "originalCompID", d_long (-1));
        d_set (&d, "compInfo", comp);
        if (!s.filters.empty ())
        {
            DValue list = d_list ();
            for (size_t i = 0; i < s.filters.size (); i++)
            {
                const doc::SmartFilter& f = s.filters[i];
                DValue it                 = d_object ("null");
                d_set (&it, "Idnt", d_text (f.id));
                d_set (&it, "Nm  ", d_text (f.name));
                d_set (&it, "prms", d_text (f.params));
                d_set (&it, "enab", d_bool (f.enabled));
                d_set (&it, "Md  ", d_text (blend_key (f.blend)));
                d_set (&it, "Opct", d_double (f.opacity));
                list.items.push_back (it);
            }
            d_set (&d, "photocSmartFilters", list);
            d_set (&d, "photocFiltersOn", d_bool (s.filters_enabled));
        }
        std::vector<uint8_t> out;
        wr_key4 (&out, "soLD");
        wr_u32 (&out, 4);
        wr_u32 (&out, 16);
        write_descriptor (&out, d);
        return out;
    }

    /* One embedded file of an lnk2 block. */
    void write_linked_file (const std::string& uuid, const std::string& filename, const char type[4],
        const std::vector<uint8_t>& data, std::vector<uint8_t>* out)
    {
        std::vector<uint8_t> e;
        wr_key4 (&e, "liFD");
        wr_u32 (&e, 7);
        wr_pascal (&e, uuid, 1);
        wr_unicode (&e, filename, false);
        char t[5] = {type[0], type[1], type[2], type[3], 0};
        wr_key4 (&e, t);
        wr_key4 (&e, "8BIM");
        wr_u64 (&e, data.size ());
        wr_u8 (&e, 0);
        wr_bytes (&e, data.data (), data.size ());
        wr_u32 (&e, 0);
        wr_f64 (&e, 0.0);
        wr_u8 (&e, 0);
        wr_u64 (out, e.size ());
        wr_bytes (out, e.data (), e.size ());
        while (out->size () % 4 != 0)
        {
            wr_u8 (out, 0);
        }
    }

} // namespace psd
