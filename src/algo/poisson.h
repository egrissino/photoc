#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace algo
{

    void solve_membrane (size_t w, size_t h, const std::vector<bool>& unknown, float* v);
    std::vector<float> seamless_clone (
        size_t w, size_t h, size_t ch, const float* src, const float* dst, const std::vector<bool>& mask);
    std::vector<float> membrane_fill (size_t w, size_t h, size_t ch, const float* img, const std::vector<bool>& hole);
    bool best_offset (size_t w, size_t h, size_t ch, const float* img, const std::vector<bool>& hole, size_t ring,
        int32_t max_radius, int32_t* dx, int32_t* dy);

} // namespace algo
