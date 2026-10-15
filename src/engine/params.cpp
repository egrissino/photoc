#include "engine/params.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace engine
{

    /* Command parameters are a JSON object. These accessors never throw: they return false when the
       key is missing or holds the wrong type, matching the reference's as_f64 / as_u64 / as_str. A
       JSON null counts as missing. */

    const Value* param (const Value& p, const char* key)
    {
        if (!p.is_object ())
        {
            return NULL;
        }
        Value::const_iterator it = p.find (key);
        if (it == p.end () || it->is_null ())
        {
            return NULL;
        }
        return &*it;
    }

    bool param_present (const Value& p, const char* key)
    {
        return param (p, key) != NULL;
    }

    bool param_f64 (const Value& p, const char* key, double* out)
    {
        const Value* v = param (p, key);
        if (v == NULL || !v->is_number ())
        {
            return false;
        }
        *out = v->get<double> ();
        return true;
    }

    /* Non-negative integers only (1.0 is not accepted). */
    bool param_u64 (const Value& p, const char* key, uint64_t* out)
    {
        const Value* v = param (p, key);
        if (v == NULL)
        {
            return false;
        }
        if (v->is_number_unsigned ())
        {
            *out = v->get<uint64_t> ();
            return true;
        }
        if (v->is_number_integer () && v->get<int64_t> () >= 0)
        {
            *out = (uint64_t) v->get<int64_t> ();
            return true;
        }
        return false;
    }

    bool param_i64 (const Value& p, const char* key, int64_t* out)
    {
        const Value* v = param (p, key);
        if (v == NULL)
        {
            return false;
        }
        if (v->is_number_unsigned ())
        {
            uint64_t u = v->get<uint64_t> ();
            if (u > (uint64_t) INT64_MAX)
            {
                return false;
            }
            *out = (int64_t) u;
            return true;
        }
        if (v->is_number_integer ())
        {
            *out = v->get<int64_t> ();
            return true;
        }
        return false;
    }

    bool param_bool (const Value& p, const char* key, bool* out)
    {
        const Value* v = param (p, key);
        if (v == NULL || !v->is_boolean ())
        {
            return false;
        }
        *out = v->get<bool> ();
        return true;
    }

    bool param_str (const Value& p, const char* key, std::string* out)
    {
        const Value* v = param (p, key);
        if (v == NULL || !v->is_string ())
        {
            return false;
        }
        *out = v->get_ref<const std::string&> ();
        return true;
    }

    int hex_byte (const char* s)
    {
        if (!isxdigit ((unsigned char) s[0]) || !isxdigit ((unsigned char) s[1]))
        {
            return -1;
        }
        char pair[3] = {s[0], s[1], 0};
        return (int) strtol (pair, NULL, 16);
    }

    /* "#rrggbb" or "#rrggbbaa" (the '#' is optional) as normalised RGBA. */
    bool parse_hex (const char* s, float out[4])
    {
        while (*s == '#')
        {
            s++;
        }
        size_t len = strlen (s);
        if (len != 6 && len != 8)
        {
            return false;
        }
        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (size_t i = 0; i < len / 2; i++)
        {
            int b = hex_byte (s + i * 2);
            if (b < 0)
            {
                return false;
            }
            v[i] = (float) b / 255.0f;
        }
        memcpy (out, v, sizeof v);
        return true;
    }

    /* Lower case without ' ', '_', '-', '(' and ')': "Linear Dodge (Add)" == "lineardodgeadd". */
    std::string blend_norm (const char* s)
    {
        std::string out;
        for (; *s; s++)
        {
            if (*s == ' ' || *s == '_' || *s == '-' || *s == '(' || *s == ')')
            {
                continue;
            }
            out += (char) tolower ((unsigned char) *s);
        }
        return out;
    }

    /* The reference's enum variant name ("LinearDodge", "PaintNetColorBurn"): how blend modes appear
       in serialised settings. Most are the menu label without spaces. */
    std::string blend_variant_name (color::BlendMode m)
    {
        switch (m)
        {
        case color::BlendMode::LinearDodge:
            return "LinearDodge";
        case color::BlendMode::Xor:
            return "Xor";
        case color::BlendMode::PaintNetColorBurn:
            return "PaintNetColorBurn";
        case color::BlendMode::PaintNetColorDodge:
            return "PaintNetColorDodge";
        default:
            break;
        }
        std::string out;
        for (const char* p = color::blend_label (m); *p; p++)
        {
            if (*p != ' ')
            {
                out += *p;
            }
        }
        return out;
    }

    /* Matches either the menu label or the enum name ("Linear Dodge (Add)", "LinearDodge"). */
    bool blend_from_str (const char* s, color::BlendMode* out)
    {
        std::string want = blend_norm (s);
        color::BlendMode candidates[1 + color::LAYER_MODE_COUNT + color::PAINT_NET_MODE_COUNT];
        size_t n        = 0;
        candidates[n++] = color::BlendMode::PassThrough;
        for (size_t i = 0; i < color::LAYER_MODE_COUNT; i++)
        {
            candidates[n++] = color::LAYER_MODES[i];
        }
        for (size_t i = 0; i < color::PAINT_NET_MODE_COUNT; i++)
        {
            candidates[n++] = color::PAINT_NET_MODES[i];
        }
        for (size_t i = 0; i < n; i++)
        {
            if (blend_norm (color::blend_label (candidates[i])) == want
                || blend_norm (blend_variant_name (candidates[i]).c_str ()) == want)
            {
                *out = candidates[i];
                return true;
            }
        }
        return false;
    }

} // namespace engine
