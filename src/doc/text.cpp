#include "doc/text.h"

#include <string.h>

namespace doc
{

    /* Type layers: the text (UTF-8, paragraphs separated by '\n'), character style runs covering it
       by byte length, one paragraph style per paragraph, point or box shape and the transform from
       text space (pixels, y down, the first baseline of point text at y = 0) to the document.
       Sizes, leading, indents and spacing are in points (px = pt x dpi / 72), tracking in 1/1000 em,
       as in Photoshop's Character and Paragraph panels. */

    CharStyle CharStyle::make ()
    {
        CharStyle s;
        s.family     = "";
        s.style      = "Regular";
        s.postscript = "";
        s.size_pt    = 12.0f;
        s.color[0] = s.color[1] = s.color[2] = 0.0f;
        s.tracking                           = 0.0f;
        s.leading_pt                         = 0.0f;
        s.baseline_shift_pt                  = 0.0f;
        s.hscale                             = 1.0f;
        s.vscale                             = 1.0f;
        s.underline                          = false;
        s.strikethrough                      = false;
        s.faux_bold                          = false;
        s.faux_italic                        = false;
        s.all_caps                           = false;
        s.kerning                            = true;
        s.ligatures                          = true;
        return s;
    }

    ParaStyle ParaStyle::make ()
    {
        ParaStyle p;
        p.align           = TextAlign::Left;
        p.first_indent_pt = 0.0f;
        p.start_indent_pt = 0.0f;
        p.end_indent_pt   = 0.0f;
        p.space_before_pt = 0.0f;
        p.space_after_pt  = 0.0f;
        p.auto_leading    = 1.2f;
        return p;
    }

    TextLayer TextLayer::make ()
    {
        TextLayer t;
        TextRun r = {0, CharStyle::make ()};
        t.runs.push_back (r);
        ParaRun p = {0, ParaStyle::make ()};
        t.paras.push_back (p);
        t.box         = false;
        t.box_rect[0] = t.box_rect[1] = t.box_rect[2] = t.box_rect[3] = 0.0f;
        t.transform[0]                                                = 1.0;
        t.transform[1]                                                = 0.0;
        t.transform[2]                                                = 0.0;
        t.transform[3]                                                = 1.0;
        t.transform[4]                                                = 0.0;
        t.transform[5]                                                = 0.0;
        t.antialias                                                   = AntiAlias::Smooth;
        return t;
    }

    CharStyle TextLayer::first_style () const
    {
        return runs.empty () ? CharStyle::make () : runs[0].style;
    }

    /* The runs covering the text exactly: the last run stretched or cut, empty runs dropped (an
       empty text keeps its first run's style at length 0). */
    std::vector<TextRun> TextLayer::char_runs () const
    {
        std::vector<TextRun> out;
        size_t at = 0;
        for (size_t i = 0; i < runs.size () && at < text.size (); i++)
        {
            size_t len = runs[i].len;
            if (len == 0)
            {
                continue;
            }
            if (at + len > text.size ())
            {
                len = text.size () - at;
            }
            TextRun r = {len, runs[i].style};
            out.push_back (r);
            at += len;
        }
        if (out.empty ())
        {
            TextRun r = {text.size (), first_style ()};
            out.push_back (r);
            return out;
        }
        if (at < text.size ())
        {
            out.back ().len += text.size () - at;
        }
        return out;
    }

    /* One run per paragraph (each covering its '\n'), styles from `paras` in order (the last one
       repeated when there are more paragraphs). */
    std::vector<ParaRun> TextLayer::para_runs () const
    {
        std::vector<ParaRun> out;
        size_t start = 0, k = 0;
        for (;;)
        {
            size_t nl  = text.find ('\n', start);
            size_t end = nl == std::string::npos ? text.size () : nl + 1;
            ParaRun r  = {end - start,
                paras.empty () ? ParaStyle::make () : paras[k < paras.size () ? k : paras.size () - 1].style};
            out.push_back (r);
            k++;
            if (nl == std::string::npos)
            {
                break;
            }
            start = end;
        }
        return out;
    }

    bool same_char_style (const CharStyle& a, const CharStyle& b)
    {
        return a.family == b.family && a.style == b.style && a.postscript == b.postscript && a.size_pt == b.size_pt
            && a.color[0] == b.color[0] && a.color[1] == b.color[1] && a.color[2] == b.color[2]
            && a.tracking == b.tracking && a.leading_pt == b.leading_pt && a.baseline_shift_pt == b.baseline_shift_pt
            && a.hscale == b.hscale && a.vscale == b.vscale && a.underline == b.underline
            && a.strikethrough == b.strikethrough && a.faux_bold == b.faux_bold && a.faux_italic == b.faux_italic
            && a.all_caps == b.all_caps && a.kerning == b.kerning && a.ligatures == b.ligatures;
    }

    bool same_para_style (const ParaStyle& a, const ParaStyle& b)
    {
        return a.align == b.align && a.first_indent_pt == b.first_indent_pt && a.start_indent_pt == b.start_indent_pt
            && a.end_indent_pt == b.end_indent_pt && a.space_before_pt == b.space_before_pt
            && a.space_after_pt == b.space_after_pt && a.auto_leading == b.auto_leading;
    }

    bool same_text (const TextLayer& a, const TextLayer& b)
    {
        if (a.text != b.text || a.box != b.box || a.antialias != b.antialias)
        {
            return false;
        }
        for (int i = 0; i < 4; i++)
        {
            if (a.box_rect[i] != b.box_rect[i])
            {
                return false;
            }
        }
        for (int i = 0; i < 6; i++)
        {
            if (a.transform[i] != b.transform[i])
            {
                return false;
            }
        }
        std::vector<TextRun> ra = a.char_runs (), rb = b.char_runs ();
        if (ra.size () != rb.size ())
        {
            return false;
        }
        for (size_t i = 0; i < ra.size (); i++)
        {
            if (ra[i].len != rb[i].len || !same_char_style (ra[i].style, rb[i].style))
            {
                return false;
            }
        }
        std::vector<ParaRun> pa = a.para_runs (), pb = b.para_runs ();
        if (pa.size () != pb.size ())
        {
            return false;
        }
        for (size_t i = 0; i < pa.size (); i++)
        {
            if (!same_para_style (pa[i].style, pb[i].style))
            {
                return false;
            }
        }
        return true;
    }

    /* Merges neighbouring runs of the same style. */
    void merge_runs (std::vector<TextRun>* runs)
    {
        std::vector<TextRun> out;
        for (size_t i = 0; i < runs->size (); i++)
        {
            if (!out.empty () && same_char_style (out.back ().style, (*runs)[i].style))
            {
                out.back ().len += (*runs)[i].len;
            }
            else if ((*runs)[i].len > 0 || runs->size () == 1)
            {
                out.push_back ((*runs)[i]);
            }
        }
        if (out.empty () && !runs->empty ())
        {
            out.push_back ((*runs)[0]);
        }
        *runs = out;
    }

    CharStyle text_style_at (const TextLayer& t, size_t pos)
    {
        std::vector<TextRun> runs = t.char_runs ();
        /* The style of the character before pos (typing continues it), else the first. */
        size_t at = 0;
        for (size_t i = 0; i < runs.size (); i++)
        {
            if (pos > at && pos <= at + runs[i].len)
            {
                return runs[i].style;
            }
            at += runs[i].len;
        }
        return runs[0].style;
    }

    size_t count_newlines (const std::string& s, size_t start, size_t end)
    {
        size_t n = 0;
        for (size_t i = start; i < end && i < s.size (); i++)
        {
            n += s[i] == '\n';
        }
        return n;
    }

    /* Replaces bytes [start, end) with `insert`, which takes the style at start. Paragraphs merged
       by the deletion keep the first one's style; new paragraphs take it too. */
    void text_replace (TextLayer* t, size_t start, size_t end, const std::string& insert)
    {
        if (start > t->text.size ())
        {
            start = t->text.size ();
        }
        if (end > t->text.size ())
        {
            end = t->text.size ();
        }
        if (end < start)
        {
            end = start;
        }
        std::vector<TextRun> runs  = t->char_runs ();
        CharStyle style            = text_style_at (*t, start);
        std::vector<ParaRun> paras = t->para_runs ();
        size_t p0                  = count_newlines (t->text, 0, start);
        size_t removed = count_newlines (t->text, start, end), added = 0;
        for (size_t i = 0; i < insert.size (); i++)
        {
            added += insert[i] == '\n';
        }

        /* Character runs: cut [start, end), then put the insertion in as its own run. */
        std::vector<TextRun> out;
        size_t at   = 0;
        bool placed = false;
        for (size_t i = 0; i < runs.size (); i++)
        {
            size_t a = at, b = at + runs[i].len;
            at = b;
            if (b <= start || a >= end)
            {
                if (a >= end && !placed)
                {
                    TextRun ins = {insert.size (), style};
                    out.push_back (ins);
                    placed = true;
                }
                out.push_back (runs[i]);
                continue;
            }
            if (a < start)
            {
                TextRun before = {start - a, runs[i].style};
                out.push_back (before);
            }
            if (!placed)
            {
                TextRun ins = {insert.size (), style};
                out.push_back (ins);
                placed = true;
            }
            if (b > end)
            {
                TextRun after = {b - end, runs[i].style};
                out.push_back (after);
            }
        }
        if (!placed)
        {
            TextRun ins = {insert.size (), style};
            out.push_back (ins);
        }
        t->text.replace (start, end - start, insert);
        merge_runs (&out);
        t->runs = out;

        std::vector<ParaRun> np;
        for (size_t i = 0; i < p0 && i < paras.size (); i++)
        {
            np.push_back (paras[i]);
        }
        ParaStyle ps = p0 < paras.size () ? paras[p0].style : paras.back ().style;
        for (size_t i = 0; i <= added; i++)
        {
            ParaRun r = {0, ps};
            np.push_back (r);
        }
        for (size_t i = p0 + removed + 1; i < paras.size (); i++)
        {
            np.push_back (paras[i]);
        }
        t->paras = np;
        t->paras = t->para_runs ();
    }

    /* Applies `style` to bytes [start, end) (the whole text when they are equal and the text is
       empty or start = 0 and end = size). */
    void text_set_style (TextLayer* t, size_t start, size_t end, const CharStyle& style)
    {
        if (end > t->text.size ())
        {
            end = t->text.size ();
        }
        if (t->text.empty () || (start == 0 && end >= t->text.size ()))
        {
            TextRun r = {t->text.size (), style};
            t->runs.assign (1, r);
            return;
        }
        std::vector<TextRun> runs = t->char_runs (), out;
        size_t at                 = 0;
        for (size_t i = 0; i < runs.size (); i++)
        {
            size_t a = at, b = at + runs[i].len;
            at = b;
            if (b <= start || a >= end)
            {
                out.push_back (runs[i]);
                continue;
            }
            if (a < start)
            {
                TextRun r = {start - a, runs[i].style};
                out.push_back (r);
            }
            TextRun mid = {(b < end ? b : end) - (a > start ? a : start), style};
            out.push_back (mid);
            if (b > end)
            {
                TextRun r = {b - end, runs[i].style};
                out.push_back (r);
            }
        }
        merge_runs (&out);
        t->runs = out;
    }

    /* The paragraph containing byte pos: [start, end) without its '\n'. */
    void text_paragraph_range (const std::string& s, size_t pos, size_t* start, size_t* end)
    {
        if (pos > s.size ())
        {
            pos = s.size ();
        }
        size_t a = pos;
        while (a > 0 && s[a - 1] != '\n')
        {
            a--;
        }
        size_t b = s.find ('\n', pos);
        *start   = a;
        *end     = b == std::string::npos ? s.size () : b;
    }

    /* Applies `style` to every paragraph touched by bytes [start, end]. */
    void text_set_para (TextLayer* t, size_t start, size_t end, const ParaStyle& style)
    {
        std::vector<ParaRun> paras = t->para_runs ();
        size_t p0 = count_newlines (t->text, 0, start), p1 = count_newlines (t->text, 0, end);
        for (size_t i = p0; i <= p1 && i < paras.size (); i++)
        {
            paras[i].style = style;
        }
        t->paras = paras;
    }

} // namespace doc
