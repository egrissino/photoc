#pragma once

#include "psd/bytes.h"
#include <stdint.h>
#include <string>
#include <vector>

namespace psd
{

    enum class DType : uint8_t
    {
        Object,
        List,
        Double,
        UnitFloat,
        Text,
        Enum,
        Long,
        LargeInt,
        Bool,
        Class,
        Raw,
        Reference,
    };

    struct DValue
    {
        DType type;
        std::string cls;
        std::string str;
        double num;
        int64_t i;
        bool b;
        std::vector<std::string> keys;
        std::vector<DValue> items;
    };

    DValue d_object (const char* cls);
    DValue d_double (double v);
    DValue d_unit (const char* unit, double v);
    DValue d_text (const std::string& s);
    DValue d_enum (const char* type, const char* value);
    DValue d_long (int32_t v);
    DValue d_bool (bool v);
    DValue d_list ();
    DValue d_raw (const char* type, const std::string& bytes);
    void d_set (DValue* obj, const char* key, const DValue& v);
    const DValue* d_get (const DValue& obj, const char* key);
    double d_num (const DValue& obj, const char* key, double fallback);
    bool d_flag (const DValue& obj, const char* key, bool fallback);
    std::string d_enum_of (const DValue& obj, const char* key);

    bool parse_descriptor (Reader* r, DValue* out);
    void write_descriptor (std::vector<uint8_t>* w, const DValue& obj);

} // namespace psd
