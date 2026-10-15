#pragma once

#include "color/blend.h"
#include <stdint.h>
#include <string>

#include <nlohmann/json.hpp>

namespace engine
{

    typedef nlohmann::json Value;

    const Value* param (const Value& p, const char* key);
    bool param_present (const Value& p, const char* key);
    bool param_f64 (const Value& p, const char* key, double* out);
    bool param_u64 (const Value& p, const char* key, uint64_t* out);
    bool param_i64 (const Value& p, const char* key, int64_t* out);
    bool param_bool (const Value& p, const char* key, bool* out);
    bool param_str (const Value& p, const char* key, std::string* out);

    bool parse_hex (const char* s, float out[4]);
    bool blend_from_str (const char* s, color::BlendMode* out);
    std::string blend_variant_name (color::BlendMode m);

} // namespace engine
