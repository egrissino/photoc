#pragma once

#include "color/convert.h"
#include "doc/text.h"
#include "raster/raster.h"
#include "text/fonts.h"
#include "text/layout.h"

namespace text
{

    raster::Surface render_text (const doc::TextLayer& t, const Layout& layout, FontDb* db, color::PixelFormat format,
        const color::CmykSpace* cmyk);
    bool render_layer_text (const doc::TextLayer& t, FontDb* db, float dpi, color::PixelFormat format,
        const color::CmykSpace* cmyk, raster::Surface* out);

} // namespace text
