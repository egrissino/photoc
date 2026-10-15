#pragma once

#include "doc/vector.h"
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace psd
{

    bool parse_path_records (const uint8_t* data, size_t len, uint32_t width, uint32_t height, doc::Path* out);
    void write_path_records (const doc::Path& p, uint32_t width, uint32_t height, std::vector<uint8_t>* out);
    bool parse_vector_mask (const uint8_t* data, size_t len, uint32_t width, uint32_t height, doc::VectorMask* out);
    std::vector<uint8_t> write_vector_mask (const doc::VectorMask& m, uint32_t width, uint32_t height);
    bool parse_solid_color (const uint8_t* data, size_t len, float out[3]);
    std::vector<uint8_t> write_solid_color (const float c[3]);
    bool parse_stroke_style (const uint8_t* data, size_t len, doc::ShapeStroke* out, bool* fill_enabled);
    std::vector<uint8_t> write_stroke_style (const doc::ShapeStroke& s, bool fill_enabled);

} // namespace psd
