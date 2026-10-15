#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace algo
{

    bool inpaint (size_t w, size_t h, size_t ch, std::vector<float>* img, const std::vector<bool>& hole,
        const std::vector<bool>* source, uint64_t seed);

}
