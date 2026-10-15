#include "psd/vector.h"

#include "psd/bytes.h"
#include "psd/descriptor.h"
#include "psd/effects.h"
#include <math.h>
#include <string.h>

namespace psd
{

    /* Vector data in Photoshop files. Paths are runs of 26-byte records: a selector, then either a
       subpath length record (knot count and, since CS6, the subpath's operation) or a knot (in
       control, anchor, out control as vertical / horizontal 8.24 fixed fractions of the canvas
       height / width). Saved paths are image resources 2000-2997 (named by the resource name), the
       work path resource 1025; a vector mask (vmsk / vsms) is a version, flags (invert, unlinked,
       disabled) and the path. A shape layer is a solid colour fill layer (SoCo) with a vector mask
       and its stroke (vstk, with the stroke colour also in vscg). */

    const uint16_t SEL_CLOSED_LEN      = 0;
    const uint16_t SEL_CLOSED_LINKED   = 1;
    const uint16_t SEL_CLOSED_UNLINKED = 2;
    const uint16_t SEL_OPEN_LEN        = 3;
    const uint16_t SEL_OPEN_LINKED     = 4;
    const uint16_t SEL_OPEN_UNLINKED   = 5;
    const uint16_t SEL_FILL_RULE       = 6;
    const uint16_t SEL_CLIPBOARD       = 7;
    const uint16_t SEL_INITIAL_FILL    = 8;

    doc::PathOp op_from_psd (int16_t op)
    {
        switch (op)
        {
        case 0:
            return doc::PathOp::Exclude;
        case 2:
            return doc::PathOp::Subtract;
        case 3:
            return doc::PathOp::Intersect;
        default:
            return doc::PathOp::Combine;
        }
    }

    int16_t op_to_psd (doc::PathOp op)
    {
        switch (op)
        {
        case doc::PathOp::Exclude:
            return 0;
        case doc::PathOp::Subtract:
            return 2;
        case doc::PathOp::Intersect:
            return 3;
        default:
            return 1;
        }
    }

    void read_point (Reader* r, uint32_t width, uint32_t height, double out[2])
    {
        double v = rd_i32 (r) / 16777216.0, h = rd_i32 (r) / 16777216.0;
        out[0] = h * width;
        out[1] = v * height;
    }

    void write_point (std::vector<uint8_t>* w, const double p[2], uint32_t width, uint32_t height)
    {
        double v = height > 0 ? p[1] / height : 0.0, h = width > 0 ? p[0] / width : 0.0;
        v = fmin (fmax (v, -127.0), 127.0);
        h = fmin (fmax (h, -127.0), 127.0);
        wr_u32 (w, (uint32_t) (int32_t) llround (v * 16777216.0));
        wr_u32 (w, (uint32_t) (int32_t) llround (h * 16777216.0));
    }

    bool parse_path_records (const uint8_t* data, size_t len, uint32_t width, uint32_t height, doc::Path* out)
    {
        *out        = doc::Path::make ();
        Reader r    = reader (data, len);
        size_t left = 0;
        while (r.ok && remaining (r) >= 26)
        {
            uint16_t sel = rd_u16 (&r);
            size_t start = r.pos;
            if (sel == SEL_CLOSED_LEN || sel == SEL_OPEN_LEN)
            {
                doc::Subpath s;
                s.closed = sel == SEL_CLOSED_LEN;
                left     = rd_u16 (&r);
                s.op     = op_from_psd (rd_i16 (&r));
                out->subpaths.push_back (s);
            }
            else if (sel >= SEL_CLOSED_LINKED && sel <= SEL_OPEN_UNLINKED && sel != SEL_OPEN_LEN)
            {
                doc::Knot k;
                read_point (&r, width, height, k.in_ctrl);
                read_point (&r, width, height, k.anchor);
                read_point (&r, width, height, k.out_ctrl);
                k.smooth = sel == SEL_CLOSED_LINKED || sel == SEL_OPEN_LINKED;
                if (!out->subpaths.empty () && left > 0)
                {
                    out->subpaths.back ().knots.push_back (k);
                    left--;
                }
            }
            else if (sel == SEL_INITIAL_FILL)
            {
                out->inverted = rd_u16 (&r) != 0;
            }
            r.pos = start + 24;
            (void) SEL_FILL_RULE;
            (void) SEL_CLIPBOARD;
        }
        return r.ok;
    }

    void zeros (std::vector<uint8_t>* w, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            wr_u8 (w, 0);
        }
    }

    void write_path_records (const doc::Path& p, uint32_t width, uint32_t height, std::vector<uint8_t>* out)
    {
        wr_u16 (out, SEL_FILL_RULE);
        zeros (out, 24);
        wr_u16 (out, SEL_INITIAL_FILL);
        wr_u16 (out, p.inverted ? 1 : 0);
        zeros (out, 22);
        for (size_t i = 0; i < p.subpaths.size (); i++)
        {
            const doc::Subpath& s = p.subpaths[i];
            if (s.knots.empty ())
            {
                continue;
            }
            wr_u16 (out, s.closed ? SEL_CLOSED_LEN : SEL_OPEN_LEN);
            wr_u16 (out, (uint16_t) s.knots.size ());
            wr_u16 (out, (uint16_t) op_to_psd (i == 0 ? doc::PathOp::Combine : s.op));
            wr_u16 (out, 1);
            wr_u32 (out, 0);
            wr_u32 (out, (uint32_t) i);
            zeros (out, 10);
            for (size_t k = 0; k < s.knots.size (); k++)
            {
                const doc::Knot& n = s.knots[k];
                uint16_t sel       = s.closed ? (n.smooth ? SEL_CLOSED_LINKED : SEL_CLOSED_UNLINKED)
                                              : (n.smooth ? SEL_OPEN_LINKED : SEL_OPEN_UNLINKED);
                wr_u16 (out, sel);
                write_point (out, n.in_ctrl, width, height);
                write_point (out, n.anchor, width, height);
                write_point (out, n.out_ctrl, width, height);
            }
        }
    }

    bool parse_vector_mask (const uint8_t* data, size_t len, uint32_t width, uint32_t height, doc::VectorMask* out)
    {
        if (len < 8)
        {
            return false;
        }
        Reader r = reader (data, len);
        rd_u32 (&r);
        uint32_t flags = rd_u32 (&r);
        out->enabled   = (flags & 4) == 0;
        out->linked    = (flags & 2) == 0;
        out->density   = 1.0f;
        out->feather   = 0.0f;
        if (!parse_path_records (data + 8, len - 8, width, height, &out->path))
        {
            return false;
        }
        if (flags & 1)
        {
            out->path.inverted = true;
        }
        return true;
    }

    std::vector<uint8_t> write_vector_mask (const doc::VectorMask& m, uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> w;
        wr_u32 (&w, 3);
        wr_u32 (&w, (m.path.inverted ? 1u : 0u) | (m.linked ? 0u : 2u) | (m.enabled ? 0u : 4u));
        write_path_records (m.path, width, height, &w);
        return w;
    }

    /* SoCo: version, then a descriptor with the colour. */
    bool parse_solid_color (const uint8_t* data, size_t len, float out[3])
    {
        Reader r = reader (data, len);
        rd_u32 (&r);
        DValue d;
        if (!parse_descriptor (&r, &d))
        {
            return false;
        }
        const DValue* c = d_get (d, "Clr ");
        if (c == NULL)
        {
            return false;
        }
        color_from (c, out);
        return true;
    }

    std::vector<uint8_t> write_solid_color (const float c[3])
    {
        std::vector<uint8_t> w;
        wr_u32 (&w, 16);
        DValue d = d_object ("null");
        d_set (&d, "Clr ", color_value (c));
        write_descriptor (&w, d);
        return w;
    }

    const char* const CAP_IDS[3]   = {"strokeStyleButtCap", "strokeStyleRoundCap", "strokeStyleSquareCap"};
    const char* const JOIN_IDS[3]  = {"strokeStyleMiterJoin", "strokeStyleRoundJoin", "strokeStyleBevelJoin"};
    const char* const ALIGN_IDS[3] = {"strokeStyleAlignInside", "strokeStyleAlignCenter", "strokeStyleAlignOutside"};

    int enum_index (const std::string& v, const char* const* ids, int n, int fallback)
    {
        for (int i = 0; i < n; i++)
        {
            if (v == ids[i])
            {
                return i;
            }
        }
        return fallback;
    }

    /* vstk: version, then the strokeStyle descriptor. */
    bool parse_stroke_style (const uint8_t* data, size_t len, doc::ShapeStroke* out, bool* fill_enabled)
    {
        Reader r = reader (data, len);
        rd_u32 (&r);
        DValue d;
        if (!parse_descriptor (&r, &d))
        {
            return false;
        }
        out->enabled       = d_flag (d, "strokeEnabled", out->enabled);
        *fill_enabled      = d_flag (d, "fillEnabled", true);
        out->width         = (float) d_num (d, "strokeStyleLineWidth", out->width);
        out->miter_limit   = (float) d_num (d, "strokeStyleMiterLimit", out->miter_limit);
        out->opacity       = (float) (d_num (d, "strokeStyleOpacity", 100.0) / 100.0);
        out->cap           = (doc::LineCap) enum_index (d_enum_of (d, "strokeStyleLineCapType"), CAP_IDS, 3, 0);
        out->join          = (doc::LineJoin) enum_index (d_enum_of (d, "strokeStyleLineJoinType"), JOIN_IDS, 3, 0);
        out->align         = (doc::StrokeAlign) enum_index (d_enum_of (d, "strokeStyleLineAlignment"), ALIGN_IDS, 3, 0);
        const DValue* dash = d_get (d, "strokeStyleLineDashSet");
        out->dashes.clear ();
        if (dash != NULL && dash->type == DType::List)
        {
            for (size_t i = 0; i < dash->items.size () && i < 16; i++)
            {
                out->dashes.push_back ((float) dash->items[i].num);
            }
        }
        const DValue* content = d_get (d, "strokeStyleContent");
        if (content != NULL)
        {
            color_from (d_get (*content, "Clr "), out->color);
        }
        return true;
    }

    std::vector<uint8_t> write_stroke_style (const doc::ShapeStroke& s, bool fill_enabled)
    {
        DValue d = d_object ("strokeStyle");
        d_set (&d, "strokeStyleVersion", d_long (2));
        d_set (&d, "strokeEnabled", d_bool (s.enabled));
        d_set (&d, "fillEnabled", d_bool (fill_enabled));
        d_set (&d, "strokeStyleLineWidth", d_unit ("#Pxl", s.width));
        d_set (&d, "strokeStyleLineDashOffset", d_unit ("#Pnt", 0.0));
        d_set (&d, "strokeStyleMiterLimit", d_double (s.miter_limit));
        d_set (&d, "strokeStyleLineCapType", d_enum ("strokeStyleLineCapType", CAP_IDS[(int) s.cap]));
        d_set (&d, "strokeStyleLineJoinType", d_enum ("strokeStyleLineJoinType", JOIN_IDS[(int) s.join]));
        d_set (&d, "strokeStyleLineAlignment", d_enum ("strokeStyleLineAlignment", ALIGN_IDS[(int) s.align]));
        d_set (&d, "strokeStyleScaleLock", d_bool (false));
        d_set (&d, "strokeStyleStrokeAdjust", d_bool (false));
        DValue dash = d_list ();
        for (size_t i = 0; i < s.dashes.size (); i++)
        {
            dash.items.push_back (d_unit ("#Nne", s.dashes[i]));
        }
        d_set (&d, "strokeStyleLineDashSet", dash);
        d_set (&d, "strokeStyleBlendMode", d_enum ("BlnM", "Nrml"));
        d_set (&d, "strokeStyleOpacity", d_unit ("#Prc", s.opacity * 100.0));
        DValue content = d_object ("solidColorLayer");
        d_set (&content, "Clr ", color_value (s.color));
        d_set (&d, "strokeStyleContent", content);
        d_set (&d, "strokeStyleResolution", d_double (72.0));
        std::vector<uint8_t> w;
        wr_u32 (&w, 16);
        write_descriptor (&w, d);
        return w;
    }

} // namespace psd
