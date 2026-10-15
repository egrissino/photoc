#pragma once

#include "color/convert.h"
#include "doc/doc.h"
#include "text/fonts.h"
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace psd
{

    bool is_psd (const uint8_t* data, size_t len);
    bool read_document (doc::IdSource& ids, const std::string& name, const uint8_t* data, size_t len,
        const color::CmykSpace* cmyk, text::FontDb* fonts, doc::Document* out, std::vector<std::string>* warnings,
        char* err, size_t err_len);
    bool write_document (const doc::Document& d, const color::CmykSpace* cmyk, text::FontDb* fonts,
        std::vector<uint8_t>* out, std::vector<std::string>* warnings, char* err, size_t err_len);

} // namespace psd
