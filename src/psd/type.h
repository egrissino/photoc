#pragma once

#include "doc/text.h"
#include "text/fonts.h"
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace psd
{

    bool tysh_to_text (const uint8_t* data, size_t len, float dpi, text::FontDb* fonts, doc::TextLayer* out,
        std::vector<std::string>* missing);
    std::vector<uint8_t> text_to_tysh (
        const doc::TextLayer& t, const std::vector<uint8_t>* old, float dpi, text::FontDb* fonts);

} // namespace psd
