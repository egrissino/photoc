#pragma once

#include "color/color.h"
#include "doc/adjust.h"
#include <stdint.h>
#include <string>
#include <vector>

namespace psd
{

    struct AdjBlock
    {
        std::string key;
        std::vector<uint8_t> data;
    };

    bool is_adjustment_key (const char* key);
    doc::Adjustment parse_adjustment (const std::string& key, const std::vector<uint8_t>& data,
        const std::vector<uint8_t>* cged, color::ColorMode mode);
    std::vector<AdjBlock> write_adjustment (const doc::Adjustment& a);

} // namespace psd
