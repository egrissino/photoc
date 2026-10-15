#pragma once

#include "color/color.h"
#include "color/convert.h"
#include "doc/adjust.h"
#include "engine/error.h"
#include "engine/params.h"
#include "raster/raster.h"
#include <stddef.h>
#include <stdint.h>

namespace engine
{

    struct PhotoFilterPreset
    {
        const char* id;
        const char* label;
        uint8_t color[3];
    };

    extern const PhotoFilterPreset PHOTO_FILTERS[];
    extern const size_t PHOTO_FILTER_COUNT;

    bool adjustment_from_params (doc::AdjustmentKind kind, const Value& p, color::ColorMode mode,
        const doc::Adjustment* base, doc::Adjustment* out, Error* err);
    Value adjustment_to_params (const doc::Adjustment& a);
    void adjust_surface (raster::Surface* s, const doc::Adjustment& adj, const raster::Surface* sel,
        color::ColorMode mode, const color::CmykSpace* cmyk);

} // namespace engine
