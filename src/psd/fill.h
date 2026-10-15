#pragma once

#include "doc/doc.h"
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace psd
{

    bool parse_gradient_fill (const uint8_t* data, size_t len, doc::FillLayer* out);
    std::vector<uint8_t> write_gradient_fill (const doc::FillLayer& f);

} // namespace psd
