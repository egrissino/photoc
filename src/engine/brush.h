#pragma once

#include "doc/doc.h"
#include "engine/session.h"
#include "paint/paint.h"
#include <stdint.h>
#include <vector>

namespace engine
{

    bool resolve_brush (const Session& s, const Value& p, const char* cmd, paint::BrushSettings* out, Error* err);
    bool tool_brush (const Session& s, const char* cmd, const Value& p, paint::BrushSettings* out, Error* err);
    bool is_mask_target (const Value& p);
    bool is_quick_mask (const Value& p);
    bool parse_points (const Value& p, const char* cmd, std::vector<paint::StrokePoint>* out, Error* err);
    raster::Surface* stroke_surface (doc::Document* d, doc::LayerId id, const Value& p, bool* lock, Error* err);
    void finish_brush (paint::BrushSettings* b, const raster::Surface& surf, bool lock, bool mask, bool auto_erase,
        const paint::StrokePoint* first, const float background[4], const color::CmykSpace* cmyk);
    void translate_layer (doc::Layer* l, int32_t dx, int32_t dy);
    bool move_layers (
        doc::Document* d, const std::vector<doc::LayerId>& roots, int32_t dx, int32_t dy, bool mask_target, Error* err);

} // namespace engine
