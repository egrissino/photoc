#include "psd/descriptor.h"

#include <string.h>

namespace psd
{

    /* Photoshop's action descriptors: a class id and keyed, typed values (objects, lists, numbers
       with units, text, enumerations...). Keys and ids are 4-character codes or longer strings. */

    const int MAX_DEPTH = 64;

    DValue d_make (DType t)
    {
        DValue v;
        v.type = t;
        v.num  = 0.0;
        v.i    = 0;
        v.b    = false;
        return v;
    }

    DValue d_object (const char* cls)
    {
        DValue v = d_make (DType::Object);
        v.cls    = cls;
        return v;
    }

    DValue d_double (double x)
    {
        DValue v = d_make (DType::Double);
        v.num    = x;
        return v;
    }

    DValue d_unit (const char* unit, double x)
    {
        DValue v = d_make (DType::UnitFloat);
        v.cls    = unit;
        v.num    = x;
        return v;
    }

    DValue d_text (const std::string& s)
    {
        DValue v = d_make (DType::Text);
        v.str    = s;
        return v;
    }

    DValue d_enum (const char* type, const char* value)
    {
        DValue v = d_make (DType::Enum);
        v.cls    = type;
        v.str    = value;
        return v;
    }

    DValue d_long (int32_t x)
    {
        DValue v = d_make (DType::Long);
        v.i      = x;
        v.num    = x;
        return v;
    }

    DValue d_bool (bool x)
    {
        DValue v = d_make (DType::Bool);
        v.b      = x;
        return v;
    }

    DValue d_list ()
    {
        return d_make (DType::List);
    }

    DValue d_raw (const char* type, const std::string& bytes)
    {
        DValue v = d_make (DType::Raw);
        v.cls    = type;
        v.str    = bytes;
        return v;
    }

    void d_set (DValue* obj, const char* key, const DValue& v)
    {
        for (size_t i = 0; i < obj->keys.size (); i++)
        {
            if (obj->keys[i] == key)
            {
                obj->items[i] = v;
                return;
            }
        }
        obj->keys.push_back (key);
        obj->items.push_back (v);
    }

    const DValue* d_get (const DValue& obj, const char* key)
    {
        for (size_t i = 0; i < obj.keys.size (); i++)
        {
            if (obj.keys[i] == key)
            {
                return &obj.items[i];
            }
        }
        return NULL;
    }

    double d_num (const DValue& obj, const char* key, double fallback)
    {
        const DValue* v = d_get (obj, key);
        if (v == NULL)
        {
            return fallback;
        }
        if (v->type == DType::Double || v->type == DType::UnitFloat)
        {
            return v->num;
        }
        if (v->type == DType::Long || v->type == DType::LargeInt)
        {
            return (double) v->i;
        }
        return fallback;
    }

    bool d_flag (const DValue& obj, const char* key, bool fallback)
    {
        const DValue* v = d_get (obj, key);
        return v != NULL && v->type == DType::Bool ? v->b : fallback;
    }

    std::string d_enum_of (const DValue& obj, const char* key)
    {
        const DValue* v = d_get (obj, key);
        return v != NULL && v->type == DType::Enum ? v->str : std::string ();
    }

    /* A key or class id: a length, or 0 and four characters. */
    std::string rd_id (Reader* r)
    {
        uint32_t n = rd_u32 (r);
        if (n == 0)
        {
            return rd_key4 (r);
        }
        if (n > remaining (*r))
        {
            r->ok = false;
            return std::string ();
        }
        const uint8_t* p = rd_bytes (r, n);
        return p != NULL ? std::string ((const char*) p, n) : std::string ();
    }

    void wr_id (std::vector<uint8_t>* w, const std::string& id)
    {
        if (id.size () == 4)
        {
            wr_u32 (w, 0);
            wr_bytes (w, id.data (), 4);
        }
        else
        {
            wr_u32 (w, (uint32_t) id.size ());
            wr_bytes (w, id.data (), id.size ());
        }
    }

    bool parse_value (Reader* r, const std::string& type, DValue* out, int depth);

    bool parse_object (Reader* r, DValue* out, int depth)
    {
        if (depth > MAX_DEPTH)
        {
            return false;
        }
        *out       = d_make (DType::Object);
        out->str   = rd_unicode (r);
        out->cls   = rd_id (r);
        uint32_t n = rd_u32 (r);
        for (uint32_t i = 0; i < n && r->ok; i++)
        {
            std::string key  = rd_id (r);
            std::string type = rd_key4 (r);
            DValue v;
            if (!parse_value (r, type, &v, depth + 1))
            {
                return false;
            }
            out->keys.push_back (key);
            out->items.push_back (v);
        }
        return r->ok;
    }

    /* A reference ('obj '): read past, kept as a marker only. */
    bool skip_reference (Reader* r)
    {
        uint32_t n = rd_u32 (r);
        for (uint32_t i = 0; i < n && r->ok; i++)
        {
            std::string t = rd_key4 (r);
            if (t == "prop")
            {
                rd_unicode (r);
                rd_id (r);
                rd_id (r);
            }
            else if (t == "Clss")
            {
                rd_unicode (r);
                rd_id (r);
            }
            else if (t == "Enmr")
            {
                rd_unicode (r);
                rd_id (r);
                rd_id (r);
                rd_id (r);
            }
            else if (t == "rele")
            {
                rd_unicode (r);
                rd_id (r);
                rd_u32 (r);
            }
            else if (t == "Idnt" || t == "indx")
            {
                rd_u32 (r);
            }
            else if (t == "name")
            {
                rd_unicode (r);
                rd_id (r);
                rd_unicode (r);
            }
            else
            {
                return false;
            }
        }
        return r->ok;
    }

    bool parse_value (Reader* r, const std::string& type, DValue* out, int depth)
    {
        if (depth > MAX_DEPTH)
        {
            return false;
        }
        if (type == "Objc" || type == "GlbO")
        {
            return parse_object (r, out, depth);
        }
        if (type == "VlLs")
        {
            *out       = d_make (DType::List);
            uint32_t n = rd_u32 (r);
            for (uint32_t i = 0; i < n && r->ok; i++)
            {
                DValue v;
                if (!parse_value (r, rd_key4 (r), &v, depth + 1))
                {
                    return false;
                }
                out->items.push_back (v);
            }
            return r->ok;
        }
        if (type == "doub")
        {
            *out = d_double (rd_f64 (r));
        }
        else if (type == "UntF")
        {
            std::string unit = rd_key4 (r);
            *out             = d_unit (unit.c_str (), rd_f64 (r));
        }
        else if (type == "TEXT")
        {
            *out = d_text (rd_unicode (r));
        }
        else if (type == "enum")
        {
            std::string t = rd_id (r);
            std::string v = rd_id (r);
            *out          = d_enum (t.c_str (), v.c_str ());
        }
        else if (type == "long")
        {
            *out = d_long (rd_i32 (r));
        }
        else if (type == "comp")
        {
            *out     = d_make (DType::LargeInt);
            out->i   = (int64_t) rd_u64 (r);
            out->num = (double) out->i;
        }
        else if (type == "bool")
        {
            *out = d_bool (rd_u8 (r) != 0);
        }
        else if (type == "type" || type == "GlbC")
        {
            *out     = d_make (DType::Class);
            out->str = rd_unicode (r);
            out->cls = rd_id (r);
        }
        else if (type == "alis" || type == "tdta")
        {
            *out             = d_make (DType::Raw);
            uint32_t n       = rd_u32 (r);
            const uint8_t* p = rd_bytes (r, n);
            if (p != NULL)
            {
                out->str.assign ((const char*) p, n);
            }
            out->cls = type;
        }
        else if (type == "obj ")
        {
            *out = d_make (DType::Reference);
            return skip_reference (r);
        }
        else if (type == "UnFl")
        {
            *out             = d_make (DType::List);
            std::string unit = rd_key4 (r);
            uint32_t n       = rd_u32 (r);
            for (uint32_t i = 0; i < n && r->ok; i++)
            {
                out->items.push_back (d_unit (unit.c_str (), rd_f64 (r)));
            }
        }
        else
        {
            return false;
        }
        return r->ok;
    }

    /* A descriptor (without the version) from the reader. */
    bool parse_descriptor (Reader* r, DValue* out)
    {
        return parse_object (r, out, 0);
    }

    void write_value (std::vector<uint8_t>* w, const DValue& v);

    void write_object (std::vector<uint8_t>* w, const DValue& obj)
    {
        wr_unicode (w, obj.str, true);
        wr_id (w, obj.cls);
        wr_u32 (w, (uint32_t) obj.keys.size ());
        for (size_t i = 0; i < obj.keys.size (); i++)
        {
            wr_id (w, obj.keys[i]);
            write_value (w, obj.items[i]);
        }
    }

    void write_value (std::vector<uint8_t>* w, const DValue& v)
    {
        switch (v.type)
        {
        case DType::Object:
            wr_key4 (w, "Objc");
            write_object (w, v);
            break;
        case DType::List:
            wr_key4 (w, "VlLs");
            wr_u32 (w, (uint32_t) v.items.size ());
            for (size_t i = 0; i < v.items.size (); i++)
            {
                write_value (w, v.items[i]);
            }
            break;
        case DType::Double:
            wr_key4 (w, "doub");
            wr_f64 (w, v.num);
            break;
        case DType::UnitFloat:
            wr_key4 (w, "UntF");
            wr_key4 (w, v.cls.c_str ());
            wr_f64 (w, v.num);
            break;
        case DType::Text:
            wr_key4 (w, "TEXT");
            wr_unicode (w, v.str, true);
            break;
        case DType::Enum:
            wr_key4 (w, "enum");
            wr_id (w, v.cls);
            wr_id (w, v.str);
            break;
        case DType::Long:
            wr_key4 (w, "long");
            wr_u32 (w, (uint32_t) (int32_t) v.i);
            break;
        case DType::LargeInt:
            wr_key4 (w, "comp");
            wr_u64 (w, (uint64_t) v.i);
            break;
        case DType::Bool:
            wr_key4 (w, "bool");
            wr_u8 (w, v.b ? 1 : 0);
            break;
        case DType::Class:
            wr_key4 (w, "type");
            wr_unicode (w, v.str, true);
            wr_id (w, v.cls);
            break;
        case DType::Raw:
            wr_key4 (w, v.cls.size () == 4 ? v.cls.c_str () : "tdta");
            wr_u32 (w, (uint32_t) v.str.size ());
            wr_bytes (w, v.str.data (), v.str.size ());
            break;
        case DType::Reference:
            /* Not written back: an empty reference. */
            wr_key4 (w, "obj ");
            wr_u32 (w, 0);
            break;
        }
    }

    void write_descriptor (std::vector<uint8_t>* w, const DValue& obj)
    {
        write_object (w, obj);
    }

} // namespace psd
