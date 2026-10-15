#include "text/fonts.h"

#include <algorithm>
#include <ctype.h>
#include <fontconfig/fontconfig.h>
#include <hb-ft.h>
#include <hb-ot.h>
#include <string.h>
#include <strings.h>

namespace text
{

    /* The fonts type can use: every scalable system font Fontconfig knows, plus fonts added from
       memory (the app's own) or files. Faces open on first use. A character a face lacks comes from
       another face found through Fontconfig's coverage data (or any added face that has it). */

    FontDb::FontDb ()
    {
        lib_ = NULL;
        FT_Init_FreeType (&lib_);
        scan_system ();
    }

    FontDb::~FontDb ()
    {
        for (size_t i = 0; i < faces_.size (); i++)
        {
            if (faces_[i].hb != NULL)
            {
                hb_font_destroy (faces_[i].hb);
            }
            if (faces_[i].ft != NULL)
            {
                FT_Done_Face (faces_[i].ft);
            }
        }
        if (lib_ != NULL)
        {
            FT_Done_FreeType (lib_);
        }
    }

    int FontDb::find (const std::string& path, int index) const
    {
        for (size_t i = 0; i < faces_.size (); i++)
        {
            if (faces_[i].memory == NULL && faces_[i].path == path && faces_[i].index == index)
            {
                return (int) i;
            }
        }
        return -1;
    }

    int FontDb::add_face (const FontFace& f)
    {
        for (size_t i = 0; i < faces_.size (); i++)
        {
            if (strcasecmp (faces_[i].family.c_str (), f.family.c_str ()) == 0
                && strcasecmp (faces_[i].style.c_str (), f.style.c_str ()) == 0)
            {
                return (int) i;
            }
        }
        faces_.push_back (f);
        return (int) faces_.size () - 1;
    }

    void FontDb::scan_system ()
    {
        if (!FcInit ())
        {
            return;
        }
        FcPattern* pat = FcPatternCreate ();
        FcPatternAddBool (pat, FC_SCALABLE, FcTrue);
        FcObjectSet* os = FcObjectSetBuild (
            FC_FAMILY, FC_STYLE, FC_FILE, FC_INDEX, FC_WEIGHT, FC_SLANT, FC_POSTSCRIPT_NAME, (char*) 0);
        FcFontSet* set = FcFontList (NULL, pat, os);
        for (int i = 0; set != NULL && i < set->nfont; i++)
        {
            FcChar8 *fam = NULL, *sty = NULL, *file = NULL, *ps = NULL;
            int index = 0, weight = FC_WEIGHT_REGULAR, slant = FC_SLANT_ROMAN;
            if (FcPatternGetString (set->fonts[i], FC_FAMILY, 0, &fam) != FcResultMatch
                || FcPatternGetString (set->fonts[i], FC_FILE, 0, &file) != FcResultMatch)
            {
                continue;
            }
            FcPatternGetString (set->fonts[i], FC_STYLE, 0, &sty);
            FcPatternGetInteger (set->fonts[i], FC_INDEX, 0, &index);
            FcPatternGetInteger (set->fonts[i], FC_WEIGHT, 0, &weight);
            FcPatternGetInteger (set->fonts[i], FC_SLANT, 0, &slant);
            FcPatternGetString (set->fonts[i], FC_POSTSCRIPT_NAME, 0, &ps);
            /* Variable-font named instances share a file; only plain faces are listed. */
            if (index > 0xFFFF)
            {
                continue;
            }
            FontFace f;
            f.family     = (const char*) fam;
            f.style      = sty != NULL ? (const char*) sty : "Regular";
            f.path       = (const char*) file;
            f.postscript = ps != NULL ? (const char*) ps : "";
            f.index      = index;
            f.weight     = weight;
            f.italic     = slant != FC_SLANT_ROMAN;
            f.memory     = NULL;
            f.memory_len = 0;
            f.ft         = NULL;
            f.hb         = NULL;
            add_face (f);
        }
        if (set != NULL)
        {
            FcFontSetDestroy (set);
        }
        FcObjectSetDestroy (os);
        FcPatternDestroy (pat);
        /* The system's default sans serif. */
        FcPattern* def = FcNameParse ((const FcChar8*) "sans-serif");
        FcConfigSubstitute (NULL, def, FcMatchPattern);
        FcDefaultSubstitute (def);
        FcResult res;
        FcPattern* m = FcFontMatch (NULL, def, &res);
        FcChar8* fam = NULL;
        if (m != NULL && FcPatternGetString (m, FC_FAMILY, 0, &fam) == FcResultMatch)
        {
            default_family_ = (const char*) fam;
        }
        if (m != NULL)
        {
            FcPatternDestroy (m);
        }
        FcPatternDestroy (def);
    }

    /* A font in memory (kept by the caller for the database's lifetime). */
    void FontDb::add_memory_font (const uint8_t* data, size_t len)
    {
        FT_Face face;
        if (lib_ == NULL || FT_New_Memory_Face (lib_, data, (FT_Long) len, 0, &face) != 0)
        {
            return;
        }
        FontFace f;
        f.family      = face->family_name != NULL ? face->family_name : "Unnamed";
        f.style       = face->style_name != NULL ? face->style_name : "Regular";
        f.path        = "";
        f.postscript  = FT_Get_Postscript_Name (face) != NULL ? FT_Get_Postscript_Name (face) : "";
        f.index       = 0;
        f.weight      = FcWeightFromOpenType ((face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400);
        f.italic      = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
        f.memory      = data;
        f.memory_len  = len;
        f.ft          = face;
        f.hb          = NULL;
        size_t before = faces_.size ();
        if ((size_t) add_face (f) < before)
        {
            FT_Done_Face (face);
        }
        if (default_family_.empty ())
        {
            default_family_ = f.family;
        }
    }

    void FontDb::add_file (const char* path)
    {
        FT_Face face;
        if (lib_ == NULL || FT_New_Face (lib_, path, 0, &face) != 0)
        {
            return;
        }
        FontFace f;
        f.family      = face->family_name != NULL ? face->family_name : "Unnamed";
        f.style       = face->style_name != NULL ? face->style_name : "Regular";
        f.path        = path;
        f.postscript  = FT_Get_Postscript_Name (face) != NULL ? FT_Get_Postscript_Name (face) : "";
        f.index       = 0;
        f.weight      = FcWeightFromOpenType ((face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400);
        f.italic      = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
        f.memory      = NULL;
        f.memory_len  = 0;
        f.ft          = face;
        f.hb          = NULL;
        size_t before = faces_.size ();
        if ((size_t) add_face (f) < before)
        {
            FT_Done_Face (face);
        }
        if (default_family_.empty ())
        {
            default_family_ = f.family;
        }
    }

    std::vector<std::string> FontDb::families () const
    {
        std::vector<std::string> out;
        for (size_t i = 0; i < faces_.size (); i++)
        {
            out.push_back (faces_[i].family);
        }
        std::sort (out.begin (), out.end ());
        out.erase (std::unique (out.begin (), out.end ()), out.end ());
        return out;
    }

    std::vector<std::string> FontDb::styles (const std::string& family) const
    {
        std::vector<std::pair<int, std::string>> found;
        for (size_t i = 0; i < faces_.size (); i++)
        {
            if (faces_[i].family == family)
            {
                found.push_back (std::make_pair (faces_[i].weight * 2 + (faces_[i].italic ? 1 : 0), faces_[i].style));
            }
        }
        std::sort (found.begin (), found.end ());
        std::vector<std::string> out;
        for (size_t i = 0; i < found.size (); i++)
        {
            out.push_back (found[i].second);
        }
        return out;
    }

    std::string FontDb::default_family () const
    {
        if (!default_family_.empty ())
        {
            return default_family_;
        }
        return faces_.empty () ? std::string () : faces_[0].family;
    }

    /* The face for a family and style: the exact style, else Regular (or the face nearest to a
       normal weight), else the default family; -1 when there are no fonts at all. */
    int FontDb::resolve (const std::string& family, const std::string& style)
    {
        const std::string& fam = family.empty () ? default_family () : family;
        int best = -1, best_score = 1 << 30;
        for (int pass = 0; pass < 2 && best < 0; pass++)
        {
            const std::string& want = pass == 0 ? fam : default_family ();
            for (size_t i = 0; i < faces_.size (); i++)
            {
                if (strcasecmp (faces_[i].family.c_str (), want.c_str ()) != 0)
                {
                    continue;
                }
                /* The exact style, else the upright face nearest a regular weight. */
                int score;
                if (strcasecmp (faces_[i].style.c_str (), style.c_str ()) == 0)
                {
                    score = 0;
                }
                else
                {
                    score = 10 + abs (faces_[i].weight - FC_WEIGHT_REGULAR) + (faces_[i].italic ? 1000 : 0);
                }
                if (score < best_score)
                {
                    best       = (int) i;
                    best_score = score;
                }
            }
        }
        if (best < 0 && !faces_.empty ())
        {
            best = 0;
        }
        return best;
    }

    FT_Face FontDb::ft (int i)
    {
        if (i < 0 || (size_t) i >= faces_.size ())
        {
            return NULL;
        }
        FontFace& f = faces_[(size_t) i];
        if (f.ft == NULL && lib_ != NULL)
        {
            if (f.memory != NULL)
            {
                FT_New_Memory_Face (lib_, f.memory, (FT_Long) f.memory_len, f.index, &f.ft);
            }
            else
            {
                FT_New_Face (lib_, f.path.c_str (), f.index, &f.ft);
            }
        }
        return f.ft;
    }

    /* A HarfBuzz font over the face, scaled per use with hb_font_set_scale. */
    hb_font_t* FontDb::hb (int i)
    {
        FT_Face face = ft (i);
        if (face == NULL)
        {
            return NULL;
        }
        FontFace& f = faces_[(size_t) i];
        if (f.hb == NULL)
        {
            hb_face_t* hf = hb_ft_face_create_referenced (face);
            f.hb          = hb_font_create (hf);
            hb_face_destroy (hf);
            hb_ot_font_set_funcs (f.hb);
        }
        return f.hb;
    }

    const FontFace& FontDb::face (int i) const
    {
        return faces_[(size_t) (i < 0 ? 0 : i)];
    }

    bool FontDb::has_glyph (int i, uint32_t cp)
    {
        FT_Face face = ft (i);
        return face != NULL && FT_Get_Char_Index (face, cp) != 0;
    }

    /* A face for a character `face` lacks: Fontconfig's best match for the character (in the same
       style when it can), else any face that has it; `face` itself when none does. */
    int FontDb::fallback_for (int face, uint32_t cp)
    {
        uint64_t key                                   = (uint64_t) (uint32_t) face << 32 | cp;
        std::unordered_map<uint64_t, int>::iterator it = fallback_.find (key);
        if (it != fallback_.end ())
        {
            return it->second;
        }
        int found      = -1;
        FcPattern* pat = FcPatternCreate ();
        FcCharSet* cs  = FcCharSetCreate ();
        FcCharSetAddChar (cs, cp);
        FcPatternAddCharSet (pat, FC_CHARSET, cs);
        FcPatternAddBool (pat, FC_SCALABLE, FcTrue);
        if (face >= 0 && (size_t) face < faces_.size ())
        {
            FcPatternAddInteger (pat, FC_WEIGHT, faces_[(size_t) face].weight);
            FcPatternAddInteger (pat, FC_SLANT, faces_[(size_t) face].italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
        }
        FcConfigSubstitute (NULL, pat, FcMatchPattern);
        FcDefaultSubstitute (pat);
        FcResult res;
        FcFontSet* set = FcFontSort (NULL, pat, FcTrue, NULL, &res);
        for (int i = 0; set != NULL && i < set->nfont && found < 0; i++)
        {
            FcChar8* file   = NULL;
            int index       = 0;
            FcCharSet* have = NULL;
            if (FcPatternGetString (set->fonts[i], FC_FILE, 0, &file) != FcResultMatch)
            {
                continue;
            }
            if (FcPatternGetCharSet (set->fonts[i], FC_CHARSET, 0, &have) == FcResultMatch
                && !FcCharSetHasChar (have, cp))
            {
                continue;
            }
            FcPatternGetInteger (set->fonts[i], FC_INDEX, 0, &index);
            int k = find ((const char*) file, index);
            if (k >= 0 && has_glyph (k, cp))
            {
                found = k;
            }
        }
        if (set != NULL)
        {
            FcFontSetDestroy (set);
        }
        FcCharSetDestroy (cs);
        FcPatternDestroy (pat);
        for (size_t i = 0; i < faces_.size () && found < 0; i++)
        {
            if (faces_[i].memory != NULL && has_glyph ((int) i, cp))
            {
                found = (int) i;
            }
        }
        if (found < 0)
        {
            found = face;
        }
        fallback_[key] = found;
        return found;
    }

    std::string squeeze (const std::string& s)
    {
        std::string out;
        for (size_t i = 0; i < s.size (); i++)
        {
            if (s[i] != ' ' && s[i] != '-' && s[i] != '_')
            {
                out.push_back ((char) tolower ((unsigned char) s[i]));
            }
        }
        return out;
    }

    /* The face with PostScript name `name`; else a guess from the usual "Family-Style" form (the
       family with spaces taken out, "It" for Italic), or -1. */
    int FontDb::find_postscript (const std::string& name)
    {
        for (size_t i = 0; i < faces_.size (); i++)
        {
            if (faces_[i].postscript == name)
            {
                return (int) i;
            }
        }
        size_t dash     = name.find ('-');
        std::string fam = squeeze (dash == std::string::npos ? name : name.substr (0, dash));
        std::string sty = dash == std::string::npos ? "regular" : squeeze (name.substr (dash + 1));
        if (fam.size () > 2 && fam.compare (fam.size () - 2, 2, "mt") == 0)
        {
            fam = fam.substr (0, fam.size () - 2);
        }
        int best = -1, best_score = 1 << 30;
        for (size_t i = 0; i < faces_.size (); i++)
        {
            if (squeeze (faces_[i].family) != fam)
            {
                continue;
            }
            std::string s = squeeze (faces_[i].style);
            int score     = 100;
            if (s == sty)
            {
                score = 0;
            }
            else if (s + "it" == sty || s == sty + "italic"
                     || (sty.size () > 2 && sty.compare (sty.size () - 2, 2, "it") == 0
                         && s == sty.substr (0, sty.size () - 2) + "italic"))
            {
                score = 1;
            }
            else if (s == "regular" || s == "book" || s == "roman")
            {
                score = 50;
            }
            if (score < best_score)
            {
                best       = (int) i;
                best_score = score;
            }
        }
        return best;
    }

    /* The PostScript name for a family and style: the face's own, else "FamilyStyle-Style". */
    std::string FontDb::postscript_for (const std::string& family, const std::string& style)
    {
        int f = resolve (family, style);
        if (f >= 0 && strcasecmp (faces_[(size_t) f].family.c_str (), family.c_str ()) == 0
            && !faces_[(size_t) f].postscript.empty ())
        {
            return faces_[(size_t) f].postscript;
        }
        std::string a, b;
        for (size_t i = 0; i < family.size (); i++)
        {
            if (family[i] != ' ')
            {
                a.push_back (family[i]);
            }
        }
        for (size_t i = 0; i < style.size (); i++)
        {
            if (style[i] != ' ')
            {
                b.push_back (style[i]);
            }
        }
        return a + "-" + (b.empty () ? std::string ("Regular") : b);
    }

} // namespace text
