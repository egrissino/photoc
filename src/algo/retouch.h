#pragma once

#include <stddef.h>
#include <vector>

namespace algo
{

    enum class ToneRange
    {
        Shadows,
        Midtones,
        Highlights,
    };

    float luma (const float c[3]);
    float tone_curve (float v, float e, ToneRange range, bool burn);
    void dodge_burn (const float c[3], float e, ToneRange range, bool burn, bool protect_tones, float out[3]);
    float saturation (const float c[3]);
    void sponge (const float c[3], float amount, bool saturate, bool vibrance, float out[3]);
    void local_blur (const float* data, size_t w, size_t h, size_t ch, int alpha, size_t x0, size_t y0, size_t x1,
        size_t y1, float sigma, std::vector<float>* out);
    void local_sharpen (const float* data, size_t w, size_t h, size_t ch, int alpha, size_t x0, size_t y0, size_t x1,
        size_t y1, float amount, bool protect_detail, std::vector<float>* out);

} // namespace algo
