#pragma once

#include "doc/text.h"
#include "text/fonts.h"
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace text
{

    struct Glyph
    {
        int face;
        uint32_t id;
        float x;
        float y;
        float size_px;
        float hscale;
        float vscale;
        float shear;
        float bold;
        float color[3];
    };

    struct Caret
    {
        size_t pos;
        float x;
    };

    struct Line
    {
        float baseline;
        float ascent;
        float descent;
        float left;
        float right;
        size_t start;
        size_t end;
        std::vector<Caret> carets;
    };

    struct Deco
    {
        float x0;
        float x1;
        float y;
        float thickness;
        float color[3];
    };

    struct Layout
    {
        std::vector<Glyph> glyphs;
        std::vector<Line> lines;
        std::vector<Deco> decos;
        float bounds[4];
    };

    float pt_to_px (float pt, float dpi);
    bool layout_text (const doc::TextLayer& t, FontDb* db, float dpi, Layout* out);
    size_t line_of (const Layout& l, size_t pos);
    void caret_at (const Layout& l, size_t pos, float* x, float* top, float* bottom);
    size_t hit_test (const Layout& l, float x, float y);
    size_t caret_move_vertical (const Layout& l, size_t pos, int dir);
    size_t line_home (const Layout& l, size_t pos);
    size_t line_end (const Layout& l, size_t pos);

} // namespace text
