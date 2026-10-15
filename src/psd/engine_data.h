#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace psd
{

    enum class EType : uint8_t
    {
        Dict,
        Array,
        String,
        Int,
        Real,
        Bool,
        Name,
    };

    struct EValue
    {
        EType type;
        std::string str;
        int64_t i;
        double num;
        bool b;
        std::vector<std::string> keys;
        std::vector<EValue> items;
    };

    EValue e_dict ();
    EValue e_array ();
    EValue e_string (const std::string& utf8);
    EValue e_int (int64_t v);
    EValue e_real (double v);
    EValue e_bool (bool v);
    const EValue* e_get (const EValue& d, const char* key);
    const EValue* e_path (const EValue& d, const char* const* keys, size_t n);
    EValue* e_get_mut (EValue* d, const char* key);
    void e_set (EValue* d, const char* key, const EValue& v);
    bool e_num (const EValue* v, double* out);
    bool e_flag (const EValue* v, bool* out);

    bool parse_engine_data (const uint8_t* data, size_t len, EValue* out);
    std::vector<uint8_t> write_engine_data (const EValue& v);

} // namespace psd
