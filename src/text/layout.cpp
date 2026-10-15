#include "text/layout.h"

#include <algorithm>
#include <fribidi.h>
#include <hb.h>
#include <linebreak.h>
#include <math.h>
#include <string.h>
#include <wctype.h>

namespace text
{

    /* Type layout: a text layer's model to positioned glyphs in text space (pixels, y down; point
       text has its first baseline at y = 0 and aligns around x = 0, box text fills its box).

       Each paragraph is split into items of one character style, one font face (a face missing a
       character hands it to a fallback face) and one bidi level (FriBidi, Unicode UAX #9), shaped by
       HarfBuzz, then broken into lines at the opportunities of UAX #14 (libunibreak) where box text
       runs out of width, reordered for display by the bidi levels, and stacked with Photoshop's
       rules: the baseline distance is the largest leading on the line (auto leading = the
       paragraph's factor x the size), plus space before / after, indents, alignment and
       justification (extra width goes to the spaces). Tracking is 1/1000 em added after each
       character; horizontal scale widens advances, vertical scale only the glyphs. */

    const float FAUX_ITALIC_SHEAR = 0.21f;

    float pt_to_px (float pt, float dpi)
    {
        return pt * (dpi > 0.0f ? dpi : 72.0f) / 72.0f;
    }

    /* UTF-8 decoding: the code point at s[i] and the byte after it. */
    uint32_t utf8_next (const std::string& s, size_t i, size_t* next)
    {
        uint8_t c = (uint8_t) s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80)
        {
            cp = c, n = 1;
        }
        else if ((c >> 5) == 6)
        {
            cp = c & 0x1F, n = 2;
        }
        else if ((c >> 4) == 14)
        {
            cp = c & 0x0F, n = 3;
        }
        else if ((c >> 3) == 30)
        {
            cp = c & 0x07, n = 4;
        }
        else
        {
            cp = 0xFFFD, n = 1;
        }
        if (i + n > s.size ())
        {
            *next = s.size ();
            return 0xFFFD;
        }
        for (size_t k = 1; k < n; k++)
        {
            cp = cp << 6 | ((uint8_t) s[i + k] & 0x3F);
        }
        *next = i + n;
        return cp;
    }

    void utf8_put (std::string* out, uint32_t cp)
    {
        if (cp < 0x80)
        {
            out->push_back ((char) cp);
        }
        else if (cp < 0x800)
        {
            out->push_back ((char) (0xC0 | cp >> 6));
            out->push_back ((char) (0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out->push_back ((char) (0xE0 | cp >> 12));
            out->push_back ((char) (0x80 | ((cp >> 6) & 0x3F)));
            out->push_back ((char) (0x80 | (cp & 0x3F)));
        }
        else
        {
            out->push_back ((char) (0xF0 | cp >> 18));
            out->push_back ((char) (0x80 | ((cp >> 12) & 0x3F)));
            out->push_back ((char) (0x80 | ((cp >> 6) & 0x3F)));
            out->push_back ((char) (0x80 | (cp & 0x3F)));
        }
    }

    bool is_space (uint32_t cp)
    {
        return cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x3000;
    }

    struct Shaped
    {
        uint32_t id;
        size_t ci;
        float adv;
        float dx;
        float dy;
    };

    struct Item
    {
        size_t a;
        size_t b;
        int face;
        int level;
        size_t run;
        std::vector<Shaped> glyphs;
    };

    /* One paragraph's characters: code points, byte offsets (relative to the paragraph), style
       runs, faces, bidi levels and the widths shaping gives them. */
    struct Para
    {
        std::string text;
        std::string shaped_text;
        std::vector<uint32_t> cps;
        std::vector<size_t> offs;
        std::vector<size_t> run;
        std::vector<int> face;
        std::vector<FriBidiLevel> level;
        std::vector<float> width;
        std::vector<Item> items;
        std::vector<char> breaks;
    };

    struct RunInfo
    {
        doc::CharStyle style;
        float size_px;
        float leading_px;
        float tracking_px;
        float shift_px;
    };

    /* Font metrics of a face at a size: ascent and descent (positive down), underline position and
       thickness. */
    void face_metrics (FontDb* db, int face, float size_px, float* asc, float* desc, float* ul_pos, float* ul_thick)
    {
        FT_Face f = db->ft (face);
        if (f == NULL || f->units_per_EM == 0)
        {
            *asc      = size_px * 0.8f;
            *desc     = size_px * 0.2f;
            *ul_pos   = size_px * 0.1f;
            *ul_thick = size_px * 0.05f;
            return;
        }
        float k   = size_px / (float) f->units_per_EM;
        *asc      = (float) f->ascender * k;
        *desc     = -(float) f->descender * k;
        *ul_pos   = -(float) f->underline_position * k;
        *ul_thick = fmaxf ((float) f->underline_thickness * k, 1.0f);
    }

    void shape_item (FontDb* db, Para* p, Item* it, const RunInfo& ri)
    {
        hb_font_t* font = db->hb (it->face);
        if (font == NULL)
        {
            return;
        }
        int scale = (int) lroundf (ri.size_px * 64.0f);
        hb_font_set_scale (font, scale, scale);
        hb_buffer_t* buf = hb_buffer_create ();
        size_t ba = p->offs[it->a], bb = p->offs[it->b];
        hb_buffer_add_utf8 (buf, p->shaped_text.data (), (int) p->shaped_text.size (), (unsigned) ba, (int) (bb - ba));
        hb_buffer_guess_segment_properties (buf);
        hb_buffer_set_direction (buf, (it->level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_feature_t feats[3];
        unsigned nf = 0;
        if (!ri.style.kerning)
        {
            hb_feature_from_string ("kern=0", -1, &feats[nf++]);
        }
        if (!ri.style.ligatures)
        {
            hb_feature_from_string ("liga=0", -1, &feats[nf++]);
            hb_feature_from_string ("clig=0", -1, &feats[nf++]);
        }
        hb_shape (font, buf, feats, nf);
        unsigned n               = 0;
        hb_glyph_info_t* info    = hb_buffer_get_glyph_infos (buf, &n);
        hb_glyph_position_t* pos = hb_buffer_get_glyph_positions (buf, &n);
        float hs                 = ri.style.hscale > 0.0f ? ri.style.hscale : 1.0f;
        uint32_t prev_cluster    = UINT32_MAX;
        for (unsigned i = 0; i < n; i++)
        {
            Shaped g;
            g.id = info[i].codepoint;
            /* The character the glyph's cluster starts at. */
            size_t ci =
                std::lower_bound (p->offs.begin (), p->offs.end (), (size_t) info[i].cluster) - p->offs.begin ();
            if (ci >= p->cps.size ())
            {
                ci = p->cps.size () - 1;
            }
            g.ci  = ci;
            g.adv = (float) pos[i].x_advance / 64.0f * hs;
            g.dx  = (float) pos[i].x_offset / 64.0f * hs;
            g.dy  = (float) pos[i].y_offset / 64.0f;
            /* Tracking once per cluster. */
            if (info[i].cluster != prev_cluster)
            {
                g.adv += ri.tracking_px;
            }
            prev_cluster = info[i].cluster;
            it->glyphs.push_back (g);
            p->width[ci] += g.adv;
        }
        hb_buffer_destroy (buf);
    }

    /* Visual order of the line's items (rule L2: from the highest level down to the lowest odd
       one, reverse every sequence at that level or above). */
    std::vector<size_t> visual_order (const std::vector<int>& levels)
    {
        std::vector<size_t> order (levels.size ());
        for (size_t i = 0; i < order.size (); i++)
        {
            order[i] = i;
        }
        int hi = 0, lo_odd = 255;
        for (size_t i = 0; i < levels.size (); i++)
        {
            hi = std::max (hi, levels[i]);
            if (levels[i] & 1)
            {
                lo_odd = std::min (lo_odd, levels[i]);
            }
        }
        for (int k = hi; k >= lo_odd && k > 0; k--)
        {
            size_t i = 0;
            while (i < order.size ())
            {
                if (levels[order[i]] < k)
                {
                    i++;
                    continue;
                }
                size_t j = i;
                while (j < order.size () && levels[order[j]] >= k)
                {
                    j++;
                }
                std::reverse (order.begin () + (ptrdiff_t) i, order.begin () + (ptrdiff_t) j);
                i = j;
            }
        }
        return order;
    }

    bool layout_text (const doc::TextLayer& t, FontDb* db, float dpi, Layout* out)
    {
        out->glyphs.clear ();
        out->lines.clear ();
        out->decos.clear ();
        init_linebreak ();
        std::vector<doc::TextRun> runs  = t.char_runs ();
        std::vector<doc::ParaRun> paras = t.para_runs ();
        std::vector<RunInfo> infos (runs.size ());
        std::vector<size_t> run_start (runs.size ());
        size_t acc = 0;
        for (size_t i = 0; i < runs.size (); i++)
        {
            RunInfo& ri   = infos[i];
            ri.style      = runs[i].style;
            ri.size_px    = pt_to_px (fmaxf (ri.style.size_pt, 0.01f), dpi);
            ri.leading_px = ri.style.leading_pt > 0.0f ? pt_to_px (ri.style.leading_pt, dpi) : 0.0f;
            ri.tracking_px =
                ri.style.tracking / 1000.0f * ri.size_px * (ri.style.hscale > 0.0f ? ri.style.hscale : 1.0f);
            ri.shift_px  = pt_to_px (ri.style.baseline_shift_pt, dpi);
            run_start[i] = acc;
            acc += runs[i].len;
        }
        float box_x = t.box_rect[0], box_y = t.box_rect[1], box_w = t.box_rect[2];
        float baseline      = 0.0f;
        bool first_line     = true;
        float pending_after = 0.0f;
        size_t para_start   = 0;
        for (size_t pi = 0; pi < paras.size (); pi++)
        {
            const doc::ParaStyle& ps = paras[pi].style;
            size_t plen              = paras[pi].len;
            bool has_nl = plen > 0 && para_start + plen <= t.text.size () && t.text[para_start + plen - 1] == '\n';
            Para p;
            p.text = t.text.substr (para_start, has_nl ? plen - 1 : plen);
            /* Code points and offsets; all caps shapes an upper-cased copy (same byte lengths). */
            for (size_t i = 0; i < p.text.size ();)
            {
                size_t next;
                uint32_t cp = utf8_next (p.text, i, &next);
                p.cps.push_back (cp);
                p.offs.push_back (i);
                i = next;
            }
            size_t n = p.cps.size ();
            p.offs.push_back (p.text.size ());
            p.shaped_text = p.text;
            p.run.resize (n);
            p.face.resize (n);
            p.level.assign (n, 0);
            p.width.assign (n, 0.0f);
            size_t r = 0;
            for (size_t i = 0; i < n; i++)
            {
                size_t abs = para_start + p.offs[i];
                while (r + 1 < runs.size () && abs >= run_start[r] + runs[r].len)
                {
                    r++;
                }
                p.run[i] = r;
            }
            std::string caps;
            bool any_caps = false;
            for (size_t i = 0; i < n; i++)
            {
                uint32_t cp = p.cps[i];
                if (infos[p.run[i]].style.all_caps)
                {
                    uint32_t up = (uint32_t) towupper ((wint_t) cp);
                    std::string a, b;
                    utf8_put (&a, cp);
                    utf8_put (&b, up);
                    if (a.size () == b.size ())
                    {
                        cp = up;
                    }
                    any_caps = true;
                }
                utf8_put (&caps, cp);
            }
            if (any_caps)
            {
                p.shaped_text = caps;
            }
            /* Faces, with fallback for missing characters. */
            for (size_t i = 0; i < n; i++)
            {
                const doc::CharStyle& st = infos[p.run[i]].style;
                int base                 = db->resolve (st.family, st.style);
                int f                    = base;
                uint32_t cp              = p.cps[i];
                if (base >= 0 && cp >= 0x20 && !is_space (cp) && !db->has_glyph (base, cp))
                {
                    f = db->fallback_for (base, cp);
                }
                p.face[i] = f;
            }
            /* Bidi levels. */
            if (n > 0)
            {
                std::vector<FriBidiCharType> types (n);
                std::vector<FriBidiBracketType> brackets (n);
                std::vector<FriBidiChar> chars (p.cps.begin (), p.cps.end ());
                fribidi_get_bidi_types (chars.data (), (FriBidiStrIndex) n, types.data ());
                fribidi_get_bracket_types (chars.data (), (FriBidiStrIndex) n, types.data (), brackets.data ());
                FriBidiParType dir = FRIBIDI_PAR_ON;
                if (!fribidi_get_par_embedding_levels_ex (
                        types.data (), brackets.data (), (FriBidiStrIndex) n, &dir, p.level.data ()))
                {
                    p.level.assign (n, 0);
                }
            }
            /* Items, shaped. */
            for (size_t i = 0; i < n;)
            {
                size_t j = i + 1;
                while (j < n && p.run[j] == p.run[i] && p.face[j] == p.face[i] && p.level[j] == p.level[i])
                {
                    j++;
                }
                Item it;
                it.a     = i;
                it.b     = j;
                it.face  = p.face[i];
                it.level = p.level[i];
                it.run   = p.run[i];
                p.items.push_back (it);
                i = j;
            }
            for (size_t k = 0; k < p.items.size (); k++)
            {
                shape_item (db, &p, &p.items[k], infos[p.items[k].run]);
            }
            /* Break opportunities after each character. */
            p.breaks.assign (p.text.size () + 1, LINEBREAK_NOBREAK);
            if (!p.text.empty ())
            {
                set_linebreaks_utf8 ((const utf8_t*) p.text.data (), p.text.size (), NULL, p.breaks.data ());
            }

            /* Lines: [a, b) character ranges. */
            std::vector<std::pair<size_t, size_t>> lines;
            float indent_start = pt_to_px (ps.start_indent_pt, dpi), indent_end = pt_to_px (ps.end_indent_pt, dpi),
                  indent_first = pt_to_px (ps.first_indent_pt, dpi);
            if (!t.box || n == 0)
            {
                lines.push_back (std::make_pair ((size_t) 0, n));
            }
            else
            {
                size_t a = 0;
                while (a < n)
                {
                    float avail       = box_w - indent_start - indent_end - (a == 0 ? indent_first : 0.0f);
                    float w           = 0.0f;
                    size_t last_break = SIZE_MAX, b = a;
                    while (b < n)
                    {
                        float wb = w + p.width[b];
                        /* Spaces may hang past the edge. */
                        if (wb > avail && !is_space (p.cps[b]) && b > a)
                        {
                            break;
                        }
                        w           = wb;
                        size_t endb = p.offs[b + 1] - 1;
                        if (p.breaks[endb] == LINEBREAK_ALLOWBREAK)
                        {
                            last_break = b + 1;
                        }
                        b++;
                    }
                    if (b < n && last_break != SIZE_MAX && last_break > a)
                    {
                        b = last_break;
                    }
                    lines.push_back (std::make_pair (a, b));
                    a = b;
                }
            }
            /* Place the lines. */
            for (size_t li = 0; li < lines.size (); li++)
            {
                size_t a = lines[li].first, b = lines[li].second;
                bool last = li + 1 == lines.size ();
                /* Metrics: the largest leading, ascent and descent of the line's characters (the
                   style at the paragraph's position for an empty line). */
                float lead = 0.0f, asc = 0.0f, desc = 0.0f;
                size_t mr = n > 0 ? p.run[std::min (a, n - 1)] : 0;
                if (n == 0)
                {
                    size_t abs = para_start;
                    for (size_t k = 0; k < runs.size (); k++)
                    {
                        if (abs >= run_start[k] && (abs < run_start[k] + runs[k].len || k + 1 == runs.size ()))
                        {
                            mr = k;
                        }
                    }
                }
                for (size_t c = a; c <= b && (c < b || a == b); c++)
                {
                    size_t rr         = n > 0 && c < n ? p.run[c] : mr;
                    const RunInfo& ri = infos[rr];
                    float l           = ri.leading_px > 0.0f ? ri.leading_px : ps.auto_leading * ri.size_px;
                    float fa, fd, up, ut;
                    face_metrics (db, n > 0 && c < n ? p.face[c] : db->resolve (ri.style.family, ri.style.style),
                        ri.size_px, &fa, &fd, &up, &ut);
                    lead = fmaxf (lead, l);
                    asc  = fmaxf (asc, fa);
                    desc = fmaxf (desc, fd);
                    if (a == b)
                    {
                        break;
                    }
                }
                if (first_line)
                {
                    baseline   = t.box ? box_y + asc : 0.0f;
                    first_line = false;
                }
                else
                {
                    baseline += lead + (li == 0 ? pending_after + pt_to_px (ps.space_before_pt, dpi) : 0.0f);
                }
                /* Visual fragments: the items' glyphs inside [a, b), in display order. */
                std::vector<size_t> frag_items;
                std::vector<int> frag_levels;
                for (size_t k = 0; k < p.items.size (); k++)
                {
                    if (p.items[k].b <= a || p.items[k].a >= b)
                    {
                        continue;
                    }
                    frag_items.push_back (k);
                    frag_levels.push_back (p.items[k].level);
                }
                std::vector<size_t> order = visual_order (frag_levels);
                /* Trailing spaces don't count for alignment. */
                size_t content_end = b;
                while (content_end > a && is_space (p.cps[content_end - 1]))
                {
                    content_end--;
                }
                float w       = 0.0f;
                size_t spaces = 0;
                for (size_t c = a; c < content_end; c++)
                {
                    w += p.width[c];
                    if (p.cps[c] == ' ')
                    {
                        spaces++;
                    }
                }
                float first = li == 0 ? indent_first : 0.0f;
                float x0;
                float extra       = 0.0f;
                doc::TextAlign al = ps.align;
                bool justify      = al == doc::TextAlign::JustifyAll
                            || (!last
                                && (al == doc::TextAlign::JustifyLeft || al == doc::TextAlign::JustifyCenter
                                    || al == doc::TextAlign::JustifyRight));
                if (last && al == doc::TextAlign::JustifyCenter)
                {
                    al = doc::TextAlign::Center;
                }
                else if (last && al == doc::TextAlign::JustifyRight)
                {
                    al = doc::TextAlign::Right;
                }
                else if (last && al == doc::TextAlign::JustifyLeft)
                {
                    al = doc::TextAlign::Left;
                }
                if (t.box)
                {
                    float area0 = box_x + indent_start + first, area1 = box_x + box_w - indent_end;
                    float avail = area1 - area0;
                    if (justify && t.box && spaces > 0 && avail > w)
                    {
                        extra = (avail - w) / (float) spaces;
                        x0    = area0;
                    }
                    else if (al == doc::TextAlign::Center)
                    {
                        x0 = area0 + (avail - w) * 0.5f;
                    }
                    else if (al == doc::TextAlign::Right)
                    {
                        x0 = area1 - w;
                    }
                    else
                    {
                        x0 = area0;
                    }
                }
                else if (al == doc::TextAlign::Center || al == doc::TextAlign::JustifyCenter)
                {
                    x0 = -w * 0.5f;
                }
                else if (al == doc::TextAlign::Right || al == doc::TextAlign::JustifyRight)
                {
                    x0 = -w - indent_end;
                }
                else
                {
                    x0 = indent_start + first;
                }
                Line line;
                line.baseline = baseline;
                line.ascent   = asc;
                line.descent  = desc;
                line.start    = para_start + p.offs[a];
                line.end      = para_start + p.offs[b];
                /* Glyphs; character spans for the caret. */
                std::vector<float> cl (n, INFINITY), cr (n, -INFINITY);
                float pen = x0;
                for (size_t oi = 0; oi < order.size (); oi++)
                {
                    const Item& it    = p.items[frag_items[order[oi]]];
                    const RunInfo& ri = infos[it.run];
                    for (size_t g = 0; g < it.glyphs.size (); g++)
                    {
                        const Shaped& sg = it.glyphs[g];
                        if (sg.ci < a || sg.ci >= b)
                        {
                            continue;
                        }
                        float adv = sg.adv + (p.cps[sg.ci] == ' ' && sg.ci < content_end ? extra : 0.0f);
                        Glyph gl;
                        gl.face    = it.face;
                        gl.id      = sg.id;
                        gl.x       = pen + sg.dx;
                        gl.y       = baseline - sg.dy - ri.shift_px;
                        gl.size_px = ri.size_px;
                        gl.hscale  = ri.style.hscale > 0.0f ? ri.style.hscale : 1.0f;
                        gl.vscale  = ri.style.vscale > 0.0f ? ri.style.vscale : 1.0f;
                        gl.shear   = ri.style.faux_italic ? FAUX_ITALIC_SHEAR : 0.0f;
                        gl.bold    = ri.style.faux_bold ? ri.size_px / 24.0f : 0.0f;
                        memcpy (gl.color, ri.style.color, sizeof gl.color);
                        if (!is_space (p.cps[sg.ci]))
                        {
                            out->glyphs.push_back (gl);
                        }
                        cl[sg.ci] = fminf (cl[sg.ci], pen);
                        cr[sg.ci] = fmaxf (cr[sg.ci], pen + adv);
                        /* Underline and strikethrough, merged along the line. */
                        for (int d = 0; d < 2; d++)
                        {
                            if (!(d == 0 ? ri.style.underline : ri.style.strikethrough) || sg.ci >= content_end)
                            {
                                continue;
                            }
                            float fa, fd, up, ut;
                            face_metrics (db, it.face, ri.size_px, &fa, &fd, &up, &ut);
                            Deco dc;
                            dc.x0        = pen;
                            dc.x1        = pen + adv;
                            dc.y         = d == 0 ? baseline + up - ri.shift_px : baseline - fa * 0.3f - ri.shift_px;
                            dc.thickness = ut;
                            memcpy (dc.color, ri.style.color, sizeof dc.color);
                            Deco* prev = out->decos.empty () ? NULL : &out->decos.back ();
                            if (prev != NULL && fabsf (prev->x1 - dc.x0) < 0.01f && prev->y == dc.y
                                && memcmp (prev->color, dc.color, sizeof dc.color) == 0)
                            {
                                prev->x1 = dc.x1;
                            }
                            else
                            {
                                out->decos.push_back (dc);
                            }
                        }
                        pen += adv;
                    }
                }
                /* Characters inside a ligature share its width. */
                for (size_t c = a; c < b;)
                {
                    if (cl[c] == INFINITY)
                    {
                        c++;
                        continue;
                    }
                    size_t e = c + 1;
                    while (e < b && cl[e] == INFINITY)
                    {
                        e++;
                    }
                    float w0 = cl[c], w1 = cr[c];
                    for (size_t k = c; k < e; k++)
                    {
                        cl[k] = w0 + (w1 - w0) * (float) (k - c) / (float) (e - c);
                        cr[k] = w0 + (w1 - w0) * (float) (k - c + 1) / (float) (e - c);
                    }
                    c = e;
                }
                for (size_t c = a; c < b; c++)
                {
                    if (cl[c] == INFINITY)
                    {
                        cl[c] = cr[c] = pen;
                    }
                    Caret ct = {para_start + p.offs[c], (p.level[c] & 1) ? cr[c] : cl[c]};
                    line.carets.push_back (ct);
                }
                if (last || a == b)
                {
                    float ex = x0;
                    if (b > a)
                    {
                        ex = (p.level[b - 1] & 1) ? cl[b - 1] : cr[b - 1];
                    }
                    Caret ct = {para_start + p.offs[b], ex};
                    line.carets.push_back (ct);
                }
                line.left  = x0;
                line.right = x0 + w;
                out->lines.push_back (line);
            }
            pending_after = pt_to_px (ps.space_after_pt, dpi);
            para_start += plen;
            (void) has_nl;
        }
        /* Bounds of the lines' boxes. */
        out->bounds[0] = out->bounds[1] = INFINITY;
        out->bounds[2] = out->bounds[3] = -INFINITY;
        for (size_t i = 0; i < out->lines.size (); i++)
        {
            const Line& l  = out->lines[i];
            out->bounds[0] = fminf (out->bounds[0], l.left);
            out->bounds[1] = fminf (out->bounds[1], l.baseline - l.ascent);
            out->bounds[2] = fmaxf (out->bounds[2], fmaxf (l.right, l.left));
            out->bounds[3] = fmaxf (out->bounds[3], l.baseline + l.descent);
        }
        if (out->lines.empty ())
        {
            out->bounds[0] = out->bounds[1] = out->bounds[2] = out->bounds[3] = 0.0f;
        }
        return true;
    }

    /* The line holding caret position pos (a position between lines belongs to the later one). */
    size_t line_of (const Layout& l, size_t pos)
    {
        for (size_t i = 0; i < l.lines.size (); i++)
        {
            const Line& ln = l.lines[i];
            bool is_last   = i + 1 == l.lines.size ();
            if (pos >= ln.start && (pos < ln.end || (pos == ln.end && (is_last || l.lines[i + 1].start != pos))))
            {
                return i;
            }
        }
        return l.lines.empty () ? 0 : l.lines.size () - 1;
    }

    void caret_at (const Layout& l, size_t pos, float* x, float* top, float* bottom)
    {
        *x      = 0.0f;
        *top    = 0.0f;
        *bottom = 0.0f;
        if (l.lines.empty ())
        {
            return;
        }
        const Line& ln = l.lines[line_of (l, pos)];
        *top           = ln.baseline - ln.ascent;
        *bottom        = ln.baseline + ln.descent;
        *x             = ln.left;
        float best     = INFINITY;
        for (size_t i = 0; i < ln.carets.size (); i++)
        {
            float d = fabsf ((float) ln.carets[i].pos - (float) pos);
            if (d < best)
            {
                best = d;
                *x   = ln.carets[i].x;
            }
        }
    }

    /* The caret position nearest to (x, y) in text space. */
    size_t hit_test (const Layout& l, float x, float y)
    {
        if (l.lines.empty ())
        {
            return 0;
        }
        size_t li  = 0;
        float best = INFINITY;
        for (size_t i = 0; i < l.lines.size (); i++)
        {
            const Line& ln = l.lines[i];
            float top = ln.baseline - ln.ascent, bot = ln.baseline + ln.descent;
            float d = y < top ? top - y : y > bot ? y - bot : 0.0f;
            if (d < best)
            {
                best = d;
                li   = i;
            }
        }
        const Line& ln = l.lines[li];
        size_t pos     = ln.start;
        best           = INFINITY;
        for (size_t i = 0; i < ln.carets.size (); i++)
        {
            float d = fabsf (ln.carets[i].x - x);
            if (d < best)
            {
                best = d;
                pos  = ln.carets[i].pos;
            }
        }
        return pos;
    }

    /* Up (dir < 0) or down a line, keeping the x. */
    size_t caret_move_vertical (const Layout& l, size_t pos, int dir)
    {
        if (l.lines.empty ())
        {
            return pos;
        }
        size_t li = line_of (l, pos);
        float x, top, bot;
        caret_at (l, pos, &x, &top, &bot);
        if (dir < 0 && li == 0)
        {
            return l.lines[0].start;
        }
        if (dir > 0 && li + 1 >= l.lines.size ())
        {
            return l.lines.back ().carets.empty () ? pos : l.lines.back ().carets.back ().pos;
        }
        const Line& ln = l.lines[dir < 0 ? li - 1 : li + 1];
        return hit_test (l, x, ln.baseline);
    }

    size_t line_home (const Layout& l, size_t pos)
    {
        if (l.lines.empty ())
        {
            return pos;
        }
        return l.lines[line_of (l, pos)].start;
    }

    size_t line_end (const Layout& l, size_t pos)
    {
        if (l.lines.empty ())
        {
            return pos;
        }
        const Line& ln = l.lines[line_of (l, pos)];
        size_t e       = ln.start;
        for (size_t i = 0; i < ln.carets.size (); i++)
        {
            e = std::max (e, ln.carets[i].pos);
        }
        return e;
    }

} // namespace text
