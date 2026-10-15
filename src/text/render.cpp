#include "text/render.h"

#include <math.h>
#include <string.h>
#include <vector>

#include FT_OUTLINE_H

namespace text
{

    /* Rendering a layout: every glyph's outline (FreeType, unhinted) mapped by one matrix that
       combines the glyph's scale, faux italic and the layer's transform, rasterized as coverage
       spans painted in the run's colour (source over, premultiplied) into a canvas covering the
       text; underlines and strikethroughs are filled the same way as quads. The anti-aliasing
       setting shapes the coverage: None thresholds it, Sharp / Crisp / Strong steepen or thicken
       it, Smooth keeps it. */

    struct Canvas
    {
        int32_t x0;
        int32_t y0;
        int32_t w;
        int32_t h;
        std::vector<float> px;
        float color[3];
        doc::AntiAlias aa;
    };

    float shape_coverage (float c, doc::AntiAlias aa)
    {
        switch (aa)
        {
        case doc::AntiAlias::None:
            return c >= 0.5f ? 1.0f : 0.0f;
        case doc::AntiAlias::Sharp:
            return fminf (fmaxf ((c - 0.5f) * 1.6f + 0.5f, 0.0f), 1.0f);
        case doc::AntiAlias::Crisp:
            return fminf (fmaxf ((c - 0.5f) * 1.25f + 0.5f, 0.0f), 1.0f);
        case doc::AntiAlias::Strong:
            return powf (c, 0.75f);
        default:
            return c;
        }
    }

    /* FreeType spans: y counts up, so canvas row = -y - 1. */
    void paint_spans (int y, int count, const FT_Span* spans, void* user)
    {
        Canvas* cv  = (Canvas*) user;
        int32_t row = -y - 1 - cv->y0;
        if (row < 0 || row >= cv->h)
        {
            return;
        }
        for (int i = 0; i < count; i++)
        {
            float c = shape_coverage (spans[i].coverage / 255.0f, cv->aa);
            if (c <= 0.0f)
            {
                continue;
            }
            for (int x = spans[i].x; x < spans[i].x + spans[i].len; x++)
            {
                int32_t col = x - cv->x0;
                if (col < 0 || col >= cv->w)
                {
                    continue;
                }
                float* p = &cv->px[((size_t) row * (size_t) cv->w + (size_t) col) * 4];
                for (int k = 0; k < 3; k++)
                {
                    p[k] = cv->color[k] * c + p[k] * (1.0f - c);
                }
                p[3] = c + p[3] * (1.0f - c);
            }
        }
    }

    /* Maps text-space point (tx, ty) through the layer transform into FreeType space (y up). */
    void to_ft (const double m[6], double tx, double ty, double* fx, double* fy)
    {
        *fx = m[0] * tx + m[2] * ty + m[4];
        *fy = -(m[1] * tx + m[3] * ty + m[5]);
    }

    void render_outline (FT_Library lib, FT_Outline* o, Canvas* cv)
    {
        FT_Raster_Params params;
        memset (&params, 0, sizeof params);
        params.flags         = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
        params.gray_spans    = paint_spans;
        params.user          = cv;
        params.clip_box.xMin = cv->x0;
        params.clip_box.xMax = cv->x0 + cv->w;
        params.clip_box.yMin = -(cv->y0 + cv->h);
        params.clip_box.yMax = -cv->y0;
        FT_Outline_Render (lib, o, &params);
    }

    /* The glyph's outline in FreeType space (26.6) for the layer transform; false when it has
       none. */
    bool glyph_outline (FontDb* db, const Glyph& g, const double m[6], FT_Outline* out, FT_Library lib)
    {
        FT_Face face = db->ft (g.face);
        if (face == NULL)
        {
            return false;
        }
        if (FT_Set_Char_Size (face, 0, (FT_F26Dot6) lroundf (g.size_px * 64.0f), 72, 72) != 0)
        {
            return false;
        }
        if (FT_Load_Glyph (face, g.id, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
        {
            return false;
        }
        if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
        {
            return false;
        }
        FT_Outline* src = &face->glyph->outline;
        if (FT_Outline_New (lib, (FT_UInt) src->n_points, src->n_contours, out) != 0)
        {
            return false;
        }
        FT_Outline_Copy (src, out);
        if (g.bold > 0.0f)
        {
            FT_Outline_Embolden (out, (FT_Pos) lroundf (g.bold * 64.0f));
        }
        /* Glyph (gx, gy up) -> text (x + hs gx + shear gy, y - vs gy) -> document -> FreeType. */
        double xx = m[0] * g.hscale, xy = m[0] * g.shear * g.vscale - m[2] * g.vscale;
        double yx = -m[1] * g.hscale, yy = -(m[1] * g.shear * g.vscale) + m[3] * g.vscale;
        FT_Matrix mat;
        mat.xx = (FT_Fixed) lround (xx * 65536.0);
        mat.xy = (FT_Fixed) lround (xy * 65536.0);
        mat.yx = (FT_Fixed) lround (yx * 65536.0);
        mat.yy = (FT_Fixed) lround (yy * 65536.0);
        FT_Outline_Transform (out, &mat);
        double ox, oy;
        to_ft (m, g.x, g.y, &ox, &oy);
        FT_Outline_Translate (out, (FT_Pos) lround (ox * 64.0), (FT_Pos) lround (oy * 64.0));
        return true;
    }

    /* A quad in text space (x0..x1, y0..y1) as an outline. */
    void quad_outline (const double m[6], float x0, float y0, float x1, float y1, FT_Outline* o, FT_Library lib)
    {
        FT_Outline_New (lib, 4, 1, o);
        float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
        for (int i = 0; i < 4; i++)
        {
            double fx, fy;
            to_ft (m, xs[i], ys[i], &fx, &fy);
            o->points[i].x = (FT_Pos) lround (fx * 64.0);
            o->points[i].y = (FT_Pos) lround (fy * 64.0);
            o->tags[i]     = FT_CURVE_TAG_ON;
        }
        o->contours[0] = 3;
        o->flags       = FT_OUTLINE_NONE;
    }

    raster::Surface render_text (const doc::TextLayer& t, const Layout& layout, FontDb* db, color::PixelFormat format,
        const color::CmykSpace* cmyk)
    {
        format.alpha = true;
        raster::Surface out (format);
        FT_Library lib;
        if (FT_Init_FreeType (&lib) != 0)
        {
            return out;
        }
        /* Outlines first, to size the canvas. */
        std::vector<FT_Outline> outlines;
        std::vector<const float*> colors;
        for (size_t i = 0; i < layout.glyphs.size (); i++)
        {
            FT_Outline o;
            if (!glyph_outline (db, layout.glyphs[i], t.transform, &o, lib))
            {
                continue;
            }
            outlines.push_back (o);
            colors.push_back (layout.glyphs[i].color);
        }
        for (size_t i = 0; i < layout.decos.size (); i++)
        {
            const Deco& d = layout.decos[i];
            FT_Outline o;
            quad_outline (t.transform, d.x0, d.y - d.thickness * 0.5f, d.x1, d.y + d.thickness * 0.5f, &o, lib);
            outlines.push_back (o);
            colors.push_back (d.color);
        }
        FT_BBox bb = {LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
        for (size_t i = 0; i < outlines.size (); i++)
        {
            FT_BBox b;
            FT_Outline_Get_CBox (&outlines[i], &b);
            if (b.xMin < bb.xMin)
            {
                bb.xMin = b.xMin;
            }
            if (b.yMin < bb.yMin)
            {
                bb.yMin = b.yMin;
            }
            if (b.xMax > bb.xMax)
            {
                bb.xMax = b.xMax;
            }
            if (b.yMax > bb.yMax)
            {
                bb.yMax = b.yMax;
            }
        }
        if (!outlines.empty () && bb.xMin < bb.xMax && bb.yMin < bb.yMax)
        {
            Canvas cv;
            cv.x0 = (int32_t) floor (bb.xMin / 64.0) - 1;
            cv.y0 = (int32_t) floor (-bb.yMax / 64.0) - 1;
            cv.w  = (int32_t) ceil (bb.xMax / 64.0) + 1 - cv.x0;
            cv.h  = (int32_t) ceil (-bb.yMin / 64.0) + 1 - cv.y0;
            cv.aa = t.antialias;
            if ((int64_t) cv.w * cv.h <= (int64_t) 1 << 28)
            {
                cv.px.assign ((size_t) cv.w * (size_t) cv.h * 4, 0.0f);
                for (size_t i = 0; i < outlines.size (); i++)
                {
                    memcpy (cv.color, colors[i], sizeof cv.color);
                    render_outline (lib, &outlines[i], &cv);
                }
                /* Into the surface's channels. */
                size_t n = format.channels (), count = (size_t) cv.w * (size_t) cv.h;
                std::vector<float> vals (count * n);
                for (size_t i = 0; i < count; i++)
                {
                    const float* p = &cv.px[i * 4];
                    /* The canvas holds premultiplied colour. */
                    float inv     = p[3] > 0.0f ? 1.0f / p[3] : 0.0f;
                    float rgba[4] = {p[0] * inv, p[1] * inv, p[2] * inv, p[3]};
                    if (p[3] > 0.0f)
                    {
                        raster::from_rgba (format, rgba, cmyk, &vals[i * n]);
                    }
                }
                geom::Rect r = geom::Rect::from_xywh (cv.x0, cv.y0, (uint32_t) cv.w, (uint32_t) cv.h);
                out.write_region (r, vals.data (), vals.size ());
                out.prune ();
            }
        }
        for (size_t i = 0; i < outlines.size (); i++)
        {
            FT_Outline_Done (lib, &outlines[i]);
        }
        FT_Done_FreeType (lib);
        return out;
    }

    bool render_layer_text (const doc::TextLayer& t, FontDb* db, float dpi, color::PixelFormat format,
        const color::CmykSpace* cmyk, raster::Surface* out)
    {
        Layout layout;
        if (!layout_text (t, db, dpi, &layout))
        {
            return false;
        }
        *out = render_text (t, layout, db, format, cmyk);
        return true;
    }

} // namespace text
