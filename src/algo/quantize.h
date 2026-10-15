#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace algo
{

    enum class PaletteKind
    {
        Exact,
        SystemMac,
        SystemWindows,
        Web,
        Uniform,
        Perceptual,
        Selective,
        Adaptive,
    };

    enum class Forced
    {
        None,
        BlackWhite,
        Primaries,
        Web,
    };

    enum class Dither
    {
        None,
        Diffusion,
        Pattern,
        Noise,
    };

    enum class BitmapMethod
    {
        Threshold,
        Pattern,
        Diffusion,
        Halftone,
    };

    enum class HalftoneShape
    {
        Round,
        Ellipse,
        Line,
        Square,
        Diamond,
        Cross,
    };

    bool palette_kind_from_id (const char* s, PaletteKind* out);
    bool halftone_shape_from_id (const char* s, HalftoneShape* out);
    std::vector<uint8_t> web_palette ();
    std::vector<uint8_t> mac_palette ();
    std::vector<uint8_t> windows_palette ();
    std::vector<uint8_t> uniform_palette (size_t n);
    bool build_palette (const std::vector<float>& rgba, PaletteKind kind, size_t colors, Forced forced,
        std::vector<uint8_t>* out, std::string* err);
    void quantize (std::vector<float>* rgba, size_t w, const std::vector<uint8_t>& pal, Dither dither, float amount,
        int transparent);
    void to_bitmap (
        std::vector<float>* gray, size_t w, BitmapMethod method, float cell, float angle, HalftoneShape shape);

} // namespace algo
