#include "psd/engine_data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace psd
{

    /* Photoshop's EngineData, the text engine document inside a type layer's TySh block: a
       PostScript-like syntax of dictionaries << /Key value >>, arrays [ ], strings ( ) holding
       UTF-16BE behind a FE FF mark with backslash escapes, numbers, true / false and /Names. */

    const int MAX_DEPTH = 64;

    EValue e_make (EType t)
    {
        EValue v;
        v.type = t;
        v.i    = 0;
        v.num  = 0.0;
        v.b    = false;
        return v;
    }

    EValue e_dict ()
    {
        return e_make (EType::Dict);
    }

    EValue e_array ()
    {
        return e_make (EType::Array);
    }

    EValue e_string (const std::string& utf8)
    {
        EValue v = e_make (EType::String);
        v.str    = utf8;
        return v;
    }

    EValue e_int (int64_t x)
    {
        EValue v = e_make (EType::Int);
        v.i      = x;
        v.num    = (double) x;
        return v;
    }

    EValue e_real (double x)
    {
        EValue v = e_make (EType::Real);
        v.num    = x;
        v.i      = (int64_t) x;
        return v;
    }

    EValue e_bool (bool x)
    {
        EValue v = e_make (EType::Bool);
        v.b      = x;
        return v;
    }

    const EValue* e_get (const EValue& d, const char* key)
    {
        if (d.type != EType::Dict)
        {
            return NULL;
        }
        for (size_t i = 0; i < d.keys.size (); i++)
        {
            if (d.keys[i] == key)
            {
                return &d.items[i];
            }
        }
        return NULL;
    }

    const EValue* e_path (const EValue& d, const char* const* keys, size_t n)
    {
        const EValue* v = &d;
        for (size_t i = 0; i < n && v != NULL; i++)
        {
            v = e_get (*v, keys[i]);
        }
        return v;
    }

    EValue* e_get_mut (EValue* d, const char* key)
    {
        if (d->type != EType::Dict)
        {
            return NULL;
        }
        for (size_t i = 0; i < d->keys.size (); i++)
        {
            if (d->keys[i] == key)
            {
                return &d->items[i];
            }
        }
        return NULL;
    }

    void e_set (EValue* d, const char* key, const EValue& v)
    {
        EValue* have = e_get_mut (d, key);
        if (have != NULL)
        {
            *have = v;
            return;
        }
        d->keys.push_back (key);
        d->items.push_back (v);
    }

    bool e_num (const EValue* v, double* out)
    {
        if (v == NULL || (v->type != EType::Int && v->type != EType::Real))
        {
            return false;
        }
        *out = v->type == EType::Int ? (double) v->i : v->num;
        return true;
    }

    bool e_flag (const EValue* v, bool* out)
    {
        if (v == NULL)
        {
            return false;
        }
        if (v->type == EType::Bool)
        {
            *out = v->b;
        }
        else if (v->type == EType::Int)
        {
            *out = v->i != 0;
        }
        else
        {
            return false;
        }
        return true;
    }

    struct EParser
    {
        const uint8_t* b;
        size_t n;
        size_t i;
    };

    bool delimiter (uint8_t c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '[' || c == ']' || c == '<' || c == '>'
            || c == '(' || c == '/' || c == 0;
    }

    void skip_ws (EParser* p)
    {
        while (
            p->i < p->n
            && (p->b[p->i] == ' ' || p->b[p->i] == '\t' || p->b[p->i] == '\r' || p->b[p->i] == '\n' || p->b[p->i] == 0))
        {
            p->i++;
        }
    }

    std::string read_name (EParser* p)
    {
        p->i++;
        size_t start = p->i;
        while (p->i < p->n && !delimiter (p->b[p->i]))
        {
            p->i++;
        }
        return std::string ((const char*) p->b + start, p->i - start);
    }

    void put_utf8 (std::string* out, uint32_t cp)
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

    /* A string's bytes: UTF-16BE after FE FF, else Latin-1. */
    std::string decode_string (const std::vector<uint8_t>& raw)
    {
        std::string out;
        if (raw.size () >= 2 && raw[0] == 0xFE && raw[1] == 0xFF)
        {
            for (size_t k = 2; k + 1 < raw.size (); k += 2)
            {
                uint32_t c = (uint32_t) raw[k] << 8 | raw[k + 1];
                if (c >= 0xD800 && c < 0xDC00 && k + 3 < raw.size ())
                {
                    uint32_t lo = (uint32_t) raw[k + 2] << 8 | raw[k + 3];
                    if (lo >= 0xDC00 && lo < 0xE000)
                    {
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        k += 2;
                    }
                }
                put_utf8 (&out, c);
            }
        }
        else
        {
            for (size_t k = 0; k < raw.size (); k++)
            {
                put_utf8 (&out, raw[k]);
            }
        }
        return out;
    }

    bool parse_value (EParser* p, EValue* out, int depth)
    {
        if (depth > MAX_DEPTH)
        {
            return false;
        }
        skip_ws (p);
        if (p->i >= p->n)
        {
            return false;
        }
        uint8_t c = p->b[p->i];
        if (c == '<' && p->i + 1 < p->n && p->b[p->i + 1] == '<')
        {
            p->i += 2;
            *out = e_dict ();
            for (;;)
            {
                skip_ws (p);
                if (p->i >= p->n)
                {
                    return false;
                }
                if (p->b[p->i] == '>' && p->i + 1 < p->n && p->b[p->i + 1] == '>')
                {
                    p->i += 2;
                    return true;
                }
                if (p->b[p->i] != '/')
                {
                    return false;
                }
                std::string key = read_name (p);
                EValue v;
                if (!parse_value (p, &v, depth + 1))
                {
                    return false;
                }
                out->keys.push_back (key);
                out->items.push_back (v);
            }
        }
        if (c == '[')
        {
            p->i++;
            *out = e_array ();
            for (;;)
            {
                skip_ws (p);
                if (p->i >= p->n)
                {
                    return false;
                }
                if (p->b[p->i] == ']')
                {
                    p->i++;
                    return true;
                }
                EValue v;
                if (!parse_value (p, &v, depth + 1))
                {
                    return false;
                }
                out->items.push_back (v);
            }
        }
        if (c == '(')
        {
            p->i++;
            std::vector<uint8_t> raw;
            for (;;)
            {
                if (p->i >= p->n)
                {
                    return false;
                }
                uint8_t ch = p->b[p->i];
                if (ch == '\\')
                {
                    if (p->i + 1 < p->n)
                    {
                        raw.push_back (p->b[p->i + 1]);
                    }
                    p->i += 2;
                }
                else if (ch == ')')
                {
                    p->i++;
                    break;
                }
                else
                {
                    raw.push_back (ch);
                    p->i++;
                }
            }
            *out = e_string (decode_string (raw));
            return true;
        }
        if (c == '/')
        {
            *out     = e_make (EType::Name);
            out->str = read_name (p);
            return true;
        }
        size_t start = p->i;
        while (p->i < p->n && !delimiter (p->b[p->i]))
        {
            p->i++;
        }
        std::string tok ((const char*) p->b + start, p->i - start);
        if (tok.empty ())
        {
            p->i++;
            return false;
        }
        if (tok == "true" || tok == "false")
        {
            *out = e_bool (tok == "true");
            return true;
        }
        char* end = NULL;
        if (tok.find_first_of (".eE") != std::string::npos)
        {
            double d = strtod (tok.c_str (), &end);
            if (end == tok.c_str ())
            {
                return false;
            }
            *out = e_real (d);
            return true;
        }
        long long v = strtoll (tok.c_str (), &end, 10);
        if (end == tok.c_str ())
        {
            return false;
        }
        *out = e_int ((int64_t) v);
        return true;
    }

    bool parse_engine_data (const uint8_t* data, size_t len, EValue* out)
    {
        EParser p = {data, len, 0};
        return parse_value (&p, out, 0);
    }

    void tabs (std::vector<uint8_t>* out, size_t n)
    {
        out->insert (out->end (), n, '\t');
    }

    void put_str (std::vector<uint8_t>* out, const char* s)
    {
        out->insert (out->end (), s, s + strlen (s));
    }

    bool is_container (const EValue& v)
    {
        if (v.type == EType::Dict)
        {
            return true;
        }
        if (v.type != EType::Array)
        {
            return false;
        }
        for (size_t i = 0; i < v.items.size (); i++)
        {
            if (v.items[i].type == EType::Dict || v.items[i].type == EType::Array)
            {
                return true;
            }
        }
        return false;
    }

    /* Reals as Photoshop writes them: "12.0", ".5", "-.25". */
    void put_real (std::vector<uint8_t>* out, double r)
    {
        char buf[64];
        if (!isfinite (r))
        {
            r = 0.0;
        }
        if (r == floor (r) && fabs (r) < 1e15)
        {
            snprintf (buf, sizeof buf, "%.1f", r);
            put_str (out, buf);
            return;
        }
        snprintf (buf, sizeof buf, "%.5f", r);
        size_t len = strlen (buf);
        while (len > 0 && buf[len - 1] == '0')
        {
            buf[--len] = 0;
        }
        const char* s = buf;
        if (strncmp (s, "0.", 2) == 0)
        {
            s++;
        }
        if (strncmp (buf, "-0.", 3) == 0)
        {
            out->push_back ('-');
            s = buf + 2;
        }
        put_str (out, s);
    }

    void write_value (std::vector<uint8_t>* out, const EValue& v, size_t depth)
    {
        switch (v.type)
        {
        case EType::Dict:
            put_str (out, "<<\n");
            for (size_t i = 0; i < v.keys.size (); i++)
            {
                tabs (out, depth + 1);
                out->push_back ('/');
                put_str (out, v.keys[i].c_str ());
                if (is_container (v.items[i]))
                {
                    out->push_back ('\n');
                    tabs (out, depth + 1);
                }
                else
                {
                    out->push_back (' ');
                }
                write_value (out, v.items[i], depth + 1);
                out->push_back ('\n');
            }
            tabs (out, depth);
            put_str (out, ">>");
            break;
        case EType::Array:
            if (is_container (v))
            {
                put_str (out, "[\n");
                for (size_t i = 0; i < v.items.size (); i++)
                {
                    tabs (out, depth);
                    write_value (out, v.items[i], depth);
                    out->push_back ('\n');
                }
                tabs (out, depth);
                out->push_back (']');
            }
            else
            {
                out->push_back ('[');
                for (size_t i = 0; i < v.items.size (); i++)
                {
                    out->push_back (' ');
                    write_value (out, v.items[i], depth);
                }
                put_str (out, " ]");
            }
            break;
        case EType::String:
            {
                out->push_back ('(');
                std::vector<uint8_t> raw;
                raw.push_back (0xFE);
                raw.push_back (0xFF);
                const std::string& s = v.str;
                for (size_t k = 0; k < s.size ();)
                {
                    uint8_t c = (uint8_t) s[k];
                    uint32_t cp;
                    size_t len;
                    if (c < 0x80)
                    {
                        cp = c, len = 1;
                    }
                    else if ((c >> 5) == 6)
                    {
                        cp = c & 0x1F, len = 2;
                    }
                    else if ((c >> 4) == 14)
                    {
                        cp = c & 0x0F, len = 3;
                    }
                    else
                    {
                        cp = c & 0x07, len = 4;
                    }
                    for (size_t m = 1; m < len && k + m < s.size (); m++)
                    {
                        cp = cp << 6 | ((uint8_t) s[k + m] & 0x3F);
                    }
                    k += len;
                    if (cp >= 0x10000)
                    {
                        uint32_t x = cp - 0x10000, hi = 0xD800 + (x >> 10), lo = 0xDC00 + (x & 0x3FF);
                        raw.push_back ((uint8_t) (hi >> 8));
                        raw.push_back ((uint8_t) hi);
                        raw.push_back ((uint8_t) (lo >> 8));
                        raw.push_back ((uint8_t) lo);
                    }
                    else
                    {
                        raw.push_back ((uint8_t) (cp >> 8));
                        raw.push_back ((uint8_t) cp);
                    }
                }
                for (size_t k = 0; k < raw.size (); k++)
                {
                    if (raw[k] == '(' || raw[k] == ')' || raw[k] == '\\')
                    {
                        out->push_back ('\\');
                    }
                    out->push_back (raw[k]);
                }
                out->push_back (')');
                break;
            }
        case EType::Int:
            {
                char buf[32];
                snprintf (buf, sizeof buf, "%lld", (long long) v.i);
                put_str (out, buf);
                break;
            }
        case EType::Real:
            put_real (out, v.num);
            break;
        case EType::Bool:
            put_str (out, v.b ? "true" : "false");
            break;
        case EType::Name:
            out->push_back ('/');
            put_str (out, v.str.c_str ());
            break;
        }
    }

    std::vector<uint8_t> write_engine_data (const EValue& v)
    {
        std::vector<uint8_t> out;
        put_str (&out, "\n\n");
        write_value (&out, v, 0);
        out.push_back ('\n');
        return out;
    }

} // namespace psd
