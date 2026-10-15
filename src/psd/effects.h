#pragma once

#include "color/blend.h"
#include "doc/doc.h"
#include "psd/descriptor.h"
#include <stdint.h>
#include <vector>

namespace psd
{

    const char* blend_key (color::BlendMode m);
    bool blend_from_key (const char* key, color::BlendMode* out);
    void color_from (const DValue* c, float out[3]);
    DValue color_value (const float c[3]);
    bool parse_lfx2 (const uint8_t* data, size_t len, doc::LayerStyle* out);
    std::vector<uint8_t> write_lfx2 (const doc::LayerStyle& st);

} // namespace psd
