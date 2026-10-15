#pragma once

#include "doc/doc.h"
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace psd
{

    bool parse_smart_layer (const uint8_t* sold, size_t sold_len, const uint8_t* plld, size_t plld_len,
        doc::SmartLayer* out, uint32_t* w, uint32_t* h);
    void parse_linked_files (const uint8_t* data, size_t len, std::vector<doc::SmartContents>* out);
    std::vector<uint8_t> write_sold (const doc::SmartLayer& s, uint32_t w, uint32_t h, float dpi);
    void write_linked_file (const std::string& uuid, const std::string& filename, const char type[4],
        const std::vector<uint8_t>& data, std::vector<uint8_t>* out);

} // namespace psd
