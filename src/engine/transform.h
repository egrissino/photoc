#pragma once

#include "doc/doc.h"
#include "engine/session.h"
#include "geom/geom.h"

namespace engine
{

    geom::Rect transform_bounds (const doc::Document& d, const doc::Layer& l);
    bool apply_deform_command (Session& s, doc::Document* d, const char* cmd, const Value& p, Error* err);
    geom::Rect target_bounds (const doc::Document& d, const raster::Surface& surf);
    bool apply_transform (Session& s, doc::Document* d, doc::LayerId id, const Value& p, double rect[4], Error* err);

} // namespace engine
