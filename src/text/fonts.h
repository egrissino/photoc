#pragma once

#include <ft2build.h>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>
#include FT_FREETYPE_H
#include <hb.h>

namespace text
{

    struct FontFace
    {
        std::string family;
        std::string style;
        std::string path;
        std::string postscript;
        int index;
        int weight;
        bool italic;
        const uint8_t* memory;
        size_t memory_len;
        FT_Face ft;
        hb_font_t* hb;
    };

    class FontDb
    {
    public:
        FontDb ();
        ~FontDb ();

        void add_memory_font (const uint8_t* data, size_t len);
        void add_file (const char* path);
        std::vector<std::string> families () const;
        std::vector<std::string> styles (const std::string& family) const;
        std::string default_family () const;
        int resolve (const std::string& family, const std::string& style);
        int fallback_for (int face, uint32_t codepoint);
        bool has_glyph (int face, uint32_t codepoint);
        FT_Face ft (int face);
        hb_font_t* hb (int face);
        const FontFace& face (int face) const;
        int find_postscript (const std::string& name);
        std::string postscript_for (const std::string& family, const std::string& style);

        FontDb (const FontDb&)            = delete;
        FontDb& operator= (const FontDb&) = delete;

    private:
        void scan_system ();
        int find (const std::string& path, int index) const;
        int add_face (const FontFace& f);

        FT_Library lib_;
        std::vector<FontFace> faces_;
        std::unordered_map<uint64_t, int> fallback_;
        std::string default_family_;
    };

} // namespace text
