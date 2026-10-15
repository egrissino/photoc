#pragma once

#include "compose/compose.h"
#include "compose/psblend.h"
#include "doc/doc.h"
#include <stdint.h>

namespace compose
{

    bool has_effects (const doc::Layer& layer);
    int32_t effects_margin (const doc::Layer& layer);
    float gradient_t (const doc::Effect& e, geom::Rect bounds, float x, float y);
    void composite_with_effects (const doc::Layer& layer, const Buffer& content, Buffer* backdrop, geom::Rect bounds,
        float light_angle, float light_alt, BlendSpace space, const doc::Document* d);

} // namespace compose
