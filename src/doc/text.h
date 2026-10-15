#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace doc
{

    enum class TextAlign : uint8_t
    {
        Left,
        Center,
        Right,
        JustifyLeft,
        JustifyCenter,
        JustifyRight,
        JustifyAll,
    };

    enum class AntiAlias : uint8_t
    {
        None,
        Sharp,
        Crisp,
        Strong,
        Smooth,
    };

    struct CharStyle
    {
        std::string family;
        std::string style;
        std::string postscript;
        float size_pt;
        float color[3];
        float tracking;
        float leading_pt;
        float baseline_shift_pt;
        float hscale;
        float vscale;
        bool underline;
        bool strikethrough;
        bool faux_bold;
        bool faux_italic;
        bool all_caps;
        bool kerning;
        bool ligatures;

        static CharStyle make ();
    };

    struct TextRun
    {
        size_t len;
        CharStyle style;
    };

    struct ParaStyle
    {
        TextAlign align;
        float first_indent_pt;
        float start_indent_pt;
        float end_indent_pt;
        float space_before_pt;
        float space_after_pt;
        float auto_leading;

        static ParaStyle make ();
    };

    struct ParaRun
    {
        size_t len;
        ParaStyle style;
    };

    struct TextLayer
    {
        std::string text;
        std::vector<TextRun> runs;
        std::vector<ParaRun> paras;
        bool box;
        float box_rect[4];
        double transform[6];
        AntiAlias antialias;

        static TextLayer make ();
        std::vector<TextRun> char_runs () const;
        std::vector<ParaRun> para_runs () const;
        CharStyle first_style () const;
    };

    bool same_char_style (const CharStyle& a, const CharStyle& b);
    bool same_para_style (const ParaStyle& a, const ParaStyle& b);
    bool same_text (const TextLayer& a, const TextLayer& b);
    void text_replace (TextLayer* t, size_t start, size_t end, const std::string& insert);
    void text_set_style (TextLayer* t, size_t start, size_t end, const CharStyle& style);
    CharStyle text_style_at (const TextLayer& t, size_t pos);
    void text_set_para (TextLayer* t, size_t start, size_t end, const ParaStyle& style);
    void text_paragraph_range (const std::string& s, size_t pos, size_t* start, size_t* end);

} // namespace doc
